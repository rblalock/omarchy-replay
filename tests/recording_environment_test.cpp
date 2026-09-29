#include "recording_environment.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }

QJsonObject monitor(const QString &name = "TEST-1", int x = 0, bool focused = false) {
    return {{"id", x == 0 ? 1 : 2}, {"name", name}, {"make", "Synthetic"}, {"model", "Fixture"}, {"serial", name},
            {"description", "Synthetic fixture display"}, {"width", 1920}, {"height", 1080}, {"scale", 1.0},
            {"transform", 0}, {"x", x}, {"y", 0}, {"disabled", false}, {"dpmsStatus", true}, {"mirrorOf", "none"},
            {"focused", focused}};
}
QJsonObject window(QString app = "fixture.editor", int x = 20) {
    return {{"address", "0x1234"}, {"class", app}, {"initialClass", app}, {"title", "Synthetic invoice"},
            {"mapped", true}, {"hidden", false}, {"visible", true}, {"at", QJsonArray{x, 50}},
            {"size", QJsonArray{800, 700}}, {"monitor", 1}, {"pinned", false}};
}
replay::EnvironmentObservation ready() {
    replay::EnvironmentObservation result;
    result.compositorAvailable = result.lockNotificationsAvailable = result.compositorLockKnown = true;
    result.compositorLocked = false;
    result.sessionKnown = result.sessionActive = result.sleepKnown = result.configKnown = true;
    result.exclusionsVerified = true;
    result.sessionLocked = result.sleeping = result.shuttingDown = result.configError = false;
    result.compositorInstance = "synthetic-session-one";
    result.configGeneration = 1;
    result.waylandDisplay = "wayland-fixture";
    result.monitors = {monitor(), monitor("TEST-2", 1920)};
    result.windows = {window()};
    return result;
}
replay::EnvironmentOptions options() {
    replay::EnvironmentOptions result; result.output = "TEST-1"; result.exclusionMaskToken = QString(64, 'a'); return result;
}

void lifecycle() {
    auto observed = ready(); replay::RecordingEnvironment environment([&] { return observed; }); environment.configure(options());
    const auto initial = environment.snapshot();
    require(initial.captureAllowed && !initial.outputIdentity.isEmpty(), "Verified desktop was blocked or identity unpinned");
    require(environment.snapshot().generation == initial.generation, "Stable snapshots invalidated a capture");
    observed.compositorLocked = true;
    require(environment.snapshot().reason == "locked", "Compositor lock ignored when logind hint stayed false");
    observed.compositorLocked = false; observed.sessionLocked = true;
    require(environment.snapshot().reason == "locked", "logind locked hint ignored");
    observed.sessionLocked = false;
    const auto beforeRapidLock = environment.snapshot();
    observed.eventGeneration += 2;
    const auto afterRapidLock = environment.snapshot();
    require(afterRapidLock.captureAllowed && afterRapidLock.generation != beforeRapidLock.generation,
            "Rapid lock and unlock did not invalidate in-flight capture");
    require(afterRapidLock.configGeneration == beforeRapidLock.configGeneration, "Lock event invalidated the mask receipt");
    ++observed.configGeneration;
    require(environment.snapshot().configGeneration != afterRapidLock.configGeneration, "Config reload did not invalidate mask receipt");
    observed.sleeping = true;
    require(environment.snapshot().reason == "sleeping", "Sleep was not excluded");
    observed.sleeping = false; observed.sessionActive = false;
    require(environment.snapshot().reason == "session_inactive", "Inactive VT/session was not excluded");
    observed.sessionActive = true; observed.shuttingDown = true;
    require(environment.snapshot().reason == "shutting_down", "Shutdown was not excluded");
    observed.shuttingDown = false; observed.configError = true;
    require(environment.snapshot().reason == "compositor_configuration", "Config errors did not pause capture");
    observed.configError = false;
    require(environment.snapshot().captureAllowed, "Automatic environmental recovery did not permit capture");
    // Capture intent is not part of this module, so recovery cannot unpause or
    // restart a manually paused/stopped coordinator.
    std::cout << "PASS lock, rapid transitions, sleep, session, shutdown and configuration recovery\n";
}

