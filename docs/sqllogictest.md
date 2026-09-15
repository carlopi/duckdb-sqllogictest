# sqllogictest extension

Runs DuckDB's own sqllogictests (`.test` files) from SQL, so a DuckDB build can be tested
without the `unittest` binary:

```sql
-- from the duckdb root, with a build that produced the extension:
--   build/reldebug/duckdb -unsigned
LOAD 'build/reldebug/extension/sqllogictest/sqllogictest.duckdb_extension';

FROM run_test('test/sql/types/test_typeof.test');
FROM run_tests('test/sql/types/**');
```

Both functions take a path; `run_tests` also accepts a glob or a directory (a directory means
everything below it) and only picks up `.test`, `.test_slow` and `.test_coverage` files. The two
names behave identically otherwise.

| column | meaning |
| --- | --- |
| `name` | path of the test file |
| `status` | `SUCCESS`, `FAIL`, `SKIPPED` or `UNSUPPORTED` |
| `message` | failure text with its `file:line` locator, or the skip reason |
| `statements_passed` / `statements_failed` / `statements_skipped` | statement tallies |
| `assertions` | result comparisons performed |
| `duration_ms` | wall-clock time of the test |

`SKIPPED` means the test was deliberately excluded (a skip list or a tag selection).
`UNSUPPORTED` means the environment cannot run it, i.e. a `require` / `require-env` that is not
satisfied.

Named parameters:

- `temp_directory` — everything a run writes goes under it: the databases tests create,
  `{TEST_DIR}`, spilled data, installed extensions. Defaults to `./test_tmp`, and the run empties
  what it created. Deliberately not the database's own temp directory: for an in-memory database
  that is `./.tmp`, which DuckDB keeps reachable with `enable_external_access` off, so a run living
  inside it would expose its HOME to the tests and the external-access tests would stop seeing the
  refusals they assert on.
- `home_directory` — where `INSTALL` and secrets land, keeping them out of the caller's
  `~/.duckdb`. Follows `temp_directory` unless set.
- `workers` — how many test files to run at once. Defaults to 1.

There is no working-directory parameter: tests read their data relative to wherever you started
DuckDB (`data/csv/...`), so run from the tree the test paths are written against. A run never
changes the process's working directory. The bundled corpus reads nothing relative, so it does not
care where you run it from.

The failure detail (the expected/actual diff) is printed to stderr exactly as the `unittest`
binary prints it; the returned row carries the summary.

## The corpus that ships with the extension

A build's own test tree is not on a machine that just installed a binary, so a corpus of tests is
compiled into the extension and served under `sqllogic://`:

```sql
LOAD sqllogictest;
FROM run_tests('sqllogic://duckdb/');            -- the whole corpus
FROM run_test('sqllogic://duckdb/types/test_typeof.test');
SELECT * FROM glob('sqllogic://**');             -- what is in there
```

The first path segment names the provider, so another extension can bundle its own corpus without
colliding with this one; a trailing separator reads as a directory and stands for an implied `**`.
It is an ordinary read-only file system, registered on the database the extension is loaded into
and on none of the databases the tests themselves run on — those are meant to be an unmodified
DuckDB.

The selection is every test in `test/sql` that passes with the process started outside the tree,
i.e. one that reads nothing but itself. To refresh it after tests are added or removed, sweep from
a directory that is not the duckdb root and keep what passed:

```sql
COPY (SELECT replace(name, '<duckdb root>/', '') FROM run_tests('<duckdb root>/test/sql/**/*.test')
      WHERE status = 'SUCCESS' ORDER BY 1) TO 'bundled_tests.list' (HEADER false);
```

then put the two header comments back on top of
`test/extension/sqllogictest/bundled_tests.list`. The list is checked in so the selection reads as
a diff; the content is read from the tree at build time, so it cannot drift from the tests.

## Running tests in parallel

```sql
SET preserve_insertion_order = false;   -- see below
FROM run_tests('test/sql/**/*.test', workers := 8);
```

On a 10-core machine the full fast suite (3747 files) goes from 96s to 26s at 8 workers. The whole
suite including `.test_slow` (4476 files) runs clean at 8 workers too.

Two things to know. **A consumer that preserves order makes the scan serial** — `COPY (FROM
run_tests(...)) TO 'x.csv'` runs single-threaded until you `SET preserve_insertion_order=false`,
because a table function here cannot hand the ordered path a batch index. Add an `ORDER BY name`
if you want a stable order back. And **each worker's tests bring their own DuckDB instance with
its own thread pool**, so `workers := N` oversubscribes the machine the same way `test/run
--workers` does; past the core count it stops helping.

**Tests that need a deep stack are skipped in a parallel run.** A parallel run executes tests on
DuckDB's task threads, which get the platform default stack — 512KB on macOS, against the main
thread's 8MB — while `max_expression_depth` (1000 by default) is calibrated for the latter. A test
that deliberately recurses that far, like
`test/sql/catalog/function/test_recursive_macro.test`, would overflow the stack and take the
process down. Such tests declare `require deep_stack`, which is satisfied everywhere except on a
parallel run's worker threads, so they come back `UNSUPPORTED` instead. `workers := 1` runs them
normally.

Three tests need it today: the two recursive-macro tests (binder recursion) and
`test/sql/parser/giant_query.test_slow` (parser recursion). The mechanism is opt-in, so a test that
recurses deeply *without* declaring `require deep_stack` still crashes a parallel run rather than
skipping — each one has to be found the hard way once.

The `workers` a test sees is the value you asked for, not the value capped to the number of files,
so re-running one file with `workers := 2` reproduces what it did inside a big parallel run instead
of quietly running it on the main thread.

## Remote test files

A path the file system considers remote is fetched through the *calling* connection's file system —
whatever that database has registered, httpfs included — and run as a stream:

```sql
LOAD httpfs;
FROM run_test('https://example.com/tests/foo.test');
```

The databases the test itself runs on are local and need no such extension; only the caller does.
One gap: an `include` directive inside a remote test file still resolves locally.

## How it runs tests

The tests do not run on the calling connection: the runner creates the DuckDB instances they use,
the same way it does inside the `unittest` binary. `restart`, `load`, multiple databases and
multiple connections therefore behave as they normally do — the calling connection only drives the
run.

## Limitations

- **No crash isolation.** A test that segfaults or trips an assertion takes down the process it is
  running in, along with the results of every test that had not been returned yet. The `unittest`
  binary has the same property; what protects a full-suite run there is `test/run` sharding tests
  across processes.
- **Process-global state.** `HOME` belongs to the process, so two unrelated `run_tests` queries
  cannot run at once — the second fails with *A test run is already in progress in this process*.
  Workers within one run are fine, and so is a test that calls `run_test` itself (the meta test
  does). Pointing the inner databases at an extension and secret directory instead would remove
  the last of this.
- **Concurrent extension loading is not supported.** An extension's init function runs once per
  `DatabaseInstance`, so a parallel run can execute two `<ext>_duckdb_cpp_init` calls at the same
  time in one process, and whatever the extension keeps in file-scope statics (httpfs's connection
  caches, say) is shared by every test running at that moment. DuckDB tracks loaded extensions per
  instance, not per process, so nothing serializes this. Tests that `require` an extension are only
  safe at `workers := 1`, which is the default. Nothing has been observed to fail here — but nothing
  has exercised it either, since a default build links no extensions and every such test skips.
- **Built for one DuckDB build.** The extension uses DuckDB's internal C++ API, so it has to be
  loaded into the build it was compiled against.
- **Not built on Windows**, where the internal symbols it needs are not exported from the DLL.
