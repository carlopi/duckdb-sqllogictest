# This file is included by DuckDB's build system. It specifies which extension to load

duckdb_extension_load(sqllogictest
    DONT_LINK
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
)