void unknowns() {
    auto observed = ready(); replay::RecordingEnvironment environment([&] { return observed; }); environment.configure(options());
    for (bool replay::EnvironmentObservation::*field : {&replay::EnvironmentObservation::compositorAvailable,
            &replay::EnvironmentObservation::lockNotificationsAvailable, &replay::EnvironmentObservation::compositorLockKnown,
            &replay::EnvironmentObservation::sessionKnown, &replay::EnvironmentObservation::sleepKnown,
            &replay::EnvironmentObservation::configKnown, &replay::EnvironmentObservation::exclusionsVerified}) {
        observed = ready(); observed.*field = false;
        require(!environment.snapshot().captureAllowed, "Unknown lifecycle state allowed capture");
    }
    observed = ready(); auto client = window(); client.remove("visible"); observed.windows = {client};
    require(environment.snapshot().reason == "window_state_unknown", "Unsupported visibility metadata allowed capture");
    observed = ready(); auto display = monitor(); display.remove("dpmsStatus"); observed.monitors = {display};
    require(!environment.snapshot().captureAllowed, "Incomplete monitor state allowed capture");
    std::cout << "PASS unavailable protocols, missing state and malformed metadata fail closed\n";
}

void outputs() {
    auto observed = ready(); replay::RecordingEnvironment environment([&] { return observed; }); environment.configure(options());
    const auto before = environment.snapshot();
    observed.monitors = {monitor("TEST-2", 1920)};
    require(environment.snapshot().reason == "output_unavailable", "Disconnected output silently switched monitors");
    observed.monitors = {monitor()};
    require(environment.snapshot().captureAllowed, "Same monitor did not reconnect");
    auto replaced = monitor(); replaced["serial"] = "replacement"; observed.monitors = {replaced};
    require(environment.snapshot().reason == "output_identity_changed", "Changed device on same connector was accepted");
    observed.monitors = {monitor()}; auto off = monitor(); off["dpmsStatus"] = false; observed.monitors = {off};
    require(environment.snapshot().reason == "output_off", "DPMS off did not suspend capture");
    off["dpmsStatus"] = true; off["mirrorOf"] = "2"; observed.monitors = {off};
    require(environment.snapshot().reason == "output_mirrored", "Mirrored monitor was accepted without support");
    observed.monitors = {monitor()}; observed.compositorInstance = "synthetic-session-two";
    const auto restart = environment.snapshot();
    require(restart.captureAllowed && restart.generation != before.generation, "Compositor reconnect did not invalidate capture");
    std::cout << "PASS pinned output, reconnect, replacement, DPMS, mirror and compositor generation\n";
}

