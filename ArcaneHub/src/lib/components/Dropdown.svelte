<script module lang="ts">
  // One popover vocabulary for every dropdown-shaped control (the Add button,
  // the grid sorter) -- extracted from ProjectsView when the sorter's native
  // <select> was replaced: the OS popup could only be color-scheme'd into
  // cooperation, never styled to match, and two inline copies of the same
  // mechanics in one file is how they drift apart.
  export type DropdownItem = {
    label: string;
    /** The current choice in a pick-one menu -- checked and announced. When
     * any item carries this, every item reserves the check slot so labels
     * stay aligned, and the whole menu speaks menuitemradio. */
    active?: boolean;
    disabled?: boolean;
    title?: string;
    onselect: () => void;
  };
</script>

<script lang="ts">
  import type { Snippet } from "svelte";
  import Icon from "$lib/components/Icon.svelte";
  import { firstEnabled, menuKey } from "$lib/menuKeys";

  // The trigger is a snippet, not a baked-in button: Add wants a full ghost
  // Button, the sorter wants a bare pill-resident control, and both must
  // carry the menu-pattern ARIA -- so the snippet receives {open, toggle,
  // aria} and spreads them onto whatever it renders.
  let { items, ariaLabel, width = 200, trigger }: {
    items: DropdownItem[];
    ariaLabel: string;
    /** Menu width in px; the default matches ProjectMenu's proportions. */
    width?: number;
    trigger: Snippet<[{ open: boolean; toggle: () => void; aria: Record<string, unknown> }]>;
  } = $props();

  let open = $state(false);
  let wrap = $state<HTMLElement | null>(null);

  const radio = $derived(items.some((i) => i.active !== undefined));

  const buttons = () =>
    wrap ? [...wrap.querySelectorAll<HTMLButtonElement>(".menu button")] : [];
  // aria-disabled, not the `disabled` attribute: a disabled button receives
  // no mouse events, so its title -- the item's explanation of WHY it is
  // unavailable -- never showed. The keyboard helper skips these instead.
  const disabledFlags = (f: HTMLButtonElement[]) =>
    f.map((b) => b.getAttribute("aria-disabled") === "true");

  // First ENABLED item, not first item: a disabled one cannot be chosen.
  $effect(() => {
    if (!open) return;
    const f = buttons();
    f[firstEnabled(disabledFlags(f))]?.focus();
  });

  const triggerEl = () => wrap?.querySelector<HTMLElement>("button") ?? null;

  function close(restoreFocus = true) {
    if (!open) return;
    open = false;
    if (restoreFocus) triggerEl()?.focus();
  }

  function choose(it: DropdownItem) {
    if (it.disabled) return;
    // Close FIRST, same rule as ProjectMenu: an action may open a dialog, and
    // a popover left behind the scrim catches clicks the user cannot see.
    open = false;
    // Focus the trigger BEFORE dispatching: the item is about to unmount, and
    // a dialog the action opens records whatever had focus as the place to
    // return to -- without this that was <body>, and closing the dialog
    // dropped a keyboard user at the top of the window.
    triggerEl()?.focus();
    it.onselect();
  }

  // The shared menu keys (lib/menuKeys.ts) -- the same rules as ProjectMenu.
  function onMenuKeys(e: KeyboardEvent) {
    const f = buttons();
    if (f.length === 0) return;
    const act = menuKey(e.key, f.indexOf(document.activeElement as HTMLButtonElement),
                        disabledFlags(f));
    if (!act) return;
    e.preventDefault();
    if ("close" in act) close();
    else f[act.focus].focus();
  }

  function onWindowKey(e: KeyboardEvent) {
    if (open && e.key === "Escape") {
      e.preventDefault();
      close();
    }
  }

  function onWindowPointerDown(e: PointerEvent) {
    if (open && !wrap?.contains(e.target as Node)) close(false);
  }
</script>

<svelte:window onkeydown={onWindowKey} onpointerdown={onWindowPointerDown} />

<div class="wrap" bind:this={wrap}>
  {@render trigger({
    open,
    toggle: () => (open ? close() : (open = true)),
    aria: { "aria-haspopup": "menu", "aria-expanded": open },
  })}
  {#if open}
    <div class="menu" role="menu" tabindex="-1" aria-label={ariaLabel}
         onkeydown={onMenuKeys} style="width:{width}px">
      {#each items as it (it.label)}
        <button class="item" type="button" tabindex="-1"
                role={radio ? "menuitemradio" : "menuitem"}
                aria-checked={radio ? !!it.active : undefined}
                aria-disabled={it.disabled ? "true" : undefined} title={it.title}
                onclick={() => choose(it)}>
          {#if radio}<span class="slot">{#if it.active}<Icon name="check" size={13} />{/if}</span>{/if}
          {it.label}
        </button>
      {/each}
    </div>
  {/if}
</div>

<style>
  /* inline-flex, not block: the wrapper sits in toolbar rows and pills, and
     must shrink-wrap its trigger rather than take the row's width. */
  .wrap { position: relative; display: inline-flex; }

  /* Absolute under the trigger, not ProjectMenu's fixed layer: every call
     site lives in chrome that neither clips nor scrolls, so the anchor
     cannot go stale. Left edges aligned, dropdown-button style (user call
     2026-07-29). Surface tokens match ProjectMenu's .menu so the popovers
     read as one species. */
  .menu { position: absolute; z-index: 30; top: calc(100% + 4px); left: 0;
          padding: 5px;
          background: var(--surface); border: 1px solid var(--border);
          border-radius: var(--r-panel);
          box-shadow: var(--shadow-pop);
          animation: rise var(--dur) var(--ease); }

  .item { display: flex; align-items: center; gap: 7px; width: 100%;
          text-align: left; font: inherit; font-size: 13px; padding: 8px 10px;
          border: 0; border-radius: 5px; background: none; color: var(--text);
          cursor: default; transition: background var(--dur) var(--ease); }
  .item:hover:not([aria-disabled="true"]) { background: var(--hover-wash); }
  /* Present but honest -- the title says what to do about it. Dim by colour
     alone, not opacity, so the label still clears AA (--text-dim). */
  .item[aria-disabled="true"] { color: var(--text-dim); }

  /* Reserved on every item of a pick-one menu, so the labels align whether
     or not the check is there. */
  .slot { width: 14px; display: grid; place-items: center; flex: none; }

  @keyframes rise { from { opacity: 0; transform: translateY(-4px) } }
</style>
