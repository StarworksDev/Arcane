// The Hub's half of the editor lock: read <root>/Saved/editor.lock and
// decide whether the editor it names is STILL RUNNING. The editor writes the
// lock while it holds a project (Arcane's Project.cpp, EditorLock namespace
// -- MIRRORED FORMAT, change both) and clears it on clean exit; this is how
// a fresh Hub instance knows which projects are open after close-mode killed
// the in-memory pid map with the process.
//
// Unity's lockfile model minus the flaw its users hate: the lock names
// {pid, process CREATION time}, and read_live only believes it when that
// exact pid with that exact birth is alive. A crash's stale lock fails the
// check and is ignored -- never a false "already open"; a recycled pid has
// a different birth and fails the same way.
//
// THREE tells, matching C++ EditorLock::ReadLive (Project.cpp). Tells 1+2
// (pid opens, birth matches) are not enough: a process OBJECT outlives the
// process while any handle remains, and the Hub's wait thread holds exactly
// that handle (`Child`). Exit time is the tell that cannot be faked by a
// lingering handle -- nonzero means it exited. sweep_stale then deletes a
// lock that PARSES and whose editor is PROVEN dead, so Saved/ does not keep
// a crash's lock forever (the Hub never owned Clear; only the editor did) --
// but never one it merely could not validate (see Probe).

use std::path::Path;

/// What a lock file claims. Pure parse so the format contract is testable.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Lock {
    pub pid: u32,
    /// Process creation FILETIME as u64; 0 = "no time recorded" (validation
    /// then degrades to pid-exists, still better than trusting the file).
    pub start: u64,
}

pub fn parse(text: &str) -> Option<Lock> {
    let doc: serde_json::Value = serde_json::from_str(text).ok()?;
    let pid = u32::try_from(doc.get("pid")?.as_u64()?).ok()?;
    if pid == 0 {
        return None;
    }
    let start = doc.get("start").and_then(|s| s.as_u64()).unwrap_or(0);
    Some(Lock { pid, start })
}

pub fn lock_path(project_root: &Path) -> std::path::PathBuf {
    project_root.join("Saved").join("editor.lock")
}

struct ProcessTimes {
    start: u64,
    /// Nonzero FILETIME means the process has exited; the object may still
    /// be queryable because a handle (the Hub's Child) is keeping it around.
    exited: bool,
}

/// What asking Windows about a pid established. THREE answers, not two:
/// "no such process" and "not allowed to ask" used to collapse into one
/// None, and sweep_stale then deleted the lock of an editor it merely could
/// not see -- another user's editor on a shared drive, or one running
/// elevated -- leaving that project unguarded against a second editor.
enum Probe {
    /// The process object exists; here are its times.
    Times(ProcessTimes),
    /// Windows says there is no such process (ERROR_INVALID_PARAMETER from
    /// OpenProcess): proven gone.
    Absent,
    /// Anything else -- access denied above all: the process may well be
    /// alive, we just cannot prove either way.
    Unknown,
}

/// ERROR_INVALID_PARAMETER is what OpenProcess reports for a pid that names
/// no process. ERROR_ACCESS_DENIED (5), and every other code, prove nothing
/// about liveness. Pure so the classification is tested without needing a
/// process this user cannot open.
fn classify_open_error(code: u32) -> Probe {
    const ERROR_INVALID_PARAMETER: u32 = 87;
    if code == ERROR_INVALID_PARAMETER {
        Probe::Absent
    } else {
        Probe::Unknown
    }
}

fn probe(pid: u32) -> Probe {
    use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, FILETIME};
    use windows_sys::Win32::System::Threading::{
        GetProcessTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION,
    };
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() {
            return classify_open_error(GetLastError());
        }
        let mut created: FILETIME = std::mem::zeroed();
        let mut exited: FILETIME = std::mem::zeroed();
        let mut kernel: FILETIME = std::mem::zeroed();
        let mut user: FILETIME = std::mem::zeroed();
        let ok = GetProcessTimes(h, &mut created, &mut exited, &mut kernel, &mut user);
        CloseHandle(h);
        if ok == 0 {
            return Probe::Unknown;
        }
        Probe::Times(ProcessTimes {
            start: ((created.dwHighDateTime as u64) << 32) | created.dwLowDateTime as u64,
            exited: exited.dwHighDateTime != 0 || exited.dwLowDateTime != 0,
        })
    }
}

