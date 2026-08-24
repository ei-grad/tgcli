include(Catch)

add_executable(tgcli_unit_tests
    account_audit_test.cpp
    account_removal_test.cpp
    account_removal_remote_test.cpp
    activity_tracker_test.cpp
    account_cli_test.cpp
    account_commands_test.cpp
    auth_bootstrap_test.cpp
    chat_summary_test.cpp
    chats_td_runtime_conversion_test.cpp
    message_commands_test.cpp
    message_recovery_test.cpp
    message_td_runtime_conversion_test.cpp
    m3_foundation_test.cpp
    login_commands_test.cpp
    lock_order_test.cpp
    logout_commands_test.cpp
    config_test.cpp
    config_runtime_test.cpp
    daemon_lock_test.cpp
    dependency_lock_schema_test.cpp
    destructive_contract_test.cpp
    direct_td_runtime_test.cpp
    direct_rpc_test.cpp
    send_td_runtime_test.cpp
    forward_test.cpp
    single_send_test.cpp
    destructive_plan_test.cpp
    file_spool_test.cpp
    fetch_domain_test.cpp
    fetch_commands_test.cpp
    removal_filesystem_test.cpp
    removal_journal_test.cpp
    removal_planner_test.cpp
    removal_recovery_test.cpp
    frame_test.cpp
    hash_canonical_test.cpp
    idempotency_store_test.cpp
    write_kernel_test.cpp
    write_commands_test.cpp
    paths_test.cpp
    query_registry_test.cpp
    update_bus_test.cpp
    dispatch_test.cpp
    request_session_test.cpp
    ready_read_test.cpp
    read_domain_test.cpp
    read_commands_test.cpp
    read_td_runtime_conversion_test.cpp
    request_fingerprint_test.cpp
    resolver_test.cpp
    resolver_consumer_test.cpp
    user_resolver_test.cpp
    chats_commands_test.cpp
    routing_test.cpp
    server_test.cpp
    saved_commands_test.cpp
    prompt_test.cpp
    render_test.cpp
    schema_matcher.cpp
    schema_catalog_test.cpp
    schema_cli_test.cpp
    schema_test.cpp
    session_schema_test.cpp
    stream_td_client_test.cpp
    stream_td_runtime_conversion_test.cpp
    stream_schema_test.cpp
    stream_metadata_test.cpp
    stream_model_test.cpp
    secret_hook_test.cpp
    cli_test.cpp
    td_client_test.cpp
    td_logging_test.cpp
    td_runtime_conversion_test.cpp
    td_runtime_test.cpp
    support/scripted_td_runtime.cpp)
target_link_libraries(tgcli_unit_tests
    PRIVATE tgcli_proto tgcli_util tgcli_daemon tgcli_cli tgcli_core tgcli_schema Td::TdStatic
        Catch2::Catch2WithMain jsoncons)
add_dependencies(tgcli_unit_tests tgcli_version_header)
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

add_test(
    NAME schema-asset-generator
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}"
        -p "test_schema_asset_generator.py")
set_tests_properties(
    schema-asset-generator
    PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(
    NAME dependency-lock-offline
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/verify_dependency_lock.py"
        --repo-root "${CMAKE_SOURCE_DIR}")
add_test(
    NAME tdlib-session-source-contract
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/verify_tdlib_session_source_contract.py"
        --repo-root "${CMAKE_SOURCE_DIR}")
add_test(
    NAME td-api-source-boundary
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/verify_td_api_source_boundary.py"
        --repo-root "${CMAKE_SOURCE_DIR}")
add_test(
    NAME dependency-lock-enforcement
    COMMAND "${CMAKE_COMMAND}"
        -DREPO_ROOT=${CMAKE_SOURCE_DIR}
        -DTEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-lock-enforcement
        -DPYTHON_EXECUTABLE=${Python3_EXECUTABLE}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/dependency_lock_enforcement_test.cmake")
add_test(
    NAME re2-cmake-scope
    COMMAND "${CMAKE_COMMAND}"
        -DREPO_ROOT=${CMAKE_SOURCE_DIR}
        -DTEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/re2-cmake-scope
        -P "${CMAKE_CURRENT_SOURCE_DIR}/re2_cmake_scope_test.cmake")
add_test(
    NAME version-revision
    COMMAND "${CMAKE_COMMAND}"
        -DREPO_ROOT=${CMAKE_SOURCE_DIR}
        -DTEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/version-revision
        "-DTEST_GENERATOR=${CMAKE_GENERATOR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/version_revision_test.cmake")
add_test(
    NAME release-toolchain-helpers
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/release_toolchain_helpers_test.py"
        "${CMAKE_SOURCE_DIR}")
set_tests_properties(
    release-toolchain-helpers
    PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(
    NAME test-dc-contract-tools
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}"
        -p "test_test_dc_tools.py")
