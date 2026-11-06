// JSON file persistence for the Hub's per-user state.
//
// One place, shared by state.rs and settings.rs, which previously each
// hand-rolled the same read-or-default + create_dir_all + write sequence. Two
// copies meant two chances to get durability wrong, and both had it wrong the
// same way.
//
// Four rules this enforces that the hand-rolled versions did not:
//
// 1. A write is ATOMIC and DURABLE. Writing in place leaves a truncated file if
//    the process dies mid-write, and a truncated file is indistinguishable from
//    a corrupt one on the next read; the temp file is flushed to disk before
//    the rename so a power cut cannot publish a name over unwritten blocks.
// 2. A file that EXISTS but does not parse is PRESERVED, not silently replaced.
//    The old `read_to_string().ok().and_then(parse).unwrap_or_default()` chain
//    turned one bad byte into "the user has no projects", and the next save
//    then overwrote the evidence. Recovering by hand is only possible if the
//    bytes still exist -- so each quarantine gets its own timestamped name.
// 3. Every file carries a FORMAT VERSION (the `{version, items}` envelope), so
//    a future breaking shape change has a seam to branch on -- and a file from
//    a NEWER Hub is recognised and left alone rather than quarantined as
//    corrupt. serde(default) still covers additive drift without a bump.
// 4. A file this session could not READ, or that a newer Hub wrote, is never
//    WRITTEN this session (the REFUSED set). Rule 3 once stopped at "left
//    alone on read" -- and the very next save wrote this build's defaults over
//    it, wiping the newer Hub's engines and projects anyway.

use serde::{de::DeserializeOwned, Deserialize, Serialize};
use std::collections::HashSet;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Mutex;

/// The on-disk format this build writes and the newest it understands.
///
/// Bump it ONLY with a breaking shape change, alongside the migration that
/// reads the old shape -- additive fields ride serde(default) and need no
/// bump. Version 1 is the `{version, items}` envelope introduced 2026-07-28.
/// (The bare pre-envelope shape was readable for a few hours that day; that
/// fallback was removed once this machine's files -- the only machine's --
/// were confirmed on the envelope.)
const STATE_FORMAT_VERSION: u32 = 1;

/// The envelope every state file is written inside. Store-level on purpose:
/// the version describes the FILE format, not any one payload type, and the
/// IPC surface never sees it -- `read_or_default` unwraps to plain `T`.
#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Versioned<T> {
    version: u32,
    items: T,
}

/// A process-wide counter for scratch-file names. The pid alone was not
/// enough once writers ran on several threads of ONE process (the backfill
/// thread, the shell-open thread, async commands): two threads writing the
/// same target shared `<name>.<pid>.tmp`, so one could truncate or rename
/// away the other's half-written scratch file. pid + counter is unique per
/// write across every thread and every Hub process.
static TEMP_SEQ: AtomicU64 = AtomicU64::new(0);

/// The sibling scratch file one write of `p` goes through.
fn temp_path(p: &Path) -> PathBuf {
    let n = TEMP_SEQ.fetch_add(1, Ordering::Relaxed);
    let mut name = p.file_name().unwrap_or_default().to_os_string();
    name.push(format!(".{}.{n}.tmp", std::process::id()));
    p.with_file_name(name)
}

/// Files this process must NOT overwrite for the rest of the session: one
/// that existed but could not be READ (an IO error other than NotFound), one
/// written by a NEWER Hub, or a corrupt one that could not be set aside. All
/// of them used to fall back to defaults and then the next ordinary save -- a
/// star, a launch -- wrote those defaults over the real file: every engine
/// and project gone because a file was locked for a moment or belonged to a
/// later build. Sticky on purpose: the frontend already holds the defaults it
/// was handed, and a later successful read would not stop it saving them back
/// (save_settings writes the whole object). A QUARANTINED corrupt file is not
/// listed: its bytes are safe beside it, so a fresh file is the recovery.
static REFUSED: Mutex<Option<HashSet<PathBuf>>> = Mutex::new(None);

fn refuse(p: &Path) {
    REFUSED
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .get_or_insert_with(HashSet::new)
        .insert(p.to_path_buf());
}

