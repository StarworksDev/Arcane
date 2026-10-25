import { describe, it, expect } from "vitest";
import {
  isCompatible, filterProjects, coverFor, sortProjects, nextSort, normalisePath,
  projectKey, projectDir, projectNameError, projectPathPreview, resolveEngine,
  engineChipText, engineChipTitle, compatibilityNote, missingNote, menuItemsFor,
  since, scanReportLines, projectCountLine, launchLabel, sortHeaderLabel, menuPlacement,
  addNotice, replaceNotices, capNotices, defaultEngineFor, isNewerState,
  type SortKey, type Notice, type NoticeSource,
} from "./format";

describe("isCompatible", () => {
  // Exact parity with the inline rule this replaces (+page.svelte:139):
  //   !selectedEngine || p.engineAbi === 0 || p.engineAbi === selectedEngine.engineAbi
  it("matching abi is compatible", () => {
    expect(isCompatible(7, 7)).toBe(true);
  });
  it("differing abi is not compatible", () => {
    expect(isCompatible(5, 7)).toBe(false);
  });
  it("unknown project abi (0) is treated as compatible", () => {
    // 0 means the manifest did not state one -- we cannot prove a conflict,
    // so we must not brand it broken.
    expect(isCompatible(0, 7)).toBe(true);
  });
  it("no engine selected is compatible", () => {
    // Nothing to conflict with; the UI disables launching separately.
    expect(isCompatible(5, null)).toBe(true);
  });
  it("unknown abi with no engine is compatible", () => {
    expect(isCompatible(0, null)).toBe(true);
  });
});

describe("filterProjects", () => {
  const items = [
    { name: "Aphelyon", path: "D:/dev/starworks/Gacha/Aphelyon.arcproj" },
    { name: "SampleProject", path: "D:/dev/starworks/Arcane/Samples/SampleProject" },
    { name: "OldPrototype", path: "D:/dev/archive/OldPrototype.arcproj" },
  ];
  it("empty query returns everything", () => {
    expect(filterProjects(items, "")).toHaveLength(3);
  });
  it("whitespace-only query returns everything", () => {
    expect(filterProjects(items, "   ")).toHaveLength(3);
  });
  it("matches name case-insensitively", () => {
    expect(filterProjects(items, "aphel").map((i) => i.name)).toEqual(["Aphelyon"]);
  });
  it("matches path as well as name", () => {
    // Typing a folder you remember must find the project.
    expect(filterProjects(items, "archive").map((i) => i.name)).toEqual(["OldPrototype"]);
  });
  it("no match returns empty", () => {
    expect(filterProjects(items, "zzz")).toEqual([]);
  });
  it("does not mutate the input array", () => {
    const copy = [...items];
    filterProjects(items, "aphel");
    expect(items).toEqual(copy);
  });
});

describe("sortProjects", () => {
  // Recency-ordered input, the shape the Rust recents Vec arrives in.
  const p = (name: string, opened: string, abi: number, favorite = false, engine = "E") => ({
    name, lastOpenedUtc: opened, engineAbi: abi, favorite, engine,
  });
  const byEngine = (x: { engine: string }) => x.engine;

  it("favorites sit above the rest whatever the sort", () => {
    const items = [p("B", "300", 2), p("A", "200", 1), p("Z", "100", 3, true)];
    for (const key of ["opened", "name", "abi", "engine"] as SortKey[]) {
      expect(sortProjects(items, key, false, byEngine)[0].name).toBe("Z");
    }
  });
  it("opened compares numerically, not lexicographically", () => {
    // "999" > "1000" as strings -- the trap this test pins.
    const items = [p("Old", "999", 1), p("New", "1000", 1)];
    expect(sortProjects(items, "opened", true, byEngine).map((i) => i.name))
      .toEqual(["New", "Old"]);
  });
  it("a never-opened stamp sorts as oldest", () => {
    const items = [p("Never", "never", 1), p("Once", "50", 1)];
    expect(sortProjects(items, "opened", true, byEngine)[0].name).toBe("Once");
  });
  it("name sorts case-insensitively both directions", () => {
    const items = [p("banana", "1", 1), p("Apple", "2", 1)];
    expect(sortProjects(items, "name", false, byEngine).map((i) => i.name))
      .toEqual(["Apple", "banana"]);
    expect(sortProjects(items, "name", true, byEngine).map((i) => i.name))
      .toEqual(["banana", "Apple"]);
  });
  it("abi sorts numerically", () => {
    const items = [p("Ten", "1", 10), p("Two", "2", 2)];
    expect(sortProjects(items, "abi", false, byEngine).map((i) => i.name))
      .toEqual(["Two", "Ten"]);
  });
  it("engine sorts by the injected label", () => {
    const items = [
      p("A", "1", 1, false, "Zeta build"),
      p("B", "2", 1, false, "Alpha build"),
    ];
    expect(sortProjects(items, "engine", false, byEngine).map((i) => i.name))
      .toEqual(["B", "A"]);
  });
  it("ties keep their incoming recency order", () => {
    const items = [p("First", "9", 5), p("Second", "8", 5)];
    expect(sortProjects(items, "abi", false, byEngine).map((i) => i.name))
      .toEqual(["First", "Second"]);
  });
  it("does not mutate the input array", () => {
    const items = [p("B", "1", 1), p("A", "2", 2)];
    const copy = [...items];
    sortProjects(items, "name", false, byEngine);
    expect(items).toEqual(copy);
  });
});