void focusedOutput() {
    // Focused mode has no configured display; selection comes from the compositor.
    auto observed = ready();
    replay::RecordingEnvironment environment([&] { return observed; });
    auto config = options(); config.followFocus = true; config.output.clear();
    require(replay::RecordingEnvironment::validateOptions(config).isEmpty(), "Focused mode required a configured display");
    auto fixedOptions = options(); fixedOptions.output.clear();
    require(!replay::RecordingEnvironment::validateOptions(fixedOptions).isEmpty(), "Fixed mode accepted a missing display selection");
    environment.configure(config);

    // R5: zero focused displays block; nothing is substituted.
    require(environment.snapshot().reason == "focus_unknown", "An unfocused desktop allowed focused capture");
    auto first = monitor();                // TEST-1, x = 0
    auto second = monitor("TEST-2", 1920); // TEST-2, x = 1920

    // R3: exactly one focused display is selected, with its own bounds.
    first["focused"] = true; observed.monitors = {first, second};
    const auto selected = environment.snapshot();
    require(selected.captureAllowed && selected.selectedOutput == "TEST-1", "The focused display was not selected");
    require(selected.outputIdentity.isEmpty(), "Focused mode pinned a hardware identity");

    // R6: an excluded app on the unfocused display does not pause the focused
    // capture, and the same app pauses once its own display has focus.
    observed.windows = {window("com.onepassword.OnePassword", 2000)};
    require(environment.snapshot().captureAllowed, "An excluded window on the unfocused display paused the capture");
    first["focused"] = false; second["focused"] = true; observed.monitors = {first, second};
    const auto switched = environment.snapshot();
    require(!switched.captureAllowed && switched.reason == "excluded_window" && switched.selectedOutput == "TEST-2",
            "The exclusion did not follow focus to the other display");
    first["focused"] = true; second["focused"] = false; observed.monitors = {first, second};
    require(environment.snapshot().captureAllowed, "Restoring focus did not restore capture");

    // R5: two focused displays are as unusable as none.
    second["focused"] = true; observed.monitors = {first, second};
    require(environment.snapshot().reason == "focus_unknown", "Two focused displays allowed focused capture");

    // R7: a focused display that is off or mirrored blocks; the other display
    // is never captured instead.
    second["focused"] = false; first["dpmsStatus"] = false; observed.monitors = {first, second};
    const auto off = environment.snapshot();
    require(!off.captureAllowed && off.reason == "output_off" && off.selectedOutput == "TEST-1",
            "A focused display that is off did not block capture");
    first["dpmsStatus"] = true; first["mirrorOf"] = "2"; observed.monitors = {first, second};
    require(environment.snapshot().reason == "output_mirrored", "A focused mirrored display did not block capture");

    // Focused mode never pins a hardware identity: a replacement device on the
    // focused display stays eligible.
    first["mirrorOf"] = "none"; first["serial"] = "replacement"; observed.monitors = {first, second};
    require(environment.snapshot().captureAllowed, "A replaced device blocked the focused display");

    // A configured identity is echoed but never compared in focused mode.
    auto pinned = options(); pinned.followFocus = true; pinned.outputIdentity = QString(64, 'b');
    environment.configure(pinned);
    const auto echoed = environment.snapshot();
    require(echoed.captureAllowed && echoed.outputIdentity == QString(64, 'b'),
            "Focused mode compared or replaced the configured identity");

    // R10: fixed mode still selects the configured display and pins its identity.
    auto fixedObserved = ready();
    replay::RecordingEnvironment fixed([&] { return fixedObserved; });
    fixed.configure(options());
    const auto fixedReady = fixed.snapshot();
    require(fixedReady.captureAllowed && fixedReady.selectedOutput == "TEST-1" && !fixedReady.outputIdentity.isEmpty(),
            "Fixed mode lost its configured display selection or identity pin");
    std::cout << "PASS focused display selection, focus_unknown, focus-following exclusions, blocked focus and unpinned identity\n";
}

