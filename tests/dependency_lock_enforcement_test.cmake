cmake_minimum_required(VERSION 3.24)

foreach(required_variable REPO_ROOT TEST_OUTPUT_DIR PYTHON_EXECUTABLE)
    if(NOT DEFINED ${required_variable})
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

set(probe "${REPO_ROOT}/tests/dependency_lock_failure_probe.cmake")
set(verifier "${REPO_ROOT}/scripts/verify_dependency_lock.py")
set(canonical_lock "${REPO_ROOT}/release/dependencies.lock.json")
set(canonical_contract "${REPO_ROOT}/release/linux-musl-toolchain.json")
set(canonical_recipe "${REPO_ROOT}/scripts/release/build-linux-musl.sh")
file(MAKE_DIRECTORY "${TEST_OUTPUT_DIR}")
file(READ "${canonical_lock}" lock_json)

function(lock_field component_id field output_variable)
    string(JSON component_count LENGTH "${lock_json}" components)
    math(EXPR last_component "${component_count} - 1")
    foreach(component_index RANGE 0 ${last_component})
        string(JSON candidate_id GET "${lock_json}" components ${component_index} id)
        if(candidate_id STREQUAL component_id)
            string(JSON value GET
                "${lock_json}" components ${component_index} ${field})
            set(${output_variable} "${value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Missing lock component: ${component_id}")
endfunction()

function(expect_failure name expected_pattern)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR "${name} unexpectedly succeeded")
    endif()
    string(CONCAT combined_output "${output}" "\n" "${error}")
    if(NOT combined_output MATCHES "${expected_pattern}")
        message(FATAL_ERROR
            "${name} failed without expected diagnostic ${expected_pattern}:\n"
            "${combined_output}")
    endif()
endfunction()

function(expect_success name)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name} failed:\n${output}\n${error}")
    endif()
endfunction()

lock_field(cli11 immutable_ref cli11_ref)
lock_field(cli11 source_repository cli11_repository)
lock_field(cli11 archive_sha256 cli11_archive_sha256)
lock_field(cli11 source_tree_sha256 cli11_source_tree_sha256)
lock_field(tdlib immutable_ref tdlib_ref)
lock_field(tdlib source_repository tdlib_repository)

string(REPLACE "${cli11_ref}" "0000000000000000000000000000000000000000"
    mismatched_lock_json "${lock_json}")
set(mismatched_lock "${TEST_OUTPUT_DIR}/mismatched-lock.json")
file(WRITE "${mismatched_lock}" "${mismatched_lock_json}")
expect_failure(
    "CMake lock mismatch" "differs"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=lock
    -DLOCK_FILE=${mismatched_lock}
    -DEXPECTED_REF=${cli11_ref}
    -P "${probe}")

set(duplicate_script "${TEST_OUTPUT_DIR}/duplicate-tdlib-pin.sh")
file(WRITE "${duplicate_script}"
    "# TDLIB_REV=${tdlib_ref}\n"
    "TDLIB_REV=${tdlib_ref}\n"
    "TDLIB_REV=0000000000000000000000000000000000000000\n"
    "TDLIB_REPOSITORY=${tdlib_repository}\n")
expect_failure(
    "CMake duplicate TDLib assignment" "exactly one active TDLIB_REV assignment"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=script
    -DEXPECTED_REF=${tdlib_ref}
    -DBUILD_SCRIPT_FILE=${duplicate_script}
    -P "${probe}")
expect_failure(
    "Python duplicate TDLib assignment" "exactly one active TDLib pin assignment"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --build-script-file "${duplicate_script}")

expect_failure(
    "resolved checkout mismatch" "differs"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=resolved
    -DSOURCE_DIRECTORY=${REPO_ROOT}
    -DEXPECTED_REF=${cli11_ref}
    -P "${probe}")

set(missing_archive_provenance "${TEST_OUTPUT_DIR}/missing-archive-provenance")
file(MAKE_DIRECTORY "${missing_archive_provenance}")
expect_failure(
    "archive mode is release-only" "no Git identity outside locked release archive mode"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=no_archive
    -DSOURCE_DIRECTORY=${missing_archive_provenance}
    -DEXPECTED_REF=${cli11_ref}
    -P "${probe}")

set(mismatched_archive_provenance "${TEST_OUTPUT_DIR}/mismatched-archive-provenance")
file(MAKE_DIRECTORY "${mismatched_archive_provenance}")
file(WRITE "${mismatched_archive_provenance}/arbitrary.cpp" "int arbitrary;\n")
file(WRITE "${mismatched_archive_provenance}/.tgcli-source.json"
    "{\n"
    "  \"archive_sha256\": \"${cli11_archive_sha256}\",\n"
    "  \"immutable_ref\": \"0000000000000000000000000000000000000000\",\n"
    "  \"schema_version\": 1,\n"
    "  \"source_repository\": \"${cli11_repository}\"\n"
    "}\n")
