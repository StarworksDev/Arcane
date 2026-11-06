#include <catch2/catch_test_macros.hpp>
#include <Arcane/Crypto/Crypto.hpp>  // first after Catch2: its Logger.hpp defines SPDLOG_ACTIVE_LEVEL
#include <Arcane/Remote/Commands/CompareCommands.hpp>
#include <Arcane/Remote/Commands/CaptureCommands.hpp>
#include <Arcane/Host/ReferenceImages.hpp>   // ReferenceNameIsSafe: the copy the wire check is pinned to (G-3)
#include <Arcane/Remote/Dispatcher.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Assets/ImageCompare.hpp>
#include <Arcane/Assets/ImageIo.hpp>
#include "RemoteTestSupport.hpp"
#include <Servitor/Verbs.hpp>
#include <Json.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(ARC_BUILD_DIST)

// Installs one case's capture hooks, compare hooks and JobTable* and clears all
// of them on scope exit (rulings F11, S3-9).
struct RemoteCompareScope {
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::CaptureHooks> capture;
    RemoteTest::ScopedRemotePointer<const Arcane::Remote::CompareHooks> compare;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> captureJobs;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> compareJobs;
    RemoteCompareScope(const Arcane::Remote::CaptureHooks& cap, const Arcane::Remote::CompareHooks& cmp,
                    Arcane::Remote::JobTable& jobs)
        : capture([](const Arcane::Remote::CaptureHooks* p) {
                      Arcane::Remote::SetCaptureHooks(p ? *p : Arcane::Remote::CaptureHooks{});
                  }, &cap),
          compare([](const Arcane::Remote::CompareHooks* p) {
                      Arcane::Remote::SetCompareHooks(p ? *p : Arcane::Remote::CompareHooks{});
                  }, &cmp),
          captureJobs(&Arcane::Remote::RegisterCaptureCommands, &jobs),
          compareJobs(&Arcane::Remote::RegisterCompareCommand, &jobs) {}
};

static std::filesystem::path g_ref;
static int g_blesses = 0;
static int g_copies = 0;
static int g_reads = 0;
static std::uint64_t g_ticket = 0;
static bool g_retired = false;
// The probe images are solid kProbeSide x kProbeSide squares. CompareImages is
// Playwright's ssim-cie94 cascade: it pads both images with a 15 px
// magenta/green border, and a pixel whose 3x3 neighbourhood varies is judged
// by a 31x31 SSIM window. A 1x1 probe's one pixel sits inside that identical
// padding, so even black against white scores as antialiasing and passes.
// A solid square has flat interiors, so a solid colour change is a real
// difference at every pixel.
static constexpr std::uint32_t kProbeSide = 32;
static std::vector<unsigned char> SolidProbe(unsigned char value) {
    std::vector<unsigned char> rgba(static_cast<std::size_t>(kProbeSide) * kProbeSide * 4, value);
    for (std::size_t i = 3; i < rgba.size(); i += 4)
        rgba[i] = 255;
    return rgba;
}
static const std::vector<unsigned char> kWhite = SolidProbe(255);
static bool WriteBlackProbe(const std::filesystem::path& file) {
    const std::vector<unsigned char> black = SolidProbe(0);
    return Arcane::WritePngRgba(file, kProbeSide, kProbeSide, black.data());
}

static bool ArmWhite(std::uint64_t ticket, void*) {
    ++g_copies;
    g_ticket = ticket;
    g_retired = false;
    return true;
}
static bool DrainWhite(std::uint64_t ticket, Arcane::Remote::CaptureMapped* raw, void*) {
    if (!g_retired || ticket != g_ticket)
        return false;
    ++g_reads;
    raw->width = kProbeSide;
    raw->height = kProbeSide;
    raw->rowPitch = kProbeSide * 4;
    raw->bgra = true;
    raw->mapped = kWhite.data();
    raw->mappedBytes = static_cast<std::uint32_t>(kWhite.size());
    return true;
}
static Arcane::Remote::ResolvedReference ResolveProbe(const std::filesystem::path&, const std::string& slot, void*) {
    Arcane::Remote::ResolvedReference out;
    out.triedPaths.push_back(g_ref.generic_string());
    if (slot != "servitor-bless-probe" || !std::filesystem::exists(g_ref))
        return out;
    out.found = true;
    out.path = g_ref;
    out.level = "temp";
    return out;
}
static bool BlessProbe(const std::filesystem::path&, const std::string&,
                       std::uint32_t w, std::uint32_t h, const unsigned char* rgba, void*) {
    ++g_blesses;
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe.png";
    return Arcane::WritePngRgba(g_ref, w, h, rgba);
}

