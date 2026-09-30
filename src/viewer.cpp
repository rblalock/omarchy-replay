#include "viewer.h"
#include "agent_prompt.h"
#include "exclusion_presets.h"
#include "recorder.h"
#include "index_service.h"
#include "replay_config.h"
#include "recording_service.h"
#include "storage_forecast.h"
#include "meeting_index.h"
#include "meeting_view.h"

#include <QApplication>
#include <QClipboard>
#include <QCryptographicHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QFileDialog>
#include <QJsonDocument>
#include <QProcess>
#include <QUrl>
#include <QVariantAnimation>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QSlider>
#include <QStandardPaths>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QProgressBar>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <sys/stat.h>
#include <unistd.h>

namespace replay {
namespace {

struct ReplayColors {
    QColor background = QColor("#16181c"), foreground = QColor("#e5e7eb");
    QColor accent = QColor("#8ab4f8"), surface, muted, border;
};

QColor mix(const QColor& a, const QColor& b, double amount) {
    return QColor::fromRgbF(a.redF() * amount + b.redF() * (1 - amount),
        a.greenF() * amount + b.greenF() * (1 - amount),
        a.blueF() * amount + b.blueF() * (1 - amount));
}

ReplayColors theme(QWidget* widget) {
    ReplayColors colors;
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state");
    const QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    for (const QString& path : {state + "/omarchy/current/theme/colors.toml", config + "/omarchy/current/theme/colors.toml"}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QString data = QString::fromUtf8(file.read(65536));
        for (const auto& entry : {std::pair{"background", &colors.background},
                                  std::pair{"foreground", &colors.foreground},
                                  std::pair{"accent", &colors.accent}}) {
            const auto match = QRegularExpression(QString("(?m)^%1\\s*=\\s*[\"'](#[0-9a-fA-F]{6})[\"']").arg(entry.first)).match(data);
            if (match.hasMatch()) *entry.second = QColor(match.captured(1));
        }
        break;
    }
    colors.surface = mix(colors.foreground, colors.background, .045);
    colors.muted = mix(colors.foreground, colors.background, .72);
    colors.border = mix(colors.foreground, colors.background, .23);
    const auto buttonFill = mix(colors.foreground, colors.background, .075);
    const auto buttonBorder = mix(colors.foreground, colors.background, .48);
    const auto buttonHover = mix(colors.foreground, colors.background, .14);
    const auto buttonSelected = mix(colors.accent, colors.background, .18);
    const auto disabledText = mix(colors.foreground, colors.background, .42);
    const auto primaryHover = mix(colors.foreground, colors.accent, .15);
    QPalette palette = widget->palette();
    palette.setColor(QPalette::Window, colors.background);
    palette.setColor(QPalette::Base, colors.background);
    palette.setColor(QPalette::WindowText, colors.foreground);
    palette.setColor(QPalette::Text, colors.foreground);
    palette.setColor(QPalette::PlaceholderText, colors.muted);
    palette.setColor(QPalette::ButtonText, colors.foreground);
    palette.setColor(QPalette::Highlight, colors.accent);
    palette.setColor(QPalette::HighlightedText, colors.background);
    widget->setPalette(palette);
    widget->setAutoFillBackground(true);
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QFile terminal(config + "/alacritty/alacritty.toml");
    if (terminal.open(QIODevice::ReadOnly)) {
        const auto match = QRegularExpression("family\\s*=\\s*\"([^\"]+)\"").match(QString::fromUtf8(terminal.read(65536)));
        if (match.hasMatch()) font.setFamily(match.captured(1));
    }
    font.setPointSize(10);
    widget->setFont(font);
    widget->setStyleSheet(QString(R"(
        QWidget { color: %1; }
        QLineEdit, QComboBox { background: %2; border: 1px solid %8; padding: 8px 12px; selection-background-color: %4; selection-color: %5; }
        QLineEdit:focus, QComboBox:focus { border-color: %4; }
        QComboBox QAbstractItemView { background: %5; selection-background-color: %4; selection-color: %5; }
        QAbstractSpinBox, QPlainTextEdit, QTableWidget { background: %5; border: 1px solid %8; padding: 4px; }
        QAbstractSpinBox:focus, QPlainTextEdit:focus, QTableWidget:focus { border-color: %4; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: transparent; color: %6; padding: 9px 12px; border-bottom: 2px solid transparent; }
        QTabBar::tab:selected { color: %1; border-bottom-color: %4; }
        QHeaderView::section { background: %2; color: %6; border: none; padding: 5px; }
        QPushButton { background: %7; border: 1px solid %8; border-radius: 2px; padding: 6px 10px; }
        QPushButton:hover { background: %9; border-color: %6; }
        QPushButton:checked { background: %10; border-color: %4; }
        QPushButton:pressed { background: %10; border-color: %4; padding: 7px 10px 5px 10px; }
        QPushButton:focus { border: 2px solid %4; padding: 5px 9px; }
        QPushButton:focus:pressed { padding: 6px 9px 4px 9px; }
        QPushButton[primary="true"] { background: %4; color: %5; border-color: %4; }
        QPushButton[primary="true"]:hover { background: %12; border-color: %12; }
        QPushButton[primary="true"]:pressed { background: %4; border-color: %1; }
        QPushButton[primary="true"]:focus { border-color: %1; }
        QPushButton:disabled, QPushButton[primary="true"]:disabled { background: %2; color: %11; border: 1px solid %3; padding: 6px 10px; }
        QListWidget { background: transparent; border: none; outline: none; }
        QListWidget::item { padding: 3px 12px; border: none; border-bottom: 2px solid transparent; color: %6; }
        QListWidget::item:hover { background: %2; }
        QListWidget::item:selected { border-bottom-color: %4; background: %2; color: %1; }
        QListWidget:focus { border-bottom: 1px solid %3; }
        QProgressBar { border: none; background: %3; height: 3px; }
        QProgressBar::chunk { background: %4; }
        QLabel[keycap="true"] { color: %1; background: %5; padding: 4px 7px; }
        QWidget#helpPanel { background: %2; }
        QScrollArea { border: none; background: %2; }
        QScrollBar:horizontal { height: 5px; background: %5; }
        QScrollBar:vertical { width: 6px; background: %5; }
        QScrollBar::handle { background: %3; min-width: 16px; min-height: 16px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QLabel#keyboardHelp, QLabel#indexState, QLabel#recallStatus { color: %6; }
        QWidget#detailsPanel { background: %2; }
    )").arg(colors.foreground.name(), colors.surface.name(), colors.border.name(),
            colors.accent.name(), colors.background.name(), colors.muted.name(),
            buttonFill.name(), buttonBorder.name(), buttonHover.name(), buttonSelected.name(),
            disabledText.name(), primaryHover.name()));
    return colors;
}

class ImageCanvas final : public QWidget {
public:
    QImage image;
    QVector<QRect> highlights;
    bool showHighlights = true;
    std::function<void()> selectionStarted;
    std::function<void()> selectionCancelled;
    std::function<void(const QRect&)> selectionFinished;
    explicit ImageCanvas(const QColor& accent) : accent_(accent) {
        setObjectName("recordedImage");
        setAccessibleName("Recorded screen. Drag to copy text, or press S to select with the keyboard.");
        setFocusPolicy(Qt::StrongFocus);
        clearSelection();
    }
    void setSelectionAvailable(bool available) {
        available_ = available;
        setCursor(available ? Qt::CrossCursor : Qt::ArrowCursor);
        if (!available) clearSelection();
    }
    bool selectionActive() const { return dragging_ || keyboard_ || !selection_.isEmpty(); }
    void clearSelection() {
        dragging_ = keyboard_ = false;
        selection_ = {};
        publishSelection();
    }
    void beginKeyboardSelection(QRect visible) {
        if (!available_ || image.isNull()) return;
        if (selectionStarted) selectionStarted();
        setFocus(Qt::ShortcutFocusReason);
        visible = visible.intersected(rect());
        if (visible.isEmpty()) return;
        const QSizeF size(std::min(320, visible.width()), std::min(100, visible.height()));
        selection_ = QRectF(QPointF(visible.center()) - QPointF(size.width() / 2, size.height() / 2), size);
        keyboard_ = true;
        publishSelection();
    }
    bool selectionKey(QKeyEvent* key) {
        if (!keyboard_) return false;
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && key->modifiers() == Qt::NoModifier) {
            keyboard_ = false;
            if (selectionFinished) selectionFinished(sourceSelection());
            return true;
        }
        if (key->modifiers() != Qt::NoModifier && key->modifiers() != Qt::ShiftModifier) return false;
        QPointF delta;
        switch (key->key()) {
        case Qt::Key_Left: delta.setX(-5); break;
        case Qt::Key_Right: delta.setX(5); break;
        case Qt::Key_Up: delta.setY(-5); break;
        case Qt::Key_Down: delta.setY(5); break;
        default: return false;
        }
        if (key->modifiers() == Qt::ShiftModifier) {
            selection_.setWidth(std::clamp(selection_.width() + delta.x(), 1., width() - selection_.left()));
            selection_.setHeight(std::clamp(selection_.height() + delta.y(), 1., height() - selection_.top()));
        } else {
            selection_.moveLeft(std::clamp(selection_.left() + delta.x(), 0., width() - selection_.width()));
            selection_.moveTop(std::clamp(selection_.top() + delta.y(), 0., height() - selection_.height()));
        }
        publishSelection();
        return true;
    }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !available_ || image.isNull()) return;
        if (selectionStarted) selectionStarted();
        setFocus(Qt::MouseFocusReason);
        anchor_ = bounded(event->position());
        dragging_ = true;
        selection_ = {};
        publishSelection();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (!dragging_) return;
        updateDrag(event->position());
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || !dragging_) return;
        updateDrag(event->position());
        dragging_ = false;
        const QRect crop = sourceSelection();
        if (!crop.isEmpty() && selectionFinished) selectionFinished(crop);
        else clearSelection();
        event->accept();
    }
    void resizeEvent(QResizeEvent* event) override {
        if ((dragging_ || keyboard_) && selectionCancelled) selectionCancelled();
        else if (selectionActive()) clearSelection();
        QWidget::resizeEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override {
        if (keyboard_ && selectionCancelled) selectionCancelled();
        QWidget::focusOutEvent(event);
    }
    void paintEvent(QPaintEvent*) override {
        if (image.isNull()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(rect(), image);
        if (showHighlights) {
            painter.save();
            painter.scale(double(width()) / image.width(), double(height()) / image.height());
            QPen pen(QColor("#f6cc59"));
            pen.setCosmetic(true);
            pen.setWidth(2);
            painter.setPen(pen);
            painter.setBrush(QColor(246, 204, 89, 62));
            for (const auto& box : highlights) painter.drawRect(box);
            painter.restore();
        }
        if (!selection_.isEmpty()) {
            painter.setPen(QPen(QColor(0, 0, 0, 190), 4));
            QColor fill = accent_; fill.setAlpha(28);
            painter.setBrush(fill);
            painter.drawRect(selection_);
            painter.setPen(QPen(accent_, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selection_);
        }
    }
private:
    QPointF bounded(QPointF point) const {
        return {std::clamp(point.x(), 0., double(width())), std::clamp(point.y(), 0., double(height()))};
    }
    void updateDrag(QPointF point) {
        point = bounded(point);
        selection_ = (point - anchor_).manhattanLength() >= QApplication::startDragDistance()
            ? QRectF(anchor_, point).normalized() : QRectF();
        publishSelection();
    }
    QRect sourceSelection() const {
        if (selection_.isEmpty() || image.isNull() || width() <= 0 || height() <= 0) return {};
        const double sx = double(image.width()) / width(), sy = double(image.height()) / height();
        // Enclose every selected pixel. At fractional fit scales, translating a
        // fixed-size widget rectangle can change its integer crop extent by one.
        const int left = std::clamp(int(std::floor(selection_.left() * sx)), 0, image.width());
        const int top = std::clamp(int(std::floor(selection_.top() * sy)), 0, image.height());
        const int right = std::clamp(int(std::ceil(selection_.right() * sx)), left, image.width());
        const int bottom = std::clamp(int(std::ceil(selection_.bottom() * sy)), top, image.height());
        return {left, top, right - left, bottom - top};
    }
    void publishSelection() {
        setProperty("textSelectionActive", selectionActive());
        setProperty("textSelectionRect", sourceSelection());
        update();
    }
    QColor accent_;
    bool available_ = false, dragging_ = false, keyboard_ = false;
    QPointF anchor_;
    QRectF selection_;
};

class EvidenceView final : public QScrollArea {
public:
    explicit EvidenceView(const QColor& accent) {
        setObjectName("evidenceView");
        setAccessibleName("Recorded screen image");
        setAlignment(Qt::AlignCenter);
        setFrameShape(QFrame::NoFrame);
        canvas_ = new ImageCanvas(accent);
        setWidget(canvas_);
        setFocusPolicy(Qt::StrongFocus);
    }
    void setImage(const QImage& image) {
        canvas_->clearSelection(); canvas_->image = image;
        canvas_->setSelectionAvailable(!image.isNull());
        setProperty("hasImage", !image.isNull()); render();
    }
    ImageCanvas* canvas() const { return canvas_; }
    void beginKeyboardSelection() {
        canvas_->beginKeyboardSelection(QRect(canvas_->mapFrom(viewport(), QPoint()), viewport()->size()));
    }
    void setHighlights(const QVector<QRect>& boxes) { canvas_->highlights = boxes; canvas_->update(); }
    void toggleHighlights() { canvas_->showHighlights = !canvas_->showHighlights; canvas_->update(); }
    void setFit(bool fit) { fit_ = fit; setProperty("fit", fit); render(); }
protected:
    void resizeEvent(QResizeEvent* event) override { QScrollArea::resizeEvent(event); render(); }
private:
    void render() {
        const auto& image = canvas_->image;
        const QSize target = image.isNull() ? viewport()->size() : fit_
            ? image.size().scaled(viewport()->size(), Qt::KeepAspectRatio) : image.size();
        canvas_->resize(target);
        canvas_->update();
    }
    ImageCanvas* canvas_ = nullptr;
    bool fit_ = true;
};

class TimelineView final : public QSlider {
public:
    explicit TimelineView(const ReplayColors& colors) : QSlider(Qt::Horizontal), colors_(colors) {
        setObjectName("recallTimeline");
        setAccessibleName("Recorded history timeline");
        setRange(0, 1000000);
        setFixedHeight(56);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        marker_.setDuration(160);
        marker_.setStartValue(4.5);
        marker_.setEndValue(3.);
        marker_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&marker_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            markerRadius_ = value.toDouble(); update();
        });
    }
    void setOverview(const TimelineOverview& overview) { overview_ = overview; update(); }
    void setMatches(const TimelineOverview& matches) {
        hits_.clear();
        for (const auto& point : matches.points) hits_.append(point);
        setProperty("matchMarkerCount", hits_.size());
        update();
    }
    std::function<void(qint64)> activateMatch;
    std::function<void(qint64)> activateMeeting;
    void setMeetings(const QVector<MeetingTimelinePoint>& meetings) {
        meetings_ = meetings;
        setProperty("meetingMarkerCount", meetings_.size());
        update();
    }
    void setMoment(qint64 moment) {
        if (moment_ && moment_ != moment && isVisible()) { marker_.stop(); marker_.start(); }
        moment_ = moment;
        setAccessibleDescription(QDateTime::fromMSecsSinceEpoch(moment).toString("dddd MMMM d HH:mm:ss"));
        const QSignalBlocker blocker(this);
        setValue(int(fraction(moment) * maximum()));
        update();
    }
    qint64 timeAt(int value) const {
        return overview_.firstTimestampMs + qint64(double(value) / maximum() *
            (overview_.lastTimestampMs - overview_.firstTimestampMs));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        const int left = 12, right = width() - 12, y = 19;
        p.setPen(QPen(colors_.border, 3, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(left, y, right, y);
        if (!overview_.totalFrames) return;
        QSet<int> occupied;
        p.setPen(Qt::NoPen);
        p.setBrush(colors_.muted);
        for (const auto& point : overview_.points) {
            const int x = left + fraction(point.timestampMs) * (right - left);
            if (occupied.contains(x / 3)) continue;
            occupied.insert(x / 3);
            p.drawEllipse(QPointF(x, y), 1.5, 1.5);
        }
        occupied.clear();
        p.setPen(QPen(colors_.accent, 1.5));
        p.setBrush(colors_.background);
        for (const auto& meeting : meetings_) {
            const int x = left + fraction(meeting.startedAtMs) * (right - left);
            if (occupied.contains(x / 8)) continue;
            occupied.insert(x / 8);
            p.drawRect(QRectF(x - 3, 4, 6, 6));
        }
        occupied.clear();
        p.setPen(Qt::NoPen);
        p.setBrush(colors_.accent);
        for (const auto& hit : hits_) {
            const int x = left + fraction(hit.timestampMs) * (right - left);
            if (occupied.contains(x / 5)) continue;
            occupied.insert(x / 5);
            p.drawRoundedRect(QRectF(x - 3, y - 4, 6, 8), 3, 3);
        }
        const int cursor = left + fraction(moment_) * (right - left);
        p.setPen(QPen(colors_.foreground, 1));
        p.drawLine(cursor, 4, cursor, 31);
        p.setBrush(colors_.foreground);
        p.drawEllipse(QPointF(cursor, y), markerRadius_, markerRadius_);
        p.setPen(colors_.muted);
        p.drawText(QRect(left, 35, width()/2-12, 20), Qt::AlignLeft,
            QDateTime::fromMSecsSinceEpoch(overview_.firstTimestampMs).toString("MMM d · HH:mm:ss"));
        p.drawText(QRect(width()/2, 35, width()/2-12, 20), Qt::AlignRight,
            QDateTime::fromMSecsSinceEpoch(overview_.lastTimestampMs).toString("MMM d · HH:mm:ss"));
        if (hasFocus()) { p.setPen(colors_.accent); p.drawLine(12, height()-1, width()-12, height()-1); }
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton) return;
        setFocus();
        if (event->position().y() <= 14 && activateMeeting) {
            qint64 nearest = 0;
            double distance = 10;
            for (const auto& meeting : meetings_) {
                const double delta = std::abs(12 + fraction(meeting.startedAtMs) * (width() - 24) - event->position().x());
                if (delta < distance) { nearest = meeting.id; distance = delta; }
            }
            if (nearest) { activateMeeting(nearest); return; }
        }
        if (std::abs(event->position().y() - 19) < 12 && activateMatch) {
            qint64 nearest = 0;
            double distance = 9;
            bool found = false;
            for (const auto& hit : hits_) {
                const double delta = std::abs(12 + fraction(hit.timestampMs) * (width() - 24) - event->position().x());
                if (delta < distance) { nearest = hit.id; distance = delta; found = true; }
            }
            if (found) { activateMatch(nearest); return; }
        }
        scrub(event->position().x());
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        if (event->buttons() & Qt::LeftButton) scrub(event->position().x());
    }

private:
    double fraction(qint64 time) const {
        if (overview_.lastTimestampMs <= overview_.firstTimestampMs) return 0;
        return std::clamp(double(time - overview_.firstTimestampMs) /
            (overview_.lastTimestampMs - overview_.firstTimestampMs), 0., 1.);
    }
    void scrub(double x) { setValue(int(std::clamp((x - 12) / std::max(1, width() - 24), 0., 1.) * maximum())); }
    ReplayColors colors_;
    TimelineOverview overview_;
    QVector<TimelinePoint> hits_;
    QVector<MeetingTimelinePoint> meetings_;
    qint64 moment_ = 0;
    QVariantAnimation marker_;
    double markerRadius_ = 3;
};

struct DecodedFrame {
    qint64 id = 0;
    QString directory;
    QImage image;
    QString error;
};

struct RecallResult {
    FrameRecord frame;
    MeetingSearchResult meeting;
    bool isMeeting = false;
    qint64 key() const { return isMeeting ? -meeting.meeting.id : frame.id; }
};

struct HistorySnapshot {
    quint64 generation = 0;
    quint64 selectionRevision = 0;
    QString query;
    QString source;
    bool preserve = false;
    qint64 currentId = 0;
    QJsonObject indexing;
    QJsonObject service, policy;
    QJsonObject recording;
    QVector<FrameRecord> matches;
    QVector<RecallResult> rows;
    QVector<MeetingTimelinePoint> meetings;
    qint64 meetingCount = 0;
    qint64 totalResults = 0;
    qint64 resultOffset = 0;
    int selectedRow = -1;
    TimelineOverview timeline;
    SearchPage page;
    qint64 offset = 0;
    int targetRow = -1;
    qint64 anchorFrameId = 0;
    std::optional<FrameRecord> current;
    QString error;
};

// Meeting results are grouped first in All; screen matches retain their existing
// chronological paging. Never load transcript bodies merely to render a page.
void readRecallResults(const QString& directory, HistorySnapshot& result) {
    const bool blank = result.query.trimmed().isEmpty();
    result.meetingCount = listMeetings(directory, 1).totalMatches;
    if (result.source != "screens") result.meetings = meetingTimeline(directory);
    if (result.source == "screens") {
        if (blank) result.matches = listFrames(directory, 200, 0);
        else {
            result.page = searchFramePage(directory, result.query, 100, result.offset,
                SearchMode::PrefixLastToken, 1000, result.anchorFrameId);
            result.matches = result.page.frames;
        }
        result.totalResults = result.page.totalMatches;
        result.resultOffset = result.page.offset;
        result.selectedRow = result.page.selectedRow;
        for (const auto& frame : result.matches) result.rows.append({frame, {}, false});
        return;
    }
    const auto meetings = [&](qint64 offset) {
        return blank ? listMeetings(directory, 100, offset) : searchMeetings(directory, result.query, 100, offset);
    };
    auto meetingPage = meetings(result.offset);
    const qint64 meetingTotal = meetingPage.totalMatches;
    const bool screens = result.source == "all" && !blank;
    qint64 offset = result.offset;
    if (screens) {
        result.page = searchFramePage(directory, result.query, 100, std::max<qint64>(0, offset - meetingTotal),
            SearchMode::PrefixLastToken, 1000, result.anchorFrameId);
        if (result.anchorFrameId && result.page.selectedRow >= 0) {
            offset = meetingTotal + result.page.offset;
            result.selectedRow = result.page.selectedRow;
        }
    }
    result.totalResults = meetingTotal + (screens ? result.page.totalMatches : 0);
    const qint64 validOffset = result.totalResults && offset >= result.totalResults
        ? ((result.totalResults - 1) / 100) * 100 : offset;
    if (validOffset != offset) {
        offset = validOffset;
        meetingPage = meetings(offset);
        if (screens) result.page = searchFramePage(directory, result.query, 100, std::max<qint64>(0, offset - meetingTotal),
            SearchMode::PrefixLastToken, 1000);
    }
    result.resultOffset = offset;
    if (offset < meetingTotal) {
        for (const auto& meeting : meetingPage.results) result.rows.append({{}, meeting, true});
    }
    if (screens) {
        result.matches = result.page.frames;
        for (const auto& frame : result.matches) {
            if (result.rows.size() >= 100) break;
            result.rows.append({frame, {}, false});
        }
    } else if (blank && result.source == "all") {
        result.matches = listFrames(directory, 200, 0);
        // Keep the ordinary empty-query screen list (including pending images)
        // intact when no meeting rows are present.
        if (!meetingTotal) for (const auto& frame : result.matches) result.rows.append({frame, {}, false});
    }
}

struct TimelineNeighbors {
    qint64 id = 0;
    QString directory;
    std::optional<FrameRecord> previous, next;
    QString error;
};

struct IndexingRequestResult {
    qint64 frameId = 0;
    QString directory;
    bool catchUp = false;
    int changed = 0;
    QString error;
};

struct ServiceControlResult { QString action, error; QJsonObject status; };

void removeButtonIcons(QWidget* widget) {
    for (auto* button : widget->findChildren<QAbstractButton*>()) button->setIcon({});
}

QJsonArray connectedDisplays() {
    QProcess process;
    process.start("hyprctl", {"-j", "monitors"});
    if (!process.waitForFinished(1000)) { process.kill(); process.waitForFinished(250); return {}; }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || process.bytesAvailable() > 1024 * 1024) return {};
    return QJsonDocument::fromJson(process.readAllStandardOutput()).array();
}