describe("normalisePath", () => {
  // MIRROR of Rust state::normalise_path, the fold projectKey builds on.
  it("folds backslashes, trailing separators and case", () => {
    expect(normalisePath("D:\\Games\\MyGame\\")).toBe("d:/games/mygame");
    expect(normalisePath("D:/Games/MyGame")).toBe("d:/games/mygame");
    expect(normalisePath("d:/games/MYGAME///")).toBe("d:/games/mygame");
  });
  it("keeps a manifest file distinct from its folder", () => {
    // Unlike projectKey, which folds the manifest away.
    expect(normalisePath("D:/Games/MyGame/MyGame.arcproj"))
      .toBe("d:/games/mygame/mygame.arcproj");
  });
});

describe("nextSort", () => {
  it("clicking the active column flips direction", () => {
    expect(nextSort("name", false, "name")).toEqual({ key: "name", desc: true });
    expect(nextSort("name", true, "name")).toEqual({ key: "name", desc: false });
  });
  it("a new column starts at its natural direction", () => {
    // Opened: newest first. Everything else: ascending.
    expect(nextSort("name", true, "opened")).toEqual({ key: "opened", desc: true });
    expect(nextSort("opened", true, "name")).toEqual({ key: "name", desc: false });
    expect(nextSort("opened", true, "abi")).toEqual({ key: "abi", desc: false });
    expect(nextSort("opened", true, "engine")).toEqual({ key: "engine", desc: false });
  });
});

describe("coverFor", () => {
  it("monogram is the first alphanumeric character, uppercased", () => {
    expect(coverFor("aphelyon", "x").monogram).toBe("A");
    expect(coverFor("  sample", "x").monogram).toBe("S");
    expect(coverFor("3d-demo", "x").monogram).toBe("3");
  });
  it("falls back to ? when there is no alphanumeric character", () => {
    expect(coverFor("---", "x").monogram).toBe("?");
    expect(coverFor("", "x").monogram).toBe("?");
  });
  it("accepts non-Latin letters as the monogram", () => {
    expect(coverFor("守护者", "x").monogram).toBe("守");
    expect(coverFor("éclair", "x").monogram).toBe("É");
  });
  it("still falls back for a name with no letter or digit", () => {
    expect(coverFor("--- ***", "x").monogram).toBe("?");
  });
  it("angle is deterministic for the same path", () => {
    // THE requirement: a card must not change appearance between launches.
    expect(coverFor("A", "D:/one").angle).toBe(coverFor("A", "D:/one").angle);
  });
  it("angle differs for different paths", () => {
    expect(coverFor("A", "D:/one").angle).not.toBe(coverFor("A", "D:/two").angle);
  });
  it("angle stays inside the intended band", () => {
    for (const p of ["D:/a", "D:/b", "D:/c", "D:/dev/x/y", "", "//?/UNC"]) {
      const a = coverFor("N", p).angle;
      expect(a).toBeGreaterThanOrEqual(100);
      expect(a).toBeLessThanOrEqual(200);
    }
  });
  it("angle does not depend on the name", () => {
    expect(coverFor("Zebra", "D:/one").angle).toBe(coverFor("Apple", "D:/one").angle);
  });
  it("a project keeps its cover when recorded as a manifest instead of a folder", () => {
    // Switching the Open dialog to pick the .arcproj re-records existing
    // projects by file path. Their cards must not change appearance.
    expect(coverFor("MyGame", "D:/Games/MyGame/MyGame.arcproj").angle)
      .toBe(coverFor("MyGame", "D:/Games/MyGame").angle);
  });
});