TEST_CASE("compare Failed is ok true exit 8, and bless then matches", "[remote][compare]") {
    g_blesses = 0;
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe-black.png";
    REQUIRE(WriteBlackProbe(g_ref));
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks cap;
    cap.framesInFlight = 2;
    cap.armHost = &ArmWhite;
    cap.drainHost = &DrainWhite;
    cap.outDir = std::filesystem::temp_directory_path() / "remote-compare-out";
    std::filesystem::create_directories(cap.outDir);
    Arcane::Remote::CompareHooks hooks;
    hooks.resolve = &ResolveProbe;
    hooks.bless = &BlessProbe;
    hooks.projectRoot = std::filesystem::temp_directory_path();
    RemoteCompareScope scope(cap, hooks, jobs);
    auto run = [&](bool bless, double ratio) {
        g_copies = 0;
        g_reads = 0;
        g_retired = false;
        Arcane::Remote::Call call;
        call.command = "capture.compare";
        call.args = nlohmann::json{{"reference", "servitor-bless-probe"}, {"source", "host"}, {"bless", bless}};
        if (ratio >= 0.0)
            call.args["maxDiffPixelRatio"] = ratio;
        auto result = Arcane::Remote::CommandRegistry::Get().Invoke(call);
        REQUIRE(result.completesJob != 0);
        const auto id = Arcane::Remote::JobId{result.completesJob};
        Arcane::Remote::HostGate gate;
        gate.frame = 10;
        jobs.StepAll(gate);
        CHECK(g_copies == 1);
        CHECK(g_reads == 0);
        CHECK(gate.frame == 10);
        REQUIRE(jobs.Find(id) != nullptr);
        CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
        CHECK(jobs.Find(id)->startedFrame == 0);
        CHECK_FALSE(jobs.Find(id)->envelope.data.contains("verdict"));
        jobs.StepAll(gate);
        CHECK(g_reads == 0);
        CHECK(gate.frame == 10);
        CHECK(jobs.Find(id)->state == Arcane::Remote::JobState::Running);
        g_retired = true;
        jobs.StepAll(gate);
        CHECK(g_reads == 1);
        CHECK(gate.frame == 10);
        return jobs.Find(id)->envelope;
    };
    const auto failed = run(false, -1.0);
    CHECK(failed.ok);
    CHECK(failed.errors.empty());
    CHECK(failed.data["verdict"] == "Failed");
    CHECK(failed.data["resolvedLevel"] == "temp");
    REQUIRE(failed.data["triedPaths"].size() == 1);
    CHECK(Arcane::Remote::ExitFromHost(failed) == 8);
    const auto ratioPass = run(false, 1.0);
    CHECK(ratioPass.ok);
    CHECK(ratioPass.data["verdict"] == "Passed");
    CHECK(ratioPass.data["thresholds"]["maxDiffPixelRatio"] == 1.0);
    CHECK(Arcane::Remote::ExitFromHost(ratioPass) == 0);
    const auto blessed = run(true, -1.0);
    CHECK(g_blesses == 1);
    CHECK(blessed.ok);
    const auto again = run(false, -1.0);
    CHECK(again.ok);
    CHECK(again.data["verdict"] == "Passed");
    CHECK(Arcane::Remote::ExitFromHost(again) == 0);
    g_ref.clear();
    const auto created = run(true, -1.0);
    CHECK(created.ok);
    CHECK(created.errors.empty());
    CHECK(created.data["verdict"] == "Passed");
    CHECK(Arcane::Remote::ExitFromHost(created) == 0);
    CHECK(g_blesses == 2);
    CHECK(std::filesystem::exists(g_ref));
    g_copies = 0;
    g_reads = 0;
    g_ref.clear();
    Arcane::Remote::Call missingCall;
    missingCall.command = "capture.compare";
    missingCall.args = nlohmann::json{{"reference", "servitor-bless-probe"}, {"source", "host"}, {"bless", false}};
    const auto missing = Arcane::Remote::CommandRegistry::Get().Invoke(missingCall);
    CHECK(missing.completesJob == 0);
    CHECK(g_copies == 0);
    CHECK(g_reads == 0);
    CHECK_FALSE(missing.envelope.ok);
    CHECK(missing.envelope.errors[0].code == "compare.missing-reference");
    CHECK(Arcane::Remote::ExitFromHost(missing.envelope) == 6);
}

