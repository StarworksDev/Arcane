import { describe, it, expect } from "vitest";
import { menuKey, firstEnabled } from "./menuKeys";

describe("menuKey", () => {
  const none = [false, false, false, false];
  it("Down and Up move and wrap", () => {
    expect(menuKey("ArrowDown", 0, none)).toEqual({ focus: 1 });
    expect(menuKey("ArrowDown", 3, none)).toEqual({ focus: 0 });
    expect(menuKey("ArrowUp", 0, none)).toEqual({ focus: 3 });
    expect(menuKey("ArrowUp", 2, none)).toEqual({ focus: 1 });
  });
  it("Down from nothing focused lands on the first item, Up on the last", () => {
    expect(menuKey("ArrowDown", -1, none)).toEqual({ focus: 0 });
    expect(menuKey("ArrowUp", -1, none)).toEqual({ focus: 3 });
  });
  it("Home and End go to the ends", () => {
    expect(menuKey("Home", 2, none)).toEqual({ focus: 0 });
    expect(menuKey("End", 0, none)).toEqual({ focus: 3 });
  });
  it("skips disabled items in every direction", () => {
    const d = [true, false, true, false, true];
    expect(menuKey("ArrowDown", 1, d)).toEqual({ focus: 3 });
    expect(menuKey("ArrowDown", 3, d)).toEqual({ focus: 1 });
    expect(menuKey("ArrowUp", 1, d)).toEqual({ focus: 3 });
    expect(menuKey("Home", 3, d)).toEqual({ focus: 1 });
    expect(menuKey("End", 1, d)).toEqual({ focus: 3 });
  });
  it("does not get stuck when the focused item is the only enabled one", () => {
    expect(menuKey("ArrowDown", 1, [true, false, true])).toEqual({ focus: 1 });
  });
  it("Escape and Tab close the menu", () => {
    expect(menuKey("Escape", 0, none)).toEqual({ close: true });
    expect(menuKey("Tab", 0, none)).toEqual({ close: true });
  });
  it("movement does nothing when every item is disabled", () => {
    expect(menuKey("ArrowDown", -1, [true, true])).toBeNull();
    expect(menuKey("Home", -1, [true, true])).toBeNull();
  });
  it("other keys are not the menu's", () => {
    expect(menuKey("a", 0, none)).toBeNull();
    expect(menuKey("Enter", 0, none)).toBeNull();
  });
});

describe("firstEnabled", () => {
  it("finds the first enabled item, or -1", () => {
    expect(firstEnabled([true, false, false])).toBe(1);
    expect(firstEnabled([true, true])).toBe(-1);
    expect(firstEnabled([])).toBe(-1);
  });
});
