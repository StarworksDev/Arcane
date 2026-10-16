<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import type { Settings } from "$lib/api";

  // Settings is a SHEET, not a view: it is somewhere you go, change one thing
  // and leave, and routing it through the rail meant the project list -- the
  // reason the Hub is open -- disappeared behind it. As a modal the list stays
  // visible behind the scrim and Escape puts you back on it.
  let { settings, busy, recentCount, hubDir, version, error,
        onSave, onBrowseDir, onReveal, onClearRecents, onClose }: {
    settings: Settings;
    busy: boolean;
    recentCount: number;
    hubDir: string;
    version: string;
    /** The last guard() failure. Shown here because the banner is under the scrim. */
    error: string;
    /** Resolves true only if the save actually ran (not failed, not dropped). */
    onSave: (s: Settings) => Promise<boolean>;
    onBrowseDir: () => Promise<string | null>;
    onReveal: () => void;
    onClearRecents: () => void;
    onClose: () => void;
  } = $props();

  type Cat = "projects" | "launching" | "data" | "about";
  const CATS: { id: Cat; label: string }[] = [
    { id: "projects", label: "Projects" },
    { id: "launching", label: "Launching" },
    { id: "data", label: "Data" },
    { id: "about", label: "About" },
  ];
  // Reset to the first category on every open, because the sheet is mounted
  // fresh each time. Remembering the last one would mean re-opening Settings
  // lands somewhere the user did not ask for and may not recognise.
  let cat = $state<Cat>("projects");

  // Local mirror of the text field. Committing on change (blur/Enter) rather
  // than on every keystroke keeps a half-typed path out of the settings file.
  //
  // Re-synced from the prop rather than seeded once: Rust normalises the folder
  // on save, so a typed "D:\Games\" comes back as "D:\Games" and the field must
  // show what was actually stored. The sheet stays mounted across saves, so a
  // one-shot seed would leave the two permanently out of step.
  let dirDraft = $state("");
  // Guarded on the STORED value changing, not on `settings` being reassigned.
  // Every save replaces the whole object, so an unguarded sync also fired when
  // an unrelated setting changed -- toggling the checkbox discarded whatever
  // the user had typed here but not yet committed. A plain variable, so reading
  // it does not make this effect depend on itself.
  let lastSynced: string | null = null;
  $effect(() => {
    const stored = settings.defaultProjectDir;
    if (stored !== lastSynced) {
      lastSynced = stored;
      dirDraft = stored;
    }
  });
  // Two-step confirm: clearing is reversible only by re-opening every project,
  // so it should not be one stray click.
  let confirmClear = $state(false);

  function commitDir() {
    if (dirDraft === settings.defaultProjectDir) return;
    onSave({ ...settings, defaultProjectDir: dirDraft });
  }

  async function browse() {
    const picked = await onBrowseDir();
    if (picked === null) return;
    dirDraft = picked;
    onSave({ ...settings, defaultProjectDir: picked });
  }

  function clearDir() {
    dirDraft = "";
    onSave({ ...settings, defaultProjectDir: "" });
  }

  function doClear() {
    confirmClear = false;
    onClearRecents();
  }

  // The checkbox and the radios bind ONE-WAY (`checked={settings...}`): the
  // click toggles the DOM at once and the save follows. If the save fails or
  // is dropped, `settings` does not change, so Svelte has nothing to re-render
  // -- and the control kept showing a value that was never stored. Re-assert
  // every control in the group from `settings` whenever a save did not land.
  async function saveFrom(el: HTMLInputElement, next: Settings) {
    const group = el.type === "radio"
      ? [...(el.closest("[role=radiogroup]")?.querySelectorAll<HTMLInputElement>("input") ?? [])]
      : [el];
    if (await onSave(next)) return;
    for (const c of group) {
      c.checked = c.type === "radio" ? c.value === settings.launchBehavior : settings.confirmDelete;
    }
  }

  // One entry per launch behaviour, in the order they appear. The label names
  // the effect; the detail names what it costs or buys, so choosing does not
  // require trying all three.
  const BEHAVIORS: { id: Settings["launchBehavior"]; label: string; d: string }[] = [
    { id: "tray", label: "Minimize to the tray",
      d: "The Hub waits in the system tray while editors run — click the icon to bring it back, right-click it to launch a recent project or quit — and comes back when the last editor closes." },
    { id: "close", label: "Close the Hub",
      d: "The Hub exits once the editor is up. The editor keeps running on its own; launch the Hub again from the Start menu or a project file. Launching a project the Hub no longer knows is open starts a second editor on it." },
    { id: "stay", label: "Keep the Hub open",
      d: "The window stays where it is, beside the editor." },
  ];
