
cmake_minimum_required(VERSION 3.14)
project(
    Lotman
    DESCRIPTION "A C++ library to interface to LotMan"
    VERSION 0.1.0
    LANGUAGES CXX
    )

option(LOTMAN_BUILD_UNITTESTS "Build the lotman-cpp unit tests" OFF)
option(LOTMAN_EXTERNAL_GTEST "Use an external/pre-installed copy of GTest" OFF)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake;${CMAKE_MODULE_PATH}")

# Fetch sqlite_orm
include(FetchContent)
FetchContent_Declare(
    sqlite_orm
    GIT_REPOSITORY https://github.com/fnc12/sqlite_orm.git
    GIT_TAG v1.8.2
)
FetchContent_MakeAvailable(sqlite_orm)

set(CMAKE_BUILD_TYPE RelWithDebInfo) # -g -O2

include(GNUInstallDirs)

# Allow user to override library install directory
set(LIB_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}" CACHE PATH "Installation directory for libraries")

if(APPLE)
  find_package(Sqlite3 REQUIRED)
  find_package(nlohmann_json REQUIRED)
  find_package(nlohmann_json_schema_validator REQUIRED)
  set(CMAKE_MACOSX_RPATH ON)

elseif(UNIX)
  include (FindPkgConfig)

  pkg_check_modules(SQLITE REQUIRED sqlite3)
  find_package(nlohmann_json REQUIRED)
  find_package(nlohmann_json_schema_validator REQUIRED)
endif()

add_library(LotMan SHARED src/lotman.cpp src/lotman_db.cpp src/lotman_internal.cpp)
target_compile_features(LotMan PUBLIC cxx_std_17)

configure_file(lotman_version.h.in lotman_version.h)
include_directories("${PROJECT_BINARY_DIR}")

target_link_libraries(LotMan PUBLIC ${SQLITE_LIBRARIES} nlohmann_json nlohmann_json_schema_validator sqlite_orm::sqlite_orm)

if (NOT APPLE AND UNIX)
  set_target_properties(LotMan PROPERTIES LINK_FLAGS "-Wl,--version-script=${PROJECT_SOURCE_DIR}/configs/export-symbols")
endif()

install(
  TARGETS LotMan
  LIBRARY DESTINATION ${LIB_INSTALL_DIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(
  FILES src/lotman.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lotman
  )


if(LOTMAN_BUILD_UNITTESTS)
	if(NOT LOTMAN_EXTERNAL_GTEST)
    include(ExternalProject)
    ExternalProject_Add(gtest
        PREFIX external/gtest
        URL ${CMAKE_CURRENT_SOURCE_DIR}/vendor/gtest
        INSTALL_COMMAND :
    )
  endif()
  enable_testing()
  add_subdirectory(test)
endif()
