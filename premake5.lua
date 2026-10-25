-- Arcane Engine Workspace
-- premake5 for Visual Studio 2026 (generates Arcane.slnx)
--
-- CRT rule (architecture spec 2026-06-11, decision 3): the entire Arcane
-- workspace builds /MD (dynamic CRT) -- memory crosses the ArcaneClient.dll /
-- Game.dll boundary, so all modules must share one heap. Server/ and
-- Tools/ keep their static-runtime conventions, unaffected.

-- vcpkg required for SDL3 (platform layer; deep build system -> vcpkg per
-- the repo rule). Overlay triplet x64-windows-static-md pins v143 + /MD and
-- builds SDL3 (only) as a DLL: SDL3 is a prebuilt vendor MODULE
-- (ThirdParty/SDL3/SDL3.arcmodule) -- one SDL3.dll per process, staged beside
-- every program whose module closure reaches it. The discovery below feeds
-- that descriptor's @SDL3_INCLUDE_DIR@ / @SDL3_PREFIX@ / @SDL3_LIB_DIR@.
-- Linux/macOS (Linux port, 2026-10-05): SDL3 is NOT taken from vcpkg there --
-- it comes from the system (pkg-config `sdl3`, e.g. a from-source install in
-- /usr/local; Ubuntu 24.04 does not package SDL3). VCPKG_ROOT is therefore a
-- Windows-target requirement only, and VCPKG_INSTALLED_MD is nil elsewhere.
-- The discovery itself is shared with SDK consumers (a package links SDL3 too):
-- arc_discover_sdl3() in build/modules.lua.
include "build/modules.lua"
arc_discover_sdl3()

-- ---- Per-target file-name spellings (Linux port, 2026-10-05) ---------------
-- Every postbuild line below names a built binary. A Windows target keeps its
-- exact historical spelling (Foo.dll / Foo.exe / {COPYDIR}); an ELF target
-- spells the same artifact libFoo.so / Foo / a contents-copy. Premake's
-- POSIX {COPYDIR} is a bare `cp -rf src dst`, which NESTS src inside dst on
-- every build after the first (xcopy copies contents) -- hence arcane_copydir.
ARCANE_WINDOWS = (os.target() == "windows")
-- A workspace SharedLib that consumers LINK (ArcaneCore, ArcaneClient): ELF
-- keeps premake's lib prefix so -lArcaneCore resolves.
function arcane_shlib(name)
    if ARCANE_WINDOWS then return name .. ".dll" end
    if os.target() == "macosx" then return "lib" .. name .. ".dylib" end
    return "lib" .. name .. ".so"
end
-- A LOADED module (game module, test plugin): no lib prefix anywhere, the
-- platform's shared-library extension (Arcane::Platform::kSharedLibraryExtension).
function arcane_module(name)
    if ARCANE_WINDOWS then return name .. ".dll" end
    if os.target() == "macosx" then return name .. ".dylib" end
    return name .. ".so"
end
function arcane_exe(name)
    return ARCANE_WINDOWS and (name .. ".exe") or name
end
function arcane_copydir(src, dst)
    if ARCANE_WINDOWS then return '{COPYDIR} "' .. src .. '" "' .. dst .. '"' end
    return 'mkdir -p "' .. dst .. '" && cp -rf "' .. src .. '/." "' .. dst .. '"'
end
-- A postbuild line that only means something on a Windows target (the crash
-- reporter, the vendored dxc DLLs, the D3D12 Agility SDK). Returns false
-- elsewhere; arcane_cmds drops the falses, so a Windows target's command list
-- stays byte-identical, in its original order.
function arcane_win(cmd)
    if ARCANE_WINDOWS then return cmd end
    return false
end
function arcane_posix(cmd)
    if ARCANE_WINDOWS then return false end
    return cmd
end
-- The runtime compile service's DXC library, staged beside a host/test exe
-- when present (never vendored off-Windows). Linux: libdxcompiler.so +
-- libdxil.so from scripts/fetch-dxc-linux.sh. macOS: libdxcompiler.dylib
-- from the Vulkan SDK (scripts/fetch-vulkan-sdk-macos.sh links it into
-- ThirdParty/tools/dxc-macos/); no libdxil -- a Mac emits SPIR-V only.
function arcane_stage_dxc()
    if ARCANE_WINDOWS then return false end
    if os.target() == "macosx" then
        return 'if [ -f "%{wks.location}/ThirdParty/tools/dxc-macos/lib/libdxcompiler.dylib" ]; then cp -fL "%{wks.location}/ThirdParty/tools/dxc-macos/lib/libdxcompiler.dylib" "%{cfg.buildtarget.directory}/"; fi'
    end
    return 'if [ -f "%{wks.location}/ThirdParty/tools/dxc-linux/lib/libdxcompiler.so" ]; then cp -f "%{wks.location}/ThirdParty/tools/dxc-linux/lib/libdxcompiler.so" "%{wks.location}/ThirdParty/tools/dxc-linux/lib/libdxil.so" "%{cfg.buildtarget.directory}/"; fi'
end
function arcane_cmds(list)
    local out = {}
    for _, c in ipairs(list) do
        if c then table.insert(out, c) end
    end
    return out
end

-- The vendor runtime files a GPU-capable program stages beside itself, in ONE
-- place (P3). Engine module DLLs and the prebuilt vendor modules (SDL3) are
-- NOT here: arc_module_stage (build/modules.lua) stages a program's module
-- closure. MoltenVK joins this list with the macOS package (P5).
function arcane_vendor_runtime()
    return arcane_cmds {
        -- Vendored dxc trio (minus dxc.exe): the runtime compile service
        -- (ShaderCompiler) LoadLibrary's these from the exe directory.
        arcane_win('{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxcompiler.dll" "%{cfg.buildtarget.directory}/dxcompiler.dll"'),
        arcane_win('{COPYFILE} "%{wks.location}/ThirdParty/tools/dxc/dxil.dll" "%{cfg.buildtarget.directory}/dxil.dll"'),
        -- Linux/macOS: the same service dlopen()s libdxcompiler (+ libdxil on
        -- Linux), staged when present (arcane_stage_dxc above).
        arcane_stage_dxc(),
        -- Agility SDK redistributable (NRI Phase 1 Task 3): the D3D12 loader
        -- reads the exe's exported D3D12SDKPath (".\D3D12\", see each main.cpp /
        -- test_main.cpp) and looks there for D3D12Core.dll to unlock enhanced
        -- barriers / ID3D12Device10+ in NRI's D3D12 backend.
        arcane_win('{MKDIR} "%{cfg.buildtarget.directory}/D3D12"'),
        arcane_win('{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/D3D12Core.dll" "%{cfg.buildtarget.directory}/D3D12/D3D12Core.dll"'),
        arcane_win('{COPYFILE} "%{wks.location}/ThirdParty/AgilitySDK/x64/d3d12SDKLayers.dll" "%{cfg.buildtarget.directory}/D3D12/d3d12SDKLayers.dll"'),
    }
end

-- imgui's IMGUI_API spelling per target. __declspec is a PE/COFF-only
-- keyword: GCC rejects it outright on ELF and Clang needs -fdeclspec. ELF
-- exports every default-visibility symbol (no import decoration exists), so
-- the empty spelling is exact there while the workspace keeps default
-- visibility (no -fvisibility=hidden). The monolithic layout links ImGui
-- statically into one image: empty everywhere.
if ARCANE_WINDOWS and _OPTIONS.layout ~= "monolithic" then
    ARCANE_IMGUI_EXPORT = "__declspec(dllexport)"
    ARCANE_IMGUI_IMPORT = "__declspec(dllimport)"
else
    ARCANE_IMGUI_EXPORT = ""
    ARCANE_IMGUI_IMPORT = ""
end

-- macOS port (2026-10-07): a Mac target builds for the HOST's architecture --
-- arm64 on Apple Silicon (the supported Mac), x86_64 on an Intel Mac --
-- overridable with ARCANE_MAC_ARCH=arm64|x86_64 (e.g. to cross-generate from
-- Linux with --os=macosx). Every other target keeps "x64". premake reports
-- ARM64 as %{cfg.architecture} = "AARCH64", so the Mac bin dirs are
-- Binaries/<Config>-macosx-AARCH64-md (scripts/arcane-bin-dir.sh spells it once).
-- build/arcane.lua carries the same rule so a game module matches its engine.
function arcane_mac_architecture()
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
ARCANE_ARCH = (os.target() == "macosx") and arcane_mac_architecture() or "x64"

newoption {
    trigger = "layout",
    value = "LAYOUT",
    description = "Engine link layout",
    allowed = { { "modular" }, { "monolithic" } },
    default = "modular",
}

-- The monolithic layout (engine modularization s2.5/s3.3): every Runtime and
-- Client module as a static library, plus ArcaneRuntimeHost -- the pieces a
-- packaged game links (`arcbuild package`). Generated under Intermediate/Mono/
-- (ARC_MONOLITHIC_PROJECTS below), so neither layout overwrites the other;
-- outputs land in Binaries/<cfg>-<sys>-<arch>-mono/. Programs, tests and fixtures
-- are modular only.
ARC_MONOLITHIC_LAYOUT = (_OPTIONS.layout == "monolithic")
-- Where the monolithic layout's workspace + project files go (gitignored): a
-- separate tree, because gmake names every project file Makefile and the
-- workspace's Makefile sits at the root -- generated in place they would
-- overwrite the modular ones. Sources are located by module root (modules.lua).
ARC_MONOLITHIC_PROJECTS = ARC_MONOLITHIC_LAYOUT and "Intermediate/Mono" or nil
-- The repo root as the engine's paths spell it: %{wks.location} in the modular
-- layout (its generated files sit at the root), the absolute root in the
-- monolithic one (whose workspace lives in Intermediate/Mono).
ARC_ROOT = ARC_MONOLITHIC_LAYOUT and _MAIN_SCRIPT_DIR or "%{wks.location}"

workspace "Arcane"
    architecture(ARCANE_ARCH)
    startproject "ArcaneTests"
    configurations { "Debug", "Release", "Dist" }
    if ARC_MONOLITHIC_LAYOUT then
        defines { "ARC_MONOLITHIC" }
        location(ARC_MONOLITHIC_PROJECTS)
    end
    multiprocessorcompile "On"

    -- MSVC: /utf-8 ensures source + execution charsets are UTF-8.
    -- Required by fmt 11+ (bundled in spdlog 1.17).
    filter "system:windows"
        buildoptions { "/utf-8", "/arch:AVX2" }   -- AVX2 is the x86 min-spec for the engine (Arcane::Simd)
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }         -- gcc/clang x64 parity; ARM port supplies NEON flags later
    -- ELF: every static lib that links INTO a shared object (enkiTS +
    -- Manifold2D into libArcaneCore.so, NRI/imgui/freetype/msdfgen into
    -- libArcaneClient.so) must be position-independent, or ld refuses the
    -- relocation (R_X86_64_TPOFF32 against enkiTS's thread_local, first).
    -- -ffp-contract=off: the determinism rule. MSVC's /fp:strict never fuses
    -- a*b+c; GCC/Clang with -mfma do by default (premake's floatingpoint
    -- "Strict" gives GCC only the x87-only -ffloat-store and Clang nothing),
    -- which would diverge the physics parity tests and goldens.
    filter "system:not windows"
        pic "On"
        buildoptions { "-ffp-contract=off", "-fno-fast-math" }
        -- The crash path walks a FOREIGN thread's stack (a faulting thread
        -- parked in its signal handler, a hung main thread) by its frame-
        -- pointer chain (Base/PortableStack.cpp): ELF has no PE-style
        -- RtlVirtualUnwind over an arbitrary context, and a DWARF unwinder
        -- for one is a libunwind-sized dependency. Kept in every config, as
        -- Ubuntu 24.04 and Fedora now do distro-wide; the cost is one
        -- register, low single-digit percent at worst.
        buildoptions { "-fno-omit-frame-pointer", "-mno-omit-leaf-frame-pointer" }
        -- $ORIGIN FIRST: the staged layout (libArcaneCore.so/libArcaneClient.so
        -- beside the exe) must win, exactly as the PE loader searches the exe
        -- directory first -- otherwise the dev-tree Binaries/<cfg>/ArcaneCore copy
        -- loads and the injected-module census calls the engine foreign.
        -- runpathdirs (unlike linkoptions) is emitted AHEAD of the
        -- ../ArcaneCore-style entries premake adds for linked siblings, which
        -- stay as the fallback for an unstaged tool.
        runpathdirs { "%{cfg.targetdir}" }
    filter {}

    -- C4251 ("needs to have dll-interface"): disabled workspace-wide, deliberately.
    -- Every hit is an ARC_API class holding STL members. The warning exists for
    -- DLL/client CRT-layout mismatches; this workspace's foundational rule is /MD
    -- everywhere + one toolset + one shared heap (memory crosses the
    -- ArcaneClient.dll/Game.dll boundary by design), so the mismatch it warns about is
    -- structurally impossible here. The "real" fixes (pimpl-everything, function-
    -- only exports) buy nothing under that contract; Unreal ships with 4251
    -- disabled engine-wide for the same reason.
    -- MSVC-numbered, so MSVC-only: premake passes it through verbatim to
    -- GCC/Clang as -Wno-4251 (Clang: -Wunknown-warning-option on every TU).
    filter "system:windows"
        disablewarnings { "4251" }
    filter {}

    -- Each module's own name, for the cvar registry's module capture (settings
    -- spec s4.3/s4.4, O1): Arcane/Config/CVarModule.hpp stringizes it, so an
    -- ARC_CVAR/ARC_COMMAND outside a plugin host's CVarModuleScope names the
    -- module that declared it. Workspace scope, so every project -- engine,
    -- hosts, tests, test plugins -- gets its own project name.
    defines { "ARC_MODULE_NAME=%{prj.name}" }

    -- "-md" suffix keeps ThirdParty wrapper outputs (each dep builds into
    -- Binaries/ under its own dir) separate from the static-CRT flavors the
    -- Server workspace builds from the same wrapper scripts.
    outputdir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-" ..
        (_OPTIONS.layout == "monolithic" and "mono" or "md")

    -- Shared ThirdParty wrappers read these; their defaults preserve the
    -- Server/Tools behavior (static CRT, vcxproj in the dep root).
    THIRDPARTY_STATICRUNTIME    = "off"
    THIRDPARTY_PROJECT_LOCATION = ARC_MONOLITHIC_LAYOUT and "ide-mono" or "ide-md"
    -- imgui wrapper: SDL3 backend includes come from the same vcpkg -md install
    -- used by the rest of the workspace (identical path to IncludeDir["SDL3"]).
    THIRDPARTY_SDL3_INCLUDE     = SDL3_INCLUDE_DIR
    -- imgui lives inside ArcaneClient.dll (ImGuiLayer + the frame graph's ImGuiNriNode).
    -- Export it so GImGui and the sdl3 backend symbols live in ONE module:
    -- the imgui static lib builds with dllexport, the DLL's own TUs match,
    -- and consumers (ArcaneTests, ArcaneRuntime) import. Without this each module
    -- keeps its own null GImGui and ShowDemoWindow() from the test exe asserts.
    THIRDPARTY_IMGUI_API        = ARCANE_IMGUI_EXPORT
    -- imgui-node-editor links into ArcaneEditor.exe, which IMPORTS imgui from
    -- ArcaneClient.dll -- its ImGui calls must be dllimport (see the wrapper).
    THIRDPARTY_NODE_EDITOR_IMGUI_API = ARCANE_IMGUI_IMPORT

    IncludeDir = {}
    IncludeDir["nlohmann"]         = ARC_ROOT .. "/ThirdParty/nlohmann"
    IncludeDir["picosha2"]         = ARC_ROOT .. "/ThirdParty/picosha2"
    IncludeDir["spdlog"]           = ARC_ROOT .. "/ThirdParty/spdlog/include"
    IncludeDir["Catch2"]           = ARC_ROOT .. "/ThirdParty/Catch2/src"
    IncludeDir["rapidcheck"]       = ARC_ROOT .. "/ThirdParty/rapidcheck/include"
    IncludeDir["rapidcheck_catch"] = ARC_ROOT .. "/ThirdParty/rapidcheck/extras/catch/include"
    IncludeDir["glm"]              = ARC_ROOT .. "/ThirdParty/glm"
    IncludeDir["stb"]              = ARC_ROOT .. "/ThirdParty/stb"
    IncludeDir["miniaudio"]        = ARC_ROOT .. "/ThirdParty/miniaudio"
    IncludeDir["Astra"]            = ARC_ROOT .. "/ThirdParty/Astra/include"
    IncludeDir["enkiTS"]           = ARC_ROOT .. "/ThirdParty/enkiTS/src"
    IncludeDir["tracy"]            = ARC_ROOT .. "/ThirdParty/tracy/public"
    IncludeDir["freetype"]         = ARC_ROOT .. "/ThirdParty/freetype/include"
    IncludeDir["msdfgen"]          = ARC_ROOT .. "/ThirdParty/msdfgen"
    IncludeDir["NRI"]              = ARC_ROOT .. "/ThirdParty/NRI/Include"
    IncludeDir["VulkanHeaders"]    = ARC_ROOT .. "/ThirdParty/Vulkan-Headers/include"
    IncludeDir["DirectXHeaders"]   = ARC_ROOT .. "/ThirdParty/DirectX-Headers/include"
    IncludeDir["VMA"]              = ARC_ROOT .. "/ThirdParty/VMA/include"
    IncludeDir["D3D12MA"]          = ARC_ROOT .. "/ThirdParty/D3D12MA/include"
    IncludeDir["SDL3"]             = SDL3_INCLUDE_DIR
    IncludeDir["imgui"]            = ARC_ROOT .. "/ThirdParty/imgui"
    IncludeDir["imguinodeeditor"]  = ARC_ROOT .. "/ThirdParty/imgui-node-editor"
    IncludeDir["Manifold2D"]       = ARC_ROOT .. "/ThirdParty/Manifold2D/include"
    IncludeDir["Mosaic"]           = ARC_ROOT .. "/ThirdParty/Mosaic/include"
    IncludeDir["bc7enc_rdo"]       = ARC_ROOT .. "/ThirdParty/bc7enc_rdo"
    IncludeDir["cgltf"]            = ARC_ROOT .. "/ThirdParty/cgltf"
    IncludeDir["meshoptimizer"]    = ARC_ROOT .. "/ThirdParty/meshoptimizer/src"

    local engine_modules = arc_modules_load {
        "Source", "ThirdParty/imgui/ImGui.arcmodule", "ThirdParty/imgui-node-editor/ImGuiNodeEditor.arcmodule",
        "ThirdParty/SDL3/SDL3.arcmodule",
    }
    -- ARC_GAME_DEFAULT_MODULES (a descriptor-less game's set) and the game
    -- descriptor reader (arc_game_descriptor) live in build/modules.lua, shared
    -- with build/arcane.lua; the game-type fixtures below read their own
    -- .arcmodule through it (game_fixture_modules).

-- Vendor static libraries share the "ThirdParty" folder with the vendor modules.
group "ThirdParty"
    -- Test and asset-pipeline vendors are modular-only (no package links them).
    if not ARC_MONOLITHIC_LAYOUT then
        include "ThirdParty/Catch2"
        include "ThirdParty/rapidcheck"
    end
    include "ThirdParty/enkiTS"
    include "ThirdParty/tracy"
    include "ThirdParty/freetype"
    include "ThirdParty/msdfgen"
    include "ThirdParty/NRI"
    include "ThirdParty/Manifold2D"
    if not ARC_MONOLITHIC_LAYOUT then
        include "ThirdParty/bc7enc_rdo"
        include "ThirdParty/meshoptimizer"
    end
group ""


-- Descriptor-driven module projects. Every build-only exception lives here.
local function module_defaults()
    floatingpoint "Strict"
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }
    filter "system:not windows"
        if not ARC_MONOLITHIC_LAYOUT then links { "dl", "pthread" } end   -- a package links them once
        inlinesvisibility "Hidden"
    filter "system:linux"
        linkoptions { "-Wl,-z,defs" }
    -- A package links with --gc-sections / -dead_strip: per-function sections
    -- make the stripping function-granular (the MSVC twin is /Gy, on by default).
    if ARC_MONOLITHIC_LAYOUT then
        filter "system:not windows"
            buildoptions { "-ffunction-sections", "-fdata-sections" }
    end
    filter { "system:not windows", "toolset:gcc" }
        buildoptions { "-fno-gnu-unique" }
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

ARC_MODULE_EXTRAS = {}
for _, name in ipairs(engine_modules.order) do ARC_MODULE_EXTRAS[name] = module_defaults end
ARC_MODULE_EXTRAS.ArcaneCore = function()
    module_defaults()
    -- System libraries live in the descriptors (platforms.<os>.links): a
    -- monolithic static module must not absorb them (the package links them once).
    filter "system:windows"
        removefiles { ARC_MODULE_ROOT .. "/src/Arcane/Base/Posix/**" }
    filter {}
end
ARC_MODULE_EXTRAS.ArcaneRender = function()
    module_defaults()
    defines { "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1" }
    includedirs { "%{IncludeDir.DirectXHeaders}/directx" }
    filter "system:windows"
        defines { "VK_USE_PLATFORM_WIN32_KHR" }
        prebuildcommands { 'call "' .. ARC_ROOT .. '/data/shaders/compile-shaders.bat"' }
    filter "system:not windows"
        prebuildcommands { 'sh "' .. ARC_ROOT .. '/data/shaders/compile-shaders.sh"' }
        removefiles { ARC_MODULE_ROOT .. "/src/Arcane/Render/DeviceCreationD3D12.cpp", ARC_MODULE_ROOT .. "/src/Arcane/Render/GpuCrashD3D12.cpp" }
    filter "system:linux"
        includedirs { ARC_ROOT .. "/ThirdParty/tools/dxc-linux/include/dxc" }
    filter "system:macosx"
        includedirs { ARC_ROOT .. "/ThirdParty/tools/dxc-macos/include/dxc" }
    filter {}
end
ARC_MODULE_EXTRAS.ArcaneAssetPipeline = function()
    module_defaults()
    -- cgltf's entry points ArcaneTests calls directly (CgltfImpl.cpp's note).
    filter "system:windows"
        linkoptions { "/EXPORT:cgltf_parse", "/EXPORT:cgltf_validate", "/EXPORT:cgltf_free" }
    filter {}