describe("projectDir", () => {
  it("drops the manifest, leaving the folder", () => {
    expect(projectDir("D:\\Games\\MyGame\\MyGame.arcproj")).toBe("D:\\Games\\MyGame");
    expect(projectDir("D:/Games/MyGame/MyGame.arcproj")).toBe("D:/Games/MyGame");
  });
  it("keeps the original spelling, unlike projectKey", () => {
    // projectKey lowercases and rewrites separators because it is an identity.
    // This value is displayed, so it must be the path the user actually has.
    expect(projectDir("D:\\Games\\MyGame\\MyGame.arcproj")).toBe("D:\\Games\\MyGame");
  });
  it("leaves a folder-shaped entry alone", () => {
    // Entries recorded before the dialog asked for a .arcproj are folders.
    expect(projectDir("D:/Games/MyGame")).toBe("D:/Games/MyGame");
  });
  it("ignores a trailing separator", () => {
    expect(projectDir("D:/Games/MyGame/")).toBe("D:/Games/MyGame");
  });
  it("ignores the extension's case", () => {
    expect(projectDir("D:/G/MyGame.ARCPROJ")).toBe("D:/G");
  });
  it("does not strip a folder that merely ends in .arcproj-like text", () => {
    expect(projectDir("D:/Games/MyGame.arcprojects")).toBe("D:/Games/MyGame.arcprojects");
  });
  it("keeps a bare manifest name when there is no folder to show", () => {
    // Returning "" here would render an empty path line, which reads as a bug.
    expect(projectDir("MyGame.arcproj")).toBe("MyGame.arcproj");
  });
});

describe("projectKey", () => {
  it("a manifest path and its folder collapse to one key", () => {
    expect(projectKey("D:/Games/MyGame/MyGame.arcproj")).toBe(projectKey("D:/Games/MyGame"));
  });
  it("folds separators and case the way the Rust side dedupes", () => {
    expect(projectKey("D:\\Games\\MyGame")).toBe(projectKey("d:/games/mygame"));
  });
  it("ignores a trailing separator", () => {
    expect(projectKey("D:/Games/MyGame/")).toBe(projectKey("D:/Games/MyGame"));
  });
  it("keeps distinct projects distinct", () => {
    expect(projectKey("D:/Games/A")).not.toBe(projectKey("D:/Games/B"));
  });
  it("a bare manifest with no folder keeps its own name as the key", () => {
    // Guards the empty-string branch: stripping the only component would
    // collapse every such path to "", giving them all one cover.
    expect(projectKey("/MyGame.arcproj")).toBe("/mygame.arcproj");
  });
  it("does not strip a folder that merely ends in .arcproj-like text", () => {
    expect(projectKey("D:/Games/notarcproj")).toBe("d:/games/notarcproj");
  });
  it("produces the running-editor key shape Rust's project_dir_key sends", () => {
    // running_projects / running-changed carry these keys; the badge matches
    // with projectKey(p.path), whichever shape the entry was recorded in.
    expect(projectKey("D:\\Games\\MyGame\\MyGame.arcproj")).toBe("d:/games/mygame");
    expect(projectKey("D:\\Games\\MyGame\\")).toBe("d:/games/mygame");
  });
});

