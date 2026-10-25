<script module lang="ts">
  // MODULE script, not the instance script: a type has to be exported at module
  // scope to be importable as `import Sidebar, { type View } from "..."`.
  // Declaring it in the instance <script> below would not export it.
  // Settings is deliberately absent: it is a sheet you open over the current
  // view, not a destination the rail can point at.
  export type View = "projects" | "engines" | "packages";
</script>

<script lang="ts">
  import Icon, { type IconName } from "$lib/components/Icon.svelte";
  import type { EngineEntry } from "$lib/api";
  let { view, engine, loading = false, settingsOpen, onNavigate, onSettings }: {
    view: View;
    engine: EngineEntry | null;
    /** Startup has not loaded state yet: say nothing rather than "None registered". */
    loading?: boolean;
    settingsOpen: boolean;
    onNavigate: (v: View) => void;
    onSettings: () => void;
  } = $props();

  type NavItem = { id: View; label: string; icon: IconName };

  // What you DO with the Hub sits at the top; how the Hub itself behaves sits
  // at the bottom, out of the way of the daily path.
  //
  // The gear belongs to Settings, where it is the near-universal convention;
  // Engines takes the box, reading as the thing that gets installed and picked;
  // Packages, when it returns, takes stacked layers.
  //
  // Packages is HIDDEN until it is built (user call 2026-10-13): its page is a
  // "not built yet" placeholder citing an internal spec, which a public
  // preview must not ship. PackagesView and the "packages" view id stay, so
  // bringing it back is this one line.
  const items: NavItem[] = [
    { id: "projects", label: "Projects", icon: "folder" },
    { id: "engines", label: "Engines", icon: "box" },
  ];
</script>

<aside class="side">
  <!-- aria-current="true", not "page": these are buttons switching an in-app
       view, not links to documents, so the generic boolean form is the accurate
       one. EngineRow uses the same value for the same reason. -->
  <nav aria-label="Main">
    {#each items as it (it.id)}
      <button class="nav" type="button" class:on={view === it.id}
              onclick={() => onNavigate(it.id)}
              aria-current={view === it.id ? "true" : undefined}>
        <Icon name={it.icon} />{it.label}
      </button>
    {/each}
  </nav>

  <!-- Pushed to the bottom as one block so Settings sits directly above the
       engine card rather than floating in the middle of the empty rail. -->
  <div class="foot">
    <!-- Outside the <nav> and carrying aria-haspopup, because it opens a sheet
         over the current view rather than changing which view the rail points
         at. Same pill styling regardless, so the rail still reads as one
         column; `on` here means "the sheet is up", not "you are here". -->
    <button class="nav" type="button" class:on={settingsOpen}
            aria-haspopup="dialog" aria-expanded={settingsOpen}
            onclick={onSettings}>
      <Icon name="gear" />Settings
    </button>
    <div class="eng">
      <!-- "Default", not "Active": a project with its own pinned engine ignores
           this, so calling it active would misdescribe what launches. -->
      <div class="label">Default engine</div>
      {#if loading}
        <!-- An empty line of the same height: "None registered" for the few
             milliseconds before state arrives would be a false statement. -->
        <div class="v none" aria-hidden="true">&nbsp;</div>
      {:else if engine}
        <!-- title so the full build string is still reachable once the rail is
             narrow enough to clip it. -->
        <div class="v" title={engine.missing
               ? `${engine.build} — nothing is at its path any more`
               : engine.build}>
          <span class="dot" class:bad={engine.missing} aria-hidden="true"></span>
          <span class="build">{engine.build}</span>
        </div>
        <!-- Lowercase: a code-style token in mono, like the project chips. -->
        <code>abi {engine.engineAbi}</code>
      {:else}
        <div class="v none">None registered</div>
      {/if}
    </div>
  </div>
</aside>

<style>
  /* 22% of the window, not a fixed 198px, so the rail scales with the frame.
     `flex: none` keeps that exact share -- without it the sidebar would shrink
     under a wide main area and the percentage would be a suggestion.
     At the 800px window minimum this is 176px, narrower than the 198px it
     replaces, which is why the engine name below is truncated rather than left
     to wrap. */
  /* Capped at 260px: an uncapped 22% made the rail 422px of mostly empty
     column on a maximised 1920px window (2026-10-13). The floor keeps the
     176px it always had at the 800px minimum. */
  .side { width: clamp(176px, 22%, 260px); flex: none; background: var(--surface-2);
          border-right: 1px solid var(--border-soft);
          padding: 10px 11px 14px; display: flex; flex-direction: column;
          min-width: 0; }
  nav { display: flex; flex-direction: column; gap: 2px; }
  /* margin-top:auto on the GROUP, not on the engine card: Settings has to
     travel down with it, and a separator reads as the boundary between "what
     you do" and "how the Hub behaves". */
  .foot { margin-top: auto; display: flex; flex-direction: column; gap: 12px;
          padding-top: 12px; border-top: 1px solid var(--border-soft); }
  .nav { display: flex; align-items: center; gap: 11px; padding: 10px 11px;
         border-radius: 6px; font: inherit; font-size: 13.5px; text-align: left;
         background: none; border: 0; color: var(--text-muted); cursor: default;
         transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .nav:hover:not(.on) { background: var(--hover-wash); color: var(--text); }
  /* Selection by SURFACE, not by accent. This was a gold-tinted fill plus a
     gold inset rail; the active nav item is on screen permanently, so it was
     one of the largest constant sources of colour in the app. A lighter
     neutral pill reads just as unambiguously against the sidebar. */
  .nav.on { background: var(--surface-sel); color: var(--text); }
  /* No opacity dimming on the icon: it inherits the row's colour via
     currentColor, so it brightens with the label on hover and on the active
     item instead of staying permanently washed out. */

  .eng { background: var(--surface); border: 1px solid var(--border-soft);
         border-radius: var(--r-panel); padding: 12px 13px; }
  .v { font-size: 13px; margin-top: 6px; display: flex; align-items: center; gap: 7px; }
  /* --text-dim, deliberately: an absent engine is inert, not actionable and
     not an error. Rendering it in the accent would invite a click that does
     nothing, and in --fail would overstate a normal first-run state. */
  .v.none { color: var(--text-dim); }
  .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--ok); flex: none; }
  /* The default engine's exe stopped resolving: the one place coral appears in
     the rail, because the daily path (Launch) is about to be refused. */
  .dot.bad { background: var(--fail); }
  /* Build strings run long ("Arcane 0.1 (M6) [Debug]"). Ellipsis rather than
     wrap: a two-line engine name pushes the footer around as the window
     resizes, which is exactly what a percentage-width rail makes common. */
  .build { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  code { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim);
         display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
</style>
