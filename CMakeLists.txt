cmake_minimum_required(VERSION 3.16)
# -O2, not CMake's default -O3: -O3 measured 20-30% slower on the decoder's 2-row wavefront kernels
# (photo ga8/g8 decode), encoder unchanged. Set before project() so it becomes the cached default.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release C flags")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()
project(pngfast VERSION 1.0.0 LANGUAGES C)

option(PNGFAST_BUILD_TESTS "Build tests" ON)
option(PNGFAST_BUILD_TOOLS "Build pngfast-cli and the benchmark tool" ON)
option(PNGFAST_BUILD_SHARED "Build a shared library (default: static)" OFF)
option(PNGFAST_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(PNGFAST_LIBFUZZER "Build libFuzzer fuzz targets (clang)" OFF)
set(PNGFAST_CORPUS_DIR "" CACHE PATH "Optional directory with the generated format corpus (tests)")
set(PNGFAST_PNGSUITE_DIR "" CACHE PATH "Optional PngSuite directory (tests)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # GNU C11: target attributes, __builtin_cpu_supports
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(PNGFAST_LIBFUZZER)
  add_compile_options(-fsanitize=fuzzer-no-link)   # coverage instrumentation for the library itself
endif()
if(PNGFAST_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- vendored libdeflate (modified; see third_party/libdeflate/PNGFAST_CHANGES.md) ----
set(LD ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdeflate)
file(GLOB LD_SOURCES ${LD}/lib/*.c ${LD}/lib/x86/*.c ${LD}/lib/arm/*.c ${LD}/lib/riscv/*.c)
add_library(pf_libdeflate OBJECT ${LD_SOURCES})
target_include_directories(pf_libdeflate PRIVATE ${LD})
target_compile_options(pf_libdeflate PRIVATE -include ${LD}/pf_rename.h -w)
target_compile_definitions(pf_libdeflate PRIVATE LIBDEFLATE_STATIC)

# ---- pngfast ----
set(PF_SOURCES src/common.c src/decode.c src/unfilter.c src/expand.c src/encode.c src/filter_enc.c)
if(PNGFAST_BUILD_SHARED)
  add_library(pngfast SHARED ${PF_SOURCES} $<TARGET_OBJECTS:pf_libdeflate>)
  target_compile_definitions(pngfast PUBLIC PNGFAST_SHARED)
else()
  add_library(pngfast STATIC ${PF_SOURCES} $<TARGET_OBJECTS:pf_libdeflate>)
endif()
target_include_directories(pngfast PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
                                   PRIVATE src ${LD})
target_compile_definitions(pngfast PRIVATE LIBDEFLATE_STATIC)
target_compile_options(pngfast PRIVATE -Wall -Wextra -Wno-unused-parameter)
set_target_properties(pngfast PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

include(GNUInstallDirs)
install(TARGETS pngfast EXPORT pngfastTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/pngfast.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(pngfast.pc.in pngfast.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pngfast.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(EXPORT pngfastTargets NAMESPACE pngfast:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pngfast)

if(PNGFAST_BUILD_TOOLS)
  add_executable(pngfast-cli tools/pngfast-cli.c)
  target_link_libraries(pngfast-cli PRIVATE pngfast)
  add_executable(pfbench bench/pfbench.c)
  target_link_libraries(pfbench PRIVATE pngfast)
  target_include_directories(pfbench PRIVATE src ${LD})   # raw-deflate benchmark uses the vendored libdeflate API
endif()

# Fuzz targets: always built as standalone replay/mutation drivers (also usable with AFL++);
# with -DPNGFAST_LIBFUZZER=ON (clang) additionally as libFuzzer binaries.
if(PNGFAST_BUILD_TESTS OR PNGFAST_LIBFUZZER)
  foreach(t decode roundtrip)
    add_executable(fuzz_${t} fuzz/fuzz_${t}.c fuzz/standalone.c)
    target_link_libraries(fuzz_${t} PRIVATE pngfast)
    if(PNGFAST_LIBFUZZER)
      add_executable(libfuzzer_${t} fuzz/fuzz_${t}.c)
      target_compile_options(libfuzzer_${t} PRIVATE -fsanitize=fuzzer)
      target_link_options(libfuzzer_${t} PRIVATE -fsanitize=fuzzer)
      target_link_libraries(libfuzzer_${t} PRIVATE pngfast)
    endif()
  endforeach()
endif()

if(PNGFAST_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
