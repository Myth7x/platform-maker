add_executable(opm_client
	src/main.cpp
	src/client_app.cpp
	src/net_client.cpp
	src/tile_layer.cpp
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

if(OPM_CAN_BUILD_VULKAN_CLIENT)
	add_library(opm_imgui STATIC
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui_draw.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui_tables.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/imgui_widgets.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/backends/imgui_impl_glfw.cpp
		${CMAKE_SOURCE_DIR}/extern/imgui/backends/imgui_impl_vulkan.cpp
	)

	target_include_directories(opm_imgui
		PUBLIC
			${CMAKE_SOURCE_DIR}/extern/imgui
			${CMAKE_SOURCE_DIR}/extern/imgui/backends
	)

	target_link_libraries(opm_imgui PUBLIC ${OPM_GLFW_TARGET} Vulkan::Vulkan)

	target_compile_definitions(opm_client PRIVATE OPM_CLIENT_WITH_VULKAN=1)
	target_link_libraries(opm_client PRIVATE opm_imgui ${OPM_GLFW_TARGET} Vulkan::Vulkan)
else()
	message(WARNING "Vulkan path disabled (Vulkan and/or GLFW missing).")
endif()

if(OpenGL_FOUND AND OPM_HAS_GLFW)
	target_compile_definitions(opm_client PRIVATE OPM_CLIENT_WITH_OPENGL_STUB=1)
	target_link_libraries(opm_client PRIVATE OpenGL::GL ${OPM_GLFW_TARGET})
endif()

if(PNG_FOUND)
	target_compile_definitions(opm_client PRIVATE OPM_CLIENT_HAS_PNG=1)
	target_link_libraries(opm_client PRIVATE PNG::PNG)
endif()

target_link_libraries(opm_client
	PRIVATE
		opm_engine
		opm_project_options
		opm_project_warnings
)
