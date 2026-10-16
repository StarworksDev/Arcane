<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import type { EngineEntry, RecentProject } from "$lib/api";

  // Reached only from a project's engine band, which is disabled until some
  // engine is registered -- so `engines` is never empty here, and this dialog
  // no longer carries a "register an engine first" branch nothing could show.
  let { project, engines, defaultEngine, busy, error, onChoose, onCancel }: {
    project: RecentProject;
    engines: EngineEntry[];
    defaultEngine: EngineEntry | null;
    busy: boolean;
    /** The last failure, shown here because the banner is under the scrim. */
    error: string;
    /** null = follow the Hub default. The parent closes the dialog on success. */
    onChoose: (engineId: string | null) => void;
    onCancel: () => void;
  } = $props();

  // "" is the radio value for "follow the default" -- a radio group cannot
  // carry a null value, and an empty string is never a real engine id.
  const DEFAULT = "";
  // Seeded once, deliberately: +page mounts the dialog fresh per project
  // (`{#if modal.kind === "engine"}`) and from then on `choice` is the user's
  // edit, which a re-render must not overwrite. A pin to an engine that is
  // missing (or no longer registered) is not offered as the starting choice:
  // confirming it would re-pin a project to an exe that is not there.
  // svelte-ignore state_referenced_locally
  const pinnedLive = engines.some((e) => e.id === project.engineId && !e.missing);
  // svelte-ignore state_referenced_locally
  let choice = $state(pinnedLive ? (project.engineId ?? DEFAULT) : DEFAULT);

  const apply = () => onChoose(choice === DEFAULT ? null : choice);
</script>

<Modal title="Engine for {project.name}" onClose={onCancel}>
  <p class="lead">
    Projects follow the Hub's default engine unless they choose their own.
  </p>

  <div class="opts" role="radiogroup" aria-label="Engine for {project.name}">
    <label class="opt" class:on={choice === DEFAULT}>
      <input type="radio" bind:group={choice} value={DEFAULT} disabled={busy} />
      <span class="txt">
        <span class="nm">Use the default</span>
        {#if defaultEngine}
          <span class="sub">
            Currently {defaultEngine.build} &middot; ABI {defaultEngine.engineAbi}.
            Follows the default engine (chosen in Engines), so changing it moves
            this project too.
          </span>
        {/if}
      </span>
    </label>

    {#each engines as e (e.id)}
      <!-- A missing engine is listed (so a project pinned to it can see why)
           but cannot be picked: pinning a project to an exe that is not on
           disk only moves the failure to launch time. -->
      <label class="opt" class:on={choice === e.id} class:gone={e.missing}
             title={e.missing ? "Nothing is at this engine's path any more. See Engines." : undefined}>
        <input type="radio" bind:group={choice} value={e.id} disabled={busy || e.missing} />
        <span class="txt">
          <span class="nm">{e.build}{#if e.missing}<span class="badge">missing</span>{/if}</span>
          <span class="sub">ABI {e.engineAbi} &middot; <code>{e.path}</code></span>
        </span>
      </label>
    {/each}
  </div>

  {#if error}
    <p class="banner error" role="alert">{error}</p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <!-- The button names what it will do: "Use this engine" was wrong when
         the selected option is the default, which is not an engine. -->
    <Button variant="primary" onclick={apply} disabled={busy}>
      {choice === DEFAULT ? "Use the default" : "Use this engine"}
    </Button>
  {/snippet}
</Modal>

<style>
  .lead { font-size: 13px; color: var(--text-dim); margin: 0; line-height: 1.55; }
  .opts { display: flex; flex-direction: column; gap: 6px; }

  .opt { display: flex; align-items: flex-start; gap: 11px; cursor: default;
         padding: 11px 13px; border: 1px solid var(--border-soft);
         border-radius: var(--r-btn); background: var(--surface-2);
         transition: border-color var(--dur) var(--ease),
                     background var(--dur) var(--ease); }
  .opt:hover:not(.gone) { border-color: var(--border-hover); }
  /* Neutral, like every other selected state. The radio's own accent-color
     stays the accent, so the chosen option still carries it -- on the
     control itself rather than across the whole row. */
  .opt.on { border-color: var(--border-hover); background: var(--surface-sel); }
  .opt.gone .nm { color: var(--text-muted); }

  /* A real radio, restyled: keeps native group semantics and arrow-key
     navigation instead of faking a group out of divs. */
  input[type="radio"] { flex: none; width: 14px; height: 14px; margin: 2px 0 0;
                        accent-color: var(--accent); cursor: default; }
  .txt { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
  .nm { font-size: 13.5px; font-weight: 600; color: var(--text);
        overflow-wrap: anywhere; }
  .sub { font-size: 12px; color: var(--text-dim); line-height: 1.5;
         overflow-wrap: anywhere; }
  code { font-family: var(--font-mono); font-size: 11px; }
  /* The same dim-plus-coral badge the project and engine rows use. */
  .badge { margin-left: 7px; font-family: var(--font-mono); font-size: 11px;
           font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent);
           border-radius: 3px; padding: 0 4px; }
</style>
