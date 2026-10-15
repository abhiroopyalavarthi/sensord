find_package(GTest QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(unit_tests
  test_crc.cpp
  test_frame.cpp
  test_messages.cpp
  test_queue.cpp
  test_devices.cpp
  test_pipeline.cpp
)
target_link_libraries(unit_tests PRIVATE sensord_core GTest::gtest_main)
gtest_discover_tests(unit_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)

# Concurrency stress test: 8 devices, 20 subscribers joining and leaving at
# random. Seed and duration come from SENSORD_STRESS_SEED / SENSORD_STRESS_SECONDS.
add_executable(stress_tests test_stress.cpp)
target_link_libraries(stress_tests PRIVATE sensord_core GTest::gtest_main)
foreach(seed 1 2 3)
  add_test(NAME stress_seed_${seed} COMMAND stress_tests)
  set_tests_properties(stress_seed_${seed} PROPERTIES
    ENVIRONMENT "SENSORD_STRESS_SEED=${seed};SENSORD_STRESS_SECONDS=3"
    TIMEOUT 120)
endforeach()

# End-to-end: real gRPC server + client in one process.
add_executable(e2e_tests test_e2e.cpp)
target_link_libraries(e2e_tests PRIVATE sensord_server GTest::gtest_main)
gtest_discover_tests(e2e_tests DISCOVERY_TIMEOUT 30 PROPERTIES TIMEOUT 120)

# Replays every saved fuzz input through the parser (regression corpus).
add_executable(fuzz_regression ../fuzz/fuzz_frame.cpp ../fuzz/regression_main.cpp)
target_link_libraries(fuzz_regression PRIVATE sensord_core)
add_test(NAME fuzz_regression COMMAND fuzz_regression ${CMAKE_SOURCE_DIR}/fuzz/corpus)
