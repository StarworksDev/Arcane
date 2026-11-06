-- ============================================================================
-- arcane.lua -- the Arcane SDK premake module (engine-as-SDK, Slice 5; the
-- module contract is engine modularization s4, 2026-10-11).
--
-- An EXTERNAL project (one built OUTSIDE the Arcane.slnx workspace) consumes the
-- engine through this file. The "SDK" is the engine's own build output consumed
-- IN PLACE via the ARCANE_SDK env var (a packaged/installed SDK with a
-- multi-version registry is a later nicety -- spec 2026-07-22 S10/S12):
--
--   ARCANE_SDK  ->  the Arcane engine repo root (e.g. D:\dev\starworks\Arcane)
--                   include surface = every linked module's $ARCANE_SDK/Source/<Type>/<Module>/src
--                                      (+ its public ThirdParty headers), read from the .arcmodule descriptors
--                   import libs      = $ARCANE_SDK/Binaries/<cfg>-<sys>-x86_64-md/<Module>/<Module>.lib
--                   module DLLs      ship beside the host exe (the host stages them)
--
-- Three entry points:
--
--   arcane_game_module("MyGame")   -- declares the game module (-> Binaries/MyGame.dll).
--       Reads the source directory (Source/ by default, or the manifest's
--       "sourceDir", e.g. Source/Game) from the one .arcproj beside this
--       premake5.lua, and the game's engine modules from the descriptor
--       <sourceDir>/MyGame.arcmodule (type Game; dependencies Runtime/Client
--       modules only). Only that descriptor's module CLOSURE is linked and
--       on the include path; a header of an undeclared module is a missing
--       header. No descriptor = the default set (ARC_GAME_DEFAULT_MODULES in
--       modules.lua: every Runtime + Client module, ImGui, ArcaneImGui).
--
--   arcane_module_consumer { "ArcaneCore", "ArcaneNet", ... }
--       -- inside any `project` block: a NON-game consumer (a server exe, a
--       static lib, a test exe) links those modules' closure on /MD with the
--       engine's flavor contract. See its block below.
--
--   arcane_stage_modules { ... }   -- inside an exe's `project` block: postbuild
--       copy of those modules' DLL closure beside the exe.
--
-- With --layout=monolithic (`arcbuild package`), arcane_game_module builds the
-- game as a static library and also declares the packaged <Project>.exe
-- (arcane_package_exe below): engine, host and game in one executable.
--
-- Usage from a game project's premake5.lua:
--   workspace "MyGame"
--       architecture "x64"
--       configurations { "Debug", "Release", "Dist" }
--   include(os.getenv("ARCANE_SDK") .. "/build/arcane.lua")
--   arcane_game_module("MyGame")
--
-- The module implements the extern-C plugin ABI (Arcane/Plugin/PluginABI.hpp --
-- see kGamePluginABIVersion in Arcane/Core/AbiVersion.hpp); the host's ABI
-- gate refuses a cross-build mismatch. ReferenceProject is the in-repo
-- consumer; Aphelyon the external one -- so an external build matches an
-- in-tree one.
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

-- --layout=modular|monolithic (engine modularization s2.5): modular builds the
-- game module DLL the hosts load; monolithic builds the packaged <Project>.exe
-- (`arcbuild package`). Declared here for consumers; the engine's own
-- premake5.lua declares the same option.
if not premake.option.get("layout") then
    newoption {
        trigger = "layout",
        value = "LAYOUT",
        description = "Engine link layout",
        allowed = { { "modular" }, { "monolithic" } },
        default = "modular",
    }
end
include(ARCANE_SDK .. "/build/modules.lua") -- shared descriptor reader for P4 consumers
local ARCANE_MODULES = arc_modules_load { ARCANE_SDK .. "/Source", ARCANE_SDK .. "/ThirdParty/imgui/ImGui.arcmodule", ARCANE_SDK .. "/ThirdParty/imgui-node-editor/ImGuiNodeEditor.arcmodule", ARCANE_SDK .. "/ThirdParty/SDL3/SDL3.arcmodule" }
local ARCANE_TP = ARCANE_SDK .. "/ThirdParty"       -- vendored header-only deps live inside the SDK repo

