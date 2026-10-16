// Arcane Hub -- Tauri entry point.
//
// Split, mirroring Tools/setup-wizard: all DECISION logic lives in pure
// modules that `cargo test` covers; the IO halves each own one story --
// spawn.rs (process spawning), resolve.rs (recorded-path resolution + disk
// choreography), launch.rs (the launch lifecycle: the launch core, its wait
// thread, the shell route, the running set), watch.rs (the background disk
// poll) -- and THIS file is only the #[tauri::command] IPC surface plus the
// state read-modify-write each command performs, and the run() builder.
// A command with a body longer than a screen belongs in one of those halves;
// this file earned that rule by quietly growing to 1,100 untested lines
// before the 2026-07-29 split restored it.
//
// Two rules every command here follows:
//
//  * STATE CHANGES GO THROUGH state::mutate -- one process-wide lock around
//    load-modify-save. Probes, manifest reads and disk work happen BEFORE
//    (outside) it, so a seconds-long probe never holds the lock.
//  * ANYTHING THAT PROBES, LOADS STATE (a stat per listed project), READS
//    COVERS OR TOUCHES A PROJECT ON DISK IS `#[tauri::command(async)]`.
//    Tauri 2 runs a plain sync command on the MAIN thread, so a probe (up to
//    its 10s timeout) or a slow network drive froze the whole window. Only
//    the trivially cheap commands (settings, version, window) stay sync.
//
// The Hub is INSTALLED per-user (%LOCALAPPDATA%\Arcane Hub\), so it must
// never derive a repo root from current_exe() and must never assume an
// engine sits beside it. That is setup-wizard's repo-root-portable assumption
// and it is wrong here. Engines are REGISTERED BY PATH and validated with the
// engine's own --print-engine-info probe.

pub mod assoc;
pub mod editorlock;
pub mod engine;
pub mod launch;
pub mod paths;
pub mod project;
pub mod resolve;
pub mod settings;
pub mod spawn;
pub mod state;
pub mod store;
pub mod tray;
pub mod watch;

use std::collections::HashMap;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

use launch::{OpenOutcome, RunningEditors};

/// "'<path>' is not in the project list" -- the one wording every
/// membership check uses (the frontend shows it as-is).
fn not_listed(path: &str) -> String {
    format!("'{path}' is not in the project list")
}

/// Is `path` (either recorded shape) in the list right now? The IPC gate for
/// the commands that act on a project's files: a path the user never listed
/// must not be deletable or copyable just because a caller named it.
fn is_listed(path: &str) -> bool {
    let key = state::project_dir_key(path);
    state::load().recents.iter().any(|e| state::project_dir_key(&e.path) == key)
}

#[tauri::command(async)]
fn load_state() -> state::HubState {
    state::load()
}

// Register an engine by the path the user picked or pasted. The path is
// CANONICALISED before the id is derived from it (engine::canonical_editor_
// exe): quotes stripped, required absolute, folder resolved to the exe, the
// disk's own spelling with no \\?\ prefix. It used to be stored as typed, so
// one engine pasted two ways registered twice, and a quoted paste failed.
#[tauri::command(async)]
fn register_engine(path: String) -> Result<state::EngineEntry, String> {
    let exe = engine::canonical_editor_exe(&path)?;
    let info = spawn::probe_engine(&exe)?;

    let entry = state::EngineEntry::new(&exe.to_string_lossy(), info.engine_abi, info.build);
    state::mutate(|s| {
        state::upsert_engine(&mut s.engines, entry.clone());
        Ok(())
    })?;
    Ok(entry)
}

