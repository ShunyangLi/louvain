# Tests for kcore extension

add_executable(case_kcore_country case_kcore_country.cc)

target_include_directories(case_kcore_country PRIVATE
    ${CMAKE_SOURCE_DIR}/extension/kcore/include
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(case_kcore_country PRIVATE
    neug
    ${GFLAGS_LIBRARIES}
)
add_dependencies(case_kcore_country neug_kcore_extension)

add_test(NAME case_kcore_country COMMAND case_kcore_country)

install(TARGETS case_kcore_country
    RUNTIME DESTINATION bin
)