fn is_refused(p: &Path) -> bool {
    REFUSED
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .as_ref()
        .is_some_and(|set| set.contains(p))
}

/// The error a refused write reports. One wording, asserted by the tests.
fn refused_message(p: &Path) -> String {
    format!(
        "{} could not be read this session (or belongs to a newer Hub), so the Hub \
         will not overwrite it; restart the Hub",
        p.display()
    )
}

/// Where a corrupt file gets moved so its contents survive the recovery:
/// `<name>.<unix-seconds>.corrupt`, or `<name>.<unix-seconds>-<n>.corrupt`
/// when that is taken. TIMESTAMPED because the old fixed `<name>.corrupt` was
/// overwritten by the next corruption, destroying the earlier evidence --
/// the one thing quarantine exists to keep. Pure so the naming is pinned.
fn quarantine_name(p: &Path, stamp: u64, n: u32) -> PathBuf {
    let mut name = p.file_name().unwrap_or_default().to_os_string();
    if n == 0 {
        name.push(format!(".{stamp}.corrupt"));
    } else {
        // Two corruptions inside one second still keep both.
        name.push(format!(".{stamp}-{n}.corrupt"));
    }
    p.with_file_name(name)
}

/// The first free quarantine name for `p` right now.
fn quarantine_path(p: &Path) -> PathBuf {
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let mut n = 0;
    loop {
        let candidate = quarantine_name(p, stamp, n);
        if !candidate.exists() || n >= 1000 {
            return candidate;
        }
        n += 1;
    }
}

/// Read `p`, retrying a non-NotFound failure briefly before giving up: a
/// virus scanner or backup agent can hold a file for a moment, and a refusal
/// is sticky for the session, so one unlucky millisecond must not cost the
/// user the ability to save until they restart.
fn read_with_retry(p: &Path) -> std::io::Result<String> {
    let mut attempt = 0;
    loop {
        match std::fs::read_to_string(p) {
            Ok(t) => return Ok(t),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Err(e),
            Err(e) if attempt >= 2 => return Err(e),
            Err(_) => {
                attempt += 1;
                std::thread::sleep(std::time::Duration::from_millis(25));
            }
        }
    }
}

/// Read and parse `p`, falling back to `T::default()`.
///
/// A missing file is normal (first run) and reports nothing. A file that
/// exists but does not PARSE is moved aside to `<name>.<stamp>.corrupt` and
/// reported through `warnings`, so the caller can tell the user instead of
/// leaving them to discover an empty list on their own; the path stays
/// writable, since the bytes are safe beside it.
///
/// A file that exists but cannot be READ, or that a NEWER Hub wrote, also
/// yields defaults (with a warning) -- but is REFUSED for writing for the
/// rest of the session (see `REFUSED`): the bytes on disk are real data this
/// build could not see, and its defaults must never be saved over them.
pub fn read_or_default<T: Default + DeserializeOwned>(p: &Path, warnings: &mut Vec<String>) -> T {
    let text = match read_with_retry(p) {
        Ok(t) => t,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return T::default(),
        Err(e) => {
            refuse(p);
            warnings.push(format!(
                "Could not read {}: {e}. Running with defaults; the file is untouched and \
                 will not be overwritten this session -- restart the Hub.",
                p.display()
            ));
            return T::default();
        }
    };

    // Version check on the raw document BEFORE typed parsing: a newer format's
    // `items` may not parse under this build's types at all, and that must
    // read as "newer Hub wrote this", never as corruption to quarantine.
    if let Ok(doc) = serde_json::from_str::<serde_json::Value>(&text) {
        if let Some(v) = doc.get("version").and_then(serde_json::Value::as_u64) {
            if v > u64::from(STATE_FORMAT_VERSION) {
                refuse(p);
                warnings.push(format!(
                    "{} was written by a newer Hub (format {v}, this build reads \
                     up to {STATE_FORMAT_VERSION}). Running with defaults; the \
                     file is untouched and will not be overwritten.",
                    p.display()
                ));
                return T::default();
            }
        }
    }

    match serde_json::from_str::<Versioned<T>>(&text) {
        Ok(v) => v.items,
        Err(e) => {
            let kept = quarantine_path(p);
            let note = match std::fs::rename(p, &kept) {
                Ok(()) => format!("kept a copy at {}", kept.display()),
                // Report the failure rather than claiming a copy exists -- and
                // refuse to write over what is now the only copy of the bytes.
                Err(re) => {
                    refuse(p);
                    format!("could not set the file aside ({re}), so it will not be overwritten")
                }
            };
            warnings.push(format!(
                "{} was unreadable ({e}) and has been reset — {note}.",
                p.display()
            ));
            T::default()
        }
    }
}

