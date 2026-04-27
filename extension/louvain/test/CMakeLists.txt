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

add_executable(case_infra_datasource_country case_infra_datasource_country.cc)

target_include_directories(case_infra_datasource_country PRIVATE
    ${CMAKE_SOURCE_DIR}/extension/louvain/include
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(case_infra_datasource_country PRIVATE
    neug
    ${GFLAGS_LIBRARIES}
)
add_dependencies(case_infra_datasource_country neug_louvain_extension)

add_test(NAME test_louvain_install COMMAND test_louvain_install)
add_test(NAME test_louvain_ldbc COMMAND test_louvain_ldbc)
add_test(NAME case_infra_datasource_country COMMAND case_infra_datasource_country)

install(TARGETS test_louvain_install test_louvain_ldbc
                case_infra_datasource_country
    RUNTIME DESTINATION bin
)
