if(NOT DEFINED BUILD_ROOT OR NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "completion install roots are required")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(prefix "${BUILD_ROOT}/completion-install-${suffix}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${BUILD_ROOT}" --prefix "${prefix}"
    RESULT_VARIABLE install_status
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error)
if(NOT install_status EQUAL 0)
    message(FATAL_ERROR "completion install failed: ${install_output}${install_error}")
endif()

set(manifest_file "${SOURCE_ROOT}/docs/release/command-assets.json")
if(NOT EXISTS "${manifest_file}" OR IS_SYMLINK "${manifest_file}")
    message(FATAL_ERROR "release command asset manifest is missing or unsafe")
endif()
file(READ "${manifest_file}" manifest)
string(JSON schema_version GET "${manifest}" schema_version)
string(JSON asset_count LENGTH "${manifest}" assets)
if(NOT schema_version EQUAL 1 OR NOT asset_count EQUAL 6)
    message(FATAL_ERROR "release command asset manifest root differs")
endif()

math(EXPR last_asset "${asset_count} - 1")
foreach(index RANGE 0 ${last_asset})
    string(JSON source_name GET "${manifest}" assets ${index} source)
    string(JSON package_name GET "${manifest}" assets ${index} package)
    string(JSON shell_type TYPE "${manifest}" assets ${index} shell)
    set(source_file "${SOURCE_ROOT}/${source_name}")
    set(installed_file "${prefix}/${package_name}")
    if(NOT EXISTS "${installed_file}" OR IS_SYMLINK "${installed_file}")
        message(FATAL_ERROR "installed command asset is missing or unsafe: ${package_name}")
    endif()
    file(SHA256 "${source_file}" source_sha)
    file(SHA256 "${installed_file}" installed_sha)
    if(NOT source_sha STREQUAL installed_sha)
        message(FATAL_ERROR "installed command asset differs: ${package_name}")
    endif()
    if(shell_type STREQUAL "STRING")
        string(JSON shell GET "${manifest}" assets ${index} shell)
        set(runtime_file "${BUILD_ROOT}/completion-runtime-${suffix}-${shell}")
        execute_process(
            COMMAND "${prefix}/bin/tgcli" completion "${shell}"
            RESULT_VARIABLE runtime_status
            OUTPUT_FILE "${runtime_file}"
            ERROR_VARIABLE runtime_error)
        if(NOT runtime_status EQUAL 0 OR NOT runtime_error STREQUAL "")
            message(FATAL_ERROR "installed completion runtime failed: ${shell}: ${runtime_error}")
        endif()
        file(SHA256 "${runtime_file}" runtime_sha)
        if(NOT source_sha STREQUAL runtime_sha)
            message(FATAL_ERROR "installed runtime completion differs: ${shell}")
        endif()
    endif()
endforeach()
