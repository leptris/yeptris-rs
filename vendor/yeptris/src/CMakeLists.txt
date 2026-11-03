# libyeptris — core library sources.
#
# Subsystems land with their TODO.impl items (common/ 02, memory/ 03,
# simd/ 04, encoding/ 05, scan/ 06, parse/ 07+). The SIMD per-ISA TUs get
# their own -m flags here when item 04 lands.

set(YEPTRIS_SOURCES
    yeptris/version.c
    yeptris/common/cpu.c
    yeptris/common/chartype.c
    yeptris/common/string_view.c
    yeptris/common/error.c
    yeptris/common/nametab.c
    yeptris/common/simd_text.c
    yeptris/common/simd_text_scalar.c
    yeptris/memory/allocator.c
    yeptris/memory/pool.c
    yeptris/memory/arena.c
    yeptris/encoding/bom.c
    yeptris/encoding/transcode.c
    yeptris/encoding/utf8_validate.c
    yeptris/scan/json.c
    yeptris/scan/scan.c
    yeptris/parse/scalars.c
    yeptris/parse/engine.c
    yeptris/dom/dom.c
    yeptris/dom/ytape.c
    yeptris/dom/hpool.c
    yeptris/dom/mapindex.c
    yeptris/dom/mutate.c
    yeptris/parse.c
    yeptris/build.c
    yeptris/events/capture.c
    yeptris/events/push.c
    yeptris/events/pull.c
    yeptris/events/recorder.c
    yeptris/events/iterparse.c
    yeptris/events/yaml_compat.c
    yeptris/resolve/tags.c
    yeptris/resolve/core12.c
    yeptris/resolve/compat11.c
    yeptris/resolve/json12.c
    yeptris/parse/numbers.c
    yeptris/events/values.c
    yeptris/schema.c
    yeptris/validate/regexp.c
    yeptris/validate/validate.c
    yeptris/validate/compile.c
    yeptris/validate/jsonschema.c
    yeptris/tape.c
    yeptris/plan.c

    yeptris/marshal.c
    yeptris/visit/json_visit.c
    yeptris/visit/yaml_visit.c
    yeptris/visit/dom_visit.c
    yeptris/emit/style.c
    yeptris/emit/writer.c
    yeptris/emit/emit.c
    yeptris/emit/float/print.c
    yeptris/emit/float/dragon.c
    yeptris/emit/float/fpconv.c
)

if(YEPTRIS_WITH_CBOR)
    list(APPEND YEPTRIS_SOURCES yeptris/cbor/decode.c yeptris/cbor/encode.c
        yeptris/cbor/from_marshal.c)
endif()

# AOT SIMD TUs (TODO.impl/04): per-ISA compile flags; the TUs and the
# dispatch share architecture guards, so links resolve on every platform.
# CMAKE_OSX_ARCHITECTURES participates: an -arch x86_64 cross-build on
# an arm Mac compiles the dispatch for x86 — the source pick must
# follow the TARGET arch, not the host (the all-platform wheels ride
# this; host-processor-only selection linked NEON objects into an
# x86_64 image).
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|i[3-6]86)$" OR
    CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    list(APPEND YEPTRIS_SOURCES yeptris/common/simd_text_avx2.c)
    if(MSVC)
        set_source_files_properties(yeptris/common/simd_text_avx2.c
            PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(yeptris/common/simd_text_avx2.c
            PROPERTIES COMPILE_OPTIONS "-mavx2;-mpopcnt")
    endif()
elseif((CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR
    CMAKE_OSX_ARCHITECTURES MATCHES "arm64") AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
    list(APPEND YEPTRIS_SOURCES yeptris/common/simd_text_neon.c)
endif()

set(YEPTRIS_PUBLIC_HEADERS
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/api.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/dom.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/error.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/events.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/emit.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/resolve.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/parse.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/values.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/schema.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/validate.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/tape.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/plan.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/marshal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/visit.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/json.h
    ${CMAKE_CURRENT_SOURCE_DIR}/include/yeptris/types.h
)

set(YEPTRIS_INCLUDE_DIRS
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
)

# json-c drop-in (TODO.impl/21): REAL json-c symbol names in its own
# library — users link -lyeptris-jsonc instead of -ljson-c.
option(YEPTRIS_BUILD_YAJL_COMPAT
    "Build libyeptris-yajl: the yajl generator drop-in (TODO.impl/21)" ON)
if(YEPTRIS_BUILD_YAJL_COMPAT AND YEPTRIS_BUILD_STATIC)
    add_library(yeptris_yajl STATIC yeptris/jsonapi/yajl_compat.c)
    set_target_properties(yeptris_yajl PROPERTIES OUTPUT_NAME yeptris-yajl)
    target_link_libraries(yeptris_yajl PUBLIC yeptris_static)
    target_include_directories(yeptris_yajl PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_include_directories(yeptris_yajl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/yeptris)
    yeptris_set_warnings(yeptris_yajl)
    add_library(yeptris::yajl ALIAS yeptris_yajl)
endif()

if(YEPTRIS_BUILD_JSONC_COMPAT AND YEPTRIS_BUILD_STATIC)
    add_library(yeptris_jsonc STATIC yeptris/jsonapi/jsonc_compat.c)
    set_target_properties(yeptris_jsonc PROPERTIES OUTPUT_NAME yeptris-jsonc)
    target_link_libraries(yeptris_jsonc PUBLIC yeptris_static)
    target_include_directories(yeptris_jsonc PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    # internal headers (emit/writer.h, float/api.h): PRIVATE — they
    # never leak into the compat library's public interface
    target_include_directories(yeptris_jsonc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/yeptris)
    yeptris_set_warnings(yeptris_jsonc)
    add_library(yeptris::jsonc ALIAS yeptris_jsonc)
endif()

if(YEPTRIS_BUILD_STATIC)
    find_package(Threads REQUIRED)

add_library(yeptris_static STATIC ${YEPTRIS_SOURCES})
    add_library(yeptris::yeptris ALIAS yeptris_static)
    set_target_properties(yeptris_static PROPERTIES OUTPUT_NAME yeptris)
    target_include_directories(yeptris_static
        PUBLIC ${YEPTRIS_INCLUDE_DIRS}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/yeptris) # internal root-relative includes
    target_compile_definitions(yeptris_static PRIVATE YEPTRIS_BUILDING)
    if(YEPTRIS_WITH_CBOR)
        target_compile_definitions(yeptris_static PUBLIC YEPTRIS_WITH_CBOR)
    endif()
    target_link_libraries(yeptris_static PRIVATE Threads::Threads)
    yeptris_set_warnings(yeptris_static)
endif()

if(YEPTRIS_BUILD_SHARED)
    add_library(yeptris_shared SHARED ${YEPTRIS_SOURCES})
    set_target_properties(yeptris_shared
        PROPERTIES OUTPUT_NAME yeptris
                    SOVERSION ${PROJECT_VERSION_MAJOR})
    target_include_directories(yeptris_shared
        PUBLIC ${YEPTRIS_INCLUDE_DIRS}
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/yeptris
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/yeptris/emit/float)
    target_compile_definitions(yeptris_shared PRIVATE YEPTRIS_BUILDING)
    if(YEPTRIS_WITH_CBOR)
        target_compile_definitions(yeptris_shared PUBLIC YEPTRIS_WITH_CBOR)
    endif()
    yeptris_set_warnings(yeptris_shared)
endif()