QString chooseItem(QWidget* parent, const QString& title, const QString& prompt, const QStringList& items, bool& accepted) {
    QInputDialog dialog(parent);
    theme(&dialog);
    dialog.setWindowTitle(title); dialog.setLabelText(prompt);
    dialog.setComboBoxItems(items); dialog.setComboBoxEditable(false);
    removeButtonIcons(&dialog);
    accepted = dialog.exec() == QDialog::Accepted;
    return accepted ? dialog.textValue() : QString();
}

bool currentStorageStatus(const QJsonObject& status, const QString& directory) {
    const QString reportedDirectory = status.value("history_directory").toString();
    return status.value("running").toBool() && !reportedDirectory.isEmpty() &&
        QDir(reportedDirectory).absolutePath() == QDir(directory).absolutePath() && status.value("storage_available").toBool(true);
}

QString storageCapacityText(const QJsonObject& status, const QString& directory) {
    if (!status.value("running").toBool()) return "Storage estimate unavailable while the background service is stopped.";
    if (!currentStorageStatus(status, directory))
        return "Storage estimate unavailable for this history folder.";
    const auto forecast = status.value("storage_forecast").toObject();
    const auto usage = status.value("usage").toObject();
    QStringList parts;
    if (usage.contains("disk_bytes") && status.value("max_disk_mib").toDouble() > 0) {
        parts << QString("Storage: %1 / %2 GiB used.")
            .arg(usage.value("disk_bytes").toDouble() / (1024 * 1024 * 1024), 0, 'f', 1)
            .arg(status.value("max_disk_mib").toDouble() / 1024, 0, 'f', 1);
    }
    const QString state = forecast.value("state").toString();
    const bool freeSpace = forecast.value("limiting_factor").toString() == "free-space";
    const double activeHours = forecast.value("capacity_active_hours").toDouble(-1);
    if (activeHours >= 0 && std::isfinite(activeHours)) {
        parts << QString("This allowance holds about %1 active recording hours at your recent rate.").arg(activeHours, 0, 'f', 1);
    } else if (state == "no-growth") {
        parts << "No recent storage growth; history capacity cannot be estimated yet.";
    } else if (state == "insufficient-data") {
        parts << "More recorded history is needed to estimate capacity.";
    } else {
        parts << "History capacity estimate is unavailable.";
    }
    const double calendarDays = forecast.value("capacity_calendar_days").toDouble(-1);
    const double retentionBytes = forecast.value("estimated_retention_bytes").toDouble(-1);
    if (calendarDays >= 0 && retentionBytes >= 0 && std::isfinite(calendarDays) && std::isfinite(retentionBytes)) {
        parts << QString("About %1 days at your observed usage; %2 days would need roughly %3 GiB.")
            .arg(calendarDays, 0, 'f', 1).arg(forecast.value("retention_days").toInt())
            .arg(retentionBytes / (1024 * 1024 * 1024), 0, 'f', 1);
    } else if (activeHours >= 0) {
        parts << "Calendar estimates need at least seven retained days, all under the current capture settings.";
    }
    if (freeSpace) parts << "Available disk space reduces this capacity.";
    parts << "Oldest history rolls off at the size or age limit; recording continues.";
    return parts.join(' ');
}

QString storageCapacitySummary(const QJsonObject& status, const QString& directory) {
    if (!status.value("running").toBool()) return "Storage estimate unavailable while the service is stopped.";
    if (!currentStorageStatus(status, directory)) return "Storage estimate unavailable for this folder.";
    const auto forecast = status.value("storage_forecast").toObject();
    const auto usage = status.value("usage").toObject();
    QStringList lines;
    if (usage.contains("disk_bytes") && status.value("max_disk_mib").toDouble() > 0)
        lines << QString("%1 / %2 GiB used")
            .arg(usage.value("disk_bytes").toDouble() / (1024 * 1024 * 1024), 0, 'f', 1)
            .arg(status.value("max_disk_mib").toDouble() / 1024, 0, 'f', 1);
    const double hours = forecast.value("capacity_active_hours").toDouble(-1);
    const double days = forecast.value("capacity_calendar_days").toDouble(-1);
    if (days >= 0 && std::isfinite(days)) lines << QString("About %1 days of history at recent usage").arg(days, 0, 'f', 1);
    else if (hours >= 0 && std::isfinite(hours)) lines << QString("About %1 active recording hours at recent usage").arg(hours, 0, 'f', 1);
    else if (forecast.value("state").toString() == "no-growth") lines << "No recent storage growth to estimate capacity.";
    else if (forecast.value("state").toString() == "insufficient-data") lines << "More recorded history needed to estimate capacity.";
    else lines << "Capacity estimate unavailable.";
    if (forecast.value("limiting_factor").toString() == "free-space") lines << "Limited by available disk space.";
    return lines.join('\n');
}

// Combobox sentinel for the "Focused display" choice. It is a mode, never a
// connector name, so it is never written to `output`.
const QString kFocusedDisplayChoice = QStringLiteral("@focused");

