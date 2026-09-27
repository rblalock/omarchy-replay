#include "index_service.h"
#include "recorder.h"
#include "selection_ocr.h"

#include <QCoreApplication>
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
#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace replay {
namespace {
constexpr int MaxJson = 65536;
constexpr int HeartbeatMs = 2000;
[[noreturn]] void fail(const QString &message) { throw std::runtime_error(message.toStdString()); }
qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

QString datasetPath(const QString &directory) {
    const QString path = QFileInfo(directory).canonicalFilePath();
    if (path.isEmpty() || !QFileInfo(path).isDir() || !QFileInfo(QDir(path).filePath("index.sqlite")).isFile())
        fail("Indexing requires an existing saved history.");
    return path;
}

bool privateDirectory(const QString &path, bool create) {
    struct stat st{};
    const auto bytes = QFile::encodeName(path);
    if (lstat(bytes.constData(), &st) != 0) {
        if (errno != ENOENT || !create) return false;
        if (::mkdir(bytes.constData(), 0700) != 0 && errno != EEXIST) fail("Cannot create private indexing runtime directory.");
        if (lstat(bytes.constData(), &st) != 0) fail("Cannot inspect indexing runtime directory.");
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077))
        fail("Indexing runtime directory must be owned by you, private, and not a symlink.");
    return true;
}

QString runtimeDirectory(const QString &directory, bool create) {
    QString base = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (base.isEmpty()) base = QString("/run/user/%1").arg(geteuid());
    if (!privateDirectory(base, false)) {
        base = QString("/dev/shm/omarchy-replay-%1").arg(geteuid());
        if (!privateDirectory(base, create)) return {};
    }
    const QString app = QDir(base).filePath("replay");
    if (!privateDirectory(app, create)) return {};
    const auto digest = QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString result = QDir(app).filePath(QString::fromLatin1(digest));
    return privateDirectory(result, create) ? result : QString();
}

QJsonObject readObject(const QString &path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return {};
        fail("Cannot read indexing state safely.");
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077) || st.st_size > MaxJson) {
        ::close(fd); fail("Indexing state must be a bounded, private file owned by you.");
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) { ::close(fd); fail("Cannot read indexing state."); }
    const auto bytes = file.read(MaxJson + 1);
    if (bytes.size() > MaxJson) fail("Indexing state is too large.");
    const auto json = QJsonDocument::fromJson(bytes);
    if (!json.isObject()) fail("Indexing state is invalid JSON.");
    return json.object();
}

void writeObject(const QString &path, const QJsonObject &object) {
    struct stat st{};
    if (lstat(QFile::encodeName(path).constData(), &st) == 0 &&
        (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)))
        fail("Refusing to replace unsafe indexing state.");
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaxJson) fail("Indexing state is too large.");
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) fail("Cannot write indexing state.");
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit()) fail("Cannot save indexing state.");
}

class Lease {
    int fd_ = -1;
public:
    explicit Lease(const QString &path, bool create = true) {
        fd_ = ::open(QFile::encodeName(path).constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
        if (fd_ < 0) {
            if (!create && errno == ENOENT) return;
            fail("Cannot open indexing lease.");
        }
        struct stat st{};
        if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0077)) {
            ::close(fd_); fd_ = -1; fail("Indexing lease must be private and owned by you.");
        }
    }
    ~Lease() { if (fd_ >= 0) ::close(fd_); }
    bool acquire() { return fd_ >= 0 && flock(fd_, LOCK_EX | LOCK_NB) == 0; }
    bool exists() const { return fd_ >= 0; }
};

bool leaseHeld(const QString &path) {
    Lease lease(path, false);
    return lease.exists() && !lease.acquire();
}