void exclusions() {
    auto observed = ready(); replay::RecordingEnvironment environment([&] { return observed; }); environment.configure(options());
    observed.windows = {window("omarchy-replay")};
    require(environment.snapshot().captureAllowed && environment.snapshot().excludedApps.contains("omarchy-replay"),
            "Verified Replay mask did not allow other desktop content to be recorded");
    observed.exclusionsVerified = false;
    const auto unverified = environment.snapshot();
    require(unverified.reason == "exclusions_unverified" && !unverified.captureAllowed && unverified.visibleWindows.size() == 1,
            "Unverified mask allowed capture or hid safe local settings metadata");
    observed.exclusionsVerified = true;
    observed.windows = {window("com.onepassword.OnePassword")};
    require(environment.snapshot().reason == "excluded_window", "Verified password manager ID was not excluded");
    auto client = window("unrelated.current.id"); client["initialClass"] = "com.onepassword.OnePassword";
    observed.windows = {client};
    require(!environment.snapshot().captureAllowed, "Initial application identity was ignored");
    client = window("omarchy-replay"); client["hidden"] = true; observed.windows = {client};
    require(environment.snapshot().captureAllowed, "Hidden grouped window unnecessarily paused capture");
    client["hidden"] = false; client["visible"] = false; observed.windows = {client};
    require(environment.snapshot().captureAllowed, "Inactive workspace window unnecessarily paused capture");
    client = window("com.onepassword.OnePassword", 2000); observed.windows = {client};
    require(environment.snapshot().captureAllowed, "Window entirely on another display was excluded");
    client["at"] = QJsonArray{1800, 20}; client["monitor"] = 2; client["pinned"] = true; observed.windows = {client};
    require(!environment.snapshot().captureAllowed, "Unfocused pinned window straddling selected output was missed");
    auto config = options(); config.excludedApps = {"fixture.editor"};
    environment.configure(config); observed.windows = {window()};
    const auto blocked = environment.snapshot();
    require(!blocked.captureAllowed && blocked.visibleWindows.size() == 1 && blocked.visibleWindows[0].toObject()["excluded"].toBool(),
            "Private UI metadata lost visible exclusion status");
    observed.windows = {window("omarchy-replay")};
    require(environment.snapshot().captureAllowed && environment.snapshot().excludedApps.contains("omarchy-replay"),
            "Explicit app list removed mandatory Replay protection");
    observed.windows = {window()};
    config.excludedApps.clear(); config.excludedWindows = {{"^Synthetic invoice$", "fixture.editor", "0x1234", "output", "synthetic-session-one"}};
    environment.configure(config);
    require(!environment.snapshot().captureAllowed, "ANDed app/title/address window rule failed");
    client = window(); client["address"] = "0xabcd"; observed.windows = {client};
    require(environment.snapshot().captureAllowed, "Address rule was treated as OR rather than AND");
    observed.compositorInstance = "synthetic-session-two";
    require(environment.snapshot().reason == "stale_window_exclusion", "Session-address rule silently matched a new compositor");
    observed.compositorInstance = "synthetic-session-one";
    config.excludedWindows = {{"[", "", "", "output"}}; environment.configure(config);
    require(environment.snapshot().reason == "invalid_configuration", "Invalid regex did not block capture");
    config.excludedWindows = {{"invoice", "", "", "focused"}}; environment.configure(config);
    require(environment.snapshot().reason == "invalid_configuration", "Focused-only privacy rule was accepted");
    std::cout << "PASS viewer/password exclusion, visibility, cross-display geometry and rule validation\n";
}

void presetDefaults() {
    auto observed = ready();
    replay::RecordingEnvironment environment([&] { return observed; });
    environment.configure(options());
    const auto removableDefaults = replay::privacyAppExclusions() + QStringList{"steam", "Steam"};
    for (const auto &app : removableDefaults) {
        auto client = window(app);
        client["initialClass"] = "fixture.changed.id";
        observed.windows = {client};
        require(environment.snapshot().reason == "excluded_window", "Default app current class was not excluded");
        client = window("fixture.changed.id"); client["initialClass"] = app;
        observed.windows = {client};
        require(!environment.snapshot().captureAllowed, "Default app initial class was ignored");
        client = window(app, 2000); observed.windows = {client};
        require(environment.snapshot().captureAllowed, "Default app on another display paused this output");
        observed.windows = {window(app + ".fixture")};
        require(environment.snapshot().captureAllowed, "Default app exclusion matched a longer identifier");
    }
    auto allowedApps = replay::gamingAppExclusions() + replay::mediaAppExclusions();
    allowedApps.removeAll("steam"); allowedApps.removeAll("Steam");
    allowedApps.append({"fixture.editor", "steam_app_12345", "SteamGame", "orgXgnomeXWorldXSecrets"});
    for (const auto &app : allowedApps) {
        observed.windows = {window(app)};
        require(environment.snapshot().captureAllowed, "Defaults blocked an optional preset, game or unrelated app");
    }
    auto config = options(); config.excludedApps.clear(); config.skippedApps.clear();
    environment.configure(config);
    for (const auto &app : removableDefaults) {
        observed.windows = {window(app)};
        require(environment.snapshot().captureAllowed, "Explicit empty app list did not opt out of a removable default");
    }
    std::cout << "PASS privacy/Steam defaults, optional presets, exact identities, display scope and explicit opt-out\n";
}

