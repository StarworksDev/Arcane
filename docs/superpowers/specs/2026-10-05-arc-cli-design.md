# arc CLI (D27) -- design

**Naming (AM7, 2026-10-13):** the tool is now Servitor and the engine side is ArcaneRemote; see s0.29 AM7 for the mapping. AM8 (exit 9, engine root) and AM9 (an engine-version-independent client) follow it. The body below keeps its original `arc` spellings.

**APPROVED 2026-10-09 (user): every recommended pick in s0 (0.1-0.28) plus amendments AM1-AM6 (s0.29). Build scope now: P0 + `play|stop|pause|resume|step` pulled forward from P1 (s13). Branch `feat/arc-cli`.** Drafted 2026-10-05.

**Roadmap row:** D27 (`docs/superpowers/specs/2026-10-05-arcane-1.0-roadmap.md`). **Research:** `docs/research/2026-10-05-arc-cli-research.md` (the external survey and the seam inventory this spec builds on; its Q1-Q30 table is superseded by section 0 below). **Verified against:** Arcane `main` at `c0fb42e1`, plus `feat/settings-s7` at `b982b4a8` for the settings-arc types (`CVarContext`, `Audience`, `RemoteCVarService`), which are not on `main` yet.

**Depends on:** the settings arc closing to `main` (the context model, `RemoteCVarService`, `Arcane::Paths`, and the S4 editor action registry); the entity-record undo arc for transactional structural edits (P1 only); and two small Astra upstream changes (s5.3).

---

## 0. Decisions needing the user

Each item is a real fork. The recommended pick is what the rest of this spec assumes; a different pick changes only the sections named in the item.

**0.1 Transport** (s4.2, s6.1)
- (a) **Loopback HTTP/1.1 + Server-Sent Events for streams.** What Unity, Defold, Bevy BRP and Unreal MCP all ship. `curl`-debuggable, one code path on Windows and Linux, reachable from WSL2 with mirrored networking. Costs: a TCP port every local process of any user can connect to (so a token is mandatory), browser-reachable in principle (so `Origin` and `Host` checks are mandatory), and some agent sandboxes (Codex) block loopback by default.
- (b) Named pipe on Windows / AF_UNIX on Linux. The OS enforces the user boundary and no browser can reach it. Costs: two code paths, no `curl`, not reachable from WSL to Windows, and sandboxes restrict socket files too.
- (c) WebSocket. Bidirectional push. Costs: a heavier server and client reconnect logic, for push that SSE already covers in the one direction needed.
- (d) Hybrid: HTTP and a pipe from day one.
- **Recommended: (a)**, with the transport behind one seam (`IArcTransport`) so (b) can be added later without touching dispatch, as `RemoteCVarService` is already transport-agnostic. One code path now; (d) doubles the test matrix for a sandbox problem that has a configuration answer (s14).

