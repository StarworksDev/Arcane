#include <Arcane/Base/ForeignModules.hpp>

#include <Arcane/Base/Log.hpp>
#include <Arcane/Base/Diagnostics.hpp>   // Scan() also publishes the injected-module snapshot the crash thread reads (Task 5, R14)
#include <Arcane/Base/ModuleTable.hpp>   // Scan() publishes the snapshot Diagnostics::ModuleTable::Find reads (crash window plan 1, Task 3)

#include <algorithm>
#include <cctype>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
// PSAPI_VERSION 2 routes EnumProcessModulesEx / GetModuleFileNameExW to
// their kernel32 K32* exports (Windows 7+), so no psapi.lib is needed and
// the same DLL that already hosts everything else answers.
#define PSAPI_VERSION 2
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <Arcane/Base/Engine.hpp>        // ExecutablePathUtf8: the exe directory, an owned root
#include <Arcane/Platform/Process.hpp>   // MachImageExtent
#include <filesystem>
#include <mach-o/dyld.h>                 // _dyld_image_count & co: the Mach-O loader's image list
#else
#include <Arcane/Base/Engine.hpp>   // ExecutablePathUtf8: the exe directory, an owned root
#include <filesystem>
#include <link.h>                   // dl_iterate_phdr: the ELF loader's module list
#endif

namespace Arcane::ForeignModules
{
    namespace
    {
        // The table, verbatim from docs/research/2026-09-22-injected-overlay-
        // modules.md (Tier 1 and Tier 2 sections, plus the per-product
        // remedies). Base names only: matching is by exact, case-insensitive
        // base name, so a proxy dxgi.dll/d3d12.dll beside the exe (ReShade's
        // other install shape) is deliberately NOT listed -- it would match
        // the real one. Every consequence reads after "It ", because that is
        // how Report prints it.
        constexpr std::string_view kGpuTweakProduct =
            "ASUS GPU Tweak III on-screen display (GTIII-OSD64.exe / GTIII-OSDCtrl.exe)";
        constexpr std::string_view kGpuTweakConsequence =
            "hooks the present path and releases this process's ID3D12Device more often than it "
            "acquires it, so the device can be destroyed under the renderer at close (an unhandled "
            "STATUS_BREAKPOINT in D3D12SDKLayers without the reference armor); its own D3D12 errors "
            "appear in this log tagged [d3d12]";
        constexpr std::string_view kGpuTweakRemedy =
            "add ArcaneEditor.exe, ArcaneRuntime.exe and ArcaneTests.exe to GPU Tweak III's OSD "
            "Blacklist (\"exclude applications that do not use OSD\"), or disable its OSD";

        constexpr std::string_view kNahimicProduct =
            "Nahimic audio suite in-game overlay (A-Volute; bundled by MSI, ASUS and others)";
        constexpr std::string_view kNahimicConsequence =
            "is a present-path hook with a long record of crashing D3D11/D3D12 hosts and browser GPU "
            "processes (Firefox blocklists this family)";
        constexpr std::string_view kNahimicRemedy =
            "disable the in-game overlay in the Nahimic app or stop its service -- exiting the tray "
            "app alone is not enough, it restarts at boot";

        constexpr std::string_view kSonicProduct =
            "ASUS Sonic Studio overlay (A-Volute lineage)";
        constexpr std::string_view kSonicConsequence =
            "carries the same OSD hook family as Nahimic, with documented game crashes at launch and "
            "in D3D11 tests";
        constexpr std::string_view kSonicRemedy =
            "disable Sonic Studio's overlay or stop its service";

        constexpr std::string_view kRtssProduct =
            "RivaTuner Statistics Server (ships with MSI Afterburner, EVGA Precision X1 and others)";
        constexpr std::string_view kRtssConsequence =
            "hooks the present path and the swapchain as a frame limiter, with widely documented "
            "D3D12 launch failures and crashes -- several games refuse to start with it";
        constexpr std::string_view kRtssRemedy =
            "add this host to RTSS's application profiles with application detection level \"None\", "
            "or close RTSS";

