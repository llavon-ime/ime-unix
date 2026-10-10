cmake_minimum_required(VERSION 3.20)

if(TARGET llavon_ime_protocol)
    return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../engine/cmake/Warnings.cmake")
add_library(llavon_ime_protocol STATIC src/protocol.cpp)
set_target_properties(llavon_ime_protocol PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_EXTENSIONS OFF)
target_compile_features(llavon_ime_protocol PUBLIC cxx_std_23)
target_include_directories(llavon_ime_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
llavon_ime_enable_warnings(llavon_ime_protocol)
