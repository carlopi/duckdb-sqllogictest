#define DUCKDB_EXTENSION_MAIN

#include "duckdb.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/virtual_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "bundled_file_system.hpp"
#include "sqllogic_test_runner.hpp"
#include "test_config.hpp"
#include "test_helpers.hpp"
#include "test_reporter.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>

namespace duckdb {

namespace {

//! One test file's verdict.
struct TestOutcome {
	string name;
	string status;
	string message;
	idx_t statements_passed = 0;
	idx_t statements_failed = 0;
	idx_t statements_skipped = 0;
	idx_t assertions = 0;
	double duration_ms = 0;
};

//! Reporter for a run driven by run_test/run_tests: failures unwind as TestFailureException (the
//! default behavior), and the test name is served from here because the per-test temp dir
//! ({TEST_DIR}, TEST_ID) is derived from it.
class RunTestsReporter : public TestReporter {
public:
	string CurrentTestName() override {
		return test_name;
	}

	string test_name;
};

//! Set while this thread is inside a test, so that a test which itself calls run_test is recognized
//! as nesting instead of as a competing run.
thread_local bool thread_runs_test = false;

//! Owns the process-global state a run touches: HOME and the temp-directory bookkeeping. One run owns them at a time - a second, unrelated run_tests query cannot be
//! interleaved into meaning, so it is refused. A run nested inside a test is not a competing run and
//! borrows the state of the run it sits in.
class RunOwnership {
public:
	explicit RunOwnership(void *owner_p) : nested(thread_runs_test), owner(owner_p) {
		if (nested) {
			return;
		}
		lock_guard<mutex> guard(lock);
		if (active_owner && active_owner != owner) {
			throw InvalidInputException("A test run is already in progress in this process");
		}
		active_owner = owner;
	}
	~RunOwnership() {
		if (nested) {
			return;
		}
		lock_guard<mutex> guard(lock);
		active_owner = nullptr;
	}

private:
	static mutex lock;
	static void *active_owner;

private:
	bool nested;
	void *owner;
};

mutex RunOwnership::lock;
void *RunOwnership::active_owner = nullptr;

//! Marks the calling thread as executing a test for as long as it is in scope.
class ThreadRunsTestScope {
public:
	ThreadRunsTestScope() : previous(thread_runs_test) {
		thread_runs_test = true;
	}
	~ThreadRunsTestScope() {
		thread_runs_test = previous;
	}

private:
	bool previous;
};

bool test_environment_initialized = false;

//! One-time process setup, mirroring what unittest.cpp's main does before a Catch session starts.
//! Materializes the run's temp tree under the directory the caller named, anchored to an absolute
//! path so that nothing downstream has to resolve it again.
void PrepareTempDirectory(const string &directory) {
	auto local_fs = FileSystem::CreateLocal();
	auto root = directory;
	if (!FileSystem::IsRemoteFile(root) && !local_fs->IsPathAbsolute(root)) {
		root = local_fs->JoinPath(TestGetCurrentDirectory(), root);
	}
	SetTempDirRoot(root);
	string error;
	if (!PrepareTempDir(error)) {
		throw IOException("Failed to prepare the temp directory '%s': %s", root, error);
	}
	TestConfiguration::Get().UpdateEnvironment();
}

void InitializeTestEnvironment() {
	if (test_environment_initialized) {
		return;
	}
	TestConfiguration::Get().Initialize();
	TestConfiguration::Get().UpdateEnvironment();
	test_environment_initialized = true;
}

//! Points HOME at a directory for the duration of the run, restoring the previous value after.
//! Tests INSTALL extensions and write secrets, which land in ~/.duckdb - so the default is a
//! throwaway home under the run's temp dir, and writing into a real one has to be asked for.
class HomeDirectory {
public:
	explicit HomeDirectory(const string &directory) {
		auto previous_home = std::getenv(HOME_VARIABLE);
		had_previous_home = previous_home != nullptr;
		if (had_previous_home) {
			previous_value = previous_home;
		}
		auto home_dir = directory.empty() ? GetTempDirHome() : directory;
		auto local_fs = FileSystem::CreateLocal();
		if (!local_fs->IsPathAbsolute(home_dir)) {
			home_dir = local_fs->JoinPath(TestGetCurrentDirectory(), home_dir);
		}
		SetHome(home_dir.c_str());
	}
	~HomeDirectory() {
		if (had_previous_home) {
			SetHome(previous_value.c_str());
		} else {
			UnsetHome();
		}
	}

private:
#ifdef DUCKDB_WINDOWS
	static constexpr const char *HOME_VARIABLE = "USERPROFILE";
	static void SetHome(const char *value) {
		_putenv_s(HOME_VARIABLE, value);
	}
	static void UnsetHome() {
		_putenv_s(HOME_VARIABLE, "");
	}
#else
	static constexpr const char *HOME_VARIABLE = "HOME";
	static void SetHome(const char *value) {
		setenv(HOME_VARIABLE, value, 1);
	}
	static void UnsetHome() {
		unsetenv(HOME_VARIABLE);
	}
#endif

private:
	bool had_previous_home = false;
	string previous_value;
};

//! The directory a test was written to run from: the nearest ancestor holding a data/ directory.
//! Empty when there is none, in which case nothing is suggested.
string InferWorkingDirectory(const string &test_path) {
	auto local_fs = FileSystem::CreateLocal();
	auto directory = test_path;
	for (idx_t depth = 0; depth < 16; depth++) {
		auto parent = local_fs->JoinPath(directory, "..");
		if (!local_fs->DirectoryExists(parent)) {
			break;
		}
		directory = parent;
		if (local_fs->DirectoryExists(local_fs->JoinPath(directory, "data"))) {
			return directory;
		}
	}
	return string();
}

//! Tests that assert on state a run here necessarily configures, so they cannot pass under it and
//! are not defects. Deliberately tiny: more than a couple of entries would mean this diverges from
//! the unittest binary structurally, which is worth fixing rather than listing.
struct SkippedTest {
	const char *path_suffix;
	const char *reason;
};

constexpr SkippedTest SKIPPED_TESTS[] = {
    {"test/sql/extensions/allowed_directories_install.test",
     "asserts on which directories extensions may be installed into, which a run redirects in order "
     "to keep installs out of the caller's home"},
    {"test/sql/sqllogictest.test",
     "tests this extension itself, by running fixtures that fail on purpose - useful in CI, but "
     "noise when it is swept up in a run over the tree"},
};

//! The reason this test is skipped here, or empty when it is not.
string SkipReason(const string &path) {
	auto normalized = StringUtil::Replace(path, "\\", "/");
	for (auto &skipped : SKIPPED_TESTS) {
		if (StringUtil::EndsWith(normalized, skipped.path_suffix)) {
			return skipped.reason;
		}
	}
	return string();
}

//! Why this test cannot share the process with others, or empty when it can.
string ParallelUnsafeReason(ClientContext &context, const string &path);

//! Runs one .test file. A verdict for the test comes back as the returned outcome; anything the
//! setup around the run itself throws is turned into an outcome by RunTestFileSafe below.
struct RunTestsBindData : public TableFunctionData {
	vector<string> files;
	//! required: everything the run writes goes under it
	string temp_directory;
	//! empty: follows temp_directory
	string home_directory;
	idx_t workers = 1;
};

//! Local filesystem that only permits writes below the directories the run was given. Reads are
//! governed separately, by the allow-list on the config - this is the half DuckDB has no knob for.
//! With no writable directories, which is the default, a test cannot leave anything on the machine.
TestOutcome RunTestFile(ClientContext &context, const RunTestsBindData &options, const string &path) {
	TestOutcome outcome;
	outcome.name = path;

	auto skip_reason = SkipReason(path);
	if (!skip_reason.empty()) {
		outcome.status = "SKIPPED";
		outcome.message = skip_reason;
		return outcome;
	}

	if (options.workers > 1) {
		auto unsafe = ParallelUnsafeReason(context, path);
		if (!unsafe.empty()) {
			outcome.status = "UNSUPPORTED";
			outcome.message = unsafe;
			return outcome;
		}
	}

	RunTestsReporter reporter;
	reporter.test_name = path;
	TestReporterScope reporter_scope(reporter);
	ThreadRunsTestScope thread_scope;

	auto start = std::chrono::high_resolution_clock::now();
	auto &test_config = TestConfiguration::Get();

	// materialize this test's temp dir ({TEST_DIR} / TEMP_DIR), as the Catch wrapper does. With no
	// temp directory it still gets a path - one that is never created and that the sandbox refuses,
	// so a test needing it is refused outright. An empty path would silently resolve to something
	// usable and the test would pass having verified nothing.
	string test_dir_path = TestDirectoryPath();
	string test_dir_absolute = test_dir_path;
	{
		auto local_fs = FileSystem::CreateLocal();
		if (!FileSystem::IsRemoteFile(test_dir_absolute) && !local_fs->IsPathAbsolute(test_dir_absolute)) {
			test_dir_absolute = local_fs->JoinPath(TestGetCurrentDirectory(), test_dir_absolute);
		}
	}

	string initial_dbpath = test_config.GetInitialDBPath();
	test_config.ProcessPath(initial_dbpath, path);
	SQLLogicTestRunner runner(std::move(initial_dbpath));
	// hand the databases this test creates the same extensions the calling binary links, so that
	// `require <ext>` answers what the binary under test can actually do
	runner.config->linked_extensions = DBConfig::GetConfig(*context.db).linked_extensions;
	// Nothing here sets temp_directory or home_directory on the database. The runner already puts
	// spilled data under TestDirectoryPath(), which is inside temp_directory - and setting either as
	// a DuckDB option changes what the engine considers an allowed path, which tests that assert on
	// permissions can see. Extensions and secrets are contained by HOME instead, as the unittest
	// binary does, for the same reason.


	for (auto &kv : test_config.GetTestEnvMap()) {
		runner.environment_variables[kv.first] = kv.second;
	}
	runner.environment_variables["WORKING_DIR"] = TestGetCurrentDirectory();
	runner.environment_variables["TEST_NAME"] = path;
	runner.environment_variables["TEST_NAME__NO_SLASH"] = StringUtil::Replace(path, "/", "_");
	runner.environment_variables["TEST_ID"] = TestNameToId(path);
	runner.environment_variables["TEMP_DIR"] = test_dir_path;
	runner.environment_variables["TEMP_DIR_ABSOLUTE"] = test_dir_absolute;
	runner.environment_variables["CATALOG_DIR"] = test_dir_path + "/" + runner.environment_variables["TEST_UUID"];

	try {
		// Anything the parser's ifstream cannot reach - a remote path, or the bundled corpus - is
		// fetched through the calling database's file system and run as a stream.
		if (FileSystem::IsRemoteFile(path) || StringUtil::StartsWith(path, BundledFileSystem::SCHEME)) {
			// the parser reads local files with an ifstream, which cannot reach a remote one - fetch it
			// through the calling database's file system (that is where httpfs and friends live) and run
			// it as a stream, the same way the runner consumes stdin
			auto &fs = FileSystem::GetFileSystem(context);
			auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
			auto file_size = NumericCast<idx_t>(handle->GetFileSize());
			string contents(file_size, '\0');
			handle->Read(const_cast<char *>(contents.data()), file_size); // NOLINT: writing into the buffer
			std::istringstream stream(contents);
			runner.ExecuteStream(stream, path);
		} else {
			runner.ExecuteFile(path);
		}
		outcome.status = "SUCCESS";
	} catch (TestFailureException &failure) {
		outcome.status = "FAIL";
		outcome.message = failure.message;
		if (!failure.file.empty()) {
			auto locator = StringUtil::Format("%s:%llu", failure.file, failure.line);
			outcome.message = outcome.message.empty() ? locator : locator + ": " + outcome.message;
		}
	} catch (std::exception &ex) {
		outcome.status = "FAIL";
		outcome.message = ErrorData(ex).Message();
	}
	// A refused write means this environment cannot run the test - the same disposition as an unmet
	// `require`, not a defect in the test.
	// Say what the test could not find. Not a claim that the miss caused the failure - a test may
	// probe for a file it expects to be absent - but it is the first thing worth checking.
	// Say why it failed, not just where. And when the reason is a file the test reads that is not
	// there, the environment did not give the test what it needs - the same disposition as an unmet
	// require, and the first thing to check is where the run was told to read from.
	if (outcome.status == "FAIL" && !runner.last_error_message.empty()) {
		outcome.message += ": " + runner.last_error_message;
		if (StringUtil::Contains(runner.last_error_message, "No files found that match the pattern") ||
		    StringUtil::Contains(runner.last_error_message, "No such file or directory")) {
			outcome.status = "UNSUPPORTED";
			auto suggestion = InferWorkingDirectory(path);
			if (!suggestion.empty()) {
				outcome.message += StringUtil::Format(" [run from '%s']", suggestion);
			}
		}
	}
	// a test that opted out (require / require-env / a skip list) never reaches its assertions
	if (runner.test_skipped_requirement) {
		outcome.status = runner.test_skip_kind == TestSkipKind::REQUIREMENT ? "UNSUPPORTED" : "SKIPPED";
		outcome.message = runner.test_skip_reason;
	}

	auto on_cleanup = test_config.OnCleanupCommand();
	if (!on_cleanup.empty() && runner.db) {
		try {
			if (!runner.con) {
				runner.Reconnect();
			}
			auto res = runner.con->Query(on_cleanup);
			if (res->HasError()) {
				res->GetErrorObject().Throw();
			}
		} catch (std::exception &ex) {
			outcome.status = "FAIL";
			outcome.message = "Error while running clean-up routine:\n" + ErrorData(ex).Message();
		}
	}

	runner.test_succeeded = outcome.status != "FAIL";
	DestroyTestTempDir(runner.test_succeeded);

	outcome.statements_passed = runner.test_stat_passes;
	outcome.statements_failed = runner.test_stat_fails;
	outcome.statements_skipped = runner.test_stat_skip_mode;
	outcome.assertions = reporter.assertion_count;
	auto end = std::chrono::high_resolution_clock::now();
	outcome.duration_ms = std::chrono::duration<double, std::milli>(end - start).count();

	return outcome;
}

//! Runs one .test file, guaranteeing exactly one outcome per file: a test can only ever fail its
//! own row, never the query that is running the suite.
TestOutcome RunTestFileSafe(ClientContext &context, const RunTestsBindData &options, const string &path) {
	try {
		return RunTestFile(context, options, path);
	} catch (TestFailureException &failure) {
		TestOutcome outcome;
		outcome.name = path;
		outcome.status = "FAIL";
		outcome.message = failure.message;
		return outcome;
	} catch (std::exception &ex) {
		TestOutcome outcome;
		outcome.name = path;
		outcome.status = "FAIL";
		outcome.message = ErrorData(ex).Message();
		return outcome;
	} catch (...) {
		TestOutcome outcome;
		outcome.name = path;
		outcome.status = "FAIL";
		outcome.message = "Unknown exception";
		return outcome;
	}
}

//! Tests that recurse deep enough to need a main-thread-sized stack. A parallel run executes tests
//! on DuckDB's task threads, which get the platform default (512KB on macOS), so these would
//! overflow it and take the process down with them. Held here rather than as a `require` in the
//! test files, so running against a stock DuckDB needs no change to its test tree.
constexpr const char *DEEP_STACK_TESTS[] = {
    "test/sql/catalog/function/test_recursive_macro.test",
    "test/sql/catalog/function/test_recursive_macro_no_dependency.test",
    "test/sql/parser/giant_query.test_slow",
};

//! Reads a test file through the calling database's file system, which reaches local paths, remote
//! ones and the bundled corpus alike.
string ReadTestFile(ClientContext &context, const string &path) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = NumericCast<idx_t>(handle->GetFileSize());
	string contents(file_size, '\0');
	handle->Read(const_cast<char *>(contents.data()), file_size); // NOLINT: writing into the buffer
	return contents;
}

//! Whether the file declares `require tpch`. The TPC-H generator loads its distributions into
//! globals, so two dbgen calls in one process corrupt each other - see load_dists in
//! extension/tpch/dbgen/dbgen_gunk.cpp. Directives sit at column 0, so a prefix test is enough.
bool RequiresTpch(const string &contents) {
	for (auto &raw : StringUtil::Split(contents, '\n')) {
		auto line = StringUtil::Replace(raw, "\r", "");
		if (line == "require tpch" || StringUtil::StartsWith(line, "require tpch ")) {
			return true;
		}
	}
	return false;
}

//! Why this test cannot share the process with others, or empty when it can.
string ParallelUnsafeReason(ClientContext &context, const string &path) {
	auto normalized = StringUtil::Replace(path, "\\", "/");
	for (auto &suffix : DEEP_STACK_TESTS) {
		if (StringUtil::EndsWith(normalized, suffix)) {
			return "recurses deeper than a worker thread's stack allows - run it with workers := 1";
		}
	}
	try {
		if (RequiresTpch(ReadTestFile(context, path))) {
			return "require tpch, whose generator keeps global state - run it with workers := 1";
		}
	} catch (...) {
		// unreadable here is the runner's problem to report, with its own message
	}
	return string();
}

bool IsTestFile(const string &path) {
	return StringUtil::EndsWith(path, ".test") || StringUtil::EndsWith(path, ".test_slow") ||
	       StringUtil::EndsWith(path, ".test_coverage");
}


unique_ptr<FunctionData> RunTestsBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<RunTestsBindData>();
	for (auto &kv : input.named_parameters) {
		// NULL reads as "not specified" for every parameter, so no null can reach the code below
		if (kv.second.IsNull()) {
			continue;
		}
		if (kv.first == "temp_directory") {
			result->temp_directory = StringValue::Get(kv.second);
		} else if (kv.first == "home_directory") {
			result->home_directory = StringValue::Get(kv.second);
		} else if (kv.first == "workers") {
			auto workers = kv.second.GetValue<int64_t>();
			if (workers < 1) {
				throw BinderException("workers must be at least 1");
			}
			result->workers = NumericCast<idx_t>(workers);
		} else {
			throw BinderException("Unknown named parameter '%s' for the sqllogictest", kv.first.GetIdentifierName());
		}
	}

