#include <Arcane/Plugin/Module.hpp>

#include <Arcane/Base/Engine.hpp>           // ExecutablePathUtf8 -- the application directory (POSIX bare-name search)
#include <Arcane/Base/ForeignModules.hpp>   // ForeignModules::NoteOwned -- what we load ourselves is ours

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <tlhelp32.h>
#elif defined(__APPLE__)
    #include <dlfcn.h>
    #include <mach-o/dyld.h>     // the dyld image list: the loaded image's header + slide
    #include <Arcane/Platform/Process.hpp>   // MachImageExtent: its LC_SEGMENT_64 extent
#else
    #include <dlfcn.h>
    #include <link.h>   // dlinfo(RTLD_DI_LINKMAP), dl_iterate_phdr: the loaded image's extent
#endif

namespace
{
    // Last load failure reason, for the diagnostic. Thread-local so a worker
    // load never clobbers the main thread's pending message.
    thread_local std::string t_lastLoadError;

#if defined(_WIN32)
    bool EqualsNoCase(std::string_view a, std::string_view b) noexcept
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }

    bool StartsWithNoCase(std::string_view s, std::string_view prefix) noexcept
    {
        return s.size() >= prefix.size() && EqualsNoCase(s.substr(0, prefix.size()), prefix);
    }

    // ANY of these in the import table marks the image Debug-CRT -- one debug
    // import poisons the whole module regardless of what else it links.
    constexpr std::string_view kDebugCrtImports[] = {
        "ucrtbased.dll", "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll",
    };
    // Positive release evidence (the ucrt also surfaces as api-ms-win-crt-*
    // apiset forwarders). Only consulted when no debug import was seen.
    constexpr std::string_view kReleaseCrtImports[] = {
        "ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll",
    };
#endif
}

namespace Arcane
{
    Module::Module(std::filesystem::path path, NativeHandle handle) noexcept
        : m_path(std::move(path)), m_handle(handle)
    {
    }

    Module::~Module()
    {
        Unload();
    }

    Module::Module(Module&& other) noexcept
        : m_path(std::move(other.m_path)), m_handle(other.m_handle)
    {
        other.m_handle = nullptr;
    }

