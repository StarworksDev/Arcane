// The launch lifecycle: everything between "open this project" and "the
// editor exited". Carved out of lib.rs 2026-07-29, when the "thin IPC skin"
// had quietly accreted the most decision-rich code in the app -- the launch
// core and its wait thread, the boot watchdog, close-mode's delayed exit,
// the shell router, the running-set merge -- while its header still promised
// it held none. One story, one file:
//
//   do_open_project   the single launch brain, whatever door the launch came
//                     through (IPC command or a double-clicked .arcproj)
//   shell_open        the file-association door: engine choice + outcome
//                     surfacing for a launch nobody clicked a card for
//   running_keys      "which projects have a live editor", map + locks
//
// The RunningEditors map is managed Tauri state owned by run(); this module
// reads it through the AppHandle.

use std::collections::HashMap;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::time::Duration;

use tauri::Manager;

use crate::{editorlock, engine, project, resolve, settings, spawn, state, tray};

/// Live editors, keyed by `state::project_dir_key` of the PROJECT -> pid.
/// One editor per project: the dangerous case is the same project open twice
/// (two editors saving one project's files clobber each other); two different
/// projects on one engine build stay legal. The pid, not the Child, lives
/// here -- the wait thread owns the Child for its whole lifetime.
///
/// project_dir_key, not normalise_path: the editor lock that guards the same
/// rule lives per FOLDER (Saved/editor.lock), and a folder-shaped legacy row
/// and a manifest-shaped launch of one project must be one key, or the badge
/// and the double-open guard each miss half the cases.
pub struct RunningEditors(pub Mutex<HashMap<String, u32>>);

/// The running map, recovering from poison. A panic on a thread that held it
/// (the wait thread, a command) must not turn every later launch, exit and
/// badge refresh into a second panic; the map is only pids, and a stale one
/// is re-checked against the process table before it is believed.
fn running_map(app: &tauri::AppHandle) -> MutexGuard<'_, HashMap<String, u32>> {
    app.state::<RunningEditors>()
        .inner()
        .0
        .lock()
        .unwrap_or_else(|e| e.into_inner())
}

/// Disk-touching operations in flight: Duplicate, Scan, Rename, Delete,
/// Create. Close-mode's delayed exit used to `app.exit(0)` on a timer, which
/// could kill a Duplicate mid-copy (a half-copied "X Copy" left behind with
/// no listing) because the user had launched something else meanwhile.
static IN_FLIGHT: AtomicUsize = AtomicUsize::new(0);

/// RAII token for one in-flight operation; dropping it (including on an
/// early `?` return or a panic) releases it.
pub struct Busy(());

impl Drop for Busy {
    fn drop(&mut self) {
        IN_FLIGHT.fetch_sub(1, Ordering::SeqCst);
    }
}

/// Mark a disk operation as in flight until the returned guard drops.
pub fn busy() -> Busy {
    IN_FLIGHT.fetch_add(1, Ordering::SeqCst);
    Busy(())
}