// Re-probe every registered engine and refresh its cached identity (abi/build).
//
// Registration caches the probe's answer, and in the dev loop the SAME exe
// path is rebuilt in place daily -- so a cached abi goes stale the moment the
// engine bumps, and the compatibility display then inverts: a project stamped
// by the CURRENT engine reads as incompatible against last week's number
// (caught live in the 2026-07-28 review: entry said abi 7, the exe answered 8).
// Called once per launch from the frontend, AFTER the first paint -- each probe
// is milliseconds, but it is still a process spawn per engine and does not
// belong on every refresh().
//
// The probes run on a SNAPSHOT, outside the state lock; only the results are
// applied under state::mutate, and only to engines still registered by then.
// This command used to load, probe for seconds, then save the stale snapshot
// -- erasing any star, launch or registration that landed meanwhile.
//
// An engine that fails its probe keeps its cached data: failure proves nothing
// new (the exe may be mid-rebuild), and open_project still reports the truth
// at launch time.
#[tauri::command(async)]
fn refresh_engines() -> state::HubState {
    let probed: Vec<(String, u32, String)> = state::load()
        .engines
        .iter()
        .filter_map(|e| {
            spawn::probe_engine(Path::new(&e.path))
                .ok()
                .map(|info| (e.id.clone(), info.engine_abi, info.build))
        })
        .collect();

    let applied = state::mutate(|s| {
        for (id, abi, build) in &probed {
            if let Some(e) = s.engines.iter_mut().find(|e| &e.id == id) {
                e.engine_abi = *abi;
                e.build.clone_from(build);
            }
        }
        Ok(s.clone())
    });
    match applied {
        Ok(s) => s,
        Err(err) => {
            let mut s = state::load();
            s.warnings.push(format!("Could not save refreshed engine info: {err}"));
            s
        }
    }
}

// Resolves a folder to the exe the same way register_engine does, so the two
// are symmetric: without it, forget_engine("C:/eng") matched nothing and
// reported success. Absence stays a silent no-op on purpose -- removing an
// engine twice is idempotent, and an error banner for "already gone" would be
// noise. Contrast set_project_engine, where a miss means state would be wrong.
#[tauri::command(async)]
fn forget_engine(path: String) -> Result<(), String> {
    let exe = engine::resolve_editor_exe(Path::new(&path));
    // The pin is an engine id, which IS the normalised exe path.
    let id = state::normalise_path(&exe.to_string_lossy());
    state::mutate(|s| {
        state::remove_engine(&mut s.engines, &exe.to_string_lossy());
        // Projects pinned to it fall back to the default rather than dangling.
        state::unpin_engine(&mut s.recents, &id);
        Ok(())
    })
}

// Pin a project to a specific registered engine, or pass null to send it back
// to following the Hub default.
//
// Both arguments are VALIDATED. This is an IPC surface, and writing a pin to an
// unregistered engine would create exactly the dangling state unpin_engine
// exists to prevent -- silently, and persisted.
#[tauri::command(async)]
fn set_project_engine(path: String, engine_id: Option<String>) -> Result<(), String> {
    state::mutate(|s| {
        if let Some(id) = &engine_id {
            if !s.engines.iter().any(|e| &e.id == id) {
                return Err(format!("no engine is registered with id '{id}'"));
            }
        }
        if !state::set_project_engine(&mut s.recents, &path, engine_id.clone()) {
            return Err(not_listed(&path));
        }
        Ok(())
    })
}

// Delete a project from disk, then drop it from the list.
//
// To the RECYCLE BIN, never remove_dir_all: this erases a whole project folder
// on one confirmation, and the difference between "recoverable" and "gone" is
// the difference between a bad afternoon and a lost one. `trash` goes through
// the Windows shell's IFileOperation, which is the same path Explorer's Delete
// takes, so the folder lands somewhere the user already knows how to restore
// from.
//
// Four things have to hold before anything is deleted:
//   1. the path is IN THE LIST -- an IPC caller cannot aim this at a folder
//      the user never listed;
//   2. the FOLDER is this project's alone (resolve::resolve_owned_project):
//      a folder-shaped path must hold exactly one .arcproj, and a
//      manifest-shaped path must be the ONLY .arcproj in its folder. The
//      lenient resolve_project used to take a manifest at its word, so
//      deleting D:/Games/A.arcproj sent all of D:/Games -- every other
//      project in it -- to the bin;
//   3. it is absolute and is not a drive root (project::delete_guard);
//   4. it still exists -- and if it does not, the entry is simply unlisted.
//      A project already gone from disk still has to be removable, or its row
//      would be stuck in the list with no control that can shift it.
#[tauri::command(async)]
fn delete_project(path: String) -> Result<(), String> {
    if !is_listed(&path) {
        return Err(not_listed(&path));
    }
    let _busy = launch::busy();
    let recorded = PathBuf::from(&path);

    if recorded.exists() {
        let (root, _) = resolve::resolve_owned_project(&recorded, "delete")?;
        if let Some(why) = project::delete_guard(&root) {
            return Err(format!("refusing to delete {}: {why}", root.display()));
        }
        trash::delete(&root).map_err(|e| {
            format!(
                "could not delete {}: {e}. Is the project open in the editor?",
                root.display()
            )
        })?;
    }

    state::mutate(|s| {
        state::remove_recent(&mut s.recents, &path);
        Ok(())
    })
}

