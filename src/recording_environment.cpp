#include "recording_environment.h"

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QRectF>
#include <QSocketNotifier>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <wayland-client.h>

namespace replay {
namespace {
constexpr int IpcDeadlineMs = 200;
constexpr int MaxIpcBytes = 4 * 1024 * 1024;
constexpr auto LoginName = "org.freedesktop.login1";
constexpr auto LoginPath = "/org/freedesktop/login1";
constexpr auto LoginManager = "org.freedesktop.login1.Manager";
constexpr auto LoginSession = "org.freedesktop.login1.Session";

// Wire interface metadata for the public hyprland-lock-notify-v1 protocol.
// No generated or compositor-internal headers are required by the application.
const wl_message notificationRequests[]{{"destroy", "", nullptr}};
const wl_message notificationEvents[]{{"locked", "", nullptr}, {"unlocked", "", nullptr}};
const wl_interface notificationInterface{"hyprland_lock_notification_v1", 1, 1, notificationRequests, 2, notificationEvents};
const wl_interface *notificationTypes[]{&notificationInterface};
const wl_message notifierRequests[]{{"destroy", "", nullptr}, {"get_lock_notification", "n", notificationTypes}};
const wl_interface notifierInterface{"hyprland_lock_notifier_v1", 1, 2, notifierRequests, 0, nullptr};

struct Fd {
    int fd = -1;
    explicit Fd(int value = -1) : fd(value) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd &) = delete;
    int release() { const int value = fd; fd = -1; return value; }
};

bool peer(int fd, pid_t *pid = nullptr) {
    ucred value{}; socklen_t size = sizeof(value);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &value, &size) || value.uid != geteuid() || value.pid <= 1) return false;
    if (pid) *pid = value.pid;
    return true;
}

int connectLocal(const QString &path) {
    const QByteArray encoded = QFile::encodeName(path);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (encoded.size() >= qsizetype(sizeof(address.sun_path))) return -1;
    std::memcpy(address.sun_path, encoded.constData(), encoded.size() + 1);
    Fd fd(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (fd.fd < 0) return -1;
    if (::connect(fd.fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) && errno != EINPROGRESS) return -1;
    pollfd waiting{fd.fd, POLLOUT, 0};
    if (::poll(&waiting, 1, IpcDeadlineMs) <= 0) return -1;
    int error = 0; socklen_t size = sizeof(error);
    if (getsockopt(fd.fd, SOL_SOCKET, SO_ERROR, &error, &size) || error || !peer(fd.fd)) return -1;
    return fd.release();
}

QByteArray request(const QString &socket, const QByteArray &command, pid_t expectedPid) {
    Fd fd(connectLocal(socket)); pid_t actual = 0;
    if (fd.fd < 0 || !peer(fd.fd, &actual) || actual != expectedPid) return {};
    // Hyprland handles its command socket synchronously. Always send immediately
    // and close on every path; never leave an idle control connection open.
    if (::send(fd.fd, command.constData(), command.size(), MSG_NOSIGNAL) != command.size()) return {};
    QElapsedTimer elapsed; elapsed.start();
    QByteArray result;
    while (elapsed.elapsed() < IpcDeadlineMs) {
        pollfd waiting{fd.fd, POLLIN, 0};
        const int status = ::poll(&waiting, 1, std::max(1, IpcDeadlineMs - int(elapsed.elapsed())));
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0) return {};
        char bytes[16384];
        const auto count = ::recv(fd.fd, bytes, sizeof(bytes), 0);
        if (!count) return result;
        if (count < 0) { if (errno == EAGAIN || errno == EINTR) continue; return {}; }
        result.append(bytes, count);
        if (result.size() > MaxIpcBytes) return {};
    }
    return {};
}

QDBusMessage loginCall(const QString &path, const QString &interface, const QString &method, const QVariantList &args = {}) {
    auto message = QDBusMessage::createMethodCall(LoginName, path, interface, method);
    message.setAutoStartService(false); message.setArguments(args);
    return QDBusConnection::systemBus().call(message, QDBus::Block, IpcDeadlineMs);
}

QVariantMap properties(const QString &path, const QString &interface) {
    const auto reply = loginCall(path, "org.freedesktop.DBus.Properties", "GetAll", {interface});
    if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return {};
    return qdbus_cast<QVariantMap>(reply.arguments().first());
}

bool boolean(const QVariantMap &values, const QString &key) { return values.contains(key) && values[key].metaType().id() == QMetaType::Bool; }
bool boolField(const QJsonObject &object, const char *key) { return object.value(key).isBool(); }
bool integerField(const QJsonObject &object, const char *key) {
    const auto value = object.value(key);
    return value.isDouble() && std::isfinite(value.toDouble()) && std::floor(value.toDouble()) == value.toDouble();
}
bool pair(const QJsonValue &value, double *x, double *y) {
    if (!value.isArray()) return false;
    const auto data = value.toArray();
    if (data.size() != 2 || !data[0].isDouble() || !data[1].isDouble()) return false;
    *x = data[0].toDouble(); *y = data[1].toDouble();
    return std::isfinite(*x) && std::isfinite(*y) && std::abs(*x) < 1000000 && std::abs(*y) < 1000000;
}

