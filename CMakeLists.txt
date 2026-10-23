cmake_minimum_required(VERSION 3.16)
# -O2, not CMake's default -O3: -O3 measured 20-30% slower on the decoder's 2-row wavefront kernels
# (photo ga8/g8 decode), encoder unchanged. Set before project() so it becomes the cached default.
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "Release C flags")
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()
project(libqpng VERSION 1.2.0 LANGUAGES C)

option(QPNG_BUILD_TESTS "Build tests" ON)
option(QPNG_BUILD_TOOLS "Build qpng-cli and the benchmark tool" ON)
option(QPNG_BUILD_SHARED "Build a shared library (default: static)" OFF)
option(QPNG_SANITIZE "Build with -fsanitize=address,undefined" OFF)
option(QPNG_LIBFUZZER "Build libFuzzer fuzz targets (clang)" OFF)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib-ng/zlib-ng.h.in)
  set(QPNG_ZLIBNG_DEFAULT ON)
else()
  set(QPNG_ZLIBNG_DEFAULT OFF)
endif()
option(QPNG_WITH_ZLIBNG "Build the optional zlib-ng deflate backend (third_party/zlib-ng)" ${QPNG_ZLIBNG_DEFAULT})
set(QPNG_CORPUS_DIR "" CACHE PATH "Optional directory with the generated format corpus (tests)")
set(QPNG_PNGSUITE_DIR "" CACHE PATH "Optional PngSuite directory (tests)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # GNU C11: target attributes, __builtin_cpu_supports
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(QPNG_LIBFUZZER)
  add_compile_options(-fsanitize=fuzzer-no-link)   # coverage instrumentation for the library itself
endif()
if(QPNG_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- vendored libdeflate (modified; see third_party/libdeflate/QPNG_CHANGES.md) ----
set(LD ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdeflate)
file(GLOB LD_SOURCES ${LD}/lib/*.c ${LD}/lib/x86/*.c ${LD}/lib/arm/*.c ${LD}/lib/riscv/*.c)
add_library(qpng_libdeflate OBJECT ${LD_SOURCES})
target_include_directories(qpng_libdeflate PRIVATE ${LD})
target_compile_options(qpng_libdeflate PRIVATE -include ${LD}/qpng_rename.h -w)
target_compile_definitions(qpng_libdeflate PRIVATE LIBDEFLATE_STATIC)

# ---- optional vendored zlib-ng (native zng_* API, ZLIB_COMPAT OFF: no clash with a system zlib) ----
# Built as a static library in its own scope; its objects are folded into libqpng so consumers need nothing.
# Runtime CPU dispatch (WITH_RUNTIME_CPU_DETECTION), never -march=native.
if(QPNG_WITH_ZLIBNG)
  set(ZLIB_COMPAT OFF CACHE BOOL "" FORCE)
  set(WITH_GTEST OFF CACHE BOOL "" FORCE)
  set(WITH_GZFILEOP OFF CACHE BOOL "" FORCE)
  set(WITH_NATIVE_INSTRUCTIONS OFF CACHE BOOL "" FORCE)
  set(WITH_RUNTIME_CPU_DETECTION ON CACHE BOOL "" FORCE)
  set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
  set(QPNG_SAVED_BUILD_TESTING ${BUILD_TESTING})
  set(BUILD_TESTING OFF)
  set(BUILD_SHARED_LIBS OFF)
  add_subdirectory(third_party/zlib-ng EXCLUDE_FROM_ALL)
  set(BUILD_TESTING ${QPNG_SAVED_BUILD_TESTING})
  unset(BUILD_SHARED_LIBS)
  target_compile_options(zlib-ng PRIVATE -w)
endif()

# ---- qpng ----
# Symbol hygiene: libqpng exports only the public qpng_* API declared with QPNG_API in include/qpng.h.
#  * every object is built with hidden visibility (vendored code included);
#  * shared: linker version script (generated from the header) makes everything else local;
#  * static: the objects are partially linked into one relocatable object (ld -r) and objcopy localizes every
#    global outside the API (vendored libdeflate / zlib-ng internals, zng_*, qpng__* helpers), so the archive can be
#    linked next to stock zlib / zlib-ng / libdeflate. Tests and tools that need the internals link
#    qpng_internal (same objects, nothing localized).
option(QPNG_HIDE_SYMBOLS "Localize all non-API symbols in libqpng (needs ld -r + objcopy, ELF)" ON)
set(QPNG_SOURCES src/common.c src/decode.c src/unfilter.c src/expand.c src/encode.c src/filter_enc.c src/deflate_backend.c src/metadata.c src/stream.c src/apng.c)
add_library(qpng_obj OBJECT ${QPNG_SOURCES})
target_include_directories(qpng_obj PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                       PRIVATE src ${LD})
target_compile_definitions(qpng_obj PRIVATE LIBDEFLATE_STATIC)
target_compile_options(qpng_obj PRIVATE -Wall -Wextra -Wno-unused-parameter)
set(QPNG_ALL_OBJECTS $<TARGET_OBJECTS:qpng_obj> $<TARGET_OBJECTS:qpng_libdeflate>)
if(QPNG_WITH_ZLIBNG)
  target_compile_definitions(qpng_obj PRIVATE QPNG_HAVE_ZLIBNG ZLIBNG_NATIVE_API)
  target_include_directories(qpng_obj PRIVATE $<TARGET_PROPERTY:zlib-ng,INCLUDE_DIRECTORIES>)
  set_target_properties(zlib-ng PROPERTIES C_VISIBILITY_PRESET hidden)
  list(APPEND QPNG_ALL_OBJECTS $<TARGET_OBJECTS:zlib-ng>)
endif()
if(QPNG_BUILD_SHARED)
  target_compile_definitions(qpng_obj PUBLIC QPNG_SHARED)
endif()

# public API symbol list, from the header
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/include/qpng.h QPNG_API_LINES REGEX "^QPNG_API ")
set(QPNG_EXPORTS "")
foreach(l ${QPNG_API_LINES})
  string(REGEX MATCH "(qpng_[a-z0-9_]+)\\(" m "${l}")
  list(APPEND QPNG_EXPORTS ${CMAKE_MATCH_1})
endforeach()
list(REMOVE_DUPLICATES QPNG_EXPORTS)
string(REPLACE ";" "\n" QPNG_EXPORTS_TXT "${QPNG_EXPORTS}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qpng.exports "${QPNG_EXPORTS_TXT}\n")
string(REPLACE ";" ";\n    " QPNG_EXPORTS_MAP "${QPNG_EXPORTS}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/qpng.map "QPNG_1 {\n  global:\n    ${QPNG_EXPORTS_MAP};\n  local: *;\n};\n")

# internal static library (tests, benchmark tool)
add_library(qpng_internal STATIC ${QPNG_ALL_OBJECTS})
target_include_directories(qpng_internal PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
set_target_properties(qpng_internal PROPERTIES LINKER_LANGUAGE C)

find_program(QPNG_OBJCOPY NAMES objcopy llvm-objcopy)
set(QPNG_LD ${CMAKE_LINKER})
if(QPNG_BUILD_SHARED)
  add_library(qpng SHARED ${QPNG_ALL_OBJECTS})
  target_compile_definitions(qpng PUBLIC QPNG_SHARED)
  if(QPNG_HIDE_SYMBOLS)
    target_link_options(qpng PRIVATE -Wl,--version-script=${CMAKE_CURRENT_BINARY_DIR}/qpng.map)
    set_property(TARGET qpng APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/qpng.map)
  endif()
elseif(QPNG_HIDE_SYMBOLS AND QPNG_OBJCOPY AND QPNG_LD AND NOT APPLE AND NOT WIN32)
  set(QPNG_PUB ${CMAKE_CURRENT_BINARY_DIR}/qpng_public.o)
  add_custom_command(OUTPUT ${QPNG_PUB}
    COMMAND ${QPNG_LD} -r -o ${QPNG_PUB}.tmp ${QPNG_ALL_OBJECTS}
    COMMAND ${QPNG_OBJCOPY} --keep-global-symbols=${CMAKE_CURRENT_BINARY_DIR}/qpng.exports ${QPNG_PUB}.tmp ${QPNG_PUB}
    DEPENDS ${QPNG_ALL_OBJECTS} ${CMAKE_CURRENT_BINARY_DIR}/qpng.exports
    COMMAND_EXPAND_LISTS VERBATIM
    COMMENT "libqpng: partial link + localize non-API symbols")
  set_source_files_properties(${QPNG_PUB} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
  add_library(qpng STATIC ${QPNG_PUB})
  set_target_properties(qpng PROPERTIES LINKER_LANGUAGE C)
  add_dependencies(qpng qpng_obj qpng_libdeflate)   # object-file DEPENDS alone does not order the targets
  if(QPNG_WITH_ZLIBNG)
    add_dependencies(qpng zlib-ng)
  endif()
else()
  add_library(qpng STATIC ${QPNG_ALL_OBJECTS})
  set_target_properties(qpng PROPERTIES LINKER_LANGUAGE C)
endif()
target_include_directories(qpng PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
set_target_properties(qpng PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})

include(GNUInstallDirs)
install(TARGETS qpng EXPORT qpngTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/qpng.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
configure_file(qpng.pc.in qpng.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/qpng.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
install(EXPORT qpngTargets NAMESPACE qpng:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qpng)

if(QPNG_BUILD_TOOLS)
  add_executable(qpng-cli tools/qpng-cli.c)
  target_link_libraries(qpng-cli PRIVATE qpng)
  add_executable(qpngbench bench/qpngbench.c)
  target_link_libraries(qpngbench PRIVATE qpng_internal)
  target_include_directories(qpngbench PRIVATE src ${LD})   # raw-deflate benchmark uses the vendored libdeflate API
  if(QPNG_WITH_ZLIBNG)   # ... and zlib-ng's (symbols inside libqpng)
    target_compile_definitions(qpngbench PRIVATE QPNG_HAVE_ZLIBNG ZLIBNG_NATIVE_API)
    target_include_directories(qpngbench PRIVATE $<TARGET_PROPERTY:zlib-ng,INCLUDE_DIRECTORIES>)
  endif()
endif()

# Fuzz targets: always built as standalone replay/mutation drivers (also usable with AFL++);
# with -DQPNG_LIBFUZZER=ON (clang) additionally as libFuzzer binaries.
if(QPNG_BUILD_TESTS OR QPNG_LIBFUZZER)
  foreach(t decode roundtrip stream)
    add_executable(fuzz_${t} fuzz/fuzz_${t}.c fuzz/standalone.c)
    target_link_libraries(fuzz_${t} PRIVATE qpng)
    if(QPNG_LIBFUZZER)
      add_executable(libfuzzer_${t} fuzz/fuzz_${t}.c)
      target_compile_options(libfuzzer_${t} PRIVATE -fsanitize=fuzzer)
      target_link_options(libfuzzer_${t} PRIVATE -fsanitize=fuzzer)
      target_link_libraries(libfuzzer_${t} PRIVATE qpng)
    endif()
  endforeach()
endif()

if(QPNG_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