QJsonObject checkedPolicy(const QJsonObject &source) {
    if (source.isEmpty()) return {};
    QJsonObject result;
    const QString scheduler = source.value("scheduler").toString("fixed");
    const QString mode = source.value("ocr_mode").toString("incremental");
    if ((scheduler != "fixed" && scheduler != "adaptive") || (mode != "full" && mode != "incremental" && mode != "regions"))
        fail("Saved indexing policy has an invalid scheduler or OCR mode.");
    result["scheduler"] = scheduler; result["ocr_mode"] = mode;
    const auto number = [&](const char *key, double fallback, double low, double high, bool integer = false) {
        const auto value = source.value(key);
        if (!value.isUndefined() && !value.isDouble()) fail("Saved indexing policy contains an invalid number.");
        const double n = value.toDouble(fallback);
        if (!std::isfinite(n) || n < low || n > high || (integer && n != std::floor(n)))
            fail("Saved indexing policy contains an out-of-range value.");
        result[key] = n;
    };
    number("ocr_cpu_percent", 10, scheduler == "adaptive" ? 1 : 0, 100);
    const double cpu = result["ocr_cpu_percent"].toDouble();
    if (cpu > 0 && cpu < 1) fail("OCR allowance must be zero or at least one percent.");
    number("ocr_cpu_ceiling_percent", 0, 0, 100);
    const double ceiling = result["ocr_cpu_ceiling_percent"].toDouble();
    if (ceiling > 0 && ceiling < 1) fail("Worker ceiling must be zero or at least one percent.");
    const auto reuse = source.value("ocr_reuse");
    if (!reuse.isUndefined() && !reuse.isBool()) fail("Saved OCR reuse setting must be boolean.");
    result["ocr_reuse"] = reuse.toBool(false);
    number("ocr_max_wall_ms", 60000, 1, 60000, true);
    number("ocr_max_height", 0, 0, 8192, true);
    const int height = result["ocr_max_height"].toInt();
    if (height > 0 && height < 256) fail("Saved OCR height must be zero or at least 256.");
    const auto langs = source.value("ocr_langs");
    if (!langs.isUndefined() && !langs.isString()) fail("Saved OCR languages are invalid.");
    const QString languages = langs.toString().trimmed();
    if (!languages.isEmpty() && !validOcrLanguages(languages))
        fail("Saved OCR languages must be Tesseract language names joined by +, such as eng+fra.");
    result["ocr_langs"] = languages.isEmpty() ? QString("eng") : languages;
    if (scheduler == "adaptive") {
        number("idle_seconds", 60, 1, 3600, true);
        number("idle_cpu_percent", 40, 1, 100);
        number("request_cpu_percent", 30, 1, 100);
        number("pressure_cpu_percent", 10, 1, 100);
    }
    const auto model = source.value("ocr_data_path");
    if (!model.isUndefined() && !model.isString()) fail("Saved OCR model path is invalid.");
    if (!model.toString().isEmpty()) {
        const QString path = QFileInfo(model.toString()).canonicalFilePath();
        if (path.isEmpty()) fail("Saved OCR model is unavailable.");
        for (const QString &language : result["ocr_langs"].toString().split('+')) {
            const QFileInfo file(QDir(path).filePath(language + ".traineddata"));
            if (!file.isFile() || !file.isReadable() || file.size() == 0)
                fail(QString("Saved OCR model must contain a readable, nonempty %1.traineddata file").arg(language));
        }
        result["ocr_data_path"] = path;
    }
    return result;
}

QString statePath(const QString &directory) { return QDir(directory).filePath(".index-service.json"); }
QJsonObject savedPolicySource(const QString &path) {
    const auto state = readObject(statePath(path));
    if (state.contains("policy")) return state.value("policy").toObject();
    if (QFileInfo(path).fileName() != "dataset") return {};
    const auto trial = readObject(QDir(QFileInfo(path).path()).filePath("trial.json"));
    return trial.value("config").toObject();
}

QStringList policyArguments(const QJsonObject &policy) {
    QStringList result;
    for (auto it = policy.begin(); it != policy.end(); ++it) {
        if (it.key() == "ocr_reuse") {
            if (it->toBool()) result << "--ocr-reuse";
            continue;
        }
        QString key = it.key(); key.replace('_', '-');
        result << "--" + key << (it->isString() ? it->toString() : QString::number(it->toDouble(), 'g', 15));
    }
    return result;
}

