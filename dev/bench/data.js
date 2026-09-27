window.BENCHMARK_DATA = {
  "lastUpdate": 1790547310416,
  "repoUrl": "https://github.com/ydb-campus/antb1",
  "entries": {
    "antb1 micro benchmarks": [
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "224e50c7e8ce3add0149fe1e54f876dbfd9c2478",
          "message": "feat(exec,engine,cli): operators, exact aggregates and benchmarks (#7)\n\n## Summary\n\nCompletes the thin vertical slice.\n\n- Pull-based operators (scan, filter, project, scalar aggregate, limit,\nrow count); integer SUM exact in 128 bits,\nexact AVG, MIN/MAX for numbers, dates and byte-wise VARCHAR; DuckDB\nNULL/empty semantics.\n- `antb1 bench` (ClickBench-format JSON), Google Benchmark micro\nbenchmarks, `bench.yml` (never gating).\n- Harness: full supported feature set in the differential generator, slt\nsuites per area, active metamorphic\n  relations, coverage floors raised to the plan targets.\n- **ClickBench ratchet → Q0, Q1, Q2, Q3, Q6 (+ Q19 incidentally)**,\nverified against DuckDB 1.5.5 on `hits_0`; the\n  other 37 queries fail cleanly with exit code 4.\n\n### Fixes after an independent review\n\nThe automatic Claude review found no problems. A second, independent\nreview split #7 into six areas: aggregates, operators, CLI/engine/bench,\nplan/io, tests and policy, and black-box testing of about 7,000 queries\nagainst DuckDB. It found four real defects, each reproduced with the\nbuilt CLI and confirmed by two verifiers. They are fixed in separate\ncommits, and each fix has a regression test that fails without it:\n\n- **MIN/MAX and NaN** (`fix(exec)`): a batch whose selected values were\nall NaN left MIN/MAX stuck at NaN, so the answer depended on batch\nboundaries and file order. MIN/MAX now ignore NaN whatever the split, as\nD10 documents. They return NaN only when every value is NaN.\n- **Results with a VARCHAR column over 2 GiB** (`fix(engine)`): the\nformatter indexed only the first chunk after `CombineChunks`, which\nArrow splits for binary data over 2 GiB. Rows past it were read out of\nbounds and came out empty (exit 0) or crashed (exit 70). Output is now\nformatted chunk by chunk and matches DuckDB byte for byte on a 2.3 GiB\nresult.\n- **Ill-formed UTF-8 in JSON** (`fix(engine)`, `fix(cli)`): overlong\nforms, surrogates and code points above U+10FFFF passed through raw, so\n`--format json`, the JSON error object and the bench report could be\ninvalid UTF-8. One RFC 3629 escaper is now used for all three. It was\nchecked exhaustively against a reference over 4.7M byte sequences.\n- **WHERE on FLOAT columns** (`fix(engine)`, `docs`): antb1 compares\nFLOAT columns in double precision, while DuckDB rounds an integer or\nDECIMAL literal to FLOAT. Matching DuckDB needs the binder to know the\nstorage type, a design change left for later. For now the difference is\nregistered as D11, with ADR 0004's wording adjusted, and pinned by\n`engine.SessionTest.FloatColumnsCompareInDoublePrecision`. The random\ngenerator keeps FLOAT columns out.\n\nA second review of the fix commits (per commit plus a cross-cutting\npass, with differential runs against DuckDB) found no P0/P1 problems.\nTwo P2 follow-ups remain for later PRs:\n- one shared UTF-8 validator in `common`, replacing the copies in\n`sql/lexer.cc`, `engine/format.cc` and the harness;\n- chunk-safe indexing in the harness's `ToResultSet`.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check-full            # lint, ci, asan, tidy, coverage (floors), fuzz-smoke, ci-gcc\n100% tests passed out of 880     (every leg; exit 0)\n$ pixi run test-data             # data.clickbench.status: pass = [0, 1, 2, 3, 6, 19]\n100% tests passed out of 6\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random   # seeds 7, 42, 20260925 (before the fixes)\n0 failures\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change\n- [x] Docs updated where behavior, commands or architecture changed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) planned, wrote and verified this change with\nparallel implementer and adversarial reviewer agents; a human directed\nthe design decisions.\n- Accountable human (has read and understands the whole diff): @Hor911\n(please confirm before merging)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 (1M context) <noreply@anthropic.com>",
          "timestamp": "2026-09-26T10:54:01+03:00",
          "tree_id": "95228a252b8fc83726cc33cf87b8acadf457f62a",
          "url": "https://github.com/ydb-campus/antb1/commit/224e50c7e8ce3add0149fe1e54f876dbfd9c2478"
        },
        "date": 1790409421590,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2934.205897335242,
            "unit": "ns/iter",
            "extra": "iterations: 239498\ncpu: 2933.1054497323566 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84250.1465419144,
            "unit": "ns/iter",
            "extra": "iterations: 7909\ncpu: 84240.72246807437 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222435.8777777799,
            "unit": "ns/iter",
            "extra": "iterations: 3150\ncpu: 222426.84634920643 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 443207.09734513285,
            "unit": "ns/iter",
            "extra": "iterations: 1582\ncpu: 443055.080278129 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 405067.9126891733,
            "unit": "ns/iter",
            "extra": "iterations: 1718\ncpu: 405005.65075669385 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2134999.62580647,
            "unit": "ns/iter",
            "extra": "iterations: 310\ncpu: 2134755.4870967744 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "6d5651c7227cb93d1d27a34c4986c5b181c912cd",
          "message": "test(harness): read antb1 results chunk by chunk (#14)\n\n## Summary\n\nA follow-up from #7. It fixed the output formatter's handling of\nmulti-chunk columns, and the test harness still had the same pattern.\n`ToResultSet` in `tests/slt/runner/antb1_engine.cc` converts antb1's\nresults for the slt, oracle, random differential and ClickBench data\nrunners. It called `CombineChunks()` and then read every row from\n`chunk(0)`. Arrow keeps a binary column larger than 2 GiB in several\nchunks even after `CombineChunks`, so rows past the first chunk would be\nread out of bounds. No current test gets near that size, but a future\nharness query projecting a large VARCHAR column would.\n\n- `ToResultSet` now walks each column chunk by chunk with a running row\nindex. It returns `Invalid` for a column whose length differs from the\ntable's, instead of reading past it. It is declared in `antb1_engine.h`\nso it can be unit tested.\n- The session test's `Rows` helper gets the same chunk walk.\n- New `harness.ToResultSet.*` tests:\n- the same rows for a multi-chunk layout (empty and sliced chunks, NULLs\nand values in later chunks) as for one chunk;\n  - an empty result with no chunks;\n  - a column-length mismatch;\n  - a column-count mismatch.\n\nAs with #7's formatter test, small data cannot force Arrow's 2 GiB\nsplit, so the multi-chunk test guards the chunk walk itself.\n- `tests/integration/integration_util.h` is unchanged: it reads only the\nsingle row of a `COUNT(*)`.\n\n## Type of change\n\n- [x] refactor, test, docs, build, ci or chore\n\n## Verification\n\n```text\n$ pixi run test -R 'ToResultSet|SessionTest|^slt\\.|^oracle\\.'\n100% tests passed out of 53; ANTB1-TESTS: PASS\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 884 (ci, asan, coverage, ci-gcc); tidy clean; fuzz-smoke 2/2\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change\n- [x] Docs updated where behavior, commands or architecture changed, or\nnot needed (harness-internal)\n- [x] No ClickBench-derived data is committed\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the change and tests.\n- Accountable human (has read and understands the whole diff): @Hor911\n(please confirm before merging)",
          "timestamp": "2026-09-26T14:03:39+03:00",
          "tree_id": "f898c37f7bd1d84cd9859735209a1cbb54d2be89",
          "url": "https://github.com/ydb-campus/antb1/commit/6d5651c7227cb93d1d27a34c4986c5b181c912cd"
        },
        "date": 1790420704598,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2340.3953164966565,
            "unit": "ns/iter",
            "extra": "iterations: 299861\ncpu: 2340.2953335045236 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 75546.52747869023,
            "unit": "ns/iter",
            "extra": "iterations: 8916\ncpu: 75536.01615074025 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 84913.93644837142,
            "unit": "ns/iter",
            "extra": "iterations: 8261\ncpu: 84904.40527781137 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 380001.08933405107,
            "unit": "ns/iter",
            "extra": "iterations: 1847\ncpu: 379958.81321061193 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 352812.24845995865,
            "unit": "ns/iter",
            "extra": "iterations: 1948\ncpu: 352777.72176591365 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2203162.4748428278,
            "unit": "ns/iter",
            "extra": "iterations: 318\ncpu: 2202797.333333332 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "20c2963d3ed73e4e2a63adc718763ae16dd60259",
          "message": "refactor(common): share one utf-8 validator (#15)\n\n## Summary\n\nA follow-up from #7. Three hand-written copies of the RFC 3629 / Unicode\nTable 3-7 sequence check existed:\n- `Utf8Length` in `src/sql/lexer.cc`, for non-ASCII identifier bytes;\n- `Utf8SequenceLength` in `src/engine/format.cc`, for JSON escaping;\n- `Utf8SequenceLength` in `tests/slt/runner/canonical.cc`, for the\nharness's canonical text.\n\nBefore #7, the engine copy had drifted: it lacked the overlong,\nsurrogate and above-U+10FFFF checks. That was the JSON bug #7 fixed.\nThis PR keeps a single copy.\n\n- New public header `src/common/include/antb1/common/utf8.h`:\n`antb1::Utf8SequenceLength(std::string_view s, std::size_t i)` returns 2\nto 4 for a well-formed sequence starting at `s[i]`, and 0 for ASCII or\nfor ill-formed or truncated bytes. It requires `i < s.size()`. The\ndefinition is in `src/common/utf8.cc` and is Arrow-free.\n- The three callers use it and keep their own handling of ASCII and of\nill-formed bytes, so behavior is unchanged. Their existing tests pass\nunmodified: `sql.ParserRobustnessTest.EmbeddedNulAndInvalidUtf8`,\n`sql.Syntax/RejectTest.*/InvalidUtf8`, `engine.FormatResultTest.Json*`,\n`cli.CliTest.JsonErrorObjectEscapesIllFormedUtf8` and\n`harness.SltCell.KeepsValidUtf8AndEscapesInvalidBytes`.\n- No new module edge: `sql` and `engine` already depend on `common`, and\nthe harness links through `engine`. So there is no ADR, and no \"Ask a\nhuman first\" path is touched.\n- `docs/architecture.md` lists the helper under `common`.\n\nNew `common.Utf8.*` tests:\n- the well-formed boundaries (U+0080 … U+10FFFF), also in the middle of\na string;\n- every ill-formed class: C0, C1 and F5..FF leads, lone continuation\nbytes, overlong forms, surrogates, above U+10FFFF, bad second, third and\nfourth bytes, and truncation;\n- a comparison with a reference decoder (bit-pattern decode, then a code\npoint check) over every 1- and 2-byte string with a non-ASCII lead,\nevery 3-byte string with an E0..FF lead, and 4-byte strings with an\nF0..FF lead and boundary fourth bytes. It takes 1.9 s in Debug and 8 s\nunder ASan.\n\nA mutation check (widening the F4 second-byte range) makes two of the\nthree new tests fail.\n\n## Type of change\n\n- [x] refactor, test, docs, build, ci or chore\n\n## Verification\n\n```text\n$ pixi run test -R 'Utf8|Parser|Lexer|FormatResult|SltCell|CliTest'\n100% tests passed out of 80; ANTB1-TESTS: PASS\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 887 (ci, asan, coverage, ci-gcc); tidy clean; fuzz-smoke 2/2\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change\n- [x] Docs updated where behavior, commands or architecture changed\n- [x] No ClickBench-derived data is committed\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the change after a plan the maintainer approved.\n- Accountable human (has read and understands the whole diff): @Hor911\n(please confirm before merging)",
          "timestamp": "2026-09-26T15:51:07+03:00",
          "tree_id": "338aa028170637e91e1606f7d3c7d687e14be27b",
          "url": "https://github.com/ydb-campus/antb1/commit/20c2963d3ed73e4e2a63adc718763ae16dd60259"
        },
        "date": 1790427143649,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2351.6942999140915,
            "unit": "ns/iter",
            "extra": "iterations: 300206\ncpu: 2351.1004343684003 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84202.2072673326,
            "unit": "ns/iter",
            "extra": "iterations: 8091\ncpu: 84175.22432332223 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 91144.86237933746,
            "unit": "ns/iter",
            "extra": "iterations: 7666\ncpu: 91118.94677798064 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 293816.2785445376,
            "unit": "ns/iter",
            "extra": "iterations: 2391\ncpu: 293743.0727728983 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 382450.40260444925,
            "unit": "ns/iter",
            "extra": "iterations: 1843\ncpu: 382367.17905588687 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2177514.8650305993,
            "unit": "ns/iter",
            "extra": "iterations: 326\ncpu: 2176810.2944785273 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "037cf8b3dad3024133f8d815a0a730d7d443de5a",
          "message": "fix(plan): compare float columns like duckdb (#16)\n\n## Summary\n\nA follow-up from #7, where this was registered as divergence D11. A\nParquet FLOAT column is widened to DOUBLE on read, and `WHERE` compared\nit with the literal's nearest *double*. DuckDB casts an integer or\nDECIMAL literal to *FLOAT* and compares in FLOAT. So `WHERE price =\n19.99` returned the stored `19.99F` rows in DuckDB and **no rows** in\nantb1, and range filters moved their boundary. It affected most decimal\nliterals (0.1, 19.99, 3.3, …) and integers above 2^24.\n\nAs agreed, antb1 now mirrors DuckDB exactly, including its rounding\nquirks, rather than rounding correctly:\n\n- **`plan::Table::StoredAsFloat(field)`** (public header, default\n`false`): `io::ParquetTable` answers it from its storage schema. The\ncolumn stays `LogicalType::kDouble` everywhere else, and there is no new\nmodule edge.\n- **`plan::DuckDbFloatOf(text, negative)`** (public header): the FLOAT\nthat DuckDB 1.5.5 casts a literal to, or `nullopt` when DuckDB types it\nas DOUBLE.\n- Typing: an integer by value (INTEGER/BIGINT/HUGEINT/UHUGEINT; outside\n-2^127 … 2^128-1 it is DOUBLE). A decimal is DECIMAL(all digits\nincluding leading zeros, fraction digits); above 38 digits, or with an\nexponent, it is DOUBLE.\n  - Casts, ported from DuckDB's source:\n- `TryCastDecimalToFloatingPoint`: `float(unscaled) / float(1e<scale>)`\non the fast path (int16 storage, scale 0, or |unscaled| ≤ 2^24),\notherwise `float(div) + float(mod) / float(1e<scale>)`;\n- `Hugeint::TryCast` to float through `CastBigintToFloating<double>`,\nincluding its `upper == -1` case;\n    - UHUGEINT through a double;\n    - `static_cast` for INTEGER/BIGINT.\n- So `16777217.5`, `0.1` written with 16 or 24 decimals, and HUGEINT\nliterals get DuckDB's FLOAT, which is not always the nearest one.\nLiterals beyond the FLOAT range become ±inf.\n- **Binder:** for a DOUBLE column stored as FLOAT, the constant becomes\nthat FLOAT, widened. Widening is exact and preserves order, so the\nexisting double comparison selects DuckDB's rows, and nothing changes in\n`exec`. DOUBLE-typed literals and DOUBLE-stored columns behave as\nbefore.\n- **Docs:** the Binding and Types tables in `docs/sql-subset.md`, ADR\n0004 (wording only, status unchanged), `docs/testing.md` (the new\nfixture) and the harness comments. D11 now covers only the remaining\ndifference: FLOAT *results* print as DOUBLE, where DuckDB keeps FLOAT.\nSo the random generator still never references FLOAT columns.\n\n## Tests\n\n- `plan.DuckDbFloatOf.MatchesDuckDbCasts`: 48 literals across every\ntyping class and path boundary. The expected values were generated with\nDuckDB 1.5.5 and are compared bit for bit, so signed zero counts too.\n`DoubleLiteralsAreNotConverted` covers the DOUBLE-typed forms.\n- `harness.FloatLiteralOracle.MatchesDuckDbOnRandomLiterals`: 20,000\nseeded random literals checked against the DuckDB library at test time,\nboth the FLOAT value and whether DuckDB types the literal as DOUBLE. The\nliterals include random integers and decimals and mutated edges near\n2^24, 2^63, 2^64, 2^127, 2^128 and the FLOAT maximum. It takes 1.6 s,\nand is compiled only when DuckDB is available, like `diff.random`.\n- A new fixture, `floats.parquet` (bit-exact FLOAT values), with\n`tests/slt/cases/where/float.slt`: 40 records whose expectations\n`slt-complete` wrote from DuckDB. They run as `slt.*` on antb1 and\n`oracle.*` on DuckDB. The fixture digest gains one line; the existing\nfixtures are unchanged.\n- `engine.SessionTest.FloatColumnsCompareLikeDuckDb` (renamed): DuckDB's\nanswers for a FLOAT column; a DOUBLE column with the same values still\nuses the nearest double; results stay DOUBLE.\n- Mutation check: forcing the DECIMAL fast path everywhere makes the\nunit table, the oracle test and `slt.where.float` fail.\n- The `reviewer` agent ran its own 450-literal end-to-end comparison\nwith 0 differences. Its two docs findings (an overstated \"16 or more\ndecimals\" and the fixture list in `docs/testing.md`) are fixed.\n\n## Type of change\n\n- [x] fix: wrong results or a crash\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 893 (ci, asan, coverage, ci-gcc); tidy clean; fuzz-smoke 2/2\n$ pixi run diff-random\nDIFF: PASS seed=1470416822 queries=2000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change\n- [x] Docs updated where behavior, commands or architecture changed\n- [x] No ClickBench-derived data is committed\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the change after a plan the maintainer approved (the \"mirror\nDuckDB\" option), ported DuckDB's cast code from its v1.5.5 source, and\nran a reviewer agent on the diff.\n- Accountable human (has read and understands the whole diff): @Hor911\n(please confirm before merging)",
          "timestamp": "2026-09-26T17:14:28+03:00",
          "tree_id": "2ddcac3541e40befa480709c13028ad9cbc75459",
          "url": "https://github.com/ydb-campus/antb1/commit/037cf8b3dad3024133f8d815a0a730d7d443de5a"
        },
        "date": 1790432162044,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2650.1040640510273,
            "unit": "ns/iter",
            "extra": "iterations: 264539\ncpu: 2649.8015302091558 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 87182.51640624786,
            "unit": "ns/iter",
            "extra": "iterations: 7680\ncpu: 87174.34062500001 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 94359.8889934178,
            "unit": "ns/iter",
            "extra": "iterations: 7441\ncpu: 94354.94476548853 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 368811.87651077216,
            "unit": "ns/iter",
            "extra": "iterations: 1903\ncpu: 368739.06095638446 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 381695.80995716644,
            "unit": "ns/iter",
            "extra": "iterations: 1868\ncpu: 381682.9823340471 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2599388.7910447232,
            "unit": "ns/iter",
            "extra": "iterations: 268\ncpu: 2598412.9104477647 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "334225516+antb1-bot[bot]@users.noreply.github.com",
            "name": "antb1-bot[bot]",
            "username": "antb1-bot[bot]"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "e2f1ff3ed8ff7298a8ca478da3fb1fcbb3aca5db",
          "message": "build(deps): update pixi.lock (#18)\n\n# Explicit dependencies\n\n|Dependency|Before|After|Change|Environments|\n|-|-|-|-|-|\n\n|[typos](https://prefix.dev/channels/conda-forge/packages/typos)|1.50.2|1.50.3|Patch\nUpgrade|lint on *all platforms*|\n\n# Implicit dependencies\n\n|Dependency|Before|After|Change|Environments|\n|-|-|-|-|-|\n\n|[libexpat](https://prefix.dev/channels/conda-forge/packages/libexpat)|2.8.4|2.8.5|Patch\nUpgrade|{default, lint} on *all platforms*<br/>gcc on linux-64|\n\n|[platformdirs](https://prefix.dev/channels/conda-forge/packages/platformdirs)|4.11.12|4.11.15|Patch\nUpgrade|lint on *all platforms*|\n\n|[virtualenv](https://prefix.dev/channels/conda-forge/packages/virtualenv)|21.12.0|21.12.1|Patch\nUpgrade|lint on *all platforms*|\n\n[^1]: **Bold** means explicit dependency.\n[^2]: Dependency got downgraded.\n\n\nGenerated by `.github/workflows/pixi-lock-update.yml` with pixi 0.81.0.\nCI runs as for any PR;\na maintainer reviews the diff, approves and squash-merges.\n\nCo-authored-by: antb1-bot[bot] <334225516+antb1-bot[bot]@users.noreply.github.com>",
          "timestamp": "2026-09-26T18:42:04+03:00",
          "tree_id": "8f017c27e796b1f96c59f16b986540b610aba35f",
          "url": "https://github.com/ydb-campus/antb1/commit/e2f1ff3ed8ff7298a8ca478da3fb1fcbb3aca5db"
        },
        "date": 1790437420279,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2645.0324073024985,
            "unit": "ns/iter",
            "extra": "iterations: 264786\ncpu: 2644.472000030213 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85532.01135586324,
            "unit": "ns/iter",
            "extra": "iterations: 7309\ncpu: 85506.58270625258 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 93140.62470087809,
            "unit": "ns/iter",
            "extra": "iterations: 7522\ncpu: 93122.37702738633 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 359225.5684966653,
            "unit": "ns/iter",
            "extra": "iterations: 1949\ncpu: 359109.0354027707 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 368200.42773644783,
            "unit": "ns/iter",
            "extra": "iterations: 1882\ncpu: 368173.85972369835 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2551290.4285714044,
            "unit": "ns/iter",
            "extra": "iterations: 266\ncpu: 2550117.77067669 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "048b4b4e9ebbd65e723b8bea87efe07e37374cb3",
          "message": "feat(sql): parse group by, order by, offset and count distinct (#19)\n\n## Summary\n\nPR 1 of the approved GROUP BY / ORDER BY / COUNT(DISTINCT) plan (next:\nhash aggregation, then ORDER BY with top-N, then COUNT(DISTINCT)). This\nPR adds the **syntax only**. No query that ran before behaves\ndifferently, and none of the new constructs is answered yet.\n\n- **Parser** (`src/sql`) accepts:\n  - `GROUP BY col, …`;\n- `ORDER BY item, …`, where an item is a column or an aggregate call\nwith optional `ASC`/`DESC` and `NULLS FIRST`/`LAST`;\n  - `LIMIT` and `OFFSET` in either order, as DuckDB does;\n  - `COUNT(DISTINCT col)`.\n\nThe AST gains `group_by`, `order_by`, `offset` (with spans) and\n`AggregateCall::distinct`. `ToSql` prints the canonical form (`LIMIT n\nOFFSET m`), and `EqualIgnoringSpans` covers every new field.\n- **Still parser rejections (exit 4):**\n- positions (`ORDER BY 2`), `ALL`, constants and expressions in GROUP BY\nand ORDER BY;\n- `GROUPING SETS`, an aggregate `FILTER` in ORDER BY, `ORDER BY …\nUSING`;\n  - `SUM`/`AVG`/`MIN`/`MAX(DISTINCT)`;\n  - HAVING.\n\nMalformed forms are syntax errors (exit 1): `GROUP` without `BY`,\n`NULLS` without `FIRST`/`LAST`, `COUNT(DISTINCT *)`, clauses out of\norder. The \"expected …\" hints now list the clauses that can still\nfollow.\n- **Binder** (`CheckNotYetSupported`): rejects `COUNT(DISTINCT …)`, then\nGROUP BY, ORDER BY and OFFSET as `kUnsupported` (exit 4), pointing at\nthe call or clause, **before any other check**. So a new construct can\nnever reach planning or become a bind error.\n- **HAVING replaces GROUP BY as the \"unsupported\" example**, since GROUP\nBY will soon be answered. The swap covers the CLI goldens `unsupported`\n/ `unsupported_explain` (the latter now uses `SELECT DISTINCT`), the\nCLI, bench, session and slt-runner tests, the write-slt-test recipe, and\nthe harness canary. The canary's pending query is now tagged with a new\n`Feature::kHaving` marker (`kGroupBy` stays never-generated until PR 2).\n- **Tests:** the binder-rejected queries are *added* next to the\nparser-rejected ones at binder, session and CLI level, so the new exit-4\npath is covered end to end.\n- **Docs:** `docs/sql-subset.md` covers the grammar, a \"parsed, not\nanswered yet\" paragraph, the Binding precedence rule and the exit-code\ntable. ADR 0008's list of unsupported examples is updated (content only,\nstatus unchanged).\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 930 (ci, asan, coverage, ci-gcc); tidy clean; fuzz-smoke 2/2\n```\n\n- **Parser tests:**\n- new positive tests (the full clause set with spans, OFFSET before\nLIMIT and alone, COUNT(DISTINCT), `nulls`/`first`/`last` still usable as\nnames);\n  - rejection cases for every construct listed above;\n- canonical-form, round-trip and structural-difference cases for each\nnew field.\n- **Property test:** the random AST generator now emits GROUP BY, ORDER\nBY (with aggregates, DESC, NULLS), OFFSET and COUNT(DISTINCT). Token\naccounting counts their commas, parentheses, stars and numbers.\n- **Fuzz:** two new seeds that fully parse. The dictionary moves\nDISTINCT, GROUP BY, ORDER BY and OFFSET to the subset section and adds\nASC, DESC and the NULLS forms.\n- **Review:** the `reviewer` agent ran on the diff. Its findings are all\nfixed:\n- FILTER in ORDER BY and GROUPING SETS were syntax errors instead of\nunsupported;\n  - engine- and CLI-level coverage of the binder rejection was missing;\n  - the Binding docs' precedence rule was out of date.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change\n- [x] Docs updated where behavior, commands or architecture changed\n- [x] No ClickBench-derived data is committed\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) implemented PR 1 of the plan the maintainer approved, and ran a\nreviewer agent on the diff.\n- Accountable human (has read and understands the whole diff): @Hor911\n(please confirm before merging)",
          "timestamp": "2026-09-27T03:16:25+03:00",
          "tree_id": "ae2420c50c50234ac9fd7d14755e49b30033b0c0",
          "url": "https://github.com/ydb-campus/antb1/commit/048b4b4e9ebbd65e723b8bea87efe07e37374cb3"
        },
        "date": 1790468292751,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3044.1634142099765,
            "unit": "ns/iter",
            "extra": "iterations: 229980\ncpu: 3043.924941299244 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84484.90383631726,
            "unit": "ns/iter",
            "extra": "iterations: 7820\ncpu: 84477.4074168798 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 221659.08333333436,
            "unit": "ns/iter",
            "extra": "iterations: 3156\ncpu: 221636.4214195185 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 440771.89607097814,
            "unit": "ns/iter",
            "extra": "iterations: 1578\ncpu: 440668.9315589355 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 359847.0401647788,
            "unit": "ns/iter",
            "extra": "iterations: 1942\ncpu: 359791.5926879503 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2121184.542682936,
            "unit": "ns/iter",
            "extra": "iterations: 328\ncpu: 2120988.9908536593 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "db5a1c6225a444ba44e9592b49236b1ba48efbec",
          "message": "feat(plan,exec): group by with hash aggregation (#20)\n\n## Summary\n\nPR 2 of the approved GROUP BY / ORDER BY / COUNT(DISTINCT) plan: **GROUP\nBY on plain columns**, checked against DuckDB. ORDER BY, OFFSET and\nCOUNT(DISTINCT) stay \"not answered yet\" (exit 4) until PRs 3 and 4.\n\n- **Binder:**\n- GROUP BY names are table columns, or else the alias of a plain select\ncolumn. When two items share the alias, the last one wins, as in DuckDB.\n  - Duplicate keys are one key, and keys need not be selected.\n- Every plain select column (and every column of `SELECT *`) must be a\nkey; otherwise it's a bind error at that column.\n- Plan: `Scan ← [Filter] ← GroupAggregate(keys, aggregates) ← Project ←\n[Limit]`. The new `GroupAggregateNode` has its own optimizer pruning and\nEXPLAIN line, and the COUNT(*)→RowCount rewrite can never match it.\n- **Executor:**\n- `GroupAggregateOperator` materializes the selected rows and maps their\nkeys to group ids with Arrow's `compute::Grouper` (in\n`libarrow_compute`, no Acero, no new dependency).\n- NULL is a key value. DOUBLE keys are normalized first, so `-0.0`\ngroups with `0.0` and all NaNs group together, as in DuckDB. Each\ngroup's key is output as first seen.\n  - Empty input gives no rows.\n- **`GroupedAggregateState`:** mirrors each scalar state per group:\nexact Int128 SUM/AVG, checked HUGEINT, DOUBLE sums in row order, and\nMIN/MAX with the D10 NaN rule. It is **mergeable through a group map**,\nso a later parallel executor can combine per-row-group partial results\n(the \"parallel-ready\" requirement).\n- **Harness:**\n- `group_by` is a supported, generated feature, and the random generator\nadds 1–2 keys to aggregate queries.\n- **A LIMIT without ORDER BY is now checked properly**\n(`CompareLimited`): an exact comparison first, which is cheap and usual\nfor projections; else antb1's rows must be a multiset subset of DuckDB's\nanswer to the same query without the LIMIT (built with antb1's own\nparser, `UnlimitedSql`). This replaces the old count-only check, which\ncouldn't see wrong values. It is used by the random test, the query\nfiles and the ClickBench runner.\n- **ClickBench: Q17 passes** (GROUP BY + LIMIT without ORDER BY). The\nratchet becomes `[0, 1, 2, 3, 6, 17, 19]`.\n- **Docs:**\n- `docs/sql-subset.md`: grammar, Binding rules, GROUP BY semantics, plan\nnodes, exit codes, ClickBench table;\n  - `docs/architecture.md`;\n  - `tests/slt/README.md`;\n  - new **ADR 0010 \"Grouped aggregation\"**.\n\n**Maintainer sign-off needed:**\n- `tests/data/clickbench_status.json` is an \"Ask a human first\" path. It\ngains 17, as the approved plan said PR 2 would.\n- ADR 0010 is written as **Accepted**, as part of the approved plan.\nPlease confirm both, or tell me to mark the ADR Proposed.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 963 (ci, asan, coverage, ci-gcc); tidy clean; coverage: PASS; fuzz-smoke 2/2\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS queries=20000 failed=0 (grouped queries included)\n$ pixi run test-data\nCLICKBENCH pass=[0, 1, 2, 3, 6, 17, 19]; 100% tests passed out of 6\n$ ANTB1_HITS_FILES=\"~/.cache/antb1/clickbench/full/*.parquet\" pixi run test-data   # host, all 100 files\n100% tests passed out of 6 (3m37s wall; peak RSS 6.7 GB across the processes, DuckDB included)\n```\n\n- **`exec` unit tests:**\n- every aggregate × every input type, group by group, equals the scalar\nstate over that group's rows (bit-exact);\n  - merging partial states with remapped ids equals the single pass;\n  - NaN-only groups, also through Merge;\n  - HUGEINT overflow;\n- operator tests for selections, NULL keys, empty input, -0/NaN keys,\nevery key type, and batch-size invariance;\n  - malformed plans.\n- **`.slt` (`tests/slt/cases/groupby/`):** 25 grouped queries whose\nexpectations DuckDB wrote (`slt-complete`), run as `slt.*` and\n`oracle.*`. They cover every key type, NULL keys, FLOAT ±0, multi-file\ntables, alias keys, empty results, every aggregate, and the grouping\nerrors.\n- **Metamorphic:** grouped results are invariant to batch size and to\nfile split.\n- **Comparator self-tests:** subset, multiset, count, R tolerance, lazy\nunlimited query, and `UnlimitedSql`.\n- **EXPLAIN golden:** `explain_group_by`.\n- **Performance of the harness change:** `diff.random` takes 11.8s\n(10.1s before). A first version of the subset check took 69s. It now\ncompares exactly first and indexes rows by their exact cells.\n- **Review:** the `reviewer` agent's DuckDB cross-checks all matched\n(-0/NaN, NULL, empty input, `SELECT *`, shadowing aliases). Its findings\nare fixed:\n  - the last-alias rule for a repeated alias;\n  - self-tests for the new comparator.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change\n- [x] Docs updated where behavior, commands or architecture changed\n- [x] No ClickBench-derived data is committed\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (`tests/data/clickbench_status.json`: +17 per the\napproved plan; ADR 0010 status)\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) implemented PR 2 of the approved plan and ran a reviewer agent on\nthe diff.\n- Accountable human (has read and understands the whole diff): @Hor911\n(please confirm before merging)",
          "timestamp": "2026-09-27T11:16:17+03:00",
          "tree_id": "4857f3b926666f11368ec8cc8aa359bf840dec51",
          "url": "https://github.com/ydb-campus/antb1/commit/db5a1c6225a444ba44e9592b49236b1ba48efbec"
        },
        "date": 1790497073516,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2763.448638809788,
            "unit": "ns/iter",
            "extra": "iterations: 252720\ncpu: 2763.407011712567 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 87785.94515206497,
            "unit": "ns/iter",
            "extra": "iterations: 7694\ncpu: 87760.32505848714 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 94664.45851942357,
            "unit": "ns/iter",
            "extra": "iterations: 7389\ncpu: 94617.95601569898 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 368938.7167721537,
            "unit": "ns/iter",
            "extra": "iterations: 1896\ncpu: 368740.9810126582 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 375228.5780748745,
            "unit": "ns/iter",
            "extra": "iterations: 1870\ncpu: 375067.05935828877 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2584739.490774887,
            "unit": "ns/iter",
            "extra": "iterations: 271\ncpu: 2583604.8044280433 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "04727fa1e29ceae22e677c6074d7d029c4f696c4",
          "message": "feat(plan,exec): order by, offset and top-n (#21)\n\n## Summary\n\nPR 3 of the approved GROUP BY / ORDER BY / COUNT(DISTINCT) plan: **ORDER\nBY, OFFSET and top-N**, checked against DuckDB. COUNT(DISTINCT) stays\n\"not answered yet\" (exit 4) until PR 4.\n\n- **Binder:**\n- ORDER BY items are a select alias first (the last item with it, also\nover a same-named column, as in DuckDB), else a table column, or an\naggregate call. ASC/DESC and NULLS FIRST/LAST are supported; the default\nis NULLS LAST for both directions (DuckDB).\n  - A projection may order by columns it does not select.\n- A grouped query orders by keys and aggregates. An ORDER BY aggregate\nthat the select list lacks becomes a hidden aggregate of the\n`GroupAggregate`.\n- A global aggregate has one row, so its ORDER BY is only checked and\ngets no Sort.\n- An ORDER BY aggregate makes the query an aggregate query, and plain\ncolumns are then a bind error.\n  - `OFFSET m`, with or without LIMIT, in either order.\n- **Plan:**\n  - New `SortNode` below the final `Project`.\n  - `LimitNode` gets `std::optional limit` and `offset`.\n- New optimizer rule **Limit below Project**, which puts the Limit right\nabove the Sort.\n- EXPLAIN prints `Sort x DESC NULLS LAST, ...` and `Limit 10 OFFSET 5` /\n`Limit ALL OFFSET 1`.\n- **Executor (`src/exec/sort.{h,cc}`):**\n- `RowComparator` gives DuckDB's order: NULL placement independent of\ndirection, NaN above every number, `-0.0 == 0.0`, VARCHAR by bytes,\nHUGEINT exact.\n- `SortBuffer` sorts stably, so tied rows keep input order and a top-N\nequals the window of the full sort. With `limit + offset` it drops rows\nthat cannot make the cut as they arrive and compacts, keeping memory\nO(k).\n- `SortBuffer::Merge` appends another buffer in input order\n(parallel-ready, like the grouped states).\n- The physical planner runs `Limit(Sort)` with a positive limit as one\ntop-N `SortOperator`.\n- `LimitOperator` handles the offset and narrows selections instead of\nmaterializing them.\n- **Fix to #20 found on the full dataset:** `GroupAggregateOperator`\nconcatenated every group's VARCHAR key into one array, and more than 2\nGiB of distinct keys overflowed Arrow's 32-bit binary offsets. It now\nemits one batch per chunk of new groups.\n- **Harness: a tie-aware ordered comparator**\n(`tests/slt/runner/ordered_compare.{h,cc}`):\n- DuckDB runs an augmented query: the original select list plus the\nORDER BY keys (aliases resolved as the binder does), without\nLIMIT/OFFSET. Its limit grows until the run of ties at the window's end\nis complete, capped at 2^20 rows.\n- antb1's row `i` must be a distinct row of the run of equal keys at\nrank `offset + i`.\n- If that run is longer than the cap (millions of groups tied at a count\non the full data), one more query fetches only the run's rows that equal\nantb1's rows there, written as SQL literals. Text that SQL cannot hold\nis reported as a harness limitation, never as a pass.\n- `CompareQueryAnswers` picks ordered, unordered-limit or plain\ncomparison from antb1's own parse of the SQL. The random test, the query\nfiles and the ClickBench runner all use it, so no query text is stored.\n- The generator gains `order_by`, `nulls_order` and `offset`. It sorts\nonly by columns, aliases and aggregates with I/T values: a DOUBLE\nSUM/AVG can differ in the last bits and order near-ties differently.\n- **ClickBench: +16 queries pass** (Q7, 12, 14, 15, 16, 24, 25, 26, 30,\n31, 32, 33, 36, 37, 38, 41). The ratchet goes from 7 to 23 queries.\n- **Docs:**\n- `docs/sql-subset.md`: grammar, Binding, ORDER BY / LIMIT / OFFSET\nsemantics, plan nodes, exit codes, ClickBench table;\n- `docs/architecture.md`, `docs/testing.md`, `tests/slt/README.md`,\n`docs/recipes/write-slt-test.md`;\n  - new **ADR 0011 \"Sorting and top-N\"**.\n\n**Maintainer sign-off needed:**\n- `tests/data/clickbench_status.json` is an \"Ask a human first\" path. It\ngains the 16 queries above, as the approved plan said PR 3 would.\n- ADR 0011 is written as **Accepted**, like ADR 0010 in #20. Please\nconfirm both, or tell me to mark the ADR Proposed.\n- `.agents/skills/write-slt-test/SKILL.md` (a protected path, not\nchanged here) still says \"there is no ORDER BY yet\". It needs the same\none-line update as `docs/recipes/write-slt-test.md`.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [x] fix: bug fix (the GROUP BY key overflow above)\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 1007 (ci, asan, coverage, ci-gcc); tidy clean; Coverage gate: PASS; fuzz-smoke passed\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3338235670 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\nCLICKBENCH pass=[0, 1, 2, 3, 6, 7, 12, 14, 15, 16, 17, 19, 24, 25, 26, 30, 31, 32, 33, 36, 37, 38, 41]; 100% tests passed out of 6\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # host, all 100 files\n100% tests passed out of 6 (8m20s wall; peak RSS 25.6 GB for one process, DuckDB included)\n```\n\n- **Full data, before the two fixes above:**\n  - Q33 failed with the key-concatenation overflow.\n- The harness took 645 s and 87 GB on Q32, fetching tens of millions of\ntied rows.\n  - antb1 alone on Q32 over all files: 53 s, 16 GB peak RSS.\n- **`exec` unit tests (`sort_test.cc`):**\n- the comparator for every engine type, both directions and both NULL\nplacements, NaN and ±0, bytes;\n- Sort and top-N checked against an independent `std::stable_sort` model\nover random data, for 16 direction/NULL combinations and LIMIT/OFFSET\nwindows around batch and compaction boundaries (INT64_MAX included);\n  - merged buffers (sorted and unsorted parts) equal one buffer;\n- selections, error paths, and LIMIT/OFFSET over selections across\nbatches.\n- **Planner test:** `Limit(Sort)` becomes a top-N `SortOperator`, while\nLIMIT 0 and OFFSET-only keep a `LimitOperator`.\n- **Grouped test:** output comes one batch per chunk of new groups, and\na re-run gives the same batches.\n- **`.slt` (`tests/slt/cases/orderby/`):** 37 queries plus errors, with\nexpectations written by DuckDB (`slt-complete`), run as `slt.*` and\n`oracle.*`. They cover alias precedence, non-selected keys, hidden\naggregates, multi-file tables, DATE, every integer type, VARCHAR bytes\nand OFFSET edges.\n- **Metamorphic:**\n  - sort and top-N are invariant to batch size and file split;\n  - a top-N is the window of the full sort;\n  - a grouped top-N is batch-size invariant.\n- **Comparator self-tests:**\n  - ties in any order and ties at the window edges;\n  - the growing limit;\n  - R tolerance;\n  - oracle failures;\n- the long-run path against the DuckDB library, including wrong rows,\nduplicates, NULLs and non-UTF-8 text.\n- **CLI goldens:** new `explain_order_by` and `query_order_by`.\n`explain_group_by` and `explain_projection` change because the Limit now\nsits below the Project.\n- **Review:** the `reviewer` agent found no P0. Its P1 on missing\nplanner tests is fixed. Its P1 on the ratchet is the sign-off above. Its\nP2 is a follow-up: a full sort without LIMIT gathers output with one\n`AppendArraySlice` per row, where a Take per chunk would be faster.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ratchet update is in the approved plan; please\nconfirm in review\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs, and ran the verification above; the `reviewer` subagent\nreviewed the diff.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-27T17:06:18+03:00",
          "tree_id": "8b9784a1736371562bcb850f34fdd79a9cf7637c",
          "url": "https://github.com/ydb-campus/antb1/commit/04727fa1e29ceae22e677c6074d7d029c4f696c4"
        },
        "date": 1790518093116,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3061.7988855116505,
            "unit": "ns/iter",
            "extra": "iterations: 228984\ncpu: 3061.4421749991266 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84777.62583056532,
            "unit": "ns/iter",
            "extra": "iterations: 7224\ncpu: 84774.99169435218 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222051.61250396594,
            "unit": "ns/iter",
            "extra": "iterations: 3151\ncpu: 222033.187559505 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 459712.45494923356,
            "unit": "ns/iter",
            "extra": "iterations: 1576\ncpu: 459699.1078680203 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 348260.20895522233,
            "unit": "ns/iter",
            "extra": "iterations: 2010\ncpu: 348236.869651741 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2117166.2303030537,
            "unit": "ns/iter",
            "extra": "iterations: 330\ncpu: 2117082.0242424244 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "52181bda3cac79aee1e8d1014dac041e2aa2a5a0",
          "message": "fix(exec): finalize grouped aggregates per output chunk (#22)\n\n## Summary\n\nFollow-up (b) from #21. A grouped VARCHAR `MIN` or `MAX` built its\nresult for all groups as one binary array. More than 2 GiB of result\nstrings (millions of groups with long values) would overflow Arrow's\n32-bit offsets, and the query would fail with an execution error. #21\nalready emits the group keys in several batches. This PR does the same\nfor the aggregate values.\n\n- `GroupedAggregateState::Finalize(begin, end, pool)` returns the values\nof groups `[begin, end)`. It is Invalid unless `begin <= end <=\nnum_groups()`. A non-virtual `Finalize(pool)` covers every group.\n- `GroupAggregateOperator` finalizes each output batch's range of groups\nas it emits the batch. It no longer keeps full-length finalized arrays,\nso it also holds less memory at once.\n\n## Type of change\n\n- [x] fix: bug fix\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 1007 (ci, asan, coverage, ci-gcc); tidy clean; Coverage gate: PASS; fuzz-smoke passed\n```\n\n- **New test\n(`exec.GroupedAggregateTest.EveryGroupEqualsTheScalarStateOverItsRows`):**\nfor every aggregate and input type, ranges of groups equal the slices of\nthe whole result. That includes empty ranges and ranges at the ends.\nRanges outside the state are Invalid.\n- `EmitsOneBatchPerChunkOfNewGroups` and the grouped `.slt`, metamorphic\nand random tests pass unchanged.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed: not needed (no behavior\nchange below the limit)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: none changed\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests and ran the verification above.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-27T19:49:17+03:00",
          "tree_id": "49c7e88f3d813a264c814e224cb9c4f758b81ca9",
          "url": "https://github.com/ydb-campus/antb1/commit/52181bda3cac79aee1e8d1014dac041e2aa2a5a0"
        },
        "date": 1790527854715,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2261.137971982777,
            "unit": "ns/iter",
            "extra": "iterations: 306526\ncpu: 2260.978452072581 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73218.74596441699,
            "unit": "ns/iter",
            "extra": "iterations: 8487\ncpu: 73202.41981854601 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 84805.54863719064,
            "unit": "ns/iter",
            "extra": "iterations: 8255\ncpu: 84787.28310115082 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 376237.30971550953,
            "unit": "ns/iter",
            "extra": "iterations: 1863\ncpu: 376159.2463768116 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 354104.9812563319,
            "unit": "ns/iter",
            "extra": "iterations: 1974\ncpu: 354041.73708206724 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2195462.647798735,
            "unit": "ns/iter",
            "extra": "iterations: 318\ncpu: 2194989.169811321 ns\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "fc25408f720f605f21c71b146e06042f17440ae0",
          "message": "perf(exec): sort by a first-key prefix and gather with typed builders (#23)\n\n## Summary\n\nFollow-up (a) from #21, the reviewer's P2 note: a full sort without\n`LIMIT` gathered its output one `AppendArraySlice` call per row. A\nprofile of the new benchmark showed the row comparisons cost more than\nthe gather, about 70% of the time against 15%, so this PR fixes both.\n\n- **Prefix sort:** before sorting, each row gets an order-preserving\n64-bit prefix of its first key (`RowComparator::PrefixOf`) and a NULL\ngroup (NULLs first, a value, NULLs last).\n  - Most comparisons become two integer compares.\n- The full comparator runs only when prefixes tie: from key 1 on when\nthe prefix is the whole value (integers, DATE, DOUBLE), from key 0 for\nHUGEINT (its high 64 bits) and VARCHAR (the first 8 bytes).\n- DOUBLE prefixes keep DuckDB's order: NaN above every number, `-0.0`\nequal to `0.0`. Descending keys invert the bits; the NULL group does not\ndepend on the direction.\n- **Typed gather:** the output is gathered per column with the typed\nbuilder of each engine type. The type is resolved once per column, then\na tight loop runs per row. Binary data is reserved up front. Any other\ntype keeps the per-run slice path.\n- **New micro benchmarks** `BM_SortRows` (full sort) and `BM_TopNRows`\n(`LIMIT 10`): 1Mi rows with a random BIGINT key and a VARCHAR payload.\nBoth are listed in `docs/benchmarks.md`.\n\n| Release `pixi run bench` | before | after |\n| --- | ---: | ---: |\n| `BM_SortRows` (1Mi rows, full sort) | 503 ms | 200 ms |\n| `BM_TopNRows` (LIMIT 10) | 25.5 ms | 14.9 ms |\n\n## Type of change\n\n- [x] perf: performance improvement\n\n## Verification\n\n```text\n$ pixi run check      # after the last commit\nlint: PASS; 100% tests passed out of 1008\n$ pixi run asan / ci-gcc / coverage / fuzz-smoke / tidy\n100% tests passed out of 1008 (asan, ci-gcc); Coverage gate: PASS; fuzz-smoke 2/2; tidy clean\n```\n\n- **New `exec.SortTest.PrefixesAgreeWithTheComparator`:** for every\nengine type, both directions and both NULL orders, and every pair of\nrows, a smaller (group, prefix) sorts first. For exact prefixes, equal\nprefixes tie on the key. The rows include integer edges, NaN of both\nsigns, ±0, ±inf, subnormals, strings with zero bytes and shared 8-byte\nprefixes, and HUGEINT values across the 64-bit boundary.\n- The existing checks pass unchanged:\n- the model-based Sort/top-N test (16 direction/NULL combinations with\nLIMIT/OFFSET windows);\n  - the merge test;\n  - the `.slt` ORDER BY cases;\n  - the metamorphic sort relations.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed: `docs/benchmarks.md`,\n`docs/architecture.md`\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: none changed\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code profiled the\nsort, wrote the change, the benchmark and the tests, and ran the\nverification above.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-27T19:52:23+03:00",
          "tree_id": "0032464f3cf6699c4ffab9bcdf4baade920dad3f",
          "url": "https://github.com/ydb-campus/antb1/commit/fc25408f720f605f21c71b146e06042f17440ae0"
        },
        "date": 1790528065103,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3233.173701201222,
            "unit": "ns/iter",
            "extra": "iterations: 216527\ncpu: 3231.458908126931 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84864.13406900196,
            "unit": "ns/iter",
            "extra": "iterations: 7884\ncpu: 84827.10692541856 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222029.31630710184,
            "unit": "ns/iter",
            "extra": "iterations: 3152\ncpu: 222012.30393401007 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 437442.25595983746,
            "unit": "ns/iter",
            "extra": "iterations: 1594\ncpu: 437274.4692597239 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 348948.26321037055,
            "unit": "ns/iter",
            "extra": "iterations: 2006\ncpu: 348808.4521435692 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2127005.791410974,
            "unit": "ns/iter",
            "extra": "iterations: 326\ncpu: 2126118.9754601247 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 209.23663666666434,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 209.1935283333332 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.374935624999807,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.3734615 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "b7a321594c25d8d9839eed0d2fbea11814ef29ba",
          "message": "feat(exec): count distinct (#26)\n\n## Summary\n\nPR 4, the last one of the approved GROUP BY / ORDER BY / COUNT(DISTINCT)\nplan: **`COUNT(DISTINCT col)`**, global and grouped, in the select list\nand in `ORDER BY`, checked against DuckDB.\n\n- **Semantics (probed on DuckDB 1.5.5):**\n  - distinct non-NULL values;\n  - DOUBLE `-0.0` equals `0.0`, and every NaN is one value;\n  - VARCHAR compares by bytes;\n  - BIGINT, 0 over no values;\n  - named `count(DISTINCT x)`, with the argument as written.\n- **Plan:**\n- new `AggKind::kCountDistinct`, so every exhaustive switch handles it;\n  - EXPLAIN prints `COUNT(DISTINCT x)`;\n- in `ORDER BY` it reuses an equal select aggregate (a `COUNT(x)` is a\ndifferent aggregate) or becomes a hidden one;\n- the binder's \"not supported yet\" step is gone: nothing parsed is\nunanswered any more;\n- `SUM`, `AVG`, `MIN` and `MAX` with `DISTINCT` stay exit code 4 in the\nparser.\n- **Exec:**\n- global: an Arrow `Grouper` over the selected values; NULL is a key of\nits own, left out of the count;\n- grouped: a `Grouper` over the (group id, value) pairs, where a pair\nseen for the first time adds one to its group unless the value is NULL;\n- both merge (the parallel-ready requirement): `Merge` feeds the other\nstate's distinct values, or its pairs remapped through the group map,\ninto this one;\n- DOUBLE values are normalized like GROUP BY keys: the normalizer moved\nto the private `src/exec/double_key.{h,cc}`;\n  - `MakeGroupedAggregateState` takes the operator's memory pool.\n- **Harness:** `count_distinct` is a supported, generated feature, and\nit is orderable because it is BIGINT. The random test's self-test used a\nfake antb1 that recognized \"COUNT(*) only\" queries by keywords, and\ngenerated ORDER BY / DISTINCT queries could slip through it. Its keyword\nlist is now complete.\n- **ClickBench: +7 queries pass** (Q4, 5, 8, 9, 10, 11, 13). The ratchet\ngoes from 23 to 30 of 43.\n- **Docs:** `docs/sql-subset.md` (grammar, Binding, semantics, exit\ncodes, ClickBench table) and `docs/architecture.md`.\n\n**Maintainer sign-off needed:** `tests/data/clickbench_status.json` is\nan \"Ask a human first\" path. It gains the 7 queries above, as the\napproved plan said PR 4 would.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check / asan / tidy / coverage / fuzz-smoke / ci-gcc   (check-full's legs; tidy re-run after its fix)\nlint: PASS; 100% tests passed out of 1022 (ci, asan, ci-gcc); tidy clean; Coverage gate: PASS; fuzz-smoke 2/2\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=325968058 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\nCLICKBENCH pass=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 24, 25, 26, 30, 31, 32, 33, 36, 37, 38, 41]\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # host, all 100 files\n100% tests passed out of 6 (10m53s wall; peak RSS 22.5 GB for one process, DuckDB included)\n```\n\n- **`exec` unit tests:**\n- the scalar state on doubles with ±0, NaN of both signs and NULL, on\nstrings with a selection, on NULL-only and empty input, and through\nmerges;\n- the grouped state against an independent model (a set of normalized\nvalues per group) for every column type, across two batches;\n- both states are in the existing grouped tests (equal to the scalar\nstate per group, merge with remapped ids, finalizing ranges);\n  - invalid combinations are rejected.\n- **Plan tests:** result naming (quoted and case-preserved arguments),\nORDER BY reuse versus a hidden `COUNT(x)`, EXPLAIN, and bind errors.\n- **`.slt` (`tests/slt/cases/distinct/`):** 13 queries plus errors, with\nexpectations written by DuckDB. They cover every column type, FLOAT ±0,\nmulti-file and NULL-heavy tables, empty results, and grouped and ordered\nqueries.\n- **Metamorphic:**\n- `COUNT(DISTINCT x)` equals the number of non-NULL groups of `GROUP BY\nx`, for an INTEGER and a VARCHAR column;\n- the global and grouped counts are invariant to batch size and file\nsplit.\n- **Review:** the `reviewer` agent found no P0. Its P1s are the ratchet\nsign-off above, and a note that the maintainer's untracked `result.json`\n(ClickBench bench output) sits in the repo root: it was never staged,\nsince files are added by name.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ratchet update is in the approved plan; please\nconfirm in review\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs and ran the verification above; the `reviewer` subagent\nreviewed the diff.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-27T21:00:18+03:00",
          "tree_id": "17f277a7ae72dc3db1b9addc3b060608840bea80",
          "url": "https://github.com/ydb-campus/antb1/commit/b7a321594c25d8d9839eed0d2fbea11814ef29ba"
        },
        "date": 1790532133860,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3069.738721353093,
            "unit": "ns/iter",
            "extra": "iterations: 224894\ncpu: 3069.4238263359625 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85009.57342657236,
            "unit": "ns/iter",
            "extra": "iterations: 7293\ncpu: 85006.05004799122 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222391.5063451804,
            "unit": "ns/iter",
            "extra": "iterations: 3152\ncpu: 222360.3426395939 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 442300.946902655,
            "unit": "ns/iter",
            "extra": "iterations: 1582\ncpu: 442283.97914032877 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 362066.9067357484,
            "unit": "ns/iter",
            "extra": "iterations: 1930\ncpu: 362030.85336787545 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2138165.716923088,
            "unit": "ns/iter",
            "extra": "iterations: 325\ncpu: 2138092.9076923067 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 220.6457056666693,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 220.61047600000006 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.438383428571388,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.43776418367347 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "3d41bb42ec5c32852bb257481771db59539d65c7",
          "message": "feat(sql,exec): like and not like (#27)\n\n## Summary\n\n**`column [NOT] LIKE 'pattern'` in `WHERE`**, with DuckDB's semantics\nand checked against DuckDB.\n\n- **Semantics (probed on DuckDB 1.5.5):**\n- `%` matches any sequence of characters (none included), and `_`\nmatches exactly one UTF-8 character: `'é' LIKE '_'` is true;\n- there is no escape character (`\\` is a literal byte), and the match is\ncase-sensitive;\n  - a NULL value rejects the row for `LIKE` and for `NOT LIKE`;\n  - LIKE on a non-VARCHAR column is a bind error, as in DuckDB.\n- Bytes that are not UTF-8 count as one character each. DuckDB refuses\nto read such text as VARCHAR at all, so this is antb1's own rule, and\nthe docs say so.\n- **Parser:** `LIKE` and `NOT LIKE` become comparisons\n(`CompareOp::kLike`/`kNotLike`), always with the column on the left and\na literal pattern on the right. The unparser round-trips them. These\nstay unsupported (exit code 4): `ILIKE`, `LIKE ... ESCAPE`, a column as\nthe pattern, a literal on the left, and LIKE outside `WHERE`.\n- **Plan:**\n- `Predicate::Kind::kLike`/`kNotLike`, with the pattern as a VARCHAR\nconstant;\n- a pattern of only `%` folds: `LIKE` becomes `IS NOT NULL`, and `NOT\nLIKE` becomes `FALSE`, which reads nothing;\n  - EXPLAIN prints `s LIKE '%x%'`.\n- **Exec (`exec::LikePattern`, `src/exec/like.{h,cc}`):**\n- A pattern without `_` is its literal segments between the `%`s: a\nprefix, a suffix and substrings in order, each found by the leftmost\nsearch. So `'%x%'` is one substring search per row.\n- A pattern with `_` uses a backtracking matcher that steps through\nwhole characters.\n  - `FilterOperator` checks the column type and the pattern at `Open`.\n- **Harness:** `like` is a supported, generated feature. Patterns come\nfrom the ASCII characters of sample values (substrings, prefixes,\nsuffixes and several segments, with `_` inserted) and from edge patterns\n(`%`, `''`, `_`, `\\`, a 2-byte character).\n- **ClickBench: +4 queries pass** (Q20, Q21, Q22, Q23). The ratchet goes\nfrom 30 to 34 of 43.\n- **Docs:**\n- `docs/sql-subset.md`: grammar, the VARCHAR row, LIKE semantics, the\nClickBench table;\n  - `docs/architecture.md`;\n  - a fuzz dictionary entry and a seed.\n\n**Maintainer sign-off needed:** `tests/data/clickbench_status.json` is\nan \"Ask a human first\" path. Unlike PRs 1–4, this change is **not**\ncovered by an approved plan. It adds Q20–Q23; please approve it\nexplicitly in the review.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check-full   (asan passed there; tidy and ci-gcc re-run after their fixes)\nlint: PASS; 100% tests passed out of 1053 (ci, asan, ci-gcc); tidy clean; Coverage gate: PASS; fuzz-smoke 2/2\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2573700789 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\nCLICKBENCH pass=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 30, 31, 32, 33, 36, 37, 38, 41]\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # host, all 100 files\n100% tests passed out of 6 (14m13s wall; peak RSS 21.8 GB for one process, DuckDB included)\n```\n\n- **Matcher tests:**\n- the DuckDB-probed cases, and the segment fast path (overlapping prefix\nand suffix, repeated `%`, empty pattern);\n- a 20k-case property test against an independent recursive definition\nover characters, with 2-byte UTF-8 characters and invalid bytes in the\ntext, both with and without `_`;\n  - NULL handling and negation in `Evaluate`.\n- **Operator, plan and SQL tests:**\n- the filter with LIKE and NOT LIKE over NULLs, and malformed predicates\nrejected at `Open`;\n  - binder kinds, `%` folding and bind errors, and EXPLAIN;\n  - parser spans, the rejected forms, and unparse round-trips.\n- **`.slt` (`tests/slt/cases/like/`):** 18 queries plus errors, with\nexpectations written by DuckDB. They cover UTF-8 `_`, the backslash, the\nempty pattern, case, NULLs, `%` folding, multi-file tables, and grouped\nand ordered queries.\n- **Metamorphic:** `COUNT(col)` equals `LIKE p` plus `NOT LIKE p` for 6\npattern shapes over 2 columns with NULLs. A split table's count is the\nsum of its files' counts.\n- **Review:** the `reviewer` agent found no engine problem. It flagged\nthat my first draft used ClickBench query text: a LIKE pattern in a doc\nexample, a unit test and the fuzz seed. I replaced it with invented\nwords before anything was pushed. A scan of all tracked files for\nClickBench queries, WHERE clauses and LIKE patterns now finds nothing.\nIts other P1 is the ratchet sign-off above.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ratchet needs your approval in this review\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs and ran the verification above; the `reviewer` subagent\nreviewed the diff.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-27T22:07:50+03:00",
          "tree_id": "eec5c9b53eab7dc62c94be0d6a9e72a87393d414",
          "url": "https://github.com/ydb-campus/antb1/commit/3d41bb42ec5c32852bb257481771db59539d65c7"
        },
        "date": 1790536192812,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2311.992579600625,
            "unit": "ns/iter",
            "extra": "iterations: 303892\ncpu: 2311.8723131902125 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73772.14910053923,
            "unit": "ns/iter",
            "extra": "iterations: 9061\ncpu: 73756.13828495752 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 84893.5467992252,
            "unit": "ns/iter",
            "extra": "iterations: 8248\ncpu: 84881.25509214356 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 377629.44743934466,
            "unit": "ns/iter",
            "extra": "iterations: 1855\ncpu: 377579.7040431269 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 355779.53238866205,
            "unit": "ns/iter",
            "extra": "iterations: 1976\ncpu: 355698.4782388665 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2336493.1229235767,
            "unit": "ns/iter",
            "extra": "iterations: 301\ncpu: 2335947.4850498345 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 239.74983866666358,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 239.71689499999994 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.848000627118559,
            "unit": "ms/iter",
            "extra": "iterations: 59\ncpu: 11.846865864406784 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Hor911",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "e1c23612a4f87037f7401fbdee05fec157ceb0b1",
          "message": "feat(sql,exec): in and not in (#28)\n\n## Summary\n\n**`column [NOT] IN (literal, ...)` in `WHERE`**, checked against DuckDB.\nThis is the first of the three PRs agreed before the expressions plan:\nIN, GROUP BY / ORDER BY positions, HAVING.\n\n- **Semantics (probed on DuckDB 1.5.5):**\n- `c IN (v1, v2, ...)` is `c = v1 OR c = v2 OR ...` with Kleene logic,\nand `NOT IN` is its negation, so a NULL value rejects the row for both.\n- Each value is typed and folded exactly like `c = v`. A value no column\nvalue can equal (out of the type's range, or not an integer for an\ninteger column) is dropped.\n  - With no values left, `IN` is `FALSE` and `NOT IN` is `IS NOT NULL`.\n- `-0.0` matches `0.0`. NaN never matches, as for `=`; divergence D10\nnow says so for IN.\n- **DuckDB types the whole list as one type.** When one number in the\nlist is DuckDB-DOUBLE-typed (an exponent, or more than 38 digits), every\nnumber is read as a double:\n- integer columns compare with the nearest doubles (divergence D7,\nextended to IN lists);\n- a FLOAT column then compares in DOUBLE: `f IN (0.1, 1e0)` does not\nmatch the FLOAT `0.1`, while `f IN (0.1, 2)` does.\n- **Parser:** `IN` and `NOT IN` are comparisons with a value list\n(`Comparison::list`); the unparser and `EqualIgnoringSpans` handle it.\n- Rejected with exit code 4: `IN (SELECT ...)`, a column in the list, a\nliteral on the left, and IN outside `WHERE`.\n  - Syntax errors: an empty list, a missing or unclosed parenthesis.\n- **Plan and exec:** `Predicate::Kind::kIn`/`kNotIn` with the folded\n`values`, and EXPLAIN `c IN (1, 2)`. `FilterOperator` computes one Arrow\n`equal` per value combined with `or_kleene`, and `invert` for NOT IN.\n- **Harness:** `in` is a supported, generated feature: 1–4 values made\nby the same literal generator as comparisons, with edges and\nout-of-range values.\n- **ClickBench: +1 query** (Q40). The ratchet goes from 34 to 35 of 43.\n- **Docs:** `docs/sql-subset.md` (grammar, rejected forms, IN semantics,\nD7 and D10, the ClickBench table), `docs/architecture.md`, and a fuzz\nseed.\n- **Also fixed:** a stray comment line left in `binder.cc` by #27.\n\n**Maintainer sign-off needed:** `tests/data/clickbench_status.json` is\nan \"Ask a human first\" path. It adds Q40, and no approved plan covers\nit; please approve it explicitly in the review.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check-full   (asan passed there; tidy, coverage, fuzz-smoke and ci-gcc re-run after the tidy fix)\nlint: PASS; 100% tests passed out of 1076 (ci, asan, ci-gcc); tidy clean; Coverage gate: PASS; fuzz-smoke 2/2\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3166748253 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\nCLICKBENCH pass=[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 24, 25, 26, 30, 31, 32, 33, 36, 37, 38, 40, 41]\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # host, all 100 files\n100% tests passed out of 6 (14m24s wall; peak RSS 22.7 GB for one process, DuckDB included)\n```\n\n- **Parser:** the parsed list with its span, the rejected and malformed\nforms, and unparse round-trips.\n- **Binder:**\n  - dropped values, and folding with no values left;\n  - type errors per value;\n- DOUBLE-typed lists on integer columns (2^53 + 1 becomes 2^53 only when\nan exponent is in the list).\n- **EXPLAIN** of the folded list.\n- **Filter:** IN and NOT IN over NULLs, and malformed predicates\nrejected at `Open`.\n- **`.slt` (`tests/slt/cases/in/`):** 18 queries plus errors, with\nexpectations written by DuckDB. They cover every integer type with\nout-of-range and fractional values, DOUBLE, VARCHAR, DATE, FLOAT ±0,\nDOUBLE-typed lists over FLOAT and BIGINT, and a grouped query over a\nsplit table.\n- **Metamorphic:** `COUNT(col)` equals IN plus NOT IN, and IN equals the\nsum of the equalities, over three columns with NULLs.\n- **Review:** the `reviewer` agent found that per-value typing differed\nfrom DuckDB for lists with a DOUBLE-typed number (on FLOAT: `f IN (0.1,\n1e0)`). That is fixed and tested as described above. A scan of all\ntracked files for ClickBench queries, WHERE clauses, LIKE patterns and\nIN lists finds nothing.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ratchet needs your approval in this review\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs and ran the verification above; the `reviewer` subagent\nreviewed the diff.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T01:13:05+03:00",
          "tree_id": "a3eb1c3d7f8094ca35ffce0f5be6f0a03d667a63",
          "url": "https://github.com/ydb-campus/antb1/commit/e1c23612a4f87037f7401fbdee05fec157ceb0b1"
        },
        "date": 1790547309839,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3040.4527429065433,
            "unit": "ns/iter",
            "extra": "iterations: 228061\ncpu: 3039.5114114206285 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85031.20041726284,
            "unit": "ns/iter",
            "extra": "iterations: 7669\ncpu: 85006.23718868168 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222119.67226624486,
            "unit": "ns/iter",
            "extra": "iterations: 3155\ncpu: 222060.61299524567 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 439639.65726817056,
            "unit": "ns/iter",
            "extra": "iterations: 1596\ncpu: 439496.16416040115 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 401281.4090648293,
            "unit": "ns/iter",
            "extra": "iterations: 1743\ncpu: 401252.21916236344 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2158656.2215384655,
            "unit": "ns/iter",
            "extra": "iterations: 325\ncpu: 2157721.8430769234 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 239.55489999999693,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 239.5048036666664 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.437701104166756,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.432680583333338 ms\nthreads: 1"
          }
        ]
      }
    ]
  }
}