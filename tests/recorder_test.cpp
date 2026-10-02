#include "recorder.h"

#include <QApplication>
#include <QDirIterator>
#include <QFileInfo>
#include <QPainter>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <omp.h>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

template <class Callback>
void requireError(Callback callback, const char *message) {
    bool rejected = false;
    try { callback(); } catch (const std::exception &) { rejected = true; }
    require(rejected, message);
}

QImage textFrame() {
    QImage image(640, 360, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setPen(Qt::black);
    painter.setFont(QFont("DejaVu Sans", 24));
    painter.drawText(30, 100, "Patrick XYZ invoice 1024");
    return image;
}

void checkCodec(const QString &root, const QString &codec) {
    const QImage firstImage = textFrame();
    QImage secondImage = firstImage;
    { QPainter painter(&secondImage); painter.setPen(Qt::black); painter.drawText(30, 200, "Another detail"); }
    replay::RecorderOptions options;
    options.directory = root + "/" + codec;
    options.codec = codec;
    options.segmentFrames = 2;
    replay::Recorder recorder(options);
    const auto first = recorder.addFrame(firstImage, 1000);
    const auto duplicate = recorder.addFrame(firstImage, 2000);
    recorder.addFrame(secondImage, 3000);
    recorder.addFrame(firstImage, 4000);
    recorder.finish();
    recorder.finish(); // Finishing twice is harmless; adding later is rejected.
    requireError([&] { recorder.addFrame(firstImage, 5000); }, "stopped recorder accepted another frame");

    require(first.stored && duplicate.duplicate && first.frameId == duplicate.frameId, "exact duplicate was not suppressed");
    const auto frames = replay::listFrames(options.directory);
    require(frames.size() == 3, "wrong retained frame count");
    require(frames[0].observationCount == 2 && frames[0].timestampMs == 1000 && frames[0].lastTimestampMs == 2000,
            "duplicate observation times were lost");
    const auto observations = replay::listObservations(options.directory);
    require(observations.size() == 4 && observations[1].toObject()["timestamp_ms"].toInteger() == 2000,
            "timeline observation was lost");
    require(replay::searchFrames(options.directory, "Patrick: XYZ invoice").size() == 3,
            "original-pixel OCR or punctuation-safe search failed");
    const auto highlights = replay::matchingTextRects(options.directory, frames[0].id, "Patrick invoice");
    require(!highlights.isEmpty() && highlights[0].contains(QPoint(100, 90)),
            "full OCR did not persist matching line geometry at original coordinates");
    require(replay::matchingTextRects(options.directory, frames[0].id, "Patrick absentword").isEmpty(),
            "nonmatching query acquired misleading highlights");
    require(replay::searchFrames(options.directory, "() * \"").empty(), "punctuation-only search should be empty");
    require(replay::searchFrames(options.directory, "Patrick OR nonexistentword").empty(), "FTS operators were executed instead of treated literally");
    const auto previous = replay::adjacentFrame(options.directory, frames[1].id, -1);
    const auto next = replay::adjacentFrame(options.directory, frames[1].id, 1);
    require(previous && previous->id == frames[0].id && next && next->id == frames[2].id,
            "adjacent timeline navigation failed");
    require(!replay::adjacentFrame(options.directory, frames[0].id, -1), "timeline moved before first frame");
    require(!replay::adjacentFrame(options.directory, frames[2].id, 1), "timeline moved after last frame");
    for (const auto &frame : frames) {
        require(frame.available, "finalized media is marked unavailable");
        const QImage decoded = replay::loadFrame(options.directory, frame.id);
        require(decoded.size() == firstImage.size(), "decoded frame dimensions differ");
        if (codec == "webp") {
            const QImage expected = frame.id == frames[1].id ? secondImage : firstImage;
            require(decoded.convertToFormat(QImage::Format_RGBA8888) == expected, "lossless media changed pixels");
        }
    }
    requireError([&] { replay::Recorder overwrite(options); }, "existing dataset was overwritten");
    requireError([&] { replay::loadFrame(options.directory, 99999); }, "missing frame was accepted");
    const auto permissions = QFileInfo(options.directory).permissions();
    require(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                             QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther)),
            "dataset directory is not private");
    const auto stats = recorder.statsJSON();
    require(stats["observations"].toInteger() == 4 && stats["duplicate_frames"].toInteger() == 1 &&
            stats["finished"].toBool() && !stats["failed"].toBool(), "recorder completion statistics are wrong");
    std::cout << "PASS " << codec.toStdString() << ": OCR, literal search, observations, retrieval and private storage\n";
}

