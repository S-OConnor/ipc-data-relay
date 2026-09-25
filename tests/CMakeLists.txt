# Unit tests (self-contained mini framework, no external test dependency)
add_executable(unit_tests
  unit/test_main.cpp
  unit/test_byteorder.cpp
  unit/test_wire_protocol.cpp
  unit/test_reassembler.cpp
  unit/test_sequence_tracker.cpp
  unit/test_capture_format.cpp
  unit/test_config.cpp
  unit/test_json_writer.cpp
  unit/test_zmq_pubsub.cpp
  unit/test_ctl_protocol.cpp
  ${CMAKE_SOURCE_DIR}/src/bridge/bridge_config.cpp
  ${CMAKE_SOURCE_DIR}/tools/receiver/receiver_config.cpp)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tools)
target_link_libraries(unit_tests PRIVATE ipcrelay_receiver_core ipcrelay_ctl_core
  ipcrelay_common ipcrelay_warnings)
add_test(NAME unit_tests COMMAND unit_tests)

# Helper: independent ZeroMQ subscriber that counts messages (used to show the
# bridge does not steal messages from other subscribers, BRG-131).
add_executable(sub_counter helpers/sub_counter.cpp)
target_link_libraries(sub_counter PRIVATE ipcrelay_common ipcrelay_warnings)

find_package(Python3 COMPONENTS Interpreter)

set(IPCRELAY_TEST_ENV
  "BRIDGE=$<TARGET_FILE:ipc-relay-bridge>"
  "RECEIVER=$<TARGET_FILE:ipc-relay-receiver>"
  "TESTPUB=$<TARGET_FILE:ipc-relay-testpub>"
  "CTL=$<TARGET_FILE:ipc-relay-ctl>"
  "SUB_COUNTER=$<TARGET_FILE:sub_counter>"
  "INSPECT=${CMAKE_SOURCE_DIR}/tools/capture_inspect.py"
  "SOURCE_DIR=${CMAKE_SOURCE_DIR}")

# End-to-end: testpub (3 IPC sources) -> bridge -> multicast -> receiver ->
# capture file -> capture_inspect.py, plus an independent subscriber.
add_test(NAME e2e_multi_source_pipeline
  COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/test_e2e.sh)
set_tests_properties(e2e_multi_source_pipeline PROPERTIES
  ENVIRONMENT "${IPCRELAY_TEST_ENV}" TIMEOUT 120)

# Runtime control: record on/off over the ZeroMQ TCP command channel and
# statistics over the ZeroMQ TCP stats channel.
add_test(NAME e2e_runtime_control
  COMMAND ${CMAKE_SOURCE_DIR}/tests/integration/test_control.sh)
set_tests_properties(e2e_runtime_control PROPERTIES
  ENVIRONMENT "${IPCRELAY_TEST_ENV}" TIMEOUT 120)

if(Python3_Interpreter_FOUND)
  # Receiver fault injection with raw UDP datagrams built by an independent
  # Python implementation of the wire protocol: sequence gaps, missing,
  # out-of-order, duplicate and malformed fragments.
  add_test(NAME receiver_fault_injection
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/test_receiver_faults.py)
  set_tests_properties(receiver_fault_injection PROPERTIES
    ENVIRONMENT "${IPCRELAY_TEST_ENV}" TIMEOUT 120)

  # Control tool web backend: HTTP frontend, WebSocket stats push at the
  # configured rate and start/stop recording from the browser protocol.
  add_test(NAME ctl_web_backend
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/test_ctl_web.py)
  set_tests_properties(ctl_web_backend PROPERTIES
    ENVIRONMENT "${IPCRELAY_TEST_ENV}" TIMEOUT 120)

  # Capture inspector self-test against a synthetic capture file.
  add_test(NAME capture_inspect_selftest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/capture_inspect.py --self-test)
endif()
