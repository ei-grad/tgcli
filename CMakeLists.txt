cmake_minimum_required(VERSION 3.24)

project(tgcli VERSION 0.1.0 LANGUAGES CXX)

# C++20 is applied per-target through tgcli_common below — a global
# CMAKE_CXX_STANDARD would leak into the FetchContent tdlib build and break
# its C++17 sources (u8"" literals become char8_t under C++20).
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TGCLI_SYSTEM_TDLIB
    "Use a prebuilt tdlib prefix (scripts/build-tdlib.sh) instead of building via FetchContent"
    OFF)
option(TGCLI_BUILD_TESTS "Build tests" ON)
option(TGCLI_WERROR "Treat compiler warnings as errors" OFF)

# The revision here and in scripts/build-tdlib.sh must stay in sync; bumping
# it is a contract-change-class PR (REVIEW.md §7).
set(TGCLI_TDLIB_REV a17f87c4cff7b90b278d12b91ba0614383aaee82) # v1.8.65

include(FetchContent)

FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG v2.5.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt
    GIT_TAG 11.2.0
    GIT_SHALLOW TRUE)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(cli11 nlohmann_json fmt tomlplusplus)

if(TGCLI_SYSTEM_TDLIB)
    find_package(Td REQUIRED)
    if(NOT TARGET Td::TdStatic)
        message(FATAL_ERROR "Td package found but Td::TdStatic is missing; "
            "build the prefix with scripts/build-tdlib.sh")
    endif()
else()
    set(BUILD_TESTING OFF)
    FetchContent_Declare(td
        GIT_REPOSITORY https://github.com/tdlib/td
        GIT_TAG ${TGCLI_TDLIB_REV})
    FetchContent_MakeAvailable(td)
    # The installed-prefix path imports tdlib headers as SYSTEM; give the
    # in-tree build the same treatment so tdlib's headers are exempt from
    # our -Werror warning set.
    foreach(td_target tdclient tdapi tdactor tdcore tdutils tdnet tddb tdmtproto tdjson_private)
        if(TARGET ${td_target})
            get_target_property(td_includes ${td_target} INTERFACE_INCLUDE_DIRECTORIES)
            if(td_includes)
                set_target_properties(${td_target} PROPERTIES
                    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${td_includes}")
            endif()
        endif()
    endforeach()
endif()

configure_file(cmake/version.hpp.in include/tgcli/version.hpp @ONLY)

add_library(tgcli_common INTERFACE)
target_compile_features(tgcli_common INTERFACE cxx_std_20)
target_include_directories(tgcli_common INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/include)
target_compile_options(tgcli_common INTERFACE
    -Wall -Wextra -Wpedantic
    $<$<BOOL:${TGCLI_WERROR}>:-Werror>)

add_library(tgcli_proto STATIC
    src/proto/frame.cpp
    src/proto/frame_io.cpp)
target_link_libraries(tgcli_proto
    PUBLIC tgcli_common nlohmann_json::nlohmann_json)

add_library(tgcli_util STATIC
    src/common/daemon_lock.cpp
    src/common/paths.cpp)
target_link_libraries(tgcli_util
    PUBLIC tgcli_common)

add_library(tgcli_td_runtime STATIC
    src/core/td_runtime.cpp)
target_link_libraries(tgcli_td_runtime
    PUBLIC tgcli_common
    PRIVATE Td::TdStatic)

add_library(tgcli_core STATIC
    src/core/td_client.cpp)
target_link_libraries(tgcli_core
    PUBLIC tgcli_common
    PRIVATE tgcli_td_runtime)

add_library(tgcli_daemon STATIC
    src/daemon/dispatch.cpp
    src/daemon/request_session.cpp
    src/daemon/commands.cpp
    src/daemon/server.cpp
    src/daemon/daemon_run.cpp)
target_link_libraries(tgcli_daemon
    PUBLIC tgcli_proto tgcli_util
    PRIVATE tgcli_core)

add_library(tgcli_cli STATIC
    src/cli/client.cpp
    src/cli/prompt.cpp
    src/cli/render.cpp)
target_link_libraries(tgcli_cli
    PUBLIC tgcli_proto tgcli_util tgcli_daemon
    PRIVATE fmt::fmt)

add_executable(tgcli
    src/main.cpp)
target_link_libraries(tgcli
    PRIVATE tgcli_cli tgcli_daemon CLI11::CLI11)

if(TGCLI_BUILD_TESTS)
    set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "Disable jsoncons upstream tests" FORCE)
    FetchContent_Declare(jsoncons
        GIT_REPOSITORY https://github.com/danielaparker/jsoncons
        GIT_TAG cb54cdc3134a62634466bf7bcd24f1a906f4ef25)
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2
        GIT_TAG v3.8.1
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(catch2 jsoncons)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    enable_testing()
    add_subdirectory(tests)
endif()
