#include "fixture.h"
#include "recorder.h"
#include "viewer.h"
#include "index_service.h"
#include "replay_config.h"
#include "agent_prompt.h"
#include "exclusion_presets.h"
#include "selection_ocr.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QTabWidget>
#include <QMutex>
#include <QWaitCondition>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontMetrics>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPainter>
#include <QProcess>
#include <QSlider>
#include <QScrollBar>
#include <QScrollArea>
#include <QScopeGuard>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QTest>
#include <QWidget>
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace {
class ViewerEnvironment {
public:
    explicit ViewerEnvironment(const QString& root) {
        for (const auto* name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"}) {
            saved_.insert(name, {qEnvironmentVariableIsSet(name), qgetenv(name)});
            qputenv(name, QDir(root).filePath(name).toUtf8());
        }
    }
    ~ViewerEnvironment() {
        for (auto item = saved_.cbegin(); item != saved_.cend(); ++item)
            if (item.value().first) qputenv(item.key().constData(), item.value().second); else qunsetenv(item.key().constData());
    }
private:
    QMap<QByteArray, QPair<bool, QByteArray>> saved_;
};
struct FakeRecording {
    QMutex mutex;
    QStringList actions;
    bool usedGuiThread = false;
    QJsonObject status{{"running", false}, {"intent", "stopped"}, {"state", "offline"}, {"indexing", false}, {"indexing_paused", false}};
    replay::ViewerServiceHooks hooks() {
        return {[this] { QMutexLocker guard(&mutex); usedGuiThread |= QThread::currentThread() == qApp->thread(); return status; },
            [this](const QString& action, const QJsonObject&) {
                QMutexLocker guard(&mutex);
                usedGuiThread |= QThread::currentThread() == qApp->thread(); actions.append(action);
                status["running"] = true;
                if (action == "start" || action == "resume") { status["intent"] = "running"; status["state"] = "recording"; }
                else if (action == "pause") { status["intent"] = "paused"; status["state"] = "paused"; }
                else if (action == "stop") { status["intent"] = "stopped"; status["state"] = "stopped"; }
                else if (action == "index-pause") status["indexing_paused"] = true;
                else if (action == "index-resume") status["indexing_paused"] = false;
                else if (action == "reload") status["history_directory"] = replay::replayHistoryDirectory(replay::loadReplayConfig().config);
                return status;
            }, [] { return QJsonArray{
                QJsonObject{{"name", "SYNTHETIC-1"}, {"model", "Fixture display"}, {"width", 1920}, {"height", 1080}},
                QJsonObject{{"name", "SYNTHETIC-2"}, {"model", "Second fixture display"}, {"width", 2560}, {"height", 1440}}}; }};
    }
    QStringList calls() { QMutexLocker guard(&mutex); return actions; }
};

struct SelectionOcrProbe : std::enable_shared_from_this<SelectionOcrProbe> {
    QMutex mutex;
    QWaitCondition changed;
    QVector<QImage> crops;
    QVector<std::shared_ptr<std::atomic_bool>> cancellations;
    QVector<replay::SelectionOcrResult> replies;
    int released = 0, active = 0, peak = 0;
    bool usedGuiThread = false, honorCancellation = false;
    replay::ViewerServiceHooks hooks() {
        replay::ViewerServiceHooks result;
        result.selectionOcr = [self = shared_from_this()](const QImage& image, const std::shared_ptr<std::atomic_bool>& cancelled) {
            QMutexLocker guard(&self->mutex);
            const int call = self->crops.size();
            self->crops.append(image); self->cancellations.append(cancelled);
            self->usedGuiThread |= QThread::currentThread() == qApp->thread();
            self->peak = std::max(self->peak, ++self->active);
            // Deliberately return success even after cancellation. The viewer
            // must reject stale results independently of backend cooperation.
            QElapsedTimer deadline; deadline.start();
            while (call >= self->released && deadline.elapsed() < 5000 &&
                   !(self->honorCancellation && cancelled && cancelled->load()))
                self->changed.wait(&self->mutex, 20);
            --self->active;
            if (self->honorCancellation && cancelled && cancelled->load())
                return replay::SelectionOcrResult{{}, {}, true};
            return call < self->replies.size() ? self->replies[call]
                : replay::SelectionOcrResult{QString("Selected text %1").arg(call + 1), {}, false};
        };
        return result;
    }
    void release(int count) { QMutexLocker guard(&mutex); released = count; changed.wakeAll(); }
    void cooperateWithCancellation() { QMutexLocker guard(&mutex); honorCancellation = true; }
    int count() { QMutexLocker guard(&mutex); return crops.size(); }
    QImage crop(int call) { QMutexLocker guard(&mutex); return crops.value(call); }
    bool cancelled(int call) { QMutexLocker guard(&mutex); return call < cancellations.size() && cancellations[call]->load(); }
    int maximumActive() { QMutexLocker guard(&mutex); return peak; }
    bool onGuiThread() { QMutexLocker guard(&mutex); return usedGuiThread; }
};
}

// This exercises the GUI with a deterministic index, independently of OCR/model
// accuracy. The CLI feasibility harness verifies OCR against fixtureGroundTruth.
class ViewerTest final : public QObject {
    Q_OBJECT