QRegularExpression titleExpression(const QString &text) {
    // Bound PCRE's work even for a user-supplied pathological expression.
    return QRegularExpression("(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=1000)(?:" + text + ')');
}

// Only fields consumed by capture guards belong in the retention fence.
// In particular, capture itself can change compositor render bookkeeping such
// as directScanoutTo. Including it would make a safe capture invalidate itself.
const QStringList MonitorSafetyFields{
    "name", "id", "make", "model", "serial", "description", "disabled", "dpmsStatus",
    "x", "y", "width", "height", "scale", "transform", "mirrorOf"};
const QStringList WindowSafetyFields{
    "mapped", "hidden", "visible", "at", "size", "class", "initialClass", "title", "address"};
const QStringList MonitorDiagnosticFields = MonitorSafetyFields + QStringList{
    "focused", "directScanoutTo", "directScanoutBlockedBy", "activelyTearing", "vrr", "refreshRate",
    "activeWorkspace", "specialWorkspace", "reserved", "currentFormat", "availableModes"};
const QStringList WindowDiagnosticFields = WindowSafetyFields + QStringList{
    "focusHistoryID", "pid", "monitor", "workspace", "pinned", "floating", "fullscreen",
    "fullscreenClient", "initialTitle", "xwayland", "swallowing", "grouped", "tags", "inhibitingIdle"};

QJsonArray canonicalRecords(const QJsonArray &records, const QStringList &fields, bool remainder = false) {
    QVector<std::pair<QByteArray, QJsonValue>> sorted;
    sorted.reserve(records.size());
    for (const auto &record : records) {
        QJsonValue value = record;
        if (record.isObject()) {
            const auto object = record.toObject();
            QJsonObject selected;
            if (remainder) {
                selected = object;
                for (const auto &key : fields) selected.remove(key);
            } else {
                for (const auto &key : fields)
                    if (object.contains(key)) selected.insert(key, object.value(key));
            }
            value = selected;
        }
        // Keep missing keys, nulls and wrong types distinct. Sorting removes
        // client enumeration order without removing any client or its identity.
        sorted.append({QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact), value});
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &left, const auto &right) { return left.first < right.first; });
    QJsonArray result;
    for (const auto &entry : sorted) result.append(entry.second);
    return result;
}

QJsonObject safetyState(const EnvironmentObservation &observed) {
    return {
        {"compositor", observed.compositorAvailable}, {"notify", observed.lockNotificationsAvailable},
        {"lock_known", observed.compositorLockKnown}, {"locked", observed.compositorLocked},
        {"session_known", observed.sessionKnown}, {"active", observed.sessionActive}, {"session_locked", observed.sessionLocked},
        {"sleep_known", observed.sleepKnown}, {"sleep", observed.sleeping}, {"shutdown", observed.shuttingDown},
        {"config_known", observed.configKnown}, {"config_error", observed.configError},
        {"exclusions_verified", observed.exclusionsVerified}, {"config_generation", qint64(observed.configGeneration)},
        {"instance", observed.compositorInstance}, {"wayland_display", observed.waylandDisplay},
        {"events", qint64(observed.eventGeneration)},
        {"monitors", observed.monitors.size() <= 64 ? QJsonValue(canonicalRecords(observed.monitors, MonitorSafetyFields)) : QJsonValue("over_limit")},
        {"windows", observed.windows.size() <= 4096 ? QJsonValue(canonicalRecords(observed.windows, WindowSafetyFields)) : QJsonValue("over_limit")}};
}

void increment(QJsonObject &counts, const QString &key) {
    // Keys are fixed labels from this file; never put event payloads or arbitrary
    // compositor keys into diagnostic output. All counters saturate in memory.
    if (!counts.contains(key) && counts.size() >= 64) return;
    counts.insert(key, std::min(1000000000.0, counts.value(key).toDouble() + 1));
}

QString diagnosticEventName(const QByteArray &event) {
    static const QList<QByteArray> known{
        "workspace", "workspacev2", "focusedmon", "focusedmonv2", "activewindow", "activewindowv2",
        "fullscreen", "monitorremoved", "monitoradded", "monitoraddedv2", "createworkspace", "createworkspacev2",
        "destroyworkspace", "destroyworkspacev2", "moveworkspace", "moveworkspacev2", "renameworkspace",
        "activespecial", "activespecialv2", "activelayout", "openwindow", "closewindow", "movewindow", "movewindowv2",
        "openlayer", "closelayer", "submap", "changefloatingmode", "urgent", "minimize", "screencast", "screencastv2",
        "windowtitle", "windowtitlev2", "togglegroup", "moveintogroup", "moveoutofgroup", "ignoregrouplock",
        "lockgroups", "configreloaded", "pin", "bell"};
    const auto separator = event.indexOf(">>");
    const auto name = separator < 1 ? QByteArray{} : event.left(separator);
    return known.contains(name) ? QString::fromLatin1(name) : QStringLiteral("other");
}
}

