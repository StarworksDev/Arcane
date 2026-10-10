-- msdfgen premake5 build script
-- MIT -- multi-channel signed distance field generation (text glyphs).
-- Vendored subset: core (all .cpp) + ext/import-font (FreeType bridge).
-- Excluded: ext/import-svg (tinyxml2), ext/save-png (libpng), ext/resolve-shape-geometry (Skia).
--
-- Included by the Arcane workspace (dynamic CRT, ide-md/ project location).
-- Globals THIRDPARTY_STATICRUNTIME / THIRDPARTY_PROJECT_LOCATION default to
-- the Server/Tools behavior; Arcane overrides them so lib outputs never collide.

project "msdfgen"
    kind "StaticLib"
    language "C++"
    cppdialect "C++17"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "core/**.cpp",
        "core/**.h",
        "core/**.hpp",
        "ext/import-font.cpp",
        "ext/import-font.h",
        "msdfgen.h",
        "msdfgen-ext.h",
    }

    -- Include root is the vendor subdir itself so that:
    --   core/base.h includes <msdfgen/msdfgen-config.h>  -> ThirdParty/msdfgen/msdfgen/msdfgen-config.h
    --   consumer code includes <msdfgen.h>               -> ThirdParty/msdfgen/msdfgen.h
    includedirs { ".", "../freetype/include" }

    defines { "MSDFGEN_PUBLIC=", "_CRT_SECURE_NO_WARNINGS" }

    -- C4005 ("macro redefinition"): save-bmp.cpp, save-tiff.cpp, and
    -- shape-description.cpp each #define _CRT_SECURE_NO_WARNINGS unguarded,
    -- colliding with the command-line define above. Upstream vendored code
    -- -- not ours to patch.
    filter "system:windows"
        disablewarnings { "4005" }   -- MSVC-numbered: never passed to GCC/Clang as -Wno-4005
    filter {}

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }

    filter "configurations:Dist"
        runtime "Release"
        optimize "on"
        defines { "NDEBUG" }
