#include <Arcane/Host/HostConfig.hpp>
#include <Arcane/Cli/Cli.hpp>
#include <Arcane/Host/VerifyReport.hpp>   // Arcane::ParseProbe -- reused for the --probe parse-time refusal below
#include <Arcane/Host/ReferenceImages.hpp>   // Arcane::ReferenceNameIsSafe -- reused for --compare's parse-time refusal (Task 8, Finding 2)
#include <cmath>    // std::isfinite -- --max-diff-pixel-ratio's range refusal below
#include <algorithm>   // std::find -- SanitizeRelaunchLine's strip-set lookups
#include <cstdio>
#include <format>
#include <string_view>
namespace Arcane
{
    namespace
    {
        // THE STRIP SET (SanitizeRelaunchLine, see HostConfig.hpp for the two
        // rules). Split by whether the flag consumes the NEXT argv entry: a
        // value-taking flag drops its value too, or the value would survive as
        // a stray positional the relaunch then chokes on.
        //
        // The second half of each list is the dependants rule: --settle-timeout,
        // --bless and --max-diff-* have no meaning without a parent that is
        // already going, and --probe/--fixed-dt/--fixed-time cannot parse
        // without --headless.
        constexpr std::string_view kStripWithValue[] = {
            "crash-gpu", "hang-main", "frames", "report", "compare", "settle", "screenshot",
            "settle-timeout", "probe", "max-diff-pixels", "max-diff-pixel-ratio",
            "fixed-dt", "fixed-time", "pick-probe", "window-size",
        };
        constexpr std::string_view kStripBareFlags[] = { "headless", "bless" };

        [[nodiscard]] bool InSet(std::span<const std::string_view> set, std::string_view name)
        {
            return std::find(set.begin(), set.end(), name) != set.end();
        }

        // A relaunch line is re-split on whitespace by whoever runs it, so an
        // argument that contains a space has to come back as one token. Empty
        // is quoted for the same reason -- it would otherwise vanish.
        [[nodiscard]] std::string QuoteIfNeeded(const std::string& arg)
        {
            if (!arg.empty() && arg.find(' ') == std::string::npos)
                return arg;
            return "\"" + arg + "\"";
        }
    }

    std::string SanitizeRelaunchLine(std::span<const std::string> argv)
    {
        std::string out;
        for (std::size_t i = 0; i < argv.size(); ++i)
        {
            const std::string& arg = argv[i];
            if (arg.rfind("--", 0) == 0)
            {
                // BOTH spellings Cli accepts (Cli.cpp:146): "--name value" and
                // "--name=value". Matching only the first would leave the
                // inline form in the line, which is the whole point of the
                // sanitize.
                std::string_view body(arg);
                body.remove_prefix(2);
                const std::size_t eq = body.find('=');
                const bool inlineValue = eq != std::string_view::npos;
                const std::string_view name = inlineValue ? body.substr(0, eq) : body;

                if (InSet(kStripBareFlags, name))
                    continue;
                if (InSet(kStripWithValue, name))
                {
                    // Its value is the next entry, UNLESS it was inline or the
                    // next entry is itself an option. A negative number
                    // (`--fixed-time -1`) starts with one dash, not two, so it
                    // is correctly consumed as the value it is.
                    if (!inlineValue && i + 1 < argv.size() && argv[i + 1].rfind("--", 0) != 0)
                        ++i;
                    continue;
                }
            }
            if (!out.empty()) out.push_back(' ');
            out += QuoteIfNeeded(arg);
        }
        return out;
    }

