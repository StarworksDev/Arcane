// The Linux backend of the crash path (Diagnostics POSIX port, 2026-10-05):
// what only it does, pinned where the shared [diag] cases cannot see it.
//
// The shared cases prove a `.dmp` EXISTS. This one proves it is the artifact
// the port claims -- a Breakpad-format minidump (Base/Posix/MinidumpWriter.cpp)
// holding every thread, the engine's own module with its build id, and the
// DUMP_REQUESTED exception stream a survivable report carries -- and that the
// snapshot signal that captured the OTHER threads parked them and let them go:
// a helper thread blocked in a wait during the report must come out of it and
// finish normally, or a hang report would have broken the process it reports on.

#include <Arcane/Platform/Platform.hpp>

#if ARC_PLATFORM_LINUX

#include <Arcane/Base/Diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    std::vector<unsigned char> ReadBytes(const std::filesystem::path& p)
    {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }

    template <typename T>
    T At(const std::vector<unsigned char>& b, std::size_t off)
    {
        T v{};
        if (off + sizeof(T) <= b.size()) std::memcpy(&v, b.data() + off, sizeof(T));
        return v;
    }

    struct Stream { std::uint32_t size = 0; std::uint32_t rva = 0; bool found = false; };

    Stream FindStream(const std::vector<unsigned char>& b, std::uint32_t type)
    {
        const auto count = At<std::uint32_t>(b, 8);
        const auto dir   = At<std::uint32_t>(b, 12);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const std::size_t row = dir + std::size_t(i) * 12;
            if (At<std::uint32_t>(b, row) == type)
                return { At<std::uint32_t>(b, row + 4), At<std::uint32_t>(b, row + 8), true };
        }
        return {};
    }

    // MINIDUMP_STRING (u32 byte length + UTF-16) -> ASCII, for the names here.
    std::string ReadName(const std::vector<unsigned char>& b, std::uint32_t rva)
    {
        const auto bytes = At<std::uint32_t>(b, rva);
        std::string out;
        for (std::uint32_t i = 0; i + 1 < bytes && rva + 4 + i + 1 < b.size(); i += 2)
            out.push_back(static_cast<char>(b[rva + 4 + i]));
        return out;
    }
}

TEST_CASE("Diagnostics on Linux writes a Breakpad-format minidump of every thread, and the snapshotted threads carry on", "[diag]")
{
    std::error_code ec;
    const auto dir = std::filesystem::temp_directory_path() / "arcane-diag-test" / "posix-minidump";
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    Arcane::Diagnostics::Config cfg;
    cfg.appName             = "DiagPosixTest";
    cfg.dumpDir             = dir.string();
    cfg.installCrashHandler = false;
    cfg.startHangWatchdog   = false;
    cfg.spawnReporter       = false;
    Arcane::Diagnostics::Install(cfg);
    struct Disarm { ~Disarm() { Arcane::Diagnostics::Shutdown(); } } disarm;

    // A thread parked in a condition-variable wait for the whole report: the
    // snapshot signal interrupts that wait, and it must resume it.
    std::mutex              m;
    std::condition_variable cv;
    bool                    release = false;
    std::atomic<bool>       waiting{false};
    std::atomic<bool>       finished{false};
    std::thread helper([&] {
        std::unique_lock lock(m);
        waiting = true;
        cv.wait(lock, [&] { return release; });
        finished = true;
    });
    while (!waiting) std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const std::string txt = Arcane::Diagnostics::WriteReport("hang: posix minidump shape");
    REQUIRE_FALSE(txt.empty());

    { std::lock_guard lock(m); release = true; }
    cv.notify_all();
    helper.join();
    CHECK(finished.load());   // the snapshotted thread was released and ran to completion

    std::filesystem::path dmp(txt);
    dmp.replace_extension(".dmp");
    REQUIRE(std::filesystem::exists(dmp));
    const auto b = ReadBytes(dmp);
    REQUIRE(b.size() > 32);

    CHECK(At<std::uint32_t>(b, 0) == 0x504d444du);                // "MDMP"
    CHECK((At<std::uint32_t>(b, 4) & 0xFFFFu) == 0xa793u);         // MINIDUMP_VERSION

    // Thread list: this thread (the submitter, walked) AND the helper, each
    // with a full AMD64 context and some stack.
    const Stream threads = FindStream(b, 3);
    REQUIRE(threads.found);
    const auto threadCount = At<std::uint32_t>(b, threads.rva);
    CHECK(threadCount >= 2u);
    std::uint32_t withContextAndStack = 0;
    for (std::uint32_t i = 0; i < threadCount; ++i)
    {
        const std::size_t row = threads.rva + 4 + std::size_t(i) * 48;
        if (At<std::uint32_t>(b, row + 40) == 1232u && At<std::uint32_t>(b, row + 32) > 0u)
            ++withContextAndStack;
    }
    CHECK(withContextAndStack >= 2u);

    // Module list: the engine itself, with a GNU build id as a BpEL record.
    const Stream modules = FindStream(b, 4);
    REQUIRE(modules.found);
    bool sawCore = false, coreHasBuildId = false;
    const auto moduleCount = At<std::uint32_t>(b, modules.rva);
    for (std::uint32_t i = 0; i < moduleCount; ++i)
    {
        const std::size_t row = modules.rva + 4 + std::size_t(i) * 108;
        const std::string name = ReadName(b, At<std::uint32_t>(b, row + 20));
        if (name.find(Arcane::Platform::SharedLibraryFileName("ArcaneCore")) == std::string::npos) continue;
        sawCore = true;
        const auto cvSize = At<std::uint32_t>(b, row + 76);
        const auto cvRva  = At<std::uint32_t>(b, row + 80);
        coreHasBuildId = cvSize > 4u && At<std::uint32_t>(b, cvRva) == 0x4270454cu;   // "BpEL"
    }
    CHECK(sawCore);
    CHECK(coreHasBuildId);

    // A survivable report is a snapshot, not a fault: DUMP_REQUESTED.
    const Stream exception = FindStream(b, 6);
    REQUIRE(exception.found);
    CHECK(At<std::uint32_t>(b, exception.rva + 8) == 0xFFFFFFFFu);

    // Breakpad's Linux streams: the memory map a symbolizer places modules by.
    CHECK(FindStream(b, 0x47670009u).found);   // MD_LINUX_MAPS
    CHECK(FindStream(b, 7).found);             // SystemInfo
}

#endif
