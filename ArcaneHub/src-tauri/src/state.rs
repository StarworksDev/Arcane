// Hub state: the recent-projects list and the registered-engines list.
//
// The list operations here are PURE and unit-tested. Loading and saving are
// not: `load` stats every listed path (to stamp `missing`) and, through
// store.rs, may QUARANTINE a corrupt file or refuse to overwrite one it could
// not read -- falling back to defaults either way, because a hand-edited or
// truncated state file must never brick the Hub.
//
// EVERY read-modify-write goes through `mutate`, which holds one process-wide
// lock around load -> change -> save. Before it, writers on several threads
// (the startup backfill, the shell-open thread, async Duplicate and Scan,
// refresh_engines probing for seconds between its load and its save) each
// loaded, changed and saved on their own, so the last save silently erased
// whatever the others had written in between -- a star or a freshly listed
// copy vanishing for no visible reason. Probes and disk work happen OUTSIDE
// the lock; only the state edit is inside it.

use serde::{Deserialize, Serialize};
use std::path::Path;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

use crate::{paths, store};

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct RecentProject {
    pub path: String,
    pub name: String,
    pub last_opened_utc: String,
    pub engine_abi: u32,

    /// Which registered engine opens THIS project. `None` = follow the Hub's
    /// default (the sidebar selection).
    ///
    /// Hub state, deliberately NOT the .arcproj. This is an `EngineEntry::id`,
    /// which is a normalised absolute exe path -- machine-specific by nature.
    /// Unreal reaches the same conclusion from the other direction: its
    /// `EngineAssociation` does live in the .uproject, but only ever as a
    /// portable version string or a random GUID resolved through the local
    /// registry, and DesktopPlatformBase.cpp:444 blanks it entirely when saving
    /// a non-foreign project "to allow portability between source control
    /// databases". A raw local path in shared project content is the one thing
    /// that design avoids.
    #[serde(default)]
    pub engine_id: Option<String>,

    /// Extra command-line arguments appended after `--project <path>`, as the
    /// user typed them. Empty = launch with no extras.
    ///
    /// Stored RAW rather than pre-split: what the user typed is what the field
    /// must show them next time, and a re-joined token list is not that. It is
    /// split at launch, by `project::split_args`.
    ///
    /// Hub state, not the .arcproj, for the same reason as `engine_id`: these
    /// are one person's local switches (a backend override, a scene to boot
    /// into), not a property of the project that belongs in shared content.
    #[serde(default)]
    pub args: String,

    /// Starred: the frontend pins favourites above the rest of the list,
    /// whatever the active sort (the Godot model -- starring visibly
    /// promotes). Persisted, unlike `missing`: a favourite is a user choice,
    /// not a disk fact. `#[serde(default)]` so every pre-favourites file
    /// loads unstarred.
    #[serde(default)]
    pub favorite: bool,

    /// True when `path` no longer resolves on disk -- the row renders greyed
    /// with Locate/Remove instead of vanishing (a missing project is usually a
    /// moved folder, not an abandoned one). Stamped by `load` on EVERY read,
    /// so whatever value the file holds is overwritten before the UI sees it;
    /// it rides the persisted struct only because save/load and the
    /// `load_state` IPC response share one Serialize impl (same constraint as
    /// `HubState::warnings`).
    #[serde(default)]
    pub missing: bool,

    /// The manifest's `guid`, canonical form, recorded whenever a command
    /// reads the manifest (launch, scan, locate, duplicate). None until then
    /// -- including for projects whose manifest predates the field, which the
    /// engine stamps at next open.
    ///
    /// This is the Hub's durable identity for a project: `heal_by_guid` uses
    /// it to recognise a MOVED project at its new path and repoint the entry
    /// in place -- keeping the star, the pin, the arguments, the history --
    /// instead of minting a stranger row while the old one sits greyed
    /// forever. Only a MISSING entry can be healed, so a hand-copied folder
    /// (which carries the same guid while the original still exists) lists as
    /// the separate project it is.
    #[serde(default)]
    pub guid: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct EngineEntry {
    /// Stable key for the frontend and for per-project pins. Always
    /// `normalise_path(path)` -- construct through `EngineEntry::new` so the
    /// two cannot drift apart.
    pub id: String,
    pub path: String,
    pub engine_abi: u32,
    pub build: String,

    /// True when `path` no longer resolves on disk. Same rule as
    /// `RecentProject::missing`: stamped by `load` on EVERY read, never
    /// trusted from the file. A missing engine keeps its row (registration is
    /// user intent; a clean rebuild restores it in place) but the UI says so,
    /// and the pre-launch probe already refuses it honestly.
    #[serde(default)]
    pub missing: bool,
}

impl EngineEntry {
    /// The ONE place `id` is derived from `path`. It was previously computed at
    /// the call site, leaving two fields that had to agree by convention.
    pub fn new(exe_path: &str, engine_abi: u32, build: String) -> Self {
        Self {
            id: normalise_path(exe_path),
            path: exe_path.to_string(),
            engine_abi,
            build,
            missing: false,
        }
    }
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct HubState {
    #[serde(default)]
    pub recents: Vec<RecentProject>,
    #[serde(default)]
    pub engines: Vec<EngineEntry>,

    /// Problems found while loading, for the UI to show. Populated by `load`.
    ///
    /// Exists because recovering from a corrupt state file used to be silent:
    /// the user simply found an empty project list and no explanation.
    ///
    /// MUST stay serializable. `save` writes `recents` and `engines`
    /// individually, so this struct's Serialize impl has exactly one consumer:
    /// the `load_state` IPC response. A `skip_serializing` here (which this
    /// briefly had, reasoning "it is never persisted") removes the field from
    /// the only place it was ever meant to appear, and the frontend then sees
    /// `undefined` where it expects an array.
    #[serde(default)]
    pub warnings: Vec<String>,

    /// Which load this snapshot came from: every `load` takes the next value
    /// of a process-wide counter (starting at 1), so a snapshot read LATER
    /// always carries a higher number. Snapshots reach the frontend by
    /// several routes that can arrive out of order -- a command's reply, the
    /// watcher's `state-changed`, the shell route's -- and the frontend
    /// ignores any snapshot older than the last one it adopted, instead of
    /// letting a slow reply roll the list back. Never persisted (save writes
    /// the two lists individually); like `warnings` it must stay serialised.
    #[serde(default)]
    pub revision: u64,
}

/// The next snapshot revision. Starts at 1 so 0 can mean "never loaded".
static NEXT_REVISION: AtomicU64 = AtomicU64::new(1);

/// The process-wide state lock `mutate` holds around load-modify-save.
static STATE_LOCK: Mutex<()> = Mutex::new(());

// Compare-only normalisation. Windows paths are case-insensitive and accept
// either separator, so the SAME project reached two ways must collapse to one
// entry. The ORIGINAL string is what gets stored and displayed.
pub fn normalise_path(p: &str) -> String {
    p.replace('\\', "/")
        .trim_end_matches('/')
        .to_lowercase()
}

/// The identity a project is keyed on for DEDUPLICATION, independent of how
/// it was recorded: recents holds folder-shaped entries (pre-dialog era) and
/// manifest-file entries, and both must collapse to the folder. Mirror of
/// `projectKey` in src/lib/format.ts -- change BOTH.
pub fn project_dir_key(path: &str) -> String {
    let norm = normalise_path(path);
    match norm.rsplit_once('/') {
        Some((dir, file)) if file.ends_with(".arcproj") && !dir.is_empty() => dir.to_string(),
        _ => norm,
    }
}

/// Repoint the MISSING entry whose recorded guid matches onto the path where
/// the project just turned up -- the moved-folder heal. The entry keeps its
/// star, engine pin, arguments and history (relocate_recent's contract);
/// no launcher in the field does this, they all make you re-add from scratch.
///
/// The one rule that keeps this out of the Unity productGUID trap: ONLY a
/// missing entry heals. A guid match against an entry whose path still exists
/// is a hand-copied folder, and merging would let the copy impersonate the
/// original -- the caller lists it as the new project it is. `missing` is
/// disk truth stamped by `load` on every read, so this stays pure.
///
/// And no heal when `new_path` is ALREADY listed (add_scanned's own rule,
/// now enforced here for every caller). A launch used to heal regardless:
/// launching a listed clone or worktree that shares its guid with a missing
/// sibling repointed the SIBLING's entry onto the listed path, and the
/// collision absorb then dropped the launched project's own row -- its pin,
/// arguments and star silently replaced by the missing sibling's.
pub fn heal_by_guid(
    v: &mut Vec<RecentProject>,
    guid: &str,
    new_path: &str,
    new_name: &str,
    new_abi: u32,
) -> bool {
    let new_key = project_dir_key(new_path);
    if v.iter().any(|e| project_dir_key(&e.path) == new_key) {
        return false;
    }
    let Some(old_path) = v
        .iter()
        .find(|e| e.missing && e.guid.as_deref() == Some(guid))
        .map(|e| e.path.clone())
    else {
        return false;
    };
    relocate_recent(v, &old_path, new_path, new_name, new_abi, Some(guid.to_string()))
}

/// Append every scanned entry whose project is not already listed, at the
/// END: a bulk import must not bury the real recents under a wall of
/// never-opened rows. A found project whose guid matches a MISSING entry
/// heals that entry in place instead of appending a stranger beside it.
/// Returns (added, already listed, relocated).
pub fn add_scanned(v: &mut Vec<RecentProject>, found: Vec<RecentProject>) -> (u32, u32, u32) {
    let mut have: std::collections::HashSet<String> =
        v.iter().map(|e| project_dir_key(&e.path)).collect();
    let (mut added, mut existing, mut relocated) = (0, 0, 0);
    for entry in found {
        let key = project_dir_key(&entry.path);
        if have.contains(&key) {
            existing += 1;
        } else if entry
            .guid
            .as_deref()
            .is_some_and(|g| heal_by_guid(v, g, &entry.path, &entry.name, entry.engine_abi))
        {
            have.insert(key);
            relocated += 1;
        } else {
            have.insert(key);
            v.push(entry);
            added += 1;
        }
    }
    (added, existing, relocated)
}

// Insert-or-move-to-front. Re-opening a project must refresh it, not duplicate
// it, so the list stays newest-first without growing.
//
// IDENTITY IS project_dir_key HERE AND IN EVERY EDIT BELOW (set_*, remove,
// rename, relocate), not normalise_path. Recents holds folder-shaped legacy
// rows beside manifest-shaped ones, and with the narrower key a legacy row
// "D:/G/My" and a launch of "D:/G/My/My.arcproj" were two different
// projects: the launch minted a second row and neither carried the other's
// pin or star, while add_scanned and the running badges already treated
// them as one. Consequence, deliberate: two manifests in ONE folder fold to
// one row -- the engine refuses such a folder, the editor lock is per
// folder, and the Hub's destructive commands refuse it (resolve.rs).
pub fn touch_recent(v: &mut Vec<RecentProject>, mut entry: RecentProject) {
    let key = project_dir_key(&entry.path);

    // Carry the per-project engine pin across the re-insert. open_project builds
    // a fresh entry from the launch and has no reason to know about the pin, so
    // without this every launch would silently reset the project to the default.
    // Same for the saved command-line arguments. Clearing them is done through
    // set_project_args, which edits the entry in place, so an empty string
    // arriving HERE always means "the caller had nothing to say", never
    // "the user emptied the field".
    if let Some(prev) = v.iter().find(|e| project_dir_key(&e.path) == key) {
        if entry.engine_id.is_none() {
            entry.engine_id.clone_from(&prev.engine_id);
        }
        if entry.args.is_empty() {
            entry.args.clone_from(&prev.args);
        }
        // A launch is never an unfavourite: the fresh entry knows nothing
        // about the star, so the previous value always wins. |= rather than
        // an if: unstarring is set_favorite's job, which edits in place.
        entry.favorite |= prev.favorite;
        // The identity survives a caller that did not (or could not) read the
        // manifest; a caller that DID read it wins, refreshing a stale value.
        if entry.guid.is_none() {
            entry.guid.clone_from(&prev.guid);
        }
    }

    v.retain(|e| project_dir_key(&e.path) != key);
    v.insert(0, entry);
}

/// Save the extra launch arguments for one project. False if it is not listed.
pub fn set_project_args(v: &mut [RecentProject], path: &str, args: &str) -> bool {
    let key = project_dir_key(path);
    match v.iter_mut().find(|e| project_dir_key(&e.path) == key) {
        Some(e) => {
            e.args = args.to_string();
            true
        }
        None => false,
    }
}

/// Star or unstar one project IN PLACE -- no reorder: favouriting pins the
/// entry above the rest visually (the frontend partitions), it does not
/// pretend the project was just opened. False if it is not listed.
pub fn set_favorite(v: &mut [RecentProject], path: &str, favorite: bool) -> bool {
    let key = project_dir_key(path);
    match v.iter_mut().find(|e| project_dir_key(&e.path) == key) {
        Some(e) => {
            e.favorite = favorite;
            true
        }
        None => false,
    }
}

/// Repoint a listed project at the path the user just located it at.
///
/// Same in-place contract as `rename_recent` below (the pin, the arguments and
/// last-opened survive; the list is not reordered; a collision with an entry
/// already at the new path is absorbed) -- but the name and ABI are REPLACED
/// from the manifest the caller just read, because the project may have been
/// renamed or rebuilt while the Hub could not see it.
pub fn relocate_recent(
    v: &mut Vec<RecentProject>,
    old_path: &str,
    new_path: &str,
    new_name: &str,
    new_abi: u32,
    new_guid: Option<String>,
) -> bool {
    let old_key = project_dir_key(old_path);
    let new_key = project_dir_key(new_path);

    let Some(i) = v.iter().position(|e| project_dir_key(&e.path) == old_key) else {
        return false;
    };
    v[i].path = new_path.to_string();
    v[i].name = new_name.to_string();
    v[i].engine_abi = new_abi;
    v[i].missing = false;
    // Some = the manifest just read has an identity, record it. None = it does
    // not (pre-guid manifest); keep whatever was known rather than forgetting.
    if new_guid.is_some() {
        v[i].guid = new_guid;
    }

    // Retain by index so the entry just edited survives its own key check.
    let mut n = 0;
    v.retain(|e| {
        let keep = n == i || project_dir_key(&e.path) != new_key;
        n += 1;
        keep
    });
    true
}

/// Repoint a listed project at its new path and name after a rename.
///
/// Edits IN PLACE rather than remove-then-touch: everything else on the entry
/// (the engine pin, the saved arguments, the ABI, when it was last opened) has
/// to survive, and a rename is not an open, so it must not jump to the top of
/// the list either. Any other entry that would now collide with the new path is
/// dropped, so a rename can never mint a duplicate.
pub fn rename_recent(v: &mut Vec<RecentProject>, old_path: &str, new_path: &str, new_name: &str) -> bool {
    let old_key = project_dir_key(old_path);
    let new_key = project_dir_key(new_path);

    let Some(i) = v.iter().position(|e| project_dir_key(&e.path) == old_key) else {
        return false;
    };
    v[i].path = new_path.to_string();
    v[i].name = new_name.to_string();

    // Retain by index so the entry just edited survives its own key check.
    let mut n = 0;
    v.retain(|e| {
        let keep = n == i || project_dir_key(&e.path) != new_key;
        n += 1;
        keep
    });
    true
}

// Pin a project to a registered engine, or clear the pin with `None` to send it
// back to following the Hub default. Returns false if the project is not listed.
pub fn set_project_engine(v: &mut [RecentProject], path: &str, engine_id: Option<String>) -> bool {
    let key = project_dir_key(path);
    match v.iter_mut().find(|e| project_dir_key(&e.path) == key) {
        Some(e) => {
            e.engine_id = engine_id;
            true
        }
        None => false,
    }
}

// Forgetting an engine must not leave projects pinned to it. The pin is cleared
// rather than repointed: which engine should replace it is the user's call, and
// falling back to the default is the honest answer until they make it.
pub fn unpin_engine(v: &mut [RecentProject], engine_id: &str) {
    for e in v.iter_mut() {
        if e.engine_id.as_deref() == Some(engine_id) {
            e.engine_id = None;
        }
    }
}

pub fn remove_recent(v: &mut Vec<RecentProject>, path: &str) -> bool {
    let key = project_dir_key(path);
    let before = v.len();
    v.retain(|e| project_dir_key(&e.path) != key);
    v.len() != before
}

// Re-registering the same engine refreshes its probed data rather than
// appending a duplicate row.
pub fn upsert_engine(v: &mut Vec<EngineEntry>, entry: EngineEntry) {
    let key = normalise_path(&entry.path);
    v.retain(|e| normalise_path(&e.path) != key);
    v.push(entry);
}

pub fn remove_engine(v: &mut Vec<EngineEntry>, path: &str) -> bool {
    let key = normalise_path(path);
    let before = v.len();
    v.retain(|e| normalise_path(&e.path) != key);
    v.len() != before
}

/// Record identities for entries that have none: `lookup` resolves a recorded
/// path to its manifest's guid. Run once at startup -- without it,
/// heal_by_guid is blind to a project until the user happens to launch or
/// scan it, so a move made BEFORE that first read could never heal. Only
/// live entries are consulted (a missing path has no manifest to read), and
/// a recorded guid is never overwritten here -- refreshing is the job of the
/// commands that read the manifest anyway. Returns how many changed.
pub fn backfill_guids(
    v: &mut [RecentProject],
    lookup: impl Fn(&str) -> Option<String>,
) -> u32 {
    let mut changed = 0;
    for e in v.iter_mut() {
        if e.guid.is_none() && !e.missing {
            if let Some(g) = lookup(&e.path) {
                e.guid = Some(g);
                changed += 1;
            }
        }
    }
    changed
}

/// Why relocating `old_path` onto `new_path` must be refused, or None.
///
/// Locate used to absorb whatever entry already sat at the new path -- fine
/// for a MISSING duplicate (the user re-added it, then found the stale row),
/// but a LIVE listed project there was silently merged away: its star, pin
/// and arguments replaced by the located entry's. Pointing an entry at the
/// same path it already holds is not a conflict.
pub fn relocate_conflict(v: &[RecentProject], old_path: &str, new_path: &str) -> Option<String> {
    let (old_key, new_key) = (project_dir_key(old_path), project_dir_key(new_path));
    if old_key == new_key {
        return None;
    }
    v.iter()
        .find(|e| project_dir_key(&e.path) == new_key && !e.missing)
        .map(|e| format!("{new_path} is already in the list as {}", e.name))
}

/// List an existing project WITHOUT launching it (the add_project command),
/// returning the entry as stored. Already listed (by project_dir_key): the
/// existing entry, unchanged and NOT reordered -- adding what is there is a
/// no-op, not an interaction. Not listed but a MISSING entry carries its
/// guid: that entry heals onto the path (a moved project re-added by hand
/// keeps its star, pin and arguments). Otherwise a fresh row at the top,
/// stamped "0" -- never opened, which is the truth.
pub fn add_existing(v: &mut Vec<RecentProject>, entry: RecentProject) -> RecentProject {
    let key = project_dir_key(&entry.path);
    if let Some(e) = v.iter().find(|e| project_dir_key(&e.path) == key) {
        return e.clone();
    }
    let healed = entry
        .guid
        .as_deref()
        .is_some_and(|g| heal_by_guid(v, g, &entry.path, &entry.name, entry.engine_abi));
    if !healed {
        touch_recent(v, RecentProject { last_opened_utc: "0".to_string(), ..entry });
    }
    v.iter()
        .find(|e| project_dir_key(&e.path) == key)
        .cloned()
        .expect("the entry was just listed")
}

/// A fresh snapshot of the Hub's state from the real per-user files.
pub fn load() -> HubState {
    load_at(&paths::recents_file(), &paths::engines_file())
}

/// `load` against explicit files -- the seam the tests use, so they never
/// read or write the real %LOCALAPPDATA%\Arcane\hub.
fn load_at(recents_file: &Path, engines_file: &Path) -> HubState {
    let mut warnings = Vec::new();
    let mut recents: Vec<RecentProject> = store::read_or_default(recents_file, &mut warnings);
    // Stamped on every load, never trusted from the file: `missing` is a fact
    // about the disk right now, and a stale persisted value in either
    // direction would grey a healthy row or offer Launch on a gone one.
    for e in recents.iter_mut() {
        e.missing = !Path::new(&e.path).exists();
    }
    let mut engines: Vec<EngineEntry> = store::read_or_default(engines_file, &mut warnings);
    for e in engines.iter_mut() {
        e.missing = !Path::new(&e.path).exists();
    }
    HubState {
        recents,
        engines,
        warnings,
        revision: NEXT_REVISION.fetch_add(1, Ordering::SeqCst),
    }
}

/// The ONE way to change persisted state: load, apply `f`, save -- all under
/// the process-wide STATE_LOCK, so no other writer can load between this
/// load and this save (see the module header for the lost updates that made
/// this necessary). `f` returning Err saves nothing. A change that leaves
/// both lists as loaded saves nothing either -- a no-op command must not
/// rewrite the files.
///
/// Keep `f` SHORT: no probes, no disk walks, no copies. Read what you need
/// first, then mutate. The lock recovers from poison (a panicking closure
/// must not wedge every later command), which is safe because the state it
/// guards lives on disk, not behind the lock.
pub fn mutate<T>(f: impl FnOnce(&mut HubState) -> Result<T, String>) -> Result<T, String> {
    mutate_at(&paths::recents_file(), &paths::engines_file(), f)
}

fn mutate_at<T>(
    recents_file: &Path,
    engines_file: &Path,
    f: impl FnOnce(&mut HubState) -> Result<T, String>,
) -> Result<T, String> {
    let _guard = STATE_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let mut s = load_at(recents_file, engines_file);
    let (recents_before, engines_before) = (s.recents.clone(), s.engines.clone());
    let out = f(&mut s)?;
    if s.recents != recents_before || s.engines != engines_before {
        save_at(recents_file, engines_file, &s)?;
    }
    Ok(out)
}

/// The disk-truth fingerprint the background watcher compares between polls:
/// every listed path paired with whether it is missing right now. Membership
/// changes flip it too, which is fine -- a redundant refresh after a command
/// is idempotent, while a MISSED transition is exactly the stale-row bug the
/// watcher exists to prevent (a project moved mid-session looked healthy
/// until some other action reloaded the list).
pub fn disk_fingerprint(s: &HubState) -> Vec<(String, bool)> {
    s.recents
        .iter()
        .map(|e| (e.path.clone(), e.missing))
        .chain(s.engines.iter().map(|e| (e.path.clone(), e.missing)))
        .collect()
}

/// Private on purpose: the only caller is `mutate_at`, so no command can
/// save a snapshot it loaded outside the lock.
fn save_at(recents_file: &Path, engines_file: &Path, s: &HubState) -> Result<(), String> {
    // Engines FIRST. The two files are written separately, so a failure between
    // them leaves a mismatch; ordering it this way means the surviving mismatch
    // is "an engine exists that nothing points at yet" rather than "a project
    // is pinned to an engine that was never recorded".
    store::write_atomic(engines_file, &s.engines)?;
    store::write_atomic(recents_file, &s.recents)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rp(path: &str, when: &str) -> RecentProject {
        RecentProject {
            path: path.to_string(),
            name: "N".to_string(),
            last_opened_utc: when.to_string(),
            engine_abi: 7,
            engine_id: None,
            args: String::new(),
            favorite: false,
            missing: false,
            guid: None,
        }
    }

    fn ee(path: &str, abi: u32, build: &str) -> EngineEntry {
        EngineEntry::new(path, abi, build.to_string())
    }

    #[test]
    fn engine_entry_derives_its_id_from_its_path() {
        // The invariant the whole pin mechanism rests on: a pin stores an id,
        // and forget_engine looks one up by normalising a path.
        let e = EngineEntry::new("C:\\Eng\\ArcaneEditor.exe", 7, "b".into());
        assert_eq!(e.id, normalise_path(&e.path));
        assert_eq!(e.path, "C:\\Eng\\ArcaneEditor.exe", "the original spelling is kept");
    }

    #[test]
    fn two_spellings_of_one_engine_share_an_id() {
        let a = EngineEntry::new("C:/Eng/ArcaneEditor.exe", 7, "b".into());
        let b = EngineEntry::new("c:\\eng\\arcaneeditor.exe", 7, "b".into());
        assert_eq!(a.id, b.id);
    }

    #[test]
    fn touch_recent_inserts_newest_first() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].path, "C:/b");
    }

    #[test]
    fn touch_recent_moves_existing_to_front_without_duplicating() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        touch_recent(&mut v, rp("C:/a", "3"));
        assert_eq!(v.len(), 2, "re-opening must not duplicate the entry");
        assert_eq!(v[0].path, "C:/a");
        assert_eq!(v[0].last_opened_utc, "3", "timestamp must refresh");
    }

    #[test]
    fn touch_recent_dedupes_case_and_separator_variants() {
        // Windows: the SAME project reached two ways must be one entry.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Dev/MyGame", "1"));
        touch_recent(&mut v, rp("c:\\dev\\mygame", "2"));
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn touch_recent_keeps_the_original_spelling_for_display() {
        // Normalisation is for COMPARISON only; what we store is what we show.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:\\Dev\\MyGame", "1"));
        assert_eq!(v[0].path, "C:\\Dev\\MyGame");
    }

    #[test]
    fn touch_recent_preserves_a_per_project_engine_pin() {
        // THE regression this guards: open_project rebuilds the entry from the
        // launch, so without carry-over every launch would reset the pin and
        // the setting would appear to not stick.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_engine(&mut v, "C:/a", Some("eng-1".into())));
        touch_recent(&mut v, rp("C:/a", "2"));
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"));
        assert_eq!(v[0].last_opened_utc, "2", "the rest of the entry still refreshes");
    }

    #[test]
    fn touch_recent_lets_an_explicit_pin_win_over_the_carried_one() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        set_project_engine(&mut v, "C:/a", Some("old".into()));
        let mut next = rp("C:/a", "2");
        next.engine_id = Some("new".into());
        touch_recent(&mut v, next);
        assert_eq!(v[0].engine_id.as_deref(), Some("new"));
    }

    #[test]
    fn set_project_engine_clears_with_none_and_reports_unknown_paths() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_engine(&mut v, "C:/a", Some("eng-1".into())));
        assert!(set_project_engine(&mut v, "C:/a", None), "clearing is still a hit");
        assert_eq!(v[0].engine_id, None);
        assert!(!set_project_engine(&mut v, "C:/nope", Some("x".into())));
    }

    #[test]
    fn set_project_engine_matches_case_and_separator_variants() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Dev/MyGame", "1"));
        assert!(set_project_engine(&mut v, "c:\\dev\\mygame", Some("eng-1".into())));
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"));
    }

    #[test]
    fn unpin_engine_clears_only_projects_using_that_engine() {
        // Forgetting an engine must not leave a project pointing at nothing.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        set_project_engine(&mut v, "C:/a", Some("gone".into()));
        set_project_engine(&mut v, "C:/b", Some("kept".into()));
        unpin_engine(&mut v, "gone");
        let a = v.iter().find(|e| e.path == "C:/a").unwrap();
        let b = v.iter().find(|e| e.path == "C:/b").unwrap();
        assert_eq!(a.engine_id, None);
        assert_eq!(b.engine_id.as_deref(), Some("kept"));
    }

    #[test]
    fn a_recents_file_written_before_pins_existed_still_loads() {
        // serde(default) on engine_id: an older file has no such key.
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert_eq!(back[0].engine_id, None);
    }

    #[test]
    fn touch_recent_preserves_saved_launch_arguments() {
        // Same regression class as the pin: open_project rebuilds the entry, so
        // without carry-over launching a project would wipe its own arguments.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_args(&mut v, "C:/a", "--backend vulkan"));
        touch_recent(&mut v, rp("C:/a", "2"));
        assert_eq!(v[0].args, "--backend vulkan");
    }

    #[test]
    fn set_project_args_can_clear_and_reports_unknown_paths() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(set_project_args(&mut v, "C:/a", "--frames 10"));
        // Clearing has to survive the next touch_recent, which is why the
        // carry-over above is keyed on "the caller said nothing" -- not on the
        // stored value being non-empty.
        assert!(set_project_args(&mut v, "c:\\a", ""));
        assert_eq!(v[0].args, "");
        touch_recent(&mut v, rp("C:/a", "2"));
        assert_eq!(v[0].args, "", "a cleared field must stay cleared");
        assert!(!set_project_args(&mut v, "C:/nope", "x"));
    }

    #[test]
    fn a_recents_file_written_before_args_existed_still_loads() {
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert_eq!(back[0].args, "");
    }

    #[test]
    fn rename_recent_keeps_everything_except_path_and_name() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Games/Old/Old.arcproj", "1"));
        set_project_engine(&mut v, "C:/Games/Old/Old.arcproj", Some("eng-1".into()));
        set_project_args(&mut v, "C:/Games/Old/Old.arcproj", "--frames 3");

        assert!(rename_recent(
            &mut v,
            "C:/Games/Old/Old.arcproj",
            "C:/Games/New/New.arcproj",
            "New",
        ));
        assert_eq!(v[0].path, "C:/Games/New/New.arcproj");
        assert_eq!(v[0].name, "New");
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"), "the pin must survive");
        assert_eq!(v[0].args, "--frames 3", "the arguments must survive");
        assert_eq!(v[0].engine_abi, 7);
        assert_eq!(v[0].last_opened_utc, "1", "renaming is not opening");
    }

    #[test]
    fn rename_recent_does_not_reorder_the_list() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        // b is at the front; renaming a must not promote it.
        assert!(rename_recent(&mut v, "C:/a", "C:/z", "z"));
        assert_eq!(v[0].path, "C:/b");
        assert_eq!(v[1].path, "C:/z");
    }

    #[test]
    fn rename_recent_absorbs_a_colliding_entry() {
        // Renaming onto a path already in the list must leave ONE row, not two
        // rows the user then has to reconcile by hand.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        touch_recent(&mut v, rp("C:/b", "2"));
        assert!(rename_recent(&mut v, "C:/a", "c:\\b", "b"));
        assert_eq!(v.len(), 1);
        // The RENAMED entry is the survivor, spelled as the caller passed it.
        assert_eq!(v[0].path, "c:\\b");
    }

    #[test]
    fn rename_recent_reports_an_unknown_project() {
        let mut v = Vec::new();
        assert!(!rename_recent(&mut v, "C:/a", "C:/b", "b"));
    }

    #[test]
    fn relocate_recent_keeps_the_pin_and_args_but_refreshes_name_and_abi() {
        // The split that makes this NOT rename_recent: the manifest at the new
        // path is the authority on name and ABI (the project may have changed
        // while it was away), while the pin and arguments are Hub state that
        // the move cannot have touched.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/Games/Old/Old.arcproj", "1"));
        set_project_engine(&mut v, "C:/Games/Old/Old.arcproj", Some("eng-1".into()));
        set_project_args(&mut v, "C:/Games/Old/Old.arcproj", "--frames 3");
        v[0].missing = true;

        assert!(relocate_recent(
            &mut v,
            "C:/Games/Old/Old.arcproj",
            "D:/Elsewhere/Old/Old.arcproj",
            "Old",
            8,
            None,
        ));
        assert_eq!(v[0].path, "D:/Elsewhere/Old/Old.arcproj");
        assert_eq!(v[0].name, "Old");
        assert_eq!(v[0].engine_abi, 8, "the ABI must come from the manifest just read");
        assert!(!v[0].missing, "a located project is not missing");
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"), "the pin must survive");
        assert_eq!(v[0].args, "--frames 3", "the arguments must survive");
        assert_eq!(v[0].last_opened_utc, "1", "locating is not opening");
    }

    #[test]
    fn project_dir_key_folds_a_manifest_to_its_folder() {
        // Mirror of format.ts projectKey -- the parity the scan dedupe rides.
        assert_eq!(project_dir_key("D:\\Games\\MyGame\\MyGame.arcproj"), "d:/games/mygame");
        assert_eq!(project_dir_key("D:/Games/MyGame"), "d:/games/mygame");
        assert_eq!(project_dir_key("D:/Games/MyGame/"), "d:/games/mygame");
        // A bare manifest name has no folder to fold to; keep it whole.
        assert_eq!(project_dir_key("MyGame.arcproj"), "mygame.arcproj");
    }

    #[test]
    fn add_scanned_appends_only_unlisted_projects_at_the_end() {
        let mut v = vec![rp("C:/Games/Listed/Listed.arcproj", "9")];
        let (added, existing, relocated) = add_scanned(
            &mut v,
            vec![
                // Same project, recorded folder-shaped: must count as existing.
                rp("C:/Games/Listed", "0"),
                rp("C:/Games/New/New.arcproj", "0"),
            ],
        );
        assert_eq!((added, existing, relocated), (1, 1, 0));
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].path, "C:/Games/Listed/Listed.arcproj", "recents stay on top");
        assert_eq!(v[1].path, "C:/Games/New/New.arcproj", "imports append at the end");
    }

    #[test]
    fn add_scanned_dedupes_within_one_scan_too() {
        let mut v = Vec::new();
        let (added, existing, _) = add_scanned(
            &mut v,
            vec![rp("C:/G/A/A.arcproj", "0"), rp("C:/G/A", "0")],
        );
        assert_eq!((added, existing), (1, 1), "one folder, one entry, however recorded");
    }

    #[test]
    fn touch_recent_preserves_a_favorite() {
        // Same carry class as the pin and the arguments: open_project builds a
        // fresh entry that knows nothing about the star, and a LAUNCH must
        // never read as an unfavourite.
        let mut v = vec![rp("C:/Games/A/A.arcproj", "1")];
        v[0].favorite = true;
        touch_recent(&mut v, rp("C:/Games/A/A.arcproj", "2"));
        assert!(v[0].favorite, "launching must not unstar");
    }

    #[test]
    fn set_favorite_stars_and_unstars_in_place() {
        let mut v = vec![rp("C:/Games/A/A.arcproj", "2"), rp("C:/Games/B/B.arcproj", "1")];
        assert!(set_favorite(&mut v, "C:/Games/B/B.arcproj", true));
        assert!(v[1].favorite);
        assert_eq!(v[0].path, "C:/Games/A/A.arcproj", "starring must not reorder");
        assert!(set_favorite(&mut v, "C:/Games/B/B.arcproj", false));
        assert!(!v[1].favorite);
        assert!(!set_favorite(&mut v, "C:/Games/None.arcproj", true), "unknown path is a miss");
    }

    #[test]
    fn a_recents_file_written_before_favorites_existed_loads_unstarred() {
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/Games/A/A.arcproj","name":"A","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert!(!back[0].favorite);
    }

    #[test]
    fn relocate_recent_keeps_the_favorite() {
        let mut v = vec![rp("C:/Games/Old/Old.arcproj", "1")];
        v[0].favorite = true;
        assert!(relocate_recent(
            &mut v,
            "C:/Games/Old/Old.arcproj",
            "D:/Elsewhere/Old/Old.arcproj",
            "Old",
            8,
            None,
        ));
        assert!(v[0].favorite, "moving a folder does not change what the user starred");
    }

    #[test]
    fn disk_fingerprint_flips_when_a_project_goes_missing() {
        let mut s = HubState {
            recents: vec![rp("C:/Games/A/A.arcproj", "1")],
            ..Default::default()
        };
        let before = disk_fingerprint(&s);
        s.recents[0].missing = true;
        assert_ne!(disk_fingerprint(&s), before, "a move must change the fingerprint");
        s.recents[0].missing = false;
        assert_eq!(disk_fingerprint(&s), before, "and moving it back must restore it");
    }

    #[test]
    fn disk_fingerprint_sees_engines_as_well_as_projects() {
        let mut s = HubState {
            engines: vec![EngineEntry::new("C:/eng/ArcaneEditor.exe", 8, "Arcane 0.1".into())],
            ..Default::default()
        };
        let before = disk_fingerprint(&s);
        s.engines[0].missing = true;
        assert_ne!(disk_fingerprint(&s), before, "an engine's disappearance counts too");
    }

    #[test]
    fn relocate_recent_does_not_reorder_and_absorbs_a_collision() {
        // Locating a moved project onto a path the list ALREADY holds (the user
        // re-added it by hand before finding the stale row) must leave one row.
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/gone", "1"));
        touch_recent(&mut v, rp("C:/found/G.arcproj", "2"));
        assert!(relocate_recent(&mut v, "C:/gone", "c:\\found\\G.arcproj", "G", 7, None));
        assert_eq!(v.len(), 1);
        // The RELOCATED entry is the survivor, spelled as the caller passed it.
        assert_eq!(v[0].path, "c:\\found\\G.arcproj");
    }

    #[test]
    fn relocate_recent_reports_an_unknown_project() {
        let mut v = Vec::new();
        assert!(!relocate_recent(&mut v, "C:/a", "C:/b", "b", 7, None));
    }

    const GUID: &str = "a5e0c1de-1111-4222-8333-444455556666";

    fn rp_guided(path: &str, when: &str, guid: &str, missing: bool) -> RecentProject {
        let mut e = rp(path, when);
        e.guid = Some(guid.to_string());
        e.missing = missing;
        e
    }

    #[test]
    fn heal_by_guid_repoints_a_missing_entry_and_keeps_its_metadata() {
        // The moved-folder story: the entry went missing, the project turned up
        // elsewhere with the same guid -- everything the user set survives.
        let mut v = vec![rp_guided("C:/Games/Old/Old.arcproj", "5", GUID, true)];
        v[0].favorite = true;
        v[0].engine_id = Some("eng-1".into());
        v[0].args = "--frames 3".into();

        assert!(heal_by_guid(&mut v, GUID, "D:/New/Old/Old.arcproj", "Old", 8));
        assert_eq!(v.len(), 1, "healed, not duplicated");
        assert_eq!(v[0].path, "D:/New/Old/Old.arcproj");
        assert!(!v[0].missing);
        assert!(v[0].favorite, "the star survives the move");
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"), "the pin survives");
        assert_eq!(v[0].args, "--frames 3", "the arguments survive");
        assert_eq!(v[0].last_opened_utc, "5", "healing is not opening");
        assert_eq!(v[0].guid.as_deref(), Some(GUID));
    }

    #[test]
    fn heal_by_guid_refuses_a_hand_copy_of_a_living_project() {
        // THE anti-Unity rule: same guid at a new path while the original still
        // exists on disk (missing == false) is a hand-copied folder. Healing
        // would let the copy steal the original's entry.
        let mut v = vec![rp_guided("C:/Games/G/G.arcproj", "5", GUID, false)];
        assert!(!heal_by_guid(&mut v, GUID, "C:/Copies/G/G.arcproj", "G", 8));
        assert_eq!(v[0].path, "C:/Games/G/G.arcproj", "the original entry is untouched");
    }

    #[test]
    fn heal_by_guid_needs_a_recorded_guid_to_match() {
        // An entry from before the guid landed (or never launched since) has
        // None recorded; it cannot be healed, only Located by hand.
        let mut v = vec![rp("C:/Games/Old/Old.arcproj", "5")];
        v[0].missing = true;
        assert!(!heal_by_guid(&mut v, GUID, "D:/New/Old/Old.arcproj", "Old", 8));
    }

    #[test]
    fn add_scanned_heals_a_missing_entry_instead_of_appending_a_stranger() {
        let mut v = vec![rp_guided("C:/Games/Moved/Moved.arcproj", "5", GUID, true)];
        v[0].favorite = true;

        let (added, existing, relocated) = add_scanned(
            &mut v,
            vec![
                rp_guided("D:/Found/Moved/Moved.arcproj", "0", GUID, false),
                rp("D:/Found/Fresh/Fresh.arcproj", "0"),
            ],
        );
        assert_eq!((added, existing, relocated), (1, 0, 1));
        assert_eq!(v.len(), 2, "one healed in place, one appended");
        assert_eq!(v[0].path, "D:/Found/Moved/Moved.arcproj", "healed, in its old position");
        assert!(v[0].favorite, "with its star");
        assert_eq!(v[1].path, "D:/Found/Fresh/Fresh.arcproj");
    }

    #[test]
    fn add_scanned_lists_a_hand_copy_as_its_own_project() {
        // Scan finds a copied folder while the original is alive and listed:
        // same guid, but the original is not missing -- two rows is the truth.
        let mut v = vec![rp_guided("C:/Games/G/G.arcproj", "5", GUID, false)];
        let (added, existing, relocated) =
            add_scanned(&mut v, vec![rp_guided("C:/Copies/G/G.arcproj", "0", GUID, false)]);
        assert_eq!((added, existing, relocated), (1, 0, 0));
        assert_eq!(v.len(), 2);
    }

    #[test]
    fn touch_recent_carries_the_guid_and_lets_a_fresh_read_win() {
        let mut v = vec![rp_guided("C:/G/G.arcproj", "1", GUID, false)];
        // A toucher that could not read the manifest keeps the known identity...
        touch_recent(&mut v, rp("C:/G/G.arcproj", "2"));
        assert_eq!(v[0].guid.as_deref(), Some(GUID));
        // ...and one that did read it refreshes.
        let other = "b6f1d2ef-2222-4333-9444-555566667777";
        touch_recent(&mut v, rp_guided("C:/G/G.arcproj", "3", other, false));
        assert_eq!(v[0].guid.as_deref(), Some(other));
    }

    #[test]
    fn relocate_recent_records_a_guid_but_never_forgets_one() {
        let mut v = vec![rp_guided("C:/old", "1", GUID, true)];
        // Locate onto a pre-guid manifest: None must not erase what is known.
        assert!(relocate_recent(&mut v, "C:/old", "C:/new", "N", 8, None));
        assert_eq!(v[0].guid.as_deref(), Some(GUID));
        // Locate onto a stamped manifest: the fresh read wins.
        let other = "b6f1d2ef-2222-4333-9444-555566667777";
        assert!(relocate_recent(&mut v, "C:/new", "C:/newer", "N", 8, Some(other.into())));
        assert_eq!(v[0].guid.as_deref(), Some(other));
    }

    #[test]
    fn backfill_guids_fills_only_live_entries_that_lack_one() {
        let mut v = vec![
            rp("C:/live/NoGuid.arcproj", "1"),                       // live, none -> filled
            rp_guided("C:/live/Has.arcproj", "2", GUID, false),      // live, has -> kept
            rp("C:/gone/Missing.arcproj", "3"),                      // missing -> untouched
        ];
        v[2].missing = true;

        let filled = "b6f1d2ef-2222-4333-9444-555566667777";
        let changed = backfill_guids(&mut v, |p| {
            assert!(!p.contains("gone"), "a missing path must never be looked up");
            if p.contains("NoGuid") { Some(filled.into()) } else { Some("WRONG".into()) }
        });
        assert_eq!(changed, 1);
        assert_eq!(v[0].guid.as_deref(), Some(filled));
        assert_eq!(v[1].guid.as_deref(), Some(GUID), "a recorded guid is never overwritten");
        assert_eq!(v[2].guid, None);
    }

    #[test]
    fn backfill_guids_reports_zero_when_manifests_have_no_guid_yet() {
        // Pre-guid manifests answer None; nothing changes, nothing saves.
        let mut v = vec![rp("C:/live/Old.arcproj", "1")];
        assert_eq!(backfill_guids(&mut v, |_| None), 0);
        assert_eq!(v[0].guid, None);
    }

    #[test]
    fn a_recents_file_written_before_guids_existed_loads_none() {
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert_eq!(back[0].guid, None);
    }

    #[test]
    fn an_engines_file_written_before_missing_existed_still_loads() {
        let back: Vec<EngineEntry> = serde_json::from_str(
            r#"[{"id":"c:/e.exe","path":"C:/e.exe","engineAbi":7,"build":"b"}]"#,
        )
        .unwrap();
        assert!(!back[0].missing);
    }

    #[test]
    fn a_recents_file_written_before_missing_existed_still_loads() {
        // serde(default) on missing: an older file has no such key -- and load()
        // re-stamps it from the disk anyway, so false is only the parse default.
        let back: Vec<RecentProject> = serde_json::from_str(
            r#"[{"path":"C:/a","name":"N","lastOpenedUtc":"1","engineAbi":7}]"#,
        )
        .unwrap();
        assert!(!back[0].missing);
    }

    #[test]
    fn remove_recent_reports_whether_it_removed() {
        let mut v = Vec::new();
        touch_recent(&mut v, rp("C:/a", "1"));
        assert!(remove_recent(&mut v, "C:/a"));
        assert!(!remove_recent(&mut v, "C:/a"), "second remove is a no-op");
        assert!(v.is_empty());
    }

    #[test]
    fn upsert_engine_replaces_same_path_rather_than_appending() {
        let mut v = Vec::new();
        upsert_engine(&mut v, ee("C:/eng/ArcaneEditor.exe", 7, "old"));
        upsert_engine(&mut v, ee("C:/ENG/arcaneeditor.exe", 8, "new"));
        assert_eq!(v.len(), 1, "re-registering must not append");
        assert_eq!(v[0].engine_abi, 8, "re-registering must refresh probed data");
        assert_eq!(v[0].build, "new");
    }

    #[test]
    fn two_distinct_engines_coexist() {
        // A dev build and a release install side by side is the day-one case.
        let mut v = Vec::new();
        upsert_engine(&mut v, ee("C:/repo/bin/Debug/ArcaneEditor.exe", 7, "Debug"));
        upsert_engine(&mut v, ee("C:/repo/bin/Release/ArcaneEditor.exe", 7, "Release"));
        assert_eq!(v.len(), 2);
        assert!(remove_engine(&mut v, "C:/repo/bin/Debug/ArcaneEditor.exe"));
        assert_eq!(v.len(), 1);
    }

    #[test]
    fn empty_state_is_valid_not_an_error() {
        // An installed Hub with no engine registered is a NORMAL first run.
        let s = HubState::default();
        assert!(s.engines.is_empty());
        assert!(s.recents.is_empty());
    }

    #[test]
    fn hub_state_serialises_every_field_the_frontend_reads() {
        // The round-trip test below cannot catch a dropped field: it compares a
        // value whose `warnings` is empty, so a skipped field still round-trips
        // to an equal struct. This asserts the KEY reaches the wire, which is
        // what the frontend actually destructures.
        let mut s = HubState::default();
        s.warnings.push("something went wrong".to_string());
        let text = serde_json::to_string(&s).unwrap();
        let doc: serde_json::Value = serde_json::from_str(&text).unwrap();

        assert!(doc.get("recents").is_some_and(|v| v.is_array()));
        assert!(doc.get("engines").is_some_and(|v| v.is_array()));
        assert!(
            doc.get("warnings").is_some_and(|v| v.is_array()),
            "load_state is this struct's only Serialize consumer -- a missing \
             key here is `undefined` in the UI, not an empty list: {text}"
        );
        assert_eq!(doc["warnings"][0], "something went wrong");
        // The frontend drops snapshots older than the last one it adopted;
        // without the key every snapshot reads as revision `undefined`.
        assert!(doc.get("revision").is_some_and(|v| v.is_u64()), "{text}");
    }

    #[test]
    fn state_round_trips_through_json() {
        let mut s = HubState::default();
        touch_recent(&mut s.recents, rp("C:/a", "1"));
        upsert_engine(&mut s.engines, ee("C:/e.exe", 7, "b"));
        let text = serde_json::to_string(&s).unwrap();
        let back: HubState = serde_json::from_str(&text).unwrap();
        assert_eq!(back, s);
    }

    #[test]
    fn corrupt_state_text_does_not_parse_so_the_caller_falls_back() {
        let back: Result<HubState, _> = serde_json::from_str::<HubState>("{ not json");
        assert!(back.is_err());
    }

    #[test]
    fn a_state_file_missing_one_list_still_loads_the_other() {
        // Forward/backward compat: serde(default) on both lists.
        let back: HubState = serde_json::from_str(r#"{"recents":[]}"#).unwrap();
        assert!(back.engines.is_empty());
    }

    // ---- identity folding (project_dir_key) -------------------------------

    #[test]
    fn a_folder_shaped_row_and_a_manifest_launch_are_one_project() {
        // A legacy row recorded the FOLDER; a launch records the MANIFEST.
        // With the narrower key the launch minted a second row and dropped
        // the pin and the star on the floor.
        let mut v = vec![rp("D:/Games/My", "1")];
        v[0].favorite = true;
        v[0].engine_id = Some("eng-1".into());
        touch_recent(&mut v, rp("D:\\Games\\My\\My.arcproj", "2"));
        assert_eq!(v.len(), 1, "one project, one row");
        assert_eq!(v[0].path, "D:\\Games\\My\\My.arcproj", "the launch's spelling wins");
        assert!(v[0].favorite, "the star carries");
        assert_eq!(v[0].engine_id.as_deref(), Some("eng-1"), "the pin carries");
    }

    #[test]
    fn every_in_place_edit_finds_a_project_by_either_shape() {
        let mut v = vec![rp("D:/Games/My/My.arcproj", "1")];
        assert!(set_favorite(&mut v, "d:\\games\\my", true));
        assert!(set_project_args(&mut v, "D:/Games/My", "--x"));
        assert!(set_project_engine(&mut v, "D:/Games/My/", Some("e".into())));
        assert!(rename_recent(&mut v, "D:/Games/My", "D:/Games/Ny/Ny.arcproj", "Ny"));
        assert!(relocate_recent(&mut v, "d:/games/ny", "E:/Ny/Ny.arcproj", "Ny", 7, None));
        assert!(remove_recent(&mut v, "E:/Ny"));
        assert!(v.is_empty());
    }

    // ---- heal guard --------------------------------------------------------

    #[test]
    fn heal_by_guid_skips_a_path_that_is_already_listed() {
        // A worktree/clone sharing its guid with a MISSING sibling, launched
        // while it is itself listed: healing used to repoint the sibling onto
        // it and the collision absorb dropped the launched project's own row,
        // swapping in the sibling's pin, arguments and star.
        let mut v = vec![
            rp_guided("D:/Work/Tree/G.arcproj", "5", GUID, false),
            rp_guided("D:/Gone/G/G.arcproj", "4", GUID, true),
        ];
        v[0].engine_id = Some("mine".into());
        v[1].engine_id = Some("sibling".into());
        v[1].favorite = true;

        assert!(!heal_by_guid(&mut v, GUID, "D:/Work/Tree/G.arcproj", "G", 8));
        assert_eq!(v.len(), 2, "nothing merged");
        assert_eq!(v[0].engine_id.as_deref(), Some("mine"), "the launched row keeps its own pin");
        assert!(!v[0].favorite, "and does not inherit the sibling's star");
        assert!(v[1].missing, "the sibling stays as it was");
        // Folder-shaped spelling of the same listed project: still listed.
        assert!(!heal_by_guid(&mut v, GUID, "d:\\work\\tree", "G", 8));
    }

    // ---- relocate conflict -------------------------------------------------

    #[test]
    fn relocate_conflict_refuses_a_live_listed_target_only() {
        let mut v = vec![rp("C:/gone/G.arcproj", "1"), rp("C:/found/G.arcproj", "2")];
        v[0].missing = true;
        v[1].name = "Found".into();
        let why = relocate_conflict(&v, "C:/gone/G.arcproj", "c:\\found\\G.arcproj").unwrap();
        assert!(why.contains("already in the list as Found"), "{why}");

        // A MISSING entry at the target is the re-added-then-found case the
        // absorb exists for; it is not a conflict.
        v[1].missing = true;
        assert_eq!(relocate_conflict(&v, "C:/gone/G.arcproj", "C:/found/G.arcproj"), None);
        // Nor is pointing an entry at its own folder.
        assert_eq!(relocate_conflict(&v, "C:/gone/G.arcproj", "C:/gone"), None);
        // Nor an unlisted target.
        assert_eq!(relocate_conflict(&v, "C:/gone/G.arcproj", "C:/new/G.arcproj"), None);
    }

    // ---- add_existing (the add_project command) ----------------------------

    #[test]
    fn add_existing_lists_a_new_project_as_never_opened() {
        let mut v = vec![rp("C:/a/A.arcproj", "9")];
        let got = add_existing(&mut v, rp("C:/b/B.arcproj", "123"));
        assert_eq!(got.last_opened_utc, "0", "adding is not opening");
        assert_eq!(v.len(), 2);
        assert_eq!(got, v.iter().find(|e| e.path == "C:/b/B.arcproj").unwrap().clone());
    }

    #[test]
    fn add_existing_returns_a_listed_project_unchanged_and_in_place() {
        let mut v = vec![rp("C:/a/A.arcproj", "9"), rp("C:/b", "8")];
        v[1].favorite = true;
        let before = v.clone();
        let got = add_existing(&mut v, rp("C:/b/B.arcproj", "123"));
        assert_eq!(v, before, "no reorder, no restamp, no respelling");
        assert_eq!(got, before[1]);
    }

    #[test]
    fn add_existing_heals_a_moved_project_by_guid() {
        let mut v = vec![rp_guided("C:/old/G/G.arcproj", "5", GUID, true)];
        v[0].favorite = true;
        let got = add_existing(&mut v, rp_guided("D:/new/G/G.arcproj", "0", GUID, false));
        assert_eq!(v.len(), 1, "healed, not duplicated");
        assert_eq!(got.path, "D:/new/G/G.arcproj");
        assert!(got.favorite, "the star survives");
        assert_eq!(got.last_opened_utc, "5", "the history survives");
    }

    // ---- load / mutate (temp-dir backed; NEVER the real hub dir) -----------

    fn scratch(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-state-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn files(dir: &Path) -> (std::path::PathBuf, std::path::PathBuf) {
        (dir.join("recents.archub"), dir.join("engines.archub"))
    }

    #[test]
    fn every_load_carries_a_higher_revision() {
        let dir = scratch("revision");
        let (r, e) = files(&dir);
        let a = load_at(&r, &e).revision;
        let b = load_at(&r, &e).revision;
        assert!(a >= 1, "0 is reserved for never-loaded");
        assert!(b > a, "a later snapshot must outrank an earlier one: {a} then {b}");
    }

    #[test]
    fn mutate_serialises_concurrent_writers_without_losing_updates() {
        // THE lost-update bug: two threads each load, bump, save -- and one
        // bump vanishes. Under mutate every one of them must land.
        let dir = scratch("mutate-concurrent");
        let (r, e) = files(&dir);
        mutate_at(&r, &e, |s| {
            s.engines.push(EngineEntry::new("C:/eng/ArcaneEditor.exe", 0, "b".into()));
            Ok(())
        })
        .unwrap();

        let threads: Vec<_> = (0..8)
            .map(|_| {
                let (r, e) = (r.clone(), e.clone());
                std::thread::spawn(move || {
                    for _ in 0..25 {
                        mutate_at(&r, &e, |s| {
                            s.engines[0].engine_abi += 1;
                            Ok(())
                        })
                        .unwrap();
                    }
                })
            })
            .collect();
        for t in threads {
            t.join().unwrap();
        }
        assert_eq!(load_at(&r, &e).engines[0].engine_abi, 200, "every update landed");
    }

    #[test]
    fn mutate_saves_nothing_on_error_or_on_a_no_op() {
        let dir = scratch("mutate-noop");
        let (r, e) = files(&dir);
        let err = mutate_at(&r, &e, |s| -> Result<(), String> {
            s.recents.push(rp("C:/x", "1"));
            Err("refused".into())
        });
        assert_eq!(err, Err("refused".to_string()));
        assert!(!r.exists() && !e.exists(), "an Err must not save");

        let n = mutate_at(&r, &e, |s| Ok(s.recents.len())).unwrap();
        assert_eq!(n, 0);
        assert!(!r.exists() && !e.exists(), "a no-op must not write the files");

        mutate_at(&r, &e, |s| {
            s.recents.push(rp("C:/x", "1"));
            Ok(())
        })
        .unwrap();
        assert_eq!(load_at(&r, &e).recents.len(), 1, "a real change saves");
    }

    #[test]
    fn mutate_recovers_from_a_poisoned_lock() {
        // A panicking closure poisons STATE_LOCK; every later command must
        // still run instead of panicking on unwrap.
        let dir = scratch("mutate-poison");
        let (r, e) = files(&dir);
        let (r2, e2) = (r.clone(), e.clone());
        let _ = std::thread::spawn(move || {
            let _ = mutate_at(&r2, &e2, |_| -> Result<(), String> { panic!("boom") });
        })
        .join();
        assert!(mutate_at(&r, &e, |_| Ok(())).is_ok());
    }
}
