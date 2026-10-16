<script lang="ts">
  import type { Snippet } from "svelte";
  import type { HTMLButtonAttributes } from "svelte/elements";
  // `primary` is the single primary action per view; `ghost` is everything
  // secondary; `danger` is a destructive action sitting among ordinary ones and
  // is only coloured on hover, so a list of rows is not a wall of red;
  // `destructive` is the FILLED form, for the confirm button of a dialog whose
  // whole purpose is the destruction -- there the colour is the point, and a
  // quiet control would understate what pressing it does.
  //
  // `rest` passes ARIA through (aria-haspopup/aria-expanded for a menu-opening
  // button) -- added for the Add dropdown, whose trigger must speak the menu
  // pattern without rebuilding this component's styling by hand.
  let {
    variant = "ghost",
    disabled = false,
    title = "",
    onclick,
    children,
    ...rest
  }: {
    variant?: "primary" | "ghost" | "danger" | "destructive";
    disabled?: boolean;
    title?: string;
    onclick?: () => void;
    children: Snippet;
  } & Omit<HTMLButtonAttributes, "disabled" | "title" | "onclick"> = $props();
</script>

<button {...rest} class="btn {variant}" type="button" {disabled} {title} {onclick}>{@render children()}</button>

<style>
  .btn {
    font: inherit; font-size: 13px; font-weight: 600; cursor: default;
    /* 7px vertical, not the 10px this shipped with: with 13px type that made
       a ~40px control, tall against the text beside it (user call
       2026-07-29). ~32px now sits flush with the toolbar's 30px pills. */
    border-radius: var(--r-btn); padding: 7px 16px; border: 1px solid transparent;
    transition: background var(--dur) var(--ease), border-color var(--dur) var(--ease),
                color var(--dur) var(--ease);
  }
  .btn:disabled { opacity: .4; }

  /* White ink, not the near-black the gold fill used: this accent is dark, so
     the two are the other way round. */
  .primary { background: var(--accent); color: var(--accent-ink); }        /* 6.1:1 */
  .primary:hover:not(:disabled) { background: var(--accent-hover); }      /* 5.0:1 */

  /* The ghost's RESTING fill is the plain wash and its hover the strong one:
     it reads as a control at rest, so it starts a step above a bare surface
     and hovering takes the next step. */
  .ghost { background: var(--hover-wash); color: var(--text);
           border-color: var(--border); }
  .ghost:hover:not(:disabled) { background: var(--hover-wash-strong);
                                border-color: var(--border-hover); }

  .danger { background: none; color: var(--text-dim); }
  .danger:hover:not(:disabled) { background: color-mix(in srgb, var(--fail-accent) 12%, transparent);
                                 color: var(--fail); }

  /* Near-black ink, not white: --fail-accent is a LIGHT coral (luminance .35),
     so it is the mirror of --accent -- white on it measures 2.6:1 and fails,
     while this reads 7.4:1. The same inversion the accent tokens document. */
  .destructive { background: var(--fail-accent); color: #1a0508; }
  .destructive:hover:not(:disabled) { background: var(--fail); }
</style>
