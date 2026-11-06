<script lang="ts">
  import Button from "./Button.svelte";
  import type { EngineEntry } from "$lib/api";

  let { engine, selected, busy = false, onSelect, onForget }:
    {
      engine: EngineEntry; selected: boolean; busy?: boolean;
      onSelect: () => void; onForget: () => void;
    } = $props();
</script>

<div class="row" class:sel={selected} class:gone={engine.missing}
     title={engine.missing
       ? "Nothing is at this path any more. A rebuild restores it in place, or Remove it."
       : undefined}>
  <!-- aria-current, not aria-pressed: picking an engine is "this one is now
       current" in a mutually exclusive set, not a two-state toggle you can
       un-press. Same vocabulary as Sidebar's active nav item. -->
  <button class="pick" type="button" onclick={onSelect}
          aria-current={selected ? "true" : undefined}>
    <span class="nm">{engine.build}</span>
    <!-- title: the path is truncated with an ellipsis, and the part cut off
         (the configuration folder, the exe) is the part that tells two
         builds of one engine apart. -->
    <code class="path" title={engine.path}>{engine.path}</code>
  </button>
  {#if engine.missing}
    <!-- The badge takes the abi slot: a number probed from an exe that is no
         longer there is not worth repeating. Same treatment as project rows.
         Lowercase "abi 59" is a code-style token (mono); prose says "ABI". -->
    <code class="abi badge">missing</code>
  {:else}
    <code class="abi">abi {engine.engineAbi}</code>
  {/if}
  <Button variant="danger" disabled={busy} onclick={onForget}>Remove</Button>
</div>

<style>
  .row { display: flex; align-items: center; gap: 14px; padding: 13px 14px;
         border: 1px solid var(--border-soft); border-radius: var(--r-panel);
         background: var(--surface); margin-bottom: 8px;
         transition: border-color var(--dur) var(--ease); }
  .row:hover { border-color: var(--border-hover); }
  /* Selection is a lighter surface, same vocabulary as the sidebar's active
     item. `.row.sel`, not bare `.sel`: `.row` sets background at equal
     specificity, so a bare class would win only by declaration order. */
  .row.sel { background: var(--surface-sel); border-color: var(--border-hover); }
  .pick { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 3px;
          background: none; border: 0; padding: 0; text-align: left;
          font: inherit; color: inherit; cursor: default; }
  .nm { font-size: 14px; font-weight: 600; }
  .path { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .abi { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim); flex: none; }

  /* Same dim-plus-coral-badge pair the project rows use for a gone path. */
  .row.gone .nm { color: var(--text-muted); }
  .row.gone .badge { font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent);
           border-radius: 3px; padding: 0 4px; }
</style>
