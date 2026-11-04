#include "recorder.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <sqlite3.h>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

qint64 sql(const QString &directory, const QByteArray &query) {
    sqlite3 *db = nullptr;
    require(sqlite3_open(QDir(directory).filePath("index.sqlite").toUtf8().constData(), &db) == SQLITE_OK,
            "Could not open synthetic index for scheduler regression");
    sqlite3_busy_timeout(db, 1000);
    sqlite3_stmt *statement = nullptr;
    const int prepared = sqlite3_prepare_v2(db, query.constData(), -1, &statement, nullptr);
    if (prepared != SQLITE_OK) { sqlite3_close(db); throw std::runtime_error("Could not prepare synthetic scheduler query"); }
    const int state = sqlite3_step(statement);
    const qint64 result = state == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
    sqlite3_finalize(statement); sqlite3_close(db);
    require(state == SQLITE_ROW || state == SQLITE_DONE, "Synthetic scheduler query failed");
    return result;
}

QImage sourceFrame(int invoice, bool blank = false) {
    QImage image(960, 540, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    if (!blank) {
        QPainter painter(&image);
        painter.setPen(Qt::black);
        QFont font("DejaVu Sans");
        font.setPixelSize(32);
        painter.setFont(font);
        painter.drawText(48, 90, "SYNTHETIC TEST CONTENT");
        painter.drawText(48, 185, "Patrick / workshop invoice");
        painter.drawText(48, 280, QString("Invoice XYZ-%1").arg(invoice));
        painter.drawText(48, 375, "Original text must remain searchable.");
    }
    return image;
}

replay::RecorderOptions deferredOptions(const QString &directory) {
    replay::RecorderOptions options;
    options.directory = directory;
    options.deferredOcr = true;
    options.minFreeBytes = 0;
    return options;
}

replay::IndexerOptions indexOptions(const QString &directory) {
    replay::IndexerOptions options;
    options.directory = directory;
    options.ocrMode = "full";
    return options;
}

int drain(const QString &directory) {
    replay::Indexer indexer(indexOptions(directory));
    int processed = 0;
    for (; processed < 20; ++processed) {
        const auto result = indexer.processNext();
        if (!result.processed && result.state == "idle") return processed;
        require(result.processed && !result.canceled && result.state == "ready",
                "Indexing did not complete a pending source");
    }
    throw std::runtime_error("Indexer did not become idle after bounded fixture work");
}

void capturedBeforeIndexed(const QString &root) {
    const QVector<QImage> sources{sourceFrame(1042), sourceFrame(1043), sourceFrame(0, true)};
    auto baselineOptions = deferredOptions(root + "/baseline");
    baselineOptions.deferredOcr = false;
    baselineOptions.ocrMode = "full";
    {
        replay::Recorder recorder(baselineOptions);
        for (int i = 0; i < sources.size(); ++i) recorder.addFrame(sources[i], 1000 + i * 2000);
        recorder.finish();
    }
    const auto baseline = replay::listFrames(baselineOptions.directory);
    auto options = deferredOptions(root + "/deferred");
    replay::Recorder recorder(options);
    for (int i = 0; i < sources.size(); ++i) recorder.addFrame(sources[i], 1000 + i * 2000);
    auto frames = replay::listFrames(options.directory);
    require(frames.size() == sources.size(), "Capture did not retain every accepted source before indexing");
    require(replay::searchFrames(options.directory, "Patrick").empty(), "Pending text was falsely advertised as indexed");
    require(replay::matchingTextRects(options.directory, frames[0].id, "Patrick").isEmpty(),
            "pending OCR acquired premature geometry");
    for (int i = 0; i < frames.size(); ++i) {
        require(frames[i].ocrState == "pending" && frames[i].text.isEmpty(), "New capture was not explicitly pending");
        require(replay::loadFrame(options.directory, frames[i].id).convertToFormat(QImage::Format_RGBA8888) == sources[i],
                "Original pixels could not be browsed before indexing");
    }
    // The recorder stays alive: the independent indexer opens the same dataset.
    require(drain(options.directory) == sources.size(), "Indexer skipped an accepted source");
    frames = replay::listFrames(options.directory);
    for (int i = 0; i < frames.size(); ++i) {
        require(frames[i].ocrState == "ready", "Completed source is not marked ready");
        require(frames[i].text == baseline[i].text, "Deferred OCR differs from full OCR on the original pixels");
    }
    require(frames.last().text.trimmed().isEmpty(), "Blank source acquired stale text");
    for (int i = 0; i < 2; ++i) {
        const auto found = replay::searchFrames(options.directory, QString("Patrick XYZ-%1").arg(1042 + i));
        require(found.size() == 1 && found[0].id == frames[i].id, "Indexed invoice resolves to the wrong original moment");
        const auto highlights = replay::matchingTextRects(options.directory, frames[i].id, "Patrick");
        require(!highlights.isEmpty() && highlights[0].contains(QPoint(100, 175)),
                "deferred publication lost original-coordinate OCR geometry");
    }
    recorder.finish();
    std::cout << "PASS capture before index, independent drain, original-pixel OCR and honest blank state\n";
}

void boundedOverflow(const QString &root) {
    auto options = deferredOptions(root + "/overflow");
    options.maxPendingFrames = 2;
    replay::Recorder recorder(options);
    const QImage first = sourceFrame(1042), second = sourceFrame(1043), third = sourceFrame(1044);
    const auto a = recorder.addFrame(first, 1000);
    const auto b = recorder.addFrame(second, 3000);
    const auto rejected = recorder.addFrame(third, 5000);
    require(a.stored && b.stored && rejected.backlogFull && !rejected.stored && !rejected.duplicate,
            "Full pending queue did not explicitly reject the new observation");
    const auto accepted = replay::listFrames(options.directory);
    require(accepted.size() == 2 && replay::listObservations(options.directory).size() == 2,
            "Overflow silently changed accepted history or added a nonexistent observation");
    require(replay::loadFrame(options.directory, a.frameId).convertToFormat(QImage::Format_RGBA8888) == first &&
            replay::loadFrame(options.directory, b.frameId).convertToFormat(QImage::Format_RGBA8888) == second,
            "Overflow evicted accepted source pixels");
    {
        replay::Indexer indexer(indexOptions(options.directory));
        require(indexer.processNext().state == "ready", "Queue could not free capacity by indexing");
    }
    require(recorder.addFrame(third, 5000).stored, "Capture did not resume when indexing freed queue capacity");
    recorder.finish();
    require(drain(options.directory) == 2, "Remaining accepted frames were lost after overflow");
    require(replay::searchFrames(options.directory, "Patrick").size() == 3, "Overflow recovery lost searchable history");
    std::cout << "PASS bounded overflow preserves accepted history and resumes after drain\n";
}

void pendingByteLimit(const QString &root) {
    auto options = deferredOptions(root + "/byte-limit");
    options.maxPendingBytes = 1024 * 1024;
    replay::Recorder recorder(options);
    QImage pixels(512, 512, QImage::Format_RGBA8888);
    quint32 random = 173;
    auto fill = [&] {
        for (int y = 0; y < pixels.height(); ++y)
            for (int x = 0; x < pixels.width(); ++x) {
                random ^= random << 13; random ^= random >> 17; random ^= random << 5;
                pixels.setPixelColor(x, y, QColor(random & 255, (random >> 8) & 255, (random >> 16) & 255));
            }
    };
    fill();
    const QImage acceptedPixels = pixels;
    const auto accepted = recorder.addFrame(pixels, 1000);
    fill();
    const auto rejected = recorder.addFrame(pixels, 3000);
    require(accepted.stored && rejected.backlogFull && !rejected.stored,
            "Pending byte budget did not bound lossless source storage");
    require(replay::listFrames(options.directory).size() == 1 &&
            replay::loadFrame(options.directory, accepted.frameId).convertToFormat(QImage::Format_RGBA8888) == acceptedPixels,
            "Byte overflow changed accepted evidence");
    recorder.finish();
    std::cout << "PASS pending byte ceiling rejects new work without evicting evidence\n";
}

void videoPendingPixels(const QString &root) {
    auto options = deferredOptions(root + "/video");
    options.codec = "h264";
    const auto original = sourceFrame(1042);
    replay::Recorder recorder(options);
    const auto added = recorder.addFrame(original, 1000);
    require(replay::loadFrame(options.directory, added.frameId).convertToFormat(QImage::Format_RGBA8888) == original,
            "Unsealed video hid its retained original image");
    require(drain(options.directory) == 1, "Video source was not indexed independently");
    require(replay::loadFrame(options.directory, added.frameId).convertToFormat(QImage::Format_RGBA8888) == original,
            "Indexing deleted the only browsable pixels before video sealing");
    recorder.finish();
    require(replay::loadFrame(options.directory, added.frameId).size() == original.size(), "Finalized video could not be retrieved");
    require(replay::searchFrames(options.directory, "Patrick XYZ-1042").size() == 1, "Video lost original-text search after sealing");
    std::cout << "PASS lossless pending evidence survives indexing until video sealing\n";
}

void indexedVideoReleasesQueuePressure(const QString &root) {
    auto options = deferredOptions(root + "/video-queue-pressure");
    options.codec = "h264";
    options.maxPendingFrames = 1;
    // Leave the normal segment length unchanged: neither this first frame nor
    // the second one reaches its ordinary frame-count sealing threshold.
    replay::Recorder recorder(options);
    const QImage first = sourceFrame(1042), second = sourceFrame(1043);
    const auto initial = recorder.addFrame(first, 1000);
    require(initial.stored && drain(options.directory) == 1, "First video source was not indexed");
    const auto before = replay::listFrames(options.directory);
    require(before.size() == 1 && before[0].ocrState == "ready" && !before[0].archiveAvailable &&
            !before[0].originalPath.isEmpty(), "Test did not retain an indexed original behind an unsealed video");

    const auto admitted = recorder.addFrame(second, 3000);
    require(admitted.stored && !admitted.backlogFull, "Indexed original permanently blocked the one-frame queue");
    const auto after = replay::listFrames(options.directory);
    require(after.size() == 2 && after[0].archiveAvailable,
            "Queue pressure did not seal the old video before accepting another original");
    require(replay::indexingStatus(options.directory)["source_frames"].toInteger() <= 1,
            "Queue pressure admitted another frame by exceeding its original-image cap");
    recorder.finish();
    require(drain(options.directory) == 1, "Second video source did not finish indexing");
    const auto completed = replay::listFrames(options.directory);
    require(completed.size() == 2, "Queue-pressure recovery changed accepted history");
    for (int i = 0; i < completed.size(); ++i) {
        require(completed[i].ocrState == "ready" && completed[i].archiveAvailable,
                "Queue-pressure recovery left incomplete indexed evidence");
        require(replay::loadFrame(options.directory, completed[i].id).size() == first.size(),
                "A pressure-sealed video frame could not be retrieved");
        const auto found = replay::searchFrames(options.directory, QString("Patrick XYZ-%1").arg(1042 + i));
        require(found.size() == 1 && found[0].id == completed[i].id,
                "Queue-pressure recovery lost or misattributed original invoice text");
    }
    std::cout << "PASS indexed video pressure seals and frees a one-frame queue without losing evidence\n";
}

void failedSourceIsNotEmptyReady(const QString &root) {
    auto options = deferredOptions(root + "/corrupt");
    { replay::Recorder recorder(options); recorder.addFrame(sourceFrame(1042), 1000); recorder.finish(); }
    const auto pending = replay::listFrames(options.directory);
    require(pending.size() == 1 && !pending[0].originalPath.isEmpty(), "No lossless source exists for pending OCR");
    QFile file(QDir(options.directory).filePath(pending[0].originalPath));
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "Could not corrupt synthetic source fixture");
    require(file.write("invalid synthetic image") > 0, "Could not write corrupt source fixture");
    file.close();
    replay::Indexer indexer(indexOptions(options.directory));
    const auto result = indexer.processNext();
    const auto frames = replay::listFrames(options.directory);
    require(result.state == "failed" && frames[0].ocrState == "failed" && !frames[0].ocrError.isEmpty(),
            "Unreadable original became ready-empty instead of an explicit indexing failure");
    require(replay::searchFrames(options.directory, "Patrick").empty(), "Failed source introduced a search hit");
    std::cout << "PASS unreadable original is explicitly failed, never ready-empty\n";
}