    Module& Module::operator=(Module&& other) noexcept
    {
        if (this != &other)
        {
            Unload();
            m_path = std::move(other.m_path);
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    const std::string& Module::LastLoadError() noexcept { return t_lastLoadError; }

    std::optional<Module> Module::Load(std::filesystem::path path)
    {
        t_lastLoadError.clear();
#if defined(_WIN32)
        NativeHandle handle = reinterpret_cast<NativeHandle>(::LoadLibraryW(path.c_str()));
        if (!handle)
        {
            const DWORD err = ::GetLastError();
            char buf[512] = {};
            ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, err, 0, buf, sizeof(buf) - 1, nullptr);
            t_lastLoadError = "error " + std::to_string(err) + ": " + buf;
        }
#else
        // A BARE file name ("HotReloadPluginV1.so"): dlopen would search only
        // the loader's library path, while LoadLibraryW searches the
        // application directory first. Mirror that order -- the exe's
        // directory, then the current directory, then the system search -- so
        // a host or test names a module beside itself the same way on both
        // platforms. Any path with a directory part is used exactly as given.
        std::filesystem::path resolved = path;
        if (!path.empty() && !path.has_parent_path())
        {
            std::error_code ec;
            const std::string self = ExecutablePathUtf8();
            const std::filesystem::path besideExe =
                self.empty() ? std::filesystem::path{} : std::filesystem::path(self).parent_path() / path;
            if (!besideExe.empty() && std::filesystem::exists(besideExe, ec))
                resolved = besideExe;
            else if (std::filesystem::exists(path, ec))
                resolved = std::filesystem::path(".") / path;
        }
        NativeHandle handle = ::dlopen(resolved.c_str(), RTLD_NOW | RTLD_LOCAL);

        // The DEPENDENCY half of the same rule. The PE loader maps a module's
        // own imports (a game module's ArcaneClient.dll) from the application
        // directory too; ELF looks only at the module's RUNPATH,
        // LD_LIBRARY_PATH and the system paths -- and a hot-reload copy runs
        // from a temp directory, where $ORIGIN finds nothing. So when the
        // failure names a missing DEPENDENCY that sits beside the exe, map it
        // from there and retry: an already-loaded library satisfies a later
        // DT_NEEDED by name. Bounded; anything else fails as before.
        for (int attempt = 0; !handle && attempt < 8; ++attempt)
        {
            const char* raw = ::dlerror();
            const std::string err = raw ? raw : "dlopen failed";
            t_lastLoadError = err;

#if defined(__APPLE__)
            // dyld: "dlopen(<module>, 0x0002): Library not loaded:
            // @rpath/<dependency>\n  Referenced from: ...". A Mach-O module
            // names its engine dylibs by install name (@rpath/libX.dylib);
            // the leaf is what sits beside the exe.
            constexpr std::string_view kNotLoaded = "Library not loaded: ";
            const std::size_t at = err.find(kNotLoaded);
            if (at == std::string::npos)
                break;
            std::string dependency = err.substr(at + kNotLoaded.size());
            dependency = dependency.substr(0, dependency.find_first_of("\r\n"));
            if (const std::size_t slash = dependency.rfind('/'); slash != std::string::npos &&
                dependency.rfind("@rpath/", 0) == 0)
                dependency = dependency.substr(slash + 1);
#else
            // glibc: "<dependency>: cannot open shared object file: ..."
            constexpr std::string_view kMissing = ": cannot open shared object file";
            const std::size_t tail = err.find(kMissing);
            if (tail == std::string::npos)
                break;
            const std::size_t head = err.rfind(": ", tail == 0 ? 0 : tail - 1);
            const std::string dependency =
                err.substr(head == std::string::npos ? 0 : head + 2,
                           tail - (head == std::string::npos ? 0 : head + 2));
#endif
            const std::string self = ExecutablePathUtf8();
            if (dependency.empty() || dependency.find('/') != std::string::npos || self.empty())
                break;   // the module itself is missing, or the name is a path: not this rule
            std::error_code ec;
            const std::filesystem::path besideExe = std::filesystem::path(self).parent_path() / dependency;
            if (!std::filesystem::exists(besideExe, ec))
                break;
            if (!::dlopen(besideExe.c_str(), RTLD_NOW | RTLD_LOCAL))
            {
                const char* depErr = ::dlerror();
                t_lastLoadError = depErr ? depErr : err;
                break;
            }
            handle = ::dlopen(resolved.c_str(), RTLD_NOW | RTLD_LOCAL);
        }
        if (handle)
            t_lastLoadError.clear();
#endif
        if (!handle)
            return std::nullopt;

        // "We loaded this ourselves": its directory is one of this host's own
        // trees from now on, so the injected-module scan (Base/ForeignModules)
        // never lists a game module under <project>/Binaries or a plugin in
        // its own folder as a foreign body. The ONE loader every engine-
        // initiated load goes through, which is why the note lives here.
        ForeignModules::NoteOwned(path.generic_string());

        return Module(std::move(path), handle);
    }

    void* Module::Symbol(const char* name) const noexcept
    {
        if (!m_handle || !name)
            return nullptr;

#if defined(_WIN32)
        return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(m_handle), name));
#else
        return ::dlsym(m_handle, name);
#endif
    }

    namespace
    {
        // identityPath is the file the handle was opened from. Mach-O has no
        // handle-to-image query; the dyld name can also disagree with the path
        // we loaded (/private/var vs /var/folders on the macos-15 runner), so
        // the Apple walk falls back to filesystem::equivalent against it.
        Module::ImageSpan ImageFromHandle(void* handle, const std::filesystem::path& identityPath = {}) noexcept
        {
            if (!handle)
                return {};
#if defined(_WIN32)
            (void)identityPath;
            // On Windows an HMODULE IS the image base. SizeOfImage is read straight
            // out of the mapped PE headers rather than via GetModuleInformation so
            // this costs no psapi link. Both signatures are checked because a bad
            // read here would hand back a range that disowns the wrong module's
            // descriptors -- far worse than returning "unknown".
            const auto* base = reinterpret_cast<const unsigned char*>(handle);
            const auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            if (dos->e_magic != IMAGE_DOS_SIGNATURE)
                return {};
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature != IMAGE_NT_SIGNATURE)
                return {};
            return Module::ImageSpan{handle, static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage)};