void operationalLog(const QString &runtime, const QString &message) {
    // Only fixed service messages enter this log, never worker OCR output.
    const QString path = QDir(runtime).filePath("service-log.json");
    QString text = readObject(path).value("text").toString();
    text += QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + " " + message.left(300) + '\n';
    writeObject(path, {{"text", text.right(8192)}});
}

QJsonObject numericProgress(const QJsonObject &index) {
    QJsonObject result;
    for (const auto *key : {"pending", "ready", "failed", "disabled"}) result[key] = index.value(key).toInteger();
    return result;
}
}

QJsonObject savedIndexPolicy(const QString &directory) {
    return checkedPolicy(savedPolicySource(datasetPath(directory)));
}

bool indexServiceEnabled(const QString &directory) {
    return readObject(statePath(datasetPath(directory))).value("enabled").toBool();
}

QJsonObject indexServiceStatus(const QString &directory) {
    const QString path = datasetPath(directory);
    const auto intent = readObject(statePath(path));
    const QString runtime = runtimeDirectory(path, false);
    QJsonObject result = runtime.isEmpty() ? QJsonObject{} : readObject(QDir(runtime).filePath("service.json"));
    const bool running = !runtime.isEmpty() && leaseHeld(QDir(runtime).filePath("service.lock"));
    const bool worker = leaseHeld(QDir(path).filePath(".indexer.lock"));
    const bool enabled = intent.value("enabled").toBool(), paused = intent.value("paused").toBool();
    QJsonObject policy;
    QString policyError;
    try { policy = savedIndexPolicy(path); }
    catch (const std::exception &error) { policyError = QString::fromUtf8(error.what()).left(500); }
    result["configured"] = !policy.isEmpty(); result["policy"] = policy;
    result["enabled"] = enabled; result["paused"] = paused; result["running"] = running;
    result["worker_running"] = worker;
    result["external_worker"] = worker && (!running || result.value("worker_pid").toInteger() <= 0);
    const qint64 now = nowMs(), heartbeat = result.value("heartbeat_ms").toInteger();
    result["heartbeat_age_ms"] = heartbeat ? std::max<qint64>(0, now - heartbeat) : -1;
    const auto activity = runtime.isEmpty() ? QJsonObject{} : readObject(QDir(runtime).filePath("worker.json"));
    const qint64 updated = activity.value("updated_ms").toInteger();
    const qint64 age = updated ? std::max<qint64>(0, now - updated) : -1;
    result["policy_updated_ms"] = updated; result["policy_age_ms"] = age;
    result["effective_cpu_percent"] = QJsonValue();
    if (worker && age >= 0 && age <= 6000) {
        result["effective_cpu_percent"] = activity.value("effective_cpu_percent");
        result["worker_policy"] = activity;
        if (running && !paused) result["state"] = activity.value("mode").toString("unknown");
    }
    if (!running) {
        if (worker) { result["state"] = "waiting"; result["reason"] = "Another recorder or viewer is indexing this history."; }
        else if (paused) { result["state"] = "paused"; result["reason"] = "Paused until you resume indexing."; }
        else if (result.value("state").toString() != "error" || !enabled) {
            result["state"] = "stopped"; result["reason"] = "Background indexing is stopped.";
        }
        result["worker_pid"] = 0;
    }
    if (running && paused && !worker) { result["state"] = "paused"; result["reason"] = "Paused until you resume indexing."; }
    if (worker && (paused || result.value("external_worker").toBool())) {
        result["state"] = "waiting";
        result["reason"] = result.value("external_worker").toBool()
            ? "Waiting for another recorder or viewer to finish indexing."
            : "Pausing the current OCR pass; its retained image remains available.";
    }
    if (worker && (age < 0 || age > 6000) && result.value("state").toString() != "waiting") {
        result["state"] = "unknown"; result["reason"] = "Worker is running; its latest policy update is unavailable or stale.";
    }
    const qint64 oldest = result.value("oldest_pending_timestamp_ms").toInteger();
    result["oldest_pending_age_ms"] = oldest ? std::max<qint64>(0, now - oldest) : 0;
    if (!runtime.isEmpty()) result["log_tail"] = readObject(QDir(runtime).filePath("service-log.json")).value("text").toString();
    if (!result.contains("progress")) result["progress"] = QJsonObject{};
    if (!result.contains("recovery")) result["recovery"] = "Start or resume background indexing for this saved history.";
    if (!policyError.isEmpty()) {
        result["state"] = "error"; result["policy_error"] = policyError; result["error"] = policyError;
        result["recovery"] = "Stop remains available. Restore the saved model or start with valid explicit indexing settings.";
    }
    return result;
}

