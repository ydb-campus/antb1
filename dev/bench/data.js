window.BENCHMARK_DATA = {
  "lastUpdate": 1790885950363,
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
          "id": "5505f014091f6bac799b6e94e11c815bd460e9a7",
          "message": "perf(exec): parallel GROUP BY (#45)\n\n## Summary\n\nThis is step 4 of ADR 0013: parallel GROUP BY.\n\n- **Before this PR:** since #43, a GROUP BY over a part pipeline read\nthe part union. Its scan and expressions ran in parallel, but the\ngrouping itself was serial.\n- **Per-part grouping:** each part (row group) is grouped into its own\n`exec::GroupTable` on the pool.\n- **Ordered merge:** the consumer merges the tables in part order,\nthrough `PartScheduler`, with the same window, memory adaptation, OOM\nrerun and error order as before. For each part:\n- its unique normalized keys (`Grouper::GetUniques`), fed through the\nmerged table's grouper, give the group map;\n- a new group keeps the part's first-seen key values (the DOUBLE\n`-0.0`/`0.0` and NaN spelling from the earliest part);\n  - the grouped states merge with `GroupedAggregateState::Merge`.\n- **Bounded chunks:** a merge adds new groups in chunks of at most 64Ki.\nIt takes keys per source chunk and never concatenates a part's keys, so\nno key array (and no `Finalize` range) grows with a part.\n- **`exec::GroupTable`** (private) is the grouping state taken out of\n`GroupAggregateOperator`. That operator keeps its behavior for input\nthat is not a part pipeline.\n\n**Semantics** (docs/sql-subset.md):\n- Results are byte-identical for any thread count.\n- Group order follows the row groups and their batches. It is still\ndeterministic, but not the single-pass order: Arrow's grouper numbers a\nbatch's new groups in its own order. SQL leaves group order open.\n- A grouped DOUBLE `SUM`/`AVG` adds per row group, then in order, as a\nglobal one already did.\n\n## Performance: full ClickBench data, 128 threads, paired A/B\n\nThe host was heavily loaded by other users during the run (load average\nabout 200 on 128 cores). So this is a paired A/B: for each query, the\n#44 binary and this PR's binary each ran 3 tries (best taken),\nalternating which went first. Absolute times are inflated; the ratios\nare what matter.\n\n- **Medium-cardinality GROUP BY:** about 2× faster (Q8-11, Q27, Q35,\nQ42; Q28 1.7×).\n- **Very-high-cardinality GROUP BY** (Q14, Q16, Q18, Q32): neutral to\nslightly slower. With nearly every row its own group, the serial merge\non the consumer re-inserts about every row and dominates. An unloaded\nsingle run earlier showed Q32 +8%.\n- The fix is a radix-partitioned merge, merging partitions in parallel.\nIt is the next step named in ADR 0013.\n- **Differences elsewhere** are on queries this PR does not change, so\nthey are load noise.\n\n<details><summary>Per-query seconds (queries over 0.3 s)</summary>\n\n| Query | main (#44) | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| Q4 | 4.74 | 4.57 | 1.04× |\n| Q5 | 3.38 | 3.43 | 0.98× |\n| Q8 | 11.50 | 6.36 | 1.81× |\n| Q9 | 13.94 | 7.06 | 1.98× |\n| Q10 | 1.79 | 0.72 | 2.49× |\n| Q11 | 1.53 | 0.85 | 1.81× |\n| Q12 | 5.69 | 5.42 | 1.05× |\n| Q13 | 8.77 | 8.45 | 1.04× |\n| Q14 | 5.95 | 7.82 | 0.76× |\n| Q15 | 9.36 | 7.44 | 1.26× |\n| Q16 | 12.95 | 15.27 | 0.85× |\n| Q17 | 25.56 | 18.69 | 1.37× |\n| Q18 | 32.46 | 32.95 | 0.99× |\n| Q20 | 1.12 | 1.12 | 1.00× |\n| Q21 | 1.23 | 1.25 | 0.98× |\n| Q22 | 2.79 | 3.34 | 0.84× |\n| Q23 | 4.74 | 4.81 | 0.99× |\n| Q24 | 0.80 | 0.86 | 0.93× |\n| Q25 | 0.89 | 0.85 | 1.04× |\n| Q26 | 0.87 | 0.84 | 1.03× |\n| Q27 | 1.93 | 0.87 | 2.23× |\n| Q28 | 13.18 | 7.91 | 1.67× |\n| Q30 | 2.88 | 2.30 | 1.25× |\n| Q31 | 4.57 | 4.36 | 1.05× |\n| Q32 | 25.88 | 30.08 | 0.86× |\n| Q33 | 16.53 | 13.93 | 1.19× |\n| Q34 | 16.38 | 13.74 | 1.19× |\n| Q35 | 5.89 | 3.03 | 1.94× |\n| Q36 | 0.78 | 0.85 | 0.92× |\n| Q37 | 0.82 | 0.62 | 1.31× |\n| Q38 | 1.13 | 1.27 | 0.88× |\n| Q39 | 2.63 | 3.22 | 0.82× |\n| Q40 | 0.57 | 0.49 | 1.16× |\n| Q41 | 0.58 | 0.52 | 1.11× |\n| Q42 | 0.76 | 0.34 | 2.25× |\n| **Total (43 queries)** | **245.1** | **216.3** | **1.13×** |\n\n</details>\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1323\n$ pixi run test-data\n100% tests passed out of 6\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: FAIL seed=4165926010 queries=20000 failed=1 unsupported=0\n```\n\n**The one diff-random failure predates this PR** and is a harness false\npositive, not a wrong answer:\n- **The case:** case 19768 is an ORDER BY on a column where about 8,000\nrows tie, with `OFFSET 5` and no LIMIT. There is no GROUP BY, so this PR\ndoes not touch it.\n- **Same answer:** antb1 and DuckDB return the same row count, the same\nkey runs, and the same five skipped rows.\n- **The flag:** the harness's tie-group check still rejects antb1's\norder.\n- **Also on main:** the same case fails on `main` (#44) built from this\ntree with the change stashed.\n\nIt is recorded as a follow-up to fix in the harness's tie-group\ncomparison.\n\n**New tests** (`src/exec/tests/parallel_test.cc`):\n- Grouping per part and merging equals one pass, as a set of rows:\n- over COUNT(*), COUNT, SUM, VARCHAR MIN, DOUBLE MAX and\nCOUNT(DISTINCT);\n- with DOUBLE keys (NaN and signed zeros), BIGINT+VARCHAR keys, no keys,\nNULL keys and empty parts.\n- The same bytes on 4 threads as on 1, and the key spelling from the\nearliest part (a later part sees the other spelling first).\n- Errors come in part order.\n- Grouped DOUBLE SUM/AVG are byte-identical across thread counts, and\nequal one running sum up to 1e-9 relative.\n- Merging 150k groups from 3 source chunks gives chunks of\n65536/65536/18928 with the part's rows. This fails with a concatenating\nmerge.\n- The existing `parallel` label, the memory-limit tests (the GROUP BY\nplan now goes through `PartGroupAggregate`), slt/oracle and metamorphic\nall pass.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand tests, ran the verification and the paired A/B. The `reviewer` agent\nfound two problems, both fixed:\n- a merge concatenated a part's keys into one array (the 2 GiB\nbinary-array risk);\n  - grouped DOUBLE SUM/AVG had no end-to-end test.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T14:47:45+03:00",
          "tree_id": "9cdc9f2a3d4de35ab3f774dc2f7e1a4cceecb9b3",
          "url": "https://github.com/ydb-campus/antb1/commit/5505f014091f6bac799b6e94e11c815bd460e9a7"
        },
        "date": 1790682644594,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4087.597984511479,
            "unit": "ns/iter",
            "extra": "iterations: 171869\ncpu: 4087.4407833873474 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84520.65737103694,
            "unit": "ns/iter",
            "extra": "iterations: 7638\ncpu: 84490.94880858864 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222064.433692893,
            "unit": "ns/iter",
            "extra": "iterations: 3152\ncpu: 221986.66338832484 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 449901.6789107001,
            "unit": "ns/iter",
            "extra": "iterations: 1579\ncpu: 449670.9898670045 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 402830.9688940162,
            "unit": "ns/iter",
            "extra": "iterations: 1736\ncpu: 402804.6278801842 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2110101.1807229086,
            "unit": "ns/iter",
            "extra": "iterations: 332\ncpu: 2109705.4156626505 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 183.65494933333557,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 183.6325966666668 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.40169440816332,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.398958734693867 ms\nthreads: 1"
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
          "id": "573baf37182ce4054566368772afe4a7b6fd6bc0",
          "message": "perf(exec): partitioned GROUP BY merge (#46)\n\n## Summary\n\nThis is the follow-up named in ADR 0013 to #45's GROUP BY sink: a\nradix-partitioned merge.\n\n**The problem:** #45 grouped each row group in parallel, but merged the\npart tables on one thread. For high-cardinality keys that merge\nre-inserted about every row, so Q14/16/18/32 did not improve.\n\n**Partitioning** (`exec::GroupTable::Partition`, on the part's worker\nthread):\n- The part's groups are split into **64 partitions**.\n- The partition is a seed-free hash of the normalized keys:\n`arrow::internal::ComputeStringHash` over each value's bytes, a constant\nfor NULL, combined over keys.\n- The unique keys are sliced per partition.\n- 64 is a constant, never the thread count, so results stay\nbyte-identical for any number of threads.\n\n**Merge** (`PartGroupAggregateOperator`):\n- The consumer keeps one `GroupTable` per partition.\n- For each part, in part order through `PartScheduler` (unchanged), it\nmerges the part's partitions in parallel on the pool and waits for all\nof them. Each partition table is touched by one task at a time, and the\nfirst failed partition in partition order decides the error.\n- Groups are emitted partition by partition. Group order is still\ndeterministic and thread-count independent; docs/sql-subset.md says it\nfollows a key hash, and SQL leaves it open.\n\n**`GroupedAggregateState::MergeGroups(other, from, to)`** (public\nheader): a sparse merge whose time is proportional to the merged groups.\n`Merge(other, map)` is now a non-virtual wrapper over it.\n\n**COUNT(DISTINCT):** it builds its distinct pairs by group once\n(`std::call_once`) and merges only the requested groups' pairs. This\nmakes it safe and cheap for the 64 partitions that read the same part\nstate at the same time.\n\n## Performance: full ClickBench data, 128 threads, paired A/B on a quiet\nhost\n\nFor each query, the #45 binary and this PR's binary each ran 3 tries\n(best taken), alternating which went first. The load average was 7 at\nthe start and 39 at the end.\n\n- **Total:** 133.3 s → 53.1 s, 2.5× faster. DuckDB takes 12.9 s, so the\ngap went from 12.8× (#43) to about 4×.\n- **High-cardinality GROUP BY:** 3.0-6.5× faster: Q12-18, and Q32-34\n(Q18: 16.9 s → 2.6 s; Q32: 25.6 s → 6.8 s).\n- **Medium-cardinality GROUP BY:** about 2× (Q8-9, Q30-31, Q35).\n- **Low-cardinality GROUP BY after a selective filter** (Q21, Q22):\n0.91-0.92×. The 64 partitions add a fixed per-part overhead there. The\nabsolute cost is about 0.1 s.\n\n<details><summary>Per-query seconds (queries over 0.3 s)</summary>\n\n| Query | main (#45) | this PR | speedup | DuckDB 1.5.5 |\n| --- | ---: | ---: | ---: | ---: |\n| Q4 | 2.27 | 2.27 | 1.00× | 0.14 |\n| Q5 | 1.68 | 1.69 | 0.99× | 0.23 |\n| Q8 | 3.01 | 1.62 | 1.85× | 0.19 |\n| Q9 | 2.98 | 1.68 | 1.77× | 0.21 |\n| Q12 | 2.47 | 0.72 | 3.43× | 0.25 |\n| Q13 | 3.97 | 1.16 | 3.41× | 0.29 |\n| Q14 | 2.71 | 0.90 | 3.03× | 0.22 |\n| Q15 | 3.35 | 0.69 | 4.86× | 0.19 |\n| Q16 | 7.29 | 1.43 | 5.10× | 0.40 |\n| Q17 | 7.12 | 1.13 | 6.28× | 0.37 |\n| Q18 | 16.93 | 2.61 | 6.49× | 0.69 |\n| Q20 | 0.63 | 0.63 | 0.99× | 0.34 |\n| Q21 | 0.69 | 0.76 | 0.91× | 0.28 |\n| Q22 | 1.29 | 1.41 | 0.92× | 0.57 |\n| Q23 | 4.16 | 4.18 | 1.00× | 0.48 |\n| Q24 | 0.74 | 0.75 | 0.99× | 0.21 |\n| Q25 | 0.73 | 0.72 | 1.01× | 0.14 |\n| Q26 | 0.74 | 0.74 | 1.01× | 0.09 |\n| Q27 | 0.83 | 0.88 | 0.94× | 0.37 |\n| Q28 | 7.03 | 6.42 | 1.09× | 2.80 |\n| Q30 | 1.98 | 0.98 | 2.03× | 0.26 |\n| Q31 | 3.77 | 1.59 | 2.37× | 0.36 |\n| Q32 | 25.57 | 6.83 | 3.75× | 0.77 |\n| Q33 | 12.19 | 2.54 | 4.79× | 0.78 |\n| Q34 | 12.11 | 2.53 | 4.79× | 1.01 |\n| Q35 | 2.63 | 1.22 | 2.15× | 0.24 |\n| Q36 | 0.78 | 0.75 | 1.05× | 0.13 |\n| Q37 | 0.55 | 0.58 | 0.94× | 0.10 |\n| Q38 | 0.62 | 0.64 | 0.97× | 0.07 |\n| Q39 | 1.46 | 1.38 | 1.06× | 0.24 |\n| **Total (43 queries)** | **133.3** | **53.1** | **2.51×** | **12.9** |\n\n</details>\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1326\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3851070685 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**New tests:**\n- **`MergeGroups` for every state kind and key type:** a part state is\nsplit into even and odd groups, merged from two threads at once, and\neach group comes out exactly once. Out-of-range groups and mismatched\nlists are Invalid.\n- **The partition hash is pinned by a golden:** fixed keys map to fixed\npartitions (NULL to the same one for every key type). A change would\nchange GROUP BY output order, so it must be deliberate.\n- **Merge errors:**\n- a HUGEINT SUM that overflows only when two parts merge fails with the\nsame error on 1 and 4 threads;\n  - an out-of-range partition is Invalid;\n  - `Next()` after `Close()` is Invalid.\n- **Existing GROUP BY tests** now run through the partitioned merge:\npart vs single-pass equality over all aggregates and key types, 4\nthreads vs 1, key spelling, errors, grouped DOUBLE sums, and bounded\nchunks. So do the `parallel` label and the memory-limit tests.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand tests, ran the verification and the paired A/B. The pool tests\ncaught a race: concurrent `GetUniques` on a part's COUNT(DISTINCT)\ngrouper. The `reviewer` agent found three problems, all fixed:\n  - `Next()` after `Close()` dereferenced a null scheduler;\n  - the merge error path was untested;\n- the skip-sentinel map made each partition scan all of a part's groups,\nwhich is now replaced by the sparse `MergeGroups`.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T17:50:38+03:00",
          "tree_id": "f51a43547d7f848d5691c8afc6916e87c59aad28",
          "url": "https://github.com/ydb-campus/antb1/commit/573baf37182ce4054566368772afe4a7b6fd6bc0"
        },
        "date": 1790693589827,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4093.060569373508,
            "unit": "ns/iter",
            "extra": "iterations: 168501\ncpu: 4092.2793751965864 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84658.38798026936,
            "unit": "ns/iter",
            "extra": "iterations: 7704\ncpu: 84653.67328660436 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222320.84934982486,
            "unit": "ns/iter",
            "extra": "iterations: 3153\ncpu: 222314.94576593715 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 440558.9805153957,
            "unit": "ns/iter",
            "extra": "iterations: 1591\ncpu: 440533.5964802009 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 401010.88665132027,
            "unit": "ns/iter",
            "extra": "iterations: 1738\ncpu: 400959.65132336 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2130678.889908259,
            "unit": "ns/iter",
            "extra": "iterations: 327\ncpu: 2130602.299694188 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 196.07981200000305,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 196.07369733333346 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.462210666666605,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.461259062499991 ms\nthreads: 1"
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
          "id": "4c2c662e773f05eba6bc19515b55d6e141049f0f",
          "message": "perf(exec): parallel top-N (#47)\n\n## Summary\n\nThis is step 5 of ADR 0013: the top-N sink.\n\n**Before this PR:** `ORDER BY ... LIMIT n` over a part pipeline sent\nevery filtered row, all columns, from the part union to one serial top-N\n`SortOperator`.\n\n**Now,** with the new `PartTopNOperator` (planned for `Limit(Sort)` with\na positive limit over a part pipeline):\n- Each part keeps its first `limit + offset` rows in its own\n`SortBuffer`, sorted and compacted on its worker.\n- The consumer merges those small buffers in part order\n(`SortBuffer::Merge`), sorts again keeping `limit + offset`, and emits\nthe window.\n\n**Exactness:**\n- `SortBuffer` is stable, and a compacted buffer's order is a valid\ninput order for its rows. Merging parts in part order therefore gives\nexactly the rows and tie order of the serial top-N over the part union,\nbyte-identical for any number of threads.\n- A row a part drops has at least `limit + offset` rows before it in\nthat part's stable order, so the serial top-N could never keep it\neither.\n\n**Memory:** each part's rows carry their own `MemoryReservation` until\nthey are merged, including while they wait in the scheduler's window.\n\n**Out of scope:** a full `ORDER BY` without `LIMIT` still sorts the part\nunion on one thread. A k-way merge of sorted parts is noted in ADR 0013\nas the follow-up. `LIMIT 0`, a limit without ORDER BY, and a sort over\nnon-pipeline input (e.g. over a GROUP BY) keep their existing operators.\n\n## Performance: full ClickBench data, 128 threads, paired A/B on a quiet\nhost\n\nFor each query, the #46 binary and this PR's binary each ran 3 tries\n(best taken), alternating which went first. The load average was 3 at\nthe start.\n\n- **The top-N queries:** Q24 3.97×, Q25 5.02×, Q26 3.88× (0.73 s → 0.18\ns).\n- **Q23** is a `SELECT *` top-N: 1.11×. Decoding all of its columns\ndominates, so it needs late materialization (read the sort and filter\ncolumns first, then only the winning rows), a separate follow-up.\n- **Everything else** is unchanged within noise. The total is 53.5 s →\n51.2 s.\n\n<details><summary>Per-query seconds (queries over 0.3 s)</summary>\n\n| Query | main (#46) | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| Q4 | 2.27 | 2.27 | 1.00× |\n| Q5 | 1.70 | 1.69 | 1.00× |\n| Q8 | 1.63 | 1.63 | 1.00× |\n| Q9 | 1.71 | 1.70 | 1.01× |\n| Q10 | 0.42 | 0.41 | 1.02× |\n| Q11 | 0.43 | 0.43 | 1.01× |\n| Q12 | 0.72 | 0.73 | 0.98× |\n| Q13 | 1.16 | 1.15 | 1.01× |\n| Q14 | 0.89 | 0.90 | 0.99× |\n| Q15 | 0.71 | 0.71 | 1.00× |\n| Q16 | 1.46 | 1.47 | 1.00× |\n| Q17 | 1.11 | 1.14 | 0.98× |\n| Q18 | 2.69 | 2.67 | 1.01× |\n| Q20 | 0.63 | 0.63 | 1.00× |\n| Q21 | 0.78 | 0.76 | 1.03× |\n| Q22 | 1.37 | 1.37 | 1.00× |\n| Q23 | 4.29 | 3.86 | 1.11× |\n| Q24 | 0.73 | 0.18 | 3.97× |\n| Q25 | 0.74 | 0.15 | 5.02× |\n| Q26 | 0.73 | 0.19 | 3.88× |\n| Q27 | 0.88 | 0.90 | 0.98× |\n| Q28 | 6.41 | 6.42 | 1.00× |\n| Q30 | 0.96 | 0.95 | 1.01× |\n| Q31 | 1.60 | 1.58 | 1.02× |\n| Q32 | 6.85 | 6.85 | 1.00× |\n| Q33 | 2.59 | 2.56 | 1.01× |\n| Q34 | 2.56 | 2.53 | 1.01× |\n| Q35 | 1.23 | 1.20 | 1.03× |\n| Q36 | 0.77 | 0.76 | 1.02× |\n| Q37 | 0.58 | 0.59 | 0.98× |\n| Q38 | 0.62 | 0.65 | 0.95× |\n| Q39 | 1.39 | 1.31 | 1.06× |\n| **Total (43 queries)** | **53.5** | **51.2** | **1.04×** |\n\n</details>\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1327\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3426846718 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**New test `PartOperatorsTest.TopNKeepsEachPartsFirstRows`:**\n- **Coverage:** windows inside, across and past the rows (`LIMIT 1`, `5\nOFFSET 3`, `40 OFFSET 20`, `30 OFFSET 125`, `10 OFFSET 200`, `INT64_MAX\nOFFSET 110`), ascending and descending, with NULLs first and last (NULL\nties span parts).\n- **Reference:** each case is compared with the serial `SortOperator`\ntop-N over one scan of the whole table, byte for byte, and 4 threads\nwith 1.\n- **Errors and memory:** errors come in part order, and a tiny budget\nfails with OutOfMemory and gives every byte back.\n\nThe existing `.slt` case `LIMIT 9223372036854775807 OFFSET 10` caught an\noverflow in the first version; the window arithmetic now saturates.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand tests, ran the verification and the paired A/B. The `reviewer` agent\nconfirmed the exactness argument and found that a finished part's buffer\nleft the memory budget while it waited to be merged (fixed: the\nreservation now lives with the buffer). It also found that the test's\n\"serial\" reference was itself a one-part `PartTopNOperator` (fixed: it\nis now the real `SortOperator`).\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T18:37:16+03:00",
          "tree_id": "642c03962fdeb10f4c5cf09da292b5ed1d91f146",
          "url": "https://github.com/ydb-campus/antb1/commit/4c2c662e773f05eba6bc19515b55d6e141049f0f"
        },
        "date": 1790696379474,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4156.403981110582,
            "unit": "ns/iter",
            "extra": "iterations: 167290\ncpu: 4155.990148843326 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84455.67729855406,
            "unit": "ns/iter",
            "extra": "iterations: 7744\ncpu: 84447.328125 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222254.25166825554,
            "unit": "ns/iter",
            "extra": "iterations: 3147\ncpu: 222179.94756911337 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 444657.5618330243,
            "unit": "ns/iter",
            "extra": "iterations: 1593\ncpu: 444546.0257376022 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 401647.9582843733,
            "unit": "ns/iter",
            "extra": "iterations: 1702\ncpu: 401553.98648648657 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2133414.0823170966,
            "unit": "ns/iter",
            "extra": "iterations: 328\ncpu: 2132907.6219512206 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 217.62349266666567,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 217.58804966666673 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.507500624999873,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.503771187500014 ms\nthreads: 1"
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
          "id": "c57be667eff4b32dcfd651e30ea1088b950f4d1f",
          "message": "perf(exec): parallel COUNT(DISTINCT) without GROUP BY (#49)\n\n## Summary\n\nThis is the first ClickBench follow-up after #47: parallel global\n`COUNT(DISTINCT)`.\n\n**The problem:** a global `COUNT(DISTINCT x)` aggregated each part in\nparallel, but merged every part's distinct-value grouper into one on the\nconsumer thread. For high-cardinality columns that merge was serial over\nnearly every value: Q4 took 2.25 s against DuckDB's 0.14 s.\n\n**The rewrite:** a global aggregation whose calls are all\n`COUNT(DISTINCT x)` of one column, over a part pipeline, is now planned\nas `COUNT(key0)` over a `GROUP BY x`. It is done in the physical\nplanner, so the logical plan and EXPLAIN are unchanged.\n- The `GROUP BY x` runs through the partitioned parallel merge of #46.\n- **Equivalence** (confirmed by review):\n- NULL is its own group, and `COUNT` skips it as `COUNT(DISTINCT)` skips\nNULL.\n- DOUBLE keys go through the same `NormalizeDoubleKey`, so -0.0 goes\nwith 0.0 and there is one NaN.\n  - Every key type uses the same Arrow grouper.\n  - An empty input gives one row with 0.\n  - The output schema, names and nullability are unchanged.\n\n**Not rewritten** (they keep today's per-part states):\n- Mixed aggregates and different columns.\n- Grouped `COUNT(DISTINCT)`. The same idea there (`GROUP BY k, x` then\n`GROUP BY k`) sped up Q8 2×, but made Q11 and Q13 1.7-2.8× slower: the\nouter grouping is serial over up to every row. Doing it well needs the\ninner table partitioned by `k` alone, which is a separate change.\n\n## Performance: full ClickBench data, 128 threads, paired A/B on a quiet\nhost\n\nFor each query, the #48 binary and this PR's binary each ran 3 tries\n(best taken), alternating which went first. The load average was 3 at\nthe start.\n\n| Query | main (#48) | this PR | speedup | DuckDB 1.5.5 |\n| --- | ---: | ---: | ---: | ---: |\n| Q4 | 2.25 | 0.44 | 5.13× | 0.14 |\n| Q5 | 1.69 | 0.58 | 2.91× | 0.23 |\n| **Total (43 queries)** | **51.3** | **48.1** | 1.07× | 12.9 |\n\nNo other query moved by more than 8%.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1330\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2329981181 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**New tests:**\n- **`PartOperatorsTest.CountDistinctAloneIsAParallelGroupBy`:** the\nrewrite equals the serial `ScalarAggregateOperator` with the\n`COUNT(DISTINCT)` state over one scan, byte for byte, on 1 thread and a\n4-thread pool. It covers:\n  - DOUBLE with -0.0, 0.0, both NaN signs and NULL;\n  - BIGINT with NULLs;\n  - VARCHAR;\n  - repeated calls;\n  - no rows.\n- **`PhysicalPlannerTest.CountDistinctAloneBecomesAGroupBy`:** the plan\nshape, which fails if the rewrite is removed. Mixed columns or\naggregates keep the per-part aggregate. It also checks the counts, with\nNULLs.\n- **Existing tests now on the rewritten path:** the slt/oracle\n`COUNT(DISTINCT)` cases, the diff runs and `MemoryLimitTest`'s\n`COUNT(DISTINCT)` plan.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand tests, and ran the verification and the paired A/B, which found the\ngrouped variant's regressions (dropped). The `reviewer` agent confirmed\nequivalence on every point above. It found that no test would notice the\nrewrite being removed (fixed: the plan-shape test).\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-29T22:13:10+03:00",
          "tree_id": "cad419ee33cdd37383fd9174374c6310ee1fead6",
          "url": "https://github.com/ydb-campus/antb1/commit/c57be667eff4b32dcfd651e30ea1088b950f4d1f"
        },
        "date": 1790709341707,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4074.264595862642,
            "unit": "ns/iter",
            "extra": "iterations: 172429\ncpu: 4072.6794390734744 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84117.23396464637,
            "unit": "ns/iter",
            "extra": "iterations: 7920\ncpu: 84060.97348484848 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222142.85379004225,
            "unit": "ns/iter",
            "extra": "iterations: 3153\ncpu: 222033.4256263875 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 443332.5535264458,
            "unit": "ns/iter",
            "extra": "iterations: 1588\ncpu: 443063.8753148616 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 402889.1644623272,
            "unit": "ns/iter",
            "extra": "iterations: 1739\ncpu: 402714.66705002857 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2135127.6073619323,
            "unit": "ns/iter",
            "extra": "iterations: 326\ncpu: 2134069.162576688 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 224.98784633333457,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 224.96815799999993 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.57294004166639,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.566668395833334 ms\nthreads: 1"
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
          "id": "fb1d1545baf143b6dd108358b3cdaeadc8120d8d",
          "message": "perf(io,exec): skip row groups by their statistics (#50)\n\n## Summary\n\nThis is the next ClickBench follow-up after #49: skipping Parquet row\ngroups using the min/max statistics in the footer. It is also groundwork\nfor TPC-H date-range filters.\n\n**The problem:** Q36-Q42 filter on columns the files are sorted by, so\nalmost every row group can be ruled out from its footer statistics.\nantb1 read every row group of the referenced columns anyway.\n\n**The change:**\n- **`plan::Table::part_stats(part, field)`** (new; defaults to\n\"unknown\"). It returns exact `min`/`max` over the non-NULL values plus\n`null_count` and `rows` for a part.\n- **`io::ParquetTable`** fills it from each row group's column-chunk\nstatistics:\n- only for integer-valued engine columns: SMALLINT, INTEGER, BIGINT,\nUSMALLINT, and DATE as day numbers, including the DATE override;\n- the field is mapped to its Parquet **leaf** index through the schema\nmanifest.\n  - **Never used for skipping:**\n- HUGEINT: fixed-length decimal statistics are unreliable across\nwriters.\n    - DOUBLE: NaN may be left out of min/max.\n    - VARCHAR: min/max may be truncated.\n    - Files without statistics.\n- **`exec/part_pruning`** (private) plus the physical planner:\n- **Which predicates are used:** those of the Filters directly on a part\npipeline's scan, with their columns mapped to table fields through\n`ScanNode::fields`.\n- **When a part is skipped:** only when some predicate is false for\n**every** row of it:\n    - `= < <= > >=` against `[min, max]`;\n    - `<>` when every non-NULL value equals the constant;\n    - `IN` with every value outside `[min, max]`;\n    - `FALSE`;\n    - comparisons, `IN` and `IS NOT NULL` over an all-NULL part.\n- **Never skips:** anything else, including column-vs-column\ncomparisons, `NOT IN`, `LIKE`, computed conditions and Filters above a\nCompute.\n  - **How the kept parts are used:**\n- Part union, aggregate, GROUP BY, top-N and `COUNT(DISTINCT)` pipelines\niterate the kept parts in part order.\n- A skipped part contributes exactly what a part with no selected rows\ndid before, so results stay byte-identical for every thread count.\n    - With no part kept, a query behaves as over an empty table.\n- **EXPLAIN and the logical plan are unchanged.** Docs: architecture.md\n(\"Skipping parts\") and ADR 0013 (plan item 6).\n\n## Performance: full ClickBench data, 128 threads, paired A/B\n\nFor each query, the #49 binary and this PR's binary each ran 3 tries\n(best taken), alternating which went first. There were no failed runs.\nTimes are in seconds.\n\n| Query | main (#49) | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| Q19 | 0.030 | 0.022 | 1.37× |\n| Q36 | 0.757 | 0.195 | 3.88× |\n| Q37 | 0.614 | 0.100 | 6.17× |\n| Q38 | 0.662 | 0.101 | 6.58× |\n| Q39 | 1.363 | 0.440 | 3.10× |\n| Q40 | 0.235 | 0.036 | 6.51× |\n| Q41 | 0.218 | 0.032 | 6.74× |\n| Q42 | 0.186 | 0.040 | 4.69× |\n| **Total (43 queries)** | **48.4** | **45.2** | 1.07× |\n\nNo other query moved by more than the run-to-run noise (about 5%).\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1336\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3315665945 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**New tests:**\n- **exec, `FiltersSkipPartsByTheirStatistics`:** a truth table over\nevery operator at and around the min/max edges, `IN`, `<>` and unknown\nstatistics. The expected kept parts are computed from the data.\n- **exec, `NullPartsAndFalseSkipEverything`:** all-NULL parts and\n`FALSE`.\n- **exec, `SkippingMapsScanColumnsToTableFields`:** scan fields in a\ndifferent order than the table fields; a Filter above a Compute does not\nskip.\n- **io, `PartStatisticsOfIntegerColumns`:**\n- integer columns and the DATE override from USMALLINT and INTEGER\n(including dates before 1970);\n  - all-NULL row groups;\n  - HUGEINT, DOUBLE and VARCHAR (none);\n  - a file written without statistics;\n  - out-of-range arguments.\n- **io, `NestedFieldsShiftLeafIndices` (extended):** the statistics come\nfrom the column's own leaf when a struct shifts its leaf index. The\nstruct itself has none.\n- **integration, `Scan.PartStatisticsMatchThePartsRows`:** for every\nfixture, part and field, the reported min, max and NULL count equal\nthose of the part's scanned rows. Other types report none.\n- **engine, `SkippedRowGroupsDoNotChangeAnswers`:** SQL answers on a\nsorted file with many row groups, on 1 and 4 threads. It covers\naggregates, `IN`, top-N, GROUP BY and `COUNT(DISTINCT)` over the kept\nparts only and over no part at all, NULL-only parts, and filters beside\ncomputed conditions.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs, ran the gates and the benchmark, and ran a read-only\nreviewer agent on the diff. Its findings (HUGEINT statistics, the\nuntested leaf mapping, and skipped parts under GROUP BY and top-N) are\nfixed.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T00:59:24+03:00",
          "tree_id": "f63ddd0f4c6c7941475924dee215c1662da972c3",
          "url": "https://github.com/ydb-campus/antb1/commit/fb1d1545baf143b6dd108358b3cdaeadc8120d8d"
        },
        "date": 1790719307143,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4281.4928834055645,
            "unit": "ns/iter",
            "extra": "iterations: 166723\ncpu: 4281.175980518586 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84469.02076634389,
            "unit": "ns/iter",
            "extra": "iterations: 7464\ncpu: 84460.12138263667 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222144.01109702018,
            "unit": "ns/iter",
            "extra": "iterations: 3154\ncpu: 222091.95719720988 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 443243.13269841677,
            "unit": "ns/iter",
            "extra": "iterations: 1575\ncpu: 443175.04126984114 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 361104.0590979894,
            "unit": "ns/iter",
            "extra": "iterations: 1929\ncpu: 361035.76516329695 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2125493.7269938714,
            "unit": "ns/iter",
            "extra": "iterations: 326\ncpu: 2125291.2944785296 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 217.28938733333317,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 217.234822 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.849394680850843,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.84818185106383 ms\nthreads: 1"
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
          "id": "855aede93855bb399b1b4bf768b5daeb7870b6ed",
          "message": "perf(exec): two-level grouped count(distinct) (#51)\n\n## Summary\n\nThis PR speeds up aggregations with `COUNT(DISTINCT)` by running them in\ntwo levels. The design is in the new **ADR 0014** (status Proposed); the\nplan was agreed on 2026-09-30.\n\n**The problem** (profiled on the full data at 128 threads): grouped\n`COUNT(DISTINCT x) ... GROUP BY K` kept its distinct pairs in a\nper-group state, merged in 64 partitions by `hash(K)`. Two costs:\n- **Skew:** all pairs of one K land in one partition. On Q8 the busiest\npartition did about 6.7× the average work while the other 63 waited.\n- **A serial step:** every part's pairs were sorted by group\n(`PairsByGroup`, about 0.4 s in total on Q8) under a `call_once` that\nthe other 63 partition tasks waited on.\n\n**The change: `PartTwoLevelAggregateOperator`** (exec only; the logical\nplan and EXPLAIN are unchanged)\n- **Inner level:** each part builds one `GroupTable` per distinct column\n(K ∪ {x_i}) plus a plain table by K for the other calls. Rows are never\ncopied, unlike Expand.\n- **Heavy keys:**\n- A sample of the first parts (≥ 4M rows, chosen from `part_rows`\nmetadata, never by the thread count) feeds a Misra–Gries summary\n(`HeavyHitters`) of `hash(K)`.\n- A K is heavy when it may hold more than 1/128 of the sample's inner\ngroups.\n- **Partitioning** (`GroupTable::Partition(prefix, heavy)`, a pure\nfunction of each group's keys):\n  - a light K goes to `hash(K)` in every table;\n  - a heavy K is spread by `hash(K, x_i)`.\n- **Outer level** (`OuterGroups`), per partition in parallel:\n  - group by K;\n- `COUNT(DISTINCT x_i)` is the number of inner groups with a non-NULL\nx_i;\n  - plain states merge through `MergeGroups`;\n  - the light groups' output rows are built in the same step.\n- **Heavy merge:** the heavy K's partial groups merge serially across\npartitions in partition order. The counts add exactly, because each (K,\nx_i) lives in exactly one partition.\n- **When it applies** (`TwoLevelAggregation`):\n  - some `COUNT(DISTINCT)` of a non-key column;\n  - no DOUBLE key;\n- every other call independent of its merge order: COUNT, integer\nSUM/AVG, DATE/TIMESTAMP AVG, and MIN/MAX of anything but DOUBLE.\n\nSeveral distinct columns and global aggregations are included; a global\n`COUNT(DISTINCT x)` alone keeps the #49 rewrite. Everything else keeps\nthe existing operators.\n- **Also:** the partitioned GROUP BY merge now takes one Take per key\ncolumn, then zero-copy slices, instead of 64 Takes. This speeds up every\nparallel GROUP BY with many small parts.\n- **Invariants:** internal ones in the new code are `ANTB1_CHECK`s\n(programming errors), not runtime statuses.\n\nResults are byte-identical for any number of threads: constant\npartitions, a fixed hash, a metadata-chosen sample, and merges in part\nand partition order. They match the serial operators; the row order\ndiffers, as SQL allows.\n\n## Performance: full ClickBench data, 128 threads, paired A/B against\nmain (#50)\n\nBest of 3 per query per binary, alternating which ran first. The host\nwas busy (load average 50-70), so small differences are noise.\n\n| Query | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| Q8 | 1.637 | 0.889 | 1.84× |\n| Q9 | 1.704 | 0.953 | 1.79× |\n| Q10 | 0.414 | 0.235 | 1.76× |\n| Q11 | 0.421 | 0.254 | 1.66× |\n| Q13 | 1.184 | 0.877 | 1.35× |\n| Q21 (GROUP BY, single Take) | 1.043 | 0.781 | 1.34× |\n| Q22 | 1.947 | 1.708 | 1.14× |\n| **Total (43 queries)** | **47.2** | **44.0** | 1.07× |\n\nNo query got slower beyond run-to-run noise (queries under 0.2 s vary by\nabout ±8%).\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1342\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2894401062 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**New tests:**\n- **exec, `TwoLevelAggregationIsTheSerialOne`:** the operator equals the\nserial `GroupAggregateOperator`/`ScalarAggregateOperator` rows, and is\nbyte-identical on 1 and 4 threads.\n- Keys: a skewed BIGINT key with NULL, two keys, a VARCHAR key, a DATE\nkey, and none.\n- Calls: two distinct columns (one repeated) with COUNT(*), COUNT, SUM,\nAVG, MIN and MAX.\n  - Samples of 0, 3 and all 16 parts; empty parts; no rows.\n- **exec, `TwoLevelAggregationNeedsOrderIndependentCalls`:** the\neligibility rules.\n- **exec, `HeavyKeysSpreadOverThePartitions`:**\n- light keys land in exactly one partition, heavy keys in many,\ndeterministically;\n  - with no keys, the groups spread by the column.\n- **exec, `HeavyHittersTest`:** the Misra–Gries guarantees and\ndeterminism.\n- **exec, `PhysicalPlannerTest.CountDistinctRunsInTwoLevels`:** the plan\nshapes, including every fallback (DOUBLE key, DOUBLE SUM, a distinct\ncolumn that is a key, a single global distinct column).\n- **exec, `MemoryLimitTest`:** a two-level plan fails cleanly past the\nmemory limit.\n- **slt, `distinct/count_distinct.slt`:** 5 cases, with expectations\nfrom `pixi run slt-complete`:\n  - two distinct columns with other calls under a low-cardinality key;\n  - DATE and multi-key grouping;\n  - a high-cardinality VARCHAR key;\n- a global aggregation with several distinct columns, and one with no\nrows.\n\n  The `parallel.*` label runs them on 4 threads against 1 thread.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code profiled the\nmerge, wrote the code, tests, ADR and docs, ran the gates and the\nbenchmark, and ran a read-only reviewer agent on the diff. The reviewer\nfound no P0/P1 issues. Its P2 is fixed: a global aggregation did not\nspread its groups over the partitions.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T05:21:07+03:00",
          "tree_id": "c2eb9069ac22d8508282a8951d917e5d2f43b9a4",
          "url": "https://github.com/ydb-campus/antb1/commit/855aede93855bb399b1b4bf768b5daeb7870b6ed"
        },
        "date": 1790735020215,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4172.0540034810165,
            "unit": "ns/iter",
            "extra": "iterations: 168341\ncpu: 4170.705971807224 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84338.24329453458,
            "unit": "ns/iter",
            "extra": "iterations: 7904\ncpu: 84328.40270748988 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 221884.9990491322,
            "unit": "ns/iter",
            "extra": "iterations: 3155\ncpu: 221871.97527733748 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 439560.1797323182,
            "unit": "ns/iter",
            "extra": "iterations: 1569\ncpu: 439528.25175270875 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 361282.4678332522,
            "unit": "ns/iter",
            "extra": "iterations: 1943\ncpu: 361154.7081832218 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2101785.8484848756,
            "unit": "ns/iter",
            "extra": "iterations: 330\ncpu: 2101583.7575757587 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 184.55039075000457,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 184.54106925000002 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.865616234042717,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.863896276595744 ms\nthreads: 1"
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
          "id": "b78d502ad708425e533c41330c341332936c6864",
          "message": "perf(exec): merge group partitions without per-part barriers (#52)\n\n## Summary\n\nThis is fix 1 of the antb1 vs DuckDB comparison. That comparison found\nantb1 3.5× behind DuckDB in total, with GROUP BY queries keeping far\nfewer threads busy (Q32: 14× against DuckDB's 43×).\n\n**The problem, measured on the full data at 128 threads:**\n- Every part's merge ran its 64 partition tasks and then waited for all\nof them before the next part could start.\n- Summed over the 325 parts, the slowest partition took 3.6× the mean on\nQ32 and 8× on Q8. The other partitions idled.\n- After the merge, one thread finalized and emitted every group: 4.3 s\nof Q32's 8.2 s.\n\n**The change** (`exec` only; results unchanged):\n- **Partition lanes** (`src/exec/partition_lanes.{h,cc}`,\n`PartitionLanes`): each partition merges the parts in part order in its\nown lane, a serial queue drained by one executor task at a time.\n  - `Add(part)` hands a part to every lane and returns without waiting.\n  - A part is freed once every lane has merged it.\n- Back-pressure: at most `window` parts wait to be merged, and one under\nmemory pressure.\n- Errors: the failure of the smallest (part, partition) wins, and it is\ncombined with the scheduler's failures so the earliest failing part is\nreported, whatever the timing. `bad_alloc` becomes `OutOfMemory`.\n- Without an executor, a part merges into lane 0, 1, ... on the calling\nthread, as before.\n- Used by `PartGroupAggregateOperator` and\n`PartTwoLevelAggregateOperator`.\n- **Parallel output:** `PartGroupAggregateOperator` builds the\npartitions' rows in parallel, as many partitions at a time as threads,\none under memory pressure so the rows built ahead stay bounded. They are\nemitted in partition order as before.\n- **Out-of-memory retry:** `PartScheduler::set_before_retry` lets both\noperators wait for their lanes to finish before a part that ran out of\nmemory runs again alone. The retry then has the headroom it had before\n(reviewer finding).\n- **Partition count stays 64.** 128 and 256 partitions were 10-60%\nslower, because every part then pays more small merges.\n- Docs: ADR 0013 (amended plan item, no status change), ADR 0014\nwording, and architecture.md (operator row, memory section).\n\n## Performance: full ClickBench data, 128 threads, paired A/B against\nmain (#51)\n\nBest of 3 per query per binary, alternating which ran first.\n\n| Query | #51 | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| Q7 | 0.097 | 0.035 | 2.74× |\n| Q8 | 0.877 | 0.332 | 2.64× |\n| Q9 | 0.947 | 0.407 | 2.33× |\n| Q32 | 6.812 | 3.461 | 1.97× |\n| Q31 | 1.551 | 0.822 | 1.89× |\n| Q10 | 0.222 | 0.143 | 1.55× |\n| Q11 | 0.251 | 0.161 | 1.56× |\n| Q30 | 0.946 | 0.607 | 1.56× |\n| Q4 | 0.428 | 0.316 | 1.36× |\n| Q13 | 0.860 | 0.635 | 1.35× |\n| Q16 | 1.390 | 1.057 | 1.32× |\n| Q15 | 0.675 | 0.516 | 1.31× |\n| Q14 | 0.868 | 0.661 | 1.31× |\n| Q17 | 1.068 | 0.821 | 1.30× |\n| Q18 | 2.568 | 2.028 | 1.27× |\n| Q5 | 0.565 | 0.447 | 1.26× |\n| Q12 | 0.687 | 0.556 | 1.24× |\n| Q33 | 2.500 | 2.092 | 1.19× |\n| Q34 | 2.532 | 2.113 | 1.20× |\n| Q35 | 1.106 | 0.954 | 1.16× |\n| **Total (43 queries)** | **42.7** | **33.3** | 1.28× |\n\nNo query got slower beyond run-to-run noise.\n\n**Against DuckDB 1.5.5** (same setup): antb1 went from 42.8 s to 33.2 s\nagainst DuckDB's 12.2 s, so the gap is now 2.7×, down from 3.5×. Q8 now\nkeeps 32 threads busy, up from 15.\n\nWhere Q32's remaining time goes (measured after this change, quiet host;\nQ32 has about 100 million groups, one per row):\n\n| Phase | Time |\n| --- | ---: |\n| Reading and grouping the parts, until the last part is taken | 2.5 s |\n| Lanes still merging after the last part | 1.1 s |\n| Building the output rows | 0.4 s |\n| ORDER BY … LIMIT above it, on one thread | 1.0 s |\n\nThe profile is dominated by hash-table inserts and state merges. Each\ngroup is hashed twice (in its part, then in the merge), because per-part\npre-aggregation cannot reduce one-group-per-row data. Skipping\npre-aggregation there is the larger follow-up; a per-partition top-N is\nthe smaller one.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\nlint PASS; ci, asan, ci-gcc: 100% tests passed out of 1350; tidy clean; fuzz-smoke 2/2\ncoverage: PASS on rerun (see note)\n$ pixi run tsan\n100% tests passed out of 1350\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=94908035 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**Coverage note:** one coverage run reported io branch coverage 86.42%\nagainst the 86.6% floor. This PR does not touch io. A rerun gave 86.79%\n(230/265), the same as on #51.\n- Diffing the two runs' lcov shows three io branches in\n`parquet_table.cc` flipping between covered and uncovered from run to\nrun.\n- io sits exactly at its floor, so one lost branch fails the gate.\n- The floor is unchanged. The flakiness probably belongs in its own\nissue.\n\n**New tests:**\n- **`PartitionLanesTest`,** on 1 thread and the 4-thread pool:\n  - every lane merges the parts in order;\n  - without an executor, lanes run inline in partition order;\n  - a slow lane holds back no other;\n- pending parts stay bounded (and at 1 under pressure), and merge\nfunctions are released;\n  - the earliest (part, lane) failure wins over 20 runs;\n  - `bad_alloc` becomes `OutOfMemory`;\n  - a failed submit on a shut-down pool fails the lane.\n- **`PartOperatorsTest.MergeAndReadFailuresFollowPartOrder`:** a HUGEINT\nmerge overflow plus a failed read at earlier and later parts. The same\nerror is reported on 1 and 4 threads.\n-\n**`PartSchedulerTest.OutOfMemoryInParallelFallsBackToOnePartAtATime`:**\nthe retry hook runs exactly once, before the rerun.\n- **`MemoryLimitTest.SinksGiveTheSameResultsUnderPressure`:** every sink\nplan above half of the limit gives the unlimited results. This covers\none-at-a-time merges and row building.\n- **Existing tests:** the grouping and two-level tests are\nbyte-identical across thread counts, including the `parallel.*` slt.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code benchmarked\nagainst DuckDB, profiled, and wrote the code, tests and docs. It ran the\ngates and the A/B, and ran a read-only reviewer agent on the diff. The\nreviewer found no P0 issues. Both P1s are fixed:\n  - the out-of-memory retry now waits for the lanes;\n- tests were added for the submit-failure, merge-versus-read error order\nand pressure paths.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T08:33:41+03:00",
          "tree_id": "35d51dfa97aada3e641774ae51fdb1cd2fea2434",
          "url": "https://github.com/ydb-campus/antb1/commit/b78d502ad708425e533c41330c341332936c6864"
        },
        "date": 1790746555451,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3086.06541644468,
            "unit": "ns/iter",
            "extra": "iterations: 227053\ncpu: 3085.869946664435 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73209.41861958258,
            "unit": "ns/iter",
            "extra": "iterations: 9345\ncpu: 73202.61391118245 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 84704.33611716377,
            "unit": "ns/iter",
            "extra": "iterations: 8262\ncpu: 84672.04103122733 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 376208.63412016764,
            "unit": "ns/iter",
            "extra": "iterations: 1864\ncpu: 376061.35515021475 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 353561.0223523204,
            "unit": "ns/iter",
            "extra": "iterations: 1879\ncpu: 353505.4167110165 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2190792.5440251073,
            "unit": "ns/iter",
            "extra": "iterations: 318\ncpu: 2190548.4748427696 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 202.38349733333885,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 202.342686 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.713955283333158,
            "unit": "ms/iter",
            "extra": "iterations: 60\ncpu: 11.712748633333328 ms\nthreads: 1"
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
          "id": "7afc13b65b5fff1e1411249642fb24a4c3a7ee53",
          "message": "feat(cli,exec): explain --analyze with per-operator profiles (#53)\n\n## Summary\n\n`antb1 explain --analyze` runs a query, drops its rows, and prints every\n**physical** operator with what it did. The design is in the new **ADR\n0015** (status Proposed).\n\n**Why:** the performance work (the DuckDB comparison, #51 and #52) kept\nneeding timers patched into operators by hand, rebuilt and thrown away.\nOnce that misled us: the ORDER BY LIMIT above Q32's GROUP BY was taken\nfor most of its remaining time, and it measured at a fifth.\n\n**Scope** (the maintainer's choice, 2026-09-30): a CLI flag only, with\ntext plus `--format json`. The SQL `EXPLAIN [ANALYZE]` statement is\ndeferred; the parser still rejects the keyword.\n\n**Output on a hermetic fixture** (4 files, 11 row groups):\n\n```text\n$ antb1 explain --analyze --threads 1 -c \"SELECT RegionID, COUNT(*) AS c FROM t WHERE IsMobile = 1 GROUP BY RegionID ORDER BY c DESC, RegionID LIMIT 3\" --table t=hits_like_split/part-*.parquet\nOutput: RegionID:INTEGER c:BIGINT\nTotal: time=39.106ms rows=3 peak_memory=4.59 MB threads=1\nProject RegionID, c  [rows=3 batches=1 time=38.946ms self=0.007ms]\n  TopN Sort c DESC NULLS LAST, RegionID ASC NULLS LAST Limit 3  [rows=3 batches=1 time=38.939ms self=11.831ms sort=0.011ms]\n    PartGroupAggregate GroupAggregate keys=[RegionID] COUNT(*)  [rows=4357 batches=698 time=27.108ms parts=11 skipped=0 part_time=6.161ms wait=6.185ms lanes_tail=0.001ms build=2.297ms groups=4357]\n      Filter IsMobile = 1  [rows=5064 batches=11 parts=11 time=2.075ms (summed over parts) self=0.288ms]\n        Scan table=t source=parquet(files=4, rows=10000) columns=[RegionID, IsMobile]  [rows=10000 batches=11 parts=11 time=1.787ms (summed over parts)]\n```\n\nThe physical plan shows what the logical EXPLAIN hides: part pipelines,\npruned parts, the top-N, and the COUNT(DISTINCT) and two-level rewrites.\n\n**Checked against the Q32 investigation** (full data, 128 threads,\nrelease build; operator names and numbers only):\n\n| Measure | Manual timers | `explain --analyze` |\n| --- | ---: | ---: |\n| Lanes merging after the last part | 1.09 s | 1.109 s |\n| Row building | 0.39 s | 0.375 s |\n| TopN on one thread | 1.0 s | 0.971 s (self) |\n| Waiting for parts | 2.5 s | 2.563 s |\n\n**Design:**\n- **Profile tree:** `exec::ProfileNode` (public `profile.h`), one node\nper physical operator.\n  - Atomic counters: rows, batches, time, runs.\n  - Named metrics (counts, durations, bytes) under a mutex.\n- Text from `plan::ExplainNode`, which is the logical EXPLAIN line\nrefactored out; `antb1 explain` output is unchanged.\n- **Zero cost when off:** only `BuildPhysicalPlan(plan, root)` with a\nroot profiles.\n- It wraps each operator in the private `ProfiledOperator`, which times\nOpen, Next and Close and counts batches.\n- It hands the node to the operator through `Operator::set_profile`, so\nthe operator can add its own metrics.\n  - Without a root there is no wrapper and no clock read.\n- **Part pipelines:** their nodes are created by the plan-time sample\nbuild. Every part's operators add into the same nodes from any thread.\n- Workers only read a node's name, detail and per-part flag. The\nreviewer caught a race there, now fixed.\n- Counts don't depend on the thread count, except under a LIMIT (it\nstops parts that already started) and after an out-of-memory retry. The\ndocs say so.\n- **Metrics:**\n- part operators: `parts`, `skipped` by statistics, `part_time`, `wait`;\n  - GROUP BY: `lanes_tail`, `build`, `groups`;\n  - two-level: `sample_parts`, `heavy_keys`, `outer`, `heavy_groups`;\n  - `merge` for PartAggregate and PartTopN, and `sort`;\n  - query peak memory via a new `MemoryBudget::ResetPeak`.\n- **Engine and CLI:**\n- engine: `Session::ExplainAnalyze` gives a `QueryProfile`;\n`FormatProfile` renders text or JSON.\n- CLI: `explain --analyze` with `--threads`, `--memory-limit` and\n`--format text|json` (`--format` only with `--analyze`, else exit 2).\nJSON output also gives JSON errors.\n- **Docs:** ADR 0015, sql-subset.md (an `explain --analyze` section),\narchitecture.md (pipeline, and \"Where to add things\": an operator names\nitself and adds metrics), benchmarks.md (profiling a ClickBench query\nwithout committing its plan text).\n\n**Left for later** (listed in the ADR): per-operator peak memory,\nprofiles in `antb1 bench` JSON, the SQL `EXPLAIN` statement.\n\n**Needs your approval, not included:** one row of the AGENTS.md command\ntable, to mention `antb1 explain --analyze` next to the dev CLI command.\nAGENTS.md is a protected path.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1364 (ci, asan, ci-gcc); Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1364\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=1128002589 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n(The tsan, diff and data runs were on the code before three clang-tidy\nstyle fixes: a `ranges::any_of`, a char overload and a `reserve`.\ncheck-full ran on the final code.)\n\n**Overhead with profiling off:** a paired A/B against #52 on all 43\nqueries came out at 39.45 s against 38.64 s, with scattered changes in\nboth directions. The host was saturated by another workload during it\n(load average up to 155), so this doesn't show overhead either way. I'll\nrerun it on a quiet host and add the table here before merge. By\nconstruction nothing changes when profiling is off: no wrapper, and only\na null check at phase boundaries.\n\n**New tests:**\n- **`ProfileTest`** (exec), on 1 and 4 threads unless noted:\n- the tree mirrors the physical plan, with exact counts, pruned parts\nand details;\n- every operator is named, with its metrics: PartTopN, grouped and\nglobal two-level, a serial GROUP BY, Sort and ScalarAggregate over\nanother aggregation;\n- a LIMIT stops parts: the limit's rows are exact and the parts' counts\nbounded;\n  - a failing part passes its status through, and its runs are counted;\n  - nothing is wrapped without a root;\n  - nodes add up correctly across threads.\n- **`MemoryBudgetTest.ResetPeakStartsFromTheBytesInUse`.**\n- **`SessionTest.ExplainAnalyzeProfilesTheQuery`** (engine): the rows of\nExecute, text and JSON structure, the same counts on 1 and 4 threads,\nand errors equal to Execute's.\n- **CLI goldens:**\n- `explain_analyze` and `explain_analyze_json`, with times and memory\nmatched by regex;\n- `explain_analyze_bind_error` and `explain_analyze_json_error` (the\nJSON error object);\n  - `usage_explain_format_needs_analyze` (exit 2).\n- The plain `explain` goldens are unchanged.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed (the AGENTS.md row awaits\napproval, see above)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests, ADR and docs, ran the gates, and ran a read-only reviewer agent\non the diff. The reviewer found one P0 and three P1s, all fixed:\n  - a data race on the shared profile nodes' names;\n  - an overclaimed \"exact on any thread count\";\n  - JSON errors printed as text;\n  - missing tests.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T09:52:12+03:00",
          "tree_id": "fc3c6e1078efe9f1ec356e8490d400e429bf933e",
          "url": "https://github.com/ydb-campus/antb1/commit/7afc13b65b5fff1e1411249642fb24a4c3a7ee53"
        },
        "date": 1790751254740,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3319.822028365768,
            "unit": "ns/iter",
            "extra": "iterations: 211382\ncpu: 3319.325373021354 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 86447.61796160508,
            "unit": "ns/iter",
            "extra": "iterations: 7761\ncpu: 86442.9684319031 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 95415.87460470288,
            "unit": "ns/iter",
            "extra": "iterations: 7273\ncpu: 95410.84353086757 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 300074.14614703716,
            "unit": "ns/iter",
            "extra": "iterations: 2258\ncpu: 300065.3737821082 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 392844.57418987167,
            "unit": "ns/iter",
            "extra": "iterations: 1759\ncpu: 392830.77146105724 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2207622.7232704517,
            "unit": "ns/iter",
            "extra": "iterations: 318\ncpu: 2207481.188679246 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 164.51187749999718,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 164.49952374999998 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.705413881355945,
            "unit": "ms/iter",
            "extra": "iterations: 59\ncpu: 11.704694694915254 ms\nthreads: 1"
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
          "id": "354e0577e959aa97120a0b90954da51ca7393f49",
          "message": "perf(exec): late materialization of top-N columns (#54)\n\n## Summary\n\nThis is fix 2 of the antb1 vs DuckDB comparison: **late materialization\nfor top-N**. The design is in the new **ADR 0016** (status Proposed). It\nalso answers the earlier question of who decides when to use it.\n\n**The problem, from `antb1 explain --analyze` on Q23** (full data, 128\nthreads; query numbers and metadata only):\n- The part pipeline decoded every column of the table (105) in every row\ngroup (325): 431-502 s of CPU summed over the parts.\n- The filter and the top-N merge cost almost nothing next to that, and\nthe result is a handful of rows.\n\n**The change** (exec only; the logical plan, the optimizer and plain\nEXPLAIN are unchanged):\n- **Which columns are late:** for `Limit(Sort(part pipeline))`, where\nthe pipeline is Filter and Compute nodes over a Scan, the planner\n(`LateSplit`) marks late every scan column that no filter predicate, no\ncomputation and no sort key reads.\n- **When it applies:** when `limit + offset ≤ kept parts / 2` (after\nstatistics pruning) and ≤ 65536. So at least half of the late decoding\nis saved. The planner decides from metadata; a cost-based choice waits\nfor column statistics.\n- **Narrow scan** (`LateScan` in `TableScanOperator`):\n  - Each part reads only the early columns.\n- Late columns become `arrow::NullArray` placeholders, which cost no\nbuffers and no decoding.\n- One of them carries each row's id: the part number times 2^32, plus\nthe position in the part.\n- Filter, Compute and the part top-N buffers carry the narrow rows\nunchanged. Sort and tie order are the same: input order within a part,\nthen part order.\n- **Fetch** (`PartTopNOperator::Fetch`): after the merge, the output\nwindow's rows are grouped by part.\n- One task per (part, late column) runs on the executor: it reads that\ncolumn of that part and takes the rows.\n  - The values go back in window order with the original schema.\n- **Profile:** `explain --analyze` shows `late=N columns` and the\nmetrics `late_columns`, `late_parts` and `late_fetch`.\n- **Docs:** ADR 0016 (new) and its index row, a pointer in ADR 0011, the\nPartTopN row of architecture.md, and a sql-subset.md optimizer note.\n\n**Q23 in `explain --analyze`, before and after:**\n\n| | main | this PR |\n| --- | ---: | ---: |\n| Query time | 4.98 s | 1.08 s |\n| Scan CPU (summed over parts) | 431 s | 45 s |\n| Peak memory | 21.4 GB | 3.6 GB |\n| Late fetch | - | 103 columns from 7 of 325 parts, 141 ms |\n\n## Performance: paired A/B against main (#53), full data, 128 threads\n\nThis uses a new layout-robust method. After #53 we found that the length\nof the binary's path (argv[0], which shifts the stack) alone moves Q4 by\nabout 15%. So each binary now runs from 3 paths of different lengths\n(best of 3 tries each), and the median is kept; the binaries alternate.\n- Q0-Q26 ran on a quiet host.\n- Q27-Q42 were rerun after another workload pushed the host load to 180\nduring the first run.\n\n| Query | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| **Q23** | 3.945 | 0.866 | **4.55×** |\n| Q24-Q26 (top-N, every column early) | 0.15-0.19 | same | within 3% |\n| Every other query | | | within 6%, most within 3% |\n\nQ24-Q26 read only columns their filters or keys use, so nothing is late;\nas expected, they are unchanged. The total over all 43 queries dropped\nby about 3 s, which is Q23.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1371 (ci, asan, ci-gcc, coverage); Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1371\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3886143830 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n(The diff and data runs were on the code before the clang-tidy style\nfixes, which rewrote the row-id encoding from shifts to arithmetic with\nthe same values. check-full and tsan ran on the final code.)\n\n**New tests:**\n- **`PartOperatorsTest.TopNFetchesLateColumns`:** 25 late and plain\nplans over a wide 16-part table.\n- The plans: every column type with NULLs, ties on the key, a filter\nwith statistics-pruned parts, a filter with no rows, a computed key, and\na grid of (limit, offset) windows inside, across and past the rows.\n  - Each result is byte-identical to the plain top-N on 1 and 4 threads.\n- Failures pass through: a failed part read, and a failed late fetch (a\nnew `MemoryTable::FailField` hook).\n- **`PartOperatorsTest.NarrowScanCarriesRowIds`:** NULL placeholders,\nrow ids across batches, invalid settings.\n- **`PartOperatorsTest.LateTopNChecksItsColumns`:** malformed\n`LateColumns` fail to open.\n- **`PhysicalPlannerTest.TopNReadsUnusedColumnsLate`:**\n  - The rule applies at `keep` ≤ half the parts and not above.\n  - Only unread columns are late.\n- It does not apply when every column is read, or when a Project inside\nthe pipeline renumbers columns.\n- **slt `orderby/late.slt`:** 4 queries on the multi-file fixture, with\nexpected results from `pixi run slt-complete`. The `parallel.*` label\nruns them on 4 threads.\n- **Profile test:** the PartTopN detail now says `late=1 columns`.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code profiled Q23\nwith `explain --analyze`, wrote the code, tests, ADR and docs, ran the\ngates and the A/B, and ran a read-only reviewer agent on the diff. The\nreviewer found no P0 issues and two P1s, both fixed:\n- ClickBench-derived figures (a selectivity and the query's shape) in\nthe ADR draft were replaced by metadata counts;\n  - the late top-N's validation is now tested.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T11:57:57+03:00",
          "tree_id": "6fa5e6abf19c07fff4ed0b1f01a02ecc8c82efe4",
          "url": "https://github.com/ydb-campus/antb1/commit/354e0577e959aa97120a0b90954da51ca7393f49"
        },
        "date": 1790758807155,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3126.883834482311,
            "unit": "ns/iter",
            "extra": "iterations: 213609\ncpu: 3126.1338894896758 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73465.27910221631,
            "unit": "ns/iter",
            "extra": "iterations: 8599\ncpu: 73442.80637283405 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 84723.44553258817,
            "unit": "ns/iter",
            "extra": "iterations: 8271\ncpu: 84713.38181598356 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 375812.19903431396,
            "unit": "ns/iter",
            "extra": "iterations: 1864\ncpu: 375656.1893776822 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 355151.86002018413,
            "unit": "ns/iter",
            "extra": "iterations: 1986\ncpu: 355076.49144008034 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2176106.82608685,
            "unit": "ns/iter",
            "extra": "iterations: 322\ncpu: 2175570.6739130425 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 180.20651624999573,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 180.16205775000006 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.777687233332776,
            "unit": "ms/iter",
            "extra": "iterations: 60\ncpu: 11.77542645000001 ms\nthreads: 1"
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
          "id": "3469588119b292a8a0b638cc8e83db5a4f5c33d5",
          "message": "perf(exec): route rows of non-reducing parts straight to the partitions (#55)\n\n## Summary\n\nThis is fix 4 of the antb1 vs DuckDB comparison: **skip per-part\npre-aggregation when it does not reduce**.\n\n**The problem:** in a GROUP BY with about one group per row (Q32: 100 M\ngroups from 100 M rows), each part inserts every row into its own table,\nsplits that table by key hash, and then every group is inserted again\ninto its partition's table (keys copied, states merged). The per-part\nwork is almost pure overhead.\n\n**The change** (exec only; results unchanged):\n- **Routing:** a part aggregates its first **4096 rows** (or its first\nbatch, if smaller) in its own table. If they make more groups than\n**3/4** of those rows, the part routes its other rows straight to the\npartitions.\n- `GroupTable::RouteRows` splits them by the **same key hash** as\n`Partition()`, with DOUBLE keys normalized, using one stable Take and\none slice per partition.\n- Each lane merges the part's own groups first, then consumes its routed\nrows in input order. So first-seen keys, tie rules and COUNT(DISTINCT)\nare exactly as before.\n- The decision depends only on the data and the batch size, never on\nthreads. Waiting for 4096 rows keeps a selective WHERE (a few rows per\nbatch) from triggering it (reviewer finding).\n- Plans with a DOUBLE SUM/AVG (rounding follows the parts) or a HUGEINT\nSUM/AVG (the overflow check follows the order of additions) keep the\nparts' own tables.\n- **Joined output chunks:** `GroupTable::NextChunk` now joins\nconsecutive small chunks of new groups, up to 64Ki groups and 16 MiB of\nkeys, and finalizes each state once per range.\n- Rows merged a few at a time otherwise made about 100,000 tiny output\nbatches (Q30: 20,627 before routing, 100,320 with it), and the top-N\nabove them paid per batch.\n- The byte bound keeps VARCHAR keys far from Arrow's 2 GiB binary limit,\nwith a test.\n- This also speeds up plain GROUP BYs (Q30, Q31) by giving the top-N\nfewer batches.\n- **Profile:** `raw_parts` and `raw_rows` in `explain --analyze`.\n- **Docs:** architecture.md (the PartGroupAggregate row), an ADR 0013\namendment (plan item 4), and the sql-subset.md metric list.\n\n## Performance\n\nThese are paired runs against main (#54), full data, 128 threads, using\nthe layout-robust method (each binary from 3 path lengths, best of 3,\nmedian), during the threshold sweep. The host load rose from 10 to 60\nduring these runs; the pairs alternate, but small differences are noise.\n\n| Query | main | ¾ threshold (this PR) | never route (joined chunks\nonly) |\n| --- | ---: | ---: | ---: |\n| Q32 | 3.54 | **2.54 (1.39×)** | 3.51 |\n| Q18 | 2.06 | 1.93 (1.07×) | 2.09 |\n| Q16 | 1.09 | 1.03 (1.06×) | 1.03 |\n| Q30 | 0.63 | 0.57 (1.10×) | 0.54 |\n| Q31 | 0.84 | 0.80 (1.05×) | 0.75 |\n| Q33 | 2.11 | 2.12 | 2.12 |\n| Q34 | 2.10 | 2.13 | 2.10 |\n\n**Q33 and Q34 (GROUP BY a long string) don't change.**\n- Their parts do reduce within the first 4096 rows: Q34 routes only 2 of\n325 parts.\n- Their time is the largest row groups, each aggregated on one thread.\nThat's a separate fix: splitting large row groups (ADR 0013's\nload-balance list).\n\n**Still to do:** a final all-query A/B on a quiet host. The host has\nbeen saturated by another workload (load 150-190) since this build. I'll\nadd the table here before merge.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1377; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1377\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2399248164 queries=20000 failed=0 unsupported=0\nDIFF: PASS seed=2355773739 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\nAbout the diff runs: two earlier diff runs reported 13 and 16 failures.\nEvery one was DuckDB's \"No files found\" for the fixtures: another test\nrun was regenerating `build/dev/fixtures` at the same time. Rerun alone,\nboth seeds pass with 0 failures.\n\n**New tests:**\n- **`PartOperatorsTest.RoutedRowsGiveTheSameGroups`:** the result equals\nthe serial GROUP BY (as a set) and is byte-identical on 1 and 4 threads,\nwith exact `raw_parts` counts.\n- Keys: unique keys, two keys, few keys, a DOUBLE key with\n-0.0/0.0/NaN/NULL, DOUBLE with unique, VARCHAR with unique.\n- Calls: COUNT(*), COUNT, integer SUM, VARCHAR MIN, DOUBLE MAX,\nCOUNT(DISTINCT).\n  - A DOUBLE SUM keeps the parts' own tables.\n  - A selective filter never routes.\n- **`PartOperatorsTest.RoutedRowsFollowTheirGroupsPartition`:** routed\nrows land in the partition of their group, DOUBLE keys included.\n- **`GroupedAggregateTest.LargeKeyChunksStaySeparate`:** the byte bound\non joined keys. `EmitsOneBatchPerChunkOfNewGroups` is updated: small\nchunks are now joined.\n- **slt `groupby/routed.slt`:** 3 queries with expectations from `pixi\nrun slt-complete`. The `parallel.*` run reads batches of 1000 rows, so\nthe fixture's 1024-row parts route rows.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code profiled with\n`explain --analyze`, swept the threshold, and wrote the code, tests and\ndocs. It ran the gates and a read-only reviewer agent on the diff. The\nreviewer found no P0 or P1 issues; its P2 and notes are addressed:\n  - the decision waits for 4096 rows;\n  - HUGEINT sums are excluded;\n  - a stale comment is fixed.\n\nOne suggestion was not taken: charging routed rows to a reservation.\nThey are already Arrow buffers of the budgeted pool, so a reservation\nwould count them twice.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T14:24:55+03:00",
          "tree_id": "61ffd46fdfc3d339609d5d46a7f4a10dd353823a",
          "url": "https://github.com/ydb-campus/antb1/commit/3469588119b292a8a0b638cc8e83db5a4f5c33d5"
        },
        "date": 1790767641165,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3992.347782674532,
            "unit": "ns/iter",
            "extra": "iterations: 174264\ncpu: 3991.4217049993113 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94339.91687657441,
            "unit": "ns/iter",
            "extra": "iterations: 7146\ncpu: 94302.02071088721 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 124723.27923118151,
            "unit": "ns/iter",
            "extra": "iterations: 5619\ncpu: 124654.34347748711 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 486209.1444444457,
            "unit": "ns/iter",
            "extra": "iterations: 1440\ncpu: 485914.19999999984 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 455043.9109811556,
            "unit": "ns/iter",
            "extra": "iterations: 1539\ncpu: 455005.8167641324 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2227226.4126984156,
            "unit": "ns/iter",
            "extra": "iterations: 315\ncpu: 2226938.8158730175 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 201.8456493333313,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 201.81764833333352 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 15.105987652173988,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 15.105285847826098 ms\nthreads: 1"
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
          "id": "6f09d2d549821bf8adc5ceeaf686fc3da44e51c2",
          "message": "perf(exec): regexp_replace once per distinct value of a batch (#56)\n\n## Summary\n\nThis is fix 3 of the antb1 vs DuckDB comparison, **rescoped** with the\nmaintainer on 2026-09-30.\n\n**Why rescoped:** the original fix 3 would evaluate LIKE, regex and\ngrouping on Parquet dictionaries. The footers show that the large string\ncolumns (URL, Title, Referer, OriginalURL) are mostly plain-encoded: the\ndictionary fills about 1 MB early in each row group and the writer falls\nback to plain pages. Dictionary-page evaluation would have barely\nhelped.\n\n**What `explain --analyze` showed** (full data, 128 threads):\n- The LIKE queries (Q20-Q22) spend about 85% of their CPU **decoding**\nthe strings (snappy, plain byte arrays). LIKE itself is about 15%.\n- Q28 spends **81%** of its CPU (316 s summed over parts) in\n`regexp_replace`, in RE2's capture-group engine, about 3.9 µs per row.\n- Values repeat heavily within a batch; about a quarter of the values in\na row group are distinct (measured locally).\n\n**The change** (`src/exec/compute.cc`, `RegexpReplace`):\n- The batch's values are dictionary-encoded with\n`arrow::compute::DictionaryEncode`.\n- The regex runs on the distinct values only.\n- `Take` puts the results back on their rows.\n\nNULLs keep NULL indices; values are hashed as raw bytes, so invalid\nUTF-8 behaves as before (divergence D15); the invalid-pattern and\ninvalid-replacement paths are unchanged. Results are unchanged. Q28's\nregex CPU falls from 317 s to 141 s.\n\n## Performance: full data, 128 threads, paired A/B against main (#55)\n\n**Method:** layout-robust. Each binary runs from 3 path lengths, best of\n3 tries each, and the median is kept.\n\n| Query | main | this PR | speedup | DuckDB 1.5.5 |\n| --- | ---: | ---: | ---: | ---: |\n| **Q28** | 5.835 | 2.636 | **2.21×** | ~2.8 |\n| **Total (43 queries)** | **29.44** | **26.16** | 1.13× | |\n\nQ28 is now faster than DuckDB.\n\nEvery other query is within ±5%. Q5, Q24 and Q26 first showed 0.88-0.91×\nwhile the host load rose to about 55 from another workload. They don't\nuse `regexp_replace`, and a quiet rerun put them within 2-3% (Q5 0.442\nvs 0.450, Q24 0.191 vs 0.198, Q26 0.193 vs 0.197).\n\n**Not in this PR:** the LIKE queries. Their cost is decoding, not\nmatching, so a faster LIKE can save at most about 15%. What would help\nthem is skipping the decode of other columns in row groups where the\nfilter matches nothing; that is a separate change.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1377; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1377\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=840978341 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n(The all-NULL test was added after this run. `pixi run test -R\nStringFunctions` and `pixi run tidy` pass with it.)\n\n**Tests:**\n- **`ComputeTest.StringFunctions`** gains:\n- a batch with repeated values, NULLs, values without a match, `\"\"` and\na single-character value;\n- an all-NULL batch, which leaves the dictionary empty (a reviewer\nsuggestion).\n- **The existing regexp cases** still pass: the metamorphic\n`regexp_replace_*` relations, including batch-size invariance, and the\ninvalid pattern and replacement cases.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code read the Parquet\nfooters, profiled with `explain --analyze` and `perf`, wrote the change\nand tests, and ran the gates and the A/B. A read-only reviewer agent\nfound no P0/P1 issues; its optional all-NULL test is added.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T16:03:37+03:00",
          "tree_id": "6b53ca261cd8f1a376e0041510627521128a86df",
          "url": "https://github.com/ydb-campus/antb1/commit/6f09d2d549821bf8adc5ceeaf686fc3da44e51c2"
        },
        "date": 1790773546761,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2460.8223666261706,
            "unit": "ns/iter",
            "extra": "iterations: 282233\ncpu: 2460.4408378892617 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73514.40157056453,
            "unit": "ns/iter",
            "extra": "iterations: 9296\ncpu: 73507.7673192771 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 77128.27487630086,
            "unit": "ns/iter",
            "extra": "iterations: 9095\ncpu: 77110.84771852668 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 271234.08077671326,
            "unit": "ns/iter",
            "extra": "iterations: 2575\ncpu: 271219.4027184465 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 319170.4869922383,
            "unit": "ns/iter",
            "extra": "iterations: 2191\ncpu: 319143.3929712461 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1906843.2712328578,
            "unit": "ns/iter",
            "extra": "iterations: 365\ncpu: 1906492.0657534257 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 154.26966019999782,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 154.22810120000003 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 9.938553000000486,
            "unit": "ms/iter",
            "extra": "iterations: 74\ncpu: 9.936944459459466 ms\nthreads: 1"
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
          "id": "5337c479a4721bdf184b57e1c8e6feabad31a775",
          "message": "perf(cli): keep freed allocator memory in the process (#57)\n\n## Summary\n\n**What changes:** on Linux, `antb1` restarts itself once at start-up\nwith `MIMALLOC_PURGE_DELAY=-1`. mimalloc, Arrow's default memory pool,\nthen keeps the memory a query frees instead of returning it to the\nsystem and faulting it back in. At 128 threads those page faults contend\nin the kernel.\n\n**Why a restart:**\n- mimalloc reads its options when `libarrow` loads, before `main`.\n- Arrow does not export `mi_option_set`.\n- An executable's `.preinit_array` runs early enough, but glibc resets\nthe environment after it.\n\nAll three were verified with `MIMALLOC_VERBOSE=1` (ADR 0017).\n\n**The restart** (`src/cli/allocator.{h,cc}`, first thing in `main`):\n- `execve(\"/proc/self/exe\", argv, environ + MIMALLOC_PURGE_DELAY=-1)`\nkeeps the same pid, arguments and open files, stdin included.\n- Only the new process gets the setting. There is no `setenv`, so a\nfailed exec leaves the running process unchanged.\n\nIt happens only when all of these hold:\n- the process was started as `antb1` itself: `/proc/self/exe` and\n`argv[0]` have the same file name. Run through the dynamic loader or an\nemulator they don't, and it runs without the setting.\n- `MIMALLOC_PURGE_DELAY` is unset. A value the user sets wins, and the\nrestarted process has it, so it never restarts twice.\n- Arrow's default pool is mimalloc: `ARROW_DEFAULT_MEMORY_POOL` is unset\nor `mimalloc`.\n\n**Costs:**\n- The CLI starts once more, about 26 ms. `antb1 bench` times queries\ninside the process, so its numbers don't include it.\n- The process keeps its peak resident memory until it exits.\n`--memory-limit` is unchanged, because the budget counts Arrow\nallocations.\n\n**Alternatives measured and rejected** (full data, 128 threads, the same\nbinary, all 43 queries):\n\n| | total |\n| --- | ---: |\n| mimalloc, today | 25.9-26.1 s |\n| **mimalloc, `MIMALLOC_PURGE_DELAY=-1`** | **24.6-24.7 s** |\n| gperftools tcmalloc 2.18.1, 8 KiB pages | 26.9 s |\n| tcmalloc, 256 KiB pages | 32.1 s |\n| tcmalloc, 256 KiB pages, 4 GiB thread cache | 25.2 s |\n| Arrow jemalloc / system pools | up to 2.7× slower on single queries;\n20-100 s of kernel CPU per query |\n\nThe 256 KiB tcmalloc wins the high-cardinality GROUP BYs (Q32 2.12 s)\nbut loses 15-18% on the scan-heavy queries (Q20-Q22, Q27).\n\n## Performance: full data, 128 threads\n\n**Paired A/B against main:** a process per query, each binary from 3\npath lengths, best of 3 tries per path, median of the 3.\n\n| | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| **Total (43 queries)** | **25.60** | **24.66** | **1.038×** |\n| Q32 | 2.648 | 2.437 | 1.09× |\n| Q13 | 0.642 | 0.607 | 1.06× |\n| Q16 | 1.041 | 0.978 | 1.06× |\n| Q18 | 1.915 | 1.819 | 1.05× |\n| Q21 | 0.738 | 0.706 | 1.05× |\n| Q23 | 0.861 | 0.818 | 1.05× |\n| Q34 | 2.108 | 2.010 | 1.05× |\n\nNo query is more than 2% slower (Q30: 0.565 → 0.577).\n\n**In one process over all 43 queries** (the setting through the\nenvironment on the same binary, 3 runs), the gain is 5.4%: 25.98 → 24.57\ns. Kernel CPU per run halves, from about 480 s to 220-245 s. Q36 gains\n22% there, and Q32 and Q39 about 14%.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1379; Coverage gate: PASS (cli 94.39% lines, 86.69% branches)\n$ pixi run tsan\n100% tests passed out of 1379\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**Manual checks:**\n- `MIMALLOC_VERBOSE=1 antb1 query ...` shows `purge_delay: 1000` from\nthe first process, then `-1` from the restarted one.\n- `MIMALLOC_PURGE_DELAY=7` is kept.\n- `ARROW_DEFAULT_MEMORY_POOL=jemalloc` runs one `execve`, so there is no\nrestart.\n- `/lib64/ld-linux-x86-64.so.2 antb1 query ...` runs normally, without\nthe restart.\n\n**Tests:**\n- `AllocatorTest`: the restart decision (unset, set, empty, other pools)\nand `IsSameProgram` (PATH, relative and absolute paths, the loader, an\nemulator, a renamed link).\n- The 63 CLI golden tests run through the restart in the dev, ci, ci-gcc\nand coverage presets, including the stdin test. The sanitizer presets\nuse the system pool, so they don't restart.\n- The random DuckDB differential run was not repeated: the engine is\nunchanged.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Claude Code:\n- profiled the scan-bound queries, including thread scaling, per-part\ntimelines and kernel time;\n- measured the allocator alternatives, including a source build of\ngperftools with 256 KiB pages, kept outside the repo;\n- wrote the change, its tests and ADR 0017, and ran the gates and the\nA/B.\n\nA read-only reviewer agent found no P0 or P1 issues. From its notes,\nthis PR adds the dynamic-loader guard and corrects the ADR's claim about\nthe sanitizer presets. The switch from `setenv` to `execve` came from\nclang-tidy's thread-safety check.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T20:40:52+03:00",
          "tree_id": "b04c2c9a6c73972a01cf6c7b16a99fdbb5dcf17c",
          "url": "https://github.com/ydb-campus/antb1/commit/5337c479a4721bdf184b57e1c8e6feabad31a775"
        },
        "date": 1790790204701,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4134.441027678857,
            "unit": "ns/iter",
            "extra": "iterations: 168613\ncpu: 4134.277315509479 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84632.12659863147,
            "unit": "ns/iter",
            "extra": "iterations: 7741\ncpu: 84626.97093398788 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 221512.5393862681,
            "unit": "ns/iter",
            "extra": "iterations: 3161\ncpu: 221451.21037646313 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 441199.36988028634,
            "unit": "ns/iter",
            "extra": "iterations: 1587\ncpu: 441104.66036546906 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 347607.5879721723,
            "unit": "ns/iter",
            "extra": "iterations: 2012\ncpu: 347546.2072564614 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2117688.8398792,
            "unit": "ns/iter",
            "extra": "iterations: 331\ncpu: 2117523.141993957 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 178.15031125000047,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 178.11589124999983 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.487045265306213,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.486045408163264 ms\nthreads: 1"
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
          "id": "2fe31ba9cd76d46150729fe4e773fb04aa3d167b",
          "message": "perf(plan): group by the keys that determine the others (#59)\n\n## Summary\n\n**What changes:** a new optimizer rule drops a GROUP BY key that is\ncomputed only from other keys. It computes that key once per group in a\n`Compute` above the `GroupAggregate`, with a `Project` restoring the\noutput columns (ADR 0018). `GROUP BY x, x + 1, x * 2` groups by `x`\nalone: the same groups, fewer keys to hash.\n\n**Where it applies:** a key is dropped when all of these hold:\n- it is an expression of the `Compute` right below the `GroupAggregate`\nand reads at least one column;\n- every column it reads is a key passed through that `Compute`;\n- none of those keys is DOUBLE. A DOUBLE key groups -0.0 with 0.0 and\nevery NaN together, and a function of it (`1 / d`) could tell them\napart.\n\n**Exactness:**\n- **Groups:** a key that is a function of other keys cannot split or\nmerge groups.\n- **Values:** computed from the group's first-seen values of the kept\nkeys. Those aren't DOUBLE, so every row of the group has them.\n- **NULL:** a NULL key gives a NULL expression, as before.\n- **Errors:** the expressions see the same set of key values, so an\noverflow fails the query exactly when it did before.\n\n**Where it doesn't apply:** under a `LIMIT` with no `ORDER BY` in\nbetween. The nodes above would stop reading after the rows they need, so\nthe dropped keys would be computed for some groups only. An overflow in\nanother group would then no longer fail the query, while DuckDB fails\nit. A `Sort` reads every group, so `ORDER BY ... LIMIT` keeps the\nrewrite. A second reviewer pass confirmed no other operator above a\n`GroupAggregate` stops reading early.\n\n**Why:** Q35 (four keys, one column and arithmetic of it, about 9.8 M\ngroups) took 0.85 s, against 0.23 s in DuckDB and 0.12 s in ClickHouse,\nwhich drops such keys too. `explain --analyze` put about 48 of its 59.5\ns of part CPU into hashing and aggregating the keys.\n\n**Follow-up:** recomputing the dropped keys over 9.8 M groups is a\nserial `Compute`, about 0.3 s of Q35. A parallel `Compute` over\nmaterialized input should remove most of it, taking Q35 toward 0.4 s.\nThat will be a separate executor PR. Moving the recomputation above the\nSort/Limit is not an option, for the error reason above.\n\n## Performance: full data, 128 threads\n\n**Paired A/B against main (#57):** a process per query, each binary from\n3 path lengths, best of 3 tries per path, median of the 3.\n\n| | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| **Q35** | 0.822 / 0.862 | 0.680 / 0.687 | **1.21× / 1.25×** (all-query\nrun / quiet rerun) |\n| **Total (43 queries)** | **24.69** | **24.47** | **1.009×** |\n\nOther queries are unchanged, since none has such keys. Q15 and Q25 moved\n±5-8% while the host load rose to about 50. Neither has a key the rule\napplies to, and a quiet rerun put both within 3% (Q15 0.503 vs 0.494,\nQ25 0.151 vs 0.146).\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1386; Coverage gate: PASS (plan 95.70% lines, 90.40% branches)\n$ pixi run tsan\n100% tests passed out of 1386\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2349312846 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**Tests:**\n- **`OptimizerTest`:**\n- The rewrite's EXPLAIN shape: integer keys, ORDER BY ... LIMIT with the\nLimit still right above the Sort, a VARCHAR key with HAVING, and a\ndetermining key that isn't the first key.\n- The cases it leaves alone: a DOUBLE key, a non-key column, plain\ncolumns, an expression of a column that isn't a key, and a LIMIT without\nORDER BY, with and without HAVING and OFFSET.\n  - Idempotence.\n- **`tests/slt/cases/groupby/dependent_keys.slt`**, expectations from\nDuckDB (`pixi run slt-complete`):\n  - NULL keys, VARCHAR with `strlen`, several dependent keys;\n- ORDER BY and HAVING on a dependent key, a select list with only the\ndependent key;\n- COUNT(DISTINCT) (two-level aggregation), parallel parts, a DOUBLE key;\n- overflow errors: without LIMIT, under a plain LIMIT, and under ORDER\nBY ... LIMIT.\n- **The plain-LIMIT overflow test fails without the fix:** the first\ncommit's binary returned a row with exit 0.\n- **The EXPLAIN golden** `cli.explain_dependent_keys`.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Claude Code:\n  - profiled Q35, Q36 and Q39 with `explain --analyze`;\n  - measured the potential by grouping by the single column locally;\n- planned the rule (plan approved), wrote it with its tests and ADR\n0018, and ran the gates and the A/B.\n\nA read-only reviewer agent found a P0: the plain-LIMIT error case, now\nfixed with a test that fails without the fix. It also found a missing\nrule entry in docs/sql-subset.md, now added. A second review of the fix\nfound no P0 or P1.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-09-30T23:07:36+03:00",
          "tree_id": "b8ec70acc06aa0d5526c600da9a0471c93e0f8f1",
          "url": "https://github.com/ydb-campus/antb1/commit/2fe31ba9cd76d46150729fe4e773fb04aa3d167b"
        },
        "date": 1790799014793,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4121.672210187918,
            "unit": "ns/iter",
            "extra": "iterations: 171732\ncpu: 4121.295763165863 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85059.99743589654,
            "unit": "ns/iter",
            "extra": "iterations: 7800\ncpu: 85052.59423076926 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 221789.76228209372,
            "unit": "ns/iter",
            "extra": "iterations: 3155\ncpu: 221730.0782884311 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 441004.99937146663,
            "unit": "ns/iter",
            "extra": "iterations: 1591\ncpu: 440975.28095537407 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 348956.7427860731,
            "unit": "ns/iter",
            "extra": "iterations: 2010\ncpu: 348903.4970149253 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2139795.2286584834,
            "unit": "ns/iter",
            "extra": "iterations: 328\ncpu: 2139540.8963414636 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 198.56583266666425,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 198.55021000000022 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.449292687499948,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.446731125000001 ms\nthreads: 1"
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
          "id": "cc887a06a76f063da07f63a196fcc347887db011",
          "message": "perf(exec): compute batches in parallel over a whole input (#60)\n\n## Summary\n\n**What changes:** a `Compute` over a whole input (an aggregation or a\nsort) now computes its batches in parallel on the session's pool. It\nuses `ParallelComputeOperator` (private,\n`src/exec/parallel_compute.{h,cc}`), which the physical planner picks\nfor a Compute outside a part pipeline. Inside part pipelines the parts\nare already parallel, and `ComputeOperator` stays.\n\n**Why:** since #59, Q35 groups by one key and computes the three dropped\nkeys once per group. That is a Compute over 9.8 M groups, which ran\nserially and took about 0.3 s of the query's 0.68 s.\n\n**Semantics are the serial Compute's:**\n- **Order:** batches come back strictly in input order, so the output is\nidentical.\n- **Errors:** the first error in batch order is reported, after the\nbatches before it.\n- **An input failure** is held and returned after the batches read\nbefore it, including their errors.\n- **A batch read ahead but never returned** (a Limit above stopped)\nnever reports its error, just as the serial Compute never computes it.\n\n**Memory, following the part scheduler (#44):**\n- The window starts at `threads` batches. Every batch taken under\npressure halves it, and every other one widens it by one. Under\npressure, no batch starts while another is in flight.\n- A batch that runs out of memory on a worker is computed again alone on\nthe consumer thread. The batches in flight are dropped and computed\nagain when reached. Only a failure alone fails the query.\n- A task's `std::bad_alloc` becomes `OutOfMemory`, as in every other\nexecutor task.\n- Results go into slots that the consumer thread takes or clears, so no\nbuffer is freed on a worker after the consumer has moved on.\n\n`ComputeOperator` now shares `ComputedSchema` and `ComputeBatch` with\nthe new operator. ADR 0013's decision text (\"everything above the sink\nstays serial\") and docs/architecture.md (step 7 and the operator table)\nare amended.\n\n## Performance: full data, 128 threads\n\n**Paired A/B against main (#59):** a process per query, each binary from\n3 path lengths, best of 3 tries per path, median of the 3.\n\n| | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| **Q35** | 0.681 | 0.381 | **1.79×** |\n| **Total (43 queries)** | **24.36** | **24.20** | 1.007× |\n\nNo other query moves by more than 4%; none of them has a large Compute\nabove an aggregation.\n\n**Q35 across the two PRs:** 0.85 s → 0.38 s. DuckDB takes 0.23 s and\nClickHouse 0.12 s.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1395; Coverage gate: PASS (exec 96.63% lines, 86.22% branches; parallel_compute.cc 95.92% lines)\n$ pixi run tsan\n100% tests passed out of 1395\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2196248912 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\nThe diff-random and test-data runs were on 044fac9. Only tidy fixes\nfollowed: Submit returns its future, one const, one pair of parentheses.\ncheck-full and tsan ran on the final commit.\n\n**Tests: `src/exec/tests/parallel_compute_test.cc`**\n- **Same batches in order,** without and with an executor.\n- **Errors:** the first failing batch in order, with later failing\nbatches never returned; and the input's failure after the batches before\nit and their errors.\n- **Early close with batches in flight,** then Open again.\n- **Memory:**\n- under pressure one batch at a time (never more than one read ahead),\nwith the same output;\n  - a tiny budget fails with OutOfMemory, and every byte comes back;\n- **deterministic retry:** a pool that fails every allocation off the\nconsumer thread makes every batch run out of memory on a worker, also at\nwindow 1 with nothing in flight. Each batch is computed again alone, the\noutput is the serial one, and the budget ends at 0. A retry that still\nfails, fails the query.\n- **The operator choice** (`physical_planner_test`):\nParallelComputeOperator over a sort, ComputeOperator over a scan (a part\npipeline).\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Claude Code wrote the operator, its tests and the doc\namendments, and ran the gates and the A/B. Two rounds of a read-only\nreviewer agent shaped it:\n  - **First round:**\n    - a P0: `bad_alloc` on a worker would abort the process;\n    - results freed on workers;\n    - no out-of-memory retry;\n    - missing memory and planner tests;\n    - the ADR text.\n- **Second round:** the retry condition checked the state at take time\ninstead of at run time, and the retry test was not deterministic.\n\n  All are fixed as described above.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T00:51:45+03:00",
          "tree_id": "fd5751b0771b0a7686174f78efbab12f492ffd00",
          "url": "https://github.com/ydb-campus/antb1/commit/cc887a06a76f063da07f63a196fcc347887db011"
        },
        "date": 1790805267412,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4067.2870860505986,
            "unit": "ns/iter",
            "extra": "iterations: 172666\ncpu: 4066.950928381963 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 85529.3693768064,
            "unit": "ns/iter",
            "extra": "iterations: 7269\ncpu: 85519.47833264549 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222150.2209523795,
            "unit": "ns/iter",
            "extra": "iterations: 3150\ncpu: 222139.05587301584 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 438548.73918496043,
            "unit": "ns/iter",
            "extra": "iterations: 1595\ncpu: 438511.237617555 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 350060.93167083425,
            "unit": "ns/iter",
            "extra": "iterations: 2005\ncpu: 349970.85586034873 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2131198.8553845994,
            "unit": "ns/iter",
            "extra": "iterations: 325\ncpu: 2130062.3999999994 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 204.18013033332727,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 204.11885899999996 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.385626693878322,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.384285897959186 ms\nthreads: 1"
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
          "id": "35247e98f430ec081d3de9fefdb247b3feee2e77",
          "message": "perf(exec): top-N per partition of a grouped aggregation (#61)\n\n## Summary\n\n**What changes:** for `ORDER BY ... LIMIT` directly over a partitioned\n`GROUP BY` (`Limit` over `Sort` over `GroupAggregate`), each of the\naggregation's 64 partitions keeps only its first `limit + offset` rows\nin the top-N's order, in parallel, as it builds its rows. The top-N\nabove (`SortOperator`, unchanged) then reads at most 64 × `limit +\noffset` rows instead of every group.\n\n**Why:**\n- A scaling measurement (antb1 vs DuckDB at 1, 16 and 128 threads)\nshowed Q32 only 1.35× slower than DuckDB on one thread but 3.2× on 128.\nantb1 kept about 33 threads busy on average, DuckDB 73.\n- `explain --analyze` showed why: a serial top-N over all 100 M groups\n(0.87 s), fed by building 100 M output rows to keep 10.\n\n**Same result, ties included:**\n- A row in the top `k` of all groups, stably sorted with partitions in\norder, has fewer than `k` rows before it in its own partition, so its\npartition keeps it.\n- Each partition's kept rows are stably sorted, and the partitions still\ncome in order. So the stable top-N above returns the same rows in the\nsame order.\n- `LIMIT 0` is excluded, and `keep` saturates at INT64_MAX. A partition\nwith no more than `keep` rows passes them unchanged, since the stable\nsort above gives the same order.\n\n**Where it applies:**\n- **Only `PartGroupAggregateOperator`.** The two-level COUNT(DISTINCT)\naggregation and the serial `GroupAggregateOperator` are unchanged, as\nare plans with HAVING, a Compute or a Project between the Sort and the\naggregation.\n- **Planner:** it passes a `PartitionTopN` (sort keys, keep) through a\nnew `Build` parameter.\n- **The per-partition step** is `KeepFirstRows`: a `SortBuffer` with\nthat keep, its containers reserved on the budget as `PartTopNOperator`\ndoes.\n\n`explain --analyze` shows `top-N per partition keep=N` on the\naggregation. ADR 0011 and docs/architecture.md are amended.\n\n## Performance: full data, 128 threads\n\n**Paired A/B against main (#60):** a process per query, each binary from\n3 path lengths, best of 3 tries per path, median of the 3.\n\n| | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| **Q32** | 2.439 | 1.611 | **1.51×** |\n| Q15 | 0.492 | 0.353 | 1.39× |\n| Q18 | 1.823 | 1.333 | 1.37× |\n| Q16 | 0.966 | 0.768 | 1.26× |\n| Q31 | 0.765 | 0.652 | 1.17× |\n| Q12, Q14, Q30, Q33, Q34 | | | 1.08-1.09× |\n| **Total (43 queries)** | **24.23** | **21.90** | **1.106×** |\n\nNo query is slower by more than 4%.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1400; Coverage gate: PASS (exec 96.63% lines, 86.28% branches)\n$ pixi run tsan\n100% tests passed out of 1400\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3914648504 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**Tests:**\n- **`PartOperatorsTest.GroupedTopNIsTheTopNOfAllGroups`:** the planned\ntop-N equals rows `[offset, offset + limit)` of the same GROUP BY sorted\nwithout a limit, on 1 and 4 threads.\n- **Data:** 1500 groups in 12 parts, so about 23 per partition, with a\nNULL group.\n- **Orders:** COUNT ascending (every group but NULL ties, so the first\nrows all come from the first partition), COUNT descending, two keys, the\ngroup key ASC NULLS FIRST, and DESC.\n  - **Limit/offset pairs:** up to an offset past all groups.\n- **Mutation-checked:** keeping one row fewer per partition fails it. My\nfirst version of the test passed that mutation, and the data was changed\nuntil it failed.\n- **`PartOperatorsTest.KeepFirstRowsIsAStableTopOfThePartition`:** the\nstable order of ties, the pass-through of small partitions, and out of\nmemory at three budgets, each failing inside the helper with nothing\nleft allocated.\n- **`tests/slt/cases/orderby/group_top_n.slt`**, expectations from\nDuckDB: GROUP BY ... ORDER BY ... LIMIT/OFFSET on parallel parts, with\nNULLs, two keys, and an offset.\n- **Updated expectations:** `ProfileTest` (the aggregation's detail) and\nthe `cli.explain_analyze` regex (the aggregation now emits 64 × keep\nrows).\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Claude Code:\n  - measured the scaling against DuckDB and profiled Q32;\n  - wrote the change, its tests and the doc amendments;\n  - ran the gates and the A/B.\n\nA read-only reviewer agent confirmed the correctness argument. It found\nthree issues, all fixed:\n- a test whose out-of-memory check never reached the new code, now a\ndirect test of `KeepFirstRows`;\n  - unreserved sort-buffer containers;\n  - a needless sort of partitions with at most `keep` rows.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T02:29:59+03:00",
          "tree_id": "758e38f0161d2de339f0889343b34fcb3c41932e",
          "url": "https://github.com/ydb-campus/antb1/commit/35247e98f430ec081d3de9fefdb247b3feee2e77"
        },
        "date": 1790811147840,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3986.8437645008394,
            "unit": "ns/iter",
            "extra": "iterations: 176714\ncpu: 3985.707340674763 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94524.99986002284,
            "unit": "ns/iter",
            "extra": "iterations: 7144\ncpu: 94487.68868980961 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 124654.69642539576,
            "unit": "ns/iter",
            "extra": "iterations: 5623\ncpu: 124574.10368130899 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 486637.095006931,
            "unit": "ns/iter",
            "extra": "iterations: 1442\ncpu: 486332.8016643549 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 458318.69759895554,
            "unit": "ns/iter",
            "extra": "iterations: 1541\ncpu: 458246.19013627485 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2242000.9807073805,
            "unit": "ns/iter",
            "extra": "iterations: 311\ncpu: 2241725.286173634 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 201.79792499999868,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 201.7700523333333 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 15.39998104347849,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 15.397688000000015 ms\nthreads: 1"
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
          "id": "ad652b7e6abdf268b84789fb1dfdbbafb5548baf",
          "message": "perf(exec): route parts that reduce less than 4 to 1 (#62)\n\n## Summary\n\n**What changes:** two constants of the #55 rule that sends a part's rows\nstraight to the partitions (\"routing\") when its own hash table does not\nreduce them.\n\n- **Threshold:** a part routes when its groups are more than **1/4** of\nits rows (was 3/4).\n- **Decision point:** a part decides after **64Ki** rows have reached\nits table (was 4096). Without a filter, nothing changes here: the\ndecision already came after the first batch, since the point is capped\nat the batch size. Under a filter that leaves small batches, the part\nnow waits for 64Ki kept rows instead of deciding on its first few\nthousand.\n\n**Why:** a single-thread `explain --analyze` of Q16 and Q33 (2× slower\nthan DuckDB even on one thread) shows the merge into the partitions\ncosting about as much as the parts' own aggregation:\n\n| one thread | scan | parts' aggregation | merge into partitions | build\n|\n| --- | ---: | ---: | ---: | ---: |\n| Q16 (24 M groups) | 4.5 s | ~9.4 s | ~8.0 s | 0.8 s |\n| Q33 (18 M groups) | 11.2 s | ~13.5 s | ~12.0 s | 1.0 s |\n\n**The cost argument:**\n- **Double insertion:** every group is inserted twice, in its part's\ntable and then in its partition's. A routed part inserts each row once.\n- **Who routed:** under the 3/4 rule only 2-3 of 325 parts routed.\n- **Where it pays:** a sweep on the same binary showed routing pays from\nabout 1/4 on. Routing every part is worse, because Q15 and Q35 still\ngain from local aggregation.\n- **Why wait for 64Ki rows:** with a filter, the first few thousand rows\nlook more distinct than the part. The 1/4 threshold with the old point\nmade Q30 11% slower, and the new point turns that into a gain.\n\nResults are unchanged: routing is exact (#55), its DOUBLE SUM/AVG and\nHUGEINT exclusions are unchanged, and the decision depends only on the\npart's data and the batch size.\n\n## Performance: full data, 128 threads\n\n**Paired A/B against main (#61):** a process per query, each binary from\n3 path lengths, best of 3 tries per path, median of the 3.\n\n| | main | this PR | speedup |\n| --- | ---: | ---: | ---: |\n| Q36 | 0.215 | 0.163 | 1.32× |\n| Q34 | 1.960 | 1.571 | 1.25× |\n| Q33 | 1.845 | 1.484 | 1.24× |\n| Q39 | 0.452 | 0.370 | 1.22× |\n| Q18 | 1.333 | 1.174 | 1.13× |\n| Q16, Q17, Q30, Q12, Q38 | | | 1.04-1.07× |\n| **Total (43 queries)** | **22.16** | **20.93** | **1.059×** |\n\nNo query is more than 4% slower.\n\n**Threshold and decision-point sweep** (10 GROUP BY queries, totals):\n3/4 after 4096 rows 9.25 s; 1/4 after 4096 8.36 s (Q30 +11%); 1/2 after\n64Ki 8.88 s; **1/4 after 64Ki 8.26 s**.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # on 82213c1 (the code change); later commits change tests and docs only\ncheck-full exit 0; 100% tests passed out of 1400; Coverage gate: PASS (exec 96.63% lines, 86.31% branches)\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=2633072822 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n$ pixi run check && pixi run tidy && pixi run tsan   # on the test commit 1a82e04\nlint: PASS; 100% tests passed out of 1401; tidy clean; tsan: 100% tests passed out of 1401\n$ pixi run ci-gcc                                    # on the final commit 3fbb153\n100% tests passed out of 1401\n```\n\nThe first CI run failed the GCC leg: a test constant `kRows` shadowed\nthe fixture's member (GCC's `-Wshadow`; Clang does not warn). It is\nrenamed in 3fbb153.\n\n```text\n```\n\n**Tests:**\n- **`RoutedRowsGiveTheSameGroups`** now runs in batches of 32 rows (it\nwas 8), because 5 values in 8 rows exceed 1/4. It adds two cases on\neither side of the new threshold, so going back to 3/4 fails it:\n  - v (11 values in 32 rows) routes;\n  - s (7 values) does not.\n- **`RoutingDecidesAfterSixtyFourKiRowsReachTheTable`:** batches of\n128Ki rows, of which a filter keeps 1 in 16. A key distinct within the\nfirst batch's kept rows but repeating every 8192 of them does not route,\nwhile a 40000-value key does. Going back to the 4096-row point fails it.\n\nBoth mutations were checked.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Claude Code:\n- measured antb1 against DuckDB at 1, 16 and 128 threads, and profiled\nQ16 and Q33 on one thread;\n  - swept the threshold and decision point;\n- wrote the change, its tests and the ADR 0013 amendment, and ran the\ngates and the A/B.\n\nA read-only reviewer agent found that the updated test pinned neither\nconstant. The two new tests above do, both mutation-checked. It also\nfound inexact doc wording about the decision point, now fixed.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T08:43:01+03:00",
          "tree_id": "a77673403804584ac2e4c1e87fe7621ab588f5fc",
          "url": "https://github.com/ydb-campus/antb1/commit/ad652b7e6abdf268b84789fb1dfdbbafb5548baf"
        },
        "date": 1790833520510,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3981.043458748527,
            "unit": "ns/iter",
            "extra": "iterations: 176029\ncpu: 3980.185338779406 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94563.73751281117,
            "unit": "ns/iter",
            "extra": "iterations: 6827\ncpu: 94523.18793027684 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 124527.11981484415,
            "unit": "ns/iter",
            "extra": "iterations: 5617\ncpu: 124523.71212390954 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 484516.1641273964,
            "unit": "ns/iter",
            "extra": "iterations: 1444\ncpu: 484379.48130193906 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 456349.8907672436,
            "unit": "ns/iter",
            "extra": "iterations: 1538\ncpu: 456199.1495448638 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2240934.217252361,
            "unit": "ns/iter",
            "extra": "iterations: 313\ncpu: 2240513.3801916926 ns\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 198.47381366666164,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 198.43977900000007 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 15.15085147826149,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 15.148832239130439 ms\nthreads: 1"
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
          "id": "97d2e2dc70b3f83f2c5e5d83521c5b8d3876a5a1",
          "message": "feat(io,plan): scans that apply a row filter on decoded values (#64)\n\n## Summary\n\nThis is the io side of filter pushdown into the scan (ADR 0020,\n`docs/adr/0020-filter-pushdown.md`).\n\n**What it adds:**\n- **An interface:** a scan can apply a row filter while it decodes, and\ncopy only the rows that pass.\n- **The Parquet implementation** of that interface.\n- **No planner change:** nothing in the engine uses it yet, so query\nresults and speed do not change. The executor side comes in the next PR.\n\n**Why:** the string scans (Q20-Q22) are bound by memory traffic.\n- Arrow's high-level Parquet reader copies every string into a\n`BinaryArray`, which the filter then reads again.\n- Decoding through Parquet's column readers instead gives\n`parquet::ByteArray` views into the decompressed pages.\n- Matching on those views was 1.31-1.88× faster on one thread and\n**1.59-2.18× on 128 threads** for URL, Title and SearchPhrase (a local\nexperiment on the full ClickBench data). Decompression is the same\neither way.\n- This is the third granularity of pushing WHERE predicates down: row\ngroups by footer statistics exist (#50); pages by statistics are later;\nrows on decoded values are this.\n\n**`plan` (`table.h`):** the interface lives here, so `exec` implements\nthe predicates and `io` applies them, with no new module edge and\nnothing moved.\n- **`ScanValues`:** a column's decoded values. Fixed-width columns are\nan Arrow array; VARCHAR columns are a `std::string_view` per row plus\nvalidity, valid during the call only.\n- **`ScanFilter`:** `columns()`, and `Apply(column, values, offset,\nselected)`, which clears the bits of the rows that fail that column's\npredicates.\n- **One column at a time:** pushed predicates each read one column and\nare ANDed.\n- **Piece by piece:** a string view is valid only until its column\nreader reads its next page, and the pages of different columns do not\nline up.\n- **`Table::supports_scan_filter(fields)`** (false by default), and a\n`ScanPart` overload with a filter that returns only the rows that pass,\nin part order.\n\n**`io`:**\n- **`src/io/filtered_scan.{h,cc}`:** `ParquetFileReader` →\n`RowGroupReader` → `TypedColumnReader`, for flat INT16, INT32,\nUSMALLINT, DATE (including the USMALLINT/INTEGER overrides), BIGINT,\nDOUBLE (also stored as FLOAT) and VARCHAR columns. Per batch:\n- **the filter's columns first:** fixed-width whole; strings piece by\npiece, keeping the bytes of the rows still selected in the query's pool;\n  - **then every other column:** only the rows that pass are copied;\n  - **a batch with no passing row** is skipped.\n- **Errors:** as the high-level path reports them: an IOError naming the\nfile, OutOfMemory, the footer checks. A filter's own errors pass\nthrough.\n- **`ParquetTable`:** `supports_scan_filter` is true only when every\nscanned column is supported, and `DoScanPartFiltered` dispatches to the\nnew reader. The segment and footer checks move to\n`src/io/scan_segment.h`, shared by both paths.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc (final commit 1dddcc4)\ncheck-full exit 0; 100% tests passed out of 1415; Coverage gate: PASS (io 95.37% lines, 88.43% branches; plan 95.71%, 90.37%)\n$ pixi run tsan                                 # 13c8fc6; only test lint fixes followed\n100% tests passed out of 1415\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random   # e63e8c5; no engine behavior changes in this PR\nDIFF: PASS seed=3029943232 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n$ pixi run bench --benchmark_filter=StringFilter    # one thread, 1 Mi URL-like strings\nBM_StringFilterAfterScan 141 ms   BM_StringFilterInScan 107 ms   (1.32×, the same 20972 matches)\n```\n\n**Tests (`src/io/tests/parquet_filter_test.cc`):**\n- **Parity:** a filtered `ScanPart` must equal the unfiltered scan's\nrows that pass, byte for byte. It covers:\n- every supported type, required and nullable columns, and the DATE\noverrides;\n- dictionary, plain and dictionary-fallback encodings, 1 KiB pages, and\nbatch sizes of 1, 7, 1000 and 64Ki;\n- filters that keep none, some or all rows, on one or two columns, with\nNULLs;\n  - another projection order, and parts of two files.\n- **Errors and limits:**\n- out of memory at every point of a scan, by sweeping a capped pool: an\nOutOfMemory status, every byte given back, and the right rows once\nmemory suffices;\n  - filter errors on string and fixed-width columns;\n- a corrupt data page, an IOError naming the file, from a filter column\nand from another;\n  - a file changed after Open;\n  - unsupported columns (BOOLEAN, TIMESTAMP, repeated, nested);\n- `FilteredColumnOf`'s type and physical-type checks, and invalid\nrequests.\n- **Mutation-checked:** skipping the filter on strings, and ignoring the\nselection, both fail.\n- **`plan` (`TableTest.FilteredScansAreOptIn`):** a scan without a\nfilter forwards to the plain scan; filtering is NotImplemented unless a\ntable opts in.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Claude Code:\n- measured the string scans (hardware counters, the BINARY_VIEW probe,\nand the low-level-reader experiment);\n- planned this in plan mode (approved), and wrote the interface, the\nreader, the tests, a micro benchmark and ADR 0020;\n  - ran the gates.\n\nA read-only reviewer agent checked conversions, levels and Skip\nsemantics, view lifetimes, alignment and error mapping, and found no\ncorrectness issue. It found five test gaps, all added: required columns,\nUSMALLINT dates, corrupt pages, several files and `plan`'s defaults. Its\nnotes are done too: kept strings in the query's pool, the readers'\ndestruction order, and ADR wording. CI-style gates then added\nout-of-memory and readable-leaf tests to keep io's branch coverage above\nits floor.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T15:53:10+03:00",
          "tree_id": "eeab98724042d843ae67bedd41b3ea01331eda8d",
          "url": "https://github.com/ydb-campus/antb1/commit/97d2e2dc70b3f83f2c5e5d83521c5b8d3876a5a1"
        },
        "date": 1790859355633,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4168.470131857765,
            "unit": "ns/iter",
            "extra": "iterations: 169880\ncpu: 4168.030568636685 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 84374.05381579012,
            "unit": "ns/iter",
            "extra": "iterations: 7600\ncpu: 84367.60671052632 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 222639.48744836758,
            "unit": "ns/iter",
            "extra": "iterations: 3147\ncpu: 222624.48331744506 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 444453.8827629802,
            "unit": "ns/iter",
            "extra": "iterations: 1578\ncpu: 444432.52091254736 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 402634.06563039235,
            "unit": "ns/iter",
            "extra": "iterations: 1737\ncpu: 402481.86010362714 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2100556.536363697,
            "unit": "ns/iter",
            "extra": "iterations: 330\ncpu: 2099998.9424242424 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 51.48396692307791,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 51.475985 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 34.6910423500006,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.681732899999986 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 212.68901566666423,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 212.67271666666684 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.397367510203864,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.3970258367347 ms\nthreads: 1"
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
          "id": "1e38331ecba14c29a5376dd7b1fd84dbc0b832d6",
          "message": "perf(exec): push WHERE predicates on scan columns into the scan (#65)\n\n## Summary\n\nThis is the executor side of filter pushdown into the scan (ADR 0020,\n`docs/adr/0020-filter-pushdown.md`; the io side is #64). The WHERE\npredicates on scan columns are now applied while the scan decodes:\nstrings are matched on views into the decoded pages, and only the rows\nthat pass are copied.\n\n**Result:** the 43 ClickBench queries went from 21.09 s to 19.50 s at\n128 threads (−7.5%). Q20-Q22 (LIKE over long strings) are 1.57-1.75×\nfaster.\n\n**`exec` (`src/exec/scan_filter.{h,cc}`, new):**\n- **`PushableToScan`:** a predicate is pushed when it reads one column\nagainst literals: `<op>`, `[NOT] IN`, `[NOT] LIKE`, `IS NOT NULL`. These\nstay in the `Filter`:\n  - comparisons of two columns;\n  - `IS TRUE` of a computed condition;\n- a folded `FALSE` (the Filter ends the stream without reading\nanything).\n- **`MakeScanFilter`** implements `plan::ScanFilter`, with the same rows\nas `FilterOperator`:\n- **fixed-width columns:** the Filter's own `PredicateEvaluator` on the\none-column array, combined into the selection with bitmap ANDs (NULL\nfails);\n- **VARCHAR views:** `LikePattern::Matches`, bytewise `<op>` (unsigned,\nas Arrow's kernels), IN / NOT IN by binary search, and validity for\nNULL.\n- **`TableScanOperator`** takes the pushed predicates and builds the\nfilter at `Open`, so its kernels allocate from the query's pool.\n- **Physical planner:** a `Filter` directly over the scan of a part\npipeline pushes what it can when the table `supports_scan_filter` the\nscanned fields, and keeps the rest.\n- Not pushed: late materialization's narrow scans (ADR 0016), whose row\nids count the part's rows.\n  - `KeptParts` (row groups by statistics) is unchanged and runs first.\n- **`explain --analyze`:** the `Scan` line ends with `, N pushed\npredicates`, and its `rows` are those that passed.\n\n**`io` (`src/io/filtered_scan.cc`), speedups to #64's read path:** a\nfirst A/B showed a constant ~20 ms regression on queries that read few\nrow groups (Q1, Q7, Q19, Q40-Q42). Profiling put it in the fixed-width\npath, which now:\n- decodes straight into a pool buffer, which becomes the array when no\nrow is NULL (no second copy);\n- converts with one type dispatch per batch, not per row;\n- compacts selected rows by runs (`VisitSetBitRunsVoid`, `CopyBitmap`),\nnot row by row;\n- reserves string builders per piece and appends unchecked.\n\nIt also gives the decode buffers back to the pool at `Close`.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc (final commit f7cad91)\ncheck-full exit 0; 100% tests passed out of 1424; Coverage gate: PASS\n$ pixi run tsan\n100% tests passed out of 1424\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nDIFF: PASS seed=3711224658 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**Speed:** a paired A/B of release builds against main 97d2e2d, all 43\nClickBench queries on the full data at 128 threads (3 binary paths ×\nbest of 3, medians, alternating order):\n- **Total:** 21.09 s → 19.50 s.\n- **Faster by more than 5%:**\n\n  | Queries | Change |\n  | --- | --- |\n  | Q20, Q21, Q22 | −40%, −36%, −43% |\n  | Q27, Q38 | −27%, −24% |\n  | Q1, Q36, Q37, Q39 | −12% to −22% |\n  | Q7, Q19, Q26, Q29 | −6% to −12% |\n\n- **Slower by more than 3%:**\n- Q5: +3.7% in the full run, −0.2% on a re-run. It has no WHERE, so this\nis noise.\n- Q12 (a string `<>` filter feeding a GROUP BY): +3.9% in the full run,\nthen +0.1% and +2.0% on re-runs, at 128 threads only. On 1 thread it is\n16% faster (9.81 s → 8.26 s), and on 16 threads 6% faster. Its profile\nat 128 threads shows more time in the shared memory budget's counters.\n\n**Tests:**\n- **`src/exec/tests/scan_filter_test.cc` (new):** the scan filter must\nkeep exactly the rows the Filter operator keeps, for:\n  - every pushed kind and every comparison operator;\n- BIGINT, INTEGER, DOUBLE (NaN, ±0, ±inf) and VARCHAR (bytes above 0x7f,\nempty strings, with and without NULLs);\n  - LIKE patterns with `%`/`_`, IN with NaN, and conjunctions;\n  - views in pieces of 1, 3 and 64 rows, at selection offsets 0 and 5.\n\nIt also checks malformed predicates and values (errors, nothing cleared)\nand `columns()` order. Mutations of a string comparison, of view NULL\nhandling and of fixed-width NULL handling each fail it.\n- **Planner (`physical_planner_test.cc`):**\n- pushed vs kept predicates, with the same rows with and without\npushdown at batch sizes 1, 3 and 64;\n  - the profile detail;\n  - nothing pushed with only a column comparison;\n  - `FALSE`: nothing pushed or scanned;\n  - late scans are not filtered.\n- **`TableScanOperator`:** predicates without a part, or with a late\nscan, are Invalid.\n- **`MemoryTable`** (exec tests) now applies scan filters as a Parquet\ntable does: VARCHAR as views, 3 rows at a time. So every exec, parallel\nand profile test with a split table runs through pushdown.\n- **`tests/slt/cases/where/pushdown.slt` (new):** pushed and kept\npredicates mixed over parts, NULLs and every column type. Expectations\nfrom DuckDB (`pixi run slt-complete`).\n- **Changed expectations:** the profile test and the two\n`explain_analyze` CLI goldens now show the pushed count and the Scan's\npassing rows.\n\n**Docs:** ADR 0020 (the executor bullet; status unchanged),\n`docs/architecture.md` (\"Filtering while scanning\"),\n`docs/sql-subset.md` (`explain --analyze`).\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none touched)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs, profiled and measured the change, and ran the gates. A\nread-only reviewer agent found no P0/P1 in the code.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T17:46:04+03:00",
          "tree_id": "294c110298aa27d44d96732e829f2d943454bf6f",
          "url": "https://github.com/ydb-campus/antb1/commit/1e38331ecba14c29a5376dd7b1fd84dbc0b832d6"
        },
        "date": 1790866076849,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2275.197033242222,
            "unit": "ns/iter",
            "extra": "iterations: 332619\ncpu: 2275.117007747603 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 65424.40746382325,
            "unit": "ns/iter",
            "extra": "iterations: 10504\ncpu: 65420.595392231546 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 72588.23191686295,
            "unit": "ns/iter",
            "extra": "iterations: 9719\ncpu: 72580.587509003 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 264321.608897481,
            "unit": "ns/iter",
            "extra": "iterations: 2585\ncpu: 264300.7036750484 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 267827.5385474845,
            "unit": "ns/iter",
            "extra": "iterations: 2685\ncpu: 267802.59031657374 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1548903.6638477987,
            "unit": "ns/iter",
            "extra": "iterations: 473\ncpu: 1548741.401691332 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 31.055285363636514,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.047742863636348 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 30.120144916666664,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 30.11560108333333 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 136.74122920000116,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 136.70694579999994 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 8.900860192307702,
            "unit": "ms/iter",
            "extra": "iterations: 78\ncpu: 8.900133666666664 ms\nthreads: 1"
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
          "id": "6e523d5ff721888393170df5d95f8b512c3adcd3",
          "message": "perf(exec,io): push WHERE predicates into late-materialization scans (#66)\n\n## Summary\n\nThis PR makes the narrow scans of late materialization (ADR 0016) apply\npushed WHERE predicates too. These are the scans that `ORDER BY ...\nLIMIT` uses over many columns, and #65 left them out. This is the ADR\n0020 follow-up for row ids.\n\n**Why they were left out:** a narrow scan's row ids name each row's\nposition in its part, and the scan counted rows to get them. A pushed\nfilter drops rows, so the count no longer gives the position.\n\n**Result:** Q23 (`SELECT *` with a URL LIKE, `ORDER BY ... LIMIT 10`)\ngoes from 0.82 s to 0.56 s (1.47×) at 128 threads. Its URL LIKE now runs\non views into the decoded pages, as Q20-Q22 do since #65.\n\n**`plan` (`table.h`):** the filtered `ScanPart` takes `positions`. With\nit, every batch ends with a `position` column (INT64, not null) holding\neach row's position in the part. Positions increase. Asking for\npositions without a filter is Invalid.\n\n**`io` (`filtered_scan.cc`):** the reader tracks each batch's first row\nin the row group and builds the positions from the selection's set-bit\nruns, allocating from the pool. A batch the filter empties still\nadvances the position.\n\n**`exec`:**\n- **`TableScanOperator`:** a narrow scan with pushed predicates re-binds\nthem from output positions to the early columns it reads\n(`NarrowFilter`). A predicate on a late column is Invalid. The scan asks\nthe table for positions and makes the row ids from them.\n- The positions column must be last, INT64 and without NULLs, and its\nvalues must lie in `[0, 2^32)`. Anything else is Invalid.\n- **Planner:** the `Filter` builder now pushes into narrow scans too,\nchecking `supports_scan_filter` on the fields actually read\n(`ReadFields`).\n- **`PartTopNOperator::Fetch`:** unchanged. It already fetches late\ncolumns by row id.\n\n**Docs:**\n- **ADR 0020:** narrow scans are filtered, with the positions contract.\nPage skipping by statistics and per-dictionary-entry evaluation are now\nmarked deferred until a dataset needs them: the ClickBench files have no\npage index and large pages, and their long strings are mostly\nplain-encoded.\n- **ADR 0016:** a line on filtered narrow scans.\n- **`docs/architecture.md`:** the \"Filtering while scanning\" paragraph.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc (final commit 0041e61)\ncheck-full exit 0; 100% tests passed out of 1426; Coverage gate: PASS\n$ pixi run tsan                # 7c91ec0; only a test was tidied after it\n100% tests passed out of 1426\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random   # cc9a5e8; later commits add checks and tests only\nDIFF: PASS seed=704071166 queries=20000 failed=0 unsupported=0\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n**Speed:** a paired A/B of release builds against main 1e38331 (#65),\nall 43 ClickBench queries at 128 threads (3 binary paths × best of 3,\nmedians):\n- **Q23:** 0.818 s → 0.557 s, and 0.820 s → 0.558 s on a re-run.\n- **Total:** 20.24 s → 19.87 s.\n- **Other queries:** only Q23 runs a filtered narrow scan, so nothing\nelse should change. In the full run a few were 3-6% apart either way. I\nre-ran every query that was more than 3% slower, and all came back\nwithin ±3% of main: Q1, Q5, Q12, Q21, Q24, Q32.\n\n**Tests:**\n- **io parity (`parquet_filter_test.cc`):** with `positions`, every\nfilter case, encoding, batch size (1 to 64Ki) and part must give the\nunfiltered rows that pass, plus their positions in the part. The OOM\nsweep runs with positions, and positions without a filter are Invalid. A\nmutation that drops the batch's first-row offset fails the test.\n- **`OperatorsTest`:**\n- **`FilteredNarrowScanNumbersTheRowsThatPass`:** row ids are the\npassing rows' positions at batch sizes 1, 2 and 64, including emptied\nbatches. A predicate on a late column is Invalid.\n- **`FilteredNarrowScanChecksThePositions`:** a stub table returns\nbatches without positions, with non-INT64, NULL, negative or past-2^32\npositions, or with INT64 max. All of these are Invalid.\n- **`PhysicalPlannerTest.LateScansAreFiltered`:** the plan stays late\nand the narrow Scan shows `2 pushed predicates`. The results, late\ncolumns included, equal those without pushdown, for ASC and DESC at\nbatch sizes 1, 3 and 64.\n- **`MemoryTable`:** reports positions, so the exec, parallel and\nprofile tests cover the path.\n- **`where/pushdown.slt`:** three `ORDER BY ... LIMIT` queries that take\nthe late path (checked with `explain --analyze`). Expectations come from\nDuckDB.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none touched)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the code,\ntests and docs, measured the change and ran the gates.\n  - A read-only reviewer agent found no P0.\n- It raised two P1s about the change, both done: survey figures came out\nof the ADR, and tests were added for the narrow scan's position checks\n(with an overflow-safe bound).\n- Its third P1 was an untracked local file, which is not part of the PR.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T20:42:15+03:00",
          "tree_id": "02126bb69ee9062833a5b5755ac2258a89a41d10",
          "url": "https://github.com/ydb-campus/antb1/commit/6e523d5ff721888393170df5d95f8b512c3adcd3"
        },
        "date": 1790876671789,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2694.7603808081026,
            "unit": "ns/iter",
            "extra": "iterations: 280036\ncpu: 2694.0161872045023 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73317.93909799261,
            "unit": "ns/iter",
            "extra": "iterations: 9113\ncpu: 73295.372434983 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 76910.80771782328,
            "unit": "ns/iter",
            "extra": "iterations: 9044\ncpu: 76909.72036709423 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 270476.6360108678,
            "unit": "ns/iter",
            "extra": "iterations: 2577\ncpu: 270451.8711680248 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 321257.84210525407,
            "unit": "ns/iter",
            "extra": "iterations: 2185\ncpu: 321250.6118993135 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1912498.9247911405,
            "unit": "ns/iter",
            "extra": "iterations: 359\ncpu: 1912201.571030639 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 41.92175244444406,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 41.898662111111086 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 37.309189888888895,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 37.28911661111114 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 155.3821309999961,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 155.1969390000001 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 9.706669249999955,
            "unit": "ms/iter",
            "extra": "iterations: 72\ncpu: 9.70454188888888 ms\nthreads: 1"
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
          "id": "875379c27fe60f5536c9433adfbdeefde02f64f7",
          "message": "test(exec): run the grouped top-N test in larger batches to fit the ASan timeout (#67)\n\n## Summary\n\n`exec.PartOperatorsTest.GroupedTopNIsTheTopNOfAllGroups` (from #61)\ntimed out in `clang-asan` on #66, at 120 s. That test does not touch\n#66's code; a re-run passed.\n- **Already near the limit:** it takes 68 s on main's CI ASan runs, and\n30 s locally, while its sibling tests take at most 4 s.\n- **A slow runner:** on #66's first attempt every test ran about 2×\nslower (the whole suite 285 s vs 161 s), which pushed this one over the\ntimeout.\n\n**Why it is slow:** it runs about 120 plans (5 orders × 6 limit/offset\npairs × serial and parallel) over 3000 rows in batches of 3 rows, so\neach plan reads about 1000 batches.\n\n**The change:** this test now runs its plans in batches of 50 rows.\n`Run` gets an optional `batch_size`, default 3, so other tests are\nunchanged. The orders, limits, offsets, executors and assertions are the\nsame. What a partition drops is decided by its groups, not by batch\nsize, and the out-of-memory part still uses batches of 3.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc\ncheck-full exit 0; 100% tests passed out of 1424; Coverage gate: PASS\nASan time of the test (local junit): 29.9 s -> 6.5 s; dev build 1.3 s\n```\n\n**The test still catches the bug it was written for:** passing `keep -\n1` to `KeepFirstRows` in `PartGroupAggregateOperator` fails it.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none touched)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code found the cause\nin the CI logs, made the change, checked it with the mutation above and\nran the gates.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T20:53:55+03:00",
          "tree_id": "d8d254e7cda97a29e4fa529781f6744e0aefa3f7",
          "url": "https://github.com/ydb-campus/antb1/commit/875379c27fe60f5536c9433adfbdeefde02f64f7"
        },
        "date": 1790877397937,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4024.928897731892,
            "unit": "ns/iter",
            "extra": "iterations: 174903\ncpu: 4024.2387094560986 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 94530.11910426682,
            "unit": "ns/iter",
            "extra": "iterations: 7145\ncpu: 94520.80181945417 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 124909.1745946871,
            "unit": "ns/iter",
            "extra": "iterations: 5613\ncpu: 124895.6274719402 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 485916.5392088758,
            "unit": "ns/iter",
            "extra": "iterations: 1441\ncpu: 485635.20610687067 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 456629.2426805449,
            "unit": "ns/iter",
            "extra": "iterations: 1537\ncpu: 456448.638256344 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2250713.1993568805,
            "unit": "ns/iter",
            "extra": "iterations: 311\ncpu: 2249826.742765274 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 52.107899769230066,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.0989026153846 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 48.19884935714315,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 48.17185407142859 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 230.0027439999989,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 229.95865533333298 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 15.06264636170267,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 15.060442595744675 ms\nthreads: 1"
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
          "id": "59a7422ee0e63b95eca806d597bcadd3580e663d",
          "message": "perf(exec): one shared counter update per memory budget allocation (#68)\n\n## Summary\n\nAt 128 threads the session's memory budget (`exec::MemoryBudget`) showed\nup in profiles of string-heavy GROUP BYs. Charge, Allocate and Free\ntogether took about 5% of the time.\n\nEvery Arrow allocation in a query went through three atomic\nread-modify-writes on one cache line shared by all threads: bytes in\nuse, total bytes allocated and the allocation count, plus a load and\nsometimes a compare-and-swap of the peak.\n\n**The change:**\n- **One counter per allocation.** An allocation or reallocation now\nupdates only `used_`, which the limit needs, plus `peak_` while it\ngrows.\n- **The totals come from the backend.** `total_bytes_allocated()` and\n`num_allocations()` return the backend pool's statistics, which Arrow's\npool keeps anyway. Nothing in the engine, CLI, bench or profiles reads\nthem through a budget, so only a test changes.\n- **Semantic change:** with the default backend these two numbers are\nnow process-wide, not per budget. The header says so.\n- **Separate cache lines.** `used_` and `peak_` each get their own\n(`alignas(64)`).\n- **Unchanged:** the limit, the OutOfMemory behavior, the peak,\n`ResetPeak` and `under_pressure`.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full          # lint, ci, asan, tidy, coverage, fuzz-smoke, ci-gcc (final commit)\ncheck-full exit 0; 100% tests passed out of 1426; Coverage gate: PASS\n$ pixi run tsan                # the first commit; only the test changed after it\nno failures\n```\n\n**Test (`memory_test.cc`):** the budget now runs over its own\n`arrow::ProxyMemoryPool`, so the backend's totals are exact. The test\nchecks that the budget reports the backend's numbers: 1 allocation and\n600 bytes. The two refused requests (over the limit) never reach the\nbackend. A mutation that lets a refused request reach the backend fails\nthe test.\n\n**Speed:** paired A/B of release builds against main 875379c, all 43\nClickBench queries at 128 threads (3 binary paths × best of 3, medians).\nThe shared host was busy during some passes, so totals vary between\nruns.\n\n| Run | main | this PR | Change |\n| --- | ---: | ---: | ---: |\n| Upper bound: a build with no accounting at all | 18.96 s | 18.59 s |\n−1.9% |\n| This change, first run | 18.97 s | 18.65 s | **−1.7%** |\n| This change, final commit (Q32–Q34, Q38, Q1, Q6 re-run after a\nbusy-host pass) | 19.73 s | 19.47 s | **−1.3%** |\n\n- **Faster by 5–14%:** Q12, Q14, Q15, Q16, Q17, Q36, and Q5 in the first\nrun.\n- **Slower:** nothing beyond noise. The re-runs of Q32–Q34 are within\n±2%, and Q1, Q6, Q19 and Q38 differ by under 1 ms.\n\nPer-thread memory credit, which would remove the last shared update, was\nconsidered and not done: at most about 0.2% more would be left to gain.\nIt would also make the limit, the peak and the \"every byte back\" tests\ninexact by up to threads × chunk.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed (internal statistics; the\nheader comment says it)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none touched)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code measured the\nupper bound and the variant, made the change and the test, and ran the\ngates. A read-only reviewer agent found no P0. Its P1, that the adjusted\ntest was too weak, is fixed: the test now asserts exact backend totals\nand fails on the mutation above.\n- Accountable human (has read and understands the whole diff): @hor911",
          "timestamp": "2026-10-01T23:16:51+03:00",
          "tree_id": "d4edf57c02e3f0fe49d21be04e9352b97c87f722",
          "url": "https://github.com/ydb-campus/antb1/commit/59a7422ee0e63b95eca806d597bcadd3580e663d"
        },
        "date": 1790885949331,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3120.5148335160284,
            "unit": "ns/iter",
            "extra": "iterations: 222604\ncpu: 3120.258252322509 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 73062.36276204862,
            "unit": "ns/iter",
            "extra": "iterations: 9254\ncpu: 73044.35476550683 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 84650.5559051314,
            "unit": "ns/iter",
            "extra": "iterations: 8264\ncpu: 84635.38952081316 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 375693.2286634476,
            "unit": "ns/iter",
            "extra": "iterations: 1863\ncpu: 375589.07514761144 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 353314.8876008017,
            "unit": "ns/iter",
            "extra": "iterations: 1984\ncpu: 353222.34022177436 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 2170416.59752323,
            "unit": "ns/iter",
            "extra": "iterations: 323\ncpu: 2169246.8328173365 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 42.31927823529451,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 42.31633794117648 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 38.94172026315795,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 38.92230799999998 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 171.8434990000013,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 171.81195049999997 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.717151483333529,
            "unit": "ms/iter",
            "extra": "iterations: 60\ncpu: 11.715150299999996 ms\nthreads: 1"
          }
        ]
      }
    ]
  }
}