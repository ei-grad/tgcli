if(NOT DEFINED TGCLI_LSAN_PROBE OR NOT DEFINED TGCLI_LSAN_SUPPRESSIONS)
    message(FATAL_ERROR "LSan control requires probe and suppression paths")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "LSAN_OPTIONS=suppressions=${TGCLI_LSAN_SUPPRESSIONS}:print_suppressions=0"
        "${TGCLI_LSAN_PROBE}"
    RESULT_VARIABLE probe_result
    OUTPUT_VARIABLE probe_stdout
    ERROR_VARIABLE probe_stderr)

if(probe_result EQUAL 0)
    message(FATAL_ERROR "tgcli-owned leak was incorrectly suppressed")
endif()
if(NOT probe_stderr MATCHES "LeakSanitizer: detected memory leaks")
    message(FATAL_ERROR "LSan did not diagnose the tgcli-owned leak: ${probe_stderr}")
endif()
if(NOT probe_stderr MATCHES "tgcli_owned_leak_control")
    message(FATAL_ERROR "LSan report did not retain the tgcli-owned allocation frame")
endif()
