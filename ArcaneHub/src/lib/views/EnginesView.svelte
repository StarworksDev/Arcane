<script module lang="ts">
  import type { EngineEntry } from "$lib/api";

  // The per-engine action vocabulary as one object, mirroring ProjectActions:
  // the view that renders the controls owns the contract they call.
  export type EngineActions = {
    /** Folder dialog -> register whatever ArcaneEditor.exe it holds. */
    register: () => void;
    /** Register a known path -- the found-one-nearby suggestion button. */
    registerPath: (path: string) => void;
    select: (e: EngineEntry) => void;
    forget: (e: EngineEntry) => void;
  };
</script>

<script lang="ts">
  import Button from "$lib/components/Button.svelte";
  import EmptyState from "$lib/components/EmptyState.svelte";
  import EngineRow from "$lib/components/EngineRow.svelte";

  let { engines, selected, suggestion, busy, actions }:
    {
      engines: EngineEntry[]; selected: EngineEntry | null; suggestion: EngineEntry | null;
      busy: boolean; actions: EngineActions;
    } = $props();
</script>

<!-- Same header shape as ProjectsView: title hard left, actions hard right on
     ONE row, with the count dropping to a slim second row. The two views are
     the same kind of surface and should not be laid out differently.
     Ghost-left / primary-right also matches the order there. -->
<header class="top">
  <h2 class="display view-title">Engines</h2>
  <div class="acts">
    <!-- "Add engine…", not "Locate": the project menu's "Locate…" repoints a
         MOVED project, and one word for two different jobs made each read like
         the other. "Add" matches the Projects page's Add (put something that
         already exists on disk into the list), which is the distinction from
         Install beside it. The prop and the Rust command stay `register` --
         that is still the effect. -->
    <Button disabled={busy} onclick={actions.register}
            title="Point the Hub at a folder containing ArcaneEditor.exe">Add engine&hellip;</Button>
    <!-- Wrapped in a titled span because a DISABLED button receives no mouse
         events, so its own title never renders a tooltip -- and a greyed
         control with no explanation is just a dead end. -->
    <!-- Primary-and-disabled is a CHOSEN exception to the no-dead-controls
         rule (user decision, 2026-07-28 review): the primary slot deliberately
         advertises where the Hub is going -- engine installs are the
         version-management destination -- and the tooltip carries the honesty.
         Do not re-flag it in reviews; revisit when installs exist. -->
    <span title="Downloading and installing engines is not built yet">
      <Button variant="primary" disabled>Install engine</Button>
    </span>
  </div>
</header>

<p class="view-sub meta">
  <!-- &nbsp; because Svelte trims the block's leading whitespace: a plain
       space here rendered as "registered· using". -->
  {engines.length} registered{#if selected}&nbsp;&middot; using {selected.build}{/if}
</p>

{#if engines.length === 0}
  <EmptyState title="No engine registered"
              body="The Hub launches projects with an engine you register. Point it at a folder containing ArcaneEditor.exe.">
    {#if suggestion}
      <!-- {@const} binds the narrowed value so TypeScript does not have to
           re-narrow a reactive prop inside the callback closure below. -->
      {@const s = suggestion}
      <Button variant="primary" disabled={busy} onclick={() => actions.registerPath(s.path)}>
        Found one nearby &mdash; register {s.build}
      </Button>
    {/if}
  </EmptyState>
{:else}
  {#each engines as e (e.id)}
    <EngineRow engine={e} selected={selected?.id === e.id} {busy}
               onSelect={() => actions.select(e)} onForget={() => actions.forget(e)} />
  {/each}
  <p class="hint">The selected engine is the default: it launches any project that
    has not been given its own. Change a project's engine from its card.</p>
{/if}

<style>
  /* Identical to ProjectsView's .top/.acts, less the search field: there is
     nothing between the title and the buttons here, so the group is pushed
     right by its own margin rather than by the field's. */
  .top { display: flex; align-items: center; gap: 12px; }
  .acts { display: flex; gap: 8px; flex: none; margin-left: auto; }

  /* Same rhythm as ProjectsView's second row. Beats the global .view-sub
     margin, which is written for a subtitle tucked under a title. */
  .meta { margin: 10px 0 14px; }

  .hint { font-size: 12.5px; color: var(--text-dim); margin: 14px 0 0; }
</style>
