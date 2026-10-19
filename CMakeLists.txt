cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME sqllogictest)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The runner lives in DuckDB's test tree, which is not a public API: these are compiled straight
# out of the submodule, so the pinned DuckDB and this extension always agree.
set(DUCKDB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/duckdb")

# The runner resolves {BUILD_DIRECTORY} and its local extension repository against DuckDB's build dir
add_definitions(-DDUCKDB_BUILD_DIRECTORY="${CMAKE_BINARY_DIR}")

# The bundled corpus: the list is checked in so the selection is reviewable, the content is read
# from the submodule at build time so it cannot drift from the tests themselves.
set(BUNDLED_TESTS_CPP "${CMAKE_CURRENT_BINARY_DIR}/bundled_tests.cpp")
# the tests themselves are dependencies too, so a submodule bump re-embeds them
file(GLOB_RECURSE BUNDLED_TEST_SOURCES "${DUCKDB_SRC}/test/sql/*.test")
add_custom_command(
  OUTPUT ${BUNDLED_TESTS_CPP}
  COMMAND ${CMAKE_COMMAND} -E echo "Embedding bundled sqllogictests"
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_bundled_sqllogictests.py
          ${CMAKE_CURRENT_SOURCE_DIR}/src/bundled_tests.list ${DUCKDB_SRC} ${BUNDLED_TESTS_CPP}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/bundled_tests.list
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_bundled_sqllogictests.py
          ${BUNDLED_TEST_SOURCES}
  COMMENT "Embedding bundled sqllogictests")

set(EXTENSION_SOURCES
    src/sqllogictest_extension.cpp
    src/bundled_file_system.cpp
    ${DUCKDB_SRC}/test/sqlite/result_helper.cpp
    ${DUCKDB_SRC}/test/sqlite/sqllogic_command.cpp
    ${DUCKDB_SRC}/test/sqlite/sqllogic_parser.cpp
    ${DUCKDB_SRC}/test/sqlite/sqllogic_test_logger.cpp
    ${DUCKDB_SRC}/test/sqlite/sqllogic_test_runner.cpp
    ${DUCKDB_SRC}/test/helpers/pid.cpp
    ${DUCKDB_SRC}/test/helpers/test_config.cpp
    ${DUCKDB_SRC}/test/helpers/test_helpers.cpp
    ${DUCKDB_SRC}/test/helpers/test_reporter.cpp
    ${BUNDLED_TESTS_CPP})

build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_include_directories(
  ${LOADABLE_EXTENSION_NAME}
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/include"
          "${DUCKDB_SRC}/test/include"
          "${DUCKDB_SRC}/test/sqlite"
          "${DUCKDB_SRC}/third_party/re2")
# result comparison uses re2, whose symbols are not exported from the duckdb library
target_link_libraries(${LOADABLE_EXTENSION_NAME} duckdb_re2)