// Star or unstar a project. The frontend pins favourites above the rest.
#[tauri::command(async)]
fn set_project_favorite(path: String, favorite: bool) -> Result<(), String> {
    state::mutate(|s| {
        if !state::set_favorite(&mut s.recents, &path, favorite) {
            return Err(not_listed(&path));
        }
        Ok(())
    })
}

// Extra arguments appended after `--project <path>` when this project launches.
#[tauri::command(async)]
fn set_project_args(path: String, args: String) -> Result<(), String> {
    state::mutate(|s| {
        if !state::set_project_args(&mut s.recents, &path, args.trim()) {
            return Err(not_listed(&path));
        }
        Ok(())
    })
}

// Open the project's folder in Explorer.
#[tauri::command]
fn reveal_project(path: String) -> Result<(), String> {
    let recorded = PathBuf::from(&path);
    let dir = resolve::project_dir(&recorded);
    if !dir.is_dir() {
        return Err(format!("{} is no longer on disk", dir.display()));
    }

    let mut cmd = Command::new("explorer");
    if recorded.is_file() {
        // `/select,<file>` opens the folder with the manifest highlighted.
        // raw_arg, not arg: Command would quote the whole `/select,C:\My
        // Games\x.arcproj` token as one string once it contains a space, and
        // explorer parses its own command line and does not accept that form.
        cmd.raw_arg(format!("/select,\"{}\"", recorded.display()));
    } else {
        cmd.arg(&dir);
    }
    // NOT status-checked: explorer.exe returns a non-zero exit code even when
    // it opens the window successfully.
    cmd.spawn()
        .map_err(|e| format!("could not open {}: {e}", dir.display()))?;
    Ok(())
}

// Rename a project: its folder, its .arcproj file, the `name` inside that
// manifest (resolve::rename_project_files, tested there -- including the
// case-only rename the old inline version skipped), then the Hub's own entry.
//
// This is safe to do because NOTHING inside a project stores an absolute path.
// `Project::Open` derives the root from whatever path it is handed, mounts
// `game://` at `<root>/Content` and REBUILDS the Guid -> mount-path map from
// scratch on every open (AssetRegistry::ScanContent), with the GUIDs living in
// the asset files or their `.meta` sidecars -- so they travel with the files.
// `bootScene` is a `game://` URI and plugins resolve at `<root>/Plugins/<name>`.
// The manifest's `name` has one consumer, the editor window title
// (EditorApp.cpp:95).
//
// What this deliberately does NOT touch: `gameModule` (build output named by
// the project's own build scripts), anything under Source/, and generated
// solution files. Renaming a project is not renaming its game module, and
// quietly rewriting build inputs would be worse than leaving them alone.
#[tauri::command(async)]
fn rename_project(path: String, new_name: String) -> Result<String, String> {
    let new_name = new_name.trim().to_string();
    if let Some(why) = project::name_error(&new_name) {
        return Err(why);
    }

    // Membership is checked BEFORE any disk work, so a stranger path is
    // refused while nothing has moved.
    if !is_listed(&path) {
        return Err(not_listed(&path));
    }
    let _busy = launch::busy();
    let new_manifest = resolve::rename_project_files(Path::new(&path), &new_name)?;

    let new_path = new_manifest.to_string_lossy().to_string();
    state::mutate(|s| {
        // A false here means the entry was removed while the disk work ran;
        // the folder is renamed regardless, and there is nothing to repoint.
        state::rename_recent(&mut s.recents, &path, &new_path, &new_name);
        Ok(())
    })?;
    Ok(new_path)
}

