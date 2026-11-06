<script module lang="ts">
  // Unique per instance so two modals can never emit the same DOM id. A
  // hardcoded id was safe only as long as nothing opened two at once, which
  // nothing enforced.
  let seq = 0;
</script>

<script lang="ts">
  import { onMount, type Snippet } from "svelte";
  import Icon from "$lib/components/Icon.svelte";

  // Generic modal shell: scrim, panel, header, and the keyboard contract.
  // Content and footer are snippets so the shell owns none of the form logic.
  //
  // Two sizes, one shell. `dialog` is the centred panel sized to its content;
  // `full` is the near-full-screen sheet Settings uses. They differ only in
  // measurements -- the scrim, the focus trap, the Escape contract and the
  // labelled-dialog wiring are the parts worth having once, and a second
  // component would have had to reimplement all four.
  let { title, size = "dialog", onClose, children, footer, aside }: {
    title: string;
    size?: "dialog" | "full";
    onClose: () => void;
    children: Snippet;
    footer?: Snippet;
    /** Rendered beside the title -- the version chip on the Settings sheet. */
    aside?: Snippet;
  } = $props();

  // The corner X is the FULL variant's only visible dismissal: it carries no
  // footer, so without it the sheet is closable by Escape and the scrim alone,
  // neither of which is visible. The dialog variant already ends in a Cancel
  // button, and a second dismissal in its corner is noise at that size.
  const closeMark = $derived(size === "full");

  const titleId = `modal-title-${seq++}`;

  // A PLAIN variable, not $state: nothing needs to re-render when it is
  // assigned, and making it reactive is what previously dragged `panel` into
  // the focus effect's dependency set -- so the effect ran twice (once before
  // bind:this, once after) and its cleanup fired a focus round-trip in
  // between, re-capturing the restore target on the way through.
  let panel: HTMLElement | null = null;

  const FOCUSABLE =
    'button:not(:disabled), input:not(:disabled), select:not(:disabled), ' +
    'textarea:not(:disabled), [href], [tabindex]:not([tabindex="-1"])';

  const focusables = () =>
    panel ? [...panel.querySelectorAll<HTMLElement>(FOCUSABLE)] : [];

  // onMount, not $effect: this is a once-after-mount action with an
  // on-unmount undo, which is exactly onMount's contract. It runs after
  // bind:this has assigned `panel`, and it cannot re-enter.
  onMount(() => {
    // Whatever had focus when the modal opened. Without restoring it,
    // dismissing drops focus to <body> and a keyboard user loses their place.
    const opener = document.activeElement as HTMLElement | null;
    focusables()[0]?.focus();
    return () => opener?.focus();
  });

  function onkeydown(e: KeyboardEvent) {
    if (e.key === "Escape") {
      e.preventDefault();
      onClose();
      return;
    }
    if (e.key !== "Tab") return;

    // Trap. A dialog you can Tab out of is modal in appearance only: focus
    // lands on the cards behind the scrim, which cannot be seen or clicked.
    const f = focusables();
    if (f.length === 0) return;
    const first = f[0];
    const last = f[f.length - 1];
    const active = document.activeElement;
    const outside = !panel?.contains(active);

    if (e.shiftKey && (active === first || outside)) {
      e.preventDefault();
      last.focus();
    } else if (!e.shiftKey && (active === last || outside)) {
      e.preventDefault();
      first.focus();
    }
  }
</script>

