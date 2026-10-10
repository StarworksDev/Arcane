#pragma once
// HostConfig: the typed result of parsing a runtime host's command line. Owns
// the host option vocabulary; delegates the parsing to the generic Arcane::Cli
// (Core). PRESENTATION-FREE + C++23-clean.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <Arcane/Base/Api.hpp>
#include <Arcane/Base/DiagnosticsSettingsData.hpp>   // kHangMainSeconds derives from hangSeconds
#include <Arcane/Core/Constant.hpp>
#include <Arcane/Render/GraphicsBackend.hpp>   // Arcane::GraphicsBackend
#include <Arcane/Sim/SimSettingsData.hpp>   // fixedDtSeconds' default is one sim.fixedHz step
namespace Arcane
{
    struct ARC_API HostConfig
    {
        // backend and vsync are what the process boots with: Parse fills them
        // from --backend / --no-vsync, and HostBoot::ApplyEarlyConfigRungs
        // replaces them with the published render.backend / render.vsync
        // (which those flags feed, on the CommandLine rung).
        GraphicsBackend backend   = kDefaultGraphicsBackend;   // D3D12 on Windows, Vulkan elsewhere
        bool            backendSupplied = false;   // --backend was on the command line
        std::uint64_t   maxFrames = 0;             // 0 = run until quit
        bool            vsync     = true;
        bool            perf      = false;
        // Empty = "no explicit --plugin": the project's gameModule is what loads. The editor
        // tolerates having nothing to host (project-less workshop); ArcaneRuntime refuses.
        std::string     pluginPath = "";
        std::string     projectPath = "";   // .arcproj or project folder; "" = data/-next-to-exe
        // Boot this scene (asset Guid text) instead of the manifest's bootScene. Empty = follow
        // the manifest. Editor separate-window play passes the ACTIVE scene here.
        std::string     sceneOverride = "";

        // Write the LAST rendered frame to this PNG before exiting. Empty = off.
        //
        // This is the scripted GPU-verify's missing half. `--frames N` proved a host
        // could run N frames without a validation error; it could not tell anyone
        // WHAT was on screen, so "the scene renders" stayed a desk-only claim and a
        // scene that drew every sprite as a single pixel (the 2026-07-30 TypeContext
        // bug) looked identical to a healthy run in the log. Pairs with --frames:
        // `--frames 60 --screenshot out.png` is an unattended visual check.
        //
        // Captures the BACKBUFFER, after tonemap and ImGui -- the actual pixels a
        // player sees, not an intermediate the eye never gets.
        std::string     screenshotPath = "";

        // Print one line of engine-identity JSON to stdout and exit, without creating
        // a window or device. The Arcane Hub probes this to learn the plugin ABI it
        // must stamp into a new .arcproj -- see HostBoot::EngineInfoJson.
        bool            printEngineInfo = false;

        // --headless. Render the real frame graph with NO window shown and NO
        // swapchain. Named for what every comparable tool calls this mode
        // (Chrome: "creates, but doesn't display, any platform windows"), so a
        // user or agent reaching for the obvious flag finds it. The word was
        // only unavailable while the engine spent it on DEVICE-LESS, which this
        // mode is not; that sense is now spelled `device-less`, and the flag's
        // previous spelling was REMOVED outright -- no alias, refused as an
        // unknown argument (docs/specs/2026-08-27-headless-rename-design.md).
        //
        // "Offscreen" survives as the name of the TECHNIQUE, deliberately:
        // OffscreenVehicle, CreateOffscreen(), IsOffscreen(), the [offscreen]
        // log tag. --headless is the MODE a caller asks for; rendering to an
        // offscreen target with no swapchain is HOW it is delivered, and that
        // is the more precise word for that layer.
        //
        // REQUIRES --frames N, refused at parse time otherwise: an unmapped
        // window has no close button and cannot be focused for the `quit`
        // action, so an open-ended headless run has no exit of its own. It
        // also REFUSES --pick-probe, which an offscreen context declines to
        // arm. Both refusals carry their full reasoning at the check in the
        // .cpp.
        bool            headless = false;