// Duplicate a project on disk beside the original and list the copy.
//
// `async fn` ON PURPOSE: this is the one command whose duration scales with
// the project (Content can be gigabytes), and a sync command runs on the main
// thread with the whole window frozen for the duration. On the async runtime
// the frontend's busy state stays honest instead. The disk half lives in
// resolve::duplicate_project_files (tested there); this is the listing.
#[tauri::command]
async fn duplicate_project(path: String) -> Result<String, String> {
    if !is_listed(&path) {
        return Err(not_listed(&path));
    }
    let _busy = launch::busy();
    let dup = resolve::duplicate_project_files(Path::new(&path))?;

    // List the copy. The engine pin and arguments carry from the source (the
    // same engine opens it, the same switches apply); the star does not --
    // favouriting the original is not favouriting its copies. The stamp is
    // NOW even though the copy has never been opened: the Opened column's
    // real job is recency-of-interaction, creating IS the interaction, and a
    // "never"-stamped copy would sort to the bottom and read as a failure.
    let new_path = dup.manifest.to_string_lossy().to_string();
    let key = state::project_dir_key(&path);
    state::mutate(|s| {
        let (engine_id, args) = s
            .recents
            .iter()
            .find(|e| state::project_dir_key(&e.path) == key)
            .map(|e| (e.engine_id.clone(), e.args.clone()))
            .unwrap_or_default();
        state::touch_recent(
            &mut s.recents,
            state::RecentProject {
                path: new_path.clone(),
                name: dup.name.clone(),
                last_opened_utc: launch::now_utc_iso(),
                engine_abi: dup.abi,
                engine_id,
                args,
                favorite: false,
                missing: false,
                // The freshly minted identity, never the original's.
                guid: Some(dup.guid.clone()),
            },
        );
        Ok(())
    })?;
    Ok(new_path)
}

// Remove ONE project from the Hub's own list. It does not touch the project on
// disk -- that is delete_project's job, and the menu labels the two apart.
// Restored 2026-07-28: the project-actions wave dropped it when Delete took its
// place on the card, which left "stop showing me this" impossible without
// erasing the folder. Unity Hub and Unreal both keep the two separate.
#[tauri::command(async)]
fn forget_project(path: String) -> Result<(), String> {
    state::mutate(|s| {
        state::remove_recent(&mut s.recents, &path);
        Ok(())
    })
}

// Repoint a listed project whose folder moved -- the greyed row's Locate...
//
// `new_path` is a .arcproj the user just picked (the dialog only yields
// those), validated by the engine's own exactly-one-manifest rule via
// resolve_project. The entry keeps its engine pin and launch arguments --
// moving a folder changes neither -- but takes its name and ABI from the
// manifest just read, because the project may have changed while it was away.
// Locating is not opening: the list is not reordered and nothing launches.
//
// REFUSED when the new path is already listed as a LIVE project
// (state::relocate_conflict): the absorb used to merge the located entry
// over it silently, replacing that project's star, pin and arguments.
#[tauri::command(async)]
fn relocate_project(path: String, new_path: String) -> Result<(), String> {
    let (_root, manifest) = resolve::resolve_project(Path::new(&new_path))?;
    // 0 = unknown, same as open_project: a manifest without a readable abi is
    // "no conflict provable", never a guessed number.
    let abi = resolve::manifest_abi(&manifest).unwrap_or(0);
    let guid = resolve::manifest_guid(&manifest);
    let name = project::display_name(&new_path);

    state::mutate(|s| {
        if let Some(why) = state::relocate_conflict(&s.recents, &path, &new_path) {
            return Err(why);
        }
        if !state::relocate_recent(&mut s.recents, &path, &new_path, &name, abi, guid.clone()) {
            return Err(not_listed(&path));
        }
        Ok(())
    })
}

// Same contract as forget_project, for the whole list: Hub state only.
#[tauri::command(async)]
fn clear_recents() -> Result<(), String> {
    state::mutate(|s| {
        s.recents.clear();
        Ok(())
    })
}

