// Typed wrappers over the Rust commands. Keeping the invoke strings in one
// place means a renamed command breaks here, not in a component.
import { invoke } from "@tauri-apps/api/core";
import type { ProjectView, SortKey } from "$lib/format";

export type RecentProject = {
  path: string;
  name: string;
  lastOpenedUtc: string;
  engineAbi: number;
  /** Pinned engine id, or null to follow the Hub default. See resolveEngine. */
  engineId: string | null;
  /** Extra launch arguments, as typed. Empty = none. Split Rust-side at launch. */
  args: string;
  /** Starred: sortProjects pins favourites above the rest, whatever the sort. */
  favorite: boolean;
  /**
   * The recorded path no longer resolves on disk. Stamped Rust-side on every
   * load, so it is a fact about right now, not about when the file was saved.
   * The row renders greyed with Locate/Remove instead of vanishing.
   */
  missing: boolean;
  /**
   * The manifest's project identity, recorded whenever Rust reads the
   * manifest; null until then. Rust-side only (moved-project healing) --
   * carried here because the two sides share one serialized shape.
   */
  guid: string | null;
};

export type EngineEntry = {
  id: string;
  path: string;
  engineAbi: number;
  build: string;
  /**
   * The exe no longer resolves on disk. Stamped Rust-side on every load, like
   * RecentProject.missing. The row stays (a rebuild restores it in place);
   * the pre-launch probe refuses it with a reason if launched anyway.
   */
  missing: boolean;
};

export type HubState = {
  recents: RecentProject[];
  engines: EngineEntry[];
  /**
   * Problems found while loading state -- currently, a state file that existed
   * but could not be parsed and has been set aside. Recovering from that used
   * to be silent, so the user just found an empty project list.
   */
  warnings: string[];
  /**
   * Monotonic per Hub process, bumped on every change; 0 = unknown (an older
   * backend). Snapshots arrive from several places and IPC replies can land
   * out of order, so the page ignores one older than what it already shows
   * (format.isNewerState).
   */
  revision: number;
};

/** Mirrors `settings::Settings`. Every field is read somewhere. */
export type Settings = {
  /** Starting directory for the New Project and Open dialogs; "" = OS default. */
  defaultProjectDir: string;
  /**
   * What the Hub window does after a successful launch. Read RUST-side
   * (open_project parks/hides/exits, the wait thread restores); the
   * frontend only edits it. `tray` (default) parks in the system tray while
   * editors run and comes back when the last exits; `close` really EXITS
   * the Hub once the editor survives its boot watchdog (editors always
   * outlive the Hub); `stay` keeps the window open throughout. Rust
   * normalises through `clean_behavior`. Replaced hideWhileRunning
   * 2026-07-29; `hide` retired for `close` the same day.
   */
  launchBehavior: "tray" | "close" | "stay";
  /**
   * Project list layout. Rust normalises this through `clean_view` on both
   * load and save, so it is always one of the two -- never a stray string.
   */
  projectView: ProjectView;
  /**
   * Sort column for the project list, normalised Rust-side like projectView.
   * Favourites sit above the rest whatever this says (sortProjects).
   */
  projectSort: SortKey;
  /** Direction, kept separate so flipping never loses the column. */
  projectSortDesc: boolean;
  /** Show the confirmation dialog before deleting. Read in the delete handler. */
  confirmDelete: boolean;
  /**
   * The Hub default engine (EngineEntry.id), picked in Engines. null, or an id
   * that is no longer registered, falls back to the first engine
   * (format.defaultEngineFor). Persisted so the choice survives a restart --
   * it used to live only in page state and reset to engines[0] every launch.
   */
  defaultEngineId: string | null;
};

export const loadState = () => invoke<HubState>("load_state");
export const registerEngine = (path: string) => invoke<EngineEntry>("register_engine", { path });
/**
 * Re-probe every registered engine and return state with refreshed abi/build.
 * Once per launch, after the first paint: registration caches the probe, and a
 * dev-loop engine rebuilt in place makes that cache lie about compatibility.
 */
export const refreshEngines = () => invoke<HubState>("refresh_engines");
export const forgetEngine = (path: string) => invoke<void>("forget_engine", { path });
/**
 * Delete the project's folder to the RECYCLE BIN, then drop it from the list.
 * A project whose folder is already gone is simply unlisted.
 */
export const deleteProject = (path: string) => invoke<void>("delete_project", { path });
/** Remove one project from the list. Hub state ONLY -- nothing touches disk. */
export const forgetProject = (path: string) => invoke<void>("forget_project", { path });
/**
 * Copy the project on disk as "<Name> Copy" beside it (skipping build output
 * and .git), rename its manifest, and list it. Returns the new .arcproj path.
 */
export const duplicateProject = (path: string) =>
  invoke<string>("duplicate_project", { path });
