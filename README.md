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

## Layout

- `src/` — the extension itself, plus the bundled-corpus file system and its checked-in selection.
- `duckdb/` — pinned submodule. The extension compiles the runner straight out of its test tree
  (`test/sqlite`, `test/helpers`), which is not a public API — hence the pin.
- `scripts/generate_bundled_sqllogictests.py` — embeds the corpus at build time, reading the test
  content from the submodule so it cannot drift from the tests themselves.
