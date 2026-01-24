#
# Simple C99 API Example
#
# This example demonstrates using the simple C99 API embedded in the slang library.
# No separate library is needed - just link with slang.
#

add_executable(test-shader
    main.c
)

target_include_directories(test-shader
    PRIVATE ${slang_SOURCE_DIR}/include
)

target_link_libraries(test-shader
    PRIVATE slang
)

set_target_properties(test-shader PROPERTIES
    FOLDER "examples"
    C_STANDARD 99
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>/bin"
)

# Add to all-examples meta target if it exists
if(TARGET all-examples)
    add_dependencies(all-examples test-shader)
endif()
