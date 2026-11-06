-- The single reader for engine and external-project .arcmodule descriptors.
local modules_dir = _SCRIPT_DIR

ARC_THIRDPARTY = {
    nlohmann={include="ThirdParty/nlohmann"}, picosha2={include="ThirdParty/picosha2"},
    spdlog={include="ThirdParty/spdlog/include"}, Catch2={include="ThirdParty/Catch2/src",link="Catch2"},
    rapidcheck={include="ThirdParty/rapidcheck/include",link="rapidcheck"},
    rapidcheck_catch={include="ThirdParty/rapidcheck/extras/catch/include"},
    glm={include="ThirdParty/glm"}, stb={include="ThirdParty/stb"},
    miniaudio={include="ThirdParty/miniaudio"}, Astra={include="ThirdParty/Astra/include"},
    enkiTS={include="ThirdParty/enkiTS/src",link="enkiTS"}, tracy={include="ThirdParty/tracy/public",link="tracy"},
    freetype={include="ThirdParty/freetype/include",link="freetype"},
    msdfgen={include="ThirdParty/msdfgen",link="msdfgen"}, NRI={include="ThirdParty/NRI/Include",link="NRI"},
    VulkanHeaders={include="ThirdParty/Vulkan-Headers/include"},
    DirectXHeaders={include="ThirdParty/DirectX-Headers/include"},
    VMA={include="ThirdParty/VMA/include"}, D3D12MA={include="ThirdParty/D3D12MA/include"},
    imgui={include="ThirdParty/imgui",link="imgui"},
    imguinodeeditor={include="ThirdParty/imgui-node-editor",link="imgui-node-editor"},
    Manifold2D={include="ThirdParty/Manifold2D/include",link="Manifold2D"},
    Mosaic={include="ThirdParty/Mosaic/include"},
    bc7enc_rdo={include="ThirdParty/bc7enc_rdo",link="bc7enc_rdo"},
    cgltf={include="ThirdParty/cgltf"}, meshoptimizer={include="ThirdParty/meshoptimizer/src",link="meshoptimizer"},
}

-- SDL3 discovery (the prebuilt vendor module's @SDL3_*@ placeholders,
-- ThirdParty/SDL3/SDL3.arcmodule), shared by the engine workspace and every SDK
-- consumer that links SDL3 (a packaged game). Windows: the vcpkg overlay
-- triplet's SDL3 (VCPKG_ROOT required). Linux/macOS: SDL3_ROOT or pkg-config.
-- Idempotent; a value already set wins.
function arc_discover_sdl3()
    if SDL3_INCLUDE_DIR then return end
    VCPKG_ROOT = os.getenv("VCPKG_ROOT")
    if os.target() == "windows" then
        -- scripts\generate.bat's rule: an unset VCPKG_ROOT falls back to C:\vcpkg
        -- when vcpkg is installed there (the hosted runners), so arcbuild's own
        -- premake run (`arcbuild package`) resolves SDL3 the way the engine build does.
        if not VCPKG_ROOT and os.isfile("C:/vcpkg/vcpkg.exe") then VCPKG_ROOT = "C:/vcpkg" end
        if not VCPKG_ROOT then
            error("VCPKG_ROOT environment variable is not set.\nSet it to your vcpkg installation directory, e.g.:\n  setx VCPKG_ROOT C:\\vcpkg\nThen restart your terminal and generate again.")
        end
        VCPKG_INSTALLED_MD = VCPKG_ROOT .. "/installed/x64-windows-static-md"
        SDL3_PREFIX      = VCPKG_INSTALLED_MD
        SDL3_INCLUDE_DIR = VCPKG_INSTALLED_MD .. "/include"
    else
        -- `pkg-config --variable=includedir sdl3` names the include ROOT (the one
        -- holding SDL3/SDL.h), which is what every <SDL3/...> include expects.
        -- SDL3_ROOT (an install prefix) overrides it; neither -> the compiler's
        -- default search path, which already covers /usr/include and /usr/local/include.
        local sdlRoot = os.getenv("SDL3_ROOT")
        if sdlRoot then
            SDL3_INCLUDE_DIR = sdlRoot .. "/include"
            SDL3_LIB_DIR     = sdlRoot .. "/lib"
        else
            local inc = os.outputof("pkg-config --variable=includedir sdl3 2>/dev/null")
            local lib = os.outputof("pkg-config --variable=libdir sdl3 2>/dev/null")
            if inc and inc ~= "" then SDL3_INCLUDE_DIR = inc end
            if lib and lib ~= "" then SDL3_LIB_DIR = lib end
        end
        SDL3_INCLUDE_DIR = SDL3_INCLUDE_DIR or "/usr/local/include"
    end
end

