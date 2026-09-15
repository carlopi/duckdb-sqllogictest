//===----------------------------------------------------------------------===//
//                         DuckDB
//
// bundled_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_system.hpp"

namespace duckdb {

//! Serves the tests compiled into this extension under sqllogic://, so they can be listed, globbed
//! and read like any other path. Registered on the database the extension is loaded into and
//! nowhere else: the databases a test runs on are meant to be an unmodified DuckDB, so the corpus
//! is visible to the harness and invisible to what it is testing.
class BundledFileSystem : public FileSystem {
public:
	static constexpr const char *SCHEME = "sqllogic://";

	unique_ptr<FileHandle> OpenFile(const string &path, FileOpenFlags flags,
	                                optional_ptr<FileOpener> opener = nullptr) override;
	int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	void Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) override;
	int64_t GetFileSize(FileHandle &handle) override;
	void Seek(FileHandle &handle, idx_t location) override;
	idx_t SeekPosition(FileHandle &handle) override;
	bool CanSeek() override;
	bool OnDiskFile(FileHandle &handle) override;
	bool FileExists(const string &filename, optional_ptr<FileOpener> opener = nullptr) override;
	vector<OpenFileInfo> Glob(const string &path, FileOpener *opener = nullptr) override;
	bool CanHandleFile(const string &path) override;
	string GetName() const override;
};

} // namespace duckdb
