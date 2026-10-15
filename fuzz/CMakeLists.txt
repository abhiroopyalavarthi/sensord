# libFuzzer needs clang. On macOS use Homebrew LLVM (Apple's clang ships
# without libFuzzer):
#   cmake -B build-fuzz -DSENSORD_BUILD_FUZZ=ON \
#     -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "SENSORD_BUILD_FUZZ needs clang (libFuzzer)")
endif()

add_executable(fuzz_frame fuzz_frame.cpp
  ../src/protocol/crc16.cpp
  ../src/protocol/frame.cpp
  ../src/protocol/messages.cpp)
target_include_directories(fuzz_frame PRIVATE ../src)
target_compile_options(fuzz_frame PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
target_link_options(fuzz_frame PRIVATE -fsanitize=fuzzer,address,undefined)

# Generates seed inputs (a few valid frames, split points, garbage).
add_executable(make_seeds make_seeds.cpp)
target_link_libraries(make_seeds PRIVATE sensord_core)
