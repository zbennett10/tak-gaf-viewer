cmake_minimum_required(VERSION 3.16)
project(gaf-viewer C)

set(CMAKE_C_STANDARD 11)

# ── Find SDL2 ──────────────────────────────────────────────────────────────
# Supports: vcpkg, system install, or manual SDL2_DIR
find_package(SDL2 REQUIRED)

# ── GAF Viewer executable ─────────────────────────────────────────────────
add_executable(gaf-viewer
    src/main.c
    src/gaf.c
    src/palette.c
    src/pal_lookup.c
)

target_include_directories(gaf-viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Handle both modern SDL2::SDL2 targets and legacy SDL2 variables
if(TARGET SDL2::SDL2)
    target_link_libraries(gaf-viewer PRIVATE SDL2::SDL2 SDL2::SDL2main)
else()
    target_include_directories(gaf-viewer PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(gaf-viewer PRIVATE ${SDL2_LIBRARIES})
endif()

# ── Copy SDL2.dll on Windows ──────────────────────────────────────────────
if(WIN32 AND TARGET SDL2::SDL2)
    add_custom_command(TARGET gaf-viewer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:gaf-viewer>
        COMMENT "Copying SDL2.dll"
    )
endif()

# ── Install ───────────────────────────────────────────────────────────────
install(TARGETS gaf-viewer RUNTIME DESTINATION bin)