/// Is `pid` a running process? Exited (even while a handle keeps its object
/// around) or absent = dead; a process this user may not query = ALIVE,
/// because "cannot see it" is not evidence of death. The running-editors
/// map uses this to decide whether a mapped editor is still there -- it used
/// to ask "does it have a window yet", which an editor in its first seconds
/// never does.
pub fn pid_alive(pid: u32) -> bool {
    match probe(pid) {
        Probe::Times(t) => !t.exited,
        Probe::Absent => false,
        Probe::Unknown => true,
    }
}

/// Whether a parsed lock's editor is still the process it described.
/// Some(true) = proven live, Some(false) = proven dead (absent, exited, or a
/// recycled pid with a different birth), None = cannot tell.
fn lock_alive(lock: &Lock) -> Option<bool> {
    match probe(lock.pid) {
        Probe::Absent => Some(false),
        Probe::Unknown => None,
        Probe::Times(t) => {
            if lock.start != 0 && lock.start != t.start {
                Some(false)
            } else {
                Some(!t.exited)
            }
        }
    }
}

/// Some(pid) only when the project's lock names a process that is PROVEN to
/// still be the process it described. Every other shape -- no file,
/// unparseable, dead pid, recycled pid with a different birth,
/// exited-but-handle-held, or a process we may not query -- reads as "not
/// running": a badge or a refused launch must rest on evidence, never on a
/// file we cannot validate (Unity's stale-lock flaw).
pub fn read_live(project_root: &Path) -> Option<u32> {
    let text = std::fs::read_to_string(lock_path(project_root)).ok()?;
    let lock = parse(&text)?;
    match lock_alive(&lock) {
        Some(true) => Some(lock.pid),
        _ => None,
    }
}