TEST_CASE("compare while a screenshot is in flight is host.busy capture-pending", "[remote][compare]") {
    g_copies = 0;
    g_reads = 0;
    g_retired = false;
    g_ticket = 0;
    g_blesses = 0;
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks cap;
    cap.framesInFlight = 2;
    cap.armWindow = &ArmWhite;
    cap.drainWindow = &DrainWhite;
    cap.armHost = &ArmWhite;
    cap.drainHost = &DrainWhite;
    cap.outDir = std::filesystem::temp_directory_path() / "remote-compare-busy";
    std::filesystem::create_directories(cap.outDir);
    Arcane::Remote::CompareHooks hooks;
    hooks.resolve = &ResolveProbe;
    hooks.bless = &BlessProbe;
    hooks.projectRoot = std::filesystem::temp_directory_path();
    RemoteCompareScope scope(cap, hooks, jobs);
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe-busy.png";
    REQUIRE(WriteBlackProbe(g_ref));

    Arcane::Remote::Call shot;
    shot.command = "capture.window";
    shot.args = nlohmann::json::object();
    const auto armed = Arcane::Remote::CommandRegistry::Get().Invoke(shot);
    REQUIRE(armed.completesJob != 0);
    Arcane::Remote::HostGate gate;
    gate.frame = 10;
    jobs.StepAll(gate);
    CHECK(g_copies == 1);
    CHECK(g_reads == 0);
    const auto firstTicket = g_ticket;
    CHECK(firstTicket != 0);
    CHECK(gate.frame == 10);

    Arcane::Remote::Call cmp;
    cmp.command = "capture.compare";
    cmp.args = nlohmann::json{{"reference", "servitor-bless-probe"}, {"source", "host"}, {"bless", false}};
    const auto busy = Arcane::Remote::CommandRegistry::Get().Invoke(cmp);
    CHECK(busy.completesJob == 0);
    CHECK_FALSE(busy.envelope.ok);
    CHECK(busy.envelope.errors[0].code == "host.busy");
    CHECK(busy.envelope.errors[0].detail["state"] == "capture-pending");
    CHECK(Arcane::Remote::ExitFromHost(busy.envelope) == 5);
    CHECK(g_copies == 1);
    CHECK(g_ticket == firstTicket);
    CHECK(g_reads == 0);
    CHECK(gate.frame == 10);
}

