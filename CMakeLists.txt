cmake_minimum_required(VERSION 3.20)
project(sensord LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# ---- options ---------------------------------------------------------------
# SENSORD_SANITIZE: "", "address,undefined" or "thread"
set(SENSORD_SANITIZE "" CACHE STRING "Sanitizers to build with")
option(SENSORD_BUILD_TESTS "Build unit/stress/e2e tests" ON)
option(SENSORD_BUILD_DASHBOARD "Build the Qt 6 dashboard (needs Qt6 Widgets)" ON)
option(SENSORD_BUILD_FUZZ "Build the libFuzzer target (needs clang with libFuzzer)" OFF)
option(SENSORD_WERROR "Treat warnings as errors" OFF)

set(SENSORD_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
if(SENSORD_WERROR)
  list(APPEND SENSORD_WARNINGS -Werror)
endif()

if(SENSORD_SANITIZE)
  message(STATUS "sensord: building with -fsanitize=${SENSORD_SANITIZE}")
  add_compile_options(-fsanitize=${SENSORD_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${SENSORD_SANITIZE})
endif()

# Keep frame pointers in normal builds too, so perf call graphs are readable.
add_compile_options(-fno-omit-frame-pointer)

find_package(Threads REQUIRED)

# std::jthread / std::stop_token: fine on GCC 11+, Clang + libstdc++, and
# Apple clang 17+ (Xcode 16.3+). Older libc++ hides them behind
# -fexperimental-library, so turn that on only if needed.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-std=c++20")
check_cxx_source_compiles("
  #include <thread>
  #include <stop_token>
  int main() { std::jthread t([](std::stop_token) {}); return 0; }" SENSORD_HAVE_JTHREAD)
if(NOT SENSORD_HAVE_JTHREAD)
  set(CMAKE_REQUIRED_FLAGS "-std=c++20 -fexperimental-library")
  check_cxx_source_compiles("
    #include <thread>
    #include <stop_token>
    int main() { std::jthread t([](std::stop_token) {}); return 0; }" SENSORD_HAVE_JTHREAD_EXPERIMENTAL)
  if(SENSORD_HAVE_JTHREAD_EXPERIMENTAL)
    add_compile_options(-fexperimental-library)
    add_link_options(-fexperimental-library)
  else()
    message(FATAL_ERROR "This compiler's standard library has no std::jthread. "
                        "Update Xcode / Command Line Tools, or use Homebrew LLVM.")
  endif()
endif()
unset(CMAKE_REQUIRED_FLAGS)

# ---- core library: protocol + devices + pipeline (no gRPC) ------------------
add_library(sensord_core
  src/protocol/crc16.cpp
  src/protocol/frame.cpp
  src/protocol/messages.cpp
  src/devices/signal_generator.cpp
  src/devices/simulated_device.cpp
  src/devices/serial_device.cpp
  src/devices/tcp_device.cpp
  src/devices/fd_util.cpp
  src/pipeline/device_reader.cpp
  src/pipeline/hub.cpp
  src/pipeline/telemetry_core.cpp
)
target_include_directories(sensord_core PUBLIC src)
target_compile_options(sensord_core PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord_core PUBLIC Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(sensord_core PUBLIC util)   # openpty
endif()

# ---- gRPC / protobuf ----------------------------------------------------------
find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()
find_package(gRPC CONFIG REQUIRED)

set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/proto/telemetry.proto)
set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})
set(GEN_SRCS
  ${GEN_DIR}/telemetry.pb.cc ${GEN_DIR}/telemetry.pb.h
  ${GEN_DIR}/telemetry.grpc.pb.cc ${GEN_DIR}/telemetry.grpc.pb.h)

if(TARGET protobuf::protoc)
  set(PROTOC_EXE $<TARGET_FILE:protobuf::protoc>)
else()
  set(PROTOC_EXE ${Protobuf_PROTOC_EXECUTABLE})
endif()

add_custom_command(
  OUTPUT ${GEN_SRCS}
  COMMAND ${PROTOC_EXE}
    --proto_path=${CMAKE_CURRENT_SOURCE_DIR}/proto
    --cpp_out=${GEN_DIR}
    --grpc_out=${GEN_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
    ${PROTO_SRC}
  DEPENDS ${PROTO_SRC}
  COMMENT "Generating C++ from telemetry.proto")

add_library(sensord_proto ${GEN_SRCS})
target_include_directories(sensord_proto PUBLIC ${GEN_DIR})
target_link_libraries(sensord_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

add_library(sensord_server
  src/server/telemetry_service.cpp
  src/server/convert.cpp
)
target_compile_options(sensord_server PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord_server PUBLIC sensord_core sensord_proto)

# ---- executables --------------------------------------------------------------
add_executable(sensord apps/sensord_main.cpp apps/device_spec.cpp)
target_compile_options(sensord PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord PRIVATE sensord_server)

add_executable(sensord-sim apps/sim_main.cpp)
target_compile_options(sensord-sim PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord-sim PRIVATE sensord_core)

add_executable(sensorctl cli/sensorctl.cpp)
target_compile_options(sensorctl PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensorctl PRIVATE sensord_proto)

add_executable(sensord-bench bench/loadgen.cpp)
target_compile_options(sensord-bench PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord-bench PRIVATE sensord_server)

add_executable(sensord-crc-bench bench/crc_bench.cpp)
target_compile_options(sensord-crc-bench PRIVATE ${SENSORD_WARNINGS})
target_link_libraries(sensord-crc-bench PRIVATE sensord_core)

# ---- Qt dashboard -------------------------------------------------------------
if(SENSORD_BUILD_DASHBOARD)
  find_package(Qt6 COMPONENTS Widgets QUIET)
  if(Qt6_FOUND)
    add_subdirectory(dashboard)
  else()
    message(WARNING "Qt6 Widgets not found, skipping dashboard (brew install qt)")
  endif()
endif()

# ---- tests --------------------------------------------------------------------
if(SENSORD_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- fuzzing ------------------------------------------------------------------
if(SENSORD_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
