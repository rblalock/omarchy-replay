#pragma once

#include "replay_config.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <functional>
#include <memory>

namespace replay {

struct EnvironmentOptions {
    QString output;
    // Persist the first accepted identity with the recording configuration.
    // An empty identity is pinned to the first verified selected output.
    QString outputIdentity;
    // Follow the focused monitor instead of the configured output. The
    // configured output and identity are then ignored for selection.
    bool followFocus = false;
    QStringList excludedApps = defaultAppExclusions();
    QStringList skippedApps = defaultSkippedApps();
    QVector<WindowExclusion> excludedWindows;
    // Successful compositor installer receipt; verified again before retention.
    QString exclusionMaskToken;
};

// Raw, bounded observations are also an injection seam for synthetic tests.
// Unknown inputs always prevent capture. This contains no captured pixels.
struct EnvironmentObservation {
    bool compositorAvailable = false;
    bool lockNotificationsAvailable = false;
    bool compositorLockKnown = false;
    bool compositorLocked = true;
    bool sessionKnown = false;
    bool sessionActive = false;
    bool sessionLocked = true;
    bool sleepKnown = false;
    bool sleeping = true;
    bool shuttingDown = false;
    bool configKnown = false;
    bool configError = true;
    bool exclusionsVerified = false;
    QString compositorInstance;
    QString waylandDisplay;
    quint64 eventGeneration = 0;
    quint64 configGeneration = 0;
    QJsonArray monitors;
    QJsonArray windows;
};

struct EnvironmentSnapshot {
    bool captureAllowed = false;
    QString reason = "environment_unknown";
    QString detail = "Waiting for verified desktop state.";
    quint64 generation = 0;
    quint64 configGeneration = 0;
    QString outputIdentity;
    // Display chosen by this snapshot: the configured output in fixed mode, the
    // focused monitor in focus mode. Empty when no selection succeeded.
    QString selectedOutput;
    QString compositorInstance;
    QString waylandDisplay;
    // Private local UI data only; do not put window titles in numeric reports.
    QJsonArray visibleWindows;
    QStringList excludedApps;
    QJsonObject json() const;
};

// Use on the owning Qt thread. Call snapshot before capture and again before
// retention; require both allowed and the same generation. The monitor never
// changes recording intent, desktop configuration, or screen contents.
class RecordingEnvironment final : public QObject {
    Q_OBJECT
public:
    using Source = std::function<EnvironmentObservation()>;
    explicit RecordingEnvironment(QObject *parent = nullptr);
    explicit RecordingEnvironment(Source source, QObject *parent = nullptr);
    ~RecordingEnvironment() override;
    void configure(const EnvironmentOptions &options);
    EnvironmentSnapshot snapshot();
    quint64 generation() const;
    // Explicit, temporary diagnostics only. No metadata values or history are
    // emitted. Disabling freezes the counters; enabling starts a fresh sample.
    void setDiagnosticsEnabled(bool enabled);
    QJsonObject diagnostics() const;
    // Hash of compositor-provided make/model/serial, or description fallback.
    static QString monitorIdentity(const QJsonObject &monitor);
    static QString validateOptions(const EnvironmentOptions &options);
    static bool invalidatingEvent(const QByteArray &event);

private slots:
    void prepareForSleep(bool sleeping);
    void prepareForShutdown(bool shuttingDown);
    void sessionSignal();
    void sessionProperties(const QString &, const QVariantMap &, const QStringList &);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace replay