#[tauri::command]
fn load_settings() -> settings::Settings {
    settings::load()
}

#[tauri::command]
fn save_settings(settings: settings::Settings) -> Result<(), String> {
    settings::save(&settings::Settings {
        default_project_dir: settings::clean_dir(&settings.default_project_dir),
        project_view: settings::clean_view(&settings.project_view),
        launch_behavior: settings::clean_behavior(&settings.launch_behavior),
        project_sort: settings::clean_sort(&settings.project_sort),
        default_engine_id: settings::clean_engine_id(settings.default_engine_id.as_deref()),
        ..settings
    })
}

// The dialog's starting directory. A configured folder that has since been
// deleted degrades to "let the OS choose" rather than erroring -- the user
// wanted a file picker, not a lecture about their settings.
#[tauri::command]
fn default_dialog_dir() -> Option<String> {
    let dir = settings::clean_dir(&settings::load().default_project_dir);
    if dir.is_empty() || !Path::new(&dir).is_dir() {
        return None;
    }
    Some(dir)
}

#[tauri::command]
fn hub_data_dir() -> String {
    paths::hub_dir().to_string_lossy().to_string()
}

#[tauri::command]
fn reveal_hub_data_dir() -> Result<(), String> {
    let dir = paths::hub_dir();
    // Explorer fails on a path that does not exist yet, and the folder is only
    // created on first save -- so a fresh install would otherwise no-op.
    std::fs::create_dir_all(&dir).map_err(|e| format!("create {}: {e}", dir.display()))?;
    // NOT status-checked: explorer.exe returns a non-zero exit code even when
    // it opens the window successfully.
    Command::new("explorer")
        .arg(&dir)
        .spawn()
        .map_err(|e| format!("could not open {}: {e}", dir.display()))?;
    Ok(())
}

#[tauri::command]
fn hub_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

// Surface the Hub window (and take the tray icon down). For the frontend to
// call before it shows a message that would otherwise land in a hidden or
// parked window -- the tray-launch route reports failures this way.
#[tauri::command]
fn show_hub(app: tauri::AppHandle) {
    tray::show_hub(&app);
}

// Launch the editor on a project, as a TRACKED child: the command returns
// immediately (launch.rs's wait thread owns the child), several editors may
// run at once, and closing the Hub still cannot close editors -- Windows
// children survive their parent absent a job object. The whole story --
// the registered-engine gate, focus-existing, the probe, the behaviour, the
// watchdog -- lives in launch::do_open_project, shared with the .arcproj
// shell route.
#[tauri::command(async)]
fn open_project(
    app: tauri::AppHandle,
    project_path: String,
    engine_path: String,
) -> Result<OpenOutcome, String> {
    launch::do_open_project(&app, project_path, engine_path)
}

/// The running key set, pulled: the frontend's initial paint cannot wait for
/// an event that only fires on the next transition. Keys are
/// state::project_dir_key-shaped (see launch::running_keys).
#[tauri::command(async)]
fn running_projects(app: tauri::AppHandle) -> Vec<String> {
    launch::running_keys(&app)
}

// Cover thumbnails for a batch of recorded project paths, keyed by the exact
// path handed in. One IPC round-trip for the whole grid; only projects WITH
// a cover appear in the map, so the frontend's fallback is a simple miss.
#[tauri::command(async)]
fn project_covers(paths: Vec<String>) -> HashMap<String, String> {
    paths
        .into_iter()
        .filter_map(|p| resolve::project_cover(Path::new(&p)).map(|c| (p, c)))
        .collect()
}

/// What a Scan did, for the frontend's one-line report. Every count is
/// surfaced -- a scan that silently dropped ambiguous folders or gave up on
/// a budget would read as "found everything" when it did not. `relocated`
/// counts missing entries healed onto a found project by guid: those rows
/// un-greyed rather than appeared, which deserves its own word.
#[derive(serde::Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ScanReport {
    pub added: u32,
    pub already_listed: u32,
    pub relocated: u32,
    pub ambiguous: u32,
    pub truncated: bool,
}

