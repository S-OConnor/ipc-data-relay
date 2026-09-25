# Capture file -> one CSV per testpub message type
add_library(ipcrelay_csv_export STATIC csv_export.cpp)
target_include_directories(ipcrelay_csv_export PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
target_link_libraries(ipcrelay_csv_export PUBLIC ipcrelay_receiver_core ipcrelay_telemetry
  PRIVATE ipcrelay_warnings)

add_executable(ipc-relay-capture-to-csv main.cpp)
target_link_libraries(ipc-relay-capture-to-csv PRIVATE ipcrelay_csv_export ipcrelay_warnings)

install(TARGETS ipc-relay-capture-to-csv
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