describe("projectNameError", () => {
  it("accepts ordinary names", () => {
    expect(projectNameError("MyGame")).toBeNull();
    expect(projectNameError("My Game")).toBeNull();
    expect(projectNameError("3d-demo")).toBeNull();
    expect(projectNameError("game_2")).toBeNull();
  });
  it("rejects an empty or whitespace-only name", () => {
    expect(projectNameError("")).toBeTruthy();
    expect(projectNameError("   ")).toBeTruthy();
  });
  it("rejects path separators, which would escape the chosen folder", () => {
    expect(projectNameError("a/b")).toBeTruthy();
    expect(projectNameError("a\\b")).toBeTruthy();
  });
  it("rejects the other characters Windows forbids", () => {
    for (const c of ["<", ">", ":", '"', "|", "?", "*"]) {
      expect(projectNameError(`a${c}b`), `expected "${c}" to be rejected`).toBeTruthy();
    }
  });
  it("rejects a control character", () => {
    expect(projectNameError(`a${String.fromCharCode(7)}b`)).toBeTruthy();
  });
  it("rejects a trailing dot or space, which Windows silently strips", () => {
    // The folder on disk would not match the typed name.
    expect(projectNameError("MyGame.")).toBeTruthy();
    // Note the quoted trailing space survives trim() only inside the string.
    expect(projectNameError("MyGame .")).toBeTruthy();
  });
  it("rejects reserved DOS device names", () => {
    expect(projectNameError("CON")).toBeTruthy();
    expect(projectNameError("nul")).toBeTruthy();
    expect(projectNameError("COM1")).toBeTruthy();
    expect(projectNameError("lpt9.game")).toBeTruthy();
  });
  it("does not reject a name that merely starts with a reserved word", () => {
    expect(projectNameError("Console")).toBeNull();
    expect(projectNameError("Conquest")).toBeNull();
  });
  it("rejects a name long enough to threaten the path limit", () => {
    // The name appears twice in <dir>/<name>/<name>.arcproj.
    expect(projectNameError("a".repeat(65))).toBeTruthy();
    expect(projectNameError("a".repeat(64))).toBeNull();
  });
});

describe("resolveEngine", () => {
  const a = { id: "eng-a", engineAbi: 7 };
  const b = { id: "eng-b", engineAbi: 5 };
  const engines = [a, b];

  it("no pin follows the Hub default", () => {
    expect(resolveEngine(null, engines, a)).toEqual({
      engine: a, pinned: false, dangling: false,
    });
  });
  it("a live pin wins over the default", () => {
    expect(resolveEngine("eng-b", engines, a)).toEqual({
      engine: b, pinned: true, dangling: false,
    });
  });
  it("pinning to what the default already is still counts as pinned", () => {
    // The choice was made explicitly, so changing the default later must not
    // move this project -- the UI has to be able to show that.
    expect(resolveEngine("eng-a", engines, a).pinned).toBe(true);
  });
  it("a pin to an unregistered engine falls back and reports dangling", () => {
    expect(resolveEngine("eng-gone", engines, a)).toEqual({
      engine: a, pinned: false, dangling: true,
    });
  });
  it("no pin and no default resolves to nothing, not a crash", () => {
    expect(resolveEngine(null, [], null)).toEqual({
      engine: null, pinned: false, dangling: false,
    });
  });
  it("a dangling pin with no default still reports dangling", () => {
    expect(resolveEngine("eng-gone", [], null)).toEqual({
      engine: null, pinned: false, dangling: true,
    });
  });
});

