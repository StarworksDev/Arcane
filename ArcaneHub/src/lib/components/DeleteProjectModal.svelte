<script lang="ts">
  import Modal from "$lib/components/Modal.svelte";
  import Button from "$lib/components/Button.svelte";
  import { projectDir } from "$lib/format";
  import type { RecentProject } from "$lib/api";

  let { project, busy, error, onDelete, onCancel }: {
    project: RecentProject;
    busy: boolean;
    /** The last guard() failure, shown here because the banner is under the scrim. */
    error: string;
    onDelete: () => Promise<boolean>;
    onCancel: () => void;
  } = $props();

  const dir = $derived(projectDir(project.path));

  const submit = () => { if (!busy) onDelete(); };
</script>

<Modal title="Delete project" onClose={onCancel}>
  <p class="lead">
    <strong>{project.name}</strong> and everything inside its folder will be
    deleted &mdash; scenes, assets, source, and any local history that is not
    committed somewhere else.
  </p>

  <!-- The exact folder, in full. This dialog is the last thing standing between
       a mis-click and a project, so it has to show precisely what goes. -->
  <div class="f">
    <span class="label">Folder</span>
    <p class="path">{dir}</p>
  </div>

  <!-- Said plainly rather than buried: it is the difference between a bad
       afternoon and a lost one, and the user should know it before pressing
       the button, not after. -->
  <p class="note">
    It goes to the Recycle Bin, so it can be restored from there &mdash; until
    the bin is emptied. Close the project in the editor first; Windows will not
    delete a folder that is in use.
  </p>

  {#if error}
    <p class="banner error" role="alert">{error}</p>
  {/if}

  {#snippet footer()}
    <Button onclick={onCancel} disabled={busy}>Cancel</Button>
    <Button variant="destructive" onclick={submit} disabled={busy}>Delete project</Button>
  {/snippet}
</Modal>

<style>
  /* overflow-wrap: the project name is user text with no guaranteed break
     point, and one long unbroken name made the whole dialog scroll sideways. */
  .lead { font-size: 13px; color: var(--text); margin: 0; line-height: 1.55;
          overflow-wrap: anywhere; }
  strong { font-weight: 600; color: var(--text-bright); }

  .f { display: flex; flex-direction: column; gap: 5px; }
  .path { font-family: var(--font-mono); font-size: 11.5px; color: var(--text-muted);
          background: var(--well); border: 1px solid var(--border-soft);
          border-radius: var(--r-btn); padding: 7px 9px; margin: 0;
          overflow-wrap: anywhere; user-select: text; }

  .note { font-size: 12px; color: var(--text-dim); margin: 0; line-height: 1.55; }
</style>
