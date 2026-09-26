# Testing

Every behavior change ships with tests in the same PR, and `pixi run check` runs all hermetic tests before a PR is
opened. This page describes the test layers, the ctest labels, the rules every test follows and the workflows for
SQL logic tests, differential tests, goldens, fixtures, coverage, fuzzing and the ClickBench data tests. The
strategy and the data policy are decided in [ADR 0006](adr/0006-test-strategy-and-data-policy.md); the harness
internals are described next to the code in [tests/README.md](../tests/README.md),
[tests/slt/README.md](../tests/slt/README.md) and [fuzz/regressions/README.md](../fuzz/regressions/README.md).

## Test layers

| Layer | Where | What it checks | Status |
| --- | --- | --- | --- |
| Unit tests (GoogleTest) | `src/<module>/tests/` | one module in isolation: lexer, parser, unparser, binder, types, Parquet table, operators, formatter, CLI | in use |
| Integration tests | `tests/integration/` | `engine::Session` end to end; corrupt, missing and mismatched Parquet inputs; globs | in use |
| SQL logic tests | `tests/slt/cases/` | `.slt` files with expected results written by DuckDB, run against antb1 | in use |
| Oracle tests | `tests/slt/cases/` | the same `.slt` files run against DuckDB, so every expectation stays DuckDB's answer | in use |
| Random differential tests | `tests/slt/runner/` | seeded generated queries, antb1 against DuckDB | in use |
| Metamorphic tests | `tests/metamorphic/` | relations between answers: split files, path vs name, batch sizes, predicate partitions | in use |
| CLI golden tests | `tests/cli/` | stdout, stderr and exit code of the `antb1` binary, including EXPLAIN output | in use |
| Harness self-tests | `tests/harness/`, `tests/slt/`, `tests/cli/`, `fuzz/`, `tools/fixturegen/` | a deliberately wrong engine is caught, redaction leaks nothing, fixtures match their digest | in use |
| Fuzzing | `fuzz/` | parser round trip `Parse(ToSql(ast)) == ast`, idempotent unparser, no crash or UB | in use |
| Coverage floors | `tools/ci/` | line and branch coverage of each module never drops below its floor | in use |
| ClickBench data tests | `tests/data/` | antb1 against DuckDB on the pinned `hits_0` partition, metamorphic relations on it and the ClickBench ratchet, with redacted output | in use |
| Benchmark smoke tests | `bench/` | every micro benchmark runs once (`pixi run release`), so they keep building and running | in use |

