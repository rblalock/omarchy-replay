# Release readiness

**0.2.0 (2026-11-08):** the owner authorized the 0.2.0 release after #6 merged. It adds the recall CLI (#1), the FFmpeg SIGXFSZ fix (#2), configurable OCR languages (#5) and focused-display capture (#6). See the [release notes](releases/0.2.0.md). It was validated with the [runbook](release-weekend.md) steps, with the version substituted. The marketplace listing needs a newer-commit verification request for the release SHA. The 0.1.0 record below is unchanged.

Updated 2026-09-26. The repository is public. The owner confirmed the installation check and authorized publishing **0.1.0** and submitting it to the Omarchy marketplace. This closes the owner installation-check gate; it does not establish performance or lifecycle compatibility on every machine. Release publication and marketplace listing approval remain separate outcomes.

Version [0.1.0](https://github.com/rblalock/omarchy-replay/releases/tag/v0.1.0) is published. [Marketplace submission #8839](https://github.com/omacom/omarchy-plugin-marketplace/issues/8839) passed automated validation; native recorder and installer review remains pending. The [maintainer's requested correction](https://github.com/omacom/omarchy-plugin-marketplace/issues/8839#issuecomment-5847663665) removes the root `AGENTS.md` from the current plugin tree, including its historical push-authorization statement. The ignore rules also exclude local `AGENTS.md` and `AGENTS.override.md` files. Existing user and development guides remain ordinary documentation, with no replacement automatically loaded instruction file. This changes no native runtime or installer behavior; the corrected source commit requires re-validation.

Use the [weekend runbook](release-weekend.md), [release notes](releases/0.1.0.md) and [marketplace submission body](marketplace-submission.md). Check the [release page](https://github.com/rblalock/omarchy-replay/releases/tag/v0.1.0) for publication and the marketplace issue for listing approval. Earlier dated evidence below retains the state at each review.

## Marketplace requirements

The [publishing guide](https://plugins.omarchy.org/publish.html) requires a public GitHub repository, root manifest, README, license and safe installation/removal. The [submission contract](https://github.com/omacom/omarchy-plugin-marketplace/blob/main/SUBMISSION.md) also requires documented dependencies and a permanent unique plugin ID outside `omarchy.*`. A preview is optional.

Listings are validated against an exact commit. The [security baseline](https://github.com/omacom/omarchy-plugin-marketplace/blob/main/SECURITY.md) and explicit maintainer approval are separate from local testing. Replay's service management, setup commands and capture capabilities must be described accurately. A review-required result is not automatically a rejection; accepted capabilities still need the maintainer's decision.

| Requirement | Current state |
| --- | --- |
| License | MIT selected; root [LICENSE](../LICENSE) and [third-party notices](../THIRD_PARTY_NOTICES.md) added. |
| Public repository | The owner made it public on September 26; GitHub visibility was verified. |
| Root manifest and working QML entry | `io.github.rblalock.omarchy-replay`, version 0.1.0, with a native bar widget and keyboard-driven quick actions. |
| Installation/removal documentation | Standalone payload, transactional setup/update and history-preserving uninstall implemented; see [installation](installation.md). |
| Dependencies | Source dependencies documented, including Qt's Wayland plugin. The owner confirmed the installation check; tested versions are recorded below, not claimed as universal minimums. |
| Local validation | All 44 current suites passed during the meeting-integration work. A fresh committed-source build, runtime verification and eight package tests passed on September 25. Earlier Arch container results are retained below. No GitHub Actions workflow is used. |
| Preview | Optional; use fictional history, never personal captures. |
| Submission | Body finalized and submission authorized by the owner. Automated validation and marketplace maintainer approval are separate from publishing a GitHub release. |

Implemented listing identity: `io.github.rblalock.omarchy-replay`, category **Productivity**, tags **hyprland**, **system**. No collision was found in the current registry, including retired IDs; recheck before submission. Do not tag agent features as shipped while recall integration remains planned.

## 1. Installed native runtime and real plugin — implemented

The installed Omarchy plugin manager clones, validates and loads QML. Update fast-forwards the plugin checkout; removal disables the shell component and deletes that checkout. There are **no install, update or removal hooks**. It will not build Replay or clean up a separate user service.

The first release has a QML bar widget with setup/update status, Open history, Settings and Start/Stop/Resume recording. Use the marketplace's manual-setup designation. The panel should explain missing dependencies or an absent runtime and present an explicit setup action. It must not compile, install packages or start recording merely because the shell loads it. Keep capture and OCR in the existing native processes.

Explicit setup builds from source with system packages, using an external cache directory and two compiler jobs. A verified versioned payload installs outside the plugin checkout. It contains `bin/replay`, runtime Python helpers, Lua resources, both license notices and the agent guide. Its manifest records version, source revision/dirty state and file hashes. The relative helper layout is preserved. Installed launchers never fall back to building missing source.

Acceptance:

- A root manifest declares the bar-widget entry and quick-action panel; `omarchy plugin validate` and QML checks pass.
- Setup, opening, closing, plugin disable/re-enable and shell restart have defined behavior.
- The native app works with the development checkout absent. Settings' copied prompts resolve installed paths.
- A version command identifies the installed build; partial/mismatched updates produce a useful error.
- Fresh setup leaves capture stopped and login startup disabled. Loading the plugin never changes saved recording intent.

## 2. Transactional installation and updates — implemented

The installer stages a verified payload before stopping an existing coordinator. Managed-file snapshots and the prior runtime pointer support rollback if integration or restart fails. Installation preserves actual systemd enablement, capture intent and indexing pause; it also checks that the restarted coordinator is the service’s MainPID.

Preflight checks dependencies, path ownership, existing files, shortcut conflicts and compositor compatibility. Rollback restores owned files and the previous pointer independently; concurrent edits remain intact and produce a recovery error. A conflicting or invalid desktop prevents automatic recording restart. Capture guards are never relaxed to recover an update.

Acceptance:

- Update running, paused, stopped and indexing-paused installations, preserving config, history and unknown TOML keys.
- Inject failures during build, initialization, service write, compositor reload and shortcut installation; the previous installation remains usable.
- Handle custom XDG locations and paths containing spaces, Unicode and percent signs.
- Reject ambiguous managed markers, unexpected symlinks and unmanaged destination files.
- Build only application targets for installation; development tests and experiments remain explicit build options.

## 3. History-preserving native removal — implemented

The installed launcher provides an explicit `uninstall` command. Document its use **before** `omarchy plugin remove`. The plugin manager has no cleanup hook, so deleting the plugin folder alone cannot remove the external native installation.

The uninstaller must stop verified Replay-owned processes, disable/remove its user unit, and remove only owned desktop entries, shortcut blocks and compositor includes/masks. Reload and validate Hyprland after removing rules. Preserve settings, history and custom storage by default. History deletion stays a separate, explicit action.

Acceptance:

- Removal while recording leaves no owned worker, enabled unit, broken binding or persistent Replay mask.
- Repeated removal and partial-install removal are safe.
- Unrelated user settings and all archive bytes remain unchanged, including an external storage folder.
- Reinstallation discovers retained settings/history but does not silently restart recording.
- The root README gives the exact installed uninstall command and explains what survives.

## 4. Prove the release on a clean Omarchy installation

The source build uses Qt, Wayland capture protocols, Tesseract, SQLite, WebP, Python and systemd user services. A running development machine can hide missing packages. Check Qt Wayland support, the Tesseract CLI and English data, Python 3.11 or newer, Lua window rules and the required capture protocols. FFmpeg remains needed for legacy video history and experimental paths.

The owner reported successful laptop use on September 25 and confirmed the requested installation check on September 26. That is owner-reported acceptance, separate from the automated fixtures below. For future release rehearsals, use fictional content and cover install/open/configure/record/search/update/remove, display selection, OCR highlights and region copy, lock/unlock, screensaver, suspend/resume, display disconnect and recovery. Exercise both strict privacy masks and Replay-only skips; verify incoming meeting content without weakening local privacy rules.

Measure retained coverage, pending age, CPU, memory, storage growth and foreground responsiveness together. Passing headless tests does not establish capture safety or long-session performance on another machine.

## 5. Prepare the publication snapshot

Review the final tree and every commit that will become reachable publicly. Exclude personal images, OCR databases, logs, credentials, private paths and diagnostic artifacts. Review author metadata, dependency notices and any preview. Keep local backups private; do not push backup refs.

After packaging and installation tests pass, record one release version and exact source commit. Run the local validation and marketplace compatibility/static checks against that same commit. The root README must clearly distinguish current features from planned agent recall and remote storage.

The owner chose local validation to avoid GitHub Actions costs. This repository has no Actions workflow; hosted CI is not a release gate. The marketplace's own submission validation and maintainer review remain separate requirements.

Prepare the marketplace issue with the prescribed headings, category, tags and checklist. Include the manual setup requirement, native dependencies, background service, compositor changes, local-data behavior and uninstall path in maintainer notes. The owner authorized release publication and submission on September 26; repository visibility was already public. Record and verify the resulting release and issue URLs when those operations complete.

## Earlier verification

- Audited all seven reachable commits and 229 historical blobs for captured-media/runtime file extensions, local user paths and several credential formats. No matches were found; author metadata uses GitHub's noreply address. This is limited pattern screening, not a complete secrets audit, and must be repeated on the final outgoing snapshot.
- Twenty-six isolated installer tests cover first setup, update with preserved recording/indexing choices and actual service enablement, restart/reload failures, concurrent edits, managed-only removal, symlink/path conflicts and customized shortcut preservation. Package tests run a real copied native binary after deleting the synthetic source checkout.
- Verified the plugin manager's lack of native lifecycle hooks against installed command source and the current marketplace contract.
- Before the packaging changes, all 34 local CTest suites passed in 212.38 seconds with `REPLAY_TEST_RESOURCE_SCOPE=1`, including both opt-in native user-service suites. Fixtures used synthetic history; this is not clean-machine installation or live suspend/resume proof. Diagnostics remain local under ignored `runs/`.
- The local test environment used Omarchy 4.0.4, Hyprland 0.56.2, Qt 6.11.2, Tesseract 5.5.3, Python 3.14.7 and wayland-protocols 1.49. These are tested versions, not established minimum requirements.
- A pinned Arch container image and the documented dependency list built successfully with no host mounts. The first full container run passed 31 suites, skipped the two native user-service suites and exposed an overly strict viewer-test assertion: translating a fitted selection can change its outward-rounded source crop by one pixel. The test now permits that rounding and requires exact reverse/forward round trips; OCR crop behavior is unchanged. The focused viewer suite then passed on the host (18.58 seconds) and a fresh pinned container (19.89 seconds), resolving all 32 container-enabled suites. Both temporary containers were removed. This validates the build/test commands locally, not an installed Omarchy plugin.

For each release, validate its final source commit and keep native acceptance distinct from automated checks and marketplace approval. Agent recall remains the next product milestone. Current user commands are documented in [installation](installation.md).

### Runtime and plugin verification — 2026-09-24

- The expanded 37-suite native/Python test run passed in 212.05 seconds with native user-service checks enabled. The three added plugin suites also passed: model logic, actual Quickshell keyboard/state behavior, and the native bar entry in an isolated compositor. All 40 registered suites have passed; the installer’s later focused regressions passed separately.
- The local installation was upgraded from the checkout-based service to the standalone runtime. The restarted coordinator remained in recording state; capture intent, indexing pause, login enablement, selected display, exact TOML bytes and archive identity were preserved. No config or exclusion error was reported.
- The real shell loaded the local bar widget with existing bar entries preserved. Its Settings action opened a floating native viewer, and a second invocation reused that viewer. Test-created viewer windows were closed afterward. Synthetic native checks exercise menu start/stop states; the host recording was not toggled for testing.
- The host installation is a local preview of uncommitted source, not a published release or marketplace acceptance. Full clean-machine install/record/update/remove and hardware lifecycle checks remain in step 4. No remote release, visibility change or marketplace submission was performed.

The bridge’s status path was sampled ten times on the development host: approximately 52 ms wall time and 40 ms child CPU per invocation, or 0.40% of one CPU at a ten-second interval per widget instance. This is a short process-cost sample, not a whole-system power measurement or a long-session benchmark. No continuous resource monitor was added.

### Weekend preparation — 2026-09-25

The application snapshot checked was `74bc91df44b8c677b2c99e15615d3ede15ed4c57` (the completed-meeting integration). Release documentation prepared afterward is not part of that SHA; record and validate the final release commit before publication.

- A fresh Git clone of that commit built the Release application with two compiler jobs and no development build artifacts. Its standalone payload passed hash/mode verification and all eight package tests. The manifest reports version `0.1.0`, the expected source revision and `source_dirty: false`. This is local packaging proof, not a portable binary release.
- Clean-source `omarchy plugin validate` passed. Validate a clean snapshot: the development directory's ignored experiments contain symlinks that the plugin validator correctly rejects.
- The current marketplace validator and scanner were inspected at `d9be5323b5054fbce8b84b2023de25936621f6c6`. Local exact-Git-blob checks passed its community manifest rules and found no ID collision across 4,178 current catalog entries. The baseline returned **zero findings**, `review-required`, and `blocksApproval: false`.
- Expected capability review covers `installer`, `service-management`, `privilege`, `package-manager` and `remote-build`. The package/privilege flags identify the documented user-run dependency command; Replay setup builds its own source and does not install packages automatically. These capabilities need disclosure and maintainer review, not removal to avoid scanning.
- The local scanner adapter did not test public repository reachability. After publication, run the upstream submission validator and scanner against remote HEAD, compare its recorded commit to the intended release SHA, and obtain the marketplace's `approved-and-verified` decision. Local success is not listing approval.
- Laptop use is user-reported successful; no exact hardware, lifecycle, performance or removal claims are inferred. This preparation did not restart the personal recorder, inspect private captures or change installed settings.
- Publication screening covered ten commits reachable from the checked application SHA, all 314 historical text blobs and 180 current source/documentation files, including the new release drafts. No checked private-path, runtime-media/database, credential or author-email patterns matched. Author and committer emails use GitHub noreply. This bounded screening is not a complete secrets audit; repeat the final snapshot review before changing visibility. GitHub read-back confirmed Actions disabled and the repository private.

Build, package and scanner evidence stays under ignored `runs/release-0.1.0-20260925/` and `runs/release-marketplace-preflight-20260925/`.
