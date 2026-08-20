# Resolves the build revision that `tgcli version` reports and materializes
# the generated version header. Runs both at configure time and on every
# build, so a rebuilt working tree never reports a stale revision; the
# generated header is replaced only when its contents actually change.
#
# Required cache entries: TGCLI_SOURCE_DIR, TGCLI_TEMPLATE, TGCLI_OUTPUT,
# TGCLI_VERSION.

foreach(tgcli_required_variable TGCLI_SOURCE_DIR TGCLI_TEMPLATE TGCLI_OUTPUT TGCLI_VERSION)
    if(NOT DEFINED ${tgcli_required_variable} OR "${${tgcli_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${tgcli_required_variable} is required")
    endif()
endforeach()

set(TGCLI_BUILD_COMMIT "")

find_program(TGCLI_GIT_EXECUTABLE NAMES git)

# A source tree that merely sits inside an unrelated checkout must report no
# revision at all: git resolves from parent directories, so the work-tree root
# has to be the source tree itself before any revision is trusted.
set(TGCLI_SOURCE_IS_CHECKOUT FALSE)
if(TGCLI_GIT_EXECUTABLE AND IS_DIRECTORY "${TGCLI_SOURCE_DIR}")
    execute_process(
        COMMAND "${TGCLI_GIT_EXECUTABLE}" -C "${TGCLI_SOURCE_DIR}" rev-parse --show-toplevel
        RESULT_VARIABLE tgcli_toplevel_result
        OUTPUT_VARIABLE tgcli_toplevel
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(tgcli_toplevel_result EQUAL 0 AND NOT tgcli_toplevel STREQUAL "")
        file(REAL_PATH "${TGCLI_SOURCE_DIR}" tgcli_source_real)
        file(REAL_PATH "${tgcli_toplevel}" tgcli_toplevel_real)
        if(tgcli_source_real STREQUAL tgcli_toplevel_real)
            set(TGCLI_SOURCE_IS_CHECKOUT TRUE)
        endif()
    endif()
endif()

if(TGCLI_SOURCE_IS_CHECKOUT)
    execute_process(
        COMMAND "${TGCLI_GIT_EXECUTABLE}" -C "${TGCLI_SOURCE_DIR}"
                rev-parse --verify --short=7 "HEAD^{commit}"
        RESULT_VARIABLE tgcli_sha_result
        OUTPUT_VARIABLE tgcli_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    string(LENGTH "${tgcli_sha}" tgcli_sha_length)
    if(tgcli_sha_result EQUAL 0 AND tgcli_sha MATCHES "^[0-9a-f]+$" AND
       tgcli_sha_length GREATER_EQUAL 7)
        execute_process(
            COMMAND "${TGCLI_GIT_EXECUTABLE}" -C "${TGCLI_SOURCE_DIR}" status --porcelain
                    --untracked-files=no --ignore-submodules=none
            RESULT_VARIABLE tgcli_status_result
            OUTPUT_VARIABLE tgcli_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(tgcli_status_result EQUAL 0)
            if(NOT tgcli_status STREQUAL "")
                set(TGCLI_BUILD_COMMIT "${tgcli_sha}-dirty")
            else()
                set(TGCLI_BUILD_COMMIT "${tgcli_sha}")
            endif()
        endif()
    endif()
endif()

configure_file("${TGCLI_TEMPLATE}" "${TGCLI_OUTPUT}.candidate" @ONLY)
# Tests replace only this command to exercise the otherwise platform-dependent
# copy failure path; production generation always uses copy_if_different.
if(DEFINED TGCLI_COPY_COMMAND)
    set(tgcli_copy_command "${TGCLI_COPY_COMMAND}")
else()
    set(tgcli_copy_command "${CMAKE_COMMAND}" -E copy_if_different)
endif()
execute_process(COMMAND ${tgcli_copy_command} "${TGCLI_OUTPUT}.candidate" "${TGCLI_OUTPUT}"
                RESULT_VARIABLE tgcli_copy_result
                ERROR_VARIABLE tgcli_copy_error)
file(REMOVE "${TGCLI_OUTPUT}.candidate")
if(NOT tgcli_copy_result EQUAL 0)
    message(FATAL_ERROR "failed to replace ${TGCLI_OUTPUT}: ${tgcli_copy_error}")
endif()
