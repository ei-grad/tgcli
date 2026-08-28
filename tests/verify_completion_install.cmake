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

set(shells bash zsh fish)
set(source_files
    "${SOURCE_ROOT}/completions/tgcli.bash"
    "${SOURCE_ROOT}/completions/_tgcli"
    "${SOURCE_ROOT}/completions/tgcli.fish")
set(installed_files
    "${prefix}/share/bash-completion/completions/tgcli"
    "${prefix}/share/zsh/site-functions/_tgcli"
    "${prefix}/share/fish/vendor_completions.d/tgcli.fish")

foreach(index RANGE 0 2)
    list(GET shells ${index} shell)
    list(GET source_files ${index} source_file)
    list(GET installed_files ${index} installed_file)
    if(NOT EXISTS "${installed_file}" OR IS_SYMLINK "${installed_file}")
        message(FATAL_ERROR "installed completion is missing or unsafe: ${shell}")
    endif()
    file(SHA256 "${source_file}" source_sha)
    file(SHA256 "${installed_file}" installed_sha)
    if(NOT source_sha STREQUAL installed_sha)
        message(FATAL_ERROR "installed completion differs: ${shell}")
    endif()
    set(runtime_file "${prefix}/runtime-${shell}")
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
endforeach()

if(NOT EXISTS "${prefix}/share/man/man1/tgcli.1" OR
   IS_SYMLINK "${prefix}/share/man/man1/tgcli.1")
    message(FATAL_ERROR "installed tgcli(1) page is missing or unsafe")
endif()
