#pragma once

#include "recorder.h"
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

struct sqlite3;
namespace replay {

struct MeetingRecord {
    qint64 id = 0;
    QString title;
    QString revision; // Import signature; changes when content, metadata or source path changes.
    qint64 startedAtMs = 0;
    bool timeKnown = false;
    qint64 durationSeconds = 0;
    QString sourceDirectory;
    QString manifestPath;
    QString transcript; // Plain text; populated only by readMeeting().
};
struct MeetingSearchResult {
    MeetingRecord meeting;
    int matchingPassages = 0;
    QStringList passages; // Bounded matching lines; filled only when requested.
};
struct MeetingSearchPage {
    QVector<MeetingSearchResult> results;
    qint64 totalMatches = 0;
    qint64 offset = 0;
};
struct MeetingTimelinePoint { qint64 id = 0; qint64 startedAtMs = 0; QString title; };

// Never creates a database/schema. An archive without imported meetings is empty.
MeetingSearchPage listMeetings(const QString &directory, int limit = 100, qint64 offset = 0);
// Optional recall filters. A nonzero bound keeps only meetings with a known
// start whose [start, start + duration] overlaps the inclusive window; meetings
// without a known time are excluded whenever a bound is set. passageLimit > 0
// returns up to that many matching transcript lines, each at most
// MeetingPassageChars characters, without exposing the transcript body.
struct MeetingSearchOptions { qint64 sinceMs = 0, untilMs = 0; int passageLimit = 0; };
constexpr int MeetingPageLimit = 100, MeetingPassageChars = 240;
MeetingSearchPage searchMeetings(const QString &directory, const QString &query,
                                int limit = 100, qint64 offset = 0,
                                SearchMode mode = SearchMode::PrefixLastToken,
                                const MeetingSearchOptions &options = {});
std::optional<MeetingRecord> readMeeting(const QString &directory, qint64 id);
std::optional<FrameRecord> screenNearMeetingStart(const QString &directory, qint64 timestampMs, qint64 toleranceMs = 10000);
QVector<MeetingTimelinePoint> meetingTimeline(const QString &directory, int limit = 1000);
// UTF-16 offsets for safe plain-text highlights. No HTML or Markdown execution.
QVector<QPair<int, int>> meetingMatchRanges(const QString &text, const QString &query,
                                           SearchMode mode = SearchMode::PrefixLastToken);

struct MeetingSyncResult {
    int scanned = 0, imported = 0, updated = 0, removed = 0;
    bool more = false, sourceAvailable = false, canceled = false, limited = false;
};
// Keep one importer per (archive, source) on its worker thread. Each call visits
// at most 32 source entries and reads at most 2 MiB of changed text. No audio is
// opened. Files must be unchanged across two passes before publication.
class MeetingImporter {
public:
    MeetingImporter(const QString &directory, const QString &sourceDirectory,
                    std::function<bool()> canceled = {},
                    quint64 maxDiskBytes = 0, quint64 minFreeBytes = 0);
    ~MeetingImporter();
    MeetingImporter(const MeetingImporter &) = delete;
    MeetingImporter &operator=(const MeetingImporter &) = delete;
    MeetingSyncResult sync(qint64 nowMs, qint64 expireBeforeMs);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Called inside the recorder's existing write transaction. Only acts if the
// meeting schema exists. Tombstones retain opaque identity, never transcript.
// Unknown/imported dates use first-import time for expiry and range deletion.
qint64 deleteMeetingsInRange(sqlite3 *db, qint64 fromInclusiveMs, qint64 toExclusiveMs,
                            int limit = 1000, bool retention = false);
// Removes at most one oldest imported transcript, recording its tombstone.
bool evictOldestMeeting(sqlite3 *db);

} // namespace replay