-- A prebuilt vendor module's discovery, run the first time its entry is
-- resolved -- so a consumer that never links SDL3 (a server) never needs vcpkg.
ARC_PREBUILT_DISCOVERY = { SDL3 = arc_discover_sdl3 }

local function fail(check, detail)
    error("[arcmodule] " .. check .. ": " .. detail, 0)
end

local function keys(value, allowed, file, field)
    if type(value) ~= "table" then fail("schema", file .. ": " .. field .. " must be an object") end
    for key in pairs(value) do
        if not allowed[key] then fail("schema", file .. ": unknown key " .. field .. "." .. tostring(key)) end
    end
end

local function strings(value, file, field)
    if type(value) ~= "table" then fail("schema", file .. ": " .. field .. " must be an array") end
    local count = 0
    for key, item in pairs(value) do
        if type(key) ~= "number" or key < 1 or key % 1 ~= 0 or type(item) ~= "string" or item == "" then
            fail("schema", file .. ": " .. field .. " must be a string array")
        end
        count = count + 1
    end
    if count ~= #value then fail("schema", file .. ": " .. field .. " must be a dense array") end
end

local types = {Runtime=true, Client=true, Developer=true, Editor=true, Game=true, Program=true}
local allowed_edges = {
    Runtime={Runtime=true}, Client={Runtime=true,Client=true},
    Developer={Runtime=true,Client=true,Developer=true,Game=true,Program=true},
    Editor={Runtime=true,Client=true,Developer=true,Editor=true,Game=true,Program=true},
    Game={Runtime=true,Client=true},
    Program={Runtime=true,Client=true,Developer=true,Editor=true,Game=true,Program=true},
}

local function suffix(name)
    -- "ImGui" is one word in the macro (ArcaneImGui -> ARC_IMGUI_API, not ARC_IM_GUI_API).
    local short = (name:match("^Arcane(.+)$") or name):gsub("ImGui", "Imgui")
    return short:gsub("(%l)(%u)", "%1_%2"):gsub("(%u)(%u%l)", "%1_%2"):upper()
end

local function api_macro(mod)
    return mod.apiMacro or ("ARC_" .. suffix(mod.name) .. "_API")
end

local function load_one(file)
    local raw = io.readfile(file)
    if not raw then fail("schema", file .. ": cannot read descriptor") end
    local doc, err = json.decode(raw)
    if not doc then fail("schema", file .. ": " .. tostring(err)) end
    keys(doc, {name=true,type=true,dependencies=true,thirdParty=true,platforms=true,apiMacro=true,apiHeader=true,vendor=true,prebuilt=true}, file, "descriptor")
    if doc.vendor ~= nil and type(doc.vendor) ~= "boolean" then fail("schema", file .. ": vendor must be boolean") end
    if doc.prebuilt ~= nil then
        -- A PREBUILT vendor module: a library the engine links but never builds
        -- (no premake project). Per platform: include dir, link name, libdir,
        -- the runtime file(s) staged beside a program, rpath (ELF/Mach-O), and a
        -- "debug" override of libdir/runtime for the Debug configuration.
        -- A value may name a premake5.lua global as @NAME@ (the discovery stays
        -- there); an unresolved one drops the field.
        if not doc.vendor then fail("schema", file .. ": prebuilt requires vendor=true") end
        keys(doc.prebuilt, {windows=true,linux=true,macosx=true}, file, "prebuilt")
        for platform, entry in pairs(doc.prebuilt) do
            local field = "prebuilt." .. platform
            keys(entry, {include=true,link=true,libdir=true,runtime=true,rpath=true,debug=true}, file, field)
            for _, key in ipairs({"include", "link", "libdir"}) do
                if entry[key] ~= nil and (type(entry[key]) ~= "string" or entry[key] == "") then
                    fail("schema", file .. ": " .. field .. "." .. key .. " must be a string")
                end
            end
            if entry.runtime ~= nil then strings(entry.runtime, file, field .. ".runtime") end
            if entry.rpath ~= nil and type(entry.rpath) ~= "boolean" then
                fail("schema", file .. ": " .. field .. ".rpath must be boolean")
            end
            if entry.debug ~= nil then
                keys(entry.debug, {libdir=true,runtime=true}, file, field .. ".debug")
                if entry.debug.libdir ~= nil and (type(entry.debug.libdir) ~= "string" or entry.debug.libdir == "") then
                    fail("schema", file .. ": " .. field .. ".debug.libdir must be a string")
                end
                if entry.debug.runtime ~= nil then strings(entry.debug.runtime, file, field .. ".debug.runtime") end
            end
        end
    end
    if type(doc.name) ~= "string" or doc.name ~= path.getbasename(file) then
        fail("schema", file .. ": name must equal descriptor stem")
    end
    if not types[doc.type] then fail("schema", file .. ": invalid type " .. tostring(doc.type)) end
    if doc.apiMacro and (doc.name ~= "ArcaneCore" or doc.apiMacro ~= "ARC_CORE_API") then
        fail("schema", file .. ": apiMacro override is reserved for ArcaneCore")
    end
    if doc.apiHeader ~= nil and (type(doc.apiHeader) ~= "string" or doc.apiHeader == "" or path.isabsolute(doc.apiHeader)) then
        fail("schema", file .. ": apiHeader must be a relative path")
    end
    for _, field in ipairs({"dependencies", "thirdParty"}) do
        keys(doc[field], {public=true,private=true}, file, field)
        for _, visibility in ipairs({"public", "private"}) do
            strings(doc[field][visibility], file, field .. "." .. visibility)
        end
    end
    if doc.platforms ~= nil then
        keys(doc.platforms, {windows=true,linux=true,macosx=true}, file, "platforms")
        for platform, settings in pairs(doc.platforms) do
            keys(settings, {links=true,defines=true}, file, "platforms." .. platform)
            for field, list in pairs(settings) do strings(list, file, "platforms." .. platform .. "." .. field) end
        end
    end
    for _, visibility in ipairs({"public", "private"}) do
        for _, vendor in ipairs(doc.thirdParty[visibility]) do
            if not ARC_THIRDPARTY[vendor] then fail("schema", file .. ": unknown thirdParty " .. vendor) end
        end
    end
    doc.file = file
    doc.root = path.getdirectory(file)
    return doc
