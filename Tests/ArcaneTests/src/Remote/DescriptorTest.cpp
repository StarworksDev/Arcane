#if !defined(ARC_BUILD_DIST)
#include <catch2/catch_test_macros.hpp>
#include <Arcane/Remote/Descriptor.hpp>
#include <Arcane/Remote/Http.hpp> // MakeToken (test-only; the descriptor code never calls it)
#include <Arcane/Platform/Process.hpp>
#include <Arcane/Project/Project.hpp>
#include <Json.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#endif

namespace
{
#ifdef _WIN32
// The final file's DACL as SDDL; empty when either call fails (the CHECKs then fail).
std::wstring DaclString(const std::filesystem::path& path)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                nullptr, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
        return {};
    LPWSTR text = nullptr;
    const BOOL ok = ::ConvertSecurityDescriptorToStringSecurityDescriptorW(
        sd, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr);
    ::LocalFree(sd);
    if (!ok || !text)
        return {};
    std::wstring out(text);
    ::LocalFree(text);
    return out;
}
#endif
} // namespace

TEST_CASE("Self is live and a flipped start time is not", "[remote][descriptor]") {
    const Arcane::EditorLock::Info self = Arcane::EditorLock::Self();
    REQUIRE(self.pid != 0);
    REQUIRE(self.start != 0);
    CHECK(Arcane::EditorLock::IsLive(self));
    CHECK(Arcane::Platform::IsProcessLive(self.pid, self.start));
    CHECK(Arcane::Platform::IsProcessLive(self.pid, 0)); // start 0 = any start
    Arcane::EditorLock::Info recycled = self;
    recycled.start += 1;
    CHECK_FALSE(Arcane::EditorLock::IsLive(recycled));
    CHECK_FALSE(Arcane::Platform::IsProcessLive(self.pid, self.start + 1));
}

#ifdef _WIN32
TEST_CASE("a running process this user may not query is live, never deletable", "[remote][descriptor]") {
    // An elevated host (or a SYSTEM service) refuses OpenProcess with
    // ERROR_ACCESS_DENIED to a normal user. Find one such pid by probing the
    // pid space (Windows pids are multiples of 4); an elevated test run may
    // open every process, so none is found and the case is skipped.
    std::uint32_t denied = 0;
    for (std::uint32_t pid = 4; pid < 1u << 18 && denied == 0; pid += 4) {
        const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h != nullptr)
            ::CloseHandle(h);
        else if (::GetLastError() == ERROR_ACCESS_DENIED)
            denied = pid;
    }
    if (denied == 0)
        SKIP("every process opened: the run is elevated, so no access-denied pid exists to probe");
    CHECK(Arcane::Platform::IsProcessLive(denied, 0));
    CHECK(Arcane::Platform::IsProcessLive(denied, 12345)); // its start cannot be compared
    CHECK_FALSE(Arcane::Platform::IsProcessLive(0, 0));
}
#endif

TEST_CASE("descriptor write is atomic, owner-only, and a stale pid is not live", "[remote][descriptor]") {
    const auto dir = std::filesystem::temp_directory_path() / "remote-desc-test";
    std::filesystem::create_directories(dir);
    const Arcane::EditorLock::Info self = Arcane::EditorLock::Self();
    const auto path = Arcane::Remote::DescriptorPath(dir, Arcane::Remote::HostKind::Runtime, self.pid);
    CHECK(path.filename() == "runtime-" + std::to_string(self.pid) + ".json");
    CHECK(path.parent_path() == Arcane::Remote::DescriptorDir(dir, false));
    CHECK(path.parent_path().filename() == "Remote");
    Arcane::Remote::Descriptor d;
    d.protocol = 1;
    d.hostKind = Arcane::Remote::HostKind::Runtime;
    d.pid = self.pid;
    d.processStart = self.start;
    d.port = 9;
    d.token = Arcane::Remote::MakeToken();
    d.state = "ready";
    d.heartbeatUnixMs = 1;
    d.abi = 61;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, d));
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    CHECK_FALSE(std::filesystem::exists(tmp));
#if !defined(_WIN32)
    namespace fs = std::filesystem;
    CHECK((fs::status(path).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
#endif
#ifdef _WIN32
    const std::wstring dacl = DaclString(path);
    std::string daclAscii; // SDDL is ASCII
    for (const wchar_t c : dacl) daclAscii += static_cast<char>(c);
    INFO("DACL: " << daclAscii);
    REQUIRE_FALSE(dacl.empty());
    CHECK(dacl.find(L"OW") != std::wstring::npos);
    CHECK(dacl.find(L"SY") != std::wstring::npos);
    CHECK(dacl.find(L"WD") == std::wstring::npos);
#endif
    auto back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->token == d.token);
    CHECK(back->abi == 61);
    CHECK(back->protocol == 1);
    CHECK(back->hostKind == Arcane::Remote::HostKind::Runtime);
    CHECK(back->pid == self.pid);
    CHECK(back->processStart == self.start);
    CHECK(back->port == 9);
    CHECK(back->state == "ready");
    CHECK_FALSE(Arcane::Remote::IsStale(*back));
    back->processStart += 1;
    CHECK(Arcane::Remote::IsStale(*back));
    CHECK(Arcane::Remote::IsHung(*back, back->heartbeatUnixMs + 10001));
    CHECK_FALSE(Arcane::Remote::IsHung(*back, back->heartbeatUnixMs + 10000));

    // A second write replaces the first in place (rename over an owner-only file).
    d.port = 10;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, d));
    CHECK_FALSE(std::filesystem::exists(tmp));
    auto again = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(again);
    CHECK(again->port == 10);
    std::filesystem::remove_all(dir);
}

