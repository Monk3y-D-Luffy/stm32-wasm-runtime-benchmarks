cmake_minimum_required(VERSION 3.20)
project(linux_wasm3_fft C)

set(CMAKE_C_STANDARD 11)

add_executable(linux_wasm3_fft
    src/main.c
    src/m3_core.c
    src/m3_env.c
    src/m3_parse.c
    src/m3_compile.c
    src/m3_exec.c
    src/m3_code.c
    src/m3_function.c
    src/m3_module.c
    src/m3_info.c
    src/m3_api_libc.c
    src/m3_bind.c
    src/m3_emit.c
)

target_include_directories(linux_wasm3_fft PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_options(linux_wasm3_fft PRIVATE -O3)

target_link_libraries(linux_wasm3_fft PRIVATE m)
