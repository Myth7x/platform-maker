add_executable(opm_server
	src/main.cpp
	src/server_app.cpp
	src/server.cpp
	src/net/sender.cpp
	src/net/wire.cpp
	src/net/client_connection.cpp
	src/game/lobby.cpp
	src/game/lobby_manager.cpp
	src/game/level_storage.cpp
	src/runtime/scoped_timer_resolution.cpp
	src/runtime/tick_pacer.cpp
)

target_include_directories(opm_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(opm_server
	PRIVATE
		opm_engine
		opm_project_options
		opm_project_warnings
)
