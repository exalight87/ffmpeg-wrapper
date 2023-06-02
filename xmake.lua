add_requires("ffmpeg", {configs = { libx264 = true }})
add_requires("stb")

add_rules("mode.debug", "mode.release")
set_languages("cxxlatest")

target("encoder")
    set_kind("binary")

    add_packages("ffmpeg", "stb")

    add_files("src/*.cpp")
    add_headerfiles("include/*.hpp")
    add_includedirs("include")