// Godot's Scan: walk a folder tree and list every project found under it.
//
// `async` like duplicate_project: the walk's duration scales with the tree
// the user picks. Found projects APPEND with a never-opened stamp -- a bulk
// import must not bury the real recents, and "never" is the truth about a
// project the Hub has not launched. Engine pins are not invented; each entry
// follows the default until the user says otherwise.
#[tauri::command]
async fn scan_for_projects(dir: String) -> Result<ScanReport, String> {
    let _busy = launch::busy();
    let outcome = resolve::scan_tree(Path::new(&dir))?;

    let found: Vec<state::RecentProject> = outcome
        .manifests
        .iter()
        .map(|m| {
            let path = m.to_string_lossy().to_string();
            state::RecentProject {
                name: project::display_name(&path),
                engine_abi: resolve::manifest_abi(m).unwrap_or(0),
                // Read here so add_scanned can heal a moved project's missing
                // entry onto this find instead of listing a stranger beside it.
                guid: resolve::manifest_guid(m),
                path,
                last_opened_utc: "0".to_string(),
                engine_id: None,
                args: String::new(),
                favorite: false,
                missing: false,
            }
        })
        .collect();

    let (added, already_listed, relocated) =
        state::mutate(|s| Ok(state::add_scanned(&mut s.recents, found)))?;
    Ok(ScanReport {
        added,
        already_listed,
        relocated,
        ambiguous: outcome.ambiguous,
        truncated: outcome.truncated,
    })
}

// List an existing project WITHOUT launching it -- the "Add" button: pick a
// .arcproj (or its folder) and it joins the list, never opened. Lenient
// resolution (the file as given, or a folder's single manifest): nothing on
// disk is touched. The manifest path is what gets recorded, the same shape a
// launch records. Already listed: the existing entry comes back unchanged
// (no reorder). A missing entry with the same guid heals onto it (a moved
// project re-added by hand keeps its star, pin and history). Returns the
// entry as stored.
#[tauri::command(async)]
fn add_project(path: String) -> Result<state::RecentProject, String> {
    let (_root, manifest) = resolve::resolve_project(Path::new(path.trim()))?;
    let recorded = manifest.to_string_lossy().to_string();
    let entry = state::RecentProject {
        name: project::display_name(&recorded),
        engine_abi: resolve::manifest_abi(&manifest).unwrap_or(0),
        guid: resolve::manifest_guid(&manifest),
        path: recorded,
        last_opened_utc: "0".to_string(),
        engine_id: None,
        args: String::new(),
        favorite: false,
        missing: false,
    };
    state::mutate(|s| Ok(state::add_existing(&mut s.recents, entry)))
}