struct RecordingEnvironment::Impl {
    RecordingEnvironment *owner;
    Source source;
    EnvironmentOptions options;
    QString invalidOptions, pinnedIdentity;
    quint64 sequence = 1, events = 0, configEvents = 0;
    QJsonObject lastState;
    bool diagnosticsEnabled = false, diagnosticBaseline = false;
    QJsonObject diagnosticEvents, diagnosticInvalidatingEvents, diagnosticFields, diagnosticStateFields, diagnosticTransitions;
    QJsonArray diagnosticMonitors, diagnosticWindows;
    quint64 diagnosticSnapshots = 0, diagnosticStateChanges = 0;
    QString socketDirectory, instance, waylandName, sessionPath, sessionId;
    pid_t compositorPid = 0;
    int eventFd = -1;
    QByteArray eventBuffer;
    std::unique_ptr<QSocketNotifier> eventNotifier, waylandNotifier;
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_proxy *lockNotifier = nullptr, *lockNotification = nullptr;
    bool lockState = true, lockReady = false;
    bool sleepSignal = false, shutdownSignal = false, subscribed = false;
    qint64 suspendOffsetNs = -1;
    QElapsedTimer reconnectClock;

    Impl(RecordingEnvironment *object, Source injected) : owner(object), source(std::move(injected)) {}
    ~Impl() { closeCompositor(); }
    void transition(const QString &category = {}) {
        ++sequence; ++events;
        if (diagnosticsEnabled && !category.isEmpty()) increment(diagnosticTransitions, category);
    }
    void observeDiagnostics(const EnvironmentObservation &observed, const QJsonObject &state) {
        if (!diagnosticsEnabled) return;
        diagnosticSnapshots = std::min(quint64(1000000000), diagnosticSnapshots + 1);
        if (observed.monitors.size() > 64 || observed.windows.size() > 4096) {
            increment(diagnosticFields, "over_limit");
            diagnosticMonitors = {}; diagnosticWindows = {}; diagnosticBaseline = false;
            return;
        }
        if (diagnosticBaseline) {
            if (state != lastState) {
                diagnosticStateChanges = std::min(quint64(1000000000), diagnosticStateChanges + 1);
                for (auto field = state.begin(); field != state.end(); ++field)
                    if (field.value() != lastState.value(field.key())) increment(diagnosticStateFields, field.key());
            }
            auto compare = [&](const QJsonArray &before, const QJsonArray &after, const QStringList &fields, const QString &prefix) {
                if (before == after) return;
                for (const auto &field : fields)
                    if (canonicalRecords(before, {field}) != canonicalRecords(after, {field})) increment(diagnosticFields, prefix + field);
                if (canonicalRecords(before, fields, true) != canonicalRecords(after, fields, true)) increment(diagnosticFields, prefix + "other");
            };
            compare(diagnosticMonitors, observed.monitors, MonitorDiagnosticFields, "monitors.");
            compare(diagnosticWindows, observed.windows, WindowDiagnosticFields, "windows.");
        }
        diagnosticMonitors = observed.monitors; diagnosticWindows = observed.windows;
        diagnosticBaseline = true;
    }
    void closeCompositor() {
        eventNotifier.reset(); waylandNotifier.reset();
        if (eventFd >= 0) ::close(eventFd);
        eventFd = -1; eventBuffer.clear();
        if (lockNotification) wl_proxy_destroy(lockNotification);
        if (lockNotifier) wl_proxy_destroy(lockNotifier);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
        lockNotification = lockNotifier = nullptr; registry = nullptr; display = nullptr;
        lockReady = false; lockState = true; compositorPid = 0; socketDirectory.clear(); waylandName.clear();
    }
    void lostCompositor() { transition("compositor_lost"); closeCompositor(); }
    static void locked(void *data, wl_proxy *) {
        auto &self = *static_cast<Impl *>(data); self.lockState = true; self.transition("locked");
    }
    static void unlocked(void *data, wl_proxy *) {
        auto &self = *static_cast<Impl *>(data); self.lockState = false; self.transition("unlocked");
    }
    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
        auto &self = *static_cast<Impl *>(data);
        if (std::strcmp(interface, notifierInterface.name) || self.lockNotifier) return;
        self.lockNotifier = static_cast<wl_proxy *>(wl_registry_bind(registry, name, &notifierInterface, 1));
        self.lockNotification = wl_proxy_marshal_flags(self.lockNotifier, 1, &notificationInterface, 1, 0, nullptr);
        static void (*listener[])(void) {reinterpret_cast<void (*)(void)>(locked), reinterpret_cast<void (*)(void)>(unlocked)};
        wl_proxy_add_listener(self.lockNotification, listener, &self);
    }
    static void globalRemoved(void *data, wl_registry *, uint32_t) { static_cast<Impl *>(data)->transition("registry_removed"); }
    bool dispatchWayland(int timeout = 0) {
        if (!display) return false;
        while (wl_display_prepare_read(display) != 0) if (wl_display_dispatch_pending(display) < 0) return false;
        if (wl_display_flush(display) < 0 && errno != EAGAIN) { wl_display_cancel_read(display); return false; }
        pollfd waiting{wl_display_get_fd(display), POLLIN, 0};
        const int result = ::poll(&waiting, 1, timeout);
        if (result <= 0) { wl_display_cancel_read(display); return result == 0 || errno == EINTR; }
        if (!(waiting.revents & POLLIN)) { wl_display_cancel_read(display); return false; }
        return wl_display_read_events(display) >= 0 && wl_display_dispatch_pending(display) >= 0;
    }
    bool synchronizeWayland() {
        bool done = false;
        auto *callback = wl_display_sync(display);
        static const wl_callback_listener listener{[](void *data, wl_callback *callback, uint32_t) {
            *static_cast<bool *>(data) = true; wl_callback_destroy(callback);
        }};
        wl_callback_add_listener(callback, &listener, &done);
        QElapsedTimer elapsed; elapsed.start();
        while (!done && elapsed.elapsed() < IpcDeadlineMs)
            if (!dispatchWayland(std::max(1, IpcDeadlineMs - int(elapsed.elapsed())))) break;
        if (!done) wl_callback_destroy(callback);
        return done;
    }
    void readEvents() {
        if (eventFd < 0) return;
        char bytes[8192];
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto count = ::recv(eventFd, bytes, sizeof(bytes), 0);
            if (!count) { lostCompositor(); return; }
            if (count < 0) {
                if (errno == EINTR) continue;
                if (errno != EAGAIN) lostCompositor();
                return;
            }
            eventBuffer.append(bytes, count);
            if (eventBuffer.size() > 256 * 1024) { lostCompositor(); return; }
            qsizetype end;
            while ((end = eventBuffer.indexOf('\n')) >= 0) {
                const QByteArray line = eventBuffer.left(end); eventBuffer.remove(0, end + 1);
                if (line.startsWith("configreloaded>>")) ++configEvents;
                const bool invalidates = RecordingEnvironment::invalidatingEvent(line);
                if (diagnosticsEnabled) {
                    const auto name = diagnosticEventName(line);
                    increment(diagnosticEvents, name);
                    if (invalidates) increment(diagnosticInvalidatingEvents, name);
                }
                if (invalidates) transition();
            }
        }
        // A flooded event channel cannot supply trustworthy transition coverage.
        lostCompositor();
    }
    bool identifySession(pid_t pid) {
        auto reply = loginCall(LoginPath, LoginManager, "GetSessionByPID", {uint(pid)});
        // UWSM starts Hyprland in the per-user manager, outside a logind process
        // scope. Its explicitly exported graphical session ID is the fallback;
        // never pick whichever unrelated session happens to be active.
        if (reply.type() != QDBusMessage::ReplyMessage) {
            const QString configured = sessionId.isEmpty() ? qEnvironmentVariable("XDG_SESSION_ID") : sessionId;
            if (configured.isEmpty()) return false;
            reply = loginCall(LoginPath, LoginManager, "GetSession", {configured});
        }
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().size() != 1) return false;
        const QString path = qvariant_cast<QDBusObjectPath>(reply.arguments().first()).path();
        const auto values = properties(path, LoginSession);
        const auto user = qvariant_cast<QDBusArgument>(values.value("User"));
        uint uid = uint(-1); QDBusObjectPath userPath;
        if (user.currentType() == QDBusArgument::StructureType) {
            user.beginStructure(); user >> uid >> userPath; user.endStructure();
        }
        if (uid != geteuid() || values.value("Type").toString() != "wayland" ||
            !boolean(values, "Remote") || values.value("Remote").toBool()) return false;
        const QString id = values.value("Id").toString();
        if (id.isEmpty() || (!sessionId.isEmpty() && sessionId != id)) return false;
        if (sessionPath != path) {
            if (!sessionPath.isEmpty()) QDBusConnection::systemBus().disconnect(LoginName, sessionPath, {}, {}, owner, nullptr);
            sessionId = id; sessionPath = path;
            auto bus = QDBusConnection::systemBus();
            if (!bus.connect(LoginName, path, LoginSession, "Lock", owner, SLOT(sessionSignal())) ||
                !bus.connect(LoginName, path, LoginSession, "Unlock", owner, SLOT(sessionSignal())) ||
                !bus.connect(LoginName, path, "org.freedesktop.DBus.Properties", "PropertiesChanged", owner,
                             SLOT(sessionProperties(QString,QVariantMap,QStringList)))) return false;
        }
        return true;
    }
    bool connectCompositor() {
        if (display && eventFd >= 0) return true;
        if (reconnectClock.isValid() && reconnectClock.elapsed() < 500) return false;
        reconnectClock.restart();
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!QFileInfo(runtime).isDir() || QFileInfo(runtime).ownerId() != geteuid()) return false;
        const QString expected = qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE");
        const QDir hypr(runtime + "/hypr");
        QStringList candidates = hypr.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
        if (candidates.size() > 32) return false;
        if (candidates.removeAll(expected)) candidates.prepend(expected);
        for (const QString &candidate : candidates) {
            if (reconnectClock.elapsed() >= 1000) return false;
            const QString directory = hypr.filePath(candidate);
            if (QFileInfo(directory).isSymLink() || QFileInfo(directory).ownerId() != geteuid()) continue;
            // Inspect the command peer with a real read-only request, avoiding
            // the compositor's idle-control-connection timeout.
            Fd eventsFd(connectLocal(directory + "/.socket2.sock")); pid_t pid = 0;
            if (eventsFd.fd < 0 || !peer(eventsFd.fd, &pid) || !identifySession(pid)) continue;
            QStringList displays = QDir(runtime).entryList({"wayland-*"}, QDir::System, QDir::Name);
            if (displays.size() > 64) return false;
            const QString preferred = qEnvironmentVariable("WAYLAND_DISPLAY");
            if (displays.removeAll(preferred)) displays.prepend(preferred);
            for (const auto &name : displays) {
                if (reconnectClock.elapsed() >= 1000) return false;
                if (name.endsWith(".lock")) continue;
                Fd socket(connectLocal(QDir(runtime).filePath(name))); pid_t waylandPid = 0;
                if (socket.fd < 0 || !peer(socket.fd, &waylandPid) || waylandPid != pid) continue;
                display = wl_display_connect_to_fd(socket.release());
                if (!display) continue;
                registry = wl_display_get_registry(display);
                static const wl_registry_listener listener{global, globalRemoved};
                wl_registry_add_listener(registry, &listener, this);
                if (!synchronizeWayland() || !lockNotification || !synchronizeWayland()) { closeCompositor(); continue; }
                lockReady = true;
                // Absence of an initial locked event means unlocked after the
                // notification object's creation has completed on the server.
                const auto locked = QJsonDocument::fromJson(request(directory + "/.socket.sock", "j/locked", pid));
                if (!locked.isObject() || !locked.object().value("locked").isBool()) { closeCompositor(); continue; }
                lockState = locked.object().value("locked").toBool();
                socketDirectory = directory; compositorPid = pid; instance = candidate; waylandName = name;
                eventFd = eventsFd.release();
                eventNotifier = std::make_unique<QSocketNotifier>(eventFd, QSocketNotifier::Read);
                QObject::connect(eventNotifier.get(), &QSocketNotifier::activated, owner, [this] { readEvents(); });
                waylandNotifier = std::make_unique<QSocketNotifier>(wl_display_get_fd(display), QSocketNotifier::Read);
                QObject::connect(waylandNotifier.get(), &QSocketNotifier::activated, owner, [this] {
                    if (!dispatchWayland()) lostCompositor();
                });
                ++configEvents; transition("compositor_connected");
                return true;
            }
        }
        return false;
    }
    EnvironmentObservation observe() {
        EnvironmentObservation value;
        // BOOTTIME includes suspension, MONOTONIC does not. This closes the
        // wake-up gap when capture blocked Qt's delivery of a sleep signal.
        timespec boot{}, monotonic{};
        if (clock_gettime(CLOCK_BOOTTIME, &boot) || clock_gettime(CLOCK_MONOTONIC, &monotonic)) return value;
        const qint64 offset = qint64(boot.tv_sec - monotonic.tv_sec) * 1000000000 + boot.tv_nsec - monotonic.tv_nsec;
        if (suspendOffsetNs >= 0 && offset - suspendOffsetNs > 10000000) transition("resume_clock");
        suspendOffsetNs = offset;
        if (!subscribed) {
            auto bus = QDBusConnection::systemBus();
            subscribed = bus.isConnected() &&
                bus.connect(LoginName, LoginPath, LoginManager, "PrepareForSleep", owner, SLOT(prepareForSleep(bool))) &&
                bus.connect(LoginName, LoginPath, LoginManager, "PrepareForShutdown", owner, SLOT(prepareForShutdown(bool)));
        }
        if (!subscribed || !connectCompositor()) return value;
        readEvents();
        if (!display || !dispatchWayland()) { if (display) lostCompositor(); return value; }
        // A sync fence drains both lock and unlock events that occurred while a
        // capture call blocked the owning Qt thread. A rapid pair increments
        // generation even when the final locked state equals the initial one.
        if (!synchronizeWayland()) { lostCompositor(); return value; }
        const auto session = properties(sessionPath, LoginSession);
        value.sessionKnown = boolean(session, "Active") && boolean(session, "LockedHint") && session.value("Type").toString() == "wayland";
        value.sessionActive = session.value("Active").toBool() && session.value("State").toString() != "closing";
        value.sessionLocked = session.value("LockedHint").toBool();
        const auto manager = properties(LoginPath, LoginManager);
        value.sleepKnown = boolean(manager, "PreparingForSleep") && boolean(manager, "PreparingForShutdown");
        value.sleeping = sleepSignal || manager.value("PreparingForSleep").toBool();
        value.shuttingDown = shutdownSignal || manager.value("PreparingForShutdown").toBool();
        const auto monitors = QJsonDocument::fromJson(request(socketDirectory + "/.socket.sock", "j/monitors all", compositorPid));
        const auto windows = QJsonDocument::fromJson(request(socketDirectory + "/.socket.sock", "j/clients", compositorPid));
        const auto locked = QJsonDocument::fromJson(request(socketDirectory + "/.socket.sock", "j/locked", compositorPid));
        const auto errors = QJsonDocument::fromJson(request(socketDirectory + "/.socket.sock", "j/configerrors", compositorPid));
        if (QRegularExpression("^[0-9a-f]{64}$").match(options.exclusionMaskToken).hasMatch()) {
            const QByteArray command = "/eval assert(_G.oma_replay_capture_exclusions == \"" +
                options.exclusionMaskToken.toLatin1() + "\")";
            value.exclusionsVerified = request(socketDirectory + "/.socket.sock", command, compositorPid).trimmed() == "ok";
        }
        readEvents();
        if (!display || !synchronizeWayland()) { if (display) lostCompositor(); return value; }
        value.compositorAvailable = monitors.isArray() && windows.isArray();
        value.monitors = monitors.array(); value.windows = windows.array();
        value.configKnown = errors.isArray(); value.configError = false;
        for (const auto &error : errors.array()) {
            // Hyprland 0.56 emits [""] for an error-free Lua configuration.
            if (!error.isString() || !error.toString().trimmed().isEmpty()) value.configError = true;
        }
        value.compositorLockKnown = locked.isObject() && locked.object().value("locked").isBool();
        value.compositorLocked = lockState || locked.object().value("locked").toBool(true);
        value.lockNotificationsAvailable = lockReady;
        value.compositorInstance = instance; value.eventGeneration = events;
        value.waylandDisplay = waylandName;
        value.configGeneration = configEvents;
        return value;
    }
};