describe("project labels", () => {
  it("an unpinned project is shown as following the default", () => {
    expect(engineChipText("Release", false, false)).toBe("Default: Release");
  });
  it("a pinned project shows its own engine, with no 'default' prefix", () => {
    expect(engineChipText("Debug", true, false)).toBe("Debug");
  });
  it("a dangling pin says so instead of naming the fallback", () => {
    // Naming the fallback would read as if the pin resolved.
    expect(engineChipText("Release", false, true)).toBe("Engine missing");
  });
  it("the tooltip distinguishes all three states", () => {
    const dflt = engineChipTitle("Release", false, false);
    const pin = engineChipTitle("Release", true, false);
    const gone = engineChipTitle("Release", false, true);
    expect(new Set([dflt, pin, gone]).size).toBe(3);
    expect(dflt).toContain("default");
    expect(pin).toContain("Always");
    expect(gone).toContain("no longer registered");
  });

  it("a compatible project shows its path", () => {
    expect(compatibilityNote(true, "D:/g/A.arcproj", 7, "Release", 7))
      .toBe("D:/g/A.arcproj");
  });
  it("an incompatible project NAMES the engine rather than saying 'the selected engine'", () => {
    // Regression: engines are per-project, so "the selected engine" described
    // the sidebar rather than the engine that would actually open this.
    const note = compatibilityNote(false, "D:/g/A.arcproj", 5, "Debug 0.1", 7);
    // "ABI" in prose; the lowercase "abi 59" is reserved for the mono chips.
    expect(note).toContain("ABI 5");
    expect(note).toContain("Debug 0.1");
    expect(note).toContain("ABI 7");
    expect(note).not.toContain("the selected engine");
  });

  it("a missing project names its stale path and both ways out", () => {
    // The copy must point at Locate AND Remove: one repairs the row, the other
    // retires it, and a user staring at a greyed project needs to know both.
    const note = missingNote("D:/g/A.arcproj");
    expect(note).toContain("D:/g/A.arcproj");
    expect(note).toContain("Locate");
    expect(note).toContain("Remove from list");
  });
});

describe("menuItemsFor", () => {
  it("a missing project offers exactly repair-or-retire", () => {
    const kinds = menuItemsFor(true, true).map((i) => i.kind);
    expect(kinds).toEqual(["locate", "forget"]);
  });
  it("a healthy project gets the full vocabulary, delete last and danger", () => {
    const items = menuItemsFor(false, true);
    expect(items.map((i) => i.kind))
      .toEqual(["reveal", "rename", "duplicate", "args", "forget", "delete"]);
    expect(items.at(-1)?.danger).toBe(true);
    expect(items.filter((i) => i.danger)).toHaveLength(1);
  });
  it("remove-from-list opens the below-the-line group in both states", () => {
    for (const missing of [true, false]) {
      const items = menuItemsFor(missing, true);
      expect(items.find((i) => i.kind === "forget")?.sep).toBe(true);
      expect(items.filter((i) => i.sep)).toHaveLength(1);
    }
  });
  it("the delete ellipsis follows the confirmation setting", () => {
    // An ellipsis promises a dialog; with confirmation off there is none.
    const label = (confirm: boolean) =>
      menuItemsFor(false, confirm).find((i) => i.kind === "delete")?.label;
    expect(label(true)).toBe("Delete project…");
    expect(label(false)).toBe("Delete project");
  });
});

describe("projectPathPreview", () => {
  it("shows the manifest the Rust side will actually write", () => {
    // create_project does root = dir.join(name), then writes <name>.arcproj.
    expect(projectPathPreview("D:\\Games", "MyGame")).toBe("D:\\Games\\MyGame\\MyGame.arcproj");
  });
  it("keeps forward slashes when that is what the path uses", () => {
    expect(projectPathPreview("D:/Games", "MyGame")).toBe("D:/Games/MyGame/MyGame.arcproj");
  });
  it("does not double a trailing separator", () => {
    expect(projectPathPreview("D:\\Games\\", "MyGame")).toBe("D:\\Games\\MyGame\\MyGame.arcproj");
  });
  it("treats a bare drive as a Windows path", () => {
    expect(projectPathPreview("D:", "MyGame")).toBe("D:\\MyGame\\MyGame.arcproj");
  });
  it("is empty until both halves are known", () => {
    expect(projectPathPreview("", "MyGame")).toBe("");
    expect(projectPathPreview("D:/Games", "")).toBe("");
    expect(projectPathPreview("D:/Games", "   ")).toBe("");
  });
});

describe("since", () => {
  // lastOpenedUtc is SECONDS since the epoch, as a string -- the Rust side's
  // contract. "0" and anything unparseable mean the project was never opened.
  const now = 1_700_000_000_000; // ms
  const ago = (s: number) => String(now / 1000 - s);
  it("reads never-opened stamps as never", () => {
    expect(since("0", now)).toBe("never");
    expect(since("", now)).toBe("never");
    expect(since("garbage", now)).toBe("never");
    expect(since("-5", now)).toBe("never");
  });
  it("says just now under a minute", () => {
    expect(since(ago(0), now)).toBe("just now");
    expect(since(ago(59), now)).toBe("just now");
  });
  it("counts minutes, hours and days", () => {
    expect(since(ago(60), now)).toBe("1m ago");
    expect(since(ago(3599), now)).toBe("59m ago");
    expect(since(ago(3600), now)).toBe("1h ago");
    expect(since(ago(86399), now)).toBe("23h ago");
    expect(since(ago(86400 * 3), now)).toBe("3d ago");
  });
  it("treats a stamp from the future (clock skew) as just now", () => {
    expect(since(ago(-120), now)).toBe("just now");
  });
});

