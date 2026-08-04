if(NOT DEFINED TGCLI_DEPENDENCY_LOCK_FILE)
    set(TGCLI_DEPENDENCY_LOCK_FILE
        "${CMAKE_CURRENT_SOURCE_DIR}/release/dependencies.lock.json")
endif()
if(NOT EXISTS "${TGCLI_DEPENDENCY_LOCK_FILE}")
    message(FATAL_ERROR "Dependency lock is missing: ${TGCLI_DEPENDENCY_LOCK_FILE}")
endif()
file(READ "${TGCLI_DEPENDENCY_LOCK_FILE}" TGCLI_DEPENDENCY_LOCK_JSON)

function(tgcli_dependency_lock_field dependency field output_variable)
    string(JSON component_count LENGTH "${TGCLI_DEPENDENCY_LOCK_JSON}" components)
    math(EXPR last_component "${component_count} - 1")
    foreach(component_index RANGE 0 ${last_component})
        string(JSON component_id GET
            "${TGCLI_DEPENDENCY_LOCK_JSON}" components ${component_index} id)
        if(component_id STREQUAL dependency)
            string(JSON field_value GET
                "${TGCLI_DEPENDENCY_LOCK_JSON}"
                components ${component_index} ${field})
            set(${output_variable} "${field_value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Dependency ${dependency} is absent from the source lock")
endfunction()

function(tgcli_dependency_lock_ref dependency output_variable)
    tgcli_dependency_lock_field("${dependency}" immutable_ref immutable_ref)
    set(${output_variable} "${immutable_ref}" PARENT_SCOPE)
endfunction()

function(tgcli_assert_dependency_lock dependency expected_ref)
    tgcli_dependency_lock_ref("${dependency}" locked_ref)
    if(NOT locked_ref STREQUAL expected_ref)
        message(FATAL_ERROR
            "Dependency ${dependency} pin ${expected_ref} differs from lock ${locked_ref}")
    endif()
endfunction()

function(tgcli_assert_resolved_git_revision dependency source_directory expected_ref)
    find_package(Git REQUIRED QUIET)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_directory}" rev-parse HEAD
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE resolved_ref
        ERROR_VARIABLE git_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT git_result EQUAL 0)
        message(FATAL_ERROR
            "Cannot resolve fetched ${dependency} revision: ${git_error}")
    endif()
    if(NOT resolved_ref STREQUAL expected_ref)
        message(FATAL_ERROR
            "Fetched ${dependency} revision ${resolved_ref} differs from lock ${expected_ref}")
    endif()
endfunction()

function(tgcli_assert_tdlib_build_script expected_ref)
    if(ARGC GREATER 1)
        set(build_script "${ARGV1}")
    else()
        set(build_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-tdlib.sh")
    endif()
    file(READ "${build_script}" build_script_text)
    string(REGEX MATCHALL
        "(^|\n)[ \t]*TDLIB_REV=[^\n]*"
        tdlib_assignments
        "${build_script_text}")
    list(LENGTH tdlib_assignments assignment_count)
    if(NOT assignment_count EQUAL 1)
        message(FATAL_ERROR
            "${build_script} must contain exactly one active TDLIB_REV assignment")
    endif()
    list(GET tdlib_assignments 0 tdlib_assignment)
    string(REGEX MATCH
        "TDLIB_REV=([0-9a-f]+)([ \t]+#[^\n]*)?$"
        tdlib_pin_match
        "${tdlib_assignment}")
    set(script_ref "${CMAKE_MATCH_1}")
    string(LENGTH "${script_ref}" script_ref_length)
    if(NOT tdlib_pin_match OR NOT script_ref_length EQUAL 40
       OR NOT script_ref STREQUAL expected_ref)
        message(FATAL_ERROR
            "${build_script} TDLib pin differs from ${expected_ref}")
    endif()
endfunction()

function(tgcli_assert_tdlib_prefix_provenance td_package_directory expected_ref)
    get_filename_component(prefix_root
        "${td_package_directory}/../../.." REALPATH)
    set(provenance_file "${prefix_root}/share/tgcli/tdlib-source.json")
    if(NOT EXISTS "${provenance_file}" OR IS_SYMLINK "${provenance_file}")
        message(FATAL_ERROR
            "TDLib prefix provenance is missing: ${provenance_file}; "
            "rebuild it with scripts/build-tdlib.sh")
    endif()
    file(READ "${provenance_file}" provenance_json)
    string(JSON provenance_size LENGTH "${provenance_json}")
    string(JSON provenance_schema_type TYPE "${provenance_json}" schema_version)
    string(JSON provenance_schema GET "${provenance_json}" schema_version)
    string(JSON provenance_repository GET "${provenance_json}" source_repository)
    string(JSON provenance_ref GET "${provenance_json}" immutable_ref)
    tgcli_dependency_lock_field(tdlib source_repository locked_repository)
    if(NOT provenance_size EQUAL 3 OR NOT provenance_schema_type STREQUAL NUMBER
       OR NOT provenance_schema EQUAL 1
       OR NOT provenance_repository STREQUAL locked_repository
       OR NOT provenance_ref STREQUAL expected_ref)
        message(FATAL_ERROR
            "TDLib prefix provenance ${provenance_file} differs from the source lock")
    endif()
endfunction()