TEST_CASE("a read-only bless is refused and a read-only compare still runs", "[remote][compare]") {
    // Ruling F19: through the real dispatcher gate, which reads Desc::accessFor.
    g_copies = 0;
    g_reads = 0;
    g_retired = false;
    g_ticket = 0;
    g_blesses = 0;
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe-ro.png";
    REQUIRE(WriteBlackProbe(g_ref));
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::CaptureHooks cap;
    cap.framesInFlight = 2;
    cap.armHost = &ArmWhite;
    cap.drainHost = &DrainWhite;
    cap.outDir = std::filesystem::temp_directory_path() / "remote-compare-ro";
    std::filesystem::remove_all(cap.outDir);
    std::filesystem::create_directories(cap.outDir);
    Arcane::Remote::CompareHooks hooks;
    hooks.resolve = &ResolveProbe;
    hooks.bless = &BlessProbe;
    hooks.projectRoot = std::filesystem::temp_directory_path();
    RemoteCompareScope scope(cap, hooks, jobs);
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::HostGate gate;
    gate.readOnly = true;                                  // ARC_REMOTE_READ_ONLY on this host
    Arcane::Remote::RemoteDispatcher dispatcher(queue, Arcane::Remote::CommandRegistry::Get(), gate, jobs,
                                          cap.outDir / "audit.ndjson");
    Arcane::Remote::Call bless;
    bless.command = "capture.compare";
    bless.args = nlohmann::json{{"reference", "servitor-bless-probe"}, {"source", "host"}, {"bless", true}};
    REQUIRE(queue.TryPush(1, bless));
    dispatcher.Drain(1000.0, 8);
    Arcane::Remote::CommandResult refused;
    REQUIRE(queue.TryTake(1, refused));
    CHECK(refused.envelope.errors[0].code == "permission.read-only");
    CHECK(Arcane::Remote::ExitFromHost(refused.envelope) == 3);
    CHECK(g_copies == 0);
    CHECK(g_blesses == 0);
    CHECK_FALSE(jobs.AnyRunning({"capture.compare"}));
    Arcane::Remote::Call plain = bless;
    plain.args["bless"] = false;
    REQUIRE(queue.TryPush(2, plain));
    dispatcher.Drain(1000.0, 8);                           // runs the command: arms the job
    CHECK(jobs.AnyRunning({"capture.compare"}));
    dispatcher.Drain(1000.0, 8);                           // copy step
    CHECK(g_copies == 1);
    g_retired = true;
    dispatcher.Drain(1000.0, 8);                           // completing drain Completes waiter 2
    Arcane::Remote::CommandResult ran;
    REQUIRE(queue.TryTake(2, ran));
    CHECK(ran.envelope.ok);
    CHECK(ran.envelope.data["verdict"] == "Failed");       // white vs black, zero budget
    CHECK(g_blesses == 0);
}

// ---- Task 28 cases beyond the brief's three: refusals, failures, artifacts ----

namespace {

int g_resolves = 0;

Arcane::Remote::ResolvedReference CountingResolve(const std::filesystem::path& root, const std::string& slot,
                                                  void* user) {
    ++g_resolves;
    return ResolveProbe(root, slot, user);
}
bool RefuseBless(const std::filesystem::path&, const std::string&, std::uint32_t, std::uint32_t,
                 const unsigned char*, void*) {
    ++g_blesses;
    return false;
}

void ResetProbe() {
    g_copies = 0;
    g_reads = 0;
    g_retired = false;
    g_ticket = 0;
    g_blesses = 0;
    g_resolves = 0;
}

Arcane::Remote::CaptureHooks HostPair(const std::filesystem::path& outDir) {
    Arcane::Remote::CaptureHooks cap;
    cap.framesInFlight = 2;
    cap.armHost = &ArmWhite;
    cap.drainHost = &DrainWhite;
    cap.outDir = outDir;
    return cap;
}

Arcane::Remote::CompareHooks ProbeHooks() {
    Arcane::Remote::CompareHooks hooks;
    hooks.resolve = &CountingResolve;
    hooks.bless = &BlessProbe;
    hooks.projectRoot = std::filesystem::temp_directory_path();
    return hooks;
}

Arcane::Remote::CommandResult InvokeCompare(const nlohmann::json& args, std::uint32_t timeoutMs = 30000) {
    Arcane::Remote::Call call;
    call.command = "capture.compare";
    call.args = args;
    call.timeoutMs = timeoutMs;
    return Arcane::Remote::CommandRegistry::Get().Invoke(call);
}

// Arms, steps the copy, retires the ticket and steps the completing drain.
const Arcane::Remote::Job* RunToEnd(Arcane::Remote::JobTable& jobs, const Arcane::Remote::CommandResult& armed) {
    Arcane::Remote::HostGate gate;
    jobs.StepAll(gate);
    g_retired = true;
    jobs.StepAll(gate);
    return jobs.Find(Arcane::Remote::JobId{armed.completesJob});
}

void WriteBlack(const std::filesystem::path& file) {
    REQUIRE(WriteBlackProbe(file));
}

} // namespace

