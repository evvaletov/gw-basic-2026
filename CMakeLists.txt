cmake_minimum_required(VERSION 3.10)
project(gwbasic C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Warnings
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Include paths
include_directories(include)

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
    platform/hal_posix.c
)

add_executable(gwbasic ${SOURCES})
target_link_libraries(gwbasic m)
