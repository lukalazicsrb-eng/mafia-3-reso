cmake_minimum_required(VERSION 3.21)
project(MafiaIIIReflections VERSION 0.1.0 LANGUAGES CXX)

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

add_library(mafia_reflections_proxy SHARED src/dinput8_proxy.cpp)
set_target_properties(mafia_reflections_proxy PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
target_link_libraries(mafia_reflections_proxy PRIVATE bcrypt)

add_library(mafia_reflections_observer SHARED src/native_d3d11_hooks.cpp src/reflection_observer.cpp)
set_target_properties(mafia_reflections_observer PROPERTIES PREFIX "")
target_link_libraries(mafia_reflections_observer PRIVATE bcrypt d3d11 dxgi)

add_library(reflection_msaa STATIC src/msaa_reflection.cpp)
target_include_directories(reflection_msaa PUBLIC src)
target_link_libraries(reflection_msaa PUBLIC d3d11 dxgi)

include(CTest)
if(BUILD_TESTING)
    add_executable(msaa_reflection_test tests/msaa_reflection_test.cpp)
    target_link_libraries(msaa_reflection_test PRIVATE reflection_msaa d3dcompiler)
    add_test(NAME reflection_msaa_warp COMMAND msaa_reflection_test)
endif()