void corruptModelKeepsJobsPending(const QString &root) {
    const QVector<QImage> originals{sourceFrame(1042), sourceFrame(1043)};
    auto options = deferredOptions(root + "/corrupt-model-jobs");
    {
        replay::Recorder recorder(options);
        for (int i = 0; i < originals.size(); ++i)
            require(recorder.addFrame(originals[i], 1000 + i * 2000).stored,
                    "Could not retain pending corrupt-model test source");
        recorder.finish();
    }
    const auto before = replay::listFrames(options.directory);
    require(before.size() == 2 && before[0].ocrState == "pending" && before[1].ocrState == "pending",
            "Corrupt-model regression requires two pending jobs");

    const QString modelDirectory = root + "/corrupt-model-data";
    require(QDir().mkpath(modelDirectory), "Could not create isolated model directory");
    QFile model(QDir(modelDirectory).filePath("eng.traineddata"));
    require(model.open(QIODevice::WriteOnly), "Could not create corrupt synthetic model");
    require(model.write("This readable file is not a Tesseract model.\n") > 0,
            "Could not write corrupt synthetic model");
    model.close();
    auto workerOptions = indexOptions(options.directory);
    workerOptions.ocrDataPath = modelDirectory;
    replay::Indexer indexer(workerOptions);
    // An unloadable model is a configuration problem, not evidence about any
    // frame: every call stops with OcrEngineUnavailable and nothing is marked
    // failed. Repeated calls use the same worker and must retry initialization
    // rather than dereference an engine left unusable by the first failed Init.
    for (int i = 0; i < 3; ++i) {
        bool unavailable = false;
        try { indexer.processNext(); }
        catch (const replay::OcrEngineUnavailable &error) {
            unavailable = QString::fromUtf8(error.what()).contains(modelDirectory);
        }
        require(unavailable, "Corrupt model did not stop indexing with an error naming its data path");
    }
    require(indexer.statsJSON().value("failed_jobs").toInteger() == 0, "Corrupt model counted frames as failed jobs");
    const auto after = replay::listFrames(options.directory);
    require(after.size() == originals.size(), "Failed initialization lost accepted history");
    for (int i = 0; i < after.size(); ++i) {
        require(after[i].id == before[i].id && after[i].ocrState == "pending" &&
                after[i].text.isEmpty() && after[i].ocrError.isEmpty() && !after[i].originalPath.isEmpty(),
                "Failed initialization changed job state, published partial text or discarded its original reference");
        require(replay::loadFrame(options.directory, after[i].id).convertToFormat(QImage::Format_RGBA8888) == originals[i],
                "Failed initialization changed or removed retained original pixels");
    }
    const auto status = replay::indexingStatus(options.directory);
    require(status["failed"].toInteger() == 0 && status["pending"].toInteger() == 2 &&
            status["ready"].toInteger() == 0 && replay::searchFrames(options.directory, "Patrick").empty(),
            "Failed initialization was advertised as searchable or failed retained work");
    std::cout << "PASS corrupt model stops indexing, keeps both jobs pending and preserves both originals\n";
}