class SettingsDialog final : public QDialog {
public:
    explicit SettingsDialog(QWidget* parent, const QJsonObject& recordingStatus, std::function<QJsonArray()> displays) : QDialog(parent),
        recordingStatus_(recordingStatus),
        visibleWindows_(recordingStatus.value("visible_windows").toArray()),
        compositorInstance_(recordingStatus.value("compositor_instance").toString(qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE"))) {
        const auto colors = theme(this);
        setObjectName("replaySettings");
        setWindowTitle("Omarchy Replay settings");
        resize(740, 740);
        setMinimumSize(560, 420);
        auto* layout = new QVBoxLayout(this);
        layout->setSpacing(12);
        auto* tabs = new QTabWidget;
        tabs->setObjectName("settingsTabs");
        layout->addWidget(tabs, 1);
        const auto makePage = [this, tabs](const QString& title, const QString& name) {
            auto* scroll = new QScrollArea;
            scroll->setObjectName(name);
            scroll->setWidgetResizable(true);
            scroll->setFrameShape(QFrame::NoFrame);
            scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            scroll->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
            auto* page = new QWidget;
            page->setFont(font());
            page->setMaximumWidth(660);
            auto* contents = new QVBoxLayout(page);
            contents->setContentsMargins(18, 18, 18, 18);
            contents->setSpacing(12);
            contents->setAlignment(Qt::AlignTop);
            scroll->setWidget(page);
            page->setAutoFillBackground(false);
            tabs->addTab(scroll, title);
            return contents;
        };
        const auto section = [this](QVBoxLayout* page, const QString& title) {
            if (page->count()) page->addSpacing(10);
            auto* label = new QLabel(title);
            auto font = this->font(); font.setBold(true); label->setFont(font);
            page->addWidget(label);
        };
        const auto makeForm = [](QVBoxLayout* page) {
            auto* form = new QFormLayout;
            form->setHorizontalSpacing(20);
            form->setVerticalSpacing(10);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            form->setRowWrapPolicy(QFormLayout::WrapLongRows);
            page->addLayout(form);
            return form;
        };
        const auto note = [colors](const QString& text) {
            auto* label = new QLabel(text);
            label->setWordWrap(true); label->setTextFormat(Qt::PlainText);
            label->setStyleSheet(QString("color: %1;").arg(colors.muted.name()));
            return label;
        };
        const auto details = [this, &note](QVBoxLayout* page, const QString& name, const QString& text) {
            auto* toggle = new QPushButton("Details");
            toggle->setObjectName(name); toggle->setCheckable(true); toggle->setAutoDefault(false);
            auto* content = note(text); content->setObjectName(name + "Text"); content->hide();
            page->addWidget(toggle, 0, Qt::AlignLeft); page->addWidget(content);
            connect(toggle, &QPushButton::toggled, this, [toggle, content](bool open) {
                content->setVisible(open); toggle->setText(open ? "Hide details" : "Details");
            });
            return content;
        };
        auto* recording = makePage("&Recording", "recordingSettingsScroll");
        section(recording, "Capture");
        auto* form = makeForm(recording);
        output_ = new QComboBox;
        output_->setObjectName("settingOutput");
        output_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        output_->setMinimumContentsLength(24);
        output_->addItem("Choose a display", QString());
        output_->addItem("Focused display · follows your focus", kFocusedDisplayChoice);
        connect(output_, &QComboBox::activated, this, [this] { displayChosen_ = true; });
        form->addRow("&Display", output_);
        displayNote_ = note("Looking for connected displays…");
        form->addRow(QString(), displayNote_);
        interval_ = new QDoubleSpinBox;
        interval_->setObjectName("settingInterval");
        interval_->setRange(.25, 60); interval_->setSingleStep(.25); interval_->setSuffix(" seconds");
        form->addRow("Capture &interval", interval_);
        login_ = new QCheckBox("Start Replay at login");
        login_->setObjectName("settingLoginStartup");
        form->addRow(QString(), login_);
        details(recording, "captureSettingsDetails",
            "Opening Replay does not start recording. Use Controls to start it. Your pause and stop choices survive restarts.");
        section(recording, "History limits");
        auto* limits = makeForm(recording);
        days_ = new QSpinBox;
        days_->setObjectName("settingRetentionDays");
        days_->setRange(1, 3650); days_->setSuffix(" days");
        limits->addRow("&Keep up to", days_);
        disk_ = new QSpinBox; disk_->setObjectName("settingMaxDiskMiB");
        disk_->setRange(64, 1048576); disk_->setSuffix(" MiB");
        limits->addRow("Disk &allowance", disk_);
        free_ = new QSpinBox; free_->setObjectName("settingMinFreeMiB");
        free_->setRange(0, 1048576); free_->setSuffix(" MiB");
        limits->addRow("Leave &free", free_);
        recording->addWidget(note("Oldest history is deleted at the age or space limit, including its text and pending work. Recording continues."));
        capacity_ = note({});
        capacity_->setObjectName("settingsStorageCapacity");
        recording->addWidget(capacity_);
        capacityDetails_ = details(recording, "storageCapacityDetails", {});
        capacityDetails_->setObjectName("settingsStorageDetails");
        section(recording, "Storage location");
        auto* storageForm = makeForm(recording);
        storage_ = new QLineEdit; storage_->setObjectName("settingStorageDirectory");
        storage_->setReadOnly(true); storage_->setAccessibleName("History storage folder");
        const QString viewedHistory = parent->property("historyDirectory").toString();
        storage_->setText(recordingStatus.value("history_directory").toString(viewedHistory.isEmpty() ? replayPaths().historyDirectory : viewedHistory));
        storageForm->addRow("History &folder", storage_);
        auto* folders = new QHBoxLayout;
        auto* chooseFolder = new QPushButton("Choose folder…"); chooseFolder->setObjectName("chooseStorageDirectory");
        auto* openFolder = new QPushButton("Open folder"); openFolder->setObjectName("openStorageDirectory");
        auto* defaultFolder = new QPushButton("Use default"); defaultFolder->setObjectName("resetStorageDirectory");
        folders->addWidget(chooseFolder); folders->addWidget(openFolder); folders->addWidget(defaultFolder); folders->addStretch();
        recording->addLayout(folders);
        recording->addWidget(note("Changing folders leaves existing history where it is."));
        details(recording, "storageSettingsDetails",
            "Use an empty folder on this computer or a mounted disk, or choose an existing Replay archive to reopen it.");
        connect(chooseFolder, &QPushButton::clicked, this, [this] {
            const auto folder = QFileDialog::getExistingDirectory(this, "Choose history folder", storage_->text(),
                QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
            if (!folder.isEmpty()) { storageDirectory_ = QDir(folder).absolutePath(); storage_->setText(storageDirectory_); }
        });
        connect(openFolder, &QPushButton::clicked, this, [this] {
            if (!QFileInfo(storage_->text()).isDir()) { showError("This folder is unavailable. Check the selected location or reconnect its disk."); return; }
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(storage_->text()))) showError("Could not open the history folder in your file manager.");
        });
        connect(defaultFolder, &QPushButton::clicked, this, [this] { storageDirectory_.clear(); storage_->setText(replayPaths().historyDirectory); });
        auto* resources = makePage("&Resources", "resourceSettingsScroll");
        section(resources, "Text indexing");
        resources->addWidget(note("CPU allowances are percentages of one core. Lower values leave more CPU for other apps, but text may take longer to become searchable."));
        auto* resourceForm = makeForm(resources);
        const QVector<QPair<QString, QString>> budgets{{"While you work", "settingActiveCpu"}, {"When idle", "settingIdleCpu"},
            {"Requested moments", "settingRequestCpu"}, {"Computer busy", "settingPressureCpu"}, {"Worker ceiling", "settingCpuCeiling"}};
        for (const auto& entry : budgets) {
            auto* spin = new QDoubleSpinBox;
            spin->setObjectName(entry.second); spin->setRange(entry.second == "settingCpuCeiling" ? 0 : 1, 100);
            spin->setSuffix(" %"); spin->setDecimals(1);
            if (entry.second == "settingCpuCeiling") spin->setSpecialValueText("No ceiling");
            resourceForm->addRow(entry.first, spin); budgets_.append(spin);
        }
        idle_ = new QSpinBox; idle_->setObjectName("settingIdleSeconds");
        idle_->setRange(1, 3600); idle_->setSuffix(" seconds");
        resourceForm->addRow("Idle after", idle_);
        details(resources, "resourceSettingsDetails",
            "100% means one full CPU core, regardless of how many cores your computer has. Requested moments use the requested allowance when you ask Replay to process them. The busy allowance applies during sustained CPU pressure. A worker ceiling of 0 disables the safety limit.");
        section(resources, "Help choosing limits");
        resources->addWidget(note("Your coding agent can review local timing and CPU data, then recommend settings for this computer."));
        auto* resourcePrompt = new QPushButton("Copy resources prompt"); resourcePrompt->setObjectName("copyResourcesPrompt");
        resources->addWidget(resourcePrompt, 0, Qt::AlignLeft);
        connect(resourcePrompt, &QPushButton::clicked, this, [this] { copyAgentPrompt(AgentPromptTopic::Resources); });

        auto* exclusionLayout = makePage("&Exclusions", "exclusionSettingsScroll");
        exclusionLayout->addWidget(note("Exact app IDs, one per line. Replay and the screensaver stay protected."));
        section(exclusionLayout, "Skip in Replay");
        exclusionLayout->addWidget(note("Pauses Replay only. Screenshots and sharing stay visible; not a privacy guarantee."));
        skippedApps_ = new QPlainTextEdit; skippedApps_->setObjectName("settingSkippedApps");
        skippedApps_->setAccessibleName("Apps to skip in Replay only");
        skippedApps_->setMinimumHeight(60); skippedApps_->setMaximumHeight(76); skippedApps_->setTabChangesFocus(true);
        exclusionLayout->addWidget(skippedApps_);
        auto* chooseApp = new QPushButton("Choose visible app…");
        chooseApp->setObjectName("chooseExcludedApp"); chooseApp->setEnabled(!visibleWindows_.isEmpty());
        chooseApp->setToolTip("Add an app to Skip in Replay; screenshots and sharing remain visible");
        exclusionLayout->addWidget(chooseApp, 0, Qt::AlignLeft);
        connect(chooseApp, &QPushButton::clicked, this, [this] {
            QStringList names;
            for (const auto& entry : visibleWindows_) {
                const auto window = entry.toObject();
                const QString app = window.value("app_id").toString(window.value("initial_app_id").toString());
                if (!app.isEmpty() && app != "omarchy-replay" && app != "org.omarchy.screensaver" && !names.contains(app)) names.append(app);
            }
            names.sort();
            if (names.isEmpty()) return;
            bool accepted = false;
            const QString selected = chooseItem(this, "Skip an app in Replay", "Pause Replay when this app is visible; keep screenshots and sharing unchanged", names, accepted);
            if (accepted) addAppExclusions(skippedApps_, {selected});
        });
        section(exclusionLayout, "Hide from screenshots and sharing");
        exclusionLayout->addWidget(note("Pauses Replay and masks these apps in screenshots and screen sharing."));
        apps_ = new QPlainTextEdit; apps_->setObjectName("settingExcludedApps");
        apps_->setAccessibleName("Apps hidden from screenshots and sharing");
        apps_->setMinimumHeight(76); apps_->setMaximumHeight(95); apps_->setTabChangesFocus(true);
        exclusionLayout->addWidget(apps_);
        auto* presetRow = new QHBoxLayout;
        auto* preset = new QComboBox;
        preset->setObjectName("exclusionPreset"); preset->setAccessibleName("App exclusion preset");
        preset->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        preset->setMinimumContentsLength(22);
        preset->addItem("Passwords & authentication", "privacy");
        preset->addItem("Gaming apps", "gaming");
        preset->addItem("Media players", "media");
        auto* addPreset = new QPushButton("Add preset");
        addPreset->setObjectName("addExclusionPreset"); addPreset->setAutoDefault(false);
        presetRow->addWidget(preset, 1); presetRow->addWidget(addPreset);
        exclusionLayout->addLayout(presetRow);
        exclusionLayout->addWidget(note("Passwords add screenshot protection; games and media skip Replay only. Save to apply."));
        exclusionLayout->addWidget(note("Gaming apps do not cover every game. Existing screenshot protection stays in place."));
        connect(addPreset, &QPushButton::clicked, this, [this, preset] {
            const QString id = preset->currentData().toString();
            const QStringList additions = id == "privacy" ? privacyAppExclusions()
                : id == "gaming" ? gamingAppExclusions() : mediaAppExclusions();
            addAppExclusions(id == "privacy" ? apps_ : skippedApps_, additions);
        });
        section(exclusionLayout, "Window rules");
        exclusionLayout->addWidget(note("Screenshot protection; all filled fields must match. Title patterns use regular expressions."));
        windows_ = new QTableWidget(0, 3);
        windows_->setObjectName("settingExcludedWindows");
        windows_->setHorizontalHeaderLabels({"Exact app", "Title pattern", "Window address"});
        windows_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        windows_->verticalHeader()->hide();
        windows_->setCornerButtonEnabled(false);
        windows_->setSelectionBehavior(QAbstractItemView::SelectRows);
        windows_->setAccessibleName("Window exclusion rules. Nonempty fields must all match.");
        windows_->setMinimumHeight(132); windows_->setMaximumHeight(180);
        exclusionLayout->addWidget(windows_);
        auto* ruleActions = new QHBoxLayout;
        auto* add = new QPushButton("&Add rule"); auto* remove = new QPushButton("&Remove rule");
        auto* chooseWindow = new QPushButton("Choose visible window…");
        chooseWindow->setObjectName("chooseExcludedWindow");
        chooseWindow->setEnabled(!visibleWindows_.isEmpty() && !compositorInstance_.isEmpty());
        add->setObjectName("addWindowExclusion"); remove->setObjectName("removeWindowExclusion");
        ruleActions->addWidget(add); ruleActions->addWidget(remove); ruleActions->addWidget(chooseWindow); ruleActions->addStretch();
        exclusionLayout->addLayout(ruleActions);
        connect(add, &QPushButton::clicked, this, [this] {
            const int row = windows_->rowCount(); windows_->insertRow(row);
            for (int column = 0; column < 3; ++column) windows_->setItem(row, column, new QTableWidgetItem);
            windows_->setCurrentCell(row, 0); windows_->editItem(windows_->item(row, 0));
        });
        connect(remove, &QPushButton::clicked, this, [this] { if (windows_->currentRow() >= 0) windows_->removeRow(windows_->currentRow()); });
        connect(chooseWindow, &QPushButton::clicked, this, [this] {
            QStringList labels;
            QVector<QJsonObject> windows;
            for (const auto& entry : visibleWindows_) {
                const auto window = entry.toObject();
                const QString app = window.value("app_id").toString(window.value("initial_app_id").toString());
                if (app.isEmpty() || app == "omarchy-replay" || app == "org.omarchy.screensaver" || window.value("address").toString().isEmpty()) continue;
                windows.append(window);
                labels.append(app + " · " + window.value("title").toString().left(80) + " · " + window.value("address").toString());
            }
            if (labels.isEmpty()) return;
            bool accepted = false;
            const QString selected = chooseItem(this, "Exclude a window", "Choose a window for this desktop session", labels, accepted);
            if (!accepted || labels.indexOf(selected) < 0) return;
            const auto window = windows[labels.indexOf(selected)];
            const int row = windows_->rowCount(); windows_->insertRow(row);
            windows_->setItem(row, 0, new QTableWidgetItem(window.value("app_id").toString(window.value("initial_app_id").toString())));
            windows_->setItem(row, 1, new QTableWidgetItem);
            const QString addressText = window.value("address").toString().toLower();
            auto* address = new QTableWidgetItem(addressText);
            address->setData(Qt::UserRole, compositorInstance_); address->setData(Qt::UserRole + 1, addressText);
            windows_->setItem(row, 2, address); windows_->setCurrentCell(row, 0);
        });
        exclusionLayout->addWidget(note("Address rules last for this desktop session. The capture mask also covers other windows matching the rule's app and title fields."));
        details(exclusionLayout, "exclusionSettingsDetails",
            "Address rules also need an app or title. Choose visible window fills the app and address for this session; choose visible app excludes every window from that app.");
        section(exclusionLayout, "Help choosing exclusions");
        exclusionLayout->addWidget(note("Tell your coding agent what to exclude, using this prompt."));
        auto* exclusionPrompt = new QPushButton("Copy exclusions prompt"); exclusionPrompt->setObjectName("copyExclusionsPrompt");
        exclusionLayout->addWidget(exclusionPrompt, 0, Qt::AlignLeft);
        connect(exclusionPrompt, &QPushButton::clicked, this, [this] { copyAgentPrompt(AgentPromptTopic::Exclusions); });

        const bool recorderAvailable = meetingRecorderAvailable();
        auto* meetings = makePage("&Meetings", "meetingSettingsScroll");
        const int meetingTab = tabs->count() - 1;
        section(meetings, "Omarchy Meeting Recorder");
        meetings->addWidget(note("A separate Omarchy plugin records and transcribes meetings. Replay makes its completed transcripts searchable alongside your screen history."));
        auto* meetingRepository = new QPushButton("Installation and documentation");
        meetingRepository->setObjectName("openMeetingRecorderRepository");
        meetingRepository->setToolTip("github.com/jankeesvw/omarchy-meeting-recorder");
        meetings->addWidget(meetingRepository, 0, Qt::AlignLeft);
        connect(meetingRepository, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(QUrl("https://github.com/jankeesvw/omarchy-meeting-recorder"));
        });
        meetingsEnabled_ = new QCheckBox("Include meeting transcripts");
        meetingsEnabled_->setObjectName("settingMeetingsEnabled");
        meetings->addWidget(meetingsEnabled_);
        auto* meetingForm = makeForm(meetings);
        meetingsDirectory_ = new QLineEdit;
        meetingsDirectory_->setObjectName("settingMeetingsDirectory");
        meetingForm->addRow("Meetings &folder", meetingsDirectory_);
        auto* chooseMeetings = new QPushButton("Choose folder");
        chooseMeetings->setObjectName("chooseMeetingsDirectory");
        meetings->addWidget(chooseMeetings, 0, Qt::AlignLeft);
        connect(chooseMeetings, &QPushButton::clicked, this, [this] {
            const QString directory = QFileDialog::getExistingDirectory(this, "Meeting Recorder folder", meetingsDirectory_->text());
            if (!directory.isEmpty()) meetingsDirectory_->setText(directory);
        });
        meetings->addWidget(note("Audio stays with Meeting Recorder. Replay keeps a searchable transcript and a marker at the meeting's known start."));
        details(meetings, "meetingSettingsDetails",
            "Existing completed meetings inside Replay's retention window are included. Transcript edits and folder renames are checked automatically. Turning this off stops new imports; saved transcripts remain until deleted or expired. Replay never deletes the recorder's originals.");
        auto* meetingStatus = note(recorderAvailable ? "Meeting Recorder detected." : "Meeting Recorder is unavailable. New imports are paused.");
        meetingStatus->setObjectName("meetingIntegrationStatus");
        meetings->addWidget(meetingStatus);
        const auto meetingState = recordingStatus.value("meetings").toObject();
        if (!meetingState.value("error").toString().isEmpty()) meetings->addWidget(note(meetingState.value("error").toString()));

        error_ = new QLabel;
        error_->setObjectName("settingsError"); error_->setWordWrap(true); error_->setTextFormat(Qt::PlainText);
        error_->hide();
        layout->addWidget(error_);
        promptNotice_ = new QLabel; promptNotice_->setObjectName("agentPromptNotice"); promptNotice_->setWordWrap(true);
        promptNotice_->hide();
        layout->addWidget(promptNotice_);
        auto* footer = new QHBoxLayout;
        auto* setupPrompt = new QPushButton("Copy setup prompt"); setupPrompt->setObjectName("copySetupPrompt");
        footer->addWidget(setupPrompt); footer->addStretch();
        connect(setupPrompt, &QPushButton::clicked, this, [this] { copyAgentPrompt(AgentPromptTopic::Setup); });
        buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        buttons_->button(QDialogButtonBox::Save)->setProperty("primary", true);
        buttons_->setObjectName("settingsButtons"); footer->addWidget(buttons_); layout->addLayout(footer);
        removeButtonIcons(this);
        connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons_, &QDialogButtonBox::accepted, this, [this] { save(); });
        try {
            const auto resolved = resolveReplayConfig();
            document_ = resolved.document;
            configEditable_ = resolved.configError.isEmpty();
            const auto& config = document_.config;
            meetingsEnabled_->setChecked(config.meetingsEnabled);
            meetingsEnabled_->setEnabled(recorderAvailable || config.meetingsEnabled);
            meetingsDirectory_->setText(replayMeetingsDirectory(config));
            tabs->setTabVisible(meetingTab, recorderAvailable || config.meetingsEnabled);
            if (config.displayMode == "focused") output_->setCurrentIndex(output_->findData(kFocusedDisplayChoice));
            else if (!config.output.isEmpty()) { output_->addItem(config.output + " · checking availability", config.output); output_->setCurrentIndex(output_->findData(config.output)); }
            interval_->setValue(config.intervalSeconds);
            storageDirectory_ = config.storageDirectory; storage_->setText(replayHistoryDirectory(config));
            days_->setValue(config.retentionDays); disk_->setValue(config.maxDiskMiB); free_->setValue(config.minFreeMiB);
            login_->setChecked(config.loginStartup); idle_->setValue(config.idleSeconds);
            const QVector<double> values{config.activeCpuPercent, config.idleCpuPercent, config.requestCpuPercent,
                                         config.pressureCpuPercent, config.cpuCeilingPercent};
            for (int i = 0; i < values.size(); ++i) budgets_[i]->setValue(values[i]);
            auto editableApps = config.excludedApps;
            editableApps.removeAll("omarchy-replay");
            editableApps.removeAll("org.omarchy.screensaver");
            apps_->setPlainText(editableApps.join('\n'));
            skippedApps_->setPlainText(config.skippedApps.join('\n'));
            for (const auto& rule : config.excludedWindows) {
                const int row = windows_->rowCount(); windows_->insertRow(row);
                windows_->setItem(row, 0, new QTableWidgetItem(rule.appId));
                windows_->setItem(row, 1, new QTableWidgetItem(rule.titleRegex));
                auto* address = new QTableWidgetItem(rule.address);
                address->setData(Qt::UserRole, rule.compositorInstance);
                address->setData(Qt::UserRole + 1, rule.address);
                windows_->setItem(row, 2, address);
            }
            if (!configEditable_) {
                showError(resolved.configError + "\nShowing the last accepted settings. Copy the setup prompt to repair config.toml, then reopen Settings. Nothing has been changed.");
                buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
            }
        } catch (const std::exception& error) {
            configEditable_ = false;
            showError(QString::fromUtf8(error.what()) + "\nCorrect config.toml and reopen Settings. Existing settings have not been changed.");
            buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
        }
        const auto updateCapacity = [this] { updateStorageCapacity(); };
        connect(storage_, &QLineEdit::textChanged, this, updateCapacity);
        connect(disk_, &QSpinBox::valueChanged, this, updateCapacity);
        connect(free_, &QSpinBox::valueChanged, this, updateCapacity);
        connect(days_, &QSpinBox::valueChanged, this, updateCapacity);
        connect(interval_, &QDoubleSpinBox::valueChanged, this, updateCapacity);
        connect(output_, &QComboBox::currentIndexChanged, this, [this] { updateDisplayNote(); });
        connect(output_, &QComboBox::currentIndexChanged, this, updateCapacity);
        updateStorageCapacity();
        connect(&writer_, &QFutureWatcher<QString>::finished, this, [this] {
            const QString error = writer_.result();
            if (error.isEmpty()) accept();
            else { showError(error); buttons_->setEnabled(true); }
        });
        connect(&displays_, &QFutureWatcher<QJsonArray>::finished, this, [this] {
            const QString selected = output_->currentData().toString();
            const QSignalBlocker blocker(output_); output_->clear(); output_->addItem("Choose a display", QString());
            output_->addItem("Focused display · follows your focus", kFocusedDisplayChoice);
            for (const auto& value : displays_.result()) {
                const auto monitor = value.toObject(); const auto name = monitor.value("name").toString();
                if (name.isEmpty() || monitor.value("disabled").toBool()) continue;
                QString label = name;
                const QString model = monitor.value("model").toString();
                if (!model.isEmpty()) label += " · " + model;
                const int width = monitor.value("width").toInt(), height = monitor.value("height").toInt();
                if (width > 0 && height > 0) label += QString(" · %1 × %2").arg(width).arg(height);
                output_->addItem(label, name);
            }
            if (!selected.isEmpty() && output_->findData(selected) < 0) output_->addItem(selected + " · disconnected", selected);
            output_->setCurrentIndex(std::max(0, output_->findData(selected)));
            updateDisplayNote();
        });
        displays_.setFuture(QtConcurrent::run([displays = std::move(displays)] { try { return displays(); } catch (...) { return QJsonArray(); } }));
        output_->setFocus();
    }
    ~SettingsDialog() override { writer_.waitForFinished(); displays_.waitForFinished(); }
protected:
    void reject() override { if (!writer_.isRunning()) QDialog::reject(); }
