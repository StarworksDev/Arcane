<script lang="ts">
  import ProjectMenu, { type ProjectActions } from "$lib/components/ProjectMenu.svelte";
  import Icon from "$lib/components/Icon.svelte";
  import { coverFor, engineChipText, engineChipTitle, compatibilityNote,
           missingNote, projectDir, since, launchLabel } from "$lib/format";
  import type { RecentProject } from "$lib/api";

  let { project, compatible, engineAbi, engineLabel, pinned, dangling,
        running = false, cover = undefined, disabled = false, hasEngine = true,
        confirmDelete, actions }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      /** Build name of the engine that will actually launch this project. */
      engineLabel: string;
      /** An explicit per-project choice is in effect. */
      pinned: boolean;
      /** Pinned to an engine that is no longer registered. */
      dangling: boolean;
      /** A live editor has this project open; launching focuses it. */
      running?: boolean;
      /**
       * Cover thumbnail as a data URL (Unreal's chain: <Name>.png beside the
       * manifest, else the editor's Saved/AutoScreenshot.png). Replaces the
       * monogram when present -- a screenshot IS the project's identity, and
       * two identity marks on one tile would compete.
       */
      cover?: string;
      /** An action is running: every control here waits for it. */
      disabled?: boolean;
      /**
       * Some engine will open this project. Gates ONLY launching and the
       * engine band (the picker has nothing to offer without an engine); the
       * action menu stays open to a user who has not registered one yet.
       */
      hasEngine?: boolean;
      confirmDelete: boolean;
      /** The whole per-project vocabulary -- see ProjectActions in ProjectMenu. */
      actions: ProjectActions;
    } = $props();

  const mono = $derived(coverFor(project.name, project.path));
  // The recorded path stopped resolving. Everything that acts on the folder
  // (launch, the engine band) disables; the menu shrinks to Locate/Remove.
  const gone = $derived(project.missing);
  // The same folder line the list row shows. A tile that named the project and
  // nothing else was the one place in the Hub where you could not tell two
  // same-named projects apart.
  const dir = $derived(projectDir(project.path));

  // Shared with ProjectRow via format.ts: the two layouts must say the same
  // thing about the same project, and this copy states facts about the system,
  // so it is tested rather than retyped per layout.
  const engineText = $derived(engineChipText(engineLabel, pinned, dangling));
  const engineTitle = $derived(engineChipTitle(engineLabel, pinned, dangling));
  const why = $derived(
    compatibilityNote(compatible, project.path, project.engineAbi, engineLabel, engineAbi),
  );
  const launchName = $derived(
    launchLabel(project.name, { missing: gone, running, compatible, hasEngine }),
  );

  // Right-click opens the SAME menu the ellipsis does, at the cursor.
  let menu: ProjectMenu;
</script>

<!-- The tile carries EXACTLY what the list row carries -- name, folder, opened,
     engine, abi -- stacked instead of in columns. They are two views of one
     record, so a fact that only appears in one of them is a reason to pick a
     layout for the wrong reason.
     Three distinct state classes, not one `bad`: `incompat` (card, wrong ABI),
     `gone` (card, path no longer on disk) and `missing` (engine chip, dangling
     pin) are independent failure states, and one class name meaning two things
     in one file is a trap for the next edit. -->
<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- Right-click is a redundant MOUSE affordance for a menu already sitting on
     a focusable button on this card, so a keyboard user loses nothing and there
     is no role that means "card you may right-click". Same reasoning as the
     modal scrim and WindowChrome's double-click-to-maximize. -->
