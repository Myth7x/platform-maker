add_library(opm_engine STATIC
	src/engine.cpp
	src/protocol.cpp
	src/assets.cpp
	src/level.cpp
	src/tile_metadata.cpp
)

target_include_directories(opm_engine
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opm_engine
	PUBLIC
		opm_project_options
		opm_project_warnings
)

if(OPM_BUILD_ENGINE_TESTS)
	enable_testing()
	add_executable(opm_engine_determinism_test
		src/engine_determinism_test.cpp
	)
	target_link_libraries(opm_engine_determinism_test PRIVATE opm_engine)
	add_test(NAME opm_engine_determinism_test COMMAND opm_engine_determinism_test)

	add_executable(opm_protocol_test
		src/protocol_test.cpp
	)
	target_link_libraries(opm_protocol_test PRIVATE opm_engine)
	add_test(NAME opm_protocol_test COMMAND opm_protocol_test)
endif()