        // Fixed simulation delta, seconds. --headless only, refused elsewhere. The
        // host loop is otherwise wall-clock (RuntimeApp.cpp:331), which makes
        // `--frames N` advance the sim by however long those N frames happened to
        // take -- different on a loaded CI box than an idle desk.
        // Defaults to one sim.fixedHz step at its declared default (settings
        // arc S6-8): the same double as the old 1.0 / 60.0 literal.
        double          fixedDtSeconds = 1.0 / SimSettings{}.fixedHz;

        // Whether --fixed-dt was actually typed on the command line, as opposed
        // to fixedDtSeconds simply holding its registered default. Needed
        // because that default (1/60) is a legitimate value a caller could also
        // pass explicitly -- comparing the RESOLVED value against the default
        // cannot tell "never passed" from "passed the default on purpose" apart
        // (the same r.Supplied() reasoning HostConfig.cpp already applies to
        // this exact flag internally). A host that refuses --fixed-dt when it
        // does nothing (ArcaneEditor's main.cpp) needs this to avoid refusing
        // every ordinary run that never mentioned the flag at all.
        bool            fixedDtSupplied = false;

        // --fixed-time: pin the ABSOLUTE clock the scene sees, so `Time` stops
        // being a function of the frame count. --fixed-dt pins the STEP; this
        // pins the CLOCK. Without it, changing --frames 60 to --frames 90
        // re-shades anything animated on Time (ReferenceProject's PulseSprite
        // computes sin(Time * PulseSpeed)) and silently invalidates a blessed
        // reference.
        //
        // Optional-absent, NOT zero-defaulted: 0.0 is a legitimate pin and must
        // be distinguishable from "not asked for".
        std::optional<double> fixedTimeSeconds;

        // Raw, unparsed `--probe` arguments in command-line order. Parsed by
        // VerifyReport, not here: the kinds are that component's vocabulary.
        std::vector<std::string> probes;

        // Repeatable `--set name=value`. Applied at SetBy::CommandLine after
        // the config layers. Not a cvar itself.
        std::vector<std::string> cvarSets;

        // --settle N. --headless only, refused elsewhere -- same idiom as
        // fixedDtSeconds above -- and requires --screenshot or --report too
        // (RuntimeFrame.cpp's CaptureTail has nowhere to land the comparison
        // otherwise, and the loop would never know when to stop). 0 = off.
        //
        // fixedDtSeconds fixes TIME; it does not fix CONTENT. Async shader
        // compiles finish on a WALL-CLOCK schedule the sim clock does not
        // control: ShaderCompiler::Poll's `readyAt` is measured in sim
        // seconds, so DISPATCH is frame-indexed and deterministic, but the
        // compile itself runs on a background thread and completes on real
        // milliseconds. A `--frames N` run landing its capture before that
        // finish shows the untextured fallback; one landing after shows the
        // bound material -- same binary, same flags, back to back (see
        // .superpowers/sdd/2026-08-23-agent-verification-offscreen-hosts/
        // task-6-report.md's bisection: the boundary is exactly 12 frames,
        // 0.2s debounce / (1/60s fixed dt)).
        //
        // N > 1 (>= 2; a bare 1 is refused at parse time -- one attempt has
        // nothing to compare against and would always fail) means: once the
        // ordinary --frames budget above is spent, FREEZE the render/sim
        // clock (RuntimeFrame.cpp's AdvanceSim) and keep re-rendering that
        // same instant -- so nothing but async resource state can still
        // change frame to frame -- capturing again each time, paced by
        // kSettleIntervalMs, until two CONSECUTIVE captures compare BYTE-EQUAL
        // AND the shader compiler reports IsIdle() (fix round 1, item 1 -- byte
        // equality alone is a STABILITY predicate, not the QUIESCENCE one
        // this mode actually needs: Drain() is not clock-gated, only Submit's
        // debounce dispatch is, so two frozen frames can agree while a
        // dispatched-but-undrained compile is still genuinely in flight).
        // Freezing the clock is load-bearing, not an optimisation:
        // ReferenceProject's PulseSprite material shades as a function of
        // Time (Content/materials/pulse_sprite.arcmat), so comparing
        // un-frozen frames would never converge on anything -- every frame
        // would legitimately differ regardless of whether the compile race
        // has resolved.
        //
        // N IS THE ATTEMPT HALF OF THE BAIL CONJUNCTION, never a standalone
        // cap. The loop gives up only when BOTH N and settleTimeoutMs are
        // spent -- settleTimeoutMs's own comment, further down in this same
        // struct, states the identical rule, and Arcane/Host/SettleBound.hpp
        // is the shared predicate both hosts call. A bare N stops nothing on
        // its own.
        //
        // Byte equality, never a tolerance: same mode, same format, same
        // capture path every attempt, so an honest bitwise compare is
        // correct, and a perceptual comparator would hide exactly the
        // failure this flag exists to catch (that is a later plan's job).
        //
        // NON-CONVERGENCE IS A FAILURE, reported explicitly -- exitReason
        // "settle-not-converged" in the JSON report and a nonzero process
        // exit code -- rather than silently handing back whatever the last
        // attempt happened to render. See RuntimeFrame.cpp's CaptureTail and
        // RuntimeApp.cpp's ShutdownGraphPath.
        std::uint64_t settleAttempts = 0;

