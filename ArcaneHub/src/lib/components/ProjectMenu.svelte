<script module lang="ts">
  import type { RecentProject } from "$lib/api";

  // The per-project action vocabulary, defined WHERE the menu is: this
  // component owns "what can you do to a project", so the callback contract
  // lives beside the item list it feeds. ONE object instead of eight props --
  // the chain (+page -> ProjectsView -> Card/Row -> here) was four layers of
  // hand-forwarded callbacks, and extending it meant touching every layer
  // (the Locate... addition missed two destructures on its first pass, which
  // is exactly the drift this collapses away).
  export type ProjectActions = {
    launch: (p: RecentProject) => void;
    /** Star/unstar. Not a menu item -- the star control on card and row IS
     * the affordance; this rides the vocabulary so both layouts share it. */
    toggleFavorite: (p: RecentProject) => void;
    /** Open the pin-an-engine picker for this project. */
    changeEngine: (p: RecentProject) => void;
    reveal: (p: RecentProject) => void;
    rename: (p: RecentProject) => void;
    /** Copy the project on disk (minus build output and .git) and list it. */
    duplicate: (p: RecentProject) => void;
    args: (p: RecentProject) => void;
    /** Remove from the LIST only -- `delete` is the one that touches disk. */
    forget: (p: RecentProject) => void;
    /** Repoint a MISSING project at its moved .arcproj. */
    locate: (p: RecentProject) => void;
    delete: (p: RecentProject) => void;
  };
</script>

<script lang="ts">
  import { tick } from "svelte";
  import Icon from "$lib/components/Icon.svelte";
  import { menuItemsFor, menuPlacement, type MenuItem } from "$lib/format";
  import { firstEnabled, menuKey } from "$lib/menuKeys";

  // The per-project action menu, shared by the tile and the list row. It owns
  // the popover; the ITEM LIST itself lives in format.ts (menuItemsFor) where
  // it is unit-tested -- "what can you do to a project" is one vocabulary,
  // and it is copy that states facts, so it gets tested like the chip text.
  let { project, disabled = false, running = false, confirmDelete = true, actions }: {
    project: RecentProject;
    /** Only while an action runs. NOT "no engine": no item here launches. */
    disabled?: boolean;
    /** A live editor holds the folder: Rename and Delete say so instead. */
    running?: boolean;
    /** Whether Delete opens a confirmation. Only affects this item's LABEL. */
    confirmDelete?: boolean;
    actions: ProjectActions;
  } = $props();

  const items = $derived(menuItemsFor(project.missing, confirmDelete, running));

  let open = $state(false);
  let trigger: HTMLButtonElement;
  let menu = $state<HTMLElement | null>(null);
  // Set once the menu has rendered and been MEASURED (see place). Hidden until
  // then, so it never flashes at a guessed position first.
  let placement = $state("visibility:hidden;");

  const MENU_W = 236;

  // What the menu is opened FROM: the ellipsis button's box, or a zero-height
  // point at the cursor; `left` is where the menu's left edge wants to be.
  // One routine for both ways in, so they cannot drift apart at the edges.
  let anchor = { left: 0, top: 0, bottom: 0 };

  // Render hidden, measure, place, THEN focus the first enabled item: a
  // visibility:hidden element cannot take focus, so focusing any earlier
  // silently failed and left the arrow keys with nothing to move from.
  async function openFrom(a: typeof anchor) {
    anchor = a;
    placement = "visibility:hidden;";
    open = true;
    await tick();
    place();
    await tick();
    if (open) buttons()[firstEnabled(disabledFlags())]?.focus();
  }

  // Measured, not estimated: the old rule opened downward whenever 200px were
  // free below, and the menu is ~228px, so near the bottom of a short window
  // its last items -- Delete among them -- were cut off. format.menuPlacement
  // flips it above or clamps it, so the whole menu is always in the window.
  function place() {
    if (!menu) return;
    const r = menu.getBoundingClientRect();
    const p = menuPlacement(anchor, { width: r.width, height: r.height },
                            { width: window.innerWidth, height: window.innerHeight });
    placement = `left:${p.left}px; top:${p.top}px;`;
  }

  function show() {
    const r = trigger.getBoundingClientRect();
    // Right-aligned to the trigger: it sits at the right edge of a row, so a
    // left-aligned menu would hang off-screen.
    void openFrom({ left: r.right - MENU_W, top: r.top, bottom: r.bottom });
  }

  /**
   * Open at a point instead of under the trigger -- the right-click path.
   *
   * A component export, reached through `bind:this`, rather than a second menu
   * built in the card: "mirrors the ellipsis exactly" is only true if it IS the
   * ellipsis menu, sharing one item list and one set of keyboard rules.
   *
   * Top-left at the cursor, which is what every desktop context menu does.
   * Honours `disabled` exactly as the ellipsis does: a right-click mid-action
   * used to open the menu the button beside it refused.
   */
  export function openAt(x: number, y: number) {
    if (disabled) return;
    void openFrom({ left: x, top: y, bottom: y });
  }

  function hide(restoreFocus = true) {
    if (!open) return;
    open = false;
    if (restoreFocus) trigger.focus();
  }

  const buttons = () =>
    menu ? [...menu.querySelectorAll<HTMLButtonElement>("button")] : [];
  // aria-disabled rather than the attribute, so a disabled item still shows
  // its tooltip; the keyboard helper skips them all the same.
  const disabledFlags = () => items.map((it) => !!it.disabled);

  function choose(it: MenuItem) {
    if (it.disabled) return;
    // Close FIRST: several of these open a dialog, and leaving a popover behind
    // the scrim is how a stray click lands on a control the user cannot see.
    open = false;
    // Then focus the trigger, BEFORE dispatching. The focused item unmounts
    // with the menu, and a dialog the action opens records the active element
    // as where to return on close -- which was <body>, so Escape out of Rename
    // left a keyboard user at the top of the window instead of on this row.
    trigger.focus();
    // The item's kind IS a ProjectActions key: dispatch by lookup, so there is
    // no hand-written mapping to fall out of step with the vocabulary.
    actions[it.kind](project);
  }

  // The shared menu keys (lib/menuKeys.ts): Up/Down/Home/End skipping
  // disabled items, Escape and Tab closing. Tab dismisses rather than trapping
  // -- this is a menu, not a dialog -- and hide() puts focus back on the
  // TRIGGER, because the items unmount on this very keystroke and the default
  // would drop the user at the top of the document.
  function onMenuKeys(e: KeyboardEvent) {
    const f = buttons();
    if (f.length === 0) return;
    const act = menuKey(e.key, f.indexOf(document.activeElement as HTMLButtonElement),
                        disabledFlags());
    if (!act) return;
    e.preventDefault();
    if ("close" in act) hide();
    else f[act.focus].focus();
  }

  function onWindowKey(e: KeyboardEvent) {
    if (open && e.key === "Escape") {
      e.preventDefault();
      hide();
    }
  }

  // pointerdown, not click: the menu must be gone before whatever was clicked
  // reacts. The trigger is excluded so its own click can toggle rather than
  // close-then-reopen.
  function onPointerDown(e: PointerEvent) {
    if (!open) return;
    const t = e.target as Node;
    if (trigger.contains(t) || menu?.contains(t)) return;
    hide(false);
  }