#elif defined(__APPLE__)
            // Mach-O (macOS port, 2026-10-07): dyld has no handle -> image query,
            // but a handle for an already-loaded image is exactly what dlopen
            // returns for that image's own path with RTLD_NOLOAD (refcounted, so
            // closed again). The matching image's LC_SEGMENT_64 commands, slid,
            // span the mapped image (Platform::MachImageExtent) -- the Mach-O
            // reading of PE's [base, base + SizeOfImage). No match is "unknown".
            // equivalent() compares device + inode, which no symlink spelling of
            // the path can defeat.
            const std::uint32_t count = ::_dyld_image_count();
            for (std::uint32_t i = 0; i < count; ++i)
            {
                const char* name = ::_dyld_get_image_name(i);
                if (!name)
                    continue;
                bool match = false;
                if (void* probe = ::dlopen(name, RTLD_LAZY | RTLD_NOLOAD))
                {
                    match = probe == handle;
                    ::dlclose(probe);
                }
                if (!match && !identityPath.empty())
                {
                    std::error_code ec;
                    match = std::filesystem::equivalent(std::filesystem::path(name), identityPath, ec) && !ec;
                }
                if (!match)
                    continue;

                const Arcane::Platform::ImageExtent extent =
                    Arcane::Platform::MachImageExtent(::_dyld_get_image_header(i), ::_dyld_get_image_vmaddr_slide(i));
                if (extent.size == 0)
                    return {};
                return Module::ImageSpan{ reinterpret_cast<const void*>(extent.base), static_cast<std::size_t>(extent.size) };
            }
            return {};
#else
            (void)identityPath;
            // ELF (Linux port, 2026-10-05): the object's link_map names its load
            // bias (l_addr); dl_iterate_phdr then yields that same object's
            // program headers, and the PT_LOAD segments' union IS the mapped
            // image -- the ELF reading of PE's [base, base + SizeOfImage).
            // Matched on BOTH the load bias and the link_map's name, so a
            // mismatch is "unknown" (callers skip disowning) rather than a range
            // that would disown another module's descriptors.
            link_map* map = nullptr;
            if (::dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || !map)
                return {};

            struct Query
            {
                const link_map*     map = nullptr;
                Module::ImageSpan   span{};
            } query{ map, {} };

            ::dl_iterate_phdr([](dl_phdr_info* info, std::size_t, void* user) -> int
            {
                auto* q = static_cast<Query*>(user);
                if (info->dlpi_addr != q->map->l_addr)
                    return 0;
                const char* a = info->dlpi_name ? info->dlpi_name : "";
                const char* b = q->map->l_name ? q->map->l_name : "";
                if (std::strcmp(a, b) != 0)
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
                    q->span.base = reinterpret_cast<const void*>(info->dlpi_addr + lo);
                    q->span.size = static_cast<std::size_t>(hi - lo);
                }
                return 1;   // found the object: stop iterating
            }, &query);
            return query.span;