        // --settle-timeout MS. The TIME half of settle's bail conjunction; the
        // loop gives up only when BOTH this and settleAttempts are spent (see
        // Arcane/Host/SettleBound.hpp). 0 means "no time bound", degrading to
        // the old attempts-only behaviour rather than becoming unsatisfiable.
        //
        // 5000 is INHERITED: Playwright's toHaveScreenshot timeout "Defaults to
        // timeout in TestConfig.expect", and that default is 5 seconds. The
        // 50ms poll interval beside it (kSettleIntervalMs) is OURS -- do not
        // present the two as having the same provenance.
        std::uint64_t settleTimeoutMs = 5000;

        // --compare <name>. A bare reference NAME, resolved through
        // Arcane::ResolveReference against the project root. Empty = no
        // comparison. Refused without --settle, refused on an explicit empty
        // value, and refused on an unsafe name (a path separator, "..", or a
        // leading '.') -- all three at parse time; see HostConfig.cpp.
        //
        // Wired into RuntimeFrame.cpp's settle loop as a THIRD convergence
        // conjunct (byteEqual && idle && matchesReference) -- see that
        // file's CaptureTail. --bless (below) DISABLES that conjunct for the
        // whole run rather than gating on it: blessing exists to REPLACE the
        // reference, so the stale one on disk must never also be the
        // convergence goal, or an intentional visual change (or a first
        // bless, with nothing yet on disk) could never converge at all.
        std::string compareReference;

        // --bless. Accept the converged capture as the reference --compare
        // names, writing to the level ResolveReference's own resolution
        // named (RuntimeApp::ShutdownGraphPath). Refused without --compare
        // (Rule 3: no silently inert flags) -- see HostConfig.cpp.
        bool bless = false;

        // The two aggregate knobs CompareImages accepts, both unset by
        // default -- which means a budget of ZERO differing pixels
        // (Playwright's own default). When both are set the SMALLER budget
        // wins (ImageCompareOptions' own contract). Refused without
        // --compare, same as --bless. maxDiffPixelRatio is additionally
        // RANGE-refused at parse time -- finite, and in [0, 1] -- because it
        // reaches an unsigned narrowing cast in ImageCompare.cpp where a
        // negative/NaN/huge double is undefined behaviour rather than a
        // saturating clamp (final-review I-1). A value present here is
        // therefore always a usable fraction; downstream need not re-check it.
        std::optional<std::uint64_t> maxDiffPixels;
        std::optional<double>        maxDiffPixelRatio;

        // Write the observation report to this JSON path. Empty = off.
        std::string     reportPath = "";