/// Wait until no operation is in flight, polling, for at most `max`.
/// Returns whether the Hub went idle. Bounded so a wedged copy cannot keep a
/// closed Hub alive forever.
pub fn wait_idle(max: Duration) -> bool {
    let deadline = std::time::Instant::now() + max;
    loop {
        if IN_FLIGHT.load(Ordering::SeqCst) == 0 {
            return true;
        }
        if std::time::Instant::now() >= deadline {
            return false;
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

/// How long close-mode's exit waits for in-flight operations to finish.
const EXIT_DRAIN_MAX: Duration = Duration::from_secs(600);

impl RunningEditors {
    pub fn new() -> Self {
        Self(Mutex::new(HashMap::new()))
    }
}

impl Default for RunningEditors {
    fn default() -> Self {
        Self::new()
    }
}

/// What open_project actually did. A launch refused because a recorded path
/// vanished is an OUTCOME, not an error: the frontend answers it by refreshing
/// the list -- the row flips to its missing treatment, which already carries
/// the explanation -- instead of raising the error banner over a page that is
/// about to change under it. (Caught live 2026-07-29: a stale "project not
/// found" banner sat over a row still offering Launch, and clicking it read
/// as a jarring page reload.) Probe failures stay hard errors: they are facts
/// about a binary that IS there but cannot answer for itself.
#[derive(Debug, Clone, Copy, PartialEq, serde::Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum OpenOutcome {
    Launched,
    Focused,
    ProjectMissing,
    EngineMissing,
}

/// How long a spawned editor gets to prove it can boot. The wait thread
/// reports any exit inside this window as a refusal (the project gate exits
/// before a window exists); close-mode's delayed exit waits this out plus a
/// margin so the report always has a Hub left to show it.
const BOOT_WATCHDOG: std::time::Duration = std::time::Duration::from_secs(2);

/// Could this run reach the editor's POST-BOOT meaning of exit code 3?
///
/// THE COLLISION (Source/Programs/ArcaneEditor/src/main.cpp's exit table states it in full):
/// exit codes 2 and 3 mean different things depending on WHEN in the run they
/// fire, and the wait thread's decoder only sees a number and a duration.
///
/// * 2 -- pre-boot: the project gate (wrong abi, unreadable manifest).
///   post-boot: RenderErrorCount grew across the run.
/// * 3 -- pre-boot: the project is already open in another live editor.
///   post-boot: a golden capture/compare failed.
///
/// A run that reaches EditorApp::Run() has cleared both pre-boot refusals, so
/// from there on the later meanings apply -- but from outside the process the
/// Hub sees only the code, and any exit inside BOOT_WATCHDOG looks pre-boot.
///
/// ONLY CODE 3 IS DECIDABLE FROM THE ARGS, which is why this predicate covers
/// it alone. A post-boot 3 requires a golden COMPARE or a --settle run, and
/// both are DEV/DESK vocabulary this Hub never passes on its own -- the only
/// route is a user deliberately saving them into a project's extra launch
/// args. So their presence is real evidence.
///
/// CODE 2 HAS NO SUCH PREDICATE AND MUST NOT BE GIVEN ONE. This function's
/// ancestor also guarded code 2, keying on `--frames`/`--golden-`/`--nri-graph`
/// on the premise that only a run carrying `--nri-graph` took the graph path.
/// NRI Phase 5a (Task 2b) made the frame graph the only render path and
/// retired that flag to a parsed-and-ignored no-op, which falsified the
/// premise for EVERY launch since: an ordinary Hub launch carries none of
/// those tokens yet reaches the graph, and so can exit 2 for a render error
/// while being reported as an engine/abi refusal. The decoder names both
/// meanings instead.
///
/// `--frames` is likewise NOT evidence and is deliberately absent: on its own
/// it cannot produce a post-boot 2 or 3, so counting it only suppressed a
/// correct pre-boot diagnosis on short scripted runs.
///
/// TWO flags can produce a post-boot exit 3, and this keys on BOTH:
///
///  * `--compare` -- it is what asks for a golden comparison at all.
///  * `--settle`  -- both hosts fold settle NON-CONVERGENCE into exit 3 on
///    their own, with no compare anywhere in sight:
///    `RuntimeApp.cpp`'s ShutdownGraphPath (`settleFailed && m_graphExit == 0`
///    `-> 3`) and `EditorApp.cpp`'s line-for-line twin. The editor's own exit
///    table (`Source/Programs/ArcaneEditor/src/main.cpp`) names `settle-not-converged` and
///    `compare-failed` under the SAME code.
///
/// CORRECTION, 2026-08-28 (final review). This comment, and the arc spec's
/// Decision 8, both claimed `--compare` was "the only flag that can produce a
/// post-boot exit 3". THAT IS FALSE. Saved args of
/// `--headless --frames 60 --settle 30 --report r.json` that fail to converge
/// exit 3 with no `--compare` token present, and were reported to the user as
/// "that project is already open in another editor" -- the exact misreport
/// this predicate exists to eliminate, merely narrowed rather than closed.
///
/// MATCHING IS EXACT-OR-`=`, NEVER A BARE PREFIX. `--settle-timeout` is a REAL
/// flag this same arc added (HostConfig.cpp registers `settle` AND
/// `settle-timeout`), and a bare `starts_with("--settle")` would swallow it --
/// so a run that merely widened the settle TIME bound would read as evidence
/// of a settle run. Both `--flag value` and `--flag=value` are engine-accepted
/// forms (ArcaneCore `Cli.cpp` splits on the first `=` before the long lookup),
/// so both spellings are covered for each flag.
///
/// `--headless` and `--report` stay NON-evidence: both are reachable without
/// either exit-3 route. `--frames` stays non-evidence for the reason above.
fn golden_run(extra: &[String]) -> bool {
    extra.iter().any(|a| {
        a == "--compare" || a.starts_with("--compare=")
            || a == "--settle" || a.starts_with("--settle=")
    })
}

pub fn now_utc_iso() -> String {
    // Seconds since the epoch is enough to sort "last opened" and avoids
    // pulling a date crate in for a display string.
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    secs.to_string()
}

/// Which projects have a live editor: the in-memory map (editors THIS Hub
/// spawned, including mid-boot ones with no lock yet) merged with each
/// listed project's editor lock (editors that outlived a Hub restart, or
/// were launched by hand). Sorted so set-comparison and the emitted payload
/// are deterministic. Keys are `state::project_dir_key` of the recorded
/// project path: lower-cased, '/' separators, and the FOLDER of a manifest
/// (format.ts `projectKey` is the mirror the running badge must match with).
pub fn running_keys(app: &tauri::AppHandle) -> Vec<String> {
    let mut keys: std::collections::HashSet<String> = running_map(app).keys().cloned().collect();
    for e in state::load().recents {
        if editorlock::read_live(&resolve::project_dir(Path::new(&e.path))).is_some() {
            keys.insert(state::project_dir_key(&e.path));
        }
    }
    let mut keys: Vec<String> = keys.into_iter().collect();
    keys.sort();
    keys
}

/// Tell the frontend which projects have a live editor right now. Emitted on
/// every spawn and every exit (the disk watcher covers lock transitions the
/// map cannot see); the payload is the full key set rather than a delta, so
/// a missed event costs one stale badge until the next, not a permanently
/// wrong count.
pub fn emit_running(app: &tauri::AppHandle) {
    use tauri::Emitter;
    let _ = app.emit("running-changed", running_keys(app));
}

// The launch core, callable from BOTH entry points: the open_project command
// and shell_open (a double-clicked .arcproj). One brain -- the probe, the
// one-editor-per-project focus, the args, the wait thread -- whatever door
// the launch came through.
pub fn do_open_project(
    app: &tauri::AppHandle,
    project_path: String,
    engine_path: String,
) -> Result<OpenOutcome, String> {
    let proj = PathBuf::from(&project_path);
    let exe = engine::resolve_editor_exe(Path::new(&engine_path));

    // Loaded before anything else: the engine gate below and the project's
    // saved arguments both read it. A SNAPSHOT -- the write at the end goes
    // through state::mutate, never through this copy.
    let snapshot = state::load();

    // IPC TRUST: the engine must be one the user REGISTERED. `engine_path` is
    // an IPC argument, and without this gate any caller holding the IPC
    // could have the Hub execute an arbitrary exe with a project argument.
    // Registered-but-missing still gets the EngineMissing outcome below.
    if registered_engine(&snapshot.engines, &engine_path).is_none() {
        return Err(format!("{engine_path} is not a registered engine"));
    }

    if !proj.exists() {
        return Ok(OpenOutcome::ProjectMissing);
    }
    if !exe.exists() {
        return Ok(OpenOutcome::EngineMissing);
    }

    // One editor per project, checked BEFORE the probe: focusing a running
    // editor must not depend on the engine binary being able to answer for
    // itself (it may be mid-rebuild while its editor runs on). Two sources
    // of truth, in order:
    //  1. The in-memory map -- editors THIS Hub spawned, including one still
    //     mid-boot whose lock is not on disk yet. The entry is believed while
    //     its PROCESS is alive (editorlock::pid_alive), whether or not it has
    //     a window yet; only a dead pid is dropped as stale. This used to
    //     drop any pid whose window could not be found -- which is every
    //     editor in its first seconds -- and then spawn a rival against it,
    //     the very case rule 2 below exists to prevent.
    //  2. The editor's own lock in the project (pid + start-time validated,
    //     editorlock.rs) -- which survives Hub restarts, so close-mode still
    //     guards. A live lock refuses the rival spawn even when its window
    //     cannot be focused yet: an editor mid-boot has no window, and
    //     spawning a second editor against it is exactly the
    //     two-editors-one-project case this exists to prevent.
    let project_key = state::project_dir_key(&project_path);
    let mapped = running_map(app).get(&project_key).copied();
    if let Some(pid) = mapped {
        if editorlock::pid_alive(pid) {
            let _ = spawn::focus_process_window(pid);
            return Ok(OpenOutcome::Focused);
        }
        // Dead: drop it -- but only if it is still THAT pid; a concurrent
        // launch may have replaced it while the lock was released.
        let mut live = running_map(app);
        if live.get(&project_key) == Some(&pid) {
            live.remove(&project_key);
        }
    }
    if let Some(pid) = editorlock::read_live(&resolve::project_dir(&proj)) {
        let _ = spawn::focus_process_window(pid);
        return Ok(OpenOutcome::Focused);
    }

    // Probe RIGHT BEFORE spawning, not only at registration or Hub launch: a
    // long-lived Hub spans in-place rebuilds, and this is the last moment the
    // compatibility story can be made true. A probe failure refuses the launch
    // with a reason -- an engine that cannot answer its own identity probe
    // (mid-rebuild, missing DLLs) is not about to open a project, and spawning
    // it anyway would produce a silent nothing.
    let info = spawn::probe_engine(&exe)?;

    // Loaded before the spawn so this project's saved extra arguments can go on
    // the command line. Passed as an argv ARRAY, never a joined string: each
    // token reaches the child exactly as typed, with no shell in between.
    let extra: Vec<String> = snapshot
        .recents
        .iter()
        .find(|e| state::project_dir_key(&e.path) == project_key)
        .map(|e| project::split_args(&e.args))
        .unwrap_or_default();

    // Can this run produce a POST-BOOT exit 3? (See golden_run's own comment
    // for the whole exit-code collision, and for why exit 2 gets no such
    // predicate.) The args appended above are the user's own saved tokens,
    // verbatim, so this is the one place that can ask.
    let golden = golden_run(&extra);

    let mut child = Command::new(&exe)
        .arg("--project")
        .arg(&proj)
        // AFTER --project, so a later flag wins if the editor takes the last
        // occurrence -- the user's own switches should override, not be
        // overridden by, the one the Hub always passes.
        .args(&extra)
        // The editor resolves shaders and plugin DLLs relative to its own
        // directory, so it must start there (same reason scripts/launch.ps1
        // sets the working directory).
        .current_dir(exe.parent().unwrap_or(Path::new(".")))
        .creation_flags(spawn::CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| format!("could not launch the editor: {e}"))?;

    let pid = child.id();
    running_map(app).insert(project_key.clone(), pid);
    emit_running(app);

    // Hand the screen to the editor, per the configured behaviour. Checked
    // AFTER a successful spawn so a refused launch never hides the window the
    // error banner lives in. `stay` does nothing by definition.
    match settings::load().launch_behavior.as_str() {
        settings::BEHAVIOR_TRAY => {
            let _ = tray::park(app);
        }
        settings::BEHAVIOR_CLOSE => {
            // Really close -- but not this instant: hide now (the window is
            // gone from the user's view immediately), then exit only after
            // the boot watchdog has had its window. If the editor dies
            // inside it, the wait thread removes the pid and shows the Hub
            // with the reason, and the check below finds the key gone and
            // spares the process. Editors always outlive the Hub; the launch
            // guard still holds afterwards through the editor's own lock
            // (editorlock.rs), which is how close-mode stays safe at all.
            //
            // Exit waits for in-flight disk work (Duplicate, Scan, Rename,
            // Delete, Create -- the `busy()` guards) to drain, bounded by
            // EXIT_DRAIN_MAX: a timer-driven exit used to kill a Duplicate
            // mid-copy. And it never closes a window the user has brought
            // BACK meanwhile (a relaunch, the drain's long wait): exiting
            // under someone who is using the Hub is not "close after launch".
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.hide();
            }
            let app = app.clone();
            let key = project_key.clone();
            std::thread::spawn(move || {
                std::thread::sleep(BOOT_WATCHDOG + Duration::from_millis(500));
                let alive = running_map(&app).get(&key) == Some(&pid);
                if !alive {
                    return;
                }
                wait_idle(EXIT_DRAIN_MAX);
                let visible = app
                    .get_webview_window("main")
                    .and_then(|w| w.is_visible().ok())
                    .unwrap_or(false);
                if !visible {
                    app.exit(0);
                }
            });
        }
        _ => {}
    }

    // The wait thread owns the child for its WHOLE lifetime (this is what
    // makes the live-editors map truthful). Three jobs on exit: drop the map
    // entry; surface a boot-refusal (an exit inside 2s -- the project gate
    // exits before any window, and a detached launch would otherwise be
    // click-then-nothing); and when the LAST editor is gone, restore the Hub
    // (re-read the setting at exit time, so toggling it mid-run behaves).
    // stderr is deliberately NOT piped: a pipe nobody drains would eventually
    // BLOCK the editor mid-log -- the exit code has to carry the story.
    {
        let app = app.clone();
        let shown = project::display_name(&project_path);
        let key = project_key.clone();
        let lock_root = resolve::project_dir(&proj);
        std::thread::spawn(move || {
            let started = std::time::Instant::now();
            let status = child.wait().ok();
            // Handle still held (`child` is in scope): tell 3 is why sweep
            // can delete the file the editor may not have Clear'd (crash,
            // kill, Init-failed-after-write). A live rival is untouched.
            let _ = editorlock::sweep_stale(&lock_root);

            use tauri::Emitter;
            let none_left = {
                let mut live = running_map(&app);
                // Only OUR entry: if this pid was dropped as dead and the
                // project relaunched meanwhile, the key now names the NEW
                // editor, and removing it would unguard a live editor.
                if live.get(&key) == Some(&pid) {
                    live.remove(&key);
                }
                live.is_empty()
            };
            emit_running(&app);

            if started.elapsed() < BOOT_WATCHDOG {
                let why = match status.and_then(|s| s.code()) {
                    // Exit 2 IS AMBIGUOUS AND NO ARG CAN DISAMBIGUATE IT.
                    // Pre-boot it is the editor's project gate (wrong abi,
                    // unreadable manifest); post-boot it is "RenderErrorCount
                    // grew". This arm used to claim the gate outright whenever
                    // the saved args looked unscripted -- correct only while
                    // `--nri-graph` selected the render path. Phase 5a made the
                    // NRI graph unconditional, so an ORDINARY launch (no saved
                    // args at all) can exit 2 for a render error, and the
                    // confident wording sent the user hunting an abi gate that
                    // was never involved. Name both, in the order they are
                    // worth checking, and let the log settle it.
                    Some(2) => {
                        "the editor exited with code 2 -- either it refused the project \
                         (engine/abi gate) or the render path reported errors during the \
                         run; the editor's log says which"
                            .to_string()
                    }
                    // Exit 3 is the editor's OWN double-open refusal (it
                    // focused the rival before exiting). Rare from here --
                    // this Hub's guard focuses first -- but reachable when a
                    // rival appeared between the guard and the editor's boot.
                    // This one KEEPS its guard: post-boot 3 is a golden
                    // capture/compare failure, which is unreachable without
                    // golden vocabulary in the args.
                    Some(3) if !golden => {
                        "that project is already open in another editor".to_string()
                    }
                    Some(3) => {
                        "a golden capture or compare failed (exit 3)".to_string()
                    }
                    Some(c) => format!("the editor exited immediately with code {c}"),
                    None => "the editor was killed before it opened".to_string(),
                };
                let _ = app.emit("launch-failed", format!("{shown}: {why}."));
            }

            // A hidden window ALWAYS restores when the last editor exits,
            // whatever hid it. Tray hands the Hub back by design (user call
            // 2026-07-29: the icon is for DURING the run); close-mode only
            // reaches here when the delayed exit was spared by a
            // boot-refusal, where showing the Hub with the reason is
            // exactly right. show_hub clears the tray icon along the way.
            // `stay` windows are never hidden, so the guard alone keeps
            // them from stealing focus from whatever the user moved on to.
            if none_left {
                let hidden = app
                    .get_webview_window("main")
                    .map(|w| !w.is_visible().unwrap_or(true))
                    .unwrap_or(false);
                if hidden {
                    tray::show_hub(&app);
                }
                // Regardless of the window: the list's opened-times and
                // missing flags deserve a refresh when an editor ends.
                let _ = app.emit("editors-idle", ());
            }
        });
    }
    // display_name, not file_name(): project_path may be a .arcproj FILE (what
    // the Open dialog now yields) or a folder (what older entries hold), and
    // both must record the same name.
    let name = project::display_name(&project_path);

    // The PROJECT's abi, from its manifest -- NOT the abi of the engine we just
    // launched. Recording the launching engine's abi made the card claim the
    // project was compatible with the very engine that had just refused it
    // (Runtime.cpp:387), so an incompatible project could never look
    // incompatible. 0 = unknown, which isCompatible treats as "no conflict
    // provable" rather than as a fault.
    let abi = resolve::manifest_abi(&proj).unwrap_or(0);

    // The manifest's identity, and the moved-folder heal it enables: launching
    // a project at a NEW path (shell double-click after a move is the common
    // case) repoints the old missing entry -- star, pin, arguments intact --
    // and the touch below then carries all of that forward, instead of a
    // stranger row appearing while the old one sits greyed forever.
    // heal_by_guid itself refuses when this path is ALREADY listed (a clone
    // or worktree sharing the guid with a missing sibling).
    let guid = resolve::manifest_guid(&proj);
    let proj_path = proj.to_string_lossy().to_string();

    // Every disk read above happened OUTSIDE the state lock; only the edit
    // is inside it (state::mutate).
    state::mutate(|s| {
        // Adopt what the probe just said: the entry's cached abi/build
        // refresh so the UI's next load speaks about the binary that
        // actually launched.
        let exe_key = state::normalise_path(&exe.to_string_lossy());
        if let Some(e) = s.engines.iter_mut().find(|e| e.id == exe_key) {
            e.engine_abi = info.engine_abi;
            e.build.clone_from(&info.build);
        }
        if let Some(g) = &guid {
            state::heal_by_guid(&mut s.recents, g, &proj_path, &name, abi);
        }
        state::touch_recent(
            &mut s.recents,
            state::RecentProject {
                path: proj_path.clone(),
                name: name.clone(),
                last_opened_utc: now_utc_iso(),
                engine_abi: abi,
                // None, NOT the engine just used: launching is not the same
                // as choosing. touch_recent carries an existing pin across,
                // so a pinned project stays pinned and an unpinned one keeps
                // following the default instead of silently pinning itself.
                engine_id: None,
                // Empty for the same reason: touch_recent carries the saved
                // arguments across, so launching never wipes them.
                args: String::new(),
                // False for the same reason: touch_recent ORs the previous
                // star in, so launching never unstars.
                favorite: false,
                // The project was just found on disk to launch; load()
                // re-stamps this on every read regardless.
                missing: false,
                // The identity read above; None (pre-guid manifest) lets
                // touch_recent keep whatever was already known.
                guid: guid.clone(),
            },
        );
        Ok(())
    })?;
    Ok(OpenOutcome::Launched)
}

/// The registered engine `engine_path` names (an exe or its folder, matched
/// by id -- the normalised exe path), or None. The IPC gate open_project
/// applies before it will execute anything. Pure for the test.
pub fn registered_engine<'a>(
    engines: &'a [state::EngineEntry],
    engine_path: &str,
) -> Option<&'a state::EngineEntry> {
    let exe = engine::resolve_editor_exe(Path::new(engine_path.trim()));
    let id = state::normalise_path(&exe.to_string_lossy());
    engines.iter().find(|e| e.id == id)
}

