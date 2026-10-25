// .arcproj file-association self-heal, run at every startup (the
// UnrealVersionSelector model: the launcher you actually run owns the
// association). Exists because of a live 2026-07-29 failure: the NSIS
// installer wrote the open command UNQUOTED, and Windows' prefix-parsing of
// an unquoted spaced path stopped at "...\AppData\Local\Arcane" -- the Hub's
// own DATA DIRECTORY -- and tried to execute it: "This app can't run on your
// PC". Rewriting the (quoted) truth on every launch means no installer,
// upgrade, or hand edit can leave the association broken for longer than one
// Hub start.
//
// HKCU only -- per-user, no elevation, same hive the per-user installer
// writes. Best-effort: a launcher must start even when the registry says no.

use std::path::Path;

use winreg::enums::HKEY_CURRENT_USER;
use winreg::RegKey;

/// Also what the installer registers; keeping one ProgId means the two
/// writers converge on the same keys instead of stacking rivals.
pub const PROG_ID: &str = "Arcane Project";

/// The shell-open command for `exe` -- QUOTED, which is the entire lesson.
/// Pure so the quoting contract is pinned by a test.
pub fn open_command(exe: &Path) -> String {
    format!("\"{}\" \"%1\"", exe.display())
}

/// The DefaultIcon value: the exe's own first icon group. Quoted for the
/// same reason as the command.
pub fn icon_value(exe: &Path) -> String {
    format!("\"{}\",0", exe.display())
}

/// Claim (or repair) the association for THIS exe. Silent on failure.
///
/// RELEASE BUILDS ONLY. A `tauri dev` (debug) build ran this too, at every
/// start, and pointed .arcproj double-clicks at the debug exe under the
/// cargo target dir -- an exe whose webview loads the dev server URL
/// (devUrl), which is dead the moment `tauri dev` stops. So one dev session
/// left every .arcproj in Explorer opening a blank, broken Hub until the
/// installed Hub was started again to claim it back. The installed (release)
/// Hub is the only exe that should own the association.
pub fn claim_arcproj_association() {
    if cfg!(debug_assertions) {
        return;
    }
    let Ok(exe) = std::env::current_exe() else { return };
    if write_keys(&exe).is_ok() {
        notify_shell();
    }
}

fn write_keys(exe: &Path) -> std::io::Result<()> {
    let classes = RegKey::predef(HKEY_CURRENT_USER).create_subkey("Software\\Classes")?.0;
    let ext = classes.create_subkey(".arcproj")?.0;
    ext.set_value("", &PROG_ID)?;
    let prog = classes.create_subkey(PROG_ID)?.0;
    prog.set_value("", &"Arcane Engine project")?;
    prog.create_subkey("DefaultIcon")?.0.set_value("", &icon_value(exe))?;
    prog.create_subkey("shell\\open\\command")?.0.set_value("", &open_command(exe))?;
    Ok(())
}

/// SHChangeNotify(SHCNE_ASSOCCHANGED): tell Explorer the association moved,
/// so icons and double-click behaviour refresh without a logoff.
fn notify_shell() {
    use windows_sys::Win32::UI::Shell::{SHChangeNotify, SHCNE_ASSOCCHANGED, SHCNF_IDLIST};
    unsafe {
        // The event id constant is declared u32 but the parameter is i32 in
        // windows-sys 0.60; the value (0x08000000) fits either way.
        SHChangeNotify(SHCNE_ASSOCCHANGED as i32, SHCNF_IDLIST, std::ptr::null(), std::ptr::null());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn the_open_command_quotes_the_exe_and_the_argument() {
        // THE regression this module exists for: an unquoted spaced path made
        // the shell execute a directory prefix. Both halves stay quoted.
        let exe = PathBuf::from(r"C:\Users\Ethan Temprovich\AppData\Local\Arcane Hub\arcane_hub.exe");
        assert_eq!(
            open_command(&exe),
            r#""C:\Users\Ethan Temprovich\AppData\Local\Arcane Hub\arcane_hub.exe" "%1""#
        );
    }

    #[test]
    fn the_icon_value_is_quoted_with_the_first_icon_index() {
        let exe = PathBuf::from(r"C:\Program Files\A B\hub.exe");
        assert_eq!(icon_value(&exe), r#""C:\Program Files\A B\hub.exe",0"#);
    }
}