void checkIncomplete(const QString &root) {
    replay::RecorderOptions options;
    options.directory = root + "/interrupted";
    options.codec = "h264";
    options.ocr = false;
    { replay::Recorder recorder(options); recorder.addFrame(textFrame(), 1000); }
    const auto frames = replay::listFrames(options.directory);
    require(frames.size() == 1 && !frames[0].available, "interrupted video advertised complete media");
    requireError([&] { replay::loadFrame(options.directory, frames[0].id); }, "interrupted video was decoded as complete");

    options.directory = root + "/bad-device";
    options.codec = "h264-vaapi";
    options.device = root + "/nonexistent-device";
    replay::Recorder failedEncoder(options);
    requireError([&] { failedEncoder.addFrame(textFrame(), 1000); failedEncoder.finish(); }, "invalid encoder device did not stop recording");
    require(failedEncoder.statsJSON()["failed"].toBool(), "encoder failure was not reported");
    std::cout << "PASS interruption and encoder failure remain explicit\n";
}

void randomize(QImage &image, quint32 &state) {
    for (int row = 0; row < image.height(); ++row) {
        auto *pixels = image.scanLine(row);
        for (int column = 0; column < image.width() * 4; ++column) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            pixels[column] = column % 4 == 3 ? 255 : state & 255;
        }
    }
}

