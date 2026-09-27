#include "recorder.h"
#include "meeting_index.h"
#include "selection_ocr.h"
#include "work_budget.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QThread>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <omp.h>
#include <sqlite3.h>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>
#include <tesseract/resultiterator.h>
#include <leptonica/allheaders.h>
#include <webp/encode.h>
#include <webp/decode.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <ctime>
#include <limits>
#include <stdexcept>
#ifdef __GLIBC__
#include <malloc.h>
#endif

namespace replay {
namespace {
constexpr quint64 MiB = 1024 * 1024;
constexpr quint64 IndexReserve = 8 * MiB;
constexpr qint64 ProcessTimeoutMs = 30000;
constexpr int FingerprintRows = 32;
constexpr int TileWidth = 128, TileHeight = 64, MaxOcrRegions = 8;
constexpr int MaxOcrReuseEntries = 256;

class SerialOcrScope {
    int previousLevels = omp_get_max_active_levels();
public:
    SerialOcrScope() { omp_set_max_active_levels(0); }
    ~SerialOcrScope() { omp_set_max_active_levels(previousLevels); }
    SerialOcrScope(const SerialOcrScope &) = delete;
    SerialOcrScope &operator=(const SerialOcrScope &) = delete;
};

struct ImageFingerprint {
    QSize size;
    QVector<QByteArray> rows;
    QVector<QByteArray> tiles;
    int tileColumns = 0;
    QByteArray digest;
};

ImageFingerprint fingerprint(const QImage &image, bool regions, bool tiled = false) {
    ImageFingerprint result;
    result.size = image.size();
    QCryptographicHash whole(QCryptographicHash::Sha256);
    whole.addData(QByteArray::number(image.width()) + 'x' + QByteArray::number(image.height()));
    if (tiled) {
        result.tileColumns = (image.width() + TileWidth - 1) / TileWidth;
        for (int top = 0; top < image.height(); top += TileHeight) {
            for (int left = 0; left < image.width(); left += TileWidth) {
                QCryptographicHash tile(QCryptographicHash::Sha256);
                const int width = std::min(TileWidth, image.width() - left);
                for (int row = top; row < std::min(top + TileHeight, image.height()); ++row)
                    tile.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(row) + left * 4), width * 4));
                result.tiles.append(tile.result());
                whole.addData(result.tiles.last());
            }
        }
        result.digest = whole.result();
        return result;
    }
    if (!regions) {
        for (int row = 0; row < image.height(); ++row)
            whole.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(row)), image.width() * 4));
        result.digest = whole.result();
        return result;
    }
    for (int top = 0; top < image.height(); top += FingerprintRows) {
        QCryptographicHash band(QCryptographicHash::Sha256);
        for (int row = top; row < std::min(top + FingerprintRows, image.height()); ++row)
            band.addData(QByteArrayView(reinterpret_cast<const char *>(image.constScanLine(row)), image.width() * 4));
        result.rows.append(band.result());
        whole.addData(result.rows.last());
    }
    result.digest = whole.result();
    return result;
}

struct TextLine { QRect box; QString text; };
struct RecognizedText {
    QString text;
    QVector<TextLine> lines;
    bool geometryComplete = true;
};

[[noreturn]] void error(const QString &message) { throw std::runtime_error(message.toStdString()); }
double cpuMs() { return 1000.0 * double(std::clock()) / CLOCKS_PER_SEC; }
QString dbPath(const QString &directory) { return QDir(directory).filePath("index.sqlite"); }

class SqliteError : public std::runtime_error {
public:
    const int code;
    SqliteError(const QString &message, int extendedCode) : std::runtime_error(message.toStdString()), code(extendedCode) {}
    bool contention() const { return (code & 0xff) == SQLITE_BUSY || (code & 0xff) == SQLITE_LOCKED; }
};

struct Database {
    sqlite3 *handle = nullptr;
    Database(const QString &path, bool create, bool writable = false) {
        const int flags = create ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : writable ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY;
        if (sqlite3_open_v2(path.toUtf8().constData(), &handle, flags, nullptr) != SQLITE_OK) {
            const QString message = handle ? QString::fromUtf8(sqlite3_errmsg(handle)) : "allocation failed";
            if (handle) sqlite3_close(handle);
            handle = nullptr;
            error("Cannot open recall index: " + message);
        }
        sqlite3_extended_result_codes(handle, 1);
        sqlite3_busy_timeout(handle, 1000);
    }
    ~Database() { if (handle) sqlite3_close(handle); }
    void exec(const char *sql) {
        char *message = nullptr;
        if (sqlite3_exec(handle, sql, nullptr, nullptr, &message) != SQLITE_OK) {
            const QString detail = QString::fromUtf8(message ? message : sqlite3_errmsg(handle));
            const int code = sqlite3_extended_errcode(handle);
            sqlite3_free(message);
            throw SqliteError("Recall index: " + detail, code);
        }
    }
};

struct Statement {
    sqlite3_stmt *handle = nullptr;
    explicit Statement(Database &db, const char *sql) {
        if (sqlite3_prepare_v2(db.handle, sql, -1, &handle, nullptr) != SQLITE_OK)
            throw SqliteError("Recall query: " + QString::fromUtf8(sqlite3_errmsg(db.handle)), sqlite3_extended_errcode(db.handle));
    }
    ~Statement() { sqlite3_finalize(handle); }
    void bind(int column, qint64 value) { sqlite3_bind_int64(handle, column, value); }
    void bind(int column, const QString &value) {
        const QByteArray utf8 = value.toUtf8();
        sqlite3_bind_text(handle, column, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
    }
    bool next() {
        const int result = sqlite3_step(handle);
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) {
            const auto db = sqlite3_db_handle(handle);
            throw SqliteError("Recall index write/read failed: " + QString::fromUtf8(sqlite3_errmsg(db)), sqlite3_extended_errcode(db));
        }
        return false;
    }
    qint64 number(int column) const { return sqlite3_column_int64(handle, column); }
    QString string(int column) const {
        const auto *value = sqlite3_column_text(handle, column);
        return value ? QString::fromUtf8(reinterpret_cast<const char *>(value)) : QString();
    }
};

quint64 metadataNumber(Database &db, const QString &key) {
    Statement query(db, "SELECT value FROM metadata WHERE key=?"); query.bind(1, key);
    if (!query.next()) return 0;
    return query.string(0).toULongLong();
}

void setMetadata(Database &db, const QString &key, quint64 value) {
    Statement statement(db, "INSERT INTO metadata(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    statement.bind(1, key); statement.bind(2, QString::number(value)); statement.next();
}

void adjustMetadata(Database &db, const QString &key, qint64 change) {
    if (change == 0) return;
    const quint64 old = metadataNumber(db, key);
    if ((change < 0 && quint64(-change) > old) || (change > 0 && old > quint64(std::numeric_limits<qint64>::max() - change)))
        error("History accounting is inconsistent");
    setMetadata(db, key, change < 0 ? old - quint64(-change) : old + quint64(change));
}

QString safeMediaPath(const QString &directory, const QString &relative) {
    const QString base = QFileInfo(directory).canonicalFilePath();
    const QFileInfo file(QDir(directory).filePath(relative));
    const QString canonical = file.canonicalFilePath();
    if (base.isEmpty() || canonical.isEmpty() ||
        (!canonical.startsWith(base + "/media/") && !canonical.startsWith(base + "/staging/")))
        error("Media is missing or outside this dataset");
    return canonical;
}

QVector<FrameRecord> readRows(Statement &query) {
    QVector<FrameRecord> result;
    while (query.next()) {
        FrameRecord frame;
        frame.id = query.number(0);
        frame.timestampMs = query.number(1);
        frame.lastTimestampMs = query.number(2);
        frame.observationCount = query.number(3);
        frame.frameIndex = query.number(4);
        frame.text = query.string(5);
        frame.path = query.string(6);
        frame.codec = query.string(7);
        frame.width = int(query.number(8));
        frame.height = int(query.number(9));
        frame.archiveAvailable = query.number(10) != 0;
        frame.ocrState = query.string(11);
        frame.ocrError = query.string(12);
        frame.originalPath = query.string(13);
        frame.available = frame.archiveAvailable || !frame.originalPath.isEmpty();
        result.append(frame);
    }
    return result;
}

bool hasIndexStates(Database &db) {
    Statement columns(db, "PRAGMA table_info(frames)");
    while (columns.next()) if (columns.string(1) == "ocr_state") return true;
    return false;
}

bool hasSchedule(Database &db) {
    Statement table(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='index_schedule'");
    return table.next();
}

void ensureRecallGeometry(Database &db) {
    // Optional extension: historical rows remain untouched, and old viewers
    // continue to read the original frame/FTS schema.
    db.exec("CREATE TABLE IF NOT EXISTS frame_ocr_geometry(frame_id INTEGER PRIMARY KEY REFERENCES frames(id) ON DELETE CASCADE,"
            "lines_json TEXT NOT NULL);"
            "CREATE INDEX IF NOT EXISTS frame_timestamp ON frames(timestamp_ms,id)");
}

void ensureOcrReuse(Database &db) {
    // References only: text/geometry keep their existing per-frame limits. Never
    // infer provenance for historical rows or for merged incremental results.
    db.exec("CREATE TABLE IF NOT EXISTS ocr_reuse("
            "pixel_key TEXT NOT NULL,profile_key TEXT NOT NULL,frame_id INTEGER NOT NULL,"
            "result_key TEXT NOT NULL,PRIMARY KEY(pixel_key,profile_key));"
            "CREATE INDEX IF NOT EXISTS ocr_reuse_frame ON ocr_reuse(frame_id);"
            "CREATE TRIGGER IF NOT EXISTS ocr_reuse_delete AFTER DELETE ON frames BEGIN "
            "DELETE FROM ocr_reuse WHERE frame_id=OLD.id; END;"
            "CREATE TRIGGER IF NOT EXISTS ocr_reuse_change AFTER UPDATE OF text,ocr_state,width,height,path,codec ON frames BEGIN "
            "DELETE FROM ocr_reuse WHERE frame_id=OLD.id; END;"
            "CREATE TRIGGER IF NOT EXISTS ocr_reuse_geometry_delete AFTER DELETE ON frame_ocr_geometry BEGIN "
            "DELETE FROM ocr_reuse WHERE frame_id=OLD.frame_id; END;"
            "CREATE TRIGGER IF NOT EXISTS ocr_reuse_geometry_change AFTER UPDATE ON frame_ocr_geometry BEGIN "
            "DELETE FROM ocr_reuse WHERE frame_id=OLD.frame_id; END;");
}

QString ocrResultKey(const QString &text, const QString &geometry) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray bytes = text.toUtf8();
    hash.addData(QByteArray::number(bytes.size()) + ':');
    hash.addData(bytes); hash.addData(geometry.toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}

QString serializedGeometry(const QVector<TextLine> &lines, const QSize &ocrSize, const QSize &originalSize) {
    if (ocrSize.isEmpty() || originalSize.isEmpty()) return "[]";
    QJsonArray result;
    for (const auto &line : lines) {
        // Round outward so resized recognition still covers the original ink.
        const int left = int(qint64(line.box.x()) * originalSize.width() / ocrSize.width());
        const int top = int(qint64(line.box.y()) * originalSize.height() / ocrSize.height());
        const int right = int((qint64(line.box.x() + line.box.width()) * originalSize.width() + ocrSize.width() - 1) / ocrSize.width());
        const int bottom = int((qint64(line.box.y() + line.box.height()) * originalSize.height() + ocrSize.height() - 1) / ocrSize.height());
        const QRect box = QRect(left, top, right - left, bottom - top).intersected(QRect(QPoint(), originalSize));
        if (!box.isEmpty()) result.append(QJsonArray{box.x(), box.y(), box.width(), box.height(), line.text});
    }
    const QByteArray json = QJsonDocument(result).toJson(QJsonDocument::Compact);
    // Geometry is optional. A pathological page must not prevent text indexing.
    return json.size() <= 3 * 1024 * 1024 ? QString::fromUtf8(json) : QString("[]");
}

void storeGeometry(Database &db, qint64 frameId, const QString &geometry) {
    Statement insert(db, "INSERT OR REPLACE INTO frame_ocr_geometry(frame_id,lines_json) VALUES(?,?)");
    insert.bind(1, frameId); insert.bind(2, geometry); insert.next();
}

QStringList searchTerms(const QString &text) {
    QStringList words;
    auto matches = QRegularExpression("[\\p{L}\\p{N}_][\\p{L}\\p{N}\\p{M}_]*", QRegularExpression::UseUnicodePropertiesOption).globalMatch(text.left(4096));
    while (matches.hasNext() && words.size() < 32) words.append(matches.next().captured());
    return words;
}

QString searchExpression(const QStringList &terms, bool prefixLast = false) {
    QStringList quoted;
    for (const auto &term : terms) quoted.append('"' + term + '"');
    if (prefixLast && !quoted.isEmpty()) quoted.last().append('*');
    return quoted.join(" AND ");
}

// Reuse FTS5's actual tokenizer rather than approximating its case, accent and
// separator rules in the overlay. Quoted query terms can contain multiple FTS
// tokens (for example an underscore), which must remain adjacent on the line.
class SearchTokenizer {
    fts5_tokenizer tokenizer{};
    Fts5Tokenizer *instance = nullptr;
public:
    explicit SearchTokenizer(Database &db) {
        fts5_api *api = nullptr;
        Statement lookup(db, "SELECT fts5(?1)");
        sqlite3_bind_pointer(lookup.handle, 1, &api, "fts5_api_ptr", nullptr);
        lookup.next();
        void *context = nullptr;
        if (!api || api->xFindTokenizer(api, "unicode61", &context, &tokenizer) != SQLITE_OK ||
            tokenizer.xCreate(context, nullptr, 0, &instance) != SQLITE_OK)
            error("Cannot initialize search highlight tokenizer");
    }
    ~SearchTokenizer() { if (instance) tokenizer.xDelete(instance); }
    QVector<QByteArray> tokens(const QString &text) {
        QVector<QByteArray> result;
        const QByteArray utf8 = text.toUtf8();
        const int status = tokenizer.xTokenize(instance, &result, FTS5_TOKENIZE_DOCUMENT, utf8.constData(), utf8.size(),
            [](void *context, int, const char *token, int bytes, int, int) {
                static_cast<QVector<QByteArray> *>(context)->append(QByteArray(token, bytes));
                return SQLITE_OK;
            });
        if (status != SQLITE_OK) error("Cannot tokenize text for highlighting");
        return result;
    }
};

bool prefixLastTerm(SearchTokenizer &tokenizer, const QStringList &terms, SearchMode mode) {
    if (mode != SearchMode::PrefixLastToken || terms.isEmpty()) return false;
    const auto tokens = tokenizer.tokens(terms.last());
    // unicode61 may split a quoted term such as foo_bar. FTS5's '*' expands
    // only its final token, so the minimum length applies to that token too.
    return !tokens.isEmpty() && QString::fromUtf8(tokens.last()).toUcs4().size() >= 3;
}

QString searchExpression(Database &db, const QStringList &terms, SearchMode mode) {
    if (mode == SearchMode::Exact) return searchExpression(terms);
    SearchTokenizer tokenizer(db);
    return searchExpression(terms, prefixLastTerm(tokenizer, terms, mode));
}

void ensureSchedule(Database &db) {
    db.exec("CREATE TABLE IF NOT EXISTS index_requests(frame_id INTEGER PRIMARY KEY,"
            "request_order INTEGER NOT NULL,rank INTEGER NOT NULL,expires_ms INTEGER NOT NULL);"
            "CREATE TABLE IF NOT EXISTS index_schedule(id INTEGER PRIMARY KEY CHECK(id=1),"
            "priority_streak INTEGER NOT NULL DEFAULT 0,catch_up_until_ms INTEGER NOT NULL DEFAULT 0,"
            "request_order INTEGER NOT NULL DEFAULT 0);"
            "INSERT OR IGNORE INTO index_schedule(id) VALUES(1);");
}

void pruneRequests(Database &db, qint64 now) {
    Statement prune(db, "DELETE FROM index_requests WHERE expires_ms<=? OR NOT EXISTS("
                        "SELECT 1 FROM frames f WHERE f.id=index_requests.frame_id AND f.ocr_state='pending')");
    prune.bind(1, now); prune.next();
}

bool indexerRunning(const QString &directory) {
    const QString path = QDir(directory).filePath(".indexer.lock");
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    const int result = flock(fd, LOCK_EX | LOCK_NB);
    const bool held = result != 0 && (errno == EWOULDBLOCK || errno == EAGAIN);
    if (result == 0) flock(fd, LOCK_UN);
    ::close(fd);
    return held;
}

QByteArray frameColumns(Database &db) {
    return QByteArray("SELECT f.id,f.timestamp_ms,f.last_timestamp_ms,f.observation_count,f.frame_index,"
        "f.text,f.path,f.codec,f.width,f.height,COALESCE(s.complete,1),") +
        (hasIndexStates(db) ? "f.ocr_state,f.ocr_error,f.source_path " : "'ready','','' ") +
        "FROM frames f LEFT JOIN segments s ON s.id=f.segment_id ";
}

quint64 directoryBytes(const QString &path) {
    quint64 bytes = 0;
    const auto entries = QDir(path).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const auto &entry : entries) bytes += std::max<qint64>(0, entry.size());
    return bytes;
}

void syncDirectory(const QString &path) {
    const int descriptor = open(path.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) error("Cannot open dataset directory for synchronization");
    const int status = fsync(descriptor);
    close(descriptor);
    if (status != 0) error("Cannot synchronize dataset directory");
}

void writeImageFile(const QString &path, const QByteArray &bytes, bool durable) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
        (durable && (!file.flush() || fsync(file.handle()) != 0)) || !file.commit())
        error("Recording stopped: cannot publish compressed original image");
    if (durable) syncDirectory(QFileInfo(path).absolutePath());
}

QImage readOriginalImage(const QString &directory, const QString &relative, int width, int height,
                         QByteArray *encodedDigest = nullptr) {
    if (width <= 0 || height <= 0 || qint64(width) * height > 32 * 1024 * 1024)
        error("Original image exceeds decoder limits");
    QFile file(safeMediaPath(directory, relative));
    const qint64 bound = qint64(width) * height * 5 + MiB;
    if (!file.open(QIODevice::ReadOnly) || file.size() < 1 || file.size() > bound) error("Original image cannot be opened within decoder limits");
    // Keep the descriptor open across a concurrent source cleanup. libwebp also
    // handles valid tiny solid-color files rejected by this host's Qt plugin.
    const QByteArray bytes = file.read(bound + 1);
    if (encodedDigest) *encodedDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    int decodedWidth = 0, decodedHeight = 0;
    const auto *data = reinterpret_cast<const uint8_t *>(bytes.constData());
    if (bytes.size() > bound || !WebPGetInfo(data, bytes.size(), &decodedWidth, &decodedHeight) || decodedWidth != width || decodedHeight != height)
        error("Original image dimensions or compressed data do not match its index");
    QImage image(width, height, QImage::Format_RGBA8888);
    if (image.isNull() || !WebPDecodeRGBAInto(data, bytes.size(), image.bits(), image.sizeInBytes(), image.bytesPerLine()))
        error("Original image cannot be decoded");
    return image;
}

void cleanReadySources(Database &db, const QString &directory) {
    if (!hasIndexStates(db)) return;
    QVector<QPair<qint64, QString>> completed;
    {
        Statement query(db, "SELECT f.id,f.source_path FROM frames f INDEXED BY held_sources LEFT JOIN segments s ON s.id=f.segment_id "
            "WHERE f.ocr_state='ready' AND f.source_path!='' AND COALESCE(s.complete,1)=1 LIMIT 1024");
        while (query.next()) completed.append({query.number(0), query.string(1)});
    }
    for (const auto &entry : completed) {
        if (entry.second.startsWith("staging/")) {
            const QString path = QDir(directory).filePath(entry.second);
            // Both the capture process and worker may release a sealed source.
            // Resolve once, and treat a concurrent unlink as already complete.
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (!canonical.isEmpty()) {
                const QString base = QFileInfo(directory).canonicalFilePath() + "/staging/";
                if (!canonical.startsWith(base)) error("Original source is outside the staging directory");
                if (!QFile::remove(canonical) && QFileInfo(canonical).exists()) continue;
            }
        }
        Statement clear(db, "UPDATE frames SET source_path='',source_bytes=0 WHERE id=? AND ocr_state='ready' AND source_path=?");
        clear.bind(1, entry.first); clear.bind(2, entry.second); clear.next();
    }
}

struct SourceUsage { qint64 frames = 0; quint64 bytes = 0, stagedBytes = 0; };