TEST_CASE("compare refusals: torn down, no renderer, no reference hooks, bad args; nothing armed", "[remote][compare]") {
    ResetProbe();
    const auto outDir = std::filesystem::temp_directory_path() / "remote-compare-refuse";
    const nlohmann::json good = {{"reference", "servitor-bless-probe"}};

    {   // a torn-down session (no table) is host.unavailable before capture.no-renderer
        Arcane::Remote::RegisterCompareCommand(nullptr);
        Arcane::Remote::SetCompareHooks({});
        Arcane::Remote::SetCaptureHooks({});
        const auto torn = InvokeCompare(good);
        CHECK(torn.completesJob == 0);
        CHECK(torn.envelope.errors[0].code == "host.unavailable");
        CHECK(Arcane::Remote::ExitFromHost(torn.envelope) == 5);
    }
    Arcane::Remote::JobTable jobs;
    {   // a server: no capture pair, no compare hooks -> capture.no-renderer (exit 2)
        const Arcane::Remote::CaptureHooks none{};
        const Arcane::Remote::CompareHooks noHooks{};
        RemoteCompareScope scope(none, noHooks, jobs);
        const auto server = InvokeCompare(good);
        CHECK(server.completesJob == 0);
        CHECK(server.envelope.errors[0].code == "capture.no-renderer");
        CHECK(Arcane::Remote::ExitFromHost(server.envelope) == 2);
    }
    {   // a renderer whose source pair is null: viewport asked, only the host pair installed
        const auto cap = HostPair(outDir);
        const auto hooks = ProbeHooks();
        RemoteCompareScope scope(cap, hooks, jobs);
        const auto viewport = InvokeCompare({{"reference", "servitor-bless-probe"}, {"source", "viewport"}});
        CHECK(viewport.envelope.errors[0].code == "capture.no-renderer");
    }
    {   // a renderer without a reference resolver (null resolve or bless) is host.unavailable
        const auto cap = HostPair(outDir);
        auto hooks = ProbeHooks();
        hooks.bless = nullptr;
        RemoteCompareScope scope(cap, hooks, jobs);
        const auto noBless = InvokeCompare(good);
        CHECK(noBless.completesJob == 0);
        CHECK(noBless.envelope.errors[0].code == "host.unavailable");
        CHECK(Arcane::Remote::ExitFromHost(noBless.envelope) == 5);
    }
    {
        const auto cap = HostPair(outDir);
        const auto hooks = ProbeHooks();
        RemoteCompareScope scope(cap, hooks, jobs);
        const std::vector<nlohmann::json> bad = {
            nlohmann::json::object(),                                            // reference missing
            {{"reference", ""}},
            {{"reference", 7}},
            {{"reference", "../escape"}},
            {{"reference", "sub/name"}},
            {{"reference", "sub\\name"}},
            {{"reference", ".hidden"}},
            {{"reference", "x"}, {"source", "panel"}},
            {{"reference", "x"}, {"bless", "yes"}},
            {{"reference", "x"}, {"maxDiffPixels", -5}},
            {{"reference", "x"}, {"maxDiffPixels", 1.5}},
            {{"reference", "x"}, {"maxDiffPixelRatio", 1.5}},
            {{"reference", "x"}, {"maxDiffPixelRatio", "0.1"}},
            {{"reference", "x"}, {"out", "/tmp/a.png"}},                         // a screenshot-only key
            {{"reference", "x"}, {"bogus", 1}},
        };
        for (const nlohmann::json& args : bad) {
            INFO(args.dump());
            const auto refused = InvokeCompare(args);
            CHECK(refused.completesJob == 0);
            REQUIRE_FALSE(refused.envelope.errors.empty());
            CHECK(refused.envelope.errors[0].code.starts_with("arg."));
            CHECK(Arcane::Remote::ExitFromHost(refused.envelope) == 2);
        }
        CHECK(g_copies == 0);
        CHECK(g_resolves == 0);
        CHECK(jobs.FinishedCount() == 0);
        CHECK_FALSE(jobs.AnyRunning({"capture.compare"}));
    }
}

