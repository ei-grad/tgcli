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

# These exact source commits are mirrored in release/dependencies.lock.json.
# A TDLib bump is a contract-change-class PR (REVIEW.md §7).
set(TGCLI_TDLIB_REV a17f87c4cff7b90b278d12b91ba0614383aaee82)
set(TGCLI_CLI11_REV 4160d259d961cd393fd8d67590a8c7d210207348)
set(TGCLI_NLOHMANN_JSON_REV 55f93686c01528224f448c19128836e7df245f72)
set(TGCLI_FMT_REV 40626af88bd7df9a5fb80be7b25ac85b122d6c21)
set(TGCLI_TOMLPLUSPLUS_REV 30172438cee64926dc41fdd9c11fb3ba5b2ba9de)
set(TGCLI_CATCH2_REV 2b60af89e23d28eefc081bc930831ee9d45ea58b)
set(TGCLI_JSONCONS_REV cb54cdc3134a62634466bf7bcd24f1a906f4ef25)

include(cmake/DependencyLock.cmake)
tgcli_assert_dependency_lock(tdlib ${TGCLI_TDLIB_REV})
tgcli_assert_dependency_lock(cli11 ${TGCLI_CLI11_REV})
tgcli_assert_dependency_lock(nlohmann_json ${TGCLI_NLOHMANN_JSON_REV})
tgcli_assert_dependency_lock(fmt ${TGCLI_FMT_REV})
tgcli_assert_dependency_lock(tomlplusplus ${TGCLI_TOMLPLUSPLUS_REV})
tgcli_assert_tdlib_build_script(${TGCLI_TDLIB_REV})

include(FetchContent)

FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11
    GIT_TAG ${TGCLI_CLI11_REV})
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG ${TGCLI_NLOHMANN_JSON_REV})
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt
    GIT_TAG ${TGCLI_FMT_REV})
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus
    GIT_TAG ${TGCLI_TOMLPLUSPLUS_REV})
FetchContent_MakeAvailable(cli11 nlohmann_json fmt tomlplusplus)
tgcli_assert_resolved_git_revision(cli11 "${cli11_SOURCE_DIR}" ${TGCLI_CLI11_REV})
tgcli_assert_resolved_git_revision(
    nlohmann_json "${nlohmann_json_SOURCE_DIR}" ${TGCLI_NLOHMANN_JSON_REV})
tgcli_assert_resolved_git_revision(fmt "${fmt_SOURCE_DIR}" ${TGCLI_FMT_REV})
tgcli_assert_resolved_git_revision(
    tomlplusplus "${tomlplusplus_SOURCE_DIR}" ${TGCLI_TOMLPLUSPLUS_REV})

if(TGCLI_SYSTEM_TDLIB)
    find_package(Td REQUIRED)
    if(NOT TARGET Td::TdStatic)
        message(FATAL_ERROR "Td package found but Td::TdStatic is missing; "
            "build the prefix with scripts/build-tdlib.sh")
    endif()
    tgcli_assert_tdlib_prefix_provenance("${Td_DIR}" ${TGCLI_TDLIB_REV})
else()
    set(BUILD_TESTING OFF)
    FetchContent_Declare(td
        GIT_REPOSITORY https://github.com/tdlib/td
        GIT_TAG ${TGCLI_TDLIB_REV})
    FetchContent_MakeAvailable(td)
    tgcli_assert_resolved_git_revision(tdlib "${td_SOURCE_DIR}" ${TGCLI_TDLIB_REV})
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
    src/proto/destructive_plan.cpp
    src/proto/frame.cpp
    src/proto/frame_io.cpp)
target_link_libraries(tgcli_proto
    PUBLIC tgcli_common nlohmann_json::nlohmann_json)

add_library(tgcli_util STATIC
    src/common/config.cpp
    src/common/daemon_lock.cpp
    src/common/paths.cpp
    src/common/secret_hook.cpp)
target_link_libraries(tgcli_util
    PUBLIC tgcli_common
    PRIVATE tomlplusplus::tomlplusplus)
target_link_libraries(tgcli_proto PRIVATE tgcli_util)

add_library(tgcli_td_runtime STATIC
    src/core/td_log.cpp
    src/core/td_runtime.cpp)
target_link_libraries(tgcli_td_runtime
    PUBLIC tgcli_common
    PRIVATE Td::TdStatic)

add_library(tgcli_core STATIC
    src/core/auth_bootstrap.cpp
    src/core/td_authorization.cpp
    src/core/td_client.cpp)
target_link_libraries(tgcli_core
    PUBLIC tgcli_common tgcli_util
    PRIVATE tgcli_td_runtime)

add_library(tgcli_daemon STATIC
    src/daemon/activity_tracker.cpp
    src/daemon/config_runtime.cpp
    src/daemon/destructive_contract.cpp
    src/daemon/dispatch.cpp
    src/daemon/request_session.cpp
    src/daemon/account_commands.cpp
    src/daemon/login_commands.cpp
    src/daemon/commands.cpp
    src/daemon/server.cpp
    src/daemon/daemon_run.cpp)
target_link_libraries(tgcli_daemon
    PUBLIC tgcli_proto tgcli_util
    PRIVATE tgcli_core)

add_library(tgcli_cli STATIC
    src/cli/client.cpp
    src/cli/control_stop.cpp
    src/cli/prompt.cpp
    src/cli/render.cpp
    src/cli/routing.cpp
    src/cli/surface_safety.cpp)
target_link_libraries(tgcli_cli
    PUBLIC tgcli_proto tgcli_util tgcli_daemon
    PRIVATE fmt::fmt)

add_executable(tgcli
    src/main.cpp)
target_link_libraries(tgcli
    PRIVATE tgcli_cli tgcli_daemon CLI11::CLI11)

if(TGCLI_BUILD_TESTS)
    set(JSONCONS_BUILD_TESTS OFF CACHE BOOL "Disable jsoncons upstream tests" FORCE)
    tgcli_assert_dependency_lock(catch2 ${TGCLI_CATCH2_REV})
    tgcli_assert_dependency_lock(jsoncons ${TGCLI_JSONCONS_REV})
    FetchContent_Declare(jsoncons
        GIT_REPOSITORY https://github.com/danielaparker/jsoncons
        GIT_TAG ${TGCLI_JSONCONS_REV})
    FetchContent_Declare(catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2
        GIT_TAG ${TGCLI_CATCH2_REV})
    FetchContent_MakeAvailable(catch2 jsoncons)
    tgcli_assert_resolved_git_revision(catch2 "${catch2_SOURCE_DIR}" ${TGCLI_CATCH2_REV})
    tgcli_assert_resolved_git_revision(jsoncons "${jsoncons_SOURCE_DIR}" ${TGCLI_JSONCONS_REV})
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    enable_testing()
    add_subdirectory(tests)
endif()
