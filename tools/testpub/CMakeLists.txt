# Simulated telemetry structures, shared with ipc-relay-capture-to-csv
# (included as "testpub/telemetry.hpp").
add_library(ipcrelay_telemetry STATIC telemetry.cpp)
target_include_directories(ipcrelay_telemetry PUBLIC
  $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/tools>)
target_link_libraries(ipcrelay_telemetry PUBLIC ipcrelay_common PRIVATE ipcrelay_warnings)

add_executable(ipc-relay-testpub main.cpp)
target_link_libraries(ipc-relay-testpub PRIVATE ipcrelay_telemetry ipcrelay_common ipcrelay_warnings)

install(TARGETS ipc-relay-testpub
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