end

function arc_modules_load(roots)
    local mods = {byName={},order={}}
    local files, seen, relative = {}, {}, {}
    for _, root in ipairs(roots) do
        local found = {}
        if root:match("%.arcmodule$") then found = {root}
        else found = os.matchfiles(path.join(root, "**.arcmodule")) end
        for _, file in ipairs(found) do
            file = path.translate(file, "/")
            if not seen[file] then
                files[#files+1] = file; seen[file] = true
                local rel = root:match("%.arcmodule$") and path.translate(root, "/") or path.getrelative(root, file)
                local root_name = path.getname(path.getabsolute(root))
                if root:match("%.arcmodule$") and path.isabsolute(rel) and ARCANE_SDK then
                    rel = path.getrelative(ARCANE_SDK, rel)
                end
                relative[file] = root:match("%.arcmodule$") and rel or path.translate((root_name == "Source" or root_name == "ThirdParty") and
                    path.join(root_name, rel) or rel, "/")
            end
        end
    end
    table.sort(files)
    for _, file in ipairs(files) do
        local mod = load_one(file)
        if mods.byName[mod.name] then fail("schema", file .. ": duplicate module " .. mod.name) end
        mods.byName[mod.name] = mod
    end
    for _, file in ipairs(files) do
        local mod = mods.byName[path.getbasename(file)]
        local source_type = relative[file]:match("^Source/([^/]+)/")
        local vendor = relative[file]:match("^ThirdParty/")
        if not source_type and not vendor then fail("folder", file .. ": outside Source/<Type>/") end
        if vendor and not mod.vendor then fail("folder", file .. ": ThirdParty descriptor requires vendor=true") end
        if source_type and source_type ~= mod.type then
            fail("folder", file .. ": " .. mod.type .. " does not match Source/" .. source_type)
        end
        for _, visibility in ipairs({"public", "private"}) do
            for _, dep in ipairs(mod.dependencies[visibility]) do
                local target = mods.byName[dep]
                if not target then fail("schema", file .. ": unknown dependency " .. dep) end
                if not allowed_edges[mod.type][target.type] then
                    fail("type-rule", mod.name .. " (" .. mod.type .. ") -> " .. dep .. " (" .. target.type .. ") in " .. file)
                end
            end
        end
    end
    local active, done, stack = {}, {}, {}
    local function visit(name)
        if active[name] then
            local cycle, started = {}, false
            for _, item in ipairs(stack) do
                if item == name then started = true end
                if started then cycle[#cycle+1] = item end
            end
            cycle[#cycle+1] = name
            fail("cycle", table.concat(cycle, " -> "))
        end
        if done[name] then return end
        active[name] = true; stack[#stack+1] = name
        local mod = mods.byName[name]
        for _, visibility in ipairs({"public", "private"}) do
            for _, dep in ipairs(mod.dependencies[visibility]) do visit(dep) end
        end
        stack[#stack] = nil; active[name] = nil; done[name] = true
        mods.order[#mods.order+1] = name
    end
    local names = {}
    for name in pairs(mods.byName) do names[#names+1] = name end
    table.sort(names)
    for _, name in ipairs(names) do visit(name) end
    local template = io.readfile(path.join(modules_dir, "templates/Api.hpp.in"))
    if not template then fail("api-header", "cannot read build/templates/Api.hpp.in") end
    for _, name in ipairs(mods.order) do
        local mod = mods.byName[name]
        if not mod.vendor then
        local count = 0
        for _, cpp in ipairs(os.matchfiles(path.join(mod.root, "src/**.cpp"))) do
            local source = (io.readfile(cpp) or ""):gsub("/%*.-%*/", " "):gsub("//[^\r\n]*", "")
            for declared in source:gmatch("ARC_MODULE%s*%(%s*([%w_]+)%s*%)") do
                if declared == name then count = count + 1 end
            end
        end
        if count ~= 1 then fail("declaration", mod.file .. ": expected exactly one ARC_MODULE(" .. name .. "), found " .. count) end
        local header = mod.apiHeader and path.join(mod.root, mod.apiHeader) or
            path.join(mod.root, "src/Arcane/Modules", name .. "Api.hpp")
        local expected = template:gsub("@MODULE@", suffix(name)):gsub("@API_MACRO@", api_macro(mod))
        local actual = io.readfile(header)
        local function normalized(s) return s and s:gsub("\r\n", "\n") end
        if normalized(actual) ~= normalized(expected) then
            fail("api-header", mod.file .. ": " .. header .. " differs from template")
        end
        end
    end
    return mods
end

-- @NAME@ placeholders resolve from premake5.lua's globals; nil when one is unset.
local function resolve(value)
    if type(value) ~= "string" then return value end
    local missing = false
    local out = value:gsub("@([%w_]+)@", function(name)
        local v = _G[name]
        if type(v) ~= "string" or v == "" then missing = true; return "" end
        return (v:gsub("\\", "/"))
    end)
    if missing then return nil end
    return out
end

local function resolve_list(list)
    local out = {}
    for _, item in ipairs(list or {}) do
        local v = resolve(item)
        if v then out[#out+1] = v end
    end
    return out
end

-- The current target's prebuilt entry of a module, resolved (nil = not prebuilt here).
function arc_module_prebuilt(mod)
    if not mod.prebuilt then return nil end
    if ARC_PREBUILT_DISCOVERY[mod.name] then ARC_PREBUILT_DISCOVERY[mod.name]() end
    local entry = mod.prebuilt[os.target()]
    if not entry then return nil end
    local debug = entry.debug or {}
    return {
        include = resolve(entry.include), link = entry.link,
        libdir = resolve(entry.libdir), runtime = resolve_list(entry.runtime), rpath = entry.rpath,
        debugLibdir = debug.libdir and resolve(debug.libdir) or nil,
        debugRuntime = debug.runtime and resolve_list(debug.runtime) or nil,
    }
end

-- Compile + link a prebuilt vendor module into the CURRENT project
-- (`headers_only`: compile only -- a monolithic static module, whose package
-- links the library once). Leaves no filter active.
local function use_prebuilt(mod, headers_only)
    local p = arc_module_prebuilt(mod)
    if not p then return end
    -- Off Windows a prebuilt's include dir can be a system prefix
    -- (/usr/local/include: SDL3's fallback), which also holds older copies of
    -- headers the engine vendors (the Vulkan SDK's vulkan/ on the macOS
    -- runners). -isystem dirs are searched after every -I dir, so the vendored
    -- ThirdParty/ headers always win whatever the module order.
    if p.include then
        if os.target() == "windows" then includedirs { p.include } else externalincludedirs { p.include } end
    end
    if headers_only then return end
    if p.link then links { p.link } end
    if p.debugLibdir then
        filter "configurations:Debug"
            libdirs { p.debugLibdir }
        filter "configurations:not Debug"
            if p.libdir then libdirs { p.libdir } end
        filter {}
    elseif p.libdir then
        libdirs { p.libdir }
    end
    if p.rpath and p.libdir and os.target() ~= "windows" then linkoptions { "-Wl,-rpath," .. p.libdir } end
end

function arc_module_closure(mods, name, kind)
    if kind ~= "public" and kind ~= "all" then fail("schema", "closure kind must be public or all") end
    if not mods.byName[name] then fail("schema", "unknown module " .. name) end
    local found = {}
    local function walk(current)
        for _, visibility in ipairs(kind == "public" and {"public"} or {"public", "private"}) do
            for _, dep in ipairs(mods.byName[current].dependencies[visibility]) do
                if not found[dep] then found[dep] = true; walk(dep) end
            end
        end
    end
    walk(name)
    local result = {}
    for _, candidate in ipairs(mods.order) do if found[candidate] then result[#result+1] = candidate end end
    return result
end

-- What a module (or a consumer naming `names`) compiles against: those names
-- plus their PUBLIC closures. A prebuilt vendor module is linked only when it
-- is visible this way -- ArcaneWindow's private SDL3 stays ArcaneWindow's.
local function visible(mods, names)
    local seen = {}
    for _, name in ipairs(names) do
        seen[name] = true
        for _, dep in ipairs(arc_module_closure(mods, name, "public")) do seen[dep] = true end
    end
    return seen
end

local function layout()
    return _OPTIONS.layout or "modular"
end

-- The bin flavor suffix of the current layout: Binaries/<cfg>-<sys>-<arch>-<suffix>/.
function arc_layout_suffix()
    return layout() == "monolithic" and "mono" or "md"
end

function arc_module_project(mods, name)
    local mod = assert(mods.byName[name], "unknown module " .. name)
    project(name)
    kind(layout() == "monolithic" and "StaticLib" or "SharedLib")
    defines { "ARC_" .. suffix(name) .. "_EXPORTS" }
    if name == "ArcaneCore" then defines { "ARC_CORE_API_EXPORTS" } end
    -- Sources by the module's ROOT, never %{prj.location}: the monolithic
    -- layout generates its project files elsewhere (ARC_MONOLITHIC_PROJECTS,
    -- premake5.lua) so neither layout's files overwrite the other's -- gmake
    -- names every project file Makefile. ARC_MODULE_ROOT is the same root for
    -- the ARC_MODULE_EXTRAS hook below.
    local src = path.getabsolute(mod.root)
    ARC_MODULE_ROOT = src
    location(ARC_MONOLITHIC_PROJECTS and path.join(ARC_MONOLITHIC_PROJECTS, name) or mod.root)
    language "C++"
    cppdialect "C++23"
    staticruntime "off"
    targetdir("Binaries/" .. outputdir .. "/%{prj.name}")
    objdir("Intermediate/Build/" .. outputdir .. "/%{prj.name}")
    files { src .. "/src/**.hpp", src .. "/src/**.cpp" }
    includedirs { src .. "/src" }
    local root = ARCANE_SDK or ARC_ROOT or "%{wks.location}"
    local vendors = {}
    -- A dependency's PUBLIC thirdParty is linked too: its public headers carry
    -- inline code that calls the vendor (e.g. PhysicsSystem.hpp -> Manifold2D),
    -- and a vendor static lib has no exports to import. The linker only pulls the
    -- objects a module references; the vendor libs linked this way hold no
    -- process-global state. A vendor library that DOES (SDL3: window, event and
    -- keyboard state) is a prebuilt vendor MODULE instead -- one shared library
    -- per process, linked by the modules that depend on it.
    local function add_vendor(vendor)
        if vendors[vendor] then return end
        vendors[vendor] = true
        local entry = ARC_THIRDPARTY[vendor]
        includedirs { root .. "/" .. entry.include }
        if entry.link then links { entry.link } end
    end
    local seen = {}
    for _, visibility in ipairs({"public", "private"}) do
        for dep in pairs(visible(mods, mod.dependencies[visibility])) do seen[dep] = true end
    end
    for _, dep in ipairs(arc_module_closure(mods, name, "all")) do
        local dep_mod = mods.byName[dep]
        if dep_mod.prebuilt then
            -- Static (monolithic): headers only. A static library that absorbed
            -- an import lib would hand every package a duplicate per module.
            if seen[dep] then use_prebuilt(dep_mod, layout() == "monolithic") end
        else
            links { dep }
            local dep_root = dep_mod.root
            includedirs { (path.isabsolute(dep_root) and dep_root or (root .. "/" .. dep_root)) .. (dep_mod.vendor and "" or "/src") }
            for _, vendor in ipairs(dep_mod.thirdParty.public) do add_vendor(vendor) end
        end
    end
    for _, visibility in ipairs({"public", "private"}) do
        for _, vendor in ipairs(mod.thirdParty[visibility]) do add_vendor(vendor) end
    end
    for platform, settings in pairs(mod.platforms or {}) do
        filter("system:" .. platform)
        -- System libraries: a static module leaves them to the package link.
        if settings.links and layout() ~= "monolithic" then links(settings.links) end
        if settings.defines then defines(settings.defines) end
    end
    filter {}
    filter "system:not windows"
        visibility "Hidden"
    filter "system:linux"
        linkoptions { "-Wl,-rpath,'$$ORIGIN'" }   -- make: $$ -> $; quotes: not the shell's (bare $ORIGIN became "RIGIN")
    filter "system:macosx"
        linkoptions { "-Wl,-rpath,@loader_path" }
    filter {}
    if ARC_MODULE_EXTRAS and ARC_MODULE_EXTRAS[name] then ARC_MODULE_EXTRAS[name]() end
end

function arc_module_stage(mods, names)
    if layout() == "monolithic" then return end
    -- An SDK consumer's own outputdir names ITS layout; the engine's is always -md.
    local out = (not ARCANE_SDK and outputdir) or "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-md"
    local selected = {}
    for _, name in ipairs(names) do
        selected[name] = true
        for _, dep in ipairs(arc_module_closure(mods, name, "all")) do selected[dep] = true end
    end
    local commands, debug_commands, release_commands = {}, {}, {}
    for _, name in ipairs(mods.order) do
        local mod = mods.byName[name]
        if selected[name] and mod.prebuilt then
            -- A prebuilt vendor module stages its runtime file(s), per configuration
            -- (none on a target where it is a system library).
            local p = arc_module_prebuilt(mod)
            if p then
                for _, file in ipairs(p.debugRuntime or p.runtime) do
                    debug_commands[#debug_commands+1] = '{COPYFILE} "' .. file .. '" "%{cfg.buildtarget.directory}/' .. path.getname(file) .. '"'
                end
                for _, file in ipairs(p.runtime) do
                    release_commands[#release_commands+1] = '{COPYFILE} "' .. file .. '" "%{cfg.buildtarget.directory}/' .. path.getname(file) .. '"'
                end
            end
        elseif selected[name] then
            local extension = os.target() == "windows" and ".dll" or (os.target() == "macosx" and ".dylib" or ".so")
            local prefix = os.target() == "windows" and "" or "lib"
            commands[#commands+1] = '{COPYFILE} "' .. (ARCANE_SDK or "%{wks.location}") .. '/Binaries/' .. out .. '/' .. name .. '/' .. prefix .. name .. extension .. '" "%{cfg.buildtarget.directory}/' .. prefix .. name .. extension .. '"'
        end
    end
    postbuildcommands(commands)
    if #debug_commands > 0 then
        filter "configurations:Debug"
            postbuildcommands(debug_commands)
        filter {}
    end
    if #release_commands > 0 then
        filter "configurations:not Debug"
            postbuildcommands(release_commands)
        filter {}
    end
    -- In-workspace: a staged-but-unlinked module still has to be BUILT first.
    if not ARCANE_SDK then
        local order = {}
        for _, name in ipairs(mods.order) do
            if selected[name] and not mods.byName[name].prebuilt then order[#order+1] = name end
        end
        dependson(order)
    end
end

-- Link + include the module closure of `names` (opts.link = false: include
-- only). opts.stage: false = do not
-- copy the module binaries beside the target (a loaded game module: the host
-- already carries them); a list = stage that closure instead (a host that maps
-- modules it never links, e.g. ArcaneServer for a game module's full set).
function arc_module_consumer(mods, names, opts)
    opts = opts or {}
    local selected = {}
    for _, name in ipairs(names) do
        selected[name] = true
        for _, dep in ipairs(arc_module_closure(mods, name, "all")) do selected[dep] = true end
    end
    local root = ARCANE_SDK or ARC_ROOT or "%{wks.location}"
    local vendors = {}
    local forced = {}
    local seen = visible(mods, names)
    for _, name in ipairs(mods.order) do
        local mod = mods.byName[name]
        if selected[name] and mod.prebuilt then
            if seen[name] then use_prebuilt(mod, opts.link == false) end
        elseif selected[name] then
            -- opts.link = false: headers only (a monolithic game's static
            -- library -- the package links every module once, whole-archived).
            if opts.link ~= false then
                links { name }
                if not mod.vendor then forced[#forced+1] = name end
            end
            if ARCANE_SDK and opts.link ~= false then libdirs { ARCANE_SDK .. "/Binaries/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-" .. arc_layout_suffix() .. "/" .. name } end
            includedirs { (path.isabsolute(mod.root) and mod.root or root .. "/" .. mod.root) .. (mod.vendor and "" or "/src") }
            if opts.vendors ~= false then
                for _, vendor in ipairs(mod.thirdParty.public) do vendors[vendor] = true end
            end
        end
    end
    local vendor_names = {}
    for vendor in pairs(vendors) do vendor_names[#vendor_names+1] = vendor end
    table.sort(vendor_names)   -- deterministic include order across generations
    for _, vendor in ipairs(vendor_names) do
        local entry = ARC_THIRDPARTY[vendor]
        if entry then includedirs { root .. "/" .. entry.include } end
    end
    -- A linked module is a LOADED module: a consumer that reaches a module only
    -- through its headers (Net's Protocol.hpp) references no export, so the
    -- linker drops the import (MSVC; GNU ld --as-needed) and the DLL never
    -- loads -- its ARC_SETTINGS / ARC_COMPONENT registrars never run (net.*
    -- unregistered, protocol.json's values ignored). The ArcModule_<Name>
    -- anchor keeps every linked engine module in the import table. Only where
    -- something links: a static library's consumer is the exe that links it.
    -- ELF: --as-needed (Ubuntu's GCC default) keeps a DSO only for a reference
    -- from an OBJECT -- a -u symbol is not one -- so the libraries link
    -- --no-as-needed (premake's LDFLAGS precede LIBS on the gmake link line).
    filter "kind:not StaticLib"
        arc_module_forced_refs(forced)
    filter { "kind:not StaticLib", "system:linux" }
        if #forced > 0 then linkoptions { "-Wl,--no-as-needed" } end
    filter {}
    if opts.stage ~= false then arc_module_stage(mods, type(opts.stage) == "table" and opts.stage or names) end
end

-- ============================================================================
-- Games (engine modularization s4). A game names its engine modules in
-- <sourceDir>/<name>.arcmodule (type Game); the SDK links and includes that
-- closure and nothing else. A game WITHOUT a descriptor gets the default set:
-- every Runtime + Client module (ArcaneClient's closure plus Net and Crypto),
-- ImGui and ArcaneImGui -- exactly the pre-descriptor surface. The C++ twin of
-- this list is Arcane::kDefaultGameModules (Arcane/Project/GameDescriptor.hpp),
-- which the editor writes into a new project's descriptor; keep them equal.
-- ============================================================================
ARC_GAME_DEFAULT_MODULES = { "ArcaneClient", "ArcaneImGui", "ImGui", "ArcaneNet", "ArcaneCrypto", "ArcanePhysics2D" }

-- Validate a game descriptor against the loaded engine graph and return the
-- game's direct engine dependencies, or nil when `file` does not exist (the
-- caller then uses ARC_GAME_DEFAULT_MODULES). Checks: the descriptor schema
-- (load_one: strict keys, name == file stem), type Game, every dependency a
-- known module, the s2.3 type rule (Game -> Runtime/Client only -- no Developer
-- or Editor module ever reaches a game), and -- when `sourceRoot` is given --
-- the declaration check's game form: exactly one ARC_GAME_MODULE /
-- ARC_GAME_MODULE_ABI in the game's .cpp files (the macro emits the
-- ArcModule_<Name> anchor). A game is a leaf, so it adds no cycle.
function arc_game_descriptor(mods, file, sourceRoot)
    if not os.isfile(file) then return nil end
    local mod = load_one(file)
    if mod.type ~= "Game" then
        fail("type-rule", file .. ": a game descriptor must have type Game, not " .. tostring(mod.type))
    end
    -- A game links its engine modules and nothing else through the SDK: no
    -- vendor/prebuilt module shape, no per-platform links, and only HEADER-ONLY
    -- third-party roots (a vendor that needs a library reaches a game through
    -- the engine module that links it) -- refused here, by name, rather than as
    -- a late unresolved external.
    if mod.vendor or mod.prebuilt or (mod.platforms and next(mod.platforms)) then
        fail("schema", file .. ": a game descriptor takes no vendor, prebuilt or platforms field")
    end
    for _, visibility in ipairs({"public", "private"}) do
        for _, vendor in ipairs(mod.thirdParty[visibility]) do
            if ARC_THIRDPARTY[vendor].link then
                fail("schema", file .. ": thirdParty " .. vendor .. " is a compiled library; a game names " ..
                     "header-only vendors only (depend on the engine module that links it)")
            end
        end
    end
    local names, seen = {}, {}
    for _, visibility in ipairs({"public", "private"}) do
        for _, dep in ipairs(mod.dependencies[visibility]) do
            local target = mods.byName[dep]
            if not target then fail("schema", file .. ": unknown dependency " .. dep) end
            if not allowed_edges.Game[target.type] then
                fail("type-rule", mod.name .. " (Game) -> " .. dep .. " (" .. target.type .. ") in " .. file)
            end
            if not seen[dep] then seen[dep] = true; names[#names+1] = dep end
        end
    end
    if sourceRoot then
        local count = 0
        for _, cpp in ipairs(os.matchfiles(path.join(sourceRoot, "**.cpp"))) do
            local source = (io.readfile(cpp) or ""):gsub("/%*.-%*/", " "):gsub("//[^\r\n]*", "")
            for _ in source:gmatch("ARC_GAME_MODULE_ABI%s*%(") do count = count + 1 end
            for _ in source:gmatch("ARC_GAME_MODULE%s*%(") do count = count + 1 end
        end
        if count ~= 1 then
            fail("declaration", file .. ": expected exactly one ARC_GAME_MODULE in " .. sourceRoot .. ", found " .. count)
        end
    end
    mod.modules = names
    mod.thirdPartyIncludes = {}
    for _, visibility in ipairs({"public", "private"}) do
        for _, vendor in ipairs(mod.thirdParty[visibility]) do
            mod.thirdPartyIncludes[#mod.thirdPartyIncludes+1] = ARC_THIRDPARTY[vendor].include
        end
    end
    return mod
end

-- Does the closure of `names` contain module `wanted`?
function arc_closure_contains(mods, names, wanted)
    for _, name in ipairs(names) do
        if name == wanted then return true end
        for _, dep in ipairs(arc_module_closure(mods, name, "all")) do
            if dep == wanted then return true end
        end
    end
    return false
end

function arc_module_forced_refs(names)
    local options = {}
    for _, name in ipairs(names) do
        local symbol = "ArcModule_" .. name
        if os.target() == "windows" then options[#options+1] = "/INCLUDE:" .. symbol
        elseif os.target() == "macosx" then options[#options+1] = "-Wl,-u,_" .. symbol
        else options[#options+1] = "-Wl,-u," .. symbol end
    end
    linkoptions(options)
end

-- ============================================================================
-- The package link (engine modularization s3.3): what a monolithic <Game>.exe
-- links for the module closure of `names`, in the CURRENT project. Every module
-- static library is whole-archived (/WHOLEARCHIVE:, --whole-archive,
-- -force_load) so its static registrars' initializers reach the link -- the
-- init-table entries keep them past dead-code stripping (s3.3's "why registrars
-- survive") -- plus one forced reference per module (a missing ARC_MODULE fails
-- the link). The vendor static libraries the closure's modules use, the prebuilt
-- vendor modules' libraries (SDL3) and the descriptors' system libraries link
-- once, normally. `sdk` is the engine root; returns the closure, in link order.
-- ============================================================================
local function vendor_dir(vendor)
    -- ThirdParty/<Dir>/... -> ThirdParty/<Dir>: where the wrapper's bin/ lives.
    return ARC_THIRDPARTY[vendor].include:match("^(ThirdParty/[^/]+)")
end

function arc_module_package(mods, names, sdk)
    local selected = {}
    for _, name in ipairs(names) do
        selected[name] = true
        for _, dep in ipairs(arc_module_closure(mods, name, "all")) do selected[dep] = true end
    end
    local closure, vendors, vendor_order = {}, {}, {}
    for _, name in ipairs(mods.order) do
        if selected[name] then
            local mod = mods.byName[name]
            if mod.type ~= "Runtime" and mod.type ~= "Client" then
                fail("package", name .. " (" .. mod.type .. ") cannot be linked into a package: only Runtime and Client modules ship")
            end
            closure[#closure+1] = name
            for _, visibility in ipairs({"public", "private"}) do
                for _, vendor in ipairs(mod.thirdParty[visibility]) do
                    if ARC_THIRDPARTY[vendor].link and not vendors[vendor] then
                        vendors[vendor] = true; vendor_order[#vendor_order+1] = vendor
                    end
                end
            end
        end
    end
    -- The engine's mono static libs live in <sdk>/Binaries/; a vendor wrapper
    -- library (premake-built under ThirdParty/<Dir>/) keeps its own bin/.
    local bin = "/Binaries/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-mono/"
    local vendor_bin = "/bin/%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}-mono/"
    local static_ext = os.target() == "windows" and ".lib" or ".a"
    local static_prefix = os.target() == "windows" and "" or "lib"
    local whole = {}
    for _, name in ipairs(closure) do
        local mod = mods.byName[name]
        if mod.prebuilt then
            use_prebuilt(mod)
        else
            local lib = sdk .. bin .. name .. "/" .. static_prefix .. name .. static_ext
            if os.target() == "windows" then whole[#whole+1] = "/WHOLEARCHIVE:" .. lib
            elseif os.target() == "macosx" then whole[#whole+1] = "-Wl,-force_load," .. lib
            else whole[#whole+1] = lib end
            includedirs { (path.isabsolute(mod.root) and mod.root or sdk .. "/" .. mod.root) .. (mod.vendor and "" or "/src") }
            for _, vendor in ipairs(mod.thirdParty.public) do
                includedirs { sdk .. "/" .. ARC_THIRDPARTY[vendor].include }
            end
        end
        for platform, settings in pairs(mod.platforms or {}) do
            filter("system:" .. platform)
            if settings.links then links(settings.links) end
            if settings.defines then defines(settings.defines) end
        end
        filter {}
    end
    if os.target() == "windows" or os.target() == "macosx" then
        linkoptions(whole)
    else
        -- ONE option string: premake de-duplicates linkoptions keeping the LAST
        -- copy, so a separate "-Wl,--whole-archive" here was dropped in favour of
        -- the game's (arcane_package_exe) and the engine archives linked as plain
        -- inputs ahead of the objects that need them (GNU ld scans in order).
        linkoptions { "-Wl,--whole-archive " .. table.concat(whole, " ") .. " -Wl,--no-whole-archive" }
    end
    for _, vendor in ipairs(vendor_order) do
        local entry = ARC_THIRDPARTY[vendor]
        links { entry.link }
        libdirs { sdk .. "/" .. vendor_dir(vendor) .. vendor_bin .. entry.link }
    end
    local forced = {}
    for _, name in ipairs(closure) do
        if not mods.byName[name].vendor then forced[#forced+1] = name end
    end
    arc_module_forced_refs(forced)
    filter "system:not windows"
        links { "dl", "pthread" }
    filter {}
    return closure
end
