# Recording and configuration

Omarchy Replay keeps one selected archive across recording sessions. The viewer reads it; a separate coordinator manages capture, indexing and expiration. Closing the viewer leaves background work unchanged.

## Open an installed app

Open **Omarchy Replay** from the app launcher, or use **Super+Alt+R** when the Replay shortcut is installed. The viewer opens as a large centered floating window. Press **Esc** to leave search, then **I** to open Controls. Choose **Settings**, review your setup, then select **Start recording**.

Installed use does not require a source checkout. For terminal commands in this guide, set `replay_bin` to the exact executable path included in a Settings copy prompt:

```bash
replay_bin='/absolute/path/from-the-Replay-prompt'
```

If needed, inspect `systemctl --user show omarchy-replay.service -p ExecStart` to find the installed executable. Do not assume a global CLI name. The [agent guide](agent-guide.md#establish-the-current-setup) describes discovery and diagnosis.

## Settings

Settings groups recording, resources and exclusions, with an optional Meetings tab when Meeting Recorder is detected or the integration is already enabled. All controls support native keyboard navigation; Save applies changes and Cancel leaves the file unchanged.

Each tab groups related fields and scrolls in smaller windows. **Details** expands the longer explanations. Use Tab to reach a control and Space to toggle it; Save and Cancel stay outside the scrolling area.

| Tab | Settings |
| --- | --- |
| Recording | Display, capture interval, history folder, retention, disk limits and login startup. |
| Resources | Active, idle, requested and pressure CPU allowances, idle delay and worker ceiling. |
| Exclusions | Apps and window rules that should stay out of future captures. |
| Meetings (optional) | Enable completed-transcript imports and choose Meeting Recorder's source folder. |

The Display dropdown lists detected connectors with their model and resolution, plus **Focused display**, which records whichever display has focus at each capture. A saved disconnected display remains visible as unavailable. In fixed mode Replay pins the selected hardware identity and waits if it disappears; deliberately choosing a different display clears the old identity so Replay can verify the new selection. Focused mode pins no identity: every connected display is eligible once it has focus, and the focused display must still pass the usual off, mirror, lock, sleep and exclusion checks. Privacy note: in focused mode any display that receives focus is recorded, including a projector or TV — choose a fixed display or add exclusions when that is not wanted.

**Copy setup prompt**, **Copy resources prompt** and **Copy exclusions prompt** prepare instructions for your coding agent. Paste the prompt into the agent you use, then describe your requested change. Prompts include the installed executable, resolved local paths, supported TOML options, diagnostic commands and a link to the remote source repository. They work without a local checkout and do not include captured OCR, screenshots or window titles. Copying a prompt does not launch an agent or apply settings. Values still being edited can differ from the saved TOML; the prompt tells the agent to check the file.

## Recording and indexing controls

| Control | Effect |
| --- | --- |
| Start recording / Resume recording | Permit new captures after desktop and storage checks pass. |
| Pause recording | Save a manual pause across restarts; indexing can continue. |
| Stop recording | Save stopped capture intent; history, indexing and maintenance remain available. |
| Pause indexing / Resume indexing | Control screen OCR and optional meeting imports; pending originals stay retained. |
| Close the viewer | Leave recording and indexing choices unchanged. |
| Delete recent… | Review and confirm permanent deletion of an interval. |

The **I** panel separates Recording and Search index. It shows capture state, storage usage, searchable/pending counts and the oldest waiting age. **Storage details** expands the forecast explanation; **Processing details** shows CPU allowances and worker-limit availability. The sections sit side by side in wide windows and stack with scrolling in smaller ones.

Capture waits while locked, asleep, inactive, disconnected from the selected display (in focused mode: while no single display has focus), blocked by an exclusion or unable to verify its environment. It resumes after a temporary block only when saved intent is running. Wake and unlock never override a manual pause or stop.

**Start Replay at login** starts the installed coordinator with saved intent. It does not turn a stopped or paused recorder into a running one.

## Optional meeting transcripts

If [Omarchy Meeting Recorder](https://github.com/jankeesvw/omarchy-meeting-recorder) is installed, **Settings → Meetings** offers **Include meeting transcripts**, off by default. Its source folder defaults to `~/Documents/Meetings`. Enabling imports leaves screen-recording intent unchanged and never starts audio recording or transcription. The external recorder continues owning its audio and original files.

Completed transcripts become searchable in Replay, grouped once per meeting. Use **All / Screen text / Meetings** to choose a source. Known meeting starts appear on the timeline; imported recordings and unknown dates remain searchable without a marker. Selecting a transcript passage does not claim that its words occurred at a specific screenshot. See the [meeting guide](meetings.md) for keyboard navigation, source updates and current limits.

The coordinator must be running and indexing unpaused for new imports. A separate low-priority process checks stable source files, usually within about a minute. Disabling integration stops new imports and preserves existing copies. Removing the optional app also stops imports without invalidating other Replay settings.

## Storage

Fresh defaults are one capture every **5 seconds**, **30 days** of retained history, a **10 GiB** archive allowance and a **1 GiB** free-space reserve.

Settings shows the full history path. **Open folder** opens that location. **Choose folder…** selects another existing local folder, including a folder on a second disk. **Use default** returns to the XDG data location.

A custom folder must be owned by your user and either empty or compatible Replay history. Use an absolute path without a trailing slash. Network filesystems and a folder that is itself a symbolic link are not supported. The selected disk must remain available; Replay blocks recording and indexing if it is missing or replaced. It does not write into a fallback folder on the main disk.

Saving a folder change requires a review. Switching does not copy, merge or delete the previous archive. Retention applies only to the selected archive. A pending deletion must finish before switching folders. After the coordinator accepts the change, the viewer follows the new archive; if storage is unavailable, it keeps the prior view until recovery. Existing trial archives remain separate.

The retention window moves forward with time. Maintenance removes expired observations and OCR, then removes originals that have no surviving references. Repeated observations can share an image. Cleanup runs in bounded batches, so disk reclamation may take several passes.

Replay keeps the newest history that fits the disk allowance and removes anything older than the retention age. When new moments need room, it deletes the oldest observations and their unneeded images, OCR text and queued work. The free-space reserve can require earlier cleanup. Recording continues after room is made; it waits only when cleanup is still working or safe reclamation cannot make enough space. An OCR backlog does not reject new captures.

Imported meeting text shares these storage limits. Its age is the known meeting start, or first import time for an unknown date. **Delete recent…** removes a whole imported meeting when that anchor falls inside the requested interval; a meeting that started earlier is unaffected even if it continued into the interval. Replay never deletes the external recorder's audio or transcripts, and removed Replay copies do not return on the next scan.

Settings reviews changes that shorten retention, reduce the disk allowance or increase the free-space reserve, because they can delete older history. Routine rollover needs no confirmation. Direct TOML edits apply without that dialog. **Delete recent…** has a separate confirmation. Neither operation promises forensic erasure from backups, filesystem snapshots or SSD media.

## Resources

The defaults allow OCR **40% of one CPU core** during activity, **50%** after 60 seconds idle or for requested work, and **10%** under sustained contention. A separate **60% whole-worker ceiling** is requested and verified when the host supports it. The panel distinguishes the requested value from actual enforcement.

These values balance foreground work and indexing delay. Display resolution, changing pixels, text layout and CPU speed all affect throughput. Start with the defaults. If lag keeps growing or the desktop feels slower, use **Copy resources prompt** to have your agent inspect local status and make a measured adjustment. See [CPU scheduling](architecture.md#cpu-scheduling) for the policy and its limits.

Those CPU settings apply to screen OCR. The optional meeting importer uses bounded batches and low priority in a separate process; it is outside OCR pacing and the OCR worker ceiling. It reads completed text only and has no speech model. The external recorder controls the resources used for transcription.

The **I** panel and Recording Settings show current usage and the estimated history the whole allowance can hold. The summary uses calendar days when available, otherwise active recording hours. Expand the storage details for both estimates and the space needed for the chosen age window. Changing the size previews capacity before saving.

Calendar estimates require at least seven retained days. A small allowance may never retain a full week; the active-hours estimate still works. Estimates use observed usage, with no assumed workday length. Changing the display, the display mode, capture interval or folder starts a fresh usage sample. The active-hours estimate returns after five recorded minutes. Calendar estimates also wait for older observations to leave the archive.

## Exclusions

Replay's own window and Omarchy's screensaver (`org.omarchy.screensaver`) are always excluded, even with a customized or empty app list. Replay's window is masked. The screensaver pauses capture while visible on the recorded display; closing it resumes capture only if recording was running and the session, display and other checks pass. Manual Pause/Stop stays in effect. OCR can continue while the computer is awake.

Settings separates two kinds of exclusions:

| Control | Effect |
| --- | --- |
| **Skip in Replay** (`skip_apps`) | Pause Replay while a matching app is visible on the recorded display. Ordinary screenshots and screen sharing remain available. Use this to reduce unwanted history, not to protect secrets. |
| **Hide from screenshots and sharing** (`apps`, plus window rules) | Pause Replay and mask the matching window through the compositor. These masks also affect other capture tools, even while Replay is stopped. Use this for sensitive content. |

Both lists match exact current or initial app identifiers, including unfocused or overlapping windows. If an app appears in both, the stronger masking rule still applies. Remove it from both lists to allow Replay to record it.

Fresh privacy defaults include known native identities for 1Password, Bitwarden, KeePassXC, Proton Pass, Enpass, QtPass, GNOME Secrets, GNOME Authenticator, OTPClient, Yubico Authenticator and Seahorse. Steam is skipped in Replay by default. These entries are removable; Replay and the screensaver remain mandatory. See the [preset inventory](exclusion-presets.md) for exact IDs and build limitations.

Under **Settings → Exclusions**, choose a preset, **Add preset**, then **Save**. Passwords & authentication adds privacy masks; Gaming apps and Media players add Replay-only skips. Presets preserve existing entries, including any older masks. To change an existing masked app to a skip, move its identifier between the lists and Save. Cancel leaves the file unchanged.

**Choose visible app…** fills the Skip in Replay list from current desktop identifiers. **Choose visible window…** creates a privacy rule. For more specific rules, use a title pattern or **Copy exclusions prompt** and describe the intended behavior to your coding agent.

### Mirrors and incoming screen shares

A phone mirror may use a general-purpose player such as `mpv`. Skipping or masking `mpv` applies to that mirror too. Leave it out of both lists if you want its contents in Replay.

A screen shared with you in Google Meet or Zoom appears as pixels inside your local browser or meeting window. Replay can retain it at the normal capture interval; it does not identify or exclude apps inside the shared video. Browsers and meeting apps are not excluded by default. The meeting must be visible on the selected display, with recording running and no excluded local window blocking that display. This remains screenshot history, not continuous meeting video or audio.

Browser extensions and password-manager pages inside an ordinary browser window are also not covered by native password-manager IDs.

Nonempty fields in one window rule are ANDed; different rules are alternatives. Address-specific rules require an app/title guard and the current compositor instance. Reselect them after a compositor restart. Their compositor masks use the broader app/title match, which can hide other matching windows too.

Exclusions protect future captures. They do not delete old recordings.

## TOML and file locations

Run `"$replay_bin" daemon paths` for resolved paths. Replay honors absolute XDG base-directory overrides.

| Purpose | Default location |
| --- | --- |
| Settings | `~/.config/omarchy-replay/config.toml` |
| Shared history and OCR | `~/.local/share/omarchy-replay/history/` |
| Saved intent and logs | `~/.local/state/omarchy-replay/` |
| Cache | `~/.cache/omarchy-replay/` |
| Local socket and leases | `$XDG_RUNTIME_DIR/omarchy-replay/` |

A private per-user temporary runtime directory is used when `XDG_RUNTIME_DIR` is absent. History, logs and settings remain local. Review diagnostic content before sharing it.

A typical configuration:

```toml
[recording]
output = "YOUR_OUTPUT"
# display_mode = "focused" # Records whichever display has focus each capture; output is then ignored.
interval_seconds = 5.0

[storage]
directory = "" # Empty uses the default history folder.
retention_days = 30
max_disk_mib = 10240
min_free_mib = 1024

[indexing]
active_cpu_percent = 40.0
idle_cpu_percent = 50.0
request_cpu_percent = 50.0
pressure_cpu_percent = 10.0
cpu_ceiling_percent = 60.0
idle_seconds = 60

[service]
login_startup = false

[meetings]
enabled = false
directory = "" # Uses ~/Documents/Meetings; otherwise a real absolute source folder.

[exclusions]
# Omit both lists to use fresh defaults from exclusion-presets.md.
# apps = ["example.private-app"] # Hide from screenshots/sharing and pause Replay.
# skip_apps = ["example.game"] # Pause Replay only.
```

An explicit list replaces its own defaults. Legacy files with `apps` and no `skip_apps` keep their existing policy, with no added skips. Each list may be empty; at most 64 entries are allowed across both. Replay and screensaver exclusions are enforced separately. Saved `recording.output_identity` is maintained after display verification. An address rule also stores `compositor_instance`.

The parser validates types, ranges and window patterns. Native writes preserve unknown TOML values and refuse to overwrite a concurrent edit. Unknown extension fields in a window rule may require direct editing when the native editor cannot preserve their meaning.

A running coordinator applies accepted mask changes even while recording is paused or stopped. Status reports `exclusions_pending` until they finish and `exclusions_error` if verification fails; retries back off. The recording choice stays unchanged. An offline coordinator must run before it can update compositor rules.

After a direct edit, run `"$replay_bin" daemon paths` and check that `config_error` is empty and `using_last_valid_config` is false. If the coordinator is running, run `"$replay_bin" daemon reload` and check status again. Leave an offline coordinator offline unless you intend to start it; offline reload starts the coordinator with capture held and can change saved running intent to paused. The coordinator keeps its last valid settings if the new file is invalid. The launcher can still open that archive and Settings can copy a repair prompt. Settings saved through the viewer request reload automatically.

If you directly change `service.login_startup`, validate the TOML as above, then run `systemctl --user enable omarchy-replay.service` for `true`, or `systemctl --user disable omarchy-replay.service` for `false`. Omit `--now` to preserve current recording and coordinator state. Check `systemctl --user is-enabled omarchy-replay.service`; a `disabled` result has a nonzero exit status. Editing the offline config alone does not change systemd enablement. A missing unit is an installation problem; it does not require a source checkout to diagnose.

## Terminal controls and diagnostics

```bash
"$replay_bin" daemon paths
"$replay_bin" daemon status
"$replay_bin" daemon start
"$replay_bin" daemon pause
"$replay_bin" daemon resume
"$replay_bin" daemon stop
"$replay_bin" daemon index-pause
"$replay_bin" daemon index-resume
"$replay_bin" daemon shutdown
```

`status` is read-only. `stop` ends new capture while allowing indexing and maintenance. `shutdown` ends the coordinator too. Offline Pause/Stop/Shutdown update saved intent without launching it. Other offline controls can start it with capture held; Start/Resume explicitly permit recording.

The `meetings` object in `daemon status` reports dependency availability, enabled/paused state, the source folder, worker state, last reported import count, sync counters, limits and errors. It contains no transcript text. A `limited` result can mean storage pressure or an importer bound; it does not indicate an OCR backlog. Counts describe the latest scan and may be stale when integration is disabled. Meeting search is currently in the viewer; the CLI `search` command remains screen OCR only.

For service failures:

```bash
systemctl --user status omarchy-replay.service
journalctl --user -u omarchy-replay.service -n 80 --no-pager
```

The state directory also holds bounded `recording.log`, one rotation and an index-worker error tail when applicable. Status can include window metadata. Keep these outputs private unless reviewed for sharing. The [agent guide](agent-guide.md) covers diagnosis, configuration and supported recall commands.

## Source installations and legacy trials

The [plugin installation guide](installation.md) covers explicit native setup, updates and removal. The installed launcher and service use a standalone versioned payload outside the plugin folder. A development checkout can be moved or removed after installation.

Earlier builds used the directory and unit name `oma-rewind`. This release accepts compatibility symlinks left by an already completed migration. If real legacy directories or the old unit remain, setup refuses to move them as part of an app update. Complete the migration with the earlier installer before upgrading; it preserves recordings and intent and refuses conflicting old/new locations. Native uninstall preserves current configuration and history.

Development trials remain separate: shared history does not import or expire finite recordings under `runs/trials/`. Open a trial from its source checkout:

```bash
./scripts/try-replay view --trial runs/trials/YOUR_TRIAL
./scripts/try-replay report --trial runs/trials/YOUR_TRIAL
```

Synthetic and bounded native tests support this implementation. Long-session throughput and actual hardware lock/sleep/display behavior still need ordinary-use validation. S3 offload and dedicated coding-agent recall tools remain [roadmap work](roadmap.md).
