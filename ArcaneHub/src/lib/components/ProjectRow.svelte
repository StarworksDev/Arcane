<script lang="ts">
  import ProjectMenu, { type ProjectActions } from "$lib/components/ProjectMenu.svelte";
  import Icon from "$lib/components/Icon.svelte";
  import { engineChipText, engineChipTitle, compatibilityNote, missingNote,
           projectDir, since, launchLabel } from "$lib/format";
  import type { RecentProject } from "$lib/api";

  // IDENTICAL prop contract to ProjectCard, so ProjectsView can swap the two
  // without a second set of wiring.
  //
  // A TABLE ROW, not a card: one grid cell per column, on the track list shared
  // with the header strip in ProjectsView. The cover art the tile carries is
  // deliberately absent -- at row scale a monogram is decoration that pushes
  // every other column right, and what makes a list scannable is the meta
  // lining up down the page, not each row being individually recognisable.
  let { project, compatible, engineAbi, engineLabel, pinned, dangling,
        running = false, disabled = false, hasEngine = true, confirmDelete, actions }:
    {
      project: RecentProject; compatible: boolean; engineAbi: number | null;
      engineLabel: string; pinned: boolean; dangling: boolean;
      /** A live editor has this project open; launching focuses it. */
      running?: boolean;
      /** An action is running: every control here waits for it. */
      disabled?: boolean;
      /** Some engine will open this. Gates launching and the engine chip only. */
      hasEngine?: boolean;
      confirmDelete: boolean;
      /** The whole per-project vocabulary -- see ProjectActions in ProjectMenu. */
      actions: ProjectActions;
    } = $props();

  // The FOLDER, not the .arcproj inside it. The file name only ever repeats the
  // project name already in the row above it, and where the project lives is
  // the thing this line is for.
  const dir = $derived(projectDir(project.path));
  // The recorded path stopped resolving. Everything that acts on the folder
  // (launch, the engine chip) disables; the menu shrinks to Locate/Remove.
  const gone = $derived(project.missing);
  const engineText = $derived(engineChipText(engineLabel, pinned, dangling));
  const engineTitle = $derived(engineChipTitle(engineLabel, pinned, dangling));
  const why = $derived(
    compatibilityNote(compatible, project.path, project.engineAbi, engineLabel, engineAbi),
  );
  // The state lives in the accessible name, as on the tile (see launchLabel).
  const launchName = $derived(
    launchLabel(project.name, { missing: gone, running, compatible, hasEngine }),
  );

  // Right-click opens the SAME menu the ellipsis does, at the cursor.
  let menu: ProjectMenu;
</script>

<!-- svelte-ignore a11y_no_static_element_interactions -->
<!-- Right-click is a redundant MOUSE affordance for a menu already sitting on
     a focusable button in this row, so a keyboard user loses nothing and there
     is no role that means "row you may right-click". Same reasoning as the
     modal scrim and WindowChrome's double-click-to-maximize. -->
