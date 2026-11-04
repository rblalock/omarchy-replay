#include "meeting_index.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QSet>
#include <sqlite3.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>

namespace replay {
namespace {
constexpr qint64 MaxTranscript = 512 * 1024, MaxManifest = 64 * 1024;
constexpr int MaxMeetings = 5000, MaxEntriesPerBatch = 32, MaxPassEntries = 10000;
constexpr qint64 MaxTextBytes = 64 * 1024 * 1024;
void fail(const char *message) { throw std::runtime_error(message); }
void exec(sqlite3 *db, const char *sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK) fail("Meeting index operation failed; retry when the history database is available.");
}
struct Statement {
    sqlite3_stmt *s = nullptr;
    Statement(sqlite3 *db, const char *sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) fail("Meeting index query failed.");
    }
    ~Statement() { sqlite3_finalize(s); }
    void bind(int n, qint64 v) { sqlite3_bind_int64(s, n, v); }
    void bind(int n, const QString &v) { const auto b = v.toUtf8(); sqlite3_bind_text(s, n, b.constData(), b.size(), SQLITE_TRANSIENT); }
    bool next() { int r = sqlite3_step(s); if (r != SQLITE_ROW && r != SQLITE_DONE) fail("Meeting index is unavailable; retry later."); return r == SQLITE_ROW; }
    qint64 number(int n) const { return sqlite3_column_int64(s, n); }
    QString text(int n) const { auto p = sqlite3_column_text(s, n); return p ? QString::fromUtf8(reinterpret_cast<const char *>(p)) : QString(); }
};
bool schema(sqlite3 *db) { Statement s(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meeting_records'"); return s.next(); }
bool realOwned(const QString &path, bool directory, bool privateMode = false) {
    struct stat s{};
    if (lstat(QFile::encodeName(path).constData(), &s) || s.st_uid != getuid()) return false;
    return (directory ? S_ISDIR(s.st_mode) : S_ISREG(s.st_mode)) && !(s.st_mode & 0022) && (!privateMode || !(s.st_mode & 0077));
}
struct Database {
    sqlite3 *db = nullptr;
    Database(const QString &directory, bool write) {
        const QString path = QDir(directory).filePath("index.sqlite");
        if (!QFileInfo::exists(path) && !write) return;
        // The coordinator creates the archive. Imports never create an archive.
        if (!realOwned(directory, true, true) || !realOwned(path, false, true)) fail("Meeting indexing requires an existing private Replay history.");
        if (sqlite3_open_v2(QFile::encodeName(path).constData(), &db, write ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            db = nullptr; fail("Cannot open meeting index.");
        }
        sqlite3_busy_timeout(db, 100);
    }
    ~Database() { if (db) sqlite3_close(db); }
};
void ensureSchema(sqlite3 *db) {
    // Coordinator retention may run concurrently. Publish all optional tables
    // and FTS triggers together, including after a killed importer is retried.
    exec(db, "BEGIN IMMEDIATE");
    try {
    exec(db, "CREATE TABLE IF NOT EXISTS meeting_records("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,identity TEXT NOT NULL UNIQUE,source_root TEXT NOT NULL,"
        "directory_key TEXT NOT NULL,source_directory TEXT NOT NULL,manifest_path TEXT NOT NULL,"
        "signature TEXT NOT NULL,fingerprint TEXT NOT NULL,title TEXT NOT NULL,started_ms INTEGER NOT NULL,"
        "time_known INTEGER NOT NULL,duration_secs INTEGER NOT NULL,retention_ms INTEGER NOT NULL,"
        "transcript TEXT NOT NULL,text_bytes INTEGER NOT NULL);"
        "CREATE INDEX IF NOT EXISTS meeting_time ON meeting_records(retention_ms,id);"
        "CREATE INDEX IF NOT EXISTS meeting_source ON meeting_records(source_root,directory_key);"
        "CREATE VIRTUAL TABLE IF NOT EXISTS meeting_text USING fts5(title,transcript,content='meeting_records',content_rowid='id',tokenize='unicode61 remove_diacritics 2');"
        "CREATE TRIGGER IF NOT EXISTS meeting_text_insert AFTER INSERT ON meeting_records BEGIN "
        "INSERT INTO meeting_text(rowid,title,transcript) VALUES(NEW.id,NEW.title,NEW.transcript); END;"
        "CREATE TRIGGER IF NOT EXISTS meeting_text_delete AFTER DELETE ON meeting_records BEGIN "
        "INSERT INTO meeting_text(meeting_text,rowid,title,transcript) VALUES('delete',OLD.id,OLD.title,OLD.transcript); END;"
        "CREATE TRIGGER IF NOT EXISTS meeting_text_update AFTER UPDATE OF title,transcript ON meeting_records BEGIN "
        "INSERT INTO meeting_text(meeting_text,rowid,title,transcript) VALUES('delete',OLD.id,OLD.title,OLD.transcript);"
        "INSERT INTO meeting_text(rowid,title,transcript) VALUES(NEW.id,NEW.title,NEW.transcript); END;"
        "CREATE TABLE IF NOT EXISTS meeting_tombstones(identity TEXT PRIMARY KEY,fingerprint TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS meeting_tombstone_fingerprint ON meeting_tombstones(fingerprint);"
        "CREATE TABLE IF NOT EXISTS meeting_deleted_ranges(start_ms INTEGER NOT NULL,end_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS meeting_state(key TEXT PRIMARY KEY,value INTEGER NOT NULL);");
        exec(db, "COMMIT");
    } catch (...) {
        sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr);
        throw;
    }
}
QString digest(const QByteArray &data) { return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()); }
QString inodeKey(const struct stat &s) { return QString::number(s.st_dev) + ':' + QString::number(s.st_ino); }
QString stamp(const struct stat &s) {
    return inodeKey(s) + ':' + QString::number(s.st_size) + ':' + QString::number(s.st_mtim.tv_sec) + ':' + QString::number(s.st_mtim.tv_nsec) + ':' + QString::number(s.st_ctim.tv_sec) + ':' + QString::number(s.st_ctim.tv_nsec);
}
bool safeStat(int fd, const QByteArray &name, struct stat &s, qint64 bound) {
    return !fstatat(fd, name.constData(), &s, AT_SYMLINK_NOFOLLOW) && S_ISREG(s.st_mode) && s.st_uid == getuid() && !(s.st_mode & 0022) && s.st_size > 0 && s.st_size <= bound;
}
std::optional<QByteArray> readFile(int dir, const QByteArray &name, const struct stat &before, qint64 maximum) {
    const int fd = openat(dir, name.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return {};
    struct stat current{};
    if (fstat(fd, &current) || stamp(current) != stamp(before) || !S_ISREG(current.st_mode)) { close(fd); return {}; }
    QByteArray bytes; bytes.resize(int(current.st_size));
    qint64 position = 0;
    while (position < bytes.size()) {
        const auto n = read(fd, bytes.data() + position, size_t(bytes.size() - position));
        if (n <= 0) { close(fd); return {}; }
        position += n;
    }
    const bool stable = !fstat(fd, &current) && stamp(current) == stamp(before) && current.st_size <= maximum;
    close(fd);
    if (!stable) return {};
    return bytes;
}
// Use the index's own normalization rather than approximating unicode61 with
// a regular expression. In particular, SQLite folds Latin diacritics and has
// its own Unicode token boundaries. This private in-memory handle never opens
// an archive and is reused on each caller thread.
class MeetingTokenizer {
    sqlite3 *db = nullptr;
    fts5_tokenizer api{};
    Fts5Tokenizer *tokenizer = nullptr;
public:
    MeetingTokenizer() {
        if (sqlite3_open(":memory:", &db) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            fail("Cannot initialize meeting text matching.");
        }
        fts5_api *fts = nullptr;
        sqlite3_stmt *statement = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_pointer(statement, 1, &fts, "fts5_api_ptr", nullptr);
            sqlite3_step(statement);
        }
        sqlite3_finalize(statement);
        void *context = nullptr;
        const char *arguments[] = {"remove_diacritics", "2"};
        if (!fts || fts->xFindTokenizer(fts, "unicode61", &context, &api) != SQLITE_OK ||
            api.xCreate(context, arguments, 2, &tokenizer) != SQLITE_OK) {
            sqlite3_close(db);
            fail("Cannot initialize meeting text matching.");
        }
    }
    ~MeetingTokenizer() { api.xDelete(tokenizer); sqlite3_close(db); }
    void each(const QByteArray &text, int flags, void *context,
              int (*callback)(void *, int, const char *, int, int, int)) {
        const int result = api.xTokenize(tokenizer, context, flags, text.constData(), int(text.size()), callback);
        if (result != SQLITE_OK && result != SQLITE_DONE) fail("Meeting text matching failed.");
    }
};
MeetingTokenizer &meetingTokenizer() { thread_local MeetingTokenizer tokenizer; return tokenizer; }
QStringList tokens(const QString &query) {
    if (query.size() > 512) return {};
    QStringList result;
    meetingTokenizer().each(query.toUtf8(), FTS5_TOKENIZE_QUERY, &result,
        [](void *context, int, const char *token, int length, int, int) {
            auto &words = *static_cast<QStringList *>(context);
            words.append(QString::fromUtf8(token, length));
            return words.size() >= 16 ? SQLITE_DONE : SQLITE_OK;
        });
    return result;
}
QString ftsQuery(const QString &query, SearchMode mode) {
    const auto words = tokens(query); QStringList escaped;
    for (int i = 0; i < words.size(); ++i) escaped << ('"' + words[i] + '"' + (mode == SearchMode::PrefixLastToken && i == words.size()-1 && words[i].size() >= 3 ? "*" : ""));
    return escaped.join(" AND ");
}
MeetingRecord record(Statement &s) {
    MeetingRecord m; m.id = s.number(0); m.title = s.text(1); m.startedAtMs = s.number(2); m.timeKnown = s.number(3);
    m.durationSeconds = s.number(4); m.sourceDirectory = s.text(5); m.manifestPath = s.text(6); m.revision = s.text(7) + m.manifestPath; return m;
}
// Matching lines in transcript order, each clipped around its first match.
QStringList matchingPassages(const QString &text, const QString &query, SearchMode mode, int limit) {
    QStringList passages;
    if (limit <= 0) return passages;
    qsizetype lastLineStart = -1;
    for (const auto &range : meetingMatchRanges(text, query, mode)) {
        const qsizetype lineStart = text.lastIndexOf('\n', range.first) + 1;
        if (lineStart == lastLineStart) continue;
        lastLineStart = lineStart;
        qsizetype lineEnd = text.indexOf('\n', range.first);
        if (lineEnd < 0) lineEnd = text.size();
        qsizetype start = lineStart, end = lineEnd;
        if (end - start > MeetingPassageChars) {
            start = std::max(lineStart, qsizetype(range.first) - MeetingPassageChars / 3);
            end = std::min(lineEnd, start + MeetingPassageChars);
            start = std::max(lineStart, end - MeetingPassageChars);
        }
        QString passage = text.mid(start, end - start).trimmed();
        if (start > lineStart) passage.prepend(QStringLiteral("…"));
        if (end < lineEnd) passage.append(QStringLiteral("…"));
        passages.append(passage);
        if (passages.size() >= limit) break;
    }
    return passages;
}
int passageCount(const QString &text, const QString &query, SearchMode mode) {
    QSet<int> lines;
    int line = 0;
    qsizetype nextBreak = text.indexOf('\n');
    for (const auto &range : meetingMatchRanges(text, query, mode)) {
        while (nextBreak >= 0 && nextBreak < range.first) {
            ++line; nextBreak = text.indexOf('\n', nextBreak + 1);
        }
        lines.insert(line);
    }
    return int(lines.size());
}
QString plainTranscript(QString text) {
    // Preserve transcript content and time labels, remove formatting decoration.
    // Consumers must still render as plain text; transcript content is untrusted.
    text.replace("\r\n", "\n"); text.remove('\r'); text.remove(QChar::Null);
    text.replace(QChar::ParagraphSeparator, '\n'); text.replace(QChar::LineSeparator, '\n');
    static const QRegularExpression heading("(?m)^#{1,6} +"); text.remove(heading);
    text.replace("**", "");
    return text.trimmed();
}
void eraseRow(sqlite3 *db, qint64 id, bool tombstone) {
    if (tombstone) { Statement t(db, "INSERT OR IGNORE INTO meeting_tombstones SELECT identity,fingerprint FROM meeting_records WHERE id=?"); t.bind(1,id); t.next(); }
    Statement s(db, "DELETE FROM meeting_records WHERE id=?"); s.bind(1,id); s.next();
}
} // namespace