QJsonObject controlIndexService(const QString &directory, const QString &action, const QJsonObject &requestedPolicy) {
    if (action != "start" && action != "ensure" && action != "pause" && action != "resume" && action != "stop")
        fail("Unknown indexing service action.");
    const QString path = datasetPath(directory), runtime = runtimeDirectory(path, true);
    Lease control(QDir(runtime).filePath("control.lock"));
    QElapsedTimer wait; wait.start();
    while (!control.acquire()) {
        if (wait.elapsed() > 2000) fail("Another indexing control request is still running.");
        QThread::msleep(20);
    }
    auto intent = readObject(statePath(path));
    // Automatic reopen/handoff must inspect current intent while holding the
    // same lock as Stop. A separate status/start sequence can undo a racing Stop.
    if (action == "ensure" && intent.value("enabled").isBool() && !intent.value("enabled").toBool()) {
        auto result = indexServiceStatus(path); result["requested_action"] = action;
        return result;
    }
    auto policy = intent.value("policy").toObject();
    if (action == "ensure" && !intent.isEmpty()) policy = checkedPolicy(policy);
    else if (action == "start" || action == "resume" || action == "ensure")
        policy = requestedPolicy.isEmpty() ? savedIndexPolicy(path) : checkedPolicy(requestedPolicy);
    else if (policy.isEmpty())
        // Pause/stop record intent even if the model has moved or no policy is
        // known yet. Preserve trial settings for a later explicit recovery.
        policy = requestedPolicy.isEmpty() ? savedPolicySource(path) : requestedPolicy;
    if (policy.isEmpty() && (action == "start" || action == "resume" || action == "ensure"))
        fail("No saved indexing policy. Start from a saved trial or provide explicit indexing settings.");
    const bool wasPaused = intent.value("paused").toBool();
    intent = {{"version", 1}, {"enabled", action != "stop"}, {"paused", action == "pause" || (action != "resume" && wasPaused)},
              {"policy", policy}, {"updated_ms", nowMs()}};
    writeObject(statePath(path), intent);
    if (action == "stop" || readObject(QDir(runtime).filePath("service.json")).value("state").toString() == "stopping") {
        QElapsedTimer stopped; stopped.start();
        while (leaseHeld(QDir(runtime).filePath("service.lock"))) {
            if (stopped.elapsed() > 9000) fail("Background indexing is still stopping; inspect status before restarting.");
            QThread::msleep(20);
        }
    }
    if ((action == "start" || action == "resume" || action == "ensure") && !leaseHeld(QDir(runtime).filePath("service.lock"))) {
        QProcess child;
        child.setProgram(QCoreApplication::applicationFilePath());
        child.setArguments({"service", "run", "--dir", path});
        child.setStandardInputFile(QProcess::nullDevice()); child.setStandardOutputFile(QProcess::nullDevice());
        child.setStandardErrorFile(QProcess::nullDevice()); child.setWorkingDirectory(path);
        qint64 pid = 0;
        if (!child.startDetached(&pid)) fail("Background indexer could not start.");
        QElapsedTimer started; started.start();
        while (!leaseHeld(QDir(runtime).filePath("service.lock")) && started.elapsed() < 1500) QThread::msleep(20);
        if (!leaseHeld(QDir(runtime).filePath("service.lock"))) fail("Background indexer did not acquire its lease; inspect service status.");
    }
    auto result = indexServiceStatus(path); result["requested_action"] = action;
    return result;
}

