# Collect source files
file(GLOB LOUVAIN_EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(LOUVAIN_EXTENSION_OBJECT_FILES ${LOUVAIN_EXTENSION_SOURCES})

# Use the extension build system
build_extension_lib("louvain")

# Set include directories
target_include_directories(neug_louvain_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR})

if(ARROW_INCLUDE_DIRS)
    target_include_directories(neug_louvain_extension PRIVATE SYSTEM ${ARROW_INCLUDE_DIRS})
endif()

# OpenMP (optional — the algorithm has serial fallbacks under #ifdef _OPENMP,
# and Apple Clang doesn't ship libomp by default).
find_package(OpenMP QUIET)

# Link libraries
target_link_libraries(neug_louvain_extension PRIVATE
    neug
)
if(OpenMP_CXX_FOUND)
    message(STATUS "Louvain: OpenMP enabled (parallel build)")
    target_link_libraries(neug_louvain_extension PRIVATE OpenMP::OpenMP_CXX)
    target_compile_options(neug_louvain_extension PRIVATE ${OpenMP_CXX_FLAGS})
else()
    message(STATUS "Louvain: OpenMP NOT found — building serial (single-threaded)")
endif()

if (${BUILD_TEST})
    add_subdirectory(test)
endif ()