</script>

<Modal title="Settings" size="full" {onClose}>
  {#snippet aside()}
    <span class="chip">Arcane Hub {version}</span>
  {/snippet}

  <div class="split">
    <!-- A second, nested rail. It navigates WITHIN the sheet, so it is a nav
         with its own label rather than a repeat of the window's sidebar. -->
    <nav class="cats" aria-label="Settings sections">
      {#each CATS as c (c.id)}
        <button class="cat" type="button" class:on={cat === c.id}
                aria-current={cat === c.id ? "true" : undefined}
                onclick={() => (cat = c.id)}>{c.label}</button>
      {/each}
    </nav>

    <div class="pane">
      {#if error}
        <p class="banner error" role="alert">{error}</p>
      {/if}

      {#if cat === "projects"}
        <section class="grp">
          <h3>Default location</h3>
          <p class="d">Where the New project and Add dialogs start. Leave it empty
            to let Windows pick the folder you used last. Nothing is moved or
            imported — this only changes where the dialogs open.</p>
          <div class="ctl">
            <input class="field" bind:value={dirDraft} spellcheck="false" placeholder="Not set"
                   aria-label="Default location for projects"
                   onchange={commitDir}
                   onkeydown={(e) => e.key === "Enter" && commitDir()} />
            <Button onclick={browse} disabled={busy}>Browse&hellip;</Button>
            <Button onclick={clearDir} disabled={busy || dirDraft === ""}>Clear</Button>
          </div>
        </section>
        <section class="grp">
          <h3>Deleting a project</h3>
          <p class="d">Deleted projects go to the Recycle Bin, so this is a
            speed bump rather than the only thing standing between you and a
            lost project &mdash; but it is the last point at which the folder
            about to go is named.</p>
          <label class="check">
            <input type="checkbox" checked={settings.confirmDelete} disabled={busy}
                   onchange={(e) => saveFrom(e.currentTarget,
                                             { ...settings, confirmDelete: e.currentTarget.checked })} />
            <span>Ask for confirmation before deleting</span>
          </label>
        </section>
      {:else if cat === "launching"}
        <section class="grp">
          <h3>After launching a project</h3>
          <p class="d">Whichever you choose, the editor runs independently
            &mdash; closing or quitting the Hub never closes an editor that is
            already open.</p>
          <div class="radios" role="radiogroup" aria-label="After launching a project">
            {#each BEHAVIORS as b (b.id)}
              <label class="check radio">
                <input type="radio" name="launch-behavior" value={b.id}
                       checked={settings.launchBehavior === b.id} disabled={busy}
                       onchange={(e) => saveFrom(e.currentTarget,
                                                 { ...settings, launchBehavior: b.id })} />
                <span class="rtext"><strong>{b.label}</strong>
                  <span class="rd">{b.d}</span></span>
              </label>
            {/each}
          </div>
        </section>
      {:else if cat === "data"}
        <section class="grp">
          <h3>Hub data folder</h3>
          <!-- The project list is SHARED: the editor reads and writes the same
               recents.archub for File > Open Recent and its start page
               (ArcaneEditor/src/Project/RecentProjects.hpp). Engines and
               settings are the Hub's alone. -->
          <p class="d">Where the project list, the registered engines and these
            settings are stored. The editor shares the project list for its
            Open Recent menu; the rest is the Hub's alone.</p>
          <p class="path"><code>{hubDir}</code></p>
          <Button onclick={onReveal} disabled={busy}>Open folder</Button>
        </section>
        <section class="grp">
          <h3>Recent projects</h3>
          <!-- States plainly what the Remove control on a card also promises. -->
          <p class="d">{recentCount} in the list. Clearing also empties the editor's
            Open Recent menu; nothing is deleted from disk.</p>
          {#if confirmClear}
            <div class="ctl">
              <Button onclick={() => (confirmClear = false)} disabled={busy}>Cancel</Button>
              <Button variant="danger" onclick={doClear} disabled={busy}>Clear the list</Button>
            </div>
          {:else}
            <Button variant="danger" disabled={busy || recentCount === 0}
                    onclick={() => (confirmClear = true)}>Clear list</Button>
          {/if}
        </section>
      {:else}
        <section class="grp">
          <h3>Arcane Hub {version}</h3>
          <!-- Says installs are COMING, not out of scope: the Engines page
               already shows a disabled Install button for exactly that, and
               the two must not contradict each other. No dates promised. -->
          <p class="d">Registers engines and launches projects. Installing and
            updating engines from the Hub arrives with the engine manager; until
            then, add an engine you already have from the Engines page.</p>
        </section>
      {/if}
    </div>
  </div>
</Modal>

<style>
  /* Matches the window sidebar's proportion at the default size without
     tracking it: the sheet is inset from the frame, so a shared percentage
     would put the two rails a few pixels out of alignment rather than in it.
     minmax(0, 1fr) on the pane keeps a long path from widening the grid. */
  .split { display: grid; grid-template-columns: 200px minmax(0, 1fr);
           min-height: 0; }
  /* Both tracks scroll independently, and both need min-height:0 -- a grid item
     defaults to min-height:auto, which refuses to shrink below its content and
     would push the sheet's own scrollbar back onto the panel. */
  .cats { min-height: 0; overflow-y: auto; display: flex; flex-direction: column;
          gap: 2px; padding: 14px 10px; background: var(--surface-2);
          border-right: 1px solid var(--border-soft); }
  .pane { min-height: 0; overflow-y: auto; padding: 20px 24px 26px; }

  /* Same pill as the window rail: two levels of navigation that look unrelated
     read as two different apps. */
  .cat { display: block; width: 100%; text-align: left; font: inherit;
         font-size: 13.5px; padding: 9px 12px; border-radius: 6px;
         background: none; border: 0; color: var(--text-muted); cursor: default;
         transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .cat:hover:not(.on) { background: var(--hover-wash); color: var(--text); }
  .cat.on { background: var(--surface-sel); color: var(--text); }

  .chip { font-size: 11.5px; color: var(--text-muted); background: var(--surface-2);
          border: 1px solid var(--border-soft); border-radius: 999px;
          padding: 3px 10px; white-space: nowrap; }

  .grp { padding-bottom: 20px; margin-bottom: 20px;
         border-bottom: 1px solid var(--border-soft); }
  .grp:last-child { padding-bottom: 0; margin-bottom: 0; border-bottom: 0; }
  /* Sentence case at text weight, not the uppercase .label token: these are
     headings inside a wide pane, and the tracked-out micro-caps used elsewhere
     stop reading as a heading once there is this much space around them. */
  h3 { font-size: 15px; font-weight: 600; color: var(--text-bright); margin: 0 0 6px; }
  /* Capped measure. The pane is ~700px wide at the default window size, which
     is well past the point where a paragraph gets hard to track back from. */
  .d { font-size: 13px; color: var(--text-muted); margin: 0 0 14px;
       line-height: 1.55; max-width: 68ch; }

  .ctl { display: flex; gap: 8px; }
  /* The field's look is the shared .field (theme.css); only its share of the
     row is local. */
  .ctl input { flex: 1; }
  .pane > .banner { margin: 0 0 18px; }

  .path { margin: 0 0 12px; }
  code { font-family: var(--font-mono); font-size: 12px; color: var(--text-muted);
         background: var(--well); border: 1px solid var(--border-soft);
         border-radius: var(--r-btn); padding: 7px 10px; display: inline-block;
         user-select: text; overflow-wrap: anywhere; }

  .check { display: flex; align-items: center; gap: 10px; font-size: 13.5px;
           cursor: default; }
  /* Real native controls, restyled: accent-color keeps the control (and its
     keyboard and screen-reader behaviour) instead of faking one with a div. */
  input[type="checkbox"], input[type="radio"] {
    flex: none; width: 17px; height: 17px; margin: 0;
    accent-color: var(--accent); cursor: default; }

  .radios { display: flex; flex-direction: column; gap: 12px; max-width: 68ch; }
  /* Top-align the dot with the label line, not the centre of the two-line
     block -- centred, the dot appears to belong to neither line. */
  .check.radio { align-items: flex-start; }
  .check.radio input[type="radio"] { margin-top: 1px; }
  .rtext { display: flex; flex-direction: column; gap: 2px; }
  .rtext strong { font-weight: 600; color: var(--text-bright); }
  .rd { font-size: 12.5px; color: var(--text-muted); line-height: 1.5; }
</style>
