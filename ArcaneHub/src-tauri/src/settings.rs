// Hub settings: user preferences the Hub actually acts on.
//
// Deliberately NOT a kitchen sink. Every field here changes observable
// behaviour at a named call site, listed beside it. A preference nothing reads
// is worse than no preference at all -- it tells the user they configured
// something when they did not.
//
// Same durability contract as state.rs: any read failure falls back to
// Default. A hand-edited or truncated settings file must never brick the Hub.
// And the same refusal (store.rs): a settings file this session could not
// read, or one a newer Hub wrote, is never overwritten with those defaults.

use serde::{Deserialize, Serialize};

use crate::{paths, store};

pub const VIEW_GRID: &str = "grid";
pub const VIEW_LIST: &str = "list";

pub const BEHAVIOR_TRAY: &str = "tray";
pub const BEHAVIOR_CLOSE: &str = "close";
pub const BEHAVIOR_STAY: &str = "stay";

pub const SORT_OPENED: &str = "opened";
pub const SORT_NAME: &str = "name";
pub const SORT_ENGINE: &str = "engine";
pub const SORT_ABI: &str = "abi";

/// Normalise the project-list sort column, defaulting anything unrecognised
/// to last-opened. Same String-with-normaliser reasoning as `clean_view`.
pub fn clean_sort(v: &str) -> String {
    let t = v.trim();
    for k in [SORT_NAME, SORT_ENGINE, SORT_ABI] {
        if t.eq_ignore_ascii_case(k) {
            return k.to_string();
        }
    }
    SORT_OPENED.to_string()
}

/// Normalise the after-launch behaviour, defaulting anything unrecognised to
/// the tray. Same String-with-normaliser reasoning as `clean_view`: a serde
/// enum fails the whole document on an unknown variant, and a settings file
/// that will not parse is treated as corrupt and reset. The retired `hide`
/// value (replaced by `close` 2026-07-29, when the tray took over its
/// come-back-later role) falls into the tray default like any unknown.
pub fn clean_behavior(v: &str) -> String {
    let t = v.trim();
    if t.eq_ignore_ascii_case(BEHAVIOR_CLOSE) {
        BEHAVIOR_CLOSE.to_string()
    } else if t.eq_ignore_ascii_case(BEHAVIOR_STAY) {
        BEHAVIOR_STAY.to_string()
    } else {
        BEHAVIOR_TRAY.to_string()
    }
}

/// Normalise the project-list layout, defaulting anything unrecognised to the
/// grid.
///
/// Deliberately a String field with a normaliser rather than a serde enum: an
/// unknown variant makes serde fail the WHOLE document, and since a settings
/// file that will not parse is treated as corrupt, one bad value here would
/// discard every other setting with it.
pub fn clean_view(v: &str) -> String {
    if v.trim().eq_ignore_ascii_case(VIEW_LIST) {
        VIEW_LIST.to_string()
    } else {
        VIEW_GRID.to_string()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "camelCase")]
pub struct Settings {
    /// Starting directory for the New Project and Open Project dialogs.
    /// Empty = let the OS pick (its own last-used location).
    #[serde(default)]
    pub default_project_dir: String,

    /// What the Hub window does after a successful launch. Three values:
    /// `tray` (default) -- hide into the system tray WHILE editors run:
    /// click brings it back early, right-click offers quick launch/quit,
    /// and when the last editor exits the Hub comes back on its own (user
    /// call 2026-07-29 -- the icon is for during the run, not after it).
    /// `close` -- actually EXIT the Hub once the editor is off the ground
    /// (it lingers hidden for the 2s boot watchdog first, so a refused
    /// launch still gets its reason shown instead of a silent exit).
    /// Editors always outlive the Hub; what closing costs is the live pid
    /// map, so a re-launch of the same project opens a second editor
    /// instead of focusing the first. `stay` -- keep the window open.
    /// Replaced hide_while_running 2026-07-29 (tray), then `hide` became
    /// `close` the same day (user call: tray owns come-back-later, so the
    /// third option should really close). No legacy reads, per the
    /// no-migration rule. Read Rust-side in open_project and the editor
    /// wait thread. Always normalised through `clean_behavior`.
    #[serde(default = "default_behavior")]
    pub launch_behavior: String,

