# Fuzz regressions

Minimized inputs that once violated the SQL parser property (`fuzz/sql_parser_property.h`): a crash, a sanitizer
report, or a failed round trip `Parse(ToSql(ast))`. Every build replays them, GCC included: the ctest
`fuzz.replay.sql_parser` (label `fuzz-replay`) runs every file here and in `fuzz/corpus/sql_parser` through the same
property. `README.md` and dotfiles are skipped.

## Adding a regression

1. Reproduce the input that libFuzzer saved in `build/fuzz/artifacts/` (`pixi run fuzz-smoke`, `pixi run fuzz`, or the
   `fuzz-artifacts` upload of a failed CI run):

   ```bash
   build/fuzz/bin/antb1-sql-parser-fuzzer build/fuzz/artifacts/crash-<sha1>
   ```

2. Minimize it:

   ```bash
   build/fuzz/bin/antb1-sql-parser-fuzzer -minimize_crash=1 -runs=100000 \
     -exact_artifact_path=build/fuzz/artifacts/minimized build/fuzz/artifacts/crash-<sha1>
   ```

3. Copy the minimized file here under a descriptive name, `<what-broke>[-<issue>]`, for example
   `unterminated-comment-at-eof-123`. The content is raw bytes: never reformat it (`fuzz/.gitattributes` turns off
   line-ending normalization).
4. Fix the bug in `src/sql` (or `src/common`), then check that the replay passes:

   ```bash
   pixi run test -R fuzz.replay
   ```

5. Commit the input together with the fix.

## Data policy

Inputs here come only from fuzzing our own seeds (`fuzz/corpus/sql_parser`). Never add ClickBench query text or
data, or anything else derived from it.
