# sqllogictest extension

Runs DuckDB's own sqllogictests (`.test` files) from SQL, as a loadable extension, so a machine
that only has a DuckDB binary can exercise the engine against its own environment.

```sql
LOAD sqllogictest;
FROM run_tests('sqllogic://duckdb/');            -- the corpus that ships with the extension
FROM run_test('test/sql/types/test_typeof.test'); -- or a path, if you have a test tree
```

See `docs/sqllogictest.md` for the full reference: result columns, named parameters
(`temp_directory`, `home_directory`, `workers`), the bundled corpus, and the limitations.

## Building and running

```bash
git submodule update --init --recursive
GEN=ninja make release      # build/release/duckdb, build/release/test/unittest, and the extension
make test                   # the extension's own test, test/sql/sqllogictest.test
```

The artifact is `build/release/extension/sqllogictest/sqllogictest.duckdb_extension`. It is
unsigned and stamped with the pinned DuckDB commit, so only a binary built from that commit loads
it, such as the CLI the build produces:

```bash
build/release/duckdb -unsigned
```

```sql
LOAD 'build/release/extension/sqllogictest/sqllogictest.duckdb_extension';
FROM run_tests('sqllogic://duckdb/', workers := 4);
```

The bundled corpus is every test that passes through this artifact with no test tree around, from
a directory outside any DuckDB checkout. Tests that `require` an extension the binary does not
have report `UNSUPPORTED` rather than failing.

## Layout

- `src/` — the extension itself, plus the bundled-corpus file system and its checked-in selection.
- `duckdb/` — pinned submodule. The extension compiles the runner straight out of its test tree
  (`test/sqlite`, `test/helpers`), which is not a public API — hence the pin.
- `scripts/generate_bundled_sqllogictests.py` — embeds the corpus at build time, reading the test
  content from the submodule so it cannot drift from the tests themselves.