    /// Project list layout: `grid` or `list`. Set from the toggle above the
    /// list rather than from the Settings tab -- it is a view control, and
    /// making the user leave the projects screen to change how the projects
    /// screen looks would be worse. Read in ProjectsView. Always normalised
    /// through `clean_view`, so consumers can trust the value.
    ///
    /// `default = "default_view"`, not a bare `#[serde(default)]`: the bare
    /// form uses `String::default()` ("") and would disagree with this struct's
    /// own Default, leaving two answers to "what does a fresh install show".
    #[serde(default = "default_view")]
    pub project_view: String,

    /// Which column orders the project list: `opened` (default) | `name` |
    /// `engine` | `abi`. Set by clicking a column header (list layout) or the
    /// sort control (grid); favourites always sit above the rest whatever
    /// this says -- the partition is the frontend's job (format.ts
    /// sortProjects). Always normalised through `clean_sort`.
    #[serde(default = "default_sort")]
    pub project_sort: String,

    /// Sort direction, persisted separately so flipping never loses the
    /// column. `default = "yes"` because the default column is `opened` and
    /// newest-first is the only order a launcher's recents make sense in.
    #[serde(default = "yes")]
    pub project_sort_desc: bool,

    /// Ask before deleting a project. Read in `+page.svelte`'s delete handler,
    /// which skips straight to `delete_project` when this is off.
    ///
    /// ON by default, and `default = "yes"` rather than a bare
    /// `#[serde(default)]` for a reason that matters more here than anywhere
    /// else in this struct: the bare form uses `bool::default()`, which is
    /// FALSE. Every settings file written before this field existed would then
    /// load with confirmation silently disabled -- turning a menu item into a
    /// one-click delete for exactly the users who never asked for it.
    #[serde(default = "yes")]
    pub confirm_delete: bool,

    /// The engine (an `EngineEntry::id`) the Hub launches with when a
    /// project has no pin of its own. None, or an id no longer registered,
    /// means the first registered engine. Persisted so the sidebar's choice
    /// survives a restart AND so the shell route (a double-clicked .arcproj,
    /// launch::shell_engine) uses the same default as a card click -- before
    /// it, the default was session state and the shell route could only
    /// guess "the first engine". Read by the frontend's engine resolution
    /// and by shell_engine. Normalised by `clean_engine_id` (trimmed, empty
    /// = None); NOT validated against the registry here, since forgetting
    /// an engine must not need to rewrite settings -- a dangling id falls
    /// back at the point of use.
    #[serde(default)]
    pub default_engine_id: Option<String>,
}

fn default_view() -> String {
    VIEW_GRID.to_string()
}

fn default_behavior() -> String {
    BEHAVIOR_TRAY.to_string()
}

fn default_sort() -> String {
    SORT_OPENED.to_string()
}

fn yes() -> bool {
    true
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            default_project_dir: String::new(),
            launch_behavior: default_behavior(),
            project_view: default_view(),
            project_sort: default_sort(),
            project_sort_desc: yes(),
            confirm_delete: yes(),
            default_engine_id: None,
        }
    }
}

/// Trim a default-engine id; an empty one means "no default" (None).
pub fn clean_engine_id(id: Option<&str>) -> Option<String> {
    id.map(str::trim).filter(|t| !t.is_empty()).map(str::to_string)
}

/// Trim a user-typed directory into the form the dialog layer wants.
///
/// Pure so it can be tested without a filesystem. Existence is checked
/// separately at the command boundary: a configured folder that has since been
/// deleted must degrade to "let the OS choose", not to an error dialog.
pub fn clean_dir(dir: &str) -> String {
    let t = dir.trim().trim_matches('"');
    // Keep a bare root ("C:\", "/") intact -- trimming its separator would turn
    // it into "C:", which Windows reads as the drive's *current* directory.
    let trimmed = t.trim_end_matches(['/', '\\']);
    if trimmed.is_empty() || trimmed.ends_with(':') {
        t.to_string()
    } else {
        trimmed.to_string()
    }
}

/// Settings discard their warnings: unlike the project list, a reset settings
/// file loses only preferences the user can see and re-set on the same screen.
pub fn load() -> Settings {
    let mut ignored = Vec::new();
    let s: Settings = store::read_or_default(&paths::settings_file(), &mut ignored);
    // Normalise on the way OUT as well as in: a file written before these
    // fields existed carries "" for them, and a hand-edited one may carry
    // anything. Consumers must never have to defend against a bad string.
    Settings {
        project_view: clean_view(&s.project_view),
        launch_behavior: clean_behavior(&s.launch_behavior),
        project_sort: clean_sort(&s.project_sort),
        ..s
    }
}

