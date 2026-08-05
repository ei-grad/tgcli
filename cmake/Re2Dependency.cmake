include_guard(GLOBAL)

function(tgcli_make_re2_available)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0077 NEW)

    # These normal variables shadow caller cache entries only while RE2 is
    # configured.  CMP0077 prevents upstream option() calls from rewriting the
    # cache, so both normal and cache state are unchanged when the function
    # returns.
    set(BUILD_SHARED_LIBS OFF)
    set(RE2_BUILD_TESTING OFF)
    set(USEPCRE OFF)
    FetchContent_MakeAvailable(re2)

    cmake_policy(POP)
    set(re2_SOURCE_DIR "${re2_SOURCE_DIR}" PARENT_SCOPE)
    set(re2_BINARY_DIR "${re2_BINARY_DIR}" PARENT_SCOPE)
endfunction()
