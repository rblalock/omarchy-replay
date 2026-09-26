#include "capture.h"
#include "fixture.h"
#include "recorder.h"
#include "meeting_index.h"
#include "viewer.h"
#include "activity_signals.h"
#include "index_scheduler.h"
#include "index_resources.h"
#include "index_service.h"
#include "recording_service.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QThread>
#include <QTimer>
#include <QTimeZone>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }
void json(const QJsonValue &value) {
    const QJsonDocument doc = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout);
}
void saveJson(const QString &path, const QJsonValue &value) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) throw std::runtime_error("Cannot write result JSON");
    const auto doc = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) throw std::runtime_error("Cannot commit result JSON");
}
double number(const QCommandLineParser &p, const QString &name, double lo, double hi) {
    bool ok;
    const double value = p.value(name).toDouble(&ok);
    if (!ok || !std::isfinite(value) || value < lo || value > hi)
        throw std::runtime_error(QString("--%1 must be between %2 and %3").arg(name).arg(lo).arg(hi).toStdString());
    return value;
}
int integer(const QCommandLineParser &p, const QString &name, int lo, int hi) {
    const double value = number(p, name, lo, hi);
    if (std::floor(value) != value) throw std::runtime_error(("--" + name + " must be an integer").toStdString());
    return int(value);
}
qint64 timeArgument(const QCommandLineParser &p, const QString &name) {
    if (!p.isSet(name)) return 0;
    const QString value = p.value(name).trimmed();
    bool epoch = false;
    const qint64 ms = value.toLongLong(&epoch);
    if (epoch) return ms;
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (parsed.isValid()) return parsed.toMSecsSinceEpoch();
    const QDate date = QDate::fromString(value, Qt::ISODate);
    if (date.isValid()) return QDateTime(date, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
    throw std::runtime_error(QString("--%1 must be ISO-8601 or epoch milliseconds").arg(name).toStdString());
}
double cpu(const rusage &u) {
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}
QJsonObject processIo() {
    QFile file("/proc/self/io");
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonObject result;
    for (const auto &line : file.readAll().split('\n')) {
        const auto parts = line.split(':');
        if (parts.size() == 2) result[QString::fromLatin1(parts[0])] = double(parts[1].trimmed().toULongLong());
    }
    return result;
}
QJsonArray frameJson(const QVector<replay::FrameRecord> &frames) {
    QJsonArray result;
    for (const auto &f : frames) result.append(QJsonObject{
        {"id", f.id}, {"timestamp_ms", f.timestampMs},
        {"timestamp", QDateTime::fromMSecsSinceEpoch(f.timestampMs, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
        {"last_timestamp_ms", f.lastTimestampMs}, {"observations", f.observationCount},
        {"text", f.text}, {"codec", f.codec}, {"available", f.available},
        {"archive_available", f.archiveAvailable}, {"ocr_state", f.ocrState}, {"ocr_error", f.ocrError}});
    return result;
}
QImage grimCapture(const QString &output) {
    QProcess process;
    process.start("grim", {"-o", output, "-t", "ppm", "-"});
    if (!process.waitForStarted(2000)) throw std::runtime_error("grim did not start");
    QElapsedTimer deadline;
    deadline.start();
    QByteArray data;
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        data += process.readAllStandardOutput();
        if (data.size() > 128 * 1024 * 1024 || deadline.elapsed() > 5000 || interrupted) {
            process.kill(); process.waitForFinished(1000);
            throw std::runtime_error("grim capture stopped: size/time budget or interruption");
        }
    }
    data += process.readAllStandardOutput();
    if (process.exitCode() != 0) throw std::runtime_error(("grim: " + QString::fromUtf8(process.readAllStandardError()).left(1000)).toStdString());
    auto image = QImage::fromData(data, "PPM");
    if (image.isNull()) throw std::runtime_error("grim returned an invalid image");
    return image;
}

class IndexWorker {
public:
    void start(const replay::RecorderOptions &options, const QStringList &schedulerArguments = {}) {
        directory = options.directory;
        QStringList arguments{
            "index", "--dir", options.directory, "--follow", "--parent-pid", QString::number(getpid()),
            "--ocr-mode", options.ocrMode, "--ocr-cpu-percent", QString::number(options.ocrCpuPercent),
            "--ocr-max-wall-ms", QString::number(options.ocrMaxWallMs),
            "--ocr-max-height", QString::number(options.ocrMaxHeight)};
        if (!options.ocrDataPath.isEmpty()) arguments << "--ocr-data-path" << options.ocrDataPath;
        arguments << schedulerArguments;
        process.start(QCoreApplication::applicationFilePath(), arguments);
        if (!process.waitForStarted(3000)) throw std::runtime_error("Cannot start background indexer");
    }
    bool running() {
        process.waitForFinished(0);
        collect();
        return process.state() != QProcess::NotRunning;
    }
    std::optional<double> cpuSeconds() const {
        if (process.state() == QProcess::NotRunning) return std::nullopt;
        const auto receipt = replay::ownedIndexWorkerPolicy(directory, process.processId());
        const auto workerPid = receipt.value("pid").toInteger();
        if (!workerPid) return std::nullopt;
        const auto readCpu = [](qint64 pid) -> std::optional<double> {
            QFile stat(QString("/proc/%1/stat").arg(pid));
            if (!stat.open(QIODevice::ReadOnly)) return std::nullopt;
            const QByteArray row = stat.readAll();
            const auto fields = row.mid(row.lastIndexOf(')') + 2).simplified().split(' ');
            if (fields.size() < 13) return std::nullopt;
            bool userOk = false, systemOk = false;
            const auto user = fields[11].toULongLong(&userOk), system = fields[12].toULongLong(&systemOk);
            const long ticks = sysconf(_SC_CLK_TCK);
            if (!userOk || !systemOk || ticks <= 0) return std::nullopt;
            return double(user + system) / ticks;
        };
        const auto workerCpu = readCpu(workerPid);
        if (workerPid == process.processId()) return workerCpu;
        const auto controllerCpu = readCpu(process.processId());
        if (!workerCpu || !controllerCpu) return std::nullopt;
        return *workerCpu + *controllerCpu;
    }
    QJsonObject stop() {
        bool forced = false;
        if (running()) {
            process.terminate();
            if (!process.waitForFinished(6500)) {
                forced = true;
                process.kill(); process.waitForFinished(1000);
            }
        }
        collect();
        QJsonObject result{{"forced_stop", forced}, {"exit_code", process.exitCode()},
                           {"normal_exit", process.exitStatus() == QProcess::NormalExit}};
        const auto report = QJsonDocument::fromJson(output);
        if (report.isObject()) result["result"] = report.object();
        if (!errors.isEmpty()) result["stderr_tail"] = QString::fromUtf8(errors);
        return result;
    }
    ~IndexWorker() {
        if (process.state() != QProcess::NotRunning) {
            process.terminate();
            if (!process.waitForFinished(6500)) { process.kill(); process.waitForFinished(1000); }
        }
    }
private:
    void collect() {
        output += process.readAllStandardOutput();
        errors += process.readAllStandardError();
        if (output.size() > 1024 * 1024) output = output.right(1024 * 1024);
        if (errors.size() > 8192) errors = errors.right(8192);
    }
    QProcess process;
    QString directory;
    QByteArray output, errors;
};
} // namespace

int main(int argc, char **argv) {
    umask(0077);
    qputenv("OMP_THREAD_LIMIT", "1");
    const QString command = argc > 1 ? QString::fromLocal8Bit(argv[1]) : "help";
    if (command != "view" && command != "fixture") qputenv("QT_QPA_PLATFORM", "offscreen");
    bool syntheticDaemon = false;
    for (int i = 2; i < argc; ++i) if (QString::fromLocal8Bit(argv[i]) == "--synthetic") syntheticDaemon = true;
    if (command == "daemon" && syntheticDaemon) {
        qputenv("QT_QPA_PLATFORMTHEME", ""); qputenv("QT_STYLE_OVERRIDE", "Fusion");
    }
    const bool gui = command == "view" || command == "fixture" || command == "demo" || command == "export-fixture" ||
                     (command == "daemon" && syntheticDaemon);
    QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
    std::unique_ptr<QCoreApplication> app;
    if (gui) app = std::make_unique<QApplication>(argc, argv);
    else app = std::make_unique<QCoreApplication>(argc, argv);
    app->setApplicationName("Omarchy Replay");
    app->setApplicationVersion(REPLAY_VERSION);
    if (command == "daemon") {
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        try {
            QStringList arguments{app->applicationFilePath()};
            arguments.append(app->arguments().mid(2));
            return replay::recordingCommand(arguments, [] { return bool(interrupted); });
        } catch (const std::exception &error) { std::fprintf(stderr, "Replay: %s\n", error.what()); return 1; }
    }
    if (auto* guiApp = qobject_cast<QGuiApplication*>(app.get()))
        // The viewer's compositor exclusion must not hide synthetic fixtures.
        guiApp->setDesktopFileName(command == "view" ? "omarchy-replay" : "omarchy-replay-fixture");
    QCommandLineParser p;
    p.setApplicationDescription("Local screen history and searchable text. Recording is explicitly controlled with daemon commands.");
    p.addHelpOption();
    p.addVersionOption();
    p.addPositionalArgument("command", "demo | record | outputs | fixture | export-fixture | index | service | prioritize | catch-up | status | search | recall | list | extract | view");
    p.addPositionalArgument("query", "Literal search words (search command only)", "[query...]");
    p.addOptions({
        {{"d", "dir"}, "New recording directory, or existing dataset for search/view.", "path", "runs/demo"},
        {"settings", "View only: open settings in this history's viewer."},
        {"codec", "webp, h264, hevc, h264-vaapi, or hevc-vaapi.", "codec", "webp"},
        {"device", "VAAPI render device.", "path", "/dev/dri/renderD128"},
        {"interval", "Normal capture interval in seconds (0.25–60).", "seconds", "2"},
        {"duration", "Finite recording/display duration in seconds (1–14400).", "seconds", "30"},
        {"frames", "Synthetic frame count (1–10000).", "count", "16"},
        {"width", "Synthetic image width.", "pixels", "1920"},
        {"height", "Synthetic image height.", "pixels", "1080"},
        {"segment-frames", "Maximum distinct frames per video segment.", "count", "30"},
        {"segment-seconds", "Maximum wall-time span per video segment.", "seconds", "60"},
        {"max-mib", "Dataset disk budget in MiB (stops instead of evicting history).", "mib", "512"},
        {"no-ocr", "Skip text recognition to isolate media cost."},
        {"ocr-mode", "Text recognition strategy: full, experimental incremental or regions.", "mode", "full"},
        {"ocr-data-path", "Optional directory containing eng.traineddata; default uses installed Tesseract data.", "path"},
        {"ocr-max-height", "Experimental OCR height cap: 0 keeps original, otherwise 256–8192 without upscaling. Archived pixels stay unchanged.", "pixels", "0"},
        {"ocr-cpu-percent", "Optional cooperative OCR CPU budget (0 disables; percent of one CPU).", "percent", "0"},
        {"ocr-cpu-ceiling-percent", "Whole index-worker CPU safety ceiling via a temporary Replay user service (0 disables).", "percent", "0"},
        {"ocr-reuse", "Enable experimental exact-image OCR reuse (off by default)."},
        {"no-ocr-reuse", "Disable exact-image OCR reuse for comparison."},
        {"pressure-cpu-percent", "Adaptive allowance during sustained CPU saturation and pressure.", "percent", "10"},
        {"ocr-max-wall-ms", "OCR wall-time limit per pass, including CPU pacing waits (1–60000).", "ms", "10000"},
        {"scheduler", "Index worker scheduling: fixed or experimental adaptive.", "mode", "fixed"},
        {"idle-seconds", "Sustained compositor inactivity before adaptive catch-up (respects idle inhibitors).", "seconds", "60"},
        {"idle-cpu-percent", "Adaptive OCR allowance when idle and CPU pressure is low.", "percent", "40"},
        {"request-cpu-percent", "Adaptive OCR allowance for requested moments/catch-up when pressure is low.", "percent", "30"},
        {"index-while-viewing", "View only: own an index worker while this viewer is open, if none is running."},
        {"context-seconds", "Prioritize command: neighboring time range around --id, at most 32 neighbors.", "seconds", "15"},
        {"boost-seconds", "Catch-up command: finite request allowance, up to 300 seconds.", "seconds", "120"},
        {"retry-failed", "Index command only: retry retained failed originals once during this run."},
        {"indexing", "Index during capture (sync) or use a bounded background queue (deferred).", "mode", "sync"},
        {"archive-first", "Retain lossless WebP images independently of OCR backlog (requires webp and deferred indexing)."},
        {"pending-frames", "Maximum held originals; 0 uses byte bounds only. Separate from history retention.", "count", "16"},
        {"pending-mib", "Maximum original-image bytes retained for deferred indexing.", "mib", "64"},
        {"drain-seconds", "After capture, allow at most this long for background indexing to catch up.", "seconds", "10"},
        {"follow", "Keep the index command waiting for newly captured frames until stopped."},
        {"parent-pid", "Internal index-worker parent process guard.", "pid", "0"},
        {"resource-unit", "Internal managed index service identity.", "unit"},
        {"owner-pid", "Internal managed index controller identity.", "pid", "0"},
        {"owner-start-ticks", "Internal managed index controller start time.", "ticks", "0"},
        {"capture-only", "Measure acquisition without retaining screen pixels or OCR."},
        {"realtime", "Pace synthetic demo to the requested interval."},
        {"static", "Repeat identical synthetic pixels for the idle/dedup case."},
        {"workload", "Synthetic workload: mixed scenes or editing one invoice.", "name", "mixed"},
        {"backend", "Wayland capture backend: native or grim.", "name", "native"},
        {"output", "Explicit Wayland output name for record.", "name"},
        {"id", "Frame ID for extraction or recall moment fetch.", "id", "1"},
        {"out", "Destination image for extraction or recall moment fetch.", "path", "runs/extracted.png"},
        {"since", "recall/list: include moments captured at or after this ISO-8601 time or epoch milliseconds.", "time"},
        {"until", "recall/list: include moments captured at or before this ISO-8601 time or epoch milliseconds.", "time"},
        {"limit", "recall/list: maximum results returned (1-1000).", "count", "20"},
        {"offset", "recall/list: results to skip before the first returned.", "count", "0"},
        {"order", "recall: chronological or rank result order.", "order", "chronological"},
        {"source", "recall: screen, meetings, or all.", "source", "screen"},
    });
    p.process(*app);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        if (command == "help") p.showHelp();
        const auto positional = p.positionalArguments();
        const QString directory = QDir(p.value("dir")).absolutePath();
        const QString schedulerMode = p.isSet("index-while-viewing") && !p.isSet("scheduler")
            ? QStringLiteral("adaptive") : p.value("scheduler");
        if (schedulerMode != "fixed" && schedulerMode != "adaptive")
            throw std::runtime_error("--scheduler must be fixed or adaptive");
        const int idleSeconds = integer(p, "idle-seconds", 1, 3600);
        replay::SchedulerOptions schedulerOptions;
        schedulerOptions.activeCpuPercent = p.isSet("ocr-cpu-percent") ? number(p, "ocr-cpu-percent", 0, 100) : 10;
        schedulerOptions.idleCpuPercent = number(p, "idle-cpu-percent", 1, 100);
        schedulerOptions.requestCpuPercent = number(p, "request-cpu-percent", 1, 100);
        schedulerOptions.pressureCpuPercent = number(p, "pressure-cpu-percent", 1, 100);
        const double cpuCeiling = number(p, "ocr-cpu-ceiling-percent", 0, 100);
        if (cpuCeiling > 0 && cpuCeiling < 1)
            throw std::runtime_error("--ocr-cpu-ceiling-percent must be 0 or between 1 and 100");
        if (schedulerMode == "adaptive" && schedulerOptions.activeCpuPercent < 1)
            throw std::runtime_error("Adaptive scheduling needs an active OCR allowance between 1 and 100 percent");
        QStringList schedulerArguments{"--scheduler", schedulerMode, "--idle-seconds", QString::number(idleSeconds),
            "--idle-cpu-percent", QString::number(schedulerOptions.idleCpuPercent),
            "--request-cpu-percent", QString::number(schedulerOptions.requestCpuPercent),
            "--pressure-cpu-percent", QString::number(schedulerOptions.pressureCpuPercent),
            "--ocr-cpu-ceiling-percent", QString::number(cpuCeiling)};
        if (p.isSet("ocr-reuse") && p.isSet("no-ocr-reuse"))
            throw std::runtime_error("Choose only one of --ocr-reuse and --no-ocr-reuse");
        if (p.isSet("ocr-reuse")) schedulerArguments << "--ocr-reuse";
        if (p.isSet("no-ocr-reuse")) schedulerArguments << "--no-ocr-reuse";
        if (p.isSet("index-while-viewing") && command != "view")
            throw std::runtime_error("--index-while-viewing is only supported by view");
        if (p.isSet("settings") && command != "view")
            throw std::runtime_error("--settings is only supported by view");
        const QString ocrMode = p.value("ocr-mode");
        if (ocrMode != "full" && ocrMode != "incremental" && ocrMode != "regions")
            throw std::runtime_error("--ocr-mode must be full, incremental or regions");
        const int ocrMaxHeight = integer(p, "ocr-max-height", 0, 8192);
        if (ocrMaxHeight > 0 && ocrMaxHeight < 256)
            throw std::runtime_error("--ocr-max-height must be 0 or between 256 and 8192");
        QString ocrDataPath = p.value("ocr-data-path");
        if (!ocrDataPath.isEmpty()) {
            ocrDataPath = QDir(ocrDataPath).absolutePath();
            const QFileInfo model(QDir(ocrDataPath).filePath("eng.traineddata"));
            if (!model.isFile() || !model.isReadable() || model.size() == 0)
                throw std::runtime_error("--ocr-data-path must contain a readable, nonempty eng.traineddata file");
        }
        if (p.isSet("retry-failed") && command != "index")
            throw std::runtime_error("--retry-failed is only supported by index");
        if (command == "service") {
            if (positional.size() != 2) throw std::runtime_error("Use service start|status|pause|resume|stop --dir <saved history>");
            const QString action = positional[1];
            if (action == "run") return replay::runIndexService(directory, [] { return bool(interrupted); });
            if (action == "status") { json(replay::indexServiceStatus(directory)); return 0; }
            QJsonObject policy;
            for (const QString &key : {QString("scheduler"), QString("ocr-mode"), QString("ocr-cpu-percent"),
                    QString("ocr-max-wall-ms"), QString("ocr-max-height"), QString("ocr-data-path"),
                    QString("idle-seconds"), QString("idle-cpu-percent"), QString("request-cpu-percent"),
                    QString("pressure-cpu-percent"), QString("ocr-cpu-ceiling-percent")}) {
                if (!p.isSet(key)) continue;
                QString field = key; field.replace('-', '_');
                if (key == "scheduler" || key == "ocr-mode" || key == "ocr-data-path") policy[field] = p.value(key);
                else { bool valid = false; const double value = p.value(key).toDouble(&valid);
                    if (!valid || !std::isfinite(value)) throw std::runtime_error("Invalid service policy number");
                    policy[field] = value;
                }
            }
            if (p.isSet("ocr-reuse") || p.isSet("no-ocr-reuse")) policy["ocr_reuse"] = p.isSet("ocr-reuse");
            json(replay::controlIndexService(directory, action, policy)); return 0;
        }
        if (command == "status") { json(replay::indexingStatus(directory)); return 0; }
        if (command == "prioritize") {
            const int count = replay::requestIndexing(directory, integer(p, "id", 1, 2147483647),
                                                       integer(p, "context-seconds", 0, 300));
            json(QJsonObject{{"requested_frames", count}, {"indexing", replay::indexingStatus(directory)}});
            return 0;
        }
        if (command == "catch-up") {
            replay::requestCatchUp(directory, integer(p, "boost-seconds", 1, 300));
            json(replay::indexingStatus(directory)); return 0;
        }
        if (command == "index") {
            const int parentPid = integer(p, "parent-pid", 0, 2147483647);
            if (parentPid && (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parentPid))
                throw std::runtime_error("Index worker parent is unavailable");
            if (getpriority(PRIO_PROCESS, 0) < 10 && setpriority(PRIO_PROCESS, 0, 10) != 0)
                throw std::runtime_error("Cannot lower indexer CPU scheduling priority");
            const QString resourceUnit = p.value("resource-unit");
            const int ownerPid = integer(p, "owner-pid", 0, 2147483647);
            bool validOwnerTicks = false;
            const auto ownerTicks = p.value("owner-start-ticks").toULongLong(&validOwnerTicks);
            std::unique_ptr<replay::IndexOwnerGuard> owner;
            QString unavailableReason;
            if (!resourceUnit.isEmpty()) {
                if (parentPid || cpuCeiling <= 0 || !validOwnerTicks || !ownerTicks)
                    throw std::runtime_error("Invalid managed index owner configuration");
                owner = std::make_unique<replay::IndexOwnerGuard>(ownerPid, ownerTicks);
            } else if (ownerPid || ownerTicks) {
                throw std::runtime_error("Index owner requires a managed resource unit");
            } else if (cpuCeiling > 0) {
                QStringList arguments{"index", "--dir", directory, "--ocr-mode", ocrMode,
                    "--ocr-cpu-percent", schedulerMode == "adaptive" ? QString::number(schedulerOptions.activeCpuPercent) : p.value("ocr-cpu-percent"),
                    "--ocr-max-height", QString::number(ocrMaxHeight)};
                arguments << schedulerArguments;
                if (p.isSet("ocr-max-wall-ms")) arguments << "--ocr-max-wall-ms" << p.value("ocr-max-wall-ms");
                if (!ocrDataPath.isEmpty()) arguments << "--ocr-data-path" << ocrDataPath;
                if (p.isSet("follow")) arguments << "--follow";
                if (p.isSet("retry-failed")) arguments << "--retry-failed";
                const auto managed = replay::runManagedIndex(arguments, cpuCeiling, [] { return bool(interrupted); });
                if (managed.launched) {
                    std::fwrite(managed.output.constData(), 1, managed.output.size(), stdout);
                    std::fwrite(managed.errors.constData(), 1, managed.errors.size(), stderr);
                    return managed.exitCode;
                }
                unavailableReason = managed.unavailableReason;
                if (interrupted) return 0;
            }
            replay::IndexResources resources(cpuCeiling, resourceUnit, unavailableReason);
            if (!resourceUnit.isEmpty() && !resources.statsJSON().value("enforced").toBool())
                throw std::runtime_error(("Managed index limits could not be verified: " + resources.statsJSON().value("reason").toString()).toStdString());
            replay::IndexerOptions options;
            options.ocrReuse = p.isSet("ocr-reuse");
            options.directory = directory; options.ocrMode = p.value("ocr-mode");
            options.ocrDataPath = ocrDataPath; options.ocrMaxHeight = ocrMaxHeight;
            options.ocrCpuPercent = schedulerMode == "adaptive" ? schedulerOptions.activeCpuPercent : number(p, "ocr-cpu-percent", 0, 100);
            if (cpuCeiling > 0 && options.ocrCpuPercent == 0 && !resources.statsJSON().value("enforced").toBool())
                throw std::runtime_error("Worker CPU ceiling unavailable; configure cooperative OCR pacing or explicitly disable the ceiling");
            options.ocrMaxWallMs = schedulerMode == "adaptive" && !p.isSet("ocr-max-wall-ms")
                ? 60000 : integer(p, "ocr-max-wall-ms", 1, 60000);
            if (options.ocrCpuPercent > 0 && options.ocrCpuPercent < 1)
                throw std::runtime_error("--ocr-cpu-percent must be 0 or between 1 and 100");
            options.stopRequested = [] { return bool(interrupted); };
            std::unique_ptr<replay::ActivitySignals> activity;
            std::unique_ptr<replay::IndexScheduler> scheduler;
            if (schedulerMode == "adaptive") {
                activity = std::make_unique<replay::ActivitySignals>(idleSeconds);
                scheduler = std::make_unique<replay::IndexScheduler>(schedulerOptions, [&] { return activity->sample(); });
            }
            QElapsedTimer policyClock; policyClock.start();
            bool policyWritten = false;
            const auto publishPolicy = [&](bool requested) {
                const double allowance = scheduler ? scheduler->cpuPercent(requested) : options.ocrCpuPercent;
                if (!policyWritten || policyClock.elapsed() >= 2000) {
                    const auto live = scheduler ? scheduler->statsJSON() : QJsonObject{
                        {"mode", "active"}, {"effective_cpu_percent", allowance}};
                    QJsonObject telemetry;
                    for (const auto *key : {"mode", "effective_cpu_percent", "idle_known", "idle", "pressure_known", "cpu_pressure_percent",
                            "cpu_busy_known", "cpu_busy_percent", "pressure_reason"})
                        if (live.contains(key)) telemetry[key] = live.value(key);
                    telemetry["scheduler"] = schedulerMode;
                    telemetry["resources"] = resources.statsJSON();
                    telemetry["process_start_ticks"] = qint64(replay::processStartTicks(getpid()));
                    telemetry["owner_pid"] = ownerPid ? ownerPid : parentPid;
                    telemetry["owner_start_ticks"] = qint64(ownerPid ? ownerTicks : replay::processStartTicks(parentPid));
                    // Telemetry must not turn successful indexing into a failure.
                    try { replay::publishIndexWorkerPolicy(directory, telemetry); } catch (...) {}
                    policyWritten = true; policyClock.restart();
                }
                return allowance;
            };
            if (scheduler || options.ocrCpuPercent > 0) options.cpuPercentProvider = publishPolicy;
            rusage selfStart{}, childStart{};
            getrusage(RUSAGE_SELF, &selfStart); getrusage(RUSAGE_CHILDREN, &childStart);
            const auto ioStart = processIo();
            QElapsedTimer elapsed; elapsed.start();
            replay::Indexer indexer(options);
            publishPolicy(false);
            const auto retried = p.isSet("retry-failed") ? indexer.retryFailed() : 0;
            int failed = 0;
            bool canceled = false;
            while (!interrupted) {
                const auto item = indexer.processNext();
                if (!scheduler) publishPolicy(false);
                if (item.canceled) { canceled = true; break; }
                if (item.state == "failed") ++failed;
                if (item.state == "busy") continue; // Already backed off; one-shot workers must retry too.
                if (!item.processed) {
                    if (!p.isSet("follow")) break;
                    publishPolicy(false);
                    QThread::msleep(100);
                }
            }
            auto stats = indexer.statsJSON();
            if (scheduler) { stats["scheduler"] = scheduler->statsJSON(); stats["activity_signals"] = activity->statsJSON(); }
            stats["resources"] = resources.statsJSON();
            stats["retried_failed_jobs"] = retried;
            stats["elapsed_seconds"] = elapsed.elapsed() / 1000.0;
            stats["interrupted"] = bool(interrupted);
            stats["canceled"] = canceled;
            rusage selfEnd{}, childEnd{};
            getrusage(RUSAGE_SELF, &selfEnd); getrusage(RUSAGE_CHILDREN, &childEnd);
            stats["self_cpu_seconds"] = cpu(selfEnd) - cpu(selfStart);
            stats["process_cpu_seconds"] = cpu(selfEnd);
            stats["children_cpu_seconds"] = cpu(childEnd) - cpu(childStart);
            stats["self_peak_rss_mib"] = selfEnd.ru_maxrss / 1024.0;
            const auto ioEnd = processIo();
            for (const auto &key : {"read_bytes", "write_bytes", "cancelled_write_bytes"})
                if (ioStart.contains(key) && ioEnd.contains(key))
                    stats[QString("process_io_") + key] = ioEnd[key].toDouble() - ioStart[key].toDouble();
            stats["indexing"] = replay::indexingStatus(directory);
            stats["worker_pid"] = qint64(getpid());
            stats["process_start_ticks"] = qint64(replay::processStartTicks(getpid()));
            stats["owner_pid"] = ownerPid ? ownerPid : parentPid;
            stats["owner_start_ticks"] = qint64(ownerPid ? ownerTicks : replay::processStartTicks(parentPid));
            stats["worker_exit_code"] = failed || (canceled && !interrupted) ? 1 : 0;
            saveJson(QDir(directory).filePath("index-run.json"), stats);
            json(stats);
            return failed || (canceled && !interrupted) ? 1 : 0;
        }
        if (command == "outputs") {
            QJsonArray outputs;
            for (const auto &name : replay::WaylandCapture::outputs()) outputs.append(name);
            json(outputs); return 0;
        }
        if (command == "search") {
            const auto query = positional.mid(1).join(' ');
            if (query.trimmed().isEmpty()) throw std::runtime_error("Provide search words after search");
            json(frameJson(replay::searchFrames(directory, query))); return 0;
        }
        if (command == "list") {
            json(frameJson(replay::listFrames(directory,
                p.isSet("limit") ? integer(p, "limit", 1, 1000) : 200,
                p.isSet("offset") ? integer(p, "offset", 0, 2147483647) : 0,
                timeArgument(p, "since"), timeArgument(p, "until"))));
            return 0;
        }
        if (command == "recall") {
            const qint64 sinceMs = timeArgument(p, "since");
            const qint64 untilMs = timeArgument(p, "until");
            if (sinceMs > 0 && untilMs > 0 && sinceMs > untilMs)
                throw std::runtime_error("--since must not be after --until");
            const int limit = integer(p, "limit", 1, 1000);
            const int offset = integer(p, "offset", 0, 2147483647);
            const QString order = p.value("order");
            if (order != "chronological" && order != "rank")
                throw std::runtime_error("--order must be chronological or rank");
            const QString source = p.value("source");
            if (source != "screen" && source != "meetings" && source != "all")
                throw std::runtime_error("--source must be screen, meetings or all");
            if (p.isSet("id")) {
                const qint64 frameId = integer(p, "id", 1, 2147483647);
                const auto frame = replay::frameById(directory, frameId);
                if (!frame) throw std::runtime_error("Recorded frame does not exist");
                QJsonObject moment{{"schema_version", 1}, {"archive", directory},
                    {"moment", QJsonObject{
                        {"id", frame->id},
                        {"timestamp_ms", frame->timestampMs},
                        {"timestamp", QDateTime::fromMSecsSinceEpoch(frame->timestampMs, QTimeZone::UTC).toString(Qt::ISODateWithMs)},
                        {"last_timestamp_ms", frame->lastTimestampMs}, {"observations", frame->observationCount},
                        {"text", frame->text}, {"ocr_state", frame->ocrState}, {"ocr_error", frame->ocrError},
                        {"codec", frame->codec}, {"width", frame->width}, {"height", frame->height},
                        {"available", frame->available}, {"archive_available", frame->archiveAvailable},
                        {"image_path", frame->archiveAvailable ? frame->path : frame->originalPath},
                        {"original_path", frame->originalPath},
                        {"lines", replay::frameTextLines(directory, frameId)}}},
                    {"neighbors", frameJson(replay::framesNear(directory, frameId, integer(p, "context-seconds", 0, 300)))}};
                const bool extractRequested = p.isSet("out");
                const auto target = p.value("out");
                if (extractRequested) {
                    if (QFile::exists(target)) throw std::runtime_error("Extraction destination already exists");
                    QDir().mkpath(QFileInfo(target).absolutePath());
                    if (!replay::loadFrame(directory, frameId).save(target)) throw std::runtime_error("Could not save extracted frame");
                    moment["extracted_image"] = QFileInfo(target).absoluteFilePath();
                }
                json(moment); return 0;
            }
            const auto query = positional.mid(1).join(' ');
            if (query.trimmed().isEmpty()) throw std::runtime_error("Provide search words, or use --id to fetch one moment");
            QJsonObject result{{"schema_version", 1}, {"archive", directory}, {"query", query},
                {"source", source}, {"order", order}, {"limit", limit}, {"offset", qint64(offset)}};
            if (source != "meetings") {
                // Prefix mode matches the viewer's final-token expansion; see searchFramePage.
                const auto page = replay::searchFramePage(directory, query, limit, offset, replay::SearchMode::PrefixLastToken,
                    2, 0, sinceMs, untilMs, order == "rank" ? replay::SearchOrder::Rank : replay::SearchOrder::Chronological);
                result["total_matches"] = page.totalMatches;
                result["results"] = frameJson(page.frames);
            } else result["total_matches"] = 0;
            if (source != "screen") {
                const auto meetings = replay::searchMeetings(directory, query, limit, offset);
                result["meetings_total_matches"] = meetings.totalMatches;
                QJsonArray entries;
                for (const auto &meeting : meetings.results) {
                    QJsonObject entry{{"id", meeting.meeting.id}, {"title", meeting.meeting.title},
                        {"started_at_ms", meeting.meeting.startedAtMs}, {"time_known", meeting.meeting.timeKnown},
                        {"duration_seconds", meeting.meeting.durationSeconds},
                        {"matching_passages", meeting.matchingPassages}};
                    if (const auto record = replay::readMeeting(directory, meeting.meeting.id))
                        entry["transcript"] = record->transcript;
                    entries.append(entry);
                }
                result["meetings"] = entries;
            } else result["meetings"] = QJsonArray{};
            result["coverage"] = replay::rangeCoverage(directory, sinceMs, untilMs);
            json(result); return 0;
        }
        if (command == "view") {
            std::unique_ptr<IndexWorker> worker;
            QTimer workerPoll;
            replay::RecorderOptions viewIndexing;
            viewIndexing.directory = directory; viewIndexing.ocrMode = ocrMode;
            viewIndexing.ocrCpuPercent = schedulerOptions.activeCpuPercent;
            viewIndexing.ocrMaxWallMs = p.isSet("ocr-max-wall-ms") ? integer(p, "ocr-max-wall-ms", 1, 60000) : 60000;
            viewIndexing.ocrDataPath = ocrDataPath; viewIndexing.ocrMaxHeight = ocrMaxHeight;
            const auto ensureViewIndexer = [&] {
                try {
                    if (worker && worker->running()) return;
                    if (replay::indexServiceEnabled(directory)) return;
                    const auto state = replay::indexingStatus(directory);
                    if (worker) {
                        // A lock race can leave another process owning the work.
                        // Otherwise a stopped owned worker needs explicit recovery,
                        // rather than an endless restart loop on a bad dataset.
                        if (!state["indexer_running"].toBool()) { workerPoll.stop(); return; }
                        worker.reset();
                    }
                    if (state["indexer_running"].toBool() || state["pending"].toInteger() == 0) return;
                    worker = std::make_unique<IndexWorker>();
                    worker->start(viewIndexing, schedulerArguments);
                } catch (const std::exception& error) {
                    workerPoll.stop();
                    std::fprintf(stderr, "Viewer indexing paused: %s\n", error.what());
                }
            };
            if (p.isSet("index-while-viewing")) {
                workerPoll.setInterval(2000);
                QObject::connect(&workerPoll, &QTimer::timeout, ensureViewIndexer);
                workerPoll.start();
                ensureViewIndexer();
            }
            QTimer interruptionPoll;
            interruptionPoll.setInterval(100);
            QObject::connect(&interruptionPoll, &QTimer::timeout, [] {
                if (interrupted) QCoreApplication::quit();
            });
            interruptionPoll.start();
            const int result = replay::showViewer(directory, p.isSet("settings"));
            if (worker) worker->stop();
            return result;
        }
        if (command == "extract") {
            const auto image = replay::loadFrame(directory, integer(p, "id", 1, 2147483647));
            const auto target = p.value("out");
            if (QFile::exists(target)) throw std::runtime_error("Extraction destination already exists");
            QDir().mkpath(QFileInfo(target).absolutePath());
            if (!image.save(target)) throw std::runtime_error("Could not save extracted frame");
            json(QJsonObject{{"image", QFileInfo(target).absoluteFilePath()}}); return 0;
        }
        const auto size = QSize(integer(p, "width", 320, 8192), integer(p, "height", 240, 8192));
        if (qint64(size.width()) * size.height() > 20 * 1024 * 1024) throw std::runtime_error("Synthetic image exceeds 20 megapixels");
        const double interval = number(p, "interval", 0.25, 60);
        const double duration = number(p, "duration", 1, 14400);
        const auto workload = p.value("workload");
        if (workload != "mixed" && workload != "editing") throw std::runtime_error("--workload must be mixed or editing");
        const bool editing = workload == "editing";
        if (command == "fixture") {
            std::signal(SIGINT, SIG_DFL); std::signal(SIGTERM, SIG_DFL);
            return replay::showFixture(int(interval * 1000), size, int(duration * 1000), editing);
        }
        const int frames = integer(p, "frames", 1, 10000);
        if (command == "export-fixture") {
            if (QFile::exists(directory)) throw std::runtime_error("Fixture destination already exists");
            if (!QDir().mkpath(directory)) throw std::runtime_error("Cannot create fixture directory");
            QJsonArray truth;
            for (int i = 0; i < frames; ++i) {
                const auto path = QDir(directory).filePath(QString("%1.png").arg(i, 4, 10, QChar('0')));
                if (!replay::fixtureFrame(i % replay::fixtureFrameCount(), size, editing).save(path)) throw std::runtime_error("Fixture save failed");
                auto entry = replay::fixtureGroundTruth(i % replay::fixtureFrameCount(), editing);
                entry["path"] = QFileInfo(path).fileName(); entry["sample_index"] = i;
                truth.append(entry);
            }
            saveJson(QDir(directory).filePath("ground-truth.json"), truth);
            json(QJsonObject{{"directory", directory}, {"frames", frames}, {"data_origin", "synthetic"}}); return 0;
        }
        if (command != "demo" && command != "record") throw std::runtime_error("Unknown command; use --help");
        if (positional.size() > 1) throw std::runtime_error("Unexpected positional arguments");
        if (getpriority(PRIO_PROCESS, 0) < 10 && setpriority(PRIO_PROCESS, 0, 10) != 0)
            throw std::runtime_error("Cannot lower recorder CPU scheduling priority");
        const QString backend = p.value("backend");
        if (backend != "native" && backend != "grim") throw std::runtime_error("--backend must be native or grim");
        if (command == "record" && p.value("output").isEmpty()) throw std::runtime_error("record requires --output: select one visible monitor explicitly");
        replay::RecorderOptions options;
        options.directory = directory; options.codec = p.value("codec"); options.device = p.value("device");
        options.intervalSeconds = interval; options.ocr = !p.isSet("no-ocr");
        options.ocrMode = p.value("ocr-mode");
        options.ocrDataPath = ocrDataPath; options.ocrMaxHeight = ocrMaxHeight;
        options.ocrCpuPercent = schedulerMode == "adaptive" ? schedulerOptions.activeCpuPercent : number(p, "ocr-cpu-percent", 0, 100);
        options.ocrMaxWallMs = schedulerMode == "adaptive" && !p.isSet("ocr-max-wall-ms")
            ? 60000 : integer(p, "ocr-max-wall-ms", 1, 60000);
        if (options.ocrCpuPercent > 0 && options.ocrCpuPercent < 1)
            throw std::runtime_error("--ocr-cpu-percent must be 0 or between 1 and 100");
        options.stopRequested = [] { return bool(interrupted); };
        const auto indexingMode = p.value("indexing");
        if (indexingMode != "sync" && indexingMode != "deferred") throw std::runtime_error("--indexing must be sync or deferred");
        options.deferredOcr = indexingMode == "deferred";
        options.archiveFirst = p.isSet("archive-first");
        if (options.archiveFirst && (!options.deferredOcr || !options.ocr || options.codec != "webp" || p.isSet("capture-only")))
            throw std::runtime_error("--archive-first requires --codec webp --indexing deferred with retained OCR captures");
        if (schedulerMode == "adaptive" && !options.deferredOcr)
            throw std::runtime_error("Adaptive recording requires --indexing deferred");
        if (options.deferredOcr && (p.isSet("no-ocr") || p.isSet("capture-only")))
            throw std::runtime_error("Deferred indexing requires stored captures and OCR enabled");
        options.maxPendingFrames = integer(p, "pending-frames", 0, 256);
        options.maxPendingBytes = quint64(number(p, "pending-mib", 1, 1024) * 1024 * 1024);
        const double drainSeconds = number(p, "drain-seconds", 0, 60);
        options.segmentFrames = integer(p, "segment-frames", 1, 300);
        options.segmentSeconds = number(p, "segment-seconds", 1, 300);
        options.maxDiskBytes = quint64(number(p, "max-mib", 16, 8192) * 1024 * 1024);
        QElapsedTimer elapsed;
        elapsed.start();
        rusage selfStart{}, childStart{};
        getrusage(RUSAGE_SELF, &selfStart); getrusage(RUSAGE_CHILDREN, &childStart);
        const auto ioStart = processIo();
        std::unique_ptr<replay::Recorder> recorder;
        if (!p.isSet("capture-only")) recorder = std::make_unique<replay::Recorder>(options);
        std::unique_ptr<IndexWorker> indexWorker;
        if (options.deferredOcr && !replay::indexServiceEnabled(directory)) {
            indexWorker = std::make_unique<IndexWorker>();
            indexWorker->start(options, schedulerArguments);
        }
        std::unique_ptr<replay::WaylandCapture> native;
        if (command == "record" && backend == "native") native = std::make_unique<replay::WaylandCapture>(p.value("output"));
        const bool paced = command == "record" || p.isSet("realtime");
        const qint64 intervalMs = qint64(interval * 1000);
        const qint64 startTimestamp = QDateTime::currentMSecsSinceEpoch();
        const qint64 runStart = elapsed.elapsed();
        qint64 next = runStart;
        int samples = 0, missedSlots = 0, timeouts = 0, cancelledSamples = 0, backlogSkipped = 0;
        bool indexWorkerStoppedEarly = false;
        double captureMs = 0;
        QJsonArray truth;
        QImage staticFrame;
        if (command == "demo" && p.isSet("static")) staticFrame = replay::fixtureFrame(0, size, editing);
        while (!interrupted) {
            if (indexWorker && !indexWorker->running()) { indexWorkerStoppedEarly = true; break; }
            if (command == "demo" && samples >= frames) break;
            if (command == "record" && elapsed.elapsed() - runStart >= duration * 1000) break;
            if (paced) {
                while (!interrupted) {
                    const qint64 remaining = next - elapsed.elapsed();
                    if (remaining <= 0) break;
                    QThread::msleep(ulong(qMin<qint64>(50, remaining)));
                }
                if (interrupted || (command == "record" && elapsed.elapsed() - runStart >= duration * 1000)) break;
            }
            QElapsedTimer stage; stage.start();
            QImage image;
            if (command == "demo") image = staticFrame.isNull() ? replay::fixtureFrame(samples % replay::fixtureFrameCount(), size, editing) : staticFrame;
            else if (native) {
                try { image = native->capture(1500); }
                catch (const replay::CaptureTimeout &) { /* Report the missed observation below. */ }
            }
            else image = grimCapture(p.value("output"));
            captureMs += stage.nsecsElapsed() / 1e6;
            const qint64 timestamp = command == "demo" && !paced ? startTimestamp + samples * intervalMs : QDateTime::currentMSecsSinceEpoch();
            if (image.isNull()) ++timeouts;
            else if (recorder) {
                replay::AddFrameResult result;
                try { result = recorder->addFrame(image, timestamp); }
                catch (const replay::OcrCancelled &) {
                    // The canceled frame has not reached media/index storage.
                    // Finalize accepted frames below rather than abandon them.
                    ++samples;
                    ++cancelledSamples;
                    break;
                }
                if (result.backlogFull) ++backlogSkipped;
                if (command == "demo" && !result.backlogFull) {
                    auto entry = replay::fixtureGroundTruth(p.isSet("static") ? 0 : samples % replay::fixtureFrameCount(), editing);
                    entry["frame_id"] = result.frameId; entry["timestamp_ms"] = timestamp; entry["sample_index"] = samples;
                    truth.append(entry);
                }
            }
            ++samples;
            if (paced) {
                next += intervalMs;
                if (next < elapsed.elapsed()) {
                    const qint64 skipped = (elapsed.elapsed() - next) / intervalMs + 1;
                    // Finishing an OCR unit may outlast a finite recording.
                    // Count only slots requested before its end, not later ones.
                    const qint64 requestedRemaining = command == "record"
                        ? qMax<qint64>(0, (runStart + qint64(duration * 1000) - next + intervalMs - 1) / intervalMs)
                        : skipped;
                    missedSlots += int(qMin(skipped, requestedRemaining));
                    next += skipped * intervalMs;
                }
            }
        }
        if (recorder) recorder->finish();
        const double captureElapsedSeconds = elapsed.elapsed() / 1000.0;
        const double captureLoopSeconds = (elapsed.elapsed() - runStart) / 1000.0;
        const double captureSelfCpu = [&] { rusage u{}; getrusage(RUSAGE_SELF, &u); return cpu(u) - cpu(selfStart); }();
        rusage captureChildren{}; getrusage(RUSAGE_CHILDREN, &captureChildren);
        const auto liveIndexCpu = indexWorker ? indexWorker->cpuSeconds() : std::optional<double>(0);
        QJsonObject indexingAtCaptureEnd, indexWorkerResult;
        double drainElapsedSeconds = 0;
        if (indexWorker) {
            indexingAtCaptureEnd = replay::indexingStatus(directory);
            QElapsedTimer drain; drain.start();
            while (!interrupted && !indexWorkerStoppedEarly && drain.elapsed() < drainSeconds * 1000) {
                if (replay::indexingStatus(directory).value("pending").toInteger() == 0) break;
                if (!indexWorker->running()) { indexWorkerStoppedEarly = true; break; }
                QThread::msleep(50);
            }
            indexWorkerResult = indexWorker->stop();
            drainElapsedSeconds = drain.elapsed() / 1000.0;
        }
        rusage selfEnd{}, childEnd{};
        getrusage(RUSAGE_SELF, &selfEnd); getrusage(RUSAGE_CHILDREN, &childEnd);
        QJsonObject stats = recorder ? recorder->statsJSON() : QJsonObject{};
        stats["data_origin"] = command == "demo" ? "synthetic-rendered" : "wayland-output";
        if (command == "demo") stats["synthetic_workload"] = workload;
        stats["backend"] = command == "demo" ? "fixture" : backend;
        if (command == "record") stats["output"] = p.value("output");
        stats["requested_interval_seconds"] = interval;
        stats["realtime"] = paced; stats["samples_attempted"] = samples;
        stats["missed_schedule_slots"] = missedSlots; stats["capture_timeouts"] = timeouts;
        stats["cancelled_samples"] = cancelledSamples;
        stats["backlog_skipped_samples"] = backlogSkipped;
        stats["capture_elapsed_seconds"] = captureElapsedSeconds;
        stats["capture_loop_seconds"] = captureLoopSeconds;
        stats["capture_process_cpu_seconds"] = captureSelfCpu;
        if (liveIndexCpu) stats["capture_total_cpu_seconds"] = captureSelfCpu + cpu(captureChildren) - cpu(childStart) + *liveIndexCpu;
        stats["capture_cpu_scope"] = "Recorder plus reaped encoder/probe children, and live index controller/worker sampled from verified /proc identities; excludes compositor/GPU";
        stats["indexing_mode"] = indexingMode;
        if (indexWorker) {
            stats["indexing_at_capture_end"] = indexingAtCaptureEnd;
            stats["index_worker"] = indexWorkerResult;
            stats["index_drain_seconds"] = drainElapsedSeconds;
            stats["index_worker_stopped_early"] = indexWorkerStoppedEarly;
            stats["indexing"] = replay::indexingStatus(directory);
        }
        stats["capture_or_render_ms"] = captureMs; stats["elapsed_seconds"] = elapsed.elapsed() / 1000.0;
        stats["self_cpu_seconds"] = cpu(selfEnd) - cpu(selfStart);
        stats["children_cpu_seconds"] = cpu(childEnd) - cpu(childStart);
        const auto separateIndexResult = indexWorkerResult.value("result").toObject();
        if (!separateIndexResult.value("resources").toObject().value("service_unit").toString().isEmpty()) {
            stats["managed_index_cpu_seconds"] = separateIndexResult.value("process_cpu_seconds").toDouble()
                + separateIndexResult.value("children_cpu_seconds").toDouble();
            stats["total_cpu_seconds"] = stats.value("self_cpu_seconds").toDouble()
                + stats.value("children_cpu_seconds").toDouble() + stats.value("managed_index_cpu_seconds").toDouble();
            stats["children_cpu_scope"] = "Reaped recorder children, including the lightweight index controller; managed OCR service is separate";
        }
        stats["self_peak_rss_mib"] = selfEnd.ru_maxrss / 1024.0;
        stats["child_peak_rss_mib"] = childEnd.ru_maxrss / 1024.0;
        const auto ioEnd = processIo();
        for (const auto &key : {"read_bytes", "write_bytes", "cancelled_write_bytes"})
            if (ioStart.contains(key) && ioEnd.contains(key))
                stats[QString("process_io_") + key] = ioEnd[key].toDouble() - ioStart[key].toDouble();
        stats["io_scope"] = "Linux /proc/self/io delta after child reaping; includes waited-for children, excludes independent managed index service (see index_worker.result), compositor and device-wide activity; write_bytes is kernel-attributed I/O, not SSD physical writes";
        stats["interrupted"] = bool(interrupted);
        stats["effective_nice"] = getpriority(PRIO_PROCESS, 0);
        stats["scheduler"] = options.deferredOcr
            ? "capture independent of OCR; bounded lossless original staging; new observations skipped when full"
            : "single work unit; missed slots dropped; no raw-frame backlog";
        if (recorder) {
            saveJson(QDir(directory).filePath("run.json"), stats);
            if (command == "demo") saveJson(QDir(directory).filePath("ground-truth.json"), truth);
        }
        json(stats);
        return indexWorkerStoppedEarly || (indexWorker && indexWorkerResult.value("exit_code").toInt() != 0) ? 1 : 0;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "replay: %s\n", e.what());
        return 1;
    }
}
