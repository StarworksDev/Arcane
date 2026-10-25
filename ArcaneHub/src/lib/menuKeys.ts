// Keyboard rules for the Hub's two popover menus (ProjectMenu and Dropdown).
// They used to carry their own copies of these rules, and the copies drifted:
// one had Home/End and the other did not, and neither knew about disabled
// items -- Dropdown's arrow keys stopped dead on a disabled "Add project…"
// because focusing a disabled button does nothing. One pure function, tested,
// used by both.

/** What a keypress asks the menu to do: move focus, close, or nothing. */
export type MenuKeyAction = { focus: number } | { close: true } | null;

/** Index of the first item that can take focus, or -1 if none can. */
export function firstEnabled(disabled: readonly boolean[]): number {
  return disabled.findIndex((d) => !d);
}

function lastEnabled(disabled: readonly boolean[]): number {
  for (let i = disabled.length - 1; i >= 0; i--) if (!disabled[i]) return i;
  return -1;
}

// The next enabled index from `from` in direction `step`, wrapping. Starting
// from -1 (nothing focused) works for both directions: +1 lands on 0, and -1
// lands on the last item, the same as a fresh Up does in a native menu.
function stepFrom(from: number, step: 1 | -1, disabled: readonly boolean[]): number {
  const n = disabled.length;
  let i = from;
  for (let k = 0; k < n; k++) {
    i = (((i + step) % n) + n) % n;
    if (!disabled[i]) return i;
  }
  return -1;
}

/**
 * The ARIA menu pattern's keys: Up/Down move (wrapping, skipping disabled
 * items), Home/End jump to the first/last enabled item, and Escape/Tab close.
 * Tab closes rather than moving on because this is a menu, not a dialog --
 * the caller puts focus back on the trigger, since the items unmount on the
 * same keystroke and focus would otherwise fall to the top of the document.
 *
 * Enter and Space are deliberately not handled: the items are real buttons,
 * which already activate on both.
 */
export function menuKey(key: string, current: number, disabled: readonly boolean[]): MenuKeyAction {
  let target: number;
  switch (key) {
    case "Escape":
    case "Tab":
      return { close: true };
    case "ArrowDown":
      target = stepFrom(current, 1, disabled);
      break;
    case "ArrowUp":
      target = stepFrom(current < 0 ? disabled.length : current, -1, disabled);
      break;
    case "Home":
      target = firstEnabled(disabled);
      break;
    case "End":
      target = lastEnabled(disabled);
      break;
    default:
      return null;
  }
  return target < 0 ? null : { focus: target };
}