TEST_CASE("compare access is Drive only for bless; the static access stays Observe", "[remote][compare]") {
    Arcane::Remote::JobTable jobs;
    Arcane::Remote::RegisterCompareCommand(&jobs);
    const Arcane::Remote::Desc* desc = Arcane::Remote::CommandRegistry::Get().Find("capture.compare");
    Arcane::Remote::RegisterCompareCommand(nullptr);
    REQUIRE(desc != nullptr);
    CHECK(desc->access == Arcane::Remote::Access::Observe);
    CHECK(desc->mutates == Arcane::Remote::Mutates::Project);
    CHECK(desc->thread == Arcane::Remote::ExecThread::Job);
    CHECK(desc->needsFrame);
    CHECK_FALSE(desc->worldScoped);
    REQUIRE(desc->accessFor != nullptr);
    Arcane::Remote::Call call;
    call.command = "capture.compare";
    call.args = {{"reference", "x"}};
    CHECK(desc->accessFor(call) == Arcane::Remote::Access::Observe);
    call.args["bless"] = false;
    CHECK(desc->accessFor(call) == Arcane::Remote::Access::Observe);
    call.args["bless"] = true;
    CHECK(desc->accessFor(call) == Arcane::Remote::Access::Drive);
    call.args["bless"] = "yes";   // malformed: gated as the stronger access, then refused by the command
    CHECK(desc->accessFor(call) == Arcane::Remote::Access::Drive);
    call.args = nlohmann::json::array();
    CHECK(desc->accessFor(call) == Arcane::Remote::Access::Observe);
}

TEST_CASE("a Failed compare writes the capture and the diff as artifacts", "[remote][compare]") {
    ResetProbe();
    const auto outDir = std::filesystem::temp_directory_path() / "remote-compare-artifacts";
    std::filesystem::remove_all(outDir);
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe-art.png";
    WriteBlack(g_ref);
    Arcane::Remote::JobTable jobs;
    const auto cap = HostPair(outDir);
    const auto hooks = ProbeHooks();
    RemoteCompareScope scope(cap, hooks, jobs);
    const auto armed = InvokeCompare({{"reference", "servitor-bless-probe"}, {"maxDiffPixels", 0}});
    REQUIRE(armed.completesJob != 0);
    CHECK(g_resolves == 1);   // resolved before Arm
    const auto* job = RunToEnd(jobs, armed);
    REQUIRE(job != nullptr);
    CHECK(job->state == Arcane::Remote::JobState::Done);
    const auto& env = job->envelope;
    CHECK(env.ok);
    CHECK(env.data["verdict"] == "Failed");
    CHECK(env.data["reference"] == "servitor-bless-probe");
    CHECK(env.data["diffPixels"] == kProbeSide * kProbeSide);   // every pixel, white against black
    CHECK(env.data["diffRatio"] == 1.0);
    CHECK(env.data["thresholds"]["maxDiffPixels"] == 0);
    CHECK(env.data["thresholds"]["maxDiffPixelRatio"].is_null());
    CHECK(env.data["blessed"] == false);
    CHECK(Arcane::Remote::ExitFromHost(env) == 8);
    REQUIRE(env.artifacts.size() == 2);
    CHECK(env.artifacts[0].kind == "capture");
    CHECK(env.artifacts[1].kind == "diff");
    for (const auto& artifact : env.artifacts) {
        const std::filesystem::path file(std::u8string(artifact.path.begin(), artifact.path.end()));
        CHECK(std::filesystem::exists(file));
        CHECK(std::filesystem::file_size(file) == artifact.bytes);
        CHECK(artifact.sha256.size() == 64);
    }
    std::uint32_t w = 0, h = 0;
    std::vector<unsigned char> rgba;
    const auto& capturePath = env.artifacts[0].path;
    REQUIRE(Arcane::LoadPngRgba(std::filesystem::path(std::u8string(capturePath.begin(), capturePath.end())), w, h, rgba));
    CHECK(w == kProbeSide);
    CHECK(h == kProbeSide);
    CHECK(rgba == kWhite);
}

