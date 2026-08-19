# Resolves the build revision that `tgcli version` reports and materializes
# the generated version header. Runs both at configure time and on every
# build, so a rebuilt working tree never reports a stale revision; the
# generated header is replaced only when its contents actually change.
#
# Required cache entries: TGCLI_SOURCE_DIR, TGCLI_TEMPLATE, TGCLI_OUTPUT,
# TGCLI_VERSION.

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
        COMMAND "${TGCLI_GIT_EXECUTABLE}" -C "${TGCLI_SOURCE_DIR}" describe --tags --exact-match
        RESULT_VARIABLE tgcli_tag_result
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT tgcli_tag_result EQUAL 0)
        execute_process(
            COMMAND "${TGCLI_GIT_EXECUTABLE}" -C "${TGCLI_SOURCE_DIR}" rev-parse --short=7 HEAD
            RESULT_VARIABLE tgcli_sha_result
            OUTPUT_VARIABLE tgcli_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(tgcli_sha_result EQUAL 0 AND tgcli_sha MATCHES "^[0-9a-f]+$")
            execute_process(
                COMMAND "${TGCLI_GIT_EXECUTABLE}" -C "${TGCLI_SOURCE_DIR}" status --porcelain
                        --untracked-files=no
                RESULT_VARIABLE tgcli_status_result
                OUTPUT_VARIABLE tgcli_status
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(tgcli_status_result EQUAL 0 AND NOT tgcli_status STREQUAL "")
                set(TGCLI_BUILD_COMMIT "${tgcli_sha}-dirty")
            else()
                set(TGCLI_BUILD_COMMIT "${tgcli_sha}")
            endif()
        endif()
    endif()
endif()

configure_file("${TGCLI_TEMPLATE}" "${TGCLI_OUTPUT}.candidate" @ONLY)
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${TGCLI_OUTPUT}.candidate"
                        "${TGCLI_OUTPUT}")
file(REMOVE "${TGCLI_OUTPUT}.candidate")
