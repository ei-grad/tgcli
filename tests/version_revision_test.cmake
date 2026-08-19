cmake_minimum_required(VERSION 3.24)

# Pins the observable contract of cmake/git_version.cmake: which build reports
# a revision, and in which exact spelling.

foreach(required_variable REPO_ROOT TEST_OUTPUT_DIR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

find_program(git_executable NAMES git REQUIRED)

file(REMOVE_RECURSE "${TEST_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

function(run_git)
    execute_process(COMMAND "${git_executable}" ${ARGN}
        WORKING_DIRECTORY "${checkout}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${output}")
    endif()
endfunction()

# Reports the revision the resolver embeds for a source tree, through the same
# entry point the build uses.
function(resolve_revision source_directory output_variable)
    set(header "${TEST_OUTPUT_DIR}/version.hpp")
    file(REMOVE "${header}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
                -DTGCLI_SOURCE_DIR=${source_directory}
                -DTGCLI_TEMPLATE=${REPO_ROOT}/cmake/version.hpp.in
                -DTGCLI_OUTPUT=${header}
                -DTGCLI_VERSION=9.9.9
                -P "${REPO_ROOT}/cmake/git_version.cmake"
        RESULT_VARIABLE status
        OUTPUT_QUIET
        ERROR_VARIABLE resolver_error)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "revision resolver failed: ${resolver_error}")
    endif()
    file(READ "${header}" contents)
    if(NOT contents MATCHES "kVersion = \"9\\.9\\.9\"")
        message(FATAL_ERROR "generated header lost the project version:\n${contents}")
    endif()
    if(NOT contents MATCHES "kBuildCommit = \"([^\"]*)\"")
        message(FATAL_ERROR "generated header has no build revision:\n${contents}")
    endif()
    set(${output_variable} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(expect_revision actual expected label)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${label}: expected \"${expected}\", got \"${actual}\"")
    endif()
endfunction()

set(checkout "${TEST_OUTPUT_DIR}/checkout")
file(MAKE_DIRECTORY "${checkout}")
run_git(init --quiet --initial-branch=main)
run_git(config user.email tgcli@example.invalid)
run_git(config user.name "tgcli test")
file(WRITE "${checkout}/tracked.txt" "committed\n")
run_git(add tracked.txt)
run_git(commit --quiet -m "initial")

execute_process(COMMAND "${git_executable}" rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${checkout}"
    OUTPUT_VARIABLE head_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE)

resolve_revision("${checkout}" untagged_clean)
expect_revision("${untagged_clean}" "${head_revision}" "untagged clean checkout")

file(WRITE "${checkout}/tracked.txt" "modified\n")
resolve_revision("${checkout}" untagged_dirty)
expect_revision("${untagged_dirty}" "${head_revision}-dirty" "untagged dirty checkout")

run_git(checkout --quiet -- tracked.txt)
file(WRITE "${checkout}/untracked.txt" "ignored by the resolver\n")
resolve_revision("${checkout}" untracked_only)
expect_revision("${untracked_only}" "${head_revision}" "untracked file present")

file(REMOVE "${checkout}/untracked.txt")
run_git(tag v9.9.9)
resolve_revision("${checkout}" tagged)
expect_revision("${tagged}" "" "exact tag")

file(WRITE "${checkout}/tracked.txt" "modified after tagging\n")
resolve_revision("${checkout}" tagged_dirty)
expect_revision("${tagged_dirty}" "" "dirty tree at an exact tag")

run_git(checkout --quiet -- tracked.txt)
run_git(commit --quiet --allow-empty -m "past the tag")
execute_process(COMMAND "${git_executable}" rev-parse --short=7 HEAD
    WORKING_DIRECTORY "${checkout}"
    OUTPUT_VARIABLE after_tag_revision
    OUTPUT_STRIP_TRAILING_WHITESPACE)
resolve_revision("${checkout}" after_tag)
expect_revision("${after_tag}" "${after_tag_revision}" "commit after the tag")

set(non_checkout "${TEST_OUTPUT_DIR}/plain")
file(MAKE_DIRECTORY "${non_checkout}")
file(WRITE "${non_checkout}/tracked.txt" "no git here\n")
resolve_revision("${non_checkout}" outside_git)
expect_revision("${outside_git}" "" "directory outside a checkout")

message(STATUS "build revision resolution contract holds")
