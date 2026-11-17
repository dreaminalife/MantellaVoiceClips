-- include subprojects
includes("lib/commonlibsse-ng")

-- set project constants
set_project("MantellaVoiceClips")
set_version("1.0.0")
set_license("MIT")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- add packages
add_requires("nlohmann_json v3.12.0")

-- define targets
target("MantellaVoiceClips")
    add_rules("commonlibsse-ng.plugin", {
        name = "MantellaVoiceClips",
        author = "Mantella",
        description = "Copies voice lines from the game for Mantella's voice services"
    })

    -- add src files
    add_packages("nlohmann_json")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")