void skipApps() {
    auto observed = ready();
    replay::RecordingEnvironment environment([&] { return observed; });
    auto config = options(); config.skippedApps = {"mpv", "steam", "omarchy-replay"};
    environment.configure(config);
    for (const auto &field : {"class", "initialClass"}) {
        auto client = window("fixture.other"); client[field] = "mpv";
        observed.windows = {client};
        auto snapshot = environment.snapshot();
        require(!snapshot.captureAllowed && snapshot.reason == "excluded_window", "Skip-only app did not suspend Replay");
        require(snapshot.visibleWindows[0].toObject()["exclusion_mode"] == "skip", "Skip-only app was reported as strict");
        client["at"] = QJsonArray{2000, 50}; observed.windows = {client};
        require(environment.snapshot().captureAllowed, "Skip-only app on another display suspended Replay");
        client["at"] = QJsonArray{1800, 50}; observed.windows = {client};
        require(!environment.snapshot().captureAllowed, "Skip-only app straddling displays was missed");
        client["hidden"] = true; observed.windows = {client};
        require(environment.snapshot().captureAllowed, "Hidden skipped app suspended Replay");
        client["hidden"] = false; client["visible"] = false; observed.windows = {client};
        require(environment.snapshot().captureAllowed, "Skipped app on inactive workspace suspended Replay");
    }
    for (const auto &app : {"chromium", "google-chrome", "zoom", "mpv.fixture", "SteamGame"}) {
        auto meeting = window(app); meeting["title"] = "Google Meet: mpv Steam screen share";
        observed.windows = {meeting};
        require(environment.snapshot().captureAllowed, "App exclusion matched meeting content or a longer ID");
    }
    observed.windows = {window("omarchy-replay")};
    auto snapshot = environment.snapshot();
    require(snapshot.captureAllowed && snapshot.visibleWindows[0].toObject()["exclusion_mode"] == "strict",
            "Skip-only entry disabled Replay's mask-without-pause exception");
    config.excludedApps.append("mpv"); environment.configure(config);
    observed.windows = {window("mpv")};
    require(environment.snapshot().visibleWindows[0].toObject()["exclusion_mode"] == "strict",
            "Skip-only entry weakened an existing strict app exclusion");
    config.excludedApps.removeAll("mpv");
    config.excludedWindows = {{"invoice", "mpv", "", "output"}}; environment.configure(config);
    require(environment.snapshot().visibleWindows[0].toObject()["exclusion_mode"] == "strict",
            "Skip-only entry weakened an existing strict window rule");
    config.excludedWindows.clear(); config.skippedApps.clear(); environment.configure(config);
    require(environment.snapshot().captureAllowed, "Removing skipped app did not restore recording eligibility");
    config.skippedApps = {"invalid\nidentifier"};
    require(!replay::RecordingEnvironment::validateOptions(config).isEmpty(), "Multiline skip identifier accepted");
    config.skippedApps.clear();
    while (config.excludedApps.size() + config.skippedApps.size() < 64) config.skippedApps.append("fixture.skip");
    require(replay::RecordingEnvironment::validateOptions(config).isEmpty(), "64 combined exclusions rejected");
    config.skippedApps.append("one.too.many");
    require(!replay::RecordingEnvironment::validateOptions(config).isEmpty(), "Combined exclusion bound ignored");
    std::cout << "PASS skip-only app visibility, meeting content, strict precedence and separate recording policy\n";
}

void screensaver() {
    auto observed = ready();
    replay::RecordingEnvironment environment([&] { return observed; });
    auto config = options(); config.excludedApps.clear();
    environment.configure(config);
    const auto before = environment.snapshot();
    observed.windows = {window("org.omarchy.screensaver")};
    const auto blocked = environment.snapshot();
    require(!blocked.captureAllowed && blocked.reason == "excluded_window" && blocked.generation != before.generation,
            "Empty configured exclusions allowed the screensaver or failed to invalidate capture");
    observed.windows = {window()};
    const auto resumed = environment.snapshot();
    require(resumed.captureAllowed && resumed.generation != blocked.generation,
            "Closing the screensaver did not restore capture eligibility");
    auto client = window("terminal.changed.id"); client["initialClass"] = "org.omarchy.screensaver";
    observed.windows = {client};
    require(!environment.snapshot().captureAllowed, "Screensaver initial class was ignored");
    client = window("org.omarchy.screensaver", 2000); observed.windows = {client};
    require(environment.snapshot().captureAllowed, "Screensaver on another display paused this output");
    client["at"] = QJsonArray{1800, 20}; observed.windows = {client};
    require(!environment.snapshot().captureAllowed, "Screensaver overlapping the recorded display was missed");
    client["visible"] = false; observed.windows = {client};
    require(environment.snapshot().captureAllowed, "Invisible screensaver paused capture");
    std::cout << "PASS mandatory screensaver exclusion, identity, display scope and recovery\n";
}