SourceUsage heldSourceUsage(Database &db, const QString &directory) {
    Statement sources(db, "SELECT COUNT(*),COALESCE(SUM(source_bytes),0),"
        "COALESCE(SUM(CASE WHEN source_path LIKE 'media/%' THEN 1 ELSE 0 END),0),"
        "COALESCE(SUM(CASE WHEN source_path LIKE 'media/%' THEN source_bytes ELSE 0 END),0) FROM frames INDEXED BY held_sources WHERE source_path!=''");
    sources.next();
    const auto staged = QDir(QDir(directory).filePath("staging")).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    SourceUsage usage;
    for (const auto &file : staged) usage.stagedBytes += std::max<qint64>(0, file.size());
    usage.frames = std::max<qint64>(sources.number(0), staged.size() + sources.number(2));
    usage.bytes = std::max<quint64>(sources.number(1), usage.stagedBytes + sources.number(3));
    return usage;
}

QByteArray encodeWebP(const QImage &image) {
    WebPConfig config;
    WebPPicture picture;
    WebPMemoryWriter writer;
    if (!WebPConfigInit(&config) || !WebPPictureInit(&picture)) error("WebP initialization failed");
    // In lossless mode quality controls effort; keep the fast method and exact pixels.
    config.lossless = 1; config.quality = 50; config.method = 0; config.exact = 1;
    config.thread_level = 0;
    picture.use_argb = 1; picture.width = image.width(); picture.height = image.height();
    WebPMemoryWriterInit(&writer);
    picture.writer = WebPMemoryWrite; picture.custom_ptr = &writer;
    const bool success = WebPPictureImportRGBA(&picture, image.constBits(), image.bytesPerLine()) && WebPEncode(&config, &picture);
    QByteArray result;
    if (success) result = QByteArray(reinterpret_cast<const char *>(writer.mem), writer.size);
    WebPPictureFree(&picture); WebPMemoryWriterClear(&writer);
    if (!success) error("Lossless WebP encoding failed");
    return result;
}