**0.2 Wire envelope** (s6)
- (a) JSON-RPC 2.0 (the research's pick). Standard ids, batches and error codes; BRP precedent. Costs: a response is either `result` or `error`, which splits the arc envelope (warnings, notifications and undo facts ride on failures too) across two shapes, and JSON-RPC batches are independent unordered calls, the opposite of arc's transactional batch (s8.3).
- (b) One REST path per command (Defold). Costs: the URL space becomes the API surface, and versioning and discovery happen per route.
- (c) **One arc envelope end to end:** `POST /arc/v1/call` takes `{id, command, args, options}`, and the reply is exactly the envelope the CLI prints in `--format json`.
- **Recommended: (c).** The wire shape and the CLI's JSON output are the same object, so there is no translation layer to drift, and batches mean one thing. An MCP or JSON-RPC adapter, if one is ever wanted (0.21), maps the catalogue, not the wire.

**0.3 Discovery: descriptor location and format** (s4.4)
- (a) Extend `editor.lock`. Costs: the Hub mirrors its format (`ArcaneHub/src-tauri/src/editorlock.rs`, "change BOTH"), and one lock cannot describe an editor plus a separate runtime plus a server on one project.
- (b) **One JSON descriptor per host process at `<project>/Saved/arc/<hostKind>-<pid>.json`**, written atomically, heartbeat-refreshed, deleted on clean exit, validated by `EditorLock`'s pid + creation-time rule. A projectless editor (the start page) writes under the settings arc's `Arcane::Paths` EditorUserDir instead.
- (c) One per-user directory (`%LOCALAPPDATA%/Arcane/arc/`) for every host on the machine. Costs: workspace-sandboxed agents (Codex, Claude's sandbox) usually cannot read outside the project, so discovery would fail exactly for the main users.
- (d) A fixed port range plus a scan. Costs: guessable ports, no token channel, and races between hosts.
- **Recommended: (b).** It sits inside the workspace the agent already has, supports several hosts per project, and leaves the Hub's lock untouched. `arc launch` names the descriptor path up front (`--arc-descriptor`), so a launched host is never confused with another.

**0.4 Arming per host** (s4.7)
- (a) Armed in every non-Dist host by default.
- (b) **The editor arms by default (an Editor Preferences cvar, `editor.arc.enable`, default on); ArcaneRuntime and ArcaneServer arm only with `--arc`.**
- (c) Off everywhere unless flagged.
- **Recommended: (b).** The editor is the desk-check target and already writes `Saved/` state; games and servers should not open a port unasked (Chrome 136 refused its debug port on default profiles for this reason). (c) makes every agent session start with "restart the editor with a flag".

**0.5 Token lifetime** (s4.5)
- (a) **One token per host process**, 32 bytes from `Crypto::GenerateSecureToken`, generated when the listener starts, never rotated while the process lives, gone with it.
- (b) A persistent per-project token file. Scripts can cache it, but a leaked token outlives every session.
- (c) Short per-attach session tokens minted through a handshake with a bootstrap secret. Stronger, but adds a round trip and a state machine for no threat (a) leaves open: the reader of the descriptor is already the user.
- (d) Passed only through the environment of a host `arc launch` started. Costs: attaching to a hand-started editor becomes impossible.
- **Recommended: (a).** The listener and token live in the host executable, never in the game DLL, so a hot reload cannot rotate the token (Unity's pre-0.4 bug cannot occur by construction).

**0.6 Session capability scoping: observe vs drive** (s10.3)
- (a) One token, full access.
- (b) Two tokens: the observe token in the descriptor, the drive token in a separate file a harness can withhold.
- (c) **One token; a session may declare itself read-only (`arc --read-only ...`, or `ARC_READ_ONLY=1`), and the host then refuses every `drive` command for that request.**
- **Recommended: (c),** documented honestly as a guard rail against agent mistakes, not a security boundary (anyone who can read one file can read the other, so (b) is not a boundary either on a single-user desk). Revisit (b) if arc ever serves a multi-user machine.

**0.7 Protected cvar values** (s10.2)
- (a) Follow the settings context table: the `Editor` context reads `Protected` values.
- (b) **arc never returns a `Protected` value in any context.** Reads answer `<protected>`; sets are allowed and audited with `<protected>` for old and new, exactly as `RemoteCVarService` already does for `ServerAdmin`.
- **Recommended: (b).** arc's output lands in agent transcripts, CI logs and model context; secrets must not. The human still reads them in the editor's own windows.

**0.8 Client implementation and packaging** (s4.6)
- (a) **A separate `arc.exe`, C++ on ArcaneCore and `Arcane::Cli`**, a top-level project beside `arcbuild/`, shipped in `bin/` and beside `ArcaneEditor.exe` in a packaged layout.
- (b) An `arcbuild arc ...` subcommand. Costs: arcbuild's contract is "works with no host and no SDK beyond what the verb needs"; folding a host client into it widens that contract and its dependency set.
- (c) Rust (shared with the Hub) or Python. Costs: a second JSON model, a second exit-code table, and a runtime to install on CI runners.
- (d) `arc` absorbs arcbuild entirely. Costs: rewrites a shipped, multi-backend-hardened tool for naming tidiness.
- **Recommended: (a).** The same nlohmann JSON, reflection types and `Arcane::Cli` refusal idiom as the hosts; milliseconds to start; `arc build` invokes arcbuild as a child (0.22).

**0.9 Command registration model and schema format** (s5)
- (a) **Each command declares a reflected C++ args struct and a reflected result struct (or a declared free-form result schema). The args schema is generated by `Astra::JsonSchemaGenerator` and the args are parsed by `ReflectionJsonReader`, so the schema and the parser are the same metadata.** The schema format is a documented subset of JSON Schema 2020-12: `type`, `properties`, `required`, `additionalProperties:false`, `enum`, `minimum`/`maximum`, `items`, `description`, `default`, `$defs`/`$ref` for nested structs.
- (b) A builder API (`.Arg("entity", ArgType::String, "help").Optional()`), like `Arcane::Cli`, emitting the schema. No reflection dependency, but a second description of every argument that reflection already provides for settings and components.
- (c) Hand-written schema strings per command. Costs: drift between schema and parser is guaranteed.
- **Recommended: (a).** It requires two upstream Astra changes first (s5.3): the generator today marks every non-hidden field `required` and emits no `default`, while `ReflectionJsonReader` leaves a missing key at its default; that mismatch is exactly the drift (a) exists to prevent.

**0.10 Output formats** (s6.5)
- (a) **`json` when stdout is not a TTY, `human` on a TTY, plus `ndjson` (streams and progress, always ending in a `result` frame) and `github` (Actions annotations from `DiagLocator`s); `ARC_FORMAT` overrides.**
- (b) Always `json`.
- (c) Unity's `tsv` when piped.
- **Recommended: (a).** Agents parse JSON; humans read text; nothing in Arcane's workflow pipes tab-separated columns.

**0.11 Exit codes** (s6.6)
- (a) **Unity-aligned:** 0 ok, 1 internal, 2 usage/refusal, 3 auth/permission, 4 no host, 5 host busy (retryable), 6 operation failed, 7 host unreachable or died (retryable), 8 a check ran and did not pass, 130/143 signals.
- (b) arcbuild's 0/2/3 only. Costs: "the host said no" and "there is no host" collapse into one code.
- (c) Per-verb codes. Costs: codes collide across verbs, the failure mode the arcane-verify skill already warns about ("never the raw exit code (codes collide)").
- **Recommended: (a).** Coarse, non-colliding codes; the detail lives in the JSON. 2 stays "refused" as in `Arcane::Cli` and arcbuild.

**0.12 Execution point in the frame** (s4.3)
- (a) Top of frame, after event/input pumping and before simulation.
- (b) End of frame (BRP's `RemoteLast`).
- (c) **(a) by default; a command that needs a rendered frame (captures, `wait settled`) is a frame-spanning job that arms the frame at (a) and completes after render.**
- **Recommended: (c).** Edits land before this frame's sim and render, so "set, then screenshot" sees the edit one frame later with no guesswork; captures never block the main thread.

**0.13 Entity addressing** (s7.2)
- (a) **`guid:<Identity.id>` as the address; `name:<Identity.name>` accepted only when unique (strict); raw Astra handles appear only as diagnostics.**
- (b) Raw Astra handles. Costs: violates the binding GUID rule; handles do not survive save/load, play, hot reload or undo.
- (c) Hierarchy paths only. Costs: hierarchy is not first-class yet (D25).
- **Recommended: (a),** with `path:a/b/c` added when hierarchy is first-class.

**0.14 Live ECS query surface** (s7.2)
- (a) **Read-only in P0 (`entity get`, `ecs query`, `ecs stats`, `resource get`); writes in P1, only through the undo path.**
- (b) Read and write together in P0.
- (c) Read-only through 1.0.
- **Recommended: (a).** Reads are useful immediately and risk nothing; writes need the undo integration (0.15) to be right first. The query shape is BRP-like JSON (`with`/`without`/`fields`/`limit`, strict by default) and must route through Astra's unified `View` once D25 provides it, so relationship storage joins without a new verb.

**0.15 Undo integration** (s8)
- (a) **Every mutating command in the editor goes through `CommandStack`, with no opt-out.** Component edits use `Begin`/`SnapshotComponent`/`Commit`; structural edits use the existing structural memento path (`ApplyRegistryMutation`) until the entity-record undo arc replaces it.
- (b) Opt-in `--undo`.
- (c) Opt-out `--no-undo`.
- **Recommended: (a).** A desk check run by an agent must leave the user's undo history exactly as a hand edit would. Commands that cannot be undone say so in their declaration and in the envelope (`undo.undoable:false`).

**0.16 Mutations in Play mode and in non-editor hosts** (s8.4)
- (a) Allowed everywhere, reported `undoable:false`.
- (b) Refused outside Edit mode.
- (c) **Per command, declared:** live tweaks (cvars, component pokes, input) are allowed in Play and in runtime hosts; authoring commands (asset ops, scene save, structural scene edits) refuse in Play with the same reason their menu items grey out.
- **Recommended: (c).**

**0.17 ImGui widget addressing and UI input** (s9)
- (a) **Our own `ArcUi` item registry fed by Dear ImGui's test-engine hooks (`IMGUI_ENABLE_TEST_ENGINE`: `ItemAdd`, `ItemInfo`, `Log`), adopting the Test Engine's ref-path grammar, plus opt-in stable tags on the widgets desk checks care about. Input is injected into ImGui's IO at the frame boundary with backend input suppressed for the duration; never OS-level `SendInput`.**
- (b) Vendor the Dear ImGui Test Engine. Strong prior art (coroutine runner, actionability defaults, JUnit export). Licensing: free for natural persons, OSI-licensed derivative software and entities under USD 2M turnover; otherwise paid after a 45-day trial, with no internal-tooling exemption. Arcane itself (MIT, public) qualifies; a closed downstream studio over the threshold would not, and the user's own studio may cross it once Aphelyon earns. It also binds arc to ImGui, which the post-1.0 own-UI plan replaces.
- (c) Pixels and OCR only. Costs: brittle, slow, and blind to state (checked, disabled, open).
- **Recommended: (a).** The hooks are the only route to item-level introspection with or without the Test Engine; owning the registry keeps the contract Arcane's and lets the own-UI rewrite become its second producer. (b) stays open later for an in-process `[ui]` test suite where its runner pays for itself. SendInput is already banned for desk checks (settings spec s14.4).

**0.18 The "see" verbs** (s7.4)
- (a) **Three capture sources (`viewport`: the scene only; `window`: the backbuffer with chrome; `panel <ref>`: a backbuffer crop to an ImGui window's rect), returned as files plus metadata (`path`, `bytes`, `sha256`, `width`, `height`, `format`); `arc compare` runs in the host through `ImageCompare::CompareImages` and `ReferenceImages::ResolveReference`; `arc ui tree --out` writes a YAML structural snapshot that can be committed and diffed as text.**
- (b) Inline base64 images. Costs: token cost in agent context, and no atomic artifact to attach to a failure.
- (c) Client-side comparison of files. Costs: a second comparator beside the Playwright-conformant one.
- **Recommended: (a),** with `--inline` allowed for crops under a size cap (64 KiB) for agents that want to look.

**0.19 The long-job model** (s7.7)
- (a) **A job table in the host executable: any command declared `long` returns `{job}` with `--detach`, otherwise the CLI blocks and renders progress; `arc job list|status|wait|cancel`. Jobs survive game-module hot reload because they live in the host exe.**
- (b) Blocking calls only, no jobs.
- (c) Unity's async twins (`build` plus `build_status`).
- **Recommended: (a).** Blocking-only breaks the moment a wait outlives an HTTP timeout; twins multiply the catalogue.

**0.20 Dist policy** (s10.6)
- (a) **Compiled out: the listener, dispatcher and every arc command are inside `#if !defined(ARCANE_DIST)`, as `Dev` cvars are.**
- (b) Present but disabled by a cvar.
- (c) Available behind a key.
- **Recommended: (a).** A shipped game must not contain a remote-control surface. Live-ops administration of a shipped server is the S7 `RemoteCVarService` over Aphelyon's HMAC RPC, not arc.

**0.21 MCP compatibility mode in v1** (s15)
- (a) Never.
- (b) **Not in v1. The catalogue is kept MCP-mappable (every command has an args schema and a description), and an `arc mcp` stdio adapter is decided after P3 against real need.**
- (c) Ship `arc mcp` in v1.
- **Recommended: (b).** The roadmap row says "NOT an MCP server" and Unity deprecated its in-editor MCP server for its CLI. Keeping the door open costs nothing; walking through it is a separate decision.

**0.22 Build and test integration** (s7.5, s7.6)
- (a) `arc build` always runs arcbuild from the client, then waits for the host's reload.
- (b) **When an editor is attached, `arc build` runs the editor's own Rebuild Game Module action (one build at a time, output in the editor's console, the editor's unload/reload sequence); with no editor attached it runs arcbuild directly and, if a runtime is attached, waits for its `PluginHost` generation to change. `arc test` runs ArcaneTests out of process with JUnit plus a JSON summary; `arc gate golden` runs `scripts/golden-gate.ps1` and reads `golden-gate-summary.json`.**
- (c) Leave builds and tests out of arc.
- **Recommended: (b).** Two builders racing on one `Binaries/` slot is the failure (a) invites; the editor already owns that sequence.

**0.23 Desk-check conversion format** (s12.3)
- (a) **`.arcscenario` files: a flat JSON list of steps, each an arc call plus optional expectations (JSON-pointer equals / approx / exists, a verdict, an exit code), run by `arc run <file>`. No variables beyond back-references to earlier results, no loops, no branches.**
- (b) Plain shell or PowerShell scripts calling arc. Costs: assertions re-implemented per script and per shell; Windows and Linux CI diverge.
- (c) Catch2 `[arc]` C++ cases only. Costs: every new check is a rebuild, the exact thing the introspection goal exists to avoid.
- **Recommended: (a) for desk checks, plus (c) for arc's own tests.** The vocabulary is deliberately too small to become a scripting language (there is no `eval`, s15).

**0.24 The cvar path** (s7.3)
- (a) **Generalize `RemoteCVarService` (settings S7): the caller's context becomes a constructor or request field (`Editor`, `LocalHost`, `ServerAdmin`), and the response gains a structured form (`{ok, value, type, flags, explain{published, pending, setBy, history}}`) beside today's `text`.** One audited service, two renderings.
- (b) arc calls `CVarRegistry` directly with its context and uses `RemoteCVarService` only on servers. Costs: two code paths for the same rules.
- (c) arc parses the text replies. Costs: prose parsing, which `VerifyReport`'s facts-not-prose rule forbids.
- **Recommended: (a),** as a small amendment to the in-flight settings spec s9.

**0.25 Who builds the editor action registry** (s5.2)
- (a) **The settings arc builds it (S4, spec s7.2), and its spec is amended now so each action also declares an argument schema (0.9), an availability predicate that returns a reason, whether it is undoable, and which hosts it exists in.**
- (b) The arc arc builds it.
- (c) Both, separately.
- **Recommended: (a).** It is already specified there with contexts and chords; adding four fields before S4 starts avoids a retrofit.

**0.26 Problems and logs in every host** (s7.2)
- (a) **Move the headless half of `DiagnosticStore` (pure data, mutex-guarded, `MatchesDiagnosticFilter`) from ArcaneEditor into ArcaneCore and install it as the diagnostics sink in every host; the editor's Problems panel reads the same store.** Add a sequenced log subscriber sink in ArcaneCore beside the file sink.
- (b) arc installs its own tee sink that chains to the previous one. Costs: the slot is process-wide and last-writer-wins (`Diagnostics::SetSink`, `ClearSinkIfCurrent` exists precisely because of that hazard).
- (c) `arc problems` is editor-only in v1.
- **Recommended: (a).** Today only the editor installs a sink (`ArcaneEditor/src/Panels/DiagnosticStore.cpp:119`), so a runtime or server has no problems feed at all; that is a works-in-editor-broken-in-runtime gap independent of arc.

**0.27 A minimized editor** (s4.3)
- (a) Keep pumping full frames while an arc session is attached.
- (b) **Drain arc requests on every loop iteration, including minimized ones; commands that need a rendered frame answer `host.busy{state:"minimized"}` with the remediation "restore the window or use a headless host".**
- (c) Ignore it.
- **Recommended: (b) for v1.** Today a minimized editor returns `FramePump::SkipFrame` before doing anything (`ArcaneEditor/src/App/EditorAppFrame.cpp:536`), so (c) means a silent hang. (a) needs the viewport graph to render without presenting the chrome swapchain, which is worth doing but is its own change (Appendix A).

**0.28 Protocol versioning** (s6.1)
- (a) An integer `protocol` plus `capabilities[]` in the descriptor.
- (b) A `/arc/v1` path prefix.
- (c) **Both: `/arc/v1` changes only on a breaking change; `capabilities[]` announces additive features; the CLI refuses a too-new major with exit 4 and a remediation, and degrades with a notification on missing capabilities.**
- **Recommended: (c).**

**0.29 Amendments (user, 2026-10-09, after an outside review by two other assistants of the s0 summary)**
Most of that review is already covered here: the Origin/Host checks and the header-only token (s10.1, s4.5); stale descriptors (the `{pid, start}` rule, s4.4); the thin client with host-served schemas (s5.3, `arc call`); one build path, since the editor's rebuild spawns arcbuild (0.22); 0.25 deferred to P1; and the Astra schema fixes carried in the Astra lane. The six points it raised that are new are adopted:
- **AM1, server tick semantics.** On a host whose loop is a fixed-tick server (ArcaneServer, or a headless runtime with no render), "frame" in 0.12, s4.3 and s7 means one **tick**. Commands drain at the tick boundary, before the simulation step. `host.frame` carries the tick number. `wait frames N` and `step N` count ticks. Captures are refused on a host with no renderer (`capture.no-renderer`, exit 2).
- **AM2, fan-out.** `--all <hostKind>` (or `--all` for every live host of the project) sends one command to every matching live host in parallel. The client prints one envelope per host, inside a `{hosts:[{pid, hostKind, envelope}]}` wrapper (an ndjson frame per host under `--format ndjson`). The exit code is the worst per-host code, ranked 7 > 6 > 8 > 5 > 4 > 3 > 2 > 0. `--all` with a `drive` command needs `--yes`. The bot-swarm milestone depends on this.
- **AM3, reserved netcode observability.** The catalogue reserves the `net.*` group for the replication arc (D10). Expected commands: `net.connections` (per connection: peer, RTT, loss, in/out bytes per second), `net.replication` (entities replicated, snapshot bytes, deltas, budget drops), `net.tick` (tick time, budget, overruns) and `ecs.archetypes` (entity counts by archetype; read-only, may land in P0). The commands register through the same declaration macro. arc builds none of them except `ecs.archetypes`.
- **AM4, `--json` alias.** `--json` equals `--format json`, and it wins over TTY auto-detection. Agents run under pseudo-terminals, so the agent instructions (the P4 skill and every dispatch brief) always pass `--json`.
- **AM5, descriptor permissions.** The descriptor holds the token. On Windows it is created with an owner-only DACL (the current user plus SYSTEM); on POSIX it is created 0600. It is written under the same rule to its temp file before the rename.
- **AM6, exit codes 5 vs 7 stated.** 5 means `host.busy`: the request was provably not run, and retrying at the same target is safe. 7 means the host is unreachable, died or is restarting: rediscover first (`arc status`), then retry against the new pid. Every `host.busy{state}` (including 0.27's minimized state and the queue-full state) is exit 5.
- **AM7 (2026-10-13, user): naming.** The client tool is **Servitor** (`servitor` / `servitor.exe`, premake project `Servitor` at `Source/Programs/Servitor`, output `Binaries/<cfg>/Servitor/`). It is the user-facing remote-control client and will be the first Hub package. The engine side keeps a generic name, the same rule as `--offscreen`: the Runtime module is `ArcaneRemote` (`Source/Runtime/ArcaneRemote/`, `ARC_REMOTE_API`, namespace `Arcane::Remote`, headers `<Arcane/Remote/...>`, client code `Arcane::Remote::Cli`), the wire paths are `/remote/v1/...`, the headers `X-Remote-*`, the host flags `--remote`, `--remote-descriptor`, `--remote-idle`, the descriptor dir `<project>/Saved/Remote/`, the cvars `editor.remote.enable`, `remote.server.frameBudgetMs`, `remote.server.queueDepth`, `log.remote.ringLines`, the read-only env var `ARC_REMOTE_READ_ONLY`, error codes `remote.*`, and Catch2 tags `[remote]` (client cases `[servitor][client]`). P2 scenario files are `.servitor`. `arc` / `ARC_` is reserved for engine macros (`ARC_*`) and backstage tools (`arcbuild`, `arccook`); a stutter such as `ARC_ARC_*` must never exist. Every `arc` in this spec's body that names the tool reads as Servitor, and every engine-side `arc` spelling reads as the mapping above. The full mapping is `.superpowers/sdd/2026-10-09-arc-cli-p0/RENAME-SERVITOR.md`; the P0 plan (rev 8.2) applies it.
- **AM8 (2026-10-13, Hub packages spec, agreed with the distribution lane): exit 9 and the engine root.**
  - **Exit 9 is reserved for the Hub's PATH launcher** (Hub packages spec s5), which runs Servitor and passes Servitor's exit code through. The launcher exits 9 when it cannot run Servitor:
    - `engine.not-found`: no engine is registered, or the chosen engine's folder is gone.
    - `package.not-installed`: the engine exists, but Servitor is not installed on it.
    - `package.incompatible` (reserved for a later user-scope install): the installed Servitor does not speak the engine's remote protocol major.
  - **The launcher's output.** The sentence goes to stderr. Under `--json` or `--format json|ndjson`, a minimal envelope (`ok:false`, `errors:[{code, message}]`) also goes to stdout; ndjson carries it inside the final result frame.
  - **Servitor never exits 9.** The static manifest (`servitor commands --format json`) lists 9 with these codes, marked launcher-only.
  - **Engine root.** Hosts to launch, and their exe-relative log and diagnostics directories, resolve in this order, first match wins:
    1. `--engine <root>`;
    2. `SERVITOR_ENGINE`;
    3. the source layout: `../ArcaneEditor`, `../ArcaneRuntime`, `../ArcaneServer` beside `Binaries/<Config>-<os>-<arch>-md/Servitor/`;
    4. the package layout: `<exeDir>/../../sdk/sdk.json` exists, and hosts are at `<root>/Binaries/<engineConfig>-<os>-<arch>-md/<Host>/`, with `engineConfig` read from that file.
  - **Roots without a usable answer.** A root with no `sdk/sdk.json` is a source checkout, which uses Servitor's own build configuration. A package root whose `sdk.json` lacks `engineConfig`, or no root at all, is `engine.unresolved` (exit 4) with remediation, never a guess.
  - **The launcher passes the root through the environment.** It sets `SERVITOR_ENGINE` in the child's environment and never edits argv, so a user's own `--engine` wins.
  - Descriptor discovery (`<project>/Saved/Remote/`) does not depend on the layout.
- **AM9 (2026-10-14, user): the client is engine-version-independent.** Servitor's code must not depend on the engine build it ships beside, so one Servitor can talk to any host that speaks a protocol major it supports.
  - **Boundary.** `servitor` links ArcaneCore, ArcaneNet and the wire-protocol module `ArcaneRemoteProtocol` only. That module holds the envelope, the error and exit tables, the descriptor and redaction, and depends on ArcaneCore alone. Servitor never links the host side of ArcaneRemote, ArcaneEngine, ArcaneScene, ArcaneSim, ArcaneAssets or ArcaneCrypto. A test checks Servitor's staged library closure. Process identity (`{pid, start}` liveness) is ArcaneCore's `Platform::IsProcessLive`; `EditorLock::IsLive` wraps it.
  - **The wire is the contract.** The client builds request `args` as JSON by field name and never includes a host command's args struct or registry.
  - **Versions.**
    - The protocol has a major (1) and a minor (0). The minor rises with each additive change. `GET /status` and the envelope carry `protocolMinor`; an absent `protocolMinor` reads 0.
    - The engine advertises both numbers in `Source/Runtime/ArcaneRemoteProtocol/remote-protocol.json`, which a test pins to the C++ constants. The Hub reads that file in source checkouts, and installed engines copy it into `sdk/sdk.json` as `remoteProtocol: {major, minor}`.
  - **Compatibility within a major.**
    - Changes are additive.
    - Both sides ignore unknown fields in the envelope, its `host` block, `data` and `GET /status`.
    - New request fields go inside `options`, whose unknown keys the host ignores. Unknown top-level request keys and unknown `args` keys stay refusals (s6.2).
    - The client declares the majors it supports (P0: `{1}`). Any other major is `protocol.mismatch` (exit 4) with remediation.
    - Features are discovered from the host's catalog, never inferred from version numbers. A verb whose command the host lacks gets `command.unknown` (exit 2), with remediation naming the command and the host's `major.minor`.
  - **Servitor's own version.** Servitor has its own semver, starting at 0.1.0. `servitor --version` and the static manifest print it with the supported majors.
  - **Supersedes.** AM7's "client code `Arcane::Remote::Cli`" (the client is the Servitor program's own code, namespace `Servitor`) and the body's hard-coded sibling launch (the engine root follows AM8). Exit 4 also covers `engine.unresolved`.
  - **Distribution.** In Hub v1, Servitor still ships per engine, in the engine-scoped package shape (Hub packages spec). A later one-per-user install (scope `user`) needs no change to this spec.

---

## 1. What the user asked for (2026-10-05)

- A command-line tool, **not an MCP server**, modelled on Unity's CLI that replaced Unity's MCP server (https://docs.unity.com/en-us/unity-cli/replace-mcp-server-unity-cli).
- It attaches to a running ArcaneEditor, ArcaneRuntime or headless host, or launches one and attaches, and **fully pilots it**: query, act, see, with JSON output and stable exit codes.
- **Why it comes next** (after the settings arc, ahead of the render foundation): it turns the human desk checks into automated gates, the biggest calendar lever on the roadmap (risk "the user is the bottleneck", roadmap s5), and it serves Claude, Codex and Grok uniformly.
- **Security:** local-only by default, a per-session token, disabled or compiled out in Dist, permissions through the cvar audience/context rules.
- It completes the Playwright-level introspection goal (2026-09-03): "can the agent ask a NEW question of a running host without a rebuild?"

## 2. Goals and non-goals

### 2.1 Goals
1. **Attach and launch.** Find a running host by project and kind, or start one and wait until it is ready, with no log scraping and no fixed sleeps.
2. **Query.** Live ECS reads (entities, components, resources, statistics), the scene tree, the selection, assets, logs, problems and frame statistics, as JSON in the scene-file dialect.
3. **Act.** Run any editor action by name through the same undoable path as menus and shortcuts; get, set and explain cvars; play, stop, pause and step; edit components; inject game input.
4. **See.** Viewport, window and panel screenshots; golden comparisons through the existing comparator; the ImGui item tree with addressable, actionable controls.
5. **Grade.** Every answer is an envelope with machine-readable errors and a coarse, non-colliding exit code. An error is never reported as a success, and "accepted" is never reported as "done".
6. **Self-describe.** The host's live catalogue, with argument and result schemas, is the documentation an agent reads.
7. **Turn desk checks into files** (`.arcscenario`) that run headless in CI.

### 2.2 Non-goals
- An MCP server (0.21).
- `eval` or an embedded scripting language. A new question without a rebuild is answered by the generic ECS/reflection surface and by registered commands ("registered commands for verdicts").
- Remote (non-loopback) control, multi-user access, or a remote UI.
- Anything in a Dist build (0.20).
- Replacing `VerifyReport`, `--report` or the golden gate. arc reuses them; one-shot `--frames` runs keep working unchanged.
- Decision tracing ("why did the camera pick that matrix"). arc exposes provenance where it is already recorded (`SetBy`, `resolvedLevel`, `triedPaths`, `exitReason`) and adds none.
- A render-graph introspection verb in v1 (Appendix A).

## 3. Where it stands (verified in code, 2026-10-05)

Every path below was read on `main` at `c0fb42e1` unless marked `s7` (branch `feat/settings-s7`, `b982b4a8`).

**Built and directly reusable:**

| Seam | Where | What arc uses |
|---|---|---|
| Host option set and the refusal idiom | `ArcaneClient/src/Arcane/Host/HostConfig.{hpp,cpp}` over `ArcaneCore/src/Arcane/Cli/Cli.hpp` | the launch vocabulary; Rule 3 ("no silently inert flags", `HostConfig.cpp:310`); stderr + exit 2 |
| The open-ended headless refusal | `HostConfig.cpp:515-536` ("an agent spawning a bare `--headless` spawns a process it cannot stop") | lifted only when arc is armed (s4.7) |
| Observation report and verdicts | `Host/VerifyReport.{hpp,cpp}` (`kSchemaVersion = 13`, worlds since 6); `Host/Verdict.hpp` (seven values, `IsGreen`); `exitReason` chain `ArcaneRuntime/src/RuntimeApp.cpp:1256-1297` | live probes, verdict vocabulary, the grading discipline |
| Witness harness | `ArcaneTests/src/Helpers/HostWitness.{hpp,cpp}` (`WitnessRun{exitCode, timedOut, progressingAtKill, reportFound, reportParsed, report, wallMs, stdoutPath, stderrPath}`); `s7` adds scripted child stdin | arc's integration-test launcher; grading precedence |
| Golden gate and comparator | `scripts/golden-gate.ps1` (lanes `runtime-scene`, `f3-cull-blend`, `editor-ui`, `editor-ui-perspective`; `golden-gate-summary.json`); `ArcaneCore/src/Arcane/Assets/ImageCompare.hpp` (`CompareImages`); `Host/ReferenceImages.hpp` (`ResolveReference`, `triedPaths`) | `arc compare`, `arc gate golden` |
| Settle (auto-wait) | `Host/SettleBound.hpp`; `ShaderCompiler::IsIdle()` (`Render/ShaderCompiler.hpp:129`); the capture tail in `ArcaneRuntime/src/RuntimeFrame.cpp:863-930` | `arc wait settled` |
| Viewport capture | `EditorApp::WriteAutoScreenshot` (`ArcaneEditor/src/App/EditorApp.cpp:1880`) through `CaptureGraphViewportPng` (`FrameDesc::capture` + `ReadCapture` on the viewport context, never the chrome) | `screenshot viewport` |
| Backbuffer capture | `--screenshot` ("captures the BACKBUFFER, after tonemap and ImGui", `HostConfig.hpp:37`) | `screenshot window`, `panel` crops |
| Reflection and the JSON codec | `ArcaneCore/src/Arcane/Reflection.hpp` (facade over Astra attributes); `Serialization/ReflectionJson.hpp` (`ReflectionJsonWriter`/`Reader`, refuses rather than drops) | component JSON, args parsing |
| JSON Schema | `ThirdParty/Astra/include/Astra/Reflection/JsonSchema.hpp` (`JsonSchemaGenerator`, draft 2020-12, tooltips as descriptions, `enum`, `minimum`/`maximum`, `additionalProperties:false`) | `describe`, `schema component` (gaps below) |
| Runtime-typed ECS access | `Astra/Registry/Registry.hpp`: `GetComponentByHash`, `HasComponentByHash`, `AddComponentByID`, `RemoveComponentByID`, `SetEnabledByID`, `IsEnabledByID`, `InspectResources`; `Astra/Debug/Inspector.hpp` (POD snapshots "so tools never hold pointers into live engine state") | `entity get`, `ecs query`, `ecs stats` |
| Durable identity | `Arcane::Identity{Guid id; std::string name}` (`ArcaneCore/src/Arcane/Scene/Components.hpp:284`); the `pick` probe already reports it | addressing |
| Cvars and the console | `Config/CVarRegistry.hpp` (`Find`, `Get`, `Set`, `Publish`, `Explain`, `List`, `Execute`, `RevertCheats`); `s7`: `CVarContext {Editor, LocalHost, ServerAdmin, Client}`, `Audience`, `CanRead`, `Snapshot()`, `PublishImmediate`, `SetPolicy`, `SetAuditSink`; `ConsoleModel.hpp` | `cvar *`, `console exec` |
| Remote cvar service | `s7`: `Config/RemoteCVarService.{hpp,cpp}` (`Handle(RemoteCVarRequest{op,name,value,callerId}) -> {ok,text}`, ServerAdmin, Protected never readable, commands only via `set` with `ServerCanExecute`, audited, publishes after a set) | the cvar path (0.24) |
| Undo | `ArcaneClient/src/Arcane/Edit/{Command,CommandStack}.hpp`; `ComponentEditCommand.hpp`; `RegistryStateCommand.hpp`; `ArcaneEditor/src/Project/AssetFileOps.hpp` | s8 |
| Diagnostics feed | `Base/Diagnostics.hpp:600-706` (`Publish(key, span)`, `Clear(key)`, `DiagLocator{Entity, Asset, File, GraphNode}`, stable dotted `code`); `ArcaneEditor/src/Panels/DiagnosticStore.hpp` | `problems`, `--format github` |
| Logs | `Base/Log.hpp` (`AttachFileSink`, `FileSinkPath`; a 512-line lock-free backlog ring for the crash path) | `logs` (needs a new sink, below) |
| Crash and hang capture | `Base/Diagnostics.hpp`, `Base/DiagEnvelope.hpp` (`.arcdiag`), `<exe dir>/diagnostics` | the "host died" envelope (exit 7) |
| Discovery precedent | `EditorLock` in `Project/Project.{hpp,cpp}` (`<project>/Saved/editor.lock`, `{pid, start}`, `ReadLive`, `RivalPid`); mirrored by the Hub | descriptor liveness rule |
| Net and crypto | `Net/TcpSocket.hpp` (`CreateListenSocket(port, bindAddr)`); `Net/RateLimiter.hpp`; `Crypto/Crypto.hpp` (`GenerateSecureToken` hex, `HexEquals` constant-time) | listener, token |
| Threads | `Base/ServiceThread.hpp` ("NOT the fork-join pool") | the listener thread |
| Hot reload | `Plugin/PluginHost.hpp` (`Poll`, `Reload(restoreState)`, `ForceReload`, `Generation()`); polled by both hosts (`EditorAppFrame.cpp:4424`, `RuntimeFrame.cpp:767`) | `arc build` reload wait |
| Build driver | `arcbuild/src/Exit.hpp` (`0` ok, `2` refused, `3` probe-would-rebuild); `Output.hpp` (`IOutput`: `Info/Always/Error/Child`, text only) | `arc build` |
| Editor intents | `MenuRequests` (`ArcaneEditor/src/Panels/EditorPanels.hpp:44`), `EditorApp::ConsumeMenuRequests`, `RunSceneAction`, `ConsumeAssetPanelActions`; `RouteAction` (`Panels/LocatorRoute.hpp:27`) | the pre-registry action surface |

**Gaps found while verifying** (each changes this design; Appendix B lists where the research differed):
1. **No action registry exists** on `main`, `feat/settings` or `feat/settings-s7`. It is the settings arc's S4 tranche (spec s7.2). P1 depends on it (0.25).
2. **`CommandStack::Cancel` discards without reverting** ("no push, no revert", `CommandStack.hpp:111`), and **structural mementos refuse to run inside an open transaction** (`CommandStack.hpp` on `InTransaction`). A failed arc batch therefore cannot rely on `Cancel` to roll back, and a batch mixing spawn/despawn with component edits cannot be one undo step today (s8.3).
3. **The JSON Schema generator marks every non-hidden field `required` and emits no `default`**, while `ReflectionJsonReader` leaves a missing key at its default. Optional arguments need an Astra change (s5.3).
4. **`DiagLocator::Entity` carries the packed Astra handle** (`e.GetValue()`, `SceneSerializer.hpp:599`, `EntityOps.cpp:606`), not the Identity GUID. arc translates it to a GUID at the boundary (s7.2).
5. **Only the editor installs a diagnostics sink**; runtime and server hosts have no problems feed (0.26).
6. **The log backlog ring is the crash path's**: 512 lines, lock-free, reads may tear, no sequence numbers. `arc logs --follow` needs its own sequenced subscriber sink (0.26).
7. **`CreateListenSocket` binds `INADDR_ANY` when `bindAddr` is null** (`TcpSocket.hpp:223-226`). The arc listener must pass `"127.0.0.1"` explicitly, and a test pins it.
8. **A minimized editor skips the whole frame** (`EditorAppFrame.cpp:536`) (0.27).
9. **`PluginHost::Generation()` exists but the last reload's outcome does not** (`Reload` returns `bool` to its caller only). arc needs `{generation, ok, reason, atFrame}` published.
10. **The context model is settings-branch only.** `main` still has the v1 `Permission`; arc lands after the settings arc merges (s13).
11. **`RemoteCVarService` returns text and is hard-wired to `ServerAdmin`** (0.24).
12. **arcbuild has no JSON output**; `arc build` reports arcbuild's exit code and captured output until arcbuild II's unified output policy lands (s7.5).

## 4. Architecture

### 4.1 Overview

```
 agent / CI / human
        |  arc <group> <verb> [args] [--format json|ndjson|human|github]
        v
 +------------------+   reads <project>/Saved/arc/<kind>-<pid>.json  (port, token, protocol, caps, state, heartbeat)
 |  arc.exe (C++)   |
 |  ArcaneCore+Cli  |
 +------------------+
        |  HTTP/1.1 to 127.0.0.1:<port>, Authorization: Bearer <token>
        |  POST /arc/v1/call      one command -> one envelope
        |  POST /arc/v1/batch     an ordered list -> one envelope (s8.3)
        |  GET  /arc/v1/status    answered off the main thread
        |  GET  /arc/v1/stream?topics=logs,problems,state,job:<id>   (SSE)
        v
 +--------------------------- host process (Editor / Runtime / Server) ---------------------------+
 | ArcListener (ServiceThread): accept, HTTP parse, peer/Origin/Host/token checks, enqueue         |
 |      |  bounded request queue                                                                  |
 |      v                                                                                         |
 | ArcDispatcher (main thread, top of frame): state gate, per-frame budget, run, serialize, reply  |
 |   ArcCommandRegistry <- engine commands (ArcaneCore, ArcaneClient)                             |
 |                      <- editor commands and actions (ArcaneEditor, settings S4 registry)       |
 |                      <- game-module commands (module-scoped, dropped on unload)                |
 |   ArcJobTable (host exe): frame-spanning commands, survives hot reload                         |
 |   services: CVarRegistry + RemoteCVarService | Astra Registry + ReflectionJson | CommandStack   |
 |             DiagnosticStore (Core) + log subscriber | capture + ImageCompare | ArcUi registry   |
 |             PluginHost reload facts | HostStatus snapshot (atomic)                              |
 +------------------------------------------------------------------------------------------------+
```

### 4.2 Where the code lives
- **`ArcaneCore/src/Arcane/Arc/`**: the protocol types (request, envelope, error codes), the command registry, the dispatcher, the job table, the HTTP listener and the descriptor writer. Core, so the Core-only `ArcaneServer` can host it. Everything inside `#if !defined(ARCANE_DIST)`.
- **`ArcaneClient/src/Arcane/Arc/`**: engine commands that need the client layer (capture, `ArcUi`, input injection, world/registry commands).
- **`ArcaneEditor/src/Arc/`**: editor commands (selection, documents, play control, undo, the action-registry bridge, `ui` verbs against editor panels).
- **`arc/`** (top level, beside `arcbuild/`): the client executable. It links ArcaneCore for the protocol types, `Arcane::Cli`, JSON and process launch; it links nothing from ArcaneClient.
- The transport sits behind `IArcTransport` (listen, accept, read request, write response, write stream event). HTTP is the only v1 implementation (0.1).

### 4.3 Threading model
1. **The listener runs on one `ServiceThread`.** It accepts loopback connections, parses HTTP, enforces the transport checks (s10.1), authenticates, and pushes a parsed request into a bounded queue (default 64; `arc.server.queueDepth`, Dev). A full queue answers `host.busy{state:"queue-full"}` without running anything. The listener **never touches the registry, the cvar registry's pending store, the undo stack or ImGui**.
2. **`GET /status` is answered on the listener thread** from an atomic `HostStatus` snapshot (`std::atomic<std::shared_ptr<const HostStatus>>`) the main thread republishes once per frame: state, frame number, worlds, PluginHost generation, last reload result, catalogue generation, project, ABI. It answers in every state, including `booting` and `modal`.
3. **Everything else runs on the main thread at one point per loop iteration**: after the platform event and input pump, before simulation (0.12). The dispatcher drains the queue under a budget (default 8 commands or 4 ms, `arc.server.frameBudgetMs`, Dev); what is left waits for the next frame. The drain runs even when the editor is minimized (0.27).
4. **Reads come from snapshots.** A command reads live state on the main thread at the frame boundary, when nothing else mutates it, and serializes its answer to JSON there; that JSON is the snapshot. The listener only writes bytes. Streams (`watch`, `logs --follow`) are produced the same way: the main thread computes the per-frame delta and hands an immutable buffer to the listener. Cvar reads may also use `CVarRegistry::Snapshot()` (s7), which is wait-free from any thread, but v1 does not need to.
5. **Nothing blocks the main thread.** A command that must wait (a rendered frame, settle, a reload, a build) is a job (s7.7): it arms work, returns, and is stepped once per frame by the dispatcher until done. A command that would block is a design error caught in review; the dispatcher logs any single command exceeding its budget by 10x as a warning with the command name.
6. **The host state gates main-thread commands.** States: `booting`, `ready`, `reloading` (PluginHost mid-reload), `compiling-shaders`, `building`, `modal` (a `ModalErrorQueue` dialog or any modal popup is open), `playing`, `paused`, `minimized`, `shutting-down`. Each command declares which states it tolerates; otherwise the answer is `host.busy{state}` and the request is **provably not run**, which is the only case the client retries automatically (s6.6).
   - **What a P0 host publishes (final fix pass, 2026-10-15).** The session recomputes the state at every drain, highest first: `shutting-down` (latched once the host leaves its loop), `reloading` (published the moment a `PluginHost` reload begins and ends, because the reload is synchronous: `GET /status` and the descriptor show it while the main thread is inside it, and no drain ever does), `building` (the editor's module rebuild is running), `modal` (an editor with an interactive window has a queued error dialog or an open ImGui modal popup; a headless editor's queued dialogs are unattended, so they stay log and Problems entries and never read `modal`), `paused`, `playing`, `minimized`, `ready`. The play axis is the editor's Play mode and its RunLoop's pause, or a runtime's or server's AM1 tick gate: held (at start, or after `stop`) is `ready`, running is `playing`, a paused run is `paused`. The `PlayPolicy` gate reads the play axis itself, so an `EditOnly` command is refused in Play even while `modal` or `building` outranks it. P0 publishes neither `booting` (the descriptor is written once the host serves) nor `compiling-shaders`.
7. **Every response carries the frame number it ran on** (`host.frame`), so `wait frames N`, `step N` and "edit then capture" are exact.
8. **Hot reload.** The listener, queue, job table, token and engine/editor commands live in the host executable and in ArcaneCore/ArcaneClient, never in the game DLL. Game-module commands are registered under the module's name (the settings S1 "current module" capture) and dropped by `PluginHost`'s unload path beside `CVarRegistry::UnregisterModule`; a request for a dropped command during a reload answers `host.busy{state:"reloading"}`, after it `command.unknown`. The catalogue generation increments on every change and is in every envelope, so a client notices.

### 4.4 Discovery
- **The descriptor** (0.3), written atomically (temp file + rename) when the listener is up, refreshed every second (`heartbeat`, and `state`), deleted on clean exit:

```json
{
  "protocol": 1,
  "endpoint": "/arc/v1",
  "hostKind": "editor",
  "pid": 23180,
  "start": 134041056000000000,
  "port": 51744,
  "token": "9f0c...64 hex chars...",
  "engineVersion": "0.x (main c0fb42e1)",
  "abi": 52,
  "config": "Debug",
  "project": "D:/dev/starworks/Arcane/ReferenceProject",
  "projectGuid": "4b1d...",
  "worlds": [ { "id": "client", "role": "Standalone" } ],
  "capabilities": [ "cvar", "ecs.read", "capture.viewport", "capture.window", "compare" ],
  "state": "ready",
  "heartbeat": "2026-10-05T18:00:01Z"
}
```

- **Validation:** a descriptor is live only when its `{pid, start}` names a running process with that creation time (the `EditorLock::ReadLive` rule). A dead one is ignored and deleted by the next `arc status`. A live process whose heartbeat is older than 10 s is reported as `hung`, not absent, with the `.arcdiag` path if the hang watchdog wrote one.
- **Selection:** `arc` with no target picks the single live host for the project in the current directory (walking up to the nearest `.arcproj`, as `--project` resolution does). More than one candidate is a refusal (`target.ambiguous`, exit 2) listing them; `--host editor|runtime|server` and `--pid N` disambiguate. This is Playwright's strict mode applied to hosts.
- **Ports:** the listener binds port 0 and writes the OS-chosen port. There is no fixed range.
- **`editor.lock` is untouched** and keeps its Hub contract.
- **Linux:** `EditorLock::Info::start` is 0 on non-Windows today; the Linux port supplies the process start time from `/proc/<pid>/stat` for both the lock and the descriptor (Appendix A).

### 4.5 Auth and session
- The token (0.5) is 32 random bytes as 64 hex characters (`Crypto::GenerateSecureToken(32)`), sent as `Authorization: Bearer <token>`, compared with `HexEquals` (constant time).
- **The token is never logged or echoed.** The host never writes it to the log or a report; `arc --verbose` redacts it; `--format github` never prints it; a failed auth does not echo the presented value.
- **A session** is per request, not a connection: there is no login. Per-request headers carry the client's name (`X-Arc-Client: claude|codex|grok|ci|<free text>`, for the audit log), the read-only flag (0.6), and an optional `X-Arc-Request-Id` the envelope echoes.
  - **The caller's name (final fix pass, 2026-10-15).** The header is `X-Remote-Client` (AM7). Its value is 1 to 128 printable ASCII characters; any other value is refused with `arg.invalid` (exit 2) before the call is queued, and an absent header leaves the audit line's `client` empty. Servitor always sends it: `--client <name>`, or by default `servitor/<version> pid <pid>`, and refuses a `--client` the host would refuse.
- Repeated auth failures from the loopback peer are rate-limited with `Net/RateLimiter.hpp` (default 20 per 10 s, then `auth.throttled`).

### 4.6 The CLI
- **Grammar:** `arc <group> <verb> [positional] [--name value | name=value ...] [global flags]`. Positional and named arguments map onto the command's args struct by its schema; `--args-json '{...}'` and `--args-file f.json` pass a whole object. Unknown names are refused before anything is sent (`arg.unknown`, exit 2), using the schema from `describe` (cached per catalogue generation in `<project>/Saved/arc/cache/`).
- **Generic access:** `arc call <command> [args]` invokes any registered command by its dotted name, including game-module commands the CLI binary has never heard of. Every friendly verb in s7 is sugar over `arc call`.
- **Global flags:** `--project <dir|.arcproj>`, `--host editor|runtime|server`, `--pid N`, `--world <id>`, `--format`, `--timeout <s>` (default 30; waits and jobs take their own), `--out <path>`, `--client <name>`, `--read-only`, `--result-only` (print `data` only), `--quiet`, `--verbose`, `--no-retry`.
- **Rule 3 holds in the client:** a flag that does not apply to the chosen command is refused, never ignored. `--world` on a command that is not world-scoped is a refusal.
- **Static manifest:** `arc commands --format json` prints the CLI's own verbs, global flags, formats and exit codes with no host running (Unity's `commands --format json`).

### 4.7 Host lifecycle
- **`arc launch editor|runtime|server [--project P] [--headless] [--backend dx12|vulkan] [-- <host flags>]`** spawns the host from the sibling directory of the `bin/<cfg>-windows-x86_64-md/` tree the CLI lives in (`../ArcaneEditor/ArcaneEditor.exe` and so on; or `--exe`), adding `--arc --arc-descriptor <path>`. It waits for that exact descriptor and `state:"ready"` (or `--until booted|ready|settled`), then prints `{pid, port, descriptor, hostKind}`. Host stdout and stderr go to `Saved/arc/logs/<kind>-<pid>.{out,err}.txt`, as `bevy_brp_mcp`'s `brp_launch` does, and their paths are in the envelope.
  - **A boot that outlives `--timeout` (final fix pass, 2026-10-15)** answers `launch.timeout` (exit 8) with `detail {pid, log, descriptor, state}`: the spawned process is still running, so it is never `host.busy` (exit 5, "provably not run, retry is safe"), and the remediation names `servitor wait ready --pid N` and `servitor quit --pid N`.
- **`--arc` on a host** arms the listener (runtime, server; the editor is armed by its cvar). **With `--arc` armed, `--headless` no longer requires `--frames`**: the reason for that refusal ("a process it cannot stop") is gone because `arc quit` is a stop path. Without `--arc` the refusal stands unchanged. A headless host with `--arc` and no `--frames` also exits by itself when no arc request has arrived for `--arc-idle-exit <s>` (default 600), so an abandoned CI host cannot live forever.
- **Held start (AM1; Task 29, 2026-10-15):** a runtime or server launched with `--remote` and no `--frames` starts held at frame 0 until `servitor play` or `servitor resume` runs it (a `--frames N` run is not held), and it says so through the startup log line `remote: holding at frame 0 (servitor play/resume to start)`, a windowed runtime's title suffix ` [held]` while the sim is held (at start, or paused or stopped by servitor) and `--remote`'s help text (an editor has no tick gate: it opens in Edit, and `servitor play` enters Play); there is no opt-out flag.
- **`arc quit [--force] [--report <path>]`** asks the host to shut down cleanly at the next frame boundary through its normal exit path (report written, `exitReason:"arc-quit"`, descriptor deleted). `--force` terminates the process after `--timeout`. The editor honours its usual unsaved-changes guard: an editor with dirty documents answers `host.busy{state:"modal"}` unless `--discard-changes` is given, which is a `drive` command and audited.
- **Waiting:** `arc wait ready|settled|frames N|reload|state <s>|job <id>` (s7.1). `settled` reuses the `--settle` predicate (two byte-equal frozen-clock captures and `ShaderCompiler::IsIdle()`), bounded by `--timeout`.
  - **`reload` on a failed reload (Task 30).** A reload that fails or is refused leaves `PluginHost::Generation()` alone, so the host also hands its session `PluginHost::ReloadAttempts()` and `LastReloadOk()`; a failed attempt after the wait armed answers `module.reload-failed` (exit 6) with `detail {pluginGeneration, atFrame}` instead of timing out.
  - **Host bound on a wait (verification N3).** A host runs a `host.wait` for at most `kMaxWaitTimeoutMs` (10 minutes), whatever `options.timeoutMs` asks for (the listener and `--timeout` both admit up to the u32 maximum of milliseconds). A longer request ends at that bound like any timeout (`timedOut`, exit 8), and the final envelope carries the warning `wait.timeout-clamped` with `detail {requestedMs, maxMs}`. The bound exists because `wait settled` freezes the host clock during its captures and nothing cancels a wait whose client went away.
- **A host that dies mid-command** is reported with exit 7 and an envelope naming what is known: the exit code if observed, whether a `.arcdiag` appeared in `<exe dir>/diagnostics` after the request was sent (its path), and the last state seen. HostWitness's precedence applies: died, then timed out, then the envelope.

## 5. Command registration and self-description

### 5.1 The declaration
```cpp
// ArcaneClient/src/Arcane/Arc/EntityCommands.cpp (illustrative)
struct EntityGetArgs
{
    std::string entity;                    // "guid:<id>" | "name:<Identity.name>"
    std::vector<std::string> components;   // empty = every reflected component
    std::string world;                     // empty = the host's default world
};
ARCANE_REFLECT_TYPE(EntityGetArgs)
    ARCANE_REFLECT_FIELD(EntityGetArgs, entity)     ARCANE_REFLECT_ATTR(Tooltip, "guid:<id> or name:<unique name>")
    ARCANE_REFLECT_FIELD(EntityGetArgs, components) ARCANE_REFLECT_ATTR(Tooltip, "component short names; empty = all") ARCANE_REFLECT_ATTR(Optional)
    ARCANE_REFLECT_FIELD(EntityGetArgs, world)      ARCANE_REFLECT_ATTR(Tooltip, "world id from `arc world list`") ARCANE_REFLECT_ATTR(Optional)
ARCANE_REFLECT_TYPE_END(EntityGetArgs)

ARC_REMOTE_COMMAND(arc_entityGet, "entity.get", EntityGetArgs,
    Arc::Desc{ .summary = "Read one entity's reflected components as scene-format JSON.",
               .tags = {"ecs", "query"}, .hosts = Arc::Hosts::All, .access = Arc::Access::Observe,
               .mutates = Arc::Mutates::None, .states = Arc::States::AnyButBooting,
               .resultSchema = Arc::Schema::Of<EntityGetResult>() },
    [](const EntityGetArgs& a, Arc::Call& call) -> Arc::Result { /* main thread */ });
```
- **`ARC_REMOTE_COMMAND`**, not `ARC_COMMAND`: `ARC_COMMAND` already names console commands in the cvar system. Console commands stay reachable through `console.exec` (s7.3) with text output; arc commands are structured.
- **Declared fields:** `name` (dotted, stable, lower camel per segment), `summary`, `tags` (for tag-first listing), `hosts` (Editor / Runtime / Server mask), `access` (`Observe` or `Drive`), `mutates` (`None`, `Undoable`, `NotUndoable`), `playMode` (`Allowed`, `EditOnly`, 0.16), `states` (the host states it tolerates, s4.3), `thread` (`Main`, the default; `Listener` only for status-class commands that read atomics; `Job` for frame-spanning work), `long` (may run as a job, 0.19), `available` (a function pointer returning `nullptr` or a stable reason code such as `playing`, `building`, `no-project`, `no-selection`: the same predicate a menu uses to grey an item out), `args` (the reflected type), `result` (a reflected type or a declared schema), and `module` (captured automatically, as the settings arc captures it for cvars).
- **Function pointers, not `std::function`,** at the DLL boundary, following `Diagnostics::Sink` and `CommandFn`.
- **Registration refuses** an empty summary, a duplicate name, a name outside `[a-z0-9.]` segments, an args type without reflection, and a `Drive` command with `mutates:None` (a drive command that changes nothing is a smell; observe it instead). A test pins each refusal.

### 5.2 The three sources
1. **Engine commands**, registered statically by ArcaneCore and ArcaneClient (s7).
2. **Editor actions.** Every action in the settings S4 registry is exposed as `action.run {name, args}` and listed by `action.list` with its context, chord, availability reason and undoability (0.25). Editor-only commands that are not actions (selection, documents, play control) are ordinary `ARC_REMOTE_COMMAND`s in ArcaneEditor.
3. **Game-module commands**, declared with the same macro inside the game DLL, registered under the module's name and dropped on unload (s4.3, step 8). This is Arcane's equivalent of Unity's `[CliCommand]` and Unreal's `AICallable`: a game answers a game-specific question ("what is this unit's turn order?") with no engine change.

### 5.3 Self-description and the schema
- `arc list [--tag t] [--query q]` returns the host's live catalogue **tags first** (tag names with counts), then commands matching a tag or query: name, summary, access, mutates, availability now. This keeps a cold agent's first read small.
- `arc describe <command>` returns the full declaration with the args and result schemas.
- `arc schema component <Name>` and `arc schema resource <Name>` return a reflected type's schema from `JsonSchemaGenerator`.
- **The schema is the documented 2020-12 subset of 0.9.** Two upstream Astra changes come first (commit in the Astra repo, then sync, per the standing rule): (1) an `Optional` attribute, honoured by the generator (not listed in `required`) and implicit for any field with a reflected default; (2) `includeDefaults` implemented, emitting the default member initializer's value; plus nested structs emitted as `$defs`/`$ref` instead of a bare `"object"`. The generator also gains a JSON (`nlohmann::json`) output beside the string. Until (1) lands, P0 commands declare every argument required and the CLI fills documented defaults client-side; that interim is removed by the change, not kept.
- **Args validation happens twice:** in the CLI against the cached schema (fast refusal, exit 2, nothing sent) and in the host by `ReflectionJsonReader` with unknown keys refused (`additionalProperties:false`). The host is authoritative.

## 6. Wire protocol and output contract

### 6.1 Endpoints
| Method and path | Thread | Purpose |
|---|---|---|
| `GET /arc/v1/status` | listener | `HostStatus` snapshot; no main-thread work; answers in every state |
| `POST /arc/v1/call` | main | one command |
| `POST /arc/v1/batch` | main | an ordered list of calls; transactional when every member allows it (s8.3) |
| `GET /arc/v1/stream?topics=...` | listener (data produced on main) | SSE: `logs`, `problems`, `state`, `catalog`, `job:<id>`, `watch:<id>` |
| `GET /arc/v1/catalog` | listener (snapshot) | the live catalogue, same as `list` with no filter |

Every response carries `Content-Type: application/json` and `X-Arc-Protocol: 1`. Non-JSON bodies, chunked request bodies, bodies over 4 MiB (`arc.server.maxBodyBytes`, Dev) and HTTP/1.0 are refused.

### 6.2 Request
```json
POST /arc/v1/call
{ "id": "r-17", "command": "entity.get",
  "args": { "entity": "name:MeshCube", "components": ["LocalTransform"] },
  "options": { "world": "client", "readOnly": false, "timeoutMs": 30000 } }
```

### 6.3 The envelope (the HTTP body and the CLI's `--format json` output)
```json
{
  "ok": true,
  "protocol": 1,
  "id": "r-17",
  "command": "entity.get",
  "host": { "kind": "editor", "pid": 23180, "world": "client", "frame": 81234, "state": "ready",
            "catalog": 7, "pluginGeneration": 3 },
  "data": {
    "entity": { "guid": "8c1e...", "name": "MeshCube" },
    "components": { "LocalTransform": { "position": [0.0, 0.5, 0.0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] } }
  },
  "errors": [],
  "warnings": [],
  "notifications": [],
  "artifacts": [],
  "undo": null,
  "timing": { "queuedMs": 3, "ranMs": 0.4 }
}
```
- **Rules:**
  - `ok:true` never coexists with a non-empty `errors`. A partial result (a non-strict query) is `ok:true` with `warnings`, never errors.
  - A failure still prints the full envelope on stdout in json mode; human diagnostics go to stderr.
  - The client adds a `client` block (`{exitCode, retried, descriptor}`) when it prints, so the printed object explains its own exit code.
  - `undo` is `{label, undoable, transaction}` for mutating commands and `null` otherwise.
  - `artifacts` lists files the command wrote: `{kind, path, bytes, sha256, width?, height?, format?}`.
- **Error object:** `{code, message, remediation?, detail?, locator?, candidates?, check?}`. `locator` reuses the `DiagLocator` shape with entities as GUIDs; `candidates` lists matches for an ambiguity; `check` names the failed actionability check (s9.4).

### 6.4 Error codes (stable, dotted; never reworded casually, like `Diagnostic::code`)
| Code | Meaning |
|---|---|
| `arg.invalid`, `arg.unknown`, `arg.missing` | the args failed the schema (Rule 3) |
| `command.unknown`, `command.unavailable` | no such command; or not available now, with the `available` reason |
| `command.host-kind` | not offered by this host kind |
| `auth.denied`, `auth.throttled` | bad or missing token; rate-limited |
| `permission.denied` | the context/audience table or the game's `CVarPolicy` said no; carries `context`, `audience`, `verdictSource` |
| `permission.read-only` | a `Drive` command in a read-only session |
| `host.busy` | not run; carries `state` (s4.3) |
| `target.ambiguous`, `target.none` | host selection (client side) |
| `entity.not-found`, `entity.ambiguous`, `entity.no-identity` | addressing (s7.2) |
| `component.unknown`, `component.absent`, `component.ambiguous` | component addressing |
| `reflect.unreadable`, `reflect.unsupported` | the codec refused a value or a field type |
| `cvar.unknown`, `cvar.type-mismatch`, `cvar.refused-weaker`, `cvar.protected` | from `SetResult` and the service |
| `cvar.refused` | any other refused set (`SetResult::Stale`, `TypeMismatch` or an unrecognized refusal); exit 6 |
| `undo.unavailable`, `undo.batch-structural` | s8 |
| `play.edit-only` | an authoring command in Play (0.16) |
| `play.not-playing` | pause, resume or step on an editor in Edit; exit 6 |
| `play.refused` | the editor refused Play or Stop, or `play.start --topology` names a different topology than the running one (`detail.running`); exit 6. `play.start` on an editor already playing (no topology, or the same one) is `ok` with `data.alreadyPlaying: true` and the running `topology` |
| `play.stopped` | a `play.step` ended before its ticks ran (stop, a held gate, Play left, or the session ended); ticks may have run, so it is exit 6, never the retryable exit 5 |
| `ui.not-found`, `ui.ambiguous`, `ui.stale-ref`, `ui.not-actionable` | s9 |
| `capture.no-frame`, `capture.minimized` | s7.4 |
| `capture.no-renderer` | a capture (or `wait settled`) on a host with no renderer; exit 2 (AM1) |
| `capture.artifact-failed` | the capture's PNG was not written; exit 6 |
| `compare.failed`, `compare.missing-reference` | mirrors `exitReason` |
| `compare.reference-unreadable` | the reference could not be resolved or decoded; exit 6 |
| `compare.bless-failed` | `--bless` wrote no reference; exit 6 |
| `compare.artifact-failed` | a Failed verdict's capture or diff PNG was not written; exit 6 |
| `job.unknown`, `job.cancelled`, `job.failed` | s7.7 |
| `arg.inapplicable` | a flag or argument that does not apply to this command or host (Rule 3); exit 2 |
| `resource.ambiguous` | a resource name that matches more than one type; exit 2 |
| `resource.not-found`, and any other `*.not-found` | addressing; exit 6 |
| `ecs.artifact-failed`, `resource.artifact-failed`, `scene.artifact-failed`, `log.artifact-failed` | a dump over the inline cap could not be written as an artifact; exit 6 |
| `module.reload-failed` | `wait reload` saw a reload fail (or be refused) after it armed; the previous module is still loaded; exit 6 |
| `quit.force-failed` | `quit --force` could not end the process (client side); exit 6 |
| `host.unavailable` | the host has no armed session for this command (stopped, or not wired on this host); exit 5 |
| `protocol.mismatch`, `engine.unresolved` | client side: a host protocol major this client does not speak; no engine root resolves (`--engine`, `SERVITOR_ENGINE`, the source sibling, a package root; AM8); exit 4 |
| `host.unreachable`, `host.died`, `host.hung`, `host.restarting` | client side: no answer, the process exited, its heartbeat stalled past the hung rule, or it is restarting; exit 7 |
| `wait.timeout` | reserved for a wait condition false at timeout; today a wait (and a `play.step`) times out as `ok` with `data.timedOut` (exit 8) |
| `call.timeout` | client side: the request body was sent and no reply came by `--timeout`, while the target watch still saw the process alive with a fresh heartbeat; the command may still be running (`detail.inFlight: true`); exit 8, never resent |
| `launch.timeout` | client side: `servitor launch` spawned a host that did not read ready within `--timeout`; it is still running (`detail.pid`); exit 8 |
| `internal` | a bug; carries the `.arcdiag` path when one exists |

### 6.5 Formats (0.10)
- `json`: one envelope.
- `ndjson`: progress frames `{"type":"progress", ...}`, stream frames `{"type":"event", "topic":..., ...}`, and always a final `{"type":"result", "count":N, "envelope":{...}}`, so truncation is detectable.
- `human`: a short summary on stdout, diagnostics on stderr, artifacts as paths.
- `github`: `::error file=..,line=..::message` / `::warning ...` from error and warning locators, then the human summary.

### 6.6 Exit codes (0.11)
| Code | Meaning |
|---|---|
| 0 | ok; includes verdicts `Passed`, `PassedOnFallback`, and `Skipped` (with the reason in JSON) |
| 1 | internal error in `arc` itself |
| 2 | usage / refusal: unknown verb, flag or argument; ambiguous target; schema failure |
| 3 | auth or permission: bad token, `permission.denied`, `permission.read-only` |
| 4 | no host: nothing to attach to, a stale descriptor, or a protocol-major mismatch (with remediation) |
| 5 | host busy or not ready within `--timeout`; provably not run; retryable |
| 6 | the host answered and the operation failed (`errors[]` set; verdict `Errored`) |
| 7 | host unreachable or died mid-command (with a `.arcdiag` path when one exists); retryable only with `--retry-on-death`, which resends only after `host.died` or `host.hung` and only to the dead host's replacement: a host of the same kind and project root (compared normalized), a different `{pid, start}`, that was not live when the call began (AM6). Servitor rescans about every 250 ms within the remaining `--timeout`; two such hosts are refused at once, and none by the deadline reports the death. `--retry-on-death` with `--all` is refused (exit 2; retry one host kind at a time with `--host <kind>`), and so is `--retry-on-death` with `--pid` (exit 2: a restarted host has a new pid) |
| 8 | a check ran and did not pass: test failures, `compare.failed`, a wait condition false at timeout, a scenario expectation, or verdict `Failed`/`Indeterminate`; also `call.timeout` and `launch.timeout` |
| 9 | reserved for the Hub's PATH launcher (AM8): `engine.not-found`, `package.not-installed`, `package.incompatible`; `servitor` itself never returns it |
| 130 / 143 | SIGINT / SIGTERM |

- **Retry rule:** the client retries automatically (twice, with backoff) only when the request was provably not run: `host.busy`, or a connection refused before the body was sent. `--no-retry` disables it.
- **Grading precedence** follows HostWitness: died, then timed out, then the exit, then the envelope, then `data`. Callers grade by `errors[0].code` and `data`, never by stdout prose.

### 6.7 Large payloads
UI trees, ECS dumps over 256 KiB, screenshots and diffs are written atomically under `<project>/Saved/arc/out/<command>-<frame>-<n>.<ext>` (or `--out`), and the envelope carries them in `artifacts`. A command never streams megabytes into the envelope. `<project>/Saved/arc/out/` is pruned to the newest 200 files on host start.

## 7. The v1 command catalogue

Dotted names are the wire names; the CLI verb follows. P = the phase that ships it (s13). Hosts: E editor, R runtime, S server.

### 7.1 status and session
| CLI | Command | Hosts | Access | P |
|---|---|---|---|---|
| `arc status [--all]` | (client + `GET /status`) | all | observe | P0 |
| `arc launch <kind> ...` | (client) | all | drive | P0 |
| `arc quit [--force] [--discard-changes]` | `host.quit` | all | drive | P0 |
| `arc wait ready\|settled\|frames N\|reload\|state <s>` | `host.wait` (job) | all | observe | P0 |
| `arc world list` | `world.list` | all | observe | P0 |
| `arc commands --format json` | (client manifest) | none | n/a | P0 |
| `arc list [--tag] [--query]`, `arc describe <cmd>`, `arc schema component\|resource <Name>` | `catalog.list`, `catalog.describe`, `schema.type` | all | observe | P0 |
| `arc call <command> [args]` | any | per command | per command | P0 |

Example, `arc status` with two hosts on one project:
```json
{ "ok": true, "command": "status",
  "data": { "hosts": [
    { "hostKind": "editor", "pid": 23180, "port": 51744, "state": "ready", "frame": 81234, "abi": 52,
      "project": "D:/dev/starworks/Arcane/ReferenceProject", "heartbeatAgeMs": 410 },
    { "hostKind": "runtime", "pid": 23512, "port": 51790, "state": "booting", "frame": 0, "abi": 52,
      "project": "D:/dev/starworks/Arcane/ReferenceProject", "heartbeatAgeMs": 120 } ],
    "stale": [ { "descriptor": "Saved/arc/editor-19004.json", "reason": "pid-not-running", "removed": true } ] },
  "errors": [], "warnings": [], "notifications": [] }
```

### 7.2 query
| CLI | Command | Hosts | P |
|---|---|---|---|
| `arc entity get <ref> [--component C ...]` | `entity.get` | E R S | P0 |
| `arc ecs query --with A,B [--without C] [--fields A.x,B] [--limit N] [--no-strict]` | `ecs.query` | E R S | P0 |
| `arc ecs stats` | `ecs.stats` (`Astra::Debug::Inspector` snapshot) | E R S | P0 |
| `arc resource list\|get <Name>` | `resource.list`, `resource.get` (`InspectResources` + codec) | E R S | P0 |
| `arc scene tree [--depth N]` | `scene.tree` | E R | P0 |
| `arc selection` | `editor.selection.get` | E | P0 |
| `arc asset list [--kind k] [--query q]`, `arc asset info\|refs <guid>` | `asset.list`, `asset.info`, `asset.refs` | E R | P1 |
| `arc logs [--since <seq>] [--level l] [--follow]` | `log.read`, stream `logs` | E R S | P0 |
| `arc problems [--severity s] [--search q] [--follow]` | `problems.list`, stream `problems` | E R S | P0 |
| `arc stats frame` | `stats.frame` (`Host/FramePerf.hpp`) | E R S | P0 |
| `arc probe <kind@x,y>` | `verify.probe` (the `VerifyReport` probes, live) | E R | P1 |

- **Addressing** (0.13). `guid:` is exact. `name:` must match exactly one `Identity.name` in the world, otherwise `entity.ambiguous` with `candidates` (GUIDs and names). An entity without `Identity` cannot be addressed; queries return it with `"guid": null` and a `handle` field marked diagnostic-only, plus an `entity.no-identity` warning. Diagnostics whose `DiagLocator::Entity` holds a packed handle are translated to the entity's GUID when it is still alive, else reported with `"guid": null` and the handle (gap 4).
- **Component names** are reflected short names (`LocalTransform`); a collision between modules is `component.ambiguous` with qualified candidates (`ReferenceGame::Health`).
- **`ecs.query`** in P0 iterates entities and filters with `HasComponentByHash`, strict by default (an unknown component name is an error; `--no-strict` turns it into a warning and an empty match). It returns `{count, truncated, rows:[{guid, name, components{...}}]}`, with `--limit` defaulting to 100 and rows beyond 256 KiB going to an artifact. When D25's unified `View` lands, the implementation moves onto it; the wire shape does not change.
- **`log.read`** reads a new **sequenced subscriber sink** in ArcaneCore (`Log::Subscribe`), a ring of 4096 structured records `{seq, time, level, logger, text}` (`log.arc.ringLines`, Dev). `--since` takes a `seq`; a gap reports `dropped:N`. It is separate from the crash path's backlog ring, which stays lock-free and untouched. When the host is dead, `arc logs` falls back to the file sink and says so (`notifications:[{code:"logs.from-file", path}]`).
- **`problems.list`** reads the Core `DiagnosticStore` (0.26) through `MatchesDiagnosticFilter`, so the panel and arc agree on what "matches" means.

Example, `arc ecs query --with LocalTransform,MeshRenderer --fields LocalTransform.position --limit 2`:
```json
{ "ok": true, "command": "ecs.query", "host": { "kind": "runtime", "pid": 23512, "world": "client", "frame": 640 },
  "data": { "count": 14, "truncated": true,
            "rows": [ { "guid": "8c1e...", "name": "MeshCube",   "components": { "LocalTransform": { "position": [0, 0.5, 0] } } },
                      { "guid": "21a7...", "name": "MeshSphere", "components": { "LocalTransform": { "position": [2, 0.5, 0] } } } ] },
  "errors": [], "warnings": [], "notifications": [] }
```

### 7.3 act
| CLI | Command | Hosts | Mutates | P |
|---|---|---|---|---|
| `arc entity set <ref> C.field=<json> ...` | `entity.set` | E R | Undoable (E) / NotUndoable (R, Play) | P1 |
| `arc entity add\|remove <ref> <Component> [--value <json>]` | `entity.addComponent`, `entity.removeComponent` | E R | Undoable (E) | P1 |
| `arc entity spawn [--name n] [--component C=<json> ...]`, `despawn <ref>`, `duplicate <ref>` | `entity.spawn`, `entity.despawn`, `entity.duplicate` | E R | Undoable (E), structural | P1 |
| `arc select <ref> [...]`, `arc select --clear` | `editor.selection.set` | E | NotUndoable | P1 |
| `arc open <asset guid\|path>` | `editor.document.open` | E | NotUndoable | P1 |
| `arc action list`, `arc action run <name> [args]` | `action.list`, `action.run` | E | per action | P1 |
| `arc cvar get\|set\|list\|explain <name> [value]` | `cvar.get`, `cvar.set`, `cvar.list`, `cvar.explain` | E R S | NotUndoable, audited | P0 |
| `arc console "<line>"` | `console.exec` | E R S | per line | P0 |
| `arc play [--topology standalone\|listen\|client+server]`, `stop`, `pause`, `resume`, `step [N]` | `play.start`, `play.stop`, `play.pause`, `play.resume`, `play.step` | E (R: pause/step) | NotUndoable | P1 |
| `arc undo`, `arc redo` | `editor.undo`, `editor.redo` | E | n/a | P1 |
| `arc input key <key> [--down\|--up\|--tap]`, `arc input mouse move\|click\|drag ...`, `arc input action <name> [--value v]` | `input.inject` | E (Play) R | NotUndoable | P1 |
| `arc batch <file.json>` | `/arc/v1/batch` | E R | s8.3 | P1 |

- **`play.start`** (P0, Task 29) answers `data {playing: true, topology}`. On an editor that is already playing, it does not restart Play: it answers `ok` with the topology that is running and `alreadyPlaying: true`, and an explicit `--topology` that differs is `play.refused` (exit 6, `detail.running`). A runtime or server refuses `--topology` (`arg.inapplicable`, exit 2): its topology is fixed at launch.
- **`play.step N`** (P0, Task 29) completes when `host.frame` has moved N: one fixed tick per advance on a server, a headless runtime, a stepped host and an editor in Play; on a windowed runtime that is running, one loop pass (whatever its fixed sub-steps), as AM1 counts `host.frame`.
  - **Overlap and bound (final fix pass, 2026-10-15).** A runtime or server keeps one step budget: a new `play.step N` raises the pending ticks to at least N and never adds, and each step completes on its own target, so two overlapping `step 10` calls run 10 ticks, not 20. A step is bounded on the host like `host.wait` (its `timeoutMs`, at most `kMaxWaitTimeoutMs`, with the `wait.timeout-clamped` warning past it); at the bound it answers `ok` with `data.timedOut` (exit 8) and gives back the ticks no other running step still needs. On the editor a step asks for single steps only while the primary loop is paused (a running loop advances `host.frame` by itself). The embedded server world mirrors the primary loop every frame, its pause and its single step of that frame, so the toolbar and servitor keep both worlds in lockstep.
- **`entity.set`** takes `Component.field.path=<json>` pairs, resolved against the reflected type; the value is parsed by `ReflectionJsonReader` and refused rather than coerced (`reflect.unreadable`). In the editor it is one transaction (s8.1).
- **`cvar.*`** goes through the generalized `RemoteCVarService` in the session's context (0.24, s10.2). `cvar.set` publishes before answering, so the reply shows the value in effect, as the service already does; `cvar.explain` returns the structured history (`published`, `pending`, `setBy`, `history[]`).
- **`console.exec`** runs `CVarRegistry::Execute(line, context, SetBy::Console)`. It is the only free-text entry point, it is permission-checked by the same table, `cvar_explain` through it is refused remotely as the service refuses it, and it is `Drive` access. Its result is the command's text plus `ok`, because console commands return text (settings S1 `CommandResult`).
- **`input.inject`** writes into the input system's snapshot seam (the `InputSnapshot` path in `ArcaneClient/src/Arcane/Input/` and `Client/ClientRuntime.hpp`), never OS-level input. It requires Play in the editor and is applied at the next drain point, so the next simulation step sees it.
- **`action.run`** refuses an unavailable action with `command.unavailable` and the action's own reason, exactly what the greyed menu item would have shown.

Example, `arc cvar set render.vsync false` against a runtime (LocalHost context):
```json
{ "ok": true, "command": "cvar.set", "host": { "kind": "runtime", "pid": 23512, "frame": 702 },
  "data": { "name": "render.vsync", "type": "Bool", "value": false, "previous": true, "setBy": "Console",
            "audience": "PlayerSafe", "apply": "Live", "context": "LocalHost" },
  "errors": [], "warnings": [], "notifications": [], "undo": { "label": null, "undoable": false } }
```
And refused, against an editor-audience name in a runtime:
```json
{ "ok": false, "command": "cvar.set",
  "errors": [ { "code": "cvar.unknown", "message": "unknown cvar 'editor.graph.fitMinZoom'",
                "remediation": "Editor-audience settings exist only in ArcaneEditor; attach to the editor (--host editor)." } ],
  "client": { "exitCode": 6 } }
```

### 7.4 see
| CLI | Command | Hosts | P |
|---|---|---|---|
| `arc screenshot viewport [--out f.png]` | `capture.viewport` (job) | E R | P0 |
| `arc screenshot window [--out f.png]` | `capture.window` (job) | E R | P0 |
| `arc screenshot panel <ui-ref> [--out f.png] [--inline]` | `capture.panel` (job) | E | P2 |
| `arc compare <reference> [--source host\|viewport\|window] [--bless] [--max-diff-pixels N] [--max-diff-pixel-ratio r]` | `capture.compare` (job) | E R | P0 |
| `arc ui tree [--window W] [--depth N] [--out f.yml]` | `ui.tree` | E (R: debug ImGui) | P2 |
| `arc ui click\|dblclick\|hover\|type\|check\|uncheck\|select\|drag\|scroll\|focus <ref> [...]` | `ui.act` | E | P2 |
| `arc ui wait <ref> --visible\|--gone\|--enabled [--timeout s]` | `ui.wait` (job) | E | P2 |
| `arc ui get <ref>` | `ui.get` | E | P2 |

- **`capture.viewport`** arms one viewport frame with `FrameDesc::capture` on the viewport context (the `CaptureGraphViewportPng` path) and completes after that frame's readback. **`capture.window`** captures the backbuffer after tonemap and ImGui, as `--screenshot` does, including in `--headless` (the offscreen chrome target). **`capture.panel`** crops the window capture to the ImGui window rect from the `ArcUi` registry.
- **Format:** PNG of the BGRA8_UNORM display-referred capture (`kGraphOffscreenFormat`), with the format named in the artifact. A minimized window answers `capture.minimized` (0.27). The envelope warns `capture.occluded-risk` for a windowed host whose window is not foreground, because some drivers return black for occluded windows.
- **`capture.compare`** captures from `--source` (default `host`: exactly what that host's `--compare` captures, so an arc compare and the golden-gate lane agree on the same pixels), resolves the reference with `ReferenceImages::ResolveReference` (per backend, shared fallback) and compares with `ImageCompare::CompareImages`. It returns `{verdict, resolvedLevel, triedPaths, diffPixels, diffRatio, maxLocalDifference, diffImage}` with the diff image as an artifact. `--bless` follows the existing bless rules exactly (the arcane-verify skill's slot table: bless the staged slot, copy to source). Exit 8 on `Failed`.

Example, a failed compare:
```json
{ "ok": true, "command": "capture.compare", "host": { "kind": "runtime", "frame": 61 },
  "data": { "verdict": "Failed", "reference": "runtime-scene", "resolvedLevel": "backend",
            "triedPaths": [ "Verify/References/dx12/runtime-scene.png" ],
            "diffPixels": 1840, "diffRatio": 0.00089, "thresholds": { "maxDiffPixels": 0, "maxDiffPixelRatio": 0 } },
  "artifacts": [ { "kind": "capture", "path": "Saved/arc/out/capture.compare-61-1.png", "width": 1280, "height": 720, "format": "BGRA8_UNORM" },
                 { "kind": "diff",    "path": "Saved/arc/out/capture.compare-61-1.diff.png" } ],
  "errors": [], "warnings": [], "notifications": [], "client": { "exitCode": 8 } }
```
A compare that ran and failed is `ok:true` (the operation succeeded; the check did not pass) and exits 8, the same split as `Failed` vs `Errored`.

### 7.5 build
| CLI | Command | P |
|---|---|---|
| `arc build [--config Debug\|Release] [--project P]` | client: editor action `editor.module.rebuild` when an editor is attached, else arcbuild; then `host.wait reload` | P3 |
| `arc reload` | `module.reload` (`PluginHost::ForceReload`) | P3 |
| `arc generate` | client: `arcbuild generate` | P3 |

- The result carries arcbuild's exit code, the captured child output as an artifact, and the host's reload record `{generation, ok, reason, atFrame}` (gap 9). arcbuild's exit 3 (`probe` would-rebuild) appears only in `arc build --probe` data, never as arc's own exit code.

### 7.6 test
| CLI | What it runs | P |
|---|---|---|
| `arc test [filter] [--config c] [--seed N]` | `ArcaneTests.exe` from its exe directory with `--reporter junit` plus a JSON summary; exit 8 on failures, with the seed in `data` | P3 |
| `arc gate golden [--lanes l,...]` | `scripts/golden-gate.ps1`, reads `golden-gate-summary.json`, maps lane verdicts | P3 |
| `arc run <file.arcscenario> [--host ...]` | the scenario runner (s12.3) | P2 |

### 7.7 job
| CLI | Command | P |
|---|---|---|
| `--detach` on any `long` command | returns `{job}` | P3 (internal jobs exist from P0) |
| `arc job list\|status\|wait\|cancel <id>` | `job.list`, `job.status`, `job.wait`, `job.cancel` | P3 |
| `arc watch entity <ref> [--component C]` | stream `watch:<id>` (per-frame deltas) | P3 |

- **The job table** lives in the host exe: `{id, command, state: queued|running|done|failed|cancelled, progress{done,total,label}, startedFrame, envelope}`. Finished jobs are kept for 10 minutes or 64 entries. Internally every frame-spanning command (captures, waits) is a job from P0; P3 exposes `--detach` and `arc job`.
- **Cancellation** is cooperative: the job's step function sees the flag at its next step. A job whose command was dropped by a module unload ends `job.failed{reason:"module-unloaded"}`.

## 8. Undo, Play mode and batches

### 8.1 Editor mutations
Every `Undoable` command runs inside one `CommandStack` transaction labelled `arc: <command> <summary>` (for example `arc: entity.set LocalTransform.position`):
- **Component edits:** `Begin(label)`, `SnapshotComponent(entity, descriptor)` for each touched component before writing, the write through `ReflectionJsonReader`, then `Commit(owner)`. This is the Inspector's own path, so one Ctrl+Z undoes an arc edit exactly as it undoes a hand edit, and the Outliner's unsaved-entity marks (`TouchedSinceState`) see it.
- **Structural edits** (spawn, despawn, duplicate, add/remove component where the structural path is required): the existing `ApplyRegistryMutation` memento path, which produces one `RegistryStateCommand` step and names its touched entities. Each structural command is its own undo step until the entity-record undo arc lands.
- **Failure inside a single command:** because `Cancel` does not revert (gap 2), the command validates every argument and resolves every target **before** `Begin`, so the write phase cannot fail on input. A write-phase failure that still occurs (a codec refusal discovered mid-write) restores the snapshotted "before" bytes explicitly and then cancels; a test forces this path.
- **Asset and file operations** use the `AssetFileOps` command family. Destructive ones (`asset.delete`, `asset.move` overwrite) require `--confirm`; `--dry-run` reports what would happen and wins over `--confirm` (Unity's convention).

### 8.2 The envelope's undo block
`{"label": "arc: entity.set LocalTransform.position", "undoable": true, "transaction": 412}`; `undoable:false` with a reason (`play-mode`, `runtime-host`, `not-undoable-command`) otherwise.

### 8.3 Batches
- `POST /arc/v1/batch {id, calls:[...], transactional:true}` runs the calls in order in one drain, with `$N.data.<json-pointer>` back-references to earlier results (Unity's `$0.instanceId`).
- **Transactional (the default in the editor):** every member must be a component edit or an observe command. The batch is one `CommandStack` transaction; a failing member restores every snapshotted component and cancels, and the envelope reports the failing index. A structural member in a transactional batch is refused up front with `undo.batch-structural` ("structural edits cannot share an undo step until the entity-record undo arc lands; send them as separate calls or set transactional:false"). That refusal is lifted by the entity-record undo arc, not by arc.
- **Non-transactional:** members run in order, each its own undo step, and the batch stops at the first failure (later members reported `not-run`).
- Limit: 200 calls per batch (Unity's limit). A batch is one drain-budget unit and may exceed the per-frame budget; it is never split across frames.

### 8.4 Play mode and non-editor hosts (0.16)
- In Play, `playMode:Allowed` commands run against the play world and report `undoable:false, reason:"play-mode"` (Play's Stop restores the edit snapshot; the pre-Play history survives, as the spec-2 desk checklist item A7 states). `EditOnly` commands answer `play.edit-only`.
- ArcaneRuntime and ArcaneServer have no `CommandStack`; their mutations are `NotUndoable` and say so.
- **World selection:** a world-scoped command takes `--world client|server|<id>` when the process runs several (the editor's client + embedded server play); with one world it is optional; with several and none named it is `arg.missing` listing the worlds.

## 9. ImGui addressing (0.17)

### 9.1 The item registry
- `IMGUI_ENABLE_TEST_ENGINE` is defined in `ThirdParty/imgui/imconfig.h` for non-Dist builds (it is commented out today, line 56). Arcane implements the hooks itself: `ImGuiTestEngineHook_ItemAdd`, `ImGuiTestEngineHook_ItemInfo`, `ImGuiTestEngineHook_Log`, and `ImGuiTestEngine_FindItemDebugLabel`.
- The hooks feed `ArcUi::ItemRegistry`, which records per frame, per item: `{id, label, window, parentId, idPath, rect, clippedRect, statusFlags (Openable, Opened, Checkable, Checked, Inputable), itemFlags (Disabled, ...), framesNotMoving, tag}`. It records only while an arc session has requested UI data in the last N frames (`g.TestEngineHookItems` gates ImGui's own calls), so an unattached editor pays nothing beyond a branch.
- **`ArcUi::Tag("inspector.LocalTransform.position.x")`** is an opt-in stable test id (Playwright's `getByTestId`) set just before a widget. Arcane's widget layer (`EditorWidgets`, `PropertyGrid`) stamps tags centrally for Inspector fields, Outliner rows, toolbar buttons, menu items and settings rows, so desk-check targets do not depend on labels.
- The registry is Arcane's contract; ImGui's hooks are its first producer. The post-1.0 own UI becomes the second.
- Device-less ImGui panel tests (for example `ArcaneTests/src/AssetStatusPanelClickTest.cpp`, which re-derives `window->DC.CursorPos` math to find a button) can switch to the registry and stop duplicating ImGui's layout formulas.

### 9.2 Reference grammar
- **Paths, from the Test Engine:** `//Window/Child/Button`, relative to a `--ref` base, `**/` wildcards (search by label across child windows), `$$5` integer ids, `###id` label-independent ids, `\/` escapes.
- **Tags:** `tag:inspector.LocalTransform.position.x`.
- **Snapshot refs:** `arc ui tree` writes YAML in which every item carries `[ref=e12]`; `e12` is valid for the frame generation it came from and later yields `ui.stale-ref` with the tree generation in the error.
- **Strict mode:** a reference matching more than one item is `ui.ambiguous` listing the candidates with their paths and tags. A reference matching none is `ui.not-found` with the nearest labels.

### 9.3 The tree
`arc ui tree` writes a YAML document (an artifact) and returns `{path, items, windows, generation}`:
```yaml
- window "Inspector" [ref=e3] rect=[1520,64,400,980]
  - group "LocalTransform" [ref=e9] openable opened
    - input "position.x" [ref=e12] tag=inspector.LocalTransform.position.x value="0.000" inputable
    - input "position.y" [ref=e13] tag=inspector.LocalTransform.position.y value="0.500" inputable
- window "Outliner" [ref=e20]
  - row "MeshCube" [ref=e21] tag=outliner.row.8c1e... selected
```
Structural goldens (`arc ui tree --out Verify/ui/inspector.yml` committed, then diffed as text, Playwright's aria snapshots) are much less brittle than a full `editor-ui.png` for layout regressions, and they explain a diff in words.

### 9.4 Actions and actionability
- **Injection:** at the drain point, `io.AddMousePosEvent`, `AddMouseButtonEvent`, `AddKeyEvent` and `AddInputCharacter`, with backend (SDL) input suppressed for the duration of the action so a human's mouse cannot interleave. Speed is `fast` (teleport) by default; `--speed normal` moves over frames for drag-sensitive widgets.
- **Before acting**, Playwright's checks, each a named failure in `ui.not-actionable{check}`:
  - `visible`: non-empty clipped rect, window not collapsed;
  - `stable`: the rect unchanged for 2 frames;
  - `receives-events`: after the move, ImGui's hovered id is the target;
  - `enabled`: no `ImGuiItemFlags_Disabled`;
  - `editable` (for `type`): `Inputable`.
- **Auto-steps** (opt-out per call): open parent menus and tree nodes on the path, scroll the item into view, uncollapse its window, wait until not moving.
- **Result:** `{ref, hovered, clicked, valueBefore, valueAfter, framesUsed}`; an input change made through the UI goes through the widget's own undo gesture (`EditGesture`), so a UI-driven edit is undoable like a hand edit.
- **Headless:** the editor's `--headless` already renders the full ImGui chrome offscreen (the `editor-ui` lanes prove it), so every `ui` verb works in CI without a window.

## 10. Security model

### 10.1 The transport rules (0.1)
1. Bind `127.0.0.1` explicitly (gap 7); a test asserts the bound address. No IPv6 wildcard.
2. Refuse any peer that is not `127.0.0.1` (defence in depth against a future bind change).
3. **Refuse any request carrying an `Origin` header**, and any `Host` header other than `127.0.0.1:<port>` or `localhost:<port>` (anti-DNS-rebinding; Unity's and Unreal MCP's rule).
4. Require the bearer token on every endpoint, `status` included (it reveals the project path and state).
5. Bounded everything: body size, header size (16 KiB), request queue, connections (16), idle connection timeout (30 s), SSE subscribers (8).
6. The descriptor is written with user-only permissions: inherited ACLs under the user's project on Windows (the project directory is the boundary, as Unity's and Defold's are), mode 0600 on Linux.

### 10.2 Contexts and audiences
An arc session **is** a context; the settings spec says the context "comes from the session role, not from which console was used":

| Host attached | arc context | Effect (settings spec s3.2 table, unchanged) |
|---|---|---|
| ArcaneEditor (Edit or Play) | `Editor` | every audience read/write; `Hidden` by exact name; `Protected` never returned (0.7) |
| ArcaneRuntime (standalone, listen host) | `LocalHost` | `Game` read, write only `Cheat` with `server.cheats` on; `PlayerSafe` and `Server` read/write |
| ArcaneServer (dedicated) | `ServerAdmin` | through `RemoteCVarService`: audited, `Protected` never readable, commands only with `ServerCanExecute` |
| a connected client process | `Client` | **arc refuses to arm** in a host whose session role is `Client` in v1; a client's view belongs to its server's admin |

- The game's `CVarPolicy` applies on top, unchanged, and its verdict source is reported in `permission.denied`.
- **Beyond cvars**, the same context gates commands: each command declares the minimum context it needs (`editor.*` require `Editor`; `entity.set` in a runtime requires `LocalHost` and is refused on a dedicated server unless the command is declared server-safe; `input.inject` is refused on a server). The rule is "an arc session can do what the console of that host could do, and no more".
- `Editor`-audience cvars and editor commands are absent outside the editor, not hidden: they live in the editor DLL.

### 10.3 Observe and drive (0.6)
- Every command is `Observe` or `Drive`. A read-only session is refused every `Drive` command with `permission.read-only`.
- `Drive` includes: every mutation, `cvar.set`, `console.exec`, `input.inject`, `ui.act`, `play.*`, `host.quit`, `module.reload`, `capture.compare --bless`.

### 10.4 Audit
- Every `Drive` call, allowed or refused, appends one line to `<project>/Saved/arc/audit.ndjson`: `{time, client, pid, command, argsDigest (sha256 of canonical args), outcome, errorCode, frame}`. Args are digested, not stored, so a set value that is a secret never lands on disk in clear.
- Sets of `Server`-audience cvars also go to the cvar audit sink (settings S7) through `RemoteCVarService`; the host installs one sink, not both (the service's own warning).

### 10.5 The D26 CI rules applied to arc
1. **No fork PR reaches a self-hosted runner** (rule 1): arc adds no new trigger; arc-driven GPU and editor gates run in the same guarded jobs as the golden gate (`push` to the owner's branches, or the `head.repo.full_name == github.repository` guard).
2. **Restricted runner group, non-admin account** (rule 2): arc needs no privileges beyond launching the host and binding a loopback port; it never needs admin.
3. **Least-privilege token and scoped secrets** (rule 3): the arc token is per process and never a GitHub secret; `--format github` and `--verbose` redact it; CI logs never contain it.
4. **Pinned actions** (rule 4): no arc-specific action exists; the workflow calls `arc.exe` from the build output.
5. **Clean workspaces** (rule 5): descriptors, audit logs and `Saved/arc/out/` live in the job's workspace and die with it; a stale descriptor from a killed job is ignored by the liveness rule anyway. CI launches hosts with `arc launch --arc-idle-exit`, so an orphaned host ends itself.
6. **The CI database** (rule 5 and 6): arc never touches Postgres; the Aphelyon services are not arc hosts.

### 10.6 Dist (0.20)
`ArcaneCore/src/Arcane/Arc/` and every `ARC_REMOTE_COMMAND` are inside `#if !defined(ARCANE_DIST)`. A Dist host refuses `--arc` at parse time with exit 2 ("arc is compiled out of Dist builds"), so the flag is never silently inert. `arc.exe` is not part of a packaged Dist game. A test builds the Dist configuration's symbol list and asserts no `Arcane::Arc` symbol survives.

### 10.7 What this does not defend against
Another process running as the same user can read the descriptor and drive the host, exactly as it could read the project and inject into the process. That is the stated boundary of every surveyed design (Unity, Defold) and of Arcane's own `Saved/` files. arc does not claim more.

## 11. How it builds on the existing seams

| arc piece | Built on | Change needed |
|---|---|---|
| Launch, open-ended headless | `HostConfig` (`--headless`, `--frames`, the refusal at `HostConfig.cpp:530`) | `--arc`, `--arc-descriptor`, `--arc-idle-exit`; the refusal lifted only when armed |
| Exit reasons, verdicts | `RuntimeApp.cpp:1256-1297`, `Verdict.hpp` | `exitReason:"arc-quit"`; verdict values reused verbatim in `data.verdict` |
| Integration tests | `HostWitness` (and `s7`'s scripted stdin) | a witness mode that waits for the descriptor and returns it |
| Golden compare | `ImageCompare::CompareImages`, `ReferenceImages::ResolveReference`, `golden-gate.ps1` | none in the comparator; `capture.compare` calls it in-host |
| Captures | `CaptureGraphViewportPng` (`EditorApp.cpp`), the `--screenshot` backbuffer path | a job wrapper; the runtime host gains the viewport-equivalent path |
| Settle | `SettleBound.hpp`, `ShaderCompiler::IsIdle()` | a frame-stepped wait reusing the predicate |
| Components as JSON | `ReflectionJsonWriter`/`Reader` | none |
| Schemas | `Astra::JsonSchemaGenerator` | Astra upstream: `Optional`, defaults, `$defs`, JSON output (s5.3) |
| ECS reads | `GetComponentByHash`, `HasComponentByHash`, `InspectResources`, `Astra::Debug::Inspector` | later: D25's unified `View` |
| Addressing | `Arcane::Identity` | GUID index per world (built lazily per query in P0) |
| Cvars | `CVarRegistry` + `RemoteCVarService` (`s7`) | context parameter + structured response (0.24) |
| Console | `CVarRegistry::Execute` | none |
| Undo | `CommandStack`, `ApplyRegistryMutation`, `AssetFileOps` | explicit restore on write-phase failure (s8.1) |
| Actions | settings S4 action registry (spec s7.2) | four declared fields (0.25) |
| Problems | `Diagnostics::Publish`, `DiagnosticStore` | store moves to ArcaneCore and is installed in every host (0.26); GUID translation of entity locators |
| Logs | `Log.hpp` sinks | a sequenced subscriber sink |
| Crash facts | `.arcdiag` in `<exe dir>/diagnostics` | the client reads the directory after a death |
| Discovery | `EditorLock`'s `{pid, start}` + `ReadLive` | a sibling writer for arc descriptors; Linux start time |
| Listener | `ServiceThread`, `TcpSocket::CreateListenSocket`, `RateLimiter` | a small HTTP/1.1 parser (hand-rolled, no new dependency) |
| Token | `Crypto::GenerateSecureToken`, `HexEquals` | none |
| Reload facts | `PluginHost::Generation()` | publish the last reload's result |
| Build | arcbuild (`Exit.hpp`), the editor's `ModuleBuild.cpp` path | none for v1; JSON output waits for arcbuild II |
| Frame stats | `Host/FramePerf.hpp` | none |
| UI | `imconfig.h` test-engine define, the widget layer | `ArcUi` registry and tags (s9) |

## 12. Testing

### 12.1 How arc itself is tested
- **Unit (`[arc]`, no GPU):**
  - envelope rules (`ok` vs `errors`, the client block), error-code and exit-code tables pinned like `VerdictTest.cpp` pins verdicts;
  - descriptor write/parse/liveness, including a recycled pid with a different start time and a stale heartbeat;
  - token generation, constant-time compare, redaction (a test greps every log line and report written during a session for the token);
  - transport checks: `Origin` present refused, bad `Host` refused, non-loopback bind impossible, oversize body refused, chunked body refused;
  - the HTTP parser under **rapidcheck** (vendored) property tests: arbitrary byte streams never crash, never allocate unboundedly, and either parse or refuse;
  - registration refusals (s5.1) and schema generation for args structs, including optional fields and defaults once Astra lands them;
  - args parsing: unknown keys refused, type mismatches refused, defaults applied;
  - the dispatcher: budget respected, a busy state answers without running (asserted by a side-effect counter), frame stamps exact;
  - the permission matrix: host kind x audience x flags x cheats x read-only, reusing the settings arc's matrix test fixtures, plus `Protected` never returned in any context;
  - module lifetime with the HotReloadPlugin fixtures: a game-module command registered, dropped on unload (`command.unknown` after, `host.busy{reloading}` during), re-registered on reload, catalogue generation incremented.
- **Integration (`[arc][gpu]`, through HostWitness):** launch the staged ArcaneRuntime, ArcaneEditor and ArcaneServer with `--headless --arc` against ReferenceProject and drive each P0 verb with the real `arc.exe`. Includes the death path: `--hang-main` and `--crash-gpu` (existing diagnostics triggers) mid-command must produce exit 7 with the `.arcdiag` path, never a hang of the client.
- **The gate must be able to fail** (the golden gate's own rule): a self-test scenario deliberately expects a wrong value and asserts exit 8; a deliberately broken command asserts exit 6. CI runs it on `main`/`milestone/*`, as `golden-gate.ps1 -SelfTest`.
- **Linux:** the unit tier runs on the cloud Linux CI from P0; the integration tier follows the Linux port.

### 12.2 Latency budget
A fresh `arc.exe` process plus one loopback round trip should be about 10 to 50 ms (the research's estimate, against Unity's measured ~0.8 s per call). P0 measures it on the desk and in CI and records the numbers; `arc shell` (a warm process) is built only if the measurement says so (s13, P4).

### 12.3 Replacing named desk checks (0.23)
Each converted checklist becomes `ReferenceProject/Tests/arc/<name>.arcscenario` (or the owning project's equivalent), runs headless in CI, and the checklist doc gains a line pointing at it.

| Desk check | What arc automates | Phase | Stays human |
|---|---|---|---|
| Golden gate desk half, `scripts/desk-verify-golden-gate.ps1` phases A2 (the gate can fail) and B (bless round trip) | launch, `wait settled`, `compare`, `compare --bless`, verdicts | P0 | phase C judgement of the image itself |
| "Is this entity where I think it is" checks across arcs (the introspection goal's first example) | `entity get`, `ecs query`, `selection` | P0 | none |
| Spec-2 undo checklist `docs/2026-07-20-spec2-desk-verify-checklist.md` part A (A1-A7: edit, one-step undo, redo variants, per-component steps, redo cleared, Play keeps pre-Play history) | `ui type` on tagged Inspector fields, `undo`/`redo`, `entity get` after each step, `play`/`stop` | P1 (A1-A6 via `entity.set`), P2 (through the real Inspector widgets) | none |
| Spec-2 part B gizmo (B1-B9) | B2 (mode keys and toolbar radios), B5/B6 (one step per drag, no-move drop) via `input` drags on the viewport and `undo` | P2 | B1/B3/B8 visual alignment and constant screen size: a screenshot plus `compare` against a blessed capture, with the user approving the capture once |
| Widget layer `docs/2026-07-28-widget-layer-desk-verify.md` items 1, 3, 6 and 7 (multi-select type-then-drag is one step; sprite ppu undo; read-only AssetRef refusal; Escape reverts renames) | `ui type`, `ui act`, `undo`, `ui get` | P2 | item 4 (label-width drag "never fights imgui.ini") needs a layout-file diff, scripted in P2 |
| Settings S3 window desk check (Preferences and Project Settings at 1920x1080; tree, search, filters, reset arrow, provenance, window undo, Restart bar), and settings spec s13 "desk by automation" | `ui tree` structural goldens, `ui type` into search, `ui click` on reset arrows, `cvar explain` to confirm the effect, `screenshot window` | P2 | the look of a theme preset (approved as captures) |
| Servitor close-out desk pass (`docs/2026-08-26-servitor-closeout-and-desk-verify.md` s2) | the verdict/`exitReason` reads become `arc` envelopes; the driver-reproduction procedure stays as is | P0-P1 | the GPU-driver reproduction |

The roadmap's risk table names desk checks as the user's bottleneck; the measure of this arc's success is that each arc after it ships with its acceptance as `.arcscenario` files instead of a checklist, and the user reviews captures and failures, not steps.

### 12.4 The scenario format
```json
{ "arcscenario": 1,
  "host": { "launch": "editor", "project": "ReferenceProject", "headless": true, "backend": "dx12" },
  "steps": [
    { "call": "host.wait", "args": { "until": "settled" } },
    { "call": "entity.get", "args": { "entity": "name:MeshCube", "components": ["LocalTransform"] },
      "expect": [ { "pointer": "/data/components/LocalTransform/position/1", "approx": 0.5, "tol": 1e-6 } ] },
    { "call": "entity.set", "args": { "entity": "name:MeshCube", "values": { "LocalTransform.position": [1, 0.5, 0] } } },
    { "call": "editor.undo" },
    { "call": "entity.get", "args": { "entity": "name:MeshCube", "components": ["LocalTransform"] },
      "expect": [ { "pointer": "/data/components/LocalTransform/position/0", "approx": 0.0 } ] },
    { "call": "capture.compare", "args": { "reference": "editor-ui" },
      "expect": [ { "verdict": "green" } ] }
  ] }
```
- **Expectations:** `equals`, `approx` (+`tol`), `exists`, `absent`, `matches` (regex on a string), `verdict` (`green` or a named verdict), `exitCode`. Back-references `$2.data...` resolve earlier results. Nothing else: no variables, loops, conditionals or arithmetic.
- `arc run` prints one envelope whose `data` holds per-step results and the first failing step, writes JUnit XML beside it for CI, and exits 8 on a failed expectation, 6 on a step error.

## 13. Phased delivery

Each phase ends at a gate (the full suite, the golden gate, and the phase's acceptance), as the settings arc's tranches do. ABI bumps are cheap and taken whenever exported layouts change.

**Prerequisites before P0:** the settings arc merged to `main` (contexts, `Arcane::Paths`, `CVarRegistry::Snapshot`); the 0.24 amendment to `RemoteCVarService`; the 0.25 amendment to the S4 action-registry spec (needed by P1); the Astra schema changes (s5.3) upstream and synced.

**P0, jack-in core (the smallest useful slice).** Goal: one desk check becomes a CI gate with no log scraping.
- `ArcaneCore/src/Arcane/Arc/`: listener, transport checks, token, descriptor with heartbeat, dispatcher with state gate and budget, registry with self-description, internal jobs.
- Hosts: `--arc`, `--arc-descriptor`, `--arc-idle-exit`; open-ended `--headless` when armed; `editor.arc.enable`; `exitReason:"arc-quit"`; minimized handling (0.27); Dist refusal.
- `arc.exe`: status, launch, quit, wait (`ready`, `settled`, `frames`, `state`), world list, commands, list, describe, schema, call; formats `json`, `human`, `ndjson`; the exit-code table.
- Verbs: `cvar get|set|list|explain`, `console`, `logs` (with the new sink), `problems` (with the Core store), `entity get`, `ecs query` (read-only), `ecs stats`, `resource list|get`, `scene tree`, `selection`, `stats frame`, `screenshot viewport|window`, `compare`.
- **Acceptance:** a CI script launches a headless editor on ReferenceProject, waits for `settled`, reads `MeshCube`'s `LocalTransform`, sets a `PlayerSafe` cvar and reads it back with `explain` provenance, compares against the `editor-ui` reference with the lane's own capture source, reads `problems`, and quits, using only envelopes and exit codes; the same against a headless runtime and a server (cvars, ecs). The `[arc]` unit and integration suites are green, and the self-test proves the gate can fail.

**P0 additions (2026-10-09):** `play|stop|pause|resume|step` (from P1; undoable:false, declared per 0.16), AM1 tick semantics, AM2 `--all` fan-out, AM4 `--json`, AM5 descriptor DACL, AM6 exit mapping, `ecs.archetypes`. Acceptance adds: a headless runtime and a server launched together, `arc --all cvar get ...` answering from both, and `arc play`/`step 10`/`stop` on the headless runtime with `host.frame` advancing exactly 10.

**P1, act.**
- `entity set|add|remove` (transactional component edits), `entity spawn|despawn|duplicate` (structural, one step each), `batch` (component-only transactional; structural members refused with `undo.batch-structural`), `undo`, `redo`.
- `action list|run` over the S4 registry; `select`, `open`, `play|stop|pause|resume|step` with `--topology` and `--world`; `input` injection; game-module commands with `commands-changed`; `asset list|info|refs`; `probe`.
- **Acceptance:** spec-2 checklist A1-A6 as a scenario ("edit, undo, assert restored; edit two components, undo twice"); a ReferenceGame module command registered, called, hot-reloaded and called again.

**P2, see.**
- `ArcUi` registry over the ImGui hooks; tags in the widget layer, Inspector, Outliner, toolbar, menus and settings rows; `ui tree|get|act|wait`; `screenshot panel`; structural UI goldens; `arc run` and the scenario format.
- **Acceptance:** the widget-layer checklist items 1, 3, 6 and 7 and the settings S3 window check run headless as scenarios; `AssetStatusPanelClickTest` moves onto the registry and deletes its re-derived layout math.

**P3, build, test and jobs.**
- `build`, `reload`, `generate`; `--detach` and `arc job *`; `watch entity`; `test`; `gate golden`; `--format github`.
- **Acceptance:** an agent loop "edit game code, `arc build`, `arc wait reload`, `arc call` the module command, `arc test [filter]`" with no manual step; a GitHub Actions job on the self-hosted runner annotates a failed scenario inline.

**P4, reach (each item decided on evidence, not scheduled by default).**
- `arc shell` (a warm ndjson process) if P0's latency measurement justifies it.
- `arc mcp` if 0.21 is revisited.
- `arc profile capture` shelling out to `tracy-capture` (D13; with `TRACY_ONLY_LOCALHOST` set in Arcane builds).
- A checked-in agent skill (`.claude/skills/arcane-arc/SKILL.md`) and `arc skill show` printing it for Codex and Grok.
- A pipe/AF_UNIX transport if sandboxes force it (0.1).

**Sequencing against the roadmap:** D27 runs after D14 closes and ahead of D1. P0 alone already removes the golden-gate desk half and the introspection goal's state-query example; P1 depends on S4 and is best after lane B's entity-record undo arc, which also lifts the batch restriction.

## 14. Risks

| Risk | Mitigation |
|---|---|
| **Scope.** The catalogue is wide, and every engine arc will want commands. | Phases with acceptance gates; P0 is small on purpose; game and engine arcs add commands through the same macro, with no arc-arc work. |
| **Main-thread stalls** from a heavy command (a big query, a large tree). | Per-frame budget, limits and artifacts for large results, a 10x-budget warning naming the command, and frame-spanning jobs for anything that waits. |
| **Agent sandboxes block loopback** (Codex blocks localhost by default; Unity had to ship `unity mcp configure codex`). The roadmap's "serves every agent CLI uniformly" depends on it. | Document the sandbox setting per agent CLI in the arc skill; keep `IArcTransport` so a pipe transport can land in P4 without redesign; P0 tests each agent CLI on the desk once and records the result. |
| **UI automation flakiness** (stale refs, moving widgets, timing). | Strict mode, actionability checks with named failures, stable tags for desk-check targets, frame-exact waits, and structural goldens instead of pixel goldens where layout is the question. |
| **Dependency slip:** S4's action registry, the entity-record undo arc, Astra's schema changes. | P0 needs none of the three except the Astra change, which is small and upstream-first; P1's structural batches wait for the undo arc by an explicit refusal, not a workaround. |
| **Security regressions** (a future change binds `INADDR_ANY`, logs the token, or accepts `Origin`). | Each rule has a test that fails on regression (s12.1); the Dist symbol test; the audit log. |
| **Protocol drift** (Bevy's 0.17 rename broke every client). | `/arc/v1` plus `capabilities[]`; names are stable once shipped and renamed only with an alias that warns, as cvar aliases do. |
| **Name collision.** `arc` is also Phabricator's Arcanist CLI, still installed on some machines. | `arc.exe` is not put on PATH by default (like arcbuild); scripts call it by path; `arc --version` prints "Arcane arc" so a collision is obvious. Revisit the name if it bites. |
| **ImGui hook cost** with large trees (the node editor, long Outliners). | Recording only while a UI subscriber is active; a per-frame item cap with a `truncated` flag; measured in P2 against the 1080p/144 fps budget's editor equivalent. |
| **Two builders** racing on one `Binaries/` slot. | 0.22: route through the attached editor's action. |

## 15. Out of scope
- An MCP server or adapter in v1 (0.21).
- `eval` and any embedded script language.
- Remote (non-loopback) control, remote UI, multi-user sessions.
- arc in Dist builds.
- Render-graph introspection and decision tracing (Appendix A).
- GPU frame capture (`arc gpu capture` through RenderDoc) and Tracy capture beyond P4's shell-out.
- The Hub using arc (it keeps `editor.lock`; the editor makes no process calls to the Hub and the Hub none to the editor).

---

## Appendix A. Open questions (not blocking the decisions above)

1. **Render-graph introspection.** The introspection goal's second example ("what did the render graph resolve this frame") needs the graph to publish its resolved pass list. Proposed as `render.graph` in P3 if the D1 render-foundation spec exposes the list; otherwise deferred to D1 itself.
2. **Rendering while minimized** (0.27 option a): render the viewport graph offscreen while the chrome swapchain is minimized, so captures work from a minimized editor. Needs its own small change in `EditorAppFrame`.
3. **WSL2.** A Linux-side agent reaching a Windows host needs WSL mirrored networking; with NAT networking `127.0.0.1` is not shared. Document, or add a `--arc-bind` override restricted to the WSL virtual adapter? Default: document only.
4. **Linux process start time** for the liveness rule (`EditorLock::Info::start` is 0 off Windows today); the Linux port should supply `/proc/<pid>/stat` field 22 for both the lock and descriptors.
5. **Several editors on one project** are refused today by `RivalPid`; several runtimes are not. Does `arc launch runtime` refuse a second runtime on a project unless `--allow-multiple`? Default: allow, and rely on strict target selection.
6. **The editor's own console as an arc client.** Should the editor Console tab accept `arc ...` lines (so a human can try a command where they see the result)? Default: no in v1.
7. **Per-agent identity** in the audit log is self-declared (`X-Arc-Client`). Good enough for a single-user desk; revisit for shared machines.
8. **Server-safe entity writes.** Which `entity.*` commands a dedicated server permits in `ServerAdmin` (a GM tool) is a replication-arc question (D10); v1 refuses them on servers.
9. **Arcbuild JSON.** When arcbuild II's unified output policy lands, `arc build` should consume arcbuild's structured output rather than its exit code plus text.

## Appendix B. Research claims corrected against the source

The research document (`docs/research/2026-10-05-arc-cli-research.md`) is committed as written. These are the points where the source says otherwise; this spec follows the source.

1. **`CommandStack::Cancel` does not revert** ("discard the open transaction (no push, no revert)", `CommandStack.hpp:111`). The research's batch rule ("on any failure the whole batch is `Cancel`led and reverted") needs explicit restoration (s8.1, s8.3).
2. **Structural edits cannot join a transaction today:** structural mementos refuse to run inside an open gesture (`CommandStack.hpp`, `InTransaction`). The research's "a batch = one transaction = one Ctrl+Z" holds only for component edits until the entity-record undo arc lands.
3. **"The schema and the parser cannot drift" is not yet true:** `JsonSchemaGenerator` marks every non-hidden field `required` and emits no defaults (`includeDefaults` is "not supported yet"), while `ReflectionJsonReader` accepts missing keys. Needs the s5.3 Astra change.
4. **`EditorLock`'s validating call is `ReadLive` (and `RivalPid`),** not `IsAlive()`; `IsAlive()` appears only in a comment (`Project.hpp:155`).
5. **There is no `Json.hpp` in ArcaneCore;** the JSON library is the vendored nlohmann/json.
6. **`DiagLocator::Entity` holds a packed Astra handle,** not a GUID (gap 4), so "`DiagLocator` is already a cross-panel address form" needs a GUID translation for arc's wire.
7. **The diagnostics sink is one process-wide slot that only the editor fills;** `DiagnosticStore` is an editor type. `arc problems` on a runtime or server has no data source today (0.26).
8. **A log ring already exists** (the 512-line crash backlog in `Log.hpp`), but it is lock-free with torn reads and no sequence numbers, so it cannot back `logs --follow`; a separate sink is still needed.
9. **`PluginHost` already has `Generation()`;** what is missing is the last reload's outcome, not the counter.
10. **The action registry is the settings arc's S4 tranche** (specified in spec s7.2); the research's Q23 calls it "S7.2". It exists on no branch yet.
11. **`CreateListenSocket` defaults to `INADDR_ANY`,** so "bind 127.0.0.1 only" is an explicit argument the listener must pass, not a default it inherits.
12. **Owed defect 2** (the editor's verify capture depending on `Saved/Diagnostics/`) is **closed** (`scripts/desk-verify-golden-gate.ps1` header; re-blessed 2026-08-30, 97abd074). The research's claim that structural UI goldens "cover owed defect 2" does not apply; they are justified on brittleness alone.
13. **The context model (`CVarContext`, `Audience`) is not on `main`;** `main` still has the v1 `Permission` parameter. The research reads it from the settings spec and `feat/settings-s7`, correctly, but arc cannot land before the merge.
14. Minor: the host flag set also includes `--pick-probe`, `--perf`, `--no-vsync`, `--print-engine-info` and the deprecated, accepted-and-ignored `--nri-graph`; the plugin ABI on `main` is 51 (52 on the settings branches), so the research's descriptor example's `abi: 52` is the post-merge value.