/** Hub state ONLY -- unlike deleteProject, nothing is removed from disk. */
export const clearRecents = () => invoke<void>("clear_recents");
/** Pin a project to an engine, or pass null to send it back to the default. */
export const setProjectEngine = (path: string, engineId: string | null) =>
  invoke<void>("set_project_engine", { path, engineId });
/** Star or unstar a project. Favourites render above the rest of the list. */
export const setProjectFavorite = (path: string, favorite: boolean) =>
  invoke<void>("set_project_favorite", { path, favorite });
export const suggestEngine = () => invoke<EngineEntry | null>("suggest_engine");
/** Extra arguments appended after `--project <path>` for this project only. */
export const setProjectArgs = (path: string, args: string) =>
  invoke<void>("set_project_args", { path, args });
/** Open the project's folder in Explorer, with its .arcproj selected. */
export const revealProject = (path: string) => invoke<void>("reveal_project", { path });
/**
 * Rename the folder, the `.arcproj`, the name inside it, and the Hub entry.
 * Returns the new manifest path.
 */
export const renameProject = (path: string, newName: string) =>
  invoke<string>("rename_project", { path, newName });
/**
 * Repoint a moved project at the `.arcproj` the user located. The engine pin
 * and launch arguments survive; name and ABI refresh from the new manifest.
 */
export const relocateProject = (path: string, newPath: string) =>
  invoke<void>("relocate_project", { path, newPath });

export const loadSettings = () => invoke<Settings>("load_settings");
export const saveSettings = (settings: Settings) => invoke<void>("save_settings", { settings });
/** The configured start folder, or null when unset or no longer on disk. */
export const defaultDialogDir = () => invoke<string | null>("default_dialog_dir");
export const hubDataDir = () => invoke<string>("hub_data_dir");
export const revealHubDataDir = () => invoke<void>("reveal_hub_data_dir");
export const hubVersion = () => invoke<string>("hub_version");

/**
 * What open_project actually did. `projectMissing`/`engineMissing` are
 * outcomes, not errors: the caller refreshes and lets the row's missing
 * treatment explain, instead of raising the error banner. Probe failures and
 * spawn failures still reject.
 */
export type OpenOutcome = {
  kind: "launched" | "focused" | "projectMissing" | "engineMissing";
};

/**
 * `projectPath` is a `.arcproj` file or a project folder; the engine takes both.
 * Rejects when `enginePath` is not a registered engine -- the UI only passes
 * registered ones, so that surfaces as an ordinary error.
 */
export const openProject = (projectPath: string, enginePath: string) =>
  invoke<OpenOutcome>("open_project", { projectPath, enginePath });

/**
 * Keys of projects with a live editor, shaped like Rust's project_dir_key --
 * match them with format.projectKey(p.path). For the initial paint;
 * transitions arrive as `running-changed` events.
 */
export const runningProjects = () => invoke<string[]>("running_projects");

/** What a Scan did. Every refusal is counted -- no silent drops. */
export type ScanReport = {
  added: number;
  alreadyListed: number;
  /**
   * Missing entries healed onto a found project by manifest guid: the moved
   * project's row un-greyed in place, star and pin intact, instead of a
   * stranger appearing beside it.
   */
  relocated: number;
  /** Folders holding MORE than one .arcproj; the engine refuses those. */
  ambiguous: number;
  /** The visit budget ran out before the tree did. */
  truncated: boolean;
};

/** Walk a folder tree and list every project under it (Godot's Scan). */
export const scanForProjects = (dir: string) =>
  invoke<ScanReport>("scan_for_projects", { dir });

/**
 * Cover thumbnails as data URLs, keyed by the exact paths handed in; a
 * project with no cover is simply absent (the card falls back to its
 * monogram). Unreal's chain: `<Name>.png` beside the manifest, else the
 * editor-written `Saved/AutoScreenshot.png`.
 */
export const projectCovers = (paths: string[]) =>
  invoke<Record<string, string>>("project_covers", { paths });

/**
 * Writes the project and LISTS it, so a launch that fails afterwards cannot
 * leave a created-but-unlisted project. Returns the `.arcproj` it wrote,
 * ready to hand to openProject.
 */
export const createProject = (dir: string, name: string, enginePath: string) =>
  invoke<string>("create_project", { dir, name, enginePath });

/**
 * List an existing project WITHOUT launching it -- "Add project…". Returns
 * the stored entry. Needs no engine: adding is about the list, not the editor.
 */
export const addProject = (path: string) => invoke<RecentProject>("add_project", { path });

/**
 * Restore and focus the Hub window. A tray quick-launch runs while the window
 * is parked in the tray, so a failure has to bring it back before it says
 * anything -- a message in a hidden window is a message nobody reads.
 */
export const showHub = () => invoke<void>("show_hub");