void signalAndRestart(const QString &root, const QString &binary) {
    auto options = deferredOptions(root + "/restart");
    { replay::Recorder recorder(options); recorder.addFrame(sourceFrame(1042), 1000); recorder.addFrame(sourceFrame(1043), 3000); recorder.finish(); }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(binary, {"index", "--dir", options.directory, "--ocr-mode", "full", "--ocr-cpu-percent", "1"});
    require(process.waitForStarted(5000), "Could not start independent indexer process");
    // A one-percent target keeps this known text pass unfinished long enough
    // to exercise the real CLI signal path without changing user's processes.
    QThread::msleep(250);
    require(process.state() == QProcess::Running, "Paced indexer exited before the interruption check");
    require(::kill(static_cast<pid_t>(process.processId()), SIGINT) == 0, "Could not signal the synthetic indexer");
    require(process.waitForFinished(10000), "Indexer ignored SIGINT or exceeded bounded shutdown");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            "Indexer did not exit gracefully on SIGINT");
    const auto interrupted = replay::listFrames(options.directory);
    int pending = 0;
    for (const auto &frame : interrupted) {
        require(frame.ocrState == "pending" || frame.ocrState == "ready", "Interrupted source acquired a false failed/empty state");
        if (frame.ocrState == "pending") { ++pending; require(frame.text.isEmpty(), "Canceled text was partially published"); }
    }
    require(pending > 0, "Interruption did not leave resumable pending work");
    process.start(binary, {"index", "--dir", options.directory, "--ocr-mode", "full"});
    require(process.waitForStarted(5000) && process.waitForFinished(20000), "Restarted indexer did not finish pending work");
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0, "Restarted indexer failed");
    const auto resumed = replay::listFrames(options.directory);
    require(resumed.size() == 2 && resumed[0].ocrState == "ready" && resumed[1].ocrState == "ready",
            "Restart did not finish every accepted frame exactly once");
    require(replay::searchFrames(options.directory, "Patrick XYZ-1042").size() == 1 &&
            replay::searchFrames(options.directory, "Patrick XYZ-1043").size() == 1,
            "Restart lost original invoice text");
    std::cout << "PASS SIGINT preserves pending work and a fresh process resumes indexing\n";
}

