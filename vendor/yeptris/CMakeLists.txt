# yeptris — Ultra-fast YAML 1.2 parser, emitter and streamer in C
# The YAML counterpart of libleptris. Pure C library with CLI and bindings.
cmake_minimum_required(VERSION 3.20)

project(yeptris
    VERSION 0.6.64
    DESCRIPTION "Ultra-fast YAML 1.2 parser, emitter and streamer in C"
    LANGUAGES C CXX
)

# C11 (the standard question, settled with evidence 2026-09-18): going
# C99 needs FOUR downgrades, not one — _Static_assert (fixed: the
# typedef-array YEPTRIS_CT_ASSERT), typedef redefinition (mapindex.c),
# _Thread_local (the per-thread error channel) and <stdatomics> (the
# lock-free handle pool). The last two are architecture-critical and
# absent from MSVC's C99 mode; pthread TLS / lock-based pools would
# regress the hot paths and weaken the concurrency contract.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# C++11 standard for tests and benchmarks.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC (the windows CI leg): the C++ suites use designated initializers
# (a GCC/Clang extension at C++11; MSVC requires C++20), and the test
# targets — test_conformance included — need test/compat on the include
# path for the dirent surface.
if(MSVC)
    set(CMAKE_CXX_STANDARD 20)
endif()

# MSVC multi-config: one runtime home so ctest -C Release and the CLI
# smoke find every exe (the per-target default scattered them)
if(MSVC)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

# MSVC's default stack is 1 MiB; the Unix builds run on 8 MiB — deep
# document paths (the depth guard's teardown) measured against the
# Unix default get the same room here
if(MSVC)
    add_link_options(/STACK:8388608)
endif()

# ---------------------------------------------------------------- options --
option(BUILD_TESTING "Build tests" ON)
option(YEPTRIS_BUILD_CLI "Build the yeptris CLI tool" ON)
option(YEPTRIS_BUILD_STATIC "Build the static library" ON)
option(YEPTRIS_BUILD_SHARED "Build the shared library" OFF)
option(YEPTRIS_BUILD_BENCHMARKS "Build performance benchmarks (TODO.impl/18)" OFF)
option(YEPTRIS_BUILD_JSONC_COMPAT
    "Build libyeptris-jsonc: the json-c drop-in migration layer (TODO.impl/21)" ON)