TEST_CASE("compare failures: an unreadable reference, a refused bless, a lost copy", "[remote][compare]") {
    ResetProbe();
    const auto outDir = std::filesystem::temp_directory_path() / "remote-compare-fail";
    Arcane::Remote::JobTable jobs;
    const auto cap = HostPair(outDir);
    auto hooks = ProbeHooks();

    // A reference file that is not a PNG: the check cannot run (exit 6, not 8).
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe-corrupt.png";
    {
        std::ofstream junk(g_ref, std::ios::binary | std::ios::trunc);
        junk << "not a png";
    }
    {
        RemoteCompareScope scope(cap, hooks, jobs);
        const auto armed = InvokeCompare({{"reference", "servitor-bless-probe"}});
        REQUIRE(armed.completesJob != 0);
        const auto* job = RunToEnd(jobs, armed);
        CHECK(job->state == Arcane::Remote::JobState::Failed);
        CHECK(job->envelope.errors[0].code == "compare.reference-unreadable");
        CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);

        // --bless replaces an unreadable reference without comparing against it.
        ResetProbe();
        const auto blessed = InvokeCompare({{"reference", "servitor-bless-probe"}, {"bless", true}});
        REQUIRE(blessed.completesJob != 0);
        const auto* bjob = RunToEnd(jobs, blessed);
        CHECK(bjob->envelope.ok);
        CHECK(bjob->envelope.data["verdict"] == "Passed");
        CHECK(bjob->envelope.data["blessed"] == true);
        CHECK(bjob->envelope.data["diffPixels"].is_null());
        CHECK(g_blesses == 1);
    }

    // A bless the host refuses to write is compare.bless-failed (exit 6).
    hooks.bless = &RefuseBless;
    {
        ResetProbe();
        RemoteCompareScope scope(cap, hooks, jobs);
        const auto armed = InvokeCompare({{"reference", "servitor-bless-probe"}, {"bless", true}});
        REQUIRE(armed.completesJob != 0);
        const auto* job = RunToEnd(jobs, armed);
        CHECK(g_blesses == 1);
        CHECK(job->state == Arcane::Remote::JobState::Failed);
        CHECK(job->envelope.errors[0].code == "compare.bless-failed");
        CHECK(Arcane::Remote::ExitFromHost(job->envelope) == 6);
    }

    // The ticket step is Task 27's: a pending copy past its deadline is
    // capture.no-frame, and a minimized host is capture.minimized.
    hooks.bless = &BlessProbe;
    WriteBlack(g_ref);
    {
        ResetProbe();
        RemoteCompareScope scope(cap, hooks, jobs);
        Arcane::Remote::HostGate gate;
        const auto timedOut = InvokeCompare({{"reference", "servitor-bless-probe"}}, 1);
        REQUIRE(timedOut.completesJob != 0);
        jobs.StepAll(gate);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        jobs.StepAll(gate);
        const auto* job = jobs.Find(Arcane::Remote::JobId{timedOut.completesJob});
        CHECK(job->state == Arcane::Remote::JobState::Failed);
        CHECK(job->envelope.errors[0].code == "capture.no-frame");
        CHECK(g_reads == 0);

        const auto minimized = InvokeCompare({{"reference", "servitor-bless-probe"}});
        REQUIRE(minimized.completesJob != 0);
        jobs.StepAll(gate);
        gate.minimized = true;
        jobs.StepAll(gate);
        const auto* mjob = jobs.Find(Arcane::Remote::JobId{minimized.completesJob});
        CHECK(mjob->envelope.errors[0].code == "capture.minimized");
        CHECK(Arcane::Remote::ExitFromHost(mjob->envelope) == 5);
    }
}

TEST_CASE("releasing the compare table cancels an armed compare and completes its waiter", "[remote][compare]") {
    ResetProbe();
    const auto outDir = std::filesystem::temp_directory_path() / "remote-compare-cancel";
    g_ref = std::filesystem::temp_directory_path() / "servitor-bless-probe-cancel.png";
    WriteBlack(g_ref);
    Arcane::Remote::JobTable jobs;
    const auto cap = HostPair(outDir);
    const auto hooks = ProbeHooks();
    RemoteCompareScope scope(cap, hooks, jobs);
    Arcane::Remote::HostGate gate;
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::RemoteDispatcher dispatcher(queue, Arcane::Remote::CommandRegistry::Get(), gate, jobs,
                                                outDir / "audit.ndjson");
    Arcane::Remote::Call call;
    call.command = "capture.compare";
    call.args = {{"reference", "servitor-bless-probe"}};
    REQUIRE(queue.TryPush(51, call));
    dispatcher.Drain(1000.0, 8);   // arms the job, binds the waiter
    dispatcher.Drain(1000.0, 8);   // the copy step
    CHECK(g_copies == 1);
    REQUIRE(jobs.AnyRunning({"capture.compare"}));

    Arcane::Remote::RegisterCompareCommand(nullptr);   // what HostSession::Teardown does
    CHECK_FALSE(jobs.AnyRunning({"capture.compare"}));
    Arcane::Remote::CommandResult answered;
    REQUIRE(queue.Wait(51, answered, std::chrono::milliseconds(1000)));
    CHECK(answered.envelope.errors[0].code == "host.unavailable");
    g_retired = true;
    jobs.StepAll(gate);   // the cancelled row is not stepped again
    CHECK(g_reads == 0);
}

