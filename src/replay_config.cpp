#include "replay_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <toml++/toml.hpp>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>

namespace replay {
namespace {
constexpr qint64 maxConfigBytes = 256 * 1024;

[[noreturn]] void invalid(const QString& message) {
    throw std::runtime_error(("Replay settings: " + message).toStdString());
}

QString xdg(const char* name, const QString& fallback) {
    const QString value = qEnvironmentVariable(name);
    return !value.isEmpty() && QDir::isAbsolutePath(value) ? QDir::cleanPath(value) : fallback;
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) invalid("cannot read " + path + ": " + file.errorString());
    const QByteArray bytes = file.read(maxConfigBytes + 1);
    if (bytes.size() > maxConfigBytes) invalid("config.toml exceeds 256 KiB");
    return bytes;
}

toml::table parse(const QByteArray& bytes) {
    try { return toml::parse(std::string_view(bytes.constData(), size_t(bytes.size()))); }
    catch (const toml::parse_error& error) {
        // The parser description and location are useful without echoing the
        // offending source line (which may contain a private exclusion rule).
        std::ostringstream description;
        description << error.description() << " (line " << error.source().begin.line << ")";
        invalid(QString::fromStdString(description.str()));
    }
}

const toml::table* section(const toml::table& table, const char* name) {
    const auto* node = table.get(name);
    if (!node) return nullptr;
    if (!node->is_table()) invalid(QString::fromUtf8(name) + " must be a table");
    return node->as_table();
}

template<typename T> void read(const toml::table* table, const char* key, T& destination) {
    if (!table || !table->contains(key)) return;
    const auto* node = table->get(key);
    bool correctType = false;
    if constexpr (std::is_same_v<T, bool>) correctType = node->is_boolean();
    else if constexpr (std::is_integral_v<T>) correctType = node->is_integer();
    else if constexpr (std::is_floating_point_v<T>) correctType = node->is_number();
    if (!correctType) invalid(QString::fromUtf8(key) + " has the wrong type");
    auto value = node->value<T>();
    if (!value) invalid(QString::fromUtf8(key) + " has the wrong type");
    destination = *value;
}

void read(const toml::table* table, const char* key, QString& destination) {
    if (!table || !table->contains(key)) return;
    const auto value = table->get(key)->value<std::string>();
    if (!value) invalid(QString::fromUtf8(key) + " must be a string");
    destination = QString::fromStdString(*value);
}

void read(const toml::table* table, const char* key, QStringList& destination) {
    if (!table || !table->contains(key)) return;
    const auto* apps = table->get(key)->as_array();
    if (!apps) invalid(QString("exclusions.%1 must be an array of exact app identifiers").arg(key));
    destination.clear();
    for (const auto& app : *apps) {
        const auto name = app.value<std::string>();
        if (!name) invalid(QString("exclusions.%1 identifiers must be strings").arg(key));
        destination.append(QString::fromStdString(*name));
    }
}

void read(const toml::table* table, const char* key, int& destination) {
    if (!table || !table->contains(key)) return;
    if (!table->get(key)->is_integer()) invalid(QString::fromUtf8(key) + " must be an integer");
    const auto value = table->get(key)->value<int64_t>();
    if (!value || *value < 0 || *value > 10000000) invalid(QString::fromUtf8(key) + " must be a bounded integer");
    destination = int(*value);
}

toml::table& writableSection(toml::table& table, const char* name) {
    if (!table.contains(name)) table.insert(name, toml::table{});
    auto* child = table.get_as<toml::table>(name);
    if (!child) invalid(QString::fromUtf8(name) + " must be a table");
    return *child;
}

void bounded(double value, double low, double high, const QString& key) {
    if (!std::isfinite(value) || value < low || value > high)
        invalid(QString("%1 must be between %2 and %3").arg(key).arg(low).arg(high));
}

void shortText(const QString& value, int limit, const QString& key) {
    if (value.size() > limit || value.contains(QChar::Null) || value.contains('\n') || value.contains('\r'))
        invalid(key + " must be a single bounded line");
}
}  // namespace

ReplayPaths replayPaths() {
    const QString home = QDir::homePath();
    return {xdg("XDG_CONFIG_HOME", home + "/.config") + "/omarchy-replay/config.toml",
            xdg("XDG_DATA_HOME", home + "/.local/share") + "/omarchy-replay/history",
            xdg("XDG_STATE_HOME", home + "/.local/state") + "/omarchy-replay",
            xdg("XDG_CACHE_HOME", home + "/.cache") + "/omarchy-replay",
            xdg("XDG_RUNTIME_DIR", QDir::tempPath() + "/omarchy-replay-user-" + QString::number(getuid())) + "/omarchy-replay"};
}