void publishIndexWorkerPolicy(const QString &directory, const QJsonObject &policy) {
    const QString path = datasetPath(directory), runtime = runtimeDirectory(path, true);
    QJsonObject result = policy;
    result["pid"] = qint64(getpid()); result["updated_ms"] = nowMs();
    writeObject(QDir(runtime).filePath("worker.json"), result);
}

QJsonObject ownedIndexWorkerPolicy(const QString &directory, qint64 controllerPid) {
    try {
        if (controllerPid <= 0) return {};
        const QString path = datasetPath(directory), runtime = runtimeDirectory(path, false);
        if (runtime.isEmpty()) return {};
        const auto result = readObject(QDir(runtime).filePath("worker.json"));
        const auto started = [](qint64 pid) -> qint64 {
            QFile file(QString("/proc/%1/stat").arg(pid));
            if (!file.open(QIODevice::ReadOnly)) return 0;
            const auto row = file.read(8192);
            const auto fields = row.mid(row.lastIndexOf(')') + 2).simplified().split(' ');
            return fields.size() > 19 && fields[0] != "Z" ? fields[19].toLongLong() : 0;
        };
        const auto pid = result.value("pid").toInteger();
        if (pid <= 0 || result.value("process_start_ticks").toInteger() <= 0 ||
            started(pid) != result.value("process_start_ticks").toInteger()) return {};
        if (pid != controllerPid && (result.value("owner_pid").toInteger() != controllerPid ||
            result.value("owner_start_ticks").toInteger() <= 0 ||
            started(controllerPid) != result.value("owner_start_ticks").toInteger())) return {};
        QFile command(QString("/proc/%1/cmdline").arg(pid));
        if (!command.open(QIODevice::ReadOnly)) return {};
        const auto arguments = command.read(65536).split('\0');
        if (arguments.size() < 5 || arguments[1] != "index" || arguments[2] != "--dir" ||
            QFile::decodeName(arguments[3]) != path ||
            QFileInfo(QString("/proc/%1/exe").arg(pid)).symLinkTarget() != QCoreApplication::applicationFilePath()) return {};
        return result;
    } catch (...) { return {}; }
}