<div class="row" class:incompat={!compatible && !gone} class:gone
     oncontextmenu={(e) => { e.preventDefault(); menu.openAt(e.clientX, e.clientY); }}>
  <!-- First column so the stars line up down the page, the way a favorites
       column reads in every list that has one. Enabled even on a gone row
       (starred is Hub state, not a disk fact) and not gated on `disabled`
       either -- guard()'s re-entry gate already drops a mid-action click,
       and a star that dims with the launch controls would look related to
       launchability, which it is not. -->
  <button class="star" type="button" class:on={project.favorite}
          onclick={() => actions.toggleFavorite(project)}
          aria-pressed={project.favorite}
          title={project.favorite ? "Unfavorite" : "Favorite"}
          aria-label="{project.favorite ? 'Unfavorite' : 'Favorite'} {project.name}">
    <Icon name="star" size={13} />
  </button>

  <button class="hit" type="button" disabled={disabled || gone || !hasEngine}
          onclick={() => actions.launch(project)}
          title={gone ? missingNote(project.path)
                      : !hasEngine ? "Register an engine on the Engines page to launch this."
                      : running ? `${project.name} is open — this focuses its window.`
                      : why} aria-label={launchName}>
    <!-- A title on each line, as on the tile: the button's own title explains
         launching, so a name or folder cut short needs its own. -->
    <span class="nm" title={project.name}>{project.name}</span>
    <span class="path" title={dir}>{dir}</span>
  </button>

  <!-- The Opened cell answers "when was this last live". While an editor has
       it open the honest answer is NOW, so `running` replaces the relative
       time rather than sitting beside it as one more chip. -->
  {#if running}
    <span class="when run" title="{project.name} is open — launching focuses its window.">
      <span class="dot" aria-hidden="true"></span>running</span>
  {:else}
    <span class="when">{since(project.lastOpenedUtc)}</span>
  {/if}

  <!-- Sibling of .hit, not nested: an interactive control inside a button is
       invalid HTML and its clicks are not reliably delivered. -->
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

  {#if gone}
    <!-- The badge slot: being gone supersedes any ABI statement, because the
         number came from a manifest that is not there to disagree with.
         Lowercase "abi 59" is a deliberate code-style token (mono, like the
         value); prose says "ABI". -->
    <code class="abi badge">missing</code>
  {:else if compatible}
    <code class="abi">abi {project.engineAbi ? project.engineAbi : "?"}</code>
  {:else}
    <code class="abi badge">abi {project.engineAbi}</code>
  {/if}

  <ProjectMenu bind:this={menu} {project} {disabled} {running} {confirmDelete} {actions} />
</div>

<style>
  /* No card: no border box, no radius, no gap between rows. A separator
     between neighbours and a hover wash is what a list of records looks like;
     bordered pills stacked 7px apart read as a column of small panels.
     The separator itself is drawn by the PARENT (.list in ProjectsView): a
     `.row + .row` rule here matches nothing, because from inside the component
     `.row` is the root and it has no sibling to pair with -- Svelte's scoped
     CSS analysis sees that and drops the rule. */
  .row { display: grid; grid-template-columns: var(--cols-project);
         gap: var(--gap-project); align-items: center; padding: 11px 12px;
         transition: background var(--dur) var(--ease); }
  .row:hover { background: var(--hover-wash); }

  /* The name cell IS the launch target -- the whole two-line stack, so the
     click area matches what reads as "the project". */
  .hit { min-width: 0; display: flex; flex-direction: column; align-items: flex-start;
         gap: 1px; background: none; border: 0; padding: 0; text-align: left;
         font: inherit; color: inherit; cursor: default; }
  .hit:disabled { opacity: .5; }

  .nm { max-width: 100%; font-size: 14px; font-weight: 600; color: var(--text);
        overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .path { max-width: 100%; font-family: var(--font-mono); font-size: 11.5px;
          color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  /* Left-aligned, like every other column: a right-aligned cell in the middle
     of a table only lines up with itself. */
  .abi { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim);
         overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .when { font-family: var(--font-mono); font-size: 11px; color: var(--text-dim);
          overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  /* The ok green the engine status dot already uses -- "alive" has one colour
     in this app. The dot doubles the signal for anyone who cannot read the
     hue. */
  .when.run { display: flex; align-items: center; gap: 5px; color: var(--ok); }
  .dot { flex: none; width: 6px; height: 6px; border-radius: 50%;
         background: currentColor; }

  /* Negative inline margin so the button's own padding does not indent its
     label past the column it sits in: the hover surface should overhang the
     track, the text should not. */
  .eng { min-width: 0; margin: 0 -8px; display: flex; align-items: center; gap: 7px;
         background: none; border: 0; padding: 6px 8px; border-radius: 5px;
         font: inherit; font-size: 11.5px; color: var(--text-dim); cursor: default;
         transition: color var(--dur) var(--ease), background var(--dur) var(--ease); }
  .eng:hover:not(:disabled) { color: var(--text-muted); background: var(--hover-wash); }
  .eng:disabled { opacity: .5; }
  .mark { flex: none; width: 7px; height: 7px; border-radius: 50%;
          border: 1.5px solid currentColor; }
  .mark.filled { background: currentColor; }
  .lbl { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .eng.pin { color: var(--text-muted); }
  .eng.missing { color: var(--fail); }

  /* Dimmed name plus a coral badge, the same pair the tile uses. The tile also
     dims its SURFACE; a flat row cannot, because the surface is what carries
     hover here and a permanent tint would swallow it. `.gone` (project not on
     disk) shares the treatment for the same reason the tile does: equally
     inert, and a second dim would read as a third state. */
  .row.incompat .nm, .row.gone .nm { color: var(--text-muted); }
  .row.incompat :focus-visible, .row.gone :focus-visible { outline-color: var(--fail); }
  .row.incompat .badge, .row.gone .badge { font-weight: 600; color: var(--fail);
           border: 1px solid color-mix(in srgb, var(--fail) 45%, transparent);
           border-radius: 3px; padding: 0 4px; }

  /* The action menu occupies the last column. It is always visible here, unlike
     the tile's hover-reveal: a row has room, and a control that appears only on
     hover is undiscoverable in a dense list. */

  /* Unstarred is BARELY there until the row is hovered -- a column of dim
     outlines would be noise on a list where most rows are not favourites --
     but starred stays on at full presence, because the star then carries
     meaning. Filled via CSS so one icon path serves both states. */
  .star { display: grid; place-items: center; width: 20px; height: 20px;
          margin: 0 -2px; padding: 0; background: none; border: 0;
          border-radius: 4px; color: var(--text-dim); opacity: 0; cursor: default;
          transition: opacity var(--dur) var(--ease), color var(--dur) var(--ease); }
  .row:hover .star, .star:focus-visible { opacity: 1; }
  .star:hover { color: var(--text); }
  .star.on { opacity: 1; color: var(--text-bright); }
  .star.on :global(svg) { fill: currentColor; }
</style>