pub fn save(s: &Settings) -> Result<(), String> {
    store::write_atomic(&paths::settings_file(), s)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_are_the_designed_behaviour() {
        let s = Settings::default();
        assert_eq!(s.default_project_dir, "");
        assert_eq!(s.launch_behavior, BEHAVIOR_TRAY, "the tray is the designed default");
        assert_eq!(s.project_view, VIEW_GRID, "the grid is what shipped first");
        assert_eq!(s.project_sort, SORT_OPENED, "recents order by recency until asked otherwise");
        assert!(s.project_sort_desc, "newest first is the only sensible recents default");
        assert!(s.confirm_delete, "deleting has always asked first");
    }

    #[test]
    fn clean_sort_accepts_the_four_columns_and_defaults_the_rest() {
        assert_eq!(clean_sort(SORT_OPENED), SORT_OPENED);
        assert_eq!(clean_sort(SORT_NAME), SORT_NAME);
        assert_eq!(clean_sort(SORT_ENGINE), SORT_ENGINE);
        assert_eq!(clean_sort(SORT_ABI), SORT_ABI);
        assert_eq!(clean_sort("  Name "), SORT_NAME, "trimmed and case-folded");
        assert_eq!(clean_sort("stars"), SORT_OPENED, "unknown falls back");
        assert_eq!(clean_sort(""), SORT_OPENED, "so does a pre-field file");
    }

    #[test]
    fn a_file_written_before_confirm_delete_existed_still_confirms() {
        // THE regression this field's `default = "yes"` exists to stop. With a
        // bare #[serde(default)] this loads as false, and every user who had
        // ever saved a setting would get a one-click project delete on the next
        // launch without touching anything.
        let back: Settings = serde_json::from_str(r#"{"launchBehavior":"stay"}"#).unwrap();
        assert!(back.confirm_delete);
    }

    #[test]
    fn a_file_written_before_launch_behavior_existed_gets_the_tray() {
        // Same defaulted-field reasoning as confirm_delete: an older file must
        // land on the designed behaviour, not on an accidental empty string.
        let back: Settings = serde_json::from_str(r#"{"confirmDelete":true}"#).unwrap();
        assert_eq!(back.launch_behavior, BEHAVIOR_TRAY);
    }

    #[test]
    fn clean_behavior_accepts_the_three_modes_and_defaults_the_rest() {
        assert_eq!(clean_behavior(BEHAVIOR_TRAY), BEHAVIOR_TRAY);
        assert_eq!(clean_behavior(BEHAVIOR_CLOSE), BEHAVIOR_CLOSE);
        assert_eq!(clean_behavior(BEHAVIOR_STAY), BEHAVIOR_STAY);
        assert_eq!(clean_behavior("  Close  "), BEHAVIOR_CLOSE, "trimmed and case-folded");
        assert_eq!(clean_behavior("hide"), BEHAVIOR_TRAY, "the retired mode falls back");
        assert_eq!(clean_behavior("closeAfterLaunch"), BEHAVIOR_TRAY, "unknown falls back");
        assert_eq!(clean_behavior(""), BEHAVIOR_TRAY, "so does a pre-field file");
    }

    #[test]
    fn confirm_delete_can_actually_be_turned_off() {
        // The other half: an explicit false must survive the round trip, or the
        // setting would be permanently stuck on.
        let back: Settings = serde_json::from_str(r#"{"confirmDelete":false}"#).unwrap();
        assert!(!back.confirm_delete);
    }

    #[test]
    fn clean_view_accepts_the_two_layouts_and_defaults_the_rest() {
        assert_eq!(clean_view(VIEW_LIST), VIEW_LIST);
        assert_eq!(clean_view(VIEW_GRID), VIEW_GRID);
        assert_eq!(clean_view("  List  "), VIEW_LIST, "trimmed and case-folded");
        assert_eq!(clean_view("carousel"), VIEW_GRID, "unknown falls back");
        assert_eq!(clean_view(""), VIEW_GRID, "so does a file written before the field");
    }

    #[test]
    fn an_unknown_behavior_does_not_discard_the_other_settings() {
        // THE reason these are Strings and not serde enums: an unknown variant
        // would fail the whole document, and a settings file that will not
        // parse is treated as corrupt and reset.
        let back: Settings =
            serde_json::from_str(r#"{"confirmDelete":false,"launchBehavior":"nonsense","projectView":"nonsense"}"#)
                .expect("must still parse");
        assert!(!back.confirm_delete, "the other settings survive");
        assert_eq!(clean_behavior(&back.launch_behavior), BEHAVIOR_TRAY);
        assert_eq!(clean_view(&back.project_view), VIEW_GRID);
    }

    #[test]
    fn settings_round_trip_through_json() {
        let s = Settings {
            default_project_dir: "D:/Games".to_string(),
            launch_behavior: BEHAVIOR_STAY.to_string(),
            project_view: VIEW_LIST.to_string(),
            project_sort: SORT_NAME.to_string(),
            project_sort_desc: false,
            confirm_delete: false,
            default_engine_id: Some("c:/eng/arcaneeditor.exe".to_string()),
        };
        let back: Settings = serde_json::from_str(&serde_json::to_string(&s).unwrap()).unwrap();
        assert_eq!(back, s);
    }

    #[test]
    fn serialises_with_camel_case_keys_for_the_frontend() {
        let text = serde_json::to_string(&Settings::default()).unwrap();
        assert!(text.contains("defaultProjectDir"), "got {text}");
        assert!(text.contains("launchBehavior"), "got {text}");
        assert!(text.contains("projectSort"), "got {text}");
        assert!(text.contains("projectSortDesc"), "got {text}");
        assert!(text.contains("confirmDelete"), "got {text}");
        assert!(text.contains("defaultEngineId"), "got {text}");
    }

    #[test]
    fn a_file_written_before_default_engine_id_existed_has_no_default() {
        let back: Settings = serde_json::from_str(r#"{"launchBehavior":"stay"}"#).unwrap();
        assert_eq!(back.default_engine_id, None, "None = the first registered engine");
    }

    #[test]
    fn clean_engine_id_trims_and_maps_empty_to_none() {
        assert_eq!(clean_engine_id(Some("  c:/e.exe ")), Some("c:/e.exe".to_string()));
        assert_eq!(clean_engine_id(Some("   ")), None);
        assert_eq!(clean_engine_id(Some("")), None);
        assert_eq!(clean_engine_id(None), None);
    }

    #[test]
    fn a_settings_file_missing_a_field_still_loads() {
        // Forward compat: an older file must not reset the fields it does have.
        let back: Settings = serde_json::from_str(r#"{"launchBehavior":"close"}"#).unwrap();
        assert_eq!(back.launch_behavior, BEHAVIOR_CLOSE);
        assert_eq!(back.default_project_dir, "");
    }

    #[test]
    fn an_unknown_field_is_ignored_rather_than_fatal() {
        // Downgrade compat: a newer Hub's file must still load in an older one.
        let back: Settings = serde_json::from_str(r#"{"somethingNew":1}"#).unwrap();
        assert_eq!(back, Settings::default());
    }

    #[test]
    fn corrupt_settings_text_does_not_parse_so_the_caller_falls_back() {
        assert!(serde_json::from_str::<Settings>("{ not json").is_err());
    }

    #[test]
    fn clean_dir_trims_whitespace_and_trailing_separators() {
        assert_eq!(clean_dir("  D:/Games/  "), "D:/Games");
        assert_eq!(clean_dir("D:\\Games\\"), "D:\\Games");
        assert_eq!(clean_dir("D:/Games"), "D:/Games");
    }

    #[test]
    fn clean_dir_strips_the_quotes_explorer_copy_as_path_adds() {
        // "Copy as path" in Explorer yields a quoted string; pasting it into
        // the field is the obvious way to fill it in.
        assert_eq!(clean_dir("\"D:\\Games\""), "D:\\Games");
    }

    #[test]
    fn clean_dir_keeps_a_bare_drive_root_usable() {
        // "D:\" -> "D:" would mean the drive's CURRENT directory on Windows,
        // which is a different folder than the user picked.
        assert_eq!(clean_dir("D:\\"), "D:\\");
        assert_eq!(clean_dir("/"), "/");
    }

    #[test]
    fn clean_dir_maps_blank_input_to_empty() {
        assert_eq!(clean_dir("   "), "");
        assert_eq!(clean_dir(""), "");
    }
}
