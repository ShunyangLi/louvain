# Tests for louvain extension

add_executable(test_louvain_install test_louvain_install.cc)

target_include_directories(test_louvain_install PRIVATE
    ${CMAKE_SOURCE_DIR}/extension/louvain/include
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_louvain_install PRIVATE
    neug
    ${GFLAGS_LIBRARIES}
)
add_dependencies(test_louvain_install neug_louvain_extension)

add_executable(test_louvain_ldbc test_louvain_ldbc.cc)

target_include_directories(test_louvain_ldbc PRIVATE
    ${CMAKE_SOURCE_DIR}/extension/louvain/include
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_louvain_ldbc PRIVATE
    neug
    ${GFLAGS_LIBRARIES}
)
add_dependencies(test_louvain_ldbc neug_louvain_extension)

add_test(NAME test_louvain_install COMMAND test_louvain_install)
add_test(NAME test_louvain_ldbc COMMAND test_louvain_ldbc)

install(TARGETS test_louvain_install test_louvain_ldbc
    RUNTIME DESTINATION bin
)