QVector<QPair<int,int>> meetingMatchRanges(const QString &text, const QString &query, SearchMode mode) {
    const auto words = tokens(query);
    if (words.isEmpty()) return {};
    const QByteArray utf8 = text.toUtf8();
    if (utf8.size() > MaxTranscript) return {};
    // Tokenizer offsets refer to original UTF-8 bytes, while QTextDocument uses
    // UTF-16 positions. Preserve surrogate pairs and decomposed accents.
    QVector<int> positions(utf8.size() + 1);
    int byte = 0;
    for (int character = 0; character < text.size();) {
        const int start = character;
        uint codepoint = text[character++].unicode();
        if (QChar::isHighSurrogate(codepoint) && character < text.size() && text[character].isLowSurrogate())
            codepoint = QChar::surrogateToUcs4(text[start], text[character++]);
        else if (QChar::isSurrogate(codepoint)) codepoint = '?'; // QString's UTF-8 replacement.
        const int width = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 : codepoint < 0x10000 ? 3 : 4;
        for (int i = 0; i < width && byte < positions.size() - 1; ++i) positions[byte++] = start;
        positions[byte] = character;
    }
    struct Matches {
        QVector<QByteArray> words;
        const QVector<int> &positions;
        QVector<QPair<int,int>> ranges;
        bool prefix;
    } matches{{}, positions, {}, mode == SearchMode::PrefixLastToken && words.last().size() >= 3};
    for (const auto &word : words) matches.words.append(word.toUtf8());
    meetingTokenizer().each(utf8, FTS5_TOKENIZE_DOCUMENT, &matches,
        [](void *context, int, const char *token, int length, int start, int end) {
            auto &match = *static_cast<Matches *>(context);
            const QByteArrayView current(token, length);
            for (int i = 0; i < match.words.size(); ++i) {
                const QByteArrayView word(match.words[i]);
                const bool found = match.prefix && i == match.words.size() - 1 ? current.startsWith(word) : current == word;
                if (!found) continue;
                if (start >= 0 && end < match.positions.size())
                    match.ranges.append({match.positions[start], match.positions[end] - match.positions[start]});
                break;
            }
            return match.ranges.size() >= 10000 ? SQLITE_DONE : SQLITE_OK;
        });
    return matches.ranges;
}
MeetingSearchPage listMeetings(const QString &directory, int limit, qint64 offset) {
    return searchMeetings(directory, {}, limit, offset);
}
MeetingSearchPage searchMeetings(const QString &directory, const QString &query, int limit, qint64 offset, SearchMode mode,
                                const MeetingSearchOptions &options) {
    MeetingSearchPage result; result.offset = std::max(qint64(0), offset);
    Database db(directory,false); if (!db.db || !schema(db.db)) return result;
    const auto match = ftsQuery(query,mode); if (!query.trimmed().isEmpty() && match.isEmpty()) return result;
    const bool searching = !match.isEmpty();
    // Interval overlap on known start times; undated imports cannot be placed.
    QByteArray window;
    if (options.sinceMs > 0 || options.untilMs > 0) window += " AND m.time_known=1";
    if (options.sinceMs > 0) window += " AND m.started_ms+m.duration_secs*1000>=?";
    if (options.untilMs > 0) window += " AND m.started_ms<=?";
    const auto bindWindow = [&](Statement &s, int column) {
        if (options.sinceMs > 0) s.bind(column++, options.sinceMs);
        if (options.untilMs > 0) s.bind(column++, options.untilMs);
        return column;
    };
    exec(db.db, "BEGIN");
    const QByteArray countSql = searching ?
        "SELECT count(*) FROM meeting_records m JOIN meeting_text ON meeting_text.rowid=m.id WHERE meeting_text MATCH ?" + window :
        "SELECT count(*) FROM meeting_records m WHERE 1" + window;
    Statement count(db.db, countSql.constData());
    int column = 1; if (searching) count.bind(column++,match); bindWindow(count, column);
    count.next(); result.totalMatches = count.number(0);
    const QByteArray rowSql = (searching ?
        "SELECT m.id,m.title,m.started_ms,m.time_known,m.duration_secs,m.source_directory,m.manifest_path,m.signature,m.transcript FROM meeting_records m JOIN meeting_text ON meeting_text.rowid=m.id WHERE meeting_text MATCH ?" + window :
        "SELECT m.id,m.title,m.started_ms,m.time_known,m.duration_secs,m.source_directory,m.manifest_path,m.signature,'' FROM meeting_records m WHERE 1" + window)
        + " ORDER BY m.retention_ms DESC,m.id DESC LIMIT ? OFFSET ?";
    Statement rows(db.db, rowSql.constData());
    column=1; if(searching) rows.bind(column++,match); column=bindWindow(rows,column);
    rows.bind(column++,std::clamp(limit,1,MeetingPageLimit)); rows.bind(column,result.offset);
    while(rows.next()) {
        MeetingSearchResult r; r.meeting=record(rows);
        if (searching) {
            const QString text = rows.text(8);
            r.matchingPassages = passageCount(text,query,mode);
            r.passages = matchingPassages(text,query,mode,options.passageLimit);
        }
        result.results.append(r);
    }
    exec(db.db, "COMMIT"); return result;
}
std::optional<MeetingRecord> readMeeting(const QString &directory,qint64 id) {
    Database db(directory,false); if (!db.db || !schema(db.db)) return {};
    Statement s(db.db,"SELECT id,title,started_ms,time_known,duration_secs,source_directory,manifest_path,signature,transcript FROM meeting_records WHERE id=?");
    s.bind(1,id); if(!s.next()) return {}; auto m=record(s); m.transcript=s.text(8); return m;
}
std::optional<FrameRecord> screenNearMeetingStart(const QString &directory,qint64 timestampMs,qint64 toleranceMs) {
    if (timestampMs <= 0 || toleranceMs < 0 || toleranceMs > 60000) return {};
    const auto frame=frameNearTimestamp(directory,timestampMs);
    if (!frame || !frame->available) return {};
    const qint64 near=std::clamp(timestampMs,frame->timestampMs,std::max(frame->timestampMs,frame->lastTimestampMs));
    if (qAbs(near-timestampMs)>toleranceMs) return {};
    Database db(directory,false); if(!db.db) return {};
    Statement deletedPolicy(db.db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='meeting_deleted_ranges'");
    if (deletedPolicy.next()) {
        Statement deleted(db.db,"SELECT 1 FROM meeting_deleted_ranges WHERE start_ms<=? AND end_ms>? LIMIT 1");
        deleted.bind(1,timestampMs);deleted.bind(2,timestampMs);
        if (deleted.next()) return {};
    }
    Statement observations(db.db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='observations'");
    if (observations.next()) {
        // Shared originals can span a subsequently deleted interval. Require an
        // actual retained observation near the requested meeting start.
        Statement observed(db.db,"SELECT 1 FROM observations WHERE timestamp_ms>=? AND timestamp_ms<=? AND frame_id=? LIMIT 1");
        observed.bind(1,timestampMs-toleranceMs);observed.bind(2,timestampMs+toleranceMs);observed.bind(3,frame->id);
        if (!observed.next()) return {};
    }
    Statement exists(db.db,"SELECT 1 FROM sqlite_master WHERE type='table' AND name='history_gaps'");
    if(exists.next()) {
        Statement gap(db.db,"SELECT 1 FROM history_gaps WHERE start_ms<=? AND end_ms>? LIMIT 1");
        gap.bind(1,std::max(near,timestampMs));gap.bind(2,std::min(near,timestampMs));
        if(gap.next())return {};
    }
    return frame;
}
QVector<MeetingTimelinePoint> meetingTimeline(const QString &directory,int limit) {
    QVector<MeetingTimelinePoint> result; Database db(directory,false); if (!db.db || !schema(db.db)) return result;
    Statement s(db.db,"SELECT id,started_ms,title FROM meeting_records WHERE time_known=1 ORDER BY started_ms DESC,id DESC LIMIT ?");
    s.bind(1,std::clamp(limit,1,1000)); while(s.next()) result.append({s.number(0),s.number(1),s.text(2)});
    std::reverse(result.begin(),result.end()); return result;
}
qint64 deleteMeetingsInRange(sqlite3 *db,qint64 from,qint64 until,int limit,bool retention) {
    if (until <= from) return 0;
    const bool hasRecords = schema(db);
    if (!hasRecords && retention) return 0;
    // A user can delete a time range before enabling meeting import. Keep its
    // small policy table without creating the optional full-text schema.
    if (!hasRecords) exec(db,"CREATE TABLE IF NOT EXISTS meeting_deleted_ranges(start_ms INTEGER NOT NULL,end_ms INTEGER NOT NULL)");
    if (retention) {
        Statement s(db,"INSERT INTO meeting_state(key,value) VALUES('cutoff',?) ON CONFLICT(key) DO UPDATE SET value=max(value,excluded.value)"); s.bind(1,until); s.next();
    } else {
        // Merge overlapping/adjacent ranges so repeated UI deletion is compact.
        Statement range(db,"SELECT min(start_ms),max(end_ms) FROM meeting_deleted_ranges WHERE start_ms<=? AND end_ms>=?");
        range.bind(1,until); range.bind(2,from); range.next();
        if (range.number(1)>0) { from=std::min(from,range.number(0)); until=std::max(until,range.number(1)); }
        Statement remove(db,"DELETE FROM meeting_deleted_ranges WHERE start_ms<=? AND end_ms>=?"); remove.bind(1,until);remove.bind(2,from);remove.next();
        Statement add(db,"INSERT INTO meeting_deleted_ranges VALUES(?,?)");add.bind(1,from);add.bind(2,until);add.next();
    }
    if (!hasRecords) return 0;
    QVector<qint64> ids; Statement rows(db,"SELECT id FROM meeting_records WHERE retention_ms>=? AND retention_ms<? ORDER BY retention_ms,id LIMIT ?");
    rows.bind(1,from);rows.bind(2,until);rows.bind(3,std::clamp(limit,1,10000));while(rows.next())ids.append(rows.number(0));
    for(auto id:ids)eraseRow(db,id,true);
    return ids.size();
}
bool evictOldestMeeting(sqlite3 *db) {
    if(!schema(db))return false;
    Statement row(db,"SELECT id FROM meeting_records ORDER BY retention_ms,id LIMIT 1");
    if(!row.next())return false;
    const auto id=row.number(0);eraseRow(db,id,true);return true;
}

struct MeetingImporter::Impl {
    QString directory,source,rootKey,archiveIdentity,indexIdentity;
    quint64 maxDiskBytes=0,minFreeBytes=0;
    std::function<bool()> canceled;
    DIR *scan=nullptr;
    int passEntries=0;
    QSet<QString> seen;
    QHash<QString,QString> stable;
    QHash<QString,int> invalid;
    Impl(QString history,QString root,std::function<bool()> stop,quint64 maxBytes,quint64 freeBytes):
        directory(std::move(history)),source(QDir::cleanPath(root)),maxDiskBytes(maxBytes),minFreeBytes(freeBytes),canceled(std::move(stop)) {
        struct stat dir{},index{};
        if (!realOwned(directory,true,true) || !realOwned(directory+"/index.sqlite",false,true) ||
            lstat(QFile::encodeName(directory).constData(),&dir) || lstat(QFile::encodeName(directory+"/index.sqlite").constData(),&index))
            fail("Meeting indexing requires an existing private Replay history.");
        archiveIdentity=inodeKey(dir);indexIdentity=inodeKey(index);
    }
    void checkArchive() const {
        struct stat dir{},index{};
        if (lstat(QFile::encodeName(directory).constData(),&dir) || lstat(QFile::encodeName(directory+"/index.sqlite").constData(),&index) ||
            !S_ISDIR(dir.st_mode) || !S_ISREG(index.st_mode) || inodeKey(dir)!=archiveIdentity || inodeKey(index)!=indexIdentity)
            fail("The meeting history location changed; reopen Replay before importing.");
    }
    ~Impl(){if(scan)closedir(scan);}
    bool stopped()const{return canceled && canceled();}
    void end(){if(scan)closedir(scan);scan=nullptr;seen.clear();passEntries=0;}
};
MeetingImporter::MeetingImporter(const QString &directory,const QString &sourceDirectory,std::function<bool()> canceled,quint64 maxDiskBytes,quint64 minFreeBytes):
    d(std::make_unique<Impl>(directory,sourceDirectory,std::move(canceled),maxDiskBytes,minFreeBytes)) {}
MeetingImporter::~MeetingImporter()=default;

MeetingSyncResult MeetingImporter::sync(qint64 nowMs,qint64 cutoff) {
    MeetingSyncResult result;
    if(d->stopped()){result.canceled=true;return result;}
    d->checkArchive();
    const QFileInfo sourceInfo(d->source);
    // Reject symlinks anywhere beneath the selected root by opening each child
    // relative to its held directory descriptor. The root itself must be real.
    if(!sourceInfo.isAbsolute() || !realOwned(d->source,true) || sourceInfo.canonicalFilePath()!=d->source){d->end();return result;}
    if (d->scan) {
        struct stat held{}, current{};
        if (fstat(dirfd(d->scan),&held) || lstat(QFile::encodeName(d->source).constData(),&current) || inodeKey(held)!=inodeKey(current)) {
            d->end(); d->stable.clear(); d->invalid.clear();
        }
    }
    if(!d->scan){
        int fd=open(QFile::encodeName(d->source).constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)return result;
        d->scan=fdopendir(fd);if(!d->scan){close(fd);return result;}
        struct stat s{};if(fstat(fd,&s)){d->end();return result;}
        d->rootKey=digest((d->source+':'+inodeKey(s)).toUtf8());
    }
    result.sourceAvailable=true;
    Database db(d->directory,true);d->checkArchive();
    if (d->maxDiskBytes && !schema(db.db)) {
        const auto space=makeHistorySpace(d->directory,d->maxDiskBytes,d->minFreeBytes,128*1024);
        if (!space.ready) { result.limited=true; return result; }
    }
    ensureSchema(db.db);
    if (cutoff > 0 && !d->stopped()) {
        exec(db.db,"BEGIN IMMEDIATE");
        try { result.removed += int(deleteMeetingsInRange(db.db,0,cutoff,1000,true)); exec(db.db,"COMMIT"); }
        catch (...) { sqlite3_exec(db.db,"ROLLBACK",nullptr,nullptr,nullptr); throw; }
    }
    qint64 bytesRead=0;
    bool finished=false;
    while(result.scanned<MaxEntriesPerBatch && bytesRead<2*1024*1024){
        if(d->stopped()){result.canceled=true;break;}
        errno=0; const auto *entry=readdir(d->scan);
        if(!entry){if(errno){d->end();result.sourceAvailable=false;return result;}finished=true;break;}
        const QByteArray name(entry->d_name); if(name=="." || name=="..")continue;
        ++result.scanned;
        if(++d->passEntries>MaxPassEntries){result.limited=true;d->end();return result;}
        if(QString::fromUtf8(name).toUtf8()!=name)continue;
        struct stat folder{};
        if(fstatat(dirfd(d->scan),name.constData(),&folder,AT_SYMLINK_NOFOLLOW)||!S_ISDIR(folder.st_mode)||folder.st_uid!=getuid()||(folder.st_mode&0022))continue;
        const QString key=inodeKey(folder);d->seen.insert(key);
        const QString meetingDir=QDir(d->source).filePath(QString::fromUtf8(name));
        const int fd=openat(dirfd(d->scan),name.constData(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(fd<0)continue;
        struct stat opened{};if(fstat(fd,&opened) || inodeKey(opened)!=key){close(fd);continue;}
        DIR *contents=fdopendir(fd);if(!contents){close(fd);continue;}
        QByteArray manifest; bool ambiguous=false;int children=0;
        while(const auto *child=readdir(contents)){
            if(++children>128){ambiguous=true;break;}
            const QByteArray file(child->d_name);if(file.endsWith(".meeting-recorder")){
                if(!manifest.isEmpty()){ambiguous=true;break;}manifest=file;
            }
        }
        struct stat meta{},text{};
        bool valid=!ambiguous && !manifest.isEmpty() && safeStat(fd,manifest,meta,MaxManifest) && safeStat(fd,"transcript.md",text,MaxTranscript);
        const QString signature=valid ? digest((QString::fromUtf8(manifest)+':'+stamp(meta)+':'+stamp(text)).toUtf8()) : QString();
        const bool stable=d->stable.value(key)==signature && !signature.isEmpty() &&
            qint64(meta.st_mtim.tv_sec)*1000+meta.st_mtim.tv_nsec/1000000 <= nowMs-1000 &&
            qint64(text.st_mtim.tv_sec)*1000+text.st_mtim.tv_nsec/1000000 <= nowMs-1000;
        d->stable.insert(key,signature);
        if(!valid){
            if(++d->invalid[key]>=2 && !d->stopped()){
                d->checkArchive();
                Statement remove(db.db,"DELETE FROM meeting_records WHERE source_root=? AND directory_key=?");remove.bind(1,d->rootKey);remove.bind(2,key);remove.next();result.removed+=sqlite3_changes(db.db);
            }
            closedir(contents);continue;
        }
        d->invalid.remove(key);
        if(!stable){closedir(contents);continue;}
        Statement existing(db.db,"SELECT id,signature,source_directory FROM meeting_records WHERE source_root=? AND directory_key=?");
        existing.bind(1,d->rootKey);existing.bind(2,key);
        const bool present=existing.next();const qint64 oldId=present?existing.number(0):0;
        if(present && existing.text(1)==signature && existing.text(2)==meetingDir){closedir(contents);continue;}
        sqlite3_reset(existing.s); // Do not pin a WAL reader during space reclamation.
        if (bytesRead + meta.st_size + text.st_size > 2*1024*1024) { closedir(contents); continue; }
        const auto metadata=readFile(fd,manifest,meta,MaxManifest);const auto transcript=readFile(fd,"transcript.md",text,MaxTranscript);
        closedir(contents);bytesRead+=meta.st_size+text.st_size;
        if(!metadata || !transcript)continue;
        QJsonParseError parse;const auto doc=QJsonDocument::fromJson(*metadata,&parse);
        QStringDecoder decoder(QStringDecoder::Utf8);const QString decoded=decoder.decode(*transcript);
        if(parse.error!=QJsonParseError::NoError || !doc.isObject() || decoder.hasError())continue;
        const auto object=doc.object();const auto title=object.value("title").toString().trimmed();
        const auto started=object.value("started_at");
        if(title.isEmpty() || title.size()>512 || (!started.isUndefined() && !started.isNull() && !started.isDouble()))continue;
        const qint64 seconds=(started.isUndefined() || started.isNull()) ? 0 : started.toInteger(-1);
        if(seconds<0 || seconds>253402300799LL)continue;
        const bool known=!object.value("imported").isString() && seconds>0;
        const qint64 startedMs=known?seconds*1000:0;
        const qint64 duration=object.value("duration_secs").toInteger(0);
        if(duration<0 || duration>366*86400LL)continue;
        const QString clean=plainTranscript(decoded);if(clean.isEmpty())continue;
        const QString fingerprint=digest((QString::number(seconds)+':'+object.value("imported").toString()+':'+digest(clean.toUtf8())).toUtf8());
        const QString identity=digest((d->rootKey+':'+key+':'+QString::number(seconds)+':'+object.value("imported").toString()).toUtf8());
        if(d->stopped()){result.canceled=true;break;}
        d->checkArchive();
        {
            // Check known exclusions before storage admission; an expired or
            // explicitly deleted source must never evict retained screens.
            qint64 retained=known?startedMs:nowMs;
            Statement previous(db.db,"SELECT retention_ms FROM meeting_records WHERE identity=?");previous.bind(1,identity);if(previous.next())retained=previous.number(0);
            Statement blocked(db.db,"SELECT EXISTS(SELECT 1 FROM meeting_tombstones WHERE identity=? OR fingerprint=?) OR EXISTS(SELECT 1 FROM meeting_deleted_ranges WHERE start_ms<=? AND end_ms>?) OR ?<coalesce((SELECT value FROM meeting_state WHERE key='cutoff'),0)");
            blocked.bind(1,identity);blocked.bind(2,fingerprint);blocked.bind(3,retained);blocked.bind(4,retained);blocked.bind(5,retained);blocked.next();
            if(retained<cutoff || blocked.number(0))continue;
        }
        if (d->maxDiskBytes) {
            // FTS postings and journals grow in addition to retained text. This
            // conservative admission reserve bounds a single publication; the
            // next batch rechecks actual archive and free-disk usage.
            const quint64 reserve=8ULL*1024*1024 + quint64(clean.toUtf8().size())*8;
            const auto space=makeHistorySpace(d->directory,d->maxDiskBytes,d->minFreeBytes,reserve);
            if (!space.ready) { result.limited=true; continue; }
        }
        d->checkArchive();
        exec(db.db,"BEGIN IMMEDIATE");
        try{
            if(d->stopped()){exec(db.db,"ROLLBACK");result.canceled=true;break;}
            d->checkArchive();
            // Preserve first-import retention time across edits and renames.
            qint64 retained=known?startedMs:nowMs;
            Statement previous(db.db,"SELECT retention_ms FROM meeting_records WHERE identity=?");previous.bind(1,identity);if(previous.next())retained=previous.number(0);
            Statement blocked(db.db,"SELECT EXISTS(SELECT 1 FROM meeting_tombstones WHERE identity=? OR fingerprint=?) OR EXISTS(SELECT 1 FROM meeting_deleted_ranges WHERE start_ms<=? AND end_ms>?) OR ?<coalesce((SELECT value FROM meeting_state WHERE key='cutoff'),0)");
            blocked.bind(1,identity);blocked.bind(2,fingerprint);blocked.bind(3,retained);blocked.bind(4,retained);blocked.bind(5,retained);blocked.next();
            if(retained<cutoff || blocked.number(0)){if(oldId)eraseRow(db.db,oldId,true);exec(db.db,"COMMIT");continue;}
            Statement usage(db.db,"SELECT count(*),coalesce(sum(text_bytes),0) FROM meeting_records WHERE id<>?");usage.bind(1,oldId);usage.next();
            if((!present && usage.number(0)>=MaxMeetings) || usage.number(1)+clean.toUtf8().size()>MaxTextBytes){result.limited=true;exec(db.db,"COMMIT");continue;}
            // A renamed folder keeps its inode and identity; if a copied folder
            // replaced a missing source, retain its row id only for exact content.
            qint64 replaceId=oldId;
            if(!replaceId){
                Statement moved(db.db,"SELECT id,source_directory,retention_ms FROM meeting_records WHERE source_root=? AND fingerprint=? LIMIT 2");moved.bind(1,d->rootKey);moved.bind(2,fingerprint);
                if(moved.next()){const auto candidate=moved.number(0);const auto oldPath=moved.text(1);const auto oldRetention=moved.number(2);if(!moved.next() && !QFileInfo::exists(oldPath)){replaceId=candidate;if(!known)retained=oldRetention;}}
            }
            if(replaceId){
                Statement update(db.db,"UPDATE meeting_records SET identity=?,directory_key=?,source_directory=?,manifest_path=?,signature=?,fingerprint=?,title=?,started_ms=?,time_known=?,duration_secs=?,retention_ms=?,transcript=?,text_bytes=? WHERE id=?");
                update.bind(1,identity);update.bind(2,key);update.bind(3,meetingDir);update.bind(4,QDir(meetingDir).filePath(QString::fromUtf8(manifest)));update.bind(5,signature);update.bind(6,fingerprint);update.bind(7,title);update.bind(8,startedMs);update.bind(9,known);update.bind(10,duration);update.bind(11,retained);update.bind(12,clean);update.bind(13,clean.toUtf8().size());update.bind(14,replaceId);update.next();++result.updated;
            }else{
                Statement insert(db.db,"INSERT INTO meeting_records(identity,source_root,directory_key,source_directory,manifest_path,signature,fingerprint,title,started_ms,time_known,duration_secs,retention_ms,transcript,text_bytes) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
                insert.bind(1,identity);insert.bind(2,d->rootKey);insert.bind(3,key);insert.bind(4,meetingDir);insert.bind(5,QDir(meetingDir).filePath(QString::fromUtf8(manifest)));insert.bind(6,signature);insert.bind(7,fingerprint);insert.bind(8,title);insert.bind(9,startedMs);insert.bind(10,known);insert.bind(11,duration);insert.bind(12,retained);insert.bind(13,clean);insert.bind(14,clean.toUtf8().size());insert.next();++result.imported;
            }
            exec(db.db,"COMMIT");
        }catch(...){sqlite3_exec(db.db,"ROLLBACK",nullptr,nullptr,nullptr);throw;}
    }
    if(finished && !result.canceled){
        // Reconcile only a complete, readable root. Unmounted/unavailable source
        // storage must not erase previously imported meeting context.
        QVector<qint64> gone;Statement rows(db.db,"SELECT id,directory_key FROM meeting_records WHERE source_root=?");rows.bind(1,d->rootKey);
        while(rows.next())if(!d->seen.contains(rows.text(1)))gone.append(rows.number(0));
        if(!d->stopped())for(auto id:gone){d->checkArchive();eraseRow(db.db,id,false);++result.removed;}
        QSet<QString> stale;for(auto i=d->stable.cbegin();i!=d->stable.cend();++i)if(!d->seen.contains(i.key()))stale.insert(i.key());
        for(const auto &key:stale){d->stable.remove(key);d->invalid.remove(key);}
        d->end();
    }
    result.more=d->scan!=nullptr;
    return result;
}
} // namespace replay