<div class="card" class:incompat={!compatible && !gone} class:gone
     oncontextmenu={(e) => { e.preventDefault(); menu.openAt(e.clientX, e.clientY); }}>
  {#if cover}
    <!-- The screenshot band. alt="" -- decorative to a screen reader, since
         the name is text right below; the launch button carries the label. -->
    <img class="shot" src={cover} alt="" draggable="false" />
  {/if}
  <div class="top">
    <!-- The accessible name carries the state (launchLabel): a screen reader
         does not read the title of a labelled button, so "missing" or "needs
         a different engine" would otherwise only exist for mouse users. -->
    <button class="hit" type="button" disabled={disabled || gone || !hasEngine}
            onclick={() => actions.launch(project)}
            title={gone ? missingNote(project.path)
                        : !hasEngine ? "Register an engine on the Engines page to launch this."
                        : running ? `${project.name} is open — this focuses its window.`
                        : why} aria-label={launchName}>
      <!-- The monogram, at BADGE scale -- the fallback identity when no
           screenshot exists yet. (It was once a 76px gradient band; shrunk
           because a gradient is decoration. The .shot band above is NOT that
           regression: a screenshot is information about the project.) Hidden
           when a cover renders, because two identity marks on one tile
           compete. -->
      {#if !cover}
        <span class="mono" style="--a: {mono.angle}deg" aria-hidden="true">{mono.monogram}</span>
      {/if}
      <span class="txt">
        <!-- A title on each line: the button's own title explains launching,
             so a name or folder cut short needs its own way to be read whole. -->
        <span class="nm" title={project.name}>{project.name}</span>
        <span class="path" title={dir}>{dir}</span>
      </span>
    </button>

    <!-- Same reveal contract as the tile's menu: hidden until the card is
         hovered, except a STARRED star stays visible -- it is information
         then, not just a control. Not gated on `disabled`/`gone`: starred is
         Hub state, not a fact about the project's disk, and guard() already
         drops mid-action clicks. -->
    <button class="star" type="button" class:on={project.favorite}
            onclick={() => actions.toggleFavorite(project)}
            aria-pressed={project.favorite}
            title={project.favorite ? "Unfavorite" : "Favorite"}
            aria-label="{project.favorite ? 'Unfavorite' : 'Favorite'} {project.name}">
      <Icon name="star" size={13} />
    </button>

    <!-- The SAME action menu the list row uses, so both layouts offer the same
         things. In flow at the end of the header rather than floating over the
         card: absolutely positioned, it sat on top of whatever the name line
         needed, and the name is the one thing a tile must always show. -->
    <ProjectMenu bind:this={menu} {project} {disabled} {running} {confirmDelete} {actions} />
  </div>

  <!-- The lowercase "abi 59" here is a CODE-STYLE token, set in mono like the
       value it echoes; prose everywhere else says "ABI". Deliberate, not
       drift. -->
  <div class="meta">
    {#if gone}
      <!-- The badge slot: being gone supersedes any ABI statement, because the
           number came from a manifest that is not there to disagree with. -->
      <span class="badge">missing</span>
    {:else if compatible}
      <span>abi {project.engineAbi ? project.engineAbi : "?"}</span>
    {:else}
      <span class="badge">abi {project.engineAbi}</span>
    {/if}
    <!-- Same substitution the list row makes: while an editor has this open,
         "when was it last live" is answered by NOW, not a relative time. -->
    {#if running}
      <span class="run" title="{project.name} is open — launching focuses its window.">
        <span class="dot" aria-hidden="true"></span>running</span>
    {:else}
      <span>{since(project.lastOpenedUtc)}</span>
    {/if}
  </div>

  <!-- A SIBLING of .hit, not a child: nesting an interactive control inside a
       button is invalid HTML and browsers do not deliver its clicks reliably.
       Behind a hairline because it is a different action from the rest of the
       card -- everything above launches, this changes what launches it. -->
  <button class="eng" type="button" disabled={disabled || gone || !hasEngine}
          onclick={() => actions.changeEngine(project)}
          class:pin={pinned} class:missing={dangling} title={engineTitle}
          aria-label="Engine for {project.name}: {engineText}">
    <!-- A CSS dot, not a glyph: hollow = following the default, filled =
         an explicit choice. Drawn rather than typed so it cannot depend on
         a symbol font and stays crisp at this size. -->
    <span class="mark" class:filled={pinned || dangling} aria-hidden="true"></span>
    <span class="lbl">{engineText}</span>
  </button>
</div>

<style>
  /* A column whose meta row is pushed to the bottom: the grid stretches every
     tile in a row to the tallest, and with names now wrapping to two lines a
     one-line tile's meta and engine band floated up, out of line with its
     neighbours'. */
  .card { display: flex; flex-direction: column; }
  .card > .meta { margin-top: auto; }
  .card { position: relative; border: 1px solid var(--border-soft);
          border-radius: var(--r-panel); overflow: hidden; background: var(--surface);
          transition: border-color var(--dur) var(--ease),
                      background var(--dur) var(--ease); }
  /* A surface wash as well as the border, matching the list row and the nav.
     Border-only was the app's hover idiom before the monochrome pass; every
     other hoverable surface has since moved to lightening. */
  .card:hover { border-color: var(--border-hover);
                background: var(--surface-hover); }

  .top { display: flex; align-items: flex-start; gap: 10px; padding: 11px 11px 0 13px; }
  .hit { flex: 1; min-width: 0; display: flex; align-items: center; gap: 10px;
         background: none; border: 0; padding: 0; text-align: left;
         font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  /* The WHOLE tile launches. The launch button itself is just the name line,
     and once the cover band arrived most of the card's surface -- the 118px
     image, the meta row, the padding -- was a dead zone, which read as
     "clicking tiles sometimes does nothing" (user report 2026-07-29). The
     stretched-button pattern: the card is already the positioning context,
     the button projects a full-card hit surface, and the genuinely separate
     controls (star, menu, engine band) sit a layer above it. Scoped to
     :not(:disabled) because a disabled button swallows ALL pointer events:
     a stretched disabled overlay would eat the right-click menu, which on a
     missing card is the only affordance left. */
  .hit:not(:disabled)::after { content: ""; position: absolute; inset: 0; }
  .star, .eng { position: relative; z-index: 1; }
  .top :global(.dots) { position: relative; z-index: 1; }

  .mono { flex: none; width: 40px; height: 40px; display: grid; place-items: center;
          border-radius: 6px; font-family: var(--font-display); font-size: 17px;
          color: var(--cover-ink);
          background: linear-gradient(var(--a), var(--cover-from), var(--cover-to)); }

  /* Same type as the list row's name cell, deliberately: one record, two
     layouts, and a project should not change weight or colour between them. */
  .txt { min-width: 0; display: flex; flex-direction: column; gap: 1px; }
  /* Up to TWO lines, then an ellipsis. At the 230px track minimum the name has
     about 90px beside the monogram, star and menu, and one line cut
     "Reference Project" to "Reference P..." (user report 2026-10-13).
     Wrapping beats shrinking the type: a tile whose name is smaller than its
     neighbour's reads as broken. The path stays one line -- it is secondary,
     and its title has the rest. */
  .nm { max-width: 100%; font-size: 14px; font-weight: 600; color: var(--text);
        line-height: 1.3; overflow: hidden; overflow-wrap: anywhere;
        display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2;
        line-clamp: 2; }
  .path { max-width: 100%; font-family: var(--font-mono); font-size: 11.5px;
          color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  .meta { display: flex; justify-content: space-between; align-items: center;
          gap: 8px; padding: 9px 13px 10px; font-family: var(--font-mono);
          font-size: 11px; color: var(--text-dim); }

  /* 16:9-ish at the 230px track minimum. object-fit: cover because the
     editor's shot follows the viewport's aspect, not the card's; display:
     block kills the baseline gap under a bare img. Dimmed with the rest of
     the card in its inert states. */
  .shot { display: block; width: 100%; height: 118px; object-fit: cover;
          border-bottom: 1px solid var(--border-soft); user-select: none; }
  .card.incompat .shot, .card.gone .shot { opacity: .35; filter: grayscale(.6); }

  /* The ok green the engine status dot uses -- "alive" has one colour in this
     app -- with the dot doubling the signal for anyone who cannot read hue. */
  .run { display: flex; align-items: center; gap: 5px; color: var(--ok); }
  .dot { flex: none; width: 6px; height: 6px; border-radius: 50%;
         background: currentColor; }

  /* Same reveal-and-fill treatment as the list row's star (see ProjectRow):
     invisible until the card is hovered, always on once starred. */
  .star { flex: none; display: grid; place-items: center; width: 22px; height: 22px;
          padding: 0; background: none; border: 0; border-radius: 4px;
          color: var(--text-dim); opacity: 0; cursor: default;
          transition: opacity var(--dur) var(--ease), color var(--dur) var(--ease); }
  .card:hover .star, .star:focus-visible { opacity: 1; }
  .star:hover { color: var(--text); }
  .star.on { opacity: 1; color: var(--text-bright); }
  .star.on :global(svg) { fill: currentColor; }

  /* INERT: surfaces drop back, name recedes to muted (5.9:1). The tile CAN dim
     its surface where the flat list row cannot -- there the surface carries
     hover and a permanent tint would swallow it.
     `.card.incompat` rather than a bare class on purpose: the base `.card` rule
     sets the same two properties at equal specificity, so a bare one would win
     only by declaration order. This rule is absolute in the spec, so it should
     not depend on where it sits in the file.
     `.gone` shares every dim rule: a project that is not on disk is inert for a
     different reason but to the same degree, and two dim treatments would read
     as a third state that does not exist. */
  .card.incompat, .card.gone { background: var(--surface-2); border-color: var(--border); }
  .card.incompat:hover, .card.gone:hover {
    background: var(--surface-2-hover); }
  .card.incompat .mono, .card.gone .mono {
    background: linear-gradient(var(--a), var(--border-soft), var(--bg-bottom));
    color: var(--text-dim); }
  .card.incompat .nm, .card.gone .nm { color: var(--text-muted); }
  /* The app-wide focus ring is the ACCENT (theme.css `:focus-visible`), which
     would paint the act colour onto an incompatible card the instant it is
     tabbed to -- a case no automated gate catches, because it only exists in
     the focus state. Coral is the card's own state colour and measures 8.2:1
     against this surface, so the ring stays plainly visible; only its hue
     changes. Since the re-hue that shift is subtler (both are reds) but it
     still keeps the act colour off a card that cannot be acted on. */
  .card.incompat :focus-visible, .card.gone :focus-visible { outline-color: var(--fail); }
  /* Second, non-chromatic signal: a bordered badge (coral is 8.2:1 here). */
  .card.incompat .badge, .card.gone .badge { font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent);
           border-radius: 3px; padding: 0 4px; }

  /* Its own band behind a hairline, the separator idiom the list and the
     settings sheet both use for "a different kind of thing starts here". */
  .eng { display: flex; align-items: center; gap: 7px; width: 100%;
         padding: 9px 13px; background: none; border: 0;
         border-top: 1px solid var(--border-soft); text-align: left;
         font: inherit; font-size: 11.5px; color: var(--text-dim);
         cursor: default;
         transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .eng:hover:not(:disabled) { color: var(--text-muted);
                              background: var(--hover-wash); }
  .eng:disabled { opacity: .5; }
  /* Inset ring: the band runs edge to edge inside a card that clips its
     contents (overflow:hidden, for the rounded cover), so the global 2px
     OUTSIDE offset drew the ring where it was cut away on three sides. */
  .eng:focus-visible { outline-offset: -2px; border-radius: 0 0 var(--r-panel) var(--r-panel); }
  .mark { flex: none; width: 7px; height: 7px; border-radius: 50%;
          border: 1.5px solid currentColor; }
  .mark.filled { background: currentColor; }
  .lbl { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  /* Pinned reads as deliberate, not as a warning -- no accent, which means act. */
  .eng.pin { color: var(--text-muted); }
  .eng.missing { color: var(--fail); }
</style>
