#pragma once

// VerifyReport: turns an offscreen host's capture into machine-readable FACTS
// an agent can assert on -- probe specs (`brightness@640,360`, `census`, ...),
// their evaluation against one captured frame, and a JSON report.
//
// This is the boundary between the engine tier and the separate, optional
// Servitor package: Servitor parses this JSON WITHOUT linking the engine, so
// `schemaVersion` is a compatibility promise, not decoration -- the same role
// `.arcproj`'s `formatVersion` and the engine's own `abi` play elsewhere. The
// engine's job stops at emitting facts; there is no assertion DSL here, and
// none belongs here -- that lives on the Servitor side of the boundary.
//
// Wiring: HostConfig (Task 2) carries `probes` (raw, unparsed, in command-line
// order) and `reportPath`. Parsing probe CONTENTS is deliberately this
// component's job, not HostConfig's -- see HostConfig.hpp's `probes` comment.
// Feeding a real capture/census into this component from a running host is
// Task 8, not here: this header only defines the component and its contract.
//
// CAPTURE FORMAT ASSUMPTION (read before touching Brightness/Luma below):
// SetCapture's bytes come from ReadCapture, which reads back
// kGraphOffscreenFormat -- BGRA8_UNORM, NOT a `_SRGB` format
// (NriGraphContext.hpp:284-293's own comment: "Display-referred... the
// tonemap already gamma-2.2 encodes, so a plain UNORM target matches what a
// real backbuffer shows"). That means every byte SetCapture receives is
// ALREADY gamma-encoded (display-referred), not scene-linear. Luma (Y') is
// defined ON gamma-encoded values -- its weights apply directly to these
// bytes, no linearisation step. Do not add an sRGB-to-linear conversion here;
// that would be correct for LUMINANCE (Y), which this is deliberately not.

#include <Arcane/Base/Api.hpp>
#include <Arcane/Core/Constant.hpp>
// SettleBail, the --settle bail decision (Task 2). Header-only and constexpr
// -- <cstdint> is its ONLY include -- so pulling it in here adds no link
// dependency and leaves this component's standalone-parse property intact:
// VerifyReportTest.cpp still parses this report's JSON without linking a
// renderer, which is the Servitor boundary this whole file exists to hold.
#include <Arcane/Host/SettleBound.hpp>
// ForeignModules::Match, what SetForeignModules takes (schemaVersion 10). A
// Core header with no render dependency -- the standalone-parse property
// above is untouched: the test still links nothing beyond Core and Client.
#include <Arcane/Base/ForeignModules.hpp>