	// Everything a run writes - databases, {TEST_DIR}, spilled data, installed extensions - goes
	// under this, so the caller decides what the run is allowed to touch. Required rather than
	// defaulted: a default would put those writes somewhere nobody chose.
	if (result->temp_directory.empty()) {
		// Not the database's own temp directory, which for an in-memory one is ./.tmp: DuckDB keeps
		// that reachable with enable_external_access off, so a run living inside it would hand the
		// tests access to its HOME and the external-access tests would stop seeing their refusals.
		result->temp_directory = "./test_tmp";
	}
	// extensions and secrets live with the temp files unless told otherwise
	if (result->home_directory.empty()) {
		result->home_directory = result->temp_directory;
	}

	auto pattern = StringValue::Get(input.inputs[0]);
	auto &fs = FileSystem::GetFileSystem(context);
	if (StringUtil::StartsWith(pattern, BundledFileSystem::SCHEME)) {
		// The bundled corpus: a bare scheme means all of it, otherwise it globs like any path.
		for (auto &file : fs.GlobFiles(pattern, FileGlobOptions::ALLOW_EMPTY)) {
			result->files.push_back(file.path);
		}
		std::sort(result->files.begin(), result->files.end());
	} else if (FileSystem::HasGlob(pattern) || fs.DirectoryExists(pattern)) {
		// a directory is shorthand for everything below it
		auto glob = fs.DirectoryExists(pattern) ? fs.JoinPath(pattern, "**/*") : pattern;
		for (auto &file : fs.GlobFiles(glob, FileGlobOptions::ALLOW_EMPTY)) {
			if (IsTestFile(file.path)) {
				result->files.push_back(file.path);
			}
		}
		std::sort(result->files.begin(), result->files.end());
	} else {
		if (!fs.FileExists(pattern)) {
			throw IOException("Test file '%s' does not exist", pattern);
		}
		result->files.push_back(pattern);
	}

