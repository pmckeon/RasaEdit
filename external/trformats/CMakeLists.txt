cmake_minimum_required(VERSION 3.16)
project(trformats CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# zlib: on Windows e.g. `vcpkg install zlib` and pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
find_package(ZLIB REQUIRED)

add_library(trformats trformats.cpp trformats.h assetlibrary.cpp assetlibrary.h gamedata.cpp gamedata.h dds.cpp dds.h)
target_include_directories(trformats PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(trformats PUBLIC ZLIB::ZLIB)
if(MSVC)
    target_compile_definitions(trformats PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_options(trformats PRIVATE /W4)
else()
    target_compile_options(trformats PRIVATE -Wall -Wextra)
endif()

# Command-line tools: built by default only when this is the top-level project, so an editor that
# includes the library with add_subdirectory() gets just the library.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(TRFORMATS_TOOLS_DEFAULT ON)
else()
    set(TRFORMATS_TOOLS_DEFAULT OFF)
endif()
option(TRFORMATS_BUILD_TOOLS "Build trtool, trassets, trgamedata and trtest" ${TRFORMATS_TOOLS_DEFAULT})

if(TRFORMATS_BUILD_TOOLS)
    # Each tool is built only if its source file is present.
    foreach(tool trtool trassets trgamedata trtest)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${tool}.cpp")
            add_executable(${tool} ${tool}.cpp)
            target_link_libraries(${tool} PRIVATE trformats)
        else()
            message(STATUS "trformats: ${tool}.cpp not found, skipping the ${tool} tool")
        endif()
    endforeach()

    # Regression test over the game's maps: cmake -DTR_MAPS_DIR=<game>/data/maps ..  then  ctest
    set(TR_MAPS_DIR "" CACHE PATH "Folder containing the game's map folders (for ctest)")
    if(TR_MAPS_DIR AND TARGET trtest)
        enable_testing()
        add_test(NAME all_maps COMMAND trtest ${TR_MAPS_DIR})
    endif()
endif()