        constexpr std::string_view kCortexProduct =
            "Razer Cortex in-game FPS overlay";
        constexpr std::string_view kCortexConsequence =
            "hooks the present path and is historically crash-prone (Mod Organizer 2 still warns "
            "about it)";
        constexpr std::string_view kCortexRemedy =
            "disable Razer Cortex's in-game overlay / FPS display";

        constexpr Entry kTable[] = {
            // ---- Tier 1: proven to corrupt or crash the host --------------
            { "GTIII-OSD64.dll",           kGpuTweakProduct, kTierDestabilising, kGpuTweakConsequence, kGpuTweakRemedy },
            { "GTIII-OSD64-GL.dll",        kGpuTweakProduct, kTierDestabilising, kGpuTweakConsequence, kGpuTweakRemedy },
            { "GTIII-OSD64-VK.dll",        kGpuTweakProduct, kTierDestabilising, kGpuTweakConsequence, kGpuTweakRemedy },
            { "NahimicOSD.dll",            kNahimicProduct,  kTierDestabilising, kNahimicConsequence,  kNahimicRemedy },
            { "NahimicMSIOSD.dll",         kNahimicProduct,  kTierDestabilising, kNahimicConsequence,  kNahimicRemedy },
            { "Nahimic2OSD.dll",           kNahimicProduct,  kTierDestabilising, kNahimicConsequence,  kNahimicRemedy },
            { "Nahimic2DevProps.dll",      kNahimicProduct,  kTierDestabilising, kNahimicConsequence,  kNahimicRemedy },
            { "NHAsusStrixOSD.dll",        kNahimicProduct,  kTierDestabilising, kNahimicConsequence,  kNahimicRemedy },
            { "AudioDevProps2.dll",        kNahimicProduct,  kTierDestabilising, kNahimicConsequence,  kNahimicRemedy },
            { "SS2OSD.dll",                kSonicProduct,    kTierDestabilising, kSonicConsequence,    kSonicRemedy },
            { "SS2DevProps.dll",           kSonicProduct,    kTierDestabilising, kSonicConsequence,    kSonicRemedy },
            { "SS3DevProps.dll",           kSonicProduct,    kTierDestabilising, kSonicConsequence,    kSonicRemedy },
            { "RTSSHooks.dll",             kRtssProduct,     kTierDestabilising, kRtssConsequence,     kRtssRemedy },
            { "RTSSHooks64.dll",           kRtssProduct,     kTierDestabilising, kRtssConsequence,     kRtssRemedy },
            { "k_fps32.dll",               kCortexProduct,   kTierDestabilising, kCortexConsequence,   kCortexRemedy },
            { "k_fps64.dll",               kCortexProduct,   kTierDestabilising, kCortexConsequence,   kCortexRemedy },
            // ---- Tier 2: present-path hooks, attribution only --------------
            { "nvspcap64.dll",             "NVIDIA ShadowPlay / GeForce Experience / NVIDIA app in-game overlay", kTierPresentHook, {}, {} },
            { "NvCamera64.dll",            "NVIDIA Ansel",                        kTierPresentHook, {}, {} },
            { "GameOverlayRenderer64.dll", "Steam overlay",                       kTierPresentHook, {}, {} },
            { "DiscordHook64.dll",         "Discord overlay",                     kTierPresentHook, {}, {} },
            { "graphics-hook64.dll",       "OBS Studio game capture",             kTierPresentHook, {}, {} },
            { "ow-graphics-hook64.dll",    "Overwolf overlay",                    kTierPresentHook, {}, {} },
            { "EOSOVH-Win64-Shipping.dll", "Epic Online Services overlay",        kTierPresentHook, {}, {} },
            { "igo64.dll",                 "EA app / Origin in-game overlay",     kTierPresentHook, {}, {} },
            { "TwitchNativeOverlay64.dll", "Twitch Studio overlay",               kTierPresentHook, {}, {} },
            { "medal-hook64.dll",          "Medal overlay",                       kTierPresentHook, {}, {} },
            { "fraps64.dll",               "Fraps",                               kTierPresentHook, {}, {} },
            { "bdcap64.dll",               "Bandicam",                            kTierPresentHook, {}, {} },
            { "ReShade64.dll",             "ReShade post-processing injector",    kTierPresentHook, {}, {} },
            { "SpecialK64.dll",            "Special K injector",                  kTierPresentHook, {}, {} },
        };