	// Absolute, resolved here against the caller's working directory: the run moves to a directory
	// of its own before opening any of them, so a relative path would stop resolving.
	{
		auto local_fs = FileSystem::CreateLocal();
		auto here = TestGetCurrentDirectory();
		for (auto &file : result->files) {
			if (StringUtil::StartsWith(file, BundledFileSystem::SCHEME)) {
				continue;
			}
			if (!FileSystem::IsRemoteFile(file) && !local_fs->IsPathAbsolute(file)) {
				file = local_fs->JoinPath(here, file);
			}
		}
	}

	names = {"name",     "status",           "message",           "statements_passed",
	         "statements_failed", "statements_skipped", "assertions", "duration_ms"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::UBIGINT,
	                LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::UBIGINT, LogicalType::DOUBLE};
	return std::move(result);
}

//! Holds the run's claim on the process-global state for as long as the query lives, and hands out
//! the test files. Set up once, on the thread that starts the scan.
struct RunTestsGlobalState : public GlobalTableFunctionState {

	RunTestsGlobalState(const RunTestsBindData &bind_data)
	    : ownership(this), home_directory(bind_data.home_directory),
	      workers(MinValue<idx_t>(bind_data.workers, MaxValue<idx_t>(bind_data.files.size(), 1))) {
	}