Build-level gates run on every PR as well: ASan and UBSan (`pixi run asan`), clang-tidy (`pixi run tidy`), the
coverage floors (`pixi run coverage`), a libFuzzer smoke run (`pixi run fuzz-smoke`), the GCC 15 compatibility
build (`pixi run ci-gcc`) and the data tests on `hits_0` (`pixi run test-data`). ThreadSanitizer (`pixi run tsan`),
shuffled test order (`pixi run ci-shuffle`), long fuzzing (`pixi run fuzz`), the data tests under ASan
(`pixi run asan-data`) and on ARM64, and a 20,000-query differential run are deep checks outside the PR gates: the
nightly workflow runs them ([ci.md](ci.md#nightly)).

## Labels

Every ctest test has exactly one label. The canonical list is the comment in `cmake/Antb1Testing.cmake`, and this
table must match it (`pixi run lint` compares them).

| Label | Status | Meaning |
| --- | --- | --- |
| `unit` | in use | module unit tests in `src/<module>/tests/*_test.cc`, named `<module>.<Suite>.<Case>` |
| `integration` | in use | cross-module gtest suites in `tests/integration/` (`integration.*`), including invalid Parquet inputs |
| `slt` | in use | sqllogictest files run against antb1 (`slt.<area>.<file>`) |
| `oracle` | in use | the same `.slt` files checked against DuckDB (`oracle.<area>.<file>`) |
| `diff` | in use | `diff.random`: seeded random differential queries against DuckDB (fixed seed, 300 queries) |
| `metamorphic` | in use | metamorphic relations on the generated fixtures (`metamorphic.*`) |
| `cli` | in use | golden tests of the `antb1` command line (`cli.<case>`): output formats, errors, exit codes, EXPLAIN |
| `harness` | in use | self-tests of the harness (`harness.*`): mutated engines, redaction canaries, fixture digest, runner unit tests |
| `fuzz-replay` | in use | `fuzz.replay.sql_parser`: the fuzz corpus and regressions replayed as an ordinary test, on every leg |
| `fuzz` | in use | `fuzz.sql_parser.smoke`: a short libFuzzer run with a fixed seed (Clang `fuzz` preset only) |
| `setup` | in use | `fixtures.generate`: writes the Parquet fixtures before any test that needs them |
| `data` | in use | ClickBench data tests (`data.*`, [below](#clickbench-data-tests)): downloaded data, always redacted; only the `data` and `asan-data` test presets run them |
| `bench-smoke` | in use | `bench.micro.smoke`: every micro benchmark once (presets with benchmarks: `ci-release`); numbers never gate |

The hermetic test presets (`dev`, `ci`, `ci-asan`, `ci-release`, `ci-gcc`, `coverage`, `tsan`, `ci-shuffle`) exclude
the `data` and `fuzz` labels. The `fuzz` test preset runs only `fuzz` and `fuzz-replay`.

## Running tests

- `pixi run test` builds the `dev` preset and runs its hermetic tests. It accepts these ctest arguments, and only
  these: `-R`, `-E`, `-L`, `-j`, `-N`, `-V`, `-VV`, `--rerun-failed`, `--output-on-failure`, `--timeout`, `--repeat`,
  `--schedule-random`, `--stop-on-failure` (`scripts/ctest.sh` rejects everything else).
- Examples: `pixi run test -R '^sql\.'` (one module), `pixi run test -L slt` (one label),
  `pixi run test -L '^(slt|oracle)$'` (two labels; repeated `-L` options must all match),
  `pixi run test -R '^cli\.CliTest\.JsonErrorObject$' -V` (one test), `pixi run test --rerun-failed`,
  `pixi run test -R '^io\.' --repeat until-fail:20` (hunt a flaky test).
- A filtered run still generates the fixtures first: every test that reads them requires the ctest fixture that
  `fixtures.generate` sets up.
- `pixi run ci`, `pixi run asan`, `pixi run ci-gcc` and `pixi run release` run the same tests in the CI
  configurations (`release` also builds the micro benchmarks and runs `bench.micro.smoke`). Each writes JUnit XML to
  `build/<preset>/junit.xml`. Benchmarks themselves are not tests: [benchmarks.md](benchmarks.md).
- `pixi run tsan` runs the hermetic tests under ThreadSanitizer (`build/tsan`); `pixi run ci-shuffle` runs the tests
  of the `ci` build in random order, each repeated until it fails (at most twice), and writes
  `build/ci/junit-shuffle.xml`.

## Hermetic rules

Every test except those labeled `data` must be hermetic:

- no network, no dependence on the machine (absolute paths, user, locale, time zone, wall-clock time) and no sleeps;
- files are written only under `::testing::TempDir()`, in a directory unique to the test, because ctest runs up to
  8 tests in parallel. The one exception is the shared Parquet fixtures, which `fixtures.generate` writes to
  `build/<preset>/fixtures` (see [Fixtures](#fixtures)); every other test only reads them;
- test data is generated by the test itself or by our fixture generator; nothing is downloaded;
- queries, tables and values are our own, never ClickBench data or query text;
- randomness uses fixed, printed seeds; the engine and DuckDB run single-threaded;
- the test presets set `OMP_NUM_THREADS=1`, `ARROW_IO_THREADS=2`, `LC_ALL=C`, `TZ=UTC` and `GTEST_COLOR=no`, and each
  gtest case has a 120-second timeout.

The sanitizer presets (`pixi run asan`, `pixi run fuzz-smoke`) additionally use `ARROW_DEFAULT_MEMORY_POOL=system`,
abort on the first ASan error and run UBSan in recoverable mode with `halt_on_error=1`, so every finding still fails
the test while the suppression files in `tools/sanitizers/` can work. The suppression files start empty; every entry
needs a comment that justifies it and a maintainer's agreement.

## Fixtures

`tools/fixturegen` (`antb1-fixturegen`) writes deterministic Parquet files with a hits-shaped schema and synthetic
values: `hits_like.parquet` (10,000 rows in 4 row groups), `hits_like_nulls.parquet` (the same rows with NULLs),
`hits_like_split/part-0.parquet` to `part-3.parquet` (the same rows in 4 files), `hits_like_required.parquet` (REQUIRED
columns and UTF8 strings), `edge.parquet` (type extremes, escapes, empty strings, NULLs), `floats.parquet` (bit-exact
FLOAT values: 0.1F and its lower neighbour, 2^24 and 2^100 with their next FLOATs, the FLOAT maximum, +-inf, +-0, NULL)
and `empty.parquet` (0 rows). The ctest `fixtures.generate` (label `setup`) writes them to `build/<preset>/fixtures`.
Values come from splitmix64 with integer-only arithmetic (no `<random>` distributions, no libm, no NaN), so every
platform generates the same data.

`harness.fixtures.digest` compares the generated files with `tests/fixtures/fixtures.digest`, a logical digest
(schema, row groups and values; not compression or page layout), on every leg, macOS included. After an intended
change of the generator, run the tests once (they regenerate the fixtures, and the digest test fails), rewrite the
digest, regenerate the `.slt` expectations and the CLI goldens, and review every diff:

```bash
pixi run test   # regenerates build/dev/fixtures; harness.fixtures.digest fails
build/dev/bin/antb1-fixture-digest --write tests/fixtures/fixtures.digest build/dev/fixtures
pixi run slt-complete
ANTB1_UPDATE_GOLDENS=1 pixi run test -L cli
```

## SQL logic tests

SQL behavior is tested with sqllogictest files in `tests/slt/cases/<area>/<file>.slt`, run by `antb1-slt`. Each file
becomes two ctest tests: `slt.<area>.<file>` checks antb1 and `oracle.<area>.<file>` checks DuckDB against the same
expectations. The file format, the tables (`tests/slt/tables.txt`) and the DuckDB lockdown are described in
[tests/slt/README.md](../tests/slt/README.md).

Workflow for new or changed SQL:

1. Add `statement ok`, `statement error` or `query <types> [nosort|rowsort|valuesort]` records to a file under
   `tests/slt/cases/`. Write only the SQL and leave out the expected block. A new file is picked up by the next build.
2. Run `pixi run slt-complete`. It regenerates the fixtures, runs every record on DuckDB and rewrites the expected
   blocks (and wrong column-type letters, printed as `NOTE`) of every registered `.slt` file. `ERROR` lines are
   records that could not be completed; `REVIEW` lines are `onlyif antb1` records, completed from antb1, which you
   must check by hand.
3. Review `git diff tests/slt`: only the records you meant to change may differ. Never edit an expected block by
   hand.
4. Run `pixi run test -L '^(slt|oracle)$'`.

An antb1 `Unsupported` answer always fails, also for `statement error`. A record for SQL that antb1 does not support
yet carries `onlyif duckdb`; the PR that implements the feature removes the guard and declares the feature in
`tests/slt/supported_features.h`, which also activates its random differential queries and metamorphic relations.
`onlyif antb1` is for antb1's own error texts and for registered divergences
([sql-subset.md](sql-subset.md#divergences-from-duckdb)).

## Random differential tests

`antb1-slt diff` generates queries over the tables of `tests/slt/tables.txt`, runs each on antb1 and on DuckDB and
compares the results and the exact column types. Query `i` of seed `s` depends only on `s`, `i`, the tables and the
supported features, so a single case reproduces on its own.

- ctest `diff.random` (label `diff`) runs 300 queries with a fixed seed on every leg.
- `pixi run diff-random` runs 2000 queries with a random seed, printed first. `ANTB1_DIFF_SEED` and
  `ANTB1_DIFF_COUNT` set the seed and the count. Extra arguments go to `antb1-slt diff`: `--list` prints the queries
  without running them, `--table NAME` restricts the tables, `--target-percent P` sets the share of queries that
  sample the full target grammar.
- Most queries use only the features declared in `tests/slt/supported_features.h`; for them an antb1 `Unsupported`
  answer is a failure. The rest sample the whole target grammar of [sql-subset.md](sql-subset.md#target-grammar):
  there an `Unsupported` answer is counted per missing feature and reported, not a failure.
- A failure prints the seed, the case index, the features, the SQL, at most 5 differing rows and the command that
  reproduces the case:

```bash
ANTB1_DIFF_SEED=<seed> ANTB1_DIFF_ONLY=<case> pixi run diff-random
ANTB1_DIFF_SEED=7 ANTB1_DIFF_COUNT=20000 pixi run diff-random   # a longer run
```

The reproduction holds while `tests/slt/supported_features.h`, the generator and the tables are unchanged. Once
the bug is fixed, add the query to an `.slt` file (with `pixi run slt-complete`) so that it stays covered.

## Metamorphic tests

`tests/metamorphic/` checks relations between antb1 answers that must hold whatever the data: `COUNT(*)` of a split
table equals the sum over its parts, a path reference equals the registered table, and batch sizes 1, 7 and 65536
give the same answers. A relation whose features are not all declared in `tests/slt/supported_features.h` is
pending: it is reported as skipped with the missing features, and once antb1 answers all its queries it fails until
the features are declared. Details: [tests/README.md](../tests/README.md#metamorphic-relations-metamorphic).

## CLI goldens

`tests/cli/` runs the `antb1` binary and compares its exit code, stdout and (where registered) stderr with the files
in `tests/cli/golden/`; fixture and source paths are normalized to `${FIXTURES}` and `${SOURCE}`. After an intended
change of the output, rewrite the golden files and review the diff:

```bash
ANTB1_UPDATE_GOLDENS=1 pixi run test -L cli
```

Exit codes are never rewritten: change `EXIT_CODE` in `tests/cli/CMakeLists.txt` deliberately.

## Harness self-tests

The `harness` label proves that the harness catches failures: every corruption of antb1's answers (`--mutate`) must
make the slt runner and the differential test fail; `--redact` output never contains SQL, values or error messages
(the canaries in `tests/slt/canary/`); the golden comparison catches changed output and exit codes; the fuzz replay
catches a broken unparser; the fixtures match their digest. A change to the harness comes with a self-test that
fails without it.

## Coverage

`pixi run coverage` (CI job `clang-coverage-fuzz`) builds the `coverage` preset (Clang source-based coverage, Debug,
`build/coverage`), runs every hermetic test and then `tools/ci/coverage.py`, which:

- merges the profiles from `build/coverage/prof/` and runs `llvm-cov` over the binaries in `build/coverage/bin`;
- counts only the code in `src/<module>/`: tests (`src/<module>/tests/` too), `fuzz/`, `bench/`, `tools/` and build
  trees do not count;
- writes `build/coverage/summary.md` (a per-module table, also used as the CI job summary and the PR comment),
  `build/coverage/report.txt` (per file) and `build/coverage/coverage.lcov` (for editors);
- fails when a module's line or branch coverage is below its floor in `tools/ci/coverage_thresholds.json`, or when a
  module has no floor. A module without branches (or lines) is N/A for that floor.

Floors only go up. Each floor is the measured value minus 2, rounded down to 0.1. A PR that raises a module's
coverage may raise its floors; a slice PR that extends a module sets its floors again from the new measurement.
Changing the thresholds file needs a maintainer's approval (AGENTS.md, "Ask a human first"). The long-term targets:

| Modules | Lines | Branches |
| --- | --- | --- |
| `common`, `sql`, `plan` | 90% | 75% |
| `io`, `exec`, `engine` | 85% | 70% |
| `cli` | 60% | 40% |

When the gate fails, `build/coverage/summary.md` names the module and the metric, and `build/coverage/report.txt`
shows the files. Add tests for the uncovered code instead of lowering a floor.

## Fuzzing

The fuzz target `antb1-sql-parser-fuzzer` checks the property in `fuzz/sql_parser_property.h` for every input: the
parser neither crashes nor triggers UB, and for every accepted query `Parse(ToSql(ast))` equals `ast` and `ToSql`
is idempotent. The seeds in `fuzz/corpus/sql_parser/` and the dictionary `fuzz/sql.dict` are our own.

| Run | Command | What it does |
| --- | --- | --- |
| Replay | `pixi run test` (every leg, GCC too) | `fuzz.replay.sql_parser` runs the seeds and `fuzz/regressions/` through the property, without libFuzzer |
| Smoke | `pixi run fuzz-smoke` (CI `clang-coverage-fuzz`) | the `fuzz` preset (Clang, ASan and UBSan, RelWithDebInfo, `-Werror`, only `common`, `sql` and `fuzz/`) runs `fuzz.sql_parser.smoke` (libFuzzer `-seed=1 -runs=200000`, deterministic) and the replay |
| Long | `pixi run fuzz` | libFuzzer for `ANTB1_FUZZ_SECONDS` (default 600) with `ANTB1_FUZZ_SEED` (default random, printed); extra arguments go to libFuzzer, e.g. `-jobs=8 -workers=8` |

The long run keeps its work corpus in `build/fuzz/corpus/sql_parser` between runs and never modifies the committed
seeds. Crash, leak and timeout inputs are written to `build/fuzz/artifacts/`; in CI a failed smoke run uploads them
as the `fuzz-artifacts` artifact for 14 days.

### Fuzz regressions

Every fuzzer finding becomes a regression test. Reproduce the saved input with the fuzzer binary
(`build/fuzz/bin/antb1-sql-parser-fuzzer`), minimize it, copy it as raw bytes to `fuzz/regressions/` under a name
like `<what-broke>[-<issue>]`, fix the bug and check the replay with `pixi run test -R fuzz.replay`. Commit the input
together with the fix. The exact commands are in [fuzz/regressions/README.md](../fuzz/regressions/README.md).

## ClickBench data tests

The data tests run antb1 on real ClickBench data: `hits_0.parquet`, the first of the 100 partitions of the `hits`
dataset (122 MB), and ClickBench's own query file. Both are pinned by sha256 and size in
`tools/data/clickbench.lock`, downloaded on demand and never committed ([data policy](#data-policy)).

```bash
pixi run fetch-data     # NETWORK: download the pinned files into ~/.cache/antb1/clickbench (kept once verified)
pixi run test-data      # fetch-data, then the ci-release build and the data tests -> build/ci-release/junit-data.xml
pixi run asan-data      # the same tests on the ASan build (nightly)
```

- `pixi run fetch-data` downloads with conda curl (retries, HTTPS only, no range requests) into a `.part` file,
  checks size and sha256 against the lock and renames the file atomically; files that already match are kept, so a
  second run needs no network. `ANTB1_DATA_DIR` moves the data directory (CI uses `.cache/clickbench` in the
  workspace; a directory inside the repository must be git-ignored).
- `pixi run test-data` runs `fetch-data` first, then the `data` workflow preset: the `ci-release` build and the
  `data` test preset (label `data`, 2 tests in parallel). The hermetic presets never run these tests, and
  `pixi run test` cannot select them.

| Test | What it checks |
| --- | --- |
| `data.hits0.verify` | the files of the lock are in the data directory with the pinned size and sha256 (`cmake/scripts/VerifyData.cmake`); the other data tests only run after it passed. On failure it says `run pixi run fetch-data` |
| `data.hits0.schema` | the hits files have the 105-column physical schema that `tools/fixturegen` models (`antb1-fixturegen --check-schema`) |
| `data.hits0.slice` | `tests/data/hits0_slice.sql`, our own queries over the hits columns, on antb1 and DuckDB (`antb1-slt queries`) |
| `data.hits0.metamorphic` | the relations of `tests/data/hits_relations.cc` on the hits data, and COUNT(*) against the rows the Parquet library decodes (`antb1-data-metamorphic`) |
| `data.clickbench.status` | ClickBench's queries on antb1 against DuckDB and the ratchet `tests/data/clickbench_status.json` (`antb1-slt clickbench`) |

### Redaction

Every data test runs with `--redact` (`pixi run lint` rejects a data test without it), so neither a value nor
ClickBench query text reaches a log, a JUnit report or a CI annotation. A failure prints only the query id
(`hits0_slice.sql:<line>` or `Q<n>`), the features, the column types, the error kind, the row counts, the first
differing row and the sha256 of each engine's canonical result, plus the unredacted command. Run that command
locally after `pixi run test-data` to see the SQL and the values:

```text
FAIL Q0: result mismatch: values differ
  rows: DuckDB 1, antb1 1; first differing row: 0
  sha256: DuckDB <64 hex digits>
          antb1  <64 hex digits>
  repro (unredacted, prints values and query text; run it locally):
    build/ci-release/bin/antb1-slt clickbench ... --only 0
```

Sanitizer reports never reach the log either, because a UBSan report prints operand values: under
`pixi run asan-data` they go to files in `build/ci-asan/data-tmp/<test>/sanitizers/`, and the test prints only
their `SUMMARY:` lines (the kind of error and the source location). The harness self-tests `harness.queries.*` and
`harness.clickbench.*` prove on the fixtures that corrupted answers are caught and that redacted reports print no
SQL and no value.

### Queries of our own: `tests/data/hits0_slice.sql`

Each query ends with `;` and follows a `-- features:` line that lists the SQL features it uses, with the names of
`tests/slt/supported_features.h` (the format is in `tests/slt/runner/query_file.h`). DuckDB runs every query. A
query whose features are all declared supported must give DuckDB's answer. The others are pending: antb1 must
answer Unsupported or reject the query with a parse or bind error (an I/O or execution error fails the test), and
once antb1 answers one, the test fails until the slice PR declares its features. Write new queries yourself; never
copy ClickBench's.

### The ClickBench ratchet

`data.clickbench.status` runs every query of ClickBench's `queries.sql` (pinned at ClickBench commit
`5a56398c975bfd9f328f544894bcb92533ed134c`; `Q<n>` is the 0-based statement number) on antb1 with EventDate read as
DATE. DuckDB runs only the queries antb1 answers. A query passes when antb1 answers and the answer equals DuckDB's.
Every other query must fail cleanly: Unsupported, or a parse or bind error. The test fails on a wrong answer, on
an unclean failure (an I/O, execution or internal error) and on any difference from the ratchet
`tests/data/clickbench_status.json`: `{"clickbench_commit": "<commit>", "pass": [<n>, ...]}`.

The PR that changes the pass set updates the ratchet and the status table in
[sql-subset.md](sql-subset.md#clickbench-status) in the same PR (`pixi run lint` checks that they agree and that
the commit matches the lock). An unexpected pass is good news: add the query to both. An unexpected fail is a
regression to fix. The ratchet is `[0]` today.

### Full dataset (host only)

```bash
pixi run fetch-data --full    # all 100 partitions (about 14.7 GB) into ~/.cache/antb1/clickbench/full
ANTB1_HITS_FILES="$HOME/.cache/antb1/clickbench/full/hits_*.parquet" pixi run test-data
```

`--full` checks the free space first. `hits_0` is verified against the pin; the other partitions are
trust-on-first-use: their sha256 is recorded in `full/SHA256SUMS` on the first download (outside the repository)
and checked on every later run. `ANTB1_HITS_FILES` takes a file, a glob in the file name or a comma-separated list;
`data.hits0.verify` then skips the pinned `hits_0` with a warning. The published full-dataset answers are compared
by hand with the CLI, never committed.

## Writing tests

- Put unit tests in `src/<module>/tests/<topic>_test.cc` and add the file to `antb1_add_module_tests(...)` in
  that module's `CMakeLists.txt`. Extra libraries go under `LIBS`: modules reachable through the allow-list (for
  example `antb1::plan` in the `cli` tests) and libraries that build test data (for example `Parquet::parquet_shared`).
  Tests of `common` and `sql` stay Arrow-free.
- Cross-module suites are registered in `tests/CMakeLists.txt` with `antb1_add_gtest(... LABEL <label> ...)`, or with
  `antb1_add_fixture_gtest(...)` when they read the fixtures (`cmake/Antb1Testing.cmake`).
- SQL behavior goes into `.slt` files ([SQL logic tests](#sql-logic-tests)); command-line behavior into CLI goldens.
- A bug fix comes with a test that fails without the fix.
- Test the error path too: the error kind, the source span and, for the CLI, the exit code.
- Never use ClickBench-derived data or query text in a test.

## Failure output

`pixi run test` prints the output of every failing test, then a reproduction hint and a final status line:

```text
Reproduce: pixi run test --rerun-failed   |   one test: pixi run test -R '^<name>$' -V
ANTB1-TESTS: FAIL preset=dev junit=build/dev/junit.xml
```

The last line is always `ANTB1-TESTS: PASS ...` or `ANTB1-TESTS: FAIL ...`, which is easy for humans and agents to
check. A failing `.slt` record prints the file and line, the SQL, the expected and actual blocks and a repro command;
a failing differential case prints its seed and case index. In CI, failing tests appear as annotations on the PR and
in the job summary (from the JUnit file), and the ctest logs are uploaded as the `logs-<job>` artifact for 7 days.

## Reproducing CI

Every CI job name contains the command that reproduces it, for example `clang-asan (pixi run asan)`.
[ci.md](ci.md) has the full map. `pixi run check-full` runs every Linux PR gate locally: `check` (lint and the Clang
Debug build with tests), `asan`, `tidy`, `coverage`, `fuzz-smoke` and `ci-gcc`. The data tests run separately:
`pixi run test-data` (CI job `clickbench-hits0`).

## Data policy

Nothing derived from ClickBench is ever committed: no Parquet files or samples, no query text and no result values.
No file larger than 1 MiB is committed either. `pixi run lint` rejects data files (`*.parquet`, `*.arrow`,
`*.feather`, `*.csv.gz`, `queries.sql`) and any other file over 1 MiB except `pixi.lock`; authors and reviewers
check the rest. Fixtures, `.slt` queries, golden files and fuzz seeds are our own. The data tests download the pinned
`hits` partition and query file into a cache outside the repository, redact their output (query ids, column types,
row counts and hashes only) and never upload data or logs as CI artifacts; committed are only pins, our own
queries and the pass/fail ratchet. The full policy is
[ADR 0006](adr/0006-test-strategy-and-data-policy.md).