        std::string Lower(std::string_view s)
        {
            std::string out(s);
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool EqualsInsensitive(std::string_view a, std::string_view b) noexcept
        {
            if (a.size() != b.size())
                return false;
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        // The comparison form of a path: lower-cased, one separator, no
        // trailing separator. Case-folding a path is wrong on a case-
        // sensitive filesystem in general, but this compares directory
        // TREES the loader reported against roots the same loader reported,
        // and on Windows -- the only place the OS half runs -- both are
        // case-insensitive.
        std::string Canonical(std::string_view path)
        {
            std::string out = Lower(path);
            std::replace(out.begin(), out.end(), '\\', '/');
            while (out.size() > 1 && out.back() == '/')
                out.pop_back();
            return out;
        }

        // `path` is strictly inside the tree at `root` (both canonical).
        bool IsUnder(const std::string& path, const std::string& root) noexcept
        {
            return !root.empty()
                && path.size() > root.size()
                && path.compare(0, root.size(), root) == 0
                && path[root.size()] == '/';
        }

        std::string DirectoryOf(std::string_view path)
        {
            const std::string canonical = Canonical(path);
            const std::size_t slash     = canonical.find_last_of('/');
            return slash == std::string::npos ? std::string{} : canonical.substr(0, slash);
        }

        // The remembered scan (LastScan), the once-per-process ledger
        // (Report) and the owned roots (NoteOwned). One mutex for all three:
        // the watchdog thread reads the scan while the main thread may be
        // re-scanning at report time.
        std::mutex                      g_mutex;
        bool                            g_scanned = false;
        std::vector<Match>              g_lastScan;
        std::unordered_set<std::string> g_reported;     // lower-cased module names already logged
        std::vector<std::string>        g_ownedRoots;   // canonical; seeded with the exe dir on first use
        bool                            g_rootsSeeded = false;

        std::string ExeDirectory()
        {
#if defined(_WIN32)
            wchar_t wide[MAX_PATH * 4]{};
            const DWORD len = GetModuleFileNameW(nullptr, wide, static_cast<DWORD>(std::size(wide)));
            if (len == 0)
                return {};
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
            if (bytes <= 0)
                return {};
            std::string narrow(static_cast<std::size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), narrow.data(), bytes, nullptr, nullptr);
            return DirectoryOf(narrow);
#else
            return DirectoryOf(ExecutablePathUtf8());
#endif
        }

        // Called under g_mutex.
        void SeedRootsLocked()
        {
            if (g_rootsSeeded)
                return;
            g_rootsSeeded = true;
            if (const std::string exeDir = ExeDirectory(); !exeDir.empty())
                g_ownedRoots.push_back(exeDir);
        }
    }

    std::span<const Entry> Table() noexcept
    {
        return std::span<const Entry>(kTable, std::size(kTable));
    }

    std::optional<Match> WindowedVulkanBlocker(std::span<const std::string> moduleBaseNames)
    {
        for (const std::string& name : moduleBaseNames)
        {
            if (!EqualsInsensitive(name, "GTIII-OSD64-VK.dll"))
                continue;
            return Classify(name);
        }
        return std::nullopt;
    }

