-- ============================================================================
-- arcane.lua -- the Arcane SDK premake module (engine-as-SDK, Slice 5).
--
-- An EXTERNAL project (one built OUTSIDE the Arcane.slnx workspace) consumes the
-- engine through this file. The "SDK" is the engine's own build output consumed
-- IN PLACE via the ARCANE_SDK env var (a packaged/installed SDK with a
-- multi-version registry is a later nicety -- spec 2026-07-22 S10/S12):
--
--   ARCANE_SDK  ->  the Arcane engine repo root (e.g. D:\dev\starworks\Arcane)
--                   include surface = $ARCANE_SDK/ArcaneClient/src (+ ThirdParty header-only)
--                   import lib       = $ARCANE_SDK/bin/<cfg>-<sys>-x86_64-md/ArcaneClient/ArcaneClient.lib
--                   ArcaneClient.dll        ships beside the host exe (host copies it)
--
-- Usage from a project's premake5.lua:
--   workspace "MyGame"
--       architecture "x64"
--       configurations { "Debug", "Release", "Dist" }
--   include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
--   arcane_game_module("MyGame")   -- declares the SharedLib game module (-> Binaries/MyGame.dll)
--   (arcane_game_module reads the game module's source directory -- Source/ by default, or the manifest's "sourceDir" e.g. Source/Game -- from the one .arcproj beside this premake5.lua)
--
-- The module implements the extern-C plugin ABI (Arcane/Plugin/PluginABI.hpp --
-- see kGamePluginABIVersion there); the host's ABI gate refuses a cross-build
-- mismatch. The include + link + define set below is the proven minimal
-- game-module recipe (ReferenceProject is the in-repo consumer; Aphelyon the
-- external one) so an external build matches an in-tree one.
-- ============================================================================

-- A consumer may pre-set the ARCANE_SDK global before including this file
-- (the in-repo ReferenceProject self-locates the SDK relatively so a fresh clone
-- builds without any env setup); the env var remains the external-project
-- contract (Aphelyon).
ARCANE_SDK = ARCANE_SDK or os.getenv("ARCANE_SDK")
if not ARCANE_SDK then
    error("ARCANE_SDK environment variable is not set.\n" ..
          "Point it at the Arcane engine repo root, e.g.:\n" ..
          "  setx ARCANE_SDK D:\\dev\\starworks\\Arcane\n" ..
          "then restart the terminal and re-generate.")
end
ARCANE_SDK = ARCANE_SDK:gsub("\\", "/")             -- normalize separators for premake tokens
local ARCANE_TP = ARCANE_SDK .. "/ThirdParty"       -- vendored header-only deps live inside the SDK repo

-- macOS port (2026-10-07): a Mac target builds for the host architecture
-- (arm64 on Apple Silicon), overridable with ARCANE_MAC_ARCH=arm64|x86_64 --
-- the SAME rule as the engine's premake5.lua (arcane_mac_architecture), so a
-- game module always links the engine flavor ARCANE_BIN below names. A
-- consumer workspace keeps its `architecture "x64"`; arcane_game_module and
-- arcane_core_consumer override it per project on a Mac target only.
function arcane_sdk_mac_architecture()
    local want = os.getenv("ARCANE_MAC_ARCH")
    if not want or want == "" then
        if os.host() == "macosx" then
            want = os.outputof("uname -m") or "arm64"
        else
            want = "arm64"
        end
    end
    want = want:lower()
    if want == "arm64" or want == "aarch64" then return "ARM64" end
    if want == "x86_64" or want == "x64" then return "x86_64" end
    error("ARCANE_MAC_ARCH must be arm64 or x86_64, got '" .. want .. "'")
end

-- The engine's per-config bin flavor. Must byte-match the engine's own outputdir
-- literal in the SDK root premake5.lua ("-md" = the dynamic-CRT flavor; /MD everywhere so
-- one heap crosses the ArcaneClient.dll/Game.dll boundary).
local ARCANE_BIN = ARCANE_SDK .. "/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"

