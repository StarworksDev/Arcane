#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Commands/LogCommands.hpp>
#include <Arcane/Remote/Commands/EcsCommands.hpp>   // SetEcsArtifactDir: the session artifact dir log.read spills to
#include <Servitor/Client.hpp>
#include <Servitor/Discover.hpp>   // HostDiagnosticsDir (Task 15)
#include <Servitor/Verbs.hpp>      // FollowEventText (Task 22 fix round 1)
#include "../Helpers/TestEnvironment.hpp" // Arcane::Test::TempDir
#include <Arcane/Remote/Dispatcher.hpp>     // HostGate
#include <Arcane/Remote/Follow.hpp>
#include <Arcane/Remote/Http.hpp>           // MakeToken
#include <Arcane/Remote/Jobs.hpp>
#include <Arcane/Remote/Listener.hpp>
#include <Arcane/Base/Log.hpp>
#include <Arcane/Net/TcpSocket.hpp>
#include "RemoteTestHttp.hpp"               // ConnectLoopback, SendAll, RecvUntil; includes RemoteTestSupport.hpp
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#if !defined(ARC_BUILD_DIST)

TEST_CASE("log.read returns the ring and follow on the call is refused", "[remote]") {
    // Read from just before the marker: a full-suite ring holds thousands of
    // older lines and log.read returns the oldest window after `since`.
    const std::uint64_t since = Arcane::Log::LatestSeq();
    Arcane::Log::Engine()->info("remote-log-read-marker");
    Arcane::Remote::RegisterLogCommand();
    Arcane::Remote::Call call;
    call.command = "log.read";
    call.args = nlohmann::json{{"since", since}};
    const auto once = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(once.envelope.ok);
    CHECK(once.completesJob == 0);
    bool saw = false;
    for (const auto& row : once.envelope.data["records"]) {
        if (row["text"].get<std::string>().find("remote-log-read-marker") != std::string::npos)
            saw = true;
    }
    CHECK(saw);
    Arcane::Remote::JobTable jobs;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> logJobs(&Arcane::Remote::SetLogJobs, &jobs);
    call.args["follow"] = true;
    const auto follow = Arcane::Remote::CommandRegistry::Get().Invoke(call);
    CHECK(follow.completesJob == 0);
    CHECK_FALSE(follow.envelope.ok);
    CHECK(follow.envelope.errors[0].code == "arg.inapplicable");
    CHECK(Arcane::Remote::ExitFromHost(follow.envelope) == 2);
    CHECK(jobs.FinishedCount() == 0);
}

