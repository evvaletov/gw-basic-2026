cmake_minimum_required(VERSION 3.10)
project(gw-basic-2026 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# POSIX extensions (opendir, chdir, strcasecmp, etc.)
add_compile_definitions(_DEFAULT_SOURCE)

# Warnings
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Include paths
include_directories(include)

# Optional PulseAudio support
include(FindPkgConfig)
pkg_check_modules(PULSEAUDIO libpulse-simple)

# Source files
set(SOURCES
    src/main.c
    src/tokens.c
    src/tokenizer.c
    src/error.c
    src/eval.c
    src/interp.c
    src/vars.c
    src/arrays.c
    src/input.c
    src/math_int.c
    src/math_float.c
    src/math_transcend.c
    src/strings.c
    src/print.c
    src/fileio.c
    src/program_io.c
    src/print_using.c
    src/graphics.c
    src/virmem.c
    src/portio.c
    src/strpool.c
    src/sound.c
    src/tui.c
    platform/hal_posix.c
)

add_executable(gwbasic ${SOURCES})
target_link_libraries(gwbasic m)

if(PULSEAUDIO_FOUND)
    target_compile_definitions(gwbasic PRIVATE HAVE_PULSEAUDIO)
    target_include_directories(gwbasic PRIVATE ${PULSEAUDIO_INCLUDE_DIRS})
    target_link_libraries(gwbasic ${PULSEAUDIO_LIBRARIES} pthread)
endif()
