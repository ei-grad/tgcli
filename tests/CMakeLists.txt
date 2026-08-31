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
    m6_foundation_test.cpp
    m6_model_test.cpp
    m6_schema_test.cpp
    m6_audit_contract_test.cpp
    m6_commands_test.cpp
    m6_capability_test.cpp
    m6_topic_scan_test.cpp
    m6_write_policy_test.cpp
    m6_td_boundary_test.cpp
    m6_td_runtime_conversion_test.cpp
    m7_foundation_schema_test.cpp
    m2_read_domain_test.cpp
    m2_read_commands_test.cpp
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
    download_td_runtime_conversion_test.cpp
    download_domain_test.cpp
    download_commands_test.cpp
    download_filesystem_test.cpp
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
    raw_audit_contract_test.cpp
    raw_audit_log_test.cpp
    raw_commands_test.cpp
    raw_contract_test.cpp
    raw_native_contract_test.cpp
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
    stream_commands_test.cpp
    stream_coordinator_test.cpp
    stream_ingress_test.cpp
    stream_service_test.cpp
    stream_subscription_test.cpp
    stream_wait_scanner_test.cpp
    stream_metadata_test.cpp
    stream_model_test.cpp
    stream_storage_test.cpp
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

add_executable(tgcli_stream_callback_safety_tests
    stream_callback_safety_test.cpp
    support/scripted_td_runtime.cpp)
target_link_libraries(tgcli_stream_callback_safety_tests
    PRIVATE tgcli_daemon tgcli_core Catch2::Catch2WithMain)
target_include_directories(tgcli_stream_callback_safety_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(tgcli_stream_callback_safety_tests
        PRIVATE "-Wno-mismatched-new-delete")
endif()
if(APPLE)
    add_library(tgcli_stream_callback_safety_apple_interpose SHARED
        support/stream_callback_safety_apple_interpose.cpp)
    target_compile_features(tgcli_stream_callback_safety_apple_interpose PRIVATE cxx_std_20)
    target_link_libraries(tgcli_stream_callback_safety_tests
        PRIVATE tgcli_stream_callback_safety_apple_interpose)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(tgcli_stream_callback_safety_tests PRIVATE
        "-Wl,--wrap=pthread_mutex_lock"
        "-Wl,--wrap=pthread_mutex_trylock"
        "-Wl,--wrap=pthread_mutex_unlock"
        "-Wl,--wrap=pthread_cond_wait"
        "-Wl,--wrap=pthread_cond_timedwait"
        "-Wl,--wrap=pthread_cond_signal"
        "-Wl,--wrap=pthread_cond_broadcast"
        "-Wl,--wrap=malloc"
        "-Wl,--wrap=calloc"
        "-Wl,--wrap=realloc"
        "-Wl,--wrap=free"
        "-Wl,--wrap=aligned_alloc"
        "-Wl,--wrap=posix_memalign"
        "-Wl,--wrap=open"
        "-Wl,--wrap=openat"
        "-Wl,--wrap=read"
        "-Wl,--wrap=pread"
        "-Wl,--wrap=write"
        "-Wl,--wrap=pwrite"
        "-Wl,--wrap=close"
        "-Wl,--wrap=fsync"
        "-Wl,--wrap=fdatasync"
        "-Wl,--wrap=poll"
        "-Wl,--wrap=select"
        "-Wl,--wrap=epoll_wait"
        "-Wl,--wrap=send"
        "-Wl,--wrap=recv")
endif()
if(APPLE)
    catch_discover_tests(tgcli_stream_callback_safety_tests
        ADD_TAGS_AS_LABELS
        PROPERTIES ENVIRONMENT
            "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:tgcli_stream_callback_safety_apple_interpose>")
else()
    catch_discover_tests(tgcli_stream_callback_safety_tests ADD_TAGS_AS_LABELS)
endif()

add_test(
    NAME stream-callback-safety-platform-contract
    COMMAND "${CMAKE_COMMAND}"
        -DREPO_ROOT=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_stream_callback_safety_platform.cmake")

add_test(
    NAME schema-asset-generator
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}"
        -p "test_schema_asset_generator.py")
set_tests_properties(
    schema-asset-generator
    PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
add_test(
    NAME schema-m7-foundation-generator
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/test_m7_foundation_assets.py")
set_tests_properties(
    schema-m7-foundation-generator
    PROPERTIES ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;TGCLI_TEST_TDLIB_SOURCE=${TGCLI_TDLIB_SOURCE}")
add_test(
    NAME command-registry-completion-assets
    COMMAND node "${CMAKE_CURRENT_SOURCE_DIR}/verify_command_assets.mjs"
        "$<TARGET_FILE:tgcli>")
add_test(
    NAME completion-install-assets
    COMMAND "${CMAKE_COMMAND}"
        -DBUILD_ROOT=${CMAKE_BINARY_DIR}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_completion_install.cmake")
add_test(
    NAME release-command-assets
    COMMAND "${CMAKE_COMMAND}"
        -DBINARY=$<TARGET_FILE:tgcli>
        -DBUILD_ROOT=${CMAKE_BINARY_DIR}
        -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_release_command_assets.cmake")
add_test(
    NAME raw-policy-source-contract
    COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/test_raw_policy.py")
set_tests_properties(
    raw-policy-source-contract
    PROPERTIES ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;TGCLI_TEST_TDLIB_SOURCE=${TGCLI_TDLIB_SOURCE}")
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
    NAME tdlib-stream-source-contract
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/scripts/verify_tdlib_stream_source_contract.py"
        --repo-root "${CMAKE_SOURCE_DIR}"
        --tdlib-source "${TGCLI_TDLIB_SOURCE}")
add_test(
    NAME tdlib-stream-source-contract-negative
    COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}"
        -p "test_tdlib_stream_source_contract.py")
set_tests_properties(
    tdlib-stream-source-contract-negative
    PROPERTIES ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1;TGCLI_TEST_TDLIB_SOURCE=${TGCLI_TDLIB_SOURCE}")
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