<svelte:window {onkeydown} />

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- svelte-ignore a11y_click_events_have_key_events -->
<!-- The scrim is a redundant MOUSE dismissal for an action already on Escape
     and on a focusable control (Cancel in the dialog footer, the X in the
     sheet's header), so a keyboard user loses nothing and there is no sensible
     role for "the area outside the dialog". Same reasoning as WindowChrome's
     double-click-to-maximize. -->
<div class="scrim" onclick={onClose}></div>

<!-- class:full rather than `class="wrap {size}"`: Svelte can see a class:
     directive when it checks which style rules are reachable, but not a class
     baked into an interpolated string, and it prunes the .full rules as unused
     if it cannot. -->
<div class="wrap" class:full={size === "full"}>
  <div class="panel" class:full={size === "full"} bind:this={panel}
       role="dialog" aria-modal="true" aria-labelledby={titleId}>
    <header class="head">
      <h2 class="display" id={titleId}>{title}</h2>
      {#if aside}{@render aside()}{/if}
      {#if closeMark}
        <button class="x" type="button" aria-label="Close" onclick={onClose}>
          <Icon name="x" size={20} />
        </button>
      {/if}
    </header>
    <div class="body">{@render children()}</div>
    {#if footer}<div class="foot">{@render footer()}</div>{/if}
  </div>
</div>

<style>
  /* FULL WINDOW, titlebar included. A dialog is a takeover: dimming everything
     except a strip across the top left the chrome looking like it belonged to
     something else, and a panel centred in the remainder sat visibly low.
     The trade is that the Hub owns minimize/maximize/close and the drag region
     (decorations:false), so none of them can be clicked while a dialog is up.
     That is deliberate and it is why every dialog carries its own dismissal --
     Escape plus a Cancel button or the sheet's X -- and why Tab is trapped in
     the panel rather than allowed to wander into unreachable chrome. */
  .scrim { position: fixed; inset: 0; z-index: 20;
           background: rgba(0, 0, 0, .6); backdrop-filter: blur(2px);
           animation: fade var(--dur) var(--ease); }
  .wrap { position: fixed; inset: 0; z-index: 21; display: grid;
          place-items: center; padding: 24px; pointer-events: none; }
  /* No padding for the sheet: it is sized as a share of the window instead, and
     padding here would shrink the grid area those percentages resolve against
     -- 90% of (window - 32px) rather than 90% of the window. */
  .wrap.full { padding: 0; }

  .panel { pointer-events: auto; width: min(470px, 100%);
           max-height: calc(100vh - 48px); overflow-y: auto;
           background: var(--surface); border: 1px solid var(--border);
           border-radius: var(--r-panel); padding: 18px 20px 16px;
           box-shadow: var(--shadow-dialog);
           animation: rise var(--dur) var(--ease); }
  /* The sheet fills the wrap and does NOT scroll as a whole: its header stays
     put and whatever it contains owns the scrollbar. Scrolling the panel would
     take the title and the close mark off screen. */
  /* 90% of the window in each dimension, centred by the wrap's place-items.
     A SHARE, not the fixed inset this replaced: a constant margin is a
     different-looking margin at every window size -- it read as generous at
     1024x600 and as a hairline maximised. The margin now scales with the
     frame, 5% on each side. */
  .panel.full { width: 90%; height: 90%; max-height: none; padding: 0;
                display: flex; flex-direction: column; overflow: hidden; }

  .head { display: flex; align-items: center; gap: 12px; margin: 0 0 16px; }
  .panel.full .head { flex: none; margin: 0; padding: 13px 16px 13px 20px;
                      border-bottom: 1px solid var(--border-soft); }

  /* Titles carry project names ("Engine for <name>"), which are user text with
     no guaranteed break point: one long unbroken name scrolled the whole
     dialog sideways. min-width: 0 lets the flex item shrink so it can wrap. */
  h2 { font-size: 17px; margin: 0; color: var(--text-bright); min-width: 0;
       overflow-wrap: anywhere; }
  .panel.full h2 { font-size: 23px; }

  /* Pushed right by margin-auto on the button, not by a spacer, so `aside`
     stays next to the title where the reference puts its version chip. */
  .x { margin-left: auto; flex: none; width: 34px; height: 34px;
       display: grid; place-items: center; background: none; border: 0;
       border-radius: 6px; color: var(--text-dim); cursor: default;
       transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .x:hover { background: var(--hover-wash-strong); color: var(--text); }

  .body { display: flex; flex-direction: column; gap: 13px; }
  /* One stretched cell: the sheet's content is a full-height layout of its own,
     not a stack of fields, so it gets the space rather than a column gap. */
  .panel.full .body { display: grid; flex: 1; min-height: 0; gap: 0; }

  .foot { display: flex; justify-content: flex-end; gap: 8px; margin-top: 20px; }

  @keyframes fade { from { opacity: 0 } }
  @keyframes rise { from { opacity: 0; transform: translateY(6px) } }
</style>