void safetyGeneration() {
    auto observed = ready();
    auto first = window(), second = window("fixture.browser", 1000);
    second["address"] = "0x5678";
    observed.windows = {first, second};
    replay::RecordingEnvironment environment([&] { return observed; }); environment.configure(options());
    const auto before = environment.snapshot();
    auto display = monitor();
    display["directScanoutTo"] = "0x1234"; display["directScanoutBlockedBy"] = "none";
    display["focused"] = false; display["activelyTearing"] = true;
    first["focusHistoryID"] = 5; second["focusHistoryID"] = 0;
    first["unrecognizedBookkeeping"] = "render-only";
    observed.monitors = {monitor("TEST-2", 1920), display};
    observed.windows = {second, first};
    const auto after = environment.snapshot();
    require(after.captureAllowed && after.generation == before.generation,
            "Render/focus bookkeeping or record ordering invalidated a safe capture");

    // Every guard input must retain its value and type in the retention fence.
    // Start each mutation from valid state to avoid relying on earlier failures.
    const QStringList monitorFields{"name", "id", "make", "model", "serial", "description", "disabled", "dpmsStatus",
        "x", "y", "width", "height", "scale", "transform", "mirrorOf"};
    for (const auto &field : monitorFields) {
        observed = ready(); const auto stable = environment.snapshot();
        display = monitor();
        const auto original = display.value(field);
        if (original.isBool()) display[field] = !original.toBool();
        else if (original.isDouble()) display[field] = original.toDouble() + 1;
        else display[field] = original.toString() + "-changed";
        observed.monitors = {display, monitor("TEST-2", 1920)};
        require(environment.snapshot().generation != stable.generation, "Monitor safety input omitted from generation");
    }
    const QStringList windowFields{"mapped", "hidden", "visible", "at", "size", "class", "initialClass", "title", "address"};
    for (const auto &field : windowFields) {
        observed = ready(); const auto stable = environment.snapshot();
        auto client = window();
        const auto original = client.value(field);
        if (original.isBool()) client[field] = !original.toBool();
        else if (original.isArray()) { auto pair = original.toArray(); pair[0] = pair[0].toDouble() + 1; client[field] = pair; }
        else client[field] = original.toString() + "-changed";
        observed.windows = {client};
        require(environment.snapshot().generation != stable.generation, "Window safety input omitted from generation");
    }
    for (const auto &field : {QString("visible"), QString("mapped"), QString("title"), QString("size")}) {
        observed = ready(); const auto stable = environment.snapshot();
        auto client = window(); client.remove(field); observed.windows = {client};
        const auto missing = environment.snapshot();
        require(!missing.captureAllowed && missing.generation != stable.generation,
                "Missing required window field did not close and invalidate capture");
        client[field] = QJsonValue::Null; observed.windows = {client};
        const auto wrong = environment.snapshot();
        require(!wrong.captureAllowed && wrong.generation != missing.generation,
                "Missing and wrongly typed required window metadata were collapsed");
    }
    observed = ready(); const auto beforeDisplayChange = environment.snapshot();
    observed.waylandDisplay = "wayland-replacement";
    require(environment.snapshot().generation != beforeDisplayChange.generation,
            "Capture socket identity omitted from generation");
    observed = ready(); const auto beforeExcludedFlash = environment.snapshot();
    // Native open/close events both advance this counter even when the excluded
    // window has disappeared by the time a blocked capture returns.
    observed.eventGeneration += 2;
    const auto afterExcludedFlash = environment.snapshot();
    require(afterExcludedFlash.captureAllowed && afterExcludedFlash.generation != beforeExcludedFlash.generation,
            "Rapid excluded-window show/hide was lost by canonicalization");
    std::cout << "PASS canonical safety generation, bookkeeping stability, required types and rapid privacy transitions\n";
}