void priorityFairnessAndReordering(const QString &root) {
    auto options = deferredOptions(root + "/priority-fairness");
    QVector<qint64> ids;
    {
        replay::Recorder recorder(options);
        for (int i = 0; i < 8; ++i)
            ids.append(recorder.addFrame(sourceFrame(1100 + i, i == 0), 1000 + i * 60000).frameId);
        recorder.finish();
    }
    require(!replay::indexingStatus(options.directory)["indexer_running"].toBool(), "Lock absence was reported as a worker");
    for (int i = 3; i <= 6; ++i)
        require(replay::requestIndexing(options.directory, ids[i], 0) == 1, "Selected-only request promoted wrong job count");
    require(replay::requestIndexing(options.directory, ids[6], 0) == 1 &&
            replay::indexingStatus(options.directory)["priority_pending"].toInteger() == 4,
            "Repeated priority request duplicated work");
    auto workerOptions = indexOptions(options.directory);
    workerOptions.ocrMode = "incremental";
    bool sawRequestedBudget = false;
    workerOptions.cpuPercentProvider = [&](bool requested) { sawRequestedBudget |= requested; return 100.0; };
    {
        replay::Indexer worker(workerOptions);
        require(replay::indexingStatus(options.directory)["indexer_running"].toBool(), "Live flock owner was not detected");
        const auto result = worker.processNext();
        require(result.state == "ready" && result.frameId == ids[6], "Newest selected job did not run before older backlog");
    }
    require(sawRequestedBudget, "Priority job did not request its interactive budget");
    require(!replay::indexingStatus(options.directory)["indexer_running"].toBool(), "Stale lock file falsely indicated a running worker");
    {
        replay::Indexer worker(workerOptions);
        for (int index : {5, 4, 0}) {
            const auto result = worker.processNext();
            require(result.state == "ready" && result.frameId == ids[index], "Priority fairness did not survive worker restart");
        }
        const auto stats = worker.statsJSON();
        require(stats["ocr_discontinuity_resets"].toInteger() == 2 &&
                stats["ocr_full_reason_initial"].toInteger() == 3,
                "Reordered jobs reused an incremental cache across a discontinuity");
        require(replay::frameById(options.directory, ids[0])->text.trimmed().isEmpty(),
                "Returning to an older blank image leaked text from a prioritized future image");
    }
    const auto status = replay::indexingStatus(options.directory);
    require(status["coverage_total_frames"].toInteger() == 8 && status["coverage_indexed_frames"].toInteger() == 4 &&
            status["coverage_frames_percent"].toDouble() == 50 && status["priority_pending"].toInteger() == 1,
            "Out-of-order coverage or remaining priority count is misleading");
    for (int index : {4, 5, 6}) {
        const auto found = replay::searchFrames(options.directory, QString("Patrick XYZ-%1").arg(1100 + index));
        require(found.size() == 1 && found[0].id == ids[index], "Reordered indexing attached invoice text to the wrong frame");
    }
    std::cout << "PASS selected priority, persisted three-to-one fairness, liveness, coverage and cache reset after reordering\n";
}

