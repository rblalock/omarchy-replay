#include "replay_config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <limits>

namespace {
void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) qFatal("Cannot write config fixture");
}
QByteArray contents(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
class Environment {
public:
    void set(const char* key, const QByteArray& value) {
        if (!saved_.contains(key)) saved_.insert(key, {qEnvironmentVariableIsSet(key), qgetenv(key)});
        qputenv(key, value);
    }
    ~Environment() {
        for (auto item = saved_.cbegin(); item != saved_.cend(); ++item)
            if (item.value().first) qputenv(item.key().constData(), item.value().second); else qunsetenv(item.key().constData());
    }
private:
    QMap<QByteArray, QPair<bool, QByteArray>> saved_;
};
}

class ReplayConfigTest final : public QObject {
    Q_OBJECT
private slots:
    void xdgDefaultsAreReadOnly() {
        QTemporaryDir directory;
        Environment environment;
        for (const auto* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
            environment.set(name, directory.filePath(name).toUtf8());
        const auto paths = replay::replayPaths();
        QCOMPARE(paths.configFile, directory.filePath("XDG_CONFIG_HOME/omarchy-replay/config.toml"));
        QCOMPARE(paths.historyDirectory, directory.filePath("XDG_DATA_HOME/omarchy-replay/history"));
        QCOMPARE(paths.stateDirectory, directory.filePath("XDG_STATE_HOME/omarchy-replay"));
        QCOMPARE(paths.cacheDirectory, directory.filePath("XDG_CACHE_HOME/omarchy-replay"));
        QCOMPARE(paths.runtimeDirectory, directory.filePath("XDG_RUNTIME_DIR/omarchy-replay"));
        const auto document = replay::loadReplayConfig();
        QVERIFY(!document.exists); QVERIFY(document.original.isEmpty());
        QCOMPARE(document.config.retentionDays, 30); QCOMPARE(document.config.cpuCeilingPercent, 60.);
        QVERIFY(document.config.output.isEmpty()); QVERIFY(!document.config.loginStartup);
        QVERIFY(!document.config.meetingsEnabled); QVERIFY(document.config.meetingsDirectory.isEmpty());
        QCOMPARE(replay::replayMeetingsDirectory(document.config), QDir::homePath() + "/Documents/Meetings");
        QCOMPARE(document.config.excludedApps, replay::defaultAppExclusions());
        QCOMPARE(document.config.skippedApps, QStringList({"steam", "Steam"}));
        QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 0);
        environment.set("XDG_CONFIG_HOME", "relative-is-not-xdg");
        QCOMPARE(replay::replayPaths().configFile, QDir::homePath() + "/.config/omarchy-replay/config.toml");
    }

    void exclusionDefaultsRespectExplicitAppLists() {
        QTemporaryDir directory;
        const QString path = directory.filePath("config.toml");
        for (const auto &source : {QByteArray(), QByteArray("[exclusions]\n")}) {
            write(path, source);
            const auto config = replay::loadReplayConfig(path).config;
            for (const auto &app : replay::privacyAppExclusions())
                QVERIFY2(config.excludedApps.contains(app), qPrintable(app));
            QCOMPARE(config.skippedApps, QStringList({"steam", "Steam"}));
            QVERIFY(!config.excludedApps.contains("steam"));
            QVERIFY(!config.excludedApps.contains("Steam"));
            auto optionalApps = replay::gamingAppExclusions() + replay::mediaAppExclusions();
            optionalApps.removeAll("steam"); optionalApps.removeAll("Steam");
            for (const auto &app : optionalApps)
                QVERIFY2(!config.excludedApps.contains(app) && !config.skippedApps.contains(app), qPrintable(app));
        }
        for (const auto &apps : {QByteArray("[]"), QByteArray("['fixture.editor']")}) {
            write(path, "[exclusions]\napps=" + apps + "\n");
            const auto document = replay::loadReplayConfig(path);
            const QStringList expected = apps == "[]" ? QStringList() : QStringList{"fixture.editor"};
            QCOMPARE(document.config.excludedApps, expected);
            QVERIFY(document.config.skippedApps.isEmpty());
            replay::saveReplayConfig(document.config, document.original, path);
            QCOMPARE(replay::loadReplayConfig(path).config.excludedApps, expected);
            QVERIFY(replay::loadReplayConfig(path).config.skippedApps.isEmpty());
        }
    }

