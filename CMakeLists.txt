cmake_minimum_required(VERSION 3.15)
project(sdvx_res_hook VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(res_hook SHARED src/res_hook.cpp)

set_target_properties(res_hook PROPERTIES
    PREFIX ""
    OUTPUT_NAME "res_hook"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/dist"
)

target_compile_definitions(res_hook PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(res_hook PRIVATE /W3 /O2 /MT /EHsc)
    target_link_options(res_hook PRIVATE /INCREMENTAL:NO)
else()
    target_compile_options(res_hook PRIVATE -Wall -Wextra -O2)
    target_link_options(res_hook PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lstdc++ -lpthread -Wl,-Bdynamic)
endif()

target_link_libraries(res_hook PRIVATE d3d9 user32 gdi32 psapi)

add_custom_command(TARGET res_hook POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/config/res_hook.ini"
        "$<TARGET_FILE_DIR:res_hook>/res_hook.ini"
)