private:
    void showError(const QString& message) {
        error_->setText(message);
        error_->setVisible(!message.isEmpty());
    }

    void updateDisplayNote() {
        if (output_->currentData().toString() == kFocusedDisplayChoice)
            displayNote_->setText("Replay records whichever display has focus at each capture.");
        else if (output_->count() > 2) displayNote_->setText("Replay follows this physical display.");
        else displayNote_->setText("No displays found. Reconnect your saved display and reopen Settings.");
    }

    void updateStorageCapacity() {
        const auto& saved = document_.config;
        const QString selected = output_->currentData().toString();
        const QString selectedMode = selected == kFocusedDisplayChoice ? QStringLiteral("focused") : QStringLiteral("fixed");
        const bool differentCapture = QDir(storage_->text()).absolutePath() != QDir(replayHistoryDirectory(saved)).absolutePath() ||
            interval_->value() != saved.intervalSeconds || selectedMode != saved.displayMode ||
            (selectedMode == "fixed" && selected != saved.output);
        const bool unappliedCapture = recordingStatus_.value("running").toBool() &&
            ((recordingStatus_.contains("interval_seconds") && recordingStatus_.value("interval_seconds").toDouble() != saved.intervalSeconds) ||
             (recordingStatus_.contains("output") && recordingStatus_.value("output").toString() != saved.output));
        if (differentCapture || unappliedCapture) {
            capacity_->setText("A new folder, display or capture interval needs its own usage estimate.");
            capacityDetails_->setText("Estimates reflect your saved capture settings. Record with the new settings to build a new estimate. Oldest history rolls off at the size or age limit.");
            capacity_->setToolTip({});
            return;
        }
        auto shown = recordingStatus_;
        const auto measured = shown.value("storage_forecast").toObject();
        StorageForecastOptions options;
        options.diskBytes = measured.value("disk_bytes").toInteger(shown.value("usage").toObject().value("disk_bytes").toInteger());
        options.maxDiskBytes = qint64(disk_->value()) * 1024 * 1024;
        options.freeBytes = measured.value("filesystem_free_bytes").toInteger(-1);
        options.minFreeBytes = qint64(free_->value()) * 1024 * 1024;
        options.retentionDays = days_->value();
        shown["storage_forecast"] = storageCapacityProjection(measured, options);
        shown["max_disk_mib"] = disk_->value();
        capacity_->setText(storageCapacitySummary(shown, replayHistoryDirectory(saved)));
        capacityDetails_->setText(storageCapacityText(shown, replayHistoryDirectory(saved)));
        capacity_->setToolTip(!currentStorageStatus(shown, replayHistoryDirectory(saved)) ? QString()
            : measured.value("estimate_note").toString());
    }
    static QStringList appEntries(QPlainTextEdit* editor) {
        QStringList entries;
        for (const auto& line : editor->toPlainText().split('\n')) {
            const QString app = line.trimmed();
            if (!app.isEmpty() && !entries.contains(app)) entries.append(app);
        }
        return entries;
    }
    void addAppExclusions(QPlainTextEdit* target, const QStringList& additions) {
        auto merged = appEntries(target);
        for (const auto& app : additions) if (!merged.contains(app)) merged.append(app);
        auto strict = target == apps_ ? merged : appEntries(apps_);
        const auto skipped = target == skippedApps_ ? merged : appEntries(skippedApps_);
        for (const QString& required : {"omarchy-replay", "org.omarchy.screensaver"})
            if (!strict.contains(required)) strict.append(required);
        const QString limitError = "These entries would exceed the combined limit of 64 app exclusions, including Replay and the screensaver. Remove some entries first.";
        if (strict.size() + skipped.size() > 64) { showError(limitError); return; }
        target->setPlainText(merged.join('\n'));
        if (error_->text() == limitError) showError({});
    }
    void copyAgentPrompt(AgentPromptTopic topic) {
        auto shown = document_.config;
        shown.intervalSeconds = interval_->value();
        shown.retentionDays = days_->value();
        shown.maxDiskMiB = disk_->value(); shown.minFreeMiB = free_->value();
        shown.activeCpuPercent = budgets_[0]->value(); shown.idleCpuPercent = budgets_[1]->value();
        shown.requestCpuPercent = budgets_[2]->value(); shown.pressureCpuPercent = budgets_[3]->value();
        shown.cpuCeilingPercent = budgets_[4]->value(); shown.idleSeconds = idle_->value();
        shown.meetingsEnabled = meetingsEnabled_->isChecked();
        shown.meetingsDirectory = meetingsDirectory_->text().trimmed();
        shown.skippedApps = appEntries(skippedApps_);
        shown.excludedApps = appEntries(apps_);
        for (const QString& required : {"omarchy-replay", "org.omarchy.screensaver"})
            if (!shown.excludedApps.contains(required)) shown.excludedApps.prepend(required);
        const AgentPromptContext context{QCoreApplication::applicationFilePath(), replayPaths(),
            replayHistoryDirectory(document_.config), shown, configEditable_};
        QApplication::clipboard()->setText(configurationAgentPrompt(topic, context));
        promptNotice_->setText("Prompt copied. Paste it into your coding agent and add your request.");
        promptNotice_->show();
        QTimer::singleShot(4000, this, [this] { promptNotice_->clear(); promptNotice_->hide(); });
    }
    void save() {
        if (!configEditable_) return;
        auto config = document_.config;
        const QString chosen = output_->currentData().toString();
        const bool focused = chosen == kFocusedDisplayChoice;
        config.displayMode = focused ? "focused" : "fixed";
        // The sentinel is a mode, not a display: the saved output and its pinned
        // identity stay untouched for when the user returns to a fixed display.
        if (!focused) {
            config.output = chosen;
            if (config.output != document_.config.output || displayChosen_) config.outputIdentity.clear();
        }
        config.storageDirectory = storageDirectory_;
        config.meetingsEnabled = meetingsEnabled_->isChecked();
        config.meetingsDirectory = meetingsDirectory_->text().trimmed();
        if (config.meetingsDirectory == replayMeetingsDirectory(ReplayConfig{})) config.meetingsDirectory.clear();
        config.intervalSeconds = interval_->value(); config.retentionDays = days_->value();
        config.maxDiskMiB = disk_->value(); config.minFreeMiB = free_->value(); config.loginStartup = login_->isChecked();
        config.activeCpuPercent = budgets_[0]->value(); config.idleCpuPercent = budgets_[1]->value();
        config.requestCpuPercent = budgets_[2]->value(); config.pressureCpuPercent = budgets_[3]->value();
        config.cpuCeilingPercent = budgets_[4]->value(); config.idleSeconds = idle_->value();
        config.skippedApps = appEntries(skippedApps_);
        config.excludedApps = {"omarchy-replay", "org.omarchy.screensaver"};
        for (const auto& line : apps_->toPlainText().split('\n'))
            if (!line.trimmed().isEmpty() && !config.excludedApps.contains(line.trimmed())) config.excludedApps.append(line.trimmed());
        config.excludedWindows.clear();
        for (int row = 0; row < windows_->rowCount(); ++row) {
            WindowExclusion rule;
            const auto cell = [this, row](int column) { const auto* item = windows_->item(row, column); return item ? item->text().trimmed() : QString(); };
            rule.appId = cell(0); rule.titleRegex = cell(1); rule.address = cell(2).toLower();
            if (!rule.address.isEmpty()) {
                const auto* address = windows_->item(row, 2);
                rule.compositorInstance = address && address->data(Qt::UserRole + 1).toString() == rule.address
                    ? address->data(Qt::UserRole).toString() : compositorInstance_;
            }
            config.excludedWindows.append(rule);
        }
        try { validateReplayConfig(config); }
        catch (const std::exception& error) { showError(QString::fromUtf8(error.what())); return; }
        QStringList deletionChanges;
        if (config.retentionDays < document_.config.retentionDays)
            deletionChanges << QString("history older than %1 days").arg(config.retentionDays);
        if (config.maxDiskMiB < document_.config.maxDiskMiB)
            deletionChanges << QString("oldest history to fit the %1 MiB allowance").arg(config.maxDiskMiB);
        if (config.minFreeMiB > document_.config.minFreeMiB)
            deletionChanges << QString("oldest history if needed to leave %1 MiB free").arg(config.minFreeMiB);
        if (!deletionChanges.isEmpty()) {
            QMessageBox confirmation(QMessageBox::NoIcon, "Keep less history?",
                QString("Saving can permanently delete %1, including images, recognized text, imported transcripts and queued work. This cannot be undone.")
                    .arg(deletionChanges.join("; ")),
                QMessageBox::Save | QMessageBox::Cancel, this);
            confirmation.setObjectName("confirmShorterRetention");
            theme(&confirmation);
            removeButtonIcons(&confirmation);
            confirmation.setDefaultButton(QMessageBox::Cancel);
            if (confirmation.exec() != QMessageBox::Save) return;
        }
        if (replayHistoryDirectory(config) != replayHistoryDirectory(document_.config)) {
            QMessageBox confirmation(QMessageBox::NoIcon, "Change history folder?",
                QString("Replay will use %1. Your existing history stays in its original folder. No images or text will be moved or deleted.")
                    .arg(replayHistoryDirectory(config)), QMessageBox::Save | QMessageBox::Cancel, this);
            confirmation.setObjectName("confirmStorageDirectory"); theme(&confirmation); removeButtonIcons(&confirmation);
            confirmation.setDefaultButton(QMessageBox::Cancel);
            if (confirmation.exec() != QMessageBox::Save) return;
        }
        buttons_->setEnabled(false); showError("Saving settings…");
        const QByteArray original = document_.original;
        writer_.setFuture(QtConcurrent::run([config, original] {
            try { saveReplayConfig(config, original); return QString(); }
            catch (const std::exception& error) { return QString::fromUtf8(error.what()); }
        }));
    }
    ReplayConfigDocument document_;
    QJsonObject recordingStatus_;
    QComboBox* output_ = nullptr;
    QLineEdit* storage_ = nullptr;
    QString storageDirectory_;
    bool displayChosen_ = false;
    bool configEditable_ = false;
    QLabel *displayNote_ = nullptr, *promptNotice_ = nullptr, *capacity_ = nullptr, *capacityDetails_ = nullptr;
    QDoubleSpinBox* interval_ = nullptr;
    QSpinBox *days_ = nullptr, *disk_ = nullptr, *free_ = nullptr, *idle_ = nullptr;
    QCheckBox* login_ = nullptr;
    QCheckBox* meetingsEnabled_ = nullptr;
    QLineEdit* meetingsDirectory_ = nullptr;
    QVector<QDoubleSpinBox*> budgets_;
    QPlainTextEdit *apps_ = nullptr, *skippedApps_ = nullptr;
    QTableWidget* windows_ = nullptr;
    QLabel* error_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QFutureWatcher<QString> writer_;
    QFutureWatcher<QJsonArray> displays_;
    QJsonArray visibleWindows_;
    QString compositorInstance_;
};

QString elapsedDescription(qint64 milliseconds) {
    if (milliseconds < 60000) return "less than a minute";
    const qint64 minutes = milliseconds / 60000;
    if (minutes < 60) return QString("%1 min").arg(minutes);
    if (minutes < 1440) return QString("%1 hr %2 min").arg(minutes / 60).arg(minutes % 60);
    return QString("%1 days %2 hr").arg(minutes / 1440).arg(minutes / 60 % 24);
}

struct SeekResult { quint64 revision = 0; std::optional<FrameRecord> frame; QString error; };
struct MeetingReadResult { quint64 revision = 0; qint64 id = 0; std::optional<MeetingRecord> meeting; QString error; };
struct HighlightResult { qint64 id = 0; QString directory, query; TextMatches matches; QString error; };

struct SelectionRequest {
    QImage image;
    QRect crop;
    qint64 frameId = 0;
    quint64 revision = 0, clipboardRevision = 0;
};

struct SelectionResult {
    SelectionOcrResult ocr;
    qint64 frameId = 0;
    quint64 revision = 0, clipboardRevision = 0;
};

class Viewer final : public QWidget {
public:
    explicit Viewer(QString directory, ViewerServiceHooks services) : directory_(QDir(directory).absolutePath()), services_(std::move(services)) {
        try {
            const auto resolved = resolveReplayConfig();
            sharedHistory_ = directory_ == QDir(replayHistoryDirectory(resolved.document.config)).absolutePath();
            if (sharedHistory_) recording_["config_error"] = resolved.configError;
        } catch (...) { sharedHistory_ = false; }
        if (!services_.recordingStatus) services_.recordingStatus = recordingServiceStatus;
        if (!services_.recordingControl) services_.recordingControl = controlRecordingService;
        if (!services_.displays) services_.displays = connectedDisplays;
        if (!services_.selectionOcr) services_.selectionOcr = recognizeSelection;
        setWindowTitle("Omarchy Replay");
        setProperty("historyDirectory", directory_);
        setObjectName("replayViewer");
        setFocusPolicy(Qt::StrongFocus);
        setMinimumSize(720, 480);
        resize(1440, 920);
        const auto colors = theme(this);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 12, 16, 10);
        layout->setSpacing(8);
        auto* header = new QHBoxLayout;
        query_ = new QLineEdit;
        query_->setObjectName("recallSearch");
        query_->setAccessibleName("Search recorded text");
        query_->setPlaceholderText("Search what you saw");
        sourceFilter_ = new QComboBox;
        sourceFilter_->setObjectName("recallSourceFilter");
        sourceFilter_->setAccessibleName("Search source");
        sourceFilter_->addItem("All", "all");
        sourceFilter_->addItem("Screen text", "screens");
        sourceFilter_->addItem("Meetings", "meetings");
        sourceFilter_->setToolTip("Filter search · Alt+S");
        sourceFilter_->hide();
        header->addWidget(sourceFilter_);
        auto* clearSearch = new QPushButton("Clear");
        clearSearch->setObjectName("clearSearch");
        clearSearch->setAccessibleName("Clear search");
        clearSearch->setToolTip("Clear search");
        clearSearch->setVisible(false);
        connect(clearSearch, &QPushButton::clicked, this, [this] { query_->clear(); query_->setFocus(); });
        connect(query_, &QLineEdit::textChanged, clearSearch, [clearSearch](const QString& text) { clearSearch->setVisible(!text.isEmpty()); });
        header->addWidget(query_, 1);
        header->addWidget(clearSearch);
        detailsToggle_ = new QPushButton("Indexing");
        detailsToggle_->setObjectName("toggleDetails");
        detailsToggle_->setCheckable(true);
        detailsToggle_->setToolTip("Search indexing status (I)");
        header->addWidget(detailsToggle_);
        helpToggle_ = new QPushButton("Help");
        helpToggle_->setObjectName("toggleHelp");
        helpToggle_->setAccessibleName("Keyboard shortcuts");
        helpToggle_->setToolTip("Keyboard shortcuts (?)");
        helpToggle_->setCheckable(true);
        header->addWidget(helpToggle_);
        layout->addLayout(header);

        detailsScroll_ = new QScrollArea;
        details_ = detailsScroll_;
        details_->setObjectName("detailsPanel");
        detailsScroll_->setWidgetResizable(true);
        detailsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        detailsScroll_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        detailsScroll_->setFocusPolicy(Qt::NoFocus);
        detailsContents_ = new QWidget;
        detailsContents_->setMaximumWidth(1160);
        detailsContents_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        panelColumns_ = new QBoxLayout(QBoxLayout::LeftToRight, detailsContents_);
        panelColumns_->setContentsMargins(18, 14, 18, 14);
        panelColumns_->setSpacing(32);
        detailsScroll_->setWidget(detailsContents_);
        detailsContents_->setAutoFillBackground(false);
        recordingPanel_ = new QWidget;
        recordingPanel_->setObjectName("recordingPanel");
        auto* recordingLayout = new QVBoxLayout(recordingPanel_);
        recordingLayout->setContentsMargins(0, 0, 0, 0);
        recordingLayout->setSpacing(8);
        auto* recordingHeader = new QHBoxLayout;
        auto* recordingTitle = new QLabel("Recording");
        auto recordingFont = recordingTitle->font(); recordingFont.setBold(true); recordingTitle->setFont(recordingFont);
        recordingHeader->addWidget(recordingTitle); recordingHeader->addStretch();
        recordingAction_ = new QPushButton("Start recording");
        recordingAction_->setObjectName("recordingServiceAction");
        recordingStop_ = new QPushButton("Stop recording");
        recordingStop_->setObjectName("stopRecordingService");
        auto* settings = new QPushButton("Settings"); settings->setObjectName("openReplaySettings");
        deleteRecent_ = new QPushButton("Delete recent…"); deleteRecent_->setObjectName("deleteRecentHistory");
        recordingHeader->addWidget(settings);
        recordingLayout->addLayout(recordingHeader);
        recordingState_ = new QLabel;
        recordingState_->setObjectName("recordingStatus"); recordingState_->setWordWrap(true); recordingState_->setTextFormat(Qt::PlainText);
        recordingLayout->addWidget(recordingState_);
        storageCapacity_ = new QLabel;
        storageCapacity_->setObjectName("recordingStorageCapacity"); storageCapacity_->setWordWrap(true); storageCapacity_->setTextFormat(Qt::PlainText);
        recordingLayout->addWidget(storageCapacity_);
        auto* recordingActions = new QHBoxLayout;
        for (auto* button : {recordingAction_, recordingStop_, deleteRecent_}) recordingActions->addWidget(button);
        recordingActions->addStretch(); recordingLayout->addLayout(recordingActions);
        auto* storageDetailsToggle = new QPushButton("Storage details");
        storageDetailsToggle->setObjectName("toggleStorageDetails"); storageDetailsToggle->setCheckable(true);
        recordingLayout->addWidget(storageDetailsToggle, 0, Qt::AlignLeft);
        storageDetails_ = new QLabel;
        storageDetails_->setObjectName("recordingStorageDetails"); storageDetails_->setWordWrap(true); storageDetails_->setTextFormat(Qt::PlainText);
        storageDetails_->hide(); recordingLayout->addWidget(storageDetails_);
        connect(storageDetailsToggle, &QPushButton::toggled, this, [this, storageDetailsToggle](bool shown) {
            storageDetailsToggle->setText(shown ? "Hide storage details" : "Storage details");
            storageDetails_->setVisible(shown); fitDetailsPanel();
        });
        recordingLayout->addStretch();
        recordingPanel_->setVisible(sharedHistory_);
        panelColumns_->addWidget(recordingPanel_, 1);
        auto* indexPanel = new QWidget;
        indexPanel->setFont(font());
        auto* detailLayout = new QVBoxLayout(indexPanel);
        detailLayout->setContentsMargins(0, 0, 0, 0); detailLayout->setSpacing(8);
        panelColumns_->addWidget(indexPanel, 1);
        connect(recordingAction_, &QPushButton::clicked, this, [this] { requestRecording(recordingAction_->property("action").toString()); });
        connect(recordingStop_, &QPushButton::clicked, this, [this] { requestRecording("stop"); });
        connect(settings, &QPushButton::clicked, this, [this] { showSettings(); });
        connect(deleteRecent_, &QPushButton::clicked, this, [this] { deleteRecent(); });
        auto* detailHeader = new QHBoxLayout;
        auto* indexTitle = new QLabel("Search index");
        auto indexFont = indexTitle->font(); indexFont.setBold(true); indexTitle->setFont(indexFont);
        detailHeader->addWidget(indexTitle);
        detailHeader->addStretch();
        serviceAction_ = new QPushButton;
        serviceAction_->setObjectName("indexServiceAction");
        stopService_ = new QPushButton("Stop");
        stopService_->setObjectName("stopIndexService");
        stopService_->setToolTip("Stop background indexing for this history; saved moments remain available");
        detailHeader->addWidget(serviceAction_);
        detailHeader->addWidget(stopService_);
        auto* refresh = new QPushButton("Refresh");
        refresh->setObjectName("refreshHistory");
        refresh->setToolTip("Refresh indexing status (F5)");
        detailHeader->addWidget(refresh);
        detailLayout->addLayout(detailHeader);
        status_ = new QLabel;
        status_->setObjectName("recallStatus");
        status_->setWordWrap(true);
        status_->setTextFormat(Qt::PlainText);
        detailLayout->addWidget(status_);
        indexProgress_ = new QProgressBar;
        indexProgress_->setObjectName("indexProgress");
        indexProgress_->setAccessibleName("Search indexing completion");
        indexProgress_->setRange(0, 1000);
        indexProgress_->setTextVisible(false);
        indexProgress_->setFixedHeight(3);
        detailLayout->addWidget(indexProgress_);
        workerHint_ = new QLabel;
        workerHint_->setObjectName("indexWorkerHint");
        workerHint_->setTextFormat(Qt::PlainText);
        workerHint_->setWordWrap(true);
        detailLayout->addWidget(workerHint_);
        pendingAge_ = new QLabel;
        pendingAge_->setObjectName("indexPendingAge"); pendingAge_->setWordWrap(true); pendingAge_->setTextFormat(Qt::PlainText);
        detailLayout->addWidget(pendingAge_);
        priorityStatus_ = new QLabel;
        priorityStatus_->setObjectName("indexingRequestStatus");
        priorityStatus_->setTextFormat(Qt::PlainText);
        priorityStatus_->setWordWrap(true);
        detailLayout->addWidget(priorityStatus_);
        auto* actions = new QHBoxLayout;
        processMoment_ = new QPushButton("Index this moment  P");
        processMoment_->setObjectName("processMoment");
        catchUp_ = new QPushButton("Catch up  C");
        catchUp_->setObjectName("catchUpIndexing");
        catchUp_->setToolTip("Request two minutes of faster indexing when resources permit");
        copyWorkerCommand_ = new QPushButton("Copy worker command");
        copyWorkerCommand_->setObjectName("copyIndexCommand");
        for (auto* button : {processMoment_, catchUp_, copyWorkerCommand_}) actions->addWidget(button);
        actions->addStretch();
        detailLayout->addLayout(actions);
        auto* workerDetailsToggle = new QPushButton("Processing details");
        workerDetailsToggle->setObjectName("toggleIndexDetails"); workerDetailsToggle->setCheckable(true);
        detailLayout->addWidget(workerDetailsToggle, 0, Qt::AlignLeft);
        workerDetails_ = new QLabel;
        workerDetails_->setObjectName("indexWorkerDetails"); workerDetails_->setWordWrap(true); workerDetails_->setTextFormat(Qt::PlainText);
        workerDetails_->hide(); detailLayout->addWidget(workerDetails_);
        connect(workerDetailsToggle, &QPushButton::toggled, this, [this, workerDetailsToggle](bool shown) {
            workerDetailsToggle->setText(shown ? "Hide processing details" : "Processing details");
            workerDetails_->setVisible(shown); fitDetailsPanel();
        });
        detailLayout->addStretch();
        for (auto* label : {recordingState_, storageCapacity_, storageDetails_, status_, workerHint_, pendingAge_, priorityStatus_, workerDetails_}) {
            label->setMinimumWidth(0);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        }
        details_->hide();
        connect(detailsToggle_, &QPushButton::toggled, this, [this](bool shown) {
            if (shown) helpToggle_->setChecked(false);
            details_->setVisible(shown);
            if (shown) fitDetailsPanel();
        });
        layout->addWidget(details_);
        help_ = new QWidget;
        help_->setObjectName("helpPanel");
        auto* helpLayout = new QGridLayout(help_);
        helpLayout->setContentsMargins(18, 14, 18, 14);
        helpLayout->setHorizontalSpacing(18);
        helpLayout->setVerticalSpacing(8);
        const QVector<QPair<QString, QString>> shortcuts{
            {"/  Ctrl+F", "Search"}, {"↑ ↓  J K", "Browse matches"}, {"← →  H L", "Explore time"},
            {"Home  End", "First / latest moment"}, {"PgUp  PgDn", "Previous / next match page"}, {"F  /  1", "Fit / original size"},
            {"Shift+arrows", "Pan original image"}, {"Ctrl+C", "Copy matching lines (or all text without a search)"},
            {"Ctrl+Shift+C", "Copy all screen text / full transcript"}, {"M", "Toggle screen highlights"},
            {"Alt+S", "Screen text / meetings filter"}, {"[  ]  Alt+↑ ↓", "Meeting passages"},
            {"Drag  /  S", "Select text to copy"}, {"Arrows / Shift+arrows", "Move / resize selection; Enter copies"},
            {"P  /  C", "Index moment / catch up"}, {"I  /  ?", "Controls / shortcuts"}, {"Esc", "Leave control / dismiss panel, then close"}};
        const int helpRows = (shortcuts.size() + 1) / 2;
        for (int i = 0; i < shortcuts.size(); ++i) {
            const int row = i % helpRows, column = i / helpRows * 2;
            auto* key = new QLabel(shortcuts[i].first); key->setProperty("keycap", true);
            auto* action = new QLabel(shortcuts[i].second);
            action->setWordWrap(true);
            helpLayout->addWidget(key, row, column, Qt::AlignLeft);
            helpLayout->addWidget(action, row, column + 1);
        }
        help_->hide();
        connect(helpToggle_, &QPushButton::toggled, this, [this](bool shown) {
            if (shown) detailsToggle_->setChecked(false);
            help_->setVisible(shown);
        });
        layout->addWidget(help_);

