cmake_minimum_required(VERSION 3.25)
project(open_platformer_maker VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "This project currently supports 64-bit builds only.")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/output/lib)

option(OPM_BUILD_SERVER "Build dedicated server" ON)
option(OPM_BUILD_CLIENT "Build Vulkan client" ON)
option(OPM_BUILD_ENGINE_TESTS "Build engine tests" OFF)

add_library(opm_project_options INTERFACE)
target_compile_features(opm_project_options INTERFACE cxx_std_23)

add_library(opm_project_warnings INTERFACE)
if(MSVC)
    target_compile_options(opm_project_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(opm_project_warnings INTERFACE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(OPM_BUILD_CLIENT)
    include(FetchContent)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)

    FetchContent_Declare(
        stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(stb)
endif()

add_subdirectory(engine)

if(OPM_BUILD_SERVER)
    add_subdirectory(server)
endif()

if(OPM_BUILD_CLIENT)
    add_subdirectory(client)
endif()
