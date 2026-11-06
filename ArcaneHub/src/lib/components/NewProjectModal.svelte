<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import { projectNameError, projectPathPreview } from "$lib/format";
  import type { EngineEntry } from "$lib/api";

  let { busy, error, engine, defaultDir, onBrowse, onCreate, onCancel }: {
    busy: boolean;
    /** The last guard() failure, shown here because the banner is under the scrim. */
    error: string;
    engine: EngineEntry | null;
    defaultDir: string;
    onBrowse: () => Promise<string | null>;
    onCreate: (name: string, dir: string) => Promise<boolean>;
    onCancel: () => void;
  } = $props();

  let name = $state("");
  // Seeded once, deliberately: the dialog is mounted fresh each time it opens
  // (+page renders it under `{#if modal.kind === "new"}`), and once open the field is the user's to edit -- a later
  // settings change must not yank the folder out from under them. Settings is
  // unreachable while the modal is up, so "later" only means the next open.
  // svelte-ignore state_referenced_locally
  let dir = $state(defaultDir);
  // Errors stay hidden until the field has been used or Create was pressed, so
  // the dialog does not open already complaining about an empty name.
  let touched = $state(false);

  const nameError = $derived(projectNameError(name));
  const preview = $derived(projectPathPreview(dir, name));
  const ready = $derived(!nameError && dir.trim() !== "");

  async function browse() {
    const picked = await onBrowse();
    if (picked !== null) dir = picked;
  }

  async function submit() {
    touched = true;
    if (!ready || busy) return;
    // The parent closes the modal on success; on failure it stays open with
    // the typed values intact so the error banner can be acted on.
    await onCreate(name.trim(), dir.trim());
  }
</script>

<Modal title="New project" onClose={onCancel}>
  <label class="f">
    <span class="label">Name</span>
    <input class="field" bind:value={name} spellcheck="false" placeholder="MyGame"
           oninput={() => (touched = true)}
           onkeydown={(e) => e.key === "Enter" && submit()} />
  </label>
  {#if touched && nameError}
    <p class="err" role="alert">{nameError}</p>
  {/if}

  <!-- A div, not a <label>: the row also holds the Browse button, and a label
       wrapping two controls names neither reliably. The input is named by
       aria-labelledby instead, so it does not announce as an unnamed field. -->
  <div class="f">
    <span class="label" id="new-project-location">Location</span>
    <div class="row">
      <input class="field" bind:value={dir} spellcheck="false" placeholder="Choose a folder"
             aria-labelledby="new-project-location"
             onkeydown={(e) => e.key === "Enter" && submit()} />
      <Button onclick={browse} disabled={busy}>Browse&hellip;</Button>
    </div>
  </div>

  <!-- Always rendered so the dialog does not change height as you type. -->
  <p class="preview" class:dim={!preview}>
    {preview || "The project folder and its .arcproj appear here."}
  </p>

  {#if error}
    <p class="banner error" role="alert">{error}</p>
  {/if}

  {#if engine}
    <p class="note">
      Stamped with <strong>{engine.build}</strong> (ABI {engine.engineAbi}), the
      default engine (chosen in Engines).
    </p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <Button variant="primary" onclick={submit} disabled={busy || !ready}>Create</Button>
  {/snippet}
</Modal>

<style>
  .f { display: flex; flex-direction: column; gap: 5px; }
  .row { display: flex; gap: 8px; }
  .row input { flex: 1; min-width: 0; }

  /* --fail, not --fail-accent: this is an inline field note, not the banner. */
  .err { color: var(--fail); font-size: 12.5px; margin: -6px 0 0; }
  .preview { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-muted);
             background: var(--well); border: 1px solid var(--border-soft);
             border-radius: var(--r-btn); padding: 7px 9px; margin: 2px 0 0;
             overflow-wrap: anywhere; user-select: text; }
  .preview.dim { color: var(--text-dim); font-family: var(--font-ui); }
  .note { font-size: 12.5px; color: var(--text-dim); margin: 0; }
  strong { color: var(--text-muted); font-weight: 600; }
</style>