/// The shell route's engine choice, mirroring what a fresh Hub session's
/// card click would use: the project's pin when it is listed and the pin
/// resolves; else the persisted default engine (Settings::default_engine_id)
/// when it resolves; else the first registered engine. A dangling pin or a
/// dangling default falls through rather than refusing the launch. None =
/// nothing registered. Pure, and extracted precisely so this policy is
/// TESTED -- it used to live inline in untestable skin.
pub fn shell_engine<'a>(
    s: &'a state::HubState,
    path: &str,
    default_engine_id: Option<&str>,
) -> Option<&'a state::EngineEntry> {
    let key = state::project_dir_key(path);
    let by_id = |id: &str| s.engines.iter().find(|en| en.id == id);
    s.recents
        .iter()
        .find(|e| state::project_dir_key(&e.path) == key)
        .and_then(|e| e.engine_id.as_deref())
        .and_then(by_id)
        .or_else(|| default_engine_id.and_then(by_id))
        .or_else(|| s.engines.first())
}

/// The project a shell invocation names, made absolute against the INVOKING
/// process's working directory. A second instance started as
/// `arcane_hub.exe MyGame.arcproj` from a console forwards a RELATIVE path,
/// and resolving it against the first instance's cwd (wherever the Hub was
/// started) opened the wrong file or none. Pure for the test.
pub fn shell_target(arg: &str, cwd: Option<&Path>) -> String {
    let p = Path::new(arg);
    match cwd {
        Some(dir) if !p.is_absolute() => dir.join(p).to_string_lossy().to_string(),
        _ => arg.to_string(),
    }
}

