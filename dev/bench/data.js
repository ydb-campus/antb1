window.BENCHMARK_DATA = {
  "lastUpdate": 1790409422563,
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
      }
    ]
  }
}