    void skipAppsAreIndependentAndExplicitListsStayAuthoritative() {
        QTemporaryDir directory;
        const QString path = directory.filePath("config.toml");
        write(path, "[exclusions]\napps=['mpv', 'Steam']\n");
        const auto legacy = replay::loadReplayConfig(path);
        QCOMPARE(legacy.config.excludedApps, QStringList({"mpv", "Steam"}));
        QVERIFY(legacy.config.skippedApps.isEmpty());
        replay::saveReplayConfig(legacy.config, legacy.original, path);
        QCOMPARE(replay::loadReplayConfig(path).config.excludedApps, legacy.config.excludedApps);
        write(path, "[exclusions]\napps=['mpv', 'Steam']\nskip_apps=['mpv', 'vlc']\nfuture_option=7\n");
        const auto original = replay::loadReplayConfig(path);
        QCOMPARE(original.config.excludedApps, QStringList({"mpv", "Steam"}));
        QCOMPARE(original.config.skippedApps, QStringList({"mpv", "vlc"}));
        replay::saveReplayConfig(original.config, original.original, path);
        const auto saved = replay::loadReplayConfig(path);
        QCOMPARE(saved.config.excludedApps, original.config.excludedApps);
        QCOMPARE(saved.config.skippedApps, original.config.skippedApps);
        QVERIFY(saved.original.contains("future_option = 7"));
        write(path, "[exclusions]\napps=[]\nskip_apps=[]\n");
        const auto empty = replay::loadReplayConfig(path);
        QVERIFY(empty.config.excludedApps.isEmpty());
        QVERIFY(empty.config.skippedApps.isEmpty());
        replay::saveReplayConfig(empty.config, empty.original, path);
        QVERIFY(replay::loadReplayConfig(path).config.skippedApps.isEmpty());

        replay::ReplayConfig bounded;
        bounded.excludedApps.clear(); bounded.skippedApps.clear();
        for (int i = 0; i < 32; ++i) {
            bounded.excludedApps.append(QString("fixture.strict.%1").arg(i));
            bounded.skippedApps.append(QString("fixture.skip.%1").arg(i));
        }
        replay::validateReplayConfig(bounded);
        bounded.skippedApps.append("fixture.extra");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::validateReplayConfig(bounded));
        bounded.skippedApps.removeLast(); bounded.skippedApps[0] = bounded.excludedApps[0];
        replay::validateReplayConfig(bounded);
        bounded.skippedApps.append(bounded.excludedApps[0]);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::validateReplayConfig(bounded));

        QStringList legacyIds;
        for (int i = 0; i < 64; ++i) legacyIds.append(QString("'fixture.legacy.%1'").arg(i));
        write(path, "[exclusions]\napps=[" + legacyIds.join(',').toUtf8() + "]\n");
        const auto oldFull = replay::loadReplayConfig(path);
        QCOMPARE(oldFull.config.excludedApps.size(), 64);
        QVERIFY(oldFull.config.skippedApps.isEmpty());
        replay::saveReplayConfig(oldFull.config, oldFull.original, path);
        QCOMPARE(replay::loadReplayConfig(path).config.excludedApps, oldFull.config.excludedApps);
        QVERIFY(replay::loadReplayConfig(path).config.skippedApps.isEmpty());
    }

    void ocrLanguagesValidateAndRoundTrip() {
        QTemporaryDir directory;
        const QString path = directory.filePath("config.toml");
        write(path, "[indexing]\nocr_languages = 'eng+fra'\n");
        const auto document = replay::loadReplayConfig(path);
        QCOMPARE(document.config.ocrLanguages, "eng+fra");
        replay::saveReplayConfig(document.config, document.original, path);
        QCOMPARE(replay::loadReplayConfig(path).config.ocrLanguages, "eng+fra");
        write(path, "[indexing]\nocr_languages = 'eng fre'\n");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::loadReplayConfig(path));
        write(path, "[indexing]\nocr_languages = 'eng;;fra'\n");
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::loadReplayConfig(path));
        write(path, "[indexing]\nocr_languages = ''\n");
        // An empty value is accepted: it falls back to eng at the OCR engine,
        // like an unset OMARCHY_OCR_LANGS in selection OCR.
        QCOMPARE(replay::loadReplayConfig(path).config.ocrLanguages, "");
    }

    void realTomlAndUnknownValuesSurviveSave() {
        QTemporaryDir directory;
        const QString path = directory.filePath("config.toml");
        write(path, R"TOML(
version_from_future = 9
[recording]
output = 'DP-3'
output_identity = 'synthetic serial'
interval_seconds = 2.5
future_option = { nested = [1, 2, 3] }
[storage]
retention_days = 45
max_disk_mib = 4096
min_free_mib = 256
[exclusions]
apps = ['org.example.Passwords', 'name#literal']
skip_apps = ['fixture.player', 'literal#skip']
[[exclusions.windows]]
app_id = 'org.example.Browser'
title_regex = 'Private\s+window'
address = '0xABC12'
compositor_instance = 'synthetic-desktop-instance'
scope = 'output'
future_rule_value = true
[agent]
preferred = 'synthetic-agent'
)TOML");
        auto document = replay::loadReplayConfig(path);
        QVERIFY(document.exists);
        QCOMPARE(document.config.output, "DP-3"); QCOMPARE(document.config.intervalSeconds, 2.5);
        QCOMPARE(document.config.outputIdentity, "synthetic serial");
        QCOMPARE(document.config.excludedApps[1], "name#literal");
        QCOMPARE(document.config.skippedApps, QStringList({"fixture.player", "literal#skip"}));
        QCOMPARE(document.config.excludedWindows[0].titleRegex, "Private\\s+window");
        QCOMPARE(document.config.excludedWindows[0].address, "0xabc12");
        document.config.intervalSeconds = 7.5;
        replay::saveReplayConfig(document.config, document.original, path);
        const auto reread = replay::loadReplayConfig(path);
        QCOMPARE(reread.config.intervalSeconds, 7.5); QCOMPARE(reread.config.retentionDays, 45);
        QCOMPARE(reread.config.excludedWindows[0].address, "0xabc12");
        QCOMPARE(reread.config.skippedApps, document.config.skippedApps);
        QVERIFY(contents(path).contains("version_from_future = 9"));
        QVERIFY(contents(path).contains("future_option"));
        QVERIFY(contents(path).contains("future_rule_value = true"));
        QVERIFY(!(QFileInfo(path).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther)));
        auto changedRule = reread.config;
        changedRule.excludedWindows[0].titleRegex = "Different title";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::saveReplayConfig(changedRule, reread.original, path));
        QCOMPARE(contents(path), reread.original);
    }

    void customHistoryPathRoundTrip() {
        QTemporaryDir directory;
        const QString path = directory.filePath("config.toml");
        replay::ReplayConfig config;
        QCOMPARE(replay::replayHistoryDirectory(config), replay::replayPaths().historyDirectory);
        config.storageDirectory = directory.filePath("separate-disk-history");
        QVERIFY(QDir().mkdir(config.storageDirectory));
        replay::saveReplayConfig(config, {}, path);
        const auto loaded = replay::loadReplayConfig(path);
        QCOMPARE(loaded.config.storageDirectory, config.storageDirectory);
        QCOMPARE(replay::replayHistoryDirectory(loaded.config), config.storageDirectory);
        QVERIFY(QDir(config.storageDirectory).isEmpty());
        // A disconnected drive is valid configuration; availability is a runtime
        // gate so a restart cannot silently select the default archive instead.
        QVERIFY(QDir().rmdir(config.storageDirectory));
        QCOMPARE(replay::loadReplayConfig(path).config.storageDirectory, config.storageDirectory);
        for (const auto &invalid : QStringList{"relative/history", "/", "/tmp/history/", "/tmp/../history", "~/history"}) {
            auto changed = config; changed.storageDirectory = invalid;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::saveReplayConfig(changed, loaded.original, path));
            QCOMPARE(contents(path), loaded.original);
        }
    }

    void optionalMeetingsRoundTripAndDetection() {
        QTemporaryDir directory;
        Environment environment;
        environment.set("PATH", directory.path().toUtf8());
        QVERIFY(!replay::meetingRecorderAvailable());
        const QString path = directory.filePath("config.toml");
        replay::ReplayConfig config;
        config.meetingsDirectory = directory.filePath("meeting-sources");
        replay::saveReplayConfig(config, {}, path);
        write(path, contents(path).replace("[meetings]\n", "[meetings]\nfuture_option = 'keep me'\n"));
        auto saved = replay::loadReplayConfig(path);
        QCOMPARE(saved.config.meetingsDirectory, config.meetingsDirectory);
        QCOMPARE(replay::replayMeetingsDirectory(saved.config), config.meetingsDirectory);
        config.meetingsEnabled = true;
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::saveReplayConfig(config, saved.original, path));
        QCOMPARE(contents(path), saved.original);

        const QString executable = directory.filePath("omarchy-meeting-recorder");
        const QString marker = directory.filePath("must-not-run");
        write(executable, "#!/bin/sh\ntouch '" + marker.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(replay::meetingRecorderAvailable());
        QVERIFY(!QFileInfo::exists(marker));
        replay::saveReplayConfig(config, saved.original, path);
        saved = replay::loadReplayConfig(path);
        QVERIFY(saved.config.meetingsEnabled);
        QVERIFY(saved.original.contains("future_option"));
        QVERIFY(!QFileInfo::exists(marker));

        // Removing the optional recorder must not invalidate unrelated settings
        // or reset the user's saved integration choice.
        QVERIFY(QFile::remove(executable));
        QVERIFY(!replay::meetingRecorderAvailable());
        QVERIFY(replay::loadReplayConfig(path).config.meetingsEnabled);
        saved.config.intervalSeconds = 6;
        replay::saveReplayConfig(saved.config, saved.original, path);
        saved = replay::loadReplayConfig(path);
        QVERIFY(saved.config.meetingsEnabled);
        write(path, saved.original + "\n[future_integration]\nkeep = true\n");
        saved = replay::loadReplayConfig(path);
        saved.config.meetingsEnabled = false;
        replay::saveReplayConfig(saved.config, saved.original, path);
        QVERIFY(contents(path).contains("keep = true"));
        saved = replay::loadReplayConfig(path);
        for (const auto& invalid : QStringList{"relative/meetings", "/", "/tmp/meetings/", "/tmp/../meetings", "~/Meetings"}) {
            auto changed = saved.config; changed.meetingsDirectory = invalid;
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::saveReplayConfig(changed, saved.original, path));
            QCOMPARE(contents(path), saved.original);
        }
    }

    void invalidCurrentConfigUsesSavedHistoryWithoutWriting() {
        QTemporaryDir directory;
        Environment environment;
        for (const auto* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
            environment.set(name, directory.filePath(name).toUtf8());
        const auto paths = replay::replayPaths();
        QVERIFY(QDir().mkpath(QFileInfo(paths.configFile).dir().absolutePath()));
        const QByteArray invalid = "[recording]\ninterval_seconds = 'invalid'\n";
        write(paths.configFile, invalid);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::resolveReplayConfig());
        QVERIFY(!QFileInfo::exists(paths.stateDirectory));
        QVERIFY(!QFileInfo::exists(paths.historyDirectory));
        QVERIFY(QDir().mkpath(paths.stateDirectory));
        const QString backup = paths.stateDirectory + "/last-valid-config.toml";
        const QString custom = directory.filePath("custom-history");
        const QByteArray valid = "[storage]\ndirectory = '" + custom.toUtf8() + "'\n";
        write(backup, valid);
        const auto resolved = replay::resolveReplayConfig();
        QVERIFY(resolved.usingLastValidConfig); QVERIFY(!resolved.configError.isEmpty());
        QCOMPARE(replay::replayHistoryDirectory(resolved.document.config), custom);
        QCOMPARE(resolved.document.original, valid);
        QCOMPARE(contents(paths.configFile), invalid); QCOMPARE(contents(backup), valid);
        QVERIFY(!QFileInfo::exists(paths.historyDirectory)); QVERIFY(!QFileInfo::exists(custom));
        write(paths.configFile, "[storage]\nretention_days = 7\n");
        const auto current = replay::resolveReplayConfig();
        QVERIFY(!current.usingLastValidConfig); QVERIFY(current.configError.isEmpty());
        QCOMPARE(current.document.config.retentionDays, 7);
        QCOMPARE(replay::replayHistoryDirectory(current.document.config), paths.historyDirectory);
        write(paths.configFile, invalid); write(backup, invalid);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::resolveReplayConfig());
        QCOMPARE(contents(paths.configFile), invalid); QCOMPARE(contents(backup), invalid);
    }

    void invalidSettingsDoNotReplaceSavedConfig() {
        QTemporaryDir directory;
        const QString path = directory.filePath("config.toml");
        const QVector<QByteArray> invalid{
            "[recording]\ninterval_seconds = 0\n", "[storage]\nretention_days = -1\n",
            "[indexing]\nactive_cpu_percent = 'fast'\n", "[indexing]\nidle_cpu_percent = nan\n",
            "[service]\nlogin_startup = 1\n", "[storage]\ndirectory = 5\n", "[exclusions]\napps = [4]\n",
            "[meetings]\nenabled = 1\n", "[meetings]\ndirectory = 5\n", "meetings = true\n",
            "[exclusions]\nskip_apps = [4]\n", "[exclusions]\nskip_apps = 'mpv'\n",
            "[exclusions]\nskip_apps = ['']\n", "[exclusions]\nskip_apps = [\"line\\nbreak\"]\n",
            "[[exclusions.windows]]\ntitle_regex = '['\n", "[[exclusions.windows]]\napp_id = 'private'\nscope = 'focused'\n",
            "[[exclusions.windows]]\naddress = '0x123'\n",
            "[[exclusions.windows]]\naddress = '0x123'\ncompositor_instance = 'synthetic-instance'\n",
            "[recording\noutput = 'DP-1'\n"};
        for (const auto& bytes : invalid) {
            write(path, bytes);
            QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::loadReplayConfig(path));
            QCOMPARE(contents(path), bytes);
        }
        write(path, "[recording]\noutput = 'DP-3'\n");
        const auto valid = replay::loadReplayConfig(path);
        auto bad = valid.config; bad.intervalSeconds = std::numeric_limits<double>::infinity();
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::saveReplayConfig(bad, valid.original, path));
        QCOMPARE(contents(path), valid.original);
        write(path, valid.original + "# changed by another writer\n");
        const auto changed = contents(path);
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, replay::saveReplayConfig(valid.config, valid.original, path));
        QCOMPARE(contents(path), changed);
    }
};

QTEST_GUILESS_MAIN(ReplayConfigTest)
#include "replay_config_test.moc"