TEST_CASE("servitor compare builds capture.compare args and refuses the rest", "[servitor][client]") {
    const Servitor::VerbRow* row = Servitor::FindVerb("compare");
    REQUIRE(row != nullptr);
    CHECK(std::string(row->command) == "capture.compare");
    REQUIRE(row->args != nullptr);
    nlohmann::json out;
    std::string error;
    const std::vector<std::string> bare = {"editor-ui"};
    REQUIRE(row->args(bare, out, &error));
    CHECK(out == nlohmann::json{{"reference", "editor-ui"}});
    const std::vector<std::string> full = {"runtime-scene", "--source", "window", "--bless", "--max-diff-pixels",
                                           "12", "--max-diff-pixel-ratio", "0.25"};
    REQUIRE(row->args(full, out, &error));
    CHECK(out["reference"] == "runtime-scene");
    CHECK(out["source"] == "window");
    CHECK(out["bless"] == true);
    CHECK(out["maxDiffPixels"] == 12);
    CHECK(out["maxDiffPixelRatio"] == 0.25);
    for (const std::vector<std::string>& bad : std::vector<std::vector<std::string>>{
             {},
             {"a", "b"},
             {"a", "--source", "panel"},
             {"a", "--source"},
             {"a", "--max-diff-pixels", "-1"},
             {"a", "--max-diff-pixels", "1.5"},
             {"a", "--max-diff-pixels", "99999999999999999999999"},
             {"a", "--max-diff-pixel-ratio", "2"},
             {"a", "--max-diff-pixel-ratio", "nan"},
             {"a", "--max-diff-pixel-ratio", "0.1x"},
             {"a", "--bless", "--bless"},
             {"a", "--source", "host", "--source", "window"},
             {"a", "--out", "x.png"},   // a screenshot flag
             {"a", "--bogus"}}) {
        error.clear();
        std::string joined;
        for (const std::string& token : bad)
            joined += token + " ";
        INFO(joined);
        CHECK_FALSE(row->args(bad, out, &error));
        CHECK_FALSE(error.empty());
    }
    // Rule 3: compare's own flags are refused on another verb.
    const Servitor::VerbRow* shot = Servitor::FindVerb("screenshot window");
    REQUIRE(shot != nullptr);
    for (const char* flag : {"--bless", "--source"}) {
        const std::vector<std::string> foreign = {flag};
        error.clear();
        CHECK_FALSE(shot->args(foreign, out, &error));
        CHECK_FALSE(error.empty());
    }
}
// Final fix pass (G-3): ArcaneRemote keeps its own copy of ArcaneVerify's
// ReferenceNameIsSafe (it must not depend on ArcaneVerify, R8-2). This pins
// the two copies to the same answer for every name, ':' included, so the wire
// refusal (arg.invalid, exit 2) and the resolver can never disagree.
TEST_CASE("capture.compare's reference-name check and ReferenceImages' agree, and both refuse ':'", "[remote][compare]") {
    const std::vector<std::string> names = {
        "runtime-scene", "servitor-accept-scratch", "a", "a.b", "dx12",
        "", ".hidden", "..", "a..b", "a/b", "a\\b", "C:shot", "foo:bar", ":", "shot:", "/abs", "\\\\server\\share",
    };
    for (const std::string& name : names) {
        INFO(name);
        CHECK(Arcane::Remote::CompareReferenceNameIsSafe(name) == Arcane::ReferenceNameIsSafe(name));
    }
    CHECK_FALSE(Arcane::Remote::CompareReferenceNameIsSafe("C:shot"));
    CHECK_FALSE(Arcane::Remote::CompareReferenceNameIsSafe("foo:bar"));
    CHECK(Arcane::Remote::CompareReferenceNameIsSafe("runtime-scene"));
}

#endif