    static qint64 storedNumber(const QString& directory, const char* sql) {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(QDir(directory).filePath("index.sqlite").toUtf8().constData(),
                            &database, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (database) sqlite3_close(database);
            return -1;
        }
        // Viewer refreshes open SQLite connections asynchronously. Match the
        // production reader's bounded wait instead of treating a transient
        // connection/lock race as a missing fixture row.
        sqlite3_busy_timeout(database, 1000);
        sqlite3_stmt* statement = nullptr;
        qint64 value = -1;
        int status = sqlite3_prepare_v2(database, sql, -1, &statement, nullptr);
        if (status == SQLITE_OK) status = sqlite3_step(statement);
        if (status == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
        else if (status != SQLITE_DONE)
            qWarning("Fixture database query failed (%d): %s", status, sqlite3_errmsg(database));
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return value;
    }

    static QImage prefixScreen(const QString& line, int moment) {
        QImage image(1280, 720, QImage::Format_RGBA8888);
        image.fill(QColor("#111820"));
        QPainter painter(&image);
        painter.fillRect(QRect(0, 0, 1280, 52), QColor("#1c2632"));
        painter.setFont(QFont("monospace", 12));
        painter.setPen(QColor("#a8b5c5"));
        painter.drawText(QRect(32, 0, 1216, 52), Qt::AlignVCenter,
                         QString("SYNTHETIC SEARCH FIXTURE                                      moment %1").arg(moment));
        painter.setFont(QFont("monospace", 28));
        painter.setPen(QColor("#ece8da"));
        painter.drawText(QRect(80, 130, 1120, 60), Qt::AlignVCenter, "Picking up where we left off");
        painter.setFont(QFont("monospace", 22));
        painter.drawText(QRect(80, 240, 1120, 60), Qt::AlignVCenter, line);
        painter.setFont(QFont("monospace", 15));
        painter.setPen(QColor("#a8b5c5"));
        painter.drawText(QRect(80, 344, 1120, 50), Qt::AlignVCenter,
                         "Local notes  /  September 19  /  Personal workspace");
        painter.drawLine(80, 423, 1200, 423);
        painter.drawText(QRect(80, 454, 1120, 50), Qt::AlignVCenter,
                         "The text and coordinates below are deterministic test data.");
        return image;
    }

    static void moveTextSelection(QWidget* canvas, const QPoint& point) {
        QMouseEvent move(QEvent::MouseMove, QPointF(point), QPointF(canvas->mapToGlobal(point)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &move);
    }

    static QRect dragTextSelection(QWidget* canvas, const QPoint& from, const QPoint& to) {
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
        moveTextSelection(canvas, to);
        const QRect selection = canvas->property("textSelectionRect").toRect();
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
        return selection;
    }

    static bool indexText(const QString& directory, qint64 id, const QString& text,
                          const QJsonArray& lines = {}) {
        sqlite3* database = nullptr;
        if (sqlite3_open(QDir(directory).filePath("index.sqlite").toUtf8().constData(), &database) != SQLITE_OK) {
            if (database) sqlite3_close(database);
            return false;
        }
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(database, sqlite3_close);
        for (const char* sql : {"UPDATE frames SET text=?,ocr_state='ready' WHERE id=?",
                               "INSERT OR REPLACE INTO frame_text(text,rowid) VALUES(?,?)",
                               "INSERT OR REPLACE INTO frame_ocr_geometry(lines_json,frame_id) VALUES(?,?)"}) {
            sqlite3_stmt* statement = nullptr;
            if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) return false;
            std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> prepared(statement, sqlite3_finalize);
            const QByteArray value = QByteArray(sql).contains("geometry")
                ? QJsonDocument(lines).toJson(QJsonDocument::Compact) : text.toUtf8();
            if (sqlite3_bind_text(statement, 1, value.constData(), value.size(), SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_bind_int64(statement, 2, id) != SQLITE_OK || sqlite3_step(statement) != SQLITE_DONE) return false;
        }
        return true;
    }

private slots:
    void copiedPromptsDescribeAnInstalledPluginWithoutSourceFiles() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::AgentPromptContext context;
        context.executable = "/opt/Omarchy plugin's folder/$literal/replay";
        context.paths = {"/user/config/omarchy-replay/config.toml", "/user/data/omarchy-replay/history",
            "/user/state/omarchy-replay", "/user/cache/omarchy-replay", "/user/runtime/omarchy-replay"};
        context.historyDirectory = "/mounted disk/replay-history";
        context.shownSettings.activeCpuPercent = 35;
        context.shownSettings.excludedApps = {"PRIVATE_APP_NOT_FOR_PROMPT"};
        context.shownSettings.skippedApps = {"PRIVATE_SKIP_NOT_FOR_PROMPT"};
        context.shownSettings.excludedWindows = {{"PRIVATE_TITLE_NOT_FOR_PROMPT", "", "", "output", ""}};
        for (const auto topic : {replay::AgentPromptTopic::Setup, replay::AgentPromptTopic::Resources,
                                replay::AgentPromptTopic::Exclusions}) {
            const auto prompt = replay::configurationAgentPrompt(topic, context);
            QVERIFY(prompt.contains(context.executable));
            QVERIFY(prompt.contains(context.paths.configFile));
            QVERIFY(prompt.contains(context.historyDirectory));
            QVERIFY(prompt.contains("https://github.com/rblalock/omarchy-replay"));
            QVERIFY(!prompt.contains("/README.md"));
            QVERIFY(!prompt.contains("./scripts/replay"));
            QVERIFY(!prompt.contains("PRIVATE_APP_NOT_FOR_PROMPT"));
            QVERIFY(!prompt.contains("PRIVATE_SKIP_NOT_FOR_PROMPT"));
            QVERIFY(!prompt.contains("PRIVATE_TITLE_NOT_FOR_PROMPT"));
            QVERIFY(prompt.contains("using_last_valid_config"));
            QVERIFY(prompt.contains("If it was offline, leave it offline"));
            QVERIFY(prompt.contains("systemctl --user enable omarchy-replay.service"));
            QVERIFY(prompt.contains("systemctl --user disable omarchy-replay.service"));
            QVERIFY(prompt.contains("without --now"));
            for (const QString& key : {"[recording]", "[storage]", "[indexing]", "[service]", "[exclusions]", "[agent]",
                                      "output_identity", "display_mode", "retention_days", "min_free_mib", "cpu_ceiling_percent",
                                      "title_regex", "compositor_instance"}) QVERIFY(prompt.contains(key));
            // An installed path with spaces, apostrophes and a dollar sign must
            // survive the shell assignment without expansion or a checkout cwd.
            const QString assignment = prompt.section("```sh\n", 1).section('\n', 0, 0);
            QProcess shell;
            shell.setWorkingDirectory(temporary.path());
            shell.start("/bin/sh", {"-c", assignment + "\nprintf '%s' \"$replay_bin\""});
            QVERIFY(shell.waitForFinished(3000)); QCOMPARE(shell.exitCode(), 0);
            QCOMPARE(QString::fromUtf8(shell.readAllStandardOutput()), context.executable);

            const QByteArray reference = prompt.section("```toml\n", 1).section("\n```", 0, 0).toUtf8();
            QVERIFY(!reference.isEmpty());
            const auto fileName = temporary.filePath("prompt-reference.toml");
            QFile file(fileName); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(reference); file.close();
            const auto config = replay::loadReplayConfig(fileName).config;
            const replay::ReplayConfig defaults;
            QCOMPARE(config.intervalSeconds, defaults.intervalSeconds);
            QCOMPARE(config.retentionDays, defaults.retentionDays);
            QCOMPARE(config.maxDiskMiB, defaults.maxDiskMiB); QCOMPARE(config.minFreeMiB, defaults.minFreeMiB);
            QCOMPARE(config.activeCpuPercent, defaults.activeCpuPercent); QCOMPARE(config.idleCpuPercent, defaults.idleCpuPercent);
            QCOMPARE(config.requestCpuPercent, defaults.requestCpuPercent); QCOMPARE(config.pressureCpuPercent, defaults.pressureCpuPercent);
            QCOMPARE(config.cpuCeilingPercent, defaults.cpuCeilingPercent); QCOMPARE(config.idleSeconds, defaults.idleSeconds);
            QCOMPARE(config.loginStartup, defaults.loginStartup); QCOMPARE(config.excludedApps, defaults.excludedApps);
            QCOMPARE(config.skippedApps, defaults.skippedApps);
            if (topic == replay::AgentPromptTopic::Resources) QVERIFY(prompt.contains("active CPU 35%"));
        }
    }

    void sharedHistoryStartsEmptyWithoutRecordingAndKeyboardControlsAreSeparate() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        const QString directory = replay::replayPaths().historyDirectory;
        FakeRecording service;
        auto viewer = replay::createViewer(directory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QCOMPARE(viewer->findChild<QLabel*>("recordedTimestamp")->text(), "Your history starts here");
        QVERIFY(service.calls().isEmpty());
        QVERIFY(!QFileInfo::exists(directory));
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* panel = viewer->findChild<QWidget*>("recordingPanel");
        QVERIFY(panel->isVisible());
        auto* action = viewer->findChild<QPushButton*>("recordingServiceAction");
        auto* stop = viewer->findChild<QPushButton*>("stopRecordingService");
        auto* indexing = viewer->findChild<QPushButton*>("indexServiceAction");
        QCOMPARE(action->text(), "Start recording");
        QVERIFY(!stop->isEnabled());
        action->setFocus(); QTest::keyClick(action, Qt::Key_Space);
        QTRY_COMPARE(action->text(), "Pause recording");
        QTest::keyClick(action, Qt::Key_Space);
        QTRY_COMPARE(action->text(), "Resume recording");
        QTest::keyClick(action, Qt::Key_Space);
        QTRY_COMPARE(action->text(), "Pause recording");
        QTRY_COMPARE(indexing->text(), "Pause indexing");
        indexing->setFocus(); QTest::keyClick(indexing, Qt::Key_Space);
        QTRY_COMPARE(indexing->text(), "Resume indexing");
        QCOMPARE(action->text(), "Pause recording");
        QTest::keyClick(indexing, Qt::Key_Space);
        QTRY_COMPARE(indexing->text(), "Pause indexing");
        stop->setFocus(); QTest::keyClick(stop, Qt::Key_Space);
        QTRY_COMPARE(action->text(), "Start recording");
        QCOMPARE(service.calls(), QStringList({"start", "pause", "resume", "index-pause", "index-resume", "stop"}));
        {
            replay::RecorderOptions options; options.directory = directory; options.ocr = false; options.minFreeBytes = 0;
            replay::Recorder recorder(options);
            recorder.addFrame(replay::fixtureFrame(0), QDateTime::currentMSecsSinceEpoch()); recorder.finish();
        }
        QTRY_VERIFY_WITH_TIMEOUT(viewer->property("displayedFrameId").toLongLong() > 0, 5000);
        QVERIFY(viewer->findChild<QPushButton*>("deleteRecentHistory")->isEnabled());
        bool reviewed = false;
        QTimer::singleShot(0, [&] {
            auto* interval = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
            QVERIFY(interval); interval->setIntValue(2);
            QTimer::singleShot(0, [&] {
                auto* confirmation = viewer->findChild<QMessageBox*>("confirmDeleteRecent");
                QVERIFY(confirmation); reviewed = true;
                QVERIFY(confirmation->text().contains("last 2 minutes"));
                QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
                QTest::keyClick(confirmation, Qt::Key_Escape);
            });
            interval->accept();
        });
        auto* deletion = viewer->findChild<QPushButton*>("deleteRecentHistory");
        deletion->setFocus(); QTest::keyClick(deletion, Qt::Key_Space);
        QVERIFY(reviewed);
        QCOMPARE(service.calls().size(), 6);
        QVERIFY(QDir().mkpath("runs/design-review"));
        QVERIFY(viewer->grab().save("runs/design-review/shared-recording-controls.png"));
        viewer->close();
        QVERIFY(!service.usedGuiThread);
        QCOMPARE(service.calls().size(), 6);
    }

    void storageCapacityUsesServiceForecastAndClearsStaleEstimates() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        const QString directory = replay::replayPaths().historyDirectory;
        FakeRecording service;
        service.status["running"] = true;
        service.status["state"] = "storage-cleanup";
        service.status["history_directory"] = directory;
        service.status["max_disk_mib"] = 10240;
        service.status["usage"] = QJsonObject{{"disk_bytes", 8.0 * 1024 * 1024 * 1024}};
        service.status["storage_forecast"] = QJsonObject{{"state", "ready"}, {"capacity_active_hours", 4.0},
            {"warning", "none"}, {"limiting_factor", "allowance"}, {"limited_sample", false},
            {"estimate_note", "Based on recent saved images; sleep is not active recording time."}};
        auto viewer = replay::createViewer(directory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* capacity = viewer->findChild<QLabel*>("recordingStorageCapacity");
        auto* details = viewer->findChild<QLabel*>("recordingStorageDetails");
        QVERIFY(capacity && capacity->isVisible());
        QVERIFY(details && !details->isVisible());
        QVERIFY(viewer->findChild<QLabel*>("recordingStatus")->text().contains("Rolling out oldest history"));
        QVERIFY(capacity->text().contains("8.0 / 10.0 GiB used"));
        QVERIFY(capacity->text().contains("4.0 active recording hours"));
        QVERIFY(capacity->toolTip().contains("sleep is not active"));
        auto refresh = [&](const QJsonObject& forecast) {
            { QMutexLocker guard(&service.mutex); service.status["storage_forecast"] = forecast; }
            viewer->findChild<QPushButton*>("refreshHistory")->click();
        };
        refresh({{"state", "ready"}, {"capacity_active_hours", .5}, {"warning", "free-space"},
            {"limiting_factor", "free-space"}, {"limited_sample", true}});
        QTRY_VERIFY(capacity->text().contains("0.5 active recording hours"));
        QVERIFY(capacity->text().contains("Limited by available disk space"));
        QVERIFY(details->text().contains("Available disk space reduces this capacity"));
        QVERIFY(details->text().contains("recording continues"));
        QVERIFY(!capacity->text().contains("Capture pauses"));
        refresh({{"state", "insufficient-data"}, {"warning", "none"}, {"limiting_factor", "allowance"}});
        QTRY_VERIFY(capacity->text().contains("More recorded history"));
        QVERIFY(!capacity->text().contains("active recording hours"));
        refresh({{"state", "insufficient-data"}, {"warning", "none"}});
        QTRY_VERIFY(capacity->text().contains("More recorded history"));
        refresh({{"state", "no-growth"}, {"warning", "none"}, {"capacity_active_hours", QJsonValue::Null}});
        QTRY_VERIFY(capacity->text().contains("No recent storage growth"));
        QVERIFY(!capacity->text().contains("0.0 active recording hours"));
        {
            QMutexLocker guard(&service.mutex);
            service.status["running"] = false;
            service.status["storage_forecast"] = QJsonObject{{"state", "ready"}, {"capacity_active_hours", 4.0}};
        }
        viewer->findChild<QPushButton*>("refreshHistory")->click();
        QTRY_VERIFY(capacity->text().contains("service is stopped"));
        QVERIFY(details->text().contains("background service is stopped"));
        QVERIFY(!capacity->text().contains("4.0"));
        {
            QMutexLocker guard(&service.mutex);
            service.status["running"] = true;
            service.status["history_directory"] = temporary.filePath("unavailable-disk/history");
            service.status["storage_available"] = false;
        }
        viewer->findChild<QPushButton*>("refreshHistory")->click();
        QTRY_VERIFY(capacity->text().contains("unavailable for this folder"));
        QVERIFY(details->text().contains("unavailable for this history folder"));
        QVERIFY(!capacity->text().contains("4.0"));
        QCOMPARE(viewer->property("historyDirectory").toString(), directory);
        QVERIFY(service.calls().isEmpty());
        QVERIFY(!QFileInfo::exists(directory));
        viewer->close();
    }

    void settingsCapacityPreviewsSizeAndRetentionWithoutSaving() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        auto document = replay::loadReplayConfig();
        document.config.output = "SYNTHETIC-1";
        replay::saveReplayConfig(document.config, document.original);
        const QByteArray original = replay::loadReplayConfig().original;
        const QString directory = replay::replayHistoryDirectory(document.config);
        FakeRecording service;
        service.status["running"] = true;
        service.status["history_directory"] = directory;
        service.status["max_disk_mib"] = 10240;
        service.status["min_free_mib"] = 1024;
        service.status["usage"] = QJsonObject{{"disk_bytes", 8.0 * 1024 * 1024 * 1024}};
        service.status["storage_forecast"] = QJsonObject{{"state", "ready"}, {"capacity_active_hours", 4.0}, {"warning", "none"},
            {"bytes_per_active_hour", 2.5 * 1024 * 1024 * 1024}, {"filesystem_free_bytes", 32.0 * 1024 * 1024 * 1024},
            {"bytes_per_calendar_day", 1024.0 * 1024 * 1024}, {"calendar_sample_days", 8}};
        auto viewer = replay::createViewer(directory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        bool checked = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings"); QVERIFY(dialog);
            auto* capacity = dialog->findChild<QLabel*>("settingsStorageCapacity"); QVERIFY(capacity);
            auto* details = dialog->findChild<QLabel*>("settingsStorageDetails"); QVERIFY(details);
            QVERIFY(!details->isVisible());
            QVERIFY(capacity->text().contains("About 10.0 days of history"));
            QVERIFY(details->text().contains("4.0 active recording hours"));
            auto* limit = dialog->findChild<QSpinBox*>("settingMaxDiskMiB");
            limit->setValue(20480);
            QVERIFY(capacity->text().contains("About 20.0 days of history"));
            QVERIFY(details->text().contains("8.0 active recording hours"));
            QVERIFY(details->text().contains("About 20.0 days at your observed usage"));
            QVERIFY(!capacity->text().contains("Save recording"));
            limit->setValue(10240);
            QVERIFY(capacity->text().contains("About 10.0 days of history"));
            QVERIFY(details->text().contains("4.0 active recording hours"));
            auto* free = dialog->findChild<QSpinBox*>("settingMinFreeMiB");
            free->setValue(40960);
            QVERIFY(capacity->text().contains("About 0.0 days of history"));
            QVERIFY(details->text().contains("0.0 active recording hours"));
            QVERIFY(capacity->text().contains("Limited by available disk space"));
            QVERIFY(details->text().contains("Available disk space reduces this capacity"));
            free->setValue(1024);
            QVERIFY(capacity->text().contains("About 10.0 days of history"));
            QVERIFY(details->text().contains("4.0 active recording hours"));
            auto* retention = dialog->findChild<QSpinBox*>("settingRetentionDays");
            retention->setValue(14);
            QVERIFY(details->text().contains("14 days would need roughly 14.0 GiB"));
            auto* interval = dialog->findChild<QDoubleSpinBox*>("settingInterval");
            interval->setValue(10);
            QVERIFY(capacity->text().contains("needs its own usage estimate"));
            interval->setValue(document.config.intervalSeconds);
            auto* folder = dialog->findChild<QLineEdit*>("settingStorageDirectory");
            folder->setText(temporary.filePath("other-disk/history"));
            QVERIFY(!capacity->text().contains("4.0"));
            folder->setText(directory);
            QVERIFY(capacity->text().contains("About 10.0 days of history"));
            QVERIFY(details->text().contains("4.0 active recording hours"));
            checked = true;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        viewer->findChild<QPushButton*>("openReplaySettings")->click();
        QVERIFY(checked);
        QCOMPARE(replay::loadReplayConfig().original, original);
        QVERIFY(service.calls().isEmpty());
        viewer->close();
    }

    void compactPanelsKeepStatusAndKeyboardDetailsAccessible() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        auto document = replay::loadReplayConfig();
        document.config.output = "SYNTHETIC-1";
        replay::saveReplayConfig(document.config, document.original);
        const QByteArray original = replay::loadReplayConfig().original;
        const QString directory = replay::replayHistoryDirectory(document.config);
        replay::RecorderOptions options;
        options.directory = directory; options.deferredOcr = true; options.minFreeBytes = 0;
        std::array<qint64, 3> ids{};
        {
            replay::Recorder recorder(options);
            for (int i = 0; i < 3; ++i)
                ids[i] = recorder.addFrame(prefixScreen("Synthetic notes for panel review", i),
                    QDateTime::currentMSecsSinceEpoch() - (9 - i) * 60000).frameId;
            recorder.finish();
        }
        QVERIFY(indexText(directory, ids[0], "Synthetic notes for panel review"));
        QVERIFY(indexText(directory, ids[1], "Synthetic notes for panel review"));
        FakeRecording service;
        service.status = {{"running", true}, {"intent", "running"}, {"state", "recording"},
            {"output", "SYNTHETIC-1"}, {"indexing", false}, {"indexing_paused", true},
            {"history_directory", directory}, {"max_disk_mib", 10240},
            {"usage", QJsonObject{{"disk_bytes", 8.0 * 1024 * 1024 * 1024}}},
            {"storage_forecast", QJsonObject{{"state", "ready"}, {"capacity_active_hours", 4.0},
                {"bytes_per_active_hour", 2.5 * 1024 * 1024 * 1024},
                {"filesystem_free_bytes", 32.0 * 1024 * 1024 * 1024}, {"warning", "none"}}},
            {"worker_resources", QJsonObject{{"enforced", true}, {"effective_cpu_percent", 60}}}};
        auto viewer = replay::createViewer(directory, service.hooks());
        viewer->resize(1440, 920); viewer->show(); viewer->activateWindow();
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[2]);
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* panel = viewer->findChild<QScrollArea*>("detailsPanel"); QVERIFY(panel);
        auto* storage = viewer->findChild<QLabel*>("recordingStorageDetails"); QVERIFY(storage);
        auto* worker = viewer->findChild<QLabel*>("indexWorkerDetails"); QVERIFY(worker);
        QVERIFY(!storage->isVisible()); QVERIFY(!worker->isVisible());
        QVERIFY(viewer->findChild<QLabel*>("recallStatus")->text().contains("2 / 3 searchable"));
        QVERIFY(viewer->findChild<QLabel*>("recallStatus")->text().contains("1 pending"));
        QVERIFY(viewer->findChild<QLabel*>("indexPendingAge")->text().contains("7 min"));
        QVERIFY(viewer->findChild<QLabel*>("indexWorkerHint")->text().contains("Paused"));
        QVERIFY(viewer->findChild<QLabel*>("recordingStorageCapacity")->text().contains("8.0 / 10.0 GiB"));
        QVERIFY(worker->text().contains("60% of one core"));
        const auto focusByTab = [](QWidget* from, QWidget* target) {
            from->setFocus();
            for (int i = 0; i < 100 && !target->hasFocus(); ++i)
                QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
            return target->hasFocus();
        };
        const auto horizontallyContained = [](QScrollArea* scroll) {
            if (scroll->horizontalScrollBar()->maximum() != 0) return false;
            for (const auto* button : scroll->widget()->findChildren<QPushButton*>()) {
                if (!button->isVisible()) continue;
                const QPoint position = button->mapTo(scroll->viewport(), QPoint());
                if (position.x() < 0 || position.x() + button->width() > scroll->viewport()->width()) return false;
            }
            return true;
        };
        QVERIFY(QDir().mkpath("runs/panel-clarity"));
        for (const auto& size : {QSize(1440, 920), QSize(900, 620)}) {
            viewer->resize(size); QTest::qWait(60);
            QVERIFY(horizontallyContained(panel));
            QVERIFY(viewer->grab().save(QString("runs/panel-clarity/index-%1-primary.png").arg(size.width())));
            for (const QString& name : {"toggleStorageDetails", "toggleIndexDetails"}) {
                auto* toggle = viewer->findChild<QPushButton*>(name); QVERIFY(toggle);
                QVERIFY(focusByTab(viewer->findChild<QPushButton*>("toggleDetails"), toggle));
                QTest::keyClick(toggle, Qt::Key_Space); QVERIFY(toggle->isChecked());
            }
            QTest::qWait(60);
            QVERIFY(storage->isVisible()); QVERIFY(worker->isVisible());
            QVERIFY(horizontallyContained(panel));
            panel->verticalScrollBar()->setValue(0);
            QVERIFY(viewer->grab().save(QString("runs/panel-clarity/index-%1-expanded.png").arg(size.width())));
            panel->ensureWidgetVisible(worker);
            QVERIFY(panel->viewport()->rect().intersects(QRect(worker->mapTo(panel->viewport(), QPoint()), worker->size())));
            if (size.width() == 900) QVERIFY(panel->verticalScrollBar()->maximum() > 0);
            for (const QString& name : {"toggleStorageDetails", "toggleIndexDetails"}) {
                auto* toggle = viewer->findChild<QPushButton*>(name);
                toggle->setFocus(); QTest::keyClick(toggle, Qt::Key_Space); QVERIFY(!toggle->isChecked());
            }
            panel->verticalScrollBar()->setValue(0);
        }
        bool settingsChecked = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings"); QVERIFY(dialog);
            // A failed assertion must not leave this modal test hanging.
            QTimer::singleShot(15000, dialog, &QDialog::reject);
            for (const QString& name : {"settingsError", "agentPromptNotice"}) {
                auto* label = dialog->findChild<QLabel*>(name); QVERIFY(label);
                QVERIFY(label->text().isEmpty()); QVERIFY(label->isHidden());
            }
            auto* tabs = dialog->findChild<QTabWidget*>("settingsTabs"); QVERIFY(tabs);
            auto* output = dialog->findChild<QComboBox*>("settingOutput"); QVERIFY(output);
            QTRY_VERIFY(output->findData("SYNTHETIC-2") >= 0);
            for (const QString& name : {"captureSettingsDetailsText", "settingsStorageDetails", "storageSettingsDetailsText",
                                        "resourceSettingsDetailsText", "exclusionSettingsDetailsText"}) {
                auto* label = dialog->findChild<QLabel*>(name); QVERIFY(label); QVERIFY(label->isHidden());
            }
            const std::array<QString, 3> pages{"recordingSettingsScroll", "resourceSettingsScroll", "exclusionSettingsScroll"};
            const std::array<QString, 3> toggles{"storageCapacityDetails", "resourceSettingsDetails", "exclusionSettingsDetails"};
            for (const auto& size : {QSize(740, 740), QSize(600, 560)}) {
                dialog->resize(size);
                for (int i = 0; i < 3; ++i) {
                    tabs->setCurrentIndex(i); QTest::qWait(60);
                    auto* scroll = dialog->findChild<QScrollArea*>(pages[i]); QVERIFY(scroll);
                    scroll->verticalScrollBar()->setValue(0);
                    QVERIFY(horizontallyContained(scroll));
                    auto* buttons = dialog->findChild<QDialogButtonBox*>("settingsButtons"); QVERIFY(buttons);
                    for (auto role : {QDialogButtonBox::Save, QDialogButtonBox::Cancel}) {
                        auto* button = buttons->button(role); QVERIFY(button && button->isVisible());
                        QVERIFY(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint()), button->size())));
                    }
                    QVERIFY(dialog->grab().save(QString("runs/panel-clarity/settings-%1-tab%2-primary.png").arg(size.width()).arg(i)));
                    auto* toggle = dialog->findChild<QPushButton*>(toggles[i]); QVERIFY(toggle);
                    QVERIFY(focusByTab(tabs, toggle));
                    QTest::keyClick(toggle, Qt::Key_Space); QVERIFY(toggle->isChecked());
                    QTest::qWait(30); QVERIFY(horizontallyContained(scroll));
                    scroll->ensureWidgetVisible(toggle);
                    QVERIFY(scroll->viewport()->rect().intersects(QRect(toggle->mapTo(scroll->viewport(), QPoint()), toggle->size())));
                    if (size.height() == 560) QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
                    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
                    QVERIFY(dialog->grab().save(QString("runs/panel-clarity/settings-%1-tab%2-details.png").arg(size.width()).arg(i)));
                    QTest::keyClick(toggle, Qt::Key_Space); QVERIFY(!toggle->isChecked());
                }
            }
            settingsChecked = true;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        viewer->findChild<QPushButton*>("openReplaySettings")->click();
        QVERIFY(settingsChecked);
        QCOMPARE(replay::loadReplayConfig().original, original);
        QVERIFY(service.calls().isEmpty());
        viewer->close();
    }

    void settingsKeyboardCancelAndRetentionReviewPreserveSavedConfig() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        auto document = replay::loadReplayConfig();
        document.config.output = "SYNTHETIC-1";
        document.config.outputIdentity = "synthetic-monitor-identity";
        replay::saveReplayConfig(document.config, document.original);
        const QByteArray original = replay::loadReplayConfig().original;
        FakeRecording service;
        service.status["compositor_instance"] = "synthetic-compositor-instance";
        service.status["visible_windows"] = QJsonArray{QJsonObject{{"app_id", "org.example.SyntheticBrowser"},
            {"title", "Synthetic visible window"}, {"address", "0x555"}}};
        auto viewer = replay::createViewer(replay::replayPaths().historyDirectory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* settings = viewer->findChild<QPushButton*>("openReplaySettings");
        bool visited = false, retentionReviewed = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings");
            QVERIFY(dialog); visited = true;
            auto* output = dialog->findChild<QComboBox*>("settingOutput");
            QTRY_VERIFY(output->findData("SYNTHETIC-2") >= 0);
            QVERIFY(!output->isEditable());
            QVERIFY(output->itemText(output->findData("SYNTHETIC-1")).contains("1920 × 1080"));
            QVERIFY(QDir().mkpath("runs/design-review"));
            QVERIFY(dialog->grab().save("runs/design-review/shared-recording-settings.png"));
            output->setFocus(); output->setCurrentIndex(output->findData("SYNTHETIC-2"));
            auto* days = dialog->findChild<QSpinBox*>("settingRetentionDays"); days->setValue(7);
            dialog->findChild<QSpinBox*>("settingMaxDiskMiB")->setValue(5120);
            dialog->findChild<QSpinBox*>("settingMinFreeMiB")->setValue(2048);
            QTimer::singleShot(0, [&] {
                auto* confirmation = viewer->findChild<QMessageBox*>("confirmShorterRetention");
                QVERIFY(confirmation); retentionReviewed = true;
                QVERIFY(confirmation->text().contains("permanently delete"));
                QVERIFY(confirmation->text().contains("older than 7 days"));
                QVERIFY(confirmation->text().contains("5120 MiB allowance"));
                QVERIFY(confirmation->text().contains("2048 MiB free"));
                QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
                QTest::keyClick(confirmation, Qt::Key_Escape);
            });
            auto* save = dialog->findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::Save);
            QVERIFY(save->icon().isNull());
            save->setFocus(); QTest::keyClick(save, Qt::Key_Space);
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        settings->setFocus(); QTest::keyClick(settings, Qt::Key_Space);
        QVERIFY(visited); QVERIFY(retentionReviewed);
        QCOMPARE(replay::loadReplayConfig().original, original);
        QVERIFY(service.calls().isEmpty());
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings");
            QVERIFY(dialog);
            auto* output = dialog->findChild<QComboBox*>("settingOutput");
            QTRY_VERIFY(output->findData("SYNTHETIC-2") >= 0);
            output->setFocus(); output->setCurrentIndex(output->findData("SYNTHETIC-2"));
            dialog->findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(2);
            QTimer::singleShot(0, [&] {
                auto* picker = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
                QVERIFY(picker); picker->accept();
            });
            auto* choose = dialog->findChild<QPushButton*>("chooseExcludedWindow");
            QVERIFY(choose->isEnabled()); choose->setFocus(); QTest::keyClick(choose, Qt::Key_Space);
            QVERIFY(dialog->grab().save("runs/design-review/shared-recording-exclusions.png"));
            auto* save = dialog->findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::Save);
            save->setFocus(); QTest::keyClick(save, Qt::Key_Space);
        });
        settings->setFocus(); QTest::keyClick(settings, Qt::Key_Space);
        QTRY_COMPARE(service.calls(), QStringList({"reload"}));
        QCOMPARE(replay::loadReplayConfig().config.output, "SYNTHETIC-2");
        QVERIFY(replay::loadReplayConfig().config.excludedApps.contains("org.omarchy.screensaver"));
        QVERIFY(replay::loadReplayConfig().config.outputIdentity.isEmpty());
        const auto rules = replay::loadReplayConfig().config.excludedWindows;
        QCOMPARE(rules.size(), 1);
        QCOMPARE(rules[0].appId, "org.example.SyntheticBrowser"); QCOMPARE(rules[0].address, "0x555");
        QCOMPARE(rules[0].compositorInstance, "synthetic-compositor-instance");
        viewer->close();
    }

    void focusedDisplayChoiceSavesModeAndPreservesConfiguredOutput() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        auto document = replay::loadReplayConfig();
        document.config.output = "SYNTHETIC-1";
        document.config.outputIdentity = "synthetic-monitor-identity";
        replay::saveReplayConfig(document.config, document.original);
        FakeRecording service;
        auto viewer = replay::createViewer(replay::replayPaths().historyDirectory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* settings = viewer->findChild<QPushButton*>("openReplaySettings");
        bool visited = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings");
            QVERIFY(dialog); visited = true;
            QTimer::singleShot(15000, dialog, &QDialog::reject);
            auto* output = dialog->findChild<QComboBox*>("settingOutput"); QVERIFY(output);
            QTRY_VERIFY(output->findData("SYNTHETIC-2") >= 0);
            // Fixed mode keeps the saved display selected, next to the new entry.
            QCOMPARE(output->currentData().toString(), QString("SYNTHETIC-1"));
            const int focused = output->findData("@focused");
            QVERIFY(focused >= 0);
            output->setFocus(); output->setCurrentIndex(focused);
            auto* save = dialog->findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::Save);
            QVERIFY(save->isEnabled());
            save->setFocus(); QTest::keyClick(save, Qt::Key_Space);
        });
        settings->setFocus(); QTest::keyClick(settings, Qt::Key_Space);
        QVERIFY(visited);
        const auto saved = replay::loadReplayConfig().config;
        QCOMPARE(saved.displayMode, "focused");
        QCOMPARE(saved.output, "SYNTHETIC-1");
        QCOMPARE(saved.outputIdentity, "synthetic-monitor-identity");
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings");
            QVERIFY(dialog);
            QTimer::singleShot(15000, dialog, &QDialog::reject);
            auto* output = dialog->findChild<QComboBox*>("settingOutput"); QVERIFY(output);
            QTRY_VERIFY(output->findData("SYNTHETIC-2") >= 0);
            QCOMPARE(output->currentData().toString(), QString("@focused"));
            QVERIFY(output->findData("@focused") >= 0);
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        settings->setFocus(); QTest::keyClick(settings, Qt::Key_Space);
        QCOMPARE(replay::loadReplayConfig().config.displayMode, "focused");
        viewer->close();
    }

    void exclusionPresetsMergeOnlyOnRequestAndSaveExplicitly() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        auto document = replay::loadReplayConfig();
        const auto privacy = replay::privacyAppExclusions();
        const auto gaming = replay::gamingAppExclusions();
        const auto media = replay::mediaAppExclusions();
        QVERIFY(!privacy.isEmpty()); QVERIFY(!gaming.isEmpty()); QVERIFY(!media.isEmpty());
        QCOMPARE(document.config.skippedApps, QStringList({"steam", "Steam"}));
        for (const auto& app : gaming) QVERIFY(!document.config.excludedApps.contains(app));
        for (const auto& app : media) {
            QVERIFY(!document.config.excludedApps.contains(app)); QVERIFY(!document.config.skippedApps.contains(app));
        }
        document.config.output = "SYNTHETIC-1";
        document.config.outputIdentity = "synthetic-display-identity";
        document.config.intervalSeconds = 4.5;
        document.config.retentionDays = 45;
        document.config.activeCpuPercent = 25;
        document.config.preferredAgent = "synthetic-agent";
        // Existing strict protection remains authoritative, even when an
        // optional preset later adds the same identity to the skip list.
        document.config.excludedApps = {"omarchy-replay", "org.omarchy.screensaver", "org.example.Custom", privacy.first(), "steam"};
        document.config.skippedApps = {"org.example.SkipCustom"};
        document.config.excludedWindows = {{"Private note", "org.example.Editor", {}, "output", {}}};
        replay::saveReplayConfig(document.config, document.original);
        {
            QFile config(replay::replayPaths().configFile); QVERIFY(config.open(QIODevice::Append));
            QVERIFY(config.write("\n[custom_user_option]\nkeep = \"unchanged\"\n") > 0);
        }
        const auto before = replay::loadReplayConfig();
        FakeRecording service;
        service.status["visible_windows"] = QJsonArray{QJsonObject{{"app_id", "org.example.Player"}, {"title", "Synthetic media"}}};
        auto viewer = replay::createViewer(replay::replayPaths().historyDirectory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* settings = viewer->findChild<QPushButton*>("openReplaySettings"); QVERIFY(settings);
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings"); QVERIFY(dialog);
            QTimer::singleShot(15000, dialog, &QDialog::reject);
            auto* output = dialog->findChild<QComboBox*>("settingOutput"); QVERIFY(output);
            QTRY_VERIFY(output->findData("SYNTHETIC-1") >= 0);
            dialog->findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(2);
            auto* apps = dialog->findChild<QPlainTextEdit*>("settingExcludedApps"); QVERIFY(apps);
            auto* skipped = dialog->findChild<QPlainTextEdit*>("settingSkippedApps"); QVERIFY(skipped);
            auto* preset = dialog->findChild<QComboBox*>("exclusionPreset"); QVERIFY(preset);
            auto* add = dialog->findChild<QPushButton*>("addExclusionPreset"); QVERIFY(add);
            QCOMPARE(preset->count(), 3);
            QCOMPARE(apps->toPlainText(), QString("org.example.Custom\n") + privacy.first() + "\nsteam");
            QCOMPARE(skipped->toPlainText(), QString("org.example.SkipCustom"));
            const QString strictBeforePicker = apps->toPlainText();
            QTimer::singleShot(0, [&] {
                auto* picker = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(picker);
                picker->accept();
            });
            dialog->findChild<QPushButton*>("chooseExcludedApp")->click();
            QCOMPARE(skipped->toPlainText(), QString("org.example.SkipCustom\norg.example.Player"));
            QCOMPARE(apps->toPlainText(), strictBeforePicker);
            apps->setPlainText("  org.example.Custom  \n" + privacy.first() + "\norg.example.Custom\nsteam\n");
            skipped->setPlainText(" org.example.SkipCustom \norg.example.Player\norg.example.SkipCustom\n");
            QStringList expectedStrict{"org.example.Custom", privacy.first(), "steam"};
            QStringList expectedSkip{"org.example.SkipCustom", "org.example.Player"};
            for (const auto& entry : {qMakePair(QString("privacy"), privacy), qMakePair(QString("gaming"), gaming),
                                      qMakePair(QString("media"), media)}) {
                preset->setCurrentIndex(preset->findData(entry.first));
                const bool strictPreset = entry.first == "privacy";
                auto* target = strictPreset ? apps : skipped;
                auto* untouched = strictPreset ? skipped : apps;
                const QString beforeClick = target->toPlainText(), otherList = untouched->toPlainText();
                QTest::qWait(20); QCOMPARE(target->toPlainText(), beforeClick);
                // Offscreen Qt can leave activation on the picker after its
                // nested event loop exits. Establish real focus, then exercise
                // the same editor → preset → action path a keyboard user takes.
                dialog->activateWindow(); apps->setFocus(); QTRY_VERIFY(apps->hasFocus());
                QTest::keyClick(apps, Qt::Key_Tab); QTRY_VERIFY(preset->hasFocus());
                QTest::keyClick(preset, Qt::Key_Tab); QTRY_VERIFY(add->hasFocus());
                QTest::keyClick(add, Qt::Key_Space);
                auto& expected = strictPreset ? expectedStrict : expectedSkip;
                for (const auto& app : entry.second) if (!expected.contains(app)) expected.append(app);
                QCOMPARE(target->toPlainText().split('\n'), expected);
                QCOMPARE(untouched->toPlainText(), otherList);
                QTest::keyClick(add, Qt::Key_Space);
                QCOMPARE(target->toPlainText().split('\n'), expected);
                QCOMPARE(replay::loadReplayConfig().original, before.original);
            }
            QVERIFY(apps->toPlainText().split('\n').contains("steam"));
            QVERIFY(skipped->toPlainText().split('\n').contains("steam"));
            dialog->resize(600, 560); QTest::qWait(60);
            auto* scroll = dialog->findChild<QScrollArea*>("exclusionSettingsScroll"); QVERIFY(scroll);
            QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
            QVERIFY(QDir().mkpath("runs/exclusion-presets"));
            scroll->verticalScrollBar()->setValue(0);
            QVERIFY(dialog->grab().save("runs/exclusion-presets/compact-top.png"));
            scroll->ensureWidgetVisible(add);
            for (auto* widget : {static_cast<QWidget*>(preset), static_cast<QWidget*>(add)})
                QVERIFY(scroll->viewport()->rect().contains(QRect(widget->mapTo(scroll->viewport(), QPoint()), widget->size())));
            QVERIFY(dialog->grab().save("runs/exclusion-presets/compact.png"));

            // Both lists and the two always-enforced identities share a
            // single limit. Reject the entire addition before editing either.
            QStringList full;
            for (int i = 0; i < 61; ++i) full.append(QString("org.example.Custom%1").arg(i));
            const QString fullText = full.join('\n');
            apps->setPlainText(fullText); skipped->setPlainText("org.example.SkipCustom");
            preset->setCurrentIndex(preset->findData("media")); add->click();
            QCOMPARE(apps->toPlainText(), fullText);
            QCOMPARE(skipped->toPlainText(), QString("org.example.SkipCustom"));
            auto* error = dialog->findChild<QLabel*>("settingsError"); QVERIFY(error);
            QVERIFY(error->isVisible()); QVERIFY(error->text().contains("64 app exclusions"));
            QCOMPARE(replay::loadReplayConfig().original, before.original);
            apps->setPlainText("org.example.Custom"); add->click();
            QVERIFY(error->isHidden());
            inspected = true;
            QTest::keyClick(dialog, Qt::Key_Escape);
        });
        settings->click();
        QVERIFY(inspected);
        QCOMPARE(replay::loadReplayConfig().original, before.original);
        QVERIFY(service.calls().isEmpty());

        bool saved = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings"); QVERIFY(dialog);
            QTimer::singleShot(15000, dialog, &QDialog::reject);
            auto* output = dialog->findChild<QComboBox*>("settingOutput"); QVERIFY(output);
            QTRY_VERIFY(output->findData("SYNTHETIC-1") >= 0);
            dialog->findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(2);
            auto* preset = dialog->findChild<QComboBox*>("exclusionPreset"); QVERIFY(preset);
            preset->setCurrentIndex(preset->findData("gaming"));
            dialog->findChild<QPushButton*>("addExclusionPreset")->click();
            QCOMPARE(replay::loadReplayConfig().original, before.original);
            auto* save = dialog->findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::Save);
            save->setFocus(); QTest::keyClick(save, Qt::Key_Space); saved = true;
        });
        settings->click();
        QVERIFY(saved);
        QTRY_COMPARE(service.calls(), QStringList({"reload"}));
        auto after = replay::loadReplayConfig();
        auto expected = before.config.skippedApps;
        for (const auto& app : gaming) if (!expected.contains(app)) expected.append(app);
        QCOMPARE(after.config.skippedApps, expected);
        QCOMPARE(after.config.excludedApps, before.config.excludedApps);
        QVERIFY(after.original.contains("[custom_user_option]")); QVERIFY(after.original.contains("unchanged"));
        // Compare every supported field after restoring the one intended edit.
        after.config.skippedApps = before.config.skippedApps;
        const QString beforePath = temporary.filePath("before.toml"), afterPath = temporary.filePath("after.toml");
        replay::saveReplayConfig(before.config, {}, beforePath);
        replay::saveReplayConfig(after.config, {}, afterPath);
        QFile beforeFile(beforePath), afterFile(afterPath);
        QVERIFY(beforeFile.open(QIODevice::ReadOnly)); QVERIFY(afterFile.open(QIODevice::ReadOnly));
        QCOMPARE(afterFile.readAll(), beforeFile.readAll());
        viewer->close();
    }

    void fixtureContainsExactDuplicatesAndSmallChanges() {
        QCOMPARE(replay::fixtureFrameCount(), 16);
        QCOMPARE(replay::fixtureGroundTruth().size(), 16);
        const QImage original = replay::fixtureFrame(0);
        QCOMPARE(original, replay::fixtureFrame(1));
        QVERIFY(original != replay::fixtureFrame(2));
        QCOMPARE(replay::fixtureFrame(9), replay::fixtureFrame(10));
        QVERIFY(replay::fixtureFrame(10) != replay::fixtureFrame(11));
        QVERIFY(replay::fixtureGroundTruth(11).value("tokens").toArray().contains("EDGE-7F8"));
    }

    void settingsPromptsContainInstructionsWithoutPrivateContextAndStorageSwitchIsReviewed() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        QVERIFY(QDir().mkpath("runs/design-review"));
        auto document = replay::loadReplayConfig();
        document.config.output = "DISCONNECTED-9";
        replay::saveReplayConfig(document.config, document.original);
        const QString originalHistory = replay::replayHistoryDirectory(document.config);
        const QString newHistory = temporary.filePath("mounted-disk/replay-history");
        QVERIFY(QDir().mkpath(newHistory));
        FakeRecording service;
        service.status["visible_windows"] = QJsonArray{QJsonObject{{"app_id", "org.example.Private"},
            {"title", "PRIVATE_TITLE_NEVER_IN_PROMPT"}, {"address", "0xabcd"}}};
        auto viewer = replay::createViewer(originalHistory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        bool reviewed = false;
        QTimer::singleShot(0, [&] {
            auto* dialog = viewer->findChild<QDialog*>("replaySettings"); QVERIFY(dialog);
            auto* output = dialog->findChild<QComboBox*>("settingOutput");
            QTRY_VERIFY(output->findData("SYNTHETIC-2") >= 0);
            QCOMPARE(output->currentData().toString(), "DISCONNECTED-9");
            QVERIFY(output->currentText().contains("disconnected"));
            auto* folder = dialog->findChild<QLineEdit*>("settingStorageDirectory");
            QCOMPARE(folder->text(), originalHistory); QVERIFY(folder->isReadOnly());
            for (const QString& name : {"copySetupPrompt", "copyResourcesPrompt", "copyExclusionsPrompt"}) {
                dialog->findChild<QPushButton*>(name)->click();
                auto* notice = dialog->findChild<QLabel*>("agentPromptNotice"); QVERIFY(notice);
                QVERIFY(notice->isVisible()); QVERIFY(!notice->text().isEmpty());
                QVERIFY(dialog->findChild<QLabel*>("settingsError")->isHidden());
                const QString prompt = QApplication::clipboard()->text();
                QVERIFY(prompt.contains("https://github.com/rblalock/omarchy-replay"));
                QVERIFY(!prompt.contains("/README.md"));
                QVERIFY(!prompt.contains("./scripts/replay"));
                QVERIFY(prompt.contains(replay::replayPaths().configFile));
                QVERIFY(prompt.contains("daemon paths")); QVERIFY(prompt.contains("daemon status"));
                QVERIFY(prompt.contains("Preserve capture intent"));
                QVERIFY(!prompt.contains("PRIVATE_TITLE_NEVER_IN_PROMPT"));
                QVERIFY(!prompt.contains("org.example.Private"));
                if (name == "copyResourcesPrompt") {
                    QVERIFY(prompt.contains("active CPU 40%")); QVERIFY(prompt.contains("window-switching responsiveness"));
                    QVERIFY(prompt.contains("recording.log"));
                }
            }
            auto* tabs = dialog->findChild<QTabWidget*>("settingsTabs");
            tabs->setCurrentIndex(1);
            QVERIFY(dialog->grab().save("runs/design-review/shared-recording-resources.png"));
            tabs->setCurrentIndex(0);
            QTimer::singleShot(0, [&] {
                auto* picker = qobject_cast<QFileDialog*>(QApplication::activeModalWidget()); QVERIFY(picker);
                picker->setDirectory(newHistory); picker->selectFile(newHistory);
                QMetaObject::invokeMethod(picker, "accept", Qt::DirectConnection);
            });
            dialog->findChild<QPushButton*>("chooseStorageDirectory")->click();
            QCOMPARE(folder->text(), newHistory);
            QTimer::singleShot(0, [&] {
                auto* confirmation = viewer->findChild<QMessageBox*>("confirmStorageDirectory"); QVERIFY(confirmation);
                reviewed = true;
                QVERIFY(confirmation->text().contains("stays in its original folder"));
                QCOMPARE(confirmation->defaultButton(), confirmation->button(QMessageBox::Cancel));
                QVERIFY(confirmation->button(QMessageBox::Save)->icon().isNull());
                confirmation->button(QMessageBox::Save)->click();
            });
            dialog->findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::Save)->click();
        });
        viewer->findChild<QPushButton*>("openReplaySettings")->click();
        QVERIFY(reviewed);
        QTRY_COMPARE(service.calls(), QStringList({"reload"}));
        QCOMPARE(replay::loadReplayConfig().config.storageDirectory, newHistory);
        QTRY_COMPARE(viewer->property("historyDirectory").toString(), newHistory);
        QVERIFY(!QFileInfo::exists(originalHistory));
        QCOMPARE(service.status.value("intent").toString(), "stopped");
        viewer->close();
    }

    void malformedConfigKeepsCustomHistoryControlsAndRepairPromptAvailable() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        const auto paths = replay::replayPaths();
        auto document = replay::loadReplayConfig();
        document.config.output = "SYNTHETIC-2";
        document.config.activeCpuPercent = 35;
        document.config.storageDirectory = temporary.filePath("external-disk/history");
        QVERIFY(QDir().mkpath(document.config.storageDirectory));
        replay::saveReplayConfig(document.config, document.original);
        replay::saveReplayConfig(document.config, {}, paths.stateDirectory + "/last-valid-config.toml");
        const QByteArray malformed("[recording\noutput = \"unfinished\n");
        QFile config(paths.configFile);
        QVERIFY(config.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(config.write(malformed), malformed.size()); config.close();
        FakeRecording service;
        service.status["history_directory"] = document.config.storageDirectory;
        service.status["config_error"] = "Current TOML cannot be parsed";
        auto viewer = replay::createViewer(document.config.storageDirectory, service.hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        QVERIFY(viewer->findChild<QWidget*>("recordingPanel")->isVisible());
        QVERIFY(viewer->findChild<QLabel*>("recordingStatus")->text().contains("Current TOML cannot be parsed"));
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto* settings = viewer->findChild<QDialog*>("replaySettings"); QVERIFY(settings);
            inspected = true;
            QVERIFY(settings->findChild<QLabel*>("settingsError")->text().contains("last accepted settings"));
            QVERIFY(settings->findChild<QLabel*>("settingsError")->isVisible());
            QVERIFY(settings->findChild<QLabel*>("agentPromptNotice")->isHidden());
            QVERIFY(!settings->findChild<QDialogButtonBox*>("settingsButtons")->button(QDialogButtonBox::Save)->isEnabled());
            QCOMPARE(settings->findChild<QLineEdit*>("settingStorageDirectory")->text(), document.config.storageDirectory);
            settings->findChild<QPushButton*>("copySetupPrompt")->click();
            QVERIFY(settings->findChild<QLabel*>("agentPromptNotice")->isVisible());
            QVERIFY(settings->findChild<QLabel*>("settingsError")->isVisible());
            auto prompt = QApplication::clipboard()->text();
            QVERIFY(prompt.contains(document.config.storageDirectory));
            QVERIFY(prompt.contains(paths.configFile));
            QVERIFY(prompt.contains("Repair its syntax"));
            QVERIFY(prompt.contains(paths.stateDirectory + "/last-valid-config.toml"));
            settings->findChild<QPushButton*>("copyResourcesPrompt")->click();
            QVERIFY(QApplication::clipboard()->text().contains("active CPU 35%"));
            QTest::keyClick(settings, Qt::Key_Escape);
        });
        viewer->findChild<QPushButton*>("openReplaySettings")->click();
        QVERIFY(inspected); QVERIFY(service.calls().isEmpty());
        QVERIFY(config.open(QIODevice::ReadOnly)); QCOMPARE(config.readAll(), malformed);
        QVERIFY(!QFileInfo::exists(paths.historyDirectory));
        viewer->close();
    }

    void keyboardSearchOpenAndTimeline() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString directory = temporary.filePath("synthetic-history");
        replay::RecorderOptions options;
        options.directory = directory;
        options.ocr = false;
        options.minFreeBytes = 0;
        constexpr qint64 start = 1789725600000;
        std::array<qint64, 4> ids{};
        {
            replay::Recorder recorder(options);
            const std::array<int, 4> fixtureIndices{0, 2, 4, 5};
            for (int i = 0; i < 4; ++i)
                ids[i] = recorder.addFrame(replay::fixtureFrame(fixtureIndices[i]), start + i * 2000).frameId;
            recorder.finish();
        }

        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        const std::array<QString, 4> indexedText{
            "Patrick invoice XYZ-1042 Northwind", "Patrick invoice XYZ-1043 Northwind",
            "Omakase demo cedar fixtures", "Omakase demo juniper completed"};
        for (int i = 0; i < 4; ++i) {
            for (const char* sql : {"UPDATE frames SET text=?,ocr_state='ready' WHERE id=?",
                                    "INSERT OR REPLACE INTO frame_text(text,rowid) VALUES(?,?)"}) {
                sqlite3_stmt* statement = nullptr;
                QCOMPARE(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr), SQLITE_OK);
                const QByteArray value = indexedText[i].toUtf8();
                QCOMPARE(sqlite3_bind_text(statement, 1, value.constData(), value.size(), SQLITE_TRANSIENT), SQLITE_OK);
                QCOMPARE(sqlite3_bind_int64(statement, 2, ids[i]), SQLITE_OK);
                QCOMPARE(sqlite3_step(statement), SQLITE_DONE);
                sqlite3_finalize(statement);
            }
        }
        QCOMPARE(sqlite3_exec(database,
            "INSERT INTO frame_ocr_geometry(frame_id,lines_json) VALUES(1,'[[122,234,140,38,\"Patrick\"],[122,504,140,38,\"Patrick\"]]')",
            nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);

        auto viewer = replay::createViewer(directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        QVERIFY(search);
        QVERIFY(results);
        QTRY_VERIFY(viewer->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClicks(search, "Patrick");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 2);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QVERIFY(results->hasFocus());

        QTest::keyClick(results, Qt::Key_Down);
        QCOMPARE(results->currentRow(), 1);
        // A selection changes synchronously; decoding completes later. Keep
        // the last pixels on screen while withholding stale OCR/copy context.
        QVERIFY(viewer->findChild<QWidget*>("evidenceView")->property("hasImage").toBool());
        if (!viewer->property("displayedFrameId").toLongLong()) {
            QVERIFY(!viewer->findChild<QLabel*>("mediaStatus")->isVisible());
            QApplication::clipboard()->setText("untouched while decoding");
            QTest::keyClick(results, Qt::Key_C, Qt::ControlModifier);
            QCOMPARE(QApplication::clipboard()->text(), "untouched while decoding");
        }
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[1]);

        // Time navigation crosses outside the matches without losing the query
        // or the user's selected result. Clicking the same card returns immediately.
        QTRY_VERIFY(viewer->findChild<QPushButton*>("laterMoment")->isEnabled());
        QTest::keyClick(results, Qt::Key_Right);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[2]);
        QCOMPARE(search->text(), "Patrick");
        QCOMPARE(results->count(), 2);
        QCOMPARE(results->currentRow(), 1);
        QTest::mouseClick(results->viewport(), Qt::LeftButton, Qt::NoModifier, results->visualItemRect(results->currentItem()).center());
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[1]);

        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "Omakase");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 2);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[2]);
        QTest::keyClick(results, Qt::Key_Down);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[3]);

        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "nothing-matches-this-fixture");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 0);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(0));
        QCOMPARE(viewer->findChild<QLabel*>("recordedTimestamp")->text(), "No matching recorded text");

        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "Patrick");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QVERIFY(QDir().mkpath("runs"));
        QTRY_COMPARE(viewer->property("highlightCount").toInt(), 2);
        QVERIFY(QDir().mkpath("runs/design-review"));
        QVERIFY(viewer->grab().save("runs/design-review/search-desktop.png"));
        viewer->resize(900, 620);
        QTest::qWait(60);
        QVERIFY(viewer->grab().save("runs/design-review/search-compact.png"));
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(search, Qt::Key_Backspace);
        // Debounced search and timeline selection need no Enter key.
        QTRY_VERIFY(!results->isVisible());
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[3]);
        auto* timeline = viewer->findChild<QSlider*>("recallTimeline");
        QVERIFY(timeline && timeline->isVisible());
        timeline->setFocus();
        QTest::keyClick(timeline, Qt::Key_Home);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QTest::keyClick(timeline, Qt::Key_Right);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[1]);
        QTest::keyClick(timeline, Qt::Key_End);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[3]);
        QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, QPoint(12, 43));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QTest::keyClick(timeline, Qt::Key_1);
        QVERIFY(!viewer->findChild<QWidget*>("evidenceView")->property("fit").toBool());
        QTest::keyClick(timeline, Qt::Key_F);
        QVERIFY(viewer->findChild<QWidget*>("evidenceView")->property("fit").toBool());
        viewer->resize(1440, 920);
        QTest::qWait(60);
        QVERIFY(viewer->grab().save("runs/design-review/timeline-desktop.png"));
        QTest::keyClick(timeline, Qt::Key_Question);
        QVERIFY(viewer->findChild<QWidget*>("helpPanel")->isVisible());
        QVERIFY(!viewer->findChild<QWidget*>("detailsPanel")->isVisible());
        QTest::keyClick(timeline, Qt::Key_Question);
        QVERIFY(!viewer->findChild<QWidget*>("helpPanel")->isVisible());

        QTest::keyClick(timeline, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());
        QVERIFY(viewer->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_Escape);
        QTRY_VERIFY(!viewer->isVisible());
    }

    void livePrefixSearchAndSeparatedPanels() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("prefix-history");
        options.ocr = false;
        options.minFreeBytes = 0;
        constexpr qint64 start = 1789837200000;
        const std::array<QString, 3> lines{
            "Continue the invoice review with Patrick.",
            "Continuous capture keeps moments available.",
            "Continuity helps us return to earlier work."};
        std::array<qint64, 3> ids{};
        {
            replay::Recorder recorder(options);
            for (int i = 0; i < 3; ++i)
                ids[i] = recorder.addFrame(prefixScreen(lines[i], i + 1), start + i * 60000).frameId;
            recorder.finish();
        }
        for (int i = 0; i < 3; ++i) {
            const auto bounds = QFontMetrics(QFont("monospace", 22)).boundingRect(
                QRect(80, 240, 1120, 60), Qt::AlignVCenter, lines[i]);
            QVERIFY(indexText(options.directory, ids[i], lines[i],
                QJsonArray{QJsonArray{bounds.x(), bounds.y(), bounds.width(), bounds.height(), lines[i]}}));
        }

        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* timeline = viewer->findChild<QSlider*>("recallTimeline");
        auto* indexState = viewer->findChild<QLabel*>("indexState");
        auto* details = viewer->findChild<QWidget*>("detailsPanel");
        auto* help = viewer->findChild<QWidget*>("helpPanel");
        auto* heading = viewer->findChild<QLabel*>("resultsHeading");
        auto* clearSearch = viewer->findChild<QPushButton*>("clearSearch");
        QVERIFY(search && results && timeline && indexState && details && help && heading && clearSearch);
        QVERIFY(!viewer->findChild<QLabel*>("matchExcerpt"));
        QCOMPARE(clearSearch->text(), "Clear");
        QVERIFY(clearSearch->icon().isNull());
        QVERIFY(!search->isClearButtonEnabled());
        QVERIFY(!clearSearch->isVisible());
        for (const auto* label : viewer->findChildren<QLabel*>()) QVERIFY(label->text() != "Replay");
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[2]);
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "con");
        // Three characters trigger the bounded prefix query without Enter.
        QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(3));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QTest::keyClicks(search, "tin");
        QTest::qWait(220); // Allow the 180 ms typing debounce to start its read.
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTRY_COMPARE(viewer->property("highlightCount").toInt(), 1);
        QCOMPARE(search->text(), "contin");
        QTRY_COMPARE(timeline->property("matchMarkerCount").toInt(), 3);
        QVERIFY(indexState->text().isEmpty());
        QVERIFY(!indexState->isVisible());
        QVERIFY(!details->isVisible());
        QVERIFY(!help->isVisible());
        QVERIFY(clearSearch->isVisible());

        // Escape keeps the live query and returns to a neutral keyboard target.
        // Match arrows and letter shortcuts then work without another click.
        QTest::keyClick(search, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());
        QVERIFY(viewer->isVisible());
        QCOMPARE(search->text(), "contin");
        QTest::keyClick(viewer.get(), Qt::Key_1);
        QVERIFY(!viewer->findChild<QWidget*>("evidenceView")->property("fit").toBool());
        QTest::keyClick(viewer.get(), Qt::Key_F);
        QVERIFY(viewer->findChild<QWidget*>("evidenceView")->property("fit").toBool());
        for (int i = 1; i < 3; ++i) {
            QTest::keyClick(viewer.get(), Qt::Key_Down);
            QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[i]);
            QTRY_COMPARE(viewer->property("highlightCount").toInt(), 1);
        }
        QVERIFY(heading->text().startsWith("3 / 3"));
        QVERIFY(results->item(0)->text().contains(':'));
        QVERIFY(!results->item(0)->text().contains('\n'));
        QVERIFY(results->mapTo(viewer.get(), QPoint()).y() >= timeline->mapTo(viewer.get(), QPoint()).y() + timeline->height());

        QVERIFY(QDir().mkpath("runs/design-review-v2"));
        QVERIFY(viewer->grab().save("runs/design-review-v2/search-desktop.png"));
        viewer->resize(900, 620);
        QTest::qWait(60);
        QVERIFY(viewer->grab().save("runs/design-review-v2/search-compact.png"));
        viewer->resize(1440, 920);
        QTest::keyClick(results, Qt::Key_I);
        QVERIFY(details->isVisible());
        QVERIFY(!help->isVisible());
        QVERIFY(!viewer->findChild<QPushButton*>("processMoment")->isVisible());
        QVERIFY(!viewer->findChild<QPushButton*>("catchUpIndexing")->isVisible());
        QVERIFY(!viewer->findChild<QPushButton*>("copyIndexCommand")->isVisible());
        QTest::qWait(60);
        QVERIFY(viewer->grab().save("runs/design-review-v2/index-ready.png"));
        QTest::keyClick(results, Qt::Key_Question);
        QVERIFY(help->isVisible());
        QVERIFY(!details->isVisible());
        QTest::qWait(60);
        QVERIFY(viewer->grab().save("runs/design-review-v2/keyboard-help.png"));
        QTest::keyClick(results, Qt::Key_I);
        QVERIFY(details->isVisible());
        QVERIFY(!help->isVisible());
        auto* refresh = viewer->findChild<QPushButton*>("refreshHistory");
        QVERIFY(refresh);
        refresh->setFocus();
        QTest::keyClick(refresh, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());
        QVERIFY(!details->isVisible());
        QVERIFY(viewer->isVisible());
        QCOMPARE(search->text(), "contin");

        QTest::keyClick(viewer.get(), Qt::Key_Question);
        QVERIFY(help->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTRY_VERIFY(search->hasFocus());
        QTest::keyClick(search, Qt::Key_Escape);
        QVERIFY(!help->isVisible());
        QVERIFY(viewer->hasFocus());
        QVERIFY(viewer->isVisible());
        QCOMPARE(search->text(), "contin");
        timeline->setFocus();
        QTest::keyClick(timeline, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());
        QVERIFY(viewer->isVisible());
        QCOMPARE(search->text(), "contin");

        clearSearch->click();
        QCOMPARE(search->text(), QString());
        QTRY_VERIFY(search->hasFocus());
        QVERIFY(!clearSearch->isVisible());
        QTRY_VERIFY(!results->isVisible());
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[2]);
        QTest::keyClick(search, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());
        QVERIFY(viewer->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_Escape);
        QTRY_VERIFY(!viewer->isVisible());

        // A pending image remains viewable, while the index panel exposes only
        // the work and recovery actions relevant to its stopped worker.
        options.directory = temporary.filePath("pending-prefix-history");
        options.ocr = true;
        options.deferredOcr = true;
        qint64 pendingId = 0;
        {
            replay::Recorder recorder(options);
            pendingId = recorder.addFrame(prefixScreen(lines[0], 4), start + 180000).frameId;
            recorder.finish();
        }
        auto pendingViewer = replay::createViewer(options.directory);
        pendingViewer->show();
        pendingViewer->activateWindow();
        QTRY_COMPARE(pendingViewer->property("displayedFrameId").toLongLong(), pendingId);
        QTest::mouseClick(pendingViewer->findChild<QPushButton*>("toggleDetails"), Qt::LeftButton);
        QVERIFY(pendingViewer->findChild<QWidget*>("detailsPanel")->isVisible());
        QVERIFY(!pendingViewer->findChild<QWidget*>("helpPanel")->isVisible());
        QVERIFY(pendingViewer->findChild<QPushButton*>("processMoment")->isVisible());
        QVERIFY(pendingViewer->findChild<QPushButton*>("catchUpIndexing")->isVisible());
        QVERIFY(pendingViewer->findChild<QPushButton*>("copyIndexCommand")->isVisible());
        QVERIFY(pendingViewer->findChild<QLabel*>("indexState")->isVisible());
        QVERIFY(pendingViewer->findChild<QLabel*>("indexWorkerHint")->text().contains("stopped"));
        QVERIFY(pendingViewer->findChild<QLabel*>("indexPendingAge")->text().contains("Oldest waiting"));
        QTest::qWait(60);
        QVERIFY(pendingViewer->grab().save("runs/design-review-v2/index-pending.png"));
        pendingViewer->close();
    }

    void copyMatchesRespectsFocusAndGeometry() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("copy-prefix-history");
        options.ocr = false;
        options.minFreeBytes = 0;
        const QString firstLine = "Continue the invoice review with Patrick.";
        const QString secondLine = "Continuous capture keeps moments available.";
        const QString unrelated = "An unrelated note must stay out of the match copy.";
        const QString fullText = "Picking up where we left off\n" + firstLine + '\n' + secondLine + '\n' + unrelated;
        const QString withoutGeometry = "Continuity remains searchable without stored positions.\nAnother unrelated note.";
        std::array<qint64, 2> ids{};
        {
            replay::Recorder recorder(options);
            QImage image = prefixScreen(firstLine, 1);
            {
                QPainter painter(&image);
                painter.fillRect(QRect(80, 330, 1120, 185), QColor("#111820"));
                painter.setFont(QFont("monospace", 15));
                painter.setPen(QColor("#ece8da"));
                painter.drawText(QRect(80, 344, 1120, 50), Qt::AlignVCenter, secondLine);
                painter.drawText(QRect(80, 454, 1120, 50), Qt::AlignVCenter, unrelated);
            }
            ids[0] = recorder.addFrame(image, 1000).frameId;
            ids[1] = recorder.addFrame(prefixScreen(withoutGeometry.section('\n', 0, 0), 2), 3000).frameId;
            recorder.finish();
        }
        QVERIFY(indexText(options.directory, ids[0], fullText, QJsonArray{
            QJsonArray{80, 130, 1120, 60, "Picking up where we left off"},
            QJsonArray{80, 240, 1120, 60, firstLine},
            QJsonArray{80, 344, 1120, 50, secondLine},
            QJsonArray{80, 454, 1120, 50, unrelated}}));
        QVERIFY(indexText(options.directory, ids[1], withoutGeometry));

        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* copyStatus = viewer->findChild<QLabel*>("copyStatus");
        QVERIFY(search && results && copyStatus);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[1]);
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "contin");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 2);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QTRY_COMPARE(viewer->property("highlightCount").toInt(), 2);
        QTest::keyClick(results, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());

        auto* clipboard = QApplication::clipboard();
        clipboard->setText("synthetic clipboard sentinel");
        QTest::keyClick(viewer.get(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard->text(), firstLine + '\n' + secondLine);
        QVERIFY(copyStatus->isVisible());
        QVERIFY(!copyStatus->text().isEmpty());
        QTest::keyClick(viewer.get(), Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(clipboard->text(), fullText);

        // The input retains ordinary text selection and copy behavior.
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        search->setSelection(0, 3);
        QTest::keyClick(search, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard->text(), QString("con"));
        QCOMPARE(search->text(), QString("contin"));
        QTest::keyClick(search, Qt::Key_Escape);
        QTRY_VERIFY(viewer->hasFocus());

        // Copy immediately after selecting another frame, before the worker's
        // queued completion is delivered, must not reuse the previous lines.
        clipboard->setText("keep while positions load");
        results->setCurrentRow(1);
        QCOMPARE(viewer->property("selectedFrameId").toLongLong(), ids[1]);
        QKeyEvent pendingCopy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
        QApplication::sendEvent(viewer.get(), &pendingCopy);
        QCOMPARE(clipboard->text(), QString("keep while positions load"));
        QVERIFY(copyStatus->isVisible());
        QVERIFY(!copyStatus->text().isEmpty());

        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[1]);
        QTRY_COMPARE(viewer->property("highlightCount").toInt(), 0);
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTRY_VERIFY(viewer->findChild<QLabel*>("indexState")->toolTip().startsWith("No matching text positions"));
        clipboard->setText("keep when positions are unavailable");
        QTest::keyClick(viewer.get(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard->text(), QString("keep when positions are unavailable"));
        QVERIFY(copyStatus->isVisible());
        QVERIFY(copyStatus->text().contains("unavailable", Qt::CaseInsensitive));
        QTest::keyClick(viewer.get(), Qt::Key_C, Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(clipboard->text(), withoutGeometry);

        search->clear();
        QTRY_VERIFY(!results->isVisible());
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        clipboard->setText("copy all without a query");
        QTest::keyClick(viewer.get(), Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(clipboard->text(), withoutGeometry);
        viewer->close();
    }

    void dragSelectionUsesOriginalPixelsAndLeavesPendingHistoryUntouched() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("selection-history");
        options.deferredOcr = true; options.minFreeBytes = 0;
        const QImage source = prefixScreen("Only the selected text is copied", 1);
        qint64 id;
        { replay::Recorder recorder(options); id = recorder.addFrame(source, 1000).frameId; recorder.finish(); }
        auto probe = std::make_shared<SelectionOcrProbe>();
        auto viewer = replay::createViewer(options.directory, probe->hooks());
        viewer->resize(960, 720); viewer->show(); viewer->activateWindow();
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), id);
        auto* canvas = viewer->findChild<QWidget*>("recordedImage"); QVERIFY(canvas);
        auto* evidence = viewer->findChild<QScrollArea*>("evidenceView"); QVERIFY(evidence);
        auto* clipboard = QApplication::clipboard();
        clipboard->setText("keep until selected text is ready");
        QTest::mouseClick(canvas, Qt::LeftButton, Qt::NoModifier, canvas->rect().center());
        QTest::qWait(40); QCOMPARE(probe->count(), 0);
        const QPoint from(canvas->width() * 3 / 4, canvas->height() * 3 / 4);
        const QPoint to(canvas->width() / 4, canvas->height() / 4);
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
        moveTextSelection(canvas, to);
        QVERIFY(canvas->property("textSelectionActive").toBool());
        const QRect fitSelection = canvas->property("textSelectionRect").toRect();
        QVERIFY(qAbs(fitSelection.left() - source.width() / 4) <= 2);
        QVERIFY(qAbs(fitSelection.top() - source.height() / 4) <= 2);
        QVERIFY(qAbs(fitSelection.width() - source.width() / 2) <= 3);
        QVERIFY(qAbs(fitSelection.height() - source.height() / 2) <= 3);
        QVERIFY(QDir().mkpath("runs/selection-ocr"));
        QVERIFY(viewer->grab().save("runs/selection-ocr/selecting.png"));
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
        QTRY_COMPARE(probe->count(), 1);
        QVERIFY(viewer->property("selectionOcrRunning").toBool());
        QCOMPARE(clipboard->text(), QString("keep until selected text is ready"));
        QCOMPARE(probe->crop(0).convertToFormat(QImage::Format_RGBA8888), source.copy(fitSelection).convertToFormat(QImage::Format_RGBA8888));
        QVERIFY(viewer->grab().save("runs/selection-ocr/reading.png"));
        probe->release(1);
        QTRY_COMPARE(clipboard->text(), QString("Selected text 1"));
        QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
        QVERIFY(viewer->grab().save("runs/selection-ocr/copied.png"));
        QVERIFY(!probe->onGuiThread());
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frames"), qint64(1));
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frames WHERE ocr_state='pending'"), qint64(1));
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frame_text"), qint64(0));

        // Full-size scroll offsets must not shift the crop within the source.
        QTest::keyClick(viewer.get(), Qt::Key_1);
        QTRY_COMPARE(canvas->size(), source.size());
        evidence->horizontalScrollBar()->setValue(200);
        evidence->verticalScrollBar()->setValue(100);
        QVERIFY(evidence->horizontalScrollBar()->value() > 0);
        QVERIFY(evidence->verticalScrollBar()->value() > 0);
        const QPoint scrolledFrom = canvas->mapFrom(evidence->viewport(), QPoint(70, 60));
        const QPoint scrolledTo = canvas->mapFrom(evidence->viewport(), QPoint(290, 155));
        const QRect fullSelection = dragTextSelection(canvas, scrolledFrom, scrolledTo);
        QTRY_COMPARE(probe->count(), 2);
        QCOMPARE(fullSelection.topLeft(), scrolledFrom);
        QVERIFY(qAbs(fullSelection.width() - 220) <= 1);
        QVERIFY(qAbs(fullSelection.height() - 95) <= 1);
        QCOMPARE(probe->crop(1).convertToFormat(QImage::Format_RGBA8888), source.copy(fullSelection).convertToFormat(QImage::Format_RGBA8888));
        probe->release(2);
        QTRY_COMPARE(clipboard->text(), QString("Selected text 2"));

        // Dragging beyond the displayed image clamps to real source pixels.
        QTest::keyClick(viewer.get(), Qt::Key_F);
        const QRect edgeSelection = dragTextSelection(canvas, canvas->rect().center(), QPoint(-40, -30));
        QCOMPARE(edgeSelection.topLeft(), QPoint(0, 0));
        QVERIFY(source.rect().contains(edgeSelection));
        QTRY_COMPARE(probe->count(), 3);
        QCOMPARE(probe->crop(2).convertToFormat(QImage::Format_RGBA8888), source.copy(edgeSelection).convertToFormat(QImage::Format_RGBA8888));
        probe->release(3);
        QTRY_COMPARE(clipboard->text(), QString("Selected text 3"));
        QCOMPARE(probe->maximumActive(), 1);
        viewer->close();
    }

    void realSelectionOcrCopiesOnlyDraggedTextAtDesktopAndCompactSizes() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        const bool hadLanguage = qEnvironmentVariableIsSet("OMARCHY_OCR_LANGS");
        const QByteArray language = qgetenv("OMARCHY_OCR_LANGS");
        const auto restoreLanguage = qScopeGuard([&] {
            if (hadLanguage) qputenv("OMARCHY_OCR_LANGS", language); else qunsetenv("OMARCHY_OCR_LANGS");
        });
        qputenv("OMARCHY_OCR_LANGS", "eng");
        QImage source(1000, 320, QImage::Format_RGBA8888); source.fill(Qt::white);
        {
            QPainter painter(&source); painter.setPen(Qt::black);
            QFont font("DejaVu Sans"); font.setPixelSize(36); painter.setFont(font);
            painter.drawText(32, 55, "OUTSIDE ABOVE");
            painter.drawText(32, 170, "SELECTED TEXT 4821");
            painter.drawText(720, 170, "OUTSIDE");
            painter.drawText(32, 285, "OUTSIDE BELOW");
        }
        replay::RecorderOptions options;
        options.directory = temporary.filePath("real-selection-history");
        options.deferredOcr = true; options.minFreeBytes = 0;
        qint64 id;
        { replay::Recorder recorder(options); id = recorder.addFrame(source, 1000).frameId; recorder.finish(); }
        // No OCR hook: this exercises displayed archive pixels, crop mapping,
        // the native subprocess backend, and the guarded clipboard completion.
        auto viewer = replay::createViewer(options.directory);
        viewer->show(); viewer->activateWindow();
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), id);
        auto* canvas = viewer->findChild<QWidget*>("recordedImage"); QVERIFY(canvas);
        QVERIFY(QDir().mkpath("runs/selection-ocr"));
        for (const auto& size : {QSize(1440, 920), QSize(900, 620)}) {
            viewer->resize(size); QTest::qWait(60);
            const auto canvasPoint = [&](const QPoint& point) {
                return QPoint(qRound(double(point.x()) * canvas->width() / source.width()),
                              qRound(double(point.y()) * canvas->height() / source.height()));
            };
            const QPoint from = canvasPoint(QPoint(16, 110));
            const QPoint to = canvasPoint(QPoint(626, 200));
            QApplication::clipboard()->setText("untouched before recognition");
            QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
            moveTextSelection(canvas, to);
            QVERIFY(canvas->property("textSelectionActive").toBool());
            QVERIFY(viewer->grab().save(QString("runs/selection-ocr/real-%1-selecting.png").arg(size.width())));
            QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
            QTRY_COMPARE_WITH_TIMEOUT(QApplication::clipboard()->text(), QString("SELECTED TEXT 4821"), 12000);
            QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
            QVERIFY(!QApplication::clipboard()->text().contains("OUTSIDE"));
            QVERIFY(viewer->grab().save(QString("runs/selection-ocr/real-%1-copied.png").arg(size.width())));
        }
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frames WHERE ocr_state='pending'"), qint64(1));
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frame_text"), qint64(0));
        viewer->close();
    }

    void selectionCopyPreservesClipboardOnFailureCancellationAndNavigation() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("selection-cancellation-history");
        options.deferredOcr = true; options.minFreeBytes = 0;
        std::array<qint64, 2> ids{};
        {
            replay::Recorder recorder(options);
            ids[0] = recorder.addFrame(prefixScreen("Earlier synthetic note", 1), 1000).frameId;
            ids[1] = recorder.addFrame(prefixScreen("Later synthetic note", 2), 2000).frameId;
            recorder.finish();
        }
        auto probe = std::make_shared<SelectionOcrProbe>();
        probe->replies = {{" \n\t", {}, false}, {{}, "Synthetic recognition failure", false}};
        auto viewer = replay::createViewer(options.directory, probe->hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[1]);
        auto* canvas = viewer->findChild<QWidget*>("recordedImage"); QVERIFY(canvas);
        auto* notice = viewer->findChild<QLabel*>("copyStatus"); QVERIFY(notice);
        const auto select = [&] { return dragTextSelection(canvas, QPoint(40, 50), QPoint(260, 150)); };
        auto* clipboard = QApplication::clipboard();
        clipboard->setText("original clipboard");
        for (int call = 1; call <= 2; ++call) {
            QVERIFY(!select().isEmpty()); QTRY_COMPARE(probe->count(), call);
            probe->release(call);
            QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
            QCOMPARE(clipboard->text(), QString("original clipboard"));
            QVERIFY(notice->isVisible()); QVERIFY(!notice->text().isEmpty());
        }
        select(); QTRY_COMPARE(probe->count(), 3);
        clipboard->setText("newer copy from another app");
        probe->release(3);
        QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
        QCOMPARE(clipboard->text(), QString("newer copy from another app"));

        select(); QTRY_COMPARE(probe->count(), 4);
        QTest::keyClick(canvas, Qt::Key_Escape);
        QTRY_VERIFY(probe->cancelled(3));
        QVERIFY(viewer->isVisible());
        probe->release(4);
        QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
        QCOMPARE(clipboard->text(), QString("newer copy from another app"));

        select(); QTRY_COMPARE(probe->count(), 5);
        QTest::keyClick(viewer.get(), Qt::Key_Left);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[0]);
        QTRY_VERIFY(probe->cancelled(4));
        probe->release(5);
        QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
        QCOMPARE(clipboard->text(), QString("newer copy from another app"));
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frames WHERE ocr_state='pending'"), qint64(2));
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM frame_text"), qint64(0));
        QVERIFY(!probe->onGuiThread());
        probe->cooperateWithCancellation();
        select(); QTRY_COMPARE(probe->count(), 6);
        QElapsedTimer closeTime; closeTime.start();
        viewer->close();
        QVERIFY(closeTime.elapsed() < 2000);
        QVERIFY(probe->cancelled(5));
        QVERIFY(!viewer->isVisible());
        QVERIFY(!viewer->property("selectionOcrRunning").toBool());
        QCOMPARE(clipboard->text(), QString("newer copy from another app"));
    }

    void newestSelectionReplacesQueuedWorkWithoutParallelOcr() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("selection-queue-history");
        options.deferredOcr = true; options.minFreeBytes = 0;
        const QImage source = prefixScreen("The newest selection wins", 1);
        { replay::Recorder recorder(options); recorder.addFrame(source, 1000); recorder.finish(); }
        auto probe = std::make_shared<SelectionOcrProbe>();
        auto viewer = replay::createViewer(options.directory, probe->hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(viewer->property("displayedFrameId").toLongLong() > 0);
        auto* canvas = viewer->findChild<QWidget*>("recordedImage"); QVERIFY(canvas);
        QApplication::clipboard()->setText("keep while newer selection waits");
        dragTextSelection(canvas, QPoint(40, 50), QPoint(200, 120));
        QTRY_COMPARE(probe->count(), 1);
        dragTextSelection(canvas, QPoint(60, 130), QPoint(270, 230));
        QTRY_VERIFY(probe->cancelled(0));
        const QRect newest = dragTextSelection(canvas, QPoint(100, 180), QPoint(400, 320));
        QCOMPARE(probe->count(), 1);
        QCOMPARE(QApplication::clipboard()->text(), QString("keep while newer selection waits"));
        probe->release(1);
        QTRY_COMPARE(probe->count(), 2);
        QCOMPARE(probe->crop(1).convertToFormat(QImage::Format_RGBA8888), source.copy(newest).convertToFormat(QImage::Format_RGBA8888));
        QCOMPARE(QApplication::clipboard()->text(), QString("keep while newer selection waits"));
        probe->release(2);
        QTRY_COMPARE(QApplication::clipboard()->text(), QString("Selected text 2"));
        QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
        QCOMPARE(probe->count(), 2); QCOMPARE(probe->maximumActive(), 1);
        QVERIFY(!probe->onGuiThread());
        viewer->close();
    }

    void keyboardSelectionMovesResizesCopiesAndCancels() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        ViewerEnvironment environment(temporary.path());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("keyboard-selection-history");
        options.deferredOcr = true; options.minFreeBytes = 0;
        const QImage source = prefixScreen("Select this text with the keyboard", 1);
        { replay::Recorder recorder(options); recorder.addFrame(source, 1000); recorder.finish(); }
        auto probe = std::make_shared<SelectionOcrProbe>();
        auto viewer = replay::createViewer(options.directory, probe->hooks());
        viewer->show(); viewer->activateWindow();
        QTRY_VERIFY(viewer->property("displayedFrameId").toLongLong() > 0);
        auto* canvas = viewer->findChild<QWidget*>("recordedImage"); QVERIFY(canvas);
        QTest::keyClick(viewer->findChild<QLineEdit*>("recallSearch"), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_S);
        QVERIFY(canvas->property("textSelectionActive").toBool());
        const QRect initial = canvas->property("textSelectionRect").toRect();
        QVERIFY(!initial.isEmpty()); QVERIFY(source.rect().contains(initial));
        QTest::keyClick(canvas, Qt::Key_Right);
        const QRect moved = canvas->property("textSelectionRect").toRect();
        QVERIFY(moved.left() > initial.left());
        QCOMPARE(moved.top(), initial.top()); QCOMPARE(moved.height(), initial.height());
        // The visible selection retains its size, but enclosing source pixels
        // can differ by one column when the image is fitted at a fractional scale.
        QVERIFY(std::abs(moved.width() - initial.width()) <= 1);
        QTest::keyClick(canvas, Qt::Key_Left);
        QCOMPARE(canvas->property("textSelectionRect").toRect(), initial);
        QTest::keyClick(canvas, Qt::Key_Right);
        QCOMPARE(canvas->property("textSelectionRect").toRect(), moved);
        QTest::keyClick(canvas, Qt::Key_Down, Qt::ShiftModifier);
        const QRect resized = canvas->property("textSelectionRect").toRect();
        QCOMPARE(resized.topLeft(), moved.topLeft()); QVERIFY(resized.height() > moved.height());
        QApplication::clipboard()->setText("keyboard selection pending");
        QTest::keyClick(canvas, Qt::Key_Return);
        QTRY_COMPARE(probe->count(), 1);
        QCOMPARE(probe->crop(0).convertToFormat(QImage::Format_RGBA8888), source.copy(resized).convertToFormat(QImage::Format_RGBA8888));
        probe->release(1);
        QTRY_COMPARE(QApplication::clipboard()->text(), QString("Selected text 1"));
        QTRY_VERIFY(!viewer->property("selectionOcrRunning").toBool());
        QTest::keyClick(viewer.get(), Qt::Key_S);
        QVERIFY(canvas->property("textSelectionActive").toBool());
        QTest::keyClick(canvas, Qt::Key_Escape);
        QVERIFY(!canvas->property("textSelectionActive").toBool());
        QVERIFY(viewer->isVisible()); QCOMPARE(probe->count(), 1);
        QCOMPARE(QApplication::clipboard()->text(), QString("Selected text 1"));

        // Abandoning a keyboard selection must clear its untimed instructions
        // without submitting OCR or leaving arrows trapped in selection mode.
        auto* notice = viewer->findChild<QLabel*>("copyStatus"); QVERIFY(notice);
        QTest::keyClick(viewer.get(), Qt::Key_S);
        QTRY_VERIFY(canvas->hasFocus());
        QVERIFY(canvas->property("textSelectionActive").toBool()); QVERIFY(notice->isVisible());
        QTest::keyClick(canvas, Qt::Key_Tab);
        QTRY_VERIFY(!canvas->hasFocus());
        QTRY_VERIFY(!canvas->property("textSelectionActive").toBool());
        QVERIFY(!notice->isVisible());

        QTest::keyClick(viewer.get(), Qt::Key_S);
        QVERIFY(canvas->property("textSelectionActive").toBool()); QVERIFY(notice->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_1);
        QTRY_VERIFY(!canvas->property("textSelectionActive").toBool());
        QVERIFY(!notice->isVisible());

        QTest::keyClick(viewer.get(), Qt::Key_F);
        QTest::keyClick(viewer.get(), Qt::Key_S);
        QVERIFY(canvas->property("textSelectionActive").toBool()); QVERIFY(notice->isVisible());
        viewer->resize(900, 620);
        QTRY_VERIFY(!canvas->property("textSelectionActive").toBool());
        QVERIFY(!notice->isVisible());
        QCOMPARE(probe->count(), 1);
        QCOMPARE(QApplication::clipboard()->text(), QString("Selected text 1"));
        viewer->close();
    }

    void prefixMatchesPageAndSelectFromTimeline() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("paged-prefix-history");
        options.ocr = false;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            for (int i = 0; i < 225; ++i) {
                QImage image(64, 64, QImage::Format_RGBA8888);
                image.fill(QColor(30 + i, 80, 120));
                recorder.addFrame(image, 1000 + i * 2000);
            }
            recorder.finish();
        }
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        QCOMPARE(sqlite3_exec(database,
            "UPDATE frames SET text='continuous timeline fixture',ocr_state='ready';"
            "UPDATE frames SET text=text || ' uniqueneedle' WHERE id=17;"
            "INSERT INTO frame_text(rowid,text) SELECT id,text FROM frames", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* timeline = viewer->findChild<QSlider*>("recallTimeline");
        QVERIFY(search && results && timeline);
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "contin");
        QTRY_COMPARE(results->count(), 100);
        QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(225));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(1));
        QCOMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(0));
        QCOMPARE(timeline->property("matchMarkerCount").toInt(), 225);
        results->setFocus();
        QTRY_VERIFY(viewer->findChild<QPushButton*>("laterMoment")->isEnabled());
        QTest::keyClick(results, Qt::Key_Right);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));
        QCOMPARE(results->currentRow(), 1);
        QTest::keyClick(results, Qt::Key_Down);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(3));
        results->setCurrentRow(99);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(100));
        // Repeated input while a page read is queued must not skip a page or
        // restore an obsolete selection from the still-visible old page.
        QTest::keyClick(results, Qt::Key_Down);
        QTest::keyClick(results, Qt::Key_Down);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(100));
        QTRY_COMPARE(results->count(), 100);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(101));
        QTest::keyClick(results, Qt::Key_Up);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(0));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(100));
        QTest::keyClick(results, Qt::Key_PageDown);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(100));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(101));
        QTest::keyClick(results, Qt::Key_PageUp);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(0));

        // The final timeline hit is outside the loaded result page. Clicking
        // it loads that result's page and selects the same exact moment.
        QTest::mouseClick(timeline, Qt::LeftButton, Qt::NoModifier, QPoint(timeline->width() - 12, 19));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(225));
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(200));
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toLongLong(), qint64(225));
        QVERIFY(viewer->findChild<QLabel*>("resultsHeading")->text().startsWith("225 / 225"));
        QCOMPARE(search->text(), "contin");

        for (const auto repeatKey : {Qt::Key_Return, Qt::Key_F5}) {
            QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
            search->setText("continuous");
            QTest::keyClick(search, Qt::Key_Return);
            QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(225));
            QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(0));
            QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(1));
            QTest::keyClick(results, Qt::Key_PageDown);
            QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(100));
            QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(101));

            // The new query starts at zero while the old query's second page
            // is still displayed. A repeated submission/refresh before the
            // completion is delivered must preserve the pending query's page.
            QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
            search->setText("uniqueneedle");
            QTest::keyClick(search, Qt::Key_Return);
            QVERIFY(viewer->property("historyLoading").toBool());
            QTest::keyClick(repeatKey == Qt::Key_Return ? search : viewer.get(), repeatKey);
            QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(1));
            QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(0));
            QTRY_COMPARE(results->count(), 1);
            QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(17));
            QCOMPARE(results->currentItem()->data(Qt::UserRole).toLongLong(), qint64(17));
            QCOMPARE(search->text(), "uniqueneedle");
        }
        viewer->close();
    }

    void rollingDeletionKeepsSearchPagesReachable() {
        QTemporaryDir temporary;
        ViewerEnvironment environment(temporary.path());
        replay::RecorderOptions options;
        options.directory = replay::replayPaths().historyDirectory;
        options.resume = true; options.archiveFirst = true; options.deferredOcr = true;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            for (int i = 0; i < 225; ++i) {
                QImage image(64, 64, QImage::Format_RGBA8888);
                image.fill(QColor(20 + i, 80, 120));
                recorder.addFrame(image, 1000 + i * 2000);
            }
            recorder.finish();
        }
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        QCOMPARE(sqlite3_exec(database,
            "UPDATE frames SET text='continuous retention fixture',ocr_state='ready';"
            "INSERT INTO frame_text(rowid,text) SELECT id,text FROM frames", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);
        FakeRecording service;
        auto viewer = replay::createViewer(options.directory, service.hooks());
        viewer->show(); viewer->activateWindow();
        // The hidden initial 200-row list must not cap ordinary time browsing.
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(225));
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        search->setText("continuous");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(225));
        QTest::keyClick(results, Qt::Key_PageDown);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(100));
        QTest::keyClick(results, Qt::Key_PageDown);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(200));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(201));
        const auto expire = [&](qint64 from, qint64 until) {
            replay::HistoryMaintenanceResult result;
            for (int pass = 0; pass < 10; ++pass) {
                result = replay::deleteHistoryRange(options.directory, from, until);
                if (!result.more && !result.busy) return true;
            }
            return false;
        };
        QVERIFY(expire(0, 21000)); // First ten moments expire; the viewed ID moves to the previous page.
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(215));
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(100));
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(201));
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toLongLong(), qint64(201));
        QTest::keyClick(results, Qt::Key_PageDown);
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(200));
        results->setCurrentRow(14);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(225));
        QVERIFY(expire(0, 441000));
        QVERIFY(expire(449000, 450000)); // Delete the selection as well as the preceding complete pages.
        const auto fallback = replay::searchFramePage(options.directory, "continuous", 100, 200,
            replay::SearchMode::PrefixLastToken, 1000, 225);
        QCOMPARE(fallback.offset, qint64(0)); QCOMPARE(fallback.totalMatches, qint64(4));
        QCOMPARE(fallback.frames.size(), 4);
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QTRY_COMPARE(viewer->property("totalMatches").toLongLong(), qint64(4));
        QTRY_COMPARE(viewer->property("matchPageOffset").toLongLong(), qint64(0));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(221));
        QCOMPARE(results->count(), 4);
        QTest::keyClick(results, Qt::Key_Down);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(222));
        search->clear();
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(224));
        QVERIFY(expire(447000, 448000));
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        // Deleting the current unfiltered moment returns to the latest retained
        // image, not row zero of the hidden oldest-frame cache.
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(223));
        viewer->close();
    }

    void pendingImagesAndBackgroundRefresh() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("pending-history");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        replay::Recorder recorder(options);
        const auto first = recorder.addFrame(replay::fixtureFrame(0), 1000);
        const auto second = recorder.addFrame(replay::fixtureFrame(2), 3000);

        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* indexState = viewer->findChild<QLabel*>("indexState");
        auto* status = viewer->findChild<QLabel*>("recallStatus");
        auto* refresh = viewer->findChild<QPushButton*>("refreshHistory");
        QVERIFY(search && results && indexState && status && refresh);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), second.frameId);
        QCOMPARE(results->count(), 2);
        QVERIFY(indexState->text().contains("pending", Qt::CaseInsensitive));

        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "Patrick");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 0);
        QVERIFY(status->text().contains("pending", Qt::CaseInsensitive));
        QVERIFY(viewer->findChild<QLabel*>("mediaStatus")->text().contains("not searchable", Qt::CaseInsensitive));

        replay::IndexerOptions indexing;
        indexing.directory = options.directory;
        indexing.ocrMode = "full";
        replay::Indexer indexer(indexing);
        QCOMPARE(indexer.processNext().state, "ready");
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QTRY_COMPARE(results->count(), 1);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), first.frameId);

        results->setFocus();
        QTRY_VERIFY(viewer->findChild<QPushButton*>("laterMoment")->isEnabled());
        QTest::keyClick(results, Qt::Key_Right);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), second.frameId);
        QVERIFY(indexState->text().contains("pending", Qt::CaseInsensitive));
        QCOMPARE(indexer.processNext().state, "ready");

        // Refresh keeps the viewed moment in place and selects its newly
        // searchable result instead of retaining an unrelated match cursor.
        QTRY_COMPARE_WITH_TIMEOUT(results->count(), 2, 5000);
        QCOMPARE(search->text(), "Patrick");
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toLongLong(), second.frameId);
        QCOMPARE(viewer->property("selectedFrameId").toLongLong(), second.frameId);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), second.frameId);
        QVERIFY(indexState->text().isEmpty());
        QVERIFY(!indexState->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), second.frameId);
        recorder.finish();
        viewer->close();
    }

    void newerSelectionCancelsTimelineSeek() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("seek-order");
        options.ocr = false;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            for (int i = 0; i < 4; ++i) {
                QImage image(64, 64, QImage::Format_RGBA8888);
                image.fill(QColor(40 + i * 40, 80, 120));
                recorder.addFrame(image, 1000 + i * 2000);
            }
            recorder.finish();
        }
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(database, sqlite3_close);
        // DELETE mode lets this fixture hold reads while a seek is in flight.
        // Nothing outside this temporary dataset is changed.
        QCOMPARE(sqlite3_exec(database,
            "PRAGMA journal_mode=DELETE;"
            "UPDATE frames SET text='timeline fixture',ocr_state='ready';"
            "INSERT INTO frame_text(rowid,text) SELECT id,text FROM frames", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_busy_timeout(database, 1000);
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* timeline = viewer->findChild<QSlider*>("recallTimeline");
        QVERIFY(search && results && timeline);
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "timeline");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 4);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(1));

        // Both actions occur before the scrub debounce fires. The later result
        // must win even after enough time passes for an uncanceled seek to finish.
        timeline->setValue(timeline->maximum());
        results->setCurrentRow(1);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));
        QTest::qWait(150);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));

        results->setCurrentRow(0);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(1));
        QTRY_VERIFY(viewer->findChild<QPushButton*>("laterMoment")->isEnabled());
        timeline->setValue(timeline->maximum());
        QTest::keyClick(results, Qt::Key_Right);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));
        QTest::qWait(150);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));

        // A database read that has already started also cannot overwrite a
        // newer card choice when its stale completion finally arrives.
        results->setCurrentRow(1);
        results->setCurrentRow(0);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(1));
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QCOMPARE(sqlite3_exec(database, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr), SQLITE_OK);
        timeline->setValue(timeline->maximum());
        QTest::qWait(100); // Past the 35 ms debounce; the SQLite read is blocked.
        results->setCurrentRow(1);
        QCOMPARE(viewer->property("selectedFrameId").toLongLong(), qint64(2));
        QCOMPARE(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));
        QTest::qWait(200);
        QCOMPARE(viewer->property("selectedFrameId").toLongLong(), qint64(2));
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));
        viewer->close();
    }

    void horizontalResultsStayPutAcrossRefresh() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("scroll-history");
        options.ocr = false;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            for (int i = 0; i < 30; ++i) {
                QImage image(64, 64, QImage::Format_RGBA8888);
                image.fill(QColor(20 + i * 7, 80, 120));
                recorder.addFrame(image, 1000 + i * 2000);
            }
            recorder.finish();
        }
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(database, sqlite3_close);
        QCOMPARE(sqlite3_exec(database,
            "UPDATE frames SET text='timeline fixture',ocr_state='ready';"
            "INSERT INTO frame_text(rowid,text) SELECT id,text FROM frames", nullptr, nullptr, nullptr), SQLITE_OK);
        auto viewer = replay::createViewer(options.directory);
        viewer->resize(900, 620);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        QVERIFY(search && results);
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "timeline");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 30);
        results->setCurrentRow(18);
        results->scrollToItem(results->currentItem(), QAbstractItemView::PositionAtCenter);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(19));
        auto* scroll = results->horizontalScrollBar();
        QTRY_VERIFY(scroll->maximum() > 1000);
        const int position = scroll->value();
        QVERIFY(position > 0);
        const qint64 selectedId = results->currentItem()->data(Qt::UserRole).toLongLong();
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QCOMPARE(scroll->value(), position);
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toLongLong(), selectedId);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), selectedId);

        // Remove a later search hit so result count proves the automatic refresh
        // actually ran. Earlier card positions and the user's viewport remain.
        QCOMPARE(sqlite3_exec(database,
            "UPDATE frames SET text='different fixture' WHERE id=30;"
            "UPDATE frame_text SET text='different fixture' WHERE rowid=30", nullptr, nullptr, nullptr), SQLITE_OK);
        QTRY_COMPARE_WITH_TIMEOUT(results->count(), 29, 5000);
        QCOMPARE(scroll->value(), position);
        QCOMPARE(results->currentItem()->data(Qt::UserRole).toLongLong(), selectedId);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), selectedId);
        viewer->close();
    }

    void disabledTextIsNotClaimedRecognized() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("disabled-history");
        options.ocr = false;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            recorder.addFrame(replay::fixtureFrame(0), 1000);
            recorder.finish();
        }
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        auto* indexState = viewer->findChild<QLabel*>("indexState");
        auto* status = viewer->findChild<QLabel*>("recallStatus");
        QVERIFY(indexState && status);
        QTRY_VERIFY(indexState->text().contains("disabled", Qt::CaseInsensitive));
        QVERIFY(status->text().contains("disabled", Qt::CaseInsensitive));
        QVERIFY(!indexState->text().contains("no text was recognized", Qt::CaseInsensitive));
        viewer->close();
    }

    void servicePauseAndStopAreVisibleAndKeyboardAccessible() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("service-history");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            recorder.addFrame(prefixScreen("Saved while indexing waits", 1),
                QDateTime::currentMSecsSinceEpoch() - 7 * 60000);
            recorder.finish();
        }
        const QJsonObject policy{{"scheduler", "fixed"}, {"ocr_mode", "incremental"},
            {"ocr_cpu_percent", 10}, {"ocr_max_wall_ms", 60000}};
        replay::controlIndexService(options.directory, "pause", policy);
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        QTRY_VERIFY(viewer->property("displayedFrameId").toLongLong() > 0);
        auto* action = viewer->findChild<QPushButton*>("indexServiceAction");
        auto* stop = viewer->findChild<QPushButton*>("stopIndexService");
        auto* hint = viewer->findChild<QLabel*>("indexWorkerHint");
        QVERIFY(action && stop && hint);
        QCOMPARE(action->text(), "Resume");
        QVERIFY(hint->text().contains("Paused"));
        QVERIFY(viewer->findChild<QLabel*>("indexPendingAge")->text().contains("7 min"));
        QVERIFY(!viewer->findChild<QPushButton*>("catchUpIndexing")->isEnabled());
        QVERIFY(!viewer->findChild<QPushButton*>("copyIndexCommand")->isVisible());
        QTest::keyClick(viewer.get(), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        QVERIFY(action->isVisible());
        QVERIFY(stop->isVisible());
        QVERIFY(QDir().mkpath("runs/design-review-service"));
        QVERIFY(viewer->grab().save("runs/design-review-service/paused-desktop.png"));
        viewer->resize(900, 620);
        QTest::qWait(60);
        QVERIFY(viewer->grab().save("runs/design-review-service/paused-compact.png"));
        action->setFocus();
        QTest::keyClick(action, Qt::Key_Tab);
        QTRY_VERIFY(stop->hasFocus());
        QTest::keyClick(stop, Qt::Key_Space);
        QTRY_VERIFY(!replay::indexServiceStatus(options.directory).value("enabled").toBool());
        QTRY_VERIFY(!viewer->property("serviceRequestInFlight").toBool());
        QVERIFY(!replay::indexServiceStatus(options.directory).value("running").toBool());
        viewer->close();
        // Reopening history alone must not undo the saved pause or start work.
        auto reopened = replay::createViewer(options.directory);
        reopened->show();
        QTRY_VERIFY(reopened->property("displayedFrameId").toLongLong() > 0);
        QVERIFY(!replay::indexServiceStatus(options.directory).value("running").toBool());
        QCOMPARE(replay::indexingStatus(options.directory).value("pending").toInteger(), qint64(1));
        reopened->close();
    }

    void brokenServiceSettingsDoNotBlockHistoryOrStop() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("invalid-service-policy");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            recorder.addFrame(prefixScreen("Saved image remains usable", 1), 1000);
            recorder.finish();
        }
        QFile state(QDir(options.directory).filePath(".index-service.json"));
        QVERIFY(state.open(QIODevice::WriteOnly));
        QVERIFY(state.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        const QJsonObject policy{{"scheduler", "fixed"}, {"ocr_data_path", temporary.filePath("removed-model")}};
        QVERIFY(state.write(QJsonDocument(QJsonObject{{"enabled", true}, {"policy", policy}}).toJson()) > 0);
        state.close();
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        QTRY_VERIFY(viewer->property("displayedFrameId").toLongLong() > 0);
        auto* hint = viewer->findChild<QLabel*>("indexWorkerHint");
        QTRY_VERIFY(hint->text().contains("needs attention"));
        QTest::keyClick(viewer.get(), Qt::Key_Escape);
        QTest::keyClick(viewer.get(), Qt::Key_I);
        auto* stop = viewer->findChild<QPushButton*>("stopIndexService");
        QVERIFY(stop->isVisible());
        stop->setFocus();
        QTest::keyClick(stop, Qt::Key_Space);
        QTRY_VERIFY(!replay::indexServiceStatus(options.directory).value("enabled").toBool());
        QTRY_VERIFY(!viewer->property("serviceRequestInFlight").toBool());
        QVERIFY(viewer->property("displayedFrameId").toLongLong() > 0);
        viewer->close();
    }

    void dwellAndKeyboardRequestsPersistWithoutStartingWorker() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("priority-history");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        std::array<qint64, 4> ids{};
        {
            replay::Recorder recorder(options);
            const std::array<int, 4> scenes{0, 2, 4, 5};
            for (int i = 0; i < 4; ++i)
                ids[i] = recorder.addFrame(replay::fixtureFrame(scenes[i], QSize(640, 360)), 1000 + i * 60000).frameId;
            recorder.finish();
        }
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* process = viewer->findChild<QPushButton*>("processMoment");
        auto* catchUp = viewer->findChild<QPushButton*>("catchUpIndexing");
        auto* hint = viewer->findChild<QLabel*>("indexWorkerHint");
        QVERIFY(results && process && catchUp && hint);
        QTRY_COMPARE(results->count(), 4);
        // Rapid scrubbing should request only the final selected moment after
        // its dwell, not every pending image passed along the way.
        results->setFocus();
        results->setCurrentRow(1);
        QTest::keyClick(results, Qt::Key_Return);
        results->setCurrentRow(2);
        QTest::keyClick(results, Qt::Key_Return);
        QCOMPARE(replay::indexingStatus(options.directory)["priority_pending"].toInteger(), qint64(0));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), ids[2]);
        QTRY_COMPARE(replay::indexingStatus(options.directory)["priority_pending"].toInteger(), qint64(1));
        QTRY_VERIFY(!viewer->property("indexingRequestInFlight").toBool());
        QCOMPARE(storedNumber(options.directory, "SELECT frame_id FROM index_requests ORDER BY request_order DESC,rank LIMIT 1"), ids[2]);
        QCOMPARE(storedNumber(options.directory, "SELECT rank FROM index_requests ORDER BY request_order DESC,rank LIMIT 1"), qint64(0));
        QVERIFY(storedNumber(options.directory, "SELECT expires_ms FROM index_requests ORDER BY request_order DESC,rank LIMIT 1") > QDateTime::currentMSecsSinceEpoch());
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM index_requests"), qint64(1));
        const qint64 order = storedNumber(options.directory, "SELECT request_order FROM index_schedule WHERE id=1");
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QTest::qWait(2200); // Includes a regular refresh; it must not renew the request.
        QCOMPARE(storedNumber(options.directory, "SELECT request_order FROM index_schedule WHERE id=1"), order);
        QCOMPARE(viewer->property("selectedFrameId").toLongLong(), ids[2]);
        QVERIFY(hint->text().contains("indexing is stopped", Qt::CaseInsensitive));
        QVERIFY(viewer->findChild<QLabel*>("indexPendingAge")->text().contains("Oldest waiting"));
        QTest::mouseClick(viewer->findChild<QPushButton*>("toggleDetails"), Qt::LeftButton);
        QVERIFY(viewer->findChild<QPushButton*>("copyIndexCommand")->isVisible());

        results->setCurrentRow(3);
        QTest::keyClick(results, Qt::Key_Return);
        QTest::keyClick(results, Qt::Key_P);
        QTRY_COMPARE(replay::indexingStatus(options.directory)["priority_pending"].toInteger(), qint64(2));
        QTRY_VERIFY(!viewer->property("indexingRequestInFlight").toBool());
        QCOMPARE(storedNumber(options.directory, "SELECT frame_id FROM index_requests ORDER BY request_order DESC,rank LIMIT 1"), ids[3]);

        QTRY_VERIFY(catchUp->isEnabled());
        const qint64 requestedAt = QDateTime::currentMSecsSinceEpoch();
        QTest::keyClick(results, Qt::Key_C);
        QTRY_VERIFY(replay::indexingStatus(options.directory)["catch_up_until_ms"].toInteger() > requestedAt);
        const qint64 until = replay::indexingStatus(options.directory)["catch_up_until_ms"].toInteger();
        QVERIFY(until >= requestedAt + 119000 && until <= requestedAt + 122000);
        QTRY_VERIFY(!viewer->property("indexingRequestInFlight").toBool());
        QTRY_VERIFY(!catchUp->isEnabled());
        QTest::keyClick(results, Qt::Key_C);
        QCOMPARE(replay::indexingStatus(options.directory)["catch_up_until_ms"].toInteger(), until);
        QCOMPARE(replay::indexingStatus(options.directory)["ready"].toInteger(), qint64(0));
        QCOMPARE(replay::indexingStatus(options.directory)["pending"].toInteger(), qint64(4));
        QVERIFY(!replay::indexingStatus(options.directory)["indexer_running"].toBool());
        {
            replay::IndexerOptions indexing;
            indexing.directory = options.directory;
            replay::Indexer worker(indexing); // Holds the worker lock; no OCR is run.
            replay::publishIndexWorkerPolicy(options.directory, {{"mode", "pressure"}, {"effective_cpu_percent", 10}});
            QTest::keyClick(viewer.get(), Qt::Key_F5);
            QTRY_VERIFY(hint->text().startsWith("Another worker is indexing", Qt::CaseInsensitive));
            QVERIFY(!hint->text().contains("10% of one core"));
            QVERIFY(viewer->findChild<QLabel*>("indexWorkerDetails")->text().contains("10% of one core"));
            QTRY_VERIFY(!viewer->findChild<QPushButton*>("copyIndexCommand")->isVisible());
            QCOMPARE(viewer->property("selectedFrameId").toLongLong(), ids[3]);
        }
        viewer->close();
    }

    void failedPriorityWriteCanBeRetried() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("retry-priority");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        qint64 frameId = 0;
        {
            replay::Recorder recorder(options);
            frameId = recorder.addFrame(replay::fixtureFrame(0, QSize(640, 360)), 1000).frameId;
            recorder.finish();
        }
        // Initialize request tables, then reject writes without damaging the
        // saved image. This tests UI recovery independently of OCR timing.
        QCOMPARE(replay::requestCatchUp(options.directory, 1), 1);
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        QCOMPARE(sqlite3_exec(database,
            "CREATE TRIGGER reject_priority BEFORE INSERT ON index_requests BEGIN SELECT RAISE(ABORT,'synthetic request failure'); END",
            nullptr, nullptr, nullptr), SQLITE_OK);
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* message = viewer->findChild<QLabel*>("indexingRequestStatus");
        QTRY_COMPARE(results->count(), 1);
        results->setFocus();
        QTest::keyClick(results, Qt::Key_P);
        QTRY_VERIFY(message->text().contains("Unable to queue", Qt::CaseInsensitive));
        QTRY_VERIFY(!viewer->property("indexingRequestInFlight").toBool());
        QCOMPARE(replay::indexingStatus(options.directory)["priority_pending"].toInteger(), qint64(0));
        QCOMPARE(sqlite3_exec(database, "DROP TRIGGER reject_priority", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);
        QTest::keyClick(results, Qt::Key_P);
        QTRY_COMPARE(replay::indexingStatus(options.directory)["priority_pending"].toInteger(), qint64(1));
        QTRY_VERIFY(!viewer->property("indexingRequestInFlight").toBool());
        QTRY_VERIFY(!message->text().contains("Unable to queue", Qt::CaseInsensitive));
        QTRY_COMPARE(storedNumber(options.directory, "SELECT frame_id FROM index_requests WHERE rank=0"), frameId);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), frameId);
        viewer->close();
    }

    void closeSettlesBlockedRequestWithoutLateWrite() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("close-pending-request");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            recorder.addFrame(replay::fixtureFrame(0, QSize(640, 360)), 1000);
            recorder.finish();
        }
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        QTRY_COMPARE(results->count(), 1);
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(database, sqlite3_close);
        QCOMPARE(sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr), SQLITE_OK);
        results->setFocus();
        QTest::keyClick(results, Qt::Key_P);
        QVERIFY(viewer->property("indexingRequestInFlight").toBool());
        QTest::qWait(100);
        QElapsedTimer elapsed;
        elapsed.start();
        viewer->close();
        QVERIFY(elapsed.elapsed() < 2500);
        QVERIFY(!viewer->isVisible());
        QVERIFY(!viewer->property("indexingRequestInFlight").toBool());
        QVERIFY(!viewer->property("historyLoading").toBool());
        QCOMPARE(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr), SQLITE_OK);
        QTest::qWait(100);
        QCOMPARE(storedNumber(options.directory, "SELECT COUNT(*) FROM index_requests"), qint64(0));
    }

    void closeCancelsAndReapsSlowImageDecoder() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("close-decoder");
        options.ocr = false;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            recorder.addFrame(replay::fixtureFrame(0, QSize(640, 360)), 1000);
            recorder.addFrame(replay::fixtureFrame(2, QSize(640, 360)), 2000);
            recorder.finish();
        }
        // Use an owned sleeping executable instead of a real video decoder.
        // Only the synthetic row's codec changes; no desktop pixels are read.
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        QCOMPARE(sqlite3_exec(database, "UPDATE frames SET codec='h264' WHERE id=1", nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);
        const QString binaryDirectory = temporary.filePath("bin");
        QVERIFY(QDir().mkpath(binaryDirectory));
        QFile decoder(QDir(binaryDirectory).filePath("ffmpeg"));
        QVERIFY(decoder.open(QIODevice::WriteOnly));
        QVERIFY(decoder.write("#!/bin/sh\nprintf '%s' \"$$\" > \"$REPLAY_TEST_DECODER_PID\"\nexec sleep 30\n") > 0);
        decoder.close();
        QVERIFY(decoder.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        struct RestoreEnvironment {
            QByteArray path = qgetenv("PATH");
            QByteArray marker = qgetenv("REPLAY_TEST_DECODER_PID");
            bool hadMarker = qEnvironmentVariableIsSet("REPLAY_TEST_DECODER_PID");
            ~RestoreEnvironment() {
                qputenv("PATH", path);
                if (hadMarker) qputenv("REPLAY_TEST_DECODER_PID", marker);
                else qunsetenv("REPLAY_TEST_DECODER_PID");
            }
        } restore;
        const QString marker = temporary.filePath("decoder.pid");
        qputenv("PATH", binaryDirectory.toUtf8() + ':' + restore.path);
        qputenv("REPLAY_TEST_DECODER_PID", marker.toUtf8());
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(2));
        const QImage previousPixels = viewer->findChild<QWidget*>("recordedImage")->grab().toImage();
        auto* timeline = viewer->findChild<QSlider*>("recallTimeline");
        timeline->setFocus(); QTest::keyClick(timeline, Qt::Key_Home);
        QTRY_VERIFY(QFileInfo::exists(marker));
        QTRY_VERIFY(QFileInfo(marker).size() > 0);
        QCOMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(0));
        QVERIFY(viewer->property("mediaLoading").toBool());
        QVERIFY(!viewer->findChild<QLabel*>("mediaStatus")->isVisible());
        QCOMPARE(viewer->findChild<QWidget*>("recordedImage")->grab().toImage(), previousPixels);
        QApplication::clipboard()->setText("Previous pixels are not copyable context");
        QTest::keyClick(timeline, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), "Previous pixels are not copyable context");
        QFile pidFile(marker);
        QVERIFY(pidFile.open(QIODevice::ReadOnly));
        const auto pid = pidFile.readAll().toLongLong();
        QVERIFY(pid > 1);
        QElapsedTimer elapsed;
        elapsed.start();
        viewer->close();
        QVERIFY(elapsed.elapsed() < 2500);
        QVERIFY(!viewer->property("mediaLoading").toBool());
        QVERIFY(::kill(pid_t(pid), 0) == -1 && errno == ESRCH);
    }

    void failedIndexExplainsEmptySearchAndKeepsImagesBrowsable() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("failed-history");
        options.deferredOcr = true;
        options.minFreeBytes = 0;
        qint64 frameId = 0;
        {
            replay::Recorder recorder(options);
            frameId = recorder.addFrame(replay::fixtureFrame(0), 1000).frameId;
            recorder.finish();
        }
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        viewer->activateWindow();
        auto* search = viewer->findChild<QLineEdit*>("recallSearch");
        auto* results = viewer->findChild<QListWidget*>("recallResults");
        auto* message = viewer->findChild<QLabel*>("mediaStatus");
        QVERIFY(search && results && message);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), frameId);
        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClicks(search, "Patrick");
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 0);
        QVERIFY(message->text().contains("pending", Qt::CaseInsensitive));

        // Inject a worker failure in this synthetic dataset without making the
        // GUI test depend on OCR speed or an intentional wall-clock timeout.
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        QCOMPARE(sqlite3_exec(database,
            "UPDATE frames SET ocr_state='failed',ocr_error='Synthetic deadline failure' WHERE ocr_state='pending'",
            nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);
        QTest::keyClick(viewer.get(), Qt::Key_F5);
        QTRY_VERIFY(!viewer->property("historyLoading").toBool());
        QCOMPARE(results->count(), 0);
        QVERIFY(message->text().contains("indexing failed", Qt::CaseInsensitive));
        QVERIFY(message->text().contains("Clear the search", Qt::CaseInsensitive));
        QVERIFY(message->text().contains("browse", Qt::CaseInsensitive));
        QVERIFY(!message->text().contains("spelling", Qt::CaseInsensitive));
        QVERIFY(!message->text().contains("shorter word", Qt::CaseInsensitive));

        QTest::keyClick(viewer.get(), Qt::Key_F, Qt::ControlModifier);
        QTest::keyClick(search, Qt::Key_Backspace);
        QTest::keyClick(search, Qt::Key_Return);
        QTRY_COMPARE(results->count(), 1);
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), frameId);
        QVERIFY(viewer->findChild<QLabel*>("indexState")->text().contains("failed", Qt::CaseInsensitive));
        viewer->close();
    }

    void legacyEmptyTextKeepsUnknownStatus() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        replay::RecorderOptions options;
        options.directory = temporary.filePath("legacy-history");
        options.ocr = false;
        options.minFreeBytes = 0;
        {
            replay::Recorder recorder(options);
            recorder.addFrame(replay::fixtureFrame(0), 1000);
            recorder.finish();
        }
        // Reproduce the old reader schema, which had no indexing-state fields.
        sqlite3* database = nullptr;
        QCOMPARE(sqlite3_open(QDir(options.directory).filePath("index.sqlite").toUtf8().constData(), &database), SQLITE_OK);
        const char* legacy =
            "ALTER TABLE frames RENAME TO new_frames;"
            "CREATE TABLE frames AS SELECT id,timestamp_ms,last_timestamp_ms,observation_count,segment_id,"
            "frame_index,path,codec,width,height,text FROM new_frames;"
            "DROP TABLE new_frames;"
            "UPDATE metadata SET value='1' WHERE key='schema_version';";
        QCOMPARE(sqlite3_exec(database, legacy, nullptr, nullptr, nullptr), SQLITE_OK);
        sqlite3_close(database);
        auto viewer = replay::createViewer(options.directory);
        viewer->show();
        auto* indexState = viewer->findChild<QLabel*>("indexState");
        QVERIFY(indexState);
        QTRY_VERIFY(indexState->text().contains("not recorded", Qt::CaseInsensitive));
        QVERIFY(!indexState->text().contains("no text was recognized", Qt::CaseInsensitive));
        QTRY_COMPARE(viewer->property("displayedFrameId").toLongLong(), qint64(1));
        viewer->close();
    }
};

QTEST_MAIN(ViewerTest)
#include "viewer_test.moc"
