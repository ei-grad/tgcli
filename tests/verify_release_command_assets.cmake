if(NOT DEFINED BINARY OR NOT DEFINED BUILD_ROOT OR NOT DEFINED SOURCE_ROOT)
    message(FATAL_ERROR "release command asset test inputs are required")
endif()

set(manifest_file "${SOURCE_ROOT}/docs/release/command-assets.json")
file(READ "${manifest_file}" manifest)
string(JSON asset_count LENGTH "${manifest}" assets)
if(NOT asset_count EQUAL 5)
    message(FATAL_ERROR "release command asset count differs")
endif()

foreach(platform linux-x86_64-musl macos-universal)
    string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
    set(test_root "${BUILD_ROOT}/release-command-assets-${platform}-${suffix}")
    set(package_name "tgcli-test-${platform}")
    set(package_root "${test_root}/stage/${package_name}")
    file(MAKE_DIRECTORY "${package_root}")
    file(COPY_FILE "${BINARY}" "${package_root}/tgcli")
    file(CHMOD "${package_root}/tgcli"
        PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE
                    WORLD_READ WORLD_EXECUTE)

    math(EXPR last_asset "${asset_count} - 1")
    foreach(index RANGE 0 ${last_asset})
        string(JSON source_name GET "${manifest}" assets ${index} source)
        string(JSON asset_package_name GET "${manifest}" assets ${index} package)
        get_filename_component(package_directory "${package_root}/${asset_package_name}" DIRECTORY)
        file(MAKE_DIRECTORY "${package_directory}")
        file(COPY_FILE
            "${SOURCE_ROOT}/${source_name}"
            "${package_root}/${asset_package_name}")
    endforeach()

    set(archive "${test_root}/${package_name}.tar.gz")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar czf "${archive}" --format=gnutar "${package_name}"
        WORKING_DIRECTORY "${test_root}/stage"
        RESULT_VARIABLE archive_status
        OUTPUT_VARIABLE archive_output
        ERROR_VARIABLE archive_error)
    if(NOT archive_status EQUAL 0)
        message(FATAL_ERROR "${platform} command asset archive failed: ${archive_output}${archive_error}")
    endif()

    set(extracted "${test_root}/extracted")
    file(MAKE_DIRECTORY "${extracted}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
        WORKING_DIRECTORY "${extracted}"
        RESULT_VARIABLE extract_status
        OUTPUT_VARIABLE extract_output
        ERROR_VARIABLE extract_error)
    if(NOT extract_status EQUAL 0)
        message(FATAL_ERROR "${platform} command asset extraction failed: ${extract_output}${extract_error}")
    endif()

    execute_process(
        COMMAND bash "${SOURCE_ROOT}/scripts/check-release-artifact.sh"
            verify-command-assets-package
            "${extracted}/${package_name}"
            "${SOURCE_ROOT}"
            "${extracted}/${package_name}/tgcli"
            "${manifest_file}"
        RESULT_VARIABLE verify_status
        OUTPUT_VARIABLE verify_output
        ERROR_VARIABLE verify_error)
    if(NOT verify_status EQUAL 0)
        message(FATAL_ERROR "${platform} command asset verification failed: ${verify_output}${verify_error}")
    endif()

    if(platform STREQUAL "linux-x86_64-musl")
        file(APPEND
            "${extracted}/${package_name}/share/tgcli/public-command-registry.json"
            "\n")
        execute_process(
            COMMAND bash "${SOURCE_ROOT}/scripts/check-release-artifact.sh"
                verify-command-assets-package
                "${extracted}/${package_name}"
                "${SOURCE_ROOT}"
                "${extracted}/${package_name}/tgcli"
                "${manifest_file}"
            RESULT_VARIABLE mismatch_status
            OUTPUT_VARIABLE mismatch_output
            ERROR_VARIABLE mismatch_error)
        if(mismatch_status EQUAL 0 OR
           NOT "${mismatch_output}${mismatch_error}" MATCHES
               "packaged command asset differs: share/tgcli/public-command-registry.json")
            message(FATAL_ERROR "release command asset mismatch did not fail closed")
        endif()
    endif()
endforeach()