end
ARC_MODULE_EXTRAS.ImGui = function()
    -- The vendor DLL: Dear ImGui's core only, exporting IMGUI_API (GImGui and the
    -- whole API live in ONE image; replaces ArcaneClient's /WHOLEARCHIVE:imgui
    -- re-export). The SDL3 platform backend compiles into ArcaneImGui, the one
    -- module that drives it, so this DLL does not depend on SDL3.
    module_defaults()
    cppdialect "C++17"
    local r = ARC_MODULE_ROOT
    files { r .. "/imgui.cpp", r .. "/imgui_draw.cpp", r .. "/imgui_tables.cpp", r .. "/imgui_widgets.cpp", r .. "/imgui_demo.cpp" }
    includedirs { r }
    defines { "IMGUI_API=" .. ARCANE_IMGUI_EXPORT }
    filter "system:not windows"
        visibility "Default"   -- IMGUI_API is empty off-Windows: export by default visibility
    filter {}
end
ARC_MODULE_EXTRAS.ImGuiNodeEditor = function()
    module_defaults()
    cppdialect "C++17"
    local r = ARC_MODULE_ROOT
    files { r .. "/imgui_node_editor.cpp", r .. "/imgui_node_editor_api.cpp", r .. "/imgui_canvas.cpp", r .. "/crude_json.cpp" }
    includedirs { r }
    defines { "IMGUI_API=" .. ARCANE_IMGUI_IMPORT, "IMGUI_NODE_EDITOR_API=" .. ARCANE_IMGUI_EXPORT }
    filter "system:not windows"
        visibility "Default"
    filter {}
end
ARC_MODULE_EXTRAS.ArcaneImGui = function()
    module_defaults()
    -- imgui's SDL3 platform backend lives here (see ImGui above); IMGUI_IMPL_API
    -- empty = module-internal, while the core API is imported from ImGui.dll.
    files { ARC_ROOT .. "/ThirdParty/imgui/backends/imgui_impl_sdl3.cpp" }
    defines { "IMGUI_API=" .. ARCANE_IMGUI_IMPORT, "IMGUI_IMPL_API=" }
end
ARC_MODULE_EXTRAS.ArcaneClient = module_defaults
-- What a package can link (s3.2: Developer and Editor modules never reach a
-- game), so the only modules the monolithic layout builds.
local function packageable(mod)
    return mod.type == "Runtime" or mod.type == "Client"
end
for _, name in ipairs(engine_modules.order) do
    local mod = engine_modules.byName[name]
    if not mod.prebuilt and (not ARC_MONOLITHIC_LAYOUT or packageable(mod)) then
        group(mod.vendor and "ThirdParty" or mod.type)
        arc_module_project(engine_modules, name)
    end
end
group ""

-- ============================================================================
-- ArcaneRuntimeHost: the standalone host's whole body (RuntimeApp, the frame
-- body, Arcane::RuntimeHost::Run) as a static library, built in BOTH layouts
-- (engine modularization s3.3). Modular: linked by ArcaneRuntime.exe with the
-- verification vehicle (ArcaneVerify; ARC_RUNTIME_HOST_VERIFY). Monolithic:
-- linked by every packaged <Game>.exe, without it -- a package never links a
-- Developer module (s3.2), so --headless/--report/--compare are refused there.
-- ============================================================================
group "Programs"
project "ArcaneRuntimeHost"
    arc_module_consumer(engine_modules,
        ARC_MONOLITHIC_LAYOUT and { "ArcaneImGui" } or { "ArcaneVerify", "ArcaneImGui", "ArcaneRemote" },
        { stage = false })
    location(ARC_MONOLITHIC_PROJECTS and path.join(ARC_MONOLITHIC_PROJECTS, "ArcaneRuntimeHost") or "Source/Programs/ArcaneRuntimeHost")
    kind "StaticLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")
    files { "Source/Programs/ArcaneRuntimeHost/src/**.cpp", "Source/Programs/ArcaneRuntimeHost/src/**.hpp" }
    includedirs {
        "Source/Programs/ArcaneRuntimeHost/src",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        -- HEADERS ONLY (see ArcaneRuntime's note below): Manifold2D-typed
        -- engine roster, Mosaic's scheduler alias, NRI-typed NriGraphContext.
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
        "%{IncludeDir.NRI}",
    }
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
    if not ARC_MONOLITHIC_LAYOUT then defines { "ARC_RUNTIME_HOST_VERIFY" } end
    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARC_BUILD_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARC_BUILD_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARC_BUILD_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
group ""

-- The monolithic layout stops here: programs, tests and fixtures are modular only.
if ARC_MONOLITHIC_LAYOUT then return end
-- Programs (Source/Programs/<Program>/), the test exe (Tests/ArcaneTests/) and
-- its fixtures (Tests/Fixtures/<Fixture>/): every project below names its own
-- solution folder right before it, and the last one resets the group.

-- ============================================================================
-- arccook: the offline asset cook CLI (F2b Task 5). Cooks a project's Content/
-- texture sources into Intermediate/Artifacts via
-- Arcane::AssetPipeline::CookSession -- the SAME session type the editor's
-- future in-process cook (Task 12) drives, so the CLI and the editor never
-- diverge on cook semantics. Depends on ArcaneAssetPipeline + ArcaneCore only
-- (no ArcaneClient/imgui/NRI surface) -- ArcaneServer's minimal ConsoleApp
-- shape above is the structural template, widened with the includedirs/links
-- an actual cook tool needs. Every stager (ArcaneRuntime/ArcaneEditor/
-- ArcaneTests) `dependson` this project and runs it in its own postbuild
-- BEFORE staging Content -- see those projects' postbuildcommands below.
-- ============================================================================
group "Programs"
project "arccook"
    arc_module_consumer(engine_modules, { "ArcaneAssetPipeline" })
    location "Source/Programs/arccook"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        -- CookSession.hpp pulls in TextureMetaSettings.hpp (a public
        -- ArcaneAssetPipeline header), which needs <Json.hpp>.
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.Astra}",      -- settings arc S6-6: TextureMetaSettings.hpp (reflected settings struct)
        "%{IncludeDir.Mosaic}",     -- ...and Astra's platform layer
    }

    -- bc7enc_rdo/meshoptimizer link into ArcaneAssetPipeline.dll itself now (P2).
    dependson { "ArcaneAssetPipeline" }

    -- Core-DLL split: ArcaneCore is a DLL now, loaded from this exe's own
    -- directory (dev bin layout: Binaries/<cfg>/arccook/).
    postbuildcommands(arcane_cmds {
    })

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning

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

-- ============================================================================
-- arcbuild: the game-project build driver (spec docs/specs/
-- 2026-09-13-arcbuild-driver-design.md) -- Unreal's Build.bat analogue. ONE
-- entry point (generate/build/rebuild/clean/probe) the editor's ModuleBuild,
-- the Gacha scripts and CI all spawn, so the premake+msbuild orchestration
-- lives once. It links ArcaneCore (Module/Project live there since the
-- Core-DLL split) for Module::ScanFileCrtFlavor -- the s4.3 slot probe is
-- the SAME verdict PluginHost refuses a cross-CRT module on, never a second
-- PE scanner -- and for Project/ProjectManifest (the .arcproj rule the hosts
-- use). Spec 2026-09-15 s1.2/s10: the presentation-free driver no longer
-- needs the presentation DLL at all. arccook above is the structural
-- template; the ArcaneCore.dll postbuild copy is ArcaneRuntime's. Its pure core
-- (Request.cpp, Slot.cpp, Compose.cpp) is ALSO source-compiled into
-- ArcaneTests ([build]).
-- ============================================================================
group "Programs"
project "arcbuild"
    arc_module_consumer(engine_modules, { "ArcaneEngine", "ArcaneNet", "ArcaneCrypto" })
    location "Source/Programs/arcbuild"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        -- Project.hpp's include closure (ProjectManifest -> <Json.hpp> + glm;
        -- Diagnostics -> spdlog/Mosaic): headers only, same set ArcaneRuntime
        -- takes minus imgui/NRI.
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Mosaic}",
    }


    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }

    -- The driver loads ArcaneCore.dll from its own directory (dev bin
    -- layout: Binaries/<cfg>/arcbuild/); a packaged layout ships it beside the
    -- editor, where the DLL already is.
    postbuildcommands(arcane_cmds {
    })

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning

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

-- ============================================================================
-- arcane-abi-digest: the compiler-derived plugin ABI digest tool (3D-readiness
-- Plan B, Task 1). HEADER-ONLY consumer of every engine module (links only ArcaneCore): it includes Core/AbiManifest.hpp, which evaluates sizeof /
-- alignof / offsetof of the game-facing by-value closure. `--write` writes
-- build/abi-digest.txt, `--check` fails when that file or kGamePluginABIDigest
-- disagrees (scripts/ci/build.* run it after the engine build). The prebuild
-- step copies the Engine group manifest (Plugin/AbiManifestEngine.hpp) with a
-- comment line appended into Intermediate/AbiDigest; AbiDigestCommented.cpp
-- compiles that copy so the [abi] test can prove a comment-only edit does not
-- move the mix. Flags mirror a game TU (build/arcane.lua): /arch:AVX2,
-- /Zc:__cplusplus.
-- ============================================================================
group "Programs"
project "arcane-abi-digest"
    arc_module_consumer(engine_modules, engine_modules.order, { link = false, stage = false })
    -- Debug keeps the inline header code that names ArcaneCore exports (Guid ctor,
    -- settings-enum warnings), so ArcaneCore is linked and staged beside the exe.
    arc_module_consumer(engine_modules, { "ArcaneCore" })
    location "Source/Programs/arcane-abi-digest"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    includedirs { "%{wks.location}/Intermediate/AbiDigest" }

    defines {
        "ARC_MODULE_NAME=ArcaneAbiDigest",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
    }

    prebuildcommands {
        '{MKDIR} "%{wks.location}/Intermediate/AbiDigest"',
        '{COPYFILE} "%{wks.location}/Source/Runtime/ArcaneEngine/src/Arcane/Plugin/AbiManifestEngine.hpp" "%{wks.location}/Intermediate/AbiDigest/AbiManifestEngine.commented.hpp"',
        'echo // appended comment line, no code>> "%{wks.location}/Intermediate/AbiDigest/AbiManifestEngine.commented.hpp"',
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        fatalwarnings { "4715" }
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }
    filter "system:not windows"
        links { "dl", "pthread" }

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