    std::optional<Match> Classify(std::string_view moduleBaseName)
    {
        for (const Entry& row : kTable)
        {
            if (EqualsInsensitive(row.module, moduleBaseName))
            {
                Match m;
                m.module      = std::string(moduleBaseName);
                m.product     = std::string(row.product);
                m.tier        = row.tier;
                m.consequence = std::string(row.consequence);
                m.remedy      = std::string(row.remedy);
                return m;
            }
        }
        return std::nullopt;
    }

    std::vector<Match> MatchAll(std::span<const std::string> moduleBaseNames)
    {
        std::vector<Match>              out;
        std::unordered_set<std::string> seen;
        for (const std::string& name : moduleBaseNames)
        {
            std::optional<Match> m = Classify(name);
            if (!m)
                continue;
            if (!seen.insert(Lower(name)).second)
                continue;
            out.push_back(std::move(*m));
        }
        return out;
    }

    Origin OriginOf(std::string_view path, std::span<const std::string> ownedRoots, std::string_view systemRoot)
    {
        if (path.empty())
            return Origin::Unknown;
        const std::string canonical = Canonical(path);
        for (const std::string& root : ownedRoots)
        {
            if (IsUnder(canonical, Canonical(root)))
                return Origin::Owned;
        }
        if (IsUnder(canonical, Canonical(systemRoot)))
            return Origin::System;
#if defined(__APPLE__)
        // macOS keeps the OS in TWO trees: /usr (SystemRoot) and /System
        // (every framework -- Metal, AppKit, IOKit -- and their drivers).
        // Both are SIP-protected and live in the dyld shared cache.
        if (IsUnder(canonical, "/system"))
            return Origin::System;
#endif
        return Origin::Foreign;
    }

    std::vector<Match> MatchAll(std::span<const LoadedModule> modules,
                                std::span<const std::string> ownedRoots,
                                std::string_view systemRoot)
    {
        std::vector<Match>              out;
        std::unordered_set<std::string> seen;
        for (const LoadedModule& loaded : modules)
        {
            // The table outranks the path: a catalogued module is what the
            // table says wherever its installer put it (AudioDevProps2.dll
            // sits in System32 on this desk).
            std::optional<Match> m = Classify(loaded.name);
            if (!m)
            {
                if (OriginOf(loaded.path, ownedRoots, systemRoot) != Origin::Foreign)
                    continue;
                m = Match{};
                m->module = loaded.name;
                m->tier   = kTierUncatalogued;
            }
            if (!seen.insert(Lower(loaded.name)).second)
                continue;
            m->path = loaded.path;
            out.push_back(std::move(*m));
        }
        return out;
    }

    std::string Tier1Names(std::span<const Match> matches)
    {
        std::string out;
        for (const Match& m : matches)
        {
            if (m.tier != kTierDestabilising)
                continue;
            if (!out.empty())
                out += ", ";
            out += m.module;
        }
        return out;
    }

    std::vector<std::string> OwnedRoots()
    {
        std::lock_guard lock(g_mutex);
        SeedRootsLocked();
        return g_ownedRoots;
    }

    void NoteOwned(std::string_view modulePath)
    {
        const std::string dir = DirectoryOf(modulePath);
        if (dir.empty())
            return;
        std::lock_guard lock(g_mutex);
        SeedRootsLocked();
        if (std::find(g_ownedRoots.begin(), g_ownedRoots.end(), dir) == g_ownedRoots.end())
            g_ownedRoots.push_back(dir);
    }