TEST_CASE("TouchHeartbeat refreshes the heartbeat, state and frame and keeps the rest", "[remote][descriptor]") {
    const auto dir = std::filesystem::temp_directory_path() / "remote-desc-heartbeat-test";
    std::filesystem::remove_all(dir);
    const Arcane::EditorLock::Info self = Arcane::EditorLock::Self();
    const auto path = Arcane::Remote::DescriptorPath(dir, Arcane::Remote::HostKind::Editor, self.pid);
    Arcane::Remote::Descriptor d;
    d.hostKind = Arcane::Remote::HostKind::Editor;
    d.pid = self.pid;
    d.processStart = self.start;
    d.port = 4242;
    d.token = Arcane::Remote::MakeToken();
    d.project = "D:/some/project";
    d.state = "starting";
    d.heartbeatUnixMs = 5;
    d.abi = 7;
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, d)); // creates the parent directory
    Arcane::Remote::TouchHeartbeat(path, 123456, "ready", 99);
    const auto back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->heartbeatUnixMs == 123456);
    CHECK(back->state == "ready");
    CHECK(back->frame == 99);
    CHECK(back->token == d.token);
    CHECK(back->port == 4242);
    CHECK(back->project == "D:/some/project");
    CHECK(back->abi == 7);
    CHECK(back->processStart == self.start);
    std::filesystem::remove_all(dir);
}

TEST_CASE("ReadDescriptor is tolerant of a newer host's descriptor", "[remote][descriptor]") {
    const auto dir = std::filesystem::temp_directory_path() / "remote-desc-tolerant-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "server-77.json";
    {
        nlohmann::json j = {
            { "protocol", 2 },
            { "hostKind", "server" },
            { "pid", 77 },
            { "processStart", 1234 },
            { "port", 5555 },
            { "token", std::string(64, 'a') },
            { "futureField", { { "nested", true } } },
        };
        std::ofstream out(path, std::ios::binary);
        out << j.dump();
    }
    const auto back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->protocol == 2);
    CHECK(back->hostKind == Arcane::Remote::HostKind::Server);
    CHECK(back->pid == 77);
    CHECK(back->port == 5555);
    CHECK(back->abi == 0); // a missing abi reads 0

    // Malformed shapes are refused, not guessed at.
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "not json";
    }
    CHECK_FALSE(Arcane::Remote::ReadDescriptor(path));
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"protocol":-1,"hostKind":"server","pid":77,"port":5555,"token":"x"})";
    }
    CHECK_FALSE(Arcane::Remote::ReadDescriptor(path));
    CHECK_FALSE(Arcane::Remote::ReadDescriptor(dir / "absent.json"));
    std::filesystem::remove_all(dir);
}

TEST_CASE("a descriptor round-trips client, and a file without the key reads empty", "[remote][descriptor]") {
    const auto dir = std::filesystem::temp_directory_path() / "remote-descriptor-client";
    std::filesystem::remove_all(dir);
    const auto path = dir / "runtime-77.json";
    Arcane::Remote::Descriptor d;
    d.hostKind = Arcane::Remote::HostKind::Runtime;
    d.pid = 77;
    d.port = 5555;
    d.token = "t";
    d.client = "grok-review-r1";
    REQUIRE(Arcane::Remote::WriteDescriptorAtomic(path, d));
    auto back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->client == "grok-review-r1");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"({"protocol":1,"hostKind":"runtime","pid":77,"port":5555,"token":"t"})";
    }
    back = Arcane::Remote::ReadDescriptor(path);
    REQUIRE(back);
    CHECK(back->client.empty());
    std::filesystem::remove_all(dir);
}
#endif
