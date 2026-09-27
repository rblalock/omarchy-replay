#include "recorder.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QTemporaryDir>
#include <QStringList>
#include <sqlite3.h>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <chrono>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

QString sql(const QString &directory, const QByteArray &query) {
    sqlite3 *db = nullptr;
    require(sqlite3_open(QDir(directory).filePath("index.sqlite").toUtf8().constData(), &db) == SQLITE_OK, "open fixture index");
    sqlite3_busy_timeout(db, 1000);
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db, query.constData(), -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(db); throw std::runtime_error("prepare fixture query");
    }
    const int status = sqlite3_step(statement);
    QString result;
    if (status == SQLITE_ROW && sqlite3_column_text(statement, 0))
        result = QString::fromUtf8(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)));
    sqlite3_finalize(statement); sqlite3_close(db);
    require(status == SQLITE_ROW || status == SQLITE_DONE, "execute fixture query");
    return result;
}

QImage frame(int version) {
    QImage image(960, 540, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    QPainter painter(&image);
    QFont font("DejaVu Sans"); font.setPixelSize(32); painter.setFont(font); painter.setPen(Qt::black);
    painter.drawText(40, 80, "SYNTHETIC CACHE FIXTURE");
    painter.drawText(40, 175, "Original evidence and exact highlights");
    painter.drawText(40, 280, QString("Invoice ALPHA %1").arg(version));
    painter.drawText(40, 405, "Every retained moment stays searchable");
    return image;
}

void record(const QString &directory, const QVector<QImage> &images) {
    replay::RecorderOptions options;
    options.directory = directory; options.deferredOcr = true; options.archiveFirst = true; options.minFreeBytes = 0;
    replay::Recorder recorder(options);
    for (int i = 0; i < images.size(); ++i)
        require(recorder.addFrame(images[i], 1000 + i * 2000).stored, "retain distinct synthetic moment");
    recorder.finish();
}

replay::IndexerOptions options(const QString &directory) {
    replay::IndexerOptions result; result.directory = directory; result.ocrMode = "full"; result.ocrReuse = true;
    return result;
}
void ready(replay::Indexer &indexer) {
    const auto result = indexer.processNext();
    if (result.state != "ready")
        throw std::runtime_error(QString("index synthetic frame: state=%1 error=%2").arg(result.state, result.error).toStdString());
}
qint64 count(const replay::Indexer &indexer, const char *key) { return indexer.statsJSON().value(key).toInteger(); }

void returnsAndRestart(const QString &root) {
    const QString directory = root + "/returns";
    const auto a = frame(1042), b = frame(2048);
    record(directory, {a, b, a, b, a});
    {
        replay::Indexer indexer(options(directory)); ready(indexer); ready(indexer); ready(indexer);
        require(count(indexer, "ocr_reuse_hits") == 1 && count(indexer, "ocr_full_frames") == 2, "A-B-A did not reuse one full result");
    }
    {
        replay::Indexer indexer(options(directory)); ready(indexer); ready(indexer);
        require(count(indexer, "ocr_reuse_hits") == 2 && count(indexer, "ocr_full_frames") == 0, "restart lost durable cache");
    }
    const auto rows = replay::listFrames(directory);
    require(rows.size() == 5 && rows[0].text == rows[2].text && rows[2].text == rows[4].text, "reuse changed text or discarded moments");
    require(replay::matchingTextRects(directory, rows[0].id, "ALPHA") == replay::matchingTextRects(directory, rows[4].id, "ALPHA") &&
            !replay::matchingTextRects(directory, rows[4].id, "ALPHA").isEmpty(), "reuse changed highlight coordinates");
    require(replay::loadFrame(directory, rows[4].id).convertToFormat(QImage::Format_RGBA8888) == a, "reuse changed archived pixels");

    const QString disabled = root + "/disabled";
    record(disabled, {a, b, a});
    auto opts = options(disabled); opts.ocrReuse = false;
    replay::Indexer indexer(opts); ready(indexer); ready(indexer); ready(indexer);
    require(count(indexer, "ocr_reuse_lookups") == 0 && count(indexer, "ocr_full_frames") == 3, "disable switch still reused");
    const auto baseline = replay::listFrames(disabled);
    for (int i = 0; i < baseline.size(); ++i)
        require(baseline[i].text == rows[i].text &&
                replay::matchingTextRects(disabled, baseline[i].id, "ALPHA") == replay::matchingTextRects(directory, rows[i].id, "ALPHA"),
                "reuse differs from fresh full OCR text or geometry");
}

void priorityAndProfile(const QString &root) {
    const QString directory = root + "/priority";
    record(directory, {frame(1), frame(2), frame(1)});
    require(replay::requestIndexing(directory, 3, 0) > 0, "request out-of-order moment");
    {
        replay::Indexer indexer(options(directory));
        require(indexer.processNext().frameId == 3, "priority order not exercised");
        ready(indexer);
        require(count(indexer, "ocr_reuse_hits") == 1, "out-of-order exact frame missed cache");
        ready(indexer);
    }
    const QString profile = root + "/profile";
    record(profile, {frame(1), frame(2), frame(1)});
    { replay::Indexer indexer(options(profile)); ready(indexer); ready(indexer); }
    auto opts = options(profile); opts.ocrMaxHeight = 400;
    replay::Indexer indexer(opts); ready(indexer);
    require(count(indexer, "ocr_reuse_hits") == 0 && count(indexer, "ocr_full_frames") == 1, "resized profile reused original-coordinate result");
}

void exactPixelsAndPartialProvenance(const QString &root) {
    const QString directory = root + "/pixels";
    auto changed = frame(1); changed.setPixelColor(950, 530, Qt::black);
    record(directory, {frame(1), frame(2), changed});
    { replay::Indexer indexer(options(directory)); ready(indexer); ready(indexer); ready(indexer);
      require(count(indexer, "ocr_reuse_hits") == 0, "one changed pixel reused an unrelated original"); }

    const QString partial = root + "/partial";
    const auto a = frame(1), b = frame(2);
    QImage blank(a.size(), a.format()); blank.fill(Qt::white);
    record(partial, {a, b, blank, b});
    auto opts = options(partial); opts.ocrMode = "incremental";
    replay::Indexer indexer(opts); ready(indexer); ready(indexer);
    require(count(indexer, "ocr_partial_frames") == 1, "fixture failed to exercise merged partial geometry");
    require(sql(partial, "SELECT COUNT(*) FROM ocr_reuse WHERE frame_id=2").toInt() == 0, "partial result became a full-provenance donor");
    ready(indexer); ready(indexer);
    require(count(indexer, "ocr_reuse_hits") == 0 && count(indexer, "ocr_full_frames") == 3, "partial donor contaminated a later full frame");
}

void invalidationAndBound(const QString &root) {
    for (int variant = 0; variant < 3; ++variant) {
        const QString directory = root + QString("/invalidate-%1").arg(variant);
        record(directory, {frame(1), frame(2), frame(1)});
        { replay::Indexer indexer(options(directory)); ready(indexer); ready(indexer); }
        if (variant == 0) sql(directory, "DELETE FROM frames WHERE id=1");
        if (variant == 1) sql(directory, "UPDATE frame_ocr_geometry SET lines_json='[]' WHERE frame_id=1");
        if (variant == 2) sql(directory, "UPDATE frames SET text='tampered' WHERE id=1");
        replay::Indexer indexer(options(directory)); ready(indexer);
        require(count(indexer, "ocr_reuse_hits") == 0, "deleted or changed donor remained reusable");
    }
    const QString directory = root + "/bound";
    record(directory, {frame(1), frame(2)});
    replay::Indexer indexer(options(directory)); ready(indexer);
    // Seed metadata pressure without hundreds of redundant OCR invocations.
    sql(directory, "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<260) "
                   "INSERT INTO ocr_reuse SELECT 'seed-'||x,c.profile_key,c.frame_id,c.result_key FROM n,ocr_reuse c WHERE c.frame_id=1");
    ready(indexer);
    require(sql(directory, "SELECT COUNT(*) FROM ocr_reuse").toInt() == 256, "reuse reference table is unbounded");
}

void publicationAndSourceGuard(const QString &root) {
    const QString directory = root + "/atomic";
    record(directory, {frame(1), frame(2), frame(1)});
    replay::Indexer indexer(options(directory));
    sql(directory, "CREATE TRIGGER fail_geometry BEFORE INSERT ON frame_ocr_geometry BEGIN SELECT RAISE(ABORT,'synthetic'); END");
    bool failed = false; try { indexer.processNext(); } catch (const std::exception &) { failed = true; }
    require(failed && sql(directory, "SELECT COUNT(*) FROM ocr_reuse").toInt() == 0 &&
            sql(directory, "SELECT ocr_state FROM frames WHERE id=1") == "pending", "failed publication leaked provenance");
    sql(directory, "DROP TRIGGER fail_geometry"); ready(indexer); ready(indexer);
    sql(directory, "CREATE TRIGGER fail_reuse BEFORE UPDATE OF ocr_state ON frames WHEN NEW.id=3 BEGIN SELECT RAISE(ABORT,'synthetic'); END");
    failed = false; try { indexer.processNext(); } catch (const std::exception &) { failed = true; }
    require(failed && sql(directory, "SELECT ocr_state FROM frames WHERE id=3") == "pending" && count(indexer, "ocr_reuse_hits") == 0,
            "failed reuse publication was counted or made ready");
    sql(directory, "DROP TRIGGER fail_reuse"); ready(indexer);
    require(count(indexer, "ocr_reuse_hits") == 1, "reuse could not recover after transaction rollback");

    const QString source = root + "/source";
    record(source, {frame(1), frame(2), frame(1)});
    { replay::Indexer first(options(source)); ready(first); ready(first); }
    const QString targetPath = QDir(source).filePath(sql(source, "SELECT source_path FROM frames WHERE id=3"));
    const QString replacementPath = QDir(source).filePath(sql(source, "SELECT path FROM frames WHERE id=2"));
    QFile replacement(replacementPath); require(replacement.open(QIODevice::ReadOnly), "read replacement synthetic pixels");
    const QByteArray replacementBytes = replacement.readAll();
    int checkpoints = 0;
    auto opts = options(source);
    opts.stopRequested = [&] {
        if (++checkpoints == 2) {
            QFile target(targetPath); require(target.open(QIODevice::WriteOnly | QIODevice::Truncate), "replace synthetic source");
            require(target.write(replacementBytes) == replacementBytes.size(), "write replacement synthetic source");
        }
        return false;
    };
    replay::Indexer guarded(opts);
    require(guarded.processNext().state == "obsolete" && count(guarded, "ocr_reuse_hits") == 0 &&
            sql(source, "SELECT ocr_state FROM frames WHERE id=3") == "pending", "source replacement reused stale decoded pixels");
    ready(guarded);
    require(count(guarded, "ocr_reuse_hits") == 1, "changed source was not re-decoded on retry");
}

bool traineddataInstalled(const QString &language) {
    // Mirror Tesseract's Init(nullptr) resolution: when TESSDATA_PREFIX is
    // set it is used exclusively (as the tessdata dir itself or its parent);
    // only without it does Tesseract fall back to the packaged datadirs.
    const QString file = language + ".traineddata";
    const QString prefix = qEnvironmentVariable("TESSDATA_PREFIX");
    QStringList dirs;
    if (!prefix.isEmpty()) dirs << prefix << prefix + "/tessdata";
    else dirs << QStringLiteral("/usr/share/tessdata")
              << QStringLiteral("/usr/share/tesseract-ocr/5/tessdata")
              << QStringLiteral("/usr/local/share/tessdata");
    for (const QString &dir : dirs)
        if (QFileInfo(QDir(dir).filePath(file)).isFile()) return true;
    return false;
}

bool configuredLanguages(const QString &root) {
    const QString directory = root + "/languages";
    QImage french(960, 540, QImage::Format_RGBA8888);
    french.fill(Qt::white);
    {
        QPainter painter(&french);
        QFont font("DejaVu Sans"); font.setPixelSize(36); painter.setFont(font); painter.setPen(Qt::black);
        painter.drawText(40, 120, QString::fromUtf8("Résumé envoyé à 14h05"));
        painter.drawText(40, 240, QString::fromUtf8("coordonnées vérifiées"));
    }
    record(directory, {french, frame(2), french, frame(2)});
    auto frenchOpts = options(directory); frenchOpts.ocrLanguages = "eng+fra";
    auto invalid = options(directory); invalid.ocrLanguages = "eng fre";
    bool rejected = false;
    try { replay::Indexer bad(invalid); } catch (const std::exception &) { rejected = true; }
    require(rejected, "invalid OCR language set was accepted");
    // The eng+fra scenario needs fra.traineddata, which the README does not
    // require. Skip (exit 77) instead of failing; the eng-only checks above
    // and elsewhere still run.
    if (!traineddataInstalled("fra")) {
        std::cout << "SKIP eng+fra reuse scenario: fra.traineddata is not installed; install tesseract-data-fra to exercise it\n";
        return false;
    }
    {
        replay::Indexer indexer(frenchOpts);
        ready(indexer); ready(indexer); ready(indexer);
        require(count(indexer, "ocr_reuse_hits") == 1 && count(indexer, "ocr_full_frames") == 2,
                "eng+fra reuse profile did not cache within its own language set");
        const auto rows = replay::listFrames(directory);
        require(rows[0].text.contains(QString::fromUtf8("sumé")),
                "accented word was not indexed with eng+fra; install tesseract-data-fra");
    }
    // A different configured language set must not reuse the cached result:
    // identical pixels were already OCR'd under eng+fra, so eng re-recognizes.
    replay::Indexer english(options(directory));
    ready(english);
    require(count(english, "ocr_reuse_hits") == 0 && count(english, "ocr_full_frames") == 1,
            "eng reused a cached eng+fra result");
    return true;
}

void busyPublicationStaysPending(const QString &root) {
    const QString directory = root + "/busy";
    record(directory, {frame(1)});
    sqlite3 *writer = nullptr;
    require(sqlite3_open(QDir(directory).filePath("index.sqlite").toUtf8().constData(), &writer) == SQLITE_OK, "open competing writer");
    int checkpoints = 0;
    std::thread release;
    auto opts = options(directory);
    opts.stopRequested = [&] {
        if (++checkpoints == 2) {
            require(sqlite3_exec(writer, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK, "hold competing write lease");
            // Outlast both the old one-second fatal timeout and the new short
            // deferral. A competing capture must not fail valid OCR evidence.
            release = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1300));
                sqlite3_exec(writer, "COMMIT", nullptr, nullptr, nullptr);
            });
        }
        return false;
    };
    replay::Indexer indexer(opts);
    bool failed = false;
    replay::IndexResult result;
    try { result = indexer.processNext(); } catch (const std::exception &) { failed = true; }
    if (release.joinable()) release.join();
    sqlite3_close(writer);
    require(!failed && !result.processed && result.state == "busy" &&
            sql(directory, "SELECT ocr_state FROM frames WHERE id=1") == "pending" &&
            count(indexer, "failed_jobs") == 0, "reuse BEGIN contention converted pending evidence into an OCR failure");
    ready(indexer);
}
} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    try {
        QTemporaryDir temporary; require(temporary.isValid(), "create synthetic fixture directory");
        returnsAndRestart(temporary.path()); priorityAndProfile(temporary.path());
        exactPixelsAndPartialProvenance(temporary.path()); invalidationAndBound(temporary.path());
        publicationAndSourceGuard(temporary.path());
        busyPublicationStaysPending(temporary.path());
        if (!configuredLanguages(temporary.path())) return 77; // eng+fra skipped: fra.traineddata missing
        std::cout << "PASS exact whole-frame OCR reuse, durable provenance, invalidation and atomic publication\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
