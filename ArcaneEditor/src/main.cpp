// Arcane Editor -- the editor shell entry point. Parses argv into a HostConfig
// (shared with ArcaneRuntime), constructs the EditorApp object, and returns
// its Run() exit code. All engine boot, the frame loop, and the load-bearing
// teardown order live in EditorApp (EditorApp.hpp/.cpp); main is just the
// wire-up (mirrors ArcaneRuntime/src/main.cpp).

#include <Arcane/Base/Assert.hpp>
#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Base/DiagnosticsSettings.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Platform/Platform.hpp>
#include <Arcane/Project/Project.hpp>   // EditorLock: the direct-launch double-open guard
#include <Arcane/Config/CVarTypes.hpp>
#include <Arcane/Host/BootSplashWindow.hpp>
#include <Arcane/Host/EarlyConfig.hpp>
#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Render/AgilitySdk.hpp>
#include <Arcane/Host/ProjectBoot.hpp>   // HostBoot::EngineInfoJson (the --print-engine-info probe)
#include "App/EditorApp.hpp"
#include "Input/EditorActions.hpp"   // the shortcut cvars, declared before the early config rungs
#include "App/HostPresentation.hpp"   // HostPresentationFor: the splash/activation rule (T3-D6 fix round 1)

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
#include <shobjidl.h>
#pragma comment(lib, "shell32.lib")

// The Arcane taskbar GROUP id -- deliberately the family root, not this
// app's own identity. The naming scheme (2026-07-29): dev.starworks.arcane
// is the shared group every Arcane process claims for taskbar stacking;
// per-APP identities hang beneath it (the Hub's installer identity is
// dev.starworks.arcanehub; dev.starworks.arcaneeditor is reserved for this
// editor the day it gains an installer or file-association identity).
// MIRRORED constant: the Hub claims the same group id
// (spawn::APP_USER_MODEL_ID, Arcane/Hub/src-tauri/src/spawn.rs) -- change
// BOTH or the Hub's and the editors' taskbar buttons stop stacking.
static constexpr const wchar_t* kAppUserModelId = L"dev.starworks.arcane";
#endif

// Agility SDK handshake: the exported version/path pair (Arcane/Render/AgilitySdk.hpp).
// The macro is empty off Windows.
ARC_AGILITY_SDK_EXPORTS();