QJsonObject EnvironmentSnapshot::json() const {
    return {{"capture_allowed", captureAllowed}, {"reason", reason}, {"detail", detail},
            {"generation", qint64(generation)}, {"config_generation", qint64(configGeneration)},
            {"output_identity", outputIdentity}, {"selected_output", selectedOutput},
            {"compositor_instance", compositorInstance},
            {"wayland_display", waylandDisplay},
            {"visible_windows", visibleWindows}, {"excluded_apps", QJsonArray::fromStringList(excludedApps)}};
}

RecordingEnvironment::RecordingEnvironment(QObject *parent) : RecordingEnvironment(Source{}, parent) {}
RecordingEnvironment::RecordingEnvironment(Source source, QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this, std::move(source))) {}
RecordingEnvironment::~RecordingEnvironment() = default;

QString RecordingEnvironment::validateOptions(const EnvironmentOptions &options) {
    // Focus mode is given its display by the compositor, so the configured
    // output is preserved but ignored and may be empty.
    if (!options.followFocus &&
        (options.output.isEmpty() || options.output.size() > 256 || options.output.contains(QRegularExpression("[\\x00-\\x20/\\\\]"))))
        return "Select one named display output.";
    if (options.excludedApps.size() + options.skippedApps.size() > 64 || options.excludedWindows.size() > 64)
        return "Too many exclusion rules.";
    if (!options.exclusionMaskToken.isEmpty() && !QRegularExpression("^[0-9a-f]{64}$").match(options.exclusionMaskToken).hasMatch())
        return "Invalid compositor exclusion receipt.";
    for (const auto &app : options.excludedApps + options.skippedApps)
        if (app.isEmpty() || app.size() > 256 || app.contains(QChar('\0')) || app.contains('\n') || app.contains('\r'))
            return "Invalid excluded application identifier.";
    for (const auto &rule : options.excludedWindows) {
        if (rule.scope != "output") return "Only whole-output window exclusions are supported.";
        if (rule.appId.isEmpty() && rule.titleRegex.isEmpty() && rule.address.isEmpty()) return "A window exclusion needs a matcher.";
        if (rule.appId.size() > 256 || rule.appId.contains(QChar('\0')) || rule.titleRegex.size() > 512 ||
            (!rule.titleRegex.isEmpty() && !titleExpression(rule.titleRegex).isValid())) return "Invalid window exclusion expression.";
        if (!rule.address.isEmpty() && !QRegularExpression("^0x[0-9a-f]{1,16}$").match(rule.address).hasMatch())
            return "Invalid window address; use its current 0x hexadecimal address.";
        if (!rule.address.isEmpty() && rule.compositorInstance.isEmpty()) return "Window addresses require their original desktop session identity.";
    }
    return {};
}

