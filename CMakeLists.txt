cmake_minimum_required(VERSION 3.16)
# -O2, not CMake's default -O3: -O3 measured 20-30% slower on the decoder's 2-row wavefront kernels
# (photo ga8/g8 decode), encoder unchanged. Set before project() so it becomes the cached default.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release C flags")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()
project(pngfast VERSION 1.2.0 LANGUAGES C)

option(PNGFAST_BUILD_TESTS "Build tests" ON)
option(PNGFAST_BUILD_TOOLS "Build pngfast-cli and the benchmark tool" ON)
option(PNGFAST_BUILD_SHARED "Build a shared library (default: static)" OFF)
option(PNGFAST_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(PNGFAST_LIBFUZZER "Build libFuzzer fuzz targets (clang)" OFF)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-ng/zlib-ng.h.in)
  set(PF_ZLIBNG_DEFAULT ON)
else()
  set(PF_ZLIBNG_DEFAULT OFF)
endif()
option(PNGFAST_WITH_ZLIBNG "Build the optional zlib-ng deflate backend (third_party/zlib-ng)" ${PF_ZLIBNG_DEFAULT})
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

# ---- optional vendored zlib-ng (native zng_* API, ZLIB_COMPAT OFF: no clash with a system zlib) ----
# Built as a static library in its own scope; its objects are folded into libpngfast so consumers need nothing.
# Runtime CPU dispatch (WITH_RUNTIME_CPU_DETECTION), never -march=native.
if(PNGFAST_WITH_ZLIBNG)
  set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
  set(WITH_GTEST OFF CACHE BOOL "" FORCE)
  set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
  set(WITH_NATIVE_INSTRUCTIONS OFF CACHE BOOL "" FORCE)
  set(WITH_RUNTIME_CPU_DETECTION ON CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  set(PF_SAVED_BUILD_TESTING ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory(third_party/zlib-ng EXCLUDE_FROM_ALL)
  set(BUILD_TESTING ${PF_SAVED_BUILD_TESTING})
  unset(BUILD_SHARED_LIBS)
  target_compile_options(zlib-ng PRIVATE -w)
endif()

# ---- pngfast ----
# Symbol hygiene: libpngfast exports only the public pf_* API declared with PF_API in include/pngfast.h.
#  * every object is built with hidden visibility (vendored code included);
#  * shared: linker version script (generated from the header) makes everything else local;
#  * static: the objects are partially linked into one relocatable object (ld -r) and objcopy localizes every
#    global outside the API (vendored libdeflate / zlib-ng internals, zng_*, pf__* helpers), so the archive can be
#    linked next to stock zlib / zlib-ng / libdeflate. Tests and tools that need the internals link
#    pngfast_internal (same objects, nothing localized).
option(PNGFAST_HIDE_SYMBOLS "Localize all non-API symbols in libpngfast (needs ld -r + objcopy, ELF)" ON)
set(PF_SOURCES src/common.c src/decode.c src/unfilter.c src/expand.c src/encode.c src/filter_enc.c src/deflate_backend.c src/metadata.c src/stream.c)
add_library(pngfast_obj OBJECT ${PF_SOURCES})
target_include_directories(pngfast_obj PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                       PRIVATE src ${LD})
target_compile_definitions(pngfast_obj PRIVATE LIBDEFLATE_STATIC)
target_compile_options(pngfast_obj PRIVATE -Wall -Wextra -Wno-unused-parameter)
set(PF_ALL_OBJECTS $<TARGET_OBJECTS:pngfast_obj> $<TARGET_OBJECTS:pf_libdeflate>)
if(PNGFAST_WITH_ZLIBNG)
  target_compile_definitions(pngfast_obj PRIVATE PF_HAVE_ZLIBNG ZLIBNG_NATIVE_API)
  target_include_directories(pngfast_obj PRIVATE $<TARGET_PROPERTY:zlib-ng,INCLUDE_DIRECTORIES>)
  set_target_properties(zlib-ng PROPERTIES C_VISIBILITY_PRESET hidden)
  list(APPEND PF_ALL_OBJECTS $<TARGET_OBJECTS:zlib-ng>)
endif()
if(PNGFAST_BUILD_SHARED)
  target_compile_definitions(pngfast_obj PUBLIC PNGFAST_SHARED)
endif()

# public API symbol list, from the header
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/pngfast.h PF_API_LINES REGEX "^PF_API ")
set(PF_EXPORTS "")
foreach(l ${PF_API_LINES})
  string(REGEX MATCH "(pf_[a-z0-9_]+)\\(" m "${l}")
  list(APPEND PF_EXPORTS ${CMAKE_MATCH_1})
endforeach()
list(REMOVE_DUPLICATES PF_EXPORTS)
string(REPLACE ";" "\n" PF_EXPORTS_TXT "${PF_EXPORTS}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/pngfast.exports "${PF_EXPORTS_TXT}\n")
string(REPLACE ";" ";\n    " PF_EXPORTS_MAP "${PF_EXPORTS}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/pngfast.map "PNGFAST_1 {\n  global:\n    ${PF_EXPORTS_MAP};\n  local: *;\n};\n")

# internal static library (tests, benchmark tool)
add_library(pngfast_internal STATIC ${PF_ALL_OBJECTS})
target_include_directories(pngfast_internal PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(pngfast_internal PROPERTIES LINKER_LANGUAGE C)

find_program(PF_OBJCOPY NAMES objcopy llvm-objcopy)
set(PF_LD ${CMAKE_LINKER})
if(PNGFAST_BUILD_SHARED)
  add_library(pngfast SHARED ${PF_ALL_OBJECTS})
  target_compile_definitions(pngfast PUBLIC PNGFAST_SHARED)
  if(PNGFAST_HIDE_SYMBOLS)
    target_link_options(pngfast PRIVATE -Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/pngfast.map)
    set_property(TARGET pngfast APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/pngfast.map)
  endif()
elseif(PNGFAST_HIDE_SYMBOLS AND PF_OBJCOPY AND PF_LD AND NOT APPLE AND NOT WIN32)
  set(PF_PUB ${CMAKE_CURRENT_BINARY_DIR}/pngfast_public.o)
  add_custom_command(OUTPUT ${PF_PUB}
    COMMAND ${PF_LD} -r -o ${PF_PUB}.tmp ${PF_ALL_OBJECTS}
    COMMAND ${PF_OBJCOPY} --keep-global-symbols=${CMAKE_CURRENT_BINARY_DIR}/pngfast.exports ${PF_PUB}.tmp ${PF_PUB}
    DEPENDS ${PF_ALL_OBJECTS} ${CMAKE_CURRENT_BINARY_DIR}/pngfast.exports
    COMMAND_EXPAND_LISTS VERBATIM
    COMMENT "libpngfast: partial link + localize non-API symbols")
  set_source_files_properties(${PF_PUB} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_library(pngfast STATIC ${PF_PUB})
  set_target_properties(pngfast PROPERTIES LINKER_LANGUAGE C)
  add_dependencies(pngfast pngfast_obj pf_libdeflate)   # object-file DEPENDS alone does not order the targets
  if(PNGFAST_WITH_ZLIBNG)
    add_dependencies(pngfast zlib-ng)
  endif()
else()
  add_library(pngfast STATIC ${PF_ALL_OBJECTS})
  set_target_properties(pngfast PROPERTIES LINKER_LANGUAGE C)
endif()
target_include_directories(pngfast PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
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
  target_link_libraries(pfbench PRIVATE pngfast_internal)
  target_include_directories(pfbench PRIVATE src ${LD})   # raw-deflate benchmark uses the vendored libdeflate API
  if(PNGFAST_WITH_ZLIBNG)   # ... and zlib-ng's (symbols inside libpngfast)
    target_compile_definitions(pfbench PRIVATE PF_HAVE_ZLIBNG ZLIBNG_NATIVE_API)
    target_include_directories(pfbench PRIVATE $<TARGET_PROPERTY:zlib-ng,INCLUDE_DIRECTORIES>)
  endif()
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