        resultsHeading_ = new QLabel;
        resultsHeading_->setObjectName("resultsHeading");
        resultsHeading_->hide();

        results_ = new QListWidget;
        results_->setObjectName("recallResults");
        results_->setAccessibleName("Matching recorded moments");
        results_->setFlow(QListView::LeftToRight);
        results_->setWrapping(false);
        results_->setWordWrap(false);
        results_->setTextElideMode(Qt::ElideRight);
        results_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        results_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        results_->setFixedHeight(34);
        results_->hide();
        mediaStatus_ = new QLabel;
        mediaStatus_->setObjectName("mediaStatus");
        mediaStatus_->setWordWrap(true);
        mediaStatus_->setTextFormat(Qt::PlainText);
        layout->addWidget(mediaStatus_);
        evidence_ = new EvidenceView(colors.accent);
        content_ = new QStackedWidget;
        content_->addWidget(evidence_);
        meetingView_ = new MeetingView;
        meetingView_->browseScreens = [this](qint64 start) { browseMeetingScreens(start); };
        content_->addWidget(meetingView_);
        layout->addWidget(content_, 1);
        auto* caption = new QHBoxLayout;
        recorded_ = new QLabel("Loading history…");
        recorded_->setObjectName("recordedTimestamp");
        recorded_->setTextFormat(Qt::PlainText);
        caption->addWidget(recorded_, 1);
        copyStatus_ = new QLabel;
        copyStatus_->setObjectName("copyStatus");
        copyStatus_->setTextFormat(Qt::PlainText);
        copyStatus_->hide();
        caption->addWidget(copyStatus_);
        copyNoticeTimer_.setSingleShot(true);
        copyNoticeTimer_.setInterval(3000);
        connect(&copyNoticeTimer_, &QTimer::timeout, copyStatus_, &QWidget::hide);
        evidence_->canvas()->selectionStarted = [this] { cancelSelectionOcr(); };
        evidence_->canvas()->selectionCancelled = [this] { cancelSelectionOcr(); };
        evidence_->canvas()->selectionFinished = [this](const QRect& crop) { requestSelectionOcr(crop); };
        connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] { ++clipboardRevision_; });
        connect(&selectionOcr_, &QFutureWatcher<SelectionResult>::finished, this, [this] {
            selectionWorkerActive_ = false;
            setProperty("selectionOcrRunning", false);
            if (closing_->load()) return;
            const auto result = selectionOcr_.result();
            if (result.revision == selectionOcrRevision_ && result.frameId == selected_.id &&
                result.frameId == property("displayedFrameId").toLongLong()) {
                setProperty("selectionOcrBusy", false);
                evidence_->canvas()->clearSelection();
                if (result.ocr.cancelled) copyStatus_->hide();
                else if (!result.ocr.error.isEmpty()) copyNotice(result.ocr.error);
                else if (result.ocr.text.trimmed().isEmpty()) copyNotice("No text found in selection.");
                else if (result.clipboardRevision != clipboardRevision_) copyNotice("Clipboard changed. Select again to copy.");
                else {
                    QApplication::clipboard()->setText(result.ocr.text);
                    copyNotice("Text copied");
                }
            }
            startSelectionOcr();
        });
        indexState_ = new QLabel;
        indexState_->setObjectName("indexState");
        indexState_->setTextFormat(Qt::PlainText);
        caption->addWidget(indexState_);
        layout->addLayout(caption);
        matchReadout_ = new QWidget;
        auto* matchLayout = new QHBoxLayout(matchReadout_);
        matchLayout->setContentsMargins(0, 0, 0, 0);
        matchLayout->setSpacing(18);
        matchLayout->addWidget(resultsHeading_);
        matchLayout->addStretch();
        previousMatch_ = new QPushButton("Previous match"); previousMatch_->setAccessibleName("Previous match");
        nextMatch_ = new QPushButton("Next match"); nextMatch_->setAccessibleName("Next match");
        matchLayout->addWidget(previousMatch_); matchLayout->addWidget(nextMatch_);
        connect(previousMatch_, &QPushButton::clicked, this, [this] { stepMatch(-1); });
        connect(nextMatch_, &QPushButton::clicked, this, [this] { stepMatch(1); });
        layout->addWidget(matchReadout_);
        matchReadout_->hide();
        timeline_ = new TimelineView(colors);
        timeline_->activateMatch = [this](qint64 id) { cancelSelectionOcr(); search(false, -1, -1, id); };
        timeline_->activateMeeting = [this](qint64 id) { openMeeting(id); };
        layout->addWidget(timeline_);
        layout->addWidget(results_);
        auto* controls = new QHBoxLayout;
        previous_ = new QPushButton("Earlier"); previous_->setObjectName("earlierMoment");
        previous_->setAccessibleName("Earlier moment");
        next_ = new QPushButton("Later"); next_->setObjectName("laterMoment");
        next_->setAccessibleName("Later moment");
        auto* fit = new QPushButton("Fit"); fit->setObjectName("fitImage");
        auto* actual = new QPushButton("100%"); actual->setObjectName("actualImageSize");
        for (auto* button : {previous_, next_, fit, actual}) controls->addWidget(button);
        controls->addStretch();
        auto* keys = new QLabel("/ search   ← → time   ↑ ↓ matches   S select text   ? help");
        keys->setObjectName("keyboardHelp");
        controls->addWidget(keys);
        connect(content_, &QStackedWidget::currentChanged, this, [this, fit, actual, keys] {
            const bool meeting = content_->currentWidget() == meetingView_;
            for (auto* button : {previous_, next_, fit, actual}) button->setVisible(!meeting);
            keys->setText(meeting ? "/ search   ↑ ↓ matches   [ ] passages   ? help"
                : "/ search   ← → time   ↑ ↓ matches   S select text   ? help");
        });
        layout->addLayout(controls);
        searchTimer_.setSingleShot(true);
        searchTimer_.setInterval(180);
        connect(&searchTimer_, &QTimer::timeout, this, [this] { search(); });
        connect(query_, &QLineEdit::textChanged, this, [this] { cancelSelectionOcr(); cancelSeek(); searchTimer_.start(); });
        connect(sourceFilter_, &QComboBox::currentIndexChanged, this, [this] { search(false, 0, 0); });
        connect(query_, &QLineEdit::returnPressed, this, [this] {
            search();
            if (query_->text().trimmed().isEmpty()) timeline_->setFocus(); else results_->setFocus();
        });

        connect(refresh, &QPushButton::clicked, this, [this] { search(true); });
        connect(results_, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { openResult(); });
        connect(results_, &QListWidget::currentRowChanged, this, [this](int) { openResult(); });
        connect(results_, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { openResult(); });
        connect(previous_, &QPushButton::clicked, this, [this] { stepTime(-1); });
        connect(next_, &QPushButton::clicked, this, [this] { stepTime(1); });
        connect(processMoment_, &QPushButton::clicked, this, [this] { requestMoment(false); });
        connect(catchUp_, &QPushButton::clicked, this, [this] { requestCatchUpNow(); });
        connect(serviceAction_, &QPushButton::clicked, this, [this] {
            requestService(serviceAction_->property("action").toString());
        });
        connect(stopService_, &QPushButton::clicked, this, [this] { requestService("stop"); });
        connect(copyWorkerCommand_, &QPushButton::clicked, this, [this] {
            QApplication::clipboard()->setText(workerCommand());
            requestMessage_ = "Index command copied. Run it in a terminal to process queued requests.";
            updateIndexingActions();
        });
        connect(fit, &QPushButton::clicked, this, [this] { evidence_->setFit(true); });
        connect(actual, &QPushButton::clicked, this, [this] { evidence_->setFit(false); });
        auto* focusSearch = new QShortcut(QKeySequence::Find, this);
        connect(focusSearch, &QShortcut::activated, this, [this] { query_->setFocus(); query_->selectAll(); });
        auto* focusSource = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_S), this);
        connect(focusSource, &QShortcut::activated, this, [this] { if (sourceFilter_->isVisible()) sourceFilter_->setFocus(); });
        auto* close = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        close->setAutoRepeat(false);
        connect(close, &QShortcut::activated, this, [this] { escape(); });
        auto* earlier = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
        connect(earlier, &QShortcut::activated, this, [this] { stepTime(-1); });
        auto* later = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Right), this);
        connect(later, &QShortcut::activated, this, [this] { stepTime(1); });
        auto* refreshShortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
        connect(refreshShortcut, &QShortcut::activated, this, [this] { search(true); });

        seekTimer_.setSingleShot(true);
        seekTimer_.setInterval(35);
        connect(timeline_, &QSlider::valueChanged, this, [this](int value) { seekTime(timeline_->timeAt(value)); });
        connect(&meetingReader_, &QFutureWatcher<MeetingReadResult>::finished, this, [this] {
            if (closing_->load()) return;
            const auto result = meetingReader_.result();
            if (result.revision != meetingRevision_ || result.id != selectedMeetingId_) { startMeetingRead(); return; }
            setProperty("meetingLoading", false);
            if (!result.meeting) {
                meetingView_->clear();
                mediaStatus_->setText(result.error.isEmpty() ? "This meeting is no longer in Replay history." : result.error);
                mediaStatus_->show();
                return;
            }
            selectedMeeting_ = *result.meeting;
            meetingView_->setMeeting(selectedMeeting_, completedQuery_);
            mediaStatus_->hide();
            if (selectedMeeting_.timeKnown) timeline_->setMoment(selectedMeeting_.startedAtMs);
            recorded_->setText(selectedMeeting_.timeKnown
                ? QDateTime::fromMSecsSinceEpoch(selectedMeeting_.startedAtMs).toString("ddd, MMM d · HH:mm:ss") + " · Meeting start"
                : "Meeting · recording time unknown");
            updateMatchReadout();
        });
        connect(&seekTimer_, &QTimer::timeout, this, [this] { startSeek(); });
        connect(&seeker_, &QFutureWatcher<SeekResult>::finished, this, [this] {
            if (closing_->load()) return;
            const auto result = seeker_.result();
            if (result.revision != seekRevision_) { if (seekPending_) startSeek(); return; }
            if (result.frame) openFrame(*result.frame);
            else if (!result.error.isEmpty()) { mediaStatus_->setText(result.error); mediaStatus_->show(); }
        });
        connect(&highlightReader_, &QFutureWatcher<HighlightResult>::finished, this, [this] {
            if (closing_->load()) return;
            const auto result = highlightReader_.result();
            if (result.directory != directory_ || result.id != selected_.id || result.query != completedQuery_) { startHighlights(); return; }
            highlighted_ = result;
            if (property("displayedFrameId").toLongLong() == selected_.id) evidence_->setHighlights(result.matches.boxes);
            setProperty("highlightCount", result.matches.boxes.size());
            indexState_->setToolTip(result.matches.boxes.isEmpty() && !completedQuery_.isEmpty()
                ? "No matching text positions stored for this moment. Older history remains searchable without highlights."
                : "Highlights mark recognized text lines, not exact word boundaries. M toggles highlights.");
        });

        dwellTimer_.setSingleShot(true);
        dwellTimer_.setInterval(500);
        connect(&dwellTimer_, &QTimer::timeout, this, [this] { requestMoment(true); });
        connect(&historyReader_, &QFutureWatcher<HistorySnapshot>::finished, this, [this] {
            if (closing_->load()) return;
            const auto snapshot = historyReader_.result();
            if (snapshot.generation != historyGeneration_) { startHistoryRead(); return; }
            setProperty("historyLoading", false);
            applyHistory(snapshot);
        });
        connect(&neighborReader_, &QFutureWatcher<TimelineNeighbors>::finished, this, [this] {
            if (closing_->load()) return;
            const auto neighbors = neighborReader_.result();
            if (neighbors.directory != directory_ || neighbors.id != selected_.id) { startNeighborRead(); return; }
            previousFrame_ = neighbors.previous;
            nextFrame_ = neighbors.next;
            previous_->setEnabled(previousFrame_.has_value());
            next_->setEnabled(nextFrame_.has_value());
            if (!neighbors.error.isEmpty()) status_->setText("Timeline unavailable: " + neighbors.error);
        });
        connect(&indexRequest_, &QFutureWatcher<IndexingRequestResult>::finished, this, [this] {
            if (closing_->load()) return;
            const auto result = indexRequest_.result();
            setProperty("indexingRequestInFlight", false);
            if (result.directory != directory_) { updateIndexingActions(); search(true); return; }
            if (!result.error.isEmpty()) {
                requestMessage_ = "Unable to queue indexing: " + result.error;
            } else {
                if (result.frameId) requestedMoments_.insert(result.frameId);
                if (result.catchUp) catchUpUntil_ = QDateTime::currentMSecsSinceEpoch() + 120000;
                requestMessage_ = result.catchUp ? "Two-minute catch-up request saved."
                    : QString("Moment request saved; %1 pending moments prioritized.").arg(result.changed);
            }
            updateIndexingActions();
            search(true);
            if (selected_.id != result.frameId && selected_.ocrState == "pending" &&
                !requestedMoments_.contains(selected_.id)) dwellTimer_.start();
        });

        connect(&serviceRequest_, &QFutureWatcher<ServiceControlResult>::finished, this, [this] {
            if (closing_->load()) return;
            const auto result = serviceRequest_.result();
            setProperty("serviceRequestInFlight", false);
            if (!result.error.isEmpty()) requestMessage_ = "Unable to change indexing: " + result.error;
            else {
                service_ = result.status;
                requestMessage_.clear();
            }
            updateIndexingActions();
            search(true);
        });

        connect(&recordingRequest_, &QFutureWatcher<ServiceControlResult>::finished, this, [this] {
            if (closing_->load()) return;
            const auto result = recordingRequest_.result();
            setProperty("recordingRequestInFlight", false);
            recordingMessage_ = result.error;
            if (result.error.isEmpty()) recording_ = result.status;
            if (result.error.isEmpty() && result.action == "reload" && followConfiguredHistory(result.status)) return;
            updateRecordingActions(); search(true);
        });

        connect(&decoder_, &QFutureWatcher<DecodedFrame>::finished, this, [this] {
            if (closing_->load()) return;
            const DecodedFrame result = decoder_.result();
            if (result.directory == directory_ && result.id == selected_.id) {
                setProperty("mediaLoading", false);
                if (result.image.isNull()) {
                    evidence_->setImage({});
                    mediaStatus_->setText(result.error.isEmpty() ? "Recorded image is unavailable." : result.error);
                    mediaStatus_->show();
                } else {
                    evidence_->setImage(result.image);
                    setProperty("displayedFrameId", result.id);
                    if (copyStatus_->text() == "Wait for the recorded image to load.") copyStatus_->hide();
                    if (highlighted_.id == result.id && highlighted_.directory == directory_ && highlighted_.query == completedQuery_)
                        evidence_->setHighlights(highlighted_.matches.boxes);
                    mediaStatus_->clear();
                    mediaStatus_->hide();
                    evidence_->setToolTip(QString("Drag to copy text · S keyboard selection · %1 × %2 · F fit / 1 original size").arg(result.image.width()).arg(result.image.height()));
                }
            } else {
                startDecode();
            }
        });
        for (QWidget* child : findChildren<QWidget*>()) child->installEventFilter(this);
        installEventFilter(this);
        updateNavigation();
        updateIndexingActions();
        updateRecordingActions();
        search();
        refreshTimer_.setInterval(2000);
        connect(&refreshTimer_, &QTimer::timeout, this, [this] {
            if (isVisible() && !searchTimer_.isActive() && !historyReader_.isRunning()) search(true);
        });
        refreshTimer_.start();
        query_->setFocus();
    }

    ~Viewer() override { finishWork(); }

    void summon(bool settings) {
        showNormal(); raise(); activateWindow();
        if (settings) showSettings();
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        finishWork();
        QWidget::closeEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (detailsScroll_) fitDetailsPanel();
    }

    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (object == sourceFilter_) return QWidget::eventFilter(object, event);
            if (selectedMeetingId_ && object != query_) {
                if ((key->key() == Qt::Key_BracketLeft && key->modifiers() == Qt::NoModifier) || (key->key() == Qt::Key_Up && key->modifiers() == Qt::AltModifier)) {
                    meetingView_->focusPassage(-1); return true;
                }
                if ((key->key() == Qt::Key_BracketRight && key->modifiers() == Qt::NoModifier) || (key->key() == Qt::Key_Down && key->modifiers() == Qt::AltModifier)) {
                    meetingView_->focusPassage(1); return true;
                }
            }
            if (object == evidence_->canvas() && evidence_->canvas()->selectionKey(key)) return true;
            if (!selectedMeetingId_ && object != query_ && key->modifiers() == Qt::ShiftModifier &&
                (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down)) {
                auto* scroll = (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)
                    ? evidence_->horizontalScrollBar() : evidence_->verticalScrollBar();
                const int direction = (key->key() == Qt::Key_Left || key->key() == Qt::Key_Up) ? -1 : 1;
                scroll->setValue(scroll->value() + direction * 100);
                return true;
            }
            if (object != query_ && key->key() == Qt::Key_C && key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
                copyText(true);
                return true;
            }
            if (key->matches(QKeySequence::Copy) && object != query_) {
                copyText(false);
                return true;
            }
            if (object != query_ && key->key() == Qt::Key_Question) { helpToggle_->toggle(); return true; }
            if (key->modifiers() == Qt::NoModifier) {
                if (object != query_) {
                    switch (key->key()) {
                    case Qt::Key_Slash: query_->setFocus(); query_->selectAll(); return true;
                    case Qt::Key_Question: helpToggle_->toggle(); return true;
                    case Qt::Key_I: detailsToggle_->toggle(); return true;
                    case Qt::Key_PageDown:
                    case Qt::Key_PageUp:
                        if (selectedMeetingId_ && object->objectName() == "meetingTranscript") break;
                        changePage(key->key() == Qt::Key_PageDown ? 1 : -1); return true;
                    case Qt::Key_F: evidence_->setFit(true); return true;
                    case Qt::Key_1: evidence_->setFit(false); return true;
                    case Qt::Key_M: evidence_->toggleHighlights(); return true;
                    case Qt::Key_S:
                        if (selectedMeetingId_) return true;
                        evidence_->beginKeyboardSelection();
                        if (evidence_->canvas()->selectionActive()) {
                            copyNotice("Arrows move · Shift+arrows resize · Enter copies");
                            copyNoticeTimer_.stop();
                        }
                        return true;
                    case Qt::Key_H: stepTime(-1); return true;
                    case Qt::Key_L: stepTime(1); return true;
                    case Qt::Key_Home: seekTime(overview_.firstTimestampMs); return true;
                    case Qt::Key_End: seekTime(overview_.lastTimestampMs); return true;
                    case Qt::Key_J: case Qt::Key_Down: if (completedQuery_.trimmed().isEmpty() && !results_->isVisible() && !selectedMeetingId_) stepTime(1); else stepMatch(1); return true;
                    case Qt::Key_K: case Qt::Key_Up: if (completedQuery_.trimmed().isEmpty() && !results_->isVisible() && !selectedMeetingId_) stepTime(-1); else stepMatch(-1); return true;
                    default: break;
                    }
                }
                if (object != query_ && key->key() == Qt::Key_P) {
                    requestMoment(false);
                    return true;
                }
                if (object != query_ && key->key() == Qt::Key_C) {
                    requestCatchUpNow();
                    return true;
                }
                if (object == query_ && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
                    searchTimer_.stop();
                    if (query_->text() != completedQuery_) search();
                    if (query_->text().trimmed().isEmpty()) timeline_->setFocus(); else results_->setFocus();
                    if (results_->currentRow() < 0 && results_->count()) results_->setCurrentRow(0);
                    return true;
                }
                if (object != query_ && (key->key() == Qt::Key_Left || key->key() == Qt::Key_Right)) {
                    stepTime(key->key() == Qt::Key_Left ? -1 : 1);
                    return true;
                }
            }
        }
        return QWidget::eventFilter(object, event);
    }