/// Write raw bytes to `p` atomically and durably: a unique sibling temp file
/// is written, flushed to disk (`sync_all`), then renamed over the target.
/// `fs::rename` replaces an existing file on Windows, so the target is either
/// the old contents or the new ones, never a partial. The flush matters
/// because without it a power cut shortly after the rename can leave the
/// name pointing at blocks that were never written -- a zero-filled file,
/// which reads as corrupt.
///
/// Used for the Hub's own state files (through write_atomic) and for the
/// project manifests rename and duplicate rewrite: a .arcproj truncated
/// mid-write is a project the engine refuses to open.
pub fn write_bytes_atomic(p: &Path, bytes: &[u8]) -> Result<(), String> {
    use std::io::Write as _;
    if is_refused(p) {
        return Err(refused_message(p));
    }
    let dir = p.parent().ok_or_else(|| format!("no parent directory for {}", p.display()))?;
    std::fs::create_dir_all(dir).map_err(|e| format!("create {}: {e}", dir.display()))?;

    let tmp = temp_path(p);
    let written = (|| -> std::io::Result<()> {
        let mut f = std::fs::OpenOptions::new().write(true).create_new(true).open(&tmp)?;
        f.write_all(bytes)?;
        f.sync_all()
    })();
    if let Err(e) = written {
        let _ = std::fs::remove_file(&tmp);
        return Err(format!("write {}: {e}", tmp.display()));
    }
    std::fs::rename(&tmp, p).map_err(|e| {
        // Leave no stray temp file behind on a failed rename.
        let _ = std::fs::remove_file(&tmp);
        format!("replace {}: {e}", p.display())
    })
}

