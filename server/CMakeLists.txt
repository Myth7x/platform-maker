add_executable(opm_server
	src/main.cpp
	src/server_app.cpp
)

target_include_directories(opm_server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(opm_server
	PRIVATE
		opm_engine
		opm_project_options
		opm_project_warnings
)