#include <Json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    // The probe vocabulary. Brightness/Luma/Rgba/Pick are POSITIONAL
    // (`kind@x,y`); Census is ARGLESS (`census`, no `@...`) -- it asks a
    // question about the whole scene's material binding, not one pixel. See
    // ParseProbe for the exact grammar and ProbeSpec below for which fields
    // each kind actually uses.
    //
    // One line each, because an agent reads the JSON this produces, not this
    // header -- but "what does this number mean" has to be answerable here,
    // without archaeology, for the next engineer:
    //   Brightness -- unweighted (R+G+B) / 765, NriGraphPixelTest.cpp's own
    //                 Luma() reused verbatim (see VerifyReport.cpp). Renamed
    //                 FROM "luma" because it is not perceptual -- pure red
    //                 and pure green both read ~0.33 under this formula
    //                 despite being ~3x apart in perceived brightness.
    //   Luma       -- Rec.709 Y': 0.2126*R + 0.7152*G + 0.0722*B, weights
    //                 applied DIRECTLY to the gamma-encoded byte values (see
    //                 the capture-format note above), normalised to [0,1].
    //   Rgba       -- the four raw channel bytes at (x, y), 0-255 each.
    //   Pick       -- the entity at (x, y), reported as a DURABLE identity
    //                 (Identity.id's Guid + Identity.name), not the raw
    //                 hit-proxy uint32 the id pass wrote -- see SetPick's
    //                 comment for why the two must never be conflated.
    //   Census     -- the six SceneRenderResolver::MaterialCensus counts.
    enum class ProbeKind : std::uint8_t { Brightness, Luma, Rgba, Pick, Census };

    struct ProbeSpec
    {
        ProbeKind    kind{};
        std::int32_t x = 0, y = 0;   // only meaningful for Brightness/Luma/Rgba/Pick
        std::string  raw;            // as typed, echoed into the report
    };

    // Parses one `--probe` argument's text (HostConfig::probes elements, one
    // call each). nullopt + `error` on anything malformed: a positional kind
    // with no `@x,y`, an argless kind WITH `@...`, a non-numeric or negative
    // coordinate, or a kind this build does not know. This is a REFUSAL, not
    // a silent skip -- a probe an agent asked for and silently never got is a
    // worse failure than one that never ran, because it is invisible in the
    // report (`ArcaneRuntime --probe brigtness@1,2` typo'd would otherwise
    // just vanish rather than tell anyone).
    //
    // Coordinates are unsigned on purpose, matching --pick-probe's own parse
    // (HostConfig.cpp): a NEGATIVE probe pixel is not a coordinate, it is a
    // typo, and evaluating it would report a fact about the wrong question.
    [[nodiscard]] ARC_API std::optional<ProbeSpec> ParseProbe(std::string_view text, std::string& error);

    // The FIRST `pick@x,y` spec among `probes` (HostConfig::probes, raw and
    // unparsed, in command-line order), or nullopt if none is present. ONE
    // pixel is ever probed per run -- FrameDesc::pickPixel is armed once
    // (RuntimeFrame.cpp's RenderGraph), from this same spec -- so this
    // helper is the SINGLE source of truth for which pixel that is, shared
    // by the frame driver (arms it, every frame) and the report writer
    // (resolves + describes it, once, at shutdown) so the two can never
    // disagree about which spec won. A run with more than one `pick@` probe
    // only ever gets an answer for this one; Evaluate's Pick case refuses
    // the others rather than silently answering the wrong question.
    //
    // Malformed specs among `probes` are silently skipped here -- ParseProbe's
    // error is reported once, later, by the real parse-and-log pass
    // (RuntimeApp.cpp's ShutdownGraphPath); this helper only answers "is
    // there a pick request", and must not double that log just to answer it.
    [[nodiscard]] ARC_API std::optional<ProbeSpec> FirstPickProbe(const std::vector<std::string>& probes);

    // Whether canvas pixel (x, y) is inside a `width`x`height` surface (fix
    // round 1, item 2). Pulled out as its own pure, testable predicate
    // because --pick-probe's OWN out-of-range latch (NriGraphContext.cpp's
    // m_probeOutOfRange) is keyed to config.pickProbe -- the OLD flag's
    // state -- and is never armed on this path (FrameDesc::pickPixel "needs
    // no arming at all"). Left unchecked, NriGraphContext::ProbeId() would
    // happily return the CLAMPED EDGE TEXEL's hit-proxy id for an
    // out-of-range request like `pick@9999,9999` -- a confident id for a
    // pixel nobody asked about, exactly the failure the flag's own latch
    // exists to prevent. RuntimeApp.cpp's ShutdownGraphPath calls this
    // BEFORE trusting ProbeId() so an out-of-range probe never reaches it at
    // all. Negative coordinates never happen in practice (ParseProbe only
    // accepts non-negative ones), but are refused here too for a caller that
    // has not gone through that parse.
    [[nodiscard]] ARC_API bool PickPixelInRange(std::int32_t x, std::int32_t y,
                                                    std::uint32_t width, std::uint32_t height) noexcept;

    // ONE WORLD a host was running when it wrote its report (schemaVersion 6,
    // Core-DLL split plan 1 Task 7). A process is no longer one world: the
    // editor's "client + embedded server" play mode runs a Client world and a
    // DedicatedServer world side by side on one ProcessContext, and an agent
    // that can only see the process cannot otherwise tell whether the second
    // one came up at all -- let alone whether it got the same scene.
    //
    //   role               -- Arcane::ToString(Runtime::Mode()): "Standalone" |
    //                         "DedicatedServer" | "ListenServer" | "Client".
    //   hasAuthority       -- Runtime::HasAuthority(): true for every role but
    //                         Client. Carried as its own field rather than left
    //                         to be re-derived from `role`, so a consumer across
    //                         the Servitor boundary never has to hardcode the
    //                         engine's role->authority table.
    //   entities           -- how many entities that world's registry holds. Two
    //                         worlds seeded from the same scene report the same
    //                         number; that equality is the fact an embedded-server
    //                         scenario actually asserts.
    //   fixedUpdateSystems -- the size of that world's fixedUpdate scheduler: the
    //                         engine pair plus whatever the module's ROLE-MASKED
    //                         factories instantiated for this world's NetMode, so
    //                         a client and a server world legitimately differ.
    struct WorldFact
    {
        std::string   role;
        bool          hasAuthority = false;
        std::uint64_t entities = 0;
        std::uint64_t fixedUpdateSystems = 0;
    };

    // Accumulates one host run's observations and renders them as one JSON
    // document. Every setter is independent and optional except Evaluate,
    // which reads back whatever SetCapture/AddCensus have been given so far
    // -- call them BEFORE Evaluate, not after, or a probe that needed that
    // data reports "not set" rather than picking up a later call.
    class ARC_API VerifyReport
    {
    public:
        // The report's own compatibility promise across the Servitor boundary.
        // A RANGE rather than a bare number (adopted from UE's
        // FImageComparisonResult, which pairs CurrentVersion with
        // OldestSupportedVersion): a consumer declares the span it understands,
        // and IsSupportedSchemaVersion answers whether a document is readable
        // at all rather than leaving every consumer to hardcode one integer.
        //
        // 4 added compare.maxLocalDifference. 5 added compare.triedPaths --
        // the ordered candidate list ResolveReference actually probed
        // (Arcane::ReferenceResolution::triedPaths), so an agent debugging a
        // "resolvedLevel none" verdict can see WHERE this run looked rather
        // than guessing from the naming convention. 6 added `worlds` -- one
        // entry per live Runtime (WorldFact above), because a process is no
        // longer one world. 3, 4 and 5 remain readable: every field a 3-, 4- or
        // 5-era consumer knows is still emitted with the same meaning, and
        // `worlds` is ABSENT on any run that did not set it. 7 added
        // `viewMode` (F4 plan 1 T12, spec s9) -- the editor viewport's
        // resolved mode, "2d" | "perspective", so a perspective witness can
        // assert the host actually booted into the mode --view-mode asked for
        // rather than inferring it from pixels; ABSENT on every host that has
        // no view mode (the runtime) and on any run that did not set it. 3..6
        // remain readable by the same rule.
        //
        // Bumped 7 -> 8 by F3 plan 1 T8: the report gained `visibility` (see
        // SetVisibility) -- the last frame's GPU-scene counts (every live
        // row, the coarse-visible ones, batches, draws), the fact a 3D
        // witness asserts on. ABSENT on any run that never set it; 3..7
        // remain readable by the same rule.
        //
        // Bumped 8 -> 9 by F3 plan 2 T5, and the bump is the POINT rather than
        // bookkeeping: `visibility` gained `transparentRows` (the direct
        // records the indirect path never draws, so a consumer can check
        // draws == batches + transparentRows itself) and `gpuVisible` STOPPED
        // BEING A COPY OF coarseVisible. It is now the GPU cull's own count,
        // read back asynchronously, and it is `null` whenever no readback has
        // completed -- an 8-era consumer that read it as a number would
        // otherwise silently read the CPU's expectation as the GPU's answer.
        // A nullable field and a changed meaning are exactly what a version
        // boundary exists for; 3..8 remain readable by the same rule for
        // every OTHER field.
        //
        // Bumped 9 -> 10 by the injected-overlay detection (the editor
        // close-crash follow-up, 2026-09-22): the report gained
        // `foreignModules` (see SetForeignModules) -- the third-party modules
        // the host found injected into its process, each with product and
        // tier, so a red lane on a desk with GPU Tweak III's OSD or RTSS in
        // it is attributable from the report alone. ABSENT on any run that
        // never scanned; an EMPTY array on one that scanned and found nothing.
        // 3..9 remain readable by the same rule.
        //
        // Bumped 10 -> 11 by the inspector-ownership arc: `inspector` {source,
        // breadcrumb} -- see SetInspector. Absent on the runtime host and any
        // run that never set it; 10 remains readable.
        //
        // 12: inspector.instances (inspector filters) -- one {id, excluded,
        // source, breadcrumb} entry per Inspector instance, what each one's
        // filter admits, which source it routed to and the breadcrumb it
        // shows (see SetInspector). Present whenever
        // `inspector` is; 11 remains readable.
        //
        // 13: documents (node page + editor upgrades s3.2) -- one {guid, kind,
        // name, compile, preview, image} entry per open shader or mesh
        // document: its PreviewStatus ids (see SetDocumentPreviews). Absent
        // unless the editor set it; 12 remains readable.
        //
        // 14: cvarSets (settings arc S6-GATE) -- one {name, value} entry per
        // --set the host was given: the PUBLISHED value after boot (null for
        // a name no cvar answers to), so a witness asserts that its --set
        // landed instead of only that no warning was logged. Absent unless
        // the host set it; 13 remains readable.
        ARC_CONSTANT("file format: verify-report schema version; readers gate on it")
        static constexpr int kSchemaVersion                = 14;
        ARC_CONSTANT("file format: verify-report schema version; readers gate on it")
        static constexpr int kOldestSupportedSchemaVersion  = 3;

        [[nodiscard]] static constexpr bool IsSupportedSchemaVersion(int v) noexcept
        {
            return v >= kOldestSupportedSchemaVersion && v <= kSchemaVersion;
        }

        // The run's own identity: which backend rendered it, how many frames
        // it completed, and why it stopped (mirrors the exit-reason
        // vocabulary the offscreen loop already logs -- "frames-complete", a
        // gpu-stall watchdog verdict, etc).
        //
        // NO `offscreen` PARAMETER (final fix wave, Fix 3 -- reconciling
        // windowed --report): this report can only ever describe a headless
        // run. HostConfig::Parse's wantsOffscreenOnly gate refuses --report
        // without --headless, unconditionally, for every host -- so
        // ArcaneRuntime's one real call site (RuntimeApp.cpp's
        // ShutdownGraphPath) could never have reached this with a windowed
        // run. A `bool offscreen` parameter that is provably always true is
        // not a fact worth carrying; ToJson's "mode" field reflects that by
        // always emitting ONE value rather than branching on a state that can
        // never be anything else. Older revisions of this component modelled a
        // "windowed" mode for a code path RuntimeApp.cpp never actually had a
        // way to reach -- see the plan's Task 9 desk item F, which describes a
        // scenario the parse-time gate makes unrunnable.
        //
        // THAT VALUE IS "headless" as of schemaVersion 3 (it was "offscreen"):
        // the MODE is spelled --headless on every host's command line, and
        // "offscreen" is the TECHNIQUE it renders with (CreateOffscreen,
        // IsOffscreen -- both keep the word, deliberately). A wire value an
        // out-of-process consumer switches on has to carry the mode's own
        // word, and a schemaVersion bump is exactly the versioned moment at
        // which changing one is legitimate rather than a silent break.
        void SetRun(std::string backend, std::uint64_t framesRendered,
                    std::string exitReason);

        // The captured frame Brightness/Luma/Rgba probes read against. `rgba` is TIGHT
        // RGBA8 (row stride == w*4), the same shape ReadCapture hands back --
        // see NriGraphPixelTest.cpp's `At()` helper, which this reuses the
        // indexing math from.
        void SetCapture(std::uint32_t w, std::uint32_t h, const std::vector<unsigned char>& rgba);

        // The scene's material-binding facts a Census probe reads -- the same
        // six counts SceneRenderResolver::MaterialCensus already computes
        // (SceneRenderResolver.hpp). VerifyReport does not compute these
        // itself: it has no scene to look at, only whatever a host (Task 8)
        // hands it.
        void AddCensus(int spriteReferenced, int spriteBound, bool postReferenced,
                       bool postBound, int meshReferenced, int meshBound);

        // The pick readback (Task 9): the entity-id probe's answer, resolved
        // to a durable identity HOST-SIDE, before this call -- VerifyReport
        // has no registry to invert a hit-proxy id itself (the same "no
        // scene to look at" reasoning AddCensus's own comment gives for
        // Census). `armedX`/`armedY` is the ONE pixel FrameDesc::pickPixel
        // was set to this run (FirstPickProbe's spec) -- Evaluate's Pick
        // case refuses any `pick@x,y` probe asking about a DIFFERENT pixel,
        // rather than answering a question this run never measured.
        //
        //   landed      -- false means the readback never arrived
        //                  (NriGraphContext::ProbeId() returned nullopt: the
        //                  run was too short, OR the pixel was outside the
        //                  surface). These are TWO DIFFERENT FACTS, not one
        //                  -- fix round 1 split them, mirroring how
        //                  Brightness/Luma/Rgba already distinguish "no
        //                  capture set" from "pixel outside capture"
        //                  (ReadTexel): "the run never captured anything" is
        //                  not the same claim as "you asked about a pixel
        //                  that does not exist", and an agent needs to tell
        //                  them apart. Evaluate derives WHICH of the two
        //                  applies itself, via PickPixelInRange against
        //                  surfaceWidth/surfaceHeight below -- it does not
        //                  trust a host-computed bool for that -- and reuses
        //                  RuntimeApp.cpp's "NO READBACK LANDED" wording only
        //                  for the genuine too-short case, now that the
        //                  out-of-range case has its own distinct message.
        //   surfaceWidth/surfaceHeight -- the pick id-buffer's own extent
        //                  (NriGraphContext::SurfaceWidth/Height), NOT the
        //                  same thing as SetCapture's capture buffer (a
        //                  different readback entirely) -- what
        //                  PickPixelInRange checks armedX/armedY against.
        //                  0/0 (the default) means "the caller did not know
        //                  or did not pass it", which Evaluate treats as
        //                  "cannot tell out-of-range from too-short" and
        //                  falls back to the combined wording -- a real
        //                  offscreen surface is never 0x0 (CreateOffscreen
        //                  refuses that extent), so 0/0 is an unambiguous
        //                  "unknown" sentinel, not a legitimate size.
        //   hitProxyId  -- the RAW, frame-scoped id the id pass wrote (0 ==
        //                  background). Carried into the report for
        //                  debugging ONLY -- meaningless outside the one
        //                  draw submission that produced it, and NEVER the
        //                  field an agent should address an entity by (see
        //                  entityGuid below).
        //   resolved    -- true iff hitProxyId named a live entity that ALSO
        //                  carries an Identity component, i.e.
        //                  entityName/entityGuid are meaningful. Only
        //                  consulted when hitProxyId != 0 -- a background
        //                  hit (id 0) is a FACT regardless of this flag, not
        //                  routed through it.
        //   entityName/entityGuid -- Identity::name / Identity::id.ToString()
        //                  (canonical lowercase 8-4-4-4-12 hex): the durable
        //                  pair an agent bootstraps a readable query from and
        //                  then addresses by id. Ignored unless
        //                  resolved && hitProxyId != 0.
        void SetPick(std::int32_t armedX, std::int32_t armedY, bool landed,
                     std::uint32_t hitProxyId, bool resolved,
                     std::string entityName, std::string entityGuid,
                     std::uint32_t surfaceWidth = 0, std::uint32_t surfaceHeight = 0);

        // The --compare verdict (Task 8). Emitted as a `compare` object ONLY
        // when this was actually called -- a run without --compare emits NO
        // such key, so an agent can distinguish "not asked" from "asked and
        // passed" (the same absence-must-be-absence contract SetCapture's
        // m_captureSet already upholds for probes). RuntimeApp::
        // ShutdownGraphPath is the one real caller, on every run that named
        // --compare -- converged-and-matched, converged-and-mismatched,
        // budget-exhausted, blessed, or resolution-missing all funnel
        // through this same call with different arguments, rather than each
        // growing its own reporting path.
        //
        //   reference      -- the bare --compare name, echoed back.
        //   resolvedLevel  -- "none" | "shared" | "backend" | "adapter", mirroring
        //                     Arcane::ReferenceLevel. "none" means no
        //                     reference existed (or, on a first --bless,
        //                     existed only after this run wrote it) -- NEVER
        //                     conflate this with a zero-difference pass; see
        //                     the missing-reference test case below.
        //   referencePath  -- the file this run actually compared against
        //                     (or blessed to), empty iff resolvedLevel is
        //                     "none" and nothing was blessed either.
        //   passed         -- true iff the comparison passed, OR a bless
        //                     wrote the reference this run (the reference
        //                     now IS the capture, by construction) -- never
        //                     true merely because nothing was compared.
        //   diffCount/diffRatio/maxDiffPixels/sizesMismatch -- lifted
        //                     verbatim from Arcane::ImageCompareResult
        //                     (CompareImages, Task 5); zero/false on a
        //                     bless or a missing-reference run, where no
        //                     comparison ever ran.
        //   diffPath       -- the diff artifact's path, written only on a
        //                     genuine mismatch; empty on a pass, a bless, or
        //                     a missing reference -- never absent, so an
        //                     agent can tell "no diff was needed" from
        //                     "SetCompare was never called" (the same
        //                     empty-not-absent contract diffPath's own test
        //                     case pins).
        //   errorMessage   -- human-readable context: CompareImages' own
        //                     message on a mismatch, or this component's
        //                     own wording for a missing reference / an
        //                     unconverged run; empty on a pass or a bless.
        //   maxLocalDifference -- the largest per-block mismatch fraction over
        //                     ImageCompare's 10x10 grid, lifted from
        //                     Arcane::ImageCompareResult alongside diffCount.
        //                     Zero on a bless or a missing-reference run, where
        //                     no comparison ever ran -- same as diffCount.
        //   triedPaths     -- (schemaVersion 5) the candidate paths this run's
        //                     ResolveReference actually probed, in try order
        //                     (backend-keyed first, then shared; the resolved
        //                     candidate is always last) -- lifted verbatim from
        //                     Arcane::ReferenceResolution::triedPaths, each
        //                     path stringified with generic_string() so the
        //                     JSON carries forward slashes on every host OS.
        //                     Empty on a REFUSED --compare name (refusal is
        //                     not a search), same as ReferenceResolution's own
        //                     contract -- never absent while `compare` itself
        //                     is present, so an agent debugging a "resolvedLevel
        //                     none" verdict can see exactly where this run
        //                     looked instead of re-deriving the search order
        //                     from the naming convention.
        //
        // NON-DEFAULTED deliberately: both call sites (RuntimeApp::
        // ShutdownGraphPath, EditorApp's own report block) are ours, and a
        // default here would let either silently keep reporting an empty
        // list rather than being forced to thread the resolution's real one
        // through.
        void SetCompare(std::string reference, std::string resolvedLevel,
                        std::string referencePath, bool passed,
                        std::uint64_t diffCount, double diffRatio,
                        std::uint64_t maxDiffPixels, bool sizesMismatch,
                        std::string diffPath, std::string errorMessage,
                        double maxLocalDifference,
                        std::vector<std::string> triedPaths);

        // The --settle verdict (Task 3). Emitted as `settleAttemptsUsed` +
        // `settleBailReason` ONLY when this was actually called -- a run
        // without --settle emits NEITHER key, the same absence-must-be-absence
        // contract SetCapture and SetCompare already uphold, so an agent can
        // tell "settle was never asked for" from "asked, and took 0 attempts".
        // Both hosts call this from their report path whenever
        // HostConfig::settleAttempts != 0 AND the loop actually reached a
        // verdict, and only then.
        //
        // A NO-VERDICT CALL RECORDS NOTHING, and that is a contract rather than
        // a silent drop: when SettleVerdictReached(converged, bail) is false the
        // loop neither converged nor gave up on a bound, so there is no settle
        // fact to state and BOTH keys stay absent. That happens on any run that
        // dies AHEAD of the settle phase -- a missing --compare reference
        // (RuntimeApp::MainLoop's fail-fast resolve), device-lost,
        // render-failed, validation-errors. Enforced HERE, not only at the two
        // call sites, so a third host cannot reintroduce the defect: without
        // it, such a run reported settleAttemptsUsed 0 alongside a
        // settleBailReason of "timeout-bound" -- telling an agent to raise
        // --settle-timeout when no timeout had been spent and the real fault
        // was a missing file. exitReason already carries what actually
        // happened, so nothing an agent needs is lost by staying silent here.
        //
        //   attemptsUsed  -- the REAL number of settle attempts taken, never a
        //                    budget. This is the field the whole task exists
        //                    for: it lived only in logs before, and the editor
        //                    host used to FORGE it to the budget as its way of
        //                    stopping the loop (EditorAppFrame.cpp's defensive
        //                    branch, now a separate m_settleAborted flag).
        //                    Shipping the field without reconciling that would
        //                    have made this report LIE on one host, which is
        //                    strictly worse than the field's absence: an absent
        //                    field is an honest gap, a present wrong one is a
        //                    false report an agent will trust.
        //   converged     -- whether two consecutive captures ever compared
        //                    byte-equal with the shader compiler idle (and,
        //                    under --compare, matched the reference).
        //   bail          -- WHICH bound governed the give-up decision
        //                    (Arcane/Host/SettleBound.hpp). Reported so the
        //                    caller is told which knob would actually change
        //                    the outcome: "attempts-bound" says raise --settle,
        //                    "timeout-bound" says raise --settle-timeout. A
        //                    converged run carries SettleBail::Keep -- there is
        //                    no governing bound on a run that never gave up.
        //   captureFailed -- no readback EVER landed, so the loop never had two
        //                    frames to compare and NEITHER bound got a fair
        //                    test. OUTRANKS the bound in the emitted reason for
        //                    exactly that reason: "the capture path is broken"
        //                    and "the scene never stabilised" are different
        //                    facts with different fixes.
        void SetSettle(std::uint64_t attemptsUsed, bool converged, SettleBail bail,
                       bool captureFailed);

        // The run's WORLD SET (schemaVersion 6) -- see WorldFact above for what
        // each field means. Emitted as a `worlds` array ONLY when this was
        // actually called: the same absence-must-be-absence contract
        // SetCapture/SetCompare/SetSettle already uphold, so "this host does not
        // report worlds" stays distinguishable from "this run had none". Order is
        // the HOST's: the editor emits its own world first, then the embedded
        // server world when one is up.
        void SetWorlds(std::vector<WorldFact> worlds);

        // The editor viewport's RESOLVED view mode (schemaVersion 7, F4 plan 1
        // T12): "2d" | "perspective", the same spelling --view-mode takes
        // (HostConfig.hpp), read from the editor camera AFTER the seed and
        // every persisted layout have had their say -- so it is what the
        // capture was actually rendered through, never what was asked for.
        // Emitted as a top-level `viewMode` ONLY when this was called: the
        // runtime host has no editor camera and must not report one, the same
        // absence-must-be-absence contract every optional section above
        // upholds.
        void SetViewMode(std::string mode);

        // The editor Inspector's RESOLVED source (schemaVersion 11, the
        // inspector-ownership arc): the source's display name and the
        // breadcrumb it shows, read from the Inspector host at shutdown --
        // never from --select-in-document, so a scripted selection that failed
        // to apply reports what the Inspector actually showed. Emitted as a
        // top-level `inspector` {source, breadcrumb} ONLY when this was
        // called: the runtime host has no Inspector.
        //
        // `instances` (schemaVersion 12, inspector filters): one entry per
        // Inspector instance -- its id, its filter's excluded kind ids (catalog
        // order; empty = All) and the SourceName() of the source it routed to
        // ("" when it routed to none), and the breadcrumb THAT instance shows
        // (InspectorCrumbText over its source and the page it draws -- its
        // pinned PageFor or Page(); "" when it routed to none). Serialized as
        // `inspector.instances`, always present (possibly empty) when this
        // was called.
        struct InspectorInstance { int id = 0; std::vector<std::string> excluded; std::string source; std::string breadcrumb; };
        void SetInspector(std::string source, std::string breadcrumb, std::vector<InspectorInstance> instances = {});

        // The open preview documents (schemaVersion 13, node page + editor
        // upgrades s3.2): one entry per open shader or mesh document -- its
        // asset guid, its Inspector kind ("material" / "mesh"), its title, and
        // the PreviewStatus ids (Documents/PreviewStatus.hpp): compile
        // "not-compiled-here" | "compiler-unavailable" | "compiling" | "errors"
        // | "ok", preview "ready" | "no-device" | "vehicle-failed" |
        // "frame-failed", plus whether an image is bound. Emitted as a
        // top-level `documents` array ONLY when this was called: the editor
        // snapshots it beside SetInspector, before CloseAll; the runtime host
        // never calls it.
        struct DocumentPreview { std::string guid, kind, name, compile, preview; bool image = false; };
        void SetDocumentPreviews(std::vector<DocumentPreview> documents);

        // The --set list echoed back (schemaVersion 14, settings arc S6-GATE):
        // for each "name=value" the host was given, the cvar's PUBLISHED value
        // text at report time (FormatCVarValue), or no value when no cvar
        // answers to the name. Emitted as a top-level `cvarSets` array ONLY
        // when this was called; both hosts call it on every --report run, so
        // a run without --set reports `[]`.
        struct CVarSetEcho { std::string name; std::optional<std::string> value; };
        void SetCVarSets(std::vector<CVarSetEcho> sets);
        // Builds the echo from the host's --set list against the global
        // CVarRegistry (aliases resolve; a malformed item is skipped).
        [[nodiscard]] static std::vector<CVarSetEcho> EchoCVarSets(const std::vector<std::string>& sets);

        // The GPU scene's visibility counts (F3 plan 1 T8, spec s4/s5; plan 2
        // T5 for the last two arguments): the last frame's GpuSceneFrame as
        // the host saw it -- `total` live rows in the GPU-scene mirror (one
        // per drawable mesh section), `coarseVisible` of them inside the mesh
        // view's widened frustum, and the `batches` / `draws` the frame
        // emitted. Emitted as a top-level `visibility` block ONLY when this
        // was called (the same absence-must-be-absence contract every optional
        // section above upholds). VerifyReport does not compute any of these
        // itself: it has no scene to look at, only whatever a host hands it --
        // AddCensus's own rule.
        //
        //   transparentRows -- the frame's ordered DIRECT draw records
        //                  (GpuSceneFrame::transparentDraws). The indirect
        //                  path never draws these and the GPU cull never sees
        //                  them, so `draws` is `batches + transparentRows` by
        //                  construction -- carried as its own field so a
        //                  witness can assert that identity instead of
        //                  re-deriving it from a count it cannot see.
        //   gpuVisible  -- what the GPU CULL PASS ITSELF emitted, summed over
        //                  every indirect batch, from the most recently
        //                  COMPLETED asynchronous readback
        //                  (Host/GpuSceneHost.hpp's GpuSceneVisibleRows), or
        //                  NULLOPT when the ring is unarmed or no result has
        //                  landed yet -- which ToJson emits as `null`.
        //                  DELIBERATELY NOT DEFAULTED, and deliberately not
        //                  fillable from coarseVisible: through schemaVersion
        //                  8 this field WAS a copy of the CPU count, which
        //                  told every consumer the GPU had agreed when nothing
        //                  had asked it. "The GPU emitted N" and "the CPU
        //                  expected N" are different facts; a run that cannot
        //                  state the first must say so.
        void SetVisibility(std::uint32_t total, std::uint32_t coarseVisible,
                           std::uint32_t batches, std::uint32_t draws,
                           std::uint32_t transparentRows,
                           std::optional<std::uint32_t> gpuVisible);

        // The process's INJECTED third-party modules (schemaVersion 10): what
        // Arcane::ForeignModules::Scan found -- overlays and present-path
        // hooks matched by base name against Base/ForeignModules.hpp's table.
        // Emitted as a top-level `foreignModules` array of
        // `{ module, path, product, tier }` ONLY when this was called; both hosts
        // call it unconditionally from their report path (a fresh Scan at
        // that moment, so a module that injected after device creation is
        // still on record), so a clean desk reports `[]` and a consumer can
        // rely on the key. The consequence/remedy text stays in the log: an
        // agent acts on module + tier, a human reads the WARN line. VerifyReport
        // computes nothing here -- AddCensus's own rule.
        void SetForeignModules(std::vector<ForeignModules::Match> modules);

        // Evaluates every spec against whatever SetCapture/AddCensus/SetPick were
        // given before this call, and appends one JSON entry per spec.
        // Callable more than once (specs accumulate) -- there is no reset,
        // matching the one-shot lifetime a host uses this for (parse ->
        // evaluate once -> write report -> exit).
        void Evaluate(const std::vector<ProbeSpec>& specs);

        // Renders the accumulated report. Follows EngineInfoJson's precedent
        // (ProjectBoot.hpp) exactly: compact (`dump(-1, ' ', false, ...)`)
        // with `error_handler_t::replace` so a malformed byte anywhere in a
        // string field (an exit reason, a probe's raw text) degrades to
        // U+FFFD instead of THROWING OUT OF THE CALLER -- an agent-facing
        // report that crashes the host it was reporting on is worse than no
        // report at all.
        [[nodiscard]] std::string ToJson() const;

        // Writes ToJson()'s bytes to `path`. false on any open/write failure
        // (path does not exist, no permission, disk full); the caller decides
        // whether that is fatal. Not the atomic temp+ReplaceFile dance
        // SaveSceneFile uses -- this report has no prior version on disk to
        // protect from a partial overwrite, it is a fresh diagnostic artifact
        // every run.
        [[nodiscard]] bool WriteTo(const std::string& path) const;

    private:
        std::string   m_backend;
        std::uint64_t m_framesRendered = 0;
        std::string   m_exitReason;

        bool                       m_captureSet = false;
        std::uint32_t              m_captureWidth = 0, m_captureHeight = 0;
        std::vector<unsigned char> m_captureRgba;

        bool m_censusSet          = false;
        int  m_spriteReferenced   = 0;
        int  m_spriteBound        = 0;
        bool m_postReferenced     = false;
        bool m_postBound          = false;
        int  m_meshReferenced     = 0;
        int  m_meshBound          = 0;

        bool          m_pickSet        = false;
        std::int32_t  m_pickArmedX     = 0, m_pickArmedY = 0;
        bool          m_pickLanded     = false;
        // 0/0 = "unknown" (see SetPick's doc comment) -- a real offscreen
        // surface is never 0x0, so this sentinel is unambiguous.
        std::uint32_t m_pickSurfaceWidth = 0, m_pickSurfaceHeight = 0;
        std::uint32_t m_pickHitProxyId = 0;
        bool          m_pickResolved   = false;
        std::string   m_pickEntityName;
        std::string   m_pickEntityGuid;

        // The --compare verdict (Task 8) -- see SetCompare's own comment for
        // what each field means and why m_compareSet gates emission the same
        // way m_captureSet/m_pickSet already gate theirs.
        bool          m_compareSet            = false;
        std::string   m_compareReference;
        std::string   m_compareResolvedLevel;
        std::string   m_compareReferencePath;
        bool          m_comparePassed         = false;
        std::uint64_t m_compareDiffCount      = 0;
        double        m_compareDiffRatio      = 0.0;
        std::uint64_t m_compareMaxDiffPixels  = 0;
        bool          m_compareSizesMismatch  = false;
        std::string   m_compareDiffPath;
        std::string   m_compareErrorMessage;
        double        m_compareMaxLocalDifference = 0.0;
        std::vector<std::string> m_compareTriedPaths;

        // The --settle verdict (Task 3) -- see SetSettle's own comment. As
        // everywhere else in this component, m_settleSet is what gates
        // emission, so "never asked" and "asked, 0 attempts" stay distinct
        // rather than collapsing into the same zero.
        bool          m_settleSet           = false;
        std::uint64_t m_settleAttemptsUsed  = 0;
        bool          m_settleConverged     = false;
        SettleBail    m_settleBail          = SettleBail::Keep;
        bool          m_settleCaptureFailed = false;

        // The world set (schemaVersion 6) -- m_worldsSet gates emission the same
        // way every other optional section's flag does.
        bool                   m_worldsSet = false;
        std::vector<WorldFact> m_worlds;

        // The view mode (schemaVersion 7) -- m_viewModeSet gates emission.
        bool        m_viewModeSet = false;
        std::string m_viewMode;
        // The inspector block (schemaVersion 11) -- m_inspectorSet gates emission.
        bool        m_inspectorSet = false;
        std::string m_inspectorSource, m_inspectorBreadcrumb;
        std::vector<InspectorInstance> m_inspectorInstances;   // schemaVersion 12
        // The open preview documents (schemaVersion 13) -- m_documentsSet gates emission.
        bool                         m_documentsSet = false;
        std::vector<DocumentPreview> m_documents;

        // The visibility counts (schemaVersion 8; transparentRows + the
        // nullable gpuVisible are 9) -- m_visibilitySet gates emission, and
        // m_visGpu's own emptiness is what makes gpuVisible `null`.
        bool          m_visibilitySet = false;
        std::uint32_t m_visTotal = 0, m_visCoarse = 0, m_visBatches = 0, m_visDraws = 0;
        std::uint32_t m_visTransparentRows = 0;
        std::optional<std::uint32_t> m_visGpu;

        // The injected modules (schemaVersion 10) -- m_foreignModulesSet gates
        // emission, so "never scanned" and "scanned, clean" stay distinct.
        bool                               m_foreignModulesSet = false;
        std::vector<ForeignModules::Match> m_foreignModules;

        // The --set echo (schemaVersion 14) -- m_cvarSetsSet gates emission.
        bool                     m_cvarSetsSet = false;
        std::vector<CVarSetEcho> m_cvarSets;

        // Already-evaluated probe entries, in Evaluate() call order.
        nlohmann::json m_probes = nlohmann::json::array();
    };
}
