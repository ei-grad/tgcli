include(Catch)

add_executable(tgcli_unit_tests
    config_test.cpp
    daemon_lock_test.cpp
    frame_test.cpp
    paths_test.cpp
    query_registry_test.cpp
    update_bus_test.cpp
    dispatch_test.cpp
    server_test.cpp
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
        TGCLI_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/docs/schemas"
        TGCLI_TEST_BINARY="$<TARGET_FILE:tgcli>")

# Catch2 tags become ctest labels so the TSan preset can exclude the [tdlib]
# tests (tdlib is uninstrumented; see CLAUDE.md sanitizer policy).
catch_discover_tests(tgcli_unit_tests ADD_TAGS_AS_LABELS)