// Create a project folder + manifest, and LIST it.
//
// Both inputs are IPC arguments that become a directory and a file name, so
// both are gated here: the name by project::name_error (the same rule
// rename_project applies -- it used to be checked only in the frontend, so a
// name like "..\x" or "CON" reached create_dir_all), and the folder must be
// absolute and exist (a relative one resolved against the Hub's working
// directory).
//
// Listed HERE, inside state::mutate, stamped NOW (creating is the
// interaction, as Duplicate's copy is): the frontend launches the new project
// next, and when that launch failed -- a probe error, a refused engine -- the
// project existed on disk but appeared nowhere in the Hub.
#[tauri::command(async)]
fn create_project(dir: String, name: String, engine_path: String) -> Result<String, String> {
    let name = name.trim().to_string();
    if let Some(why) = project::name_error(&name) {
        return Err(why);
    }
    let parent = PathBuf::from(settings::clean_dir(&dir));
    if !parent.is_absolute() {
        return Err(format!("{} is not an absolute path", parent.display()));
    }
    if !parent.is_dir() {
        return Err(format!("{} does not exist", parent.display()));
    }

    let exe = engine::resolve_editor_exe(Path::new(&engine_path));
    // Probe FIRST and abort on failure -- never fall back to a guessed ABI.
    let info = spawn::probe_engine(&exe)?;

    let _busy = launch::busy();
    let root = parent.join(&name);
    if root.exists() && std::fs::read_dir(&root).map(|mut d| d.next().is_some()).unwrap_or(false) {
        return Err(format!("{} already exists and is not empty", root.display()));
    }
    std::fs::create_dir_all(root.join("Content"))
        .map_err(|e| format!("could not create {}: {e}", root.display()))?;

    // A fresh identity at birth, same as the engine's own Project::Create --
    // so a Hub-created project never needs the engine's open-time self-heal.
    let guid = project::new_guid();
    let manifest = project::manifest_json(&name, info.engine_abi, &guid)?;
    let file = root.join(format!("{name}.{}", project::MANIFEST_EXT));
    store::write_bytes_atomic(&file, manifest.as_bytes())
        .map_err(|e| format!("could not write {}: {e}", file.display()))?;

    // Returns the .arcproj FILE, not the folder: the caller feeds this straight
    // into open_project, and recents records a newly created project the same
    // way it records one the user picked through the Open dialog.
    let path = file.to_string_lossy().to_string();
    state::mutate(|s| {
        state::touch_recent(
            &mut s.recents,
            state::RecentProject {
                path: path.clone(),
                name: name.clone(),
                last_opened_utc: launch::now_utc_iso(),
                engine_abi: info.engine_abi,
                // Follows the default until the user pins it, like a launch.
                engine_id: None,
                args: String::new(),
                favorite: false,
                missing: false,
                guid: Some(guid.clone()),
            },
        );
        Ok(())
    })
    .map_err(|e| format!("created {} but could not list it: {e}", file.display()))?;
    Ok(path)
}

// Adjacency is a SUGGESTION for the dev loop, never an assumption. Returns a
// path only if it actually probes clean. (From the installed location this
// never fires -- engines are registered by path; this exists for a portable
// exe dropped beside a build.)
#[tauri::command(async)]
fn suggest_engine() -> Option<state::EngineEntry> {
    let exe_dir = std::env::current_exe().ok()?.parent()?.to_path_buf();
    let candidates = [
        exe_dir.join(engine::EDITOR_EXE),
        exe_dir.join("..").join("ArcaneEditor").join(engine::EDITOR_EXE),
    ];
    for c in candidates {
        if let Ok(info) = spawn::probe_engine(&c) {
            // The same canonical spelling register_engine would store, so
            // accepting the suggestion does not mint a second id for it.
            let path = engine::canonical_editor_exe(&c.to_string_lossy()).unwrap_or(c);
            // EngineEntry::new: the one place an id is derived from a path.
            // It answered the probe one line up; it is on disk.
            return Some(state::EngineEntry::new(
                &path.to_string_lossy(),
                info.engine_abi,
                info.build,
            ));
        }
    }
    None
}

