# Meeting transcripts

Replay can index completed recordings from Omarchy Meeting Recorder, a separate Omarchy plugin. This is optional and off by default. The recorder handles audio, transcription and playback; Replay stores a searchable text copy and a link to the original meeting. No recorder code or speech model is bundled with Replay.

## Enable

1. Install Omarchy Meeting Recorder separately using its [installation instructions and documentation](https://github.com/jankeesvw/omarchy-meeting-recorder).
2. Open Replay's **Settings → Meetings**. This tab appears when `omarchy-meeting-recorder` is available on the executable search path. It remains available if an enabled integration loses its dependency, so you can disable it.
3. Enable **Include meeting transcripts** and confirm the meetings folder. Its default is `~/Documents/Meetings`.
4. Save. Existing eligible meetings and newly completed transcripts are checked automatically, normally within about a minute. The coordinator must be running and indexing must be unpaused.

Equivalent TOML in `~/.config/omarchy-replay/config.toml`:

```toml
[meetings]
enabled = true
directory = "" # Empty uses ~/Documents/Meetings; otherwise a clean absolute path.
```

Use the installed executable's `daemon reload` after editing TOML. A running coordinator preserves screen-recording intent. If it is offline, reload starts it with capture held; use this only when ready to run background indexing. Discovering the recorder, opening Replay or enabling transcript indexing never starts audio recording. Disabling integration stops new imports; it leaves previously indexed meetings available until deletion or expiration.

## Search and browse

**All** groups meeting results before screen matches. **Screen text** uses the existing OCR search. **Meetings** searches titles and transcripts; an empty query lists retained meetings. Each meeting appears once, with matching transcript lines counted as passages. The last search word supports prefix matching after three characters, just like screen search.

Selecting a meeting opens it immediately. **Up/Down** or **J/K** moves between results. **[ / ]** or **Alt+Up/Alt+Down** moves between passages inside the transcript. **Ctrl+C** copies selected text, matching passages, or the transcript when there is no query. **Ctrl+Shift+C** and **Copy transcript** copy the full transcript. **Alt+S** focuses the source filter; **Esc** leaves the focused control before closing Replay.

Known meeting starts appear as small square markers above the screen timeline. **Browse screens** looks for retained observations within ten seconds of that start and checks known capture/deletion gaps. If none exists, Replay keeps the transcript open and explains that screen history is unavailable. It never pairs speech with an arbitrarily distant screenshot. **Open recording** opens the source folder in Meeting Recorder when the original files and app remain available.

The anchor is the recorder's supplied start, not sentence-level synchronization. Audio pauses and device gaps are not reconstructed. Imported recordings and manifests without a known start remain searchable but have no timeline marker; their source date is not treated as a verified meeting time. Speaker labels come from the transcript, not Replay's inference.

## Storage and source changes

Local transcript copies live in the history database with separate full-text indexing. They are never inserted into screen OCR or used to draw highlights on images. Replay does not copy audio or write to the source meeting folder.

- Age limits use a known meeting start; unknown dates use first import time. Opening a meeting does not renew its age.
- Size pressure rolls out older meeting text alongside screen history. New imports respect the archive allowance and free-space reserve.
- **Delete recent** removes meetings whose start or first-import date falls in the requested interval. It removes the whole indexed meeting, rather than individual sentences. A meeting starting before the interval is unaffected even if the call continued into it.
- Deleted and expired meetings are not restored by a later folder scan. The database keeps opaque deletion identifiers and time boundaries, not their transcript text.
- Source edits and folder renames update the existing imported meeting. Source removal is reconciled after a complete scan. An unavailable root folder preserves cached transcripts until the source returns or retention expires them.

Replay's deletion never removes Meeting Recorder's original audio or transcripts. Conversely, turning integration off is not a request to delete Replay's existing copies.

## Limits and diagnostics

The importer checks immediate subfolders containing one `.meeting-recorder` JSON manifest and `transcript.md`. It requires stable files across two passes and at least one second since the last write. Symlinked sources, ambiguous manifests and unsafe filesystem entries are skipped; choose the real absolute folder path. It reads no audio.

One separate, low-priority importer visits at most 32 entries and reads at most 2 MiB of changed text per batch. Between reconciliations it sleeps for a minute. Limits are 10,000 source entries, 512 KiB per transcript, 64 KiB per manifest, 5,000 imported meetings and 64 MiB of indexed source text, in addition to Replay's storage allowance. These are implementation bounds, not configurable recording limits. Indexing pause also pauses meeting imports.

The installed executable's `daemon status` includes a `meetings` object with dependency availability, enabled/paused state, source location, imported count, synchronization counters, limits and worker errors. It contains no transcript text. The CLI `search` command searches screen OCR only. For agents, `recall --source meetings|all` searches imported meetings and returns bounded matching passages, and `recall --meeting-id ID` fetches one transcript; see the [agent guide](agent-guide.md).

The adapter was developed against recorder commit `a219d25590388c05294841354b2534c7854bbd56`. It uses local files and does not require a live status stream, upstream hooks, or automatic call detection.
