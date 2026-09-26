window.BENCHMARK_DATA = {
  "lastUpdate": 1790427144291,
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
      }
    ]
  }
}