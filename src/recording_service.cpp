#include "recording_service.h"
#include "recording_environment.h"
#include "replay_config.h"
#include "recorder.h"
#include "capture.h"
#include "fixture.h"
#include "index_service.h"
#include "storage_forecast.h"
#include "meeting_index.h"

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QScopedValueRollback>
#include <QStorageInfo>
#include <QThread>
#include <QUuid>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <csignal>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

namespace replay {
namespace {
constexpr qint64 MiB = 1024 * 1024;
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }
qint64 monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
void privateDirectory(const QString &path) {
    if (QFileInfo(path).isSymLink() || !QDir().mkpath(path) ||
        !QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        fail("Cannot create private Replay directory");
}
QJsonObject readJson(const QString &path) {
    QFile file(path); if (!file.exists()) return {};
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)) fail("Cannot read Replay service state");
    const auto bytes = file.read(128 * 1024 + 1);
    const auto document = QJsonDocument::fromJson(bytes);
    if (bytes.size() > 128 * 1024 || !document.isObject()) fail("Invalid Replay service state");
    return document.object();
}
void writeJson(const QString &path, const QJsonObject &object) {
    if (QFileInfo(path).isSymLink()) fail("Replay state must not be a symbolic link");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail("Cannot save Replay state");
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.commit()) fail("Cannot commit Replay state");
}
struct Lease {
    QFile file;
    explicit Lease(const QString &path, const QString &owner = "recording coordinator") : file(path) {
        const int fd = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        if (fd < 0) fail("Cannot open Replay " + owner + " lock");
        if (!file.open(fd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
            ::close(fd); fail("Cannot open Replay " + owner + " lock");
        }
        struct stat metadata{};
        if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() || metadata.st_nlink != 1)
            fail("Invalid Replay " + owner + " lock");
        if (flock(fd, LOCK_EX | LOCK_NB) < 0) fail("Another Replay " + owner + " is running");
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
};
RecorderOptions recordingOptions(const ReplayPaths &paths, const ReplayConfig &config) {
    RecorderOptions options;
    options.directory = paths.historyDirectory; options.resume = true; options.rollingStorage = true;
    options.archiveFirst = true; options.deferredOcr = true; options.codec = "webp";
    options.intervalSeconds = config.intervalSeconds; options.ocrMode = "incremental";
    options.ocrLanguages = config.ocrLanguages;
    options.maxDiskBytes = quint64(config.maxDiskMiB) * MiB;
    options.minFreeBytes = quint64(config.minFreeMiB) * MiB;
    options.maxPendingFrames = 0;
    return options;
}
EnvironmentOptions environmentOptions(const ReplayConfig &config) {
    EnvironmentOptions options;
    options.output = config.output; options.outputIdentity = config.outputIdentity;
    options.excludedApps = config.excludedApps; options.skippedApps = config.skippedApps;
    options.excludedWindows = config.excludedWindows;
    return options;
}

QString locationKey(const ReplayPaths &paths) {
    return QString::fromLatin1(QCryptographicHash::hash(paths.historyDirectory.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString locationToken(const ReplayPaths &paths) {
    return readJson(paths.stateDirectory + "/history-locations.json").value(locationKey(paths)).toObject().value("token").toString();
}
void checkStorageLocation(const ReplayPaths &paths, const ReplayConfig &config, bool registering = false) {
    if (config.storageDirectory.isEmpty()) return;
    const QFileInfo directory(paths.historyDirectory);
    if (!directory.isDir() || directory.isSymLink() || directory.ownerId() != getuid())
        fail("The configured history folder is unavailable or not owned by this user. Reconnect its disk or choose another folder.");
    QStorageInfo disk(paths.historyDirectory); disk.refresh();
    if (!disk.isValid() || !disk.isReady() || disk.isReadOnly()) fail("The configured history disk is unavailable or read-only.");
    const QByteArray type = disk.fileSystemType().toLower();
    if (type.startsWith("nfs") || type == "cifs" || type == "smb3" || type == "9p" || type.contains("sshfs") || type.contains("rclone"))
        fail("Replay history needs a local filesystem; network folders are not supported.");
    const QString expected = locationToken(paths);
    const QString marker = paths.historyDirectory + "/.replay-location.json";
    const auto found = readJson(marker).value("token").toString();
    if ((!expected.isEmpty() && found != expected) || (!registering && expected.isEmpty()))
        fail("The history folder does not match its saved disk identity. Reconnect the original disk; no replacement history was created.");
    if (!found.isEmpty() && !QRegularExpression("^[0-9a-f-]{36}$").match(found).hasMatch())
        fail("The history folder has an invalid Replay location marker.");
    if (expected.isEmpty() && !found.isEmpty() && !QFileInfo::exists(paths.historyDirectory + "/index.sqlite"))
        fail("The selected history folder is incomplete.");
}
void registerStorageLocation(const ReplayPaths &paths, const ReplayConfig &config) {
    if (config.storageDirectory.isEmpty() || !locationToken(paths).isEmpty()) return;
    QLockFile lease(paths.stateDirectory + "/history-locations.lock");
    lease.setStaleLockTime(10000);
    if (!lease.tryLock(1000)) fail("Another Replay process is registering a history folder; try again shortly");
    checkStorageLocation(paths, config, true);
    if (!locationToken(paths).isEmpty()) return;
    const QString marker = paths.historyDirectory + "/.replay-location.json";
    QString token = readJson(marker).value("token").toString();
    if (token.isEmpty()) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        writeJson(marker, {{"token", token}});
    }
    auto locations = readJson(paths.stateDirectory + "/history-locations.json");
    locations[locationKey(paths)] = QJsonObject{{"directory", paths.historyDirectory}, {"token", token}};
    writeJson(paths.stateDirectory + "/history-locations.json", locations);
}

class Coordinator {
public:
    ReplayPaths paths = replayPaths();
    ReplayConfigDocument document;
    std::unique_ptr<Recorder> recorder;
    std::unique_ptr<WaylandCapture> capture;
    std::unique_ptr<RecordingEnvironment> environment;
    std::unique_ptr<IndexStatusReader> indexReader;
    QProcess indexWorker;
    QProcess meetingWorker;
    QByteArray meetingOutput;
    QJsonObject meetingProgress;
    QString meetingError;
    bool meetingsAvailable = false, meetingWorkerStarted = false;
    qint64 nextMeetingDetection = 0, retryMeetingsAfter = 0;
    QJsonObject progress, usage, workerReceipt, deletion, storageForecastResult;
    EnvironmentSnapshot desktop;
    QString intent = "stopped", state = "stopped", reason = "Recording is stopped.", configError, indexError;
    QString maskToken, maskInstance, captureInstance, captureDisplay;
    quint64 maskConfigGeneration = 0;
    bool exclusionsPending = false;
    QString exclusionError;
    qint64 nextExclusionAttempt = 0;
    int exclusionFailures = 0;
    quint64 exclusionRevision = 0;
    QByteArray indexOutput, indexErrors;
    bool indexingPaused = false, shuttingDown = false, synthetic = false, workerStarted = false;
    bool tickActive = false, historyReady = false;
    QString storageError;
    qint64 nextStorageCheck = 0;
    qint64 nextCapture = 0, nextMaintenance = 0, nextConfig = 0, nextIndexPoll = 0, retryIndexAfter = 0;
    qint64 indexLastWork = 0, previousReady = 0, nextDesktopStatus = 0;
    quint64 controlRevision = 0;
    qint64 gapStart = 0, lastRetained = 0, retainedThisRun = 0, attempts = 0;
    QString gapReason;
    qint64 configModified = -1, configSize = -1;
    qint64 nextForecast = 0;
    qint64 forecastSampleAfter = 0;
    int captureRetryCount = 0;
    qint64 debugUntil = 0, debugStarted = 0, debugStartedWall = 0, debugAttempts = 0, debugRetained = 0;
    QJsonObject lastDebug{{"active", false}};

    QJsonObject currentDebug() const {
        return {{"active", debugUntil > 0}, {"started_at_ms", debugStartedWall},
            {"elapsed_ms", std::max<qint64>(0, std::min(monotonicMs(), debugUntil) - debugStarted)},
            {"capture_attempts", attempts - debugAttempts}, {"retained_moments", retainedThisRun - debugRetained},
            {"environment", environment ? environment->diagnostics() : QJsonObject{}}};
    }
    void stopDebug() {
        if (!debugUntil) return;
        lastDebug = currentDebug(); lastDebug["active"] = false;
        debugUntil = 0;
        if (environment) {
            environment->setDiagnosticsEnabled(false);
            lastDebug["environment"] = environment->diagnostics();
        }
    }
    void expireDebug() { if (debugUntil && monotonicMs() >= debugUntil) stopDebug(); }
    QJsonObject debugStatus() { expireDebug(); return debugUntil ? currentDebug() : lastDebug; }

    QJsonObject forecastCaptureSettings() const {
        return {{"directory", paths.historyDirectory}, {"output", document.config.output},
            {"output_identity", document.config.outputIdentity}, {"interval_seconds", document.config.intervalSeconds}};
    }

    explicit Coordinator(bool test, const QString &syntheticEnvironment) : synthetic(test) {
        privateDirectory(paths.stateDirectory); privateDirectory(paths.runtimeDirectory);
        privateDirectory(paths.cacheDirectory);
        try { document = loadReplayConfig(); rememberConfig(document.original); }
        catch (const std::exception &error) {
            configError = QString::fromUtf8(error.what()).left(500);
            document = loadReplayConfig(paths.stateDirectory + "/last-valid-config.toml");
            if (!document.exists) throw;
        }
        paths.historyDirectory = replayHistoryDirectory(document.config);
        const auto saved = readJson(paths.stateDirectory + "/recording.json");
        forecastSampleAfter = std::clamp<qint64>(saved.value("forecast_sample_after_ms").toInteger(), 0, now());
        const auto previousCapture = saved.value("forecast_capture").toObject();
        if (!previousCapture.isEmpty() && previousCapture != forecastCaptureSettings()) forecastSampleAfter = now();
        intent = saved.value("intent").toString("stopped");
        if (intent != "running" && intent != "paused" && intent != "stopped") fail("Invalid saved recording intent");
        indexingPaused = saved.value("indexing_paused").toBool(); deletion = saved.value("deletion").toObject();
        const QString hold = paths.runtimeDirectory + "/hold-capture-on-start";
        if (QFileInfo::exists(hold)) {
            if (QFileInfo(hold).isSymLink()) fail("Invalid startup capture hold");
            if (intent == "running") intent = "paused";
            saveIntent();
            if (!QFile::remove(hold)) fail("Could not consume startup capture hold");
        }
        if (!deletion.isEmpty() && !deletion.contains("directory")) deletion["directory"] = replayPaths().historyDirectory;
        prepareHistory();
        if (!synthetic) { environment = std::make_unique<RecordingEnvironment>(); environment->configure(environmentOptions(document.config)); }
        else if (!syntheticEnvironment.isEmpty()) {
            environment = std::make_unique<RecordingEnvironment>([syntheticEnvironment, reads = quint64(0)]() mutable {
                EnvironmentObservation observed;
                const auto input = readJson(syntheticEnvironment);
                if (!input.value("known").toBool()) return observed;
                observed.compositorAvailable = observed.lockNotificationsAvailable = observed.compositorLockKnown = true;
                observed.sessionKnown = observed.sessionActive = observed.sleepKnown = observed.configKnown = true;
                observed.compositorLocked = observed.sessionLocked = input.value("locked").toBool();
                observed.sleeping = input.value("sleeping").toBool(); observed.configError = false;
                observed.compositorInstance = "synthetic-session"; observed.eventGeneration = input.value("generation").toInteger();
                if (input.value("unstable").toBool()) observed.eventGeneration = ++reads;
                observed.monitors = input.value("monitors").toArray(); observed.windows = input.value("windows").toArray();
                observed.exclusionsVerified = true;
                return observed;
            });
            auto options = environmentOptions(document.config); options.exclusionMaskToken = QString(64, 'a');
            environment->configure(options);
        }
        if (historyReady && lastRetained > 0 && now() > lastRetained) recordGap(paths.historyDirectory, lastRetained, now(), "coordinator-restart");
        requestExclusions();
        saveIntent();
        log("Recording coordinator started.");
    }
    ~Coordinator() { stopMeetings(); stopIndex(); closeCapture(); }

    void checkStorage(bool registering = false) { checkStorageLocation(paths, document.config, registering); }
    void registerStorage() { registerStorageLocation(paths, document.config); }
    bool prepareHistory() {
        try {
            checkStorage(true);
            if (document.config.storageDirectory.isEmpty()) privateDirectory(QFileInfo(paths.historyDirectory).dir().absolutePath());
            if (!QFileInfo::exists(paths.historyDirectory + "/index.sqlite")) {
                Recorder initialize(recordingOptions(paths, document.config)); initialize.finish();
            }
            // Validate Replay ownership/schema before adding a location marker.
            usage = historyUsage(paths.historyDirectory);
            registerStorage();
            // Use a persistent read connection; no long-lived read transaction.
            indexReader = std::make_unique<IndexStatusReader>(paths.historyDirectory);
            lastRetained = usage.value("last_timestamp_ms").toInteger();
            progress = indexReader->status(); historyReady = true; storageError.clear();
            normalizeDeletionDirectory();
            return true;
        } catch (const std::exception &error) {
            historyReady = false; storageError = QString::fromUtf8(error.what()).left(500);
            indexReader.reset(); progress = {}; usage = {}; return false;
        }
    }
    void normalizeDeletionDirectory() {
        if (deletion.isEmpty() || deletion.value("directory").toString() == paths.historyDirectory) return;
        const QString oldPath = QFileInfo(deletion.value("directory").toString()).canonicalFilePath();
        const QString currentPath = QFileInfo(paths.historyDirectory).canonicalFilePath();
        // A migration compatibility symlink proves this is still the exact same
        // archive. Never reinterpret a deletion against a different directory.
        if (!oldPath.isEmpty() && oldPath == currentPath) {
            deletion["directory"] = paths.historyDirectory; saveIntent();
        }
    }
    void loseStorage(const QString &error) {
        stopMeetings(); stopIndex(); closeCapture(); indexReader.reset(); historyReady = false;
        progress = {}; usage = {}; storageError = error.left(500);
        storageForecastResult = {}; nextForecast = 0;
        gapStart = 0; gapReason.clear();
        transition("storage-unavailable", storageError);
    }
    void rememberConfig(const QByteArray &bytes) {
        if (bytes.isEmpty()) return;
        QSaveFile file(paths.stateDirectory + "/last-valid-config.toml");
        if (!file.open(QIODevice::WriteOnly)) fail("Cannot save last valid Replay settings");
        file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        if (file.write(bytes) != bytes.size() || !file.commit()) fail("Cannot commit last valid Replay settings");
    }

    void log(const QString &message) {
        const QString path = paths.stateDirectory + "/recording.log";
        if (QFileInfo(path).size() >= 64 * 1024) {
            QFile::remove(path + ".1"); QFile::rename(path, path + ".1");
        }
        QFile file(path);
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            file.write((QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " " + message.left(500) + '\n').toUtf8());
        }
    }
    void saveIntent() {
        writeJson(paths.stateDirectory + "/recording.json", {{"intent", intent}, {"indexing_paused", indexingPaused},
            {"last_retained_ms", lastRetained}, {"deletion", deletion},
            {"forecast_capture", forecastCaptureSettings()}, {"forecast_sample_after_ms", forecastSampleAfter}});
    }
    void closeCapture() {
        capture.reset();
        if (recorder) { try { recorder->finish(); } catch (...) {} recorder.reset(); }
    }
    void transition(const QString &next, const QString &detail) {
        if (next != "recording") {
            capture.reset();
            if (recorder) recorder->breakContinuity();
            nextCapture = 0;
            if (gapReason != next) {
                finishGap(); gapStart = now(); gapReason = next;
            }
        } else finishGap();
        if (state != next) {
            state = next; log("Recording state: " + next); saveIntent();
        }
        reason = detail;
    }
    void finishGap() {
        if (historyReady && gapStart && now() > gapStart) {
            try { checkStorage(); recordGap(paths.historyDirectory, gapStart, now(), gapReason); }
            catch (const std::exception &) { log("Could not retain a recording gap while history was unavailable."); }
        }
        gapStart = 0; gapReason.clear();
    }
    void stopIndex() {
        if (indexWorker.state() == QProcess::NotRunning) return;
        indexWorker.terminate();
        if (!indexWorker.waitForFinished(6500)) { indexWorker.kill(); indexWorker.waitForFinished(1000); }
        collectIndex(); workerStarted = false;
        const auto receipt = QJsonDocument::fromJson(indexOutput);
        if (receipt.isObject()) workerReceipt = receipt.object();
    }
    void collectIndex() {
        if (!indexWorker.isOpen()) return;
        indexOutput += indexWorker.readAllStandardOutput(); indexErrors += indexWorker.readAllStandardError();
        if (indexOutput.size() > 1024 * 1024) indexOutput = indexOutput.right(1024 * 1024);
        if (indexErrors.size() > 65536) indexErrors = indexErrors.right(65536);
    }
    void manageIndex(qint64 time) {
        indexWorker.waitForFinished(0); collectIndex();
        if (indexingPaused) { stopIndex(); return; }
        if (workerStarted && indexWorker.state() == QProcess::NotRunning) {
            workerStarted = false;
            workerReceipt = QJsonDocument::fromJson(indexOutput).object();
            if (indexWorker.exitStatus() != QProcess::NormalExit || indexWorker.exitCode() != 0) {
                indexError = "Index worker stopped; retained moments will retry after a short delay.";
                retryIndexAfter = time + 10000; log(indexError);
                QFile file(paths.stateDirectory + "/index-worker.stderr.tail.log");
                if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); file.write(indexErrors);
                }
            } else indexError.clear();
        }
        const qint64 ready = progress.value("ready").toInteger();
        if (progress.value("pending").toInteger() || ready != previousReady) indexLastWork = time;
        previousReady = ready;
        // Keep incremental OCR warm across nearby capture ticks, then release
        // its memory after a quiet period. One child owns the entire history.
        if (indexWorker.state() != QProcess::NotRunning) {
            if (!progress.value("pending").toInteger() && time - indexLastWork > 15000) stopIndex();
            return;
        }
        if (time < retryIndexAfter || progress.value("pending").toInteger() == 0) return;
        // The archive's lease also excludes an explicitly launched index CLI.
        if (progress.value("indexer_running").toBool()) return;
        const auto &c = document.config;
        QStringList args{"index", "--dir", paths.historyDirectory, "--follow", "--parent-pid", QString::number(getpid()),
            "--scheduler", "adaptive", "--ocr-mode", "incremental", "--ocr-max-wall-ms", "60000",
            "--ocr-cpu-percent", QString::number(c.activeCpuPercent), "--idle-cpu-percent", QString::number(c.idleCpuPercent),
            "--request-cpu-percent", QString::number(c.requestCpuPercent), "--pressure-cpu-percent", QString::number(c.pressureCpuPercent),
            "--idle-seconds", QString::number(c.idleSeconds), "--ocr-cpu-ceiling-percent", QString::number(c.cpuCeilingPercent),
            "--ocr-langs", c.ocrLanguages};
        indexOutput.clear(); indexErrors.clear();
        indexWorker.start(QCoreApplication::applicationFilePath(), args);
        if (!indexWorker.waitForStarted(2000)) { retryIndexAfter = time + 10000; indexError = "Cannot launch the index worker."; }
        else { workerStarted = true; indexLastWork = time; }
    }
    void collectMeetings() {
        if (!meetingWorker.isOpen()) return;
        meetingOutput += meetingWorker.readAllStandardOutput();
        // The worker only emits numeric diagnostics. Never pass source contents
        // or arbitrary exception messages through the coordinator status/log.
        meetingWorker.readAllStandardError();
        if (meetingOutput.size() > 65536) meetingOutput = meetingOutput.right(65536);
        qsizetype newline = -1;
        while ((newline = meetingOutput.indexOf('\n')) >= 0) {
            const auto line = meetingOutput.left(newline); meetingOutput.remove(0, newline + 1);
            const auto value = QJsonDocument::fromJson(line);
            if (!value.isObject()) continue;
            meetingProgress = value.object();
            if (!meetingProgress.value("source_available").toBool())
                meetingError = "The meetings folder is unavailable. Check its location in Settings.";
            else if (meetingProgress.value("limited").toBool())
                meetingError = "Meeting import reached a storage or import limit. Some sources may still be unindexed.";
            else meetingError.clear();
        }
    }
    void stopMeetings() {
        if (meetingWorker.state() != QProcess::NotRunning) {
            meetingWorker.terminate();
            if (!meetingWorker.waitForFinished(300)) { meetingWorker.kill(); meetingWorker.waitForFinished(1000); }
        }
        collectMeetings(); meetingWorkerStarted = false;
        meetingProgress["syncing"] = false;
    }
    void manageMeetings(qint64 time) {
        meetingWorker.waitForFinished(0); collectMeetings();
        if (time >= nextMeetingDetection) {
            meetingsAvailable = meetingRecorderAvailable(); nextMeetingDetection = time + 60000;
        }
        if (!document.config.meetingsEnabled || !meetingsAvailable || indexingPaused || !deletion.isEmpty()) {
            stopMeetings(); return;
        }
        if (meetingWorkerStarted && meetingWorker.state() == QProcess::NotRunning) {
            meetingWorkerStarted = false;
            meetingProgress["syncing"] = false;
            meetingError = "Meeting indexing stopped; Replay will retry shortly.";
            retryMeetingsAfter = time + 10000;
        }
        if (meetingWorker.state() != QProcess::NotRunning || time < retryMeetingsAfter) return;
        meetingOutput.clear(); meetingError.clear();
        meetingWorker.start(QCoreApplication::applicationFilePath(), {"daemon", "meeting-sync",
            "--history", paths.historyDirectory, "--source", replayMeetingsDirectory(document.config),
            "--retention-days", QString::number(document.config.retentionDays), "--follow",
            "--max-disk-mib", QString::number(document.config.maxDiskMiB),
            "--min-free-mib", QString::number(document.config.minFreeMiB),
            "--parent-pid", QString::number(getpid())});
        if (!meetingWorker.waitForStarted(1000)) {
            retryMeetingsAfter = time + 10000; meetingError = "Cannot launch meeting indexing.";
        } else { meetingWorkerStarted = true; meetingProgress["syncing"] = true; }
    }
    void reload() {
        // Explicit reload also refreshes optional dependency detection when
        // settings bytes are unchanged (for example after plugin removal).
        meetingsAvailable = meetingRecorderAvailable(); nextMeetingDetection = monotonicMs() + 60000;
        try {
            auto next = loadReplayConfig();
            if (next.original == document.original) { configError.clear(); return; }
            if (next.config.loginStartup != document.config.loginStartup) {
                const QString unit = QFileInfo(paths.configFile).dir().absolutePath() + "/../systemd/user/omarchy-replay.service";
                if (!QFileInfo::exists(unit) && next.config.loginStartup) fail("Install Replay's user service before enabling login startup");
                if (QFileInfo::exists(unit)) {
                    QProcess systemd; systemd.start("systemctl", {"--user", next.config.loginStartup ? "enable" : "disable", "omarchy-replay.service"});
                    if (!systemd.waitForFinished(3000) || systemd.exitCode() != 0) fail("Could not apply Replay login startup setting");
                }
            }
            const bool changedHistory = replayHistoryDirectory(next.config) != paths.historyDirectory;
            const bool changedCapture = changedHistory || next.config.output != document.config.output ||
                next.config.outputIdentity != document.config.outputIdentity || next.config.intervalSeconds != document.config.intervalSeconds;
            if (changedHistory && !deletion.isEmpty()) fail("Wait for the current history deletion before changing its folder");
            rememberConfig(next.original);
            stopMeetings(); stopIndex(); closeCapture();
            if (changedHistory) { finishGap(); indexReader.reset(); historyReady = false; }
            document = std::move(next);
            if (changedHistory) {
                paths.historyDirectory = replayHistoryDirectory(document.config);
                progress = {}; usage = {}; workerReceipt = {}; lastRetained = 0;
                previousReady = 0; nextIndexPoll = 0; nextStorageCheck = 0;
                meetingProgress = {};
                prepareHistory();
            }
            if (changedCapture) forecastSampleAfter = now();
            saveIntent();
            ++controlRevision;
            requestExclusions();
            if (environment) {
                auto options = environmentOptions(document.config); if (synthetic) options.exclusionMaskToken = QString(64, 'a');
                environment->configure(options);
            }
            nextMaintenance = 0; nextCapture = 0; nextForecast = 0; storageForecastResult = {};
            captureRetryCount = 0; configError.clear();
            nextMeetingDetection = 0; retryMeetingsAfter = 0; meetingError.clear();
            log("Validated recording settings reloaded.");
        } catch (const std::exception &error) { configError = QString::fromUtf8(error.what()).left(500); }
    }
    void maintenance(qint64 time) {
        if (!deletion.isEmpty()) {
            const auto result = deleteHistoryRange(paths.historyDirectory, deletion.value("from_ms").toInteger(),
                                                   deletion.value("to_ms").toInteger(), 1000, 128);
            if (!result.more) { deletion = {}; saveIntent(); }
            nextMaintenance = result.more ? time + 100 : time + 10000;
        } else {
            const qint64 cutoff = now() - qint64(document.config.retentionDays) * 86400000;
            const auto result = maintainHistory(paths.historyDirectory, cutoff, 1000, 128);
            nextMaintenance = result.more ? time + 100 : time + 10000;
        }
        usage = historyUsage(paths.historyDirectory);
        if (time >= nextForecast) {
            QStorageInfo disk(paths.historyDirectory); disk.refresh();
            StorageForecastOptions options;
            options.nowMs = now(); options.intervalSeconds = document.config.intervalSeconds;
            options.sampleAfterMs = forecastSampleAfter;
            options.retentionDays = document.config.retentionDays;
            options.diskBytes = usage.value("disk_bytes").toInteger();
            options.maxDiskBytes = document.config.maxDiskMiB * MiB;
            options.freeBytes = disk.isValid() && disk.isReady() ? disk.bytesAvailable() : -1;
            options.minFreeBytes = document.config.minFreeMiB * MiB;
            storageForecastResult = storageForecast(paths.historyDirectory, options);
            nextForecast = time + 60000;
        }
    }
    QJsonObject status() {
        // Read window metadata on demand for settings, without acquiring pixels or
        // polling an idle desktop when no viewer is asking for it.
        if (environment && intent != "running" && monotonicMs() >= nextDesktopStatus) {
            desktop = environment->snapshot(); nextDesktopStatus = monotonicMs() + 1000;
        }
        const auto &c = document.config;
        QJsonObject result{{"available", true}, {"running", true}, {"pid", qint64(getpid())}, {"intent", intent},
            {"state", state}, {"reason", reason}, {"config_error", configError}, {"output", c.output},
            {"exclusions_pending", exclusionsPending}, {"exclusions_error", exclusionError},
            {"progress", progress}, {"usage", usage}, {"retention_days", c.retentionDays}, {"max_disk_mib", c.maxDiskMiB},
            {"min_free_mib", c.minFreeMiB}, {"interval_seconds", c.intervalSeconds},
            {"login_startup", c.loginStartup}, {"indexing_paused", indexingPaused}, {"index_error", indexError},
            {"indexing", indexWorker.state() != QProcess::NotRunning}, {"index_controller_pid", qint64(indexWorker.processId())},
            {"visible_windows", desktop.visibleWindows}, {"excluded_apps", QJsonArray::fromStringList(desktop.excludedApps)},
            {"compositor_instance", desktop.compositorInstance}, {"history_directory", paths.historyDirectory},
            {"storage_available", historyReady}, {"storage_error", storageError},
            {"capture_attempts", attempts}, {"retained_this_run", retainedThisRun}, {"last_retained_ms", lastRetained},
            {"deleting", !deletion.isEmpty()}, {"synthetic", synthetic}};
        result["index_policy"] = QJsonObject{{"scheduler", "adaptive"}, {"ocr_mode", "incremental"},
            {"ocr_cpu_percent", c.activeCpuPercent}, {"idle_cpu_percent", c.idleCpuPercent},
            {"request_cpu_percent", c.requestCpuPercent}, {"pressure_cpu_percent", c.pressureCpuPercent},
            {"idle_seconds", c.idleSeconds}, {"ocr_cpu_ceiling_percent", c.cpuCeilingPercent},
            {"ocr_langs", c.ocrLanguages}};
        if (indexWorker.state() != QProcess::NotRunning)
            result["worker_policy"] = ownedIndexWorkerPolicy(paths.historyDirectory, indexWorker.processId());
        else if (!workerReceipt.isEmpty()) result["worker_resources"] = workerReceipt.value("resources");
        result["storage_forecast"] = storageForecastResult;
        auto meetings = meetingProgress;
        meetings["available"] = meetingsAvailable;
        meetings["enabled"] = c.meetingsEnabled;
        meetings["directory"] = replayMeetingsDirectory(c);
        meetings["paused"] = indexingPaused;
        meetings["worker_running"] = meetingWorker.state() != QProcess::NotRunning;
        meetings["worker_pid"] = qint64(meetingWorker.processId());
        meetings["error"] = meetingError;
        result["meetings"] = meetings;
        return result;
    }
    void requestExclusions() {
        maskToken.clear();
        exclusionsPending = !synthetic;
        nextExclusionAttempt = 0; exclusionFailures = 0; exclusionError.clear();
        ++exclusionRevision;
    }
    bool applyExclusions() {
        const QByteArray original = document.original;
        const QString instance = desktop.compositorInstance, display = desktop.waylandDisplay;
        const quint64 revision = exclusionRevision;
        const QString helper = QDir(QFileInfo(QCoreApplication::applicationFilePath()).dir().absolutePath())
                                   .absoluteFilePath("../scripts/install_capture_exclusions.py");
        if (!QFileInfo::exists(helper)) fail("Replay capture-exclusion installer is missing");
        QProcess process;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("HYPRLAND_INSTANCE_SIGNATURE", instance);
        env.insert("WAYLAND_DISPLAY", display);
        process.setProcessEnvironment(env);
        process.start("python3", {"-B", helper, "--config", paths.stateDirectory + "/last-valid-config.toml",
            "--config-home", QFileInfo(paths.configFile).dir().absolutePath() + "/..", "--instance", instance});
        // Keep controls responsive while the transactional installer reloads or rolls back.
        // Its bounded subprocess calls, including rollback, can take up to 35 seconds.
        QElapsedTimer deadline; deadline.start();
        while (!process.waitForFinished(25) && process.state() != QProcess::NotRunning && deadline.elapsed() < 40000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (process.state() != QProcess::NotRunning) {
            process.kill(); process.waitForFinished(1000); fail("Capture exclusion installer timed out; inspect Hyprland configuration before recording");
        }
        const auto output = process.readAllStandardOutput();
        const auto receipt = QJsonDocument::fromJson(output).object();
        // Controls run while the installer is in flight. A newer accepted
        // configuration must get its own receipt before any capture resumes.
        if (revision != exclusionRevision || original != document.original) return false;
        const auto expected = QString::fromLatin1(QCryptographicHash::hash(original, QCryptographicHash::Sha256).toHex());
        if (process.exitCode() != 0 || output.size() > 65536 || !receipt.value("validated").toBool() ||
            receipt.value("config_sha256").toString() != expected ||
            receipt.value("compositor_instance").toString() != instance ||
            !QRegularExpression("^[0-9a-f]{64}$").match(receipt.value("mask_token").toString()).hasMatch())
            fail("Capture exclusions could not be verified. " + QString::fromUtf8(process.readAllStandardError().right(1000)).trimmed());
        maskToken = receipt.value("mask_token").toString();
        auto options = environmentOptions(document.config); options.exclusionMaskToken = maskToken;
        environment->configure(options); desktop = environment->snapshot();
        maskInstance = desktop.compositorInstance; maskConfigGeneration = desktop.configGeneration;
        if (desktop.compositorInstance != instance) fail("Compositor changed while capture exclusions were being applied");
        if (desktop.reason == "exclusions_unverified") fail("Compositor did not load the expected capture exclusions");
        return true;
    }
    void reconcileExclusions(qint64 time) {
        if (!exclusionsPending || synthetic || time < nextExclusionAttempt) return;
        const quint64 revision = exclusionRevision;
        try {
            desktop = environment->snapshot();
            if (desktop.compositorInstance.isEmpty()) fail("Waiting for Hyprland before applying capture exclusions");
            if (!applyExclusions()) return;
            exclusionsPending = false; exclusionError.clear(); exclusionFailures = 0;
            nextCapture = 0;
        } catch (const std::exception &error) {
            // A reload during the installer already queued a fresh attempt.
            if (revision != exclusionRevision) return;
            exclusionError = QString::fromUtf8(error.what()).left(500);
            nextExclusionAttempt = monotonicMs() + std::min<qint64>(60000, qint64(5000) << std::min(exclusionFailures, 4));
            exclusionFailures = std::min(exclusionFailures + 1, 4);
        }
    }
    QJsonObject control(const QJsonObject &request) {
        const QString action = request.value("action").toString();
        if (action == "status") return status();
        // The shell indicator needs only the existing state. Do not enumerate
        // windows, inspect history or refresh worker diagnostics for each poll.
        if (action == "bar-status") return {{"running", true}, {"pid", qint64(getpid())},
            {"intent", intent}, {"state", state}, {"reason", reason},
            {"config_error", configError}, {"exclusions_error", exclusionError}};
        if (action == "debug-status") return debugStatus();
        if (action == "debug-stop") { stopDebug(); return debugStatus(); }
        if (action == "debug-start") {
            const auto seconds = request.value("seconds").toInteger();
            if (seconds < 1 || seconds > 300) fail("Debug duration must be between 1 and 300 seconds");
            expireDebug();
            if (debugUntil) fail("A bounded debug session is already active");
            debugStarted = monotonicMs(); debugStartedWall = now(); debugUntil = debugStarted + seconds * 1000;
            debugAttempts = attempts; debugRetained = retainedThisRun;
            if (environment) environment->setDiagnosticsEnabled(true);
            return debugStatus();
        }
        ++controlRevision;
        if (action == "start" || action == "resume") {
            if (document.config.output.isEmpty() && !synthetic) fail("Choose a display in Replay settings before recording");
            intent = "running"; nextCapture = 0; captureRetryCount = 0;
        } else if (action == "pause") intent = "paused";
        else if (action == "stop") intent = "stopped";
        else if (action == "shutdown") { intent = "stopped"; shuttingDown = true; stopMeetings(); }
        else if (action == "index-pause") { indexingPaused = true; stopMeetings(); stopIndex(); }
        else if (action == "index-resume") { indexingPaused = false; retryIndexAfter = 0; retryMeetingsAfter = 0; }
        else if (action == "reload") reload();
        else if (action == "delete-recent") {
            const qint64 seconds = request.value("seconds").toInteger();
            if (!request.value("confirmed").toBool() || seconds < 1 || seconds > 86400)
                fail("Deleting history requires confirmation and an interval of 1 to 86400 seconds");
            if (!deletion.isEmpty()) fail("A history deletion is already in progress");
            if (!historyReady) fail("The history folder is unavailable");
            checkStorage();
            stopMeetings(); stopIndex(); if (recorder) recorder->breakContinuity();
            deletion = {{"from_ms", now() - seconds * 1000}, {"to_ms", now() + 1}, {"directory", paths.historyDirectory}};
            nextMaintenance = 0; nextForecast = 0; storageForecastResult = {};
        } else fail("Unknown recording control");
        if (intent != "running") transition(intent, intent == "paused" ? "Recording is paused until you resume." : "Recording is stopped.");
        saveIntent(); return status();
    }
    void tick(const std::function<bool()> &stopRequested) {
        if (tickActive || stopRequested()) return;
        expireDebug();
        QScopedValueRollback<bool> tickGuard(tickActive, true);
        const qint64 time = monotonicMs();
        if (time >= nextConfig) {
            const QFileInfo file(paths.configFile);
            const qint64 modified = file.lastModified().toMSecsSinceEpoch(), size = file.size();
            if (modified != configModified || size != configSize) {
                reload(); configModified = modified; configSize = size;
            }
            nextConfig = time + 2000;
        }
        // Changing an exclusion also affects ordinary screenshots. Reconcile
        // even while stopped/paused or the history disk is disconnected, then
        // do no further background mask work until another change or retry.
        reconcileExclusions(time);
        if (shuttingDown || stopRequested()) return;
        if (time >= nextStorageCheck) {
            if (!historyReady) prepareHistory();
            else try { checkStorage(); } catch (const std::exception &error) { loseStorage(QString::fromUtf8(error.what())); }
            nextStorageCheck = time + 1000;
        }
        if (!historyReady) { transition("storage-unavailable", storageError); return; }
        if (!deletion.isEmpty() && deletion.value("directory").toString() != paths.historyDirectory) {
            transition("storage-unavailable", "A pending deletion belongs to the previous history folder. Select that folder to finish it."); return;
        }
        if (time >= nextMaintenance) maintenance(time);
        if (time >= nextIndexPoll) { progress = indexReader->status(); nextIndexPoll = time + 1000; }
        manageMeetings(time);
        if (deletion.isEmpty()) manageIndex(time);
        if (intent != "running") { transition(intent, intent == "paused" ? "Recording is paused until you resume." : "Recording is stopped."); return; }
        if (time < nextCapture) return;
        if (!environment) { desktop.captureAllowed = true; desktop.reason = "ready"; desktop.generation = 1; }
        else desktop = environment->snapshot();
        if (!synthetic && !desktop.compositorInstance.isEmpty() && (desktop.reason == "exclusions_unverified" ||
            (!maskToken.isEmpty() && (desktop.compositorInstance != maskInstance || desktop.configGeneration != maskConfigGeneration)))) {
            if (!exclusionsPending) { exclusionsPending = true; nextExclusionAttempt = 0; }
            reconcileExclusions(monotonicMs());
        }
        if (exclusionsPending) {
            transition("exclusions_unverified", exclusionError.isEmpty() ? "Waiting for capture exclusions to be applied." : exclusionError);
            nextCapture = std::max(monotonicMs() + 1000, nextExclusionAttempt);
            return;
        }
        if (intent != "running" || shuttingDown || stopRequested()) return;
        if (!desktop.captureAllowed) { transition(desktop.reason, desktop.detail); nextCapture = time + 1000; return; }
        if (!deletion.isEmpty()) { transition("deleting", "Removing the selected history interval."); return; }
        if (!synthetic && configError.isEmpty() && document.config.outputIdentity.isEmpty() && !desktop.outputIdentity.isEmpty()) {
            auto pinned = document.config; pinned.outputIdentity = desktop.outputIdentity;
            saveReplayConfig(pinned, document.original); document = loadReplayConfig();
            rememberConfig(document.original);
            auto options = environmentOptions(document.config); options.exclusionMaskToken = maskToken;
            environment->configure(options); desktop = environment->snapshot();
            if (!desktop.captureAllowed) { transition(desktop.reason, desktop.detail); return; }
        }
        if (time < nextCapture) return;
        ++attempts;
        try {
            checkStorage();
            const quint64 revision = controlRevision;
            if (!recorder) recorder = std::make_unique<Recorder>(recordingOptions(paths, document.config));
            QImage image;
            if (synthetic) image = fixtureFrame(int(attempts % fixtureFrameCount()), QSize(960, 540));
            else {
                if (captureInstance != desktop.compositorInstance || captureDisplay != desktop.waylandDisplay) capture.reset();
                if (!capture) {
                    if (desktop.waylandDisplay.isEmpty()) fail("Current compositor display socket is unavailable");
                    capture = std::make_unique<WaylandCapture>(document.config.output, desktop.waylandDisplay);
                    captureInstance = desktop.compositorInstance; captureDisplay = desktop.waylandDisplay;
                }
                image = capture->capture(2000);
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (revision != controlRevision || intent != "running" || shuttingDown || stopRequested() || !deletion.isEmpty()) return;
            const auto after = environment ? environment->snapshot() : desktop;
            if (!after.captureAllowed || after.generation != desktop.generation) {
                desktop = after; transition(after.captureAllowed ? "desktop-changed" : after.reason,
                    after.captureAllowed ? "Desktop changed during capture; waiting for the next moment." : after.detail);
                // Discard uncertain pixels without spinning through new capture
                // connections. Repeated changes back off up to the capture interval.
                const qint64 interval = qint64(document.config.intervalSeconds * 1000);
                const qint64 delay = std::min(interval, qint64(250) << std::min(captureRetryCount, 5));
                captureRetryCount = std::min(captureRetryCount + 1, 5);
                nextCapture = monotonicMs() + delay;
                return;
            }
            checkStorage();
            const qint64 capturedAt = now();
            const auto added = recorder->addFrame(image, capturedAt);
            if (added.stored || added.duplicate) { ++retainedThisRun; lastRetained = capturedAt; }
            captureRetryCount = 0;
            transition("recording", "Recording the selected display.");
            // Missed ticks are a gap, never a burst of captures after wake.
            nextCapture = monotonicMs() + qint64(document.config.intervalSeconds * 1000);
        } catch (const RollingStorageUnavailable &error) {
            usage = historyUsage(paths.historyDirectory); nextForecast = 0; nextMaintenance = 0;
            transition(error.retryable ? "storage-cleanup" : "storage-full", QString::fromUtf8(error.what()));
            nextCapture = monotonicMs() + std::max<qint64>(qint64(document.config.intervalSeconds * 1000), 1000);
        } catch (const std::exception &error) {
            const QString message = QString::fromUtf8(error.what());
            try { checkStorage(); }
            catch (const std::exception &storageFailure) {
                loseStorage(QString::fromUtf8(storageFailure.what())); nextCapture = monotonicMs() + 1000; return;
            }
            closeCapture();
            const bool storageBlocked = message.contains("disk", Qt::CaseInsensitive) || message.contains("budget", Qt::CaseInsensitive);
            if (storageBlocked) {
                usage = historyUsage(paths.historyDirectory); nextForecast = 0; nextMaintenance = 0;
            }
            transition(storageBlocked ? "storage-full" : "capture-error", message.left(500));
            nextCapture = monotonicMs() + 5000;
        }
    }
};

int syncMeetingSources(const QString &history, const QString &source, int retentionDays,
                       quint64 maxDiskBytes, quint64 minFreeBytes, bool follow,
                       qint64 parentPid, const std::function<bool()> &stopRequested) {
    if (parentPid && (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parentPid))
        fail("Meeting index coordinator is no longer running");
    // This process owns no recording or transcription work. A distinct process
    // bounds shutdown even if a source filesystem stalls on a read.
    setpriority(PRIO_PROCESS, 0, 15);
    const auto canceled = [&] { return stopRequested() || (parentPid && getppid() != parentPid); };
    struct stat archive{};
    if (lstat(QFile::encodeName(history).constData(), &archive) != 0 || !S_ISDIR(archive.st_mode) ||
        archive.st_uid != getuid() || (archive.st_mode & 0077)) fail("Meeting indexing requires a private history folder");
    Lease lease(history + "/.meeting-index.lock", "meeting indexer");
    MeetingImporter importer(history, source, canceled, maxDiskBytes, minFreeBytes);
    bool settling = true;
    bool limited = false;
    int scanned = 0, imported = 0, updated = 0, removed = 0;
    while (!canceled()) {
        const auto timestamp = now();
        const auto sync = importer.sync(timestamp, timestamp - qint64(retentionDays) * 86400000);
        scanned += sync.scanned; imported += sync.imported; updated += sync.updated; removed += sync.removed;
        limited = limited || sync.limited;
        if (canceled() || sync.canceled) break;
        const bool syncing = sync.more || (settling && sync.sourceAvailable);
        QJsonObject progress{{"syncing", syncing}, {"source_available", sync.sourceAvailable},
            {"count", listMeetings(history, 1).totalMatches}, {"scanned", scanned},
            {"imported", imported}, {"updated", updated}, {"removed", removed},
            {"limited", limited}, {"last_sync_ms", timestamp}};
        const QByteArray bytes = QJsonDocument(progress).toJson(QJsonDocument::Compact) + '\n';
        if (std::fwrite(bytes.constData(), 1, bytes.size(), stdout) != size_t(bytes.size()) || std::fflush(stdout) != 0)
            return 0;
        int delay = 100;
        if (!sync.more) {
            if (settling && sync.sourceAvailable) { settling = false; delay = 1000; }
            else {
                if (!follow) break;
                settling = true; delay = 60000;
                scanned = imported = updated = removed = 0;
                limited = false;
            }
        }
        // poll is interrupted by SIGTERM/parent death without a recurring wake
        // timer. A quiet optional integration sleeps once for the full minute.
        const qint64 wakeAt = monotonicMs() + delay;
        while (!canceled()) {
            const qint64 remaining = wakeAt - monotonicMs();
            if (remaining <= 0) break;
            ::poll(nullptr, 0, int(remaining));
        }
    }
    return 0;
}
} // namespace

int runRecordingService(const std::function<bool()> &stopRequested, bool synthetic, const QString &syntheticEnvironment) {
    const auto paths = replayPaths(); privateDirectory(paths.runtimeDirectory);
    Lease lease(paths.runtimeDirectory + "/coordinator.lock");
    Coordinator coordinator(synthetic, syntheticEnvironment);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    const QString socketPath = paths.runtimeDirectory + "/control.sock";
    QLocalServer::removeServer(socketPath);
    if (!server.listen(socketPath)) fail("Cannot listen on Replay control socket");
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&] {
        while (auto *socket = server.nextPendingConnection()) {
            auto *deadline = new QTimer(socket); deadline->setSingleShot(true); deadline->start(3000);
            QObject::connect(deadline, &QTimer::timeout, socket, [socket] { socket->abort(); socket->deleteLater(); });
            auto bytes = std::make_shared<QByteArray>();
            QObject::connect(socket, &QLocalSocket::readyRead, socket, [&, socket, bytes, deadline] {
                *bytes += socket->readAll();
                if (bytes->size() > 65536) { socket->abort(); return; }
                if (!bytes->contains('\n')) return;
                deadline->stop(); QJsonObject reply;
                try {
                    const auto request = QJsonDocument::fromJson(bytes->left(bytes->indexOf('\n')));
                    if (!request.isObject()) fail("Invalid Replay control request");
                    reply = coordinator.control(request.object());
                } catch (const std::exception &error) { reply = {{"error", QString::fromUtf8(error.what()).left(1000)}}; }
                socket->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                socket->flush();
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    while (!stopRequested() && !coordinator.shuttingDown) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        try { coordinator.tick(stopRequested); }
        catch (const std::exception &error) {
            coordinator.transition("error", QString::fromUtf8(error.what()).left(500));
            coordinator.nextMaintenance = monotonicMs() + 5000;
        }
        QThread::msleep(50);
    }
    coordinator.finishGap(); coordinator.saveIntent(); coordinator.stopMeetings(); coordinator.stopIndex(); coordinator.closeCapture();
    coordinator.log("Recording coordinator stopped."); server.close();
    return 0;
}

int recordingCommand(const QStringList &arguments, const std::function<bool()> &stopRequested) {
    QCommandLineParser parser; parser.setApplicationDescription("Replay shared history and background recorder.");
    parser.addHelpOption(); parser.addPositionalArgument("action", "run | init | paths | status | start | pause | resume | stop | shutdown | index-pause | index-resume | reload | delete-recent | debug");
    parser.addOptions({{"synthetic", "Use fictional images for an isolated test; never read a display."},
        {"synthetic-environment", "Synthetic lifecycle observations; requires --synthetic.", "path"},
        {"output", "Explicit display selection when initializing settings.", "name"},
        {"seconds", "Recent deletion interval, or debug duration (1–300 seconds; default 30).", "seconds"},
        {"confirmed", "Confirm deletion of the requested interval."},
        {"history", "Internal meeting index archive.", "path"},
        {"source", "Internal meeting source folder.", "path"},
        {"retention-days", "Internal meeting age window.", "days"},
        {"max-disk-mib", "Internal meeting archive allowance.", "mib"},
        {"min-free-mib", "Internal meeting free-disk reserve.", "mib"},
        {"parent-pid", "Internal meeting coordinator process.", "pid"},
        {"follow", "Internal meeting source reconciliation loop."}});
    parser.process(arguments);
    const auto positional = parser.positionalArguments();
    if (positional.size() != 1) fail("Choose exactly one recording service action");
    const auto action = positional.first();
    if (parser.isSet("synthetic") && action != "run") fail("Synthetic mode is only valid for daemon run");
    if (parser.isSet("synthetic-environment") && !parser.isSet("synthetic")) fail("Synthetic lifecycle input requires --synthetic");
    if (parser.isSet("output") && action != "init") fail("Display selection is only valid for daemon init");
    if (parser.isSet("seconds") && action != "delete-recent" && action != "debug") fail("Duration requires delete-recent or debug");
    if (parser.isSet("confirmed") && action != "delete-recent") fail("Deletion confirmation requires delete-recent");
    for (const auto &option : {"history", "source", "retention-days", "max-disk-mib", "min-free-mib", "parent-pid", "follow"})
        if (parser.isSet(option) && action != "meeting-sync") fail("Meeting worker options require meeting-sync");
    if (action == "meeting-sync") {
        bool validDays = false, validPid = true, validMax = false, validFree = false;
        const int days = parser.value("retention-days").toInt(&validDays);
        const qint64 maximum = parser.value("max-disk-mib").toLongLong(&validMax);
        const qint64 reserve = parser.value("min-free-mib").toLongLong(&validFree);
        const qint64 parent = parser.isSet("parent-pid") ? parser.value("parent-pid").toLongLong(&validPid) : 0;
        const QString history = parser.value("history"), source = parser.value("source");
        if (!validDays || days < 1 || days > 3650 || !validPid || parent < 0 || parent > INT_MAX ||
            !validMax || maximum < 64 || maximum > 1048576 || !validFree || reserve < 0 || reserve > 1048576 ||
            (parser.isSet("parent-pid") && parent < 1) || (parser.isSet("follow") && !parent) ||
            !QDir::isAbsolutePath(history) || QDir::cleanPath(history) != history || history == "/" ||
            !QDir::isAbsolutePath(source) || QDir::cleanPath(source) != source || source == "/")
            fail("Meeting worker requires absolute folders, a valid retention window and a live parent for follow mode");
        return syncMeetingSources(history, source, days, quint64(maximum) * MiB, quint64(reserve) * MiB,
                                  parser.isSet("follow"), parent, stopRequested);
    }
    if (action == "run") return runRecordingService(stopRequested, parser.isSet("synthetic"), parser.value("synthetic-environment"));
    QJsonObject result;
    if (action == "paths") {
        const auto config = resolveReplayConfig();
        const auto paths = replayPaths();
        result = {{"config", paths.configFile}, {"history", replayHistoryDirectory(config.document.config)},
            {"default_history", paths.historyDirectory}, {"state", paths.stateDirectory},
            {"cache", paths.cacheDirectory}, {"runtime", paths.runtimeDirectory},
            {"config_error", config.configError}, {"using_last_valid_config", config.usingLastValidConfig}};
    } else if (action == "init") {
        auto config = loadReplayConfig();
        if (parser.isSet("output")) { config.config.output = parser.value("output"); config.config.outputIdentity.clear(); }
        if (!config.exists || parser.isSet("output")) saveReplayConfig(config.config, config.original);
        auto paths = replayPaths(); paths.historyDirectory = replayHistoryDirectory(config.config);
        checkStorageLocation(paths, config.config, true);
        if (!QFileInfo::exists(paths.historyDirectory + "/index.sqlite")) {
            Recorder initialize(recordingOptions(paths, config.config)); initialize.finish();
        }
        if (!config.config.storageDirectory.isEmpty()) {
            historyUsage(paths.historyDirectory);
            privateDirectory(paths.stateDirectory);
            registerStorageLocation(paths, config.config);
        }
        result = {{"config", paths.configFile}, {"history", paths.historyDirectory}, {"recording_started", false}};
    } else if (action == "status") result = recordingServiceStatus();
    else if (action == "debug") {
        bool valid = true;
        const qint64 seconds = parser.isSet("seconds") ? parser.value("seconds").toLongLong(&valid) : 30;
        if (!valid || seconds < 1 || seconds > 300) fail("Debug duration must be between 1 and 300 seconds");
        controlRecordingService("debug-start", {{"seconds", seconds}});
        QElapsedTimer timer; timer.start();
        while (!stopRequested() && timer.elapsed() < seconds * 1000) QThread::msleep(50);
        result = controlRecordingService("debug-stop");
    }
    else {
        QJsonObject options;
        if (action == "delete-recent") {
            bool valid = false; const auto seconds = parser.value("seconds").toLongLong(&valid);
            if (!valid || seconds < 1 || seconds > 86400 || !parser.isSet("confirmed")) fail("Use delete-recent --seconds 300 --confirmed to delete the last five minutes");
            options = {{"seconds", seconds}, {"confirmed", true}};
        }
        result = controlRecordingService(action, options);
    }
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Indented);
    std::fwrite(bytes.constData(), 1, bytes.size(), stdout); return 0;
}
} // namespace replay