private:
    void fitDetailsPanel() {
        const bool columns = sharedHistory_ && width() >= 1040;
        panelColumns_->setDirection(columns ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        detailsContents_->setMaximumWidth(columns ? 1160 : 740);
        const int available = std::min(detailsContents_->maximumWidth(), std::max(1, width() - 40));
        const int preferred = panelColumns_->hasHeightForWidth()
            ? panelColumns_->totalHeightForWidth(available) : panelColumns_->sizeHint().height();
        detailsScroll_->setFixedHeight(std::min(std::max(120, preferred + 4), std::max(160, height() / 2)));
    }

    bool followConfiguredHistory(const QJsonObject& status) {
        const QString history = status.value("history_directory").toString();
        if (!sharedHistory_ || history.isEmpty() || !status.value("config_error").toString().isEmpty() ||
            (status.contains("storage_available") && !status.value("storage_available").toBool()) ||
            QDir(history).absolutePath() == directory_) return false;
        directory_ = QDir(history).absolutePath();
        cancelSelectionOcr();
        setProperty("historyDirectory", directory_);
        cancelSeek(); ++selectionRevision_;
        selected_ = {}; matches_.clear(); resultRows_.clear(); closeMeeting(); requestedMoments_.clear();
        evidence_->setImage({}); evidence_->setHighlights({});
        setProperty("displayedFrameId", 0); setProperty("selectedFrameId", 0);
        setProperty("highlightCount", 0); highlighted_ = {};
        pageOffset_ = 0; totalFrames_ = 0; totalMatches_ = 0;
        updateNavigation(); updateRecordingActions(); search(false, 0);
        return true;
    }

    void updateRecordingActions() {
        if (!sharedHistory_) return;
        const bool busy = recordingRequest_.isRunning();
        const QString intent = recording_.value("intent").toString("stopped");
        const bool running = recording_.value("running").toBool();
        const QString action = intent == "paused" ? "resume" : intent == "running" && running ? "pause" : "start";
        recordingAction_->setText(action == "pause" ? "Pause recording" : action == "resume" ? "Resume recording" : "Start recording");
        recordingAction_->setProperty("action", action);
        recordingAction_->setEnabled(!busy);
        recordingStop_->setEnabled(!busy && intent != "stopped");
        deleteRecent_->setEnabled(!busy && totalFrames_ > 0);
        QString state = recording_.value("state").toString("offline");
        const QMap<QString, QString> states{{"offline", "Recording is stopped."}, {"stopped", "Recording is stopped."},
            {"recording", "Recording"}, {"paused", "Recording is paused."}, {"locked", "Paused while the screen is locked."},
            {"sleeping", "Paused while the computer sleeps."}, {"output-unavailable", "Waiting for your selected display."},
            {"excluded", "Paused while an excluded window is visible."}, {"storage-cleanup", "Rolling out oldest history to make room."},
            {"storage-full", "Recording paused: storage could not be reclaimed."},
            {"excluded_window", "Paused while an excluded window is visible."},
            {"focus_unknown", "Waiting for a focused display."},
            {"output_unavailable", "Waiting for your selected display."}, {"output_off", "Waiting for your selected display."},
            {"output_identity_changed", "The selected display has changed. Review Settings."},
            {"stale_window_exclusion", "Review window exclusions for this desktop session."},
            {"deleting", "Removing recent history."}, {"capture-error", "Recording needs attention."},
            {"error", "Recording needs attention."}};
        QStringList message;
        if (busy) message << "Updating recording controls…";
        else message << states.value(state, "Waiting to record.");
        if (state == "recording") {
            const bool focused = recording_.value("display_mode").toString() == "focused";
            const QString recording = focused ? recording_.value("recording_output").toString() : recording_.value("output").toString();
            if (!recording.isEmpty()) message.last() += focused ? QString(" %1 (follows focus).").arg(recording) : " " + recording + ".";
        }
        for (const auto& value : {recording_.value("reason").toString(), recording_.value("config_error").toString(), recording_.value("storage_error").toString(), recordingMessage_})
            if (!value.isEmpty() && value != "Recording the selected display." && !message.contains(value)) message << value;
        recordingState_->setText(message.join('\n'));
        storageCapacity_->setText(storageCapacitySummary(recording_, directory_));
        storageDetails_->setText(storageCapacityText(recording_, directory_));
        storageCapacity_->setToolTip(currentStorageStatus(recording_, directory_)
            ? recording_.value("storage_forecast").toObject().value("estimate_note").toString() : QString());
        setProperty("recordingState", state);
        fitDetailsPanel();
    }

    void requestRecording(const QString& action, const QJsonObject& arguments = {}) {
        if (!sharedHistory_ || closing_->load() || recordingRequest_.isRunning()) return;
        const auto control = services_.recordingControl;
        const auto closing = closing_;
        setProperty("recordingRequestInFlight", true);
        recordingRequest_.setFuture(QtConcurrent::run([action, arguments, control, closing] {
            ServiceControlResult result; result.action = action;
            try { if (!closing->load()) result.status = control(action, arguments); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
        updateRecordingActions(); updateIndexingActions();
    }

    void showSettings() {
        if (settingsDialog_) { settingsDialog_->raise(); settingsDialog_->activateWindow(); return; }
        SettingsDialog settings(this, recording_, services_.displays);
        settingsDialog_ = &settings;
        if (settings.exec() == QDialog::Accepted) requestRecording("reload");
    }

    void deleteRecent() {
        QInputDialog interval(this);
        theme(&interval);
        interval.setWindowTitle("Delete recent history"); interval.setLabelText("Delete the last how many minutes?");
        interval.setInputMode(QInputDialog::IntInput); interval.setIntRange(1, 1440); interval.setIntValue(5);
        removeButtonIcons(&interval);
        if (interval.exec() != QDialog::Accepted) return;
        const int minutes = interval.intValue();
        QMessageBox confirmation(QMessageBox::NoIcon, "Permanently delete recent history?",
            QString("Delete screen history and queued work from the last %1 minutes? Imported meetings that started in this interval are also removed; meetings without a known start use their import date. Original meeting files stay in their folder.\n\nThis cannot be undone. Recording keeps its current start or pause choice.").arg(minutes),
            QMessageBox::Yes | QMessageBox::Cancel, this);
        confirmation.setObjectName("confirmDeleteRecent");
        theme(&confirmation);
        removeButtonIcons(&confirmation);
        confirmation.setDefaultButton(QMessageBox::Cancel);
        if (confirmation.exec() == QMessageBox::Yes) requestRecording("delete-recent", {{"seconds", minutes * 60}, {"confirmed", true}});
    }

    void escape() {
        if (evidence_->canvas()->selectionActive() || property("selectionOcrBusy").toBool()) {
            cancelSelectionOcr();
            setFocus(Qt::OtherFocusReason);
            return;
        }
        if (query_->hasFocus() && query_->text() != completedQuery_) search();
        if (details_->isVisible() || help_->isVisible()) {
            detailsToggle_->setChecked(false);
            helpToggle_->setChecked(false);
            setFocus(Qt::OtherFocusReason);
            return;
        }
        if (focusWidget() && focusWidget() != this) {
            setFocus(Qt::OtherFocusReason);
            return;
        }
        close();
    }

    void copyNotice(const QString& text) {
        copyStatus_->setText(text);
        copyStatus_->show();
        copyNoticeTimer_.start();
    }

    void copyText(bool wholeScreen) {
        if (selectedMeetingId_) { meetingView_->copyText(wholeScreen); return; }
        cancelSelectionOcr();
        if (!selected_.id || property("displayedFrameId").toLongLong() != selected_.id) {
            copyNotice("Wait for the recorded image to load.");
            return;
        }
        const bool searching = !query_->text().trimmed().isEmpty();
        QString text = selected_.text;
        if (!wholeScreen && searching) {
            if (query_->text() != completedQuery_ || highlighted_.id != selected_.id || highlighted_.query != completedQuery_) {
                copyNotice("Matching text is loading. Try copying again.");
                return;
            }
            if (!highlighted_.error.isEmpty() || !highlighted_.matches.geometryAvailable) {
                copyNotice("Matching lines unavailable. Ctrl+Shift+C copies all screen text.");
                return;
            }
            text = highlighted_.matches.text;
        }
        if (text.trimmed().isEmpty()) {
            copyNotice(searching && !wholeScreen ? "No matching lines to copy." : "No recognized text to copy.");
            return;
        }
        QApplication::clipboard()->setText(text);
        copyNotice(searching && !wholeScreen ? "Matching lines copied" : "Screen text copied");
    }

    void cancelSelectionOcr() {
        ++selectionOcrRevision_;
        if (selectionCancel_) selectionCancel_->store(true);
        pendingSelection_.reset();
        const bool hadSelection = evidence_->canvas()->selectionActive() || property("selectionOcrBusy").toBool();
        evidence_->canvas()->clearSelection();
        setProperty("selectionOcrBusy", false);
        if (hadSelection) { copyNoticeTimer_.stop(); copyStatus_->hide(); }
    }

    void requestSelectionOcr(const QRect& crop) {
        if (closing_->load() || crop.isEmpty() || selected_.id <= 0 ||
            property("displayedFrameId").toLongLong() != selected_.id) return;
        if (selectionCancel_) selectionCancel_->store(true);
        pendingSelection_ = SelectionRequest{evidence_->canvas()->image, crop, selected_.id,
            ++selectionOcrRevision_, clipboardRevision_};
        setProperty("selectionOcrBusy", true);
        copyNotice("Reading selection… Esc cancels");
        copyNoticeTimer_.stop();
        startSelectionOcr();
    }

    void startSelectionOcr() {
        if (selectionWorkerActive_ || !pendingSelection_ || closing_->load()) return;
        const auto request = std::move(*pendingSelection_);
        pendingSelection_.reset();
        selectionCancel_ = std::make_shared<std::atomic_bool>(false);
        const auto cancel = selectionCancel_;
        const auto recognize = services_.selectionOcr;
        selectionWorkerActive_ = true;
        setProperty("selectionOcrRunning", true);
        selectionOcr_.setFuture(QtConcurrent::run([request, cancel, recognize] {
            SelectionResult result{{}, request.frameId, request.revision, request.clipboardRevision};
            try {
                if (cancel->load()) result.ocr.cancelled = true;
                else result.ocr = recognize(request.image.copy(request.crop), cancel);
            } catch (const std::exception&) {
                result.ocr.error = "Could not read the selected text. Try a smaller area.";
            } catch (...) {
                result.ocr.error = "Could not read the selected text.";
            }
            return result;
        }));
    }

    void finishWork() {
        if (closing_->exchange(true)) return;
        cancelSelectionOcr();
        searchTimer_.stop();
        refreshTimer_.stop();
        dwellTimer_.stop();
        seekTimer_.stop();
        copyNoticeTimer_.stop();
        selectionOcr_.waitForFinished();
        setProperty("selectionOcrRunning", false);
        // Value-captured jobs never access widgets. Stop queued jobs, cancel an
        // active decoder, and let a started SQLite transaction finish (the core
        // uses a one-second busy timeout) before the dataset/viewer can go away.
        indexRequest_.waitForFinished();
        serviceRequest_.waitForFinished();
        recordingRequest_.waitForFinished();
        historyReader_.waitForFinished();
        neighborReader_.waitForFinished();
        decoder_.waitForFinished();
        seeker_.waitForFinished();
        highlightReader_.waitForFinished();
        meetingReader_.waitForFinished();
        setProperty("indexingRequestInFlight", false);
        setProperty("serviceRequestInFlight", false);
        setProperty("historyLoading", false);
        setProperty("mediaLoading", false);
    }

    QString indexingNotice() const {
        QStringList notices;
        if (pending_ > 0)
            notices << QString("%1 saved moments have text indexing pending; search is incomplete.").arg(pending_);
        if (failed_ > 0)
            notices << QString("Text indexing failed for %1 saved moments.").arg(failed_);
        if (disabled_ > 0)
            notices << QString("Text indexing is disabled for %1 saved moments.").arg(disabled_);
        if (legacy_)
            notices << "Older dataset: original indexing status was not recorded.";
        return notices.join(' ');
    }

    void search(bool preserve = false, int targetRow = -1, qint64 requestedOffset = -1, qint64 anchorFrameId = 0) {
        if (closing_->load()) return;
        const bool newQuery = query_->text() != completedQuery_ || sourceFilter_->currentData().toString() != completedSource_;
        if (newQuery) { requestedOffset = 0; targetRow = -1; anchorFrameId = 0; }
        else if (property("historyLoading").toBool() && requestedOffset < 0 && !anchorFrameId) {
            // A repeated search or refresh must keep the in-flight destination;
            // pageOffset_ still describes the previously displayed query/page.
            requestedOffset = requestedHistory_.offset;
            anchorFrameId = requestedHistory_.anchorFrameId;
            if (targetRow < 0) targetRow = requestedHistory_.targetRow;
            preserve = preserve && requestedHistory_.preserve;
        }
        preserve = preserve && !newQuery;
        if (preserve && !anchorFrameId && !query_->text().trimmed().isEmpty()) anchorFrameId = selected_.id;
        searchTimer_.stop();
        if (!preserve) { dwellTimer_.stop(); cancelSeek(); }
        completedQuery_ = query_->text();
        completedSource_ = sourceFilter_->currentData().toString();
        requestedHistory_ = {};
        requestedHistory_.generation = ++historyGeneration_;
        requestedHistory_.selectionRevision = selectionRevision_;
        requestedHistory_.query = completedQuery_;
        requestedHistory_.source = completedSource_;
        requestedHistory_.offset = requestedOffset >= 0 ? requestedOffset : pageOffset_;
        requestedHistory_.anchorFrameId = anchorFrameId;
        requestedHistory_.targetRow = targetRow;
        requestedHistory_.preserve = preserve;
        requestedHistory_.currentId = preserve ? selected_.id : 0;
        setProperty("historyLoading", true);
        if (!historyReader_.isRunning()) startHistoryRead();
    }

    void startHistoryRead() {
        if (closing_->load()) return;
        const QString directory = directory_;
        const auto request = requestedHistory_;
        const auto closing = closing_;
        const bool shared = sharedHistory_;
        const auto recordingStatus = services_.recordingStatus;
        historyReader_.setFuture(QtConcurrent::run([directory, request, closing, shared, recordingStatus]() mutable {
            auto result = request;
            try {
                if (closing->load()) return result;
                if (shared) {
                    try { result.recording = recordingStatus(); }
                    catch (const std::exception& error) { result.recording = {{"state", "error"}, {"reason", QString::fromUtf8(error.what())}}; }
                    result.service = result.recording.value("index_service").toObject();
                    result.service["running"] = result.recording.value("running").toBool();
                    result.service["paused"] = result.recording.value("indexing_paused").toBool();
                    result.service["enabled"] = true;
                    result.service["worker_policy"] = result.recording.value("worker_policy");
                    result.service["worker_running"] = result.recording.value("indexing").toBool();
                    result.service["last_worker_resources"] = result.recording.value("worker_resources");
                    const auto activity = result.recording.value("worker_policy").toObject();
                    const qint64 updated = activity.value("updated_ms").toInteger();
                    result.service["policy_age_ms"] = updated ? std::max<qint64>(0, QDateTime::currentMSecsSinceEpoch() - updated) : -1;
                    result.service["effective_cpu_percent"] = activity.value("effective_cpu_percent");
                    result.service["state"] = activity.value("mode").toString("waiting");
                    if (!result.recording.value("index_error").toString().isEmpty()) {
                        result.service["state"] = "error";
                        result.service["error"] = result.recording.value("index_error");
                    }
                    result.policy = result.recording.value("index_policy").toObject();
                    if (!QFileInfo::exists(QDir(directory).filePath("index.sqlite"))) return result;
                }
                result.indexing = indexingStatus(directory);
                try {
                    if (!shared) result.service = indexServiceStatus(directory);
                    if (!shared) result.policy = savedIndexPolicy(directory);
                } catch (const std::exception& error) {
                    result.service["state"] = "error";
                    result.service["error"] = QString::fromUtf8(error.what());
                }
                if (closing->load()) return result;
                result.timeline = timelineOverview(directory);
                readRecallResults(directory, result);
                if (!closing->load() && result.currentId) result.current = frameById(directory, result.currentId);
                if (!closing->load() && !result.current && result.source != "meetings" && result.query.trimmed().isEmpty() && result.timeline.totalFrames)
                    result.current = frameNearTimestamp(directory, result.timeline.lastTimestampMs);
            } catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
    }

    void applyHistory(const HistorySnapshot& snapshot) {
        if (sharedHistory_ && !recordingRequest_.isRunning()) {
            recording_ = snapshot.recording;
            if (followConfiguredHistory(recording_)) return;
            updateRecordingActions();
        }
        if (query_->text() != snapshot.query || sourceFilter_->currentData().toString() != snapshot.source) { search(); return; }
        const bool preserve = snapshot.preserve;
        const bool navigated = snapshot.selectionRevision != selectionRevision_;
        const qint64 resultId = results_->currentItem() ? results_->currentItem()->data(Qt::UserRole).toLongLong() : 0;
        const int scroll = results_->horizontalScrollBar()->value();
        if (snapshot.error.isEmpty()) {
            const auto& index = snapshot.indexing;
            legacy_ = index.value("legacy_schema").toBool();
            pending_ = index.value("pending").toInteger();
            failed_ = index.value("failed").toInteger();
            disabled_ = index.value("disabled").toInteger();
            priorityPending_ = index.value("priority_pending").toInteger();
            catchUpUntil_ = index.value("catch_up_until_ms").toInteger();
            indexerRunning_ = index.value("indexer_running").toBool();
            oldestPendingMs_ = index.value("index_lag_ms").toInteger();
            service_ = snapshot.service;
            servicePolicy_ = snapshot.policy;
            matches_ = snapshot.matches;
            resultRows_ = snapshot.rows;
            overview_ = snapshot.timeline;
            for (const auto& meeting : snapshot.meetings) {
                if (!overview_.totalFrames) overview_.firstTimestampMs = overview_.lastTimestampMs = meeting.startedAtMs;
                else {
                    overview_.firstTimestampMs = std::min(overview_.firstTimestampMs, meeting.startedAtMs);
                    overview_.lastTimestampMs = std::max(overview_.lastTimestampMs, meeting.startedAtMs);
                }
                ++overview_.totalFrames;
            }
            timeline_->setOverview(overview_);
            timeline_->setMeetings(snapshot.meetings);
            totalMatches_ = snapshot.totalResults;
            pageOffset_ = snapshot.resultOffset;
            timeline_->setMatches(snapshot.page.timeline);
            sourceFilter_->setVisible(snapshot.meetingCount > 0 || snapshot.source != "all" || recording_.value("meetings").toObject().value("enabled").toBool());
            query_->setPlaceholderText(sourceFilter_->isVisible() ? "Search your history" : "Search what you saw");
            setProperty("meetingCount", snapshot.meetingCount);
            setProperty("totalMatches", totalMatches_);
            setProperty("matchPageOffset", pageOffset_);
            const QSignalBlocker blockResults(results_);
            results_->clear();
            int selectedRow = snapshot.selectedRow >= 0 ? snapshot.selectedRow : std::max(0, snapshot.targetRow);
            bool hasMeetingRows = false;
            for (const auto& result : resultRows_) {
                auto* item = new QListWidgetItem;
                item->setData(Qt::UserRole, result.key());
                if (result.isMeeting) {
                    hasMeetingRows = true;
                    const auto& meeting = result.meeting.meeting;
                    const QString title = fontMetrics().elidedText(meeting.title, Qt::ElideRight, 255);
                    const QString date = meeting.timeKnown ? QDateTime::fromMSecsSinceEpoch(meeting.startedAtMs).toString("MMM d · HH:mm") : "Time unknown";
                    item->setText(title + "\n" + date + (completedQuery_.trimmed().isEmpty() ? " · Meeting"
                        : result.meeting.matchingPassages ? QString(" · %1 matches").arg(result.meeting.matchingPassages) : " · Title match"));
                    item->setToolTip(meeting.title + " · " + date);
                    item->setSizeHint(QSize(285, 48));
                } else {
                    item->setText(QDateTime::fromMSecsSinceEpoch(result.frame.timestampMs).toString("HH:mm:ss"));
                    item->setToolTip(QDateTime::fromMSecsSinceEpoch(result.frame.timestampMs).toString("MMM d · HH:mm:ss") + " · Screen text");
                    item->setSizeHint(QSize(108, 28));
                }
                if ((preserve || navigated) && result.key() == resultId) selectedRow = results_->count();
                results_->addItem(item);
            }
            results_->setFixedHeight(hasMeetingRows ? 56 : 34);
            const bool recent = completedQuery_.trimmed().isEmpty() && completedSource_ != "meetings";
            results_->setVisible((!recent || hasMeetingRows) && !resultRows_.isEmpty());
            resultsHeading_->setVisible(!recent || hasMeetingRows);
            matchReadout_->setVisible(!recent || hasMeetingRows);
            ready_ = index.value("ready").toInteger();
            totalFrames_ = index.value("coverage_total_frames").toInteger();
            detailsToggle_->setText(sharedHistory_ ? "Controls" : pending_ > 0 ? QString("%1 pending").arg(pending_) : failed_ > 0
                ? QString("%1 failed").arg(failed_) : "Index");
            detailsToggle_->setToolTip(sharedHistory_ ? "Recording, indexing and settings (I)" : indexingNotice().isEmpty() ? "Search indexing status (I)" : indexingNotice());
            if (!resultRows_.isEmpty()) results_->setCurrentRow(std::min(selectedRow, int(resultRows_.size()) - 1));
            if (preserve || navigated) results_->horizontalScrollBar()->setValue(scroll);
            if ((preserve || navigated) && selectedMeetingId_) {
                const auto found = std::find_if(resultRows_.cbegin(), resultRows_.cend(), [this](const auto& row) { return row.key() == -selectedMeetingId_; });
                if (found != resultRows_.cend()) openMeeting(selectedMeetingId_, found->meeting.meeting.revision);
                else openMeeting(selectedMeetingId_);
            } else if (navigated && selected_.id) {
                const auto found = std::find_if(matches_.begin(), matches_.end(), [this](const auto& row) { return row.id == selected_.id; });
                if (found != matches_.end()) openFrame(*found);
                else describeFrame();
            } else if (snapshot.current && snapshot.targetRow < 0) {
                openFrame(*snapshot.current);
            } else if (resultRows_.isEmpty()) {
                closeMeeting();
                cancelSelectionOcr();
                selected_ = {};
                ++selectionRevision_;
                dwellTimer_.stop();
                setProperty("selectedFrameId", 0);
                setProperty("displayedFrameId", 0);
                evidence_->setImage({});
                indexState_->clear();
                recorded_->setText(recent ? sharedHistory_ ? "Your history starts here" : "No recorded history in this dataset" : "No matching recorded text");
                mediaStatus_->show();
                const auto meetingState = recording_.value("meetings").toObject();
                const QString noMeetings = snapshot.meetingCount > 0
                    ? "No meetings matched. Try a shorter word or clear the search."
                    : !meetingState.value("error").toString().isEmpty() ? meetingState.value("error").toString()
                    : meetingState.value("enabled").toBool()
                        ? meetingState.value("paused").toBool() ? "Meeting imports are paused. Resume indexing in Controls to check for completed transcripts."
                            : "No completed transcripts have been imported yet. Check the meetings folder in Settings."
                        : "No meetings in this history. Enable meeting transcripts in Settings when Meeting Recorder is installed.";
                mediaStatus_->setText(completedSource_ == "meetings" ? noMeetings
                    : pending_ > 0 ? "Saved images are not searchable yet. Clear the search to browse them while indexing is pending."
                    : failed_ > 0 ? "Text indexing failed for saved images. Clear the search to browse the recorded images."
                    : recent ? sharedHistory_ ? "Open Controls with I to choose a display and start recording. Opening Replay does not record your screen."
                                             : "Run the controlled demo to create synthetic history."
                             : "Try a shorter word or another spelling. This does not prove it never appeared.");
                updateNavigation();
            } else {
                openResult();
            }
        } else {
            if (preserve) {
                status_->setText("Unable to refresh history: " + snapshot.error);
                return;
            }
            results_->clear();
            matches_.clear(); resultRows_.clear(); closeMeeting();
            cancelSelectionOcr();
            selected_ = {};
            ++selectionRevision_;
            dwellTimer_.stop();
            setProperty("selectedFrameId", 0);
            setProperty("displayedFrameId", 0);
            evidence_->setImage({});
            indexState_->clear();
            resultsHeading_->setText("History unavailable");
            recorded_->setText("Unable to read this local dataset");
            mediaStatus_->setText(snapshot.error);
            mediaStatus_->show();
            status_->setText(directory_);
            updateNavigation();
        }
        updateIndexingActions();
        updateRecordingActions();
        updateMatchReadout();
    }

    void openResult() {
        cancelSeek();
        const int row = results_->currentRow();
        if (row < 0 || row >= resultRows_.size()) return;
        const auto& result = resultRows_[row];
        if (result.isMeeting) openMeeting(result.meeting.meeting.id, result.meeting.meeting.revision);
        else openFrame(result.frame);
    }

    void closeMeeting() {
        if (!selectedMeetingId_) return;
        selectedMeetingId_ = 0;
        selectedMeeting_ = {};
        ++meetingRevision_;
        setProperty("selectedMeetingId", 0);
        setProperty("meetingLoading", false);
        content_->setCurrentWidget(evidence_);
    }

    void openMeeting(qint64 id, const QString& revision = {}) {
        if (closing_->load() || id <= 0) return;
        cancelSeek(); cancelSelectionOcr(); dwellTimer_.stop();
        const bool unchanged = selectedMeetingId_ == id && !revision.isEmpty() && selectedMeeting_.revision == revision;
        if (selectedMeetingId_ != id) {
            selectedMeeting_ = {};
            meetingView_->clear();
            ++selectionRevision_;
        }
        selectedMeetingId_ = id;
        const auto found = std::find_if(resultRows_.cbegin(), resultRows_.cend(), [id](const auto& row) { return row.key() == -id; });
        {
            const QSignalBlocker blocker(results_);
            results_->setCurrentRow(found == resultRows_.cend() ? -1 : int(std::distance(resultRows_.cbegin(), found)));
            if (results_->currentItem()) results_->scrollToItem(results_->currentItem());
        }
        selected_ = {};
        setProperty("selectedFrameId", 0);
        setProperty("displayedFrameId", 0);
        setProperty("selectedMeetingId", id);
        content_->setCurrentWidget(meetingView_);
        indexState_->clear();
        mediaStatus_->hide();
        recorded_->setText("Meeting");
        updateNavigation(); updateIndexingActions(); updateMatchReadout();
        if (unchanged) {
            // setMeeting preserves the text selection and scroll position when
            // neither body nor query changed during a background refresh.
            meetingView_->setMeeting(selectedMeeting_, completedQuery_);
            recorded_->setText(selectedMeeting_.timeKnown
                ? QDateTime::fromMSecsSinceEpoch(selectedMeeting_.startedAtMs).toString("ddd, MMM d · HH:mm:ss") + " · Meeting start"
                : "Meeting · recording time unknown");
            return;
        }
        ++meetingRevision_;
        setProperty("meetingLoading", true);
        if (!meetingReader_.isRunning()) startMeetingRead();
    }

    void startMeetingRead() {
        if (closing_->load() || !selectedMeetingId_) return;
        const auto directory = directory_;
        const auto id = selectedMeetingId_;
        const auto revision = meetingRevision_;
        const auto closing = closing_;
        meetingReader_.setFuture(QtConcurrent::run([directory, id, revision, closing] {
            MeetingReadResult result; result.id = id; result.revision = revision;
            try { if (!closing->load()) result.meeting = readMeeting(directory, id); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
    }

    void browseMeetingScreens(qint64 timestamp) {
        if (timestamp <= 0) return;
        seekTime(timestamp);
        seekFromMeeting_ = true;
    }

    void describeFrame() {
        const auto& frame = selected_;
        recorded_->setText(QDateTime::fromMSecsSinceEpoch(frame.timestampMs).toString("ddd, MMM d · HH:mm:ss"));
        if (legacy_) indexState_->setText("Indexing status not recorded");
        else if (frame.ocrState == "pending") indexState_->setText("Saved · text pending");
        else if (frame.ocrState == "failed") indexState_->setText("Saved · indexing failed");
        else if (frame.ocrState == "disabled") indexState_->setText("Saved · indexing disabled");
        else indexState_->setText(frame.text.trimmed().isEmpty() ? "No text recognized" : QString());
        indexState_->setVisible(!indexState_->text().isEmpty());
        indexState_->setToolTip(frame.ocrError);
        timeline_->setMoment(frame.timestampMs);
        updateMatchReadout();
    }

    void openFrame(const FrameRecord& frame) {
        closeMeeting();
        const bool changed = selected_.id != frame.id;
        const bool keepImage = selected_.id == frame.id && selected_.available == frame.available;
        if (!keepImage) cancelSelectionOcr();
        if (changed) { ++selectionRevision_; dwellTimer_.stop(); }
        selected_ = frame;
        setProperty("selectedFrameId", frame.id);
        {
            const auto found = std::find_if(resultRows_.cbegin(), resultRows_.cend(), [this](const auto& row) { return row.key() == selected_.id; });
            // Keep a searched result as a return point while browsing nearby
            // screens. An initial, unfiltered preview has no such selection.
            if (found != resultRows_.cend() || completedQuery_.trimmed().isEmpty()) {
                const int row = found == resultRows_.cend() ? -1 : int(std::distance(resultRows_.cbegin(), found));
                if (results_->currentRow() != row) {
                    const QSignalBlocker blocker(results_);
                    results_->setCurrentRow(row);
                    if (results_->currentItem()) results_->scrollToItem(results_->currentItem());
                }
            }
        }
        describeFrame();
        startHighlights();
        updateNavigation(changed);
        updateIndexingActions();
        if (changed && frame.ocrState == "pending" && !requestedMoments_.contains(frame.id)) dwellTimer_.start();
        if (keepImage) return;
        setProperty("displayedFrameId", 0);
        evidence_->canvas()->setSelectionAvailable(false);
        evidence_->setHighlights({});
        // Keep the previous pixels until the new image is ready. Repainting an
        // empty themed canvas produces a conspicuous flash during short decodes.
        // displayedFrameId is cleared so copy and overlays cannot use stale pixels.
        if (frame.available) { mediaStatus_->clear(); mediaStatus_->hide(); }
        else {
            evidence_->setImage({}); mediaStatus_->show();
            mediaStatus_->setText("Recorded media is not available yet.");
        }
        if (!decoder_.isRunning()) startDecode();
    }

    void startDecode() {
        if (closing_->load()) return;
        if (selected_.id <= 0 || !selected_.available) {
            setProperty("mediaLoading", false);
            return;
        }
        const QString directory = directory_;
        const qint64 id = selected_.id;
        const auto closing = closing_;
        setProperty("mediaLoading", true);
        decoder_.setFuture(QtConcurrent::run([directory, id, closing] {
            DecodedFrame result;
            result.id = id; result.directory = directory;
            try { result.image = loadFrame(directory, id, [closing] { return closing->load(); }); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
    }

    void updateNavigation(bool reset = true) {
        if (reset) {
            previousFrame_.reset();
            nextFrame_.reset();
            previous_->setEnabled(false);
            next_->setEnabled(false);
        }
        if (!neighborReader_.isRunning()) startNeighborRead();
    }

    void startNeighborRead() {
        if (closing_->load() || selected_.id <= 0) return;
        const QString directory = directory_;
        const qint64 id = selected_.id;
        const auto closing = closing_;
        neighborReader_.setFuture(QtConcurrent::run([directory, id, closing] {
            TimelineNeighbors result;
            result.id = id; result.directory = directory;
            try {
                if (closing->load()) return result;
                result.previous = adjacentFrame(directory, id, -1);
                if (!closing->load()) result.next = adjacentFrame(directory, id, 1);
            } catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
    }

    QString workerCommand() const {
        const auto quote = [](QString value) { value.replace('\'', "'\\''"); return '\'' + value + '\''; };
        return quote(QCoreApplication::applicationFilePath()) + " index --dir " + quote(directory_) +
            " --follow --scheduler adaptive --ocr-cpu-percent 10 --ocr-max-wall-ms 60000";
    }

    void updateIndexingActions() {
        const bool busy = indexRequest_.isRunning();
        const bool serviceBusy = serviceRequest_.isRunning() || (sharedHistory_ && recordingRequest_.isRunning());
        const bool configured = sharedHistory_ || !servicePolicy_.isEmpty();
        const bool paused = service_.value("paused").toBool();
        const bool running = service_.value("running").toBool();
        const bool external = service_.value("external_worker").toBool();
        const bool fixed = servicePolicy_.value("scheduler").toString() == "fixed";
        const bool catchUpActive = catchUpUntil_ > QDateTime::currentMSecsSinceEpoch();
        QStringList counts;
        counts << QString("%1 / %2 searchable").arg(ready_).arg(totalFrames_);
        if (pending_ > 0) counts << QString("%1 pending").arg(pending_);
        if (failed_ > 0) counts << QString("%1 failed").arg(failed_);
        if (disabled_ > 0) counts << QString("%1 disabled").arg(disabled_);
        if (legacy_) counts << "Index status unavailable";
        status_->setText(counts.join("  ·  "));
        indexProgress_->setValue(totalFrames_ > 0 ? int(1000. * ready_ / totalFrames_) : 0);
        indexProgress_->setVisible(totalFrames_ > 0 && !legacy_);
        processMoment_->setVisible(!legacy_ && selected_.ocrState == "pending");
        catchUp_->setVisible(!legacy_ && pending_ > 0);
        processMoment_->setEnabled(!legacy_ && selected_.ocrState == "pending" && !busy);
        catchUp_->setEnabled(!legacy_ && pending_ > 0 && !busy && !catchUpActive && !paused && !fixed);
        catchUp_->setToolTip(fixed ? "This history uses a fixed CPU allowance; catch-up requires adaptive scheduling"
            : "Request two minutes of faster indexing when resources permit");
        copyWorkerCommand_->setVisible(pending_ > 0 && !indexerRunning_ && !configured &&
            service_.value("policy_error").toString().isEmpty());
        copyWorkerCommand_->setToolTip(workerCommand());
        serviceAction_->setVisible(configured && !legacy_);
        serviceAction_->setText(paused ? sharedHistory_ ? "Resume indexing" : "Resume" : running ? sharedHistory_ ? "Pause indexing" : "Pause" : "Start indexing");
        serviceAction_->setProperty("action", paused ? "resume" : running ? "pause" : "start");
        serviceAction_->setEnabled(!serviceBusy);
        stopService_->setVisible(!sharedHistory_ && !legacy_ && (running || service_.value("enabled").toBool()));
        stopService_->setEnabled(!serviceBusy);
        QStringList messages;
        if (serviceBusy) messages << "Updating background indexing…";
        else if (busy) messages << "Saving indexing request…";
        else if (!requestMessage_.isEmpty()) messages << requestMessage_;
        if (priorityPending_ > 0) messages << QString("%1 pending moments are prioritized.").arg(priorityPending_);
        if (catchUpActive) messages << "Catch-up requested until " + QDateTime::fromMSecsSinceEpoch(catchUpUntil_).toString("HH:mm:ss") + ".";
        priorityStatus_->setText(messages.join('\n'));
        priorityStatus_->setVisible(!messages.isEmpty());
        const QString state = service_.value("state").toString();
        QString activity;
        if (state == "error") {
            const QString problem = service_.value("error").toString();
            activity = "Indexing needs attention: " + (problem.isEmpty()
                ? service_.value("reason").toString("Check pending and failed moments.") : problem);
        }
        else if (configured && !running && !indexerRunning_ && !service_.value("enabled").toBool())
            activity = "Background indexing is stopped.";
        else if (paused) activity = external ? "Background service paused; another worker is still indexing."
            : service_.value("worker_running").toBool() ? "Pausing background indexing…"
            : "Paused. Resume to make waiting moments searchable.";
        else if (external) activity = "Another worker is indexing this history.";
        else if (state == "pressure") activity = "Working gently while the computer is busy.";
        else if (state == "requested") activity = "Catching up on requested moments.";
        else if (state == "idle" && pending_ > 0) activity = "Catching up while the computer is idle.";
        else if (running && pending_ == 0 && failed_ > 0) activity = "Failed moments need attention.";
        else if (running && pending_ == 0 && disabled_ > 0) activity = "Some moments have indexing disabled.";
        else if (running && pending_ == 0) activity = "Up to date.";
        else if (state == "waiting" && running) activity = "Waiting to start the next indexing pass.";
        else if (state == "unknown" && indexerRunning_)
            activity = service_.value("effective_cpu_percent").isDouble()
                ? "Working gently; activity signals are unavailable."
                : "Indexing; waiting for a fresh activity update.";
        else if (indexerRunning_) activity = "Processing in the background.";
        else if (running) activity = "Background indexing is starting.";
        else if (pending_ > 0 || configured) activity = "Background indexing is stopped.";
        QStringList details;
        const auto budget = service_.value("effective_cpu_percent");
        const qint64 policyAge = service_.value("policy_age_ms").toInteger(-1);
        if (budget.isDouble() && policyAge >= 0 && policyAge < 15000 && indexerRunning_)
            details << (budget.toDouble() == 0 ? "OCR has no pacing cap."
                : QString("CPU allowance: %1% of one core.").arg(budget.toDouble(), 0, 'g', 3));
        const auto resources = service_.value("worker_policy").toObject().value("resources").toObject();
        if (indexerRunning_ && policyAge >= 0 && policyAge < 15000) {
            if (resources.value("enforced").toBool())
                details << QString("Worker ceiling: %1% of one core · low priority")
                    .arg(resources.value("effective_cpu_percent").toDouble(), 0, 'g', 3);
            else if (resources.value("state").toString() == "unavailable")
                details << "Worker ceiling unavailable; OCR pacing and low priority remain active.";
        }
        if (sharedHistory_ && !indexerRunning_) {
            const auto last = service_.value("last_worker_resources").toObject();
            if (last.value("enforced").toBool()) details << QString("Last worker ceiling: %1% of one core; low priority.")
                .arg(last.value("effective_cpu_percent").toDouble(), 0, 'g', 3);
            else if (last.value("state").toString() == "unavailable") details << "Last worker ceiling unavailable; OCR pacing remained active.";
            else if (servicePolicy_.contains("ocr_cpu_ceiling_percent")) details << QString("Configured worker ceiling: %1% of one core; checked when indexing starts.")
                .arg(servicePolicy_.value("ocr_cpu_ceiling_percent").toDouble(), 0, 'g', 3);
        }
        const QString recovery = service_.value("recovery").toString();
        if (!recovery.isEmpty()) details << recovery;
        workerDetails_->setText(details.isEmpty() ? "CPU details appear when an indexing worker is available." : details.join('\n'));
        pendingAge_->setText(pending_ > 0 ? "Oldest waiting: " + elapsedDescription(oldestPendingMs_) : QString());
        pendingAge_->setVisible(pending_ > 0);
        workerHint_->setText(activity);
        workerHint_->setToolTip(service_.value("recovery").toString());
        workerHint_->setVisible(!activity.isEmpty());
        fitDetailsPanel();
    }

    void requestService(const QString& action) {
        if (sharedHistory_) { requestRecording(action == "pause" || action == "stop" ? "index-pause" : "index-resume"); return; }
        if (closing_->load() || serviceRequest_.isRunning() || (servicePolicy_.isEmpty() && action != "stop")) return;
        const QString directory = directory_;
        const auto closing = closing_;
        setProperty("serviceRequestInFlight", true);
        serviceRequest_.setFuture(QtConcurrent::run([directory, action, closing] {
            ServiceControlResult result;
            result.action = action;
            try { if (!closing->load()) result.status = controlIndexService(directory, action); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
        updateIndexingActions();
    }

    void requestMoment(bool automatic) {
        if (closing_->load() || !isVisible() || legacy_ || selected_.id <= 0 || selected_.ocrState != "pending") return;
        if (automatic && requestedMoments_.contains(selected_.id)) return;
        if (indexRequest_.isRunning()) return;
        dwellTimer_.stop();
        const QString directory = directory_;
        const qint64 id = selected_.id;
        const auto closing = closing_;
        setProperty("indexingRequestInFlight", true);
        indexRequest_.setFuture(QtConcurrent::run([directory, id, closing] {
            IndexingRequestResult result;
            result.frameId = id; result.directory = directory;
            try { if (!closing->load()) result.changed = requestIndexing(directory, id, 15); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
        updateIndexingActions();
    }

    void requestCatchUpNow() {
        if (closing_->load() || !catchUp_->isEnabled() || indexRequest_.isRunning()) return;
        const QString directory = directory_;
        const auto closing = closing_;
        setProperty("indexingRequestInFlight", true);
        indexRequest_.setFuture(QtConcurrent::run([directory, closing] {
            IndexingRequestResult result;
            result.directory = directory;
            result.catchUp = true;
            try { if (!closing->load()) result.changed = requestCatchUp(directory, 120); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
        updateIndexingActions();
    }

    void cancelSeek() {
        ++seekRevision_;
        seekPending_ = false;
        seekTimer_.stop();
    }

    void seekTime(qint64 timestamp) {
        seekFromMeeting_ = false;
        cancelSelectionOcr();
        seekPending_ = true;
        seekTarget_ = timestamp;
        ++seekRevision_;
        ++selectionRevision_;
        dwellTimer_.stop();
        seekTimer_.start();
    }

    void startSeek() {
        if (closing_->load() || seeker_.isRunning() || !seekPending_) return;
        seekPending_ = false;
        const QString directory = directory_;
        const auto revision = seekRevision_;
        const auto target = seekTarget_;
        const bool fromMeeting = seekFromMeeting_;
        const auto closing = closing_;
        seeker_.setFuture(QtConcurrent::run([directory, revision, target, closing, fromMeeting] {
            SeekResult result; result.revision = revision;
            try {
                if (!closing->load()) result.frame = fromMeeting ? screenNearMeetingStart(directory, target) : frameNearTimestamp(directory, target);
                if (fromMeeting && !result.frame) result.error = "No screen history is available near this meeting's start. You can still read its transcript.";
            }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
    }

    void startHighlights() {
        highlighted_ = {};
        evidence_->setHighlights({});
        setProperty("highlightCount", 0);
        if (closing_->load() || highlightReader_.isRunning() || selected_.id <= 0 || completedQuery_.trimmed().isEmpty()) return;
        const QString directory = directory_, query = completedQuery_;
        const qint64 id = selected_.id;
        const auto closing = closing_;
        highlightReader_.setFuture(QtConcurrent::run([directory, id, query, closing] {
            HighlightResult result; result.id = id; result.directory = directory; result.query = query;
            try { if (!closing->load()) result.matches = matchingTextLines(directory, id, query, SearchMode::PrefixLastToken); }
            catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
            return result;
        }));
    }

    void updateMatchReadout() {
        const qint64 key = selectedMeetingId_ ? -selectedMeetingId_ : selected_.id;
        const auto found = std::find_if(resultRows_.cbegin(), resultRows_.cend(), [key](const auto& row) { return row.key() == key; });
        const bool meetingList = completedQuery_.trimmed().isEmpty();
        const QString label = meetingList ? "meetings" : "matches";
        if (found != resultRows_.cend()) {
            const auto ordinal = pageOffset_ + std::distance(resultRows_.cbegin(), found) + 1;
            resultsHeading_->setText(QString("%1 / %2 %3").arg(ordinal).arg(totalMatches_).arg(label));
        } else resultsHeading_->setText(QString("%1 %2").arg(totalMatches_).arg(label));
        const qint64 ordinal = pageOffset_ + results_->currentRow();
        previousMatch_->setEnabled(totalMatches_ > 0 && ordinal > 0);
        nextMatch_->setEnabled(totalMatches_ > 0 && ordinal + 1 < totalMatches_);
    }

    void changePage(int direction) {
        cancelSelectionOcr();
        if (property("historyLoading").toBool() && !requestedHistory_.preserve) return;
        if (totalMatches_ <= 0) return;
        const auto target = pageOffset_ + direction * 100;
        if (target < 0 || target >= totalMatches_) return;
        search(false, direction < 0 ? 99 : 0, target);
    }

    void stepMatch(int direction) {
        cancelSelectionOcr();
        if (property("historyLoading").toBool() && !requestedHistory_.preserve) return;
        if (resultRows_.isEmpty()) return;
        const int target = results_->currentRow() + direction;
        if (target < 0 && pageOffset_ > 0) { changePage(-1); return; }
        if (target >= resultRows_.size() && pageOffset_ + resultRows_.size() < totalMatches_) { changePage(1); return; }
        results_->setCurrentRow(std::clamp(target, 0, int(resultRows_.size()) - 1));
        results_->scrollToItem(results_->currentItem());
        openResult();
    }

    void stepTime(int direction) {
        cancelSelectionOcr();
        cancelSeek();
        const auto neighbor = direction < 0 ? previousFrame_ : nextFrame_;
        if (neighbor) openFrame(*neighbor);
    }

    TimelineView* timeline_ = nullptr;
    TimelineOverview overview_;
    QWidget* details_ = nullptr;
    QScrollArea* detailsScroll_ = nullptr;
    QWidget* detailsContents_ = nullptr;
    QBoxLayout* panelColumns_ = nullptr;
    QWidget* help_ = nullptr;
    QWidget* matchReadout_ = nullptr;
    QPushButton* helpToggle_ = nullptr;
    QPushButton* previousMatch_ = nullptr;
    QPushButton* nextMatch_ = nullptr;
    QProgressBar* indexProgress_ = nullptr;
    qint64 ready_ = 0, totalFrames_ = 0, totalMatches_ = 0, pageOffset_ = 0;
    QPushButton* detailsToggle_ = nullptr;
    QTimer seekTimer_;
    quint64 seekRevision_ = 0;
    qint64 seekTarget_ = 0;
    bool seekPending_ = false;
    QFutureWatcher<SeekResult> seeker_;
    bool seekFromMeeting_ = false;
    QFutureWatcher<MeetingReadResult> meetingReader_;
    quint64 meetingRevision_ = 0;
    qint64 selectedMeetingId_ = 0;
    MeetingRecord selectedMeeting_;
    QFutureWatcher<HighlightResult> highlightReader_;
    HighlightResult highlighted_;
    QString directory_;
    bool sharedHistory_ = false;
    ViewerServiceHooks services_;
    QJsonObject recording_;
    QString recordingMessage_;
    QWidget* recordingPanel_ = nullptr;
    QPushButton *recordingAction_ = nullptr, *recordingStop_ = nullptr, *deleteRecent_ = nullptr;
    QLabel* recordingState_ = nullptr;
    QLabel* storageCapacity_ = nullptr;
    QLabel* storageDetails_ = nullptr;
    QFutureWatcher<ServiceControlResult> recordingRequest_;
    QFutureWatcher<SelectionResult> selectionOcr_;
    std::optional<SelectionRequest> pendingSelection_;
    std::shared_ptr<std::atomic_bool> selectionCancel_;
    bool selectionWorkerActive_ = false;
    quint64 selectionOcrRevision_ = 0, clipboardRevision_ = 0;
    std::shared_ptr<std::atomic_bool> closing_ = std::make_shared<std::atomic_bool>(false);
    QString completedQuery_;
    QString completedSource_ = "all";
    qint64 pending_ = 0, failed_ = 0, disabled_ = 0, priorityPending_ = 0, catchUpUntil_ = 0;
    quint64 historyGeneration_ = 0, selectionRevision_ = 0;
    bool legacy_ = false;
    bool indexerRunning_ = false;
    qint64 oldestPendingMs_ = 0;
    QJsonObject service_, servicePolicy_;
    QPointer<QDialog> settingsDialog_;
    QString requestMessage_;
    QSet<qint64> requestedMoments_;
    HistorySnapshot requestedHistory_;
    QLineEdit* query_ = nullptr;
    QComboBox* sourceFilter_ = nullptr;
    QListWidget* results_ = nullptr;
    QLabel* resultsHeading_ = nullptr;
    QLabel* recorded_ = nullptr;
    QLabel* indexState_ = nullptr;
    QLabel* copyStatus_ = nullptr;
    QLabel* mediaStatus_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* priorityStatus_ = nullptr;
    QLabel* workerHint_ = nullptr;
    QLabel* workerDetails_ = nullptr;
    QLabel* pendingAge_ = nullptr;
    EvidenceView* evidence_ = nullptr;
    QStackedWidget* content_ = nullptr;
    MeetingView* meetingView_ = nullptr;
    QPushButton* previous_ = nullptr;
    QPushButton* next_ = nullptr;
    QPushButton* processMoment_ = nullptr;
    QPushButton* catchUp_ = nullptr;
    QPushButton* copyWorkerCommand_ = nullptr;
    QPushButton* serviceAction_ = nullptr;
    QPushButton* stopService_ = nullptr;
    QTimer searchTimer_;
    QTimer refreshTimer_;
    QTimer dwellTimer_;
    QTimer copyNoticeTimer_;
    QFutureWatcher<DecodedFrame> decoder_;
    QFutureWatcher<HistorySnapshot> historyReader_;
    QFutureWatcher<TimelineNeighbors> neighborReader_;
    QFutureWatcher<IndexingRequestResult> indexRequest_;
    QFutureWatcher<ServiceControlResult> serviceRequest_;
    QVector<FrameRecord> matches_;
    QVector<RecallResult> resultRows_;
    std::optional<FrameRecord> previousFrame_;
    std::optional<FrameRecord> nextFrame_;
    FrameRecord selected_;
};

}  // namespace

std::unique_ptr<QWidget> createViewer(const QString& datasetDirectory, ViewerServiceHooks services) {
    return std::make_unique<Viewer>(datasetDirectory, std::move(services));
}

int showViewer(const QString& datasetDirectory, bool settings) {
    // A per-history endpoint lets a bar action open settings in the existing
    // viewer, including while its modal settings dialog is already open.
    const auto runtime = replayPaths().runtimeDirectory;
    struct stat metadata{};
    if (QFileInfo(runtime).isSymLink() || !QDir().mkpath(runtime) ||
        ::lstat(QFile::encodeName(runtime).constData(), &metadata) != 0 ||
        !S_ISDIR(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
        !QFile::setPermissions(runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        throw std::runtime_error("Viewer controls require a private runtime directory");
    const auto canonical = QFileInfo(datasetDirectory).canonicalFilePath();
    if (canonical.isEmpty()) throw std::runtime_error("History directory is unavailable");
    const auto key = QString::fromLatin1(QCryptographicHash::hash(canonical.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    const auto endpoint = runtime + "/view-" + key + ".sock";
    const auto summonExisting = [&] {
        QLocalSocket client;
        client.connectToServer(endpoint);
        if (!client.waitForConnected(250)) return false;
        client.write(settings ? "settings\n" : "open\n");
        if (!client.waitForBytesWritten(500) || !client.waitForReadyRead(1500) || client.readAll() != "ok\n")
            throw std::runtime_error("The existing Replay viewer did not respond");
        return true;
    };
    if (summonExisting()) return 0;
    QLockFile lease(runtime + "/view-" + key + ".lock");
    lease.setStaleLockTime(0);
    if (!lease.tryLock(1500)) {
        if (summonExisting()) return 0;
        throw std::runtime_error("This history's viewer is starting or unresponsive");
    }
    QLocalServer::removeServer(endpoint);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(endpoint)) throw std::runtime_error("Cannot open Replay viewer controls");
    auto viewer = std::make_unique<Viewer>(canonical, ViewerServiceHooks{});
    QObject::connect(&server, &QLocalServer::newConnection, viewer.get(), [&] {
        while (auto* client = server.nextPendingConnection()) {
            client->setParent(&server);
            client->setReadBufferSize(32);
            auto* deadline = new QTimer(client);
            deadline->setSingleShot(true);
            QObject::connect(deadline, &QTimer::timeout, client, &QLocalSocket::abort);
            QObject::connect(client, &QLocalSocket::disconnected, client, &QObject::deleteLater);
            deadline->start(1500);
            const auto receive = [client, target = viewer.get()] {
                if (!client->canReadLine()) return;
                const auto command = client->readLine(32);
                if (command != "open\n" && command != "settings\n") { client->abort(); return; }
                client->write("ok\n"); client->flush(); client->disconnectFromServer();
                QTimer::singleShot(0, target, [target, command] { target->summon(command == "settings\n"); });
            };
            QObject::connect(client, &QLocalSocket::readyRead, viewer.get(), receive);
            receive();
        }
    });
    viewer->show();
    if (settings) QTimer::singleShot(0, viewer.get(), [&] { viewer->summon(true); });
    return QApplication::exec();
}

}  // namespace replay
