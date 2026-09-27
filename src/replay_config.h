#pragma once

#include "exclusion_presets.h"
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

namespace replay {

struct ReplayPaths {
    QString configFile, historyDirectory, stateDirectory, cacheDirectory, runtimeDirectory;
};
ReplayPaths replayPaths();

// Nonempty matchers are ANDed; rules are ORed. All rules cover the selected
// output, including unfocused visible windows. Addresses are session-specific.
struct WindowExclusion {
    QString titleRegex, appId, address;
    QString scope = "output";
    QString compositorInstance;
};

struct ReplayConfig {
    QString output, outputIdentity;
    // Empty uses the XDG history directory. Custom folders must already exist.
    QString storageDirectory;
    double intervalSeconds = 5;
    int retentionDays = 30;
    qint64 maxDiskMiB = 10240, minFreeMiB = 1024;
    double activeCpuPercent = 40, idleCpuPercent = 50, requestCpuPercent = 50;
    double pressureCpuPercent = 10, cpuCeilingPercent = 60;
    int idleSeconds = 60;
    bool loginStartup = false;
    QStringList excludedApps = defaultAppExclusions();
    QStringList skippedApps = defaultSkippedApps();
    QVector<WindowExclusion> excludedWindows;
    QString preferredAgent;
    bool meetingsEnabled = false;
    // Tesseract languages joined by + for background OCR indexing, e.g.
    // "eng+fra". Empty falls back to "eng", like OMARCHY_OCR_LANGS.
    QString ocrLanguages = "eng";
    // Empty uses Meeting Recorder's default ~/Documents/Meetings folder.
    QString meetingsDirectory;
};

QString replayHistoryDirectory(const ReplayConfig& config);
QString replayMeetingsDirectory(const ReplayConfig& config);
// Detection only: never execute the optional recorder to inspect its presence.
bool meetingRecorderAvailable();

struct ReplayConfigDocument {
    ReplayConfig config;
    QByteArray original;
    bool exists = false;
};

// Missing files return defaults without creating directories. Invalid values or
// malformed TOML throw; coordinators can retain their previous valid snapshot.
ReplayConfigDocument loadReplayConfig(const QString& path = replayPaths().configFile);
struct ReplayConfigResolution {
    ReplayConfigDocument document;
    QString configError;
    bool usingLastValidConfig = false;
};
// Read-only history/config discovery. Invalid current TOML uses a valid saved
// coordinator snapshot; without one this throws instead of choosing defaults.
// Settings editors must use loadReplayConfig to preserve the current file.
ReplayConfigResolution resolveReplayConfig();
void validateReplayConfig(const ReplayConfig& config);
// Atomic, private write. Refuses concurrent changes and preserves unknown TOML
// values. Callers must explicitly confirm destructive retention changes first.
void saveReplayConfig(const ReplayConfig& config, const QByteArray& expectedOriginal,
                      const QString& path = replayPaths().configFile);

}  // namespace replay
