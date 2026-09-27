window.BENCHMARK_DATA = {
  "lastUpdate": 1790518094157,
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
      }
    ]
  }
}