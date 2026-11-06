#pragma once
#include <Arcane/Modules/ArcaneEngineApi.hpp>

// Module: the engine's low-level RAII wrapper for one loaded system module
// (DLL/shared object). It knows paths and raw symbols only; plugin ABI and
// reload policy live above this layer.

#include <Arcane/Core/Api.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Arcane
{
    // Which C runtime family a PE image links, read from its import table
    // WITHOUT loading it (see Module::ScanFileCrtFlavor). Unknown means "no
    // verdict" -- not a PE, unreadable, or no recognizable CRT import -- and
    // callers must fail OPEN on it (let the OS loader produce its own error),
    // never treat it as a mismatch.
    enum class CrtFlavor : std::uint8_t { Unknown, Release, Debug };

    class ARC_ENGINE_API Module
    {
    public:
        Module() = default;
        ~Module();

        Module(const Module&) = delete;
        Module& operator=(const Module&) = delete;

        Module(Module&& other) noexcept;
        Module& operator=(Module&& other) noexcept;

        static std::optional<Module> Load(std::filesystem::path path);

        // The OS-reported reason the most recent Load() on THIS THREAD failed --
        // e.g. "error 126: The specified module could not be found." Empty after
        // a successful Load(), or if Load() has not failed yet on this thread.
        // Thread-local so a worker thread's load failure never clobbers the main
        // thread's pending diagnostic message (see PluginHost's plugin_load,
        // which runs on BootThread::Main -- but the seam itself makes no
        // assumption about which thread calls it).
        [[nodiscard]] static const std::string& LastLoadError() noexcept;

        // Classifies the CRT family a PE file on disk links against by walking
        // its import table -- WITHOUT LoadLibrary, because a config-mismatched
        // DLL's static initializers run inside LoadLibrary itself (the desk
        // signature was an AV inside VCRUNTIME140_1 at plugin_load, before any
        // export could be checked). Debug = any debug-CRT import (ucrtbased,
        // vcruntime140d, ...); Release = a release-CRT import with no debug
        // evidence; Unknown = anything else, including I/O failure. When
        // `matchedImport` is non-null it receives the import that decided the
        // verdict. Windows-only; always Unknown elsewhere. Never throws.
        [[nodiscard]] static CrtFlavor ScanFileCrtFlavor(const std::filesystem::path& path,
                                                         std::string* matchedImport = nullptr) noexcept;

        // The pure core of ScanFileCrtFlavor: same classification over an
        // in-memory copy of the FILE bytes (disk layout, not a loaded image).
        // Defensive against truncated/corrupt input -- any inconsistency yields
        // Unknown rather than a read past `size`.
        [[nodiscard]] static CrtFlavor DetectCrtFlavorFromImage(const unsigned char* data, std::size_t size,
                                                                std::string* matchedImport = nullptr) noexcept;

        // True when an image `path` names is ALREADY mapped in this process --
        // a bare file name matches by base name, a path by its full path -- so
        // a Load() of it bumps the loader's reference and runs NO static
        // initializer. Plugin::Load asks before it loads: a refused image that
        // was already mapped (an engine DLL named as a plugin by mistake) owns
        // none of the registrations under its stem, and must not drop the live
        // owner's. Never throws; false on any failure.
        [[nodiscard]] static bool IsMapped(const std::filesystem::path& path) noexcept;

        void* Symbol(const char* name) const noexcept;
        void Unload() noexcept;

        [[nodiscard]] bool IsLoaded() const noexcept { return m_handle != nullptr; }
        [[nodiscard]] const std::filesystem::path& Path() const noexcept { return m_path; }

        // The loaded image's address span. Exists so callers can DISOWN anything
        // that stored a pointer INTO this module before it is unmapped -- most
        // importantly Astra's ComponentDescriptors, whose raw function pointers
        // are left aiming at freed code by Unload() (see PluginHost::TeardownImage
        // and ComponentRegistry::UnregisterModuleRange).
        //
        // size == 0 means "unknown": not loaded, or a platform with no
        // implementation. Callers must treat that as "cannot disown" rather than
        // as an empty range.
        struct ImageSpan
        {
            const void* base = nullptr;
            std::size_t size = 0;
        };
        [[nodiscard]] ImageSpan Image() const noexcept;

        struct MappedModule
        {
            std::filesystem::path path;
            ImageSpan             image;
        };
        // Snapshot every image currently mapped in this process. PluginHost
        // diffs this around each load so dependency images that ran
        // registration statics are pinned: on Windows for the load session,
        // then disowned before the session releases them; on ELF and Mach-O
        // for the life of the process (PinsForProcessLife), never disowned.
        // Windows, Linux and macOS. Each base equals the same image's
        // Module::Image().base. macOS leaves out dyld shared-cache images
        // (MH_DYLIB_IN_CACHE): their segment union spans the cache's shared
        // __LINKEDIT, and dyld never unloads them.
        [[nodiscard]] static std::vector<MappedModule> MappedModules() noexcept;
        // Take a loader reference on the ALREADY-mapped image `mapped` names.
        // Windows locates it by its base address (so a renamed or
        // differently-pathed DLL is still the one image); destroying the
        // returned Module drops exactly that reference. Linux and macOS reopen
        // its path with RTLD_NOLOAD, require the same base, and only then pin
        // it -- Linux promotes it with RTLD_NOLOAD | RTLD_NODELETE, macOS keeps
        // that look-up reference forever (dyld's RTLD_NODELETE does not keep
        // an image loaded) -- so the image stays loaded for the life of the
        // process (see PinsForProcessLife), and an image at a different base
        // is never pinned. Mach-O: each call adds a permanent dyld reference
        // (dyld's RTLD_NODELETE does not pin, see Module.cpp), so callers pin
        // each image once. Nothing is loaded and no initializer runs. nullopt
        // when no image is mapped at that base any more.
        [[nodiscard]] static std::optional<Module> PinMapped(const MappedModule& mapped) noexcept;
        // True where a PinMapped pin keeps the image loaded for the life of
        // the process (ELF and Mach-O): the image never unloads, so its
        // registrations stay valid and callers must NOT disown them. Engine
        // libraries are never hot-reloaded, only the game module is. False on
        // Windows, where the pin is one reference and the caller disowns
        // before releasing it.
        [[nodiscard]] static constexpr bool PinsForProcessLife() noexcept
        {
#if defined(_WIN32)
            return false;
#else
            return true;
#endif
        }
        // The image span of a module `path` already mapped in this process, or
        // `{nullptr, 0}` when it is not. Plugin::Load compares this to the
        // span after Load() so a refused already-mapped engine DLL is not
        // mistaken for a fresh image whose statics this Load ran.
        [[nodiscard]] static ImageSpan MappedImage(const std::filesystem::path& path) noexcept;

    private:
        using NativeHandle = void*;

        Module(std::filesystem::path path, NativeHandle handle) noexcept;

        std::filesystem::path m_path;
        NativeHandle m_handle = nullptr;
    };
}