        // --dump-layout <path>. Write the LIVE ImGui layout to this .ini at
        // shutdown -- the authoring half of the committed
        // ReferenceProject/Saved/verify-layout.ini seed (RetargetLayoutIni's
        // pin, EditorApp.cpp). Empty = off. EDITOR ONLY: ArcaneRuntime has no
        // ImGui layout to dump and refuses this flag outright (main.cpp) --
        // a flag that parsed clean and silently did nothing would be exactly
        // the failure Plan A's Task 12 closed.
        //
        // Deliberately NOT folded into HostConfig.cpp's wantsOffscreenOnly
        // predicate: windowed authoring is the NATURAL flow (arrange the
        // panels by hand, dump, inspect), and Step 2 of the task that added
        // this ALSO requires it to work under --headless, where the point
        // is authoring a seed with no windowed session at all. It must work
        // in both -- adding it to that predicate would refuse the windowed
        // case for no reason.
        std::string     dumpLayoutPath = "";

        // --play-as <standalone|listen-server|embedded-server|client>. Enter Play
        // at the END of boot in that TOPOLOGY (Arcane::Editor::PlayTopology), rather
        // than sitting in Edit mode -- the scripted half of the play-mode picker the
        // Core-DLL split's Task 7 grew. Empty = off, which is the ordinary Edit-mode
        // boot every other run performs.
        //
        // EDITOR ONLY, and refused outright by ArcaneRuntime (its main.cpp, beside
        // --dump-layout's refusal): the runtime host has no Edit mode to leave and no
        // PlaySession to enter, so parsing it and shrugging would exit 0 having
        // silently done nothing -- the exact silent-success failure this file's
        // rule 3 refuses everywhere else.
        //
        // NOT a Cli::Choices() list, deliberately: the registered default is the
        // EMPTY string and an empty value is not one of the choices, so the value
        // check is a post-parse refusal (beside --fixed-dt's) rather than a
        // registration-time constraint that would reject every ordinary run.
        std::string     playAs = "";

        // --view-mode: the editor viewport's mode at boot, "2d" | "perspective"
        // (F4 plan 1 T7). EMPTY = no seed: the persisted [EditorViewport]
        // [Camera] block (or its 2D default) stands. When supplied it is
        // applied AFTER that block is read, so a scripted run's flag beats
        // whatever a desk left persisted. A Cli::Choices() list, unlike
        // --play-as above: Choices validates only a SUPPLIED value, so the
        // empty default passes untouched and any other spelling is refused
        // at parse time (rule 3). EDITOR ONLY, refused by ArcaneRuntime's
        // main.cpp beside --play-as: the runtime has no editor camera to seed.
        std::string     viewMode = "";

        // --select-name: editor only. Select the entity whose Identity.name
        // matches at the end of boot (after the boot scene is loaded), then
        // frame it. Empty = no seed. A missing name is a loud boot WARN, not
        // a silent skip -- scripted gizmo/outline captures would otherwise
        // look like a "no selection" golden. ArcaneRuntime refuses the flag
        // (no editor selection).
        std::string     selectName = "";

        // --tool: editor only. Viewport tool at boot: select | move | rotate |
        // scale. Empty = Select (the ordinary default). Same Choices() empty-
        // default rule as --view-mode. Pairs with --select-name for a scripted
        // gizmo capture. ArcaneRuntime refuses the flag.
        std::string     tool = "";

        // --open-asset: editor only. Open the document for this asset Guid at
        // the end of boot (after the project is open), through the same
        // OpenAssetDocument path the Asset Browser's double-click takes -- so
        // a scripted capture can put an .arcinput / .arcshader / mesh
        // document on screen with no desk input. Empty = none. An unknown or
        // unresolvable Guid is a loud boot ERROR, not a silent skip, for the
        // same reason --select-name is. ArcaneRuntime refuses the flag.
        std::string     openAsset = "";