void checkDiskBudget(const QString &root) {
    QImage noise(1024, 1024, QImage::Format_RGBA8888);
    quint32 randomState = 31987231;
    for (const QString &codec : {QString("webp"), QString("h264")}) {
        replay::RecorderOptions options;
        options.directory = root + "/limit-" + codec;
        options.codec = codec;
        options.ocr = false;
        options.maxDiskBytes = 16 * 1024 * 1024;
        options.minFreeBytes = 0;
        replay::Recorder recorder(options);
        QString failure;
        try {
            for (int i = 0; i < 30; ++i) {
                randomize(noise, randomState);
                recorder.addFrame(noise, i * 1000);
            }
        } catch (const std::exception &e) { failure = e.what(); }
        require(!failure.isEmpty(), "high-entropy recording did not stop at its disk budget");
        // A budget stop can win the race with the encoder's own exit, so judge
        // both surfaces: the stop error (the write path saw the dead encoder)
        // and the exit detail the failure cleanup reaped from a child that had
        // already exited (last_encoder_exit). The byte limit must stop the
        // encoder cleanly (EFBIG with SIGXFSZ ignored), never a signal 25 kill
        // that leaves a core dump behind; a child still running at cleanup is
        // killed there, which is not the disk-limit kill this check is about.
        require(codec == "webp" || (!failure.contains("crashed by signal") &&
                !recorder.statsJSON()["last_encoder_exit"].toString().contains("crashed by signal")),
                "video encoder was killed by a signal at the disk limit instead of exiting cleanly");
        quint64 actualBytes = 0;
        QDirIterator files(options.directory, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (files.hasNext()) { files.next(); actualBytes += files.fileInfo().size(); }
        require(actualBytes <= options.maxDiskBytes, "dataset exceeded disk budget");
        require(recorder.statsJSON()["failed"].toBool(), "disk limit was not reported as a stop");
    }
    replay::RecorderOptions options;
    options.directory = root + "/no-free-space";
    options.minFreeBytes = std::numeric_limits<quint64>::max();
    options.ocr = false;
    requireError([&] { replay::Recorder recorder(options); }, "minimum-free-space limit overflowed");
    std::cout << "PASS image/video disk ceilings and free-space reserve\n";
}

void checkOcrStop(const QString &root) {
    const int priorParallelLevels = omp_get_max_active_levels();
    replay::RecorderOptions options;
    options.directory = root + "/ocr-cancel";
    options.codec = "h264";
    bool cancelEnabled = false;
    bool ocrIsSerial = true;
    int cancellationPolls = 0;
    options.stopRequested = [&] {
        ocrIsSerial &= omp_get_max_active_levels() == 0;
        return cancelEnabled && ++cancellationPolls >= 2;
    };
    replay::Recorder recorder(options);
    require(omp_get_max_active_levels() == priorParallelLevels, "OCR initialization changed caller OpenMP policy");
    recorder.addFrame(textFrame(), 1000);
    require(ocrIsSerial && omp_get_max_active_levels() == priorParallelLevels,
            "OCR did not serialize work and restore caller OpenMP policy");
    QImage changed = textFrame();
    { QPainter painter(&changed); painter.setPen(Qt::black); painter.drawText(30, 220, "NEVERINDEXED 9999"); }
    cancelEnabled = true;
    bool canceled = false;
    try { recorder.addFrame(changed, 2000); }
    catch (const replay::OcrCancelled &) { canceled = true; }
    require(ocrIsSerial && omp_get_max_active_levels() == priorParallelLevels,
            "OCR cancellation did not restore caller OpenMP policy");
    require(canceled, "OCR cancellation did not use graceful cancellation status");
    require(!recorder.statsJSON()["failed"].toBool(), "normal OCR cancellation marked dataset failed");
    recorder.finish();
    const auto accepted = replay::listFrames(options.directory);
    require(accepted.size() == 1 && accepted[0].available && replay::listObservations(options.directory).size() == 1,
            "OCR cancellation lost prior video frame or committed a canceled observation");
    require(!replay::loadFrame(options.directory, accepted[0].id).isNull(), "prior video frame was not finalized after OCR cancellation");
    require(replay::searchFrames(options.directory, "NEVERINDEXED").empty(), "canceled OCR text leaked into index");
    require(recorder.statsJSON()["ocr_budget_cancellations"].toInteger() == 1, "OCR cancellation was not counted");

    options.directory = root + "/ocr-deadline";
    options.codec = "webp";
    options.ocrMaxWallMs = 1;
    bool delayed = false;
    // Inject a bounded delay into the first progress check, so this tests the
    // deadline deterministically rather than relying on a machine's OCR speed.
    options.stopRequested = [&] {
        if (!delayed) { delayed = true; std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
        return false;
    };
    replay::Recorder deadline(options);
    requireError([&] { deadline.addFrame(changed, 1000); }, "OCR deadline did not stop the frame");
    require(omp_get_max_active_levels() == priorParallelLevels, "OCR deadline did not restore caller OpenMP policy");
    require(deadline.statsJSON()["failed"].toBool() && replay::listFrames(options.directory).empty() &&
            replay::listObservations(options.directory).empty(), "deadline published partial OCR text or media");
    require(QDir(QDir(options.directory).filePath("media")).entryList(QDir::Files).empty(), "deadline wrote canceled frame media");
    std::cout << "PASS graceful OCR cancellation preserves prior video; deadlines publish no partial text\n";
}
} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    try {
        require(temporary.isValid(), "cannot create temporary test directory");
        for (const QString &codec : {QString("webp"), QString("h264"), QString("hevc")}) checkCodec(temporary.path(), codec);
        if (qEnvironmentVariableIsSet("REPLAY_TEST_VAAPI")) {
            checkCodec(temporary.path(), "h264-vaapi");
            checkCodec(temporary.path(), "hevc-vaapi");
        }
        checkIncomplete(temporary.path());
        checkDiskBudget(temporary.path());
        checkOcrStop(temporary.path());
    } catch (const std::exception &exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
    return 0;
}
