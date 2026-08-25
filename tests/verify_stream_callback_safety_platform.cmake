if(NOT DEFINED REPO_ROOT)
    message(FATAL_ERROR "REPO_ROOT is required")
endif()

set(interposer "${REPO_ROOT}/tests/support/stream_callback_safety_apple_interpose.cpp")
if(NOT EXISTS "${interposer}")
    message(FATAL_ERROR "Apple callback-safety interposer is missing")
endif()

file(READ "${REPO_ROOT}/tests/CMakeLists.txt" cmake_source)
file(READ "${interposer}" interposer_source)
file(READ "${REPO_ROOT}/tests/stream_callback_safety_test.cpp" test_source)

foreach(fragment
        "if(APPLE)"
        "tgcli_stream_callback_safety_apple_interpose"
        "DYLD_INSERT_LIBRARIES=$<TARGET_FILE:tgcli_stream_callback_safety_apple_interpose>")
    string(FIND "${cmake_source}" "${fragment}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Apple callback-safety CMake wiring is missing: ${fragment}")
    endif()
endforeach()

foreach(fragment
        "__DATA,__interpose"
        "tgcli_stream_callback_safety_install_apple_recorder"
        "tgcli_interpose_malloc"
        "tgcli_interpose_calloc"
        "tgcli_interpose_realloc"
        "tgcli_interpose_free"
        "tgcli_interpose_aligned_alloc"
        "pthread_mutex_lock"
        "pthread_mutex_trylock"
        "pthread_mutex_unlock"
        "pthread_cond_wait"
        "pthread_cond_signal"
        "pthread_cond_broadcast"
        "pthread_cond_timedwait"
        "posix_memalign"
        "openat"
        "pread"
        "pwrite"
        "fsync"
        "poll"
        "select"
        "send"
        "recv")
    string(FIND "${interposer_source}" "${fragment}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Apple callback-safety interposer surface is missing: ${fragment}")
    endif()
endforeach()

foreach(fragment
        "defined(__linux__) || defined(__APPLE__)"
        "InstrumentationClass::CppAllocation"
        "InstrumentationClass::CAllocation"
        "InstrumentationClass::Synchronization"
        "InstrumentationClass::Io"
        "InstrumentationClass::TdSubmission"
        "InstrumentationClass::Teardown"
        "stream_callback_active()"
        "using AppleRecorder = void (*)(std::size_t) noexcept"
        "static_assert(std::is_same_v<decltype(&record_apple_instrumentation), AppleRecorder>)"
        "tgcli_stream_callback_safety_install_apple_recorder(&record_apple_instrumentation)")
    string(FIND "${test_source}" "${fragment}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Apple callback-safety positive proof is missing: ${fragment}")
    endif()
endforeach()
