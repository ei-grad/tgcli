include(Catch)

add_executable(tgcli_unit_tests
    frame_test.cpp
    query_registry_test.cpp
    update_bus_test.cpp
    td_client_test.cpp)
target_link_libraries(tgcli_unit_tests
    PRIVATE tgcli_proto tgcli_core Catch2::Catch2WithMain)

# Catch2 tags become ctest labels so the TSan preset can exclude the [tdlib]
# tests (tdlib is uninstrumented; see CLAUDE.md sanitizer policy).
catch_discover_tests(tgcli_unit_tests ADD_TAGS_AS_LABELS)