-- ============================================================================
-- arcane-abi-digest-mutants: the digest's mutation witness (Plan B Task 1, fix
-- round 1). At GENERATE time premake writes shadow copies of three listed
-- headers (ProcessContext.hpp, Aabb.hpp, ViewTransform.hpp) into
-- Intermediate/AbiDigest/mutants, ahead of the real include dirs. Each shadow
-- carries one mutation behind a macro (a private int before the first
-- container; a same-size field swap) plus an unconditional comment line. One
-- TU per macro bakes that variant's mix; the exe prints `name hex` per variant
-- and AbiDigestTest.cpp requires base == the real mix (a comment moves
-- nothing) and every mutant != base. An anchor that no longer matches fails
-- the generate loudly: update the anchor, never the test. The shadows are
-- written at GENERATE time: after editing any shadowed header, re-run premake
-- (re-run premake: the shadow headers are stale otherwise, and the mutation
-- case fails with base != the real mix). ClientRuntime.hpp and
-- RuntimePresentation.hpp are shadowed too (a pointer before m_pres, an int
-- before gameInput).
-- ============================================================================
local function abi_shadow(rel, edits)
    local src = path.join(_MAIN_SCRIPT_DIR, rel)
    local text = assert(io.readfile(src), "abi shadow: cannot read " .. src)
    for _, e in ipairs(edits) do
        local from, to = text:find(e[1], 1, true)
        if not from then error("abi shadow: anchor not found in " .. rel .. ": " .. e[1]) end
        text = text:sub(1, from - 1) .. e[2] .. text:sub(to + 1)
    end
    local name = rel:match("/src/(.*)$")
    local dest = path.join(_MAIN_SCRIPT_DIR, "Intermediate/AbiDigest/mutants", name)
    os.mkdir(path.getdirectory(dest))
    assert(io.writefile(dest, text))
end
abi_shadow("Source/Runtime/ArcaneEngine/src/Arcane/Base/ProcessContext.hpp", {
    { "std::unique_ptr<::Arcane::TypeContext> m_owned;",
      "// abi mutant: a comment-only line, must not move the digest\n#ifdef ABI_MUT_PROCESS_PRIVATE\n        int m_abiProbe = 0;\n#endif\n        std::unique_ptr<::Arcane::TypeContext> m_owned;" },
})
abi_shadow("Source/Client/ArcaneClient/src/Arcane/Client/ClientRuntime.hpp", {
    { "RuntimePresentation m_pres;",
      "#ifdef ABI_MUT_CLIENT_PRIVATE\n        void* m_abiProbe = nullptr;\n#endif\n        RuntimePresentation m_pres;" },
})
abi_shadow("Source/Client/ArcaneClient/src/Arcane/Client/RuntimePresentation.hpp", {
    { "LocalInputUser         gameInput;",
      "#ifdef ABI_MUT_PRES_GAMEINPUT\n        int abiProbe = 0;\n#endif\n        LocalInputUser         gameInput;" },
})
abi_shadow("Source/Runtime/ArcaneCore/src/Arcane/Math/Aabb.hpp", {
    { "glm::vec3 min{0.0f, 0.0f, 0.0f};",
      "#ifndef ABI_MUT_AABB_SWAP\n        glm::vec3 min{0.0f, 0.0f, 0.0f};\n#endif" },
    { "glm::vec3 max{0.0f, 0.0f, 0.0f};",
      "glm::vec3 max{0.0f, 0.0f, 0.0f};\n#ifdef ABI_MUT_AABB_SWAP\n        glm::vec3 min{0.0f, 0.0f, 0.0f};\n#endif" },
})
abi_shadow("Source/Runtime/ArcaneScene/src/Arcane/Scene/ViewTransform.hpp", {
    { "glm::mat4  view{1.0f};",
      "#ifndef ABI_MUT_VIEW_SWAP\n        glm::mat4  view{1.0f};\n#endif" },
    { "glm::mat4  projection{1.0f};",
      "glm::mat4  projection{1.0f};\n#ifdef ABI_MUT_VIEW_SWAP\n        glm::mat4  view{1.0f};\n#endif" },
})