option(YEPTRIS_WITH_CBOR "Build the CBOR (RFC 8949) codec surface" ON)
option(YEPTRIS_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(YEPTRIS_ENABLE_TSAN "Enable ThreadSanitizer (requires all-TSAN build)" OFF)
option(YEPTRIS_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(YEPTRIS_ENABLE_FUZZING "Build libFuzzer harnesses (TODO.impl/19; requires clang)" OFF)
option(YEPTRIS_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# LTO: ON for Release/RelWithDebInfo by default (libleptris measured
# 1.3-3.5x on parse paths); override with -DYEPTRIS_ENABLE_LTO=OFF.
if(NOT DEFINED YEPTRIS_ENABLE_LTO)
    if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(YEPTRIS_ENABLE_LTO_DEFAULT ON)
    else()
        set(YEPTRIS_ENABLE_LTO_DEFAULT OFF)
    endif()
else()
    set(YEPTRIS_ENABLE_LTO_DEFAULT ${YEPTRIS_ENABLE_LTO})
endif()
option(YEPTRIS_ENABLE_LTO "Enable link-time optimization (default ON for Release)" ${YEPTRIS_ENABLE_LTO_DEFAULT})

# ------------------------------------------------------------- sanitizers --
if(YEPTRIS_ENABLE_ASAN AND YEPTRIS_ENABLE_TSAN)
    message(FATAL_ERROR "ASAN and TSAN are mutually exclusive")
endif()

if(YEPTRIS_ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
    message(STATUS "AddressSanitizer: ENABLED")
endif()

if(YEPTRIS_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=thread)
    message(STATUS "ThreadSanitizer: ENABLED")
endif()

if(YEPTRIS_ENABLE_UBSAN)
    add_compile_options(
        -fsanitize=undefined -fsanitize=float-cast-overflow
        -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=undefined -fsanitize=float-cast-overflow)
    message(STATUS "UndefinedBehaviorSanitizer: ENABLED (fatal)")
endif()

# libFuzzer implies ASAN+UBSan (the teptris fuzz.yml pattern: the
# memory-safety contracts ride the SAME binary as the fuzzer, so every
# nightly round reports UB at the crash site instead of libFuzzer's
# "combine with AddressSanitizer" note).
if(YEPTRIS_ENABLE_FUZZING)
    # instrument everything, but link the fuzzer MAIN only into the
    # harnesses: a global -fsanitize=fuzzer link option demands
    # LLVMFuzzerTestOneInput from every executable (the CLI and the
    # test binaries died on it once CXX also pinned to clang++)
    if(APPLE)
        # Apple ld64 cannot link UBSan'd static archives
        # ("invalid r_symbolnum"): ASan-only locally
        add_compile_options(-fsanitize=fuzzer-no-link,address -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address)
        message(STATUS "libFuzzer: ENABLED (implies ASAN; UBSan needs linux ld)")
    else()
        add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
            -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "libFuzzer: ENABLED (implies ASAN+UBSan)")
    endif()
endif()

# -------------------------------------------------------------------- LTO --
if(YEPTRIS_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT yeptris_lto_supported OUTPUT yeptris_lto_error)
    if(yeptris_lto_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "Link-time optimization: ENABLED")
    else()
        message(WARNING "YEPTRIS_ENABLE_LTO requested but not supported: ${yeptris_lto_error}")
    endif()
endif()

# -------------------------------------------------------------- visibility --
# Position-independent code for static libraries (embeddable in DSOs).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Hide all symbols by default; the public surface is exactly the
# YEPTRIS_API-marked functions (libleptris TODO 80 pattern).
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------- warnings --
# Scoped per-target so fetched dependencies (GTest) keep their own flags.
function(yeptris_set_warnings target)
    if(MSVC)
        # /wd4100 mirrors -Wno-unused-parameter; /experimental:c11atomics
        # un-breaks the C11 headers' stdatomic gate (the windows sentinel
        # surfaced both — the leg rides in the test matrix now)
        target_compile_options(${target} PRIVATE /W4 /wd4100 /wd4996 /experimental:c11atomics)
        if(YEPTRIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
        if(YEPTRIS_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ------------------------------------------------------------- versioning --
# CMakeLists project(VERSION) is the single source of truth (TODO.impl/01);
# scripts/bump-version.sh syncs vcpkg.json and CHANGELOG.md from it.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/src/include/yeptris/version.h.in
    ${CMAKE_BINARY_DIR}/generated/yeptris/version.h
    @ONLY
)

# --------------------------------------------------------------- subdirs --
enable_testing()

add_subdirectory(src)

if(YEPTRIS_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(BUILD_TESTING)
    if(MSVC)
        add_compile_options(-I${CMAKE_SOURCE_DIR}/test/compat)
    endif()
    add_subdirectory(test)

    # Conformance runner (TODO.impl/16): builds on demand; the corpus
    # arrives via scripts/fetch-corpora.sh.
    add_executable(test_conformance
        test/conformance/main.c
        test/conformance/suite.c
        test/conformance/tree.c)
    # Runs in CI after fetch-corpora.sh; passes-as-skip when absent.
    add_test(NAME conformance
        COMMAND test_conformance ${CMAKE_SOURCE_DIR}/test/conformance/data/yaml-test-suite/src)
    # strict: the suite is at 100% — a regression must fail CI, not hide
    # behind the non-strict exit (a tree-mismatch once slipped through)
    set_tests_properties(conformance PROPERTIES ENVIRONMENT "YEP_STRICT=1")
    # psych-pure's parse-{block,flow,scalar,props,stream} corpora,
    # ported by scripts/port-pure-tml.py (checked-in fixtures)
    add_test(NAME conformance-pure
        COMMAND test_conformance ${CMAKE_SOURCE_DIR}/test/conformance/data/psych-pure)
    set_tests_properties(conformance-pure PROPERTIES ENVIRONMENT "YEP_STRICT=1")
    target_link_libraries(test_conformance PRIVATE yeptris_static)
    target_include_directories(test_conformance PRIVATE
        ${CMAKE_SOURCE_DIR}/src/yeptris
        ${CMAKE_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/generated)
    yeptris_set_warnings(test_conformance)
endif()

if(YEPTRIS_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# --------------------------------------------------------------- install --
# (TODO.impl/20) targets, headers, pkg-config, and the find_package
# config; relative paths keep relocatable installs.
include(GNUInstallDirs)

if(TARGET yeptris_static)
    install(TARGETS yeptris_static
        EXPORT yeptris-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
if(TARGET yeptris_shared)
    install(TARGETS yeptris_shared
        EXPORT yeptris-targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(TARGET yeptris_jsonc)
    install(TARGETS yeptris_jsonc
        EXPORT yeptris-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
if(YEPTRIS_BUILD_CLI AND TARGET yeptris)
    install(TARGETS yeptris DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
)
install(FILES ${CMAKE_BINARY_DIR}/generated/yeptris/version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/yeptris
)

# pkg-config
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/yeptris.pc.in
    ${CMAKE_BINARY_DIR}/yeptris.pc @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/yeptris.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# find_package(yeptris) config
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_BINARY_DIR}/yeptris-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(EXPORT yeptris-targets
    NAMESPACE yeptris::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yeptris)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/yeptris-config.cmake.in
    ${CMAKE_BINARY_DIR}/yeptris-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yeptris)
install(FILES ${CMAKE_BINARY_DIR}/yeptris-config.cmake
    ${CMAKE_BINARY_DIR}/yeptris-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/yeptris)

