cmake_minimum_required(VERSION 3.2)

project(ReactiveSocket)

if (NOT DEFINED CPACK_GENERATOR)
  set(CPACK_GENERATOR "RPM")
endif()
include(CPack)

# CMake modules.
set(CMAKE_MODULE_PATH
  "${CMAKE_SOURCE_DIR}/cmake/"
  # For in-fbsource builds
  "${CMAKE_CURRENT_SOURCE_DIR}/../opensource/fbcode_builder/CMake"
  # For shipit-transformed builds
  "${CMAKE_CURRENT_SOURCE_DIR}/build/fbcode_builder/CMake"
  ${CMAKE_MODULE_PATH}
)

# Joins arguments and stores the result in ${var}.
function(join var)
  unset(result)
  foreach (arg ${ARGN})
    if (DEFINED result)
      set(result "${result}${arg}")
    else ()
      set(result "${arg}")
    endif ()
  endforeach ()
  set(${var} "${result}" PARENT_SCOPE)
endfunction()

# Generate compilation database for use by YouCompleteMe.
set(CMAKE_EXPORT_COMPILE_COMMANDS 1)

# make sure to bail on in-source builds for cleanliness
if (${CMAKE_SOURCE_DIR} STREQUAL ${CMAKE_BINARY_DIR})
  message(FATAL_ERROR "In-source builds not allowed. Please make a new directory (called a build directory)"
  "and run CMake from there. You may need to remove CMakeCache.txt.")
endif()

