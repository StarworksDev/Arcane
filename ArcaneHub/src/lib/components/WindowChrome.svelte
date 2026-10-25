<script lang="ts">
  import { getCurrentWindow } from "@tauri-apps/api/window";
  // decorations:false means we own minimize/maximize/close and the drag region.
  // Buttons are children WITHOUT data-tauri-drag-region, which is how Tauri's
  // handler knows not to start a drag from them.
  const win = getCurrentWindow();
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- The drag region is chrome, not a control. Double-click-to-maximize is a
     redundant MOUSE convenience for an action already exposed on the focusable
     Maximize button below, so there is nothing a keyboard user loses and no
     sensible ARIA role for "titlebar". Svelte's rule cannot see that the
     behaviour is duplicated, hence the explicit suppression. -->
<div class="chrome" data-tauri-drag-region ondblclick={() => win.toggleMaximize()}>
  <img class="mark" src="/logo.png" alt="" />
  <span class="title display">Arcane Hub</span>
  <div class="ctrls">
    <!-- 12px glyphs in a 12-unit viewBox: Windows draws its own caption marks
         at 10-12px, so anything smaller reads as a scaled-down imitation. -->
    <button class="ctrl" type="button" aria-label="Minimize" onclick={() => win.minimize()}>
      <svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true">
        <rect x="1" y="5.5" width="10" height="1" fill="currentColor" />
      </svg>
    </button>
    <button class="ctrl" type="button" aria-label="Maximize" onclick={() => win.toggleMaximize()}>
      <svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true">
        <rect x="1.5" y="1.5" width="9" height="9" fill="none"
              stroke="currentColor" stroke-width="1" />
      </svg>
    </button>
    <button class="ctrl close" type="button" aria-label="Close" onclick={() => win.close()}>
      <svg width="12" height="12" viewBox="0 0 12 12" aria-hidden="true">
        <path d="M1.5 1.5l9 9M10.5 1.5l-9 9" stroke="currentColor" stroke-width="1.2" />
      </svg>
    </button>
  </div>
  <div class="rule"></div>
</div>

<style>
  /* No right padding: the caption buttons run to the window edge, exactly as
     native ones do -- a mouse thrown at the corner lands on Close. */
  .chrome { position: sticky; top: 0; z-index: 10; height: var(--h-titlebar); flex: none;
            display: flex; align-items: center; gap: 11px; padding: 0 0 0 16px; }
  /* pointer-events:none so the logo and title are part of the drag region
     rather than dead spots in the middle of it. */
  .mark { width: 20px; height: 20px; object-fit: contain; pointer-events: none; }
  .title { font-size: 12.5px; letter-spacing: .17em; color: var(--text-muted);
           pointer-events: none; }
  /* Stretched to the titlebar's full height, no gaps: native caption buttons
     are abutting full-bleed rectangles, and the hover wash filling the whole
     corner is what makes them read as such. */
  .ctrls { margin-left: auto; align-self: stretch; display: flex; }
  /* 46px wide is the native Windows caption-button metric; the height is the
     titlebar's own (user call 2026-07-29: full squares, not the floating
     rounded 46x32 rects this replaced). */
  .ctrl { width: 46px; height: 100%; display: grid; place-items: center;
          background: transparent; border: 0; border-radius: 0;
          color: var(--text-dim); cursor: default;
          transition: background var(--dur) var(--ease), color var(--dur) var(--ease); }
  .ctrl:hover { background: var(--hover-wash-strong); color: var(--text); }
  .close:hover { background: color-mix(in srgb, var(--fail-accent) 85%, transparent); color: #fff; }
  /* Neutral hairline. This was a gold gradient spanning the full titlebar --
     a permanent band of colour across the top of every screen. */
  .rule { position: absolute; left: 0; right: 0; bottom: 0; height: 1px;
          background: linear-gradient(90deg, transparent, var(--border), transparent); }
</style>
