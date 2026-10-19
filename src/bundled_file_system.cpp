#include "bundled_file_system.hpp"

#include "bundled_tests.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

namespace {

//! Reads one bundled test. The content outlives every handle - it is a constant in the binary - so
//! the handle only needs to remember where it is.
struct BundledFileHandle : public FileHandle {
	BundledFileHandle(FileSystem &file_system, string path, FileOpenFlags flags, const BundledTest &test)
	    : FileHandle(file_system, std::move(path), flags), test(test) {
	}

	void Close() override {
	}

	const BundledTest &test;
	idx_t position = 0;
};

//! The bundled test served at this path, or nullptr.
const BundledTest *FindBundledTest(const string &path) {
	auto without_scheme = StringUtil::Replace(path, BundledFileSystem::SCHEME, "");
	for (idx_t i = 0; i < BUNDLED_TEST_COUNT; i++) {
		if (without_scheme == BUNDLED_TESTS[i].path) {
			return &BUNDLED_TESTS[i];
		}
	}
	return nullptr;
}

} // namespace

unique_ptr<FileHandle> BundledFileSystem::OpenFile(const string &path, FileOpenFlags flags,
                                                   optional_ptr<FileOpener> opener) {
	if (flags.OpenForWriting()) {
		throw PermissionException("%s is read-only", SCHEME);
	}
	auto test = FindBundledTest(path);
	if (!test) {
		throw IOException("No bundled test named '%s'", path);
	}
	return make_uniq<BundledFileHandle>(*this, path, flags, *test);
}

int64_t BundledFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &bundled = handle.Cast<BundledFileHandle>();
	auto remaining = bundled.test.size - bundled.position;
	auto to_read = MinValue<idx_t>(NumericCast<idx_t>(nr_bytes), remaining);
	memcpy(buffer, bundled.test.content + bundled.position, to_read);
	bundled.position += to_read;
	return NumericCast<int64_t>(to_read);
}

void BundledFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	auto &bundled = handle.Cast<BundledFileHandle>();
	if (location + NumericCast<idx_t>(nr_bytes) > bundled.test.size) {
		throw IOException("Read past the end of bundled test '%s'", bundled.test.path);
	}
	memcpy(buffer, bundled.test.content + location, NumericCast<idx_t>(nr_bytes));
}

int64_t BundledFileSystem::GetFileSize(FileHandle &handle) {
	return NumericCast<int64_t>(handle.Cast<BundledFileHandle>().test.size);
}

void BundledFileSystem::Seek(FileHandle &handle, idx_t location) {
	handle.Cast<BundledFileHandle>().position = location;
}

idx_t BundledFileSystem::SeekPosition(FileHandle &handle) {
	return handle.Cast<BundledFileHandle>().position;
}

bool BundledFileSystem::CanSeek() {
	return true;
}

bool BundledFileSystem::OnDiskFile(FileHandle &handle) {
	return false;
}

bool BundledFileSystem::FileExists(const string &filename, optional_ptr<FileOpener> opener) {
	return FindBundledTest(filename) != nullptr;
}

//! Matches like the local file system does: * stays within one path segment, ** spans any number
//! of segments, including none.
static bool GlobMatches(const vector<string> &name, idx_t n, const vector<string> &pattern, idx_t p) {
	if (p == pattern.size()) {
		return n == name.size();
	}
	if (pattern[p] == "**") {
		for (idx_t skip = n; skip <= name.size(); skip++) {
			if (GlobMatches(name, skip, pattern, p + 1)) {
				return true;
			}
		}
		return false;
	}
	if (n == name.size()) {
		return false;
	}
	auto &segment = name[n];
	auto &wildcard = pattern[p];
	return ::duckdb::Glob(segment.c_str(), segment.size(), wildcard.c_str(), wildcard.size()) &&
	       GlobMatches(name, n + 1, pattern, p + 1);
}

vector<OpenFileInfo> BundledFileSystem::Glob(const string &path, FileOpener *opener) {
	// Ordinary glob semantics, with one shorthand: a path ending in a separator - sqllogic:// or
	// sqllogic://duckdb/ - stands for everything below it, i.e. an implied **.
	auto pattern = StringUtil::Replace(path, SCHEME, "");
	if (pattern.empty() || StringUtil::EndsWith(pattern, "/")) {
		pattern += "**";
	}
	auto pattern_segments = StringUtil::Split(pattern, '/');
	vector<OpenFileInfo> result;
	for (idx_t i = 0; i < BUNDLED_TEST_COUNT; i++) {
		string name = BUNDLED_TESTS[i].path;
		if (GlobMatches(StringUtil::Split(name, '/'), 0, pattern_segments, 0)) {
			result.emplace_back(SCHEME + name);
		}
	}
	return result;
}

bool BundledFileSystem::CanHandleFile(const string &path) {
	return StringUtil::StartsWith(path, SCHEME);
}

string BundledFileSystem::GetName() const {
	return "BundledFileSystem";
}

} // namespace duckdb