// Route a shell-opened .arcproj (double-click, Open With, drag onto the exe)
// through the SAME launch core a card click takes -- probe, focus-existing,
// auto-listing via touch_recent, the lifecycle. No engines at all -> surface
// the window and say so; a launcher with nothing to launch with must not eat
// a double-click silently.
fn shell_open(app: &tauri::AppHandle, path: &str) {
    use tauri::Emitter;
    let s = state::load();
    let default_id = settings::load().default_engine_id;
    let Some(engine) = shell_engine(&s, path, default_id.as_deref()) else {
        tray::show_hub(app);
        let _ = app.emit(
            "launch-failed",
            format!(
                "{}: register an engine in the Hub before opening projects.",
                project::display_name(path)
            ),
        );
        return;
    };
    let (engine_path, engine_shown) = (engine.path.clone(), engine.build.clone());

    match do_open_project(app, path.to_string(), engine_path.clone()) {
        Ok(OpenOutcome::Launched) | Ok(OpenOutcome::Focused) => {
            // The list may have just gained this project. The disk watcher
            // would notice within a tick; telling the (possibly hidden but
            // alive) frontend now keeps the list honest the moment the
            // window comes back.
            let _ = app.emit("state-changed", &state::load());
            // On a shell route the window starts hidden, and only `stay`
            // earns a show: tray/close launches go straight to the editor
            // with no Hub flash (the editor's own window takes the
            // foreground when it appears, moments after this).
            if settings::load().launch_behavior == settings::BEHAVIOR_STAY {
                tray::show_hub(app);
            }
        }
        Ok(OpenOutcome::ProjectMissing) => {
            tray::show_hub(app);
            let _ = app.emit(
                "launch-failed",
                format!("{path}: the file vanished before it could open."),
            );
        }
        Ok(OpenOutcome::EngineMissing) => {
            tray::show_hub(app);
            let _ = app.emit(
                "launch-failed",
                format!(
                    "{}: {engine_shown} is no longer at {engine_path}. See Engines.",
                    project::display_name(path)
                ),
            );
        }
        Err(e) => {
            tray::show_hub(app);
            let _ = app.emit("launch-failed", e);
        }
    }
}

