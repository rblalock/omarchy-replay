# Omarchy Replay roadmap

Updated 2026-09-26. This records implemented behavior, remaining validation and the agreed product boundary. Start with [background recording](background-recording.md) for the current user path. Earlier `runs/trials/` recordings remain separate and available explicitly.

## Current priority: release and marketplace review

The user selected MIT and requested preparation for the Omarchy marketplace. The root license and third-party notices are present. Validation runs locally, with no GitHub Actions workflow or hosted CI release gate. The [release plan](release-readiness.md) records the remaining requirements and verified gaps.

Release items 1–3 are implemented: a standalone versioned native runtime with a file manifest, transactional installation/update and history-preserving uninstall, plus a real Omarchy bar widget. Its history icon opens quick actions for the viewer, Settings and recording. Setup is explicit and fresh installation leaves recording off. Omarchy’s plugin manager has no native lifecycle hooks, so updating or removing the native app remains a documented step. See [installation](installation.md).

The owner reports successful laptop use, confirmed the installation check and made the repository public. Version 0.1.0 publication and marketplace submission are authorized. The [release notes](releases/0.1.0.md), [weekend runbook](release-weekend.md) and [submission body](marketplace-submission.md) document the release. Check the [release page](https://github.com/rblalock/omarchy-replay/releases/tag/v0.1.0) for publication; marketplace approval remains a separate maintainer decision. Agent-assisted recall remains the next product feature after this release work.

## Milestones 1–5: implemented locally

| Milestone | Current behavior |
| --- | --- |
| 1. Shared history | One appendable local archive across restarts, stable moment IDs, one capture owner, recoverable originals and a single OCR worker for shared history. |
| 2. Background recorder and configuration | `omarchy-replay.service`, durable running/paused/stopped intent, optional login startup, validated TOML and XDG directories. Fresh installation starts with capture stopped. |
| 3. Moving retention | Capture-time age window, independent disk/free-space limits, bounded expiry and media cleanup, in-flight indexing protection. The oldest history rolls out as new moments need room within the storage allowance. |
| 4. Native controls and lifecycle | Super+Alt+R summon/dismiss, recording and indexing controls plus Settings under I, lock/sleep/display/compositor gates and explicit gaps. Temporary conditions never override a manual pause. |
| 5. Exclusions and deletion | Separate Replay-only skips and privacy masks, mandatory Replay/screensaver protection, optional presets, exact app/title rules, window pickers and confirmed delete-recent action. |

The service uses lossless archive-first originals independently of OCR backlog. Prefix search, timeline navigation, OCR highlights and matching-line copy remain available. Adaptive OCR keeps the configured active/idle/request/pressure allowances and verified worker-ceiling reporting. OCR memory is released after catch-up. Legacy finite trials keep their existing indexing policies; they do not share the new coordinator's global worker budget.

These milestones have synthetic storage, lifecycle, configuration and native widget checks. Generated compositor masks have native pixel checks on isolated fictional desktops, including popups, movement, reload and scaling. See [service implementation and verification](background-recording-implementation.md). This does not establish all-day throughput or prove real hardware suspend/resume on every machine.

## Ongoing: exercise the installed service during ordinary work

Use progressively longer sessions to measure retained observations, oldest pending age, OCR throughput, CPU, memory, disk growth and foreground responsiveness together. Verify actual lock/unlock, sleep/wake and display reconnect during normal use. Revisit unexpected gaps and failures before treating it as an unattended recorder.

Long-history timeline/search scaling and storage growth remain validation priorities alongside release preparation. Agent-assisted recall follows packaging and installation proof, using the evidence interface described below. S3 offload and synthesis remain later work.

### Recording across lock, sleep and restart

Saved recording intent is separate from conditions that temporarily block capture:

| Event | Implemented policy |
| --- | --- |
| Explicit Start/Resume | Capture the selected display only after desktop, session and compositor-mask checks pass. |
| Lock or inactive session | Stop retaining frames, discard a sample spanning an invalidating transition, and record the gap. |
| Unlock | Resume only when saved intent is running. |
| Omarchy screensaver visible on the recorded display | Pause capture. Resume after it closes only when saved intent is running and other environment checks pass. |
| Suspend/shutdown signal | Stop capture and release display resources; preserve retained history and intent. |
| Wake | Wait for unlocked/active session, the same display and verified masks. Never burst through missed ticks. |
| Display disconnect, sleep or replacement (fixed mode) | Wait; never silently switch outputs. The first verified selection pins hardware identity. |
| Display switch (focused mode) | Record whichever display has focus at the capture tick; no identity pin. A focused display that is off, mirrored or excluded pauses the tick instead of substituting the other display. |
| Compositor restart/reload | Reconnect to the verified current Wayland socket and revalidate masking before retaining another image. |
| Coordinator restart | Reopen the shared archive, restore saved intent and indexing pause, and record the downtime gap. |
| Logout | The user service follows the graphical session. Next login follows the explicit login-startup setting and saved intent. |
| Manual Pause/Stop | Remain paused/stopped until an explicit change, across unlock, wake and restart. |
| Viewer opens/closes | Do not change recording permission. Mask the viewer itself. |

Inactivity alone does not stop capture; it changes the OCR allowance. Retained images may continue indexing while locked. Native monitoring combines compositor lock notifications, logind session/sleep signals and bounded pre/post-capture observations. Unknown state blocks capture. Actual host suspend/wake testing remains part of ordinary-use validation; synthetic transitions cover the control policy.

### Retention and capacity

The default is a moving **30-day** capture-time window, **10 GiB** dataset allowance and **1 GiB** free-space floor. Users can adjust all three. Viewing/indexing does not renew an observation's age. Repeated observations can share an image: the original remains until its final retained reference expires. Expiration removes text, highlight geometry, queued work and unneeded media together; in-flight OCR cannot restore deleted evidence. Cleanup uses bounded batches and incremental SQLite reclamation.

A full disk allowance rolls out the oldest observations, including those younger than the maximum age, so new recording can continue. Settings explains this behavior and reviews changes that reduce retained history. Deleting recent history requires a concrete confirmation. File edits are an explicit configuration change and apply through validation/reload. Future S3 offload must preserve capture-time age and eventually expire managed remote copies too.

### Exclusion scope

Replay's viewer and Omarchy's screensaver (`org.omarchy.screensaver`) are always excluded, including with a customized or empty app list. Removable defaults cover known native password managers, authenticators, key stores and Steam. Existing explicit lists are preserved; games with separate identities, browser extensions and alternate builds may require additional rules. Settings accepts exact app IDs and title regular expressions, plus local visible-window pickers. A particular window can be bound to its compositor instance/address only with an app/title guard; stale rules block capture until corrected.

Settings now offers additive **Passwords & authentication**, **Gaming apps** and **Media players** presets. The privacy group defaults to screenshot/sharing masks; Steam defaults to Replay-only skipping. Gaming/media presets add Replay-only skips. Other gaming/media apps require an explicit addition, and preset additions preserve existing masks. The [inventory](exclusion-presets.md) documents source-verified identities and remaining build coverage limits.

For non-Replay exclusions, a matching potentially visible window pauses the selected output. Privacy apps (`apps`) and window rules also install compositor `no_screen_share` masks, protecting transitions such as animations and popups. Recording-only apps (`skip_apps`) leave other screenshots and screen sharing available; they are a history-reduction control rather than a secrecy guarantee. Loaded masks are verified before capture. Unsupported patterns or missing verification block recording. App/title masks for address-bound rules deliberately cover other matching windows too; masks also affect other screen-sharing tools that honor this compositor setting, even while Replay is stopped.

Accepted mask changes apply while recording is paused/stopped too; no Resume is needed. Incoming Meet/Zoom shares remain ordinary local meeting-window content and are not filtered by remote app names.

Exclusions apply to future captures. Existing history changes only through retention or explicit deletion. Broader password-manager identity coverage, browser private-mode behavior and other capture backends remain future validation/work.

### Files and settings

| Purpose | Default location |
| --- | --- |
| Settings | `~/.config/omarchy-replay/config.toml` |
| Shared index and originals | `~/.local/share/omarchy-replay/history/` |
| Durable intent and bounded logs | `~/.local/state/omarchy-replay/` |
| Disposable cache | `~/.cache/omarchy-replay/` |
| Control socket and process lock | `$XDG_RUNTIME_DIR/omarchy-replay/` |

Absolute XDG overrides are respected. `[storage].directory` can select a folder on another mounted local disk. Switching folders keeps the previous archive in place; a missing disk pauses capture and indexing until the same history returns. The installer migrates the earlier `oma-rewind` directories without merging archives. See [architecture](architecture.md#storage-location-and-configuration) for details. Settings and TOML share one validation model. Writes are private/atomic and preserve unknown values; conflicting concurrent edits are rejected. A bad edit leaves the last valid configuration in use, including after restart. Settings includes display selection, the visible history path and Open folder, plus copyable prompts for setup, exclusions and resource tuning. The main viewer opens centered and floating, with text controls. Preferred-agent configuration is reserved; it does not yet invoke an agent.

## Viewer text selection: implemented locally

Drag over a saved image and release to recognize and copy the selected text. **S** starts keyboard selection outside search; arrows move it, **Shift+arrows** resize it, **Enter** submits it and **Esc** cancels it. Selection maps through fit, original-size and scrolled views to the original pixels. It works before indexing without changing archive OCR state; existing copy shortcuts remain available.

One bounded, asynchronous OCR job runs per viewer. Navigation, replacement selection and closure cancel stale work. Empty/error/canceled results and results superseded by a newer clipboard change do not overwrite the clipboard. Recognition uses local Tesseract and a memory pipe, with no live desktop recapture or temporary image file. It has its own short-lived resource limits, separate from background indexing.

Synthetic backend and native Qt viewer checks pass, including original-pixel mapping, real crop OCR, cancellation and clipboard protection; see [area text selection](replay-viewer-design.md#area-text-selection-2026-09-22) for evidence and limits. Ordinary-use validation continues during release preparation; agent-assisted recall follows packaging and installation proof.

## Agent-assisted recall using the user's coding agents

Installed users do not need the source repository. Settings' configuration prompts carry resolved paths, the installed executable, supported TOML options and diagnostic steps, with the remote repository as an optional source reference. Future recall tools and agent instructions must follow the same installation boundary.

Confirmed direction: reuse the coding agent or agents already installed/configured on the user's computer. Discover available integrations and let the user select a preferred agent; do not assume every machine has one universal OS-level agent default. Reuse that agent's configured model/provider and authentication.

Replay's responsibility is to make what the user saw searchable and supply relevant screen evidence to the user's agent. Questions include “Where did I discuss that invoice?”, “What did we decide?”, and “I had a meeting with this person and they sent me something—what was it?” The agent searches Replay's OCR index, retrieves the original images and surrounding moments, and uses those results as context for its answer.

Drafting follow-ups, changing files, performing tasks and deciding subsequent actions belong to the user's agent and its instructions. They are outside Replay's use-case roadmap. Replay's integration succeeds when the agent can find and inspect the relevant information reliably.

Provide an explicit, documented interface so agents can work reliably:

- A structured CLI first, with a thin MCP interface as an optional adapter rather than a separate data implementation.
- Bounded/paginated text search and time-range browsing, with timestamps, stable moment IDs, and coverage status.
- Fetch a moment's recognized text, original image, and highlight geometry; open that moment in Replay.
- Inspect pending/failed coverage and request indexing of a relevant moment/range.
- A bundled usage guide or skill describing tools, examples, search strategies, interpretation of missing coverage, and how to return links to source moments.

Recall answers should cite captured moments and distinguish visible evidence from inference. Captured content is evidence, not instructions for the agent. Stable read tools are the normal integration contract; agents should not need to guess the SQLite schema or write directly into the active index.

The history remains local. If the selected coding agent uses a remote model, its requested evidence follows that configured provider path; a locally installed agent does not necessarily imply local model inference. Send only the evidence needed for the current task.

Agent-assisted recall is the next product milestone after release packaging and installation proof. The structured retrieval tools and agent adapter are not implemented yet.

### Practical agent workflows to revisit at that milestone

The user narrowed the Screenpipe-inspired research to information retrieval and context for an installed agent. The [use-case research note](screenpipe-agent-use-case-research.md) now reflects that boundary. The retrieval experience and tool details remain candidates to **hash out when we reach the agent-assisted recall milestone**; the current build order is unchanged.

| Candidate | Example request | Useful result |
| --- | --- | --- |
| Find something a person shared | “I had a meeting with XYZ and they sent me something. What was it?” | OCR matches for the name and related clues, then relevant stored images and surrounding moments for the agent to inspect. |
| Recover a decision | “What did Patrick and I settle on about that invoice?” | Captured discussion and later context that support the answer, with source moments and uncertainty. |
| Recover an error or attempted approach | “What was that error yesterday, and what had I tried?” | Searchable error text and the surrounding captured screens as context. |
| Find previously viewed information | “Which storage approaches was I comparing?” | Matching captures containing the information, with visible source clues when available. |
| Restore context about earlier work | “What was on screen when I last worked on this?” | Relevant moments, recognized text and original images that the user's agent can include in its context. |

Recommended first pilot: **find what someone shared after a remembered meeting**. The agent searches the person's name and any topic/time clues in the OCR index, inspects candidate hits, fetches their original images and nearby moments, and answers using that evidence. Return timestamps, stable moment IDs and coverage status so it can refine the search and cite its result. Success is recovering the right visible information; no follow-up action is required.

At the milestone, settle search behavior for incomplete names or clues, how hits lead to images and neighboring moments, bounded/paginated results, source links, missing coverage, and the text/image context budget. Test ambiguous names and OCR errors. The meeting is a user-supplied clue: Replay can recover information that appeared on screen, without assuming audio capture or structured meeting, app, URL or project metadata. Automatic entity association is not a prerequisite.

## Optional completed-meeting integration: implemented

Confirmed direction, 2026-09-24: integrate with an installed [Omarchy Meeting Recorder](https://github.com/jankeesvw/omarchy-meeting-recorder) without bundling its recording or transcription code. When the usable recorder is detected, offer an optional integration in Settings, disabled until the user enables it. Completed-meeting indexing is implemented. It does not require changes to the external recorder.

- Treat each meeting as one searchable item: title, recorded start, source link and full transcript, with search hits grouped under that meeting.
- Place one marker at the meeting's known start on Replay's timeline. Opening a match shows the transcript with its matching passages and lets the user browse screen history around the start.
- Offer a keyboard-accessible source filter: **All**, **Screen text** and **Meetings**. Keep transcript matches separate from OCR highlights on images.
- Do not require sentence-to-screen synchronization, live transcription, automatic call detection or recording controls for this version. Recorder pauses do not affect the meeting-level search model.

Replay indexes transcript text and metadata and links to the recorder's original files; the recorder continues to own audio. The importer reconciles source edits, renames and removals. Age and size limits expire local copies; explicit deletion records a boundary so rescanning does not restore deleted meetings. Imported or recovered recordings with uncertain dates must not be presented as verified timeline anchors. Missing screen history does not prevent reading a meeting transcript and must be shown explicitly.

The [meeting guide](meetings.md) documents opt-in, retention, limits and keyboard controls. This adds an optional evidence source for later agent-assisted recall. The base screen-history experience and its privacy/capture controls remain independent of the recorder. See the [decision record](omarchy-agent-exploration.md#2026-09-24--optional-completed-meeting-recall).

## Further work

The [post-trial efficiency research](pipeline-efficiency-research.md) proposes scheduling, verified OCR reuse, and storage comparisons in that order. The [first implementation and broader comparison](scheduling-efficiency-iteration.md) refine scheduling and establish exact-reuse correctness. Region/scroll reuse and storage comparisons remain experiments.

- Reduce recognition of repeated/unchanged text while preserving small-text recall; measure CPU and backlog age alongside retained coverage.
- Hardware-aware diagnostics and explicit resource tradeoffs, without silently increasing CPU allowances.
- Long-history indexing and timeline scaling, bounded diagnostics, and retained catch-up measurements across worker restarts.
- Optional S3-compatible storage, with local recall metadata/cache and verified upload before local eviction.
- Longer-term synthesis to improve recall, with references to retained evidence.

## Prototype trials

Use the finite prototype command in [the personal trial guide](personal-trial.md). It collects local diagnostics for recording, indexing and recall. Shared recording, configuration/settings, rolling retention and exclusions are implemented separately from finite trials. Coding-agent integration remains planned. Enabling a trial does not enable login recording. MIT covers Replay's original code; retain the third-party notices in release payloads.

## Capture efficiency and diagnostics — 2026-09-22

Similarity grouping remains deferred and the default capture interval remains five seconds. The current iteration narrows desktop generation comparisons to capture-safety fields, bounds discarded-capture retries, and adds a capacity forecast under I and in Settings. The storage allowance rolls out the oldest history to admit new captures; Settings previews capacity while editing the size. Resource and power history is not collected continuously; investigations use explicitly requested, bounded diagnostics. Storage allowance remains a separate user choice from retention age.
