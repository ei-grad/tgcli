cmake_minimum_required(VERSION 3.24)

# Pins the observable contract of cmake/git_version.cmake: which build reports
# a revision, and in which exact spelling.

foreach(required_variable REPO_ROOT TEST_OUTPUT_DIR TEST_GENERATOR)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

find_program(git_executable NAMES git REQUIRED)

file(REMOVE_RECURSE "${TEST_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")

function(run_git)
    execute_process(COMMAND "${git_executable}" -C "${checkout}" ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${output}")
    endif()
endfunction()

function(run_git_in directory)
    execute_process(COMMAND "${git_executable}" -C "${directory}" ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "git -C ${directory} ${ARGN} failed: ${output}")
    endif()
endfunction()

function(read_head_revision directory output_variable)
    execute_process(
        COMMAND "${git_executable}" -C "${directory}" rev-parse --verify --short=7 "HEAD^{commit}"
        RESULT_VARIABLE status
        OUTPUT_VARIABLE revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE error)
    if(NOT status EQUAL 0 OR NOT revision MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "cannot resolve fixture HEAD in ${directory}: ${error}")
    endif()
    string(LENGTH "${revision}" revision_length)
    if(revision_length LESS 7)
        message(FATAL_ERROR "fixture HEAD abbreviation is too short: ${revision}")
    endif()
    set(${output_variable} "${revision}" PARENT_SCOPE)
endfunction()

# Reports the revision the resolver embeds for a source tree, through the same
# entry point the build uses.
function(resolve_revision source_directory output_variable)
    set(header "${TEST_OUTPUT_DIR}/${output_variable}.hpp")
    set(resolver_command
        "${CMAKE_COMMAND}"
        "-DTGCLI_SOURCE_DIR=${source_directory}"
        "-DTGCLI_TEMPLATE=${REPO_ROOT}/cmake/version.hpp.in"
        "-DTGCLI_OUTPUT=${header}"
        "-DTGCLI_VERSION=9.9.9")
    if(ARGC GREATER 2)
        list(APPEND resolver_command "-DTGCLI_GIT_EXECUTABLE=${ARGV2}")
    endif()
    list(APPEND resolver_command -P "${REPO_ROOT}/cmake/git_version.cmake")
    execute_process(
        COMMAND ${resolver_command}
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

function(expect_resolver_failure source_directory output_file label)
    set(resolver_command
        "${CMAKE_COMMAND}"
        "-DTGCLI_SOURCE_DIR=${source_directory}"
        "-DTGCLI_TEMPLATE=${REPO_ROOT}/cmake/version.hpp.in"
        "-DTGCLI_OUTPUT=${output_file}"
        "-DTGCLI_VERSION=9.9.9")
    if(ARGC GREATER 3)
        list(APPEND resolver_command "-DTGCLI_COPY_COMMAND=${ARGV3}")
    endif()
    list(APPEND resolver_command -P "${REPO_ROOT}/cmake/git_version.cmake")
    execute_process(
        COMMAND ${resolver_command}
        RESULT_VARIABLE status
        OUTPUT_QUIET
        ERROR_QUIET)
    if(status EQUAL 0)
        message(FATAL_ERROR "${label}: resolver unexpectedly succeeded")
    endif()
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

read_head_revision("${checkout}" head_revision)

resolve_revision("${checkout}" untagged_clean)
expect_revision("${untagged_clean}" "${head_revision}" "untagged clean checkout")

file(WRITE "${checkout}/tracked.txt" "modified\n")
resolve_revision("${checkout}" untagged_dirty)
expect_revision("${untagged_dirty}" "${head_revision}-dirty" "untagged dirty checkout")

run_git(reset --quiet --hard HEAD)
file(WRITE "${checkout}/tracked.txt" "staged\n")
run_git(add tracked.txt)
resolve_revision("${checkout}" staged_dirty)
expect_revision("${staged_dirty}" "${head_revision}-dirty" "staged tracked modification")

run_git(reset --quiet --hard HEAD)
file(REMOVE "${checkout}/tracked.txt")
resolve_revision("${checkout}" deleted_dirty)
expect_revision("${deleted_dirty}" "${head_revision}-dirty" "tracked deletion")

run_git(reset --quiet --hard HEAD)
file(WRITE "${checkout}/untracked.txt" "ignored by the resolver\n")
resolve_revision("${checkout}" untracked_only)
expect_revision("${untracked_only}" "${head_revision}" "untracked file present")

file(REMOVE "${checkout}/untracked.txt")
run_git(tag v9.9.9)
resolve_revision("${checkout}" tagged)
expect_revision("${tagged}" "${head_revision}" "exact tag")

file(WRITE "${checkout}/tracked.txt" "modified after tagging\n")
resolve_revision("${checkout}" tagged_dirty)
expect_revision("${tagged_dirty}" "${head_revision}-dirty" "dirty tree at an exact tag")

run_git(reset --quiet --hard HEAD)
run_git(commit --quiet --allow-empty -m "past the tag")
read_head_revision("${checkout}" after_tag_revision)
resolve_revision("${checkout}" after_tag)
expect_revision("${after_tag}" "${after_tag_revision}" "commit after the tag")

set(linked_checkout "${TEST_OUTPUT_DIR}/linked-checkout")
run_git(worktree add --quiet --detach "${linked_checkout}" HEAD)
read_head_revision("${linked_checkout}" linked_revision)
resolve_revision("${linked_checkout}" linked_clean)
expect_revision("${linked_clean}" "${linked_revision}" "clean linked worktree")
file(WRITE "${linked_checkout}/tracked.txt" "linked modification\n")
resolve_revision("${linked_checkout}" linked_dirty)
expect_revision("${linked_dirty}" "${linked_revision}-dirty" "dirty linked worktree")
run_git_in("${linked_checkout}" reset --quiet --hard HEAD)

set(symlink_checkout "${TEST_OUTPUT_DIR}/symlink-checkout")
file(CREATE_LINK "${linked_checkout}" "${symlink_checkout}" SYMBOLIC RESULT link_result)
if(NOT link_result STREQUAL "0")
    message(FATAL_ERROR "cannot create canonical source-root symlink: ${link_result}")
endif()
resolve_revision("${symlink_checkout}" symlink_clean)
expect_revision("${symlink_clean}" "${linked_revision}" "symlink-spelled checkout root")

set(shallow_checkout "${TEST_OUTPUT_DIR}/shallow-checkout")
execute_process(
    COMMAND "${git_executable}" -c protocol.file.allow=always clone --quiet --depth=1 --no-tags
            "file://${checkout}" "${shallow_checkout}"
    RESULT_VARIABLE shallow_status
    OUTPUT_VARIABLE shallow_output
    ERROR_VARIABLE shallow_output)
if(NOT shallow_status EQUAL 0)
    message(FATAL_ERROR "cannot create shallow tagless checkout: ${shallow_output}")
endif()
read_head_revision("${shallow_checkout}" shallow_revision)
resolve_revision("${shallow_checkout}" shallow_clean)
expect_revision("${shallow_clean}" "${shallow_revision}" "shallow tagless checkout")
execute_process(
    COMMAND "${git_executable}" -C "${shallow_checkout}" rev-list --count HEAD
    OUTPUT_VARIABLE shallow_count
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE shallow_count_status)
execute_process(
    COMMAND "${git_executable}" -C "${shallow_checkout}" tag --list
    OUTPUT_VARIABLE shallow_tags
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE shallow_tag_status)
if(NOT shallow_count_status EQUAL 0 OR NOT shallow_tag_status EQUAL 0 OR
   NOT shallow_count STREQUAL "1" OR NOT shallow_tags STREQUAL "")
    message(FATAL_ERROR "shallow release fixture retained history or tag refs")
endif()

set(nested_source "${checkout}/nested-source")
file(MAKE_DIRECTORY "${nested_source}")
resolve_revision("${nested_source}" nested_checkout)
expect_revision("${nested_checkout}" "" "source nested inside unrelated checkout")

set(non_checkout "${TEST_OUTPUT_DIR}/plain")
file(MAKE_DIRECTORY "${non_checkout}")
file(WRITE "${non_checkout}/tracked.txt" "no git here\n")
resolve_revision("${non_checkout}" outside_git)
expect_revision("${outside_git}" "" "directory outside a checkout")

set(unborn_checkout "${TEST_OUTPUT_DIR}/unborn")
file(MAKE_DIRECTORY "${unborn_checkout}")
run_git_in("${unborn_checkout}" init --quiet --initial-branch=main)
resolve_revision("${unborn_checkout}" unborn_head)
expect_revision("${unborn_head}" "" "unborn HEAD")

resolve_revision("${checkout}" missing_git "${TEST_OUTPUT_DIR}/missing-git")
expect_revision("${missing_git}" "" "missing git executable")

set(status_git "${TEST_OUTPUT_DIR}/status-failing-git")
file(WRITE "${status_git}"
    "#!/bin/sh\nif [ \"$3\" = status ]; then exit 23; fi\nexec \"${git_executable}\" \"$@\"\n")
file(CHMOD "${status_git}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
resolve_revision("${checkout}" failed_status "${status_git}")
expect_revision("${failed_status}" "" "failed tracked status inspection")

function(write_synthetic_git output sha)
    file(WRITE "${output}" "#!/bin/sh\n\
if [ \"$3\" = rev-parse ] && [ \"$4\" = --show-toplevel ]; then\n\
    printf '%s\\n' \"${checkout}\"\n\
elif [ \"$3\" = rev-parse ]; then\n\
    printf '%s\\n' \"${sha}\"\n\
elif [ \"$3\" = status ]; then\n\
    exit 0\n\
else\n\
    exit 1\n\
fi\n")
    file(CHMOD "${output}"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endfunction()

set(short_git "${TEST_OUTPUT_DIR}/short-git")
write_synthetic_git("${short_git}" "abcdef")
resolve_revision("${checkout}" short_sha "${short_git}")
expect_revision("${short_sha}" "" "six-hex revision")

set(lengthened_git "${TEST_OUTPUT_DIR}/lengthened-git")
write_synthetic_git("${lengthened_git}" "abcdef012345")
resolve_revision("${checkout}" lengthened_sha "${lengthened_git}")
expect_revision("${lengthened_sha}" "abcdef012345" "lengthened revision")

set(uppercase_git "${TEST_OUTPUT_DIR}/uppercase-git")
write_synthetic_git("${uppercase_git}" "ABCDEF0")
resolve_revision("${checkout}" uppercase_sha "${uppercase_git}")
expect_revision("${uppercase_sha}" "" "uppercase revision")

resolve_revision("${checkout}" stable_header)
expect_revision("${stable_header}" "${after_tag_revision}" "stable header baseline")
set(stable_header_path "${TEST_OUTPUT_DIR}/stable_header.hpp")
file(TIMESTAMP "${stable_header_path}" stable_timestamp_before "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
resolve_revision("${checkout}" stable_header)
file(TIMESTAMP "${stable_header_path}" stable_timestamp_after "%s")
if(NOT stable_timestamp_before STREQUAL stable_timestamp_after)
    message(FATAL_ERROR "unchanged revision rewrote the generated header")
endif()

set(failing_copy "${TEST_OUTPUT_DIR}/failing-copy")
file(WRITE "${failing_copy}" "#!/bin/sh\nexit 29\n")
file(CHMOD "${failing_copy}"
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
set(broken_output "${TEST_OUTPUT_DIR}/broken-output.hpp")
expect_resolver_failure("${checkout}" "${broken_output}" "generated header replacement failure"
    "${failing_copy}")
if(EXISTS "${broken_output}.candidate" OR IS_SYMLINK "${broken_output}.candidate")
    message(FATAL_ERROR "failed header replacement left its candidate behind")
endif()

set(rebuild_source "${TEST_OUTPUT_DIR}/rebuild-source")
set(rebuild_build "${TEST_OUTPUT_DIR}/rebuild-build")
file(MAKE_DIRECTORY "${rebuild_source}")
run_git_in("${rebuild_source}" init --quiet --initial-branch=main)
run_git_in("${rebuild_source}" config user.email tgcli@example.invalid)
run_git_in("${rebuild_source}" config user.name "tgcli test")
set(REPO_ROOT_FOR_MINI "${REPO_ROOT}")
set(mini_cmake [=[
cmake_minimum_required(VERSION 3.24)
project(version_rebuild LANGUAGES CXX)
set(version_header "${CMAKE_CURRENT_BINARY_DIR}/include/tgcli/version.hpp")
set(version_command
    "${CMAKE_COMMAND}"
    "-DTGCLI_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTGCLI_TEMPLATE=@REPO_ROOT_FOR_MINI@/cmake/version.hpp.in"
    "-DTGCLI_OUTPUT=${version_header}"
    "-DTGCLI_VERSION=9.9.9"
    -P "@REPO_ROOT_FOR_MINI@/cmake/git_version.cmake")
execute_process(COMMAND ${version_command} RESULT_VARIABLE version_status)
if(NOT version_status EQUAL 0)
    message(FATAL_ERROR "initial version header generation failed")
endif()
add_custom_target(version_header_target ALL
    COMMAND ${version_command}
    BYPRODUCTS "${version_header}"
    VERBATIM)
add_executable(probe main.cpp)
target_include_directories(probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/include")
add_dependencies(probe version_header_target)
]=])
string(CONFIGURE "${mini_cmake}" mini_cmake @ONLY)
file(WRITE "${rebuild_source}/CMakeLists.txt" "${mini_cmake}")
file(WRITE "${rebuild_source}/main.cpp"
    "#include <iostream>\n#include <tgcli/version.hpp>\nint main() { std::cout << tgcli::kBuildCommit; }\n")
file(WRITE "${rebuild_source}/tracked.txt" "clean\n")
run_git_in("${rebuild_source}" add CMakeLists.txt main.cpp tracked.txt)
run_git_in("${rebuild_source}" commit --quiet -m "initial")
read_head_revision("${rebuild_source}" rebuild_revision)

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${rebuild_source}" -B "${rebuild_build}"
            -G "${TEST_GENERATOR}"
    RESULT_VARIABLE rebuild_configure_status
    OUTPUT_VARIABLE rebuild_output
    ERROR_VARIABLE rebuild_output)
if(NOT rebuild_configure_status EQUAL 0)
    message(FATAL_ERROR "rebuild fixture configure failed: ${rebuild_output}")
endif()

function(build_and_read_probe output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${rebuild_build}" --target probe
        RESULT_VARIABLE build_status
        OUTPUT_VARIABLE build_output
        ERROR_VARIABLE build_output)
    if(NOT build_status EQUAL 0)
        message(FATAL_ERROR "rebuild fixture build failed: ${build_output}")
    endif()
    execute_process(
        COMMAND "${rebuild_build}/probe"
        RESULT_VARIABLE probe_status
        OUTPUT_VARIABLE probe_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_VARIABLE probe_error)
    if(NOT probe_status EQUAL 0)
        message(FATAL_ERROR "rebuild fixture probe failed: ${probe_error}")
    endif()
    set(${output_variable} "${probe_output}" PARENT_SCOPE)
endfunction()

build_and_read_probe(rebuild_clean)
expect_revision("${rebuild_clean}" "${rebuild_revision}" "initial dependent build")
set(rebuild_header "${rebuild_build}/include/tgcli/version.hpp")
file(TIMESTAMP "${rebuild_header}" rebuild_timestamp_before "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
build_and_read_probe(rebuild_unchanged)
file(TIMESTAMP "${rebuild_header}" rebuild_timestamp_after "%s")
expect_revision("${rebuild_unchanged}" "${rebuild_revision}" "unchanged dependent build")
if(NOT rebuild_timestamp_before STREQUAL rebuild_timestamp_after)
    message(FATAL_ERROR "unchanged build rewrote the dependent header")
endif()

file(WRITE "${rebuild_source}/tracked.txt" "dirty\n")
build_and_read_probe(rebuild_dirty)
expect_revision("${rebuild_dirty}" "${rebuild_revision}-dirty" "dirty dependent rebuild")

run_git_in("${rebuild_source}" add tracked.txt)
run_git_in("${rebuild_source}" commit --quiet -m "advance")
read_head_revision("${rebuild_source}" rebuild_advanced_revision)
build_and_read_probe(rebuild_advanced)
expect_revision("${rebuild_advanced}" "${rebuild_advanced_revision}" "committed dependent rebuild")

message(STATUS "build revision resolution contract holds")