class Descriptor {
public:
    int fd = -1;
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor() { if (fd >= 0) ::close(fd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};

QString privateHistoryDirectory(const QString &directory) {
    const QFileInfo root(directory);
    if (root.isSymLink() || !root.isDir() || root.ownerId() != getuid()) error("History must be a private local directory");
    const QString absolute = root.absoluteFilePath();
    for (const auto &name : {QString("index.sqlite"), QString("index.sqlite-wal"), QString("index.sqlite-shm"), QString("media"), QString("staging")}) {
        const QFileInfo entry(QDir(absolute).filePath(name));
        if (entry.isSymLink() || (entry.exists() && entry.ownerId() != getuid()))
            error("History contains an unsafe database or media path");
        if (entry.exists() && name.startsWith("index.sqlite")) {
            struct stat status{};
            if (::lstat(QFile::encodeName(entry.absoluteFilePath()).constData(), &status) != 0 ||
                !S_ISREG(status.st_mode) || status.st_nlink != 1)
                error("History database must be a private regular file with a single link");
        }
    }
    if (!QFileInfo(QDir(absolute).filePath("index.sqlite")).isFile() || !QFileInfo(QDir(absolute).filePath("media")).isDir())
        error("History database or media directory is missing");
    return absolute;
}

QString mediaName(const QString &relative) {
    static const QRegularExpression pattern("^media/(frame-[0-9]{8,19}\\.webp)$");
    const auto match = pattern.match(relative);
    if (!match.hasMatch()) error("History media path is outside its managed original-image namespace");
    return match.captured(1);
}

class PrivateMedia {
    Descriptor root;
    Descriptor media;
public:
    explicit PrivateMedia(const QString &directory)
        : root(::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)),
          media(root.fd < 0 ? -1 : ::openat(root.fd, "media", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)) {
        if (root.fd < 0 || media.fd < 0) error("Cannot open private history media directory");
    }
    qint64 size(const QString &relative, bool missingAllowed = false) const {
        const QByteArray name = QFile::encodeName(mediaName(relative));
        struct stat status{};
        if (fstatat(media.fd, name.constData(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
            if (missingAllowed && errno == ENOENT) return -1;
            error("History original is missing or unreadable");
        }
        if (!S_ISREG(status.st_mode) || status.st_uid != getuid() || status.st_nlink != 1)
            error("History original is not a private regular file");
        return status.st_size;
    }
    void write(const QString &relative, const QByteArray &bytes) const {
        const QByteArray name = QFile::encodeName(mediaName(relative));
        Descriptor file(::openat(media.fd, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        if (file.fd < 0) error("Cannot create a new history original without replacing existing data");
        qsizetype written = 0;
        while (written < bytes.size()) {
            const auto result = ::write(file.fd, bytes.constData() + written, bytes.size() - written);
            if (result < 0 && errno == EINTR) continue;
            if (result <= 0) error("Cannot write history original");
            written += result;
        }
        if (::fsync(file.fd) != 0 || ::fsync(media.fd) != 0) error("Cannot synchronize history original");
    }
    qint64 remove(const QString &relative) const {
        const auto bytes = size(relative, true);
        if (bytes < 0) return -1; // A crash may have occurred after unlink and before its receipt.
        const QByteArray name = QFile::encodeName(mediaName(relative));
        if (::unlinkat(media.fd, name.constData(), 0) != 0) {
            if (errno == ENOENT) return -1;
            error("Cannot remove retired history original");
        }
        if (::fsync(media.fd) != 0) error("Cannot synchronize retired history original");
        return bytes;
    }
};

void requireHistory(Database &db) {
    if (metadataNumber(db, "history_version") != 1 || metadataNumber(db, "archive_first") != 1)
        error("This operation requires an initialized persistent archive-first history");
}

quint64 historyDiskBytes(Database &db, const QString &directory) {
    quint64 bytes = metadataNumber(db, "history_media_bytes");
    for (const char *suffix : {"", "-wal", "-shm"}) bytes += std::max<qint64>(0, QFileInfo(dbPath(directory) + suffix).size());
    return bytes;
}

qint64 allocateHistoryId(Database &db, const QString &key) {
    const auto next = metadataNumber(db, key);
    if (next == 0 || next >= quint64(std::numeric_limits<qint64>::max())) error("History identifier space is exhausted or invalid");
    setMetadata(db, key, next + 1);
    return qint64(next);
}

void initializeHistory(Database &db, const QString &directory) {
    if (metadataNumber(db, "history_version") == 1) { requireHistory(db); return; }
    if (metadataNumber(db, "archive_first") != 1 || metadataNumber(db, "schema_version") != 2)
        error("Only an archive-first WebP dataset can become persistent history");
    {
        Statement incompatible(db, "SELECT 1 FROM frames WHERE codec<>'webp' OR segment_id IS NOT NULL OR "
                                  "(source_path<>'' AND source_path<>path) LIMIT 1");
        if (incompatible.next()) error("Persistent history contains incompatible capture records");
    }
    if (!QDir(directory + "/staging").entryList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
        error("Persistent archive-first history must not contain staged video sources");
    PrivateMedia media(directory);
    db.exec("BEGIN IMMEDIATE");
    try {
        db.exec("CREATE TABLE history_media(path TEXT PRIMARY KEY,bytes INTEGER NOT NULL CHECK(bytes>=0),"
                "state TEXT NOT NULL CHECK(state IN('writing','live','retired')));"
                "CREATE INDEX history_media_state ON history_media(state,path);"
                "CREATE UNIQUE INDEX history_frame_path ON frames(path);"
                "CREATE INDEX observation_frame_time ON observations(frame_id,timestamp_ms,id);"
                "CREATE TABLE history_gaps(id INTEGER PRIMARY KEY AUTOINCREMENT,start_ms INTEGER NOT NULL,end_ms INTEGER NOT NULL,reason TEXT NOT NULL);"
                "CREATE INDEX history_gap_end ON history_gaps(end_ms,id)");
        quint64 total = 0, next = 1;
        // One migration scan inventories old prototype originals and orphans.
        // Future captures reserve every filename/byte before creating a file,
        // so crash recovery and per-frame accounting need no directory scan.
        const QDir folder(directory + "/media");
        for (const auto &name : folder.entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
            const QString path = "media/" + name;
            const auto size = media.size(path);
            bool valid = false;
            const auto number = name.mid(6, name.size() - 11).toULongLong(&valid);
            if (!valid || number >= quint64(std::numeric_limits<qint64>::max())) error("History archive filename is invalid");
            next = std::max(next, number + 1);
            total += quint64(size);
            Statement insert(db, "INSERT INTO history_media(path,bytes,state) VALUES(?,?,CASE WHEN EXISTS(SELECT 1 FROM frames WHERE path=?) THEN 'live' ELSE 'retired' END)");
            insert.bind(1, path); insert.bind(2, size); insert.bind(3, path); insert.next();
        }
        Statement frames(db, "SELECT COALESCE(MAX(id),0),COUNT(*) FROM frames"); frames.next();
        setMetadata(db, "history_next_frame", std::max(next, quint64(frames.number(0)) + 1));
        setMetadata(db, "history_frames", frames.number(1));
        Statement observations(db, "SELECT COALESCE(MAX(id),0),COUNT(*) FROM observations"); observations.next();
        setMetadata(db, "history_next_observation", quint64(observations.number(0)) + 1);
        setMetadata(db, "history_observations", observations.number(1));
        setMetadata(db, "history_media_bytes", total);
        Statement missing(db, "SELECT 1 FROM frames LEFT JOIN history_media USING(path) WHERE history_media.path IS NULL LIMIT 1");
        if (missing.next()) error("Cannot resume history with missing originals");
        setMetadata(db, "history_version", 1);
        db.exec("COMMIT");
    } catch (...) { sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}

void collectRetiredMedia(Database &db, const QString &directory, int limit, HistoryMaintenanceResult &result) {
    PrivateMedia media(directory);
    QVector<QPair<QString, qint64>> retired;
    {
        Statement rows(db, "SELECT path,bytes FROM history_media WHERE state='retired' ORDER BY path LIMIT ?");
        rows.bind(1, limit);
        while (rows.next()) retired.append({rows.string(0), rows.number(1)});
    }
    if (retired.isEmpty()) return;
    for (const auto &entry : retired) {
        {
            Statement used(db, "SELECT 1 FROM frames WHERE path=? UNION ALL "
                               "SELECT 1 FROM frames INDEXED BY held_sources WHERE source_path=? AND source_path<>'' LIMIT 1");
            used.bind(1, entry.first); used.bind(2, entry.first);
            if (used.next()) error("Refusing to remove a history original that is still referenced");
        }
        // Retired names are never reused. Unlink/fsync need not hold the writer
        // transaction; a crash leaves a durable receipt to finish next time.
        const auto reclaimed = media.remove(entry.first);
        if (reclaimed >= 0) { ++result.filesRemoved; result.bytesReclaimed += quint64(reclaimed); }
    }
    db.exec("BEGIN IMMEDIATE");
    try {
        for (const auto &entry : retired) {
            Statement erase(db, "DELETE FROM history_media WHERE path=? AND state='retired'");
            erase.bind(1, entry.first); erase.next();
            if (sqlite3_changes(db.handle) == 1) adjustMetadata(db, "history_media_bytes", -entry.second);
        }
        db.exec("COMMIT");
    } catch (...) { sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
}
} // namespace

struct OcrEngine {
    RecorderOptions options;
    std::unique_ptr<tesseract::TessBaseAPI> ocr;
    ImageFingerprint lastFingerprint;
    QVector<TextLine> cachedLines;
    bool cachedGeometryComplete = false;
    int consecutivePartialFrames = 0;
    qint64 ocrFullFrames = 0, ocrPartialFrames = 0, ocrPartialFallbacks = 0;
    qint64 ocrProcessedPixels = 0, ocrInputPixels = 0;
    qint64 ocrOriginalInputPixels = 0, ocrResizedFrames = 0;
    double ocrResizeWallMs = 0, ocrResizeCpuMs = 0;
    qint64 ocrFullPassAttempts = 0, ocrRegionComparisons = 0;
    qint64 ocrChangedBandPixels = 0, ocrChangedSpanPixels = 0, ocrCandidateRegionPixels = 0;
    qint64 ocrRegionsFrames = 0, ocrRegionsAttempted = 0, ocrRegionsUsed = 0, ocrRegionsFallbacks = 0;
    qint64 ocrRegionsCandidatePixels = 0, ocrRegionsAttemptedPixels = 0, ocrRegionsUsedPixels = 0;
    qint64 ocrRegionsChangedTiles = 0;
    qint64 ocrRegionsChangedTilePixels = 0, ocrRegionsComponentPixels = 0, ocrRegionsGeometryPixels = 0;
    qint64 ocrRegionsPaddingClamps = 0;
    QJsonObject ocrFullReasons;
    quint64 ocrBudgetCheckpoints = 0, ocrBudgetSleeps = 0, ocrBudgetCancellations = 0, ocrBudgetDeadlines = 0;
    double ocrBudgetSleepMs = 0, ocrMaxCallbackWallGapMs = 0, ocrMaxCallbackCpuGapMs = 0;
    qint64 lastAllocatorTrimMs = -250, allocatorTrimAttempts = 0, allocatorTrimReleases = 0;
    double allocatorTrimCpuMs = 0, allocatorTrimWallMs = 0;
    double initOcrMs = 0;
    QElapsedTimer lifetime;
    explicit OcrEngine(const RecorderOptions &opts) : options(opts) {
        options.ocrLanguages = options.ocrLanguages.trimmed();
        if (options.ocrLanguages.isEmpty()) options.ocrLanguages = QStringLiteral("eng");
        if (!validOcrLanguages(options.ocrLanguages))
            error("OCR languages must be Tesseract language names joined by +, such as eng+fra");
        if (options.ocrMaxHeight != 0 && (options.ocrMaxHeight < 256 || options.ocrMaxHeight > 8192))
            error("OCR maximum height must be 0 or between 256 and 8192 pixels");
        if (!options.ocrDataPath.isEmpty()) {
            for (const QString &language : options.ocrLanguages.split('+')) {
                const QFileInfo model(QDir(options.ocrDataPath).filePath(language + ".traineddata"));
                if (!QFileInfo(options.ocrDataPath).isDir() || !model.isFile() || !model.isReadable() || model.size() <= 0)
                    error(QString("OCR data path must contain a readable, nonempty %1.traineddata model").arg(language));
            }
            options.ocrDataPath = QFileInfo(options.ocrDataPath).absoluteFilePath();
        }
        for (const char *reason : {"mode", "initial", "size", "geometry", "refresh", "unchanged_pixels",
                                   "broad_change", "crop_edge", "crop_geometry", "cache_size", "region_count", "region_geometry"})
            ocrFullReasons.insert(QString::fromLatin1(reason), 0);
        lifetime.start();
    }
    void initializeOcr() {
        // libgomp reads environment limits before main; Tesseract explicitly
        // requests four threads. Serialize its regions through the runtime API,
        // then restore this calling task's setting when OCR work ends.
        const SerialOcrScope serial;
        QElapsedTimer timer; timer.start();
        auto initialized = std::make_unique<tesseract::TessBaseAPI>();
        const QByteArray dataPath = QFile::encodeName(options.ocrDataPath);
        const QByteArray languages = options.ocrLanguages.toUtf8();
        if (initialized->Init(dataPath.isEmpty() ? nullptr : dataPath.constData(), languages.constData(), tesseract::OEM_LSTM_ONLY) != 0)
            error(options.ocrDataPath.isEmpty()
                      ? QString("Cannot initialize the Tesseract %1 model; install its traineddata (e.g. tesseract-data-eng) or use --no-ocr").arg(options.ocrLanguages)
                      : QString("Cannot initialize the Tesseract %1 model from the explicit OCR data path").arg(options.ocrLanguages));
        initialized->SetPageSegMode(tesseract::PSM_AUTO);
        ocr = std::move(initialized);
        initOcrMs = timer.nsecsElapsed() / 1e6;
    }
    QImage prepareOcrImage(const QImage &original) {
        ocrOriginalInputPixels += qint64(original.width()) * original.height();
        if (!options.ocrMaxHeight || original.height() <= options.ocrMaxHeight) return original;
        QElapsedTimer timer; timer.start();
        const double cpuStart = cpuMs();
        const QImage resized = original.scaledToHeight(options.ocrMaxHeight, Qt::SmoothTransformation)
                                      .convertToFormat(QImage::Format_RGBA8888);
        if (resized.isNull()) error("Cannot allocate resized OCR input");
        ++ocrResizedFrames;
        ocrResizeWallMs += timer.nsecsElapsed() / 1e6;
        ocrResizeCpuMs += cpuMs() - cpuStart;
        return resized;
    }
    void countFullPass(const char *reason) {
        ++ocrFullPassAttempts;
        const QString key = QString::fromLatin1(reason);
        ocrFullReasons.insert(key, ocrFullReasons.value(key).toInteger() + 1);
    }
    void addExperimentStats(QJsonObject &stats) const {
        stats.insert("ocr_data_path", options.ocrDataPath);
        stats.insert("ocr_max_height", options.ocrMaxHeight);
        stats.insert("ocr_original_input_pixels", ocrOriginalInputPixels);
        stats.insert("ocr_resized_frames", ocrResizedFrames);
        stats.insert("ocr_resize_wall_ms", ocrResizeWallMs);
        stats.insert("ocr_resize_cpu_ms", ocrResizeCpuMs);
        stats.insert("ocr_full_pass_attempts", ocrFullPassAttempts);
        stats.insert("ocr_region_comparisons", ocrRegionComparisons);
        stats.insert("ocr_changed_band_pixels", ocrChangedBandPixels);
        stats.insert("ocr_changed_span_pixels", ocrChangedSpanPixels);
        stats.insert("ocr_candidate_region_pixels", ocrCandidateRegionPixels);
        stats.insert("ocr_regions_frames", ocrRegionsFrames);
        stats.insert("ocr_regions_attempted", ocrRegionsAttempted);
        stats.insert("ocr_regions_used", ocrRegionsUsed);
        stats.insert("ocr_regions_fallbacks", ocrRegionsFallbacks);
        stats.insert("ocr_regions_candidate_pixels", ocrRegionsCandidatePixels);
        stats.insert("ocr_regions_attempted_pixels", ocrRegionsAttemptedPixels);
        stats.insert("ocr_regions_used_pixels", ocrRegionsUsedPixels);
        stats.insert("ocr_regions_changed_tiles", ocrRegionsChangedTiles);
        stats.insert("ocr_regions_changed_tile_pixels", ocrRegionsChangedTilePixels);
        stats.insert("ocr_regions_component_pixels", ocrRegionsComponentPixels);
        stats.insert("ocr_regions_geometry_pixels", ocrRegionsGeometryPixels);
        stats.insert("ocr_regions_padding_clamps", ocrRegionsPaddingClamps);
        for (auto it = ocrFullReasons.constBegin(); it != ocrFullReasons.constEnd(); ++it)
            stats.insert("ocr_full_reason_" + it.key(), it.value());
    }
    void clearOcr() {
        ocr->Clear();
#ifdef __GLIBC__
        // Alternating large pages and small bands leaves freed OCR buffers in
        // glibc arenas. Return excess resident pages after work, without changing
        // global allocator settings or rebuilding the warm recognition model.
        if (lifetime.elapsed() - lastAllocatorTrimMs < 250 || mallinfo2().fordblks < 32 * MiB) return;
        QElapsedTimer timer; timer.start();
        const double cpuStart = cpuMs();
        ++allocatorTrimAttempts;
        allocatorTrimReleases += malloc_trim(8 * MiB) != 0;
        allocatorTrimCpuMs += cpuMs() - cpuStart;
        allocatorTrimWallMs += timer.nsecsElapsed() / 1e6;
        lastAllocatorTrimMs = lifetime.elapsed();
#endif
    }

    WorkBudgetOptions budgetOptions() const {
        WorkBudgetOptions limits;
        limits.cpuPercent = options.ocrCpuPercent;
        limits.dynamicCpuPercent = options.ocrCpuPercentProvider;
        limits.maxWallMs = options.ocrMaxWallMs;
        return limits;
    }
    struct CollectBudget {
        OcrEngine &owner;
        const WorkBudget *budget;
        ~CollectBudget() {
            if (!budget) return;
            const auto &stats = budget->stats();
            owner.ocrBudgetCheckpoints += stats.checkpoints;
            owner.ocrBudgetSleeps += stats.sleepCount;
            owner.ocrBudgetSleepMs += stats.sleepMs;
            owner.ocrBudgetCancellations += stats.cancellationRequested;
            owner.ocrBudgetDeadlines += stats.deadlineExceeded;
            owner.ocrMaxCallbackWallGapMs = std::max(owner.ocrMaxCallbackWallGapMs, stats.maxCallbackWallGapMs);
            owner.ocrMaxCallbackCpuGapMs = std::max(owner.ocrMaxCallbackCpuGapMs, stats.maxCallbackCpuGapMs);
        }
    };

    RecognizedText recognize(const QImage &image, const QRect &area, WorkBudget *sharedBudget = nullptr) {
        const SerialOcrScope serial;
        std::optional<WorkBudget> localBudget;
        if (!sharedBudget) localBudget.emplace(budgetOptions(), options.stopRequested);
        WorkBudget &budget = sharedBudget ? *sharedBudget : *localBudget;
        CollectBudget collect{*this, sharedBudget ? nullptr : &budget};
        tesseract::ETEXT_DESC monitor;
        monitor.cancel_this = &budget;
        monitor.progress_callback2 = [](tesseract::ETEXT_DESC *state, int, int, int, int) {
            return static_cast<WorkBudget *>(state->cancel_this)->checkpoint();
        };
        monitor.cancel = [](void *state, int) { return static_cast<WorkBudget *>(state)->shouldCancel(); };
        if (!budget.checkpoint()) {
            if (budget.stats().cancellationRequested) throw OcrCancelled("Text recognition canceled by user");
            error("Recording stopped before text recognition");
        }
        monitor.set_deadline_msecs(sharedBudget
            ? std::max(1, options.ocrMaxWallMs - int(std::ceil(budget.stats().elapsedWallMs))) : options.ocrMaxWallMs);
        // Pass the original rows directly. SetImage copies only this band rather
        // than copying an entire frame and then applying SetRectangle.
        ocrProcessedPixels += qint64(area.width()) * area.height();
        ocr->SetImage(image.constScanLine(area.top()) + area.left() * 4, area.width(), area.height(), 4, image.bytesPerLine());
        ocr->SetPageSegMode(tesseract::PSM_AUTO);
        const int status = ocr->Recognize(&monitor);
        // Poll even when Tesseract's own deadline returned an error, so budget
        // statistics and user cancellation remain accurately classified.
        const bool budgetStopped = budget.shouldCancel();
        const bool budgetContinues = !budgetStopped && budget.checkpoint();
        if (status != 0 || !budgetContinues) {
            clearOcr();
            if (budget.stats().cancellationRequested) throw OcrCancelled("Text recognition canceled by user");
            error("Recording stopped: text recognition was canceled or exceeded its work deadline");
        }
        RecognizedText result;
        std::unique_ptr<char[]> text(ocr->GetUTF8Text());
        if (!text) error("Text recognition failed");
        result.text = QString::fromUtf8(text.get());
        std::unique_ptr<tesseract::ResultIterator> iterator(ocr->GetIterator());
        if (iterator) {
            qsizetype cachedCharacters = 0;
            do {
                std::unique_ptr<char[]> lineText(iterator->GetUTF8Text(tesseract::RIL_TEXTLINE));
                if (!lineText || QString::fromUtf8(lineText.get()).trimmed().isEmpty()) continue;
                int left, top, right, bottom;
                if (!iterator->BoundingBox(tesseract::RIL_TEXTLINE, &left, &top, &right, &bottom) ||
                    left < 0 || top < 0 || right > area.width() || bottom > area.height() || right <= left || bottom <= top) {
                    result.geometryComplete = false;
                    break;
                }
                const QString line = QString::fromUtf8(lineText.get()).trimmed();
                cachedCharacters += line.size();
                if (result.lines.size() >= 8000 || cachedCharacters > 2 * 1024 * 1024) {
                    result.geometryComplete = false;
                    break;
                }
                result.lines.append({QRect(left + area.left(), top + area.top(), right - left, bottom - top), line});
            } while (iterator->Next(tesseract::RIL_TEXTLINE));
        } else if (!result.text.trimmed().isEmpty()) result.geometryComplete = false;
        clearOcr();
        return result;
    }

    QRect changedBand(const QImage &image, const ImageFingerprint &current, const char *&fullReason) {
        const QRect entire = image.rect();
        if (lastFingerprint.digest.isEmpty()) { fullReason = "initial"; return entire; }
        if (lastFingerprint.size != current.size || lastFingerprint.rows.size() != current.rows.size())
            { fullReason = "size"; return entire; }
        if (!cachedGeometryComplete) { fullReason = "geometry"; return entire; }
        if (consecutivePartialFrames >= 29) { fullReason = "refresh"; return entire; }
        ++ocrRegionComparisons;
        int top = image.height(), bottom = 0;
        for (qsizetype row = 0; row < current.rows.size(); ++row) {
            if (current.rows[row] == lastFingerprint.rows[row]) continue;
            top = std::min(top, int(row) * FingerprintRows);
            bottom = std::max(bottom, std::min(image.height(), int(row + 1) * FingerprintRows));
            ocrChangedBandPixels += qint64(image.width()) * std::min(FingerprintRows, image.height() - int(row) * FingerprintRows);
        }
        // Different original pixels can become identical after optional resizing.
        if (bottom <= top) { fullReason = "unchanged_pixels"; return entire; }
        ocrChangedSpanPixels += qint64(image.width()) * (bottom - top);
        QRect area(0, std::max(0, top - 32), image.width(), std::min(image.height(), bottom + 32) - std::max(0, top - 32));
        bool expanded;
        do {
            expanded = false;
            for (const auto &line : cachedLines) {
                if (!area.intersects(line.box)) continue;
                const QRect padded = line.box.adjusted(0, -8, 0, 8).intersected(entire);
                if (area.contains(padded)) continue;
                const QRect combined = area.united(QRect(0, padded.top(), image.width(), padded.height()));
                expanded = combined != area;
                area = combined;
            }
        } while (expanded && area.height() < image.height());
        // Widely spread changes (including scrolling) are cheaper and safer as
        // a normal page pass. There is only one region and no pending ROI queue.
        ocrCandidateRegionPixels += qint64(area.width()) * area.height();
        if (qint64(area.height()) * 100 > qint64(image.height()) * 45)
            { fullReason = "broad_change"; return entire; }
        fullReason = nullptr;
        return area;
    }

    QVector<QRect> changedRegions(const QImage &image, const ImageFingerprint &current, const char *&fullReason) {
        if (lastFingerprint.digest.isEmpty()) { fullReason = "initial"; return {}; }
        if (lastFingerprint.size != current.size || lastFingerprint.tileColumns != current.tileColumns ||
            lastFingerprint.tiles.size() != current.tiles.size()) { fullReason = "size"; return {}; }
        if (!cachedGeometryComplete) { fullReason = "geometry"; return {}; }
        if (consecutivePartialFrames >= 29) { fullReason = "refresh"; return {}; }
        QVector<quint8> changed(current.tiles.size(), 0);
        int changedCount = 0;
        for (qsizetype i = 0; i < current.tiles.size(); ++i) {
            changed[i] = current.tiles[i] != lastFingerprint.tiles[i];
            changedCount += changed[i];
            if (changed[i]) {
                const int x = int(i) % current.tileColumns, y = int(i) / current.tileColumns;
                ocrRegionsChangedTilePixels += qint64(std::min(TileWidth, image.width() - x * TileWidth)) *
                    std::min(TileHeight, image.height() - y * TileHeight);
            }
        }
        ocrRegionsChangedTiles += changedCount;
        if (!changedCount) { fullReason = "unchanged_pixels"; return {}; }
        const int columns = current.tileColumns;
        const int rows = (image.height() + TileHeight - 1) / TileHeight;
        QVector<QRect> areas;
        QVector<int> pending;
        for (int cell = 0; cell < changed.size(); ++cell) {
            if (!changed[cell]) continue;
            pending.clear(); pending.append(cell); changed[cell] = 0;
            QRect area;
            for (qsizetype cursor = 0; cursor < pending.size(); ++cursor) {
                const int currentCell = pending[cursor], x = currentCell % columns, y = currentCell / columns;
                area = area.united(QRect(x * TileWidth, y * TileHeight, TileWidth, TileHeight).intersected(image.rect()));
                const auto append = [&](int neighbor) {
                    if (changed[neighbor]) { changed[neighbor] = 0; pending.append(neighbor); }
                };
                if (x > 0) append(currentCell - 1);
                if (x + 1 < columns) append(currentCell + 1);
                if (y > 0) append(currentCell - columns);
                if (y + 1 < rows) append(currentCell + columns);
            }
            ocrRegionsComponentPixels += qint64(area.width()) * area.height();
            areas.append(area.adjusted(-16, -16, 16, 16).intersected(image.rect()));
            if (areas.size() > MaxOcrRegions) { fullReason = "region_count"; return {}; }
        }
        // Include every touched cached line, then merge overlapping contexts.
        // Expand to exact line boxes here: adding padding at every inclusion
        // can cascade through a dense page even when only one line changed.
        // A bounded expansion loop avoids turning dense/ambiguous geometry into
        // unbounded region-planning work; a full pass remains the safe fallback.
        bool expanded = true;
        for (int iteration = 0; expanded && iteration < 32; ++iteration) {
            expanded = false;
            for (auto &area : areas) {
                for (const auto &line : cachedLines) {
                    if (!area.intersects(line.box)) continue;
                    const QRect united = area.united(line.box);
                    if (united != area) { area = united; expanded = true; }
                }
            }
            for (qsizetype i = 0; i < areas.size(); ++i) {
                for (qsizetype j = i + 1; j < areas.size();) {
                    if (areas[i].intersects(areas[j])) {
                        areas[i] = areas[i].united(areas[j]); areas.removeAt(j); expanded = true;
                    } else ++j;
                }
            }
        }
        if (expanded) { fullReason = "region_geometry"; return {}; }
        // Add outer context once, stopping before any untouched cached line.
        // The crop may never partly invalidate an old line merely for padding.
        for (auto &area : areas) {
            ocrRegionsGeometryPixels += qint64(area.width()) * area.height();
            const QRect core = area;
            const QRect padded = core.adjusted(-4, -4, 4, 4).intersected(image.rect());
            QRect context = padded;
            for (const auto &line : cachedLines) {
                if (core.intersects(line.box) || !context.intersects(line.box)) continue;
                if (line.box.right() < core.left()) context.setLeft(std::max(context.left(), line.box.right() + 1));
                if (line.box.left() > core.right()) context.setRight(std::min(context.right(), line.box.left() - 1));
                if (line.box.bottom() < core.top()) context.setTop(std::max(context.top(), line.box.bottom() + 1));
                if (line.box.top() > core.bottom()) context.setBottom(std::min(context.bottom(), line.box.top() - 1));
            }
            ocrRegionsPaddingClamps += context != padded;
            area = context;
            for (const auto &line : cachedLines)
                if (area.intersects(line.box) && !area.contains(line.box)) { fullReason = "region_geometry"; return {}; }
        }
        for (qsizetype i = 0; i < areas.size(); ++i)
            for (qsizetype j = i + 1; j < areas.size(); ++j)
                if (areas[i].intersects(areas[j])) { fullReason = "region_geometry"; return {}; }
        qint64 pixels = 0;
        for (const auto &area : areas) pixels += qint64(area.width()) * area.height();
        ocrRegionsCandidatePixels += pixels;
        if (pixels * 100 > qint64(image.width()) * image.height() * 45) { fullReason = "broad_change"; return {}; }
        std::sort(areas.begin(), areas.end(), [](const QRect &a, const QRect &b) {
            return a.top() == b.top() ? a.left() < b.left() : a.top() < b.top();
        });
        fullReason = nullptr;
        return areas;
    }

    QString recognizeRegions(const QImage &image, const ImageFingerprint &current) {
        WorkBudget budget(budgetOptions(), options.stopRequested);
        CollectBudget collect{*this, &budget};
        const auto finishBudget = [&] {
            if (budget.checkpoint()) return;
            if (budget.stats().cancellationRequested) throw OcrCancelled("Text recognition canceled by user");
            error("Recording stopped: text recognition was canceled or exceeded its work deadline");
        };
        const auto full = [&](const char *reason) {
            countFullPass(reason);
            if (!lastFingerprint.digest.isEmpty()) ++ocrRegionsFallbacks;
            auto result = recognize(image, image.rect(), &budget);
            finishBudget();
            ++ocrFullFrames; consecutivePartialFrames = 0;
            cachedLines = std::move(result.lines); cachedGeometryComplete = result.geometryComplete;
            return result.text;
        };
        const char *reason = nullptr;
        const QVector<QRect> areas = changedRegions(image, current, reason);
        if (reason) return full(reason);
        QVector<TextLine> replacements;
        qint64 usedPixels = 0;
        for (const auto &area : areas) {
            ++ocrRegionsAttempted;
            const qint64 pixels = qint64(area.width()) * area.height();
            ocrRegionsAttemptedPixels += pixels; usedPixels += pixels;
            auto result = recognize(image, area, &budget);
            if (!result.geometryComplete) { ++ocrPartialFallbacks; return full("crop_geometry"); }
            for (const auto &line : result.lines) {
                if ((area.left() > 0 && line.box.left() <= area.left() + 2) ||
                    (area.right() < image.width() - 1 && line.box.right() >= area.right() - 2) ||
                    (area.top() > 0 && line.box.top() <= area.top() + 2) ||
                    (area.bottom() < image.height() - 1 && line.box.bottom() >= area.bottom() - 2)) {
                    ++ocrPartialFallbacks;
                    return full("crop_edge");
                }
            }
            replacements.append(result.lines);
        }
        QVector<TextLine> merged;
        merged.reserve(cachedLines.size() + replacements.size());
        for (const auto &line : cachedLines) {
            const bool invalidated = std::any_of(areas.constBegin(), areas.constEnd(), [&](const QRect &area) {
                return area.intersects(line.box);
            });
            if (!invalidated) merged.append(line);
        }
        merged.append(replacements);
        qsizetype characters = 0;
        for (const auto &line : merged) characters += line.text.size();
        if (merged.size() > 8000 || characters > 2 * 1024 * 1024) {
            ++ocrPartialFallbacks;
            return full("cache_size");
        }
        std::sort(merged.begin(), merged.end(), [](const TextLine &a, const TextLine &b) {
            return a.box.top() == b.box.top() ? a.box.left() < b.box.left() : a.box.top() < b.box.top();
        });
        QStringList lines;
        for (const auto &line : merged) lines.append(line.text);
        const QString text = lines.join('\n') + '\n';
        finishBudget();
        cachedLines = std::move(merged);
        ++ocrPartialFrames; ++consecutivePartialFrames; ++ocrRegionsFrames;
        ocrRegionsUsed += areas.size(); ocrRegionsUsedPixels += usedPixels;
        return text;
    }

    QString recognizeFrame(const QImage &image, const ImageFingerprint &current) {
        ocrInputPixels += qint64(image.width()) * image.height();
        if (options.ocrMode == "regions") return recognizeRegions(image, current);
        const char *fullReason = "mode";
        const QRect area = options.ocrMode == "incremental" ? changedBand(image, current, fullReason) : image.rect();
        if (fullReason) countFullPass(fullReason);
        auto result = recognize(image, area);
        bool partial = area != image.rect();
        if (partial) {
            bool clipsText = !result.geometryComplete;
            for (const auto &line : result.lines) {
                if ((area.top() > 0 && line.box.top() <= area.top() + 2) ||
                    (area.bottom() < image.height() - 1 && line.box.bottom() >= area.bottom() - 2)) clipsText = true;
            }
            if (clipsText) {
                ++ocrPartialFallbacks;
                countFullPass(result.geometryComplete ? "crop_edge" : "crop_geometry");
                result = recognize(image, image.rect());
                partial = false;
            }
        }
        if (!partial) {
            ++ocrFullFrames;
            consecutivePartialFrames = 0;
            cachedLines = std::move(result.lines);
            cachedGeometryComplete = result.geometryComplete;
            return result.text;
        }
        // Invalidate every old line touching the changed region before adding
        // its replacement. A deleted line therefore cannot leak into this frame.
        QVector<TextLine> merged;
        merged.reserve(cachedLines.size() + result.lines.size());
        for (const auto &line : cachedLines) if (!line.box.intersects(area)) merged.append(line);
        merged.append(result.lines);
        qsizetype characters = 0;
        for (const auto &line : merged) characters += line.text.size();
        if (merged.size() > 8000 || characters > 2 * 1024 * 1024) {
            ++ocrPartialFallbacks;
            countFullPass("cache_size");
            result = recognize(image, image.rect());
            ++ocrFullFrames; consecutivePartialFrames = 0;
            cachedLines = std::move(result.lines); cachedGeometryComplete = result.geometryComplete;
            return result.text;
        }
        ++ocrPartialFrames;
        ++consecutivePartialFrames;
        std::sort(merged.begin(), merged.end(), [](const TextLine &a, const TextLine &b) {
            return a.box.top() == b.box.top() ? a.box.left() < b.box.left() : a.box.top() < b.box.top();
        });
        cachedLines = std::move(merged);
        QStringList lines;
        for (const auto &line : cachedLines) lines.append(line.text);
        return lines.join('\n') + '\n';
    }

};

struct Recorder::Impl : OcrEngine {
    ImageFingerprint lastCaptureFingerprint;
    std::unique_ptr<Database> db;
    QFile captureLock;
    QProcess encoder;
    QByteArray encoderErrors;
    qint64 previousTimestamp = -1, lastFrameId = 0;
    qint64 segmentId = 0, segmentStartMs = 0;
    int segmentCount = 0, segmentWidth = 0, segmentHeight = 0;
    QString segmentPath;
    quint64 sealedMediaBytes = 0;
    qint64 observations = 0, retained = 0, duplicates = 0;
    qint64 backlogFullCount = 0, backlogByteLimitCount = 0, backlogFrameLimitCount = 0;
    bool queueBytePressure = false, queueFramePressure = false;
    qint64 sourceHighWaterFrames = 0;
    quint64 sourceHighWaterBytes = 0;
    double hashWallMs = 0, ocrWallMs = 0, mediaWallMs = 0, indexWallMs = 0;
    double stagingEncodeWallMs = 0, stagingEncodeCpuMs = 0;
    double hashCpuMs = 0, ocrCpuMs = 0, mediaCpuMs = 0, indexCpuMs = 0;
    double startCpuMs = cpuMs();
    bool finished = false, failed = false;

    explicit Impl(const RecorderOptions &opts) : OcrEngine(opts) {
        if (options.directory.isEmpty() || !std::isfinite(options.intervalSeconds) || options.intervalSeconds <= 0 ||
            options.segmentFrames < 1 || options.segmentFrames > 300 || !std::isfinite(options.segmentSeconds) ||
            options.segmentSeconds <= 0 || options.maxDiskBytes < 16 * MiB)
            error("Invalid recorder limits (minimum disk budget is 16 MiB; segment limit 1-300 frames)");
        const QStringList codecs = {"webp", "h264", "hevc", "h264-vaapi", "hevc-vaapi"};
        if (!codecs.contains(options.codec)) error("Unsupported codec: " + options.codec);
        if (options.archiveFirst && (!options.deferredOcr || !options.ocr || options.codec != "webp"))
            error("Archive-first retention requires deferred OCR and the lossless WebP codec");
        if (options.resume && !options.archiveFirst) error("Persistent capture requires archive-first WebP history");
        if (options.rollingStorage && !options.resume) error("Rolling storage requires persistent history");
        if (options.deferredOcr && (!options.ocr || options.maxPendingFrames < 0 || options.maxPendingFrames > 1024 ||
            options.maxPendingBytes < 1 || (!options.archiveFirst && options.maxPendingBytes >= options.maxDiskBytes - IndexReserve - MiB)))
            error("Deferred OCR requires OCR enabled and pending limits that leave space for archive and index");
        if (options.ocrMode != "full" && options.ocrMode != "incremental" && options.ocrMode != "regions")
            error("OCR mode must be full, incremental or regions");
        if (!std::isfinite(options.ocrCpuPercent) || options.ocrCpuPercent < 0 || options.ocrCpuPercent > 100 ||
            options.ocrMaxWallMs < 1 || options.ocrMaxWallMs > 60000)
            error("Invalid OCR CPU budget or maximum wall time");
        const QFileInfo existing(options.directory);
        const bool nonempty = existing.exists() &&
            !QDir(options.directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty();
        if (existing.isSymLink() || (existing.exists() && !existing.isDir()) || (!options.resume && nonempty))
            error("Refusing to overwrite existing dataset; choose a new or empty directory");
        const bool reopen = options.resume && QFileInfo(dbPath(options.directory)).exists();
        if (reopen) privateHistoryDirectory(options.directory);
        else if (options.resume && existing.exists() &&
                 !QDir(options.directory).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot).isEmpty())
            error("Existing directory is not a resumable history");
        if (!QDir().mkpath(QDir(options.directory).filePath("media"))) error("Cannot create private dataset directory");
        options.directory = QFileInfo(options.directory).absoluteFilePath();
        if (!QFile::setPermissions(options.directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner) ||
            !QFile::setPermissions(QDir(options.directory).filePath("media"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
            error("Cannot restrict dataset directory permissions");
        const QString capturePath = QDir(options.directory).filePath(".capture.lock");
        const int descriptor = ::open(QFile::encodeName(capturePath).constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) error("Cannot open history capture lease");
        if (!captureLock.open(descriptor, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
            ::close(descriptor); error("Cannot own history capture lease");
        }
        struct stat captureStat{};
        if (fstat(descriptor, &captureStat) != 0 || !S_ISREG(captureStat.st_mode) || captureStat.st_uid != getuid() ||
            captureStat.st_nlink != 1 || flock(descriptor, LOCK_EX | LOCK_NB) != 0)
            error("Another recorder owns this history, or its capture lease is unsafe");
        lifetime.start();
        db = std::make_unique<Database>(dbPath(options.directory), !reopen, true);
        QFile::setPermissions(dbPath(options.directory), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (!reopen) {
        if (options.resume) db->exec("PRAGMA auto_vacuum=INCREMENTAL");
        db->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA cache_size=-2048;"
                 "PRAGMA wal_autocheckpoint=256; PRAGMA journal_size_limit=1048576; PRAGMA foreign_keys=ON;"
                 "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                 "INSERT INTO metadata VALUES('schema_version','2');"
                 "CREATE TABLE segments(id INTEGER PRIMARY KEY,path TEXT NOT NULL,codec TEXT NOT NULL,complete INTEGER NOT NULL DEFAULT 0,reserved_bytes INTEGER NOT NULL DEFAULT 0);"
                 "CREATE TABLE frames(id INTEGER PRIMARY KEY,timestamp_ms INTEGER NOT NULL,last_timestamp_ms INTEGER NOT NULL,"
                 "observation_count INTEGER NOT NULL DEFAULT 1,segment_id INTEGER REFERENCES segments(id),frame_index INTEGER NOT NULL,"
                 "path TEXT NOT NULL,codec TEXT NOT NULL,width INTEGER NOT NULL,height INTEGER NOT NULL,text TEXT NOT NULL,"
                 "ocr_state TEXT NOT NULL CHECK(ocr_state IN('pending','ready','disabled','failed')),ocr_error TEXT NOT NULL DEFAULT '',"
                 "source_path TEXT NOT NULL DEFAULT '',source_bytes INTEGER NOT NULL DEFAULT 0);"
                 "CREATE INDEX frame_index_state ON frames(ocr_state,timestamp_ms,id);"
                 "CREATE INDEX held_sources ON frames(source_path) WHERE source_path!='';"
                 "CREATE TABLE observations(id INTEGER PRIMARY KEY,timestamp_ms INTEGER NOT NULL,frame_id INTEGER NOT NULL REFERENCES frames(id));"
                 "CREATE INDEX observation_time ON observations(timestamp_ms);"
                 "CREATE VIRTUAL TABLE frame_text USING fts5(text,tokenize='unicode61');");
        } else {
            db->exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-2048;"
                     "PRAGMA wal_autocheckpoint=256; PRAGMA journal_size_limit=1048576; PRAGMA foreign_keys=ON");
            if (metadataNumber(*db, "archive_first") != 1 || metadataNumber(*db, "schema_version") != 2)
                error("Existing dataset is not compatible archive-first history");
        }
        if (options.deferredOcr) {
            db->exec("PRAGMA synchronous=FULL");
            if (!QDir().mkpath(QDir(options.directory).filePath("staging"))) error("Cannot create original-image staging directory");
            QFile::setPermissions(QDir(options.directory).filePath("staging"), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        ensureSchedule(*db);
        ensureRecallGeometry(*db);
        for (const auto &item : {QPair<QString, quint64>{"max_disk_bytes", options.maxDiskBytes},
                                {"max_pending_bytes", options.deferredOcr ? options.maxPendingBytes : 0},
                                {"archive_first", options.archiveFirst ? 1 : 0}}) {
            setMetadata(*db, item.first, item.second);
        }
        if (options.resume) {
            initializeHistory(*db, options.directory);
            // The capture lease proves any unfinished publication belongs to
            // an earlier process. Its reserved bytes remain counted until GC.
            db->exec("UPDATE history_media SET state='retired' WHERE state='writing' AND "
                     "NOT EXISTS(SELECT 1 FROM frames WHERE frames.path=history_media.path)");
            HistoryMaintenanceResult recovered;
            collectRetiredMedia(*db, options.directory, 128, recovered);
        }
        if (options.ocr && !options.deferredOcr) initializeOcr();
        // Opening history, including stopped/paused initialization, must not
        // evict observations. Rolling admission happens only for a new image.
        if (!options.rollingStorage) checkDisk();
    }

    ~Impl() {
        if (encoder.state() != QProcess::NotRunning) { encoder.kill(); encoder.waitForFinished(1000); }
    }

    quint64 bytesOnDisk() const {
        if (options.resume) return historyDiskBytes(*db, options.directory);
        quint64 size = sealedMediaBytes + directoryBytes(QDir(options.directory).filePath("staging"));
        if (!segmentPath.isEmpty()) size += std::max<qint64>(0, QFileInfo(QDir(options.directory).filePath(segmentPath)).size());
        for (const char *suffix : {"", "-wal", "-shm"})
            size += std::max<qint64>(0, QFileInfo(dbPath(options.directory) + suffix).size());
        return size;
    }

    void checkDisk(quint64 additionalBytes = 0) const {
        const quint64 used = bytesOnDisk();
        if (used >= options.maxDiskBytes || additionalBytes > options.maxDiskBytes - used)
            error("Recording stopped: dataset disk budget reached; retained data was preserved");
        QStorageInfo disk(options.directory); disk.refresh();
        if (!disk.isValid() || !disk.isReady() || disk.bytesAvailable() < 0 ||
            quint64(disk.bytesAvailable()) < options.minFreeBytes ||
            quint64(disk.bytesAvailable()) - options.minFreeBytes < additionalBytes)
            error("Recording stopped: minimum free-disk reserve reached; retained data was preserved");
    }

    void admitDisk(quint64 additionalBytes) {
        if (options.rollingStorage) {
            // The common path uses the recorder's existing connection. Only a
            // capacity boundary opens a maintenance connection or scans rows.
            try { checkDisk(additionalBytes); return; }
            catch (const std::runtime_error &) {}
            const auto space = makeHistorySpace(options.directory, options.maxDiskBytes, options.minFreeBytes, additionalBytes);
            if (!space.ready) throw RollingStorageUnavailable(space.reason, space.more);
        }
        checkDisk(additionalBytes);
    }

    void drainErrors() {
        encoderErrors.append(encoder.readAllStandardError());
        if (encoderErrors.size() > 8192) encoderErrors = encoderErrors.right(8192);
    }

    void startSegment(int width, int height, qint64 timestampMs) {
        const quint64 queueReserve = options.deferredOcr ? options.maxPendingBytes : 0;
        segmentWidth = width; segmentHeight = height; segmentStartMs = timestampMs; segmentCount = 0;
        quint64 outputLimit = 0;
        try {
            db->exec("BEGIN IMMEDIATE");
            checkDisk(IndexReserve + MiB + queueReserve);
            Statement insert(*db, "INSERT INTO segments(path,codec) VALUES('',?)");
            insert.bind(1, options.codec); insert.next();
            segmentId = sqlite3_last_insert_rowid(db->handle);
            segmentPath = QString("media/segment-%1.mp4").arg(segmentId, 6, 10, QChar('0'));
            const quint64 used = bytesOnDisk();
            const quint64 reserve = IndexReserve + queueReserve;
            if (used >= options.maxDiskBytes || reserve >= options.maxDiskBytes - used)
                error("Recording stopped: no space remains for a bounded video segment");
            outputLimit = options.maxDiskBytes - used - reserve;
            Statement reservation(*db, "UPDATE segments SET path=?,reserved_bytes=? WHERE id=?");
            reservation.bind(1, segmentPath); reservation.bind(2, qint64(outputLimit)); reservation.bind(3, segmentId); reservation.next();
            db->exec("COMMIT");
        } catch (...) {
            sqlite3_exec(db->handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
        QStringList args = {"-hide_banner", "-loglevel", "error", "-nostdin", "-n"};
        if (options.codec.endsWith("-vaapi")) args << "-vaapi_device" << options.device;
        args << "-threads" << "1" << "-filter_threads" << "1" << "-f" << "rawvideo" << "-pixel_format" << "rgba"
             << "-video_size" << QString("%1x%2").arg(width).arg(height)
             << "-framerate" << QString::number(1.0 / options.intervalSeconds, 'g', 12)
             << "-i" << "pipe:0" << "-an";
        if (options.codec.endsWith("-vaapi")) {
            args << "-vf" << "format=nv12,hwupload" << "-c:v" << (options.codec == "hevc-vaapi" ? "hevc_vaapi" : "h264_vaapi")
                 << "-qp" << "22" << "-bf" << "0" << "-async_depth" << "1";
        } else if (options.codec == "hevc") {
            args << "-c:v" << "libx265" << "-preset" << "ultrafast" << "-crf" << "18"
                 << "-x265-params" << "pools=1:frame-threads=1:log-level=error" << "-pix_fmt" << "yuv420p";
        } else {
            args << "-c:v" << "libx264" << "-preset" << "ultrafast" << "-tune" << "zerolatency"
                 << "-crf" << "18" << "-pix_fmt" << "yuv420p";
        }
        args << "-threads" << "1" << "-g" << QString::number(options.segmentFrames)
             << "-fs" << QString::number(outputLimit) << QDir(options.directory).filePath(segmentPath);
        encoderErrors.clear();
        encoder.setStandardOutputFile(QProcess::nullDevice());
        encoder.setChildProcessModifier([outputLimit] {
            const rlimit limit{rlim_t(outputLimit), rlim_t(outputLimit)};
            if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(126);
        });
        encoder.start("ffmpeg", args, QIODevice::ReadWrite);
        if (!encoder.waitForStarted(5000)) error("Cannot start FFmpeg: " + encoder.errorString());
    }

    void writeVideo(const QImage &image) {
        QElapsedTimer deadline; deadline.start();
        for (int row = 0; row < image.height(); ++row) {
            const char *data = reinterpret_cast<const char *>(image.constScanLine(row));
            qint64 remaining = qint64(image.width()) * 4;
            while (remaining > 0) {
                drainErrors();
                if (encoder.state() != QProcess::Running || deadline.elapsed() > ProcessTimeoutMs)
                    error("Recording stopped: encoder exited or exceeded 30 seconds: " + QString::fromUtf8(encoderErrors));
                const qint64 length = std::min<qint64>(remaining, 65536);
                const qint64 written = encoder.write(data, length);
                if (written < 0) error("Recording stopped: encoder input failed");
                data += written; remaining -= written;
                while (encoder.bytesToWrite() > 65536) {
                    encoder.waitForBytesWritten(100);
                    drainErrors();
                    if (encoder.state() != QProcess::Running || deadline.elapsed() > ProcessTimeoutMs)
                        error("Recording stopped: encoder input stalled: " + QString::fromUtf8(encoderErrors));
                }
            }
        }
        while (encoder.bytesToWrite() > 0) {
            encoder.waitForBytesWritten(100); drainErrors();
            if (encoder.state() != QProcess::Running || deadline.elapsed() > ProcessTimeoutMs)
                error("Recording stopped: encoder did not consume frame: " + QString::fromUtf8(encoderErrors));
        }
    }

    void finishSegment() {
        if (segmentPath.isEmpty()) return;
        encoder.closeWriteChannel();
        if (!encoder.waitForFinished(ProcessTimeoutMs)) {
            encoder.kill(); encoder.waitForFinished(1000);
            error("Recording stopped: encoder finalization timed out; last segment is marked incomplete");
        }
        drainErrors();
        if (encoder.exitStatus() != QProcess::NormalExit || encoder.exitCode() != 0)
            error("Recording stopped: encoder failed; last segment is marked incomplete: " + QString::fromUtf8(encoderErrors));
        // FFmpeg's file-size ceiling can produce exit 0 with fewer frames. Never
        // advertise those observations as retrievable without checking the count.
        QProcess probe;
        probe.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_entries", "stream=nb_frames",
                               "-of", "default=noprint_wrappers=1:nokey=1", QDir(options.directory).filePath(segmentPath)});
        if (!probe.waitForStarted(5000) || !probe.waitForFinished(ProcessTimeoutMs)) {
            probe.kill(); probe.waitForFinished(1000);
            error("Recording stopped: cannot verify finalized segment frame count");
        }
        bool countValid = false;
        const int frameCount = probe.readAllStandardOutput().trimmed().toInt(&countValid);
        if (probe.exitCode() != 0 || !countValid || frameCount != segmentCount)
            error("Recording stopped: encoded frame count differs from accepted observations; segment remains incomplete");
        checkDisk();
        if (options.deferredOcr) {
            QFile archive(QDir(options.directory).filePath(segmentPath));
            if (!archive.open(QIODevice::ReadOnly) || fsync(archive.handle()) != 0) error("Cannot synchronize completed video archive");
            syncDirectory(QDir(options.directory).filePath("media"));
        }
        Statement complete(*db, "UPDATE segments SET complete=1,reserved_bytes=0 WHERE id=?");
        complete.bind(1, segmentId); complete.next();
        sealedMediaBytes += QFileInfo(QDir(options.directory).filePath(segmentPath)).size();
        segmentPath.clear(); segmentCount = 0; segmentId = 0;
        cleanReadySources(*db, options.directory);
    }

    void observation(qint64 timestampMs, qint64 frameId) {
        if (options.resume) {
            Statement query(*db, "INSERT INTO observations(id,timestamp_ms,frame_id) VALUES(?,?,?)");
            query.bind(1, allocateHistoryId(*db, "history_next_observation"));
            query.bind(2, timestampMs); query.bind(3, frameId); query.next();
            adjustMetadata(*db, "history_observations", 1);
            return;
        }
        Statement query(*db, "INSERT INTO observations(timestamp_ms,frame_id) VALUES(?,?)");
        query.bind(1, timestampMs); query.bind(2, frameId); query.next();
    }

    qint64 reserveArchive(qint64 bytes, QString &path) {
        db->exec("BEGIN IMMEDIATE");
        try {
            checkDisk(quint64(bytes) + IndexReserve);
            const auto id = allocateHistoryId(*db, "history_next_frame");
            path = QString("media/frame-%1.webp").arg(id, 8, 10, QChar('0'));
            Statement reservation(*db, "INSERT INTO history_media(path,bytes,state) VALUES(?,?,'writing')");
            reservation.bind(1, path); reservation.bind(2, bytes); reservation.next();
            adjustMetadata(*db, "history_media_bytes", bytes);
            db->exec("COMMIT");
            return id;
        } catch (...) { sqlite3_exec(db->handle, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    }

    bool queueHasRoom(quint64 bytes) {
        const auto usage = heldSourceUsage(*db, options.directory);
        sourceHighWaterFrames = std::max(sourceHighWaterFrames, usage.frames);
        sourceHighWaterBytes = std::max(sourceHighWaterBytes, usage.bytes);
        queueFramePressure = options.maxPendingFrames > 0 && usage.frames >= options.maxPendingFrames;
        queueBytePressure = bytes > options.maxPendingBytes || usage.bytes > options.maxPendingBytes - bytes;
        return !queueFramePressure && !queueBytePressure;
    }
    void countBacklogRejection() {
        ++backlogFullCount;
        backlogFrameLimitCount += queueFramePressure;
        backlogByteLimitCount += queueBytePressure;
    }
};

Recorder::Recorder(const RecorderOptions &options) : d(std::make_unique<Impl>(options)) {}
Recorder::~Recorder() = default;

AddFrameResult Recorder::addFrame(const QImage &input, qint64 timestampMs) {
    if (d->finished || d->failed) error("Recorder is already stopped");
    QString unpublishedArchive;
    quint64 unpublishedArchiveBytes = 0;
    QString reservedArchive;
    try {
        if (input.isNull() || input.width() > 16384 || input.height() > 16384 ||
            qint64(input.width()) * input.height() > 32 * 1024 * 1024)
            error("Frame is empty or exceeds the bounded 32 megapixel input limit");
        if (timestampMs < 0 || timestampMs < d->previousTimestamp) error("Capture timestamps must be nondecreasing");
        if (d->options.codec != "webp" && ((input.width() % 2) || (input.height() % 2)))
            error("Video comparison requires even frame dimensions");
        // A rolling archive must know the real encoded size before retiring
        // evidence; an oversized incoming image must leave history intact.
        if (!d->options.rollingStorage) d->checkDisk(IndexReserve);
        QElapsedTimer timer; timer.start(); double cpuStart = cpuMs();
        const QImage image = input.convertToFormat(QImage::Format_RGBA8888);
        const ImageFingerprint currentFingerprint = fingerprint(image, d->ocr && d->options.ocrMode == "incremental",
                                                                d->ocr && d->options.ocrMode == "regions");
        d->hashWallMs += timer.nsecsElapsed() / 1e6; d->hashCpuMs += cpuMs() - cpuStart;
        if (!d->segmentPath.isEmpty() && (d->segmentCount >= d->options.segmentFrames ||
            timestampMs - d->segmentStartMs >= d->options.segmentSeconds * 1000 ||
            image.width() != d->segmentWidth || image.height() != d->segmentHeight)) {
            timer.restart(); cpuStart = cpuMs(); d->finishSegment();
            d->mediaWallMs += timer.nsecsElapsed() / 1e6; d->mediaCpuMs += cpuMs() - cpuStart;
        }
        AddFrameResult result;
        QByteArray original;
        timer.restart(); cpuStart = cpuMs();
        if (currentFingerprint.digest == d->lastCaptureFingerprint.digest) {
            if (d->options.rollingStorage) {
                try { d->checkDisk(IndexReserve); }
                catch (const std::runtime_error &) {
                    original = encodeWebP(image);
                    d->stagingEncodeWallMs += timer.nsecsElapsed() / 1e6;
                    d->stagingEncodeCpuMs += cpuMs() - cpuStart;
                    // Cleanup can retire the shared frame; reserve enough to
                    // publish it anew before touching any existing observation.
                    d->admitDisk(original.size() + IndexReserve);
                }
            }
            d->db->exec("BEGIN IMMEDIATE");
            Statement query(*d->db, "UPDATE frames SET last_timestamp_ms=?,observation_count=observation_count+1 WHERE id=?");
            query.bind(1, timestampMs); query.bind(2, d->lastFrameId); query.next();
            if (sqlite3_changes(d->db->handle) == 0) {
                d->db->exec("ROLLBACK");
                breakContinuity(); // Retention deleted the last shared original.
            } else {
            d->observation(timestampMs, d->lastFrameId);
            d->db->exec("COMMIT");
            ++d->duplicates; ++d->observations; d->previousTimestamp = timestampMs;
            d->indexWallMs += timer.nsecsElapsed() / 1e6; d->indexCpuMs += cpuMs() - cpuStart;
            result.duplicate = true; result.frameId = d->lastFrameId;
            return result;
            }
        }
        if (d->options.deferredOcr) {
            if (!d->options.archiveFirst && !d->queueHasRoom(0)) {
                d->finishSegment(); // A full queue must not wait for a larger segment limit.
                if (!d->queueHasRoom(0)) { result.backlogFull = true; d->countBacklogRejection(); return result; }
            }
            if (original.isEmpty()) {
                timer.restart(); cpuStart = cpuMs();
                original = encodeWebP(image);
                d->stagingEncodeWallMs += timer.nsecsElapsed() / 1e6; d->stagingEncodeCpuMs += cpuMs() - cpuStart;
            }
            if (!d->options.archiveFirst && !d->queueHasRoom(original.size())) {
                d->finishSegment();
                if (!d->queueHasRoom(original.size())) { result.backlogFull = true; d->countBacklogRejection(); return result; }
            }
            d->admitDisk(original.size() + IndexReserve);
        }
        QString text;
        QString geometry;
        ImageFingerprint currentOcrFingerprint;
        timer.restart(); cpuStart = cpuMs();
        try {
            if (d->ocr) {
                const QImage ocrImage = d->prepareOcrImage(image);
                currentOcrFingerprint = ocrImage.size() == image.size() ? currentFingerprint
                    : fingerprint(ocrImage, d->options.ocrMode == "incremental", d->options.ocrMode == "regions");
                text = d->recognizeFrame(ocrImage, currentOcrFingerprint);
                geometry = serializedGeometry(d->cachedLines, ocrImage.size(), image.size());
            }
        } catch (...) {
            d->ocrWallMs += timer.nsecsElapsed() / 1e6; d->ocrCpuMs += cpuMs() - cpuStart;
            throw;
        }
        result.ocrMs = timer.nsecsElapsed() / 1e6;
        d->ocrWallMs += result.ocrMs; d->ocrCpuMs += cpuMs() - cpuStart;
        QString path;
        QString sourcePath;
        const QString ocrState = d->options.deferredOcr ? "pending" : d->options.ocr ? "ready" : "disabled";
        qint64 frameIndex = 0;
        qint64 reservedFrameId = 0;
        timer.restart(); cpuStart = cpuMs();
        if (d->options.codec == "webp") {
            const QByteArray bytes = original.isEmpty() ? encodeWebP(image) : original;
            if (d->options.resume) {
                reservedFrameId = d->reserveArchive(bytes.size(), path);
                reservedArchive = path;
            }
            if (d->options.archiveFirst) {
                // Encoding happens outside the write transaction. Serialize the
                // final budget check, durable publication and row commit against
                // OCR publication so they cannot spend the same disk headroom.
                d->db->exec("BEGIN IMMEDIATE");
            }
            d->checkDisk((d->options.resume ? 0 : bytes.size()) + IndexReserve);
            if (!d->options.resume) path = QString("media/frame-%1.webp").arg(d->retained + 1, 8, 10, QChar('0'));
            const QString absolutePath = QDir(d->options.directory).filePath(path);
            if (d->options.archiveFirst) {
                if (QFileInfo::exists(absolutePath)) error("Refusing to replace an existing archive image");
                unpublishedArchive = absolutePath;
            }
            if (d->options.resume) PrivateMedia(d->options.directory).write(path, bytes);
            else { writeImageFile(absolutePath, bytes, d->options.deferredOcr); d->sealedMediaBytes += bytes.size(); }
            if (d->options.archiveFirst) unpublishedArchiveBytes = bytes.size();
            if (d->options.deferredOcr) sourcePath = path;
        } else {
            if (d->segmentPath.isEmpty()) d->startSegment(image.width(), image.height(), timestampMs);
            if (d->options.deferredOcr) {
                sourcePath = QString("staging/frame-%1.webp").arg(d->retained + 1, 8, 10, QChar('0'));
                writeImageFile(QDir(d->options.directory).filePath(sourcePath), original, true);
            }
            d->writeVideo(image);
            path = d->segmentPath; frameIndex = d->segmentCount++;
        }
        result.mediaMs = timer.nsecsElapsed() / 1e6;
        d->mediaWallMs += result.mediaMs; d->mediaCpuMs += cpuMs() - cpuStart;
        timer.restart(); cpuStart = cpuMs();
        if (!d->options.archiveFirst) d->db->exec("BEGIN IMMEDIATE");
        Statement insert(*d->db, "INSERT INTO frames(id,timestamp_ms,last_timestamp_ms,segment_id,frame_index,path,codec,width,height,text,ocr_state,source_path,source_bytes) VALUES(NULLIF(?,0),?,?,NULLIF(?,0),?,?,?,?,?,?,?,?,?)");
        insert.bind(1, reservedFrameId); insert.bind(2, timestampMs); insert.bind(3, timestampMs); insert.bind(4, d->segmentId); insert.bind(5, frameIndex);
        insert.bind(6, path); insert.bind(7, d->options.codec); insert.bind(8, image.width()); insert.bind(9, image.height()); insert.bind(10, text);
        insert.bind(11, ocrState); insert.bind(12, sourcePath); insert.bind(13, original.size()); insert.next();
        d->lastFrameId = sqlite3_last_insert_rowid(d->db->handle);
        if (d->options.resume) {
            Statement publish(*d->db, "UPDATE history_media SET state='live' WHERE path=? AND state='writing'");
            publish.bind(1, path); publish.next();
            if (sqlite3_changes(d->db->handle) != 1) error("History original lost its publication reservation");
            adjustMetadata(*d->db, "history_frames", 1);
        }
        if (ocrState == "ready") {
            Statement fts(*d->db, "INSERT INTO frame_text(rowid,text) VALUES(?,?)");
            fts.bind(1, d->lastFrameId); fts.bind(2, text); fts.next();
            storeGeometry(*d->db, d->lastFrameId, geometry);
        }
        d->observation(timestampMs, d->lastFrameId);
        d->db->exec("COMMIT");
        unpublishedArchive.clear();
        unpublishedArchiveBytes = 0;
        reservedArchive.clear();
        d->indexWallMs += timer.nsecsElapsed() / 1e6; d->indexCpuMs += cpuMs() - cpuStart;
        d->lastCaptureFingerprint = currentFingerprint;
        if (d->ocr) d->lastFingerprint = std::move(currentOcrFingerprint);
        d->previousTimestamp = timestampMs; ++d->observations; ++d->retained;
        if (d->options.deferredOcr) d->queueHasRoom(0); // Sample accepted backlog, without scanning ready history.
        d->checkDisk();
        result.stored = true; result.frameId = d->lastFrameId;
        return result;
    } catch (const RollingStorageUnavailable &) {
        // Admission runs before any publication transaction. A bounded cleanup
        // can continue on the next tick without poisoning the recorder.
        throw;
    } catch (const OcrCancelled &) {
        // OCR precedes this observation's media/index write. Earlier accepted
        // frames remain intact and the caller may finalize their video segment.
        throw;
    } catch (...) {
        d->failed = true;
        sqlite3_exec(d->db->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        // Only this attempt's file can be unreferenced. Never remove a committed
        // original, including when a later bookkeeping check fails.
        if (d->options.resume && !reservedArchive.isEmpty()) {
            try {
                Statement retire(*d->db, "UPDATE history_media SET state='retired' WHERE path=? AND state='writing'");
                retire.bind(1, reservedArchive); retire.next();
                HistoryMaintenanceResult cleanup;
                collectRetiredMedia(*d->db, d->options.directory, 1, cleanup);
            } catch (...) {} // The durable reservation makes the next recovery safe.
        } else if (!d->options.resume && !unpublishedArchive.isEmpty() && QFile::remove(unpublishedArchive))
            d->sealedMediaBytes -= unpublishedArchiveBytes;
        if (d->encoder.state() != QProcess::NotRunning) { d->encoder.kill(); d->encoder.waitForFinished(1000); }
        throw;
    }
}

void Recorder::breakContinuity() {
    d->lastCaptureFingerprint = {};
    d->lastFrameId = 0;
    d->lastFingerprint = {};
    d->cachedLines.clear();
    d->cachedGeometryComplete = false;
    d->consecutivePartialFrames = 0;
}

void Recorder::finish() {
    if (d->finished) return;
    if (d->failed) error("Recorder failed; incomplete media remains marked unavailable");
    try {
        QElapsedTimer timer; timer.start(); const double cpuStart = cpuMs();
        d->finishSegment();
        d->mediaWallMs += timer.nsecsElapsed() / 1e6; d->mediaCpuMs += cpuMs() - cpuStart;
        d->db->exec(d->options.deferredOcr ? "PRAGMA wal_checkpoint(PASSIVE)" : "PRAGMA wal_checkpoint(TRUNCATE)");
        d->finished = true;
        d->captureLock.close();
    } catch (...) { d->failed = true; throw; }
}

QJsonObject Recorder::statsJSON() const {
    QJsonObject stats{{"codec", d->options.codec}, {"directory", d->options.directory}, {"observations", d->observations},
            {"retained_frames", d->retained}, {"duplicate_frames", d->duplicates}, {"disk_bytes", double(d->bytesOnDisk())},
            {"indexing_mode", d->options.deferredOcr ? "deferred" : "sync"}, {"archive_first", d->options.archiveFirst},
            {"backlog_full", d->backlogFullCount},
            {"backlog_byte_limit", d->backlogByteLimitCount}, {"backlog_frame_limit", d->backlogFrameLimitCount},
            {"max_pending_frames", d->options.maxPendingFrames}, {"max_pending_bytes", double(d->options.maxPendingBytes)},
            {"source_high_water_frames", d->sourceHighWaterFrames}, {"source_high_water_bytes", double(d->sourceHighWaterBytes)},
            {"indexing", indexingStatus(d->options.directory)},
            {"ocr_initialized", bool(d->ocr)}, {"staging_encode_wall_ms", d->stagingEncodeWallMs}, {"staging_encode_cpu_ms", d->stagingEncodeCpuMs},
            {"elapsed_ms", double(d->lifetime.elapsed())}, {"process_cpu_ms", cpuMs() - d->startCpuMs},
            {"hash_wall_ms", d->hashWallMs}, {"ocr_wall_ms", d->ocrWallMs}, {"media_wall_ms", d->mediaWallMs},
            {"index_wall_ms", d->indexWallMs}, {"hash_cpu_ms", d->hashCpuMs}, {"ocr_cpu_ms", d->ocrCpuMs},
            {"media_cpu_ms", d->mediaCpuMs}, {"index_cpu_ms", d->indexCpuMs}, {"ocr_init_ms", d->initOcrMs},
            {"ocr_mode", d->options.ocrMode}, {"ocr_full_frames", d->ocrFullFrames}, {"ocr_partial_frames", d->ocrPartialFrames},
            {"ocr_partial_fallbacks", d->ocrPartialFallbacks}, {"ocr_processed_pixels", d->ocrProcessedPixels}, {"ocr_input_pixels", d->ocrInputPixels},
            {"ocr_cpu_percent", d->options.ocrCpuPercent}, {"ocr_max_wall_ms", d->options.ocrMaxWallMs},
            {"ocr_budget_checkpoints", qint64(d->ocrBudgetCheckpoints)},
            {"ocr_budget_sleep_count", qint64(d->ocrBudgetSleeps)}, {"ocr_budget_sleep_ms", d->ocrBudgetSleepMs},
            {"ocr_budget_cancellations", qint64(d->ocrBudgetCancellations)}, {"ocr_budget_deadlines", qint64(d->ocrBudgetDeadlines)},
            {"ocr_max_callback_wall_gap_ms", d->ocrMaxCallbackWallGapMs}, {"ocr_max_callback_cpu_gap_ms", d->ocrMaxCallbackCpuGapMs},
            {"allocator_trim_attempts", d->allocatorTrimAttempts}, {"allocator_trim_releases", d->allocatorTrimReleases},
            {"allocator_trim_cpu_ms", d->allocatorTrimCpuMs}, {"allocator_trim_wall_ms", d->allocatorTrimWallMs},
            {"finished", d->finished}, {"failed", d->failed}, {"incomplete_segment", !d->segmentPath.isEmpty()},
            {"cpu_scope", "recorder process only; FFmpeg child and compositor require external measurement"}};
    d->addExperimentStats(stats);
    return stats;
}

namespace {
RecorderOptions workerOptions(const IndexerOptions &options) {
    RecorderOptions result;
    result.directory = QFileInfo(options.directory).absoluteFilePath();
    result.ocrMode = options.ocrMode; result.ocrCpuPercent = options.ocrCpuPercent;
    result.ocrMaxWallMs = options.ocrMaxWallMs; result.stopRequested = options.stopRequested;
    result.ocrDataPath = options.ocrDataPath; result.ocrMaxHeight = options.ocrMaxHeight;
    result.ocrLanguages = options.ocrLanguages;
    if ((result.ocrMode != "full" && result.ocrMode != "incremental" && result.ocrMode != "regions") || !std::isfinite(result.ocrCpuPercent) ||
        result.ocrCpuPercent < 0 || result.ocrCpuPercent > 100 || result.ocrMaxWallMs < 1 || result.ocrMaxWallMs > 60000)
        error("Invalid index-worker OCR options");
    return result;
}

} // namespace

struct Indexer::Impl : OcrEngine {
    std::unique_ptr<Database> db;
    QFile lock;
    bool legacy = false;
    bool initialized = false;
    bool reuseEnabled = false;
    QString reuseProfile;
    qint64 reuseLookups = 0, reuseHits = 0, reuseMisses = 0, reuseStores = 0, reuseInvalidations = 0;
    qint64 reusePixels = 0;
    double reuseIdentityMs = 0, reuseLookupMs = 0;
    qint64 processed = 0, failedJobs = 0, canceledJobs = 0;
    qint64 priorityJobs = 0, oldestJobs = 0, obsoleteJobs = 0, discontinuityResets = 0;
    qint64 lastIndexedFrameId = 0, lastIndexedTimestamp = -1;
    qint64 nextBoostPollMs = 0, catchUpUntilMs = 0;
    bool currentPriority = false;
    std::function<double(bool)> cpuPercentProvider;
    double ocrWallMs = 0, ocrCpuMs = 0, decodeWallMs = 0, hashWallMs = 0;
    double startCpuMs = cpuMs();
    qint64 databaseContentions = 0;
    int lastContentionCode = 0;
    double databaseRetryWaitMs = 0;
    std::exception_ptr databasePollError;

    explicit Impl(const IndexerOptions &opts) : OcrEngine(workerOptions(opts)), lock(QDir(options.directory).filePath(".indexer.lock")),
                                               reuseEnabled(opts.ocrReuse) {
        db = std::make_unique<Database>(dbPath(options.directory), false, true);
        // A short SQLite wait handles brief handoffs. Longer contention yields
        // to the worker loop so stop signals stay responsive and OCR is deferred.
        sqlite3_busy_timeout(db->handle, 100);
        if (QFileInfo(lock.fileName()).isSymLink() || !lock.open(QIODevice::ReadWrite) ||
            fcntl(lock.handle(), F_SETFD, FD_CLOEXEC) != 0 || flock(lock.handle(), LOCK_EX | LOCK_NB) != 0)
            error("Another index worker owns this dataset, or its worker lock is unavailable");
        lock.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        cpuPercentProvider = opts.cpuPercentProvider;
        if (cpuPercentProvider) options.ocrCpuPercentProvider = [this] {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 pollTime = lifetime.elapsed();
            if (pollTime >= nextBoostPollMs) {
                try {
                    Statement request(*db, "SELECT catch_up_until_ms FROM index_schedule WHERE id=1");
                    catchUpUntilMs = request.next() ? request.number(0) : 0;
                } catch (const SqliteError &) {
                    // Tesseract's noexcept monitor catches callback errors.
                    // Preserve the SQLite code for the outer worker boundary.
                    databasePollError = std::current_exception();
                    throw;
                }
                nextBoostPollMs = pollTime + 1000;
            }
            return cpuPercentProvider(currentPriority || catchUpUntilMs > now);
        };
    }

    void initializeDatabase() {
        if (initialized) return;
        // These idempotent migrations can race an active recorder at startup.
        // Run them inside the same contention boundary as subsequent jobs.
        db->exec("PRAGMA synchronous=FULL; PRAGMA cache_size=-2048; PRAGMA wal_autocheckpoint=256; PRAGMA journal_size_limit=1048576");
        legacy = !hasIndexStates(*db);
        if (!legacy) { ensureSchedule(*db); ensureRecallGeometry(*db); cleanReadySources(*db, options.directory); }
        if (!legacy && reuseEnabled) ensureOcrReuse(*db);
        initialized = true;
    }

    bool stopRequested() const { return options.stopRequested && options.stopRequested(); }

    bool deferContention(const SqliteError &exception) {
        if (!exception.contention()) throw exception;
        // Never retain an old snapshot or partial transaction while waiting.
        if (!sqlite3_get_autocommit(db->handle)) db->exec("ROLLBACK");
        resetGeometry();
        ++databaseContentions;
        lastContentionCode = exception.code;
        QElapsedTimer waiting; waiting.start();
        while (waiting.elapsed() < 100 && !stopRequested()) QThread::msleep(20);
        databaseRetryWaitMs += waiting.nsecsElapsed() / 1e6;
        return stopRequested();
    }

    void resetGeometry() {
        lastFingerprint = {}; cachedLines.clear(); cachedGeometryComplete = false; consecutivePartialFrames = 0;
        lastIndexedFrameId = 0; lastIndexedTimestamp = -1;
    }

    void initializeReuseProfile() {
        // Resolve the default model with the existing initialization path, then
        // initialize from the exact bytes we hash. A file stat/hash around Init
        // cannot prove which bytes Init read during a concurrent replacement.
        QElapsedTimer timer; timer.start();
        const QStringList languages = options.ocrLanguages.split('+');
        // Multiple models cannot be re-initialized from one byte buffer, so
        // beyond a single language this hashes each model file from disk and
        // keeps the already-initialized engine. ponytail: a model replaced on
        // disk between hashing and use is claimed under the old hash; the
        // single-language path below keeps the byte-exact guarantee.
        const bool byteExact = languages.size() == 1;
        const QString language = languages.first();
        QFile model(QDir(QString::fromUtf8(ocr->GetDatapath())).filePath(language + ".traineddata"));
        constexpr qint64 modelLimit = 64 * MiB;
        if (!model.open(QIODevice::ReadOnly) || model.size() <= 0 || model.size() > modelLimit) return;
        const QByteArray bytes = model.read(modelLimit + 1);
        if (bytes.isEmpty() || bytes.size() > modelLimit || !model.atEnd()) return;
        std::unique_ptr<tesseract::TessBaseAPI> exact;
        if (byteExact) {
            const SerialOcrScope serial;
            exact = std::make_unique<tesseract::TessBaseAPI>();
            const QByteArray languageBytes = language.toUtf8();
            if (exact->Init(bytes.constData(), int(bytes.size()), languageBytes.constData(), tesseract::OEM_LSTM_ONLY,
                            nullptr, 0, nullptr, nullptr, false, nullptr) != 0) return;
            std::vector<std::string> loaded;
            exact->GetLoadedLanguagesAsVector(&loaded);
            if (loaded != std::vector<std::string>{language.toStdString()}) return;
            exact->SetPageSegMode(tesseract::PSM_AUTO);
        }
        // Bump this revision for changes to preprocessing, layout extraction,
        // or serialization. Incremental modes share only their full results.
        // The configured language set is part of the profile: cached OCR from
        // another language set is never reused.
        QByteArray profile = "replay-full-ocr-v1;" + options.ocrLanguages.toUtf8() + ";lstm-only;psm-auto;rgba8888;smooth-height;outward-geometry;";
        profile += tesseract::TessBaseAPI::Version(); profile += ';'; profile += qVersion(); profile += ';';
        char *leptonica = getLeptonicaVersion();
        if (!leptonica) return;
        profile += leptonica; profile += ';'; lept_free(leptonica);
        profile += QByteArray::number(options.ocrMaxHeight); profile += ';';
        if (!byteExact) {
            for (const QString &named : languages) {
                QFile trained(QDir(QString::fromUtf8(ocr->GetDatapath())).filePath(named + ".traineddata"));
                if (!trained.open(QIODevice::ReadOnly) || trained.size() <= 0 || trained.size() > modelLimit) return;
                const QByteArray contents = trained.read(modelLimit + 1);
                if (contents.isEmpty() || contents.size() > modelLimit || !trained.atEnd()) return;
                profile += named.toUtf8(); profile += '=';
                profile += QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex(); profile += ';';
            }
        }
        // The exact fingerprint is raw rows in full mode, a tree of 32-row
        // SHA-256 bands in incremental mode, or 128x64 tiles in regions mode.
        profile += "pixel-key-v1:"; profile += options.ocrMode.toUtf8(); profile += ';';
        profile += QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
        reuseProfile = QString::fromLatin1(QCryptographicHash::hash(profile, QCryptographicHash::Sha256).toHex());
        if (exact) ocr = std::move(exact);
        reuseIdentityMs += timer.nsecsElapsed() / 1e6;
    }

    bool findReuse(const QString &pixels, int width, int height, QString &text, QString &geometry) {
        QElapsedTimer timer; timer.start();
        ++reuseLookups;
        Statement lookup(*db, "SELECT f.text,g.lines_json,c.result_key FROM ocr_reuse c "
            "JOIN frames f ON f.id=c.frame_id JOIN frame_ocr_geometry g ON g.frame_id=f.id "
            "WHERE c.pixel_key=? AND c.profile_key=? AND f.ocr_state='ready' AND f.width=? AND f.height=? "
            "AND length(CAST(f.text AS BLOB))<=1048576 AND length(CAST(g.lines_json AS BLOB))<=3145728");
        lookup.bind(1, pixels); lookup.bind(2, reuseProfile); lookup.bind(3, width); lookup.bind(4, height);
        bool found = false;
        if (lookup.next()) {
            text = lookup.string(0); geometry = lookup.string(1);
            found = ocrResultKey(text, geometry) == lookup.string(2);
        }
        if (!found) {
            Statement remove(*db, "DELETE FROM ocr_reuse WHERE pixel_key=? AND profile_key=?");
            remove.bind(1, pixels); remove.bind(2, reuseProfile); remove.next();
            reuseInvalidations += sqlite3_changes(db->handle);
            ++reuseMisses;
        }
        reuseLookupMs += timer.nsecsElapsed() / 1e6;
        return found;
    }

    bool sameSource(const QString &source, int width, int height, const QByteArray &expected) {
        QFile file(safeMediaPath(options.directory, source));
        const qint64 bound = qint64(width) * height * 5 + MiB;
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > bound) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 bytes = 0;
        while (!file.atEnd()) {
            const QByteArray part = file.read(64 * 1024);
            if (part.isEmpty() || (bytes += part.size()) > bound) return false;
            hash.addData(part);
        }
        return hash.result() == expected;
    }

    // Caller owns a short write transaction, including donor lookup on a hit.
    bool publish(qint64 frameId, const QString &source, int width, int height, qint64 timestamp,
                 const QString &text, const QString &geometry, const QString &pixels, bool donor) {
        checkIndexHeadroom(text.toUtf8().size(), geometry.toUtf8().size());
        Statement update(*db, "UPDATE frames SET text=?,ocr_state='ready',ocr_error='' WHERE id=? AND ocr_state='pending' "
            "AND source_path=? AND timestamp_ms=? AND width=? AND height=?");
        update.bind(1, text); update.bind(2, frameId); update.bind(3, source);
        update.bind(4, timestamp); update.bind(5, width); update.bind(6, height); update.next();
        const bool changed = sqlite3_changes(db->handle) == 1;
        if (changed) {
            Statement index(*db, "INSERT INTO frame_text(rowid,text) VALUES(?,?)");
            index.bind(1, frameId); index.bind(2, text); index.next();
            storeGeometry(*db, frameId, geometry);
            if (donor && !pixels.isEmpty() && !reuseProfile.isEmpty()) {
                Statement cache(*db, "INSERT OR REPLACE INTO ocr_reuse(pixel_key,profile_key,frame_id,result_key) VALUES(?,?,?,?)");
                cache.bind(1, pixels); cache.bind(2, reuseProfile); cache.bind(3, frameId);
                cache.bind(4, ocrResultKey(text, geometry)); cache.next();
                db->exec("DELETE FROM ocr_reuse WHERE rowid IN (SELECT rowid FROM ocr_reuse ORDER BY rowid DESC LIMIT -1 OFFSET 256)");
            }
        }
        consumeRequest(frameId);
        return changed;
    }

    bool pickNext(IndexResult &result, QString &source, int &width, int &height, qint64 &timestamp) {
        db->exec("BEGIN IMMEDIATE");
        try {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            pruneRequests(*db, now);
            int streak = 0;
            {
                Statement state(*db, "SELECT priority_streak FROM index_schedule WHERE id=1");
                if (state.next()) streak = int(state.number(0));
            }
            bool priorityPick = false;
            if (streak < 3) {
                Statement priority(*db, "SELECT f.id,f.source_path,f.width,f.height,f.timestamp_ms FROM index_requests r "
                    "JOIN frames f ON f.id=r.frame_id WHERE f.ocr_state='pending' "
                    "ORDER BY (r.rank=0) DESC,r.request_order DESC,r.rank,f.timestamp_ms,f.id LIMIT 1");
                if (priority.next()) {
                    result.frameId = priority.number(0); source = priority.string(1);
                    width = int(priority.number(2)); height = int(priority.number(3)); timestamp = priority.number(4);
                    priorityPick = true;
                }
            }
            currentPriority = priorityPick;
            if (!priorityPick) {
                Statement oldest(*db, "SELECT id,source_path,width,height,timestamp_ms,"
                    "EXISTS(SELECT 1 FROM index_requests r WHERE r.frame_id=frames.id) "
                    "FROM frames WHERE ocr_state='pending' ORDER BY timestamp_ms,id LIMIT 1");
                if (oldest.next()) {
                    result.frameId = oldest.number(0); source = oldest.string(1);
                    width = int(oldest.number(2)); height = int(oldest.number(3)); timestamp = oldest.number(4);
                    currentPriority = oldest.number(5) != 0;
                }
            }
            if (result.frameId) {
                Statement state(*db, "UPDATE index_schedule SET priority_streak=? WHERE id=1");
                state.bind(1, priorityPick ? streak + 1 : 0); state.next();
                if (priorityPick) ++priorityJobs; else ++oldestJobs;
                if (lastIndexedFrameId) {
                    // Recorder IDs follow capture order. Any jump, reverse pick
                    // or deleted-ID gap conservatively discards cached geometry.
                    if (timestamp < lastIndexedTimestamp || result.frameId - lastIndexedFrameId != 1) {
                        ++discontinuityResets;
                        resetGeometry();
                    }
                }
            }
            db->exec("COMMIT");
            return result.frameId != 0;
        } catch (...) {
            sqlite3_exec(db->handle, "ROLLBACK", nullptr, nullptr, nullptr);
            throw;
        }
    }

    void consumeRequest(qint64 frameId) {
        Statement remove(*db, "DELETE FROM index_requests WHERE frame_id=?");
        remove.bind(1, frameId); remove.next();
    }

    void checkIndexHeadroom(qsizetype textBytes, qsizetype geometryBytes) {
        const quint64 ceiling = metadataNumber(*db, "max_disk_bytes");
        if (!ceiling) error("Dataset does not declare an index disk budget");
        const quint64 required = quint64(textBytes) * 8 + quint64(geometryBytes) * 2 + 256 * 1024;
        if (metadataNumber(*db, "history_version") == 1) {
            const auto used = historyDiskBytes(*db, options.directory);
            if (used >= ceiling || required > ceiling - used)
                error("Index disk headroom is exhausted; original evidence was retained");
            return;
        }
        quint64 used = 0;
        QHash<QString, quint64> reservations;
        const quint64 staged = directoryBytes(QDir(options.directory).filePath("staging"));
        bool activeSegment = false;
        {
            Statement query(*db, "SELECT path,reserved_bytes FROM segments WHERE complete=0 AND reserved_bytes>0");
            while (query.next()) {
                activeSegment = true;
                reservations.insert(QFileInfo(query.string(0)).fileName(), query.number(1));
            }
        }
        const auto media = QDir(QDir(options.directory).filePath("media")).entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
        for (const auto &file : media) {
            used += std::max<quint64>(std::max<qint64>(0, file.size()), reservations.take(file.fileName()));
        }
        for (const auto reserved : reservations) used += reserved; // Reserved but not created yet.
        used += activeSegment ? std::max(staged, metadataNumber(*db, "max_pending_bytes")) : staged;
        for (const char *suffix : {"", "-wal", "-shm"})
            used += std::max<qint64>(0, QFileInfo(dbPath(options.directory) + suffix).size());
        if (used >= ceiling || required > ceiling - used)
            error("Index disk headroom is exhausted; original evidence was retained");
    }
};

Indexer::Indexer(const IndexerOptions &options) : d(std::make_unique<Impl>(options)) {}
Indexer::~Indexer() = default;

int Indexer::retryFailed() {
    while (!d->stopRequested()) {
        try {
            d->initializeDatabase();
            if (d->legacy) return 0;
            d->db->exec("UPDATE frames SET ocr_state='pending',ocr_error='' "
                        "WHERE ocr_state='failed' AND source_path<>''");
            d->resetGeometry();
            return sqlite3_changes(d->db->handle);
        } catch (const SqliteError &exception) {
            if (d->deferContention(exception)) return 0;
        }
    }
    return 0;
}

IndexResult Indexer::processNext() {
    d->databasePollError = nullptr;
    try { return processNextOnce(); }
    catch (const SqliteError &exception) {
        IndexResult result;
        result.canceled = d->deferContention(exception);
        result.state = result.canceled ? "pending" : "busy";
        return result;
    }
}

IndexResult Indexer::processNextOnce() {
    IndexResult result;
    if (d->stopRequested()) { result.canceled = true; result.state = "pending"; return result; }
    d->initializeDatabase();
    if (d->legacy) return result;
    cleanReadySources(*d->db, d->options.directory);
    QString source;
    int width = 0, height = 0;
    qint64 timestamp = 0;
    // Scheduling holds a short transaction only; no cursor/transaction survives
    // into image decoding or recognition while the capture process writes.
    if (!d->pickNext(result, source, width, height, timestamp)) return result;
    QString text;
    QString geometry;
    QString pixels;
    QByteArray sourceDigest;
    bool donor = false, publishing = false, published = false;
    ImageFingerprint current;
    QElapsedTimer timer; timer.start();
    try {
        if (source.isEmpty() || width <= 0 || height <= 0 || qint64(width) * height > 32 * 1024 * 1024)
            error("Pending original has invalid source geometry");
        QImage image;
        {
            const QImage original = readOriginalImage(d->options.directory, source, width, height,
                                                     d->reuseEnabled ? &sourceDigest : nullptr);
            d->decodeWallMs += timer.nsecsElapsed() / 1e6;
            if (d->reuseEnabled && d->options.ocrMaxHeight && original.height() > d->options.ocrMaxHeight) {
                timer.restart();
                pixels = QString::fromLatin1(fingerprint(original, d->options.ocrMode == "incremental",
                                                        d->options.ocrMode == "regions").digest.toHex());
                d->hashWallMs += timer.nsecsElapsed() / 1e6;
            }
            image = d->prepareOcrImage(original);
        } // A resized worker input no longer needs its full-resolution decode.
        timer.restart();
        current = fingerprint(image, d->options.ocrMode == "incremental", d->options.ocrMode == "regions");
        if (d->reuseEnabled && pixels.isEmpty()) pixels = QString::fromLatin1(current.digest.toHex());
        d->hashWallMs += timer.nsecsElapsed() / 1e6;
        if (!d->ocr) {
            d->initializeOcr();
            if (d->reuseEnabled) d->initializeReuseProfile();
        }
        if (!d->reuseProfile.isEmpty()) {
            if (d->options.stopRequested && d->options.stopRequested()) throw OcrCancelled("Indexing canceled before reuse");
            // Verify the compressed source still contains the bytes we decoded.
            // The SQL identity guard separately protects replacement of its row.
            if (!d->sameSource(source, width, height, sourceDigest)) {
                ++d->obsoleteJobs; d->resetGeometry(); result.processed = true; result.state = "obsolete";
                return result;
            }
            publishing = true;
            d->db->exec("BEGIN IMMEDIATE");
            if (d->findReuse(pixels, width, height, text, geometry)) {
                const bool changed = d->publish(result.frameId, source, width, height, timestamp, text, geometry, pixels, true);
                d->db->exec("COMMIT"); publishing = false; published = true;
                d->resetGeometry();
                if (changed) { ++d->reuseHits; ++d->reuseStores; ++d->processed; d->reusePixels += qint64(width) * height; }
                else ++d->obsoleteJobs;
                result.processed = true; result.state = changed ? "ready" : "obsolete";
                cleanReadySources(*d->db, d->options.directory);
                return result;
            }
            d->db->exec("COMMIT"); publishing = false;
        }
        timer.restart(); const double cpuStart = cpuMs();
        const qint64 fullBefore = d->ocrFullFrames;
        try { text = d->recognizeFrame(image, current); }
        catch (...) {
            result.ocrMs = timer.nsecsElapsed() / 1e6;
            d->ocrWallMs += result.ocrMs; d->ocrCpuMs += cpuMs() - cpuStart;
            throw;
        }
        result.ocrMs = timer.nsecsElapsed() / 1e6;
        d->ocrWallMs += result.ocrMs; d->ocrCpuMs += cpuMs() - cpuStart;
        if (d->options.stopRequested && d->options.stopRequested()) throw OcrCancelled("Indexing canceled before publication");
        if (text.toUtf8().size() > 1024 * 1024) error("Recognized text exceeds this prototype's per-frame index limit");
        geometry = serializedGeometry(d->cachedLines, image.size(), QSize(width, height));
        donor = d->ocrFullFrames > fullBefore && d->cachedGeometryComplete &&
                (d->cachedLines.isEmpty() || geometry != "[]");
    } catch (const OcrCancelled &) {
        ++d->canceledJobs; d->resetGeometry(); result.canceled = true; result.state = "pending";
        return result;
    } catch (const SqliteError &) {
        if (!sqlite3_get_autocommit(d->db->handle)) d->db->exec("ROLLBACK");
        d->resetGeometry();
        // Scheduler/reuse reads can fail during recognition too. Database
        // failures are not evidence of a bad image or a failed OCR result.
        throw;
    } catch (const std::exception &exception) {
        if (d->databasePollError) std::rethrow_exception(d->databasePollError);
        if (publishing || published) {
            if (publishing) sqlite3_exec(d->db->handle, "ROLLBACK", nullptr, nullptr, nullptr);
            d->resetGeometry();
            // A failed transaction leaves the job pending. Cleanup failure
            // after commit must not misreport an already-ready hit as obsolete.
            throw;
        }
        result.error = QString::fromUtf8(exception.what()).left(2000);
        Statement failed(*d->db, "UPDATE frames SET ocr_state='failed',ocr_error=? WHERE id=? AND ocr_state='pending' "
            "AND source_path=? AND timestamp_ms=? AND width=? AND height=?");
        failed.bind(1, result.error); failed.bind(2, result.frameId); failed.bind(3, source);
        failed.bind(4, timestamp); failed.bind(5, width); failed.bind(6, height); failed.next();
        const bool changed = sqlite3_changes(d->db->handle) == 1;
        d->consumeRequest(result.frameId);
        if (changed) ++d->failedJobs; else ++d->obsoleteJobs;
        d->resetGeometry(); result.processed = true; result.state = changed ? "failed" : "obsolete";
        return result;
    }
    try {
        if (!sourceDigest.isEmpty() && !d->sameSource(source, width, height, sourceDigest)) {
            ++d->obsoleteJobs; d->resetGeometry(); result.processed = true; result.state = "obsolete";
            return result;
        }
        d->db->exec("BEGIN IMMEDIATE");
        if (!d->publish(result.frameId, source, width, height, timestamp, text, geometry, pixels, donor)) {
            d->db->exec("COMMIT");
            ++d->obsoleteJobs; d->resetGeometry();
            result.processed = true; result.state = "obsolete";
            return result;
        }
        d->db->exec("COMMIT");
        if (donor && !d->reuseProfile.isEmpty()) ++d->reuseStores;
    } catch (...) {
        sqlite3_exec(d->db->handle, "ROLLBACK", nullptr, nullptr, nullptr);
        d->resetGeometry();
        throw; // A publication error leaves the durable job pending.
    }
    d->lastFingerprint = current;
    d->lastIndexedFrameId = result.frameId; d->lastIndexedTimestamp = timestamp;
    ++d->processed; result.processed = true; result.state = "ready";
    cleanReadySources(*d->db, d->options.directory);
    return result;
}

QJsonObject Indexer::statsJSON() const {
    QJsonObject stats{{"directory", d->options.directory}, {"processed", d->processed}, {"failed_jobs", d->failedJobs},
        {"database_contentions", d->databaseContentions}, {"database_last_contention_code", d->lastContentionCode},
        {"database_retry_wait_ms", d->databaseRetryWaitMs},
        {"priority_jobs", d->priorityJobs}, {"oldest_jobs", d->oldestJobs}, {"obsolete_jobs", d->obsoleteJobs},
        {"ocr_discontinuity_resets", d->discontinuityResets},
        {"ocr_reuse_enabled", d->reuseEnabled}, {"ocr_reuse_profile_valid", !d->reuseProfile.isEmpty()},
        {"ocr_reuse_limit", MaxOcrReuseEntries}, {"ocr_reuse_lookups", d->reuseLookups}, {"ocr_reuse_hits", d->reuseHits},
        {"ocr_reuse_misses", d->reuseMisses}, {"ocr_reuse_stores", d->reuseStores}, {"ocr_reuse_invalidations", d->reuseInvalidations},
        {"ocr_reuse_original_pixels", d->reusePixels}, {"ocr_reuse_identity_ms", d->reuseIdentityMs},
        {"ocr_reuse_lookup_ms", d->reuseLookupMs},
        {"ocr_cpu_percent", d->options.ocrCpuPercent}, {"ocr_max_wall_ms", d->options.ocrMaxWallMs},
        {"canceled_jobs", d->canceledJobs}, {"indexing", indexingStatus(d->options.directory)},
        {"elapsed_ms", double(d->lifetime.elapsed())}, {"process_cpu_ms", cpuMs() - d->startCpuMs},
        {"ocr_wall_ms", d->ocrWallMs}, {"ocr_cpu_ms", d->ocrCpuMs}, {"decode_wall_ms", d->decodeWallMs},
        {"hash_wall_ms", d->hashWallMs}, {"ocr_init_ms", d->initOcrMs}, {"ocr_mode", d->options.ocrMode},
        {"ocr_full_frames", d->ocrFullFrames}, {"ocr_partial_frames", d->ocrPartialFrames},
        {"ocr_partial_fallbacks", d->ocrPartialFallbacks}, {"ocr_processed_pixels", d->ocrProcessedPixels}, {"ocr_input_pixels", d->ocrInputPixels},
        {"ocr_budget_sleep_ms", d->ocrBudgetSleepMs}, {"ocr_budget_sleep_count", qint64(d->ocrBudgetSleeps)},
        {"ocr_budget_checkpoints", qint64(d->ocrBudgetCheckpoints)}, {"ocr_budget_cancellations", qint64(d->ocrBudgetCancellations)},
        {"ocr_budget_deadlines", qint64(d->ocrBudgetDeadlines)}, {"ocr_max_callback_wall_gap_ms", d->ocrMaxCallbackWallGapMs},
        {"ocr_max_callback_cpu_gap_ms", d->ocrMaxCallbackCpuGapMs}, {"allocator_trim_attempts", d->allocatorTrimAttempts},
        {"allocator_trim_releases", d->allocatorTrimReleases}, {"allocator_trim_cpu_ms", d->allocatorTrimCpuMs}, {"allocator_trim_wall_ms", d->allocatorTrimWallMs}};
    d->addExperimentStats(stats);
    return stats;
}

namespace {
HistoryMaintenanceResult removeHistoryObservations(const QString &requestedDirectory, qint64 from, qint64 until,
                                                    int maxObservations, int maxFiles, bool purgeMeetings = false, bool retention = false) {
    if (from < 0 || until < from || maxObservations < 1 || maxObservations > 10000 || maxFiles < 1 || maxFiles > 1024)
        error("Invalid bounded history maintenance range or batch size");
    const QString directory = privateHistoryDirectory(requestedDirectory);
    Database db(dbPath(directory), false, true);
    sqlite3_busy_timeout(db.handle, 100);
    requireHistory(db);
    HistoryMaintenanceResult result;
    try {
        db.exec("PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; BEGIN IMMEDIATE");
        QVector<qint64> observations;
        QHash<qint64, qint64> affected;
        {
            Statement rows(db, "SELECT id,frame_id FROM observations WHERE timestamp_ms>=? AND timestamp_ms<? ORDER BY timestamp_ms,id LIMIT ?");
            rows.bind(1, from); rows.bind(2, until); rows.bind(3, maxObservations);
            while (rows.next()) { observations.append(rows.number(0)); ++affected[rows.number(1)]; }
        }
        for (const auto id : observations) {
            Statement erase(db, "DELETE FROM observations WHERE id=?"); erase.bind(1, id); erase.next();
        }
        qint64 framesRemoved = 0;
        for (auto it = affected.cbegin(); it != affected.cend(); ++it) {
            bool survives = false;
            {
                Statement remaining(db, "SELECT 1 FROM observations WHERE frame_id=? LIMIT 1");
                remaining.bind(1, it.key()); survives = remaining.next();
            }
            if (survives) {
                Statement bounds(db, "UPDATE frames SET observation_count=observation_count-?,"
                    "timestamp_ms=(SELECT timestamp_ms FROM observations WHERE frame_id=? ORDER BY timestamp_ms,id LIMIT 1),"
                    "last_timestamp_ms=(SELECT timestamp_ms FROM observations WHERE frame_id=? ORDER BY timestamp_ms DESC,id DESC LIMIT 1) WHERE id=?");
                bounds.bind(1, it.value()); bounds.bind(2, it.key()); bounds.bind(3, it.key()); bounds.bind(4, it.key()); bounds.next();
            } else {
                Statement retire(db, "UPDATE history_media SET state='retired' WHERE path=(SELECT path FROM frames WHERE id=?)");
                retire.bind(1, it.key()); retire.next();
                for (const char *sql : {"DELETE FROM frame_text WHERE rowid=?", "DELETE FROM frame_ocr_geometry WHERE frame_id=?",
                                       "DELETE FROM index_requests WHERE frame_id=?"}) {
                    Statement erase(db, sql); erase.bind(1, it.key()); erase.next();
                }
                // Reuse is optional on histories that have never enabled it.
                {
                    Statement reuse(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='ocr_reuse'");
                    if (reuse.next()) {
                        Statement erase(db, "DELETE FROM ocr_reuse WHERE frame_id=?"); erase.bind(1, it.key()); erase.next();
                    }
                }
                Statement erase(db, "DELETE FROM frames WHERE id=?"); erase.bind(1, it.key()); erase.next();
                framesRemoved += sqlite3_changes(db.handle);
            }
        }
        adjustMetadata(db, "history_observations", -observations.size());
        adjustMetadata(db, "history_frames", -framesRemoved);
        struct Gap { qint64 id, start, end; QString reason; };
        QVector<Gap> gaps;
        {
            Statement rows(db, "SELECT id,start_ms,end_ms,reason FROM history_gaps WHERE start_ms<? AND end_ms>? ORDER BY end_ms,id LIMIT ?");
            rows.bind(1, until); rows.bind(2, from); rows.bind(3, maxObservations);
            while (rows.next()) gaps.append({rows.number(0), rows.number(1), rows.number(2), rows.string(3)});
        }
        qint64 gapsRemoved = 0;
        for (const auto &gap : gaps) {
            if (gap.start >= from && gap.end <= until) {
                Statement erase(db, "DELETE FROM history_gaps WHERE id=?"); erase.bind(1, gap.id); erase.next(); ++gapsRemoved;
            } else if (gap.start < from && gap.end > until) {
                Statement left(db, "UPDATE history_gaps SET end_ms=? WHERE id=?"); left.bind(1, from); left.bind(2, gap.id); left.next();
                Statement right(db, "INSERT INTO history_gaps(start_ms,end_ms,reason) VALUES(?,?,?)");
                right.bind(1, until); right.bind(2, gap.end); right.bind(3, gap.reason); right.next();
            } else {
                Statement clip(db, "UPDATE history_gaps SET start_ms=?,end_ms=? WHERE id=?");
                clip.bind(1, gap.start < from ? gap.start : until);
                clip.bind(2, gap.start < from ? from : gap.end); clip.bind(3, gap.id); clip.next();
            }
        }
        const auto meetingsRemoved = purgeMeetings ? deleteMeetingsInRange(db.handle, from, until, maxObservations, retention) : 0;
        db.exec("COMMIT");
        result.observationsRemoved = observations.size(); result.framesRemoved = framesRemoved; result.gapsRemoved = gapsRemoved;
        // Recover capture intents only while holding its idle lease. The active
        // recorder owns its current intent between reservation and publication.
        Descriptor idle(::open(QFile::encodeName(directory + "/.capture.lock").constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
        if (idle.fd >= 0 && flock(idle.fd, LOCK_EX | LOCK_NB) == 0)
            db.exec("UPDATE history_media SET state='retired' WHERE state='writing' AND NOT EXISTS(SELECT 1 FROM frames WHERE frames.path=history_media.path)");
        collectRetiredMedia(db, directory, maxFiles, result);
        // Incremental vacuum is bounded and effective on new persistent stores.
        // Existing prototype databases retain their page format and reuse freed pages.
        db.exec("PRAGMA incremental_vacuum(32); PRAGMA wal_checkpoint(PASSIVE)");
        Statement remaining(db, "SELECT EXISTS(SELECT 1 FROM observations WHERE timestamp_ms>=? AND timestamp_ms<?) OR "
            "EXISTS(SELECT 1 FROM history_gaps WHERE start_ms<? AND end_ms>?) OR EXISTS(SELECT 1 FROM history_media WHERE state='retired')");
        remaining.bind(1, from); remaining.bind(2, until); remaining.bind(3, until); remaining.bind(4, from);
        remaining.next(); result.more = remaining.number(0) != 0 || meetingsRemoved >= maxObservations;
        if (!result.more) {
            Statement vacuum(db, "PRAGMA auto_vacuum"); vacuum.next();
            if (vacuum.number(0) == 2) {
                Statement pages(db, "PRAGMA freelist_count"); pages.next(); result.more = pages.number(0) > 0;
            }
        }
    } catch (const SqliteError &exception) {
        if (!sqlite3_get_autocommit(db.handle)) db.exec("ROLLBACK");
        if (!exception.contention()) throw;
        result.busy = true; result.more = true;
    } catch (...) { sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
    result.diskBytes = historyDiskBytes(db, directory);
    return result;
}
} // namespace

HistoryMaintenanceResult maintainHistory(const QString &directory, qint64 expireBeforeMs, int maxObservations, int maxFiles) {
    return removeHistoryObservations(directory, 0, expireBeforeMs, maxObservations, maxFiles, true, true);
}

HistoryMaintenanceResult deleteHistoryRange(const QString &directory, qint64 fromInclusiveMs, qint64 toExclusiveMs,
                                           int maxObservations, int maxFiles) {
    return removeHistoryObservations(directory, fromInclusiveMs, toExclusiveMs, maxObservations, maxFiles, true, false);
}

HistorySpaceResult makeHistorySpace(const QString &requestedDirectory, quint64 maxDiskBytes,
                                   quint64 minFreeBytes, quint64 additionalBytes,
                                   int maxObservations, int maxFiles) {
    if (maxDiskBytes < 16 * MiB || maxObservations < 1 || maxObservations > 10000 || maxFiles < 1 || maxFiles > 1024)
        error("Invalid rolling history limits");
    HistorySpaceResult result;
    // An oversized observation must not erase history in a futile attempt to
    // fit. The caller includes the bounded OCR/index publication reserve.
    if (additionalBytes >= maxDiskBytes || additionalBytes > quint64(std::numeric_limits<qint64>::max()) - minFreeBytes) {
        result.reason = "A new moment cannot fit within the storage allowance. Increase the allowance to resume recording.";
        return result;
    }
    const QString directory = privateHistoryDirectory(requestedDirectory);
    Database db(dbPath(directory), false, true);
    sqlite3_busy_timeout(db.handle, 100);
    requireHistory(db);
    QStorageInfo disk(directory);
    auto available = [&]() -> qint64 {
        disk.refresh();
        return disk.isValid() && disk.isReady() && !disk.isReadOnly() ? disk.bytesAvailable() : -1;
    };
    auto fits = [&](quint64 used, qint64 free) {
        return free >= 0 && used < maxDiskBytes && additionalBytes <= maxDiskBytes - used &&
            quint64(free) >= minFreeBytes && additionalBytes <= quint64(free) - minFreeBytes;
    };
    try {
        auto used = historyDiskBytes(db, directory);
        const auto initialBytes = used;
        auto free = available();
        if (fits(used, free)) { result.ready = true; return result; }
        if (free < 0) { result.reason = "The history disk is unavailable or read-only. Recording will resume when it is available."; return result; }

        // Finish interrupted retirements before choosing any more observations.
        // This also reclaims reserved originals left by a prior crashed writer.
        result.maintenance = removeHistoryObservations(directory, 0, 0, maxObservations, maxFiles);
        if (result.maintenance.busy) {
            result.more = true; result.reason = "Waiting for the history database before rolling out older moments."; return result;
        }
        {
            // A passive checkpoint does not shrink the physical WAL file. Only
            // do this at a capacity boundary, with the same bounded busy wait.
            Statement checkpoint(db, "PRAGMA wal_checkpoint(TRUNCATE)");
            if (checkpoint.next() && checkpoint.number(0) != 0) {
                result.more = true; result.reason = "Waiting for history readers before reclaiming the index journal."; return result;
            }
        }
        used = historyDiskBytes(db, directory); free = available();
        if (fits(used, free)) { result.ready = true; return result; }
        if (free < 0) { result.reason = "The history disk is unavailable or read-only. Recording will resume when it is available."; return result; }

        const quint64 owned = metadataNumber(db, "history_media_bytes");
        const quint64 fixedBytes = used > owned ? used - owned : 0;
        bool shrinkable = false;
        qint64 unusedPages = 0;
        { Statement vacuum(db, "PRAGMA auto_vacuum"); vacuum.next(); shrinkable = vacuum.number(0) == 2; }
        if (shrinkable) { Statement pages(db, "PRAGMA freelist_count"); pages.next(); unusedPages = pages.number(0); }
        const bool indexPressure = fixedBytes >= maxDiskBytes || additionalBytes > maxDiskBytes - fixedBytes;
        if (!shrinkable && indexPressure) {
            result.reason = "This older history index cannot shrink incrementally to this allowance. Increase the allowance; compacting its index requires separate maintenance.";
            return result;
        }
        // Existing free pages should be reclaimed before evicting more rows.
        // The service's bounded maintenance batches can continue this between
        // capture intervals without repeatedly capturing/encoding screenshots.
        if (unusedPages > 0) {
            result.more = used < initialBytes;
            result.reason = result.more ? "Reclaiming unused history index pages before recording continues."
                                        : "The history index could not release its unused pages. Check the history disk.";
            return result;
        }
        if (result.maintenance.filesRemoved >= maxFiles) {
            result.more = true; result.reason = "Continuing bounded cleanup of older history files.";
            return result;
        }
        const quint64 recoverableIndex = shrinkable && fixedBytes > 64 * 1024 ? fixedBytes - 64 * 1024 : 0;
        if (quint64(free) < minFreeBytes + additionalBytes && owned + recoverableIndex < minFreeBytes + additionalBytes - quint64(free)) {
            result.reason = "Replay cannot recover the free-disk reserve from its own history. Free disk space or lower the reserve to resume recording.";
            return result;
        }

        // Reclaim a little beyond the next image to amortize cleanup, bounded to
        // 1% of the allowance or 32 MiB. Never discard a large arbitrary batch.
        const quint64 headroom = std::min(maxDiskBytes / 100, 32 * MiB);
        const quint64 indexFloor = shrinkable ? std::min(fixedBytes, quint64(64 * 1024)) : fixedBytes;
        const quint64 targetAdditional = std::min(additionalBytes + headroom, maxDiskBytes - indexFloor);
        const quint64 allowanceNeed = used > maxDiskBytes - targetAdditional ? used - (maxDiskBytes - targetAdditional) : 0;
        const quint64 freeNeed = quint64(free) < minFreeBytes + targetAdditional ? minFreeBytes + targetAdditional - quint64(free) : 0;
        const quint64 needed = std::max(allowanceNeed, freeNeed);
        quint64 estimated = 0;
        int observations = 0;
        qint64 lastTimestamp = 0, firstTimestamp = 0;
        QHash<qint64, qint64> counts;
        {
            Statement oldest(db, "SELECT o.timestamp_ms,o.frame_id,f.observation_count,m.bytes FROM observations o "
                "JOIN frames f ON f.id=o.frame_id JOIN history_media m ON m.path=f.path "
                "ORDER BY o.timestamp_ms,o.id LIMIT ?");
            oldest.bind(1, maxObservations);
            while (oldest.next()) {
                ++observations; lastTimestamp = oldest.number(0);
                if (observations == 1) firstTimestamp = lastTimestamp;
                if (++counts[oldest.number(1)] == oldest.number(2)) {
                    estimated += quint64(oldest.number(3));
                    if (indexPressure) break; // Release one frame's text pages, then reassess.
                }
                if (estimated >= needed) break;
            }
        }
        // Text-only meetings participate in oldest-first rolling storage.
        // Reassess actual reclaimed pages after one transcript before deciding
        // whether a newer screen observation also needs to be removed.
        {
            Statement exists(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='meeting_records'");
            if (exists.next()) {
                Statement meeting(db, "SELECT retention_ms FROM meeting_records ORDER BY retention_ms,id LIMIT 1");
                if (meeting.next() && (observations == 0 || meeting.number(0) <= firstTimestamp)) observations = 0;
            }
        }
        bool meetingEvicted = false;
        if (observations > 0) {
            const auto removed = removeHistoryObservations(directory, 0,
                lastTimestamp == std::numeric_limits<qint64>::max() ? lastTimestamp : lastTimestamp + 1, observations,
                maxFiles - int(result.maintenance.filesRemoved), true, true);
            result.maintenance.observationsRemoved += removed.observationsRemoved;
            result.maintenance.framesRemoved += removed.framesRemoved;
            result.maintenance.filesRemoved += removed.filesRemoved;
            result.maintenance.gapsRemoved += removed.gapsRemoved;
            result.maintenance.bytesReclaimed += removed.bytesReclaimed;
            result.maintenance.busy = removed.busy;
            result.maintenance.more = removed.more;
        } else {
            // A history can contain imported transcripts without any retained
            // screens. Its text index participates in the same rolling cap.
            db.exec("BEGIN IMMEDIATE");
            try { meetingEvicted = evictOldestMeeting(db.handle); db.exec("COMMIT"); }
            catch (...) { sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr); throw; }
            if (meetingEvicted) db.exec("PRAGMA incremental_vacuum(32)");
        }
        result.maintenance.diskBytes = historyDiskBytes(db, directory);
        result.ready = fits(result.maintenance.diskBytes, available());
        result.more = !result.ready && (meetingEvicted || result.maintenance.busy || result.maintenance.observationsRemoved > 0 ||
                                      result.maintenance.filesRemoved > 0 || result.maintenance.diskBytes < initialBytes);
        result.reason = result.ready ? QString() : result.more
            ? "Rolling out older moments to make room for new recording."
            : "Replay could not safely reclaim enough storage. Check the history disk or increase the allowance.";
    } catch (const SqliteError &exception) {
        if (!exception.contention()) throw;
        result.more = true; result.reason = "Waiting for the history database before rolling out older moments.";
    }
    return result;
}

QJsonObject historyUsage(const QString &requestedDirectory) {
    const QString directory = privateHistoryDirectory(requestedDirectory);
    Database db(dbPath(directory), false);
    requireHistory(db);
    db.exec("BEGIN");
    QJsonObject result{{"directory", directory}, {"disk_bytes", double(historyDiskBytes(db, directory))},
        {"media_bytes", double(metadataNumber(db, "history_media_bytes"))},
        {"observations", qint64(metadataNumber(db, "history_observations"))},
        {"frames", qint64(metadataNumber(db, "history_frames"))}, {"first_timestamp_ms", QJsonValue::Null},
        {"last_timestamp_ms", QJsonValue::Null}};
    {
        Statement first(db, "SELECT timestamp_ms FROM observations ORDER BY timestamp_ms,id LIMIT 1");
        if (first.next()) result["first_timestamp_ms"] = first.number(0);
    }
    {
        Statement last(db, "SELECT timestamp_ms FROM observations ORDER BY timestamp_ms DESC,id DESC LIMIT 1");
        if (last.next()) result["last_timestamp_ms"] = last.number(0);
    }
    Statement retired(db, "SELECT COUNT(*) FROM history_media WHERE state='retired'");
    retired.next(); result["cleanup_pending"] = retired.number(0);
    db.exec("COMMIT");
    return result;
}

void recordGap(const QString &requestedDirectory, qint64 startMs, qint64 endMs, const QString &reason) {
    if (startMs < 0 || endMs < startMs || reason.trimmed().isEmpty() || reason.toUtf8().size() > 256)
        error("Invalid history gap interval or reason");
    if (startMs == endMs) return;
    const QString directory = privateHistoryDirectory(requestedDirectory);
    Database db(dbPath(directory), false, true);
    requireHistory(db);
    db.exec("PRAGMA synchronous=FULL");
    Statement insert(db, "INSERT INTO history_gaps(start_ms,end_ms,reason) VALUES(?,?,?)");
    insert.bind(1, startMs); insert.bind(2, endMs); insert.bind(3, reason.trimmed()); insert.next();
}

QVector<FrameRecord> listFrames(const QString &directory, int limit, int offset) {
    Database db(dbPath(directory), false);
    const QByteArray sql = frameColumns(db) + "ORDER BY f.timestamp_ms,f.id LIMIT ? OFFSET ?";
    Statement query(db, sql.constData()); query.bind(1, std::clamp(limit, 1, 1000)); query.bind(2, std::max(offset, 0));
    return readRows(query);
}

QVector<FrameRecord> searchFrames(const QString &directory, const QString &text, int limit) {
    // Extract literal unicode words; user punctuation/operators never become FTS syntax.
    const auto words = searchTerms(text);
    if (words.isEmpty()) return {};
    Database db(dbPath(directory), false);
    const QByteArray sql = frameColumns(db) +
        "JOIN frame_text ON frame_text.rowid=f.id WHERE frame_text MATCH ? ORDER BY rank,f.timestamp_ms LIMIT ?";
    Statement query(db, sql.constData()); query.bind(1, searchExpression(words)); query.bind(2, std::clamp(limit, 1, 1000));
    return readRows(query);
}

SearchPage searchFramePage(const QString &directory, const QString &text, int limit, qint64 offset,
                          SearchMode mode, int timelineLimit, qint64 anchorFrameId) {
    SearchPage result;
    result.offset = std::max<qint64>(0, offset);
    const auto terms = searchTerms(text);
    if (terms.isEmpty()) return result;
    Database db(dbPath(directory), false);
    const QString expression = searchExpression(db, terms, mode);
    db.exec("BEGIN");
    qint64 firstId = 0, lastId = 0, lastStart = 0;
    {
        Statement summary(db, "SELECT COUNT(*),MIN(f.timestamp_ms),MAX(f.last_timestamp_ms),MIN(f.id),MAX(f.id),MAX(f.timestamp_ms) "
            "FROM frames f JOIN frame_text ON frame_text.rowid=f.id WHERE frame_text MATCH ?");
        summary.bind(1, expression); summary.next();
        result.totalMatches = result.timeline.totalFrames = summary.number(0);
        result.timeline.firstTimestampMs = summary.number(1);
        result.timeline.lastTimestampMs = summary.number(2);
        firstId = summary.number(3); lastId = summary.number(4); lastStart = summary.number(5);
    }
    if (!result.totalMatches) { result.offset = 0; db.exec("COMMIT"); return result; }
    const int pageSize = std::clamp(limit, 1, 1000);
    if (anchorFrameId > 0) {
        // A newly indexed earlier frame can shift every later ordinal. Resolve
        // the stable marker ID and its page in the same snapshot as the rows.
        Statement anchor(db, "SELECT f.timestamp_ms FROM frames f JOIN frame_text ON frame_text.rowid=f.id "
            "WHERE f.id=? AND frame_text MATCH ?");
        anchor.bind(1, anchorFrameId); anchor.bind(2, expression);
        if (anchor.next()) {
            const qint64 timestamp = anchor.number(0);
            Statement rank(db, "SELECT COUNT(*) FROM frames f JOIN frame_text ON frame_text.rowid=f.id "
                "WHERE frame_text MATCH ? AND (f.timestamp_ms<? OR (f.timestamp_ms=? AND f.id<?))");
            rank.bind(1, expression); rank.bind(2, timestamp); rank.bind(3, timestamp); rank.bind(4, anchorFrameId);
            rank.next(); const qint64 ordinal = rank.number(0);
            result.offset = (ordinal / pageSize) * pageSize;
            result.selectedRow = int(ordinal % pageSize);
        }
    }
    // Expiration can remove complete pages between refreshes. Keep the cursor
    // on a remaining page even when its former anchor was also deleted.
    if (result.offset >= result.totalMatches)
        result.offset = ((result.totalMatches - 1) / pageSize) * pageSize;
    {
        const QByteArray sql = frameColumns(db) +
            "JOIN frame_text ON frame_text.rowid=f.id WHERE frame_text MATCH ? ORDER BY f.timestamp_ms,f.id LIMIT ? OFFSET ?";
        Statement rows(db, sql.constData()); rows.bind(1, expression);
        rows.bind(2, pageSize); rows.bind(3, result.offset);
        result.frames = readRows(rows);
    }
    const int markerLimit = std::clamp(timelineLimit, 2, 2048);
    if (result.totalMatches <= markerLimit) {
        Statement markers(db, "SELECT f.id,f.timestamp_ms FROM frames f JOIN frame_text ON frame_text.rowid=f.id "
            "WHERE frame_text MATCH ? ORDER BY f.timestamp_ms,f.id LIMIT ?");
        markers.bind(1, expression); markers.bind(2, markerLimit);
        while (markers.next()) result.timeline.points.append({markers.number(0), markers.number(1), "ready"});
    } else {
        // Aggregate inside SQLite rather than materializing every match or
        // sampling just the first page. Both endpoints survive sparse history.
        result.timeline.points.append({firstId, result.timeline.firstTimestampMs, "ready"});
        if (markerLimit > 2) {
            Statement markers(db, "SELECT MIN(f.id),MIN(f.timestamp_ms) FROM frames f "
                "JOIN frame_text ON frame_text.rowid=f.id WHERE frame_text MATCH ? AND f.id<>? AND f.id<>? "
                "GROUP BY MIN(?,CAST((f.timestamp_ms-?)*1.0*?/(?+1.0) AS INTEGER)) ORDER BY MIN(f.timestamp_ms),MIN(f.id)");
            markers.bind(1, expression); markers.bind(2, firstId); markers.bind(3, lastId);
            markers.bind(4, markerLimit - 3); markers.bind(5, result.timeline.firstTimestampMs);
            markers.bind(6, markerLimit - 2); markers.bind(7, lastStart - result.timeline.firstTimestampMs);
            while (markers.next()) result.timeline.points.append({markers.number(0), markers.number(1), "ready"});
        }
        result.timeline.points.append({lastId, lastStart, "ready"});
    }
    db.exec("COMMIT");
    return result;
}

std::optional<qint64> searchFrameOffsetNearTimestamp(const QString &directory, const QString &text,
                                                   qint64 timestampMs, SearchMode mode) {
    const auto terms = searchTerms(text);
    if (terms.isEmpty()) return std::nullopt;
    Database db(dbPath(directory), false);
    const QString expression = searchExpression(db, terms, mode);
    db.exec("BEGIN");
    qint64 id = 0, timestamp = 0;
    {
        // Match only retained searchable moments, including intervals covered
        // by repeated observations. SQL returns one row, regardless of history.
        Statement nearest(db, "SELECT f.id,f.timestamp_ms FROM frames f JOIN frame_text ON frame_text.rowid=f.id "
            "WHERE frame_text MATCH ? ORDER BY CASE WHEN ?<f.timestamp_ms THEN f.timestamp_ms-? "
            "WHEN ?>f.last_timestamp_ms THEN ?-f.last_timestamp_ms ELSE 0 END,f.timestamp_ms,f.id LIMIT 1");
        nearest.bind(1, expression);
        for (int i = 2; i <= 5; ++i) nearest.bind(i, timestampMs);
        if (!nearest.next()) { db.exec("COMMIT"); return std::nullopt; }
        id = nearest.number(0); timestamp = nearest.number(1);
    }
    Statement rank(db, "SELECT COUNT(*) FROM frames f JOIN frame_text ON frame_text.rowid=f.id "
        "WHERE frame_text MATCH ? AND (f.timestamp_ms<? OR (f.timestamp_ms=? AND f.id<?))");
    rank.bind(1, expression); rank.bind(2, timestamp); rank.bind(3, timestamp); rank.bind(4, id);
    rank.next(); const qint64 offset = rank.number(0);
    db.exec("COMMIT");
    return offset;
}

std::optional<FrameRecord> adjacentFrame(const QString &directory, qint64 frameId, int direction) {
    Database db(dbPath(directory), false);
    // IDs follow nondecreasing recorded timestamps; this remains O(log n) and
    // deliberately ignores the viewer's current text filter.
    const QByteArray sql = frameColumns(db) + (direction < 0 ? "WHERE f.id<? ORDER BY f.id DESC LIMIT 1" : "WHERE f.id>? ORDER BY f.id LIMIT 1");
    Statement query(db, sql.constData()); query.bind(1, frameId);
    const auto rows = readRows(query);
    if (rows.isEmpty()) return std::nullopt;
    return rows.first();
}

std::optional<FrameRecord> frameById(const QString &directory, qint64 frameId) {
    Database db(dbPath(directory), false);
    const QByteArray sql = frameColumns(db) + "WHERE f.id=?";
    Statement query(db, sql.constData()); query.bind(1, frameId);
    const auto rows = readRows(query);
    if (rows.isEmpty()) return std::nullopt;
    return rows.first();
}

TimelineOverview timelineOverview(const QString &directory, int limit) {
    Database db(dbPath(directory), false);
    db.exec("BEGIN");
    TimelineOverview result;
    qint64 firstId = 0, lastId = 0;
    {
        // Keep COUNT separate so SQLite can use its B-tree count operation;
        // combining it with MIN/MAX would iterate every historical row.
        Statement count(db, "SELECT COUNT(*) FROM frames");
        count.next(); result.totalFrames = count.number(0);
    }
    if (!result.totalFrames) { db.exec("COMMIT"); return result; }
    {
        Statement first(db, "SELECT id,timestamp_ms FROM frames ORDER BY id LIMIT 1");
        first.next(); firstId = first.number(0); result.firstTimestampMs = first.number(1);
        Statement last(db, "SELECT id,last_timestamp_ms FROM frames ORDER BY id DESC LIMIT 1");
        last.next(); lastId = last.number(0); result.lastTimestampMs = last.number(1);
    }
    const QByteArray stateColumn = hasIndexStates(db) ? "ocr_state" : "'ready'";
    const int count = int(std::min<qint64>(result.totalFrames, std::clamp(limit, 2, 2048)));
    result.points.reserve(count);
    if (result.totalFrames <= count) {
        Statement rows(db, ("SELECT id,timestamp_ms," + stateColumn + " FROM frames ORDER BY id LIMIT ?").constData());
        rows.bind(1, count);
        while (rows.next()) result.points.append({rows.number(0), rows.number(1), rows.string(2)});
    } else {
        // One indexed seek per marker avoids loading OCR text or scanning the
        // history for each timeline pixel. Gaps after deletion may coalesce.
        Statement row(db, ("SELECT id,timestamp_ms," + stateColumn + " FROM frames WHERE id>=? ORDER BY id LIMIT 1").constData());
        for (int i = 0; i < count; ++i) {
            const qint64 span = lastId - firstId;
            const qint64 target = firstId + (span / (count - 1)) * i + ((span % (count - 1)) * i) / (count - 1);
            row.bind(1, target);
            if (row.next() && (result.points.isEmpty() || result.points.last().id != row.number(0)))
                result.points.append({row.number(0), row.number(1), row.string(2)});
            sqlite3_reset(row.handle);
        }
    }
    db.exec("COMMIT");
    return result;
}

std::optional<FrameRecord> frameNearTimestamp(const QString &directory, qint64 timestampMs) {
    Database db(dbPath(directory), false);
    db.exec("BEGIN");
    const QByteArray columns = frameColumns(db);
    Statement before(db, (columns + "WHERE f.timestamp_ms<=? ORDER BY f.timestamp_ms DESC,f.id DESC LIMIT 1").constData());
    before.bind(1, timestampMs);
    const auto earlier = readRows(before);
    if (!earlier.isEmpty() && earlier.first().lastTimestampMs >= timestampMs) {
        db.exec("COMMIT"); return earlier.first();
    }
    Statement after(db, (columns + "WHERE f.timestamp_ms>? ORDER BY f.timestamp_ms,f.id LIMIT 1").constData());
    after.bind(1, timestampMs);
    const auto later = readRows(after);
    db.exec("COMMIT");
    if (earlier.isEmpty()) return later.isEmpty() ? std::nullopt : std::optional<FrameRecord>(later.first());
    if (later.isEmpty()) return earlier.first();
    return timestampMs - earlier.first().lastTimestampMs <= later.first().timestampMs - timestampMs
        ? earlier.first() : later.first();
}

TextMatches matchingTextLines(const QString &directory, qint64 frameId, const QString &text, SearchMode mode) {
    const auto terms = searchTerms(text);
    if (frameId <= 0 || terms.isEmpty()) return {};
    Database db(dbPath(directory), false);
    {
        Statement exists(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='frame_ocr_geometry'");
        if (!exists.next()) return {};
    }
    // Verify exactly the same complete query as searchFrames. A timeline frame
    // containing only one of multiple requested words must not look like a hit.
    SearchTokenizer tokenizer(db);
    const bool prefix = prefixLastTerm(tokenizer, terms, mode);
    Statement query(db, "SELECT g.lines_json,f.width,f.height FROM frame_ocr_geometry g JOIN frames f ON f.id=g.frame_id "
        "JOIN frame_text ON frame_text.rowid=f.id WHERE f.id=? AND frame_text MATCH ? AND length(g.lines_json)<=3145728");
    query.bind(1, frameId); query.bind(2, searchExpression(terms, prefix));
    if (!query.next()) return {};
    const auto document = QJsonDocument::fromJson(query.string(0).toUtf8());
    if (!document.isArray() || document.array().size() > 8000) return {};
    const QRect image(0, 0, int(query.number(1)), int(query.number(2)));
    QVector<QVector<QByteArray>> needles;
    for (const auto &term : terms) {
        auto tokens = tokenizer.tokens(term);
        if (!tokens.isEmpty()) needles.append(std::move(tokens));
    }
    TextMatches result;
    QStringList matchingLines;
    for (const auto &value : document.array()) {
        const auto line = value.toArray();
        if (line.size() != 5 || !line[4].isString()) continue;
        const QRect box(line[0].toInt(), line[1].toInt(), line[2].toInt(), line[3].toInt());
        if (box.isEmpty() || !image.contains(box)) continue;
        const QString recognizedLine = line[4].toString().trimmed();
        const auto tokens = tokenizer.tokens(recognizedLine);
        if (tokens.isEmpty()) continue;
        result.geometryAvailable = true;
        for (qsizetype n = 0; n < needles.size(); ++n) {
            const auto &needle = needles[n];
            const bool lastPrefix = prefix && n == needles.size() - 1;
            bool matched = false;
            for (qsizetype start = 0; start + needle.size() <= tokens.size() && !matched; ++start) {
                matched = true;
                for (qsizetype word = 0; word < needle.size(); ++word) {
                    const auto &token = tokens[start + word];
                    if (lastPrefix && word == needle.size() - 1 ? !token.startsWith(needle[word]) : token != needle[word]) {
                        matched = false; break;
                    }
                }
            }
            if (matched) {
                result.boxes.append(box);
                matchingLines.append(recognizedLine);
                break; // Multiple requested words on one line still copy it once.
            }
        }
    }
    result.text = matchingLines.join('\n');
    return result;
}

QVector<QRect> matchingTextRects(const QString &directory, qint64 frameId, const QString &text, SearchMode mode) {
    return matchingTextLines(directory, frameId, text, mode).boxes;
}

int requestIndexing(const QString &directory, qint64 frameId, int contextSeconds) {
    if (frameId <= 0 || contextSeconds < 0 || contextSeconds > 300)
        error("Index request requires a positive frame ID and context between 0 and 300 seconds");
    Database db(dbPath(directory), false, true);
    if (!hasIndexStates(db)) return 0;
    db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    try {
        ensureSchedule(db);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        pruneRequests(db, now);
        qint64 timestamp = 0;
        bool pending = false;
        {
            Statement selected(db, "SELECT timestamp_ms,ocr_state FROM frames WHERE id=?");
            selected.bind(1, frameId);
            if (!selected.next()) { db.exec("COMMIT"); return 0; }
            timestamp = selected.number(0); pending = selected.string(1) == "pending";
        }
        QVector<qint64> neighbors;
        {
            const qint64 context = qint64(contextSeconds) * 1000;
            Statement nearby(db, "SELECT id FROM frames WHERE ocr_state='pending' AND id<>? "
                "AND timestamp_ms BETWEEN ? AND ? ORDER BY ABS(timestamp_ms-?),timestamp_ms,id LIMIT 32");
            nearby.bind(1, frameId); nearby.bind(2, std::max<qint64>(0, timestamp - context));
            nearby.bind(3, timestamp > std::numeric_limits<qint64>::max() - context
                                  ? std::numeric_limits<qint64>::max() : timestamp + context);
            nearby.bind(4, timestamp);
            while (nearby.next()) neighbors.append(nearby.number(0));
        }
        db.exec("UPDATE index_schedule SET request_order=request_order+1 WHERE id=1");
        qint64 order = 0;
        {
            Statement sequence(db, "SELECT request_order FROM index_schedule WHERE id=1");
            if (sequence.next()) order = sequence.number(0);
        }
        const auto promote = [&](qint64 id, int rank) {
            Statement request(db, "INSERT INTO index_requests(frame_id,request_order,rank,expires_ms) VALUES(?,?,?,?) "
                "ON CONFLICT(frame_id) DO UPDATE SET request_order=excluded.request_order,rank=excluded.rank,expires_ms=excluded.expires_ms");
            request.bind(1, id); request.bind(2, order); request.bind(3, rank); request.bind(4, now + 120000); request.next();
        };
        if (pending) promote(frameId, 0);
        for (qsizetype i = 0; i < neighbors.size(); ++i) promote(neighbors[i], int(i) + 1);
        db.exec("DELETE FROM index_requests WHERE frame_id NOT IN(SELECT frame_id FROM index_requests "
                "ORDER BY request_order DESC,rank,frame_id LIMIT 256)");
        db.exec("COMMIT");
        return int(neighbors.size()) + int(pending);
    } catch (...) {
        sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

int requestCatchUp(const QString &directory, int seconds) {
    if (seconds < 1 || seconds > 300) error("Catch-up duration must be between 1 and 300 seconds");
    Database db(dbPath(directory), false, true);
    if (!hasIndexStates(db)) return 0;
    db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    try {
        ensureSchedule(db);
        Statement request(db, "UPDATE index_schedule SET catch_up_until_ms=? WHERE id=1");
        request.bind(1, QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000); request.next();
        db.exec("COMMIT");
        return seconds;
    } catch (...) {
        sqlite3_exec(db.handle, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

namespace {
QJsonObject readIndexingStatus(Database &db, const QString &directory) {
    QJsonObject status{{"pending", 0}, {"ready", 0}, {"disabled", 0}, {"failed", 0},
        {"pending_frames", 0}, {"pending_bytes", 0}, {"source_frames", 0}, {"source_bytes", 0},
        {"staged_bytes", 0}, {"oldest_pending_timestamp_ms", 0}, {"index_lag_ms", 0}, {"legacy_schema", !hasIndexStates(db)},
        {"archive_first", false},
        {"priority_pending", 0}, {"catch_up_until_ms", 0}, {"indexer_running", indexerRunning(directory)},
        {"coverage_total_frames", 0}, {"coverage_indexed_frames", 0}, {"coverage_frames_percent", 0},
        {"coverage_total_observations", 0}, {"coverage_indexed_observations", 0}, {"coverage_observations_percent", 0}};
    if (status["legacy_schema"].toBool()) {
        Statement count(db, "SELECT COUNT(*),COALESCE(SUM(observation_count),0) FROM frames"); count.next(); status["ready"] = count.number(0);
        status["coverage_total_frames"] = status["coverage_indexed_frames"] = count.number(0);
        status["coverage_total_observations"] = status["coverage_indexed_observations"] = count.number(1);
        status["coverage_frames_percent"] = count.number(0) ? 100 : 0;
        status["coverage_observations_percent"] = count.number(1) ? 100 : 0;
        return status;
    }
    status["archive_first"] = metadataNumber(db, "archive_first") != 0;
    {
        Statement coverage(db, "SELECT COUNT(*),COALESCE(SUM(ocr_state='ready'),0),COALESCE(SUM(observation_count),0),"
            "COALESCE(SUM(CASE WHEN ocr_state='ready' THEN observation_count ELSE 0 END),0) FROM frames");
        coverage.next();
        status["coverage_total_frames"] = coverage.number(0); status["coverage_indexed_frames"] = coverage.number(1);
        status["coverage_total_observations"] = coverage.number(2); status["coverage_indexed_observations"] = coverage.number(3);
        status["coverage_frames_percent"] = coverage.number(0) ? 100.0 * coverage.number(1) / coverage.number(0) : 0;
        status["coverage_observations_percent"] = coverage.number(2) ? 100.0 * coverage.number(3) / coverage.number(2) : 0;
    }
    if (hasSchedule(db)) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        Statement priority(db, "SELECT COUNT(*) FROM index_requests r JOIN frames f ON f.id=r.frame_id "
            "WHERE f.ocr_state='pending' AND r.expires_ms>?");
        priority.bind(1, now); priority.next(); status["priority_pending"] = priority.number(0);
        Statement boost(db, "SELECT catch_up_until_ms FROM index_schedule WHERE id=1");
        if (boost.next() && boost.number(0) > now) status["catch_up_until_ms"] = boost.number(0);
    }
    {
        Statement counts(db, "SELECT ocr_state,COUNT(*) FROM frames GROUP BY ocr_state");
        while (counts.next()) status[counts.string(0)] = counts.number(1);
        status["pending_frames"] = status["pending"];
    }
    {
        Statement pending(db, "SELECT COALESCE(SUM(source_bytes),0),COALESCE(MIN(timestamp_ms),0) FROM frames WHERE ocr_state='pending'");
        pending.next(); status["pending_bytes"] = pending.number(0); status["oldest_pending_timestamp_ms"] = pending.number(1);
        if (status["pending"].toInteger() > 0)
            status["index_lag_ms"] = std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - pending.number(1));
    }
    const auto usage = heldSourceUsage(db, directory);
    status["staged_bytes"] = double(usage.stagedBytes);
    status["source_frames"] = usage.frames;
    status["source_bytes"] = double(usage.bytes);
    return status;
}
} // namespace

struct IndexStatusReader::Impl {
    QString directory;
    Database database;
    explicit Impl(const QString &path) : directory(path), database(dbPath(path), false) {}
};

IndexStatusReader::IndexStatusReader(const QString &directory) : d(std::make_unique<Impl>(directory)) {}
IndexStatusReader::~IndexStatusReader() = default;
QJsonObject IndexStatusReader::status() { return readIndexingStatus(d->database, d->directory); }

QJsonObject indexingStatus(const QString &directory) {
    IndexStatusReader reader(directory);
    return reader.status();
}

QJsonArray listObservations(const QString &directory, int limit) {
    Database db(dbPath(directory), false);
    Statement query(db, "SELECT id,timestamp_ms,frame_id FROM observations ORDER BY timestamp_ms,id LIMIT ?");
    query.bind(1, std::clamp(limit, 1, 10000));
    QJsonArray result;
    while (query.next()) result.append(QJsonObject{{"id", query.number(0)}, {"timestamp_ms", query.number(1)}, {"frame_id", query.number(2)}});
    return result;
}

QImage loadFrame(const QString &directory, qint64 frameId, std::function<bool()> stopRequested) {
    if (stopRequested && stopRequested()) error("Frame loading canceled");
    Database db(dbPath(directory), false);
    const QByteArray sql = frameColumns(db) + "WHERE f.id=?";
    Statement query(db, sql.constData()); query.bind(1, frameId);
    const auto frames = readRows(query);
    if (frames.isEmpty()) error("Recorded frame does not exist");
    const FrameRecord &frame = frames.first();
    if (!frame.available) error("The recording ended before this video segment was finalized; media is unavailable");
    if (!frame.archiveAvailable && !QFileInfo(QDir(directory).filePath(frame.originalPath)).exists()) {
        const auto current = frameById(directory, frameId);
        if (current && current->archiveAvailable) return loadFrame(directory, frameId, stopRequested);
    }
    if (frame.width <= 0 || frame.height <= 0 || qint64(frame.width) * frame.height > 32 * 1024 * 1024 || frame.frameIndex < 0 || frame.frameIndex >= 300)
        error("Recorded frame exceeds decoder limits");
    if (frame.codec == "webp" || !frame.archiveAvailable) {
        try {
            if (stopRequested && stopRequested()) error("Frame loading canceled");
            return readOriginalImage(directory, frame.archiveAvailable ? frame.path : frame.originalPath, frame.width, frame.height);
        }
        catch (...) {
            if (!frame.archiveAvailable) {
                const auto current = frameById(directory, frameId);
                if (current && current->archiveAvailable) return loadFrame(directory, frameId, stopRequested);
            }
            throw;
        }
    }
    const QString path = safeMediaPath(directory, frame.path);
    if (stopRequested && stopRequested()) error("Frame loading canceled");
    QProcess decoder;
    decoder.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-nostdin", "-threads", "1", "-filter_threads", "1", "-i", path,
                            "-vf", QString("select=eq(n\\,%1)").arg(frame.frameIndex), "-frames:v", "1", "-f", "image2pipe", "-vcodec", "ppm", "pipe:1"});
    if (!decoder.waitForStarted(5000)) error("Cannot start frame decoder");
    QByteArray pixels, errors;
    QElapsedTimer deadline; deadline.start();
    const qint64 maxBytes = qint64(frame.width) * frame.height * 4 + MiB;
    while (decoder.state() != QProcess::NotRunning) {
        decoder.waitForReadyRead(100);
        pixels += decoder.readAllStandardOutput(); errors = (errors + decoder.readAllStandardError()).right(8192);
        if (stopRequested && stopRequested()) {
            decoder.kill(); decoder.waitForFinished(1000); error("Frame loading canceled");
        }
        if (pixels.size() > maxBytes || deadline.elapsed() > ProcessTimeoutMs) {
            decoder.kill(); decoder.waitForFinished(1000); error("Frame decoder exceeded memory/time bound");
        }
    }
    pixels += decoder.readAllStandardOutput(); errors = (errors + decoder.readAllStandardError()).right(8192);
    if (pixels.size() > maxBytes || decoder.exitCode() != 0) error("Cannot decode recorded frame: " + QString::fromUtf8(errors));
    QImage image = QImage::fromData(pixels, "PPM");
    if (image.isNull()) error("Requested video frame was not found");
    return image;
}
} // namespace replay