QString replayHistoryDirectory(const ReplayConfig& config) {
    return config.storageDirectory.isEmpty() ? replayPaths().historyDirectory : QDir::cleanPath(config.storageDirectory);
}

QString replayMeetingsDirectory(const ReplayConfig& config) {
    return config.meetingsDirectory.isEmpty() ? QDir::homePath() + "/Documents/Meetings" : config.meetingsDirectory;
}

bool meetingRecorderAvailable() {
    return !QStandardPaths::findExecutable("omarchy-meeting-recorder").isEmpty();
}

void validateReplayConfig(const ReplayConfig& config) {
    shortText(config.meetingsDirectory, 4096, "meetings directory");
    if (!config.meetingsDirectory.isEmpty() && (!QDir::isAbsolutePath(config.meetingsDirectory) ||
        QDir::cleanPath(config.meetingsDirectory) != config.meetingsDirectory || config.meetingsDirectory == "/"))
        invalid("meetings directory must be an absolute folder path without a trailing slash, . or ..");
    shortText(config.storageDirectory, 4096, "storage directory");
    if (!config.storageDirectory.isEmpty() && (!QDir::isAbsolutePath(config.storageDirectory) ||
        QDir::cleanPath(config.storageDirectory) != config.storageDirectory || config.storageDirectory == "/"))
        invalid("storage directory must be an absolute folder path without a trailing slash, . or ..");
    shortText(config.output, 256, "output");
    shortText(config.outputIdentity, 1024, "output_identity");
    if (config.displayMode != "fixed" && config.displayMode != "focused")
        invalid("display_mode must be fixed or focused");
    shortText(config.preferredAgent, 256, "preferred agent");
    bounded(config.intervalSeconds, .25, 60, "interval_seconds");
    bounded(config.retentionDays, 1, 3650, "retention_days");
    bounded(config.maxDiskMiB, 64, 1048576, "max_disk_mib");
    bounded(config.minFreeMiB, 0, 1048576, "min_free_mib");
    bounded(config.activeCpuPercent, 1, 100, "active_cpu_percent");
    bounded(config.idleCpuPercent, 1, 100, "idle_cpu_percent");
    bounded(config.requestCpuPercent, 1, 100, "request_cpu_percent");
    bounded(config.pressureCpuPercent, 1, config.activeCpuPercent, "pressure_cpu_percent");
    bounded(config.cpuCeilingPercent, 0, 100, "cpu_ceiling_percent");
    if (config.cpuCeilingPercent > 0 && config.cpuCeilingPercent < 1) invalid("cpu_ceiling_percent must be 0 or at least 1");
    bounded(config.idleSeconds, 1, 3600, "idle_seconds");
    if (config.excludedApps.size() + config.skippedApps.size() > 64 || config.excludedWindows.size() > 64)
        invalid("at most 64 combined apps and skip_apps entries and 64 window rules are allowed");
    for (const auto& app : config.excludedApps + config.skippedApps) {
        shortText(app, 256, "excluded app");
        if (app.isEmpty()) invalid("excluded app identifiers cannot be empty");
    }
    for (const auto& rule : config.excludedWindows) {
        shortText(rule.appId, 256, "window app_id");
        shortText(rule.titleRegex, 512, "window title_regex");
        shortText(rule.compositorInstance, 256, "window compositor_instance");
        if (rule.scope != "output") invalid("window rule scope must be output");
        if (rule.appId.isEmpty() && rule.titleRegex.isEmpty() && rule.address.isEmpty()) invalid("window rule needs at least one matcher");
        if (!rule.titleRegex.isEmpty() && !QRegularExpression(rule.titleRegex).isValid()) invalid("window title_regex is invalid");
        if (!rule.address.isEmpty() && !QRegularExpression("^0x[0-9a-f]{1,16}$").match(rule.address).hasMatch())
            invalid("window address must be a lowercase 0x hexadecimal address");
        if (!rule.address.isEmpty() && rule.compositorInstance.isEmpty())
            invalid("window address needs compositor_instance; reselect the window in this desktop session");
        if (!rule.address.isEmpty() && rule.appId.isEmpty() && rule.titleRegex.isEmpty())
            invalid("window address also needs app_id or title_regex so matching windows can be hidden from captures");
    }
}