</script>

<svelte:window onkeydown={onWindowKey} onpointerdown={onPointerDown}
               onresize={() => hide(false)} onscrollcapture={() => hide(false)} />

<button class="dots" type="button" {disabled} bind:this={trigger}
        aria-haspopup="menu" aria-expanded={open}
        aria-label="Actions for {project.name}"
        title="Project actions"
        onclick={() => (open ? hide() : show())}>
  <Icon name="ellipsis" size={16} />
</button>

{#if open}
  <!-- position:fixed in a top-level layer, NOT absolute inside the card: the
       tile clips its own content (overflow:hidden, for the cover's rounded
       corners), so an absolutely positioned menu would be cut off at the card
       edge. Fixed also means the menu cannot widen a grid track. -->
  <!-- tabindex="-1" on both levels is the ARIA menu pattern: the container is
       programmatically focusable but out of the tab order, and the items are
       reached with the arrow keys (roving focus) rather than by tabbing
       through every one of them. -->
  <!-- The popover renders INSIDE the card/row that owns it, so a right-click on
       a menu item would otherwise bubble to that card's own contextmenu handler
       and yank the open menu over to the cursor. Swallowed here: preventDefault
       as well as stopPropagation, because stopping the bubble also stops the
       window-level handler that suppresses Chromium's menu. -->
  <div class="menu" role="menu" tabindex="-1" bind:this={menu} style={placement}
       aria-label="Actions for {project.name}" onkeydown={onMenuKeys}
       oncontextmenu={(e) => { e.preventDefault(); e.stopPropagation(); }}>
    {#each items as it (it.label)}
      <!-- A disabled item keeps its place and states its reason on a second
           line: a greyed entry with no explanation is a dead end, and a
           tooltip alone never reaches the keyboard. -->
      <button class="item" class:danger={it.danger} class:sep={it.sep}
              type="button" role="menuitem" tabindex="-1"
              aria-disabled={it.disabled ? "true" : undefined}
              title={it.disabled} onclick={() => choose(it)}>
        {it.label}
        {#if it.disabled}<span class="why">{it.disabled}</span>{/if}
      </button>
    {/each}
  </div>
{/if}

<style>
  .dots { width: 26px; height: 26px; display: grid; place-items: center;
          border: 0; border-radius: 4px; background: none; color: var(--text-dim);
          cursor: default;
          transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .dots:hover:not(:disabled), .dots[aria-expanded="true"] {
    color: var(--text); background: var(--hover-wash-strong); }
  .dots:disabled { opacity: .5; }

  /* max-height + scroll for the one case placement cannot fix: a window
     shorter than the menu itself. */
  .menu { position: fixed; z-index: 30; width: 236px; padding: 5px;
          max-height: calc(100vh - 16px); overflow-y: auto;
          background: var(--surface); border: 1px solid var(--border);
          border-radius: var(--r-panel);
          box-shadow: var(--shadow-pop);
          animation: rise var(--dur) var(--ease); }

  .item { display: block; width: 100%; text-align: left; font: inherit;
          font-size: 13px; padding: 8px 10px; border: 0; border-radius: 5px;
          background: none; color: var(--text); cursor: default;
          transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .item:hover:not([aria-disabled="true"]) { background: var(--hover-wash); }
  /* Dimmed by colour, not opacity, so the label and its reason still clear
     AA (--text-dim). */
  .item[aria-disabled="true"] { color: var(--text-dim); }
  .why { display: block; font-size: 11.5px; color: var(--text-dim); margin-top: 1px; }
  /* The divider lives on the item that OPENS the group, not on `danger`: the
     top corners square off so the hover wash does not float rounded against
     the line it sits under. */
  .sep { margin-top: 4px; border-top: 1px solid var(--border-soft);
         border-radius: 0 0 5px 5px; }
  /* Coloured on hover only, so an open menu is not a wall of red for an action
     that only edits a list. Matches Button's `danger` variant. */
  .danger { color: var(--text-muted); }
  .danger:hover:not([aria-disabled="true"]) {
    background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
    color: var(--fail); }
  .danger[aria-disabled="true"] { color: var(--text-dim); }

  @keyframes rise { from { opacity: 0; transform: translateY(-4px) } }
</style>