/// Spawn shell_open on its own thread for an argv that carries a .arcproj
/// (the probe may block up to its 10s timeout, and neither setup() nor the
/// single-instance callback may stall that long). Returns whether it did, so
/// the single-instance callback can tell a plain "show me the Hub" relaunch
/// from one that was really a file open. `cwd` is the INVOKING process's
/// working directory (see shell_target).
pub fn route_shell_args<'a>(
    app: &tauri::AppHandle,
    args: impl IntoIterator<Item = &'a str>,
    cwd: Option<&Path>,
) -> bool {
    let Some(p) = project::arcproj_in_args(args) else {
        return false;
    };
    let app = app.clone();
    let path = shell_target(p, cwd);
    std::thread::spawn(move || shell_open(&app, &path));
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hub(recents: Vec<state::RecentProject>, engines: Vec<state::EngineEntry>) -> state::HubState {
        state::HubState { recents, engines, ..Default::default() }
    }

    fn rp(path: &str, pin: Option<&str>) -> state::RecentProject {
        state::RecentProject {
            path: path.to_string(),
            name: "N".to_string(),
            last_opened_utc: "1".to_string(),
            engine_abi: 8,
            engine_id: pin.map(|s| s.to_string()),
            args: String::new(),
            favorite: false,
            missing: false,
            guid: None,
        }
    }

    fn ee(path: &str) -> state::EngineEntry {
        state::EngineEntry::new(path, 8, "Arcane".to_string())
    }

    #[test]
    fn shell_engine_prefers_a_live_pin() {
        let a = ee("C:/eng-a/ArcaneEditor.exe");
        let b = ee("C:/eng-b/ArcaneEditor.exe");
        let pin = b.id.clone();
        let s = hub(vec![rp("D:/G/My/My.arcproj", Some(&pin))], vec![a, b]);
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj", None).unwrap().id, pin);
        // The folder-vs-manifest fold: a double-clicked FILE must find the
        // entry however it was recorded.
        assert_eq!(shell_engine(&s, "D:/G/My", None).unwrap().id, pin);
    }

    #[test]
    fn shell_engine_falls_back_to_the_first_engine() {
        let a = ee("C:/eng-a/ArcaneEditor.exe");
        let first = a.id.clone();
        // Unlisted project -> first engine (a fresh session's default).
        let s = hub(vec![], vec![a, ee("C:/eng-b/ArcaneEditor.exe")]);
        assert_eq!(shell_engine(&s, "D:/G/New/New.arcproj", None).unwrap().id, first);
        // A DANGLING pin (engine no longer registered) falls back the same
        // way rather than refusing the launch.
        let s = hub(vec![rp("D:/G/My/My.arcproj", Some("gone"))],
                    vec![ee("C:/eng-a/ArcaneEditor.exe")]);
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj", None).unwrap().id, first);
    }

    #[test]
    fn shell_engine_with_nothing_registered_is_none() {
        let s = hub(vec![rp("D:/G/My/My.arcproj", None)], vec![]);
        assert!(shell_engine(&s, "D:/G/My/My.arcproj", None).is_none());
        assert!(shell_engine(&s, "D:/G/My/My.arcproj", Some("c:/eng/arcaneeditor.exe")).is_none());
    }

    #[test]
    fn shell_engine_uses_the_persisted_default_before_the_first_engine() {
        // The Settings default (Engines' "make default") is what a fresh
        // session's card click would pick; the shell route must agree.
        let a = ee("C:/eng-a/ArcaneEditor.exe");
        let b = ee("C:/eng-b/ArcaneEditor.exe");
        let (first, default) = (a.id.clone(), b.id.clone());
        let s = hub(vec![rp("D:/G/My/My.arcproj", None)], vec![a, b]);
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj", Some(&default)).unwrap().id, default);
        assert_eq!(shell_engine(&s, "D:/G/Unlisted.arcproj", Some(&default)).unwrap().id, default);
        // A DANGLING default (that engine was forgotten) falls back to first.
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj", Some("gone")).unwrap().id, first);
    }

    #[test]
    fn shell_engine_lets_a_live_pin_beat_the_default() {
        let a = ee("C:/eng-a/ArcaneEditor.exe");
        let b = ee("C:/eng-b/ArcaneEditor.exe");
        let (pin, default) = (a.id.clone(), b.id.clone());
        let s = hub(vec![rp("D:/G/My/My.arcproj", Some(&pin))], vec![a, b]);
        assert_eq!(shell_engine(&s, "D:/G/My/My.arcproj", Some(&default)).unwrap().id, pin);
    }

    #[test]
    fn registered_engine_matches_by_id_and_refuses_strangers() {
        // The IPC gate on open_project: an unregistered exe is never run.
        let engines = vec![ee("C:/Eng/ArcaneEditor.exe")];
        assert!(registered_engine(&engines, r"c:\eng\arcaneeditor.exe").is_some());
        assert!(registered_engine(&engines, "C:/Eng").is_some(), "the folder form resolves");
        assert!(registered_engine(&engines, "C:/Evil/ArcaneEditor.exe").is_none());
        assert!(registered_engine(&engines, "C:/Windows/System32/cmd.exe").is_none());
        assert!(registered_engine(&[], "C:/Eng/ArcaneEditor.exe").is_none());
    }

    #[test]
    fn shell_target_resolves_a_relative_arg_against_the_invoking_cwd() {
        let cwd = Path::new("D:/Games/My");
        assert_eq!(
            PathBuf::from(shell_target("My.arcproj", Some(cwd))),
            PathBuf::from("D:/Games/My/My.arcproj")
        );
        // Absolute stays as given, cwd or not.
        assert_eq!(shell_target("C:/X/X.arcproj", Some(cwd)), "C:/X/X.arcproj");
        assert_eq!(shell_target("My.arcproj", None), "My.arcproj");
    }

    #[test]
    fn busy_guards_hold_the_exit_until_they_drop() {
        // Close-mode's exit waits on these. A dropped guard (an early `?`
        // return included) must release, or the Hub would never close.
        let g = busy();
        assert!(!wait_idle(Duration::from_millis(150)), "in flight: not idle");
        let t = std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(100));
            drop(g);
        });
        assert!(wait_idle(Duration::from_secs(10)), "idle once the guard drops");
        t.join().unwrap();
    }

    fn args(v: &[&str]) -> Vec<String> {
        v.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn golden_run_keys_on_flags_the_engine_actually_registers() {
        // --compare is ONE of the two routes to a post-boot exit 3: it is what
        // asks for a golden comparison at all.
        assert!(golden_run(&args(&["--compare", "runtime-scene"])));
        assert!(golden_run(&args(&["--compare=runtime-scene"])));
        assert!(golden_run(&args(&["--headless", "--frames", "60", "--compare", "editor-ui"])));

        // THE OTHER ROUTE, and the bug the final review caught: BOTH hosts fold
        // settle non-convergence into exit 3 with no --compare involved
        // (RuntimeApp.cpp's ShutdownGraphPath and EditorApp.cpp's twin). These
        // exact saved args used to be decoded as "already open in another
        // editor" -- the very misreport this predicate exists to eliminate.
        assert!(golden_run(&args(&["--settle", "30"])));
        assert!(golden_run(&args(&["--settle=30"])));
        assert!(golden_run(&args(&[
            "--headless", "--frames", "60", "--settle", "30", "--report", "r.json"
        ])));

        // THE TRAP, and the reason the match is exact-or-`=` and never a bare
        // prefix: --settle-timeout is a REAL flag (HostConfig.cpp registers it
        // beside --settle). It widens the settle TIME bound; on its own it is
        // NOT a settle run -- HostConfig refuses it without --settle -- so it
        // must not read as exit-3 evidence. A starts_with("--settle") would
        // make every one of these pass wrongly.
        assert!(!golden_run(&args(&["--settle-timeout", "5000"])));
        assert!(!golden_run(&args(&["--settle-timeout=5000"])));
        assert!(!golden_run(&args(&["--headless", "--settle-timeout", "5000"])));
        // ...while the pair TOGETHER is evidence, on the --settle token alone.
        assert!(golden_run(&args(&["--settle", "30", "--settle-timeout", "5000"])));

        // THE BUG THIS PINS: --golden-* is retired vocabulary that no engine
        // binary registers. Keying on it made the exit-3 guard always match,
        // so a real compare failure was reported as "already open in another
        // editor" and the correct arm was unreachable.
        assert!(!golden_run(&args(&["--golden-capture", "D:/out"])));

        // Neither is --frames evidence on its own -- it cannot produce a
        // post-boot 2 or 3, which is why it was deliberately absent before.
        assert!(!golden_run(&args(&["--frames", "5"])));
        assert!(!golden_run(&args(&[])));

        // --headless and --report are reachable without either exit-3 route,
        // so neither is evidence by itself.
        assert!(!golden_run(&args(&["--headless"])));
        assert!(!golden_run(&args(&["--headless", "--frames", "60", "--report", "r.json"])));
    }

    #[test]
    fn golden_run_is_false_for_an_ordinary_launch() {
        // THE BUG THIS PINS: an ordinary launch carries no saved args at all,
        // and since the Phase 5a flip it still reaches the NRI graph -- so its
        // exit 2 must NOT be decoded through a "scripted" predicate. Nothing
        // here may ever start returning true.
        assert!(!golden_run(&[]));
        assert!(!golden_run(&args(&["--backend", "vulkan"])));
        assert!(!golden_run(&args(&["--frames", "5"])));
        assert!(!golden_run(&args(&["--frames=5"])));
        // `--nri-graph` is a retired no-op (Phase 5a, Task 2b). A stale saved
        // copy of it says nothing about the run and must not suppress the
        // pre-boot double-open diagnosis.
        assert!(!golden_run(&args(&["--nri-graph"])));
        assert!(!golden_run(&args(&["--nri-graph", "--frames", "2"])));
    }
}
