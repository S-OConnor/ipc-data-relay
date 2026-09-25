# Control tool backend protocol code (HTTP, WebSocket, JSON), also used by the
# unit tests.
add_library(ipcrelay_ctl_core STATIC
  backend/http.cpp
  backend/json_value.cpp
  backend/websocket.cpp)
target_include_directories(ipcrelay_ctl_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/backend)
target_link_libraries(ipcrelay_ctl_core PUBLIC ipcrelay_common PRIVATE ipcrelay_warnings)

# Frontend files are compiled into the binary so the tool deploys as one file;
# --web-root serves them from disk instead while developing the frontend.
set(CTL_FRONTEND_FILES index.html app.js style.css)
set(CTL_FRONTEND_PATHS ${CTL_FRONTEND_FILES})
list(TRANSFORM CTL_FRONTEND_PATHS PREPEND ${CMAKE_CURRENT_SOURCE_DIR}/frontend/)
string(REPLACE ";" "," CTL_FRONTEND_LIST "${CTL_FRONTEND_FILES}")
set(CTL_WEB_ASSETS_CPP ${CMAKE_CURRENT_BINARY_DIR}/web_assets.cpp)
add_custom_command(
  OUTPUT ${CTL_WEB_ASSETS_CPP}
  COMMAND ${CMAKE_COMMAND}
    -DBASE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/frontend
    -DFILES=${CTL_FRONTEND_LIST}
    -DOUTPUT=${CTL_WEB_ASSETS_CPP}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.cmake
  DEPENDS ${CTL_FRONTEND_PATHS} ${CMAKE_CURRENT_SOURCE_DIR}/embed_assets.cmake
  COMMENT "Embedding ipc-relay-ctl frontend"
  VERBATIM)

add_executable(ipc-relay-ctl
  main.cpp
  backend/control_server.cpp
  ${CTL_WEB_ASSETS_CPP})
target_link_libraries(ipc-relay-ctl PRIVATE ipcrelay_ctl_core ipcrelay_receiver_core ipcrelay_warnings)

install(TARGETS ipc-relay-ctl
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