    HostConfig::ParseOutcome HostConfig::Parse(int argc, char** argv)
    {
        Cli cli{ "Arcane Runtime", "standalone runtime host" };
        cli.Option("backend", kDefaultBackendCliName, "graphics backend: dx12|vulkan").Choices({ "dx12", "vulkan" });
        cli.Option("frames",  "0",           "render N frames then exit").Type(CliType::Uint);
        cli.Flag  ("no-vsync",               "present without vsync");
        cli.Flag  ("perf",                   "log per-phase ms (sets diagnostics.perfLog; interval: diagnostics.perfLogIntervalFrames)");
        cli.Option("plugin",  "",            "game DLL to host (empty = the project's gameModule; a runtime with nothing to host refuses boot)");
        cli.Option("project", "", "project folder or .arcproj to open (empty = data/-next-to-exe)");
        cli.Option("scene",   "", "asset Guid to boot instead of the manifest's bootScene (empty = follow the manifest)");
        cli.Option("screenshot", "", "write the last rendered frame to this PNG before exiting (pairs with --frames)");
        cli.Flag  ("print-engine-info",       "print engine identity JSON to stdout and exit");
        cli.Flag  ("headless",           "render with no window shown and no swapchain; "
                                         "pairs with --frames/--probe/--report");
        // 17 significant digits round-trip the default step's double exactly.
        cli.Option("fixed-dt", std::format("{:.17g}", 1.0 / SimSettings{}.fixedHz), "seconds per simulated frame "
                                         "(--headless only)").Type(CliType::Double);
        cli.Option("fixed-time", "", "pin the absolute scene clock to this many seconds, so "
                                     "Time is independent of the frame count (--headless only; "
                                     "omit to let the clock accumulate)").Type(CliType::Double);
        cli.Option("probe", "",          "repeatable: brightness@x,y | luma@x,y | rgba@x,y | pick@x,y | census").Many();
        cli.Option("set", "",            "repeatable: set a cvar, name=value, before the first frame").Many();
        cli.Option("report", "",         "write the observation report to this JSON path");
        cli.Option("dump-layout", "", "write the live ImGui layout to this .ini at shutdown "
                                      "(editor only; the authoring half of the committed "
                                      "verify-layout.ini seed)");
        cli.Option("play-as", "", "editor only: start playing at boot as standalone | "
                                  "listen-server | embedded-server | client");
        cli.Option("view-mode", "", "editor only: viewport mode to start in: 2d | perspective "
                                    "(empty = the persisted mode)").Choices({ "2d", "perspective" });
        cli.Option("select-name", "", "editor only: select the entity with this Identity.name "
                                      "at boot and frame it (empty = no seed)");
        cli.Option("tool", "", "editor only: viewport tool at boot: select | move | rotate | scale "
                               "(empty = select)").Choices({ "select", "move", "rotate", "scale" });
        cli.Option("open-asset", "", "editor only: open the document for this asset Guid at boot, "
                                     "as the Asset Browser would (empty = none)");
        cli.Option("select-asset", "", "editor only: select this asset guid in the Asset Browser at boot (its page shows in the Assets Inspector)");
        cli.Option("select-in-document", "", "editor only, with --open-asset: select this path inside "
                                             "the opened document, e.g. Player/Jump (empty = none)");
        cli.Option("window-size", "",    "automation only (needs --frames N): the host window's "
                                         "pixel extent WxH, e.g. 1920x1080 -- the headless chrome "
                                         "frame and the default layout follow it (empty = 1280x720)");
        cli.Option("settle", "0",        "repeat the capture (render clock frozen) until two consecutive "
                                         "frames compare byte-equal AND the shader compiler is idle, "
                                         "for AT LEAST N attempts -- it gives up only once BOTH N attempts "
                                         "and --settle-timeout milliseconds are spent (0 = off, 1 is "
                                         "refused -- needs >= 2; --headless only; needs --screenshot "
                                         "or --report)").Type(CliType::Uint);
        cli.Option("settle-timeout", "5000", "milliseconds the settle loop must ALSO spend before "
                                         "giving up; it bails only when the attempt budget AND "
                                         "this timeout are both spent (0 = no time bound; "
                                         "--settle only)").Type(CliType::Uint);
        // --compare / --bless (Task 8). Registered beside --settle: the
        // comparison is a THIRD conjunct in that same convergence predicate,
        // not a separate mode -- see HostConfig.hpp's compareReference comment.
        cli.Option("compare", "",        "compare the converged capture against reference image "
                                         "<name>, resolved from <project>/Verify/References "
                                         "(--headless + --settle only)");
        cli.Flag  ("bless",              "accept the converged capture AS the reference "
                                         "--compare names, writing to the level it resolved "
                                         "from; exits 0 (--compare only)");
        cli.Option("max-diff-pixels", "", "differing-pixel budget (default 0)").Type(CliType::Uint);
        cli.Option("max-diff-pixel-ratio", "", "differing-pixel budget as a fraction of the "
                                         "reference's area; when both budgets are given the "
                                         "SMALLER wins").Type(CliType::Double);
        // NOT Dist-guarded: the NRI frame graph is the ONLY render path in
        // every configuration, so the flag that used to select it has nothing
        // left to select. Kept registered and parsed-and-ignored rather than
        // deleted or refused outright --
        // scripts, the Hub's saved launch args, and the desk batteries all
        // still pass it, and a hard "unknown argument" refusal would turn a
        // harmless no-op into a boot failure at the worst moment (a shipped
        // Dist build, launched from a saved arg list nobody is watching).
        //
        // DELIBERATE EXCEPTION TO RULE 3 (silent-inertness), not an oversight
        // that Task 12's audit missed: every other HostConfig flag either does
        // something or is refused per-host (ArcaneEditor/src/main.cpp's flag
        // table). This is the one flag that stays a no-op on purpose, because
        // refusing it would break exactly the backward-compatibility case it
        // exists for. Do not "fix" this into a refusal later.
        //
        // The retired capture-mode flag `--headless` replaced went the OTHER
        // way -- removed outright, no alias -- and the two policies are
        // consistent rather than contradictory: a MODE flag with a working
        // replacement is worth a loud break, because accepting and ignoring the
        // old spelling would have silently downgraded every scripted capture to
        // a windowed run (see HostConfig.hpp's `headless` comment). A flag that
        // does nothing has nothing to downgrade and no replacement to point at,
        // so refusing it would cost a boot and buy the caller nothing.
        cli.Flag  ("nri-graph",      "DEPRECATED, accepted and ignored: the NRI frame graph is "
                                     "the only render path. Kept so existing scripts and saved "
                                     "launch args do not fail to boot.");
#if !defined(ARC_BUILD_DIST)
        cli.Option("crash-gpu", "0", "DEV: deliberately fault the GPU on frame N (0 = off) -- "
                                     "the crash-diagnostics desk trigger").Type(CliType::Uint);
        cli.Option("hang-main", "0", "DEV: on frame N block the main thread for 15 s without "
                                     "beating (0 = off) -- the hang-report desk trigger").Type(CliType::Uint);
        // Registered beside the other Dist-guarded dev flags. The render path
        // is unconditional, so this flag has no prerequisite to name.
        cli.Option("pick-probe", "",  "DEV: add the pick + JFA outline nodes, scripted "
                                      "onto the scene's first pickable entity, and print the entity id "
                                      "read back at canvas pixel x,y -- exit 0 on a hit, 1 on a miss");
#endif

        const Cli::Result r = cli.Parse(argc, argv);
        if (!r.ok) return { std::nullopt, r.exitCode };

        HostConfig cfg;
        cfg.backend    = (r.Get("backend") == "vulkan") ? GraphicsBackend::Vulkan : GraphicsBackend::D3D12;
        cfg.backendSupplied = r.Supplied("backend");
        cfg.maxFrames  = r.GetAs<std::uint64_t>("frames");
        cfg.vsync      = !r.Flag("no-vsync");
        cfg.perf       = r.Flag("perf");
        cfg.pluginPath = r.Get("plugin");
        cfg.projectPath = r.Get("project");
        cfg.sceneOverride = r.Get("scene");
        cfg.screenshotPath = r.Get("screenshot");
        cfg.printEngineInfo = r.Flag("print-engine-info");
        cfg.headless       = r.Flag("headless");
        cfg.fixedDtSeconds = r.GetAs<double>("fixed-dt");
        cfg.fixedDtSupplied = r.Supplied("fixed-dt");
        if (r.Supplied("fixed-time"))
            cfg.fixedTimeSeconds = r.GetAs<double>("fixed-time");
        cfg.probes         = r.GetMany("probe");
        cfg.cvarSets       = r.GetMany("set");
        cfg.reportPath     = r.Get("report");
        cfg.dumpLayoutPath = r.Get("dump-layout");
        cfg.playAs         = r.Get("play-as");
        cfg.viewMode       = r.Get("view-mode");   // Choices() already refused every other spelling
        cfg.selectName     = r.Get("select-name");
        cfg.openAsset      = r.Get("open-asset");
        cfg.selectAsset    = r.Get("select-asset");
        cfg.selectInDocument = r.Get("select-in-document");
        cfg.tool           = r.Get("tool");        // Choices() already refused every other spelling
        // Malformed --probe syntax is refused HERE, at parse time, not
        // deferred to evaluation. VerifyReport::Evaluate only ever sees specs
        // ParseProbe already accepted (a host parses-and-logs separately, at
        // shutdown) -- so a typo like `--probe brigtness@1,2` used to parse
        // clean, run to completion, and exit 0 with that probe simply absent
        // from the report: no error, no artifact, invisible unless a caller
        // diffs the probe list against the output by hand. Rule 3 (refuse,
        // don't silently skip) applies at THIS boundary just as much as it
        // does to --screenshot/--headless/--settle above and below. Reuses
        // ParseProbe's own error text (VerifyReport.hpp) rather than a second
        // vocabulary for the same mistake.
        for (const std::string& rawProbe : cfg.probes)
        {
            std::string probeError;
            if (!ParseProbe(rawProbe, probeError))
            {
                std::fprintf(stderr, "error: --probe '%s': %s\n", rawProbe.c_str(), probeError.c_str());
                return { std::nullopt, 2 };
            }
        }
        cfg.settleAttempts = r.GetAs<std::uint64_t>("settle");
        cfg.settleTimeoutMs = r.GetAs<std::uint64_t>("settle-timeout");
        // --compare / --bless (Task 8). maxDiffPixels/maxDiffPixelRatio stay
        // nullopt unless EXPLICITLY supplied -- r.GetAs<>() on an unsupplied
        // Option resolves its registered default ("", which GetAs would
        // parse as 0/0.0), and "the caller asked for a budget of exactly
        // zero" is a different fact from "the caller never mentioned a
        // budget at all" (CompareImages' own ImageCompareOptions documents
        // the same "unset = zero" default, but HostConfig must not collapse
        // "unset" into "explicitly zero" before it ever reaches there).
        cfg.compareReference = r.Get("compare");
        cfg.bless            = r.Flag("bless");
        if (r.Supplied("max-diff-pixels"))
            cfg.maxDiffPixels = r.GetAs<std::uint64_t>("max-diff-pixels");
        if (r.Supplied("max-diff-pixel-ratio"))
            cfg.maxDiffPixelRatio = r.GetAs<double>("max-diff-pixel-ratio");
        // "nri-graph" is intentionally never read here: it is registered
        // above (unconditionally) purely so a command line that still passes
        // it does not fail to parse. There is nothing left to store -- the
        // graph path it used to opt into is now the only one.
#if !defined(ARC_BUILD_DIST)
        cfg.crashGpuFrame = r.GetAs<std::uint64_t>("crash-gpu");
        cfg.hangMainFrame = r.GetAs<std::uint64_t>("hang-main");
#endif

        // --window-size WxH (T3-D6 fix round 1). Parsed and range-checked
        // here (rule 3: a malformed extent is refused, never ignored), and
        // automation only -- it requires --frames N, the unattended-run flag.
        if (r.Supplied("window-size"))
        {
            const std::string text = r.Get("window-size");
            const std::size_t x = text.find_first_of("xX");
            auto side = [](std::string_view digits, std::uint32_t& out)
            {
                if (digits.empty() || digits.size() > 5)
                    return false;
                std::uint32_t v = 0;
                for (const char c : digits)
                {
                    if (c < '0' || c > '9')
                        return false;
                    v = v * 10u + static_cast<std::uint32_t>(c - '0');
                }
                out = v;
                return v >= kMinWindowSide && v <= kMaxWindowSide;
            };
            std::uint32_t w = 0, h = 0;
            if (x == std::string::npos ||
                !side(std::string_view(text).substr(0, x), w) ||
                !side(std::string_view(text).substr(x + 1), h))
            {
                std::fprintf(stderr, "error: --window-size wants WxH with each side in [%u, %u] "
                                     "(e.g. 1920x1080), got '%s'\n",
                             kMinWindowSide, kMaxWindowSide, text.c_str());
                return { std::nullopt, 2 };
            }
            if (cfg.maxFrames == 0)
            {
                std::fprintf(stderr, "error: --window-size requires --frames N (it is an "
                                     "automation extent, not a user preference)\n");
                return { std::nullopt, 2 };
            }
            cfg.windowWidth = w;
            cfg.windowHeight = h;
        }

        // --screenshot only ever fires on the last frame (both hosts gate it on
        // `lastFrame`, which requires maxFrames != 0). Without --frames it
        // would silently exit 0 having written nothing -- refuse at parse time
        // instead, matching Cli's own error idiom (stderr + exit 2).
        if (!cfg.screenshotPath.empty() && cfg.maxFrames == 0)
        {
            std::fprintf(stderr, "error: --screenshot requires --frames N\n");
            return { std::nullopt, 2 };
        }

        // Refused rather than ignored -- rule 3. A flag that silently does nothing is
        // the worst failure mode an agent can meet: it exits 0 having done nothing,
        // and an agent reads that as success.
        //
        // "Was --fixed-dt supplied?" cannot be answered by comparing its resolved
        // value against the registered default -- a caller who explicitly passes
        // the default value is indistinguishable from one who never passed it at
        // all if we compare strings. r.Supplied("fixed-dt") answers the actual
        // question: did the command line contain this option.
        const bool wantsOffscreenOnly = !cfg.probes.empty() || !cfg.reportPath.empty()
                                      || r.Supplied("fixed-dt") || r.Supplied("fixed-time")
                                      || r.Supplied("settle")
                                      || r.Supplied("settle-timeout") || r.Supplied("compare");
        if (wantsOffscreenOnly && !cfg.headless)
        {
            std::fprintf(stderr, "error: --fixed-dt/--fixed-time/--probe/--report/--settle/"
                                 "--settle-timeout/--compare require --headless\n");
            return { std::nullopt, 2 };
        }
        if (!cfg.probes.empty() && cfg.maxFrames == 0)
        {
            std::fprintf(stderr, "error: --probe requires --frames N (the capture lands on the last frame)\n");
            return { std::nullopt, 2 };
        }
        // `<= 0.0` alone does NOT reject NaN: every comparison against NaN is
        // false, so `nan <= 0.0` is false and the value flowed straight through
        // into the frame clock. Same UB class as --max-diff-pixel-ratio's range
        // bug (fixed in 92792847), and refused the same way -- at the parse
        // boundary, not clamped.
        if (cfg.headless && (!std::isfinite(cfg.fixedDtSeconds) || cfg.fixedDtSeconds <= 0.0))
        {
            std::fprintf(stderr, "error: --fixed-dt wants a positive, finite number of seconds\n");
            return { std::nullopt, 2 };
        }
        // Same NaN reasoning as --fixed-dt directly above: `< 0.0` alone does
        // not reject NaN, so test isfinite explicitly. Zero IS allowed here
        // (unlike --fixed-dt, where zero means a stopped step) -- pinning the
        // scene clock to t=0 is a legitimate request.
        if (cfg.fixedTimeSeconds.has_value() &&
            (!std::isfinite(*cfg.fixedTimeSeconds) || *cfg.fixedTimeSeconds < 0.0))
        {
            std::fprintf(stderr, "error: --fixed-time wants a non-negative, finite number of seconds\n");
            return { std::nullopt, 2 };
        }
        // --play-as: a VALUE check, not a Cli::Choices() one -- the registered
        // default is "" (absent = an ordinary Edit-mode boot) and an empty string
        // is not one of the choices, so registering them would refuse every run
        // that never passed the flag. A misspelled topology must still be a
        // refusal rather than a silent fall-through to Standalone: "the editor
        // played, just not the way you asked" is exactly the invisible wrong
        // answer rule 3 exists to prevent.
        if (!cfg.playAs.empty() && cfg.playAs != "standalone" && cfg.playAs != "listen-server"
            && cfg.playAs != "embedded-server" && cfg.playAs != "client")
        {
            // A stderr one-liner and stop, no PrintUsage(): every sibling
            // post-parse refusal in this file does exactly that, and this
            // message already names all four accepted values, so a usage dump
            // would add a screen of noise and no information.
            std::fprintf(stderr, "error: --play-as must be one of standalone | listen-server | "
                                 "embedded-server | client\n");
            return { std::nullopt, 2 };
        }
        // "Was --settle supplied" mirrors --fixed-dt's own r.Supplied() reasoning
        // just above: a caller who explicitly typed the value that also means
        // "off" gets a refusal, not a silent no-op indistinguishable from never
        // having passed the flag.
        if (r.Supplied("settle") && cfg.settleAttempts == 0)
        {
            std::fprintf(stderr, "error: --settle wants a positive attempt count (0 means \"off\", "
                                 "which is what omitting the flag already means)\n");
            return { std::nullopt, 2 };
        }
        // Fix round 1, item 3: 1 is ACCEPTED syntax but a GUARANTEED failure --
        // the first settle attempt has no prior capture to compare against
        // (RuntimeFrame.cpp's io.previousCaptureValid starts false), so it can
        // never converge, on any scene, by construction. Refusing it here is
        // the same treatment the explicit-0 case just above gets: a value that
        // parses cleanly and then always fails is a worse trap than a refusal,
        // because the caller only discovers it after the run.
        if (cfg.settleAttempts == 1)
        {
            std::fprintf(stderr, "error: --settle needs at least 2 attempts to compare (one "
                                 "capture has nothing to compare against, so it would ALWAYS "
                                 "fail to converge)\n");
            return { std::nullopt, 2 };
        }
        // A timeout bounds a loop that --settle turns on. Supplying one without
        // the other is a caller error, refused rather than ignored -- the same
        // treatment --max-diff-pixels gets without --compare.
        if (r.Supplied("settle-timeout") && cfg.settleAttempts == 0)
        {
            std::fprintf(stderr, "error: --settle-timeout requires --settle (it bounds the settle "
                                 "loop, which --settle is what turns on)\n");
            return { std::nullopt, 2 };
        }
        // --select-in-document selects INSIDE the document --open-asset opens
        // (inspector-ownership spec A s4); without that flag there is nothing
        // to select inside, so it is refused rather than silently ignored.
        if (r.Supplied("select-in-document") && cfg.openAsset.empty())
        {
            std::fprintf(stderr, "error: --select-in-document requires --open-asset (it selects INSIDE the document that flag opens)\n");
            return { std::nullopt, 2 };
        }
        // Settle compares CAPTURED frames (RuntimeFrame.cpp's CaptureTail) -- with
        // neither --screenshot nor --report there is nowhere for that comparison
        // to land, and (more importantly) nothing would ever arm the capture node
        // that convergence is measured against, so the loop would spin until the
        // OS killed it rather than ever reporting a result.
        if (cfg.settleAttempts != 0 && cfg.screenshotPath.empty() && cfg.reportPath.empty())
        {
            std::fprintf(stderr, "error: --settle requires --screenshot or --report (it compares "
                                 "captured frames; with neither, there is nowhere to land the "
                                 "result and the run would never know when to stop)\n");
            return { std::nullopt, 2 };
        }
        // --compare / --bless (Task 8).
        //
        // Finding 1 (dispatch audit): every refusal below tests
        // cfg.compareReference.empty() as the "not supplied" sentinel, which
        // conflates "not supplied" with "supplied empty" -- the same
        // r.Supplied() idiom --settle already needs one screen up, because
        // "" also happens to be the registered default. Without this check,
        // `--compare ""` would parse clean and silently disable the whole
        // comparison -- exactly the Rule-3 silent inertness this file
        // refuses everywhere else.
        if (r.Supplied("compare") && cfg.compareReference.empty())
        {
            std::fprintf(stderr, "error: --compare wants a reference name (an empty value means "
                                 "\"off\", which is what omitting the flag already means)\n");
            return { std::nullopt, 2 };
        }
        // Finding 2: an unsafe name is refused HERE, at PARSE time, with its
        // own message -- not deferred to ResolveReference. Deferring it
        // would have the run reach RuntimeApp's resolve-before-the-loop
        // step, get back ReferenceLevel::None (ResolveReference refuses an
        // unsafe name the exact same way it reports a genuinely absent
        // file), and exit "compare-missing-reference" -- which is a LIE: the
        // name was refused, not missing, and those are two different bugs
        // for an agent to chase. Reuses ReferenceImages' own predicate
        // (Arcane::ReferenceNameIsSafe) rather than a second copy of the
        // same five checks -- see that function's header comment for why.
        if (!cfg.compareReference.empty() && !Arcane::ReferenceNameIsSafe(cfg.compareReference))
        {
            std::fprintf(stderr, "error: --compare '%s' is not a safe reference name (no '/', "
                                 "'\\', \"..\", or a leading '.' -- it is resolved into a file path "
                                 "under the project)\n", cfg.compareReference.c_str());
            return { std::nullopt, 2 };
        }
        // --compare needs a CONVERGED frame to be a verdict rather than a
        // frame number, so it requires --settle, which itself already requires
        // --headless and --screenshot/--report.
        if (!cfg.compareReference.empty() && cfg.settleAttempts == 0)
        {
            std::fprintf(stderr, "error: --compare requires --settle (comparing an unconverged "
                                 "frame reports which frame you got, not whether it is right)\n");
            return { std::nullopt, 2 };
        }
        // Rule 3: no silently inert flags.
        if (cfg.bless && cfg.compareReference.empty())
        {
            std::fprintf(stderr, "error: --bless has nothing to bless (--compare was not given)\n");
            return { std::nullopt, 2 };
        }
        if ((r.Supplied("max-diff-pixels") || r.Supplied("max-diff-pixel-ratio"))
            && cfg.compareReference.empty())
        {
            std::fprintf(stderr, "error: --max-diff-pixels/--max-diff-pixel-ratio require "
                                 "--compare\n");
            return { std::nullopt, 2 };
        }
        // Final-review I-1: --max-diff-pixel-ratio is RANGE-refused here, at the
        // parse boundary, and NOT clamped.
        //
        // Cli's NumericOk (Cli.cpp:69-89) only asks whether std::from_chars can
        // consume the whole token as a double -- which it can for "-1", "1e30",
        // "inf" and "nan" alike (from_chars accepts a leading '-' and the
        // inf/nan spellings). The value then flows, unchecked, into
        // ImageCompare.cpp's `static_cast<std::uint64_t>(expectedArea * ratio)`.
        // Converting a negative, non-finite, or out-of-uint64-range double to an
        // unsigned integer type is UNDEFINED BEHAVIOUR ([conv.fpint]); on MSVC/
        // x64 it yields an unspecified value, so `--max-diff-pixel-ratio 1e30`
        // ("forgive everything") can silently produce a budget of ZERO -- the
        // strictest possible verdict from the loosest possible request. An agent
        // reading that verdict has no way to tell it apart from a genuine
        // regression.
        //
        // Upper bound is 1.0 because the value is a FRACTION OF THE REFERENCE'S
        // AREA (see the --max-diff-pixel-ratio help text and ImageCompare.cpp's
        // `expectedArea * ratio`): 1.0 already means "every pixel may differ",
        // and there is nothing above it to ask for.
        //
        // NOT CLAMPED, deliberately: clamping is the silent-wrong-answer shape
        // this file refuses everywhere else (rule 3). A caller who typed a value
        // outside the range typed something they did not mean, and finding that
        // out at parse time is cheaper than finding it out from a verdict.
        if (cfg.maxDiffPixelRatio.has_value()
            && !(std::isfinite(*cfg.maxDiffPixelRatio)
                 && *cfg.maxDiffPixelRatio >= 0.0
                 && *cfg.maxDiffPixelRatio <= 1.0))
        {
            std::fprintf(stderr, "error: --max-diff-pixel-ratio wants a finite fraction in "
                                 "[0, 1] (it is a fraction of the reference image's area, so "
                                 "1 already means \"every pixel may differ\"), got '%s'\n",
                         r.Get("max-diff-pixel-ratio").c_str());
            return { std::nullopt, 2 };
        }
        // AN OPEN-ENDED HEADLESS RUN CANNOT BE STOPPED, and that is a
        // stronger claim than "it is inconvenient". A windowed run has two
        // exits the frame loop honours -- the window's close button
        // (WindowEvents::quitRequested) and the `quit` input action, which
        // needs the window FOCUSED to deliver a key. --headless never maps
        // the window, so it has neither: no titlebar to click and nothing the
        // keyboard can reach. maxFrames == 0 therefore means "run forever,
        // killable only from outside the process".
        //
        // That is exactly backwards for the workflow this mode exists for. An
        // agent spawning a bare `--headless` spawns a process it cannot stop
        // by any means it owns, and it will sit there until something else
        // reaps it. Refuse at parse time -- the same treatment, and the same
        // idiom, --probe and --screenshot already get one screen up.
        if (cfg.headless && cfg.maxFrames == 0)
        {
            std::fprintf(stderr, "error: --headless requires --frames N (an unmapped window has no "
                                 "close button and cannot be focused for the quit action, so an "
                                 "open-ended run has no way to stop itself)\n");
            return { std::nullopt, 2 };
        }

#if !defined(ARC_BUILD_DIST)
        // --pick-probe x,y. Parsed HERE rather than at
        // the use site, and refused rather than clamped, because the whole
        // value of the flag is being scriptable: a probe whose coordinate was
        // silently reinterpreted would report a confident id for a pixel
        // nobody asked about. Cli has no pair type (documented non-goal), so
        // the split is done by hand -- the refusals below are exactly the
        // parse-time treatment --backend metal gets.
        {
            const std::string probe = r.Get("pick-probe");
            if (!probe.empty())
            {
                const std::size_t comma = probe.find(',');
                bool bad = comma == std::string::npos || comma == 0 || comma + 1 >= probe.size();
                long long x = 0, y = 0;
                if (!bad)
                {
                    const std::string xs = probe.substr(0, comma);
                    const std::string ys = probe.substr(comma + 1);
                    const auto whole = [](const std::string& text, long long& out)
                    {
                        if (text.empty())
                            return false;
                        out = 0;
                        for (const char c : text)
                        {
                            if (c < '0' || c > '9')
                                return false;
                            out = out * 10 + (c - '0');
                            if (out > 0x7FFFFFFFll)
                                return false;
                        }
                        return true;
                    };
                    // Unsigned on purpose: a NEGATIVE probe pixel is not a
                    // coordinate, it is a typo -- the id target has no texels
                    // there and the run would report a miss that means nothing.
                    bad = !whole(xs, x) || !whole(ys, y);
                }
                if (bad)
                {
                    std::fprintf(stderr, "error: --pick-probe wants two non-negative integers as "
                                         "x,y (canvas pixels, y down) -- e.g. --pick-probe 640,360\n");
                    return { std::nullopt, 2 };
                }

                // Refused rather than silently no-op'd: the readback lands
                // with frames-in-flight latency, so an open-ended run never
                // reports. The nodes themselves are always available -- the
                // NRI frame graph is the only render path.
                if (cfg.maxFrames == 0)
                {
                    std::fprintf(stderr, "error: --pick-probe requires --frames N (the readback "
                                         "lands a couple of frames after the pass that wrote it)\n");
                    return { std::nullopt, 2 };
                }

                // ...AND IT DOES NOT WORK OFFSCREEN YET, so refuse it there
                // rather than let the run answer wrongly. Arming is declined
                // on an offscreen context BY CONSTRUCTION (NriGraphContext.cpp
                // -- `m_pickArmed = config.pickProbe && !IsOffscreen()`, whose
                // own comment explains that the editor's viewport drives its
                // probe per frame through FrameDesc::pickPixel instead). So
                // the pick nodes are never built, no readback ever lands, and
                // the run exits 1 reporting "the run was too short ... or the
                // probe pixel was outside the surface" -- BOTH of which are
                // false. A confidently wrong diagnosis costs more than a
                // refusal, and costs most to the agent this mode is for.
                //
                // Temporary by intent: when the offscreen pick is driven
                // per frame through FrameDesc::pickPixel, this refusal is what
                // gets deleted.
                if (cfg.headless)
                {
                    std::fprintf(stderr, "error: --pick-probe does not work with --headless yet "
                                         "(an offscreen context declines the fixed probe pixel, so "
                                         "no readback would ever land and the run would report a "
                                         "miss it never measured)\n");
                    return { std::nullopt, 2 };
                }

                cfg.pickProbe  = true;
                cfg.pickProbeX = (std::int32_t)x;
                cfg.pickProbeY = (std::int32_t)y;
            }
        }
#endif

        return { cfg, 0 };
    }
}