TEST_CASE("the SSE subscriber owns the follow job", "[remote][listener]") {
    Arcane::Remote::RequestQueue queue;
    Arcane::Remote::ListenerAtoms atoms;
    atoms.status.store(std::make_shared<const Arcane::Remote::HostStatus>());
    Arcane::Remote::RemoteListener listener(queue, atoms, Arcane::Remote::MakeToken());
    listener.Start();
    // Lines already in the ring must not be replayed into the stream (ruling F4).
    for (int i = 0; i < 64; ++i)
        Arcane::Log::Engine()->info("remote-follow-old-line {}", i);
    const SocketType client = RemoteTest::ConnectLoopback(listener.Port());
    RemoteTest::SendAll(client, "GET /remote/v1/stream?topics=logs HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(listener.Port()) + "\r\nAuthorization: Bearer " + listener.Token() + "\r\n\r\n");
    const std::string headers = RemoteTest::RecvUntil(client, "\r\n\r\n", std::chrono::seconds(1));
    CHECK(headers.find("200") != std::string::npos);
    CHECK(headers.find("text/event-stream") != std::string::npos);
    Arcane::Remote::JobTable jobs;
    RemoteTest::ScopedRemotePointer<Arcane::Remote::JobTable> logJobs(&Arcane::Remote::SetLogJobs, &jobs);
    const std::uint64_t seqBeforeOpen = Arcane::Log::LatestSeq();
    Arcane::Remote::JobId followId{};
    const Arcane::Remote::Job* opened = nullptr;
    const auto openDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (opened == nullptr && std::chrono::steady_clock::now() < openDeadline) {
        Arcane::Remote::ApplyFollowEvents(jobs, listener);   // ruling F5: the listener, not its atoms
        for (std::uint64_t id = 1; id < 8 && opened == nullptr; ++id) {
            const Arcane::Remote::Job* candidate = jobs.Find(Arcane::Remote::JobId{id});
            if (candidate && candidate->subscriberId != 0) {
                opened = candidate;
                followId = candidate->id;
            }
        }
        if (opened == nullptr)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(opened != nullptr);
    CHECK(opened->state == Arcane::Remote::JobState::Running);
    CHECK(opened->command == "log.follow");
    CHECK(opened->progress.done >= seqBeforeOpen);       // starts at the newest seq, not 0
    Arcane::Log::Engine()->info("remote-follow-while-open");
    Arcane::Remote::HostGate gate;
    jobs.StepAll(gate);
    // Loop recv until the marker arrives or 1 s passes (ruling F4).
    const std::string pushed = RemoteTest::RecvUntil(client, "remote-follow-while-open", std::chrono::seconds(1));
    CHECK(pushed.find("remote-follow-while-open") != std::string::npos);
    CHECK(pushed.find("remote-follow-old-line") == std::string::npos);
    CloseSocket(client);
    Arcane::Remote::JobState state = Arcane::Remote::JobState::Running;
    for (int i = 0; i < 50; ++i) {
        Arcane::Remote::ApplyFollowEvents(jobs, listener);
        const Arcane::Remote::Job* now = jobs.Find(followId);
        REQUIRE(now != nullptr);
        state = now->state;
        if (state == Arcane::Remote::JobState::Cancelled)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(state == Arcane::Remote::JobState::Cancelled);
    Arcane::Log::Engine()->info("remote-follow-after-close");
    jobs.StepAll(gate);
    CHECK(jobs.Find(followId)->state == Arcane::Remote::JobState::Cancelled);
    listener.Stop();
}

TEST_CASE("a dead host falls back to the file sink", "[servitor][client]") {
    const auto path = std::filesystem::temp_directory_path() / "remote-logs-fallback.log";
    std::filesystem::remove(path);
    {
        std::ofstream out(path);
        out << "remote-file-line\n";
    }
    Arcane::Remote::Envelope env;
    Servitor::ReadLogFile(path, env);
    CHECK(env.ok);
    CHECK(env.notifications[0].code == "logs.from-file");
    CHECK(env.notifications[0].message == path.string());
    CHECK(env.data["text"].get<std::string>().find("remote-file-line") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("HostLogFile picks the project log and falls back to the exe-dir log", "[servitor][client]") {
    // Ruling F28: both branches of the AttachLogSink derivation.
    const auto project = std::filesystem::temp_directory_path() / "remote-logfile-project";
    const auto withProject = Servitor::HostLogFile(Arcane::Remote::HostKind::Runtime, project);
    CHECK(withProject.lexically_normal().generic_string() ==
          (project / "Saved" / "Logs" / "ArcaneRuntime.log").lexically_normal().generic_string());
    CHECK(Servitor::HostLogFile(Arcane::Remote::HostKind::Editor, project).filename() == "ArcaneEditor.log");
    const auto exeDir = Servitor::HostDiagnosticsDir(Arcane::Remote::HostKind::Server).parent_path();
    const auto noProject = Servitor::HostLogFile(Arcane::Remote::HostKind::Server);
    CHECK(noProject.lexically_normal().generic_string() ==
          (exeDir / "Logs" / "ArcaneServer.log").lexically_normal().generic_string());
    // rev 8.3 V7: --engine naming a package root whose sdk.json lacks engineConfig is unresolved.
    const auto broken = std::filesystem::temp_directory_path() / "remote-logfile-broken";
    std::filesystem::create_directories(broken / "sdk");
    { std::ofstream(broken / "sdk" / "sdk.json") << "{}"; }
    Servitor::GlobalOptions opt;
    opt.engine = broken.string();
    CHECK(Servitor::HostLogFile(Arcane::Remote::HostKind::Server, {}, opt).empty());
    std::filesystem::remove_all(broken);
}

// --- Task 22 fix round 1 ---

TEST_CASE("ReadLogFile keeps a whole-line tail of a long log file", "[servitor][client]") {
    Arcane::Test::TempDir dir("remote-logs-tail");
    const auto small = dir.path / "small.log";
    {
        std::ofstream out(small, std::ios::binary);
        for (int i = 0; i < 20; ++i)
            out << "line-" << (100 + i) << "\n";   // 9 bytes a line, 180 bytes in all
    }
    Arcane::Remote::Envelope cut;
    Servitor::ReadLogFile(small, cut, 50);   // 50 bytes from the end lands mid-line
    REQUIRE(cut.ok);
    const std::string text = cut.data["text"].get<std::string>();
    CHECK(text.size() <= 50);
    CHECK(text.starts_with("line-"));         // the partial first line was dropped
    CHECK(text.ends_with("line-119\n"));      // the newest line is kept
    CHECK(text.find("line-113") == std::string::npos);
    REQUIRE(cut.notifications.size() == 2);
    CHECK(cut.notifications[0].code == "logs.from-file");
    CHECK(cut.notifications[1].code == "logs.truncated");
    CHECK(cut.notifications[1].detail["fileBytes"] == 180);
    CHECK(cut.notifications[1].detail["shownBytes"] == text.size());

    Arcane::Remote::Envelope onBoundary;
    Servitor::ReadLogFile(small, onBoundary, 45);   // 45 bytes from the end is exactly a line start
    CHECK(onBoundary.data["text"].get<std::string>() == "line-115\nline-116\nline-117\nline-118\nline-119\n");

    Arcane::Remote::Envelope whole;
    Servitor::ReadLogFile(small, whole, 180);
    CHECK(whole.data["text"].get<std::string>().size() == 180);
    REQUIRE(whole.notifications.size() == 1);   // no logs.truncated

    // The default cap: a file past Servitor::kLogFileTailBytes is cut to it.
    const auto big = dir.path / "big.log";
    {
        std::ofstream out(big, std::ios::binary);
        const std::string line(99, 'x');
        for (std::size_t written = 0; written <= Servitor::kLogFileTailBytes + 4096; written += 100)
            out << line << "\n";
    }
    Arcane::Remote::Envelope capped;
    Servitor::ReadLogFile(big, capped);
    REQUIRE(capped.ok);
    CHECK(capped.data["text"].get<std::string>().size() <= Servitor::kLogFileTailBytes);
    CHECK(capped.data["text"].get<std::string>().size() % 100 == 0);   // whole lines only
    REQUIRE(capped.notifications.size() == 2);
    CHECK(capped.notifications[1].code == "logs.truncated");
}

TEST_CASE("logs --follow applies --since and --level to each stream event", "[servitor][client]") {
    const nlohmann::json args = {{"since", 10}, {"level", "warn"}};
    const auto line = [&](const nlohmann::json& event, Servitor::Format format = Servitor::Format::Ndjson) {
        return Servitor::FollowEventText("logs", event.dump(), args, format);
    };
    const auto record = [](std::uint64_t seq, const char* level, const char* text) {
        return nlohmann::json{{"seq", seq}, {"time", 1}, {"level", level}, {"logger", "Arcane"}, {"text", text}};
    };
    CHECK(line(record(5, "warning", "old")).empty());        // seq <= since
    CHECK(line(record(10, "error", "at-since")).empty());    // since is exclusive
    CHECK(line(record(11, "info", "too-quiet")).empty());    // below --level warn
    const std::string warn = line(record(12, "warning", "kept-warning"));
    REQUIRE_FALSE(warn.empty());
    const nlohmann::json frame = nlohmann::json::parse(warn);
    CHECK(frame["type"] == "event");
    CHECK(frame["topic"] == "logs");
    CHECK(frame["data"]["text"] == "kept-warning");
    CHECK_FALSE(line(record(13, "critical", "kept-critical")).empty());
    CHECK_FALSE(line(nlohmann::json{{"dropped", 3}}).empty());   // the eviction notice always passes
    CHECK(line(record(14, "error", "human-line"), Servitor::Format::Human) == "14 [error] Arcane: human-line\n");
    // No filters: every record passes.
    CHECK_FALSE(Servitor::FollowEventText("logs", record(1, "trace", "t").dump(), nlohmann::json::object(),
                                          Servitor::Format::Json).empty());
}

TEST_CASE("problems --follow applies --severity and --search to each stream event", "[servitor][client]") {
    const nlohmann::json args = {{"severity", "error"}, {"search", "NEEDLE"}};
    const auto row = [](const char* severity, const char* code, const char* message, const char* detail) {
        return nlohmann::json{{"severity", severity}, {"scope", "project"}, {"code", code}, {"message", message},
                              {"detail", detail}, {"locator", nullptr}}.dump();
    };
    const auto shown = [&](const std::string& data) {
        return !Servitor::FollowEventText("problems", data, args, Servitor::Format::Ndjson).empty();
    };
    CHECK(shown(row("error", "a.b", "has a needle inside", "")));       // case-insensitive, message
    CHECK(shown(row("error", "x.needle.y", "nothing", "")));            // code
    CHECK(shown(row("error", "a.b", "nothing", "detail Needle")));      // detail
    CHECK_FALSE(shown(row("warning", "a.b", "has a needle", "")));      // severity is exact
    CHECK_FALSE(shown(row("error", "a.b", "no match here", "")));       // search misses
    CHECK(Servitor::FollowEventText("problems", row("error", "a.b", "needle", ""), args, Servitor::Format::Human) ==
          "error a.b: needle\n");
}
// Mutes the engine logger's stderr sink (its first) for a scope, so a case
// that fills the ring with long lines does not flood the suite's output.
struct QuietStderrSink {
    spdlog::level::level_enum before;
    QuietStderrSink() : before(Arcane::Log::Engine()->sinks().front()->level()) {
        Arcane::Log::Engine()->sinks().front()->set_level(spdlog::level::off);
    }
    ~QuietStderrSink() { Arcane::Log::Engine()->sinks().front()->set_level(before); }
};

// Final fix pass (C-M10): log.read applies the inline cap every other dump
// applies (spec s4.8: never megabytes in an envelope). Over it, the records go
// to one artifact under the session's out/ directory and data.records is empty
// with data.truncated true; under it nothing changes.
TEST_CASE("log.read over the inline cap writes the records to one log artifact", "[remote]") {
    Arcane::Test::TempDir tmp("remote-log-cap");
    const std::filesystem::path out = tmp.path / "out";
    RemoteTest::ScopedRemotePointer<const std::filesystem::path> dirGuard(
        [out](const std::filesystem::path* p) {
            if (p != nullptr)
                Arcane::Remote::SetEcsArtifactDir(*p);
            else
                Arcane::Remote::ReleaseEcsArtifactDir(out);
        },
        &out);
    Arcane::Remote::RegisterLogCommand();
    const std::uint64_t since = Arcane::Log::LatestSeq();
    {
        QuietStderrSink quiet;
        const std::string filler(1000, 'x');
        for (int i = 0; i < 400; ++i)
            Arcane::Log::Engine()->info("remote-log-cap {} {}", i, filler);
    }
    Arcane::Remote::Call call;
    call.command = "log.read";
    call.args = nlohmann::json{{"since", since}};
    auto big = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(big.ok);
    CHECK(big.data["records"].empty());
    CHECK(big.data["truncated"] == true);
    REQUIRE(big.artifacts.size() == 1);
    CHECK(big.artifacts[0].kind == "logs");
    const auto file = Servitor::PathFromUtf8(big.artifacts[0].path);
    REQUIRE(std::filesystem::exists(file));
    std::ifstream in(file, std::ios::binary);
    const auto dumped = nlohmann::json::parse(in);
    REQUIRE(dumped["records"].size() >= 400);
    CHECK(dumped["records"][0]["text"].get<std::string>().find("remote-log-cap 0 ") != std::string::npos);
    CHECK(Arcane::Remote::ToJson(big).dump().size() < 65536);

    const std::uint64_t small = Arcane::Log::LatestSeq();
    Arcane::Log::Engine()->info("remote-log-cap-small");
    call.args = nlohmann::json{{"since", small}};
    auto inlineRead = Arcane::Remote::CommandRegistry::Get().Invoke(call).envelope;
    REQUIRE(inlineRead.ok);
    CHECK(inlineRead.artifacts.empty());
    CHECK(inlineRead.data["truncated"] == false);
    CHECK_FALSE(inlineRead.data["records"].empty());
}

#endif