	idx_t MaxThreads() const override {
		return workers;
	}

	//! Index of the next test file to run, or files.size() when there is nothing left.
	idx_t NextTest(const RunTestsBindData &bind_data) {
		auto next = offset++;
		return MinValue<idx_t>(next, bind_data.files.size());
	}

	RunOwnership ownership;
	HomeDirectory home_directory;
	idx_t workers;
	atomic<idx_t> offset {0};
};

unique_ptr<GlobalTableFunctionState> RunTestsInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RunTestsBindData>();
	InitializeTestEnvironment();
	PrepareTempDirectory(bind_data.temp_directory);
	return make_uniq<RunTestsGlobalState>(bind_data);
}

void RunTestsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind_data = data_p.bind_data->Cast<RunTestsBindData>();
	auto &state = data_p.global_state->Cast<RunTestsGlobalState>();

	// one test per call: a test file is a big unit of work, and handing them out one at a time is
	// what keeps the workers evenly loaded
	auto next = state.NextTest(bind_data);
	if (next >= bind_data.files.size()) {
		return;
	}
	auto outcome = RunTestFileSafe(context, bind_data, bind_data.files[next]);

	output.data[0].Append(Value(outcome.name));
	output.data[1].Append(Value(outcome.status));
	output.data[2].Append(outcome.message.empty() ? Value(LogicalType::VARCHAR) : Value(outcome.message));
	output.data[3].Append(Value::UBIGINT(outcome.statements_passed));
	output.data[4].Append(Value::UBIGINT(outcome.statements_failed));
	output.data[5].Append(Value::UBIGINT(outcome.statements_skipped));
	output.data[6].Append(Value::UBIGINT(outcome.assertions));
	output.data[7].Append(Value::DOUBLE(outcome.duration_ms));
}

void LoadInternal(ExtensionLoader &loader) {
	// The bundled corpus, visible to whoever loaded this and to nothing else - the databases a test
	// runs on are meant to be an unmodified DuckDB.
	auto &fs = loader.GetDatabaseInstance().GetFileSystem();
	fs.RegisterSubSystem(make_uniq<BundledFileSystem>());
	for (auto &name : {"run_test", "run_tests"}) {
		TableFunction run_tests(name, {LogicalType::VARCHAR}, RunTestsFunction, RunTestsBind, RunTestsInit);
		run_tests.named_parameters["temp_directory"] = LogicalType::VARCHAR;
		run_tests.named_parameters["home_directory"] = LogicalType::VARCHAR;
		run_tests.named_parameters["workers"] = LogicalType::BIGINT;
		loader.RegisterFunction(run_tests);
	}
}

} // namespace

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(sqllogictest, loader) { // NOLINT
	duckdb::LoadInternal(loader);
}
}
