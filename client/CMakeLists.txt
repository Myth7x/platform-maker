add_executable(opm_client
	src/main.cpp
	src/client_app.cpp
	src/net_client.cpp
	src/tile_layer.cpp
	src/sprite_config.cpp
	src/game/actor_manager.cpp
	src/game/network_session.cpp
	src/net/message_router.cpp
	src/render/asset_registry.cpp
	src/render/hud.cpp
	src/render/render_context.cpp
	src/render/sprite.cpp
	src/render/texture_loader.cpp
	src/render/theme.cpp
	src/screens/main_menu_screen.cpp
	src/screens/level_picker_screen.cpp
	src/screens/online_level_select_screen.cpp
	src/screens/level_creator_screen.cpp
	src/screens/playing_screen.cpp
)

target_include_directories(opm_client PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(opm_client PRIVATE OPM_CLIENT_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/client/resources")

find_package(Vulkan QUIET)
find_package(glfw3 QUIET)
find_package(OpenGL QUIET)
find_package(PNG QUIET)

set(OPM_CAN_BUILD_VULKAN_CLIENT FALSE)
set(OPM_HAS_GLFW FALSE)
if(TARGET glfw)
	set(OPM_HAS_GLFW TRUE)
	set(OPM_GLFW_TARGET glfw)
elseif(TARGET glfw3::glfw)
	set(OPM_HAS_GLFW TRUE)
	set(OPM_GLFW_TARGET glfw3::glfw)
endif()

if(Vulkan_FOUND AND OPM_HAS_GLFW)
	set(OPM_CAN_BUILD_VULKAN_CLIENT TRUE)
endif()

if(OpenGL_FOUND AND OPM_HAS_GLFW)
	# Build ImGui as a static lib with GLFW + OpenGL2 (fixed-pipeline) backend.
	# OpenGL2 backend matches our existing immediate-mode rendering.
	add_library(opm_imgui STATIC
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui_draw.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui_tables.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui_widgets.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/backends/imgui_impl_glfw.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/backends/imgui_impl_opengl2.cpp
	)

	target_include_directories(opm_imgui
		PUBLIC
			${CMAKE_SOURCE_DIR}/extern/imgui
			${CMAKE_SOURCE_DIR}/extern/imgui/backends
	)

	target_link_libraries(opm_imgui PUBLIC ${OPM_GLFW_TARGET} OpenGL::GL)

	target_compile_definitions(opm_client PRIVATE
		OPM_CLIENT_WITH_OPENGL_STUB=1
		OPM_CLIENT_HAS_IMGUI=1
	)
	target_link_libraries(opm_client PRIVATE opm_imgui OpenGL::GL ${OPM_GLFW_TARGET})
endif()

if(Vulkan_FOUND AND OPM_HAS_GLFW)
	target_compile_definitions(opm_client PRIVATE OPM_CLIENT_WITH_VULKAN=1)
	target_link_libraries(opm_client PRIVATE Vulkan::Vulkan)
endif()

if(PNG_FOUND)
	target_compile_definitions(opm_client PRIVATE OPM_CLIENT_HAS_PNG=1)
	target_link_libraries(opm_client PRIVATE PNG::PNG)
endif()

if(DEFINED stb_SOURCE_DIR)
	target_compile_definitions(opm_client PRIVATE OPM_CLIENT_HAS_STB_IMAGE=1)
	target_include_directories(opm_client PRIVATE ${stb_SOURCE_DIR})
endif()

target_link_libraries(opm_client
	PRIVATE
		opm_engine
		opm_project_options
		opm_project_warnings
)