void requestExpiryAndBounds(const QString &root) {
    auto options = deferredOptions(root + "/priority-expiry");
    options.maxPendingFrames = 0;
    QVector<qint64> ids;
    {
        replay::Recorder recorder(options);
        for (int i = 0; i < 40; ++i) {
            QImage image(64, 64, QImage::Format_RGBA8888); image.fill(QColor(i, 23, 70));
            ids.append(recorder.addFrame(image, 1000 + i * 1000).frameId);
        }
        recorder.finish();
    }
    const qint64 before = QDateTime::currentMSecsSinceEpoch();
    require(replay::requestIndexing(options.directory, ids[20], 300) == 33,
            "Context request did not limit itself to selected plus 32 neighbors");
    const auto expiry = sql(options.directory, "SELECT MIN(expires_ms) FROM index_requests");
    require(expiry >= before + 120000 && expiry <= QDateTime::currentMSecsSinceEpoch() + 120000,
            "Priority request did not have a finite 120-second TTL");
    sql(options.directory, "UPDATE index_requests SET expires_ms=0");
    require(replay::indexingStatus(options.directory)["priority_pending"].toInteger() == 0,
            "Expired priority requests remained advertised as live");
    bool boosted = false;
    require(replay::requestCatchUp(options.directory, 120) == 120 &&
            replay::indexingStatus(options.directory)["catch_up_until_ms"].toInteger() > before,
            "Finite catch-up request was not durable and visible");
    auto workerOptions = indexOptions(options.directory);
    workerOptions.cpuPercentProvider = [&](bool requested) { boosted |= requested; return 100.0; };
    {
        replay::Indexer worker(workerOptions);
        const auto next = worker.processNext();
        require(next.frameId == ids[0] && next.state == "ready" && boosted,
                "Expired priorities survived restart or live catch-up did not boost the oldest job");
    }
    require(sql(options.directory, "SELECT COUNT(*) FROM index_requests") == 0,
            "Expired priority requests were not consumed by scheduling");
    sql(options.directory, "UPDATE index_schedule SET catch_up_until_ms=0");
    require(replay::indexingStatus(options.directory)["catch_up_until_ms"].toInteger() == 0,
            "Expired catch-up remained visible as active");
    bool rejected = false;
    try { replay::requestCatchUp(options.directory, 301); } catch (const std::exception &) { rejected = true; }
    require(rejected, "Unbounded catch-up duration was accepted");
    std::cout << "PASS bounded context, priority TTL, restart expiry and finite catch-up budget request\n";
}