void RecordingEnvironment::configure(const EnvironmentOptions &options) {
    d->options = options; d->invalidOptions = validateOptions(options);
    if (!d->options.excludedApps.contains("omarchy-replay")) d->options.excludedApps.append("omarchy-replay");
    if (!d->options.excludedApps.contains("org.omarchy.screensaver")) d->options.excludedApps.append("org.omarchy.screensaver");
    d->pinnedIdentity = options.outputIdentity; d->lastState = {}; d->transition("configured");
}
quint64 RecordingEnvironment::generation() const { return d->sequence; }

void RecordingEnvironment::setDiagnosticsEnabled(bool enabled) {
    if (enabled && !d->diagnosticsEnabled) {
        d->diagnosticEvents = {}; d->diagnosticInvalidatingEvents = {}; d->diagnosticFields = {};
        d->diagnosticStateFields = {}; d->diagnosticTransitions = {};
        d->diagnosticSnapshots = d->diagnosticStateChanges = 0;
        d->diagnosticBaseline = false;
    }
    d->diagnosticsEnabled = enabled;
    if (!enabled) { d->diagnosticMonitors = {}; d->diagnosticWindows = {}; d->diagnosticBaseline = false; }
}

QJsonObject RecordingEnvironment::diagnostics() const {
    return {{"enabled", d->diagnosticsEnabled}, {"snapshots", qint64(d->diagnosticSnapshots)},
        {"state_changes", qint64(d->diagnosticStateChanges)}, {"events", d->diagnosticEvents},
        {"invalidating_events", d->diagnosticInvalidatingEvents}, {"transitions", d->diagnosticTransitions},
        {"state_field_changes", d->diagnosticStateFields}, {"observed_field_changes", d->diagnosticFields}};
}

