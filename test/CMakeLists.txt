add_executable(lotman-gtest main.cpp orm_tests.cpp migration_tests.cpp strict_hierarchy_tests.cpp reclamation_tests.cpp ../src/lotman.cpp ../src/lotman_db.cpp ../src/lotman_internal.cpp)
if( NOT LOTMAN_EXTERNAL_GTEST )
    add_dependencies(lotman-gtest gtest)
    include_directories("${PROJECT_SOURCE_DIR}/vendor/gtest/googletest/include")
endif()

if(LOTMAN_EXTERNAL_GTEST)
    set(LIBGTEST "gtest")
else()
    set(LIBGTEST "${CMAKE_BINARY_DIR}/external/gtest/src/gtest-build/lib/libgtest.a")
endif()

target_link_libraries(lotman-gtest "${LIBGTEST}" pthread ${SQLITE_LIBRARIES} nlohmann_json nlohmann_json_schema_validator sqlite_orm::sqlite_orm)

add_test(
  NAME
    unit
  COMMAND
    ${CMAKE_CURRENT_BINARY_DIR}/lotman-gtest
  )
