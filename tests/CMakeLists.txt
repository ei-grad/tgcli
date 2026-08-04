include(Catch)

add_executable(tgcli_unit_tests
    account_cli_test.cpp
    account_commands_test.cpp
    auth_bootstrap_test.cpp
    login_commands_test.cpp
    config_test.cpp
    daemon_lock_test.cpp
    dependency_lock_schema_test.cpp
    frame_test.cpp
    paths_test.cpp
    query_registry_test.cpp
    update_bus_test.cpp
    dispatch_test.cpp
    request_session_test.cpp
    routing_test.cpp
    server_test.cpp
    prompt_test.cpp
    render_test.cpp
    schema_matcher.cpp
    schema_test.cpp
    secret_hook_test.cpp
    cli_test.cpp
    td_client_test.cpp
    td_runtime_conversion_test.cpp
    td_runtime_test.cpp
    support/scripted_td_runtime.cpp)
target_link_libraries(tgcli_unit_tests
    PRIVATE tgcli_proto tgcli_util tgcli_daemon tgcli_cli tgcli_core Td::TdStatic
        Catch2::Catch2WithMain jsoncons)
target_include_directories(tgcli_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_dependencies(tgcli_unit_tests tgcli)
target_compile_definitions(tgcli_unit_tests
    PRIVATE
        TGCLI_RELEASE_DIR="${CMAKE_SOURCE_DIR}/release"
        TGCLI_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/docs/schemas"
        TGCLI_GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden"
        TGCLI_TEST_BINARY="$<TARGET_FILE:tgcli>")

# Catch2 tags become ctest labels so the TSan preset can exclude the [tdlib]
# tests (tdlib is uninstrumented; see CLAUDE.md sanitizer policy).
catch_discover_tests(tgcli_unit_tests ADD_TAGS_AS_LABELS)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME dependency-lock-offline
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/verify_dependency_lock.py"
        --repo-root "${CMAKE_SOURCE_DIR}")
add_test(
    NAME dependency-lock-enforcement
    COMMAND "${CMAKE_COMMAND}"
        -DREPO_ROOT=${CMAKE_SOURCE_DIR}
        -DTEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-lock-enforcement
        -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/dependency_lock_enforcement_test.cmake")