void diagnostics() {
    auto observed = ready();
    replay::RecordingEnvironment environment([&] { return observed; }); environment.configure(options());
    environment.snapshot();
    require(!environment.diagnostics().value("enabled").toBool() && environment.diagnostics().value("snapshots").toInt() == 0,
            "Diagnostics sampled without an explicit opt-in");
    environment.setDiagnosticsEnabled(true);
    const auto before = environment.snapshot();
    auto display = monitor(); display["directScanoutTo"] = "private-window-address";
    display["private-arbitrary-field-name"] = "private-field-value";
    auto client = window(); client["focusHistoryID"] = 3;
    observed.monitors = {display, monitor("TEST-2", 1920)}; observed.windows = {client};
    require(environment.snapshot().generation == before.generation, "Diagnostic opt-in changed capture generation");
    auto report = environment.diagnostics();
    const auto fields = report.value("observed_field_changes").toObject();
    require(fields.value("monitors.directScanoutTo").toInt() == 1 && fields.value("windows.focusHistoryID").toInt() == 1 &&
            fields.value("monitors.other").toInt() == 1 && report.value("state_changes").toInt() == 0,
            "Diagnostics did not distinguish ignored raw fields from safety changes");
    client["title"] = "private-window-title"; observed.windows = {client}; environment.snapshot();
    report = environment.diagnostics();
    require(report.value("state_field_changes").toObject().value("windows").toInt() == 1,
            "Diagnostics missed a safety metadata transition");
    require(!QJsonDocument(report).toJson().contains("private-"), "Diagnostic output exposed metadata keys or values");
    environment.setDiagnosticsEnabled(false);
    const auto frozen = environment.diagnostics();
    observed = ready(); environment.snapshot();
    require(environment.diagnostics() == frozen, "Disabled diagnostics continued monitoring");
    environment.setDiagnosticsEnabled(true);
    require(environment.diagnostics().value("snapshots").toInt() == 0 &&
            environment.diagnostics().value("observed_field_changes").toObject().isEmpty(),
            "A new debug session retained old observations");
    std::cout << "PASS explicit temporary diagnostics, field-only reports, freeze and reset\n";
}

void events() {
    for (const QByteArray &event : {"openwindow>>abc,1,fixture,title", "closewindow>>abc", "movewindowv2>>abc,2,2",
            "monitorremoved>>TEST-1", "monitoradded>>TEST-1", "configreloaded>>", "windowtitlev2>>abc,title",
            "activewindowv2>>abc", "unknown>>", "malformed"})
        require(replay::RecordingEnvironment::invalidatingEvent(event), "Coverage transition was ignored");
    require(!replay::RecordingEnvironment::invalidatingEvent("screencastv2>>1,0,TEST-1"), "Own capture invalidated itself");
    std::cout << "PASS event classification and own-capture feedback prevention\n";
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--native-read-only") {
            replay::RecordingEnvironment environment; auto config = options(); config.output = QString::fromLocal8Bit(argv[2]);
            environment.configure(config); QElapsedTimer elapsed; elapsed.start();
            const auto result = environment.snapshot();
            // Explicitly omit real app names, titles, window addresses and device identity.
            std::cout << QJsonDocument(QJsonObject{{"capture_allowed", result.captureAllowed}, {"reason", result.reason},
                {"generation", qint64(result.generation)}, {"elapsed_ms", elapsed.elapsed()},
                {"visible_window_count", result.visibleWindows.size()}}).toJson(QJsonDocument::Compact).constData() << '\n';
            return result.reason == "ready" || result.reason == "excluded_window" || result.reason == "locked" ||
                result.reason == "exclusions_unverified" ? 0 : 1;
        }
        lifecycle(); unknowns(); outputs(); focusedOutput(); exclusions(); presetDefaults(); skipApps(); screensaver(); safetyGeneration(); diagnostics(); events();
    } catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return 0;
}
