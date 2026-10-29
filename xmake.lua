-- Fast Travel Time Info - SKSE plugin for Skyrim SE/AE
-- Build: the GitHub workflow in .github/workflows/build.yml does everything.
-- Local build (Windows, Visual Studio Build Tools 2022/2026 with C++ + Git + xmake 3.x):
--   double-click build.bat (see README.md)
set_xmakever("3.0.0")

set_project("FastTravelTimeInfo")
set_version("0.1.1")
set_license("GPL-3.0")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
set_defaultmode("releasedbg")

-- Build one DLL for SE, AE and VR layouts (matches CommonLib's prebuilt bundle,
-- so CI links the prebuilt library instead of compiling CommonLib from source).
set_config("skyrim_se", true)
set_config("skyrim_ae", true)
set_config("skyrim_vr", true)

includes("lib/commonlibsse-ng")

target("FastTravelTimeInfo", function()
    add_deps("commonlibsse-ng")
    add_rules("commonlibsse-ng.plugin", {
        name = "FastTravelTimeInfo",
        author = "Yulian",
        description = "Shows fast-travel duration and arrival time on world map markers"
    })

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- Optional SKSE Menu Framework support (header only; the framework is looked up at
    -- runtime, so it is never a requirement). Its header still uses <codecvt>.
    add_includedirs("extern/SKSEMenuFramework")
    add_defines("_SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING")
end)