#endif
        }
    }

    Module::ImageSpan Module::Image() const noexcept
    {
        return ImageFromHandle(m_handle, m_path);
    }

    Module::ImageSpan Module::MappedImage(const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        const HMODULE handle = ::GetModuleHandleW(path.c_str());
        return ImageFromHandle(handle);
#else
        void* handle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        if (!handle)
            return {};
        const ImageSpan span = ImageFromHandle(handle, path);
        ::dlclose(handle);
        return span;
#endif
    }

    std::vector<Module::MappedModule> Module::MappedModules() noexcept
    {
        std::vector<MappedModule> result;
#if defined(_WIN32)
        // ERROR_BAD_LENGTH is the documented transient failure while another
        // thread is loading or unloading a module; retry it a few times.
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 8 && snapshot == INVALID_HANDLE_VALUE; ++attempt)
        {
            snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
            if (snapshot == INVALID_HANDLE_VALUE && ::GetLastError() != ERROR_BAD_LENGTH)
                break;
        }
        if (snapshot == INVALID_HANDLE_VALUE)
            return result;

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (::Module32FirstW(snapshot, &entry))
        {
            do
            {
                result.push_back(MappedModule{
                    std::filesystem::path(entry.szExePath),
                    ImageSpan{entry.modBaseAddr, static_cast<std::size_t>(entry.modBaseSize)} });
                entry.dwSize = sizeof(entry);
            }
            while (::Module32NextW(snapshot, &entry));
        }
        ::CloseHandle(snapshot);
#endif
        return result;
    }

    std::optional<Module> Module::PinMapped(const MappedModule& mapped) noexcept
    {
#if defined(_WIN32)
        if (!mapped.image.base)
            return std::nullopt;
        HMODULE handle = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  static_cast<LPCWSTR>(mapped.image.base), &handle) || !handle)
            return std::nullopt;
        if (static_cast<const void*>(handle) != mapped.image.base)
        {
            // The base now lies inside some OTHER image: the one asked about
            // was unmapped and its range reused. Not ours to pin.
            ::FreeLibrary(handle);
            return std::nullopt;
        }
        return Module(mapped.path, reinterpret_cast<NativeHandle>(handle));
#else
        (void)mapped;
        return std::nullopt;