ReplayConfigDocument loadReplayConfig(const QString& path) {
    ReplayConfigDocument document;
    document.exists = QFileInfo::exists(path);
    document.original = readFile(path);
    const auto table = parse(document.original);
    auto& config = document.config;
    const auto* capture = section(table, "recording");
    read(capture, "output", config.output);
    read(capture, "output_identity", config.outputIdentity);
    read(capture, "display_mode", config.displayMode);
    read(capture, "interval_seconds", config.intervalSeconds);
    const auto* storage = section(table, "storage");
    read(storage, "directory", config.storageDirectory);
    read(storage, "retention_days", config.retentionDays);
    read(storage, "max_disk_mib", config.maxDiskMiB);
    read(storage, "min_free_mib", config.minFreeMiB);
    const auto* indexing = section(table, "indexing");
    read(indexing, "active_cpu_percent", config.activeCpuPercent);
    read(indexing, "idle_cpu_percent", config.idleCpuPercent);
    read(indexing, "request_cpu_percent", config.requestCpuPercent);
    read(indexing, "pressure_cpu_percent", config.pressureCpuPercent);
    read(indexing, "cpu_ceiling_percent", config.cpuCeilingPercent);
    read(indexing, "idle_seconds", config.idleSeconds);
    read(section(table, "service"), "login_startup", config.loginStartup);
    read(section(table, "agent"), "preferred", config.preferredAgent);
    read(section(table, "meetings"), "enabled", config.meetingsEnabled);
    read(section(table, "meetings"), "directory", config.meetingsDirectory);
    if (const auto* exclusions = section(table, "exclusions")) {
        // An existing explicit app list is authoritative. Do not add newly
        // introduced skip defaults or invalidate a full legacy 64-app policy.
        if (exclusions->contains("apps") && !exclusions->contains("skip_apps")) config.skippedApps.clear();
        read(exclusions, "apps", config.excludedApps);
        read(exclusions, "skip_apps", config.skippedApps);
        if (const auto* node = exclusions->get("windows")) {
            const auto* windows = node->as_array();
            if (!windows) invalid("exclusions.windows must be an array of tables");
            for (const auto& item : *windows) {
                const auto* window = item.as_table();
                if (!window) invalid("each window exclusion must be a table");
                WindowExclusion rule;
                read(window, "title_regex", rule.titleRegex);
                read(window, "app_id", rule.appId);
                read(window, "scope", rule.scope);
                read(window, "address", rule.address);
                read(window, "compositor_instance", rule.compositorInstance);
                rule.address = rule.address.toLower();
                config.excludedWindows.append(rule);
            }
        }
    }
    validateReplayConfig(config);
    return document;
}

ReplayConfigResolution resolveReplayConfig() {
    ReplayConfigResolution result;
    try { result.document = loadReplayConfig(); return result; }
    catch (const std::exception& error) { result.configError = QString::fromUtf8(error.what()); }
    try {
        auto fallback = loadReplayConfig(replayPaths().stateDirectory + "/last-valid-config.toml");
        if (!fallback.exists) throw std::runtime_error("Saved configuration is missing");
        result.document = std::move(fallback);
    } catch (const std::exception&) {
        throw std::runtime_error((result.configError + "; no valid saved configuration is available").toStdString());
    }
    result.usingLastValidConfig = true;
    return result;
}

