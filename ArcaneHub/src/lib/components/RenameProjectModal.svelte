<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import { projectDir, projectNameError } from "$lib/format";
  import type { RecentProject } from "$lib/api";

  let { project, busy, error, onRename, onCancel }: {
    project: RecentProject;
    busy: boolean;
    /** The last guard() failure, shown here because the banner is under the scrim. */
    error: string;
    onRename: (newName: string) => Promise<boolean>;
    onCancel: () => void;
  } = $props();

  // Seeded once: the dialog is mounted fresh per project, and from then on the
  // field is the user's to edit.
  // svelte-ignore state_referenced_locally
  let name = $state(project.name);
  let touched = $state(false);

  const dir = $derived(projectDir(project.path));
  const parent = $derived(dir.replace(/[/\\][^/\\]+$/, ""));
  const sep = $derived(dir.includes("\\") ? "\\" : "/");

  const nameError = $derived(projectNameError(name));
  const changed = $derived(name.trim() !== project.name);
  const ready = $derived(!nameError && changed);

  // What the folder and the manifest will be called. Shown rather than
  // described, because this moves a directory and the user should see where.
  const preview = $derived.by(() => {
    const n = name.trim();
    if (!n || nameError) return "";
    return `${parent}${sep}${n}${sep}${n}.arcproj`;
  });

  async function submit() {
    touched = true;
    if (!ready || busy) return;
    await onRename(name.trim());
  }
</script>

<Modal title="Rename project" onClose={onCancel}>
  <label class="f">
    <span class="label">Name</span>
    <input class="field" bind:value={name} spellcheck="false"
           oninput={() => (touched = true)}
           onkeydown={(e) => e.key === "Enter" && submit()} />
  </label>
  {#if touched && nameError}
    <p class="err" role="alert">{nameError}</p>
  {/if}

  <div class="f">
    <span class="label">Becomes</span>
    <p class="preview" class:dim={!preview}>
      {preview || "The renamed folder and its .arcproj appear here."}
    </p>
  </div>

  <!-- The honest limits, stated up front rather than discovered afterwards.
       Everything the engine resolves is relative to the project root or keyed
       by GUID, so renaming does not break content; a game module is build
       output named by the project's own build scripts, and the Hub has no
       business rewriting those. -->
  <p class="note">
    Renames the folder, the <code>.arcproj</code> and the name inside it, and
    updates this list. Scenes, assets and their GUIDs are unaffected.
  </p>
  <p class="note">
    Not changed: <code>gameModule</code>, anything under <code>Source/</code>,
    and generated solution files. Close the project in the editor first &mdash;
    Windows will not rename a folder that is in use.
  </p>

  {#if error}
    <p class="banner error" role="alert">{error}</p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <Button variant="primary" onclick={submit} disabled={busy || !ready}>Rename</Button>
  {/snippet}
</Modal>

<style>
  .f { display: flex; flex-direction: column; gap: 5px; }
  .err { color: var(--fail); font-size: 12.5px; margin: -6px 0 0; }
  .preview { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-muted);
             background: var(--well); border: 1px solid var(--border-soft);
             border-radius: var(--r-btn); padding: 7px 9px; margin: 0;
             overflow-wrap: anywhere; user-select: text; }
  .preview.dim { color: var(--text-dim); font-family: var(--font-ui); }

  .note { font-size: 12px; color: var(--text-dim); margin: 0; line-height: 1.55; }
  code { font-family: var(--font-mono); font-size: 11px; color: var(--text-muted); }

</style>