describe("scanReportLines", () => {
  const r = (o: Partial<{ added: number; alreadyListed: number; relocated: number;
                          ambiguous: number; truncated: boolean }> = {}) => ({
    added: 0, alreadyListed: 0, relocated: 0, ambiguous: 0, truncated: false, ...o,
  });
  it("always states what was added, even nothing", () => {
    expect(scanReportLines(r())).toEqual(["Scan added 0 projects."]);
  });
  it("uses the singular for one", () => {
    expect(scanReportLines(r({ added: 1 }))).toEqual(["Scan added 1 project."]);
  });
  it("lists every refusal and heal in one sentence", () => {
    expect(scanReportLines(r({ added: 2, relocated: 1, alreadyListed: 3, ambiguous: 2 })))
      .toEqual(["Scan added 2 projects, relocated 1 moved project, 3 already listed, " +
                "skipped 2 folders holding more than one .arcproj."]);
  });
  it("adds a second line when the walk was cut short", () => {
    const lines = scanReportLines(r({ added: 1, truncated: true }));
    expect(lines).toHaveLength(2);
    expect(lines[1]).toContain("stopped early");
  });
});

describe("projectCountLine", () => {
  it("counts projects with the right plural", () => {
    expect(projectCountLine(0, 0, 0)).toBe("0 projects");
    expect(projectCountLine(1, 0, 0)).toBe("1 project");
    expect(projectCountLine(3, 0, 0)).toBe("3 projects");
  });
  it("adds the incompatible and missing counts only when non-zero", () => {
    expect(projectCountLine(3, 1, 0)).toBe("3 projects · 1 needs a different engine");
    expect(projectCountLine(3, 2, 1)).toBe("3 projects · 2 need a different engine · 1 missing");
    expect(projectCountLine(3, 0, 2)).toBe("3 projects · 2 missing");
  });
});

describe("menuItemsFor while running", () => {
  // Windows refuses to rename or recycle a folder an editor holds open; the
  // menu says so up front instead of letting the action fail with an OS error.
  it("disables rename and delete with the reason", () => {
    const items = menuItemsFor(false, true, true);
    for (const kind of ["rename", "delete"]) {
      expect(items.find((i) => i.kind === kind)?.disabled).toBe("Close the editor first");
    }
  });
  it("leaves the rest enabled", () => {
    const items = menuItemsFor(false, true, true);
    expect(items.filter((i) => i.disabled).map((i) => i.kind)).toEqual(["rename", "delete"]);
  });
  it("nothing is disabled when the project is not running", () => {
    expect(menuItemsFor(false, true, false).some((i) => i.disabled)).toBe(false);
    expect(menuItemsFor(false, true).some((i) => i.disabled)).toBe(false);
  });
});

describe("launchLabel", () => {
  const ok = { missing: false, running: false, compatible: true, hasEngine: true };
  it("names the action and the project in the ordinary case", () => {
    expect(launchLabel("Aphelyon", ok)).toBe("Launch Aphelyon");
  });
  it("states each blocking or special state", () => {
    expect(launchLabel("A", { ...ok, missing: true })).toBe("A, missing");
    expect(launchLabel("A", { ...ok, hasEngine: false })).toBe("A, no engine registered");
    expect(launchLabel("A", { ...ok, compatible: false })).toBe("A, needs a different engine");
    expect(launchLabel("A", { ...ok, running: true })).toBe("A, running: focus its window");
  });
  it("missing outranks everything else", () => {
    expect(launchLabel("A", { missing: true, running: true, compatible: false, hasEngine: false }))
      .toBe("A, missing");
  });
});