/// Delete `editor.lock` only when it PARSES and its editor is PROVEN dead:
/// absent, exited, or a different birth. The mirror image of read_live's
/// caution, on purpose: read_live will not CLAIM "running" without proof,
/// and this will not DESTROY a lock without proof. An unparseable lock (a
/// newer editor's format, a write in progress) or one naming a process we
/// may not query (another user's editor on a shared drive) is left alone --
/// both were deleted before, unguarding a project a live editor may hold.
/// Returns true when a file was removed. Called on Hub start (close-mode
/// leftover), by the disk watcher, and after the wait thread's child.wait()
/// (the handle is still held -- tell 3 is why that is safe then).
pub fn sweep_stale(project_root: &Path) -> bool {
    let file = lock_path(project_root);
    let Ok(text) = std::fs::read_to_string(&file) else {
        return false;
    };
    let Some(lock) = parse(&text) else {
        return false;
    };
    if lock_alive(&lock) != Some(false) {
        return false;
    }
    std::fs::remove_file(&file).is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::windows::process::CommandExt;
    use std::path::PathBuf;
    use std::process::Command;

    const CREATE_NO_WINDOW: u32 = 0x0800_0000;

    #[test]
    fn parse_mirrors_the_engine_side_format() {
        // The same cases ProjectTest.cpp pins C++-side -- one format, two
        // parsers, and these tests are the tripwire between them.
        let l = parse(r#"{"pid":4242,"start":1311768467463790320}"#).unwrap();
        assert_eq!(l, Lock { pid: 4242, start: 0x1234_5678_9ABC_DEF0 });
        assert!(parse("not json").is_none());
        assert!(parse("{}").is_none());
        assert!(parse(r#"{"pid":0}"#).is_none(), "pid 0 is no process");
        assert!(parse(r#"{"pid":-3}"#).is_none());
        assert_eq!(parse(r#"{"pid":7}"#), Some(Lock { pid: 7, start: 0 }),
                   "missing start degrades to 0, not a refusal");
    }

    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-lock-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(dir.join("Saved")).unwrap();
        dir
    }

    fn write_lock(root: &Path, pid: u32, start: u64) {
        std::fs::write(
            lock_path(root),
            format!(r#"{{"pid":{pid},"start":{start}}}"#),
        )
        .unwrap();
    }

    #[test]
    fn a_lock_naming_this_live_process_reads_as_running() {
        let root = scratch("live");
        let pid = std::process::id();
        let Probe::Times(times) = probe(pid) else { panic!("own process must be queryable") };
        assert!(!times.exited, "the test process has not exited");
        write_lock(&root, pid, times.start);
        assert_eq!(read_live(&root), Some(pid));
        assert!(!sweep_stale(&root), "must not delete a live editor's lock");
        assert!(lock_path(&root).is_file());
    }

    #[test]
    fn a_stale_lock_with_the_wrong_birth_reads_as_not_running() {
        // THE Unity failure this design exists to avoid: same pid, different
        // creation time = a recycled pid or a crash's leftovers, never a
        // reason to refuse a launch.
        let root = scratch("stale");
        let pid = std::process::id();
        let Probe::Times(times) = probe(pid) else { panic!("own process must be queryable") };
        write_lock(&root, pid, times.start ^ 1);
        assert_eq!(read_live(&root), None);
        assert!(sweep_stale(&root), "a birth-mismatch lock is dead; delete it");
        assert!(!lock_path(&root).is_file());
    }

    #[test]
    fn no_lock_file_reads_as_not_running() {
        let root = scratch("none");
        assert_eq!(read_live(&root), None);
        assert!(!sweep_stale(&root));
    }

    #[test]
    fn an_exited_process_is_stale_even_while_a_handle_keeps_the_pid_reserved() {
        // The 2026-09-08 desk-pass zombie, Hub-side. C++ ReadLive grew tell 3
        // (nonzero exit time); this file did not, so a Hub holding `Child`
        // after the editor died kept OpenProcess succeeding with the original
        // birth and the project read as open forever.
        let root = scratch("zombie");
        let mut child = Command::new("cmd.exe")
            .args(["/C", "exit", "0"])
            .creation_flags(CREATE_NO_WINDOW)
            .spawn()
            .expect("cmd.exe /C exit");
        let pid = child.id();
        child.wait().expect("child exits");
        // `child` is still in scope -- that handle is the zombie.
        let Probe::Times(times) = probe(pid) else { panic!("handle keeps the process object queryable") };
        assert!(times.exited, "it really did exit");
        write_lock(&root, pid, times.start);
        assert_eq!(read_live(&root), None, "tell 3: exited is not live");
        assert!(sweep_stale(&root));
        assert!(!lock_path(&root).is_file());
    }

    /// A pid no process can have: Windows pids are multiples of 4 well below
    /// this, and OpenProcess answers ERROR_INVALID_PARAMETER for it.
    const NO_SUCH_PID: u32 = 0xFFFF_FFF0;

    #[test]
    fn an_absent_pid_is_dead_and_its_lock_is_swept() {
        assert!(matches!(probe(NO_SUCH_PID), Probe::Absent));
        assert!(!pid_alive(NO_SUCH_PID));
        let root = scratch("absent");
        write_lock(&root, NO_SUCH_PID, 12345);
        assert_eq!(read_live(&root), None);
        assert!(sweep_stale(&root), "proven absent: delete");
        assert!(!lock_path(&root).is_file());
    }

    #[test]
    fn an_unparseable_lock_is_never_swept() {
        // A newer editor's format, or a write caught half-way: nothing is
        // proven, so the file stays (it was deleted before).
        let root = scratch("garbage");
        std::fs::write(lock_path(&root), "{ not a lock").unwrap();
        assert_eq!(read_live(&root), None, "not believed as running either");
        assert!(!sweep_stale(&root), "but not destroyed");
        assert!(lock_path(&root).is_file());
    }

    #[test]
    fn access_denied_is_unknown_not_absent() {
        // The other user's editor on a shared drive: OpenProcess says
        // ERROR_ACCESS_DENIED (5). That must never read as "gone".
        assert!(matches!(classify_open_error(5), Probe::Unknown));
        assert!(matches!(classify_open_error(87), Probe::Absent));
        assert!(matches!(classify_open_error(1), Probe::Unknown));
    }

    #[test]
    fn pid_alive_tracks_a_real_process_through_its_exit() {
        assert!(pid_alive(std::process::id()), "the test process is alive");
        let mut child = Command::new("cmd.exe")
            .args(["/C", "exit", "0"])
            .creation_flags(CREATE_NO_WINDOW)
            .spawn()
            .expect("cmd.exe /C exit");
        let pid = child.id();
        child.wait().expect("child exits");
        // Handle still held: the object exists but has exited.
        assert!(!pid_alive(pid), "exited is dead even while a handle lingers");
    }
}
