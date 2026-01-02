cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(z_wasm3)

# Sorgenti dell'app
target_sources(app PRIVATE
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
  src/m3_api_libc.c    # opzionale, ma in genere sì
  src/m3_bind.c
  src/m3_emit.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Ignora warning float->double e unused-function per Wasm3
target_compile_options(app PRIVATE -Wno-double-promotion -Wno-unused-function)