void byteOnlyAdmission(const QString &root) {
    auto options = deferredOptions(root + "/byte-only");
    options.maxPendingFrames = 0; options.maxPendingBytes = 1024;
    replay::Recorder recorder(options);
    int retained = 0;
    bool pressure = false;
    for (int i = 0; i < 100; ++i) {
        QImage image(64, 64, QImage::Format_RGBA8888); image.fill(QColor(i, 31, 91));
        const auto result = recorder.addFrame(image, 1000 + i);
        if (result.backlogFull) { pressure = true; break; }
        require(result.stored, "Distinct tiny source was not retained"); ++retained;
    }
    require(retained > 8 && pressure, "Byte-only queue either kept the old count limit or lost its byte cap");
    const auto stats = recorder.statsJSON();
    require(stats["backlog_byte_limit"].toInteger() == 1 && stats["backlog_frame_limit"].toInteger() == 0 &&
            stats["indexing"].toObject()["source_bytes"].toDouble() <= 1024,
            "Admission diagnostics did not distinguish source bytes from job count pressure");
    recorder.finish();
    std::cout << "PASS byte-only admission holds more than eight tiny sources and still stops at its byte ceiling\n";
}

void publicationDoesNotResurrect(const QString &root) {
    for (bool remove : {false, true}) {
        auto options = deferredOptions(root + (remove ? "/deleted-during-ocr" : "/disabled-during-ocr"));
        qint64 id = 0;
        { replay::Recorder recorder(options); id = recorder.addFrame(sourceFrame(1042), 1000).frameId; recorder.finish(); }
        replay::requestIndexing(options.directory, id, 0);
        bool changed = false;
        auto workerOptions = indexOptions(options.directory);
        workerOptions.cpuPercentProvider = [&](bool) {
            if (!changed) {
                changed = true;
                sql(options.directory, remove ? "DELETE FROM frames" : "UPDATE frames SET ocr_state='disabled'");
            }
            return 100.0;
        };
        replay::Indexer worker(workerOptions);
        const auto result = worker.processNext();
        require(result.processed && result.state == "obsolete" && changed,
                "Publication did not notice a frame removed from the pending set during OCR");
        require(sql(options.directory, "SELECT COUNT(*) FROM frame_text") == 0 &&
                sql(options.directory, "SELECT COUNT(*) FROM frame_ocr_geometry") == 0 &&
                sql(options.directory, "SELECT COUNT(*) FROM frames WHERE ocr_state='ready'") == 0 &&
                sql(options.directory, "SELECT COUNT(*) FROM index_requests") == 0,
                "Obsolete work resurrected searchable text or left a live priority request");
    }
    std::cout << "PASS OCR publication cannot resurrect a deleted or no-longer-pending frame\n";
}

