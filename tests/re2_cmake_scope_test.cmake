cmake_minimum_required(VERSION 3.24)

foreach(required_variable REPO_ROOT TEST_OUTPUT_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(source_directory "${TEST_OUTPUT_DIR}/source")
set(build_directory "${TEST_OUTPUT_DIR}/build")
file(MAKE_DIRECTORY "${source_directory}/re2" "${source_directory}/fmt")
file(WRITE "${source_directory}/re2/re2.cc" "void re2_probe() {}\n")
file(WRITE "${source_directory}/re2/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(re2_probe LANGUAGES CXX)
option(RE2_BUILD_TESTING "Build RE2 tests" ON)
option(USEPCRE "Build RE2 PCRE support" ON)
add_library(re2 re2.cc)
add_library(re2::re2 ALIAS re2)
set_property(TARGET re2 PROPERTY TGCLI_PROBE_OPTIONS
    "${BUILD_SHARED_LIBS};${RE2_BUILD_TESTING};${USEPCRE}")
if(RE2_BUILD_TESTING)
    add_custom_target(testing)
endif()
if(USEPCRE)
    add_custom_target(regexp_benchmark)
endif()
]=])
file(WRITE "${source_directory}/fmt/fmt.cc" "void fmt_probe() {}\n")
file(WRITE "${source_directory}/fmt/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(fmt_probe LANGUAGES CXX)
add_library(fmt fmt.cc)
]=])
file(WRITE "${source_directory}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(re2_scope_probe LANGUAGES CXX)
include(FetchContent)
FetchContent_Declare(re2 SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/re2")
include("${TGCLI_RE2_HELPER}")

set(BUILD_SHARED_LIBS ON)
set(RE2_BUILD_TESTING ON)
set(USEPCRE ON)
tgcli_make_re2_available()

foreach(option BUILD_SHARED_LIBS RE2_BUILD_TESTING USEPCRE)
    if(NOT ${option})
        message(FATAL_ERROR "caller normal variable ${option} did not survive RE2")
    endif()
    get_property(cache_value CACHE ${option} PROPERTY VALUE)
    if(NOT cache_value STREQUAL "ON")
        message(FATAL_ERROR "caller cache entry ${option} did not survive RE2")
    endif()
endforeach()
get_target_property(re2_options re2 TGCLI_PROBE_OPTIONS)
get_target_property(re2_type re2 TYPE)
if(NOT re2_options STREQUAL "OFF;OFF;OFF" OR
   NOT re2_type STREQUAL "STATIC_LIBRARY" OR
   TARGET testing OR TARGET regexp_benchmark)
    message(FATAL_ERROR "RE2 did not retain its private static runtime options")
endif()

FetchContent_Declare(fmt SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fmt")
FetchContent_MakeAvailable(fmt)
get_target_property(fmt_type fmt TYPE)
if(NOT fmt_type STREQUAL "SHARED_LIBRARY")
    message(FATAL_ERROR "RE2 configuration leaked into the following fmt dependency")
endif()
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${source_directory}"
        -B "${build_directory}"
        -G Ninja
        -DBUILD_SHARED_LIBS=ON
        -DRE2_BUILD_TESTING=ON
        -DUSEPCRE=ON
        -DTGCLI_RE2_HELPER=${REPO_ROOT}/cmake/Re2Dependency.cmake
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "RE2 scoped configuration probe failed:\n${configure_output}\n${configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_directory}"
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "RE2 scoped build probe failed:\n${build_output}\n${build_error}")
endif()
