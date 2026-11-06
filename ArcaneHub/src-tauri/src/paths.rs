// Where the Hub keeps its per-user state.
//
// This is HUB-OWNED: the engine never reads it. Keeping user-scope state out of
// the engine preserves the rule that Core and the runtime carry no host/user
// vocabulary.
//
// Deliberately NOT derived from current_exe(). The Hub is INSTALLED per-user
// to %LOCALAPPDATA%\Arcane Hub\ (the NSIS currentUser default for the product
// name), so there is no repo root to find and no sibling engine to assume --
// that is Tools/setup-wizard's assumption, and it is wrong here. Note the
// install dir and this data dir (%LOCALAPPDATA%\Arcane\hub) are siblings
// under %LOCALAPPDATA% -- the unquoted-association incident in assoc.rs is
// what that adjacency once cost.

use std::path::PathBuf;

pub fn hub_dir() -> PathBuf {
    // %LOCALAPPDATA% (machine-local), NOT roaming %APPDATA% -- decided
    // 2026-07-28, reversing the original choice. Everything in these files is
    // a machine-specific absolute path (D:\ projects, engine exes), so a
    // roaming profile would deliver another machine a list where every row is
    // missing and every engine is gone. Settings ride along rather than
    // splitting the store in two. Falls back to temp only if the variable is
    // missing, which should not happen on Windows.
    let base = std::env::var("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(|_| std::env::temp_dir());
    base.join("Arcane").join("hub")
}

/// The Hub's state-file extension. JSON inside (store.rs's versioned
/// envelope), the Arcane family name outside -- .arcproj/.arcmat/.arcsprite/
/// .archub: these are the Hub's OWN generated files, not hand-edited config,
/// and the family extension says whose they are at a glance.
pub const STATE_EXT: &str = "archub";

// No migration machinery lives here, deliberately: the state location and
// extension both changed on 2026-07-28, but this app had touched exactly ONE
// machine at that point, and its files were migrated by hand that day. Code
// that exists to read shapes no disk holds is a liability, not a kindness.
pub fn recents_file() -> PathBuf {
    hub_dir().join(format!("recents.{STATE_EXT}"))
}

pub fn engines_file() -> PathBuf {
    hub_dir().join(format!("engines.{STATE_EXT}"))
}

pub fn settings_file() -> PathBuf {
    hub_dir().join(format!("settings.{STATE_EXT}"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn state_files_live_under_the_hub_dir() {
        let dir = hub_dir();
        assert!(recents_file().starts_with(&dir));
        assert!(engines_file().starts_with(&dir));
        assert!(settings_file().starts_with(&dir));
    }

    #[test]
    fn every_state_file_is_distinct() {
        // Two of these sharing a name would have one silently overwrite the
        // other on save, and the loss would only show up on next launch.
        let all = [recents_file(), engines_file(), settings_file()];
        for (i, a) in all.iter().enumerate() {
            for b in all.iter().skip(i + 1) {
                assert_ne!(a, b);
            }
        }
    }

    #[test]
    fn hub_dir_is_namespaced_under_arcane() {
        let p = hub_dir();
        let s = p.to_string_lossy().replace('\\', "/");
        assert!(s.ends_with("Arcane/hub"), "unexpected hub dir: {s}");
    }

    #[test]
    fn every_state_file_wears_the_family_extension() {
        for f in [recents_file(), engines_file(), settings_file()] {
            assert!(
                f.extension().is_some_and(|e| e == STATE_EXT),
                "not .{STATE_EXT}: {}",
                f.display()
            );
        }
    }

}