#endif
    }

    CrtFlavor Module::DetectCrtFlavorFromImage(const unsigned char* data, std::size_t size,
                                               std::string* matchedImport) noexcept
    {
#if defined(_WIN32)
        // Parses the FILE layout (RVAs resolved through section headers), not a
        // loaded image -- the whole point is a verdict before LoadLibrary. Every
        // read is bounds-checked; any inconsistency is Unknown, never a fault.
        const auto in = [&](std::size_t off, std::size_t n) noexcept
        { return data && off <= size && n <= size - off; };

        if (!in(0, sizeof(IMAGE_DOS_HEADER)))
            return CrtFlavor::Unknown;
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, data, sizeof dos);
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return CrtFlavor::Unknown;

        const auto ntOff = static_cast<std::size_t>(dos.e_lfanew);
        if (!in(ntOff, sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)))
            return CrtFlavor::Unknown;
        DWORD sig = 0;
        std::memcpy(&sig, data + ntOff, sizeof sig);
        if (sig != IMAGE_NT_SIGNATURE)
            return CrtFlavor::Unknown;
        IMAGE_FILE_HEADER fh{};
        std::memcpy(&fh, data + ntOff + sizeof(DWORD), sizeof fh);

        const std::size_t optOff = ntOff + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!in(optOff, fh.SizeOfOptionalHeader) || fh.SizeOfOptionalHeader < sizeof(WORD))
            return CrtFlavor::Unknown;
        WORD magic = 0;
        std::memcpy(&magic, data + optOff, sizeof magic);

        DWORD importRva = 0;
        if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
                return CrtFlavor::Unknown;
            IMAGE_OPTIONAL_HEADER64 oh{};
            std::memcpy(&oh, data + optOff, sizeof oh);
            if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
                return CrtFlavor::Unknown;
            importRva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        }
        else if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        {
            if (fh.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER32))
                return CrtFlavor::Unknown;
            IMAGE_OPTIONAL_HEADER32 oh{};
            std::memcpy(&oh, data + optOff, sizeof oh);
            if (oh.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
                return CrtFlavor::Unknown;
            importRva = oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
        }
        else
            return CrtFlavor::Unknown;
        if (importRva == 0)
            return CrtFlavor::Unknown;   // no import table at all -- no verdict

        const std::size_t secOff = optOff + fh.SizeOfOptionalHeader;
        if (!in(secOff, static_cast<std::size_t>(fh.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER)))
            return CrtFlavor::Unknown;
        const auto rvaToOff = [&](DWORD rva) noexcept -> std::size_t
        {
            for (WORD i = 0; i < fh.NumberOfSections; ++i)
            {
                IMAGE_SECTION_HEADER sh{};
                std::memcpy(&sh, data + secOff + i * sizeof sh, sizeof sh);
                const DWORD span = std::max(sh.Misc.VirtualSize, sh.SizeOfRawData);
                if (rva >= sh.VirtualAddress && rva - sh.VirtualAddress < span)
                    return static_cast<std::size_t>(sh.PointerToRawData) + (rva - sh.VirtualAddress);
            }
            return SIZE_MAX;
        };

        std::string releaseMatch;
        // Hard iteration cap: a corrupt descriptor array must terminate anyway.
        for (std::size_t idx = 0; idx < 4096; ++idx)
        {
            const std::size_t dOff =
                rvaToOff(importRva + static_cast<DWORD>(idx * sizeof(IMAGE_IMPORT_DESCRIPTOR)));
            if (dOff == SIZE_MAX || !in(dOff, sizeof(IMAGE_IMPORT_DESCRIPTOR)))
                break;
            IMAGE_IMPORT_DESCRIPTOR desc{};
            std::memcpy(&desc, data + dOff, sizeof desc);
            if (desc.Name == 0)
                break;   // all-zero terminator (Name==0 is the canonical check)

            const std::size_t nOff = rvaToOff(desc.Name);
            if (nOff == SIZE_MAX || nOff >= size)
                continue;
            std::string dllName;
            for (std::size_t p = nOff; p < size && data[p] != 0 && dllName.size() < 256; ++p)
                dllName.push_back(static_cast<char>(data[p]));

            for (const std::string_view dbg : kDebugCrtImports)
                if (EqualsNoCase(dllName, dbg))
                {
                    if (matchedImport) *matchedImport = std::move(dllName);
                    return CrtFlavor::Debug;
                }
            if (releaseMatch.empty())
            {
                for (const std::string_view rel : kReleaseCrtImports)
                    if (EqualsNoCase(dllName, rel))
                        releaseMatch = dllName;
                if (releaseMatch.empty() && StartsWithNoCase(dllName, "api-ms-win-crt-"))
                    releaseMatch = dllName;
            }
        }
        if (!releaseMatch.empty())
        {
            if (matchedImport) *matchedImport = std::move(releaseMatch);
            return CrtFlavor::Release;
        }
        return CrtFlavor::Unknown;
#else
        (void)data; (void)size; (void)matchedImport;
        return CrtFlavor::Unknown;
#endif
    }

    CrtFlavor Module::ScanFileCrtFlavor(const std::filesystem::path& path,
                                        std::string* matchedImport) noexcept
    {
#if defined(_WIN32)
        try
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                return CrtFlavor::Unknown;
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                             std::istreambuf_iterator<char>());
            return DetectCrtFlavorFromImage(bytes.data(), bytes.size(), matchedImport);
        }
        catch (...)
        {
            return CrtFlavor::Unknown;   // I/O or allocation failure: no verdict
        }
#else
        (void)path; (void)matchedImport;
        return CrtFlavor::Unknown;
#endif
    }

    bool Module::IsMapped(const std::filesystem::path& path) noexcept
    {
#if defined(_WIN32)
        // GetModuleHandle never loads: it only looks the name up among the
        // modules already mapped (base name for a bare name, full path for a
        // path) and leaves the reference count alone.
        return ::GetModuleHandleW(path.c_str()) != nullptr;
#else
        void* handle = ::dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);   // NOLOAD: look up, never map
        if (!handle)
            return false;
        ::dlclose(handle);   // RTLD_NOLOAD still took a reference
        return true;
#endif
    }

    void Module::Unload() noexcept
    {
        if (!m_handle)
            return;

#if defined(_WIN32)
        ::FreeLibrary(reinterpret_cast<HMODULE>(m_handle));
#else
        ::dlclose(m_handle);
#endif
        m_handle = nullptr;
    }
}
