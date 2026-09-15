//===----------------------------------------------------------------------===//
//                         DuckDB
//
// bundled_tests.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

//! A test compiled into the extension and served under sqllogic://. Content is a C string rather
//! than a duckdb::string so the table is a constant with no start-up cost.
struct BundledTest {
	const char *path;
	const char *content;
	idx_t size;
};

extern const BundledTest BUNDLED_TESTS[];
extern const idx_t BUNDLED_TEST_COUNT;

} // namespace duckdb
