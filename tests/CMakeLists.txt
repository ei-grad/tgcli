include(Catch)

add_executable(tgcli_unit_tests
    frame_test.cpp)
target_link_libraries(tgcli_unit_tests
    PRIVATE tgcli_proto Catch2::Catch2WithMain)

catch_discover_tests(tgcli_unit_tests)
