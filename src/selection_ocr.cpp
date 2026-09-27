#include "selection_ocr.h"

#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <algorithm>
#include <csignal>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace replay {
namespace {
constexpr int MaxDimension = 16384;
constexpr qint64 MaxPixels = 32LL * 1024 * 1024;
constexpr qsizetype MaxTextBytes = 1024 * 1024;
constexpr qsizetype MaxErrorBytes = 64 * 1024;
constexpr qint64 MaxInputQueued = 128 * 1024;
constexpr int MaxWallMs = 10000;

SelectionOcrResult failure(const QString &message) { return {{}, message, false}; }

const QRegularExpression &languagePattern() {
    static const QRegularExpression pattern("^(?:script/)?[A-Za-z0-9_]+(?:\\+(?:script/)?[A-Za-z0-9_]+)*$");
    return pattern;
}
} // namespace

bool validOcrLanguages(const QString &languages) {
    return !languages.isEmpty() && languages.size() <= 256 && languagePattern().match(languages).hasMatch();
}

SelectionOcrResult recognizeSelection(const QImage &crop, const std::shared_ptr<std::atomic_bool> &cancel) {
    const auto cancelled = [&] { return cancel && cancel->load(); };
    if (cancelled()) return {{}, {}, true};
    if (crop.isNull() || crop.width() > MaxDimension || crop.height() > MaxDimension ||
        qint64(crop.width()) * crop.height() > MaxPixels)
        return failure("Select a smaller, nonempty area to copy.");
    const QString languages = qEnvironmentVariable("OMARCHY_OCR_LANGS", "eng").trimmed();
    const QString selectedLanguages = languages.isEmpty() ? QStringLiteral("eng") : languages;
    if (!validOcrLanguages(selectedLanguages))
        return failure("Check OMARCHY_OCR_LANGS: use language names joined by +, such as eng+deu.");

    QElapsedTimer elapsed;
    elapsed.start();
    const QImage image = crop.convertToFormat(QImage::Format_RGB888);
    if (image.isNull()) return failure("Could not prepare the selected image.");
    if (cancelled()) return {{}, {}, true};
    const QByteArray header = "P6\n" + QByteArray::number(image.width()) + ' ' + QByteArray::number(image.height()) + "\n255\n";
    const qsizetype rowBytes = image.width() * 3;
    qsizetype headerOffset = 0, rowOffset = 0;
    int row = 0;
    bool inputClosed = false;
    QByteArray output;
    qsizetype errorBytes = 0;

    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("OMP_THREAD_LIMIT", "1");
    environment.insert("OMP_NUM_THREADS", "1");
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    const pid_t parent = getpid();
    process.setChildProcessModifier([parent] {
        // Keep this fork/exec hook to Linux system calls. A closed/crashed
        // viewer must not leave this private, single-request worker behind.
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent) _exit(127);
        setpriority(PRIO_PROCESS, 0, 10);
        const rlimit noCore{0, 0};
        setrlimit(RLIMIT_CORE, &noCore);
    });
    const auto stop = [&](SelectionOcrResult result) {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(1000);
        }
        return result;
    };
    process.start("tesseract", {"stdin", "stdout", "--psm", "6", "--oem", "1", "--dpi", "300",
        "-l", selectedLanguages, "-c", "preserve_interword_spaces=1"}, QIODevice::ReadWrite);
    while (true) {
        if (cancelled()) return stop({{}, {}, true});
        if (elapsed.elapsed() >= MaxWallMs)
            return stop(failure("Text recognition took too long. Try a smaller selection."));
        if (process.state() == QProcess::Starting) {
            process.waitForStarted(25);
            continue;
        }
        const QByteArray next = process.readAllStandardOutput();
        const QByteArray diagnostic = process.readAllStandardError();
        if (next.size() > MaxTextBytes - output.size() || diagnostic.size() > MaxErrorBytes - errorBytes)
            return stop(failure("Text recognition returned too much data. Try a smaller selection."));
        output.append(next);
        errorBytes += diagnostic.size();
        if (process.state() == QProcess::NotRunning) break;
        while (!inputClosed && process.bytesToWrite() < MaxInputQueued) {
            if (headerOffset == header.size() && row == image.height()) {
                process.closeWriteChannel();
                inputClosed = true;
                break;
            }
            const bool writingHeader = headerOffset < header.size();
            const char *data = writingHeader ? header.constData() + headerOffset
                : reinterpret_cast<const char *>(image.constScanLine(row)) + rowOffset;
            const qint64 remaining = writingHeader ? header.size() - headerOffset : rowBytes - rowOffset;
            const qint64 written = process.write(data, std::min(remaining, MaxInputQueued - process.bytesToWrite()));
            if (written < 0) return stop(failure("Could not send the selected image for recognition."));
            if (!written) break;
            if (writingHeader) headerOffset += written;
            else {
                rowOffset += written;
                if (rowOffset == rowBytes) { ++row; rowOffset = 0; }
            }
        }
        if (process.bytesToWrite()) process.waitForBytesWritten(10);
        process.waitForReadyRead(15);
    }
    if (cancelled()) return {{}, {}, true};
    if (process.error() == QProcess::FailedToStart)
        return failure("Could not start text recognition. Install tesseract and its language data.");
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return failure("Text recognition failed. Check that your Tesseract language data is installed.");
    if (!inputClosed)
        return failure("Text recognition ended before the selected image was sent.");
    return {QString::fromUtf8(output).trimmed(), {}, false};
}

} // namespace replay