        // --select-asset: editor only. Select this asset Guid in the Asset
        // Browser at the end of boot, the way a click does -- its page shows
        // in the Assets Inspector (inspector filters s6). Validated through
        // the project (the browser's model is not built yet at boot); an
        // unresolvable Guid is a loud boot ERROR, the --open-asset rule.
        // ArcaneRuntime refuses the flag.
        std::string     selectAsset = "";

        // --select-in-document: editor only, and only WITH --open-asset. A
        // human path inside the opened document -- "<map>[/<action>[/<binding
        // index>]]" for an input asset -- selected after the open, so a golden
        // can show that document's page in the Inspector (inspector-ownership
        // spec s4). Refused at parse without --open-asset (nothing to select
        // inside); an unresolvable path is a loud boot ERROR, the same rule as
        // --select-name. ArcaneRuntime refuses the flag.
        std::string     selectInDocument = "";

        // --window-size WxH (T3-D6 fix round 1): the host window's pixel
        // extent -- and so, under --headless, the offscreen chrome frame's and
        // the default dock layout's, which both follow the window -- for
        // automation that must judge the editor at a real desk geometry
        // (`--window-size 1920x1080`). AUTOMATION ONLY: refused without
        // --frames N, and stripped from a relaunch line (SanitizeRelaunchLine).
        // 0 x 0 = unset = GpuContext's 1280x720 default, which every golden
        // reference is captured at and therefore stays load-bearing. Each side
        // is refused outside [kMinWindowSide, kMaxWindowSide].
        ARC_CONSTANT("validation bound: the range metadata of the render.window.* size flags, not a setting")
        static constexpr std::uint32_t kMinWindowSide = 64;
        ARC_CONSTANT("validation bound: the range metadata of the render.window.* size flags, not a setting")
        static constexpr std::uint32_t kMaxWindowSide = 8192;
        std::uint32_t   windowWidth  = 0;
        std::uint32_t   windowHeight = 0;

#if !defined(ARC_BUILD_DIST)
        // DEV ONLY: fire the deliberate GPU fault (Render/GpuFaultInjector.hpp)
        // ONCE, on the first frame recorded after this many frames have
        // completed -- `--crash-gpu 30` faults during frame 31, which is the
        // point of the number: enough warm-up that the device, swapchain and
        // shaders are all genuinely live. 0 = off.
        //
        // The editor reaches the same injector through a menu item; ArcaneRuntime
        // has no menu, and the desk battery's item 2 -- "same, via ArcaneRuntime
        // (project-less fallback dir)" -- is unrunnable without a trigger. A flag
        // rather than a keybind because the battery is a scripted, repeatable
        // check: `ArcaneRuntime --project ReferenceProject --crash-gpu 30` is one
        // line, and it pairs with --frames the way --screenshot does.
        //
        // Honoured by BOTH hosts. The editor also has the menu item, but a flag
        // that one host parses and silently ignores is a trap, and scripting the
        // editor's battery items beats clicking them.
        //
        // `--crash-gpu N` ALONE reaches the dispatch -- the NRI frame graph
        // is the only render path, so it takes no second flag to select. It
        // fires NriDiagnostics::FireFault (the data/shaders/gpu_fault.hlsl TDR
        // loop, as a one-off NRI compute dispatch) once, with a
        // `pass:gpu-fault` breadcrumb and an ERROR when the injector is
        // unavailable. See RuntimeFrame.cpp's RenderGraph for the two
        // topology-forced properties -- own command buffer, fired before the
        // frame is declared -- neither of which is observable in the report.
        std::uint64_t   crashGpuFrame = 0;

        // DEV ONLY (crash window plan 2, D10): on frame N the MAIN thread
        // sleeps kHangMainSeconds without beating, then carries on -- the
        // scripted trigger for the hang report + reporter hand-off (spec
        // s5.4), the way --crash-gpu is the trigger for gpu-crash. Fires once.
        // 15 s clears Diagnostics::Config::hangSeconds' 12 s default by 3 s;
        // honoured by BOTH hosts (a flag one host silently ignores is a trap).
        std::uint64_t   hangMainFrame = 0;

