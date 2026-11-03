cmake_minimum_required(VERSION 3.25)

project(xigncode_shim_experiment LANGUAGES CXX)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(xigncode_shim SHARED
    xigncode_shim.cpp
    xigncode_shim.def
)

set_target_properties(xigncode_shim PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "x3_x64"
    SUFFIX ".xem"
)

add_custom_command(
    TARGET xigncode_shim
    POST_BUILD
    COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/finalize_bootstrap_marker.py"
        "$<TARGET_FILE:xigncode_shim>"
    COMMENT "Finalize the experimental module's bootstrap checksum marker"
    VERBATIM
)