// ===== THE EDITOR'S FULL PROCESS EXIT-CODE TABLE ============================
// Gathered in ONE place. This function's own `return rc;` at the bottom is the
// single point every path below drains through, which is what makes this
// comment honestly exhaustive rather than a claim about code scattered across
// three files.
//
//   0  -- clean exit (including a user quit / closed splash mid-boot).
//   1  -- a boot/init failure (EditorApp::InitResult::Failed), OR a graph
//         frame FAILED, OR GPU device loss. The NRI frame graph is the only
//         render path, so the graph-frame case can fire on ANY run.
//   2  -- PRE-BOOT, from THIS function: no project and no plugin with
//         --frames (a scripted run with nothing to open and nobody to
//         answer a dialog). Also HostConfig::Parse's own refusals (a bad
//         flag, a bad option value) -- those return before main() even
//         reaches this point.
//   2  -- RenderErrorCount GREW across the run, teardown included. Like
//         code 1 it is ungated, so it can fire on any run.
//   3  -- PRE-BOOT, from THIS function: this project is already open in
//         another live editor (the direct-launch double-open guard below;
//         the rival is focused before returning).
//   3  -- (Task 9) --settle N never converged (two consecutive captures
//         never compared byte-equal with the compiler idle before the bail
//         CONJUNCTION fired: BOTH the attempt budget AND --settle-timeout
//         spent, never either alone -- Arcane/Host/SettleBound.hpp) --
//         exitReason "settle-not-converged" or "compare-failed" in the JSON
//         report. Same code RuntimeApp uses for the identical fact
//         (RuntimeApp.cpp's ShutdownGraphPath) -- see the collision note
//         below for why this reuses 3 rather than claiming a fresh number.
//         NOTE FOR ARGUMENT DECODERS (ArcaneHub launch.rs golden_run): this
//         line is the evidence that --compare is NOT the only route to a
//         post-boot 3. A --settle run with no --compare anywhere reaches
//         this arm, so a predicate keying on --compare alone under-detects.
//   4  -- (Task 9) --compare named a reference that does not exist on disk
//         (or exists but failed to decode) and --bless was not given --
//         exitReason "compare-missing-reference", zero frames rendered
//         (EditorApp::MainLoop's pre-loop resolve-and-fail-fast, mirroring
//         RuntimeApp::MainLoop's own). ALSO --bless converged but failed to
//         WRITE the reference -- exitReason "compare-failed" then. Same
//         code RuntimeApp uses for both facts.
//
// THE COLLISIONS, named rather than left for a reader to rediscover:
//
// Exit code 2 means TWO DIFFERENT THINGS depending on WHEN in the run it
// fires, and ArcaneHub's launch.rs decoder (BOOT_WATCHDOG, 2 seconds) reads
// whichever one arrives inside that window. A run that reaches
// EditorApp::Run() at all has already cleared the pre-boot refusals THIS
// function performs, so from there on a 2 means the later thing above -- but
// the Hub cannot tell the difference from the outside, and would misreport a
// fast one.
//
// NO GUARD CAN CLOSE IT, and launch.rs does not pretend otherwise. The NRI
// frame graph is the only render path, unconditionally, so code 2 can fire on
// ANY ordinary Hub launch that exits inside the 2s watchdog with a validation
// error, and an ordinary launch carries none of the tokens a heuristic could
// key on. So launch.rs's code-2 arm names BOTH meanings -- project gate or
// render error -- rather than confidently reporting an engine/abi refusal that
// was never involved.
//
// Exit code 3 gained a SECOND meaning with Task 9, for the same reason code 2
// already has two: matching RuntimeApp's own m_graphExit numbering (3 =
// settle-not-converged, 4 = compare-missing-reference/bless-write-failed) so
// the two hosts' internal fold logic stays one vocabulary, rather than
// minting editor-only numbers RuntimeApp does not share, outweighs avoiding a
// reuse of 3 the double-open guard already claimed. A HUMAN running one
// deliberate command line knows which precondition applied and is never
// actually confused -- the pre-boot double-open guard fires before any engine
// boot at all, with no project ever opened and no report ever written (it is
// no longer ahead of Diagnostics::Install, which task 9 moved to the top of
// main, but Install only ARMS the report path -- a plain refusal still writes
// nothing), while the post-boot settle-not-converged/compare-failed code
// requires a full boot and a --headless --settle run.
//
// A SCRIPTED CALLER LOOPING COMMAND-LINE COMBINATIONS CANNOT ASSUME THAT,
// though (Task 12's golden-gate harness is exactly this caller: it cannot
// know from outside whether a stale editor process is holding a project
// lock in CI) -- so the disambiguator has to be a FACT ON DISK, not an
// assumption about which command was run:
//
//     THE DISAMBIGUATOR IS REPORT-FILE EXISTENCE. A pre-boot double-open
//     refusal never reaches EditorApp::Run() -- let alone MainLoop -- so it
//     writes NO report at all, regardless of whether --report was passed.
//     A post-boot settle-not-converged/compare-failed exit, by construction,
//     only ever happens on a run that got as far as ShutdownGraphPath's
//     report block, which ALWAYS writes to --report's path when that flag
//     was given (see that function -- the write is unconditional on reaching
//     the block, independent of whether the run converged). So: exit 3 with
//     --report on the command line and the named file MISSING means the
//     pre-boot refusal; exit 3 with the file PRESENT (and its exitReason
//     naming one of the two post-boot facts) means the settle/compare one.
//
// THIS IS ALSO THE GENERAL RULE, not a special case invented for this one
// collision: RuntimeApp already collapses two distinct facts onto its own
// exit 3 (a non-converged settle vs. a converged-but-mismatched --compare)
// and two onto its own exit 4 (a missing/undecodable reference vs. a bless
// that converged but failed to write) -- see this file's own table above.
// A caller that wants to know WHAT HAPPENED, on either host, should always
// read `exitReason` out of the JSON report rather than trust the raw
// process exit code alone; report-file existence is simply that same rule
// extended one step further back, to cover the one exit-3 case where the
// JSON that would explain it might not exist yet.
//
// ===== WHICH SHARED HostConfig FLAGS THIS HOST ACTUALLY HONOURS ==============
// The table above is exhaustive about EXIT CODES; this is the separate,
// easily-confused question of FLAGS, and it is called out because HostConfig
// is shared by both hosts, so a flag the editor parses is not automatically a
// flag the editor DOES anything with.
//   --project/--plugin/--frames/--backend/--no-vsync -- honoured.
//   --screenshot        -- honoured. WINDOWED it captures the VIEWPORT panel's
//                          texture, not the editor window (so the Inspector and
//                          the asset browser are not in it) -- unless
//                          `--set editor.automation.windowedFrameCapture=true`
//                          (T3-D6, Dev): then it captures the presented
//                          backbuffer, the whole composited editor window. Under --headless
//                          it captures the COMPOSITED EDITOR FRAME instead --
//                          chrome, docking, panels, and the viewport texture
//                          inside its panel -- off the offscreen chrome
//                          context's colour target (EditorAppFrame.cpp's
//                          PresentChromeFrame).
//   --window-size WxH   -- honoured (T3-D6 fix round 1; automation only, it
//                          requires --frames N): the host window's pixel
//                          extent, which the headless chrome frame and the
//                          default dock layout both follow -- a capture at the
//                          desk's 1920x1080 geometry. Unset = 1280x720, the
//                          size every golden reference is captured at.
//   --headless          -- honoured. No window is ever mapped and no swapchain
//                          is built anywhere: the chrome context becomes an
//                          OffscreenVehicle (EditorApp::CreateGraphVehicles),
//                          the boot splash below is not constructed at all, and
//                          the per-project ImGui layout is PINNED to the
//                          committed <project>/Saved/verify-layout.ini seed with
//                          io.IniFilename null, so a run can neither read a
//                          machine-local layout nor write one back.
//                          Requires --frames N (HostConfig refuses otherwise).
//   --dump-layout <path> -- HONOURED as of Task 10 (plan-b comparator): write
//                          the LIVE ImGui layout to <path> at shutdown
//                          (EditorApp::Shutdown, unconditionally on
//                          --headless -- ImGui::SaveIniSettingsToDisk never
//                          consults io.IniFilename, so it writes to the given
//                          path even though that pointer is null offscreen).
//                          This is the authoring half of the committed
//                          ReferenceProject/Saved/verify-layout.ini seed:
//                          `--project ReferenceProject --headless --frames N
//                          --dump-layout out.ini` produces a file with REAL
//                          EditorDockHost dock-node ids, no windowed session
//                          required. THE ONE FLAG THIS HOST HONOURS THAT
//                          ArcaneRuntime REFUSES OUTRIGHT (see that exe's
//                          main.cpp) -- there is no ImGui layout on a game
//                          host to dump.
//   --play-as <topology> -- HONOURED as of the Core-DLL split's plan 1 Task 7:
//                          enter Play at the END of boot (EditorApp::
//                          StageFinalize, after the boot scene is loaded) in the
//                          named topology -- standalone | listen-server |
//                          embedded-server | client, the scripted half of the
//                          transport's play-mode picker. THE SECOND FLAG THIS
//                          HOST HONOURS THAT ArcaneRuntime REFUSES OUTRIGHT (see
//                          that exe's main.cpp, beside --dump-layout's refusal):
//                          a game host has no Edit mode to leave and no play
//                          session to enter.
//   --select-name <n>   -- HONOURED: select the entity with that Identity.name
//                          at the end of boot and frame it. Missing name is a
//                          loud error, not a silent skip.
//   --tool <tool>       -- HONOURED: viewport tool at boot (select | move |
//                          rotate | scale). Pairs with --select-name for a
//                          scripted gizmo capture. THE FOURTH/FIFTH FLAGS
//                          THIS HOST HONOURS THAT ArcaneRuntime REFUSES.
//   --open-asset <guid> -- HONOURED: open the document for that asset Guid at
//                          the end of boot (EditorApp::StageFinalize; a bad
//                          Guid / no opener is a loud ARC_ERROR). REFUSED by
//                          ArcaneRuntime (that exe's main.cpp:171-176).
//   --select-in-document <path> -- HONOURED, only WITH --open-asset: select
//                          Map[/Action[/binding index]] inside the opened
//                          document and route it to the Inspector; an
//                          unresolvable path is a loud ARC_ERROR with the run
//                          completing. Refused at parse without --open-asset;
//                          REFUSED outright by ArcaneRuntime.
//   --view-mode <mode>  -- HONOURED as of F4 plan 1 T7: seed the editor
//                          viewport camera's mode (2d | perspective) at the end
//                          of boot, AFTER the persisted [EditorViewport][Camera]
//                          block has been read, so the flag beats the desk's
//                          persisted mode (EditorApp::StageFinalize + the
//                          settings handler's ReadLine). Absent = the persisted
//                          mode, or the 2D default (the headless verify layout
//                          carries no such block, so goldens run in 2D). THE
//                          THIRD FLAG THIS HOST HONOURS THAT ArcaneRuntime
//                          REFUSES OUTRIGHT: a game host has no editor camera.
//   --settle / --report -- HONOURED as of Task 9 (the editor's verification
//   --compare / --bless    surface): PresentChromeFrame's capture arm is the
//                          SAME three-conjunct predicate (byte-equal &&
//                          shader-compiler-idle && [reference-match])
//                          RuntimeFrame::CaptureTail uses, ported rather than
//                          reinvented, and ShutdownGraphPath writes the same
//                          VerifyReport schema (schemaVersion 3) the runtime
//                          does -- settle facts included, and reconciled with
//                          the runtime's in the same commit that added them,
//                          so `settleAttemptsUsed` means the same thing on
//                          both hosts. See both functions' own comments. The
//                          one divergence from the runtime is structural, not
//                          a gap: this host implements no --probe (below), so
//                          the report's `probes` array is always empty and
//                          there is no `pick` field -- `census` is unaffected,
//                          being carried unconditionally on both hosts.
//   --probe             -- REFUSED at launch on this host, loudly (see below
//                          main()). A scripted caller asking for a pixel/pick
//                          probe would otherwise get exit 0 and a report with
//                          no matching entry -- the same silent-success shape
//                          Task 8 found on the runtime (a --report with pixel
//                          probes and no --screenshot ran, exited 0, and
//                          reported "no capture set" for every probe).
//                          Parsing it and shrugging would be worse than
//                          refusing. ArcaneRuntime honours it.
//   --scene              -- REFUSED at launch on this host (final-fix-wave
//                          audit; this table used to claim it was honoured,
//                          which was FALSE -- HostConfig::sceneOverride has
//                          no consumer anywhere in ArcaneEditor/. The only
//                          hit in this tree is a passing comment in
//                          Project/RuntimeLaunch.hpp; the sole real consumer
//                          is ArcaneRuntime's RuntimeApp.cpp. Without this
//                          refusal, `ArcaneEditor --scene <guid>` parsed,
//                          booted the manifest's default scene, and exited
//                          0 -- silent success for a caller that asked for a
//                          SPECIFIC scene.
//   --fixed-dt           -- REFUSED at launch on this host (final-fix-wave
//                          audit; previously not even mentioned in this
//                          table). The sim clock (EditorAppFrame.cpp's
//                          AdvanceSim) and the chrome/plugin-input clock
//                          (EditorAppFrame.cpp:492-495) are each an
//                          INDEPENDENT std::chrono::steady_clock read --
//                          neither ever looks at fixedDtSeconds. Refusal is
//                          keyed on fixedDtSupplied (HostConfig.hpp), not the
//                          resolved value: the registered default (1/60) is
//                          itself a value a caller could legitimately pass on
//                          purpose, so comparing the resolved double against
//                          that default cannot tell "never passed" from
//                          "passed the default explicitly" apart -- same
//                          r.Supplied() reasoning HostConfig.cpp already uses
//                          for this exact flag internally.
//   --nri-graph         -- PARSED AND IGNORED: the NRI frame graph is the only
//                          render path, so there is nothing left for this flag
//                          to select. THE ONE DELIBERATE EXCEPTION to rule 3
//                          (Task 12 audit) rather than a drift nobody caught:
//                          refusing it would break saved Hub launch args and
//                          scripts that still pass it out of habit, for a flag
//                          that costs nothing to leave parsed-and-ignored. See
//                          HostConfig.cpp's own carve-out comment. Do not
//                          "fix" this one into a refusal.
//   --crash-gpu N       -- honoured (also reachable from the Debug menu).
//   --pick-probe x,y    -- REFUSED at launch on this host (Task 12 audit; see
//                          the refusal below main()). Re-derived rather than
//                          trusted: this comment used to call it a plain
//                          no-op, which undersold it. It IS a no-op from the
//                          caller's side -- nothing ever reports a hit/miss --
//                          but it is not a zero-cost one on a WINDOWED editor.
//                          NriGraphContext.cpp:524's
//                          `m_pickArmed = config.pickProbe && !IsOffscreen()`
//                          reads false only for the OFFSCREEN VIEWPORT
//                          context; the CHROME (host-window) context this
//                          editor also owns is NOT offscreen when windowed, so
//                          the flag DOES arm there -- the pick + JFA outline
//                          NODES get built and one ARC_INFO line gets logged.
//                          What still never happens is the per-frame PASS:
//                          EditorAppFrame.cpp's chrome FrameDesc never sets
//                          `pickOutline`, so AddPickNodes never runs, nothing
//                          is drawn, and no id is ever read back. Net effect
//                          for a caller is unchanged (still nothing useful),
//                          which is why refusing outright is still correct --
//                          it is also a RUNTIME desk item: one fixed canvas
//                          pixel reported as an exit code, and the editor's
//                          own pick is a live click through
//                          FrameDesc::pickPixel instead, so there is nowhere
//                          for this flag to plug in even if it were wired.
//   --perf              -- REFUSED at launch on this host (Task 12 audit; see
//                          the refusal below main()). EditorApp holds a
//                          FramePerf m_perf and never calls
//                          FrameStart/Add/Tick -- the member declaration is
//                          its ONLY reference in this tree, so
//                          no [PERF] line is emitted here by any build. The
//                          runtime drives the same class from RuntimeFrame.cpp
//                          (Tick is the sole emitter). NOT WIRED because
//                          FramePerf's seven fixed buckets (sim/rec/end/tone/
//                          imgui/present/poll) do not map onto this host's
//                          19-phase frame.
// =============================================================================
int main(int argc, char** argv)
{
    Arcane::Log::Init();
    Arcane::Log::InstallMosaicSink();
    Arcane::Assert::InstallMosaicHandler();
    Arcane::HostConfig::ParseOutcome parsed = Arcane::HostConfig::Parse(argc, argv);   // non-const: the early rungs fill backend/vsync
    if (!parsed.config) return parsed.exitCode;

    // Probe: identity to stdout, nothing else. Deliberately BEFORE any engine
    // boot -- the Arcane Hub calls this to read the plugin ABI it must stamp
    // into a new .arcproj, and it must not pay for a window, a device, or a
    // registry to answer.
    //
    // AND BEFORE Diagnostics::Install BELOW (R25), which is the ONE exception
    // to "Install is the first thing this process does": this path is a pure
    // query that prints one line and exits, so arming for it would start the
    // crash thread and the watchdog, open and rotate a log file, and leave an
    // empty diagnostics/ directory beside the exe -- per Hub probe, for a
    // process that cannot live long enough to crash or hang. Nothing between
    // here and Install depends on Install.
    if (parsed.config->printEngineInfo)
    {
        // ExecutablePathUtf8, NOT argv[0]: argv[0] is whatever the launcher typed
        // (a bare relative name under the documented cd-then-run workflow) and is
        // ANSI-codepage bytes under MSVC, which a strict-UTF-8 dump() rejects.
        std::printf("%s\n", Arcane::HostBoot::EngineInfoJson(Arcane::ExecutablePathUtf8()).c_str());
        return 0;
    }

    // POST-MORTEM CAPTURE, THE FIRST THING THIS PROCESS DOES once its argv
    // makes sense and it is going to actually RUN (crash window plan 1, task
    // 9; spec S5.1's closing paragraph).
    // A crash writes a minidump plus a portable all-thread stack; a WEDGED main
    // thread writes the same report while the process is still alive. The
    // second half is the point: Windows Error Reporting only ever fires on
    // process death, so a hang ("Not Responding") otherwise produces nothing,
    // anywhere, ever.
    //
    // FIRST (bar the probe above), and no longer after the refusals below,
    // because everything that
    // made the old position load-bearing is gone: the watchdog is a raw thread
    // stopped from an atexit hook Install registers, so an early `return` can
    // no longer leave a joinable std::thread to static destruction (which was
    // std::terminate -> abort() -> a BLOCKING Debug-CRT dialog). Every refusal
    // below is an ordinary `return` again. It is also the only placement that
    // covers the boot itself -- the window where a project scan or a shader
    // compile actually wedges.
    //
    // AFTER Log::Init/InstallMosaicSink/InstallMosaicHandler above, and that
    // order IS still load-bearing (R16): Install attaches the log file sink
    // and the crash path freezes the log backlog, neither of which exists
    // before Log::Init runs.
    // The editor.keys.* shortcut cvars are declared FIRST (S4-GATE): the
    // registry layers a rung only onto cvars that already exist, and
    // EditorActions registers its table lazily on the first Get(). Declared
    // after the early rungs, every --set editor.keys.* was refused as unknown
    // and a saved shortcut waited on the later re-application. Every other
    // editor cvar is a static (ARC_CVAR) and exists before main.
    (void)Arcane::Editor::EditorActions::Get();
    // I2: engine/project/EditorUser/user/--set rungs before Install and Runtime.
    Arcane::HostBoot::ApplyEarlyConfigRungs(*parsed.config, Arcane::CVarContext::Editor,
                                            /*editor*/ true);
    {
        // The tunables are diagnostics.* (settings arc S6-2), read after the
        // early rungs above so a project, EditorUser, user or --set value
        // reaches Install.
        Arcane::Diagnostics::Config diag =
            Arcane::Diagnostics::ConfigFromSettings(Arcane::Settings<Arcane::DiagnosticsSettings>());
        diag.appName     = "ArcaneEditor";
        diag.productName = "Arcane Editor";   // the reporter's window title
        // A --headless run has nobody to answer a reporter window: the report
        // is written, the reporter stays silent.
        diag.unattended  = parsed.config->headless;
        // Spec S5.8 (plan 2, task 9): a windowed run pre-launches the crash
        // MONITOR, so a death the crash path never sees (__fastfail, /GS, heap
        // corruption, an external kill) still becomes a report. A --headless
        // run has nobody to show it to and launches none. Install also skips
        // it on a build machine (spawnReporter forced false) and under a
        // debugger.
        diag.launchMonitor = !parsed.config->headless;
        // The RELAUNCH line the reporter's "restart" offers -- this run's argv
        // minus the capture harness, so a crashed verify run comes back as the
        // session it was rendering. See SanitizeRelaunchLine (HostConfig.hpp).
        //
        // ELEMENT 0 IS ExecutablePathUtf8(), NOT argv[0], for exactly the
        // reason the probe above states: argv[0] is whatever the launcher
        // typed -- a bare relative name under the documented cd-then-run
        // workflow, in ANSI-codepage bytes under MSVC -- so a reporter
        // relaunching it from its own working directory, or writing it into a
        // UTF-8 envelope, would get a path that does not resolve. The
        // sanitizer stays pure: the substitution is the host's job, and only
        // the host knows its own exe.
        std::vector<std::string> args(argv, argv + argc);
        if (args.empty()) args.emplace_back();
        args[0] = Arcane::ExecutablePathUtf8();
        diag.commandLine = Arcane::SanitizeRelaunchLine(args);
        Arcane::Diagnostics::Install(diag);
    }
    // The two-step Ctrl-C, the console close, and logoff/shutdown all route
    // into RequestCleanExit, which calls THIS hook (spec S5.7). Installed here
    // rather than from EditorApp so the window between Install and the app's
    // first frame -- the whole boot -- is covered too; the hook is a static
    // that sets a flag the frame loop reads, so it needs no live app (R24:
    // every host installs one, or a first Ctrl-C is declined and Windows
    // terminates as before).
    Arcane::Editor::EditorApp::InstallCleanExitHook();

    // THE HostConfig FLAGS THIS HOST DOES NOT IMPLEMENT, refused here rather
    // than silently ignored. HostConfig is SHARED with ArcaneRuntime, so
    // every one of these parses cleanly on this exe and would otherwise let
    // a scripted run exit 0 having done nothing useful -- the exact
    // silent-success shape Task 8 paid to find on the runtime (a --report
    // with pixel probes and no --screenshot ran, exited 0, and reported "no
    // capture set" for every probe). An agent reads exit 0 plus a
    // missing/unchanged result as a pass, so the refusal is the fix; wiring
    // any of them into this host is a later task, and it must delete the
    // matching line here on the same day.
    //
    // TASK 9 DELETES --report AND --settle FROM THIS CONDITION -- exactly
    // the "delete the matching line here on the same day" the comment above
    // promised. --compare/--bless need no line of their own: HostConfig.cpp
    // refuses either one without --settle already (a shared, parse-time
    // gate), so lifting --settle transitively lifts them too. --probe STAYS
    // REFUSED, on its own condition below now rather than folded into this
    // one: it is not this task's title, and ArcaneEditor implements no probe
    // evaluation at all -- folding a three-flag OR into one message was
    // exactly what made it easy to delete two-thirds of it without noticing
    // the third had to stay.
    //
    // THE ORDERING AGAINST Diagnostics::Install IS NO LONGER LOAD-BEARING
    // (crash window plan 1, task 9). This block, and every refusal below it,
    // used to have to run BEFORE Install: an early `return` taken after it
    // left the hang watchdog's std::thread joinable at static destruction,
    // which is std::terminate -> abort() -- and under a Debug CRT that is a
    // BLOCKING "Microsoft Visual C++ Runtime Library" dialog, i.e. a hang
    // rather than the documented exit code (measured, not theorised). The
    // watchdog is now a RAW thread stopped from an atexit hook Install
    // registers, so a plain `return` from anywhere is clean and Install has
    // moved to the top of main() where it covers the boot as well. Refusals
    // here are free to move, and none of them needs a Shutdown() of its own.
    if (!parsed.config->probes.empty())
    {
        std::fprintf(stderr,
            "Arcane Editor: --probe is not implemented on this host.\n"
            "  This exe would exit 0 having produced no probe entries, which is worse than\n"
            "  refusing. Use ArcaneRuntime for pixel/pick probes; ArcaneEditor supports\n"
            "  --headless --frames N --settle N --report <json> --screenshot <png>\n"
            "  --compare <name> [--bless] for a composited editor capture and verification.\n");
        return 2;
    }
#if !defined(ARC_BUILD_DIST)
    // --pick-probe: DEV-ONLY, matching HostConfig.hpp/.cpp's own
    // #if !defined(ARC_BUILD_DIST) guard around the pickProbe member and its Cli
    // registration -- the member does not exist on a Dist build, so reading
    // parsed.config->pickProbe unguarded would fail to COMPILE there, not just
    // misbehave (confirmed by building Dist below). Same reasoning and same
    // position as the block above: this is genuinely NOT a zero-cost no-op on
    // this host -- see the disposition table above main() -- NriGraphContext
    // arms the pick+outline nodes on the windowed chrome context and logs one
    // line -- but nothing ever reads an id back, so the caller still gets
    // nothing for its trouble. Refuse rather than let that drift stand.
    if (parsed.config->pickProbe)
    {
        std::fprintf(stderr, "error: --pick-probe is a RUNTIME flag; the editor's pick is a live "
                             "click through FrameDesc::pickPixel. Use ArcaneRuntime.exe.\n");
        return 2;
    }
#endif
    // --perf: same reasoning and position as above (and its own #if is not
    // needed -- HostConfig::perf is NOT Dist-guarded, unlike pickProbe).
    // EditorApp holds a FramePerf m_perf and never calls
    // FrameStart/Add/Tick, so no [PERF] line is ever emitted on this host by
    // any build -- FramePerf's seven fixed buckets do not map onto this
    // host's 19-phase frame.
    if (parsed.config->perf)
    {
        std::fprintf(stderr, "error: --perf is not implemented on the editor host "
                             "(FramePerf is constructed and never sampled). Use ArcaneRuntime.exe.\n");
        return 2;
    }

    // --fixed-dt: final-fix-wave audit finding, same reasoning and position as
    // --perf/--pick-probe above. AdvanceSim (EditorAppFrame.cpp) and the
    // chrome/plugin-input dt a few lines above it are each an independent
    // std::chrono::steady_clock read; neither ever consults fixedDtSeconds, so
    // this exe would otherwise exit 0 having silently run wall-clock anyway.
    // Gated on fixedDtSupplied, not the resolved value -- see HostConfig.hpp's
    // comment on that field for why comparing the resolved double against its
    // own default cannot tell "never passed" from "passed the default
    // explicitly" apart.
    if (parsed.config->fixedDtSupplied)
    {
        std::fprintf(stderr, "error: --fixed-dt is not implemented on the editor host "
                             "(the sim and chrome clocks are both wall-clock steady_clock reads that "
                             "never consult it). Use ArcaneRuntime.exe for a deterministic fixed "
                             "timestep.\n");
        return 2;
    }
    // --scene: final-fix-wave audit finding. HostConfig::sceneOverride has no
    // consumer anywhere in this tree -- ArcaneRuntime's RuntimeApp.cpp is the
    // only real reader. Without this refusal `ArcaneEditor --scene <guid>`
    // parsed cleanly, booted the manifest's default scene, and exited 0: a
    // silent no-op for a caller that asked for a specific scene.
    if (!parsed.config->sceneOverride.empty())
    {
        std::fprintf(stderr, "error: --scene is not implemented on the editor host "
                             "(this exe always boots the project manifest's bootScene). Use "
                             "ArcaneRuntime.exe for a scene override, or open the scene from the "
                             "editor's asset browser once booted.\n");
        return 2;
    }

    // (Diagnostics::Install USED TO BE HERE, deliberately after every refusal
    // above -- see the reciprocal note in the refusal block for why that
    // stopped being necessary. It now runs at the top of main(), right after
    // the --print-engine-info probe, which is the only placement that also
    // covers the boot.)

#ifdef _WIN32
    // One taskbar family. Windows groups taskbar buttons by AppUserModelID,
    // NOT by process parentage, so the editor claims the same id the Arcane
    // Hub sets and their buttons stack under one group -- whether this
    // process was spawned by the Hub or double-clicked directly. Must run
    // before the first window exists, hence here and not in EditorApp.
    // Deliberately unconditional rather than an --appid flag: a flag would
    // make an older editor build reject the unknown argument at launch.
    // AFTER the probe on purpose -- the probe pays for nothing it can skip.
    SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);
