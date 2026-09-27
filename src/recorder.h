#pragma once

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>
#include <functional>
#include <stdexcept>

namespace replay {

class OcrCancelled : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RollingStorageUnavailable : public std::runtime_error {
public:
    RollingStorageUnavailable(const QString &reason, bool retry) : std::runtime_error(reason.toStdString()), retryable(retry) {}
    bool retryable;
};

struct RecorderOptions {
    QString directory;
    QString codec = "webp";
    QString device = "/dev/dri/renderD128";
    double intervalSeconds = 2.0;
    int segmentFrames = 30;
    double segmentSeconds = 60.0;
    quint64 maxDiskBytes = 512ULL * 1024 * 1024;
    quint64 minFreeBytes = 256ULL * 1024 * 1024;
    bool ocr = true;
    QString ocrMode = "full"; // "incremental" and "regions" are opt-in experiments.
    double ocrCpuPercent = 0; // Zero disables cooperative OCR pacing.
    std::function<double()> ocrCpuPercentProvider;
    int ocrMaxWallMs = 10000;
    QString ocrDataPath; // Empty uses Tesseract's system model directory.
    int ocrMaxHeight = 0; // Zero keeps original OCR pixels; archives always do.
    std::function<bool()> stopRequested;
    bool deferredOcr = false;
    // Opt-in lossless history: requires deferred OCR and the WebP codec.
    // Pending limits do not gate archive admission; total disk/free-space limits do.
    bool archiveFirst = false;
    // Append to a private archive-first WebP history, creating it when absent.
    // The default remains a finite trial that refuses nonempty directories.
    bool resume = false;
    // The shared daemon rolls oldest observations out to admit new history.
    // Finite trials retain their explicit stop-at-limit behavior.
    bool rollingStorage = false;
    int maxPendingFrames = 16;
    quint64 maxPendingBytes = 64ULL * 1024 * 1024;
};

struct AddFrameResult {
    bool stored = false;
    bool duplicate = false;
    bool backlogFull = false;
    qint64 frameId = 0;
    double ocrMs = 0;
    double mediaMs = 0;
};

struct FrameRecord {
    qint64 id = 0;
    qint64 timestampMs = 0;
    qint64 lastTimestampMs = 0;
    qint64 observationCount = 0;
    qint64 frameIndex = 0;
    QString text;
    QString path;
    QString codec;
    int width = 0;
    int height = 0;
    bool available = false;
    bool archiveAvailable = false;
    QString originalPath;
    QString ocrState;
    QString ocrError;
};

struct TimelinePoint {
    qint64 id = 0;
    qint64 timestampMs = 0;
    QString ocrState;
};

struct TimelineOverview {
    qint64 firstTimestampMs = 0;
    qint64 lastTimestampMs = 0;
    qint64 totalFrames = 0;
    QVector<TimelinePoint> points;
};

enum class SearchMode { Exact, PrefixLastToken };
enum class SearchOrder { Chronological, Rank };

struct SearchPage {
    QVector<FrameRecord> frames;
    qint64 totalMatches = 0;
    qint64 offset = 0;
    int selectedRow = -1; // Set only when an anchor was found in this snapshot.
    TimelineOverview timeline;
};

struct TextMatches {
    QVector<QRect> boxes;
    QString text; // Matching recognized lines, once per line, in stored reading order.
    bool geometryAvailable = false;
};

struct IndexerOptions {
    QString directory;
    QString ocrMode = "incremental";
    double ocrCpuPercent = 0;
    std::function<double(bool)> cpuPercentProvider; // True for priority work or a live catch-up request.
    int ocrMaxWallMs = 10000;
    QString ocrDataPath;
    int ocrMaxHeight = 0;
    // Exact original-pixel reuse from committed full OCR in this dataset only.
    // Disable for an otherwise identical uncached comparison.
    bool ocrReuse = false;
    std::function<bool()> stopRequested;
};

struct IndexResult {
    bool processed = false;
    bool canceled = false;
    qint64 frameId = 0;
    QString state = "idle"; // "busy" means retry this call, not an empty queue.
    QString error;
    double ocrMs = 0;
};

// One process owns a dataset worker lock. Each call handles at most one original
// source image; pending jobs survive worker exit and cancellation.
class Indexer {
public:
    explicit Indexer(const IndexerOptions &options);
    ~Indexer();
    Indexer(const Indexer &) = delete;
    Indexer &operator=(const Indexer &) = delete;
    // Explicit recovery under the worker lock; never automatically retries a
    // failure in processNext's follow loop.
    int retryFailed();
    IndexResult processNext();
    QJsonObject statsJSON() const;
private:
    IndexResult processNextOnce();
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Single capture caller; addFrame archives one image before returning. Deferred
// OCR uses a separate Indexer and bounded disk sources, never a raw-image queue.
// All errors throw std::runtime_error; a new, private dataset is never overwritten.
class Recorder {
public:
    explicit Recorder(const RecorderOptions &options);
    ~Recorder();
    Recorder(const Recorder &) = delete;
    Recorder &operator=(const Recorder &) = delete;
    AddFrameResult addFrame(const QImage &image, qint64 timestampMs);
    void breakContinuity();
    void finish();
    QJsonObject statsJSON() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

struct HistoryMaintenanceResult {
    qint64 observationsRemoved = 0, framesRemoved = 0, filesRemoved = 0, gapsRemoved = 0;
    quint64 bytesReclaimed = 0, diskBytes = 0;
    bool more = false;
    bool busy = false; // Retry later; earlier committed batches remain valid.
};

struct HistorySpaceResult {
    bool ready = false;
    bool more = false; // A bounded cleanup batch made progress or met a busy writer.
    QString reason;
    HistoryMaintenanceResult maintenance;
};

// Admit at most one image, reclaiming a bounded oldest-first batch if needed.
// Never removes unrelated files. Rejects oversized admissions before eviction;
// reclamation includes owned media and incrementally shrinkable index pages.
HistorySpaceResult makeHistorySpace(const QString &directory, quint64 maxDiskBytes,
                                   quint64 minFreeBytes, quint64 additionalBytes,
                                   int maxObservations = 256, int maxFiles = 64);

// Each call deletes a bounded batch. Expiry uses observation capture times;
// originals shared by surviving observations remain available.
HistoryMaintenanceResult maintainHistory(const QString &directory, qint64 expireBeforeMs,
                                         int maxObservations = 1000, int maxFiles = 128);
HistoryMaintenanceResult deleteHistoryRange(const QString &directory, qint64 fromInclusiveMs, qint64 toExclusiveMs,
                                           int maxObservations = 1000, int maxFiles = 128);
QJsonObject historyUsage(const QString &directory);
void recordGap(const QString &directory, qint64 startMs, qint64 endMs, const QString &reason);

QVector<FrameRecord> listFrames(const QString &directory, int limit = 200, int offset = 0,
                                qint64 sinceMs = 0, qint64 untilMs = 0);
QVector<FrameRecord> searchFrames(const QString &directory, const QString &query, int limit = 100);
// Chronological pages and bounded markers cover the complete matching history.
// Prefix mode expands only the final token when it has at least three characters.
// A nonzero sinceMs/untilMs bounds the page by capture time; rank order pages
// by search relevance. Anchor resolution requires chronological order.
SearchPage searchFramePage(const QString &directory, const QString &query, int limit = 100,
                          qint64 offset = 0, SearchMode mode = SearchMode::Exact, int timelineLimit = 1000,
                          qint64 anchorFrameId = 0, qint64 sinceMs = 0, qint64 untilMs = 0,
                          SearchOrder order = SearchOrder::Chronological);
std::optional<qint64> searchFrameOffsetNearTimestamp(const QString &directory, const QString &query,
                                                   qint64 timestampMs, SearchMode mode = SearchMode::Exact);
std::optional<FrameRecord> adjacentFrame(const QString &directory, qint64 frameId, int direction);
std::optional<FrameRecord> frameById(const QString &directory, qint64 frameId);
// Bounded representative markers; timestamp seeking always selects real history.
TimelineOverview timelineOverview(const QString &directory, int limit = 1000);
std::optional<FrameRecord> frameNearTimestamp(const QString &directory, qint64 timestampMs);
// Original-image coordinates at line precision. Older indexed history has no
// stored boxes and returns empty; this read never performs OCR or a migration.
TextMatches matchingTextLines(const QString &directory, qint64 frameId, const QString &query,
                              SearchMode mode = SearchMode::Exact);
QVector<QRect> matchingTextRects(const QString &directory, qint64 frameId, const QString &query,
                               SearchMode mode = SearchMode::Exact);
QJsonObject indexingStatus(const QString &directory);
// Read-only recall helpers for the structured CLI. Time bounds are inclusive
// epoch milliseconds; zero disables a bound.
QJsonArray frameTextLines(const QString &directory, qint64 frameId); // Stored OCR lines with original-image boxes.
QVector<FrameRecord> framesNear(const QString &directory, qint64 frameId, int contextSeconds);
QJsonObject rangeCoverage(const QString &directory, qint64 sinceMs, qint64 untilMs);
// Reuse one read-only connection for repeated status polls. No statement or
// read transaction survives status(), so other writers can commit/checkpoint.
// Keep the reader on its owning thread.
class IndexStatusReader {
public:
    explicit IndexStatusReader(const QString &directory);
    ~IndexStatusReader();
    IndexStatusReader(const IndexStatusReader &) = delete;
    IndexStatusReader &operator=(const IndexStatusReader &) = delete;
    QJsonObject status();
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
// Requests are local, bounded and expire; already indexed frames are unchanged.
int requestIndexing(const QString &directory, qint64 frameId, int contextSeconds = 15);
int requestCatchUp(const QString &directory, int seconds = 120);
QImage loadFrame(const QString &directory, qint64 frameId, std::function<bool()> stopRequested = {});
QJsonArray listObservations(const QString &directory, int limit = 1000);

} // namespace replay
