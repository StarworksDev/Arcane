<script lang="ts">
  import "$lib/theme.css";
  import { onMount } from "svelte";
  import { listen } from "@tauri-apps/api/event";
  import { open } from "@tauri-apps/plugin-dialog";
  import WindowChrome from "$lib/components/WindowChrome.svelte";
  import Sidebar, { type View } from "$lib/components/Sidebar.svelte";
  import Button from "$lib/components/Button.svelte";
  import Icon from "$lib/components/Icon.svelte";
  import type { ProjectActions } from "$lib/components/ProjectMenu.svelte";
  import type { EngineActions } from "$lib/views/EnginesView.svelte";
  import ProjectsView from "$lib/views/ProjectsView.svelte";
  import EnginesView from "$lib/views/EnginesView.svelte";
  import PackagesView from "$lib/views/PackagesView.svelte";
  import SettingsModal from "$lib/components/SettingsModal.svelte";
  import NewProjectModal from "$lib/components/NewProjectModal.svelte";
  import ProjectEngineModal from "$lib/components/ProjectEngineModal.svelte";
  import RenameProjectModal from "$lib/components/RenameProjectModal.svelte";
  import DeleteProjectModal from "$lib/components/DeleteProjectModal.svelte";
  import ProjectArgsModal from "$lib/components/ProjectArgsModal.svelte";
  import {
    nextSort, resolveEngine, projectKey, scanReportLines, defaultEngineFor, isNewerState,
    addNotice, replaceNotices, capNotices, type Notice, type NoticeSource,
  } from "$lib/format";
  import {
    loadState, refreshEngines, registerEngine, forgetEngine, deleteProject,
    duplicateProject, forgetProject, clearRecents, projectCovers, runningProjects,
    openProject, createProject, scanForProjects, suggestEngine, setProjectEngine,
    setProjectFavorite, setProjectArgs, revealProject, renameProject, relocateProject,
    loadSettings, saveSettings, defaultDialogDir, addProject, showHub,
    hubDataDir, revealHubDataDir, hubVersion,
    type HubState, type EngineEntry, type RecentProject, type Settings,
  } from "$lib/api";

  // NOT `state`: a variable of that name makes svelte2tsx read the `$state`
  // rune as a legacy store-subscription, and svelte-check reports 6 phantom
  // errors while the app still runs fine.
  let hub = $state<HubState>({ recents: [], engines: [], warnings: [], revision: 0 });
  // The newest HubState revision adopted so far (see isNewerState). Plain, not
  // $state: nothing renders it.
  let lastRevision = 0;
  let selectedEngine = $state<EngineEntry | null>(null);
  let suggestion = $state<EngineEntry | null>(null);
  let error = $state("");
  // The re-entry gate: set the instant an action starts, checked at guard()'s
  // door, so a double-click cannot run two actions. NOT what the controls see.
  let busy = $state(false);
  // When the running action started, so a dropped click can tell a
  // double-click's second half (silence) from a real attempt (say so).
  let busySince = 0;
  // What the controls see. Most actions are a few milliseconds of IPC, and
  // flipping every disabled={busy} style for two frames dimmed the whole page
  // to 50% opacity and back -- a full-page flicker on every update. The
  // disabled visuals only engage when an action is actually SLOW (>150ms,
  // the spinner-delay rule); fast ones change nothing on screen.
  let busyUi = $state(false);
  let view = $state<View>("projects");
  let settings = $state<Settings>({
    defaultProjectDir: "", launchBehavior: "tray", projectView: "grid",
    projectSort: "opened", projectSortDesc: true, confirmDelete: true,
    defaultEngineId: null,
  });
  // State AND settings have both arrived. Until then the views do not render:
  // the empty initial `hub` used to show "No projects yet" for a moment on
  // every launch, and a layout or sort click in that window saved the
  // placeholder settings above over the user's real ones.
  let ready = $state(false);
  let hubDir = $state("");
  let version = $state("");
  // Keys of projects with a live editor, shaped like Rust's project_dir_key --
  // matched with format.projectKey. Replaced wholesale on every
  // running-changed event: the payload is the full set, so a missed event
  // self-heals on the next transition.
  let running = $state<Set<string>>(new Set());
  // The Projects view's search text and scroll offset, kept here so they
  // survive a visit to Engines (the view unmounts; this file does not).
  let projectsQuery = $state("");
  let projectsScroll = $state(0);

  // Cover thumbnails by recorded path. Fetched INCREMENTALLY: every state
  // adoption used to re-read every cover (a PNG decode and base64 per
  // project) after every click. Now only paths never asked about are
  // requested; editors-idle invalidates the lot, because the editor that
  // just closed may have written a new auto-screenshot.
  let covers = $state<Record<string, string>>({});
  // Path -> sequence number of the latest request that included it. A reply
  // is applied only for the paths it is still the latest request for, so a
  // slow, older reply can never overwrite a newer one. Plain, not $state.
  const coverAsked = new Map<string, number>();
  let coverSeq = 0;
  function refreshCovers(all = false) {
    const listed = new Set(hub.recents.map((r) => r.path));
    if (all) coverAsked.clear();
    // Forget unlisted paths, so one that comes back (an undone rename, a
    // re-added project) is fetched again rather than assumed known.
    for (const p of [...coverAsked.keys()]) if (!listed.has(p)) coverAsked.delete(p);
    if (Object.keys(covers).some((p) => !listed.has(p))) {
      covers = Object.fromEntries(Object.entries(covers).filter(([p]) => listed.has(p)));
    }
    const want = [...listed].filter((p) => !coverAsked.has(p));
    if (want.length === 0) return;
    const seq = ++coverSeq;
    for (const p of want) coverAsked.set(p, seq);
    // Fire-and-forget: the grid renders monograms until the batch lands, and
    // what is on screen stays up during a refetch -- no flash to monograms.
    projectCovers(want).then(
      (got) => {
        const next = { ...covers };
        for (const p of want) {
          if (coverAsked.get(p) !== seq) continue;
          if (p in got) next[p] = got[p];
          else delete next[p];
        }
        covers = next;
      },
      // A failed batch is retried on the next adoption, not remembered as
      // "no cover".
      () => { for (const p of want) if (coverAsked.get(p) === seq) coverAsked.delete(p); },
    );
  }

  // ONE modal at a time, as a discriminated union rather than six independent
  // flags. Owned here so a dialog renders above the error banner and a failed
  // action keeps its dialog up with the message inside it, not behind the
  // scrim. The union replaced the per-modal booleans/nullables: their modalUp
  // derived had to enumerate every one, and its own comment admitted "the
  // list has grown once already and a missed entry shows up as a message the
  // user can see but cannot read" -- with one value, a forgotten entry and
  // two-modals-at-once are unrepresentable rather than guarded against.
  type Modal =
    | { kind: "none" }
    | { kind: "new" }
    | { kind: "settings" }
    | { kind: "engine"; p: RecentProject }
    | { kind: "rename"; p: RecentProject }
    | { kind: "args"; p: RecentProject }
    | { kind: "delete"; p: RecentProject };
  let modal = $state<Modal>({ kind: "none" });
  const modalUp = $derived(modal.kind !== "none");
  // Both clear `error`: a dialog renders the error itself, and must show only
  // what went wrong WHILE it was open -- not a stale failure from the main
  // view (or the previous dialog) under an unrelated title.
  function openModal(m: Exclude<Modal, { kind: "none" }>) {
    error = "";
    modal = m;
  }
  function closeModal() {
    error = "";
    modal = { kind: "none" };
  }
  // Close only if `m` is still the dialog up: an action awaited past a
  // dialog's own lifetime must not close whatever the user opened since.
  function closeIf(m: Modal) {
    if (modal === m) closeModal();
  }

  // Facts the Hub wants seen that are not a failed action: recovery on load,
  // a scan report, an editor that died at boot. ACCUMULATED rather than read
  // straight off `hub` -- recovery renames the bad file aside, so the next
  // load reports nothing, and rendering hub.warnings directly made the notice
  // vanish on the user's next click. Each can be dismissed, a new scan report
  // REPLACES the last one (format.replaceNotices), and only the newest few
  // show (format.capNotices) so they cannot squeeze the project list away.
  let notices = $state<Notice[]>([]);
  let noticeSeq = 0;
  let noticesOpen = $state(false);
  // Three on a normal window, two on a short one: at the 520px minimum, three
  // banners left the list about two rows.
  let winH = $state(900);
  const noticeCap = $derived(winH < 640 ? 2 : 3);
  const shownNotices = $derived(capNotices(notices, noticeCap, noticesOpen));
  // Deduplicated by text: the same fact arriving twice (a watchdog re-fire, a
  // repeated click) must not stack identical banners.
  const pushNotice = (text: string, source: NoticeSource) => {
    notices = addNotice(notices, { id: ++noticeSeq, source, text });
  };
  const dismissNotice = (id: number) => {
    notices = notices.filter((n) => n.id !== id);
  };

  // A dropped action says so, briefly, without blocking anything (a toast,
  // not a banner: it is about a click, not about the projects).
  let busyFlash = $state(false);
  let busyFlashTimer: ReturnType<typeof setTimeout> | undefined;
  function flashBusy() {
    busyFlash = true;
    clearTimeout(busyFlashTimer);
    busyFlashTimer = setTimeout(() => (busyFlash = false), 2500);
  }

  // The default engine follows the saved setting, re-resolved FROM the current
  // list rather than keeping the old object: entries are fresh copies, and a
  // re-probe or re-register may have changed abi/build -- holding a stale copy
  // would keep last week's facts alive in the sidebar and every compatibility
  // row derived from it. `?? null` because an older backend omits the field.
  function resolveSelection() {
    selectedEngine = defaultEngineFor(hub.engines, settings.defaultEngineId ?? null);
  }

  // One adoption path for every HubState that arrives, whatever produced it.
  function adoptState(next: HubState) {
    // Snapshots come from action refreshes, the disk watcher and the engine
    // re-probe, and IPC replies can land out of order: never let an older one
    // overwrite a newer one. Revision 0 (unknown) is always adopted.
    if (!isNewerState(next.revision, lastRevision)) return;
    lastRevision = Math.max(lastRevision, next.revision ?? 0);
    hub = next;
    // `?? []` because this is an IPC boundary: the TS type is a promise about
    // what Rust sends, not something the compiler checks. It was briefly wrong
    // (a serde skip_serializing dropped the field), and a bare `for...of` over
    // undefined threw inside refresh -- taking the whole reload down with it,
    // including the engine selection, on every action that calls guard().
    for (const w of next.warnings ?? []) pushNotice(w, "load");
    resolveSelection();
    refreshCovers();
  }

  async function refresh() {
    adoptState(await loadState());
    // Adjacency is a suggestion for the dev loop, never an assumption.
    suggestion = hub.engines.length === 0 ? await suggestEngine() : null;
  }

  // Startup, as a promise the tray handler can wait on (a tray pick that
  // arrives before the list has loaded must not be told the project is gone).
  let startup: Promise<void> = Promise.resolve();
  async function start() {
    error = "";
    try {
      // Together, because the view needs both: state says what to list,
      // settings say how (layout, sort) and which engine is the default.
      const [st, s] = await Promise.all([loadState(), loadSettings()]);
      settings = s;
      adoptState(st);
      // Nice-to-haves: a failure in either must not keep the list from showing.
      const [sug, run] = await Promise.all([
        hub.engines.length === 0 ? suggestEngine().catch(() => null) : Promise.resolve(null),
        runningProjects().catch(() => [] as string[]),
      ]);
      suggestion = sug;
      running = new Set(run);
      // Land on Engines when there is nothing to launch with -- the one thing
      // the user must do first.
      if (hub.engines.length === 0) view = "engines";
      ready = true;
    } catch (e) {
      error = String(e);
      return;
    }
    hubDataDir().then((d) => (hubDir = d), () => {});
    hubVersion().then((v) => (version = v), () => {});
    // AFTER the first paint, not before it: each probe is a process spawn, and
    // the cached data is right far more often than not -- but a dev-loop
    // engine rebuilt in place goes stale silently, and the compat display then
    // inverts (a project stamped by the CURRENT engine reads incompatible
    // against last week's cached abi). Re-probe and adopt whatever changed.
    try { adoptState(await refreshEngines()); }
    catch (e) { error = String(e); }
  }

  onMount(() => {
    startup = start();
  });

  // Restore the Hub window before saying anything, for actions that started
  // while it sat in the tray. A failure to restore is not worth a second
  // message about the first.
  const surfaceHub = () => showHub().catch(() => {});

  // A SECOND onMount for the listeners, because they need a cleanup. The
  // launch watchdog reports an editor that died at boot (the refuse-gate exits
  // before any window, so a detached spawn is otherwise click-then-nothing);
  // it arrives as an event because the launch command returns before the
  // verdict exists. Rendered as a notice, not `error`: it is an async fact
  // about a process, not a failed Hub action.
  onMount(() => {
    const unFailed = listen<string>("launch-failed", (e) => pushNotice(e.payload, "launch"));
    // The last tracked editor exited (the Hub may just have restored itself):
    // opened-times and missing flags deserve fresh eyes, and so do covers --
    // the editor writes its auto-screenshot on the way out.
    const unIdle = listen("editors-idle", () => {
      refresh().then(() => refreshCovers(true), (e) => (error = String(e)));
    });
    // The Rust disk watcher noticed a listed path appear or vanish while the
    // Hub sat open. Adopt in place -- the rows are keyed by path, so the only
    // visible change is the row that actually changed.
    const unDisk = listen<HubState>("state-changed", (e) => adoptState(e.payload));
    // A quick-launch pick from the tray menu. Routed through launch() so the
    // tray goes through the SAME path a card click takes -- engine
    // resolution, probe, focus-existing, outcomes -- rather than a second
    // launch brain living Rust-side. Matched by projectKey so a folder- or
    // manifest-shaped payload finds its row either way.
    const unTray = listen<string>("tray-launch", async (e) => {
      await startup;
      const key = projectKey(e.payload);
      const p = hub.recents.find((r) => projectKey(r.path) === key);
      if (p) return launch(p, true);
      await surfaceHub();
      pushNotice("That project is no longer in the Hub's list.", "launch");
    });
    // Which projects have a live editor -- drives the running badge, the
    // launch tooltip's "focuses its window" phrasing and the menu's
    // close-the-editor-first items.
    const unRunning = listen<string[]>("running-changed", (e) => {
      running = new Set(e.payload);
    });
    return () => {
      unFailed.then((f) => f());
      unIdle.then((f) => f());
      unDisk.then((f) => f());
      unTray.then((f) => f());
      unRunning.then((f) => f());
      clearTimeout(busyFlashTimer);
    };
  });

  /**
   * What became of an action: it ran (and the state refreshed), it failed
   * (`error` holds why), or it was dropped because another was running.
   * Callers that close a dialog or confirm a control close ONLY on "ran".
   *
   * `surface` runs before any message is shown -- the tray path passes
   * surfaceHub, because its failures otherwise land in a hidden window.
   */
  type Outcome = "ran" | "failed" | "dropped";
  async function guard(fn: () => Promise<unknown>, surface?: () => Promise<void>): Promise<Outcome> {
    if (busy) {
      // Dropped, not queued: a queued click runs later against state the user
      // can no longer see. But no longer silent -- unless this is the second
      // click of an accidental double-click (within Windows' 500ms
      // double-click time), which must do nothing and say nothing.
      if (surface) await surface();
      if (surface || performance.now() - busySince > 500) flashBusy();
      return "dropped";
    }
    error = "";
    busy = true;
    busySince = performance.now();
    const slow = setTimeout(() => (busyUi = true), 150);
    try {
      await fn();
      await refresh();
      return "ran";
    } catch (e) {
      if (surface) await surface();
      error = String(e);
      return "failed";
    } finally {
      clearTimeout(slow);
      busy = false;
      busyUi = false;
    }
  }

  const addEngine = () => guard(async () => {
    const dir = await open({ directory: true, title: "Select an Arcane engine folder" });
    if (typeof dir === "string") await registerEngine(dir);
  });

  // Pick the .arcproj FILE, not the containing folder. Project::Open accepts
  // both, but a folder holding two manifests is ambiguous and it bails --
  // Project.cpp:159 returns nullopt, asserted by ProjectTest.cpp:52. Picking the
  // file names the project unambiguously, and it is what the editor's own Open
  // dialog already asks for (EditorApp.cpp:1309 filters "Arcane Project"/arcproj).
  //
  // LISTS the project (add_project) and nothing more. It used to launch the
  // editor -- which is not what "Add" says, and which made adding impossible
  // without a registered engine.
  const addExisting = () => guard(async () => {
    const file = await open({
      title: "Select a project (.arcproj)",
      defaultPath: (await defaultDialogDir()) ?? undefined,
      filters: [{ name: "Arcane Project", extensions: ["arcproj"] }],
    });
    if (typeof file === "string") await addProject(file);
  });

  // The greyed row's Locate…: pick the moved project's .arcproj and repoint the
  // entry at it. Same file filter as Add, because it IS the same question --
  // "which manifest is this project" -- asked about a row we already hold.
  const locate = (p: RecentProject) => guard(async () => {
    const file = await open({
      title: `Locate ${p.name} (.arcproj)`,
      defaultPath: (await defaultDialogDir()) ?? undefined,
      filters: [{ name: "Arcane Project", extensions: ["arcproj"] }],
    });
    if (typeof file === "string") await relocateProject(p.path, file);
  });

  // Godot's Scan: pick a folder, list every project found under it. The
  // report is a notice, not a toast that vanishes -- counts of what was
  // refused (ambiguous folders, a truncated walk) are the part worth reading.
  // It REPLACES the previous scan's report: two reports stacked read as two
  // scans' worth of results, and deduping identical text would make a
  // repeated scan look like it did nothing.
  const scanFolder = () => guard(async () => {
    const dir = await pickFolder("Scan a folder for Arcane projects");
    if (!dir) return;
    const r = await scanForProjects(dir);
    notices = replaceNotices(notices, "scan",
      scanReportLines(r).map((text) => ({ id: ++noticeSeq, source: "scan" as const, text })));
  });

  // Shared by the New Project dialog and the Settings default-location field.
  const pickFolder = async (title: string): Promise<string | null> => {
    const dir = await open({
      directory: true,
      title,
      defaultPath: (await defaultDialogDir()) ?? undefined,
    });
    return typeof dir === "string" ? dir : null;
  };

  // `fromTray`: the pick came from the tray menu while the window may be
  // hidden. Anything short of launched/focused -- a throw, a missing project
  // or engine, a dropped click -- brings the Hub back before it says so.
  const launch = (p: RecentProject, fromTray = false) => {
    const surface = fromTray ? surfaceHub : undefined;
    return guard(async () => {
      // The project's own engine if it has a live pin, else the Hub default.
      // Same rule the card renders, so what launches is what the card said.
      const { engine } = resolveEngine(p.engineId, hub.engines, selectedEngine);
      if (!engine) {
        throw `No engine is registered to open ${p.name}. Add one on the Engines page.`;
      }
      // Hiding-while-running happens RUST-side (open_project), which also owns
      // the restore -- the frontend no longer touches the window on launch.
      const out = await openProject(p.path, engine.path);
      if (out.kind === "launched" || out.kind === "focused") return;
      if (surface) await surface();
      // A vanished PROJECT needs no words in the window: guard()'s refresh
      // flips the row to its missing treatment, which carries the
      // explanation. From the tray there was no row in view, so it is said.
      // The engine case always gets a notice because the Projects view has
      // no row for the engine to grey out -- the story lives on another screen.
      if (out.kind === "engineMissing") {
        pushNotice(`${engine.build} is no longer at ${engine.path}. See Engines.`, "launch");
      } else if (fromTray) {
        pushNotice(`${p.name} is no longer at ${p.path}. Locate it or remove it from the list.`, "launch");
      }
    }, surface);
  };

  // Closes only once the pin actually changed: closing first and saving after
  // lost the choice silently when the save failed or was dropped.
  const chooseEngine = async (engineId: string | null) => {
    if (modal.kind !== "engine") return;
    const m = modal;
    if ((await guard(() => setProjectEngine(m.p.path, engineId))) === "ran") closeIf(m);
  };

  // Create, then launch. create_project LISTS the project itself, so once the
  // create has run the project exists in the list whatever happens next: the
  // dialog closes on create success, and a failed launch is reported in the
  // main banner beside the new row -- not inside a dialog that would invite
  // creating it a second time.
  const makeProject = async (name: string, dir: string): Promise<boolean> => {
    const engine = selectedEngine;
    if (!engine || modal.kind !== "new") return false;
    const m = modal;
    let manifest = "";
    // create_project returns the .arcproj it wrote, so the new project is
    // launched exactly the way an Open-dialog project is.
    const made = await guard(async () => { manifest = await createProject(dir, name, engine.path); });
    // A failed create keeps the typed name and folder so the message
    // (rendered inside the dialog) can be acted on.
    if (made !== "ran") return false;
    closeIf(m);
    const key = projectKey(manifest);
    const p = hub.recents.find((r) => projectKey(r.path) === key);
    if (p) await launch(p);
    else await guard(() => openProject(manifest, engine.path));
    return true;
  };

  // Both of these follow makeProject's shape: the dialog closes only when the
  // action RAN, so a failure keeps the typed value with the message beside it
  // instead of dropping the user back to a banner they cannot read.
  const doRename = async (newName: string): Promise<boolean> => {
    if (modal.kind !== "rename") return false;
    const m = modal;
    const ok = (await guard(() => renameProject(m.p.path, newName))) === "ran";
    if (ok) closeIf(m);
    return ok;
  };

  const doArgs = async (args: string): Promise<boolean> => {
    if (modal.kind !== "args") return false;
    const m = modal;
    const ok = (await guard(() => setProjectArgs(m.p.path, args))) === "ran";
    if (ok) closeIf(m);
    return ok;
  };

  // The menu item's entry point. Reads the setting at CLICK time rather than
  // caching it, so turning the confirmation off takes effect immediately.
  const askDelete = (p: RecentProject) => {
    if (settings.confirmDelete) {
      openModal({ kind: "delete", p });
      return;
    }
    // Straight through, because the user asked for that. The safety net is
    // still there and is why this is offerable at all: delete_project moves the
    // folder to the Recycle Bin, so an unconfirmed mis-click is recoverable.
    guard(() => deleteProject(p.path));
  };

  const doDelete = async (): Promise<boolean> => {
    if (modal.kind !== "delete") return false;
    const m = modal;
    const ok = (await guard(() => deleteProject(m.p.path))) === "ran";
    if (ok) closeIf(m);
    return ok;
  };

  // Chromium's own menu -- Back, Reload, Save as, Inspect -- is webview
  // furniture that means nothing in a launcher, and it appeared everywhere a
  // project card was not. Suppressed app-wide so right-clicking one pixel
  // beside a card does not produce a completely different kind of menu.
  //
  // Text fields are the exception: cut/copy/paste and select-all there are
  // genuinely useful, and we offer no replacement for them.
  function onContextMenu(e: MouseEvent) {
    if ((e.target as HTMLElement).closest("input, textarea")) return;
    e.preventDefault();
  }

  // Resolves true only when the save ran -- SettingsModal re-asserts its
  // controls otherwise. Refused outright before startup has loaded the real
  // settings: saving then would write the placeholder defaults over them.
  const applySettings = async (next: Settings): Promise<boolean> => {
    if (!ready) return false;
    return (await guard(async () => {
      await saveSettings(next);
      // Re-read rather than trusting the local copy: Rust normalises the folder
      // string on save, so what is stored may differ from what was typed.
      settings = await loadSettings();
      resolveSelection();
    })) === "ran";
  };

  // The whole per-project vocabulary as ONE object (type owned by ProjectMenu,
  // which renders it). Declared after the handlers it references -- a const
  // object literal evaluates immediately, so naming them earlier would hit the
  // temporal dead zone.
  const projectActions: ProjectActions = {
    launch: (p) => launch(p),
    toggleFavorite: (p) => guard(() => setProjectFavorite(p.path, !p.favorite)),
    changeEngine: (p) => openModal({ kind: "engine", p }),
    reveal: (p) => guard(() => revealProject(p.path)),
    rename: (p) => openModal({ kind: "rename", p }),
    duplicate: (p) => guard(() => duplicateProject(p.path)),
    args: (p) => openModal({ kind: "args", p }),
    forget: (p) => guard(() => forgetProject(p.path)),
    locate,
    delete: askDelete,
  };

  // Same shape for the Engines view -- the vocabulary object mirrors
  // ProjectActions, typed where the controls that call it are rendered.
  const engineActions: EngineActions = {
    register: addEngine,
    registerPath: (path) => guard(() => registerEngine(path)),
    // PERSISTED: picking an engine saves it as Settings.defaultEngineId, and
    // the selection follows once the save lands (applySettings re-resolves).
    // It used to set page state only, so every restart fell back to the
    // first engine in the list.
    select: (e) => { void applySettings({ ...settings, defaultEngineId: e.id }); },
    forget: (e) => guard(() => forgetEngine(e.path)),
  };
