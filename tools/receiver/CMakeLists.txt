# Receiver-only library: capture format, reassembly, sequence tracking, JSON
add_library(ipcrelay_receiver_core STATIC
  lib/capture_format.cpp
  lib/capture_reader.cpp
  lib/capture_writer.cpp
  lib/json_writer.cpp
  lib/reassembler.cpp
  lib/sequence_tracker.cpp)
target_include_directories(ipcrelay_receiver_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(ipcrelay_receiver_core PUBLIC ipcrelay_common
  PRIVATE ipcrelay_warnings)

add_executable(ipc-relay-receiver
  main.cpp
  receiver.cpp
  receiver_config.cpp
  receiver_stats.cpp)
target_link_libraries(ipc-relay-receiver PRIVATE ipcrelay_receiver_core ipcrelay_warnings)

install(TARGETS ipc-relay-receiver
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