-- The game module's source directory comes from the project's MANIFEST, never
-- from a premake option (decision record docs/research/2026-09-16-multiplayer-
-- shape-and-project-layout.md s5, ruling L3; plan ruling S1): `sourceDir`,
-- default "Source" (today's flat layout), or e.g. "Source/Game" for the
-- Unreal-style Source/<Module>/ layout. One field, three readers -- this
-- glob, the editor's Create C++ Class default, the docs -- so they cannot
-- drift. _MAIN_SCRIPT_DIR is the directory of the premake5.lua being run,
-- which is the project root for every consumer (the manifest sits beside it).
-- The validation mirrors ProjectManifest::FromJson exactly (Source itself, or
-- under Source/, no "..", no backslash, no leading slash); a bad value or an
-- ambiguous root is an error(), never a guess.
-- premake's json.decode maps a JSON null to nil, so an explicit "sourceDir":
-- null reads as ABSENT here (the module builds from Source/) while
-- ProjectManifest::FromJson rejects the whole manifest (the host refuses to
-- open the project): the host is the stricter of the two, which is the safe
-- direction. A consumer that overrides the workspace `location` would also
-- split _MAIN_SCRIPT_DIR (where the manifest is looked up) from
-- %{wks.location} (where the glob is rooted); neither in-repo nor external
-- consumer does.
local function arcane_module_source_dir()
    local root = _MAIN_SCRIPT_DIR
    local manifests = os.matchfiles(root .. "/*.arcproj")
    if #manifests == 0 then
        error("arcane_game_module: no .arcproj manifest beside " .. root .. "/premake5.lua (a game module needs its project manifest)")
    elseif #manifests > 1 then
        error("arcane_game_module: more than one .arcproj beside " .. root .. "/premake5.lua: " .. table.concat(manifests, ", "))
    end
    local text = io.readfile(manifests[1])
    if not text then error("arcane_game_module: cannot read " .. manifests[1]) end
    local doc, err = json.decode(text)
    if not doc then
        error("arcane_game_module: cannot parse " .. manifests[1] .. ": " .. tostring(err))
    end
    local dir = doc.sourceDir
    if dir == nil then
        return "Source"
    end
    if type(dir) ~= "string" then
        error("arcane_game_module: " .. manifests[1] .. ": sourceDir must be a string")
    end
    dir = dir:gsub("/+$", "")
    local underSource = (dir == "Source") or (dir:sub(1, 7) == "Source/")
    local escapes = dir:find("..", 1, true) or dir:find("\\", 1, true) or dir:find("//", 1, true)
    if not underSource or escapes or dir == "" then
        error("arcane_game_module: " .. manifests[1] .. ": sourceDir '" .. tostring(doc.sourceDir) ..
              "' must be Source or a directory under Source/ (no '..', no backslash)")
    end
    return dir
end

-- Declare + fully configure a game module (the project's primary plugin).
-- Call AFTER declaring the workspace + configurations. Builds -> Binaries/<name>.dll.
function arcane_game_module(name)
    local sourceDir = arcane_module_source_dir()
    project(name)
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"                         -- /MD: share one CRT heap with ArcaneClient.dll

        -- C4251 ("needs to have dll-interface"): disabled for every consumer, the
        -- same ruling the engine workspace makes for itself (premake5.lua, the
        -- workspace-level disablewarnings). The warning guards against a
        -- DLL/client CRT-layout mismatch; this helper's contract -- /MD, one
        -- toolset, one heap shared with ArcaneClient.dll -- makes that mismatch
        -- structurally impossible, and the host's CRT-flavor gate refuses a
        -- module that breaks it at load. Every exported engine class that holds
        -- an STL member would otherwise warn at every consumer call site.
        --
        -- 4251 is an MSVC warning NUMBER, so this is scoped to the MSVC
        -- compiler: `action:vs*` (the vs* generators do not expose a toolset
        -- to filter on) OR `toolset:msc` (the `ninja` action's default on a
        -- native Windows target). Unscoped, beta8's `gmake` action -- whose
        -- default toolset is GCC on every host, Windows included -- emits a
        -- meaningless `-Wno-4251` that GCC warns about on every TU
        -- (multibackend hardening, review F2). Every filter here is
        -- verified against real generated output for all three actions.
        filter "action:vs* or toolset:msc"
            disablewarnings { "4251" }
        filter {}

        targetname(name)
        filter "system:macosx"
            architecture(arcane_sdk_mac_architecture())
        filter {}
        -- A LOADED module is Name.so / Name.dylib off-Windows, never libName.*:
        -- the host names it by stem + the platform extension (Arcane/Platform/Platform.hpp).
        filter "system:not windows"
            targetprefix ""
            -- A game module's frames are on the crash path's walk too: the
            -- POSIX unwinder follows the frame-pointer chain (the engine's own
            -- premake5.lua sets the same flags; see Base/PortableStack.cpp).
            buildoptions { "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer" }
            -- ELF: a LOADED module keeps its own copy of every header-defined
            -- static, exactly as a PE DLL does. With default visibility GCC emits
            -- those (Astra's per-type meta factories, the pending-meta queue,
            -- fmt/spdlog statics) as STB_GNU_UNIQUE: bound process-wide AND
            -- marking the .so non-unloadable, so a hot-reloaded image's entries
            -- outlived it and were later run from unmapped code. Only the
            -- extern "C" GamePlugin_* entry points (ARCANE_GAME_MODULE_EXPORT,
            -- visibility("default")) leave the module.
            visibility "Hidden"
            inlinesvisibility "Hidden"
        -- Only when the compiler really is GCC: an external project (arcbuild,
        -- a game's own premake run) may generate gcc-toolset Makefiles and then
        -- build them with CXX=clang++, which rejects -fno-gnu-unique outright.
        if not (os.getenv("CXX") or ""):find("clang") then
        filter { "system:not windows", "toolset:gcc" }
            -- GCC still emits STB_GNU_UNIQUE for a few std:: objects, and ONE such
            -- symbol makes glibc refuse to unmap the module on dlclose; a PE module
            -- is gone after FreeLibrary, and hot reload is built on that.
            buildoptions { "-fno-gnu-unique" }
        end
        filter {}
        -- Flat Binaries/ (config-agnostic, matching the manifest's gameModule name).
        -- Dev + the host run Debug; Binaries/ holds the config the host loads.
        targetdir "%{wks.location}/Binaries"
        objdir "%{wks.location}/Intermediate/%{cfg.buildcfg}"

        -- Ninja-only: beta8 emits a DUPLICATE link edge per configuration when
        -- every configuration links directly to the same Binaries/<name>
        -- output (characterized RED: ninja on this fixture emits three
        -- `build Binaries/Fixture.dll` edges, one per Debug/Release/Dist,
        -- all targeting the identical path -- arcbuild multibackend
        -- hardening plan, Task 4). Give Ninja a configuration-unique link
        -- location instead; arcbuild's Ninja backend then copies the
        -- freshly-linked module into the canonical single slot every host
        -- expects (Binaries/<name>, the manifest's gameModule) after a
        -- successful build (arcbuild/src/Stage.cpp). Every other action
        -- (vs2026, gmake, xcode4, ...) keeps the flat targetdir above
        -- untouched.
        --
        -- The copy is arcbuild's, NOT a postbuildcommands entry, because
        -- beta8's ninja module cannot run one on Windows: it wraps the
        -- post-build in `cmd /C "..."` and escapes every inner quote as
        -- `\"`, which cmd.exe does not understand -- `\"Binaries\"` resolves
        -- to `\Binaries\`, the DRIVE ROOT (the first live run of the old
        -- {MKDIR}+{COPYFILE} pair created C:\Binaries and failed; every
        -- later run had its whole `&&` chain swallowed into `IF NOT EXIST`
        -- and did nothing while exiting 0), and the module's own appended
        -- stamp touch fails identically. Characterized live, review F4. A
        -- raw `ninja <stem>_<Config>` therefore links and stops;
        -- `arcbuild build/rebuild` is what updates the slot.
        --
        -- The unique location lives INSIDE Intermediate/<Config>/ (the
        -- objdir above) on purpose: that directory is one of the two
        -- filesystem clean targets arcbuild removes unconditionally
        -- (arcbuild/src/ProjectLayout.cpp CleanTargets -- Binaries/ and
        -- Intermediate/<config>/), so the linked DLL/PDB cannot survive an
        -- `arcbuild clean` whose backend step soft-skipped or failed. The
        -- earlier Intermediate/Ninja/<Config>/ spelling sat OUTSIDE that
        -- contract (review F4). ProjectLayout.cpp's NinjaLinkOutput is the
        -- driver-side spelling of THIS path; the [build-generator] case pins
        -- the generated link edges against it.
        filter "action:ninja"
            targetdir "%{wks.location}/Intermediate/%{cfg.buildcfg}/Ninja/Binaries"
        filter {}

        files { "%{wks.location}/" .. sourceDir .. "/**.cpp", "%{wks.location}/" .. sourceDir .. "/**.hpp" }

        -- Public engine header surface (in-place) + the header-only ThirdParty deps a
        -- game module pulls in transitively (glm/Astra scene types, imgui handoff,
        -- spdlog via Log.hpp, the Mosaic threading seam, Manifold2D through
        -- PhysicsComponents.hpp, and Json.hpp through InputActionAsset.hpp).
        includedirs {
            "%{wks.location}/" .. sourceDir,
            ARCANE_SDK .. "/ArcaneClient/src",
            -- Core's namespaced include root (<Arcane/Guid.hpp>, and since the
            -- Core-DLL split the whole headless engine layer: Base/Scene/
            -- Plugin/Project/Serialization/...) -- and its import lib: a game
            -- module links BOTH engine DLLs (spec docs/specs/
            -- 2026-09-15-core-dll-split-design.md s1.2); the host's own copies
            -- of both DLLs are what the loader binds.
            ARCANE_SDK .. "/ArcaneCore/src",
            ARCANE_TP .. "/glm",
            ARCANE_TP .. "/Astra/include",
            ARCANE_TP .. "/enkiTS/src",
            ARCANE_TP .. "/Manifold2D/include",
            ARCANE_TP .. "/imgui",
            ARCANE_TP .. "/spdlog/include",
            ARCANE_TP .. "/Mosaic/include",
            ARCANE_TP .. "/nlohmann",
        }

        -- Link the engine import libs by name out of the per-config SDK bin dirs.
        -- Neither is a project in this workspace, so premake treats them as
        -- library links resolved against libdirs (-> ArcaneCore.lib /
        -- ArcaneClient.lib). imgui's exported surface arrives through
        -- ArcaneClient's import lib (/WHOLEARCHIVE in the engine).
        libdirs { ARCANE_BIN .. "/ArcaneCore", ARCANE_BIN .. "/ArcaneClient" }
        links   { "ArcaneCore", "ArcaneClient" }

        defines {
            "GAME_BUILD_DLL",                         -- kept for an external module's own GAME_API; ARC_GAME_MODULE needs no define
            "ARC_MODULE_NAME=" .. name,               -- the cvar module (settings spec s4.3)
            -- adopt ArcaneClient's single GImGui. __declspec is PE/COFF-only;
            -- ELF and Mach-O have no import decoration (default visibility exports it).
            "IMGUI_API=" .. ((os.target() == "windows") and "__declspec(dllimport)" or ""),
            "_CRT_SECURE_NO_WARNINGS",
            "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        }

        filter "system:windows"
            systemversion "latest"
            -- The Windows system import libs a game module's engine-header
            -- closure reaches (advapi32: Astra::IsHugePagesAvailable's
            -- OpenProcessToken/PrivilegeCheck/LookupPrivilegeValueA; the rest
            -- are MSBuild's own default AdditionalDependencies set, minus the
            -- printer/ODBC ones nothing here touches). Under vs* MSBuild's
            -- project system supplies that default list implicitly, which is
            -- why this was never missed; Premake's `ninja` action drives
            -- `cl.exe /link` with an EXPLICIT list and gets none of it, so the
            -- fixture link failed on the advapi32 imports (multibackend
            -- hardening, review F1). Listed for every action -- redundant but
            -- harmless under vs*, `-l<lib>` against MinGW's import libs under
            -- gmake, `<lib>.lib` under ninja.
            links { "kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32", "uuid" }
        -- MSVC-only flags, scoped to the MSVC compiler (see the 4251 note
        -- above for why `action:vs* or toolset:msc`): beta8's `gmake` action
        -- hands g++ these verbatim otherwise, where a `/utf-8` is an input
        -- file name, not a switch.
        filter { "system:windows", "action:vs* or toolset:msc" }
            -- /arch:AVX2 matches the engine's x86 min-spec (Arcane::Simd) so inline
            -- header codegen shared across the DLL boundary agrees. /utf-8 for fmt/spdlog.
            buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        -- The same AVX2 min-spec for a GCC/Clang toolset -- on Linux/macOS
        -- (their default toolsets) and for `gmake` on Windows (MinGW).
        filter { "system:linux or system:macosx", "architecture:x86_64" }
            buildoptions { "-mavx2", "-mfma" }
        filter { "system:windows", "toolset:gcc or toolset:clang", "architecture:x86_64" }
            buildoptions { "-mavx2", "-mfma" }

        -- Per-config: runtime + NDEBUG must match ArcaneClient.dll's flavor (the vulkan.hpp
        -- dispatcher layout + inline header layouts are NDEBUG-conditional).
        filter "configurations:Debug"
            defines { "ARC_BUILD_DEBUG" }
            runtime "Debug"
            symbols "on"
        filter "configurations:Release"
            defines { "ARC_BUILD_RELEASE", "NDEBUG" }
            runtime "Release"
            optimize "speed"
            symbols "on"
        filter "configurations:Dist"
            defines { "ARC_BUILD_DIST", "NDEBUG" }
            runtime "Release"
            optimize "speed"
            symbols "off"
        filter {}
end

-- ============================================================================
-- Core-only consumers (Core-DLL split, spec docs/specs/2026-09-15-core-dll-split-
-- design.md s1.2 / s8, Plan 3). An external exe or static lib that needs the
-- headless engine -- Cli, Guid, Base/Log, Project, ... -- and NOTHING from the
-- presentation DLL links ArcaneCore.dll alone. Aphelyon's three services,
-- their Common lib and their test exes are the consumers; the recipe is lifted
-- from the engine's own ArcaneServer block in premake5.lua (the proven
-- Core-only host).
--
-- arcane_core_consumer() configures the CURRENT project: call it INSIDE a
-- `project` block, in place of `staticruntime`. It composes into whatever
-- shape the project has (ConsoleApp, StaticLib, a factory function) rather
-- than declaring one, because Core consumers do not share a shape the way
-- game modules do. It adds:
--   * staticruntime "off" -- /MD, one CRT heap across the DLL boundary (the
--     whole reason a consumer links a DLL instead of compiling Core from
--     source: objects allocated in ArcaneCore.dll are freed by the caller);
--   * the Core include root + the SDK-PRIVATE header-only deps a Core header
--     closure can reach (glm, Astra, enkiTS, Manifold2D, Mosaic). spdlog and
--     nlohmann are deliberately NOT added: a consumer vendors its own copies
--     (Aphelyon does, at the engine's versions), and two copies of a
--     header-only library on one include path is a version split waiting to
--     happen -- a consumer without them gets a clear missing-header error;
--   * the import lib + libdir;
--   * the engine's flavor contract, the same lines arcane_game_module
--     carries: /utf-8, /arch:AVX2 (ArcaneCore.dll is built AVX2 workspace-wide,
--     so the process already requires it -- matching keeps inline header
--     codegen identical across the boundary), and per-config runtime +
--     ARC_BUILD_DEBUG / ARC_BUILD_RELEASE+NDEBUG / ARC_BUILD_DIST+NDEBUG so inline
--     header layouts under #ifndef NDEBUG agree with the DLL's.
-- It ends with `filter {}` so the caller's following lines are unfiltered.
--
-- arcane_core_stage_dll() adds the postbuild copy of ArcaneCore.dll beside an
-- exe's output (the dev bin layout; ArcaneRuntime's own postbuild is the
-- template). StaticLib consumers do not call it.
-- ============================================================================
function arcane_core_consumer()
    staticruntime "off"
    filter "system:macosx"
        architecture(arcane_sdk_mac_architecture())
    filter {}

    -- C4251 ("needs to have dll-interface"): disabled for every consumer, the
    -- same ruling the engine workspace makes for itself (premake5.lua, the
    -- workspace-level disablewarnings). The warning guards against a
    -- DLL/client CRT-layout mismatch; this helper's contract -- /MD, one
    -- toolset, one heap shared with ArcaneCore.dll -- makes that mismatch
    -- structurally impossible, and the host's CRT-flavor gate refuses a
    -- module that breaks it at load. Every exported engine class that holds
    -- an STL member would otherwise warn at every consumer call site.
    -- MSVC-scoped for the same reason as arcane_game_module's (an MSVC
    -- warning number is meaningless to GCC).
    filter "action:vs* or toolset:msc"
        disablewarnings { "4251" }
    filter {}

    includedirs {
        ARCANE_SDK .. "/ArcaneCore/src",
        ARCANE_TP .. "/glm",
        ARCANE_TP .. "/Astra/include",
        ARCANE_TP .. "/enkiTS/src",
        ARCANE_TP .. "/Manifold2D/include",
        ARCANE_TP .. "/Mosaic/include",
    }

    -- The cvar module a consumer's ARC_CVAR/ARC_COMMAND declarations name
    -- (Arcane/Config/CVarModule.hpp; settings spec s4.3): its own project.
    defines { "ARC_MODULE_NAME=%{prj.name}" }

    libdirs { ARCANE_BIN .. "/ArcaneCore" }
    links   { "ArcaneCore" }

    -- Same toolset scoping as arcane_game_module: MSVC flags only reach the
    -- MSVC compiler; a GCC/Clang toolset (gmake on Windows) gets the -m
    -- spelling of the AVX2 min-spec instead.
    filter { "system:windows", "action:vs* or toolset:msc" }
        buildoptions { "/utf-8", "/arch:AVX2" }
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }
    filter { "system:windows", "toolset:gcc or toolset:clang", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }

    filter "configurations:Debug"
        defines { "ARC_BUILD_DEBUG" }
        runtime "Debug"
    filter "configurations:Release"
        defines { "ARC_BUILD_RELEASE", "NDEBUG" }
        runtime "Release"
    filter "configurations:Dist"
        defines { "ARC_BUILD_DIST", "NDEBUG" }
        runtime "Release"
    filter {}
end

function arcane_core_stage_dll()
    postbuildcommands {
        '{COPYFILE} "' .. ARCANE_BIN .. '/ArcaneCore/ArcaneCore.dll" "%{cfg.buildtarget.directory}/ArcaneCore.dll"',
    }
end
