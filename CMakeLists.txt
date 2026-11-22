cmake_minimum_required(VERSION 3.21)
project(MafiaIIIReflections VERSION 0.1.2 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Build MafiaIIIReflections with a Windows x64 toolchain.")
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

add_library(mafia_reflections_proxy SHARED
    src/dinput8_proxy.cpp src/native_d3d11_hooks.cpp src/reflection_observer.cpp
    src/reflection_menu.cpp)
set_target_properties(mafia_reflections_proxy PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
target_link_libraries(mafia_reflections_proxy PRIVATE bcrypt d3d11 dxgi)

# A genuine native GPU denoiser, built/tested independently.
# Deliberately not attached to unverified game reflection shaders yet.
add_library(ultra_spatial_filter STATIC src/ultra_spatial_filter.cpp)
target_include_directories(ultra_spatial_filter PUBLIC src)
target_link_libraries(ultra_spatial_filter PUBLIC d3d11 d3dcompiler dxgi)

add_library(reflection_msaa STATIC src/msaa_reflection.cpp)
target_include_directories(reflection_msaa PUBLIC src)
target_link_libraries(reflection_msaa PUBLIC d3d11 dxgi)

include(CTest)
if(BUILD_TESTING)
    add_executable(ultra_spatial_filter_test tests/ultra_spatial_filter_test.cpp)
    target_link_libraries(ultra_spatial_filter_test PRIVATE ultra_spatial_filter)
    add_test(NAME ultra_spatial_filter_warp COMMAND ultra_spatial_filter_test)

    add_executable(msaa_reflection_test tests/msaa_reflection_test.cpp)
    target_link_libraries(msaa_reflection_test PRIVATE reflection_msaa d3dcompiler)
    add_test(NAME reflection_msaa_warp COMMAND msaa_reflection_test)

    add_executable(reflection_settings_test tests/reflection_settings_test.cpp src/reflection_observer.cpp)
    target_include_directories(reflection_settings_test PRIVATE src)
    add_test(NAME reflection_settings COMMAND reflection_settings_test)
endif()
