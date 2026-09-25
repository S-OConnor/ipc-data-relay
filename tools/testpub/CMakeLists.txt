add_executable(ipc-relay-testpub main.cpp)
target_link_libraries(ipc-relay-testpub PRIVATE ipcrelay_common ipcrelay_warnings)

install(TARGETS ipc-relay-testpub
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