describe("sortHeaderLabel", () => {
  it("an inactive column offers to sort by it", () => {
    expect(sortHeaderLabel("Name", false, false)).toBe("Sort by Name");
  });
  it("the active column states its direction", () => {
    expect(sortHeaderLabel("Name", true, true)).toBe("Name, sorted descending");
    expect(sortHeaderLabel("Name", true, false)).toBe("Name, sorted ascending");
  });
});

describe("menuPlacement", () => {
  const view = { width: 800, height: 520 };
  const size = { width: 236, height: 228 };
  it("opens below the anchor when it fits", () => {
    expect(menuPlacement({ left: 100, top: 50, bottom: 76 }, size, view))
      .toEqual({ left: 100, top: 80 });
  });
  it("flips above when there is not room below", () => {
    // 520 - 300 = 220 below: under the 228px menu, so it flips.
    expect(menuPlacement({ left: 100, top: 274, bottom: 300 }, size, view))
      .toEqual({ left: 100, top: 274 - 4 - 228 });
  });
  it("clamps inside the window when it fits neither way", () => {
    const p = menuPlacement({ left: 100, top: 200, bottom: 200 }, size, { width: 800, height: 400 });
    expect(p.top).toBeGreaterThanOrEqual(8);
    expect(p.top + size.height).toBeLessThanOrEqual(400 - 8);
  });
  it("pins to the top edge when the menu is taller than the window", () => {
    expect(menuPlacement({ left: 0, top: 10, bottom: 10 }, size, { width: 800, height: 200 }).top)
      .toBe(8);
  });
  it("keeps the menu off the left and right edges", () => {
    expect(menuPlacement({ left: -50, top: 0, bottom: 0 }, size, view).left).toBe(8);
    expect(menuPlacement({ left: 700, top: 0, bottom: 0 }, size, view).left).toBe(800 - 236 - 8);
  });
});

describe("notices", () => {
  const n = (id: number, source: NoticeSource, text: string): Notice => ({ id, source, text });
  it("addNotice skips text that is already showing", () => {
    const list = [n(1, "launch", "x")];
    expect(addNotice(list, n(2, "launch", "x"))).toEqual(list);
    expect(addNotice(list, n(2, "launch", "y")).map((i) => i.text)).toEqual(["x", "y"]);
  });
  it("a new scan report replaces the previous one, even with the same text", () => {
    // Deduping by text would make a repeated scan look like nothing happened.
    const list = [n(1, "scan", "Scan added 0 projects."), n(2, "load", "reset"),
                  n(3, "scan", "The scan stopped early")];
    const next = replaceNotices(list, "scan", [n(4, "scan", "Scan added 0 projects.")]);
    expect(next.map((i) => i.id)).toEqual([2, 4]);
  });
  it("capNotices shows the newest few and counts the rest", () => {
    const list = [1, 2, 3, 4, 5].map((i) => n(i, "launch", String(i)));
    expect(capNotices(list, 3, false)).toEqual({ shown: list.slice(2), hidden: 2 });
    expect(capNotices(list, 3, true)).toEqual({ shown: list, hidden: 0 });
    expect(capNotices(list.slice(0, 2), 3, false)).toEqual({ shown: list.slice(0, 2), hidden: 0 });
  });
});

describe("defaultEngineFor", () => {
  const a = { id: "a" }, b = { id: "b" };
  it("uses the saved default when it is still registered", () => {
    expect(defaultEngineFor([a, b], "b")).toBe(b);
  });
  it("falls back to the first engine when unset or unregistered", () => {
    expect(defaultEngineFor([a, b], null)).toBe(a);
    expect(defaultEngineFor([a, b], "gone")).toBe(a);
  });
  it("is null with no engines", () => {
    expect(defaultEngineFor([], "a")).toBeNull();
  });
});

describe("isNewerState", () => {
  it("adopts a snapshot at or above the last revision", () => {
    expect(isNewerState(5, 4)).toBe(true);
    expect(isNewerState(5, 5)).toBe(true);
  });
  it("ignores an older snapshot", () => {
    expect(isNewerState(3, 4)).toBe(false);
  });
  it("revision 0 (unknown) is always adopted", () => {
    expect(isNewerState(0, 9)).toBe(true);
    expect(isNewerState(undefined, 9)).toBe(true);
  });
});