expect_failure(
    "arbitrary tree with copied sidecar" "archive tree differs from the source lock"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=archive
    -DSOURCE_DIRECTORY=${mismatched_archive_provenance}
    -DEXPECTED_REF=${cli11_ref}
    -P "${probe}")

set(valid_archive_provenance "${TEST_OUTPUT_DIR}/valid-archive-provenance")
file(MAKE_DIRECTORY "${valid_archive_provenance}")
file(WRITE "${valid_archive_provenance}/locked.cpp" "int locked;\n")
execute_process(
    COMMAND "${PYTHON_EXECUTABLE}"
        "${REPO_ROOT}/scripts/release/archive_tool.py"
        tree-sha256 --root "${valid_archive_provenance}"
    RESULT_VARIABLE tree_hash_result
    OUTPUT_VARIABLE valid_tree_sha256
    ERROR_VARIABLE tree_hash_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT tree_hash_result EQUAL 0)
    message(FATAL_ERROR "Cannot hash test source tree: ${tree_hash_error}")
endif()
string(REPLACE
    "\"source_tree_sha256\": \"${cli11_source_tree_sha256}\""
    "\"source_tree_sha256\": \"${valid_tree_sha256}\""
    valid_tree_lock_json "${lock_json}")
set(valid_tree_lock "${TEST_OUTPUT_DIR}/valid-tree-lock.json")
file(WRITE "${valid_tree_lock}" "${valid_tree_lock_json}")
expect_success(
    "matching locked archive tree"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=archive
    -DLOCK_FILE=${valid_tree_lock}
    -DSOURCE_DIRECTORY=${valid_archive_provenance}
    -DEXPECTED_REF=${cli11_ref}
    -P "${probe}")

set(missing_prefix "${TEST_OUTPUT_DIR}/missing-prefix")
file(MAKE_DIRECTORY "${missing_prefix}/lib/cmake/Td")
expect_failure(
    "missing prefix provenance" "provenance is missing"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=prefix
    -DTD_PACKAGE_DIRECTORY=${missing_prefix}/lib/cmake/Td
    -DEXPECTED_REF=${tdlib_ref}
    -P "${probe}")

set(mismatched_prefix "${TEST_OUTPUT_DIR}/mismatched-prefix")
file(MAKE_DIRECTORY
    "${mismatched_prefix}/lib/cmake/Td"
    "${mismatched_prefix}/share/tgcli")
file(WRITE "${mismatched_prefix}/share/tgcli/tdlib-source.json"
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"source_repository\": \"${tdlib_repository}\",\n"
    "  \"immutable_ref\": \"0000000000000000000000000000000000000000\"\n"
    "}\n")
expect_failure(
    "mismatched prefix provenance" "differs from the source lock"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=prefix
    -DTD_PACKAGE_DIRECTORY=${mismatched_prefix}/lib/cmake/Td
    -DEXPECTED_REF=${tdlib_ref}
    -P "${probe}")

set(valid_prefix "${TEST_OUTPUT_DIR}/valid-prefix")
file(MAKE_DIRECTORY
    "${valid_prefix}/lib/cmake/Td"
    "${valid_prefix}/share/tgcli")
file(WRITE "${valid_prefix}/share/tgcli/tdlib-source.json"
    "{\n"
    "  \"schema_version\": 1,\n"
    "  \"source_repository\": \"${tdlib_repository}\",\n"
    "  \"immutable_ref\": \"${tdlib_ref}\"\n"
    "}\n")
expect_success(
    "matching prefix provenance"
    "${CMAKE_COMMAND}"
    -DREPO_ROOT=${REPO_ROOT}
    -DMODE=prefix
    -DTD_PACKAGE_DIRECTORY=${valid_prefix}/lib/cmake/Td
    -DEXPECTED_REF=${tdlib_ref}
    -P "${probe}")

string(REPLACE "\"schema_version\": 2" "\"schema_version\": true"
    boolean_version_json "${lock_json}")
set(boolean_version_lock "${TEST_OUTPUT_DIR}/boolean-version-lock.json")
file(WRITE "${boolean_version_lock}" "${boolean_version_json}")
expect_failure(
    "boolean schema version" "unsupported dependency lock version"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --lock-file "${boolean_version_lock}")

string(REPLACE
    "da5c23ecdb9a55c82d6802ee55812dfb99a035a4838287c0b7c0051bd0fdb9fc"
    "0000000000000000000000000000000000000000000000000000000000000000"
    drifted_re2_archive_json "${lock_json}")
set(drifted_re2_archive_lock "${TEST_OUTPUT_DIR}/drifted-re2-archive-lock.json")
file(WRITE "${drifted_re2_archive_lock}" "${drifted_re2_archive_json}")
expect_failure(
    "RE2 archive evidence drift" "archive evidence differs"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --lock-file "${drifted_re2_archive_lock}")

file(READ "${REPO_ROOT}/CMakeLists.txt" cmake_text)
file(READ "${REPO_ROOT}/cmake/Re2Dependency.cmake" re2_cmake_text)
set(ignored_icu_cmake "${TEST_OUTPUT_DIR}/ignored-icu.cmake")
file(WRITE "${ignored_icu_cmake}" "${cmake_text}\nset(RE2_USE_ICU OFF)\n")
expect_failure(
    "nonexistent RE2 ICU option" "must not use the nonexistent RE2_USE_ICU option"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --cmake-file "${ignored_icu_cmake}")