pub fn run() {
    // Before ANY window exists: claim the taskbar family id ArcaneEditor also
    // sets, so the Hub's and every editor's buttons stack as one group.
    spawn::claim_app_user_model_id();
    // And claim (or repair) the .arcproj association for this exe -- the
    // installer's own write shipped unquoted once, and a self-heal at every
    // start outlives any installer fix. See assoc.rs for the incident (and
    // why a debug build skips it).
    assoc::claim_arcproj_association();
    tauri::Builder::default()
        // FIRST plugin registered, per its own docs: it has to win the race
        // before anything else initialises. A second launch lands in this
        // callback inside the FIRST process; the new process exits on its own.
        .plugin(tauri_plugin_single_instance::init(|app, args, cwd| {
            // A second launch carrying a .arcproj is a FILE OPEN (the shell
            // association route), not "show me the Hub" -- route it and leave
            // the window wherever the launch lifecycle puts it. A relative
            // path resolves against the SECOND instance's cwd, which is where
            // whoever typed it was standing.
            let cwd = (!cwd.is_empty()).then(|| PathBuf::from(&cwd));
            if launch::route_shell_args(app, args.iter().map(|s| s.as_str()), cwd.as_deref()) {
                return;
            }
            // Otherwise: surface the window the user already has. While
            // editors run it may be hidden or parked in the tray, and
            // relaunching the Hub is the designed way to get it back before
            // they exit; show_hub also clears the tray icon.
            tray::show_hub(app);
        }))
        .plugin(tauri_plugin_dialog::init())
        // SIZE | POSITION | MAXIMIZED only. Deliberately NOT the default
        // all-flags: VISIBLE would fight the hidden-start contract (the
        // window is created invisible so a shell-routed launch never
        // flashes), and DECORATIONS/FULLSCREEN are fixed by config.
        .plugin(
            tauri_plugin_window_state::Builder::default()
                .with_state_flags(
                    tauri_plugin_window_state::StateFlags::SIZE
                        | tauri_plugin_window_state::StateFlags::POSITION
                        | tauri_plugin_window_state::StateFlags::MAXIMIZED,
                )
                .build(),
        )
        .manage(RunningEditors::new())
        .setup(|app| {
            use tauri::Manager;
            watch::spawn_disk_watch(app.app_handle().clone());
            // One-time guid backfill: entries recorded before the manifest
            // guid existed (or never read since) have no identity on file,
            // and moved-project healing is blind to them until something
            // reads their manifest. This makes healing cover the whole list
            // from this session on. Its own thread: one small file read per
            // listed project, but disk does not belong in setup. The reads
            // happen on a SNAPSHOT, outside the state lock; only the found
            // guids are applied under state::mutate.
            std::thread::spawn(|| {
                let snapshot = state::load();
                let found: HashMap<String, String> = snapshot
                    .recents
                    .iter()
                    .filter(|e| e.guid.is_none() && !e.missing)
                    .filter_map(|e| {
                        resolve::manifest_guid(Path::new(&e.path)).map(|g| (e.path.clone(), g))
                    })
                    .collect();
                if !found.is_empty() {
                    let _ = state::mutate(|s| {
                        state::backfill_guids(&mut s.recents, |p| found.get(p).cloned());
                        Ok(())
                    });
                }
                // Close-mode leftover: this Hub process did not spawn the
                // editor, so there is no wait thread to sweep after wait().
                // A crash's editor.lock is deleted only when it parses and
                // its editor is proven dead (tell 3 included).
                for e in &snapshot.recents {
                    let _ = editorlock::sweep_stale(&resolve::project_dir(Path::new(&e.path)));
                }
            });
            // The window is created HIDDEN (tauri.conf.json): a cold start
            // via the file association used to flash the Hub for a moment
            // before the launch decision hid it again (user report,
            // 2026-07-29). Now a shell-routed start stays invisible and the
            // launch outcome decides -- tray parks, close stays away, `stay`
            // and every failure path show the window -- while a NORMAL start
            // shows right here, the same moment it used to appear.
            let args: Vec<String> = std::env::args().collect();
            let cwd = std::env::current_dir().ok();
            let routed = launch::route_shell_args(
                app.app_handle(),
                args.iter().map(|s| s.as_str()),
                cwd.as_deref(),
            );
            if !routed {
                if let Some(w) = app.get_webview_window("main") {
                    let _ = w.show();
                }
            }
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            load_state,
            register_engine,
            refresh_engines,
            forget_engine,
            delete_project,
            duplicate_project,
            forget_project,
            relocate_project,
            clear_recents,
            set_project_engine,
            set_project_favorite,
            set_project_args,
            reveal_project,
            rename_project,
            open_project,
            project_covers,
            running_projects,
            scan_for_projects,
            add_project,
            create_project,
            suggest_engine,
            load_settings,
            save_settings,
            default_dialog_dir,
            hub_data_dir,
            reveal_hub_data_dir,
            hub_version,
            show_hub
        ])
        .build(tauri::generate_context!())
        .expect("error while running the Arcane Hub")
        .run(|app, event| {
            // Catch-all half of the tray-ghost fix (the quit handler's
            // remove-before-exit in tray.rs is the direct half): whatever
            // path requests exit while the Hub is parked, take the icon
            // down while the process is still alive to do it. A dead
            // process cannot un-register a tray icon -- Windows keeps the
            // corpse until the tray repaints (2026-08-11 incident).
            if let tauri::RunEvent::ExitRequested { .. } = event {
                crate::tray::remove(app);
            }
        });
}