-- macOS port (2026-10-07): a Mac target builds for the host architecture
-- (arm64 on Apple Silicon), overridable with ARCANE_MAC_ARCH=arm64|x86_64 --
-- the SAME rule as the engine's premake5.lua (arcane_mac_architecture), so a
-- game module always links the engine flavor ARCANE_BIN below names. A
-- consumer workspace keeps its `architecture "x64"`; arcane_game_module and
-- arcane_module_consumer override it per project on a Mac target only.
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
local ARCANE_BIN = ARCANE_SDK .. "/Binaries/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"

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
    return (arcane_project_manifest())
end

-- The project's manifest beside premake5.lua: (sourceDir, manifest path).
function arcane_project_manifest()
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
        return "Source", manifests[1]
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
    return dir, manifests[1]
end

-- Declare + fully configure a game module (the project's primary plugin).
-- Call AFTER declaring the workspace + configurations. Builds -> Binaries/<name>.dll.
function arcane_game_module(name)
    local sourceDir = arcane_module_source_dir()
    -- The game's engine modules (engine modularization s4): its descriptor,
    -- validated against the SDK's module graph (schema, type Game, the type
    -- rule, one ARC_GAME_MODULE in the sources), else the default set.
    local sourceRoot = _MAIN_SCRIPT_DIR .. "/" .. sourceDir
    local descriptor = arc_game_descriptor(ARCANE_MODULES, sourceRoot .. "/" .. name .. ".arcmodule", sourceRoot)
    local modules    = descriptor and descriptor.modules or ARC_GAME_DEFAULT_MODULES
    local hasImGui   = arc_closure_contains(ARCANE_MODULES, modules, "ImGui")
    -- Monolithic (a packaged exe): the game is a static library the package
    -- links (engine modularization s3.3); arcane_package_exe below declares the
    -- exe. Generated under Intermediate/Package/Workspace/ (ARCANE_PACKAGE_
    -- WORKSPACE), so neither layout's files overwrite the other's (gmake names
    -- every one Makefile); paths below then hang off the project root, not
    -- %{wks.location}.
    local monolithic = _OPTIONS and _OPTIONS.layout == "monolithic"
    local base = monolithic and _MAIN_SCRIPT_DIR or "%{wks.location}"
    if monolithic then
        local wks = premake.api.scope.workspace
        workspace(wks.name)
            location(_MAIN_SCRIPT_DIR .. "/" .. ARCANE_PACKAGE_WORKSPACE)
    end
    project(name)
        kind(monolithic and "StaticLib" or "SharedLib")
        language "C++"
        cppdialect "C++23"
        staticruntime "off"                         -- /MD: share one CRT heap with the engine module DLLs

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
        filter "system:macosx"
            -- The Mach-O twin: dyld never unloads an image holding thread-local
            -- variables, and header-only spdlog's thread_id() caches the id in
            -- a static thread_local in every module that logs. SPDLOG_NO_TLS
            -- asks the OS each call (the same id, uncached), so a hot-reloaded
            -- module leaves on dlclose. (A game's OWN thread_local still pins
            -- its image; the engine's test modules fail their build on one.)
            defines { "SPDLOG_NO_TLS" }
        filter {}
        -- Flat Binaries/ (config-agnostic, matching the manifest's gameModule name).
        -- Dev + the host run Debug; Binaries/ holds the config the host loads.
        targetdir "%{wks.location}/Binaries"
        objdir "%{wks.location}/Intermediate/%{cfg.buildcfg}"
        if monolithic then
            -- Never the hosts' Binaries/ slot: the package's own tree. A static
            -- library keeps its lib prefix off-Windows (the loaded-module rule
            -- above does not apply): arcane_package_exe links lib<Game>.a.
            targetdir(base .. "/Intermediate/Package/%{cfg.buildcfg}/lib")
            objdir(base .. "/Intermediate/Package/%{cfg.buildcfg}/obj/" .. name)
            filter "system:not windows"
                targetprefix "lib"
            filter {}
        end

        -- Ninja-only: beta8 emits a DUPLICATE link edge per configuration when
        -- every configuration links directly to the same Binaries/<name>
        -- output (characterized RED: ninja on this fixture emits three
        -- `build Binaries/Fixture.dll` edges, one per Debug/Release/Dist,
        -- all targeting the identical path -- arcbuild multibackend
        -- hardening plan, Task 4). Give Ninja a configuration-unique link
        -- location instead; arcbuild's Ninja backend then copies the
        -- freshly-linked module into the canonical single slot every host
        -- expects (Binaries/<name>, the manifest's gameModule) after a
        -- successful build (Source/Programs/arcbuild/src/Stage.cpp). Every other action
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
        -- (Source/Programs/arcbuild/src/ProjectLayout.cpp CleanTargets -- Binaries/ and
        -- Intermediate/<config>/), so the linked DLL/PDB cannot survive an
        -- `arcbuild clean` whose backend step soft-skipped or failed. The
        -- earlier Intermediate/Ninja/<Config>/ spelling sat OUTSIDE that
        -- contract (review F4). ProjectLayout.cpp's NinjaLinkOutput is the
        -- driver-side spelling of THIS path; the [build-generator] case pins
        -- the generated link edges against it.
        filter "action:ninja"
            targetdir "%{wks.location}/Intermediate/%{cfg.buildcfg}/Ninja/Binaries"
        filter {}

        files { base .. "/" .. sourceDir .. "/**.cpp", base .. "/" .. sourceDir .. "/**.hpp" }

        -- The game's own sources. Every engine include root and vendor header
        -- root comes from the module closure below -- nothing else.
        includedirs { base .. "/" .. sourceDir }

        -- Link + include ONLY the closure of the game's modules: each module's
        -- import lib out of the per-config SDK bin dirs (none is a project in
        -- this workspace, so premake resolves them against libdirs), its include
        -- root, and its public ThirdParty headers (glm/Astra/spdlog/Mosaic via
        -- ArcaneCore, Manifold2D via ArcanePhysics2D, ...). Not staged: the host
        -- carries the DLLs.
        -- Monolithic: headers only -- a static library that linked the module
        -- libraries would hand the package a second copy of each.
        arc_module_consumer(ARCANE_MODULES, modules, { stage = false, link = not monolithic })
        -- A descriptor's own thirdParty entries: header roots only (a game
        -- links no vendor library of its own through the SDK).
        if descriptor then
            for _, include in ipairs(descriptor.thirdPartyIncludes) do
                includedirs { ARCANE_SDK .. "/" .. include }
            end
        end

        defines {
            "GAME_BUILD_DLL",                         -- kept for an external module's own GAME_API; ARC_GAME_MODULE needs no define
            "ARC_MODULE_NAME=" .. name,               -- the cvar module (settings spec s4.3) and the ArcModule_<name> anchor
            "_CRT_SECURE_NO_WARNINGS",
            "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        }
        if monolithic then
            -- Engine headers: every ARC_<MODULE>_API empty; GameModule.hpp: no
            -- exports, the ABI pinned by static_assert, ArcGameModule_Get linked.
            defines { "ARC_MONOLITHIC" }
        end
        if hasImGui then
            defines {
                -- GameModule.hpp adopts the host's ImGui context + allocators
                -- and calls OnDrawUI only when ImGui is in the game's closure.
                "ARC_GAME_HAS_IMGUI",
                -- adopt ImGui.dll's single GImGui. __declspec is PE/COFF-only;
                -- ELF and Mach-O have no import decoration (default visibility
                -- exports it); a package links ImGui into the one image.
                "IMGUI_API=" .. ((os.target() == "windows" and not monolithic) and "__declspec(dllimport)" or ""),
            }
        end

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
            -- (Not into a monolithic static library: the package exe links them.)
            if not monolithic then
                links { "kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32", "uuid" }
            end
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

        -- Per-config: runtime + NDEBUG must match the engine DLLs' flavor (the vulkan.hpp
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

    if monolithic then arcane_package_exe(name, modules) end
end

-- ============================================================================
-- The packaged game (engine modularization s3.3; `arcbuild package`): in the
-- monolithic layout arcane_game_module also declares <Project>Package, the
-- one-exe build of the game -- targetname <Project> (the manifest's stem), so
-- it never collides with a game module of the same name. It links:
--   * the game's static library, whole-archived (its ARC_COMPONENT/ARC_SYSTEM
--     registrars must reach the link), with a forced reference to its
--     ArcModule_<Game> anchor;
--   * ArcaneRuntimeHost (the standalone host's body, built without the
--     Developer-only verification vehicle);
--   * the module closure of the host's modules plus the game's, each
--     whole-archived with its forced reference, and the vendor / system
--     libraries once (modules.lua's arc_module_package). A Developer or Editor
--     module anywhere in that closure fails generation, naming it.
-- Its main is generated here (Intermediate/Package/PackageMain.cpp): the Agility
-- SDK export, the linked module list (logged at boot) and the staged project's
-- path, handed to Arcane::RuntimeHost::Run. `arcbuild package` stages the result.
-- ============================================================================
ARC_PACKAGE_HOST_MODULES = { "ArcaneClient", "ArcaneImGui", "ArcanePhysics2D" }   -- ArcaneRuntimeHost's own needs
-- The package workspace, relative to the project root (arcbuild's
-- ProjectLayout.cpp PackageWorkspaceDir spells the same path).
ARCANE_PACKAGE_WORKSPACE = "Intermediate/Package/Workspace"

function arcane_package_exe(game, gameModules)
    local _, manifest = arcane_project_manifest()
    local projectName = path.getbasename(manifest)
    local root = _MAIN_SCRIPT_DIR
    local names = {}
    for _, n in ipairs(ARC_PACKAGE_HOST_MODULES) do names[#names+1] = n end
    for _, n in ipairs(gameModules) do names[#names+1] = n end
    -- Unreal's package layout (user decision 2026-10-12): the game binary and
    -- everything it loads exe-relative (vendor runtime files, the engine's
    -- data/) live in <Project>/Binaries/<Platform>/ beside the project's own
    -- folder; the package root holds only a launcher -- <Project>.sh, or a stub
    -- <Project>.exe on Windows (arcane_package_launcher) -- so the bare game
    -- binary never collides with the <Project>/ folder (it did on Linux/macOS,
    -- where the exe has no extension) and the server / crash reporter binaries
    -- have a home beside it later.
    local platformDir = ({ windows = "Win64", linux = "Linux", macosx = "Mac" })[os.target()] or os.target()
    local packageBin = projectName .. "/Binaries/" .. platformDir .. "/"
    local launcherPath = root .. "/Intermediate/Package/" .. projectName .. ".sh"

    project(projectName .. "Package")
        targetname(projectName)
        language "C++"
        cppdialect "C++23"
        staticruntime "off"
        filter "configurations:Debug"
            kind "ConsoleApp"                    -- a console for the log in Debug
        filter "configurations:not Debug"
            kind "WindowedApp"
        filter { "system:windows", "configurations:not Debug" }
            entrypoint "mainCRTStartup"          -- WindowedApp with a plain main()
        filter "system:macosx"
            architecture(arcane_sdk_mac_architecture())
        filter {}
        targetdir(root .. "/Intermediate/Package/%{cfg.buildcfg}/bin")
        objdir(root .. "/Intermediate/Package/%{cfg.buildcfg}/obj/" .. projectName .. "Package")
        dependson { game }

        -- The package loads its shared vendor libraries (SDL3 off-Windows, DXC,
        -- the macOS Vulkan loader) from beside itself: the exe's own directory
        -- goes FIRST on the rpath, ahead of any build-machine path a prebuilt
        -- module adds. Dead-code stripping: /OPT:REF,ICF must be explicit under
        -- /DEBUG (which defaults to NOREF); --gc-sections / -dead_strip elsewhere.
        filter "system:linux"
            -- '$$ORIGIN': make turns $$ into $, the quotes stop the shell
            -- (a bare $ORIGIN reached the ELF as "RIGIN" -- make expanded $O).
            linkoptions { "-Wl,-rpath,'$$ORIGIN'", "-Wl,--gc-sections" }
        filter "system:macosx"
            linkoptions { "-Wl,-rpath,@executable_path", "-Wl,-dead_strip" }
        filter { "system:windows", "configurations:not Debug" }
            linkoptions { "/OPT:REF", "/OPT:ICF" }
        filter {}

        local closure = arc_module_package(ARCANE_MODULES, names, ARCANE_SDK)

        -- The game, whole-archived, plus its anchor; then the host library.
        local static_ext = os.target() == "windows" and ".lib" or ".a"
        local static_prefix = os.target() == "windows" and "" or "lib"
        local gameLib = root .. "/Intermediate/Package/%{cfg.buildcfg}/lib/" .. static_prefix .. game .. static_ext
        if os.target() == "windows" then
            linkoptions { "/WHOLEARCHIVE:" .. gameLib }
        elseif os.target() == "macosx" then
            linkoptions { "-Wl,-force_load," .. gameLib }
        else
            -- One string (premake de-duplicates linkoptions; see arc_module_package).
            linkoptions { "-Wl,--whole-archive " .. gameLib .. " -Wl,--no-whole-archive" }
        end
        arc_module_forced_refs({ game })
        links { "ArcaneRuntimeHost" }
        libdirs { ARCANE_SDK .. "/Binaries/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-mono/ArcaneRuntimeHost" }
        includedirs {
            ARCANE_SDK .. "/Source/Programs/ArcaneRuntimeHost/src",
            ARCANE_TP .. "/glm", ARCANE_TP .. "/Astra/include", ARCANE_TP .. "/enkiTS/src",
            ARCANE_TP .. "/Manifold2D/include", ARCANE_TP .. "/Mosaic/include",
        }

        -- The generated main, rewritten only when its text changes (no rebuild churn).
        local quoted = {}
        for _, n in ipairs(closure) do quoted[#quoted+1] = '"' .. n .. '"' end
        quoted[#quoted+1] = '"' .. game .. '"'
        local main = table.concat({
            "// GENERATED by build/arcane.lua (arcane_package_exe) -- do not edit.",
            "// The packaged " .. projectName .. ": engine modularization s3.3.",
            "#include <Arcane/Render/AgilitySdk.hpp>",
            "#include <Arcane/RuntimeHost/RuntimeHost.hpp>",
            "",
            "// Agility SDK handshake: must be exported by the exe itself.",
            "ARC_AGILITY_SDK_EXPORTS();",
            "",
            "static const char* const kModules[] = { " .. table.concat(quoted, ", ") .. " };",
            "",
            "int main(int argc, char** argv)",
            "{",
            "    Arcane::RuntimeHost::PackageInfo package;",
            '    package.projectRelPath = "../../' .. path.getname(manifest) .. '";',
            "    package.modules        = kModules;",
            "    package.moduleCount    = static_cast<int>(sizeof(kModules) / sizeof(kModules[0]));",
            '    package.appName        = "' .. projectName .. '";',
            "    return Arcane::RuntimeHost::Run(argc, argv, &package);",
            "}",
            "",
        }, "\n")
        local mainPath = root .. "/Intermediate/Package/PackageMain.cpp"
        if io.readfile(mainPath) ~= main then
            os.mkdir(root .. "/Intermediate/Package")
            local f = assert(io.open(mainPath, "wb"), "arcane_package_exe: cannot write " .. mainPath)
            f:write(main)
            f:close()
        end
        files { mainPath }

        -- The POSIX launcher (<Project>.sh at the package root): runs this
        -- platform's game binary with the same arguments. Executable at the
        -- source, so StagePackage's copy (permissions kept) is executable too.
        if os.target() ~= "windows" then
            local launcher = table.concat({
                "#!/bin/sh",
                "# GENERATED by build/arcane.lua (arcane_package_exe) -- the " .. projectName .. " package launcher:",
                "# runs the game binary for this platform with the same arguments.",
                'exec "$(dirname "$0")/' .. packageBin .. projectName .. '" "$@"',
                "",
            }, "\n")
            if io.readfile(launcherPath) ~= launcher then
                local f = assert(io.open(launcherPath, "wb"), "arcane_package_exe: cannot write " .. launcherPath)
                f:write(launcher)
                f:close()
            end
            os.chmod(launcherPath, "755")
        end

        -- The stage list `arcbuild package` copies into Packages/<Project>-
        -- <platform>-<config>/ (Source/Programs/arcbuild/src/Stage.hpp,
        -- StagePackage): the exe, the platform's vendor runtime files (the same
        -- set premake5.lua's arcane_vendor_runtime stages beside the engine's
        -- programs, plus each prebuilt vendor module's runtime file -- SDL3.dll),
        -- the engine data the host reads exe-relative, and the project itself
        -- in <Project>/ (content copied raw; D15 replaces that with a cook).
        local exe = (os.target() == "windows") and (projectName .. ".exe") or projectName
        local projectRoot = path.getdirectory(manifest)
        local function stage_entries(cfg)
            local e = {}
            -- Every binary-side entry (the exe, the vendor runtime files, the
            -- engine data the host reads exe-relative) lands in BIN.
            local function add(from, to, kind, optional)
                e[#e+1] = { from = from, to = to, kind = kind or "file", optional = optional or false }
            end
            local function addbin(from, to, kind, optional) add(from, packageBin .. to, kind, optional) end
            addbin(root .. "/Intermediate/Package/" .. cfg .. "/bin/" .. exe, exe)
            if os.target() == "windows" then
                addbin(root .. "/Intermediate/Package/" .. cfg .. "/bin/" .. projectName .. ".pdb", projectName .. ".pdb", "file", true)
                add(root .. "/Intermediate/Package/" .. cfg .. "/launcher/" .. exe, exe)
            else
                add(launcherPath, projectName .. ".sh")
            end
            for _, n in ipairs(closure) do
                local mod = ARCANE_MODULES.byName[n]
                local pre = mod.prebuilt and arc_module_prebuilt(mod)
                if pre then
                    local runtime = (cfg == "Debug" and pre.debugRuntime) or pre.runtime
                    for _, f in ipairs(runtime) do
                        addbin(f, path.getname(f))
                    end
                    -- A system shared library (SDL3 on Linux/macOS: no runtime
                    -- list in the descriptor) ships beside the exe too, found in
                    -- its libdir -- the package must not need the build
                    -- machine's install. Every soname spelling (libSDL3.so.0 is
                    -- what the loader asks for).
                    if #runtime == 0 and pre.link and pre.libdir and os.target() ~= "windows" then
                        local pattern = os.target() == "macosx" and ("lib" .. pre.link .. "*.dylib")
                                                                 or ("lib" .. pre.link .. ".so*")
                        for _, f in ipairs(os.matchfiles(pre.libdir .. "/" .. pattern)) do
                            addbin(f, path.getname(f))
                        end
                    end
                end
            end
            if os.target() == "windows" then
                addbin(ARCANE_TP .. "/tools/dxc/dxcompiler.dll", "dxcompiler.dll")
                addbin(ARCANE_TP .. "/tools/dxc/dxil.dll", "dxil.dll")
                addbin(ARCANE_TP .. "/AgilitySDK/x64/D3D12Core.dll", "D3D12/D3D12Core.dll")
                if cfg == "Debug" then
                    addbin(ARCANE_TP .. "/AgilitySDK/x64/d3d12SDKLayers.dll", "D3D12/d3d12SDKLayers.dll")
                end
            elseif os.target() == "macosx" then
                addbin(ARCANE_TP .. "/tools/dxc-macos/lib/libdxcompiler.dylib", "libdxcompiler.dylib", "file", true)
                -- The Vulkan loader + MoltenVK (scripts/fetch-vulkan-sdk-macos.sh
                -- links the SDK's lib/ here) and MoltenVK's ICD manifest.
                addbin(ARCANE_TP .. "/tools/dxc-macos/lib/libvulkan.1.dylib", "libvulkan.1.dylib", "file", true)
                addbin(ARCANE_TP .. "/tools/dxc-macos/lib/libMoltenVK.dylib", "libMoltenVK.dylib", "file", true)
                addbin(ARCANE_TP .. "/tools/dxc-macos/share/vulkan/icd.d/MoltenVK_icd.json",
                    "vulkan/icd.d/MoltenVK_icd.json", "file", true)
            else
                addbin(ARCANE_TP .. "/tools/dxc-linux/lib/libdxcompiler.so", "libdxcompiler.so", "file", true)
                addbin(ARCANE_TP .. "/tools/dxc-linux/lib/libdxil.so", "libdxil.so", "file", true)
            end
            addbin(ARCANE_SDK .. "/data/shaders/generated", "data/shaders", "dir")
            addbin(ARCANE_SDK .. "/data/shaders/materials", "data/shaders/materials", "dir")
            addbin(ARCANE_SDK .. "/data/EngineConfig", "data/EngineConfig", "dir")
            addbin(ARCANE_SDK .. "/data/images/arcane_logo.png", "data/images/arcane_logo.png")
            add(manifest, projectName .. "/" .. path.getname(manifest))
            add(projectRoot .. "/Content", projectName .. "/Content", "dir", true)
            add(projectRoot .. "/Config", projectName .. "/Config", "dir", true)
            add(projectRoot .. "/Intermediate/Artifacts", projectName .. "/Intermediate/Artifacts", "dir", true)
            return e
        end
        local stage = { package = projectName, configs = {} }
        for _, cfg in ipairs({ "Debug", "Release", "Dist" }) do stage.configs[cfg] = stage_entries(cfg) end
        local stageText = json.encode(stage)
        local stagePath = root .. "/Intermediate/Package/stage.json"
        if io.readfile(stagePath) ~= stageText then
            local f = assert(io.open(stagePath, "wb"), "arcane_package_exe: cannot write " .. stagePath)
            f:write(stageText)
            f:close()
        end

        defines {
            "ARC_MONOLITHIC",
            "ARC_MODULE_NAME=" .. projectName,
            "IMGUI_API=",
            "_CRT_SECURE_NO_WARNINGS",
            "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        }
        filter "action:vs* or toolset:msc"
            disablewarnings { "4251" }
        filter { "system:windows", "action:vs* or toolset:msc" }
            buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        filter { "system:linux or system:macosx", "architecture:x86_64" }
            buildoptions { "-mavx2", "-mfma" }
        filter "system:windows"
            systemversion "latest"
            links { "kernel32", "user32", "gdi32", "advapi32", "shell32", "ole32", "oleaut32", "uuid" }
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

    if os.target() == "windows" then arcane_package_launcher(projectName) end
end

-- The Windows package launcher (Unreal's bootstrap shape): <Pkg>/<Project>.exe,
-- a stub that runs <Project>/Binaries/Win64/<Project>.exe with the same
-- arguments and standard handles and returns its exit code
-- (Source/Programs/ArcanePackageLauncher/src/Launcher.cpp). Console in Debug
-- like the game, windowed otherwise.
function arcane_package_launcher(projectName)
    local root = _MAIN_SCRIPT_DIR
    project(projectName .. "Launcher")
        targetname(projectName)
        language "C++"
        cppdialect "C++23"
        staticruntime "off"
        filter "configurations:Debug"
            kind "ConsoleApp"
        filter "configurations:not Debug"
            kind "WindowedApp"
        filter {}
        entrypoint "wmainCRTStartup"
        targetdir(root .. "/Intermediate/Package/%{cfg.buildcfg}/launcher")
        objdir(root .. "/Intermediate/Package/%{cfg.buildcfg}/obj/" .. projectName .. "Launcher")
        files { ARCANE_SDK .. "/Source/Programs/ArcanePackageLauncher/src/Launcher.cpp" }
        defines { "UNICODE", "_UNICODE" }
        systemversion "latest"
        links { "kernel32", "user32" }
        filter "configurations:Debug"
            runtime "Debug"
            symbols "on"
        filter "configurations:not Debug"
            runtime "Release"
            optimize "speed"
        filter {}
end

-- ============================================================================
-- Non-game consumers (engine modularization s4; the recipe began as the
-- Core-DLL split's Core-only consumer, spec docs/specs/2026-09-15-core-dll-
-- split-design.md s1.2 / s8, Plan 3). An external exe or static lib that links
-- engine modules WITHOUT being a game -- Aphelyon's services, their Common lib
-- and their test exes -- names the modules it uses:
--
--   arcane_module_consumer { "ArcaneCore", "ArcaneNet", "ArcaneCrypto" }
--
-- It configures the CURRENT project: call it INSIDE a `project` block, in place
-- of `staticruntime`. It composes into whatever shape the project has
-- (ConsoleApp, StaticLib, a factory function) rather than declaring one,
-- because consumers do not share a shape the way game modules do. It adds:
--   * staticruntime "off" -- /MD, one CRT heap across the DLL boundary (objects
--     allocated in a module DLL are freed by the caller);
--   * the closure of `names`: each module's include root + import lib + libdir,
--     plus the SDK-PRIVATE header-only deps a module header closure can reach
--     (glm, Astra, enkiTS, Manifold2D, Mosaic). spdlog and nlohmann are
--     deliberately NOT added: a consumer vendors its own copies (Aphelyon does,
--     at the engine's versions), and two copies of a header-only library on one
--     include path is a version split waiting to happen -- a consumer without
--     them gets a clear missing-header error;
--   * ARC_MODULE_NAME=<project> (the cvar module its ARC_CVAR declarations name);
--   * the engine's flavor contract, the same lines arcane_game_module carries:
--     /utf-8, /arch:AVX2 (the module DLLs are built AVX2 workspace-wide, so the
--     process already requires it -- matching keeps inline header codegen
--     identical across the boundary), and per-config runtime + ARC_BUILD_DEBUG /
--     ARC_BUILD_RELEASE+NDEBUG / ARC_BUILD_DIST+NDEBUG so inline header layouts
--     under #ifndef NDEBUG agree with the DLLs'.
-- It ends with `filter {}` so the caller's following lines are unfiltered.
--
-- arcane_stage_modules(names) adds the postbuild copy of those modules' DLL
-- closure beside an exe's output (the dev bin layout). StaticLib consumers do
-- not call it.
-- ============================================================================
function arcane_module_consumer(names)
    if type(names) ~= "table" or #names == 0 then
        error("arcane_module_consumer: pass the module names, e.g. arcane_module_consumer { \"ArcaneCore\" }", 2)
    end
    staticruntime "off"
    filter "system:macosx"
        architecture(arcane_sdk_mac_architecture())
    filter {}

    -- C4251 ("needs to have dll-interface"): disabled for every consumer, the
    -- same ruling the engine workspace makes for itself (premake5.lua, the
    -- workspace-level disablewarnings). The warning guards against a
    -- DLL/client CRT-layout mismatch; this helper's contract -- /MD, one
    -- toolset, one heap shared with the module DLLs -- makes that mismatch
    -- structurally impossible, and the host's CRT-flavor gate refuses a
    -- module that breaks it at load. Every exported engine class that holds
    -- an STL member would otherwise warn at every consumer call site.
    -- MSVC-scoped for the same reason as arcane_game_module's (an MSVC
    -- warning number is meaningless to GCC).
    filter "action:vs* or toolset:msc"
        disablewarnings { "4251" }
    filter {}

    includedirs {
        ARCANE_TP .. "/glm",
        ARCANE_TP .. "/Astra/include",
        ARCANE_TP .. "/enkiTS/src",
        ARCANE_TP .. "/Manifold2D/include",
        ARCANE_TP .. "/Mosaic/include",
    }

    -- The cvar module a consumer's ARC_CVAR/ARC_COMMAND declarations name
    -- (Arcane/Config/CVarModule.hpp; settings spec s4.3): its own project.
    defines { "ARC_MODULE_NAME=%{prj.name}" }

    -- Module roots + import libs only: vendors = false keeps spdlog/nlohmann off
    -- the include path (see above); arcane_stage_modules() does the staging.
    arc_module_consumer(ARCANE_MODULES, names, { stage = false, vendors = false })

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

function arcane_stage_modules(names)
    if type(names) ~= "table" or #names == 0 then
        error("arcane_stage_modules: pass the module names, e.g. arcane_stage_modules { \"ArcaneCore\" }", 2)
    end
    arc_module_stage(ARCANE_MODULES, names)
end