void publicationRejectsReusedFrameId(const QString &root) {
    auto options = deferredOptions(root + "/reused-frame-id");
    qint64 originalId = 0;
    {
        replay::Recorder recorder(options);
        originalId = recorder.addFrame(sourceFrame(1042), 1000).frameId;
        recorder.finish();
    }
    auto workerOptions = indexOptions(options.directory);
    int checks = 0;
    bool replaced = false;
    workerOptions.stopRequested = [&] {
        // The first poll precedes job selection. The next is inside recognition,
        // after this job's original pixels and immutable identity were read.
        if (++checks == 2) {
            sql(options.directory, "DELETE FROM frames");
            sql(options.directory, "INSERT INTO frames(timestamp_ms,last_timestamp_ms,frame_index,path,codec,width,height,"
                "text,ocr_state,source_path,source_bytes) VALUES(9000,9000,0,'media/frame-00000001.webp','webp',960,540,"
                "'','pending','media/frame-00000001.webp',0)");
            replaced = true;
        }
        return false;
    };
    replay::Indexer worker(workerOptions);
    const auto result = worker.processNext();
    const auto replacement = replay::frameById(options.directory, originalId);
    require(replaced && replacement && result.frameId == originalId && result.state == "obsolete",
            "In-flight OCR did not reject a replacement row that reused the highest frame ID");
    require(replacement->timestampMs == 9000 && replacement->ocrState == "pending" && replacement->text.isEmpty() &&
            replacement->ocrError.isEmpty() && sql(options.directory, "SELECT COUNT(*) FROM frame_text") == 0 &&
            sql(options.directory, "SELECT COUNT(*) FROM frame_ocr_geometry") == 0,
            "Old OCR text or failure state was published onto the replacement frame identity");
    std::cout << "PASS source identity prevents stale OCR publication when SQLite reuses a deleted frame ID\n";
}

} // namespace

int main(int argc, char **argv) {
    qputenv("OMP_THREAD_LIMIT", "1");
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    try {
        require(temporary.isValid(), "Cannot create temporary fixture directory");
        require(argc == 2, "Pass the replay executable path to test indexer SIGINT/restart");
        capturedBeforeIndexed(temporary.path());
        boundedOverflow(temporary.path());
        pendingByteLimit(temporary.path());
        videoPendingPixels(temporary.path());
        indexedVideoReleasesQueuePressure(temporary.path());
        failedSourceIsNotEmptyReady(temporary.path());
        corruptModelKeepsJobsPending(temporary.path());
        signalAndRestart(temporary.path(), QString::fromLocal8Bit(argv[1]));
        priorityFairnessAndReordering(temporary.path());
        requestExpiryAndBounds(temporary.path());
        byteOnlyAdmission(temporary.path());
        publicationDoesNotResurrect(temporary.path());
        publicationRejectsReusedFrameId(temporary.path());
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