    std::string SystemRoot()
    {
#if defined(_WIN32)
        wchar_t wide[MAX_PATH]{};
        const UINT len = GetWindowsDirectoryW(wide, static_cast<UINT>(std::size(wide)));
        if (len == 0 || len >= std::size(wide))
            return {};
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0)
            return {};
        std::string narrow(static_cast<std::size_t>(bytes), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), narrow.data(), bytes, nullptr, nullptr);
        return narrow;
#else
        // Linux port (2026-10-05) and macOS (2026-10-07; OriginOf adds
        // /System there): the distribution's tree. /lib, /lib64 and
        // /bin are symlinks into /usr on every merged-/usr distro, and
        // EnumerateProcessModules reports CANONICAL paths, so the loader,
        // libc, the Vulkan loader/ICDs and a system SDL3 all resolve under it.
        return "/usr";
#endif
    }

    std::vector<LoadedModule> EnumerateProcessModules()
    {
        std::vector<LoadedModule> modules;
#if defined(_WIN32)
        const HANDLE process = GetCurrentProcess();
        // Two-pass: ask for the byte count, then fetch. LIST_MODULES_ALL is
        // what makes a 64-bit process list everything it holds; the default
        // filter would do the same here but the flag states the intent.
        DWORD needed = 0;
        if (!EnumProcessModulesEx(process, nullptr, 0, &needed, LIST_MODULES_ALL) || needed == 0)
            return modules;
        std::vector<HMODULE> handles(needed / sizeof(HMODULE));
        DWORD got = 0;
        if (!EnumProcessModulesEx(process, handles.data(), static_cast<DWORD>(handles.size() * sizeof(HMODULE)),
                                  &got, LIST_MODULES_ALL))
            return modules;
        const std::size_t count = std::min<std::size_t>(handles.size(), got / sizeof(HMODULE));
        modules.reserve(count);

        auto toUtf8 = [](const wchar_t* wide, DWORD len) -> std::string {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
            if (bytes <= 0)
                return {};
            std::string narrow(static_cast<std::size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(len), narrow.data(), bytes, nullptr, nullptr);
            return narrow;
        };

        for (std::size_t i = 0; i < count; ++i)
        {
            // The FULL path, and the base name taken from it -- one query
            // per module rather than two, and the two can never disagree.
            wchar_t wide[MAX_PATH * 4]{};
            const DWORD len = GetModuleFileNameExW(process, handles[i], wide, static_cast<DWORD>(std::size(wide)));
            if (len == 0)
                continue;
            LoadedModule m;
            m.path = toUtf8(wide, len);
            const std::size_t slash = m.path.find_last_of("\\/");
            m.name = slash == std::string::npos ? m.path : m.path.substr(slash + 1);
            if (m.name.empty())
                continue;

            // Load address + image size for Diagnostics::ModuleTable (crash
            // window plan 1, Task 3). Best-effort: a module this fails for
            // just keeps base=size=0 and Find() never matches it.
            MODULEINFO mi{};
            if (GetModuleInformation(process, handles[i], &mi, sizeof(mi)))
            {
                m.base = reinterpret_cast<std::uint64_t>(mi.lpBaseOfDll);
                m.size = mi.SizeOfImage;
            }
            modules.push_back(std::move(m));
        }
#elif defined(__APPLE__)
        // Mach-O: dyld's image list (the main executable first, then every
        // dylib, including the shared-cache system libraries). base/size are
        // the slid LC_SEGMENT_64 union, the same extent Module::Image reports.
        const std::uint32_t count = ::_dyld_image_count();
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const char* raw = ::_dyld_get_image_name(i);
            if (!raw || !*raw)
                continue;
            std::string path = raw;
            std::error_code ec;
            const std::filesystem::path real = std::filesystem::canonical(path, ec);
            if (!ec)
                path = real.generic_string();

            LoadedModule m;
            m.path = path;
            const std::size_t slash = m.path.find_last_of('/');
            m.name = slash == std::string::npos ? m.path : m.path.substr(slash + 1);
            if (m.name.empty())
                continue;
            const Platform::ImageExtent extent =
                Platform::MachImageExtent(::_dyld_get_image_header(i), ::_dyld_get_image_vmaddr_slide(i));
            m.base = extent.base;
            m.size = extent.size;
            modules.push_back(std::move(m));
        }