QString RecordingEnvironment::monitorIdentity(const QJsonObject &monitor) {
    QJsonArray identity;
    if (!monitor.value("serial").toString().trimmed().isEmpty())
        identity = {monitor.value("make").toString(), monitor.value("model").toString(), monitor.value("serial").toString()};
    else if (!monitor.value("description").toString().isEmpty()) identity = {monitor.value("description").toString()};
    else return {};
    return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}

bool RecordingEnvironment::invalidatingEvent(const QByteArray &event) {
    const auto separator = event.indexOf(">>");
    if (separator < 1) return true;
    const auto name = event.left(separator);
    // Screencast notifications come from Replay's own capture connection.
    // Bell/urgent/submap notifications do not change retained coverage. Focus
    // and title events remain conservative: they may accompany visibility or
    // exclusion changes that begin and end between the two snapshots.
    if (name == "screencast" || name == "screencastv2" || name == "bell" || name == "urgent" || name == "submap") return false;
    return true;
}

EnvironmentSnapshot RecordingEnvironment::snapshot() {
    QCoreApplication::sendPostedEvents(this);
    EnvironmentSnapshot result;
    auto block = [&](const QString &reason, const QString &detail) { result.reason = reason; result.detail = detail; };
    EnvironmentObservation observed;
    try { observed = d->source ? d->source() : d->observe(); }
    catch (...) { block("environment_unknown", "Desktop safety state could not be read."); }
    const QJsonObject state = safetyState(observed);
    d->observeDiagnostics(observed, state);
    if (state != d->lastState) { ++d->sequence; d->lastState = state; }
    result.generation = d->sequence;
    result.configGeneration = observed.configGeneration;
    result.outputIdentity = d->pinnedIdentity;
    result.compositorInstance = observed.compositorInstance;
    result.waylandDisplay = observed.waylandDisplay;
    if (!d->invalidOptions.isEmpty()) { block("invalid_configuration", d->invalidOptions); return result; }
    if (!observed.compositorAvailable) { block("compositor_unavailable", "Waiting for the same Hyprland desktop to reconnect."); return result; }
    for (const auto &rule : d->options.excludedWindows) {
        if (!rule.address.isEmpty() && rule.compositorInstance != observed.compositorInstance) {
            block("stale_window_exclusion", "An excluded window belongs to a previous desktop session. Remove or reselect that window."); return result;
        }
    }
    if (!observed.lockNotificationsAvailable || !observed.compositorLockKnown) {
        block("lock_state_unknown", "Capture requires Hyprland lock notifications and a verified compositor lock state."); return result;
    }
    if (!observed.sessionKnown || !observed.sleepKnown) { block("session_unknown", "Waiting for verified local session and sleep state."); return result; }
    if (observed.shuttingDown) { block("shutting_down", "The desktop is shutting down."); return result; }
    if (observed.sleeping) { block("sleeping", "Recording is suspended while the computer sleeps."); return result; }
    if (observed.compositorLocked || observed.sessionLocked) { block("locked", "Recording is suspended while the desktop is locked."); return result; }
    if (!observed.sessionActive) { block("session_inactive", "Recording is suspended while this desktop session is inactive."); return result; }
    if (!observed.configKnown || observed.configError) { block("compositor_configuration", "Recording is suspended until Hyprland configuration is valid."); return result; }
    if (observed.monitors.size() > 64 || observed.windows.size() > 4096) { block("environment_unknown", "Desktop metadata exceeded its bounds."); return result; }
    QJsonObject selected; int found = 0;
    for (const auto &entry : observed.monitors) {
        if (!entry.isObject()) { block("environment_unknown", "Display metadata is incomplete."); return result; }
        const auto monitor = entry.toObject();
        // Focus mode selects whatever the compositor marks focused. Exactly one
        // display must qualify; another display is never substituted.
        const bool match = d->options.followFocus ? boolField(monitor, "focused") && monitor.value("focused").toBool()
                                                  : monitor.value("name").toString() == d->options.output;
        if (match) { selected = monitor; ++found; }
    }
    if (found != 1) {
        if (d->options.followFocus) block("focus_unknown", "Waiting for exactly one focused display.");
        else block("output_unavailable", "Waiting for the selected display; another display will not be substituted.");
        return result;
    }
    result.selectedOutput = selected.value("name").toString();
    if (!boolField(selected, "disabled") || !boolField(selected, "dpmsStatus") || !integerField(selected, "id") ||
        !integerField(selected, "x") || !integerField(selected, "y") || !integerField(selected, "width") || !integerField(selected, "height") ||
        !selected.value("scale").isDouble() || selected.value("scale").toDouble() <= 0) {
        block("environment_unknown", "Selected display metadata is incomplete."); return result;
    }
    if (selected.value("disabled").toBool() || !selected.value("dpmsStatus").toBool()) { block("output_off", "Waiting for the selected display to turn on."); return result; }
    if (selected.value("mirrorOf").toString("none") != "none") { block("output_mirrored", "Mirrored outputs need explicit capture support."); return result; }
    // Focus mode never pins or compares a hardware identity: every display is
    // eligible by design, so the configured identity is echoed but ignored.
    if (!d->options.followFocus) {
        const QString identity = monitorIdentity(selected);
        if (!d->pinnedIdentity.isEmpty() && identity != d->pinnedIdentity) { block("output_identity_changed", "The device on this output changed; select it explicitly before recording."); return result; }
        if (d->pinnedIdentity.isEmpty()) d->pinnedIdentity = identity;
    }
    result.outputIdentity = d->pinnedIdentity;
    const double scale = selected.value("scale").toDouble();
    double width = selected.value("width").toDouble() / scale, height = selected.value("height").toDouble() / scale;
    if (selected.value("transform").toInt() % 2) std::swap(width, height);
    const QRectF displayBounds(selected.value("x").toDouble(), selected.value("y").toDouble(), width, height);
    bool malformed = false, excludedVisible = false;
    for (const auto &entry : observed.windows) {
        if (!entry.isObject()) { malformed = true; break; }
        const auto window = entry.toObject();
        if (!boolField(window, "mapped") || !boolField(window, "hidden") || !boolField(window, "visible")) { malformed = true; break; }
        if (!window.value("mapped").toBool() || window.value("hidden").toBool() || !window.value("visible").toBool()) continue;
        double x, y, w, h;
        if (!pair(window.value("at"), &x, &y) || !pair(window.value("size"), &w, &h) || w <= 0 || h <= 0 ||
            !window.value("class").isString() || !window.value("initialClass").isString() || !window.value("title").isString() ||
            !window.value("address").isString() || window.value("title").toString().size() > 4096) { malformed = true; break; }
        // Include unfocused, pinned, floating, grouped-visible and straddling
        // windows. Never infer privacy from focus or another window's occlusion.
        if (!QRectF(x, y, w, h).intersects(displayBounds)) continue;
        const auto app = window.value("class").toString(), initialApp = window.value("initialClass").toString();
        const auto title = window.value("title").toString(), address = window.value("address").toString().toLower();
        bool strict = d->options.excludedApps.contains(app) || d->options.excludedApps.contains(initialApp);
        const bool skipped = d->options.skippedApps.contains(app) || d->options.skippedApps.contains(initialApp);
        bool pause = (strict || skipped) && app != "omarchy-replay" && initialApp != "omarchy-replay";
        for (const auto &rule : d->options.excludedWindows) {
            if (!rule.appId.isEmpty() && rule.appId != app && rule.appId != initialApp) continue;
            if (!rule.address.isEmpty() && rule.address != address) continue;
            if (!rule.titleRegex.isEmpty()) {
                const auto match = titleExpression(rule.titleRegex).match(title);
                if (!match.isValid()) { malformed = true; break; }
                if (!match.hasMatch()) continue;
            }
            strict = true;
            pause = true;
        }
        if (malformed) break;
        const bool excluded = strict || skipped;
        result.visibleWindows.append(QJsonObject{{"address", address}, {"app_id", app}, {"initial_app_id", initialApp},
            {"title", title}, {"excluded", excluded}, {"exclusion_mode", strict ? "strict" : skipped ? "skip" : ""},
            {"at", window.value("at")}, {"size", window.value("size")}});
        if (excluded && !result.excludedApps.contains(app)) result.excludedApps.append(app);
        excludedVisible = excludedVisible || pause;
    }
    if (malformed) { result.visibleWindows = {}; block("window_state_unknown", "Window visibility or exclusion matching could not be verified."); return result; }
    if (d->options.exclusionMaskToken.isEmpty() || !observed.exclusionsVerified) {
        block("exclusions_unverified", "Waiting for compositor capture exclusions to be installed and verified."); return result;
    }
    if (excludedVisible) { block("excluded_window", "Recording is suspended while an excluded window is visible on this display."); return result; }
    result.captureAllowed = true; result.reason = "ready"; result.detail = "Desktop capture is available.";
    return result;
}

void RecordingEnvironment::prepareForSleep(bool sleeping) { d->sleepSignal = sleeping; d->transition("sleep"); }
void RecordingEnvironment::prepareForShutdown(bool shuttingDown) { d->shutdownSignal = shuttingDown; d->transition("shutdown"); }
void RecordingEnvironment::sessionSignal() { d->transition("session_signal"); }
void RecordingEnvironment::sessionProperties(const QString &, const QVariantMap &, const QStringList &) { d->transition("session_properties"); }

} // namespace replay
