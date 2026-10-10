-- FreeType premake5 build script
-- FTL license -- font rasterizer. Amalgamated-module build: one TU per
-- module, matching the default ftmodule.h module list. FT2_BUILD_LIBRARY
-- selects the internal-build include paths.

project "freetype"
    kind "StaticLib"
    language "C"
    location(THIRDPARTY_PROJECT_LOCATION or ".")
    staticruntime(THIRDPARTY_STATICRUNTIME or "on")

    targetdir ("bin/" .. outputdir .. "/%{prj.name}")
    objdir    ("bin-int/" .. outputdir .. "/%{prj.name}")

    files {
        "src/autofit/autofit.c",
        "src/base/ftbase.c",
        "src/base/ftbbox.c",
        "src/base/ftbdf.c",
        "src/base/ftbitmap.c",
        "src/base/ftcid.c",
        "src/base/ftdebug.c",
        "src/base/ftfstype.c",
        "src/base/ftgasp.c",
        "src/base/ftglyph.c",
        "src/base/ftgxval.c",
        "src/base/ftinit.c",
        "src/base/ftmm.c",
        "src/base/ftotval.c",
        "src/base/ftpatent.c",
        "src/base/ftpfr.c",
        "src/base/ftstroke.c",
        "src/base/ftsynth.c",
        "src/base/ftsystem.c",
        "src/base/fttype1.c",
        "src/base/ftwinfnt.c",
        "src/bdf/bdf.c",
        "src/cache/ftcache.c",
        "src/cff/cff.c",
        "src/cid/type1cid.c",
        "src/gzip/ftgzip.c",
        "src/lzw/ftlzw.c",
        "src/pcf/pcf.c",
        "src/pfr/pfr.c",
        "src/psaux/psaux.c",
        "src/pshinter/pshinter.c",
        "src/psnames/psnames.c",
        "src/raster/raster.c",
        "src/sdf/sdf.c",
        "src/sfnt/sfnt.c",
        "src/smooth/smooth.c",
        "src/svg/svg.c",
        "src/truetype/truetype.c",
        "src/type1/type1.c",
        "src/type42/type42.c",
        "src/winfonts/winfnt.c",
    }

    includedirs { "include" }

    defines { "FT2_BUILD_LIBRARY", "_CRT_SECURE_NO_WARNINGS" }

    -- C4244/C4267 (size_t truncation): upstream size_t-to-smaller-type
    -- narrowing in psaux/smooth/sfnt/type1/type42. Vendored code -- not ours
    -- to patch.
    filter "system:windows"
        disablewarnings { "4244", "4267" }   -- MSVC-numbered: never passed to GCC/Clang as -Wno-4244
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