#else
        // ELF: dl_iterate_phdr walks the loader's own list (it takes the
        // loader lock, like EnumProcessModulesEx). The main program reports an
        // empty name -- its path is the exe's; the vDSO has no file at all and
        // is skipped. base/size are the PT_LOAD union, the same extent
        // Module::Image reports.
        const std::string exePath = ExecutablePathUtf8();
        std::pair<std::vector<LoadedModule>*, const std::string*> context{ &modules, &exePath };
        ::dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* user) -> int
        {
            auto& out = *static_cast<std::pair<std::vector<LoadedModule>*, const std::string*>*>(user);
            std::string path = (info->dlpi_name && *info->dlpi_name) ? info->dlpi_name : *out.second;
            if (path.empty() || path.find('/') == std::string::npos)
                return 0;   // the vDSO ("linux-vdso.so.1"): no file behind it
            std::error_code ec;
            const std::filesystem::path real = std::filesystem::canonical(path, ec);
            if (!ec)
                path = real.generic_string();

            LoadedModule m;
            m.path = path;
            const std::size_t slash = m.path.find_last_of('/');
            m.name = slash == std::string::npos ? m.path : m.path.substr(slash + 1);
            if (m.name.empty())
                return 0;

            ElfW(Addr) lo = ~ElfW(Addr){ 0 };
            ElfW(Addr) hi = 0;
            for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
            {
                const ElfW(Phdr)& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_LOAD)
                    continue;
                lo = std::min(lo, ph.p_vaddr);
                hi = std::max(hi, ph.p_vaddr + ph.p_memsz);
            }
            if (hi > lo)
            {
                m.base = static_cast<std::uint64_t>(info->dlpi_addr + lo);
                m.size = static_cast<std::uint64_t>(hi - lo);
            }
            out.first->push_back(std::move(m));
            return 0;
        }, &context);
#endif
        return modules;
    }

    std::vector<Match> Scan()
    {
        const std::vector<std::string> roots    = OwnedRoots();
        const std::vector<LoadedModule> modules = EnumerateProcessModules();
        std::vector<Match> matches = MatchAll(modules, roots, SystemRoot());

        // Publish the module snapshot for Diagnostics::ModuleTable (crash
        // window plan 1, Task 3) -- off the crash path, same as everything
        // else Scan() does; the crash thread only ever calls Find().
        Diagnostics::ModuleTable::Refresh(modules);

        // ...and the same for the INJECTED set (crash window plan 1, Task 5,
        // R14): the report's `injected` line and the envelope's
        // foreignModules come from a fixed-size snapshot taken here, never
        // from LastScan() -- that returns a heap copy under g_mutex, and the
        // crash thread may touch neither.
        Diagnostics::SnapshotInjectedModules(matches);

        {
            std::lock_guard lock(g_mutex);
            g_scanned  = true;
            g_lastScan = matches;
        }
        return matches;
    }

    std::optional<std::vector<Match>> LastScan()
    {
        std::lock_guard lock(g_mutex);
        if (!g_scanned)
            return std::nullopt;
        return g_lastScan;
    }

    std::vector<Match> Report()
    {
        std::vector<Match> fresh;
        for (Match& m : Scan())
        {
            {
                std::lock_guard lock(g_mutex);
                if (!g_reported.insert(Lower(m.module)).second)
                    continue;
            }
            switch (m.tier)
            {
            case kTierDestabilising:
                ARC_WARN("[foreign-module] {} is injected into this process: {}. It {}. Remedy: {}. ({})",
                         m.module, m.product, m.consequence, m.remedy, m.path);
                break;
            case kTierPresentHook:
                ARC_INFO("[foreign-module] {} is injected into this process: {} (present-path hook; "
                         "noted for attribution only) ({})",
                         m.module, m.product, m.path);
                break;
            default:
                ARC_INFO("[foreign-module] {} is loaded from outside this host's and Windows' directories "
                         "and is not a catalogued overlay; noted for attribution ({})",
                         m.module, m.path);
                break;
            }
            fresh.push_back(std::move(m));
        }
        return fresh;
    }
}
