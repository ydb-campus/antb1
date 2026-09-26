# Cross-module tests

Module unit tests live in `src/<module>/tests/` (label `unit`). This directory holds the suites that span
modules. Every suite reads the Parquet fixtures of `tools/fixturegen`: the ctest `fixtures.generate` (label
`setup`) writes them to `build/<preset>/fixtures` before any test that needs them, also in filtered runs.

| Directory | ctest names | Label | What it checks |
|---|---|---|---|
| `slt/` | `slt.<area>.<file>`, `oracle.<area>.<file>` | `slt`, `oracle` | sqllogictest files on antb1 and DuckDB |
| `slt/` | `diff.random` | `diff` | generated queries, antb1 vs DuckDB (fixed seed) |
| `metamorphic/` | `metamorphic.*` | `metamorphic` | relations between antb1 answers |
| `integration/` | `integration.*` | `integration` | `engine::Session` end to end; bad Parquet inputs; globs |
| `cli/` | `cli.<case>` | `cli` | stdout, stderr and exit code of the `antb1` binary |
| `slt/`, `harness/`, `cli/` | `harness.*` | `harness` | the harness itself: mutations, redaction, digest |
| `data/` | `data.*` | `data` | ClickBench data tests on downloaded data, redacted (`pixi run test-data` only) |

```bash
pixi run test -L diff          # one label
pixi run diff-random           # 2000 generated queries, random seed (tests/slt/README.md)
pixi run slt-complete          # rewrite .slt expectations from DuckDB, then review the diff
```

Tests use our own tables, columns and queries only: never ClickBench data or ClickBench query text. The one
exception is `data/` ([docs/testing.md](../docs/testing.md#clickbench-data-tests)): it reads the ClickBench files
that `pixi run fetch-data` downloads (never committed) and runs with `--redact`, so no value or query text reaches
a log. Its own queries (`data/hits0_slice.sql`) and relations (`data/hits_relations.cc`) are ours.

## What antb1 supports: `slt/supported_features.h`

`kSupportedFeatures` declares the SQL features antb1 answers today: the whole slice grammar of
`docs/sql-subset.md` (global aggregates, projections, `WHERE` conjunctions, `LIMIT`) over every column type, in any
case, quoting and layout. The random differential test generates queries from it, and the metamorphic relations are
active or pending by it. The PR that implements a new feature adds it there; the tests then demand correct answers
for it.

## Metamorphic relations (`metamorphic/`)

`relations.cc` lists relations: queries whose answers must relate whatever the data, such as COUNT(*) over
the split table equal to the sum over its parts, a path reference equal to the registered table, and the
same answers for batch sizes 1, 7 and 65536. Each relation declares the features its queries use:

- all declared in `kSupportedFeatures`: active, the relation must hold;
- otherwise pending: at least one query must still be answered Unsupported, and the test is reported as
  skipped with the missing features. When antb1 answers every query, the test fails until the features are
  declared, which activates the relation.

The relations cover TLP-lite partitions of WHERE (`<`/`>=`, `=`/`<>`, NULLs through `COUNT(col)`), SUM, MIN and
MAX over partitions and over split files, literal folding (a decimal bound equals its integer bound, out-of-range
bounds), AND symmetry, literal-first comparisons, LIMIT, projections and batch sizes; all of them are active. Add a
relation with `r.push_back({...})` in `AllRelations()`; the checks (`AllEqual`, `FirstEqualsSumOfRest`,
`FirstEqualsMinOfRest`, `FirstEqualsMaxOfRest`, `RowCountsEqualFirst`, `RowCountsAreMinOf`) are in `relations.h`.
`metamorphic.RowCount.MatchesAnIndependentParquetScan` compares COUNT(*) with the rows the Parquet library decodes.

## CLI goldens (`cli/`)

`cli/CMakeLists.txt` registers each case with `antb1_add_cli_golden()` (`cmake/Antb1Testing.cmake`), run by
`cmake/scripts/CompareOutput.cmake`:

- the exit code must equal `EXIT_CODE` (0 ok, 1 query error, 2 usage, 3 I/O, 4 unsupported);
- stdout must equal `cli/golden/<case>.stdout` (no file: empty), or match `STDOUT_REGEX`;
- with `STDERR`, stderr must equal `cli/golden/<case>.stderr`; with `STDERR_REGEX` it must match.

Outputs are normalized first: the fixtures directory reads `${FIXTURES}`, the source tree `${SOURCE}`. After an
intended change of the output, rewrite the golden files and review the diff:

```bash
ANTB1_UPDATE_GOLDENS=1 pixi run test -L cli
```

Exit codes are never rewritten: change `EXIT_CODE` in `cli/CMakeLists.txt` deliberately.

## Harness self-tests (label `harness`)

- `harness.slt.mutate.<kind>` and `harness.diff.mutate.<kind>`: corrupting antb1's answers (`--mutate`) must
  make the slt runner and the differential test fail, with the expected report.
- `harness.slt.redact`, `harness.slt.redact_sentinels`, `harness.diff.redact`: with `--redact` a failure
  report never prints SQL, values, error messages or regexes; the sentinel files are in `slt/canary/`.
- `harness.cli.changed_stdout`, `harness.cli.changed_exit_code`: the golden comparison catches changes.
- `harness.fixtures.digest`: the fixtures this build generated match `fixtures/fixtures.digest`, a logical
  digest (schema, row groups, values; not compression or page layout), so Linux and macOS generate the same
  data. After an intended generator change:

  ```bash
  build/dev/bin/antb1-fixture-digest --write tests/fixtures/fixtures.digest build/dev/fixtures
  ```

- `harness.<Suite>.<Case>`: unit tests of the runner, comparator, query generator, differential test,
  fixture generator and digest.