int runIndexService(const QString &directory, const std::function<bool()> &stopRequested) {
    const QString path = datasetPath(directory), runtime = runtimeDirectory(path, true);
    Lease lease(QDir(runtime).filePath("service.lock"));
    if (!lease.acquire()) return 0;
    if (getpriority(PRIO_PROCESS, 0) < 10) setpriority(PRIO_PROCESS, 0, 10);
    QProcess worker;
    QJsonObject state{{"service_pid", qint64(getpid())}, {"controller_pid", 0}, {"worker_pid", 0}, {"state", "waiting"}};
    auto publish = [&] { state["heartbeat_ms"] = nowMs(); writeObject(QDir(runtime).filePath("service.json"), state); };
    const auto stopWorker = [&] {
        state["worker_pid"] = 0;
        state["controller_pid"] = 0;
        if (worker.state() == QProcess::NotRunning) return;
        worker.terminate();
        if (!worker.waitForFinished(6500)) { worker.kill(); worker.waitForFinished(1000); }
        worker.readAllStandardOutput(); worker.readAllStandardError();
    };
    int failures = 0;
    qint64 retryAfter = 0;
    QElapsedTimer heartbeat; heartbeat.start();
    QElapsedTimer controlClock; controlClock.start();
    QJsonObject intent = readObject(statePath(path));
    operationalLog(runtime, "Background indexing started."); publish();
    try {
        // Reopening the last WAL reader each heartbeat can rebuild the 32 KiB
        // shared-memory index even with SQLITE_OPEN_READONLY. Keep the actual
        // status connection open, without retaining a read transaction.
        IndexStatusReader statusReader(path);
        while (!stopRequested()) {
            if (controlClock.elapsed() >= 500) {
                intent = readObject(statePath(path)); controlClock.restart();
            }
            if (!intent.value("enabled").toBool()) break;
            const auto policy = checkedPolicy(intent.value("policy").toObject());
            if (policy.isEmpty()) fail("Saved indexing settings are unavailable.");
            const bool paused = intent.value("paused").toBool();
            worker.waitForFinished(0);
            worker.readAllStandardOutput(); worker.readAllStandardError();
            if (paused) stopWorker();
            else if (state.value("controller_pid").toInteger() && worker.state() == QProcess::NotRunning) {
                state["worker_pid"] = 0; state["controller_pid"] = 0;
                if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0) {
                    // A lock race belongs to its other owner; do not treat it as
                    // a failed model or repeatedly compete for the same lease.
                    if (!leaseHeld(QDir(path).filePath(".indexer.lock"))) {
                        ++failures; retryAfter = nowMs() + 10000;
                        operationalLog(runtime, "Index worker stopped unexpectedly; retry delayed.");
                        if (failures >= 3) fail("Index worker stopped three times. Review failed jobs and settings, then start indexing again.");
                    }
                } else failures = 0;
            }
            if (heartbeat.elapsed() >= HeartbeatMs || !state.contains("progress")) {
                if (worker.state() != QProcess::NotRunning) {
                    const auto receipt = ownedIndexWorkerPolicy(path, worker.processId());
                    state["worker_pid"] = receipt.value("pid").toInteger();
                    state["worker_start_ticks"] = receipt.value("process_start_ticks");
                }
                const auto index = statusReader.status();
                state["progress"] = numericProgress(index);
                state["oldest_pending_timestamp_ms"] = index.value("oldest_pending_timestamp_ms");
                const bool external = index.value("indexer_running").toBool() && worker.state() == QProcess::NotRunning;
                state["external_worker"] = external;
                if (paused) {
                    state["state"] = external ? "waiting" : "paused";
                    state["reason"] = external ? "Pause saved; another recorder or viewer still owns indexing." : "Paused until you resume indexing.";
                } else if (external) {
                    state["state"] = "waiting"; state["reason"] = "Another recorder or viewer owns indexing; takeover will follow its exit.";
                } else if (worker.state() != QProcess::NotRunning) {
                    state["state"] = "active"; state["reason"] = "Indexing saved moments.";
                } else if (index.value("pending").toInteger() == 0) {
                    if (index.value("failed").toInteger() > 0) {
                        state["state"] = "error"; state["reason"] = "Some saved images failed text indexing; OCR memory is released.";
                        state["recovery"] = "Review indexing settings and use index --retry-failed after correcting the cause.";
                    } else {
                        state["state"] = "waiting"; state["reason"] = "All pending moments are processed; OCR memory is released.";
                    }
                } else if (nowMs() < retryAfter) {
                    state["state"] = "waiting"; state["reason"] = "Worker stopped; retrying after a short delay.";
                } else {
                    QStringList arguments{"index", "--dir", path, "--parent-pid", QString::number(getpid())};
                    arguments << policyArguments(policy);
                    worker.start(QCoreApplication::applicationFilePath(), arguments);
                    if (!worker.waitForStarted(2000)) fail("Cannot launch the index worker.");
                    state["controller_pid"] = qint64(worker.processId()); state["worker_pid"] = 0; state["state"] = "active";
                    state["reason"] = "Indexing saved moments.";
                }
                publish(); heartbeat.restart();
            }
            QThread::msleep(worker.state() == QProcess::NotRunning ? 500 : 100);
        }
        state["state"] = "stopping"; publish();
        stopWorker(); state["state"] = "stopped"; state["reason"] = "Background indexing is stopped.";
        operationalLog(runtime, "Background indexing stopped."); publish();
        return 0;
    } catch (const std::exception &error) {
        stopWorker(); state["state"] = "error"; state["error"] = QString::fromUtf8(error.what()).left(500);
        state["reason"] = "Background indexing needs attention.";
        state["recovery"] = "Check saved indexing settings and retained failed jobs, then start indexing again.";
        operationalLog(runtime, "Background indexing stopped after a service error."); publish();
        return 1;
    }
}

} // namespace replay