group "Programs"
project "arcane-abi-digest-mutants"
    includedirs { "%{wks.location}/Intermediate/AbiDigest/mutants" }   -- the shadow headers win
    arc_module_consumer(engine_modules, engine_modules.order, { link = false, stage = false })
    arc_module_consumer(engine_modules, { "ArcaneCore" })
    location "Source/Programs/arcane-abi-digest-mutants"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
    }

    defines {
        "ARC_MODULE_NAME=ArcaneAbiDigestMutants",
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        fatalwarnings { "4715" }
    filter { "system:linux or system:macosx", "architecture:x86_64" }
        buildoptions { "-mavx2", "-mfma" }
    filter "system:not windows"
        links { "dl", "pthread" }

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

-- ============================================================================
-- Servitor (servitor): the loopback CLI that attaches to running engine hosts
-- (spec docs/superpowers/specs/2026-10-05-arc-cli-design.md, plan rev 8.3).
-- The client is program code (namespace Servitor, no export macro) and links
-- ONLY ArcaneCore, ArcaneNet and ArcaneRemoteProtocol (spec AM9, ruling V5):
-- never ArcaneRemote or ArcaneEngine. arc_module_consumer stages that closure
-- beside the exe (and adds the modules' public vendor include dirs), so there
-- is no DLL copy here; ServitorBoundaryTest.cpp ([servitor][boundary]) checks
-- the staged folder holds nothing else. The client sources (src/Servitor/**)
-- also source-compile into ArcaneTests, the arcbuild precedent; main.cpp is
-- the exe's alone. SERVITOR_OUTPUTDIR (V7) lets HostsDir map a source-checkout
-- --engine root to this configuration's Binaries folder. Never on PATH: P0
-- runs it from Binaries/.
-- ============================================================================
group "Programs"
project "Servitor"
    arc_module_consumer(engine_modules, { "ArcaneCore", "ArcaneNet", "ArcaneRemoteProtocol" })
    location "Source/Programs/Servitor"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetname "servitor"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/main.cpp",
        "%{prj.location}/src/Servitor/**.hpp",
        "%{prj.location}/src/Servitor/**.cpp",
    }

    includedirs {
        "%{prj.location}/src",
    }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "SERVITOR_OUTPUTDIR=\"" .. outputdir .. "\"",
    }

    postbuildcommands(arcane_cmds {
    })

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning

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

-- ============================================================================
-- arcbuild-process-fixture: a tiny, dependency-free console app Task 5's
-- ProcessRunner spawns directly (Process.cpp's [build] integration tests,
-- BuildDriverTest.cpp) to prove real CreateProcessW behavior end to end --
-- exact argv reconstruction through the quoting layer, merged stdout+stderr,
-- cwd, a handle-inheritance boundary probe, and child exit-code propagation.
-- Deliberately its OWN project, never part of ArcaneTests' file glob (a
-- second wmain/main in that binary would not link) -- plain Win32
-- (windows.h) only, no ArcaneCore/engine dependency of any kind.
--
-- Task 6 (multibackend hardening): emitted for a WINDOWS target only. Its one
-- source file is wmain + windows.h by design (that is what lets it prove
-- CreateProcessW's exact argv reconstruction and Win32 handle inheritance), so
-- a gmake generation on Linux/macOS must not carry it -- the POSIX process
-- cases spawn /bin/sh directly instead. ArcaneTests' matching dependson is
-- gated the same way.
-- ============================================================================
if os.target() == "windows" then
group "Tests/Fixtures"
project "arcbuild-process-fixture"
    arc_module_consumer(engine_modules, { "ArcaneCore" })
    location "Tests/Fixtures/arcbuild-process-fixture"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/ProcessFixtureMain.cpp",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "speed"
        symbols "on"

    filter "configurations:Dist"
        runtime "Release"
        optimize "speed"
        symbols "off"
    filter {}
end   -- arcbuild-process-fixture: Windows target only (Task 6)

-- ============================================================================
-- death-fixture: a tiny console app that installs Diagnostics and dies on
-- request (crash window plan 1, task 6), so the crash path (task 5) is
-- proven as a REAL process through HostWitness (CrashPathTest.cpp's
-- "death fixture: ..." cases) -- exit code 10 + the .dmp/.txt/.arcdiag/
-- .log.txt quadruple for `--die av`, exit 0 and nothing written for
-- `--die none`. Unlike arcbuild-process-fixture above, this one is NOT
-- dependency-free by design -- the whole point is exercising the real
-- crash path (Arcane/Base/Diagnostics.hpp), not Win32 alone -- so it links
-- ArcaneCore the way ArcaneTests does, narrowed to only what
-- Base/Assert.hpp, Base/Diagnostics.hpp and Base/Log.hpp need: ArcaneCore's
-- own include root plus the two vendored header-only deps those headers
-- pull in transitively (spdlog via Log.hpp, Mosaic via Assert.hpp/Log.hpp).
--
-- Every target since the Diagnostics POSIX port (2026-10-05): the fixture
-- proves the signal-based crash path exactly as it proves the SEH one. Only
-- its `--reporter`/`--monitor` hand-off stays Windows-only (below), because
-- ArcaneCrashReporter does.
-- ============================================================================
group "Tests/Fixtures"
project "death-fixture"
    arc_module_consumer(engine_modules, { "ArcaneCore" })
    location "Tests/Fixtures/death-fixture"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/DeathFixtureMain.cpp",
    }

    includedirs {
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Mosaic}",
    }


    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        -- Same hazard, same fix as ArcaneTests' own MOSAIC_ENABLE_ASSERTS
        -- define above (premake5.lua:1481 and its comment): MOSAIC_ASSERT
        -- (ARC_ASSERT's `assert` die mode) compiles to a no-op under NDEBUG
        -- unless forced on, and this project's Release/Dist filters below
        -- define NDEBUG -- without this, `--die assert` would silently do
        -- nothing in a Release/Dist fixture build.
        "MOSAIC_ENABLE_ASSERTS",
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning

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
        -- ON, unlike the engine's Dist: the fixture is a test program, and
        -- "reporter: symbolizes the death fixture's minidump" asserts that
        -- dbgeng resolves death-fixture.pdb (`!main`, DeathFixtureMain.cpp).
        -- With symbols off the Dist suite had no PDB to find and that case
        -- failed on the hosted Windows lane (Release passes it with the same
        -- optimizer settings and symbols on).
        symbols "on"
    filter {}

    -- Crash window plan 2 (spec §12 item 2): the fixture's `--reporter` mode
    -- hands off to the STAGED reporter beside it, exactly as a host does, so
    -- the hand-off is proven end to end as two real processes. The reporter
    -- is a Windows-target project (its own gate below).
    if os.target() == "windows" then
        dependson { "ArcaneCrashReporter" }
    end

    -- The fixture loads ArcaneCore.dll from its own directory, same as every
    -- other consumer (mirrors ArcaneTests' matching postbuild line).
    postbuildcommands(arcane_cmds {
        -- Crash window plan 2 (spec §12 item 2): the reporter this host hands off to lives beside it.
        arcane_win('{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/ArcaneCrashReporter/ArcaneCrashReporter.exe" "%{cfg.buildtarget.directory}/ArcaneCrashReporter.exe"'),
    })

-- ============================================================================
-- ArcaneCrashReporter (crash window plan 2; spec §6): the out-of-process crash
-- reporter every host hands off to. WindowedApp (no console; wWinMain), links
-- ArcaneCore ONLY -- never ArcaneClient, no GPU -- plus dbgeng/dbghelp for
-- out-of-process symbolization. Dear ImGui is compiled INTO this exe (the
-- core plus the Win32 and D3D11 backends). The D3D11 device is WARP, the
-- software rasterizer, never the GPU that just died. The plain Win32
-- controls are the fallback when WARP cannot be created. STAGED beside every host by that
-- host's own postbuild, exactly like ArcaneCore.dll (spec §12 item 2), which
-- is why each host `dependson` it. Its pure files (ReporterArgs, ReportView,
-- SymbolizedText, HangSession, MonitorRule) are ALSO source-compiled into
-- ArcaneTests ([reporter]); the dbgeng/Win32 files are not.
--
-- Windows target only, same gate (and same reason) as death-fixture above:
-- Diagnostics -- the thing that hands off to this exe -- is Windows-only
-- today, and dbgeng/dbghelp have no counterpart elsewhere.
-- ============================================================================
if os.target() == "windows" then
group "Programs"
project "ArcaneCrashReporter"
    arc_module_consumer(engine_modules, { "ArcaneCore" })
    location "Source/Programs/ArcaneCrashReporter"
    kind "WindowedApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.hpp",
        "%{prj.location}/src/**.cpp",
        -- Private copy. NOT the workspace imgui static lib: that one exports
        -- into ArcaneClient.dll and compiles the SDL3 backend. This exe must
        -- not link ArcaneClient. The D3D11 backend drives a WARP device.
        "%{wks.location}/ThirdParty/imgui/imgui.cpp",
        "%{wks.location}/ThirdParty/imgui/imgui_draw.cpp",
        "%{wks.location}/ThirdParty/imgui/imgui_tables.cpp",
        "%{wks.location}/ThirdParty/imgui/imgui_widgets.cpp",
        "%{wks.location}/ThirdParty/imgui/backends/imgui_impl_win32.cpp",
        "%{wks.location}/ThirdParty/imgui/backends/imgui_impl_dx11.cpp",
    }

    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.spdlog}",     -- Log.hpp
        "%{IncludeDir.Mosaic}",     -- Assert.hpp
        "%{IncludeDir.nlohmann}",   -- Monitor.cpp reads the host's session record (task 9)
        "%{wks.location}/ThirdParty/imgui",
        "%{wks.location}/ThirdParty/imgui/backends",
        "%{wks.location}/Source/Programs/ArcaneEditor/src",   -- Widgets/EditorTheme.hpp, header-only
    }

    -- No dwmapi: nothing in the reporter calls Dwm*; imgui_impl_win32.cpp
    -- pragma-links it for its own use.
    links { "dbgeng", "dbghelp", "user32", "gdi32", "shell32", "ole32", "d3d11", "dxgi" }

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        "NOMINMAX",
        "WIN32_LEAN_AND_MEAN",
        "IMGUI_IMPL_WIN32_DISABLE_GAMEPAD",   -- a crash reporter does not load XInput
    }

    -- The reporter loads ArcaneCore.dll from its own directory, same as every
    -- other consumer (arcbuild's matching postbuild line is the template).
    postbuildcommands(arcane_cmds {
    })

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARC_BUILD_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARC_BUILD_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARC_BUILD_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}
end   -- ArcaneCrashReporter: Windows target only

-- ============================================================================
-- ArcaneServer: the Core-only dedicated-server host (ArcaneServer.exe). Core-DLL
-- split (spec docs/specs/2026-09-15-core-dll-split-design.md s6, plan 1 Task 6):
-- opens a project, loads its game module, and ticks a fixed-step loop headless --
-- no window, no graphics device, no ClientRuntime. `links` below is the
-- construction proof of "Core-only": ArcaneCore ONLY, never ArcaneClient (P10 --
-- see the postbuild copy's matching comment for why ArcaneClient.dll is still
-- STAGED beside this exe despite never being linked).
-- ============================================================================
group "Programs"
project "ArcaneServer"
    -- Links Engine/Net/Crypto ONLY (P10), plus ArcaneRemote (the Servitor listener,
    -- a Runtime module, so still no Client module), but STAGES a game module's full set: the
    -- loader must resolve every module DLL a game module imports to MAP it at all.
    -- The stage list names ArcaneRemote too: the game set's closure does not hold
    -- it (or ArcaneRemoteProtocol), and the exe imports it.
    arc_module_consumer(engine_modules, { "ArcaneEngine", "ArcaneNet", "ArcaneCrypto", "ArcaneRemote", "ArcanePhysics2D" }, { stage = table.join(ARC_GAME_DEFAULT_MODULES, { "ArcaneRemote" }) })
    location "Source/Programs/ArcaneServer"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.hpp", "%{prj.location}/src/**.cpp" }
    includedirs {
        "%{prj.location}/src",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        -- Mosaic: HEADERS ONLY -- Astra's own headers reach Mosaic's IWorkScheduler
        -- alias. This exe does not link it: every Mosaic OBJECT it can reach (the
        -- JobSystem's scheduler) is created and destroyed inside ArcaneCore.dll.
        -- No Manifold2D include dir since 3D-readiness Task 8: the 2D solver is
        -- ArcanePhysics2D's (named in the consumer list above), and a stray
        -- Manifold2D include in this exe now fails the compile.
        "%{IncludeDir.Mosaic}",
    }
    -- Links the Engine/Net/Crypto closure ONLY (P10: never a Client module). R30's
    -- build-order fix (a staged-but-unlinked DLL must be built before the copy)
    -- is arc_module_stage's dependson on every staged module now.
    if os.target() == "windows" then
        dependson { "ArcaneCrashReporter" }   -- staged by the postbuild below; emitted for a Windows target only
    end
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING" }
    postbuildcommands(arcane_cmds {
        -- Crash window plan 2 (spec §12 item 2): the reporter this host hands off to lives beside it.
        arcane_win('{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/ArcaneCrashReporter/ArcaneCrashReporter.exe" "%{cfg.buildtarget.directory}/ArcaneCrashReporter.exe"'),
        -- P10: the game module (ReferenceGame.dll et al) links BOTH ArcaneCore's
        -- and ArcaneClient's import libs -- it is built as an engine-as-SDK
        -- consumer against the FULL surface, the same DLL a windowed host loads
        -- it into. The loader needs ArcaneClient.dll resolvable beside this exe
        -- to MAP the module at all, even though NOTHING in this exe's own code
        -- references it. The census (clientDllLoadedAtBoot/clientDllLoadedAfterModule)
        -- reports whether it was ever actually loaded, rather than this postbuild
        -- silently hiding the module's own dependency.
        arcane_copydir("%{wks.location}/data/EngineConfig", "%{cfg.buildtarget.directory}/data/EngineConfig"),
        -- Same staging pair ArcaneRuntime's/ArcaneEditor's postbuild carries (see
        -- ArcaneRuntime's matching comment for the full "mirror deletions" reasoning):
        -- wipe Content/Source/Verify before the whole-tree copy re-populates them, so
        -- a file removed from SOURCE does not survive forever in the staged copy.
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Content"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Content"',
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Source"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Source"',
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        arcane_copydir("%{wks.location}/ReferenceProject", "%{cfg.buildtarget.directory}/ReferenceProject"),
    })
    filter "system:windows"
        systemversion "latest"
        -- /bigobj: ServerApp.cpp pulls in Arcane/Scene/PhysicsSystem.hpp
        -- (header-only, Manifold2D-templated) directly, same section-count
        -- pressure ArcaneClient.dll's own /bigobj (above) exists for.
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARC_BUILD_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARC_BUILD_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARC_BUILD_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- ArcaneRuntime: the thin standalone host (ArcaneRuntime.exe). Engine boot +
-- RunLoop + PluginHost. Hosts a project's gameModule (--project; --plugin
-- overrides); nothing to host refuses boot. The ReferenceProject tree is copied
-- beside the exe, so `ArcaneRuntime --project ReferenceProject --frames N` is
-- the scripted GPU-verify. Folded from "Loom" 2026-07-29.
-- ============================================================================
group "Programs"
project "ArcaneRuntime"
    -- The host's body (RuntimeApp & co.); this exe is main.cpp + the staging
    -- below. FIRST: GNU ld resolves left to right, and under --as-needed a
    -- module .so listed before the archive that references it is dropped.
    links { "ArcaneRuntimeHost" }
    includedirs { "%{wks.location}/Source/Programs/ArcaneRuntimeHost/src" }
    arc_module_consumer(engine_modules, { "ArcaneVerify", "ArcaneImGui", "ArcaneNet", "ArcaneCrypto", "ArcaneRemote", "ArcanePhysics2D" })
    location "Source/Programs/ArcaneRuntime"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")
    files { "%{prj.location}/src/**.cpp", "%{prj.location}/src/**.hpp" }
    includedirs {
        "%{prj.location}/src",           -- RuntimeApp.hpp (self-resolving quoted include; kept for any future local header)
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        -- No Manifold2D include dir (3D-readiness Task 8): the roster's physics third
        -- (Scene's PhysicsComponents.hpp) is solver-free since the handle became a
        -- uint64, and the solver is ArcanePhysics2D's, named in the consumer list.
        "%{IncludeDir.Mosaic}",
        -- NRI Phase 2, Task 7: RuntimeApp holds an Arcane::NriGraphContext (the
        -- --nri-graph vehicle), and that header is NRI-typed because the node
        -- authors of Tasks 8-12 are its other consumers. HEADERS ONLY -- this
        -- exe does not link NRI, and every nri:: object it can reach is created
        -- and destroyed inside ArcaneClient.dll. (Linking a second static copy
        -- of NRI into the exe would be the bug -- see NriDevice::
        -- CreateNoneForTests's comment about ArcaneTests' own copy.)
        "%{IncludeDir.NRI}",
    }
    -- ArcaneCore is a DLL since the Core-DLL split: exactly one copy per
    -- process BY CONSTRUCTION, and it is ArcaneCore.dll. Both links below are
    -- import libs; this exe stages both DLLs beside itself (postbuild).
    -- arccook (F2b Task 5) must exist before this project's postbuild runs it.
    dependson { "arccook" }
    if os.target() == "windows" then
        dependson { "ArcaneCrashReporter" }   -- staged by the postbuild below; emitted for a Windows target only
    end
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
    postbuildcommands(arcane_cmds {
        -- F2b Task 5: cook FIRST, then stage the cooked artifacts -- cook-then-copy per
        -- stager, because under /m no postbuild orders against another project's
        -- postbuild (ArcaneEditor/ArcaneTests run this exact same pair independently).
        -- Safe by construction: the cook is idempotent by hash (a second run is free)
        -- and concurrent same-key writes are atomic-rename + deterministic (identical
        -- bytes, last rename wins -- see ArtifactStore.hpp's concurrency contract).
        '"%{wks.location}/Binaries/' .. outputdir .. '/arccook/' .. arcane_exe("arccook") .. '" --project "%{wks.location}/ReferenceProject"',
        arcane_copydir("%{wks.location}/ReferenceProject/Intermediate/Artifacts", "%{cfg.buildtarget.directory}/ReferenceProject/Intermediate/Artifacts"),
        -- Crash window plan 2 (spec §12 item 2): the reporter this host hands off to lives beside it.
        arcane_win('{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/ArcaneCrashReporter/ArcaneCrashReporter.exe" "%{cfg.buildtarget.directory}/ArcaneCrashReporter.exe"'),
        arcane_copydir("%{wks.location}/data/shaders/generated", "%{cfg.buildtarget.directory}/data/shaders"),
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/data/shaders/materials"',
        arcane_copydir("%{wks.location}/data/shaders/materials", "%{cfg.buildtarget.directory}/data/shaders/materials"),
        arcane_copydir("%{wks.location}/data/EngineConfig", "%{cfg.buildtarget.directory}/data/EngineConfig"),
        -- Task C (F2c debts): mirror DELETIONS for the subtrees whose SOURCE state can shrink
        -- across a rebuild -- Content/, Source/ (project assets + generated components) and
        -- Verify/ (golden traces) -- by wiping the staged copies immediately before the
        -- whole-tree {COPYDIR} below re-populates them. Without this, a file removed from
        -- SOURCE (e.g. a5d77e30 deleting Source/GameApi.hpp) survives forever in the staged
        -- tree because {COPYDIR} only ever adds/overwrites, never deletes (RCA:
        -- .superpowers/sdd/2026-09-10-f2c-mesh-import-plan2-runtime-editor/task12-rca2-report.md).
        -- Corollary for Verify/ specifically: this wipe-then-recopy runs on EVERY build, so a
        -- `--bless` made against a staged Verify/ tree (scripts/golden-gate.ps1's -SelfTest
        -- header, step 3) does not survive the next `msbuild` -- copy a new staged bless to
        -- SOURCE before rebuilding, or it is gone.
        -- {RMDIR} emits a BARE "rmdir /S /Q <path>" on Windows -- unlike {MKDIR} (which premake
        -- wraps in "IF NOT EXIST"), it carries no existence guard. Task F (F2c debts closeout)
        -- adds a {MKDIR} of the same path immediately before each {RMDIR} below, so the delete's
        -- target is now GUARANTEED to exist (a no-op mkdir if a prior build already staged it)
        -- rather than relying on the review-verified but incidental MSBuild fact that only the
        -- postbuild block's LAST command's exit code reaches Exec -- a future reorder of this
        -- list can no longer silently put a bare rmdir-on-missing-path ahead of a failing step.
        --
        -- Deliberately NOT mirrored the same way, subtree by subtree:
        --   Intermediate/ -- cooked artifacts; the targeted {COPYDIR} of Intermediate/Artifacts
        --                    above (fed by arccook) and any in-session cook already keep it
        --                    live, and it is additive by design (a cook cache).
        --   Saved/        -- runtime state (thumbnails, layout, diffs) plus the committed
        --                    verify-layout.ini seed; a host is expected to accumulate its own
        --                    Saved/ across runs, and wiping it would nuke that state.
        --   Binaries/     -- the single-slot game DLL; arcbuild owns its own rebuild-vs-reuse
        --                    logic, not this copy.
        --   ReferenceProject.arcproj -- the project file itself.
        -- The whole-tree {COPYDIR} below still copies all of those additively, on purpose --
        -- only Content/, Source/ and Verify/ get wiped first -- and any other non-source-of-
        -- truth subtree the whole-tree copy carries along (Config/, Plugins/, Goldens/ today):
        -- none of those are registry-mounted, so none can reproduce the phantom-asset class this
        -- task guards against. This is today's list of exceptions, not a closed one.
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Content"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Content"',
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Source"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Source"',
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        arcane_copydir("%{wks.location}/ReferenceProject", "%{cfg.buildtarget.directory}/ReferenceProject"),
    })
    postbuildcommands(arcane_vendor_runtime())
    filter "system:windows"
        systemversion "latest"
        -- /bigobj (2026-09-16): RuntimeApp.cpp's own VerifySharedTypeContext call
        -- expands the twelve-type engine roster, and each Astra::TypeID<T> pulls in
        -- another COMDAT set on top of an already-wide include surface -- C1128
        -- without this. Same reason ArcaneServer/ArcaneEditor/ArcaneClient carry it.
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
    filter "configurations:Debug"    defines { "ARC_BUILD_DEBUG" }                   runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARC_BUILD_RELEASE", "NDEBUG" }       runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARC_BUILD_DIST", "NDEBUG" }          runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- Arcane Editor: the editor shell (ArcaneEditor.exe). Engine boot + RunLoop + PluginHost
-- + ImGui docking shell. Hosts the open project's gameModule. Consumes the engine's
-- host-boot helpers (Arcane::GpuContext/FramePerf/HostConfig, Arcane/Host/ --
-- exported ARC_API from ArcaneClient.dll, same as ArcaneRuntime) rather than source-
-- compiling its own copy. Consumes only ARC_API otherwise.
-- ============================================================================
group "Programs"
project "ArcaneEditor"
    arc_module_consumer(engine_modules, engine_modules.order)
    location "Source/Programs/ArcaneEditor"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")
    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
        -- node-page phase s8.1: the crash viewer source-compiles the reporter's
        -- pure model (third consumer = promote to Core, spec 9.27 #12).
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/ReporterArgs.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/SymbolizedText.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/ReportView.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/LogTail.cpp",
    }
    includedirs {
        "%{prj.location}/src",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src",
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.imgui}",
        "%{IncludeDir.imguinodeeditor}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
        -- NRI Phase 3, Task 8: EditorApp holds TWO Arcane::NriGraphContexts on
        -- the `--nri-graph` flavor (the host-window "chrome" one and the
        -- offscreen one the Viewport panel samples), and that header is
        -- NRI-typed. HEADERS ONLY, exactly as ArcaneRuntime takes it (see that
        -- project's matching comment): this exe does not link NRI, and every
        -- nri:: object it can reach is created and destroyed inside
        -- ArcaneClient.dll. Linking a second static copy of NRI into the exe
        -- would be the bug.
        "%{IncludeDir.NRI}",
        -- Task 12's in-process cook (later F2b task) calls into the asset
        -- pipeline directly from the editor.
    }
    -- bc7enc_rdo/meshoptimizer link into ArcaneAssetPipeline.dll itself now (P2).
    -- arccook (F2b Task 5) must exist before this project's postbuild runs it.
    -- ArcaneServer: the play-mode picker's separate-server row resolves and spawns
    -- ../ArcaneServer/ArcaneServer.exe, so it has to be built beside this exe.
    dependson { "arccook", "ArcaneServer" }
    if os.target() == "windows" then
        dependson { "ArcaneCrashReporter" }   -- staged by the postbuild below; emitted for a Windows target only
    end
    defines { "_CRT_SECURE_NO_WARNINGS", "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING", "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
    postbuildcommands(arcane_cmds {
        -- F2b Task 5: cook FIRST, then stage the cooked artifacts -- same cook-then-copy
        -- shape as ArcaneRuntime's matching comment above (safe under /m: idempotent by
        -- hash, concurrent same-key writes atomic-rename + deterministic).
        '"%{wks.location}/Binaries/' .. outputdir .. '/arccook/' .. arcane_exe("arccook") .. '" --project "%{wks.location}/ReferenceProject"',
        arcane_copydir("%{wks.location}/ReferenceProject/Intermediate/Artifacts", "%{cfg.buildtarget.directory}/ReferenceProject/Intermediate/Artifacts"),
        -- Crash window plan 2 (spec §12 item 2): the reporter this host hands off to lives beside it.
        arcane_win('{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/ArcaneCrashReporter/ArcaneCrashReporter.exe" "%{cfg.buildtarget.directory}/ArcaneCrashReporter.exe"'),
        arcane_copydir("%{wks.location}/data/shaders/generated", "%{cfg.buildtarget.directory}/data/shaders"),
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/data/shaders/materials"',
        arcane_copydir("%{wks.location}/data/shaders/materials", "%{cfg.buildtarget.directory}/data/shaders/materials"),
        '{MKDIR} "%{cfg.buildtarget.directory}/data"',
        arcane_copydir("%{wks.location}/data/EngineConfig", "%{cfg.buildtarget.directory}/data/EngineConfig"),
        '{MKDIR} "%{cfg.buildtarget.directory}/data/EditorThemes"',
        arcane_copydir("%{wks.location}/data/EditorThemes", "%{cfg.buildtarget.directory}/data/EditorThemes"),
        -- Task C (F2c debts): wipe the staged Content/Source/Verify subtrees before the
        -- whole-tree {COPYDIR} below re-populates them, same reasoning (and NOT-mirrored
        -- list -- Intermediate/, Saved/, Binaries/, the .arcproj, and any other non-source-of-
        -- truth subtree the whole-tree copy carries along, e.g. Config/, Plugins/, Goldens/ --
        -- none registry-mounted, so none can reproduce the phantom-asset class; not a closed
        -- list) as ArcaneRuntime's matching comment above. Task F adds a {MKDIR} of the same
        -- path before each {RMDIR} so the delete's target is guaranteed to exist, same as
        -- ArcaneRuntime's matching lines.
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Content"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Content"',
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Source"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Source"',
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        arcane_copydir("%{wks.location}/ReferenceProject", "%{cfg.buildtarget.directory}/ReferenceProject"),
        -- Editor fonts: Inter (default) + Roboto + JetBrains Mono faces + lucide icon
        -- font, merged into the ImGui atlas by EditorFonts.cpp (exe-relative paths --
        -- must align w/ dests).
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/lucide"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/inter/static"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/roboto/static"',
        '{COPYFILE} "%{wks.location}/data/font/inter/static/Inter_18pt-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/inter/static/Inter_18pt-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/roboto/static/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/roboto/static/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/lucide/lucide.ttf" "%{cfg.buildtarget.directory}/data/font/lucide/lucide.ttf"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/aldotheapache"',
        '{COPYFILE} "%{wks.location}/data/font/aldotheapache/AldotheApache.ttf" "%{cfg.buildtarget.directory}/data/font/aldotheapache/AldotheApache.ttf"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/jetbrainsmono"',
        '{COPYFILE} "%{wks.location}/data/font/jetbrainsmono/JetBrainsMono-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/jetbrainsmono/JetBrainsMono-Regular.ttf"',
        -- Arcane logo: window/taskbar icon (Window::SetIcon) + transport-toolbar mark
        -- (LoadDisplayTexture). Same PNG, exe-relative at "data/images/arcane_logo.png".
        '{MKDIR} "%{cfg.buildtarget.directory}/data/images"',
        '{COPYFILE} "%{wks.location}/data/images/arcane_logo.png" "%{cfg.buildtarget.directory}/data/images/arcane_logo.png"',
    })
    postbuildcommands(arcane_vendor_runtime())
    filter "system:windows"
        systemversion "latest"
        -- /bigobj (Task 3, F2a): EditorApp.cpp aggregates a wide include surface
        -- (Project.hpp, MaterialAsset.hpp, PanelRegistry.hpp, ...) that pulls in
        -- Components.hpp header-only, whose ASTRA_REFLECT_TYPE blocks re-expand
        -- PER TU. MeshRenderer's two fields pushed this TU's /ZI object past the
        -- COFF section limit (C1128); the other projects in this file that hit
        -- the same limit already carry this flag (see /bigobj above).
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        -- .exe file icon (Explorer/taskbar/Alt-Tab): a Win32 ICON resource. The .rc
        -- references arcane.ico by name; resincludedirs points RC at its folder.
        files { "%{prj.location}/resources/ArcaneEditor.rc" }
        resincludedirs { "%{prj.location}/resources" }
    filter "configurations:Debug"    defines { "ARC_BUILD_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release"  defines { "ARC_BUILD_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"     defines { "ARC_BUILD_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- ArcaneTests: Catch2 + rapidcheck (Server conventions). ArcaneCore is a DLL
-- since the Core-DLL split: exactly one copy per process BY CONSTRUCTION, and
-- it is ArcaneCore.dll -- the link below is its import lib, and this exe
-- stages the DLL beside itself (postbuild).
-- ============================================================================
-- Solution folder "Tests", not "ArcaneTests": a folder with the same name as
-- a sibling project collides in the solution tree. Its fixtures (the
-- hot-reload DLLs, the process/death fixtures, the game-module fixtures) sit
-- in "Tests/Fixtures", so they never read as top-level products.
group "Tests"
project "ArcaneTests"
    arc_module_consumer(engine_modules, engine_modules.order)
    location "Tests/ArcaneTests"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    -- The test exe outgrew MSVC's incremental-link table: a Debug relink
    -- fails with LNK1116 "cannot grow ilk file" (error 487) on the hosted
    -- runner and intermittently on the desk. A full link every time is the
    -- price.
    incrementallink "Off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files {
        "%{prj.location}/src/**.cpp",
        "%{prj.location}/src/**.hpp",
        -- HostConfig (the typed host CLI result over Arcane::Cli), GpuContext,
        -- FramePerf, and ProjectBoot all moved into Arcane/Host (ArcaneClient.dll,
        -- ARC_API) alongside Module/Plugin/PluginHost (Arcane/Plugin) -- the
        -- test exe now consumes all of them via the "Arcane" link, not source-
        -- compiled. [host] round-trips HostConfig::Parse without loading ArcaneRuntime.exe.
        -- Task 3: ConsoleBuffer (Arcane Editor's log ring buffer) source-compiles into the
        -- test exe so the [editor] unit test drives it directly.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/ConsoleBuffer.cpp",
        -- Task 4: ViewportInput (pure input-gating predicates for the scene-in-a-
        -- panel viewport) source-compiles into the test exe so the [editor] unit
        -- tests drive it directly, same pattern as ConsoleBuffer above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Viewport/ViewportInput.cpp",
        -- Task 5: EntityList (entity enumeration for the Hierarchy panel)
        -- source-compiles into the test exe so the [editor] unit tests drive it
        -- directly, same pattern as ConsoleBuffer/ViewportInput above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/EntityList.cpp",
        -- Servitor P0 Task 25: editor.selection.get (the editor program's
        -- remote command over SelectionContext) source-compiles into the test
        -- exe so the [remote][scene] cases drive it directly.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Remote/SelectionCommand.cpp",
        -- Task 6: InspectorFields (reflected field classification + pure write-backs
        -- for the Inspector panel) source-compiles into the test exe so the
        -- [editor] unit tests drive it directly -- no ImGui dependency, same
        -- pattern as ConsoleBuffer/ViewportInput/EntityList above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/InspectorFields.cpp",
        -- Task 8: PlayMode (play-in-editor snapshot/restore state machine)
        -- source-compiles into the test exe so the [editor] round-trip test
        -- drives it directly against a real Arcane::Runtime, same pattern as
        -- ConsoleBuffer/ViewportInput/EntityList/InspectorFields above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/App/PlayMode.cpp",
        -- Shader-editor Slice 5: DocumentHost (open-document list + unsaved-
        -- close confirm state machine + asset->editor routing) source-compiles
        -- into the test exe so the [editor] units drive the PURE close flow
        -- with fake documents -- DrawAll (the only ImGui method) is not called.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/DocumentHost.cpp",
        -- Node page + editor upgrades s3.2: the pure PreviewStatus model.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/PreviewStatus.cpp",
        -- Shader-editor review fixes: ShaderEditorDocument source-compiles into
        -- the test exe so the [editor] units drive its HEADLESS halves directly
        -- (save-before-bind, parent-chain resolution, compile-result routing).
        -- Draw (the ImGui half) is never called; device-less services skip the
        -- preview resources in the ctor.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/ShaderEditorDocument.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/InputActionsEditorModel.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/InputActionsDocument.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/InputActionsDocumentWidgets.cpp",
        -- Input editor T7: InputActionsRows (the actions column as pure rows)
        -- source-compiles here for InputActionsRowsTest, and the rewritten
        -- InputActionsDocumentWidgets.cpp above links against it.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/InputActionsRows.cpp",
        -- Node-page phase s8.3: the pure pending add (add-and-listen), compiled for InputPendingAddTest
        -- and linked by InputActionsDocument/Widgets above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/InputPendingAdd.cpp",
        -- Input editor T10: InputActionsInspectorPage is a LINK dependency of
        -- InputActionsDocument.cpp (Page/PageFor construct it); Draw is never
        -- called headlessly, same reason as EditorWidgets.cpp below.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/InputActionsInspectorPage.cpp",
        -- Outliner slice 4: ComponentCatalog (registry enumeration + the one
        -- system-managed hide-list + selection-aware missing counts) source-
        -- compiles into the test exe so the [editor] units drive it directly --
        -- no ImGui dependency, same pattern as EntityList/InspectorFields above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Scene/ComponentCatalog.cpp",
        -- Scene authoring: SceneSession (scene identity + dirty state + the
        -- unsaved-changes confirm machine) source-compiles into the test exe so
        -- the [editor] units drive the PURE state machine directly -- there is
        -- no ImGui in it at all, same pattern as DocumentHost above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Scene/SceneSession.cpp",
        -- editor.undo.* (T1-B10; S6-33): EditorUndoSettings + ReadUndoLimits, so
        -- [undo] drives the read and its range clamps without EditorApp.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/App/UndoSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/AssetBrowserSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/AssetGraphSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/DocumentSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorConsoleSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorDocumentUiSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorGridSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorPlaySettings.cpp",
        -- astra.snapshot.compression (S6-45 fix): the Editor setting + its push into Runtime::SetSnapshotSaveConfig.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorSnapshotSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorThemeSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorThumbnailSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/GraphCanvasSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/GraphThemeSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/InspectorSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/AppearanceApplier.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorUiSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorUiStyleSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorViewportSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/AxisColors.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/ThemePresets.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/ThemePage.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/ShortcutsPage.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/FontsPage.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/LayoutLibrary.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/LayoutSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/LayoutPage.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Input/EditorActions.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Input/EditorActionTable.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Viewport/ViewportActions.cpp",
        -- Inspector polish: InspectorMeta (display-name derivation, attribute
        -- extraction, filter matching) source-compiles into the test exe so the
        -- [editor] units drive it directly. It is the whole surface the user
        -- reads in the Inspector, and EditorPanels.cpp is not compiled here --
        -- so anything left in the draw loop would have no coverage at all.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/InspectorMeta.cpp",
        -- Inspector ownership T2: InspectorHost (pure routing, no ImGui) source-
        -- compiles into the test exe so EditorInspectorHostTest drives it with
        -- fake sources. SceneInspectorSource.cpp stays OUT (it needs
        -- EditorPanels.cpp).
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/InspectorHost.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/InspectorKinds.cpp",
        -- Arc-1 debt F: InspectorWindows (the Inspector windows + the
        -- [EditorInspector][Instances] ini handler) source-compiles into the
        -- test exe so EditorInspectorHostTest drives the handler on a bare
        -- ImGui context. It needs only InspectorHost + PropertyGrid (both
        -- compiled here) and the header-only EditorTheme; DrawInspectorWindows
        -- is never called headlessly.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/InspectorWindows.cpp",
        -- F2b Task 13: TextureMetaPanel (the Inspector's texture-asset
        -- settings block's PURE half -- reading/merge-writing the ".meta"
        -- sidecar's "texture" block) source-compiles into the test exe so
        -- the [editor] units drive the merge-preserving write directly --
        -- no ImGui in it at all, same InspectorMeta/ConsoleModel split
        -- applied to this problem. Its ImGui half (TextureImportSettings.cpp,
        -- the Asset page's import-settings block) IS compiled here since
        -- inspector filters Task 5 -- see the asset-panel entries below.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/TextureMetaPanel.cpp",
        -- Scene authoring: EditorCamera (the editor's own viewport pan/zoom/
        -- framing math + the framing-bounds sweep) source-compiles into the
        -- test exe so the [editor] units drive the PURE math headlessly -- no
        -- ImGui and no engine calls in it, same pattern as SceneSession above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Viewport/EditorCamera.cpp",
        -- F4 plan 1 T7: ViewportSettings (the PURE [EditorViewport][Camera] ini
        -- line writer/reader + the --view-mode seed) source-compiles into the
        -- test exe so the [editor][settings] units drive the round-trip and the
        -- refusal table with a bare ImGuiTextBuffer (no context needed), same
        -- pattern as EditorCamera above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Viewport/ViewportSettings.cpp",
        -- F4 plan 1 T9: ViewportGrid (the PURE decade-level plan + the line
        -- draw through a ViewTransform into any Batcher2D) source-compiles into
        -- the test exe so the [editor][grid] units drive the LOD rule and the
        -- rect-clipped line counts against a recording batcher double -- no
        -- ImGui, no device, same pattern as EditorCamera above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Viewport/ViewportGrid.cpp",
        -- Widget layer: EditGesture's PURE decision core (gesture ownership +
        -- close-path verdicts) source-compiles into the test exe so the
        -- [editor] units drive the full decision table headlessly -- the ImGui
        -- skin in the same TU is never called, same pattern as
        -- ShaderEditorDocument above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Scene/EditGesture.cpp",
        -- Astra adoption Task 8: EditModeSchedule (the Edit-mode scheduler + the
        -- pending camera-frame request) source-compiles into the test exe so the
        -- [editor] units drive it headlessly -- ImGui-free by construction, same
        -- pattern as EditGesture/EditorCamera above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Scene/EditModeSchedule.cpp",
        -- Widget layer: EditorWidgets is here as a LINK dependency, not a unit
        -- surface -- ShaderEditorDocument.cpp (compiled above) calls
        -- StableTextEdit for its four inline rename rows, and without this the
        -- test exe fails to link (LNK2019). Nothing in it is called headlessly;
        -- it is pure ImGui, like the skin half of EditGesture.cpp.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Widgets/EditorWidgets.cpp",
        -- Asset panel widget vocabulary Task 6: EditorFonts is here as a LINK
        -- dependency, not a unit surface, same reason as EditorWidgets.cpp
        -- above -- EditorWidgets.cpp's new AssetPill calls GetEditorFonts()
        -- for its 12px PushFont, and without this the test exe fails to link
        -- (LNK2019). InstallEditorFonts is never called headlessly; only the
        -- accessor's empty-set fallback is reachable, same as EditorWidgets.cpp.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Widgets/EditorFonts.cpp",
        -- Colour pipeline + dense picker Task 7: ColorPickerPopup is here as a
        -- LINK dependency, not a unit surface, same reason as EditorWidgets.cpp
        -- above -- ShaderEditorDocument.cpp (compiled above) now calls
        -- ColorPopupId/ColorSwatchButton/ColorPopupBody for the material-param
        -- row and the ConstColor node, and without this the test exe fails to
        -- link (LNK2019). Nothing in it is called headlessly; it is pure ImGui.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Widgets/ColorPickerPopup.cpp",
        -- Inspector ownership T1: PropertyGrid (the shared page rows) source-
        -- compiles into the test exe: PropertyGridTest drives it under a real
        -- ImGui context, and InputActionsInspectorPage.cpp (T10) links against it.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Widgets/PropertyGrid.cpp",
        -- Node-page phase T2 (s4.5): GraphFit -- the capped frame-to-fit. Its pure
        -- maths + cvar are [graphfit]-tested, and ShaderEditorDocument.cpp
        -- (compiled above) calls GraphFitToContent, so it is a link dependency too.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Widgets/GraphFit.cpp",
        -- T3-D1: the shader graph's pin legend + its editor.graph.showPinLegend
        -- cvar. ShaderEditorDocument.cpp (compiled above) calls it, so it is a
        -- link dependency, and the cvar's default + toggle are unit-tested.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/ShaderGraphPinLegend.cpp",
        -- Widget layer Task 7: SpriteDocument source-compiles into the test exe
        -- so the [editor] units drive its UNDO half directly (ApplySpriteData,
        -- the before/after step builder, and the doc-identity anchor after the
        -- document closes). Draw (the only ImGui method) is never called --
        -- same precedent as ShaderEditorDocument above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/SpriteDocument.cpp",
        -- F2a, Task 9: MeshDocument source-compiles into the test exe so the
        -- [editor][mesh] units drive its HEADLESS halves directly (data/undo
        -- following SpriteDocument above; the offscreen-preview lifecycle
        -- following ShaderEditorDocument's -- EnsurePreviewContext/
        -- RenderPreview/DestroyPreviewContext). Draw (the ImGui half) is
        -- never called; device-less services skip the preview resources in
        -- the ctor, same precedent as ShaderEditorDocument above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/MeshDocument.cpp",
        -- GPU crash diagnostics arc, Task 10: CrashReportDocument source-
        -- compiles into the test exe so the [editor][diag] units drive its
        -- PURE model half directly (construction from an already-loaded
        -- Diag::Envelope, the CPU-report-noise filters, the .gpudump
        -- sibling's parsed section inventory). Draw (the only ImGui method)
        -- is never called -- same precedent as ShaderEditorDocument/
        -- SpriteDocument above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Documents/CrashReportDocument.cpp",
        -- Task 5 (runtime-host-fold arc): RuntimeLaunch's PURE candidate-list/
        -- argv builder (ExeCandidates/BuildArgs) source-compiles into the test
        -- exe so the [editor] units drive them directly. SpawnDetached (the
        -- one CreateProcessW call in the file) is compiled here too but never
        -- invoked by any test -- process creation is desk-verify territory,
        -- same "no spawn test" rule the task brief states outright.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/RuntimeLaunch.cpp",
        -- Core-DLL split Plan 1, Task 7: ServerLaunch -- the "Client + separate
        -- server process" play mode's exe-candidate list and argv builder. Same
        -- split, same reason as RuntimeLaunch.cpp directly above: ExeCandidates/
        -- BuildArgs are PURE and the [editor] units drive them directly, while
        -- ServerProcess::Spawn (the one CreateProcessW call in the file) is
        -- compiled here but never invoked by any test -- process creation is
        -- desk-verify territory. ServerProcess's DORMANT half (a never-spawned
        -- handle's IsRunning/Stop) IS covered, because the editor calls Stop()
        -- unconditionally after every Play->Stop.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/ServerLaunch.cpp",
        -- node-page phase s8.2: which Problems rows route, and how (pure, fake facts in LocatorRouteTest).
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/LocatorRoute.cpp",
        -- Diagnostics arc: ConsoleModel (category derivation from the engine's
        -- "Subsystem: " log prefixes + identical-row collapsing) source-compiles
        -- into the test exe so the [editor] units drive the pure functions the
        -- Console panel draws with -- EditorPanels.cpp is not compiled here.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/ConsoleModel.cpp",
        -- File -> Open Recent: RecentProjects source-compiles into the test exe
        -- so the [editor] units drive its PURE half directly -- parsing the
        -- Hub's shared recents.archub, ABI/current/missing selection, and the
        -- move-to-front touch that must preserve fields the editor does not
        -- model. No ImGui in it at all, same pattern as SceneSession/
        -- EditorCamera above. This file writes to a file the HUB owns, so the
        -- refuse-to-clobber rules are the ones most worth pinning.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/RecentProjects.cpp",
        -- Node-page phase s8.4: the start page's pure model (relative times, rows, the
        -- dialog start dir), driven by StartPageModelTest. No ImGui in it.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/StartPageModel.cpp",
        -- File -> Open Recent Scene: SceneRecents' pure list ops (Parse/
        -- Serialize/Push, and the file I/O around them) source-compile into
        -- the test exe so the [editor] units drive them directly. Unlike
        -- RecentProjects, this file has no "never clobber" contract to pin --
        -- it is the editor's own per-project file, not the Hub's shared one.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/SceneRecents.cpp",
        -- Build -> Rebuild Game Module: ModuleBuild's PURE halves (the SDK-root
        -- walk, the arcbuild.exe candidate list, the composed driver line)
        -- source-compile into the test exe so the [editor] units drive them
        -- directly. The Runner/_wpopen half and RunCapture are compiled too;
        -- no [editor] test invokes them -- only the opt-in [build-desk] cases
        -- (BuildDriverTest.cpp) run RunCapture, against the built driver.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/ModuleBuild.cpp",
        -- Build -> Open Visual Studio / open-source-in-VS: IdeLaunch's PURE
        -- halves (the ROT moniker predicate, solution-path equivalence, the
        -- devenv argv, outcome wording) source-compile into the test exe so
        -- the [editor] units drive them directly. The COM half (ROT walk +
        -- late-bound DTE) and the CreateProcessW launch are compiled too and
        -- reached ONLY by the opt-in [ide-desk] probe (SKIPs unless
        -- ARCANE_IDE_DESK is set) -- same desk-verify rule as ModuleBuild's
        -- Runner and RuntimeLaunch's SpawnDetached above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/IdeLaunch.cpp",
        -- T5 asset file ops (spec 2026-09-30 s7.3/s7.4): the pure planner, the
        -- executor and the undoable commands; AssetFileOpsTest.cpp drives them
        -- over fake facts and a real TempDir (Recycle faked, never the shell).
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/AssetFileOps.cpp",
        -- Node-page phase T2 (s4.6): OsShell -- the one "open / show / open as
        -- text" path helper. Pure halves [editor]-tested; the ShellExecuteW half is
        -- compiled (CrashReportDocument.cpp above links it) but desk-verified.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/OsShell.cpp",
        -- Assets -> Create -> C++ Class: ClassTemplates (name validation, the
        -- project-name -> namespace rule, the three rendered templates as
        -- strings) source-compiles into the test exe so the [editor] units
        -- drive the whole text half directly. Writing/registering/regenerating
        -- is EditorApp's (MintCppClass) and desk-verify.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/ClassTemplates.cpp",
        -- Source include graph: ParseIncludeDirectives / ResolveSourceInclude
        -- are pure, so the [editor] units drive them directly.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/SourceIncludes.cpp",
        -- arcbuild (the game-project build driver, spec docs/specs/
        -- 2026-09-13-arcbuild-driver-design.md): the PURE core -- Action.cpp
        -- (host/action/backend policy), Request.cpp (CLI), Slot.cpp (s4.3 CRT
        -- table, game-module only), ProjectLayout.cpp (path policy), and
        -- Compose.cpp (structured ProcessSpec/ProcessPlan composition -- executable
        -- + argv + cwd per child, never a shell string). Source-compiles into
        -- the test exe so the
        -- [build] units drive it directly, same "pure logic, no spawn" pattern
        -- as ModuleBuild.cpp above. main.cpp (the spawn + PE probe half) is
        -- NOT compiled here; the opt-in [build-desk] cases run the built
        -- arcbuild.exe instead. `--engine` (spec §6) will add a sibling TU,
        -- not grow Slot.cpp.
        "%{wks.location}/Source/Programs/arcbuild/src/Action.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Request.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Slot.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/ProjectLayout.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Compose.cpp",
        -- Orchestration correctness-stage seams: source-compile the bootstrap,
        -- output, probe, pipeline, and filesystem cleaner into the tests so
        -- [build] can drive them through narrow recording fakes. main.cpp
        -- (argv parsing + the real entry point) remains excluded.
        "%{wks.location}/Source/Programs/arcbuild/src/Bootstrap.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Environment.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Output.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Pipeline.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/Probe.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/ProjectCleaner.cpp",
        -- Multibackend hardening review F4: the Ninja single-slot staging
        -- copy (Stage.cpp -- a filesystem op driven over a temp dir) and
        -- BuildExecutor.cpp, whose Build() now runs it after a successful
        -- Ninja plan; the executor is driven through the FakeProcessRunner
        -- + a fake ninja.exe on an overridden PATH, so it still spawns
        -- nothing.
        "%{wks.location}/Source/Programs/arcbuild/src/Stage.cpp",
        "%{wks.location}/Source/Programs/arcbuild/src/BuildExecutor.cpp",
        -- Multibackend hardening Task 3: BackendResolver wraps Arcane::
        -- Toolchain's real tool discovery in std::expected, no process spawn
        -- of its own, so it source-compiles the same "pure logic" way as its
        -- siblings above -- BuildDriverTest.cpp's backend-resolver [build]
        -- units drive it directly.
        "%{wks.location}/Source/Programs/arcbuild/src/Backend.cpp",
        -- Multibackend hardening Task 5: Process.cpp (the real Windows
        -- ProcessRunner + ExecutePlan) DOES now source-compile into the test
        -- exe, unlike its siblings above -- its own [build] cases are
        -- deliberate, narrowly-scoped exceptions to "ordinary [build] tests
        -- never spawn": the pure QuoteWindowsArgument/BuildWindowsCommandLine
        -- algorithm tests spawn nothing, and the process-fixture integration
        -- tests spawn the small, dependency-free arcbuild-process-fixture.exe
        -- (this workspace's own build output, never an external tool) to
        -- prove real CreateProcessW quoting/streaming/handle-inheritance
        -- behavior end to end.
        "%{wks.location}/Source/Programs/arcbuild/src/Process.cpp",
        -- Servitor P0 Task 14 (rev 8.3 V3): the Servitor client (namespace
        -- Servitor -- Format, Client, Verbs and every later client TU) source-
        -- compiles into the test exe the way arcbuild's core does above, so the
        -- [servitor][client] units drive Render/ParseGlobals/Call directly. The
        -- glob covers later client files with no further premake edit;
        -- Source/Programs/Servitor/src/main.cpp sits outside it (the exe's own).
        "%{wks.location}/Source/Programs/Servitor/src/Servitor/**.cpp",
        -- Core-DLL split Plan 1, Task 6: ArcaneServer's own CLI (ServerConfig,
        -- over the same Arcane::Cli arcbuild's Request.cpp above already
        -- source-compiles) and its `--report` census (ServerReport) source-
        -- compile into the test exe so ServerConfigTest.cpp's [server] units
        -- drive ServerConfig::Parse directly, same "pure logic, no spawn"
        -- pattern as Request.cpp. main.cpp (the ProcessContext/Runtime/
        -- PluginHost boot + tick-loop half) is NOT compiled here -- the
        -- opt-in [witness][server] cases spawn the real staged
        -- ArcaneServer.exe instead (ServerWitnessTest.cpp), same "no spawn
        -- test" split ModuleBuild.cpp/RuntimeLaunch.cpp/arcbuild's own
        -- main.cpp already establish.
        "%{wks.location}/Source/Programs/ArcaneServer/src/ServerConfig.cpp",
        "%{wks.location}/Source/Programs/ArcaneServer/src/ServerReport.cpp",
        "%{wks.location}/Source/Programs/ArcaneServer/src/ServerAdminConsole.cpp",
        -- F2b Task 12: CookQueue (the editor's background texture cook --
        -- watcher-triggered, hash-decided, never blocks) source-compiles into
        -- the test exe so the [editor][cook] units drive its queuing/
        -- coalescing logic directly with a manually-pumped SubmitFn fake, no
        -- JobSystem/threading involved -- same "pure logic, no ImGui" pattern
        -- as ModuleBuild.cpp above (Runner's own std::thread half there has
        -- the analogous split; here it is JobSystem::Submit, tested for real
        -- threading separately in JobSystemSubmitTest.cpp).
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/CookQueue.cpp",
        -- F2b desk-checkpoint fix: ContentDiscovery (the mid-session
        -- Content/-drop discovery probe -- recursive .png enumeration plus
        -- the known-paths set difference) source-compiles into the test exe
        -- so the [editor][cook] units drive its PURE halves directly with a
        -- real temp-dir fixture, same pattern as CookQueue.cpp above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/ContentDiscovery.cpp",
        -- F2c Task 13 (embedded-texture extraction at discovery, s5.5/A4):
        -- MeshImportWave (UniqueSiblingPath/ImageFileStem/ExtractEmbeddedTextures)
        -- source-compiles into the test exe so the [editor] units drive it directly
        -- with real temp-dir fixtures and the data/gltf corpus -- no ImGui, no
        -- Project, no device in it at all, same "pure logic" pattern as
        -- CookQueue/ContentDiscovery above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/MeshImportWave.cpp",
        -- F2c Plan 2 final-review C1: MaterialPreviewHarvester source-compiles
        -- into the test exe so MeshThumbnailHarvestTest.cpp can drive TWO REAL
        -- mesh harvests through ONE preview vehicle on a real adapter. NOT a
        -- pure-logic unit (its pure half, FrameMeshBounds, is MeshImportWave.cpp
        -- above) -- it is here for the same reason AssetGraphPanel.cpp is: the
        -- defect it guards (residency cached BY GUID, so harvest #2 drew harvest
        -- #1's geometry under a session-fixed synthetic guid) has no seam short
        -- of the device, so only the picture proves it. Everything it calls
        -- outside its own TU is engine-side (Batcher2D, MeshMaterialCache,
        -- NriGraphContext, ShaderCompiler, Assets) or already listed here
        -- (MeshImportWave), so the link closed with nothing else added.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Project/MaterialPreviewHarvester.cpp",
        -- Asset-manager arc (Plan 1, Task 4): AssetPanelModel (the cached,
        -- foldable model behind the Assets panel's Browse lens) source-
        -- compiles into the test exe so the [editor] units drive it directly
        -- with fake provider lambdas -- no ImGui and no engine facade in it
        -- at all, same "pure logic" pattern as CookQueue/ContentDiscovery
        -- above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetPanelModel.cpp",
        -- Asset-manager arc (Plan 2, Task 3): AssetReferenceIndex (the pure
        -- inverted-reference index over ListAssetReferences answers --
        -- inbound counts, tombstones, dangling targets) source-compiles
        -- into the test exe so the [editor] units drive it directly with
        -- hand-built AssetRef lists, no ImGui and no engine facade in it at
        -- all, same "pure logic" pattern as AssetPanelModel.cpp above. Its
        -- test (AssetReferenceIndexTest.cpp) is globbed already via
        -- Tests/ArcaneTests/src/**.cpp above, same as every other test file.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetReferenceIndex.cpp",
        -- Asset-manager arc (Plan 2, Task 5): AssetActivityLog (the session-
        -- only ring behind the Assets panel's activity feed -- spec s9.2)
        -- source-compiles into the test exe so the [editor] units drive it
        -- directly with a synthetic clock, no ImGui and no engine facade in
        -- it at all, same "pure logic" pattern as AssetPanelModel.cpp/
        -- AssetReferenceIndex.cpp above. Its test (globbed already via
        -- Tests/ArcaneTests/src/**.cpp above) is AssetActivityLogTest.cpp.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetActivityLog.cpp",
        -- Asset-manager arc (Plan 3, Task 2): AssetGraphViewModel (the Graph
        -- lens's pure projection -- scoped/layered/breadth-capped nodes and
        -- edges over an injected entries map + AssetReferenceIndex) source-
        -- compiles into the test exe so the [editor] units drive it directly
        -- with a hand-built entries map and index, no ImGui and no engine
        -- facade in it at all, same "pure logic" pattern as AssetPanelModel/
        -- AssetReferenceIndex/AssetActivityLog above. Its test (globbed
        -- already via Tests/ArcaneTests/src/**.cpp above) is
        -- AssetGraphViewModelTest.cpp.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetGraphViewModel.cpp",
        -- Asset-manager arc (Plan 3, Task 3 -> panel-split Task 7): the three
        -- ASSET PANELS plus their shared unit. NOT pure-logic units like
        -- every entry above them: they are here so AssetsGraphCanvasTest.cpp
        -- can drive the REAL DrawAssetGraphPanel through device-less ImGui
        -- frames, the harness that caught the shader editor's frame-2
        -- EndCreate abort (GraphCanvasHeadlessTest.cpp) -- the
        -- imgui-node-editor canvas only runs inside a live ImGui frame, so no
        -- headless unit can stand in for it. Precedent for compiling a DRAW
        -- TU into the tests for exactly this reason is ShaderEditorDocument.cpp
        -- above; the link closed with nothing else added
        -- (DocumentHost/EditorWidgets/EditorFonts/AssetPanelModel/
        -- AssetReferenceIndex/AssetActivityLog are all already here, and
        -- CreateAssetDialog's half of the surface is header-only).
        --
        -- All four are listed because the link needs all four: the Graph
        -- panel calls into AssetPanelCommon, and AssetPanelCommonTest.cpp
        -- drives RevealAssetInBrowser against AssetBrowserPanel's state. NOTE
        -- the standing caveat -- ArcaneEditor's own project block (above, in
        -- this same file) finds these through premake's src/**.cpp glob, but
        -- THIS list is explicit, so a new Panels/*.cpp is never picked up on
        -- its own.
        --
        -- Panel-split Task 7 removed AssetsPanel.cpp's entry from here with
        -- the file itself: the one "Assets" shell is now three windows, each
        -- owning its own Begin/toolbar/body/bottom bar.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetPanelCommon.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetStatusPanel.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetGraphPanel.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetBrowserPanel.cpp",
        -- T3-D4: the create dialog's unit, for MakeCreateDialogState (the
        -- request -> the dialog's starting state, BeginCreateAsset's whole
        -- seeding), which CreateAssetDialogTest.cpp pins. Its other
        -- dependencies (AssetPanelModel, EditorWidgets, ClassTemplates) are
        -- already compiled here.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/CreateAssetDialog.cpp",
        -- Inspector filters Task 5: the asset source + its page (the old
        -- preview pane's content) and the texture import settings block it
        -- draws. AssetInspectorSourceTest.cpp drives DrawAssetPage through
        -- device-less ImGui frames, the same reason as the panels above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetInspectorSource.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/TextureImportSettings.cpp",
        -- 2D physics wiring Plan 2 (FieldKind::Vector): InspectorView -- the
        -- Inspector's reflected-field visitor, the ImGui half whose PURE
        -- halves (InspectorFields, InspectorMeta) are listed above. NOT a
        -- pure-logic unit: it is here so EditorInspectorVectorTest.cpp can
        -- drive the REAL DrawReflectedComponent through device-less ImGui
        -- frames and click its list controls -- the same reason, and the same
        -- precedent, as AssetGraphPanel.cpp above. The link closed with
        -- nothing else added: every symbol it calls outside its own TU is
        -- already compiled here (InspectorFields/InspectorMeta/EditorWidgets/
        -- ColorPickerPopup/EditGesture/AssetPanelModel) or header-inline
        -- (CreateAssetDialog.hpp's pill text, AssetPanelModel.hpp's three
        -- field-name heuristics).
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/InspectorView.cpp",
        -- Node page + editor upgrades s4.2: the shared asset-reference field
        -- (its pure halves here; the cell joins in T2-B2). InspectorView's
        -- AssetRef arm draws it, AssetReferenceFieldTest.cpp drives it.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Panels/AssetReferenceField.cpp",
        -- Settings arc S3: the settings windows' model/edit/apply/rows/window/host/pages, driven headless by the [settings-ui] units.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/SettingsModel.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/SettingsEdit.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/SettingsApply.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/SettingsRows.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/SettingsWindow.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/SettingsHost.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/ProjectSettingsPage.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorRestart.cpp",
        -- S6-5: assets.sprite.* (Editor audience), read by SpriteDocument::NewSpriteData above.
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/AssetsSpriteSettings.cpp",
        "%{wks.location}/Source/Programs/ArcaneEditor/src/Settings/EditorPerfSettings.cpp",
        -- Crash window plan 2, Task 4: the PURE halves of ArcaneCrashReporter
        -- source-compile into the test exe so the [reporter] units drive them
        -- directly -- same "pure logic, no spawn" pattern as arcbuild's core
        -- above. ReporterMain.cpp (wWinMain), Symbolizer.cpp (dbgeng),
        -- ReporterWindow.cpp (the window) and Monitor.cpp are NOT compiled
        -- here: a second entry point would not link, and the rest is Win32.
        -- Tasks 5, 6, 8 and 9 each append one line to this list
        -- (SymbolizedText.cpp, ReportView.cpp, HangSession.cpp,
        -- MonitorRule.cpp).
        --
        -- NOTE (task 5): this list is NOT gated on the target OS, so every
        -- file named here must compile without windows.h. SymbolizedText.cpp
        -- is the MODEL and its TEXT only; Symbolizer.cpp -- the dbgeng half --
        -- stays out, and the reporter PROJECT above is what carries the
        -- Windows gate.
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/ReporterArgs.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/SymbolizedText.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/ReportView.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/HangSession.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/MonitorRule.cpp",
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src/LogTail.cpp",   -- std-only; CrashReportDocument reads the log tail (node-page phase s8.1)
    }

    includedirs {
        "%{wks.location}/Source/Programs/ArcaneEditor/src",  -- ConsoleBuffer.hpp for the [editor] test
        "%{IncludeDir.nlohmann}",
        "%{IncludeDir.picosha2}",
        "%{IncludeDir.spdlog}",
        "%{IncludeDir.Catch2}",
        "%{IncludeDir.rapidcheck}",
        "%{IncludeDir.rapidcheck_catch}",
        "%{IncludeDir.glm}",
        "%{IncludeDir.stb}",
        "%{IncludeDir.miniaudio}",
        "%{IncludeDir.Astra}",
        "%{IncludeDir.enkiTS}",
        "%{IncludeDir.freetype}",
        "%{IncludeDir.msdfgen}",
        "%{IncludeDir.NRI}",   -- Task 4: NriSubstrateTest.cpp drives nri::Result/Device/nriCreateDevice directly
        "%{IncludeDir.imgui}",
        "%{IncludeDir.imguinodeeditor}",   -- ShaderEditorDocument.cpp (graph canvas, Slice 9)
        "%{IncludeDir.Manifold2D}",
        "%{IncludeDir.Mosaic}",
        "%{IncludeDir.bc7enc_rdo}",   -- Task 4: AssetPipelineImporterTest.cpp drives bc7decomp.h directly for the decode-block sanity test
        "%{IncludeDir.cgltf}",          -- F2c Task 1: VendorSmokeTest.cpp drives cgltf_parse/cgltf_validate directly
        "%{IncludeDir.meshoptimizer}",  -- F2c Task 1: VendorSmokeTest.cpp drives meshopt_generateVertexRemap/optimizeVertexCache directly
        "%{wks.location}/Source/Programs/arcbuild/src",   -- Driver.hpp for the [build] units (arcbuild Task 2)
        "%{wks.location}/Source/Programs/ArcaneServer/src",   -- ServerConfig.hpp/ServerReport.hpp for the [server] units (Core-DLL split Task 6)
        "%{wks.location}/Source/Programs/ArcaneCrashReporter/src",   -- ReporterArgs.hpp for the [reporter] units (crash window plan 2, Task 4)
        "%{wks.location}/Source/Programs/Servitor/src",   -- <Servitor/*.hpp> for the [servitor] units (Servitor P0 Task 14, rev 8.3 V3)
    }

    -- msdfgen, freetype, and NRI are static libs compiled separately; the smoke
    -- test calls them directly (not via ArcaneClient.dll), so all three appear
    -- in links here. Two static copies in different modules is the established
    -- pattern for this workspace. NRI: the [nri] NONE-backend lifecycle test
    -- (Task 4) calls nriCreateDevice/nriDestroyDevice directly, same reasoning.
    -- imgui is NOT linked here: it is exported from ArcaneClient.dll (IMGUI_API =
    -- dllimport below), so the test exe shares the DLL's single GImGui rather
    -- than carrying a second null context. The import lib comes via "ArcaneClient".
    -- imgui-node-editor IS linked (a plain static lib compiled with
    -- IMGUI_API=dllimport, same as this exe): ShaderEditorDocument.cpp's graph
    -- canvas calls it, and that TU source-compiles into the tests.
    -- ArcaneAssetPipeline (Task 1): [pipeline] round-trips .arcart files directly.
    -- bc7enc_rdo (Task 4): ArcaneAssetPipeline links it transitively (see ArcaneEditor's
    -- comment above), and AssetPipelineImporterTest.cpp also calls bc7decomp::unpack_bc7
    -- directly for the decode-block sanity test.
    -- meshoptimizer (F2c Task 1): ArcaneAssetPipeline links it transitively (same
    -- reasoning), and VendorSmokeTest.cpp's own arrival-gate cases call
    -- meshopt_generateVertexRemap/optimizeVertexCache directly.
    links { "Catch2", "rapidcheck", "enkiTS", "freetype", "msdfgen", "NRI", "Manifold2D", "bc7enc_rdo", "meshoptimizer" }

    -- MOSAIC_ENSURE/MOSAIC_ENSURE_ALWAYS (most of AssertRoutingTest.cpp) are
    -- defined UNCONDITIONALLY, outside the MOSAIC_ASSERTS_ACTIVE gate this
    -- define controls (Mosaic/Assert.hpp:227-247) -- those cases were never
    -- at risk of per-config drift. MOSAIC_ASSERT/MOSAIC_VERIFY are what this
    -- define actually keeps alive: they compile out entirely under NDEBUG
    -- (Assert.hpp:193-198) unless forced on, and AssertRoutingTest.cpp's
    -- fatal-guard-class case exercises MOSAIC_ASSERT -- without this define
    -- that ONE case would silently vanish in Release/Dist while the rest of
    -- [assert] kept passing, which is exactly the kind of drift that is easy
    -- to miss. Scoped to this project: the hosts are untouched and keep
    -- their normal per-config behaviour.
    defines { "MOSAIC_ENABLE_ASSERTS" }

    -- The source-compiled Servitor client reads the same define the Servitor
    -- exe does (rev 8.3 V7: HostsDir's source-checkout mapping).
    defines { "SERVITOR_OUTPUTDIR=\"" .. outputdir .. "\"" }

    -- arccook (F2b Task 5) must exist before this project's postbuild runs it.
    -- Servitor (Servitor P0 Task 14) must be built and its closure staged before
    -- ServitorBoundaryTest.cpp ([servitor][boundary]) lists ../Servitor/.
    -- arcbuild-process-fixture (Task 5, multibackend hardening) must exist
    -- before the [build] process-fixture integration tests can spawn it --
    -- this is the ONLY thing that makes that happen for a
    -- `/t:arcbuild,ArcaneTests` build (the fixture is not itself a named
    -- target there).
    dependson { "HotReloadPluginV1", "HotReloadPluginV2", "HotReloadPluginBad",
                "HotReloadPluginInitFail", "ReferenceGameUnderTest", "TemplateSmokePlugin", "arccook",
                "Servitor" }

    -- Task 6 (multibackend hardening): the fixture project exists only for a
    -- Windows target (see its own gate below), so only a Windows generation
    -- may depend on it -- a gmake generation on Linux/macOS would otherwise
    -- name a target that was never emitted. The POSIX process cases in
    -- BuildDriverTest.cpp spawn /bin/sh instead and need no build dependency.
    --
    -- death-fixture (crash window plan 1, task 6): CrashPathTest.cpp's
    -- "death fixture: ..." cases locate it as "../death-fixture/<exe>" the
    -- way BuildDriverTest.cpp locates arcbuild-process-fixture.exe, so it
    -- must exist before this project's tests can run -- on every target
    -- since the Diagnostics POSIX port, unlike the two below.
    --
    -- ArcaneCrashReporter (crash window plan 2, task 4): same reasoning again --
    -- Windows-only by its own gate. CrashPathTest.cpp's `--reporter` case
    -- requires the exe STAGED beside the death fixture (death-fixture's own
    -- dependson + postbuild do that), and the later `reporter:` cases run
    -- "../ArcaneCrashReporter/ArcaneCrashReporter.exe" directly (Task 5).
    -- death-fixture builds for every target since the Diagnostics POSIX port.
    dependson { "death-fixture" }
    -- AbiDigestTest.cpp ([abi]) runs arcane-abi-digest.exe and arcane-abi-digest-mutants.exe
    -- (sibling dirs) for the comment-stability and mutation cases; build them first.
    dependson { "arcane-abi-digest", "arcane-abi-digest-mutants" }
    -- The digest-gate fixture DLLs and their child exe (Plan B Task 2).
    dependson { "AbiDigestBits64", "AbiDigestStale", "AbiDigestOldEpoch", "AbiDigestNoExport", "AbiDigestChild" }
    if os.target() == "windows" then
        dependson { "arcbuild-process-fixture", "ArcaneCrashReporter" }
    end

    -- The test exe loads ArcaneClient.dll from its own directory.
    postbuildcommands(arcane_cmds {
        -- F2b Task 5: cook FIRST, then stage the cooked artifacts -- same cook-then-copy
        -- shape as ArcaneRuntime's/ArcaneEditor's matching comments above (safe under /m:
        -- idempotent by hash, concurrent same-key writes atomic-rename + deterministic).
        -- Unlike those two hosts, this project does NOT blanket-copy the whole
        -- ReferenceProject tree (see the targeted Verify/-only COPYDIR below), so this
        -- line is this exe's ONLY source of a staged Intermediate/Artifacts.
        '"%{wks.location}/Binaries/' .. outputdir .. '/arccook/' .. arcane_exe("arccook") .. '" --project "%{wks.location}/ReferenceProject"',
        arcane_copydir("%{wks.location}/ReferenceProject/Intermediate/Artifacts", "%{cfg.buildtarget.directory}/ReferenceProject/Intermediate/Artifacts"),
        arcane_copydir("%{wks.location}/data/shaders/generated", "%{cfg.buildtarget.directory}/data/shaders"),
        -- Material TEMPLATE SOURCES (not compiled artifacts): the material
        -- pipeline stitches + runtime-compiles these via ShaderSourceProvider.
        '{MKDIR} "%{cfg.buildtarget.directory}/data/shaders/materials"',
        arcane_copydir("%{wks.location}/data/shaders/materials", "%{cfg.buildtarget.directory}/data/shaders/materials"),
        '{MKDIR} "%{cfg.buildtarget.directory}/data/EditorThemes"',
        arcane_copydir("%{wks.location}/data/EditorThemes", "%{cfg.buildtarget.directory}/data/EditorThemes"),
        '{MKDIR} "%{cfg.buildtarget.directory}/data/fonts"',
        '{COPYFILE} "%{wks.location}/data/font/roboto/static/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/fonts/Roboto-Regular.ttf"',
        -- The editor's bundled fonts at the editor's exe-relative paths, so
        -- EditorFontsTest's InstallEditorFonts falls back to the real bundled
        -- faces (mirrors ArcaneEditor's font lines).
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/lucide"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/inter/static"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/roboto/static"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/aldotheapache"',
        '{MKDIR} "%{cfg.buildtarget.directory}/data/font/jetbrainsmono"',
        '{COPYFILE} "%{wks.location}/data/font/inter/static/Inter_18pt-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/inter/static/Inter_18pt-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/roboto/static/Roboto-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/roboto/static/Roboto-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/lucide/lucide.ttf" "%{cfg.buildtarget.directory}/data/font/lucide/lucide.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/aldotheapache/AldotheApache.ttf" "%{cfg.buildtarget.directory}/data/font/aldotheapache/AldotheApache.ttf"',
        '{COPYFILE} "%{wks.location}/data/font/jetbrainsmono/JetBrainsMono-Regular.ttf" "%{cfg.buildtarget.directory}/data/font/jetbrainsmono/JetBrainsMono-Regular.ttf"',
        '{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/HotReloadPluginV1/' .. arcane_module("HotReloadPluginV1") .. '" "%{cfg.buildtarget.directory}/' .. arcane_module("HotReloadPluginV1") .. '"',
        '{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/HotReloadPluginV2/' .. arcane_module("HotReloadPluginV2") .. '" "%{cfg.buildtarget.directory}/' .. arcane_module("HotReloadPluginV2") .. '"',
        '{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/HotReloadPluginBad/' .. arcane_module("HotReloadPluginBad") .. '" "%{cfg.buildtarget.directory}/' .. arcane_module("HotReloadPluginBad") .. '"',
        '{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/HotReloadPluginInitFail/' .. arcane_module("HotReloadPluginInitFail") .. '" "%{cfg.buildtarget.directory}/' .. arcane_module("HotReloadPluginInitFail") .. '"',
        '{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/ReferenceGameUnderTest/' .. arcane_module("ReferenceGameUnderTest") .. '" "%{cfg.buildtarget.directory}/' .. arcane_module("ReferenceGameUnderTest") .. '"',
        '{COPYFILE} "%{wks.location}/Binaries/' .. outputdir .. '/TemplateSmokePlugin/' .. arcane_module("TemplateSmokePlugin") .. '" "%{cfg.buildtarget.directory}/' .. arcane_module("TemplateSmokePlugin") .. '"',
        -- Test data fixtures: copy Tests/ArcaneTests/data's CONTENTS into the test output
        -- dir's data/ so tests find their fixtures by relative path. {COPYDIR}
        -- copies the directory's contents, merging with the data/fonts dir the
        -- lines above create. (The M6 physics_oracle fixtures were retired in
        -- v2 T8; physics_feel_reference/ now holds the Phase-B Lua feel traces.)
        arcane_copydir("%{wks.location}/Tests/ArcaneTests/data", "%{cfg.buildtarget.directory}/data"),
        -- Playwright's own image-comparison fixture corpus (Task 6): the
        -- CONFORMANCE ORACLE for ImageCompare. ImageCompareConformanceTest.cpp
        -- walks this tree by a RELATIVE path from the exe's own directory.
        arcane_copydir("%{wks.location}/ThirdParty/playwright-fixtures", "%{cfg.buildtarget.directory}/playwright-fixtures"),
        -- Task 10 (plan-b comparator): ONLY the committed layout seed, not
        -- the whole ReferenceProject tree the two HOST exes stage (this
        -- project's own COPYDIR two screens up, and ArcaneEditor's/
        -- ArcaneRuntime's matching lines) -- GoldenImageTest.cpp's
        -- "[golden]" case is a CHEAP, no-GPU text check of this ONE file and
        -- nothing else under ReferenceProject/Saved/ is read from this exe.
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Saved"',
        '{COPYFILE} "%{wks.location}/ReferenceProject/Saved/verify-layout.ini" "%{cfg.buildtarget.directory}/ReferenceProject/Saved/verify-layout.ini"',
        -- Task C (F2c debts): symmetry with the two host exes' Content/Source/Verify wipe
        -- (see ArcaneRuntime's matching comment above, incl. its NOT-mirrored list -- not a
        -- closed one -- and Task F's {MKDIR}-before-{RMDIR} guard) -- a reference PNG
        -- removed/renamed in SOURCE must not linger in this exe's staged Verify/ either.
        '{MKDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        '{RMDIR} "%{cfg.buildtarget.directory}/ReferenceProject/Verify"',
        -- Task 11 (plan-b comparator): the engine trap corpus. Task 10's own
        -- comment (above, now narrowed) predicted this would need widening
        -- "once Task 12's [gpu][golden] cases want staged reference images" --
        -- it arrived one task early instead, because ImageCompareConformance
        -- Test.cpp's trap case (Step 3) reads ReferenceProject/Verify/Traps/
        -- by a relative path from this exe's own directory, same as the
        -- playwright-fixtures COPYDIR above. Staged as a NAMED subtree
        -- (Verify/, not the whole ReferenceProject) rather than mirroring the
        -- host exes' full {COPYDIR}: nothing else under ReferenceProject/ is
        -- read from this exe today. When Task 12 lands its own References/
        -- images under this same Verify/ tree, this one line already covers
        -- them -- no further widening needed.
        arcane_copydir("%{wks.location}/ReferenceProject/Verify", "%{cfg.buildtarget.directory}/ReferenceProject/Verify"),
        -- The exclusion list is repo-level config that BOTH consumers read: this
        -- suite (ExclusionExpiryTest) and golden-gate.ps1. Staged beside the exe
        -- because tests run FROM the exe dir. An edit therefore needs a rebuild
        -- to take effect here, same as data/ and playwright-fixtures/ above.
        '{COPYFILE} "%{wks.location}/scripts/automation-exclusions.json" "%{cfg.buildtarget.directory}/automation-exclusions.json"',
    })
    postbuildcommands(arcane_vendor_runtime())

    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
        -- imgui is exported from ArcaneClient.dll; this exe imports it (matches the
        -- DLL's dllexport so <imgui.h> here resolves to the DLL's symbols).
        "IMGUI_API=" .. ARCANE_IMGUI_IMPORT,
    }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/Zc:__cplusplus", "/bigobj" }
        fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        -- ws2_32: ArcaneCore TcpSocket. d3d12/dxgi/dxguid: this exe's own
        -- statically-linked NRI.lib copy (Task 4's [nri] NONE-backend test
        -- links NRI directly, same "two static copies" reasoning as the
        -- includedirs/links comment above) pulls in NRI's D3D12 backend
        -- object code, which references D3D12CreateDevice/CreateDXGIFactory2/
        -- WKPDID_D3DDebugObjectName etc. even though the test never exercises
        -- the D3D12 backend -- mirrors ArcaneClient's own system-lib set.
        links { "ws2_32", "d3d12", "dxgi", "dxguid" }

    filter "system:not windows"
        links { "dl", "pthread" }

    filter "configurations:Debug"
        defines { "ARC_BUILD_DEBUG" }
        runtime "Debug"
        symbols "on"
        -- /Zi, not /ZI (input-seam gate): under Edit and Continue MSVC spells
        -- __LINE__ as a per-function `__LINE__Var` + offset, and the linker folds
        -- that symbol across TUs for SAME-NAMED internal-linkage functions --
        -- which every Catch2 TEST_CASE is (CATCH2_INTERNAL_TEST_<__COUNTER__>,
        -- restarting at 0 in each TU). A failing assertion then reported another
        -- file's function base plus its own offset: lines that do not exist in the
        -- file. Nothing edits-and-continues a test exe, so the cost is nil.
        editandcontinue "Off"

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

-- ============================================================================
-- Hot-reload TEST plugins: one source, four DLLs (V1 step=1, V2 step=10,
-- Bad ABI, InitFail = OnInit registers its factories then returns false --
-- PluginHost's secondary-init-failure teardown fixture, final-review fix wave C1). SharedLib, /MD. ArcaneCore is a DLL since the Core-DLL split:
-- exactly one copy per process BY CONSTRUCTION, and it is ArcaneCore.dll --
-- so a module links BOTH engine import libs, exactly as build/arcane.lua does
-- for a real game module (spec 2026-09-15 s1.2).
-- Loaded at runtime by PluginHost in ArcaneTests; never linked by the test exe.
-- Built ON Arcane/Plugin/GameModule.hpp (ARC_GAME_MODULE_ABI) -- the macro's plugin test vehicle.
-- ============================================================================
-- macOS: dyld never unloads an image that holds thread-local variables (a TLV
-- has no per-image teardown), so dlclose left every loaded module mapped and
-- its static registrations alive -- the Mach-O twin of GCC's STB_GNU_UNIQUE
-- (see -fno-gnu-unique below). spdlog is header-only here, and its thread_id()
-- caches the id in a function-local static thread_local that every module
-- that logs instantiates; SPDLOG_NO_TLS asks the OS each call instead (the
-- same id, uncached). The post-build check turns any TLV that remains into a
-- build error naming its symbols, rather than a module that silently never
-- leaves. Call inside a project, with no filter active.
local function macos_unloadable_module()
    filter "system:macosx"
        defines { "SPDLOG_NO_TLS" }
        postbuildcommands {
            'if nm -m "%{cfg.buildtarget.abspath}" | grep -E "__thread_(vars|data|bss)"; then ' ..
            'echo "error: %{prj.name}: thread-local storage in a loaded module (symbols above) -- dyld never unloads such an image, so hot reload cannot replace it" >&2; exit 1; fi'
        }
    filter {}
end

-- A game-type fixture's engine surface, from its .arcmodule descriptor through
-- the SAME reader build/arcane.lua uses for a real game (engine modularization
-- s4): link + include only the descriptor's closure, ARC_GAME_HAS_IMGUI and the
-- ImGui import decoration only when ImGui is in it. Call inside a project.
local function game_fixture_modules(descriptor, sourceRoot, handWrittenExports)
    -- handWrittenExports: the fixture hand-writes its GamePlugin_* exports (the
    -- AbiDigest* fixtures must NOT use ARC_GAME_MODULE), so the descriptor's
    -- "exactly one ARC_GAME_MODULE" source check does not apply.
    local game = arc_game_descriptor(engine_modules, path.join(_MAIN_SCRIPT_DIR, descriptor), (not handWrittenExports) and path.join(_MAIN_SCRIPT_DIR, sourceRoot) or nil)
    if not game then error("[arcmodule] schema: missing game descriptor " .. descriptor, 0) end
    arc_module_consumer(engine_modules, game.modules, { stage = false })   -- loaded by a host that carries the modules
    for _, include in ipairs(game.thirdPartyIncludes) do includedirs { "%{wks.location}/" .. include } end
    if arc_closure_contains(engine_modules, game.modules, "ImGui") then
        -- IMGUI_API=dllimport: adopt ImGui.dll's single GImGui, exactly as
        -- arcane.lua does for a real module.
        defines { "ARC_GAME_HAS_IMGUI", "IMGUI_API=" .. ARCANE_IMGUI_IMPORT }
    end
end

local function test_plugin(name, defs)
    group "Tests/Fixtures"
    project(name)
        -- One descriptor for the four builds of the one source.
        game_fixture_modules("Tests/Fixtures/HotReloadPlugin/HotReloadPlugin.arcmodule", "Tests/Fixtures/HotReloadPlugin")
        location "Tests/Fixtures/HotReloadPlugin"
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"
        targetname(name)
        -- A LOADED module is Name.so, never libName.so: the host and the tests
        -- name it Name + the platform extension (see arcane_module above).
        filter "system:not windows"
            targetprefix ""
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
        filter { "system:not windows", "toolset:gcc" }
            -- GCC still emits STB_GNU_UNIQUE for a few std:: objects, and ONE such
            -- symbol makes glibc refuse to unmap the module on dlclose; a PE module
            -- is gone after FreeLibrary, and hot reload is built on that.
            buildoptions { "-fno-gnu-unique" }
        filter {}
        macos_unloadable_module()
        targetdir ("Binaries/" .. outputdir .. "/" .. name)
        objdir ("Intermediate/Build/" .. outputdir .. "/" .. name)
        files { "%{prj.location}/HotReloadPlugin.cpp", "%{prj.location}/HotReloadShared.hpp" }
        -- The include surface is the descriptor closure's (game_fixture_modules
        -- above), exactly what build/arcane.lua gives a game module.
        defines (defs)
        filter "system:windows"
            systemversion "latest"
            buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj" }   -- /utf-8: spdlog/fmt via Log.hpp, as arcane.lua sets
            fatalwarnings { "4715" }   -- falling off a value-returning function is UB, not a warning
        filter "configurations:Debug"   defines { "ARC_BUILD_DEBUG" }                    runtime "Debug"   symbols "on"
        filter "configurations:Release" defines { "ARC_BUILD_RELEASE", "NDEBUG" }        runtime "Release" optimize "speed" symbols "on"
        filter "configurations:Dist"    defines { "ARC_BUILD_DIST", "NDEBUG" }           runtime "Release" optimize "speed" symbols "off"
        filter {}
end

test_plugin("HotReloadPluginV1",  { "HOTRELOAD_STEP=1",          "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginV2",  { "HOTRELOAD_STEP=10",         "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginBad", { "HOTRELOAD_ABI_OFFSET=999",  "_CRT_SECURE_NO_WARNINGS" })
test_plugin("HotReloadPluginInitFail", { "HOTRELOAD_INIT_FAIL=1",  "_CRT_SECURE_NO_WARNINGS" })

-- ============================================================================
-- AbiDigestBits64 / AbiDigestStale / AbiDigestOldEpoch / AbiDigestNoExport /
-- AbiDigestChild (3D-readiness Plan B Task 2): the plugin ABI-digest load gate's
-- refusal witnesses. REAL game-module DLLs built from their ArcaneEngine-only
-- descriptors (so their digest folds the committed Input / Client sub-digests,
-- Plugin/AbiManifestGame.hpp). Bits64 and Stale have the host's epoch and a
-- wrong digest (Bits64 is compiled with ASTRA_ENTITY_BITS=64 so its own
-- AbiGameMix() differs; Stale returns a constant). OldEpoch (the previous epoch)
-- and NoExport (the current epoch) export no digest at all: they pin the
-- resolve order (epoch first, then the required digest export). A disposable
-- child exe calls Plugin::Load on the path it is given. ArcaneTests spawns the
-- child once per DLL ([abi]); no fixture DLL is ever loaded into
-- ArcaneTests.exe (the 21 game headers' registrars and a 64-bit Entity would
-- corrupt its TypeContext).
-- ============================================================================
local function abi_digest_fixture(name, defs)
    group "Tests/Fixtures"
    project(name)
        game_fixture_modules("Tests/Fixtures/" .. name .. "/" .. name .. ".arcmodule", "Tests/Fixtures/" .. name, true)
        location("Tests/Fixtures/" .. name)
        kind "SharedLib"
        language "C++"
        cppdialect "C++23"
        staticruntime "off"
        targetname(name)
        filter "system:not windows"
            targetprefix ""
            visibility "Hidden"
            inlinesvisibility "Hidden"
        filter { "system:not windows", "toolset:gcc" }
            buildoptions { "-fno-gnu-unique" }
        filter {}
        macos_unloadable_module()
        targetdir ("Binaries/" .. outputdir .. "/" .. name)
        objdir ("Intermediate/Build/" .. outputdir .. "/" .. name)
        files { "%{prj.location}/" .. name .. ".cpp" }
        defines (defs)
        filter "system:windows"
            systemversion "latest"
            buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj" }
            fatalwarnings { "4715" }
        filter "configurations:Debug"   defines { "ARC_BUILD_DEBUG" }                    runtime "Debug"   symbols "on"
        filter "configurations:Release" defines { "ARC_BUILD_RELEASE", "NDEBUG" }        runtime "Release" optimize "speed" symbols "on"
        filter "configurations:Dist"    defines { "ARC_BUILD_DIST", "NDEBUG" }           runtime "Release" optimize "speed" symbols "off"
        filter {}
end

abi_digest_fixture("AbiDigestBits64", { "ASTRA_ENTITY_BITS=64", "_CRT_SECURE_NO_WARNINGS" })
abi_digest_fixture("AbiDigestStale",  { "_CRT_SECURE_NO_WARNINGS" })
abi_digest_fixture("AbiDigestOldEpoch", { "_CRT_SECURE_NO_WARNINGS" })
abi_digest_fixture("AbiDigestNoExport", { "_CRT_SECURE_NO_WARNINGS" })

group "Tests/Fixtures"
project "AbiDigestChild"
    -- ArcanePhysics2D is STAGED beside the child, not linked (3D-readiness Plan A
    -- Task 8 fix round 1): the --sim-hook mode loads a game module that imports it,
    -- so it maps after ProcessContext::Create sealed the sim hooks -- the
    -- [physics][hook] witness that a plugin-loaded hook module is refused.
    arc_module_consumer(engine_modules, { "ArcaneEngine" }, { stage = { "ArcaneEngine", "ArcanePhysics2D" } })
    location "Tests/Fixtures/AbiDigestChild"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"

    targetdir ("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir ("Intermediate/Build/" .. outputdir .. "/%{prj.name}")

    files { "%{prj.location}/AbiDigestChild.cpp" }

    defines { "_CRT_SECURE_NO_WARNINGS", "NOMINMAX", "WIN32_LEAN_AND_MEAN" }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj" }
    filter "system:not windows"
        links { "dl", "pthread" }
    filter "configurations:Debug"   defines { "ARC_BUILD_DEBUG" }                    runtime "Debug"   symbols "on"
    filter "configurations:Release" defines { "ARC_BUILD_RELEASE", "NDEBUG" }        runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"    defines { "ARC_BUILD_DIST", "NDEBUG" }           runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- ReferenceGameUnderTest: ReferenceProject's REAL game-module sources
-- (ReferenceProject/Source/Game/**), compiled as a test plugin with the
-- include surface build/arcane.lua gives a game module. The [trajectory]
-- case (Tests/ArcaneTests/src/ReferencePlayerTrajectoryTest.cpp) loads it through a
-- real PluginHost and replays a scripted input run. Because it compiles the
-- sources IN PLACE, the same unchanged test proves a rewrite of those
-- sources kept the gameplay bit-identical (input-seam spec s8 T6).
-- ============================================================================
group "Tests/Fixtures"
project "ReferenceGameUnderTest"
    -- ReferenceProject's OWN descriptor: the fixture links what the real game links.
    game_fixture_modules("ReferenceProject/Source/Game/ReferenceGame.arcmodule", "ReferenceProject/Source/Game")
    location "Tests/Fixtures/ReferenceGameUnderTest"   -- the project file only: it compiles ReferenceProject/Source/Game in place
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetname "ReferenceGameUnderTest"
    filter "system:not windows"
        targetprefix ""   -- a loaded module: Name.so (see arcane_module)
        visibility "Hidden"           -- a loaded module's statics stay its own (see test_plugin above)
        inlinesvisibility "Hidden"
    filter { "system:not windows", "toolset:gcc" }
        -- GCC still emits STB_GNU_UNIQUE for a few std:: objects, and ONE such
        -- symbol makes glibc refuse to unmap the module on dlclose; a PE module
        -- is gone after FreeLibrary, and hot reload is built on that.
        buildoptions { "-fno-gnu-unique" }
    filter {}
    macos_unloadable_module()
    targetdir ("Binaries/" .. outputdir .. "/ReferenceGameUnderTest")
    objdir ("Intermediate/Build/" .. outputdir .. "/ReferenceGameUnderTest")
    files {
        "%{wks.location}/ReferenceProject/Source/Game/**.cpp",
        "%{wks.location}/ReferenceProject/Source/Game/**.hpp",
    }
    includedirs { "%{wks.location}/ReferenceProject/Source/Game" }   -- engine + vendor roots: the descriptor closure
    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }
    filter "system:windows"
        systemversion "latest"
        -- The flags build/arcane.lua gives a real module (AVX2 included: inline
        -- header codegen shared across the DLL boundary must agree).
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        fatalwarnings { "4715" }
    filter "configurations:Debug"   defines { "ARC_BUILD_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release" defines { "ARC_BUILD_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"    defines { "ARC_BUILD_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}

-- ============================================================================
-- TemplateSmokePlugin: the editor's C++ class templates (Source/Programs/ArcaneEditor/src/
-- Project/ClassTemplates.cpp) rendered for Component SmokeComponent + System
-- SmokeSystem in project TemplateSmoke, checked in under
-- Tests/Fixtures/TemplateSmokePlugin and compiled as a real game module with
-- the include surface build/arcane.lua gives one. ClassTemplatesTest asserts
-- those files ARE Render(...)'s output byte for byte, so a template that
-- stops compiling fails THIS build (input-seam spec s8 T8), and loads + runs
-- the module through a real PluginHost.
-- ============================================================================
group "Tests/Fixtures"
project "TemplateSmokePlugin"
    game_fixture_modules("Tests/Fixtures/TemplateSmokePlugin/TemplateSmokePlugin.arcmodule", "Tests/Fixtures/TemplateSmokePlugin")
    location "Tests/Fixtures/TemplateSmokePlugin"
    kind "SharedLib"
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetname "TemplateSmokePlugin"
    filter "system:not windows"
        targetprefix ""   -- a loaded module: Name.so (see arcane_module)
        visibility "Hidden"           -- a loaded module's statics stay its own (see test_plugin above)
        inlinesvisibility "Hidden"
    filter { "system:not windows", "toolset:gcc" }
        -- GCC still emits STB_GNU_UNIQUE for a few std:: objects, and ONE such
        -- symbol makes glibc refuse to unmap the module on dlclose; a PE module
        -- is gone after FreeLibrary, and hot reload is built on that.
        buildoptions { "-fno-gnu-unique" }
    filter {}
    macos_unloadable_module()
    targetdir ("Binaries/" .. outputdir .. "/TemplateSmokePlugin")
    objdir ("Intermediate/Build/" .. outputdir .. "/TemplateSmokePlugin")
    files {
        "%{prj.location}/**.cpp",
        "%{prj.location}/**.hpp",
    }
    includedirs { "%{prj.location}" }   -- engine + vendor roots: the descriptor closure
    defines {
        "_CRT_SECURE_NO_WARNINGS",
        "_SILENCE_STDEXT_ARR_ITERS_DEPRECATION_WARNING",
    }
    filter "system:windows"
        systemversion "latest"
        -- The flags build/arcane.lua gives a real module (AVX2 included: inline
        -- header codegen shared across the DLL boundary must agree).
        buildoptions { "/utf-8", "/Zc:__cplusplus", "/bigobj", "/arch:AVX2" }
        fatalwarnings { "4715" }
    filter "configurations:Debug"   defines { "ARC_BUILD_DEBUG" }             runtime "Debug"   symbols "on"
    filter "configurations:Release" defines { "ARC_BUILD_RELEASE", "NDEBUG" } runtime "Release" optimize "speed" symbols "on"
    filter "configurations:Dist"    defines { "ARC_BUILD_DIST", "NDEBUG" }    runtime "Release" optimize "speed" symbols "off"
    filter {}

group ""