# default built type is Debug
if (NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build" FORCE)
endif(NOT CMAKE_BUILD_TYPE)

string(TOLOWER "${CMAKE_BUILD_TYPE}" BUILD_TYPE_LOWER)

if (BUILD_TYPE_LOWER MATCHES "debug")
  add_definitions(-DDEBUG)
endif ()

# Enable ASAN by default on debug macOS builds.
if (APPLE)
  set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
  if ("${BUILD_TYPE_LOWER}" MATCHES "debug")
    if (${CMAKE_CXX_COMPILER_ID} MATCHES Clang)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,integer -fno-sanitize=unsigned-integer-overflow")
    elseif (${CMAKE_CXX_COMPILER_ID} MATCHES GNU)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
    else ()
        message(FATAL_ERROR "Unsupported compiler on macOS")
    endif()
  endif()
endif()

# disable coverage mode by default
option(RSOCKET_BUILD_WITH_COVERAGE "Build with --coverage (gcov)" OFF)

# Add compiler-specific options.
if (CMAKE_COMPILER_IS_GNUCXX)
  if (RSOCKET_ASAN)
    set(ASAN_FLAGS -fsanitize=address,undefined)
  endif ()
  set(EXTRA_LINK_FLAGS ${EXTRA_LINK_FLAGS} -fuse-ld=gold)

elseif (${CMAKE_CXX_COMPILER_ID} MATCHES Clang)
  if (RSOCKET_ASAN)
    set(ASAN_FLAGS
      -fsanitize=address,undefined,integer
      -fno-sanitize=unsigned-integer-overflow)
  endif ()
endif ()

# Enable code coverage, if the compiler is supported
if (RSOCKET_BUILD_WITH_COVERAGE)
  set(COVERAGE_INFO coverage.info)

  if (${CMAKE_SYSTEM_NAME} MATCHES Linux AND ${CMAKE_CXX_COMPILER_ID} MATCHES Clang)
    # clang and linux's lcov don't play nice together; don't attempt with linux/clang
    add_custom_command(
      OUTPUT ${COVERAGE_INFO}
      COMMAND echo "Coverage info omitted for clang/linux builds")

  else ()
    add_custom_command(
      OUTPUT ${COVERAGE_INFO}
      # Capture coverage info.
      COMMAND lcov --directory . --capture --output-file ${COVERAGE_INFO}
      # Filter out system and test code.
      COMMAND lcov --remove ${COVERAGE_INFO} '*/tests/*' '*/test/*' '*/tck-test/*' '*/usr/include/*' '/usr/*' '*/gmock/*' '*/folly/*' --output-file
                   ${COVERAGE_INFO}
      # Debug before upload.
      COMMAND lcov --list ${COVERAGE_INFO})
  endif ()

  if (CMAKE_COMPILER_IS_GNUCXX)
    add_compile_options(-g -O0 --coverage)
    set(EXTRA_LINK_FLAGS ${EXTRA_LINK_FLAGS} --coverage)

  elseif (${CMAKE_CXX_COMPILER_ID} MATCHES Clang)
    add_compile_options(-g -O0 -fprofile-arcs -ftest-coverage)
    set(EXTRA_LINK_FLAGS ${EXTRA_LINK_FLAGS} -fprofile-arcs -ftest-coverage)

  else ()
    message(FATAL_ERROR "Code coverage not supported with this compiler/host combination")
  endif ()

  message(STATUS "Building with coverage")
  add_custom_target(coverage DEPENDS ${COVERAGE_INFO})
endif ()

if (DEFINED ASAN_FLAGS)
  add_compile_options(${ASAN_FLAGS})
  set(EXTRA_LINK_FLAGS ${EXTRA_LINK_FLAGS} ${ASAN_FLAGS})
endif ()

option(BUILD_BENCHMARKS "Build benchmarks" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(BUILD_TESTS "Build tests" ON)

enable_testing()

include(ExternalProject)
include(CTest)

include(${CMAKE_SOURCE_DIR}/cmake/InstallFolly.cmake)

if(BUILD_TESTS)
  # gmock
  ExternalProject_Add(
    gmock
    URL ${CMAKE_CURRENT_SOURCE_DIR}/googletest-release-1.8.0.zip
    INSTALL_COMMAND ""
  )

  ExternalProject_Get_Property(gmock source_dir)
  set(GMOCK_SOURCE_DIR ${source_dir})
  ExternalProject_Get_Property(gmock binary_dir)
  set(GMOCK_BINARY_DIR ${binary_dir})

  set(GMOCK_LIBS
    ${GMOCK_BINARY_DIR}/${CMAKE_CFG_INTDIR}/googlemock/${CMAKE_STATIC_LIBRARY_PREFIX}gmock${CMAKE_STATIC_LIBRARY_SUFFIX}
    ${GMOCK_BINARY_DIR}/${CMAKE_CFG_INTDIR}/googlemock/${CMAKE_STATIC_LIBRARY_PREFIX}gmock_main${CMAKE_STATIC_LIBRARY_SUFFIX}
  )

  include_directories(${GMOCK_SOURCE_DIR}/googlemock/include)
  include_directories(${GMOCK_SOURCE_DIR}/googletest/include)

endif()

set(CMAKE_CXX_STANDARD 14)

include(CheckCXXCompilerFlag)

# Common configuration for all build modes.
if (NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-error=pessimizing-move")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Woverloaded-virtual")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
endif()

CHECK_CXX_COMPILER_FLAG(-Wnoexcept-type COMPILER_HAS_W_NOEXCEPT_TYPE)
if (COMPILER_HAS_W_NOEXCEPT_TYPE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-noexcept-type")
endif()

if("${BUILD_TYPE_LOWER}" MATCHES "debug")
  message("debug mode was set")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unreachable-code")
else()
  message("release mode was set")
endif()

if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
  set(TEST_CXX_FLAGS ${TEST_CXX_FLAGS} -Wno-inconsistent-missing-override)
endif()

find_library(DOUBLE-CONVERSION double-conversion)

find_package(OpenSSL REQUIRED)

find_package(Gflags REQUIRED)

# find glog::glog to satisfy the folly dep.
find_package(Glog REQUIRED)

find_package(fmt CONFIG REQUIRED)

include_directories(SYSTEM ${OPENSSL_INCLUDE_DIR})

include_directories(SYSTEM ${GFLAGS_INCLUDE_DIR})

add_subdirectory(yarpl)

add_library(
  ReactiveSocket
  rsocket/ColdResumeHandler.cpp
  rsocket/ColdResumeHandler.h
  rsocket/ConnectionAcceptor.h
  rsocket/ConnectionFactory.h
  rsocket/DuplexConnection.h
  rsocket/Payload.cpp
  rsocket/Payload.h
  rsocket/RSocket.cpp
  rsocket/RSocket.h
  rsocket/RSocketClient.cpp
  rsocket/RSocketClient.h
  rsocket/RSocketErrors.h
  rsocket/RSocketException.h
  rsocket/RSocketParameters.cpp
  rsocket/RSocketParameters.h
  rsocket/RSocketRequester.cpp
  rsocket/RSocketRequester.h
  rsocket/RSocketResponder.cpp
  rsocket/RSocketResponder.h
  rsocket/RSocketServer.cpp
  rsocket/RSocketServer.h
  rsocket/RSocketServerState.h
  rsocket/RSocketServiceHandler.cpp
  rsocket/RSocketServiceHandler.h
  rsocket/RSocketStats.cpp
  rsocket/RSocketStats.h
  rsocket/ResumeManager.h
  rsocket/framing/ErrorCode.cpp
  rsocket/framing/ErrorCode.h
  rsocket/framing/Frame.cpp
  rsocket/framing/Frame.h
  rsocket/framing/FrameFlags.cpp
  rsocket/framing/FrameFlags.h
  rsocket/framing/FrameHeader.cpp
  rsocket/framing/FrameHeader.h
  rsocket/framing/FrameProcessor.h
  rsocket/framing/FrameSerializer.cpp
  rsocket/framing/FrameSerializer.h
  rsocket/framing/FrameSerializer_v1_0.cpp
  rsocket/framing/FrameSerializer_v1_0.h
  rsocket/framing/FrameTransport.h
  rsocket/framing/FrameTransportImpl.cpp
  rsocket/framing/FrameTransportImpl.h
  rsocket/framing/FrameType.cpp
  rsocket/framing/FrameType.h
  rsocket/framing/FramedDuplexConnection.cpp
  rsocket/framing/FramedDuplexConnection.h
  rsocket/framing/FramedReader.cpp
  rsocket/framing/FramedReader.h
  rsocket/framing/ProtocolVersion.cpp
  rsocket/framing/ProtocolVersion.h
  rsocket/framing/ResumeIdentificationToken.cpp
  rsocket/framing/ResumeIdentificationToken.h
  rsocket/framing/ScheduledFrameProcessor.cpp
  rsocket/framing/ScheduledFrameProcessor.h
  rsocket/framing/ScheduledFrameTransport.cpp
  rsocket/framing/ScheduledFrameTransport.h
  rsocket/internal/ClientResumeStatusCallback.h
  rsocket/internal/Common.cpp
  rsocket/internal/Common.h
  rsocket/internal/ConnectionSet.cpp
  rsocket/internal/ConnectionSet.h
  rsocket/internal/KeepaliveTimer.cpp
  rsocket/internal/KeepaliveTimer.h
  rsocket/internal/ScheduledRSocketResponder.cpp
  rsocket/internal/ScheduledRSocketResponder.h
  rsocket/internal/ScheduledSingleObserver.h
  rsocket/internal/ScheduledSingleSubscription.cpp
  rsocket/internal/ScheduledSingleSubscription.h
  rsocket/internal/ScheduledSubscriber.h
  rsocket/internal/ScheduledSubscription.cpp
  rsocket/internal/ScheduledSubscription.h
  rsocket/internal/SetupResumeAcceptor.cpp
  rsocket/internal/SetupResumeAcceptor.h
  rsocket/internal/SwappableEventBase.cpp
  rsocket/internal/SwappableEventBase.h
  rsocket/internal/WarmResumeManager.cpp
  rsocket/internal/WarmResumeManager.h
  rsocket/statemachine/ChannelRequester.cpp
  rsocket/statemachine/ChannelRequester.h
  rsocket/statemachine/ChannelResponder.cpp
  rsocket/statemachine/ChannelResponder.h
  rsocket/statemachine/ConsumerBase.cpp
  rsocket/statemachine/ConsumerBase.h
  rsocket/statemachine/FireAndForgetResponder.cpp
  rsocket/statemachine/FireAndForgetResponder.h
  rsocket/statemachine/PublisherBase.cpp
  rsocket/statemachine/PublisherBase.h
  rsocket/statemachine/RSocketStateMachine.cpp
  rsocket/statemachine/RSocketStateMachine.h
  rsocket/statemachine/RequestResponseRequester.cpp
  rsocket/statemachine/RequestResponseRequester.h
  rsocket/statemachine/RequestResponseResponder.cpp
  rsocket/statemachine/RequestResponseResponder.h
  rsocket/statemachine/StreamRequester.cpp
  rsocket/statemachine/StreamRequester.h
  rsocket/statemachine/StreamResponder.cpp
  rsocket/statemachine/StreamResponder.h
  rsocket/statemachine/StreamStateMachineBase.cpp
  rsocket/statemachine/StreamStateMachineBase.h
  rsocket/statemachine/StreamFragmentAccumulator.cpp
  rsocket/statemachine/StreamFragmentAccumulator.h
  rsocket/statemachine/StreamsWriter.h
  rsocket/statemachine/StreamsWriter.cpp
  rsocket/transports/tcp/TcpConnectionAcceptor.cpp
  rsocket/transports/tcp/TcpConnectionAcceptor.h
  rsocket/transports/tcp/TcpConnectionFactory.cpp
  rsocket/transports/tcp/TcpConnectionFactory.h
  rsocket/transports/tcp/TcpDuplexConnection.cpp
  rsocket/transports/tcp/TcpDuplexConnection.h)

target_include_directories(
    ReactiveSocket
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)


target_link_libraries(ReactiveSocket
    PUBLIC yarpl glog::glog gflags
    INTERFACE ${EXTRA_LINK_FLAGS})

target_compile_options(
  ReactiveSocket
  PRIVATE ${EXTRA_CXX_FLAGS})

enable_testing()

install(TARGETS ReactiveSocket EXPORT rsocket-exports DESTINATION lib)
install(DIRECTORY rsocket DESTINATION include FILES_MATCHING PATTERN "*.h")
install(EXPORT rsocket-exports NAMESPACE rsocket:: DESTINATION lib/cmake/rsocket)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/rsocket-config.cmake.in
    rsocket-config.cmake
    INSTALL_DESTINATION lib/cmake/rsocket
)
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/rsocket-config.cmake
    DESTINATION lib/cmake/rsocket
)

if(BUILD_TESTS)
add_executable(
  tests
  rsocket/test/ColdResumptionTest.cpp
  rsocket/test/ConnectionEventsTest.cpp
  rsocket/test/PayloadTest.cpp
  rsocket/test/RSocketClientServerTest.cpp
  rsocket/test/RSocketClientTest.cpp
  rsocket/test/RSocketTests.cpp
  rsocket/test/RSocketTests.h
  rsocket/test/RequestChannelTest.cpp
  rsocket/test/RequestResponseTest.cpp
  rsocket/test/RequestStreamTest.cpp
  rsocket/test/RequestStreamTest_concurrency.cpp
  rsocket/test/Test.cpp
  rsocket/test/WarmResumeManagerTest.cpp
  rsocket/test/WarmResumptionTest.cpp
  rsocket/test/framing/FrameTest.cpp
  rsocket/test/framing/FrameTransportTest.cpp
  rsocket/test/framing/FramedReaderTest.cpp
  rsocket/test/handlers/HelloServiceHandler.cpp
  rsocket/test/handlers/HelloServiceHandler.h
  rsocket/test/handlers/HelloStreamRequestHandler.cpp
  rsocket/test/handlers/HelloStreamRequestHandler.h
  rsocket/test/internal/AllowanceTest.cpp
  rsocket/test/internal/ConnectionSetTest.cpp
  rsocket/test/internal/KeepaliveTimerTest.cpp
  rsocket/test/internal/ResumeIdentificationToken.cpp
  rsocket/test/internal/SetupResumeAcceptorTest.cpp
  rsocket/test/internal/SwappableEventBaseTest.cpp
  rsocket/test/statemachine/RSocketStateMachineTest.cpp
  rsocket/test/statemachine/StreamStateTest.cpp
  rsocket/test/statemachine/StreamsWriterTest.cpp
  rsocket/test/test_utils/ColdResumeManager.cpp
  rsocket/test/test_utils/ColdResumeManager.h
  rsocket/test/test_utils/GenericRequestResponseHandler.h
  rsocket/test/test_utils/MockDuplexConnection.h
  rsocket/test/test_utils/MockStreamsWriter.h
  rsocket/test/test_utils/MockStats.h
  rsocket/test/transport/DuplexConnectionTest.cpp
  rsocket/test/transport/DuplexConnectionTest.h
  rsocket/test/transport/TcpDuplexConnectionTest.cpp)

add_dependencies(tests gmock)
target_link_libraries(
  tests
  ReactiveSocket
  yarpl
  yarpl-test-utils
  ${GMOCK_LIBS}  # This also needs the preceding `add_dependencies`
  glog::glog
  gflags)

target_include_directories(tests PUBLIC "${PROJECT_SOURCE_DIR}/yarpl/test/")
target_compile_options(
  tests
  PRIVATE ${TEST_CXX_FLAGS})

add_dependencies(tests gmock yarpl-test-utils ReactiveSocket)

add_test(NAME RSocketTests COMMAND tests)

### Fuzzer harnesses
add_executable(
  frame_fuzzer
  rsocket/test/fuzzers/frame_fuzzer.cpp)

target_link_libraries(
  frame_fuzzer
  ReactiveSocket
  yarpl
  glog::glog
  gflags)

add_dependencies(frame_fuzzer gmock ReactiveSocket)

add_test(
  NAME FrameFuzzerTests
  COMMAND ./scripts/frame_fuzzer_test.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()

########################################
# TCK Drivers
########################################

if(BUILD_TESTS)
  add_executable(
    tckclient
    rsocket/tck-test/client.cpp
    rsocket/tck-test/TestFileParser.cpp
    rsocket/tck-test/TestFileParser.h
    rsocket/tck-test/FlowableSubscriber.cpp
    rsocket/tck-test/FlowableSubscriber.h
    rsocket/tck-test/SingleSubscriber.cpp
    rsocket/tck-test/SingleSubscriber.h
    rsocket/tck-test/TestSuite.cpp
    rsocket/tck-test/TestSuite.h
    rsocket/tck-test/TestInterpreter.cpp
    rsocket/tck-test/TestInterpreter.h
    rsocket/tck-test/TypedCommands.h
    rsocket/tck-test/BaseSubscriber.cpp
    rsocket/tck-test/BaseSubscriber.h)

  target_link_libraries(
    tckclient
    ReactiveSocket
    yarpl
    glog::glog
    gflags)

  add_executable(
    tckserver
    rsocket/tck-test/server.cpp
    rsocket/tck-test/MarbleProcessor.cpp
    rsocket/tck-test/MarbleProcessor.h
    rsocket/test/test_utils/StatsPrinter.cpp
    rsocket/test/test_utils/StatsPrinter.h)

  add_dependencies(tckserver gmock)
  target_link_libraries(
    tckserver
    ReactiveSocket
    yarpl
    ${GMOCK_LIBS}  # This also needs the preceding `add_dependencies`
    glog::glog
    gflags
    ${DOUBLE-CONVERSION})

# Download the latest TCK drivers JAR.
  set(TCK_DRIVERS_JAR rsocket-tck-drivers-0.9.10.jar)
  if (NOT EXISTS ${CMAKE_SOURCE_DIR}/${TCK_DRIVERS_JAR})
    join(TCK_DRIVERS_URL
      "https://oss.jfrog.org/libs-release/io/rsocket/"
      "rsocket-tck-drivers/0.9.10/${TCK_DRIVERS_JAR}")
    message(STATUS "Downloading ${TCK_DRIVERS_URL}")
    file(DOWNLOAD ${TCK_DRIVERS_URL} ${CMAKE_SOURCE_DIR}/${TCK_DRIVERS_JAR})
  endif ()
endif()

########################################
# Examples
########################################

if (BUILD_EXAMPLES)
add_library(
  reactivesocket_examples_util
  rsocket/examples/util/ExampleSubscriber.cpp
  rsocket/examples/util/ExampleSubscriber.h
  rsocket/test/test_utils/ColdResumeManager.h
  rsocket/test/test_utils/ColdResumeManager.cpp
)

target_link_libraries(
  reactivesocket_examples_util
  yarpl
  ReactiveSocket
  glog::glog
  gflags)

# request-response-hello-world

add_executable(
  example_request-response-hello-world-server
  rsocket/examples/request-response-hello-world/RequestResponseHelloWorld_Server.cpp
)

target_link_libraries(
  example_request-response-hello-world-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_request-response-hello-world-client
  rsocket/examples/request-response-hello-world/RequestResponseHelloWorld_Client.cpp
)

target_link_libraries(
  example_request-response-hello-world-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

# fire-and-forget-hello-world

add_executable(
  example_fire-and-forget-hello-world-server
  rsocket/examples/fire-and-forget-hello-world/FireAndForgetHelloWorld_Server.cpp
)

target_link_libraries(
  example_fire-and-forget-hello-world-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_fire-and-forget-hello-world-client
  rsocket/examples/fire-and-forget-hello-world/FireAndForgetHelloWorld_Client.cpp
)

target_link_libraries(
  example_fire-and-forget-hello-world-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)


# stream-hello-world

add_executable(
  example_stream-hello-world-server
  rsocket/examples/stream-hello-world/StreamHelloWorld_Server.cpp
)

target_link_libraries(
  example_stream-hello-world-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_stream-hello-world-client
  rsocket/examples/stream-hello-world/StreamHelloWorld_Client.cpp
)

target_link_libraries(
  example_stream-hello-world-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

# channel-hello-world

add_executable(
  example_channel-hello-world-server
  rsocket/examples/channel-hello-world/ChannelHelloWorld_Server.cpp
)

target_link_libraries(
  example_channel-hello-world-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)



add_executable(
  example_channel-hello-world-client
  rsocket/examples/channel-hello-world/ChannelHelloWorld_Client.cpp
)

target_link_libraries(
  example_channel-hello-world-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

# stream-observable-to-flowable

add_executable(
  example_observable-to-flowable-server
  rsocket/examples/stream-observable-to-flowable/StreamObservableToFlowable_Server.cpp
)

target_link_libraries(
  example_observable-to-flowable-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_observable-to-flowable-client
  rsocket/examples/stream-observable-to-flowable/StreamObservableToFlowable_Client.cpp
)

target_link_libraries(
  example_observable-to-flowable-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

# conditional-request-handling

add_executable(
  example_conditional-request-handling-server
  rsocket/examples/conditional-request-handling/ConditionalRequestHandling_Server.cpp
  rsocket/examples/conditional-request-handling/TextRequestHandler.h
  rsocket/examples/conditional-request-handling/TextRequestHandler.cpp
  rsocket/examples/conditional-request-handling/JsonRequestHandler.cpp
  rsocket/examples/conditional-request-handling/JsonRequestHandler.h
)

target_link_libraries(
  example_conditional-request-handling-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_conditional-request-handling-client
  rsocket/examples/conditional-request-handling/ConditionalRequestHandling_Client.cpp
)

target_link_libraries(
  example_conditional-request-handling-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

# warm-resumption

add_executable(
  example_resumption-server
  rsocket/examples/resumption/Resumption_Server.cpp
)

target_link_libraries(
  example_resumption-server
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_warm-resumption-client
  rsocket/examples/resumption/WarmResumption_Client.cpp
)

target_link_libraries(
  example_warm-resumption-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

add_executable(
  example_cold-resumption-client
  rsocket/examples/resumption/ColdResumption_Client.cpp
)

target_link_libraries(
  example_cold-resumption-client
  ReactiveSocket
  reactivesocket_examples_util
  yarpl
  glog::glog
  gflags)

endif () # BUILD_EXAMPLES

########################################
# End Examples
########################################

if (BUILD_BENCHMARKS)
  add_subdirectory(rsocket/benchmarks)
endif ()
