window.BENCHMARK_DATA = {
  "lastUpdate": 1790675290508,
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
          "id": "de906d019354fdf222db973fafa4573f497ea44b",
          "message": "feat(sql,plan): group by and order by positions, and constants (#29)\n\n## Summary\n\nThe second of the three PRs agreed before the expressions plan: **GROUP\nBY and ORDER BY positions, and constant select items**, checked against\nDuckDB. Constants are part of it because the query this unlocks groups\nby the position of a constant (`SELECT 1, col, COUNT(*) ... GROUP BY 1,\ncol`).\n\n- **Constant select items (as DuckDB types and names them):**\n- An integer is INTEGER when its magnitude fits (so `-2147483648` is\nBIGINT), otherwise BIGINT or HUGEINT, and it is named by its value\n(`007` is `7`).\n  - A string is VARCHAR, named with its quotes (`'it''s'`).\n  - A `DATE` literal is DATE, named `CAST('2020-01-02' AS \"DATE\")`.\n- Decimals (DuckDB's DECIMAL) and integers beyond HUGEINT's 38 digits\nare unsupported (exit code 4).\n- Constants mix with columns and aggregates. With an aggregate the query\nhas one row, also when the only aggregate is in ORDER BY: `SELECT 1 FROM\nt ORDER BY COUNT(*)`.\n- **Positions (DuckDB's rules, probed):**\n- An integer literal in `GROUP BY`/`ORDER BY` names a select item\n(1-based; `*` counts every column). Out of range, a negative one\nincluded, is a bind error, and `GROUP BY` of an aggregate item is a bind\nerror.\n- In `GROUP BY` any other literal is a constant: no key, but the query\nis still grouped. `GROUP BY` of constants only is a grouping without\nkeys: one group over rows, none over no rows.\n- In `ORDER BY` a number or string that is not a position is a bind\nerror (\"orders nothing\"), and a DATE literal orders nothing.\n- **Plan:**\n- `ProjectNode` gains optional per-column `constants`; pruning skips\nthem and EXPLAIN prints them.\n  - The binder's select list has three item kinds.\n- A global aggregate gets a Project only when there are constants, so\nexisting plans are unchanged.\n- **Exec:**\n- `ProjectOperator` materializes only the listed columns and adds\nconstant columns (`MakeArrayFromScalar`).\n  - `GroupAggregateOperator` supports zero keys.\n- **Harness:**\n- New generated features `constant` and `position`: constant items,\nGROUP BY keys by position, GROUP BY a constant's position alone, and\nORDER BY positions.\n- The ordered comparator resolves ORDER BY positions to their select\nitems.\n- **ClickBench: +1 query** (Q34). With Q40 from #28, the ratchet goes to\n36 of 43.\n- **Docs:** `docs/sql-subset.md` (grammar, select list, constants,\npositions, result names, exit codes, ClickBench table) and\n`docs/architecture.md`; a fuzz seed.\n\n**Maintainer sign-off needed:** `tests/data/clickbench_status.json` is\nan \"Ask a human first\" path. It adds Q34, and no approved plan covers\nit; please approve it explicitly in the review.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS; 100% tests passed out of 1089 (ci, asan, coverage, ci-gcc); tidy clean; Coverage gate: PASS; fuzz-smoke 2/2\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3324354851 queries=20000 failed=0 unsupported=0\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # host, all 100 files\n100% tests passed out of 6 (14m39s wall; peak RSS 22.5 GB for one process, DuckDB included)\n```\n\n- **Binder:**\n- constant types and names (`-2147483648`, `-9223372036854775808`,\n`9223372036854775808`, `007`, `-0`, strings, dates);\n  - positions in every shape;\n  - GROUP BY of constants only;\n  - the one-row case with an ORDER BY aggregate;\n  - 11 error cases.\n- **Parser and unparser:** literals as select, GROUP BY and ORDER BY\nitems, with round-trips. The token property test now also accounts for\nliterals and IN lists.\n- **Exec:**\n- `ProjectOperator` with constants mixed with columns under a partial\nselection, constants only, and malformed input;\n  - `GroupAggregateOperator` without keys (rows and no rows).\n- **EXPLAIN** of constants and positions.\n- **`.slt` (`tests/slt/cases/positions/`):** 20 queries plus errors,\nwith expectations and exact types written by DuckDB. They cover every\nconstant kind, empty tables, positions in grouped, projected, star and\nglobal queries, and GROUP BY constants.\n- **Review:** the `reviewer` agent found that integer constants were\ntyped and named by their text: `-2147483648` was INTEGER and `007` was\nnamed `007`, while DuckDB gives BIGINT and `7`. That is fixed and\ntested. It also asked for a `ProjectOperator` unit test, which is added.\n- **Also found by the random test before commit:** `SELECT <constant>\nFROM t ORDER BY COUNT(*)` must be one row. The generator had also\ntreated a constants-only select list as a single row.\n- A scan of all tracked files for ClickBench query text finds nothing.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ratchet needs your approval in this review\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs and ran the verification above; the `reviewer` subagent\nreviewed the diff.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T01:58:35+03:00",
          "tree_id": "2ee071a0ba8f42a175352cf9cb1c3e359e67bfb7",
          "url": "https://github.com/ydb-campus/antb1/commit/de906d019354fdf222db973fafa4573f497ea44b"
        },
        "date": 1790550036256,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3141.3537789934935,
            "unit": "ns/iter",
            "extra": "iterations: 217161\ncpu: 3140.5799199672133 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 81620.7912301152,
            "unit": "ns/iter",
            "extra": "iterations: 7731\ncpu: 81619.66679601606 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 209920.89572275503,
            "unit": "ns/iter",
            "extra": "iterations: 3203\ncpu: 209919.05088979081 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 418081.0115340367,
            "unit": "ns/iter",
            "extra": "iterations: 1734\ncpu: 418068.8742791235 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 334490.9085158267,
            "unit": "ns/iter",
            "extra": "iterations: 2055\ncpu: 334384.503163017 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1949940.9665738032,
            "unit": "ns/iter",
            "extra": "iterations: 359\ncpu: 1949243.810584959 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 168.24716824999797,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 168.2366617500002 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.474197836734563,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 13.473456367346943 ms\nthreads: 1"
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
          "id": "2d50fd7c9611de088090ac4ad918e8332a1d2002",
          "message": "feat(sql,plan): having (#30)\n\n## Summary\n\nThis PR adds `HAVING`, the last of the three non-expression features\n(after `IN` in #28 and positions and constants in #29).\n\n**What HAVING accepts.** A conjunction of `<aggregate or column> <op>\nliteral`, using the operators `WHERE` has: comparisons, `[NOT] LIKE` and\n`[NOT] IN`. A literal-first comparison is normalized.\n\n**Name resolution** (DuckDB 1.5.5 semantics):\n- An aggregate call reuses an equal select-list aggregate; otherwise it\nis computed as a hidden aggregate.\n- A column is a `GROUP BY` key when the table column of that name is\none. Otherwise it is the last select alias with that name, which may\nname a key or an aggregate.\n- An alias of a constant is unsupported (exit code 4). Any other column\nis a bind error.\n- `HAVING` without `GROUP BY` makes the query one aggregate row, which\nit may filter out. `SELECT a FROM t HAVING a > 0` is a bind error.\n\n**Literal typing.** Literals are typed against the operand's type, as in\n`WHERE`: `COUNT` is BIGINT, an integer `SUM` is HUGEINT, `AVG` is\nDOUBLE. `MIN` and `MAX` of a FLOAT column compare in FLOAT, as DuckDB\ntypes them FLOAT. `SUM` and `AVG` of a FLOAT column compare as DOUBLE.\n\n**Plan.** A `Filter` over the `GroupAggregate` or `Aggregate`, below the\n`Sort`. A `Project` drops hidden aggregates. The existing Filter\noperator, optimizer rules and EXPLAIN handle it unchanged.\n\n**The \"unsupported\" example moves to JOIN.** HAVING was the repository's\nstandard example of unsupported SQL. That role moves to `JOIN`:\n- the CLI and session tests and the `unsupported` goldens;\n- the harness canary query, and `Feature::kJoin`, which replaces\n`kHaving` as the never-generated marker.\n\n**Harness.**\n- The random generator writes `HAVING` on keys, on aggregates with exact\nvalues, and on their aliases.\n- New `.slt` cases in `tests/slt/cases/having/`, whose expectations were\nwritten by DuckDB.\n- New metamorphic relations: `HAVING` and its complement partition the\ngroups (a new `FirstIsUnionOfRest` check), and the same groups are\nfiltered whatever the batch size or file layout.\n\n**Docs.** `docs/sql-subset.md` covers the grammar, binding, error order,\nsemantics and exit codes. Divergence D13 is extended: DuckDB compares a\nHUGEINT `SUM` with a decimal in DECIMAL(38, s), so it can fail with a\nconversion error where antb1 compares exactly.\n\n**ClickBench.** `HAVING` alone unlocks no ClickBench query (Q27 and Q28\nalso need expressions), so the ratchet is unchanged: 36 of 43.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\nexit 0\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2213364440 queries=20000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0, ratchet unchanged\n100% tests passed out of 6\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # all 100 files\n100% tests passed out of 6     (peak RSS 21 GB)\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: parser, property, unparser, binder,\nEXPLAIN, `.slt`, metamorphic and the generator\n- [x] Docs updated: `docs/sql-subset.md`, `docs/architecture.md`,\n`docs/recipes/write-slt-test.md`, notes in ADR 0008 and 0010 (statuses\nunchanged)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: none are changed\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, ran the verification, and ran a read-only review with the\n`reviewer` agent, which found no P0 or P1 problems.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T03:01:06+03:00",
          "tree_id": "ee9d9dbaf0b380f517b31a4e40cfd2091551c0ee",
          "url": "https://github.com/ydb-campus/antb1/commit/2d50fd7c9611de088090ac4ad918e8332a1d2002"
        },
        "date": 1790553795215,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3216.3876652793806,
            "unit": "ns/iter",
            "extra": "iterations: 217208\ncpu: 3215.987799712718 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85048.62467932419,
            "unit": "ns/iter",
            "extra": "iterations: 7796\ncpu: 85037.17111339148 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222281.45012706175,
            "unit": "ns/iter",
            "extra": "iterations: 3148\ncpu: 222252.5965692503 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 439973.09829867637,
            "unit": "ns/iter",
            "extra": "iterations: 1587\ncpu: 439738.20730938873 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 348377.6129675838,
            "unit": "ns/iter",
            "extra": "iterations: 2005\ncpu: 348363.18054862827 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2114119.688821725,
            "unit": "ns/iter",
            "extra": "iterations: 331\ncpu: 2112991.2326283986 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 189.2244439999994,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 189.13423066666664 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.882871382978545,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.876427212765947 ms\nthreads: 1"
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
          "id": "1654767111988c5c279e6144833848c456013acb",
          "message": "feat(sql): parse scalar expressions (#31)\n\n## Summary\n\nThis is PR 1 of the approved 5-PR plan for scalar expressions (the\ntarget is ClickBench 43/43). The parser now reads expressions as trees,\nand the binder still answers exactly what it did before. The ClickBench\nratchet is unchanged at 36/43.\n\n**Parser (`src/sql`)**\n- `sql::Expr` is a variant of the node kinds, with deep-copying `Box<T>`\nchildren.\n- It is parsed by precedence climbing: `OR` < `AND` < `NOT` <\ncomparisons with `[NOT] LIKE` and `[NOT] IN` (which do not chain) < `+\n-` < `* / // %` < unary `-`.\n- The grammar also covers parentheses, function calls, `CASE` (searched\nand simple), `EXTRACT(field FROM x)`, and aggregates over any argument.\n- **Depth limit (256 levels).** Every level of a tree counts, so\nparsing, copying, comparing, unparsing and destroying stay bounded on\nadversarial input. The top-level `AND` chain of WHERE and HAVING is\ncollected in a loop into conjuncts and does not count, so a\n5,000-conjunct predicate still parses.\n- `ToSql` prints the fewest parentheses the precedence needs, and\n`Parse(ToSql(x)) == x` still holds. The property test now generates\nrandom expression trees and accounts for every token.\n\n**Binder (`src/plan`)**\n- `sql::AsComparison` / `AsHavingComparison` give the normalized form of\na simple condition, with literal-first comparisons mirrored as before.\n- Every other expression is `kUnsupported` (exit code 4) at its first\ntoken. The check runs in a pre-pass before any name is resolved, with\nthe messages the parser used to give; about 50 cases moved from the\nparser tests to the binder tests.\n- New: parentheses only group, so `(a)`, `SUM((a))` and `WHERE (a = 1\nAND b = 2)` are answered. They are checked against DuckDB in\n`tests/slt/cases/where/parentheses.slt`.\n- DuckDB-only argument syntax inside function calls stays exit code 4:\n`position('a' IN s)`, `substring(s FROM 1)`, `try_cast(x AS t)` and\n`FILTER`. So do postfix `NOT` and `NOT NULL`.\n\n**Docs:** `docs/sql-subset.md` (grammar, and what the binder answers\ntoday), `docs/architecture.md`, and an update note in ADR 0008. Fuzz:\ndictionary entries and a corpus seed for expressions.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage (sql floors kept), fuzz-smoke, ci-gcc\nexit 0 (100% tests passed out of 1155; Coverage gate: PASS)\n$ ANTB1_DIFF_COUNT=5000 pixi run diff-random\nDIFF: PASS seed=1510356307 queries=5000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0: the ratchet is unchanged\n100% tests passed out of 6\n```\n\nThe data test did not run over all 100 files: this PR changes parsing,\nnot execution.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: parser (precedence, CASE, EXTRACT,\nfunctions, conjunct splitting, depth limit, error cases), property test\nover random expression trees, unparser, AST helpers, binder rejections\nand parentheses, `.slt`\n- [x] Docs updated: `docs/sql-subset.md`, `docs/architecture.md`, ADR\n0008 (a note; its status is unchanged)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: none are changed\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests and ran the verification. The `reviewer` agent found no\nwrong results and no stack overflow on 100k-level inputs under ASan; it\nraised three P1s (EXTRACT string fields breaking the round trip, DuckDB\nfunction-argument syntax and postfix NOT exiting 1 instead of 4), all\nfixed with tests.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T06:49:04+03:00",
          "tree_id": "f6603534d89dec302c15b144ca8d3fccc01ab643",
          "url": "https://github.com/ydb-campus/antb1/commit/1654767111988c5c279e6144833848c456013acb"
        },
        "date": 1790567461860,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3993.036372800024,
            "unit": "ns/iter",
            "extra": "iterations: 176588\ncpu: 3992.53056266564 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94569.64588414824,
            "unit": "ns/iter",
            "extra": "iterations: 6560\ncpu: 94551.98399390245 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 124828.48788742402,
            "unit": "ns/iter",
            "extra": "iterations: 5614\ncpu: 124795.58371927323 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 484899.30055401335,
            "unit": "ns/iter",
            "extra": "iterations: 1444\ncpu: 484643.2922437674 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 455379.8639322926,
            "unit": "ns/iter",
            "extra": "iterations: 1536\ncpu: 455333.54361979145 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2297211.6568626994,
            "unit": "ns/iter",
            "extra": "iterations: 306\ncpu: 2296819.202614382 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 235.08171733333447,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 235.05696366666663 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 15.136970782608811,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 15.134891695652186 ms\nthreads: 1"
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
          "id": "1f116c0bd3faa4a658ae9a30f178c182959c4783",
          "message": "feat(plan,exec): arithmetic expressions (#32)\n\n## Summary\n\nThis is PR 2 of the approved plan for scalar expressions. It adds\narithmetic: `+ - * / // %` and unary `-`, in the select list, aggregate\narguments, `GROUP BY`, `HAVING`, `ORDER BY` and `WHERE`. ClickBench goes\nfrom 36 to **38 of 43** (Q29, Q35); the maintainer signed off on the\nratchet change.\n\n**Typing** follows DuckDB 1.5.5:\n- An integer literal that fits the other operand's type takes that type\n(`smallint + 1` is SMALLINT).\n- Two integer types give the wider one; USMALLINT with SMALLINT gives\nBIGINT.\n- DOUBLE wins, and `/` is always DOUBLE.\n- The names are DuckDB's: `(a + 1)`, `sum((a + 1))`.\n- Unsupported (exit code 4): DECIMAL arithmetic (a decimal literal with\nan integer), DATE arithmetic, negating a USMALLINT, `//` and `%` of\nHUGEINT, and arithmetic on FLOAT columns (D11).\n\n**Semantics:**\n- Integer `+ - *` and negation compute in the result type, and an\noverflow is an execution error, as in DuckDB. They use Arrow's checked\nkernels.\n- `/` divides in DOUBLE (`x / 0` is ±inf).\n- `//` truncates and `%` takes the sign of the dividend. Both are NULL\nfor a zero divisor. They use own loops, as does HUGEINT arithmetic,\nsince Arrow's decimal kernels widen past 38 digits.\n- Two optimizer rewrites of DuckDB are reproduced, so that an overflow\nfails exactly the queries DuckDB fails:\n- `SUM(x + c)` without `GROUP BY` becomes `SUM(x) + c * COUNT(x)` in\nHUGEINT.\n- In `WHERE`, `x ± c <op> k`, `c - x <op> k` and `x * c <op> k` (when c\ndivides k) move the constant to the literal. This applies for signed\nintegers while the constants fit the type.\n- Divergence D14 records the one remaining difference: a comparison\nantb1 folds to never-true answers where DuckDB overflows.\n\n**Plan** (ADR 0012):\n- A bound `plan::Expr` tree and a new `Compute` node, which appends\ncomputed columns.\n- The node sits in three places: `WHERE` operands before the filter on\nthem; aggregate arguments and `GROUP BY` expressions after that filter;\nselect, `HAVING` and `ORDER BY` expressions above the aggregation.\n- A select or `ORDER BY` expression equal to a `GROUP BY` expression is\nthat key.\n- Inside `ORDER BY` and `HAVING` expressions a select alias is the\nfallback after table columns, as in DuckDB.\n- `WHERE`/`HAVING` can compare two operands, with a new column-to-column\npredicate.\n- The optimizer prunes unused computed columns and moves `Limit` below\n`Compute`.\n\n**Harness:**\n- The random generator writes arithmetic that cannot overflow, using\neach integer column's data range across every file.\n- New `.slt` cases in `tests/slt/cases/expressions/arithmetic.slt`\n(written by DuckDB), and metamorphic relations (the sum rewrite,\nshifting a `WHERE` literal, expression keys across batch sizes and\nfiles).\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage (floors kept), fuzz-smoke, ci-gcc\nexit 0\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random      (run three times over the PR's commits)\nDIFF: PASS seed=2569736335 queries=20000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0, ratchet with Q29 and Q35\n100% tests passed out of 6\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # all 100 files\n100% tests passed out of 6     (16:53, peak RSS 22 GB)\n$ antb1 bench (release build, all 100 files, 2 tries): Q29 0.56 s, Q35 8.6 s\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: evaluator and column-to-column filter\n(exec), typing, rewrites, constant moving, scopes, aliases and errors\n(binder), Compute pruning and Limit (optimizer), FLOAT columns (engine),\n`.slt`, metamorphic relations, the generator\n- [x] Docs updated: `docs/sql-subset.md` (binding, semantics, D9, D11,\nD14, ClickBench table), `docs/architecture.md`, ADR 0012\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `tests/data/clickbench_status.json` (+29, +35) was\napproved by @hor911\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, and ran the verification. The `reviewer` agent found two\nP0s (a global aggregate with a computed HAVING operand; BIGINT against\nDOUBLE beyond 2^53) and two divergences from DuckDB's optimizer (the\nscope of the sum rewrite; constant moving in WHERE). All are fixed with\ntests (second commit).\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T11:09:36+03:00",
          "tree_id": "24f52adca8e1b15db84c8f9a5d5b66f6a8db3060",
          "url": "https://github.com/ydb-campus/antb1/commit/1f116c0bd3faa4a658ae9a30f178c182959c4783"
        },
        "date": 1790583113243,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4175.3450004180195,
            "unit": "ns/iter",
            "extra": "iterations: 167394\ncpu: 4173.962555408199 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85016.41661333673,
            "unit": "ns/iter",
            "extra": "iterations: 7813\ncpu: 84979.30231665172 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222038.38922345956,
            "unit": "ns/iter",
            "extra": "iterations: 3155\ncpu: 221944.75435816176 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 440246.1915829071,
            "unit": "ns/iter",
            "extra": "iterations: 1592\ncpu: 440083.73429648246 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 348180.7220288398,
            "unit": "ns/iter",
            "extra": "iterations: 2011\ncpu: 348081.3635007462 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2112912.466463482,
            "unit": "ns/iter",
            "extra": "iterations: 328\ncpu: 2112406.256097562 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 197.33842533333737,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 197.31232800000006 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.364028142856986,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.362616755102037 ms\nthreads: 1"
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
          "id": "b95d193aac4cbc362a4beccceb22862584d06d10",
          "message": "feat(plan,exec): strlen and regexp_replace (#33)\n\n## Summary\n\nThis is PR 3 of the approved plan for scalar expressions. It adds\n`strlen(varchar)` and `regexp_replace(varchar, 'pattern',\n'replacement')` in every clause. ClickBench goes from 38 to **40 of 43**\n(Q27, Q28); the maintainer signed off on the ratchet change.\n\n**Binding** follows DuckDB 1.5.5:\n- `strlen` is BIGINT; `regexp_replace` is VARCHAR and takes two string\nliterals.\n- Names are DuckDB's: `strlen(URL)`, `regexp_replace(s, '^(.)', '\\1')`.\n- A wrong arity, a wrong type or a non-literal argument is a bind error.\n- Unsupported (exit code 4): other functions, DuckDB's fourth argument\n(options), `\\Q` in a pattern, and `\\8` or `\\9` in a replacement.\n\n**Semantics:**\n- `strlen` counts bytes (Arrow `binary_length`).\n- `regexp_replace` replaces the first match with RE2 in UTF-8 mode, like\nDuckDB.\n- An invalid replacement leaves the text unchanged, as in DuckDB (it\nignores `RE2::Replace`'s failure). An invalid pattern is an execution\nerror.\n- Arrow's single-replacement path re-matches the pattern on the matched\nsubstring alone, so `\\b`, `\\B`, `^` and `$` saw the wrong neighbours.\nThe evaluator therefore runs the pattern as `^(\\C*?)(pattern)`, RE2's\nown unanchored search, with the replacement's groups shifted by two. The\npattern is checked alone first, so a pattern that is only valid once\nwrapped still fails.\n- Divergence D15: bytes that are not UTF-8. DuckDB cannot read such a\nVARCHAR at all; antb1 answers.\n\n**Plan:** a `FunctionExpr` node in `plan::Expr` (ADR 0012 note), handled\nby every visitor.\n\n**Harness:**\n- The random generator writes both functions, with context-dependent\npatterns.\n- New `tests/slt/cases/expressions/strings.slt` (expected results\nwritten by DuckDB), and two metamorphic relations: an insertion adds one\nbyte per value; `regexp_replace` keys are invariant across batch sizes\nand files.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage (floors kept), fuzz-smoke, ci-gcc\nexit 0                         (on the first commit; after the review fixes: check and tidy exit 0)\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3304376479 queries=20000 failed=0 unsupported=0\n$ ANTB1_DIFF_COUNT=5000 pixi run diff-random   # after the review fixes\nDIFF: PASS seed=3870478056 queries=5000 failed=0 unsupported=0\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # all 100 files\n100% tests passed out of 6\n$ antb1 bench (release build, all 100 files, 2 tries, lukewarm): Q27 15.7 s, Q28 326 s\n  (Q28 on one file: 2.0 s vs DuckDB with threads=1 1.75 s; the regex is most of it)\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: evaluator (exec), binding, names and\nerrors (binder), `.slt`, metamorphic relations, the generator\n- [x] Docs updated: `docs/sql-subset.md` (grammar notes, binding,\nsemantics, exit codes, D15, ClickBench table), `docs/architecture.md`,\nADR 0012\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `tests/data/clickbench_status.json` (+27, +28) was\napproved by @hor911\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, and ran the verification. The random differential test\nfound that DuckDB leaves the text unchanged for an invalid replacement.\nThe `reviewer` agent found a P0 (Arrow's single replacement re-matches\non the substring, so context assertions went wrong), test queries too\nclose to ClickBench's, and the options argument's exit code. All are\nfixed with tests.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T14:14:47+03:00",
          "tree_id": "c4890118afc2c83ff19c80a46b75ae80eabfa647",
          "url": "https://github.com/ydb-campus/antb1/commit/b95d193aac4cbc362a4beccceb22862584d06d10"
        },
        "date": 1790594212139,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3812.482221771964,
            "unit": "ns/iter",
            "extra": "iterations: 182611\ncpu: 3812.234586087366 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 97303.66351848726,
            "unit": "ns/iter",
            "extra": "iterations: 6247\ncpu: 97215.83704178008 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 105020.375858385,
            "unit": "ns/iter",
            "extra": "iterations: 6553\ncpu: 104993.65328857012 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 352456.82202111254,
            "unit": "ns/iter",
            "extra": "iterations: 1989\ncpu: 352384.49572649575 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 437306.1824953377,
            "unit": "ns/iter",
            "extra": "iterations: 1611\ncpu: 437195.0775915577 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2548314.8690909212,
            "unit": "ns/iter",
            "extra": "iterations: 275\ncpu: 2547775.2545454535 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 188.01486174999837,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 187.9607162500001 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.405464538461587,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.403133173076927 ms\nthreads: 1"
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
          "id": "85cb37ded422800d473a584b76ad71f580518ee1",
          "message": "build(deps): update pixi.lock (#34)\n\n# Explicit dependencies\n\n|Dependency|Before|After|Change|Environments|\n|-|-|-|-|-|\n\n|[ccache](https://prefix.dev/channels/conda-forge/packages/ccache)|4.14|4.14.1|Patch\nUpgrade|default on *all platforms*<br/>gcc on linux-64|\n\n|[tombi](https://prefix.dev/channels/conda-forge/packages/tombi)|1.5.5|1.5.8|Patch\nUpgrade|lint on *all platforms*|\n\n# Implicit dependencies\n\n|Dependency|Before|After|Change|Environments|\n|-|-|-|-|-|\n\n|[platformdirs](https://prefix.dev/channels/conda-forge/packages/platformdirs)|4.11.15|4.12.0|Minor\nUpgrade|lint on *all platforms*|\n\n|[virtualenv](https://prefix.dev/channels/conda-forge/packages/virtualenv)|21.12.1|21.13.0|Minor\nUpgrade|lint on *all platforms*|\n\n|[filelock](https://prefix.dev/channels/conda-forge/packages/filelock)|4.0.3|4.0.4|Patch\nUpgrade|lint on *all platforms*|\n\n|[identify](https://prefix.dev/channels/conda-forge/packages/identify)|2.6.19|2.6.20|Patch\nUpgrade|lint on *all platforms*|\n\n[^1]: **Bold** means explicit dependency.\n[^2]: Dependency got downgraded.\n\n\nGenerated by `.github/workflows/pixi-lock-update.yml` with pixi 0.81.0.\nCI runs as for any PR;\na maintainer reviews the diff, approves and squash-merges.\n\nCo-authored-by: antb1-bot[bot] <334225516+antb1-bot[bot]@users.noreply.github.com>",
          "timestamp": "2026-09-28T16:34:25+03:00",
          "tree_id": "f278883f1650bf62ba39be5373922729f8a5e254",
          "url": "https://github.com/ydb-campus/antb1/commit/85cb37ded422800d473a584b76ad71f580518ee1"
        },
        "date": 1790602604962,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4241.867014451048,
            "unit": "ns/iter",
            "extra": "iterations: 166770\ncpu: 4241.52359537087 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84243.37256908907,
            "unit": "ns/iter",
            "extra": "iterations: 7816\ncpu: 84238.5475946776 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222098.84051997744,
            "unit": "ns/iter",
            "extra": "iterations: 3154\ncpu: 222067.878883957 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 442047.06632975617,
            "unit": "ns/iter",
            "extra": "iterations: 1583\ncpu: 441998.20530638035 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 402280.9942627669,
            "unit": "ns/iter",
            "extra": "iterations: 1743\ncpu: 402237.54274239804 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2131831.6341463304,
            "unit": "ns/iter",
            "extra": "iterations: 328\ncpu: 2131462.499999999 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 227.04129700000428,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 227.01518766666655 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.42510531249989,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.42427685416664 ms\nthreads: 1"
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
          "id": "62e415a50db9ba86ad176f84fa9cea2887378bf2",
          "message": "feat(plan,exec): boolean conditions and case (#35)\n\n## Summary\n\nThis is PR 4 of the approved plan for scalar expressions. It adds `AND`,\n`OR` and `NOT` as general conditions (in `WHERE`, `HAVING` and `CASE\nWHEN`) and `CASE` in both forms, in every clause. ClickBench goes from\n40 to **41 of 43** (Q39); the maintainer signed off on the ratchet\nchange.\n\n**Conditions:**\n- Each comparison, `LIKE` and `IN` inside `OR`/`NOT` is bound and folded\nexactly as a `WHERE` comparison.\n- A comparison folded to never or always true keeps its operand, so it\nis still NULL for NULL: `NOT (smallint_col = 1.5)` rejects NULL rows, as\nin DuckDB.\n- A `WHERE`/`HAVING` conjunct with `OR` or `NOT` is computed as a\nBOOLEAN column and filtered with a new `IS TRUE` predicate, so `Filter`\nstays a conjunction over columns.\n- `AND`/`OR` follow three-valued logic and compute a later argument only\nfor rows still undecided, as DuckDB does in the written order.\n- Divergence D16 records where DuckDB differs: it reorders conjunctions\nby its cost model and computes a `NOT`'s argument for every row, so an\noverflow inside a condition can fail in one engine and not the other.\n\n**CASE** (typed as DuckDB 1.5.5 types it):\n- The values take their common type. An integer literal takes the\nothers' integer type when it fits. USMALLINT with SMALLINT gives\nINTEGER, unlike arithmetic. A string literal takes VARCHAR, or DATE next\nto a DATE.\n- VARCHAR or DATE with a number is a bind error, as in DuckDB.\n- Unsupported (exit code 4): a string literal next to numbers (DuckDB\ncasts it), a decimal literal without a DOUBLE value, and a FLOAT column\nvalue (D11).\n- Each condition is computed only for the rows no earlier branch took,\nand each value only for the rows its branch answers, as in DuckDB. So\n`CASE WHEN x < 100 THEN x * 300 END` never overflows on other rows.\n- Names match DuckDB: `CASE WHEN ((a = 1)) THEN (b) ELSE NULL END`.\nInside them, NOT is folded into comparisons and IN, `<>` prints as `!=`,\nchains are flattened, and a DATE literal prints as `CAST('..' AS\n\"DATE\")`.\n\n**Plan and exec:**\n- `LogicalType::kBoolean` is internal, never a table or result column.\n- New `plan::Expr` nodes: `PredicateExpr`, `BoolExpr` and `CaseExpr`\n(ADR 0012 note).\n- The filter's per-predicate evaluation becomes\n`exec::PredicateEvaluator`, shared by `Filter` and `Compute`.\n\n**Harness:**\n- The generator writes compound conditions (`(A OR B AND C)`, `NOT (A)`)\nin `WHERE`/`HAVING` and `CASE` in select items and aggregate arguments.\nNew features: `case` and `boolean_expressions`.\n- New `.slt` files `expressions/boolean.slt` and `expressions/case.slt`\n(expected results written by DuckDB).\n- Metamorphic relations: OR/AND count like union/intersection; NOT\nsplits non-NULL values (folded comparisons included); CASE WHEN p counts\nwhat WHERE p keeps; CASE keys are invariant across batch sizes and\nfiles.\n- The CLI golden `unsupported_where` now uses `IS NULL`, since OR is\nsupported.\n\n**Also:** the data tests' TIMEOUT is raised from 1800 s to 3600 s in\n`tests/data/CMakeLists.txt`. The 100-file status run now answers Q39 on\nboth engines and took over 30 minutes; the hits_0 CI run takes about 5\nminutes.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage (floors kept), fuzz-smoke, ci-gcc\nexit 0                         (final tree)\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=1322124570 queries=20000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0, ratchet with Q39\n100% tests passed out of 6\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # all 100 files\n100% tests passed out of 6\n$ antb1 bench (release build, all 100 files, 2 tries, lukewarm): Q39 24.7 s, 26.3 s\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: binder (conditions, folding under NOT,\nCASE typing, names, errors), exec (three-valued and lazy AND/OR, lazy\nCASE, filter IS TRUE), engine (FLOAT in CASE), `.slt`, metamorphic\nrelations, the generator\n- [x] Docs updated: `docs/sql-subset.md` (what works, binding,\nsemantics, exit codes, D16, ClickBench table), `docs/architecture.md`,\nADR 0012\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `tests/data/clickbench_status.json` (+39) was\napproved by @hor911\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, and ran the verification. The `reviewer` agent (two\npasses) found:\n- eager AND/OR (DuckDB evaluates later arguments only on undecided\nrows);\n  - condition names differing from DuckDB's (including double NOT);\n  - a string literal next to numbers wrongly reported as a bind error;\n  - an untested FLOAT path.\n\n  All are fixed with tests.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T17:48:37+03:00",
          "tree_id": "4b6a877e13e39bb26ec44c12bd7b3256607bda26",
          "url": "https://github.com/ydb-campus/antb1/commit/62e415a50db9ba86ad176f84fa9cea2887378bf2"
        },
        "date": 1790607046259,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3948.398057706787,
            "unit": "ns/iter",
            "extra": "iterations: 177934\ncpu: 3947.882051772006 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94358.25275070786,
            "unit": "ns/iter",
            "extra": "iterations: 6362\ncpu: 94351.9860106885 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 124723.55634807466,
            "unit": "ns/iter",
            "extra": "iterations: 5608\ncpu: 124705.24304564911 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 486008.0984528821,
            "unit": "ns/iter",
            "extra": "iterations: 1422\ncpu: 485961.3699015471 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 455418.7061118374,
            "unit": "ns/iter",
            "extra": "iterations: 1538\ncpu: 455232.7373211963 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2254276.8778135143,
            "unit": "ns/iter",
            "extra": "iterations: 311\ncpu: 2253641.990353699 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 197.6596876666671,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 197.65763600000005 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 15.0742670638298,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 15.072331680851061 ms\nthreads: 1"
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
          "id": "062d58155c42234714057ed76d62418f18bde1a0",
          "message": "feat(plan,exec): timestamps (#36)\n\n## Summary\n\nThis is PR 5, the last of the approved plan for scalar expressions. It\nadds a computed TIMESTAMP type and the timestamp functions `toDateTime`,\n`EXTRACT` and `date_trunc`. ClickBench goes from 41 to **43 of 43**\n(Q18, Q42); the maintainer signed off on the ratchet change.\n\n**`toDateTime(t)`** is ClickBench's own DuckDB macro `epoch_ms(t *\n1000)`, taken from `duckdb-parquet/create.sql` at the pinned commit\n`5a56398`.\n- The binder binds it as exactly that expression: `t * 1000` is typed\nand checked as arithmetic, so a SMALLINT `t` above 32 overflows as in\nDuckDB.\n- `epoch_ms` fails outside DuckDB's TIMESTAMP range (290309-12-22 BC to\n294247 AD).\n- The test oracle defines the same one-line macro.\n\n**`EXTRACT(field FROM x)` and `date_trunc('unit', x)`** take a TIMESTAMP\nor a DATE.\n- Fields: year, month, day, hour, minute, second. Units: year, quarter,\nmonth, week, day, hour, minute, second (case-insensitive).\n- EXTRACT gives the civil field in astronomical years; date_trunc\nfloors, before 1970 too, with weeks starting on Monday.\n- Both are computed in 64-bit calendar arithmetic over the whole range.\nArrow's temporal kernels keep the year in 16 bits and wrap past year\n32767.\n- An infinite DATE gives NULL (EXTRACT) or stays infinite (date_trunc).\nA result outside the range fails, as in DuckDB.\n- Names match DuckDB: `todatetime(x)`, `main.date_part('minute', x)`,\n`date_trunc('Hour', x)`.\n\n**TIMESTAMP** is `timestamp[us]` and exists only as a computed value; a\nParquet timestamp column stays unsupported.\n- It sorts, groups and aggregates (MIN, MAX, COUNT, COUNT(DISTINCT)).\n- It prints as DuckDB prints it: a trimmed fraction, `(BC)`, ±infinity.\n- Unsupported (exit code 4): TIMESTAMP literals and comparisons with\nliterals, DATE vs TIMESTAMP, TIMESTAMP arithmetic, and mixed CASE\nvalues.\n- D6 now also covers `AVG` of a TIMESTAMP.\n\n**Harness:**\n- The oracle macro, a TIMESTAMP reader and `CanonicalTimestamp`.\n- The generator writes `toDateTime`, `EXTRACT` and `date_trunc` over\ninteger columns whose values times 1000 fit their type, and over DATE\ncolumns. New feature: `timestamps`.\n- `tests/slt/cases/expressions/timestamps.slt` (expected results written\nby DuckDB).\n- Metamorphic relations: EXTRACT(minute) is `(t // 60) % 60`, days per\ndate_trunc('day') equal distinct `t // 86400`, and date_trunc keys are\ninvariant across batch sizes and files.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage (floors kept), fuzz-smoke, ci-gcc\nexit 0                         (final tree; plan branch coverage 89.90%, floor 89.5%)\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3051161949 queries=20000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0, ratchet 43 of 43\n100% tests passed out of 6\n$ ANTB1_HITS_FILES=\"$HOME/.cache/antb1/clickbench/full/hits_*.parquet\" pixi run test-data   # all 100 files\n100% tests passed out of 6\n$ antb1 bench (release build, all 100 files, 2 tries, lukewarm): Q18 27.8 s, 27.2 s; Q42 2.2 s, 2.2 s\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: binder (types, names, desugaring, errors),\nplan (structure, renumbering and names of condition, CASE and function\nnodes), exec (epoch range, fields and floors incl. large years, BC, DATE\ninfinities), engine formatting, harness canonical text, `.slt`,\nmetamorphic relations, the generator\n- [x] Docs updated: `docs/sql-subset.md` (what works, binding, types,\nsemantics, D6, ClickBench table: all pass), `docs/architecture.md`, ADR\n0012\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006). The oracle's `toDateTime`\nmacro is ClickBench setup, not data\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `tests/data/clickbench_status.json` (+18, +42) was\napproved by @hor911\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, and ran the verification. The `reviewer` agent found:\n  - Arrow's temporal kernels wrapping years past 32767;\n  - an asymmetric lower bound of DuckDB's TIMESTAMP range;\n  - out-of-range and infinite DATE inputs.\n\nAll are fixed by computing fields and floors in 64-bit calendar\narithmetic, with tests. A second pass compared over 100,000 values\nacross the whole range with DuckDB.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T19:24:19+03:00",
          "tree_id": "45ce919212446dd2ba5de00e86793f47c940a199",
          "url": "https://github.com/ydb-campus/antb1/commit/062d58155c42234714057ed76d62418f18bde1a0"
        },
        "date": 1790612794878,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4275.195731483753,
            "unit": "ns/iter",
            "extra": "iterations: 166381\ncpu: 4275.09597249686 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84059.5344186637,
            "unit": "ns/iter",
            "extra": "iterations: 7801\ncpu: 84045.20599923088 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 221774.1715732826,
            "unit": "ns/iter",
            "extra": "iterations: 3159\ncpu: 221703.80816714145 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 437635.5235109664,
            "unit": "ns/iter",
            "extra": "iterations: 1595\ncpu: 437615.11410658294 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 401078.01776504633,
            "unit": "ns/iter",
            "extra": "iterations: 1745\ncpu: 401065.7667621777 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2122798.012195122,
            "unit": "ns/iter",
            "extra": "iterations: 328\ncpu: 2122714.8475609752 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 188.1339699999991,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 188.1164019999999 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.324408000000087,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.323975142857142 ms\nthreads: 1"
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
          "id": "21fc8ac072f5df5d99629066859488ea9a0472b9",
          "message": "fix(plan): move constants in having and conditions over aggregates (#37)\n\n## Summary\n\nFollow-up 1 of 3 after the scalar-expressions plan. DuckDB's optimizer\nmoves integer constants in every comparison (`x + c <op> k` becomes `x\n<op> k - c`, and likewise for `-`, `c - x` and `x * c`). antb1 did this\nonly in `WHERE` and in input-scope `CASE WHEN`. In `HAVING`, and in\nconditions over the aggregation (a grouped `CASE WHEN`, `HAVING ... OR\n...`), it computed the arithmetic and failed with an overflow where\nDuckDB answers.\n\nExample, a SMALLINT key with the value 32767: `SELECT x FROM t GROUP BY\nx HAVING x + 30000 > 5` failed with \"Overflow in addition of SMALLINT\";\nnow it answers like DuckDB.\n\nThe fix:\n- `MoveConstants` binds the operand through the scope's own binder\ninstead of always using the input scope.\n- Keys, aggregates and aliases count as operands in the output scope.\n- The call site moves constants in every scope.\n\nThe reviewer of #35 spotted this.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [x] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check\nexit 0\n$ pixi run tidy\nexit 0\n$ ANTB1_DIFF_COUNT=5000 pixi run diff-random\nDIFF: PASS seed=3957990328 queries=5000 failed=0 unsupported=0\n```\n\nChecked by hand against DuckDB 1.5.5:\n- `HAVING` over a key, over `MIN(x)` and over an alias;\n- `HAVING ... OR ...`;\n- `30000 - x`;\n- `x * 2 = 200`;\n- a grouped `CASE WHEN x + 30000 > 5`.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change:\n`plan.BinderTest.HavingMovesConstantsLikeDuckDb`, three `.slt` cases in\n`expressions/arithmetic.slt` (answered by DuckDB, over a 32767 key)\n- [x] Docs updated: `docs/sql-subset.md` (constant moving in every\ncomparison)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the fix\nand the tests and ran the verification.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-28T22:16:55+03:00",
          "tree_id": "fd91a35822fa8aeb5dddeab74aa1ff1d135eff9e",
          "url": "https://github.com/ydb-campus/antb1/commit/21fc8ac072f5df5d99629066859488ea9a0472b9"
        },
        "date": 1790623162051,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4177.693498099386,
            "unit": "ns/iter",
            "extra": "iterations: 166782\ncpu: 4176.507794606132 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 83987.57100362054,
            "unit": "ns/iter",
            "extra": "iterations: 7732\ncpu: 83981.96029487843 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222106.6010149085,
            "unit": "ns/iter",
            "extra": "iterations: 3153\ncpu: 222086.19029495722 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 437091.74219725805,
            "unit": "ns/iter",
            "extra": "iterations: 1602\ncpu: 436928.5362047439 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 402050.5954023001,
            "unit": "ns/iter",
            "extra": "iterations: 1740\ncpu: 401911.44080459746 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2123642.1854103534,
            "unit": "ns/iter",
            "extra": "iterations: 329\ncpu: 2122804.7082066853 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 189.35238899999973,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 189.34201099999993 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.369278877551011,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.367663877551028 ms\nthreads: 1"
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
          "id": "cc150c0e5b0e487737672869143fc2ef0e43ebff",
          "message": "feat(plan,exec): avg of date and timestamp (#38)\n\n## Summary\n\nFollow-up 2 of 3. **Stacked on #37**: review and merge that first; this\nPR's base then moves to `main`.\n\n`AVG` of a DATE or TIMESTAMP is now a TIMESTAMP, as in DuckDB. Before,\nit was a bind error (divergence D6, which is removed).\n- **What is averaged:** the microseconds, summed exactly in 128 bits. A\nDATE counts as its midnight, and its infinities as the TIMESTAMP ones.\n- **Rounding, as DuckDB does it (checked against DuckDB 1.5.5):** `sum /\ncount` truncated, plus one when `2 * remainder > count`. So a positive\naverage rounds to the nearest microsecond with halves down, and a\nnegative one toward zero (0.667 → 1, 0.5 → 0, -0.667 → 0, -1.75 → -1).\n- **Out of range:** a DATE beyond the TIMESTAMP range fails, as DuckDB's\ncast does.\n- **Implementation:** new scalar and grouped states, `TemporalAvgState`\nand `GroupedTemporalAvg`, with a shared internal helper\n`src/exec/temporal_average.h`.\n- **Generator:** it now averages DATE columns and timestamp expressions\nnow and then.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check\nexit 0\n$ pixi run tidy\nexit 0\n$ ANTB1_DIFF_COUNT=5000 pixi run diff-random\nDIFF: PASS seed=1568367802 queries=5000 failed=0 unsupported=0\n```\n\nChecked by hand against DuckDB, grouped and global: pre-1970 and year-1\ndates, NULL groups, averages of `date_trunc`, and ordering by the\naverage.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change:\n`exec.AggregateStateTest.TemporalAverageRoundsLikeDuckDb` (rounding\ntable from DuckDB, merge, NULL, range error, infinity), binder type\ntest, `.slt` cases in `expressions/timestamps.slt` (answered by DuckDB),\nthe generator\n- [x] Docs updated: `docs/sql-subset.md` (result types, AVG semantics,\nD6 removed)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, derived DuckDB's rounding rule from probes, and ran the\nverification.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T00:41:12+03:00",
          "tree_id": "b4d1cd03566c0192327d44bf033c78628e31a940",
          "url": "https://github.com/ydb-campus/antb1/commit/cc150c0e5b0e487737672869143fc2ef0e43ebff"
        },
        "date": 1790631873445,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3721.374836674215,
            "unit": "ns/iter",
            "extra": "iterations: 188274\ncpu: 3720.588604905616 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94301.79377765041,
            "unit": "ns/iter",
            "extra": "iterations: 7007\ncpu: 94299.06350792061 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 103299.67634360453,
            "unit": "ns/iter",
            "extra": "iterations: 6717\ncpu: 103285.43933303558 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 331032.6930740092,
            "unit": "ns/iter",
            "extra": "iterations: 2108\ncpu: 331003.996679317 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 408452.7309941467,
            "unit": "ns/iter",
            "extra": "iterations: 1710\ncpu: 408395.54210526345 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2499157.3118279553,
            "unit": "ns/iter",
            "extra": "iterations: 279\ncpu: 2498880.609318999 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 188.92380850000023,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 188.89464875000007 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.132003018518567,
            "unit": "ms/iter",
            "extra": "iterations: 54\ncpu: 13.130919981481476 ms\nthreads: 1"
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
          "id": "eb6261c5e4a1c86ed29c0f219dd19533f527b02a",
          "message": "feat(sql,plan): timestamp literals (#39)\n\n## Summary\n\nFollow-up 3, part 1 of 2. **Stacked on #38** (itself on #37); merge\nthose first, and the base then moves to `main`.\n\nThis adds TIMESTAMP literals and literal comparisons for TIMESTAMP\noperands. Before, both were unsupported.\n\n- **Syntax:** `TIMESTAMP 'text'`, parsed like `DATE 'text'`.\n- This is a new `sql::Literal::Kind::kTimestamp` in the public AST (the\napproved follow-up plan).\n  - The unparser, fuzz dictionary and property tests are updated.\n- **Text** (divergence D5 is extended):\n- `YYYY-MM-DD`, optionally a space or `T` and `HH:MM` or `HH:MM:SS`,\nthen optionally a fraction (only after the seconds) of up to 9 digits.\n  - Digits past the sixth are truncated, as in DuckDB.\n- DuckDB also takes single-digit fields, spaces and `24:00:00`; antb1\nmakes those a bind error.\n- **Binding:**\n- The constant is TIMESTAMP, named `CAST('...' AS TIMESTAMP)`, also\ninside expression names.\n- A TIMESTAMP operand compares exactly with a TIMESTAMP literal, a\nstring (the timestamp it spells) or a DATE literal (its midnight), `IN`\nlists included.\n  - In `CASE`, a string literal next to TIMESTAMP values is a TIMESTAMP.\n  - ORDER BY a TIMESTAMP literal orders nothing, like DATE.\n  - EXPLAIN prints `TIMESTAMP '...'` constants.\n- Still unsupported (exit code 4): a DATE operand against a TIMESTAMP\nliteral, DATE vs TIMESTAMP operands, and TIMESTAMP arithmetic. A number\nagainst a TIMESTAMP is a bind error.\n- **Generator:** it writes `toDateTime(c) <op> TIMESTAMP '...'` inside\nthe column's data range.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check\nexit 0\n$ pixi run tidy\nexit 0\n$ pixi run coverage\nCoverage gate: PASS\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2301751714 queries=20000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0, ratchet unchanged at 43 of 43\n100% tests passed out of 6\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: parser, unparse and round-trip cases;\n`plan.LiteralTest.ParseTimestamp` (accept/reject table); binder names,\ntypes, EXPLAIN and errors; `.slt` in `expressions/timestamps.slt` and\n`positions/positions.slt` (answered by DuckDB); the generator\n- [x] Docs updated: `docs/sql-subset.md` (grammar, constants, literal\ntable, timestamps, types, D5)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests and ran the verification. The `reviewer` agent found a\nfraction accepted after `HH:MM`, a wrong exit-code sentence in the docs,\ntwo untested paths, a latent generator edge at year 0, and literal\noperand names; all fixed.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T00:59:37+03:00",
          "tree_id": "6a995c074eb2012d147276b506e7c6ef935597c8",
          "url": "https://github.com/ydb-campus/antb1/commit/eb6261c5e4a1c86ed29c0f219dd19533f527b02a"
        },
        "date": 1790632911924,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4158.75748451521,
            "unit": "ns/iter",
            "extra": "iterations: 168715\ncpu: 4158.364917168005 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 86690.93475102117,
            "unit": "ns/iter",
            "extra": "iterations: 7571\ncpu: 86683.85695416722 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 221809.14529914866,
            "unit": "ns/iter",
            "extra": "iterations: 3159\ncpu: 221766.09275087045 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 443556.942332093,
            "unit": "ns/iter",
            "extra": "iterations: 1578\ncpu: 443470.632446134 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 348180.8891109012,
            "unit": "ns/iter",
            "extra": "iterations: 2002\ncpu: 348139.4095904093 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2120051.21450161,
            "unit": "ns/iter",
            "extra": "iterations: 331\ncpu: 2119867.3051359523 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 178.855602750005,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 178.8174442499999 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.319151306122476,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.31804093877551 ms\nthreads: 1"
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
          "id": "a50762f76e20082f2d4a05140130052c5f33df1b",
          "message": "feat(plan,exec): more extract fields and date_trunc units (#40)\n\n## Summary\n\nFollow-up 3, part 2 of 2. **Stacked on #39**, which is on #38; merge\nthose first.\n\nThis adds the rest of DuckDB's common date parts.\n- **EXTRACT fields:**\n  - `quarter`, and ISO 8601 `week` and `isoyear`;\n  - `dow` (Sunday is 0), `isodow` (Monday is 1) and `doy`;\n  - `millisecond` and `microsecond`, both including the seconds;\n  - `epoch`, DOUBLE seconds since 1970;\n- `decade`, `century` and `millennium`: `century` counts from year 1 as\ncentury 1, and goes down from -1 before year 1.\n- **date_trunc units:**\n- `millisecond`, `microsecond` and `isoyear` (the Monday of ISO week 1);\n- `decade`, `century` and `millennium`: the year is truncated toward\nzero, DuckDB's quirk (2013 → 2000 for both century and millennium, 150\nBC → 101 BC);\n- `dow`, `isodow` and `doy` truncate to the day, and `epoch` to the\nsecond, as in DuckDB.\n- **Spellings:** DuckDB's spellings are accepted, case-insensitively,\nfrom one table in the binder. Examples: plurals, `y`, `yr`, `mon`, `h`,\n`m`, `s`, `ms`, `msec`, `us`, `w`, `weekofyear`, `dayofweek`, `weekday`,\n`dayofyear`, `c`, `mil`.\n- EXTRACT names a spelling that is a DuckDB keyword by its lower-case\nfield (`Years` → `'year'`) and any other spelling as written, as DuckDB\ndoes.\n- `dec` works for `date_trunc` only, because `EXTRACT(dec ...)` does not\nparse in DuckDB.\n- DuckDB's misspelling `millenium` is left out: the typos linter rejects\nit, and `_typos.toml` is a protected path.\n- **Implementation:** everything is computed in 64-bit civil arithmetic\nin `src/exec/compute.cc`, over the whole TIMESTAMP range and for DATE\ninputs. An infinity gives NULL from EXTRACT and stays infinite through\n`date_trunc`.\n\nValues were checked against DuckDB 1.5.5: every field, unit and\nspelling, including BC and year 0, ISO weeks 52/53 at both year ends,\nleap years, fractions before 1970, the TIMESTAMP extremes, and DATE\nextremes and infinities. The reviewer agent also compared about 190,000\nvalues with DuckDB, and all match.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check\nexit 0\n$ pixi run tidy\nexit 0\n$ pixi run coverage\nCoverage gate: PASS\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=1355275055 queries=20000 failed=0 unsupported=0\n$ pixi run test-data           # hits_0, ratchet unchanged at 43 of 43\n100% tests passed out of 6\n```\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change:\n- `exec.ComputeTest.TimestampFunctions` table over DuckDB-probed values;\n  - binder names, types and errors;\n  - `.slt` in `expressions/timestamps.slt` (answered by DuckDB);\n  - generator field and unit lists;\n  - metamorphic relation `extract_dow_is_days_arithmetic`.\n- [x] Docs updated: `docs/sql-subset.md` (fields, units, spellings,\nnaming, semantics)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests, probed DuckDB for every definition and spelling, and ran\nthe verification. The `reviewer` agent found that EXTRACT names keyword\nspellings by their field (fixed), after comparing about 190k values with\nDuckDB.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T01:20:01+03:00",
          "tree_id": "66fcb0b2c3b366b511a7317f67723f89d3e3929a",
          "url": "https://github.com/ydb-campus/antb1/commit/a50762f76e20082f2d4a05140130052c5f33df1b"
        },
        "date": 1790634140577,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3580.1614734336467,
            "unit": "ns/iter",
            "extra": "iterations: 196602\ncpu: 3579.8122653889586 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 90518.96202714425,
            "unit": "ns/iter",
            "extra": "iterations: 6926\ncpu: 90515.5879295409 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 98757.58919988602,
            "unit": "ns/iter",
            "extra": "iterations: 7074\ncpu: 98749.83644331353 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 330839.2032212224,
            "unit": "ns/iter",
            "extra": "iterations: 2111\ncpu: 330787.4310753199 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 401363.81604585034,
            "unit": "ns/iter",
            "extra": "iterations: 1745\ncpu: 401266.8693409744 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2396270.874149618,
            "unit": "ns/iter",
            "extra": "iterations: 294\ncpu: 2395852.6632653065 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 180.8697109999997,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 180.83811575000007 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 12.62767198181817,
            "unit": "ms/iter",
            "extra": "iterations: 55\ncpu: 12.626020399999987 ms\nthreads: 1"
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
          "id": "69ac9e0c725e5cd2a32a8c458ff8775b5c19fb6e",
          "message": "feat(io): row-group parts (#42)\n\n## Summary\n\nThis is the first step of parallel execution (ADR 0013), planned with\njoins in mind for later TPC-H work. It adds row-group parts to tables.\nThere is no query behavior change: the executor still uses `Scan`.\n\n- **`plan::Table`** gains:\n  - `num_parts()`: the units of parallel work;\n  - `part_rows(part)`: a part's row count, when known;\n  - `ScanPart(part, fields, batch_size)`: scans one part.\n\nScanning parts `0..n-1` in order gives the rows of `Scan`. By default\nthe whole table is one part.\n- **`io::ParquetTable`**: each row group with rows is one part, in file\nand row-group order. A table without rows has no parts.\n- **Footers:** the footer read at `Open` is kept and reused by every\nscan, whole-table or per part, so no part parses a footer again. Each\nscan compares the file's size and raw footer bytes with the ones read at\n`Open`.\n- A rewritten file is an `IOError` naming the file, including a rewrite\nthat keeps the same size.\n  - Before, each scan re-read the footer and checked the column types.\n- The per-batch type and column-count checks are now true by\nconstruction, so they are removed.\n- **Docs:** `docs/architecture.md` describes parts.\n\nNext PR: the thread pool, `--threads`, the global-aggregate and\ncollector sinks with the ordered window, and a memory budget.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full\nCoverage gate: PASS (io: lines 92.06%, branches 87.30%)\ncheck-full exit 0 (lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc)\n$ pixi run test-data\n100% tests passed out of 6\n```\n\nNew tests in `src/io/tests/parquet_scan_test.cc`:\n- the parts, read in order, equal `Scan`. This covers several files, an\nempty file, column subsets, batch sizes 1, 2 and 64, and every\nengine-view conversion;\n- `part_rows` matches the rows each part returns;\n- a table without rows has no parts;\n- out-of-range parts and bad requests are rejected;\n- a file rewritten after `Open`, including a same-size rewrite with\nother values, is an `IOError` for both `Scan` and `ScanPart`.\n\nA plan test covers the single-part defaults.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand the tests and ran the verification. The `reviewer` agent found one\nproblem, now fixed: with footers kept from `Open`, a same-size rewrite\nof a file would have been decoded with the stale footer. Scans now\ncompare the raw footer bytes.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T08:22:18+03:00",
          "tree_id": "8be8ec6a0c2809b73fd29822135376f9b316b8dd",
          "url": "https://github.com/ydb-campus/antb1/commit/69ac9e0c725e5cd2a32a8c458ff8775b5c19fb6e"
        },
        "date": 1790659454991,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3730.8880581263634,
            "unit": "ns/iter",
            "extra": "iterations: 189518\ncpu: 3730.723308603932 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94207.32026425331,
            "unit": "ns/iter",
            "extra": "iterations: 6963\ncpu: 94181.43774235247 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 103375.0088875709,
            "unit": "ns/iter",
            "extra": "iterations: 6751\ncpu: 103346.85483632058 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 334844.4273381315,
            "unit": "ns/iter",
            "extra": "iterations: 2085\ncpu: 334737.74964028754 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 416906.15673421405,
            "unit": "ns/iter",
            "extra": "iterations: 1678\ncpu: 416807.83790226496 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2470306.915194349,
            "unit": "ns/iter",
            "extra": "iterations: 283\ncpu: 2469631.8727915175 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 176.91316025000248,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 176.8861639999999 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.13454052830203,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.128686471698114 ms\nthreads: 1"
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
          "id": "c4be212c890bb5cc81d3b753a1ce01ba7e56c244",
          "message": "feat(exec,engine): parallel execution over row groups (#43)\n\n## Summary\n\nThis is step 2 of ADR 0013: parallel execution over row groups, designed\nso that hash joins can later reuse the same pipeline and sink framework\n(the ADR's new \"Towards joins\" section).\n\n**How a query runs now:**\n- A **part pipeline** is a chain of `Filter`/`Compute`/`Project` over a\n`Scan`. It runs once per table part (a Parquet row group, #42) on an\nArrow `ThreadPool` owned by the `Session`.\n- Each part gets fresh operator instances, so parts share no mutable\nstate.\n\n**`exec::PartScheduler`:**\n- Hands part results back strictly in part order.\n- At most 2 × threads parts are in flight.\n- The first failing part *in part order* decides the error. Parts after\nthe consumer stops (a met `LIMIT`) are stopped and their errors dropped.\n- With one thread it runs each part inline: the same code path, so\n**results are byte-identical for any thread count**.\n\n**Operators the planner builds:**\n- `PartAggregateOperator` handles a global aggregate over a pipeline:\nper-part `AggregateSet`s, merged in part order.\n`ScalarAggregateOperator` now shares `AggregateSet`.\n- `PartUnionOperator` handles every other pipeline top: batches in part\norder. Under a `LIMIT`, each part stops after `limit + offset` rows, and\nno new parts are scheduled once the limit is met.\n- GROUP BY and ORDER BY still aggregate and sort serially, but read the\nparallel pipeline's output, so their filters and expressions (Q28's\nregex, for example) already run in parallel. Their own sinks are the\nnext PRs.\n\n**Session and CLI:**\n- `SessionOptions::threads` is validated to 1..1024. The library default\nis 1, so embedders and every test harness stay single-threaded unless\nthey ask.\n- `antb1 query` and `antb1 bench` get `--threads N`, defaulting to the\nhardware threads as decided. `bench` records `threads` in its JSON.\n\n**Semantics change:** DOUBLE `SUM`/`AVG` without GROUP BY now add each\nrow group, then the row-group sums in order. This was accepted in the\nADR and is documented in docs/sql-subset.md.\n\n**Approved protected-path edits:**\n- `parallel` added to the label list in `cmake/Antb1Testing.cmake` (lint\nR008).\n- AGENTS.md: the labels list, and the threads-in-tests sentence.\n- A 4-thread pool in the exec unit tests.\n\nNo changes to `CMakePresets.json` or `pixi.toml` were needed.\n\n## Full ClickBench data (100 files, 325 row groups), `antb1 bench\n--tries 1`, 128-CPU host\n\n| Threads | Total, 43 queries | Failed |\n| --- | --- | --- |\n| 1 | 796 s | 0 |\n| 8 | 218 s | 0 |\n| 32 | 172 s | 0 |\n| 128 | 167 s | 0 |\n\n- Scan, filter and expression-bound queries speed up 15-27× on 128\nthreads. For example, Q28 goes from 226 s to 10.4 s, Q23 from 158 s to\n6.8 s, and Q20-22 and Q36-42 improve by about 16-27×.\n- GROUP BY-heavy queries (Q8-9, Q15-18, Q32-35) gain only 1-2×, because\ngrouping is still serial. The GROUP BY sink PR addresses this.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1303 (no ThreadSanitizer reports)\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2759026187 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6 (ClickBench pass set unchanged)\n$ pixi run test -L parallel\n100% tests passed out of 42\n```\n\n**New tests:**\n- **`parallel` label:**\n- `parallel.<area>.<file>` runs every `.slt` case file with `--threads 4\n--same-as-threads 1 --batch-size 1000`: the expectations must hold, and\nevery result must equal the 1-thread result byte for byte, before any\nsorting.\n- `parallel.diff.random` runs 300 random queries against DuckDB with 4\nthreads and 700-row batches, with the same identity check.\n- **Exec unit tests (4-thread pool):** scheduler order, window bound,\nfirst error in part order, stop and wait; aggregates equal to the serial\npath; DOUBLE per-part sums; projection order; LIMIT early stop (bounded\nparts scanned); error order under LIMIT; GROUP BY and sort over the part\nunion.\n- **Session, CLI and harness:** thread validation and 1 vs 4 threads\nidentical; the `--threads 0` usage golden; the bench JSON `threads`\nfield; the CLI goldens pinned to `--threads 1`; the `--same-as-threads`\nwrapper catching any difference (rows, order, types, errors) without\nprinting values.\n\n**Note on the parallel diff seed:** `parallel.diff.random` uses the same\nseed as `diff.random` (20260925). With a new seed (20260929), one\ngenerated query makes **DuckDB itself** leak under ASan. The query is\nMIN of a VARCHAR column with GROUP BY and LIMIT, and the leak is in\nDuckDB's `MinMaxStringState` combine. No antb1 frames appear, so it is a\nbug in the test oracle, not the engine. I did not add a sanitizer\nsuppression, because the suppressions are protected and gates must not\nbe weakened. It is recorded as a follow-up to check upstream.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code designed and\nwrote the change and tests, and ran the verification and benchmarks. The\n`reviewer` agent found no races, lifetime or deadlock problems. It\nflagged a stale DOUBLE SUM line in docs/sql-subset.md and an overstated\nmemory claim in the ADR; both are fixed. Bounding a part union's\nbuffering is taken into the memory-limit PR.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T10:00:38+03:00",
          "tree_id": "0901a74b212efc6f2f4c93585ecbd616242c43e4",
          "url": "https://github.com/ydb-campus/antb1/commit/c4be212c890bb5cc81d3b753a1ce01ba7e56c244"
        },
        "date": 1790665352208,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3271.8121167216505,
            "unit": "ns/iter",
            "extra": "iterations: 214596\ncpu: 3271.7965106525744 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 83196.24515491824,
            "unit": "ns/iter",
            "extra": "iterations: 8101\ncpu: 83193.12233057649 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 89646.41239994831,
            "unit": "ns/iter",
            "extra": "iterations: 7871\ncpu: 89633.02642612117 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 294376.96821413626,
            "unit": "ns/iter",
            "extra": "iterations: 2391\ncpu: 294320.1585110834 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 383357.783060109,
            "unit": "ns/iter",
            "extra": "iterations: 1830\ncpu: 383354.26885245915 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2106741.205357117,
            "unit": "ns/iter",
            "extra": "iterations: 336\ncpu: 2106361.300595238 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 150.7269375,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 150.69781000000003 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.700113233333317,
            "unit": "ms/iter",
            "extra": "iterations: 60\ncpu: 11.697951616666662 ms\nthreads: 1"
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
          "id": "dd7b9d6c5808bd564238ff07461bab95d6a45e71",
          "message": "feat(exec,engine): memory limit (#44)\n\n## Summary\n\nThis is step 3 of ADR 0013: a session memory limit. Before it, nothing\nbounded a query's memory:\n- a big query grew until `std::bad_alloc`, which on a pool thread\naborted the process;\n- #43's parallel parts multiply the memory in flight;\n- the GROUP BY sink and hash joins will need a budget.\n\n**`exec::MemoryBudget`** (new public header):\n- It is an `arrow::MemoryPool` over the default pool, with an optional\nbyte limit.\n- It counts buffers atomically, charging before it allocates, so\nconcurrent threads never pass the limit together.\n- `Reserve`/`Release` charge memory held outside Arrow buffers.\n- Past the limit, an allocation fails with `Status::OutOfMemory` and\nleaves nothing behind.\n\n**Everything allocates through it:**\n- The `Session` owns one budget. It is `ExecContext::pool` for every\nquery, and `QueryResult::memory` keeps it alive as long as a result's\nbuffers exist.\n- `plan::Table` scans take a pool: non-virtual `Scan`/`ScanPart` over\nthe protected virtuals `DoScan`/`DoScanPart`, so no call site changed.\n- `ParquetTable` reads the file, decodes pages and runs conversions into\nthat pool.\n- Operators charge their own containers through\n`exec::MemoryReservation`:\n- `GroupedAggregateState::memory_usage()` covers the per-group vectors\nand the VARCHAR MIN/MAX heap;\n- `SortBuffer::memory_usage()` and `sort_memory()` cover the row\nreferences and the sort entries.\n\n**Parallel parts adapt instead of failing:**\n- The part scheduler's window halves each time a part is taken above\nhalf the limit, and widens by one below it.\n- A part that runs out of memory next to others does not fail the query.\nThe parts ahead are dropped and rerun when reached, and that part reruns\nalone.\n- Part results are handed over in scheduler-owned slots, so no pool\nthread holds budget memory after a part is dropped.\n- Only the schedule changes: results are identical.\n\n**Errors:**\n- `OutOfMemory` means exit code 1 with kind `memory`, as approved: \"the\nquery needs more than the memory limit of 2.00 GB (--memory-limit)\".\n- It passes unchanged through the Parquet reader and through\n`regexp_replace`.\n- `std::bad_alloc` is caught in part tasks, in `Drain` and in the scan,\nso no exception leaves exec or io.\n- `~Session` waits for the pool's workers before the budget goes.\n\n**Settings:**\n- `engine::SessionOptions::memory_limit`: there is no limit by default\nfor embedders and tests.\n- `antb1 query|bench --memory-limit SIZE` defaults to 80% of physical\nmemory, as DuckDB's `memory_limit`. It takes `4GB`, `1.5GB`, `512MiB`,\n`50%` (1000- or 1024-based units, case-insensitive).\n- The bench JSON records `memory_limit`.\n\n## Full ClickBench data, 128 threads, 128-CPU host\n\n| `--memory-limit` | Time on the answered queries | Same queries, no\nlimit | Failed (memory error, exit 1) |\n| --- | ---: | ---: | --- |\n| default (80% of RAM) | 164.5 s (best of 3) | 164.7 s (#43) | none |\n| 8 GB | 143.3 s | 109.3 s | Q32-34 |\n| 2 GB | 257.7 s | 73.0 s | Q16-18, Q32-34 |\n\n- **Default:** there is no slowdown; no query is more than 20% slower\nthan on #43.\n- **Failed queries:** they are high-cardinality GROUP BYs whose group\nstate alone exceeds the limit. That needs spilling, which is planned\nafter joins.\n- **Why capped runs are slower:** before the adaptive window, degrading\nto one part at a time under pressure made the 8 GB run take 577 s.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1319 (no ThreadSanitizer reports)\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2896423234 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**New tests:**\n- **`MemoryBudget`:**\n- the limit, exact accounting, reallocation failure and shrink, and\n`Reserve`/`Release`;\n- a deterministic 4-thread test: each thread holds 20 KiB until all have\ntried, so exactly 64 of 80 1-KiB allocations fit 64 KiB;\n  - `MemoryReservation`.\n- **Sinks:** `memory_usage()` of every grouped state kind and of the\nVARCHAR heap, and of `SortBuffer`.\n- **Operators:** every sink (GROUP BY, sort, filter+projection,\nCOUNT(DISTINCT)) with a tiny limit fails with `OutOfMemory` and gives\neverything back. With a big limit the result equals the unlimited one,\non 1 thread and on a 4-thread pool.\n- **Scheduler:**\n  - the window halves and regrows (deterministic);\n- a part that runs out of memory on the pool is rerun alone and the\nwindow regrows;\n  - under pressure, LIMIT reads only the first part.\n- **regexp_replace:** out of memory stays a memory error, while an\ninvalid pattern stays an execution error. The test fails with the fix\nreverted.\n- **Session:**\n  - thread and limit validation;\n  - a tiny limit gives `OutOfMemory`;\n  - the results survive the session's destruction;\n- 20 sessions destroyed right after a query that failed in its first row\ngroup on 4 threads (checked by ASan).\n- **io:** a scan through a pool counts the whole row group (the scan\ntest now measures that pool).\n- **CLI:** size parsing (units, %, bad input), exit code 1 with kind\n`memory`, a golden for the memory error, a usage golden for a bad size,\nand `memory_limit` in the bench JSON.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code designed and\nwrote the change and tests, ran the verification and the full-data runs.\nTwo `reviewer` passes found issues that are fixed here:\n- a use-after-free when a session was destroyed while pool workers still\nheld part buffers;\n  - Parquet column-chunk reads that the budget did not count;\n  - a timing-dependent test;\n- pool threads keeping dropped parts' results alive through Arrow's\nfuture copies;\n  - an untested `regexp_replace` error-kind fix.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T12:45:43+03:00",
          "tree_id": "3c763cc3e10d9a237a740eeea6f9cdd0fca95621",
          "url": "https://github.com/ydb-campus/antb1/commit/dd7b9d6c5808bd564238ff07461bab95d6a45e71"
        },
        "date": 1790675289930,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4095.5010497744083,
            "unit": "ns/iter",
            "extra": "iterations: 172418\ncpu: 4094.661439060886 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84077.66363393063,
            "unit": "ns/iter",
            "extra": "iterations: 7474\ncpu: 84060.09900990101 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222114.02858050022,
            "unit": "ns/iter",
            "extra": "iterations: 3149\ncpu: 222036.60685932048 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 437030.2866958174,
            "unit": "ns/iter",
            "extra": "iterations: 1601\ncpu: 436893.55715178023 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 401634.3532110097,
            "unit": "ns/iter",
            "extra": "iterations: 1744\ncpu: 401521.5487385322 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2128114.281345588,
            "unit": "ns/iter",
            "extra": "iterations: 327\ncpu: 2127908.8562691147 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 205.4703879999996,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 205.43252633333313 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.429914062499863,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.42814931249999 ms\nthreads: 1"
          }
        ]
      }
    ]
  }
}