#endif

    // No project and no explicit plugin. A SCRIPTED run (--frames N) still
    // refuses: it cannot answer a dialog, and booting project-less used to mean a
    // "data/-next-to-exe" session with no asset registry, no mounts and no
    // identity -- a half-configured editor nothing downstream expects.
    //
    // An INTERACTIVE bare launch does NOT refuse. It boots project-less and the
    // editor's start page (EditorApp::DrawStartPage, spec 2026-09-30 s8.4) offers
    // the recent projects, Open Project... and Open Folder... -- derived from
    // project state every frame, so a failed open brings it back. The editor
    // never launches or mentions the Hub; this is the path for anyone who runs
    // the exe directly.
    //
    // Both flags remain bypasses ON PURPOSE: CI and the scripted
    // `--project <p> --frames N` harness depend on --project, and --plugin is the
    // engine-dev path (hosting a plugin without a project).
    // A PLAIN `return 2` AGAIN (crash window plan 1, task 9). Task 12 measured
    // that this return, taken after Install had armed a joinable watchdog
    // std::thread, did not even reach an exit code -- std::terminate's default
    // handler popped a BLOCKING "Microsoft Visual C++ Runtime Library"
    // MessageBox and the process never exited on its own -- and paid for it
    // with a Diagnostics::Shutdown() right here. The watchdog is now a raw
    // thread stopped from Install's atexit hook, so that call has gone: the
    // return is clean on its own, and the report machinery stays armed right
    // up to process exit, which is where it belongs.
    const bool noProject = parsed.config->projectPath.empty() && parsed.config->pluginPath.empty();
    if (noProject && parsed.config->maxFrames != 0)
    {
        std::fprintf(stderr,
            "Arcane Editor: no project selected, and --frames makes this a scripted run.\n"
            "  Pass --project <folder-or-.arcproj> to open one,\n"
            "  or --plugin <dll> to host a plugin without a project.\n");
        return 2;
    }

    // The direct-launch guard: refuse to double-open a project another LIVE
    // editor already holds, and bring that editor forward instead. This is
    // the one multi-open path the Hub cannot see -- its own launches take
    // the same lock through editorlock.rs, but nothing stops two direct
    // `ArcaneEditor --project X` invocations except this. Root derivation
    // mirrors Project::Open (a .arcproj names its parent; anything else IS
    // the folder), RivalPid is self-exempt and defeats stale locks by
    // pid+creation-time, and exit code 3 is distinct from 2 (the no-project
    // refusal above) so the Hub's boot watchdog can name the reason.
    //
    // Its `return 3` is a plain return too, for the same reason the
    // no-project block above states in full: the Shutdown()-before-return
    // Task 12 added here is gone with the joinable-watchdog hazard that
    // motivated it.
    if (!parsed.config->projectPath.empty())
    {
        std::filesystem::path lockRoot(parsed.config->projectPath);
        if (lockRoot.extension() == ".arcproj")
            lockRoot = lockRoot.parent_path();
        if (const auto rival = Arcane::EditorLock::RivalPid(lockRoot))
        {
            std::fprintf(stderr,
                "Arcane Editor: '%s' is already open in another editor (pid %u) -- focusing it.\n",
                parsed.config->projectPath.c_str(), *rival);
            Arcane::EditorLock::FocusWindowOfProcess(*rival);
            return 3;
        }
    }

    // Before ANY engine boot: something on screen within ~100ms. Every guard
    // above (the probe, the no-project+--frames refusal, the rival-editor
    // lock check) returns before this line specifically so none of them pays
    // for a window they might not need. Never fails boot -- BootSplashWindow's
    // whole contract is "every error path degrades to no splash, silently".
    // ...UNLESS --headless, where the whole point is that this process maps
    // no window. The splash is a real WS_POPUP on its own thread, so
    // constructing it unconditionally would make "--headless" a lie for the
    // ~seconds boot takes -- the one window a --headless run would still
    // flash on screen. std::optional is what it takes: BootSplashWindow's
    // only constructor takes an image path and there is no "disabled" state,
    // and every consumer downstream is already null-tolerant by contract
    // (BootSplashPresenter's ctor comment: "`splash` may be null ... every
    // call then degrades to do nothing"; EditorApp guards with `if
    // (m_splash)`). Destruction order is unchanged -- `app` still lives in the
    // nested scope below and is destroyed before this object.
    //
    // ...AND UNLESS this is an AUTOMATION run (--frames N, or the windowed
    // self-capture cvar; T3-D6 fix round 1, App/HostPresentation.hpp): the
    // splash is a TOPMOST popup that activates on show, and a scripted
    // windowed capture runs while a person may be working at the desk.
    std::optional<Arcane::BootSplashWindow> splash;
    if (Arcane::Editor::HostPresentationFor(*parsed.config).bootSplash)
        splash.emplace("data/images/arcane_logo.png");

    // Scoped so ~EditorApp -- the load-bearing teardown sequence -- runs while
    // the watchdog is STILL armed. Teardown does not beat, so a deadlock in it
    // reports as a hang, which is exactly right: tearing down threaded state is
    // itself a suspect. Since task 9 EditorApp::Run also calls
    // Diagnostics::RequestCleanExit() at its quit site, so the watchdog spends
    // this whole scope as the EXIT SENTINEL (spec S5.7): a teardown that never
    // returns is reported as "hang at exit" and terminated with 12 instead of
    // sitting there forever. Shutdown() below is the END of that window, and
    // stays the last statement of main for exactly that reason.
    // Declaring app in a nested scope keeps its destruction BEFORE splash's,
    // the same relative order as when both were siblings here.
    int rc = 0;
    {
        Arcane::Editor::EditorApp app(*parsed.config, splash ? &*splash : nullptr);
        rc = app.Run();
        Arcane::Diagnostics::SetPhase("editor teardown");
        Arcane::Diagnostics::Heartbeat();
    }
    Arcane::Diagnostics::Shutdown();
    return rc;
}
