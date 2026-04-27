# Collect source files
file(GLOB KCORE_EXTENSION_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
set(KCORE_EXTENSION_OBJECT_FILES ${KCORE_EXTENSION_SOURCES})

# Use the extension build system
build_extension_lib("kcore")

# Set include directories
target_include_directories(neug_kcore_extension PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR})

if(ARROW_INCLUDE_DIRS)
    target_include_directories(neug_kcore_extension PRIVATE SYSTEM ${ARROW_INCLUDE_DIRS})
endif()

# OpenMP (optional — used to parallelize projection construction; the peeling
# algorithm itself is serial, but graph build can be sped up).
find_package(OpenMP QUIET)

# Link libraries
target_link_libraries(neug_kcore_extension PRIVATE
    neug
)
if(OpenMP_CXX_FOUND)
    message(STATUS "Kcore: OpenMP enabled (parallel build)")
    target_link_libraries(neug_kcore_extension PRIVATE OpenMP::OpenMP_CXX)
    target_compile_options(neug_kcore_extension PRIVATE ${OpenMP_CXX_FLAGS})
else()
    message(STATUS "Kcore: OpenMP NOT found — building serial (single-threaded)")
endif()

if (${BUILD_TEST})
    add_subdirectory(test)
endif ()