string(REPLACE
    "set(\${option} OFF CACHE BOOL \"Private pinned RE2 option\" FORCE)"
    "set(\${option} ON CACHE BOOL \"leak\" FORCE)"
    enabled_pcre_text "${re2_cmake_text}")
set(enabled_pcre_cmake "${TEST_OUTPUT_DIR}/enabled-pcre-helper.cmake")
file(WRITE "${enabled_pcre_cmake}" "${enabled_pcre_text}")
expect_failure(
    "enabled RE2 PCRE support" "must use a private cache transaction"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --re2-cmake-file "${enabled_pcre_cmake}")

set(extra_cmake "${TEST_OUTPUT_DIR}/extra-fetchcontent.cmake")
file(WRITE "${extra_cmake}" "${cmake_text}\n"
    "fetchcontent_declare(unlocked\n"
    "    GIT_REPOSITORY https://example.invalid/unlocked\n"
    "    GIT_TAG 0000000000000000000000000000000000000000)\n")
expect_failure(
    "lowercase extra FetchContent declaration" "FetchContent inventory differs"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --cmake-file "${extra_cmake}")

set(mixed_case_extra_cmake "${TEST_OUTPUT_DIR}/mixed-case-extra-fetchcontent.cmake")
file(WRITE "${mixed_case_extra_cmake}" "${cmake_text}\n"
    "FeTcHcOnTeNt_DeClArE(another_unlocked\n"
    "    GIT_REPOSITORY https://example.invalid/another-unlocked\n"
    "    GIT_TAG 0000000000000000000000000000000000000000)\n")
expect_failure(
    "mixed-case extra FetchContent declaration" "FetchContent inventory differs"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --cmake-file "${mixed_case_extra_cmake}")

file(READ "${REPO_ROOT}/THIRD_PARTY_NOTICES.md" notices_text)
string(REPLACE "<!-- lock-id:cli11 -->" "" drifted_notices_text "${notices_text}")
set(drifted_notices "${TEST_OUTPUT_DIR}/drifted-notices.md")
file(WRITE "${drifted_notices}" "${drifted_notices_text}")
expect_failure(
    "runtime notice drift" "runtime notice marker missing"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --notices-file "${drifted_notices}")

string(REPLACE
    "ef01c255778d37a1e5968f865318c49ce8ae391530583d6721acd548ded77557"
    "0000000000000000000000000000000000000000000000000000000000000000"
    drifted_license_json "${lock_json}")
set(drifted_license_lock "${TEST_OUTPUT_DIR}/drifted-license-lock.json")
file(WRITE "${drifted_license_lock}" "${drifted_license_json}")
expect_failure(
    "runtime license drift" "checksum mismatch"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --lock-file "${drifted_license_lock}")

file(READ "${canonical_contract}" contract_json)
string(REPLACE
    "\"dependency_lock_sha256\": \""
    "\"dependency_lock_sha256\": \"0000000000000000000000000000000000000000000000000000000000000000-"
    drifted_contract_json "${contract_json}")
set(drifted_contract "${TEST_OUTPUT_DIR}/drifted-linux-musl-toolchain.json")
file(WRITE "${drifted_contract}" "${drifted_contract_json}")
expect_failure(
    "dependency lock contract drift" "dependency lock digest differs"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --contract-file "${drifted_contract}")

set(drifted_recipe_directory "${TEST_OUTPUT_DIR}/drifted-recipe")
file(MAKE_DIRECTORY "${drifted_recipe_directory}")
file(READ "${canonical_recipe}" recipe_text)
set(drifted_recipe "${drifted_recipe_directory}/build-linux-musl.sh")
file(WRITE "${drifted_recipe}" "${recipe_text}\n")
expect_failure(
    "Linux release recipe drift" "recipe digest differs"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --release-build-script-file "${drifted_recipe}")

set(missing_archive_directory "${TEST_OUTPUT_DIR}/missing-archives")
file(MAKE_DIRECTORY "${missing_archive_directory}")
expect_failure(
    "missing staged archive" "missing .*cli11.tar.gz"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --archive-directory "${missing_archive_directory}"
    --component cli11)

set(mismatched_archive_directory "${TEST_OUTPUT_DIR}/mismatched-archives")
file(MAKE_DIRECTORY "${mismatched_archive_directory}")
file(WRITE "${mismatched_archive_directory}/cli11.tar.gz" "not the locked archive\n")
expect_failure(
    "mismatched staged archive" "archive size mismatch"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --archive-directory "${mismatched_archive_directory}"
    --component cli11)

expect_failure(
    "component selection without archive mode" "archive selection requires"
    "${PYTHON_EXECUTABLE}" "${verifier}"
    --repo-root "${REPO_ROOT}"
    --component cli11)