void saveReplayConfig(const ReplayConfig& config, const QByteArray& expectedOriginal, const QString& path) {
    validateReplayConfig(config);
    auto table = parse(expectedOriginal);
    const auto* originalCapture = section(table, "recording");
    const bool hadDisplayMode = originalCapture && originalCapture->contains("display_mode");
    bool previouslyEnabled = false;
    read(section(table, "meetings"), "enabled", previouslyEnabled);
    if (config.meetingsEnabled && !previouslyEnabled && !meetingRecorderAvailable())
        invalid("install Omarchy Meeting Recorder before enabling meeting transcripts");
    auto& capture = writableSection(table, "recording");
    capture.insert_or_assign("output", config.output.toStdString());
    capture.insert_or_assign("output_identity", config.outputIdentity.toStdString());
    // Existing files that never had the key stay byte-stable at the default.
    if (config.displayMode != "fixed" || hadDisplayMode)
        capture.insert_or_assign("display_mode", config.displayMode.toStdString());
    capture.insert_or_assign("interval_seconds", config.intervalSeconds);
    auto& storage = writableSection(table, "storage");
    storage.insert_or_assign("directory", config.storageDirectory.toStdString());
    storage.insert_or_assign("retention_days", config.retentionDays);
    storage.insert_or_assign("max_disk_mib", int64_t(config.maxDiskMiB));
    storage.insert_or_assign("min_free_mib", int64_t(config.minFreeMiB));
    auto& indexing = writableSection(table, "indexing");
    indexing.insert_or_assign("active_cpu_percent", config.activeCpuPercent);
    indexing.insert_or_assign("idle_cpu_percent", config.idleCpuPercent);
    indexing.insert_or_assign("request_cpu_percent", config.requestCpuPercent);
    indexing.insert_or_assign("pressure_cpu_percent", config.pressureCpuPercent);
    indexing.insert_or_assign("cpu_ceiling_percent", config.cpuCeilingPercent);
    indexing.insert_or_assign("idle_seconds", config.idleSeconds);
    writableSection(table, "service").insert_or_assign("login_startup", config.loginStartup);
    writableSection(table, "agent").insert_or_assign("preferred", config.preferredAgent.toStdString());
    auto& meetings = writableSection(table, "meetings");
    meetings.insert_or_assign("enabled", config.meetingsEnabled);
    meetings.insert_or_assign("directory", config.meetingsDirectory.toStdString());
    auto& exclusions = writableSection(table, "exclusions");
    toml::array apps;
    for (const auto& app : config.excludedApps) apps.push_back(app.toStdString());
    exclusions.insert_or_assign("apps", std::move(apps));
    toml::array skippedApps;
    for (const auto& app : config.skippedApps) skippedApps.push_back(app.toStdString());
    exclusions.insert_or_assign("skip_apps", std::move(skippedApps));
    toml::array windows;
    const auto* oldWindows = exclusions.get_as<toml::array>("windows");
    QVector<bool> reused(oldWindows ? oldWindows->size() : 0, false);
    for (qsizetype i = 0; i < config.excludedWindows.size(); ++i) {
        const auto& rule = config.excludedWindows[i];
        toml::table window;
        if (oldWindows) for (size_t j = 0; j < oldWindows->size(); ++j) {
            const auto* candidate = oldWindows->get(j)->as_table();
            if (!candidate || reused[j]) continue;
            WindowExclusion old;
            read(candidate, "app_id", old.appId); read(candidate, "title_regex", old.titleRegex);
            read(candidate, "address", old.address); read(candidate, "scope", old.scope);
            read(candidate, "compositor_instance", old.compositorInstance);
            if (old.appId == rule.appId && old.titleRegex == rule.titleRegex && old.address.toLower() == rule.address &&
                old.scope == rule.scope && old.compositorInstance == rule.compositorInstance) {
                window = *candidate; reused[j] = true; break;
            }
        }
        window.insert_or_assign("app_id", rule.appId.toStdString());
        window.insert_or_assign("title_regex", rule.titleRegex.toStdString());
        window.insert_or_assign("address", rule.address.toStdString());
        window.insert_or_assign("scope", rule.scope.toStdString());
        window.insert_or_assign("compositor_instance", rule.compositorInstance.toStdString());
        windows.push_back(std::move(window));
    }
    if (oldWindows) for (size_t j = 0; j < oldWindows->size(); ++j) {
        if (reused[j]) continue;
        if (const auto* old = oldWindows->get(j)->as_table()) for (const auto& [key, value] : *old) {
            Q_UNUSED(value);
            if (key != "app_id" && key != "title_regex" && key != "address" && key != "scope" && key != "compositor_instance")
                invalid("a changed window rule contains settings from a newer version; edit that rule in config.toml to preserve those values");
        }
    }
    exclusions.insert_or_assign("windows", std::move(windows));
    std::ostringstream formatted;
    formatted << table;
    const QByteArray bytes = QByteArray::fromStdString(formatted.str()) + '\n';
    if (bytes.size() > maxConfigBytes) invalid("resulting config.toml exceeds 256 KiB");
    const QString parent = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(parent)) invalid("cannot create settings directory");
    if (!QFile::setPermissions(parent, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) invalid("cannot make settings directory private");
    QLockFile lock(path + ".lock");
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(0)) invalid("settings are being changed; try again");
    if (readFile(path) != expectedOriginal) invalid("settings changed on disk; reopen Settings before saving");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) invalid("cannot save settings atomically: " + file.errorString());
}

}  // namespace replay