        // DEV ONLY: `--pick-probe x,y` -- the SCRIPTED desk check for the
        // graph's pick + JFA outline nodes.
        //
        // Turning it on does three things, and all three are the point:
        //   1. the frame grows the pick node (an R32_UINT entity-id target +
        //      a 1-pixel readback) and the outline chain (seed -> N jump-flood
        //      steps -> a composite over the tonemapped backbuffer). With the
        //      flag ABSENT the frame's shape is unchanged, so the probe cannot
        //      perturb what an ordinary run renders;
        //   2. the SELECTION is scripted -- the first pickable drawable in the
        //      scene (hit-proxy id 1) -- because this host has no editor
        //      selection to read;
        //   3. the run prints the id read back at (x, y) and exits 0 on a HIT
        //      (a non-zero id) or 1 on a MISS, so a desk battery item is one
        //      scriptable line instead of an eyeball.
        //
        // Refused at parse time without --frames N (the readback lands with
        // frames-in-flight latency, so an open-ended run would never report
        // and would exit on a window close instead). The pick/outline nodes
        // themselves are always available: the NRI frame graph is the only
        // render path.
        bool            pickProbe  = false;
        std::int32_t    pickProbeX = 0;   // canvas px, y-down; only read when pickProbe
        std::int32_t    pickProbeY = 0;
#endif

        // Forward-declared here so it names HostConfig::ParseOutcome and can be the
        // return type of Parse; DEFINED below (after the class closes) because its
        // std::optional<HostConfig> member needs HostConfig to be a complete type,
        // which it is not yet inside this member specification (MSVC enforces this).
        struct ParseOutcome;

        // Builds the Cli spec, parses argv, maps Result -> HostConfig.
        // --help / bad args -> { nullopt, exitCode }; success -> { config, 0 }.
        static ParseOutcome Parse(int argc, char** argv);
    };

    struct HostConfig::ParseOutcome { std::optional<HostConfig> config; int exitCode = 0; };

#if !defined(ARC_BUILD_DIST)
    // DEV ONLY (crash window plan 2, D10): the duration --hang-main blocks the
    // main thread for. DERIVED (settings arc S6-2): diagnostics.hangSeconds'
    // default (12 s) plus a 3 s margin, so the watchdog's hang report is
    // reliably provoked.
    ARC_CONSTANT("the --hang-main margin over diagnostics.hangSeconds' default")
    inline constexpr std::uint32_t kHangMainSeconds = DiagnosticsSettings{}.hangSeconds + 3u;
#endif

    // The RELAUNCH line a host hands to Diagnostics::Config::commandLine (crash
    // window plan 1, spec S5.1): this process's own argv with the capture
    // harness taken out, so the crash reporter's "restart" re-runs the SESSION
    // rather than the scripted run that produced the report. A crashed
    // `--headless --frames 900 --compare golden` must come back as the plain
    // project it was rendering, not as another 900-frame comparison.
    //
    // Two rules, and the second is the one a naive "drop --frames" gets wrong:
    //   1. every run-shaping / capture flag is stripped, in BOTH spellings Cli
    //      accepts (`--frames 900` and `--frames=900`);
    //   2. a stripped flag takes its DEPENDANTS with it, because
    //      HostConfig::Parse refuses each of them on its own -- --probe/
    //      --report/--settle/--settle-timeout/--compare/--fixed-dt/--fixed-time
    //      all require --headless, --probe/--screenshot require --frames, and
    //      --bless/--max-diff-* require --compare. A line that kept an orphan
    //      would exit 2 instead of relaunching anything, which is a worse
    //      answer than offering no button at all.
    //
    // Everything else is kept verbatim (--project, --plugin, --scene,
    // --backend, the editor-only seeds), and an argument containing a space is
    // quoted. Pure: no host state, no parse, no side effects -- which is what
    // makes it unit-testable (ArcaneTests/src/HostConfigTest.cpp, "[host]").
    [[nodiscard]] ARC_API std::string SanitizeRelaunchLine(std::span<const std::string> argv);
}