/// Serialise `value` into `p` inside the versioned envelope, atomically (see
/// write_bytes_atomic). Refused for a file this session could not read or
/// that a newer Hub owns (read_or_default's REFUSED set).
pub fn write_atomic<T: Serialize>(p: &Path, value: &T) -> Result<(), String> {
    let text = serde_json::to_string_pretty(&Versioned {
        version: STATE_FORMAT_VERSION,
        items: value,
    })
    .map_err(|e| e.to_string())?;
    write_bytes_atomic(p, text.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde::Deserialize;

    #[derive(Debug, Default, PartialEq, Serialize, Deserialize)]
    struct Sample {
        #[serde(default)]
        items: Vec<String>,
    }

    // Each test gets its own directory so they can run in parallel. std has no
    // tempdir, and a single shared path would make these order-dependent.
    fn scratch(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("arcane-hub-store-test-{tag}"));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// Every quarantined file in `dir`, sorted.
    fn corrupt_files(dir: &Path) -> Vec<PathBuf> {
        let mut v: Vec<PathBuf> = std::fs::read_dir(dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.path())
            .filter(|p| p.to_string_lossy().ends_with(".corrupt"))
            .collect();
        v.sort();
        v
    }

    #[test]
    fn a_missing_file_defaults_without_warning() {
        let dir = scratch("missing");
        let mut w = Vec::new();
        let v: Sample = read_or_default(&dir.join("nope.json"), &mut w);
        assert_eq!(v, Sample::default());
        assert!(w.is_empty(), "first run is normal, not a warning");
    }

    #[test]
    fn write_then_read_round_trips() {
        let dir = scratch("roundtrip");
        let p = dir.join("s.json");
        let v = Sample { items: vec!["a".into()] };
        write_atomic(&p, &v).unwrap();
        let mut w = Vec::new();
        assert_eq!(read_or_default::<Sample>(&p, &mut w), v);
        assert!(w.is_empty());
    }

    #[test]
    fn write_creates_missing_parent_directories() {
        let dir = scratch("mkdir");
        let p = dir.join("nested").join("deeper").join("s.json");
        write_atomic(&p, &Sample::default()).unwrap();
        assert!(p.is_file());
    }

    #[test]
    fn write_leaves_no_temp_file_behind() {
        let dir = scratch("notmp");
        let p = dir.join("s.json");
        write_atomic(&p, &Sample::default()).unwrap();
        let strays: Vec<_> = std::fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .map(|e| e.file_name().to_string_lossy().to_string())
            .filter(|n| n.ends_with(".tmp"))
            .collect();
        assert!(strays.is_empty(), "left behind: {strays:?}");
    }

    #[test]
    fn write_replaces_existing_contents() {
        let dir = scratch("replace");
        let p = dir.join("s.json");
        write_atomic(&p, &Sample { items: vec!["old".into()] }).unwrap();
        write_atomic(&p, &Sample { items: vec!["new".into()] }).unwrap();
        let mut w = Vec::new();
        assert_eq!(read_or_default::<Sample>(&p, &mut w).items, vec!["new".to_string()]);
    }

    #[test]
    fn every_write_gets_its_own_temp_name() {
        // Two threads of one process used to share `<name>.<pid>.tmp`, so one
        // write could truncate or publish the other's half-written scratch.
        let p = Path::new("C:/x/recents.archub");
        let (a, b) = (temp_path(p), temp_path(p));
        assert_ne!(a, b, "same process, same target, two writes: two names");
        let pid = format!(".{}.", std::process::id());
        for t in [&a, &b] {
            let name = t.file_name().unwrap().to_string_lossy().to_string();
            assert!(name.starts_with("recents.archub."), "a sibling of the target: {name}");
            assert!(name.contains(&pid), "carries the pid: {name}");
            assert!(name.ends_with(".tmp"), "{name}");
        }
    }

    #[test]
    fn concurrent_writes_to_one_target_all_succeed() {
        // The thread race the unique temp name closes: with a shared scratch
        // file these failed with "file in use" or published a torn write.
        let dir = scratch("concurrent");
        let p = dir.join("s.json");
        let threads: Vec<_> = (0..8)
            .map(|i| {
                let p = p.clone();
                std::thread::spawn(move || {
                    for j in 0..10 {
                        let v = Sample { items: vec![format!("{i}-{j}")] };
                        // A rename may lose to a concurrent rename's open
                        // handle on Windows; what must never happen is a
                        // torn file. Retry the rare collision.
                        let mut tries = 0;
                        while let Err(e) = write_atomic(&p, &v) {
                            tries += 1;
                            assert!(tries < 50, "write kept failing: {e}");
                            std::thread::sleep(std::time::Duration::from_millis(2));
                        }
                    }
                })
            })
            .collect();
        for t in threads {
            t.join().unwrap();
        }
        let mut w = Vec::new();
        let last: Sample = read_or_default(&p, &mut w);
        assert!(w.is_empty(), "the survivor is a whole document: {w:?}");
        assert_eq!(last.items.len(), 1);
    }

    #[test]
    fn a_file_from_a_newer_hub_is_left_alone_reported_and_never_overwritten() {
        // A downgrade scenario: the file belongs to the newer build, so it is
        // neither quarantined nor parsed -- defaults plus a warning, and the
        // bytes stay exactly where the newer Hub put them. THE BUG THIS PINS:
        // the next ordinary save used to write this build's defaults over it.
        let dir = scratch("toonew");
        let p = dir.join("s.json");
        let future = r#"{"version":99,"items":{"shape":"unknowable"}}"#;
        std::fs::write(&p, future).unwrap();

        let mut w = Vec::new();
        let v: Sample = read_or_default(&p, &mut w);
        assert_eq!(v, Sample::default());
        assert_eq!(w.len(), 1);
        assert!(w[0].contains("newer Hub"), "unexpected warning: {}", w[0]);
        assert!(corrupt_files(&dir).is_empty(), "and never quarantined");

        let err = write_atomic(&p, &Sample { items: vec!["defaults".into()] }).unwrap_err();
        assert!(err.contains("will not overwrite"), "unexpected error: {err}");
        assert_eq!(std::fs::read_to_string(&p).unwrap(), future, "file untouched");
    }

    #[test]
    fn a_file_that_could_not_be_read_is_never_overwritten() {
        // A directory where the file should be is a dependable stand-in for
        // "exists but cannot be read" (a lock, a permissions problem): the
        // read fails with something other than NotFound.
        let dir = scratch("unreadable");
        let p = dir.join("s.json");
        std::fs::create_dir_all(&p).unwrap();

        let mut w = Vec::new();
        let v: Sample = read_or_default(&p, &mut w);
        assert_eq!(v, Sample::default(), "must still start up");
        assert_eq!(w.len(), 1, "and must say so");
        assert!(w[0].contains("Could not read"), "unexpected warning: {}", w[0]);

        let err = write_atomic(&p, &Sample::default()).unwrap_err();
        assert!(err.contains("will not overwrite"), "unexpected error: {err}");
        assert!(err.contains("restart the Hub"), "unexpected error: {err}");
    }

    #[test]
    fn a_corrupt_file_is_preserved_not_destroyed() {
        // THE data-loss regression: the previous chain defaulted silently and
        // the next save overwrote the only copy of the user's list.
        let dir = scratch("corrupt");
        let p = dir.join("s.json");
        std::fs::write(&p, "{ this is not json").unwrap();

        let mut w = Vec::new();
        let v: Sample = read_or_default(&p, &mut w);
        assert_eq!(v, Sample::default(), "must still start up");
        assert_eq!(w.len(), 1, "and must say so");

        let kept = corrupt_files(&dir);
        assert_eq!(kept.len(), 1, "the original bytes must survive");
        assert_eq!(std::fs::read_to_string(&kept[0]).unwrap(), "{ this is not json");
        assert!(!p.exists(), "the bad file is moved, not copied");
    }

    #[test]
    fn a_second_corruption_does_not_overwrite_the_first_quarantine() {
        // The fixed `<name>.corrupt` name lost the FIRST evidence the moment
        // a second corruption was set aside -- usually inside the same second.
        let dir = scratch("corrupt-twice");
        let p = dir.join("s.json");
        let mut w = Vec::new();
        std::fs::write(&p, "first garbage").unwrap();
        let _: Sample = read_or_default(&p, &mut w);
        std::fs::write(&p, "second garbage").unwrap();
        let _: Sample = read_or_default(&p, &mut w);

        let kept: Vec<String> = corrupt_files(&dir)
            .iter()
            .map(|f| std::fs::read_to_string(f).unwrap())
            .collect();
        assert_eq!(kept.len(), 2, "both quarantines survive: {kept:?}");
        assert!(kept.contains(&"first garbage".to_string()));
        assert!(kept.contains(&"second garbage".to_string()));
    }

    #[test]
    fn a_corrupt_file_recovers_on_the_next_write() {
        let dir = scratch("recover");
        let p = dir.join("s.json");
        std::fs::write(&p, "garbage").unwrap();

        let mut w = Vec::new();
        let _: Sample = read_or_default(&p, &mut w);
        write_atomic(&p, &Sample { items: vec!["fresh".into()] }).unwrap();

        let mut w2 = Vec::new();
        assert_eq!(read_or_default::<Sample>(&p, &mut w2).items, vec!["fresh".to_string()]);
        assert!(w2.is_empty(), "the replacement must be clean");
    }

    #[test]
    fn quarantine_names_sit_beside_the_original_with_a_timestamp() {
        let p = Path::new("C:/x/recents.archub");
        assert_eq!(
            quarantine_name(p, 1_700_000_000, 0),
            PathBuf::from("C:/x/recents.archub.1700000000.corrupt")
        );
        assert_eq!(
            quarantine_name(p, 1_700_000_000, 2),
            PathBuf::from("C:/x/recents.archub.1700000000-2.corrupt")
        );
    }

    #[test]
    fn write_bytes_atomic_writes_exactly_the_bytes() {
        let dir = scratch("bytes");
        let p = dir.join("G.arcproj");
        std::fs::write(&p, "old").unwrap();
        write_bytes_atomic(&p, b"{\"name\":\"G\"}").unwrap();
        assert_eq!(std::fs::read_to_string(&p).unwrap(), "{\"name\":\"G\"}");
    }
}