</script>

<svelte:window oncontextmenu={onContextMenu} bind:innerHeight={winH} />

<div class="app">
  <WindowChrome />
  <div class="body">
    <Sidebar {view} engine={selectedEngine} loading={!ready}
             settingsOpen={modal.kind === "settings"}
             onNavigate={(v) => (view = v)}
             onSettings={() => { if (ready) openModal({ kind: "settings" }); }} />
    <main>
      <!-- Suppressed while a dialog is up: the banner sits under the scrim,
           so the dialog renders the same message itself. -->
      {#if error && !modalUp}
        <p class="banner error" role="alert">{error}</p>
      {/if}
      <!-- Distinct from `error`: these are not a failed action but facts the
           Hub had to report (recovery on load, a scan, a launch that died),
           and staying quiet about them is how a reset project list becomes
           indistinguishable from having no projects. Keyed by id now that
           each one can be dismissed on its own. -->
      {#if notices.length > 0}
        <div class="notices" class:open={noticesOpen}>
          {#if shownNotices.hidden > 0}
            <button type="button" class="more" onclick={() => (noticesOpen = true)}>
              +{shownNotices.hidden} earlier {shownNotices.hidden === 1 ? "notice" : "notices"}
            </button>
          {/if}
          {#each shownNotices.shown as n (n.id)}
            <div class="banner warn notice" role="status">
              <span class="nt">{n.text}</span>
              <button type="button" class="x" title="Dismiss" aria-label="Dismiss notice"
                      onclick={() => dismissNotice(n.id)}><Icon name="x" size={14} /></button>
            </div>
          {/each}
          {#if noticesOpen && notices.length > noticeCap}
            <button type="button" class="more" onclick={() => (noticesOpen = false)}>Show fewer</button>
          {/if}
        </div>
      {/if}
      {#if !ready}
        <!-- Quiet on purpose: startup is milliseconds of IPC, and a skeleton
             that flashes for one frame reads worse than a line of text. If
             startup failed the error banner above says why, and this offers
             the one thing to do about it. -->
        {#if error}
          <div class="loading"><Button onclick={() => (startup = start())}>Try again</Button></div>
        {:else}
          <p class="loading" role="status">Loading projects&hellip;</p>
        {/if}
      {:else if view === "projects"}
        <ProjectsView recents={hub.recents} engines={hub.engines}
                      defaultEngine={selectedEngine} busy={busyUi} {running} {covers}
                      layout={settings.projectView}
                      confirmDelete={settings.confirmDelete}
                      sort={settings.projectSort} sortDesc={settings.projectSortDesc}
                      bind:query={projectsQuery} bind:scrollTop={projectsScroll}
                      actions={projectActions}
                      onOpen={addExisting} onScan={scanFolder}
                      onNew={() => openModal({ kind: "new" })}
                      onLayout={(v) => applySettings({ ...settings, projectView: v })}
                      onSort={(k) => {
                        const n = nextSort(settings.projectSort, settings.projectSortDesc, k);
                        applySettings({ ...settings, projectSort: n.key, projectSortDesc: n.desc });
                      }} />
      {:else if view === "engines"}
        <EnginesView engines={hub.engines} selected={selectedEngine} {suggestion}
                     busy={busyUi} actions={engineActions} />
      {:else}
        <PackagesView />
      {/if}
    </main>
  </div>

  {#if modal.kind === "new"}
    <NewProjectModal busy={busyUi} {error} engine={selectedEngine}
                     defaultDir={settings.defaultProjectDir}
                     onBrowse={() => pickFolder("Where should the project live?")}
                     onCreate={makeProject} onCancel={closeModal} />
  {:else if modal.kind === "engine"}
    <ProjectEngineModal project={modal.p} engines={hub.engines}
                        defaultEngine={selectedEngine} busy={busyUi} {error}
                        onChoose={chooseEngine} onCancel={closeModal} />
  {:else if modal.kind === "rename"}
    <RenameProjectModal project={modal.p} busy={busyUi} {error}
                        onRename={doRename} onCancel={closeModal} />
  {:else if modal.kind === "args"}
    <ProjectArgsModal project={modal.p} busy={busyUi} {error}
                      onSave={doArgs} onCancel={closeModal} />
  {:else if modal.kind === "delete"}
    <DeleteProjectModal project={modal.p} busy={busyUi} {error}
                        onDelete={doDelete} onCancel={closeModal} />
  {:else if modal.kind === "settings"}
    <SettingsModal {settings} busy={busyUi} {hubDir} {version} {error}
                   recentCount={hub.recents.length}
                   onSave={applySettings}
                   onBrowseDir={() => pickFolder("Default location for projects")}
                   onReveal={() => guard(revealHubDataDir)}
                   onClearRecents={() => guard(clearRecents)}
                   onClose={closeModal} />
  {/if}

  <!-- Always in the DOM so screen readers announce what appears in it; above
       the scrim (z 20/21) because a dropped click can come from a dialog. -->
  <div class="toast-region" role="status" aria-live="polite">
    {#if busyFlash}<p class="toast">Busy — try again in a moment.</p>{/if}
  </div>
</div>

<style>
  /* Flat neutral. The two radial washes that used to sit here -- a gold glow
     top-right and an explicitly blue one bottom-left -- were the largest
     coloured surface in the app and the main reason it read as tinted.
     NO border-radius: the OS window is a plain rectangle, and rounding the
     canvas inside it just faked curved corners over square ones (user call
     2026-07-29 killing --r-win). */
  .app { display: flex; flex-direction: column; height: 100vh; overflow: hidden;
         background: linear-gradient(var(--bg-top), var(--bg-bottom)); }
  .body { flex: 1; display: flex; min-height: 0; }
  /* A column, so a view can fill the height and scroll only its own list
     (ProjectsView: the title, search and actions stay put). A view that
     doesn't (Engines, Packages) keeps its natural height and main scrolls it
     as before. The padding is named because ProjectsView's scroll region
     reaches into it, to put its scrollbar at the window edge. */
  main { --main-pad-x: 28px; --main-pad-b: 26px;
         flex: 1; min-width: 0; overflow-y: auto; display: flex; flex-direction: column;
         padding: 22px var(--main-pad-x) var(--main-pad-b); }
  /* Banners and natural-height views keep their size; only a view that asks
     to fill (flex: 1 on its own root) does. */
  main > :global(*) { flex: none; }

  /* The banner look is shared (theme.css .banner); its spacing is the page's. */
  main > .banner { margin: 0 0 14px; }

  /* Compact, because they sit above the project list and every pixel they
     take is a pixel of list gone -- capped at noticeCap, and when expanded
     they scroll among themselves rather than pushing the list off screen. */
  .notices { display: flex; flex-direction: column; gap: 6px; margin: 0 0 14px; }
  .notices.open { max-height: 40vh; overflow-y: auto; }
  .notice { display: flex; align-items: flex-start; gap: 10px; padding: 7px 8px 7px 12px;
            font-size: 12.5px; }
  .nt { flex: 1; min-width: 0; padding-top: 2px; }
  .x { flex: none; width: 22px; height: 22px; display: grid; place-items: center;
       padding: 0; border: 0; border-radius: 4px; background: none;
       color: var(--text-dim); cursor: default;
       transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .x:hover { background: var(--hover-wash-strong); color: var(--text); }
  .more { align-self: flex-start; background: none; border: 0; padding: 2px 0;
          font: inherit; font-size: 12px; color: var(--text-muted); cursor: default; }
  .more:hover { color: var(--text); }

  .loading { margin: 6px 0 0; font-size: 13px; color: var(--text-dim); }

  .toast-region { position: fixed; left: 50%; bottom: 18px; translate: -50% 0;
                  z-index: 40; pointer-events: none; }
  .toast { margin: 0; padding: 8px 14px; font-size: 13px; color: var(--text);
           background: var(--surface); border: 1px solid var(--border);
           border-radius: var(--r-btn); box-shadow: var(--shadow-pop);
           white-space: nowrap; animation: rise var(--dur) var(--ease); }
  @keyframes rise { from { opacity: 0; transform: translateY(4px) } }
</style>
