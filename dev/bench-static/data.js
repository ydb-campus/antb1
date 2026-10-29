window.BENCHMARK_DATA = {
  "lastUpdate": 1793262524158,
  "repoUrl": "https://github.com/ydb-campus/antb1",
  "entries": {
    "antb1 micro benchmarks (static build)": [
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "8e909b564cf74f92fe229f11671af4e01067a405",
          "message": "perf: run every benchmark on the static build (#238)\n\n## Summary\n\nPR 2 of the static-build plan ([ADR\n0033](docs/adr/0033-standalone-static-binary.md)): every performance\nmeasurement\nnow runs on the standalone static binary, and the dynamic `bench` preset\nis gone.\n\n- **Tasks:** `pixi run bench` and `pixi run bench-clickbench` move to\nenv `static` and build in `build/static`\n  through a new `scripts/static.sh --build TARGET...`.\n- That is still the one place that configures build/static: the Arrow\nprefix, and the commit with `-dirty`.\n- Results go to `build/static/bench-results/`, or `$ANTB1_BENCH_OUT`,\nwhere a relative path stays inside that\n    directory, so results never land in the work tree.\n- **A given binary:** with `ANTB1_BENCH_BIN`, `bench-clickbench` runs a\ndownloaded package or a campaign arm. It\n  builds and downloads nothing.\n- The path must be absolute. It is resolved, so a symlink cannot skip\nthe allocator restart.\n- A build other than the static Release one is refused unless\n`ANTB1_BENCH_ANY_BUILD=1`.\n  - On Linux, a binary whose restart did not happen is refused.\n- `version.txt` (the binary's `version --verbose`) goes next to the\nJSON.\n  - Every bad setting fails before anything is built.\n- **bench.yml:**\n  - runs in env `static`;\n  - restores main's prefix read-only, and builds it cold on a miss;\n- only the `performance` label starts or cancels a run, in both the\nconcurrency group and the job `if`;\n- uploads one artifact directory: `micro.json`, `clickbench.json` and\n`version.txt`;\n- its summary shows the CPU, whether the prefix cache hit, and the\nversion;\n- publishes to a new gh-pages series, `dev/bench-static`. `dev/bench`\nstays, frozen;\n- its push paths now cover `cmake/`, the presets, `pixi.toml` and the\nscripts.\n- **Nightly:** `static-cold` also runs `pixi run bench\n--benchmark_dry_run=true`, so the static micro bench builds and\n  runs on all three targets every night.\n- **micro.json** names its build in the report context: `antb1_commit`,\n`antb1_flavor`, `antb1_cpu_baseline` and\n  `antb1_build_type`.\n- **Docs:**\n- `docs/benchmarks.md`: the rule, the new variables, a recipe for hosts\nwithout the repository or pixi, and the\n    series break, with a ratio per benchmark.\n- `docs/ci.md`, `docs/testing.md`, `docs/agents.md`, CONTRIBUTING.md,\nAGENTS.md:41, and the clickbench-data recipe\n    and skill, which now allow exactly that recipe's download.\n- ADR 0033: a Linux A/B entry, the clock-read measurement, and a\ncorrected restart sentence (a renamed copy does\n    restart).\n- **Tests:** `tools/ci/tests/test_bench_scripts.py` (new, hermetic) runs\nthe real scripts in a sandbox repository\nwith fake `cmake`, `antb1`, `sudo`, `uname` and `static-arrow.sh`, in an\nenvironment built from scratch.\n\n## The preconditions ADR 0033 set for this move, measured on this host\n(2x Xeon Gold 6338, Linux 5.4)\n\n- **ClickBench A/B** (100 files, hot, 43 queries, every answer equal):\nstatic/bench summed hot time is 0.822 at 1\n  thread, 0.854 at 16, 0.940 at 64 and 0.950 at 128.\n  - The run was verified independently.\n  - About 28-29% fewer user-mode instructions on every query.\n- **Clock reads:** static glibc 2.28 has no vDSO, so a clock read is a\nsystem call.\n- `antb1 bench` makes 912 `clock_gettime` calls for 43 queries × 3\ntries, on 1 or 100 files, at 1, 16, 64 and\n    128 threads: 9 at start and 7 per query run.\n- Each call costs 0.34 µs more than a vDSO read, so about 2.4 µs per\nrun.\n- `explain --analyze`: at most about 4% of a query on the worst cases\nmeasured.\n- The counts never grow with data, threads or row groups, so nothing\nblocks the move.\n- **The micro-series break:** both micro binaries built from one commit,\n4 + 4 runs in ABBA order behind the host's\nidle gate, every run clean. The geometric mean of static/dynamic is\n0.919 over 38 benchmarks. They range from 0.566\n(`BM_ScanColumn`) to 1.377 (`BM_Int128AvgAccumulate`), the one clear\nloss: steady across runs, and worth a look of\n  its own. The full table is in docs/benchmarks.md.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\nOn d1bb687 (main ccb6f98 plus this change) unless noted:\n\n```text\n$ pixi run check\nlint: 11 hooks Passed; 164 passed (tool tests, 27 of them in test_bench_scripts.py)\n100% tests passed out of 2898                                    (58 s)\n\n$ pixi run static                                                # packaging, gates, the CLI goldens\n100% tests passed out of 91\nstatic: OK: build/static/package\n\n# Before the review fixes (5aee775), on the real tree:\n$ pixi run bench-clickbench                                      # built in build/static, hits_0\nbuild/static/bench-results: clickbench.json version.txt          (commit recorded by the binary, no --git-sha)\n$ pixi run bench\n38 benchmarks; micro.json context: antb1_commit 5aee775..., antb1_flavor static-thinlto,\n  antb1_cpu_baseline -march=x86-64-v3, antb1_build_type Release\n$ ANTB1_BENCH_BIN=$PWD/build/ci/bin/antb1 pixi run bench-clickbench                   # dynamic Debug build\nexit 2: ... is not the standalone static Release build (flavor 'dynamic', build type 'Debug') ...\n$ ANTB1_BENCH_BIN=$PWD/build/ci/bin/antb1 ANTB1_BENCH_ANY_BUILD=1 pixi run bench-clickbench   # one own query\nwarning: ... running it anyway (ANTB1_BENCH_ANY_BUILD=1); exit 0\n\n# After the review fixes, on the real tree:\n$ ANTB1_BENCH_BIN=<packaged antb1> ANTB1_BENCH_OUT=arm-x ANTB1_BENCH_COLD=0 pixi run bench-clickbench\nnothing built; results in build/static/bench-results/arm-x; nothing new in the work tree\n$ ANTB1_HITS_FILES=<hits_0>,/nonexistent.parquet pixi run bench-clickbench\nexit 2, before anything is built\n```\n\n`bench.yml` runs on this PR once it has the `performance` label: env\nstatic, the restore of main's prefix, the cold\nClickBench path and the artifact layout. The nightly's static-cold runs\nthe micro dry run on macOS and aarch64 after\nthe merge.\n\n## Not changed here (protected; needs a maintainer)\n\n- ADR 0006 (lines 62-63 and 101) says only the `default` and `gcc`\nenvironments install `duckdb-extension-tpch`\n(TPC's dbgen, under the TPC EULA). Env `static` installs it too, through\nfeature `base`, since #235. This PR fixes\n  CONTRIBUTING.md and `docs/testing.md`; ADR 0006 needs the same change.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (`tools/ci/tests/test_bench_scripts.py`;\n`bench.micro.smoke` runs the new context code)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, ADR 0033)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer:\n- `pixi.toml`, `CMakePresets.json`, `.github/workflows/bench.yml` and\n`nightly.yml`;\n  - `AGENTS.md:41`;\n  - `docs/adr/0033`;\n  - `.agents/skills/clickbench-data` and its `.claude` copy.\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5)\n  - A five-agent investigation and a plan review.\n- The measurements: clock counts, and the micro side-by-side behind the\nhost's idle gate.\n  - The change and its tests.\n- The `reviewer` subagent and a five-lens review with verifiers. Their\n20 confirmed findings are fixed here.\n- Accountable human (has read and understands the whole diff): @\n\nThis workload is derived from the TPC-H Benchmark and is not comparable\nto published TPC-H Benchmark results, as this\nimplementation does not comply with all requirements of the TPC-H\nBenchmark.\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-25T15:29:16+03:00",
          "tree_id": "6147d6c3ed6ac3fe71f47d45a75b2bba106bcfba",
          "url": "https://github.com/ydb-campus/antb1/commit/8e909b564cf74f92fe229f11671af4e01067a405"
        },
        "date": 1792932071327,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4614.842570451127,
            "unit": "ns/iter",
            "extra": "iterations: 151382\ncpu: 4614.364085558389 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 28511.14018235237,
            "unit": "ns/iter",
            "extra": "iterations: 24568\ncpu: 28509.499104526203 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52914.20913766236,
            "unit": "ns/iter",
            "extra": "iterations: 13417\ncpu: 52905.29529701125 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 220806.18305407016,
            "unit": "ns/iter",
            "extra": "iterations: 3163\ncpu: 220799.56180840975 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 498460.89387461456,
            "unit": "ns/iter",
            "extra": "iterations: 1404\ncpu: 498420.6837606834 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1493622.7823274988,
            "unit": "ns/iter",
            "extra": "iterations: 464\ncpu: 1493555.877155173 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 39.6736307777764,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.66877138888893 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.86847933333426,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.866463533333324 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 182.51578574999883,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 182.49237350000013 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.9391164999995,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.938614300000014 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 19.52448399999993,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.521204638888918 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 36.10079115789308,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.097411736842055 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.378701231732685,
            "unit": "ms/iter",
            "extra": "iterations: 479\ncpu: 1.3784553736951979 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 85.4912345714313,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 85.49045114285722 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.65508503225808,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.653331516128993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.520426571427834,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.517451500000018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 52.375510923079325,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.371634153846166 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 75.42503277778047,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 75.41596633333312 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 256.30153833333225,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 256.28979299999935 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1272.0919480000248,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1271.908419999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.519212359998164,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.515656000000064 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 30.912323130433812,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.910376999999887 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 53.464126538463354,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.454557615384765 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 96.6613908571422,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.65403214285716 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 312.9285134999975,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 312.9100845000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1523.8322729999823,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1523.6848589999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 55.82202169230973,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 55.81901200000007 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 58.76563125000208,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.76220083333313 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 84.81753662500324,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.81140399999987 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 145.14844019998918,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 145.11738220000012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 409.5479970000042,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 409.4931134999982 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1878.855609000027,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1878.5823280000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.04431381818098,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.04001518181855 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.53417270000364,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.52938239999986 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 95.90885657143288,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.88724171428608 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 164.4962442500031,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 164.47153075000108 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 428.4505410000179,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 428.40533700000094 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2066.5073310000253,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2066.1213660000044 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "aaaa84a1ef005e39ad65b845a779632439267ab6",
          "message": "perf(plan,exec,io): never materialise a VARCHAR column only a predicate reads (#239)\n\n## Summary\n\nA VARCHAR column that only a pushed predicate reads is decoded,\ncompared, and then **also copied\ninto every batch for no reader at all**. This reads it as dictionary\nindices and never materialises\nit. It is the \"labelled version\" that `#233`'s refusal named and could\nnot measure.\n\n| | 4 threads | 14 threads |\n| --- | ---: | ---: |\n| **Q19** | **0.688×** | **0.771×** |\n| **Q10** | **0.789×** | **0.824×** |\n| Q13 | 0.924× | 0.930× |\n| Q21, Q06 | 0.960×, 0.968× | 0.961×, 0.990× |\n| **the 22, geomean** | **0.963×** | **0.972×** |\n\nSF 100, 5 repeats interleaved and rotated, both arms `pixi run static`\nfrom one tree (ADR 0033), all\n22 answers byte-identical at both thread counts. Peak memory does not\nmove — Q18 sets it and has no\ndroppable VARCHAR. Nothing regresses beyond this host's ±1.4% noise: the\nwidest readings are Q01\n1.019× and Q04 1.008×, neither of which has a VARCHAR predicate for this\nto touch, which is what\ncalibrates that floor.\n\n**Against DuckDB this clears the suite's two worst queries off the\ntop.** Q10 1.698× → about\n1.340×, Q19 1.632× → about **1.123×**, Q13 0.999× → 0.923×, and the 22\nfrom 1.249× to about 1.203×.\nThose are compositions of two same-session ratios rather than a\nmeasurement, and the DuckDB arm was\na dynamic `ci-release` build where these figures are static ones — ADR\n0033 measures static at\n0.973× of the `bench` preset and `ci-release` within 1% of `bench`, so\nthe static standing is about\nthree points better than those numbers. Q10 was the worst query in the\nsuite and is no\nlonger.\n\n## Why `#233` measured 0.997× for what is 0.963× here\n\nThat refusal skipped the *copy into the batch* and kept everything else,\nand said plainly that it\ncould not reach the expensive path. The copy is the smaller half. On a\n4-value dictionary column\nover 600 M rows, one equality predicate measures **2.49 ns a row**\nthrough views, **0.99 ns**\nthrough the dictionary, and **10.83 ns** to materialise the column\ndensely. So:\n\n- skipping the copy alone leaves the per-row view fill and the compare —\n`#233`'s 0.997×;\n- the dictionary alone leaves the materialisation — measured here at\n**1.011× and 0.971×** on two\n  isolated scans, which is noise;\n- both together measure **0.646×** on such a scan.\n\nEach half alone is worth nothing. The item is their conjunction, and\n`#233` had no way to see that\nbecause no file this project benchmarks could reach a dictionary at all.\n\n## ADR 0020 was wrong on a point of fact, and that is why\n\nIt said a gate of our own \"could only disagree with Arrow's, and Arrow's\nis the one that governs\nwhether `ReadBatchWithDictionary` throws\". Arrow's gate and the throw\nare different questions.\n`ColumnWithExposeEncoding` reads the **optional** `encoding_stats` and,\nwhen satisfied, sets one\nfield — `exposed_encoding_`, which Arrow 25's reader sets, returns and\n**acts on nowhere else**\n(`column_reader.cc:941,977,986` are its only three occurrences). What\ngoverns the throw is\n`current_encoding_ != RLE_DICTIONARY`, tested per data page (`:1066`).\nSo the advisory flag can be\nleft to Arrow while the real question is answered from `encodings`,\nwhich Parquet **requires**.\n\nDuckDB wrote this project's TPC-H-derived data and emits no\n`encoding_stats`, so ADR 0020's\ndictionary item — built, tested, and measured in a micro-benchmark at\n0.19 ns a row — had never run\non a benchmark at all.\n\n**Only a v1 dictionary page is proof.** v1 marks the dictionary page and\nthe data pages that index\nit alike, so a chunk listing `PLAIN_DICTIONARY` and nothing else holds\nno plain page; v2 marks the\ndictionary page `PLAIN` and leaves `{PLAIN, RLE_DICTIONARY}` ambiguous.\nv2 files keep Arrow's door,\nwhich grants them, so the two paths do not overlap and neither is dead —\nasserted over all four\nshapes Arrow's writer produces, each against **both** doors.\n\n## Three things had to hold at once\n\n1. **`io` reaches the dictionary** (commit 1, worth nothing by itself).\n2. **The planner says which columns nothing above reads.**\n`FilterNode::needed_above`, captured by\n`Prune` before the Filter's own predicates join the demand — the same\nfield and capture point as\nADR 0026's `JoinNode::needed_above`, with **both** of that item's review\nfindings built in from\nthe start (intersected with the node's own outputs, replaced rather than\nappended). It is a\n`std::optional` where the join's is a plain vector, because an empty set\nis the *valuable* case\nand not the missing one: `COUNT(*)` with a `WHERE` reads nothing above\nits Filter, so every\ncolumn its scan reads is read to be compared and for nothing else. The\njoin's field gives that\n   case up.\n3. **The position survives.** Operators above read a column by the\nposition `ResolvePositions` gave\nit, so a column cannot be removed from the middle. `ScanPart` takes a\n`returned` count, the table\nputs the returned fields first, and the operator puts them back with an\nall-NULL array of the\nfield's **declared** type in each gap — not the `null` type ADR 0016's\nlate split uses, because a\nlate column is re-read by a node that knows it is a placeholder, where\nthis one can flow into a\njoin build and be gathered past the join. An `ANTB1_CHECK` in the gather\nsaid so on the first run.\n\n## Where an empty `strings` is a trap\n\nWith a dictionary the filter reads `dictionary` and `indices` and never\n`strings` — but\n`KeepByDictionary` **declines** when the dictionary holds more distinct\nvalues than the piece has\nrows, falling back to one compare a row. So `io` skips the views per\npiece under exactly that\ncondition and the filter refuses the shape no table may produce. Two\ncorrections from review:\n\n- **An empty dictionary is one of the cases that needs views**, and it\nis reachable: a chunk whose\nevery value is NULL still gets a dictionary page, of no entries, which\nboth doors grant. `0 >\nlevels` is false, so the size comparison alone said not to write views.\nIt would not have changed\nan answer today — an empty dictionary means every row is NULL and a NULL\nrow is rejected on its\nvalidity before any predicate reads its value — but that is a\ncoincidence of the predicate kinds a\n  scan can apply, not a property anything states.\n- **The two shapes were not actually distinguishable.** `views_` was\nalways *resized*, so\n`strings.size() == rows` whether or not the views were written: the\nfilter's own shape check could\nnot tell them apart, and the per-row path could have read values nobody\nwrote. It is now left\n**empty** when unfilled, which is what the second shape is, and the\npiece's row count comes from\n  the read rather than from that vector's size.\n\n## Tests\n\nEvery new case was mutation-checked: revert the ingredient, watch the\ncase fail, restore.\n\n- `EveryDataPageIsDictionary` over all four writer shapes, each asserted\nagainst both doors, with\nthe invariant that between them they reach every fully\ndictionary-encoded chunk.\n- **A file the test oracle writes**, because Arrow's writer cannot\nproduce one without\n`encoding_stats` and that is the only situation in which the new proof\ndecides anything. It\nasserts that Arrow's door declines for that file, and that\n`cols_filter_only` and `dict_pieces`\nare what they should be. **Removing either half of the item fails that\none case.**\n- The guard between a recorded set and a dropped live column: a\n`needed_above` naming an id the\nFilter does not output must keep **every** column, asserted on the\nvalues, not just the metric.\n- A key filter over a scan whose first field is filter-only — see below.\n- The `ScanValues` shapes: a dictionary with no views accepted; neither\nrefused; a dictionary larger\nthan the piece with no views refused, which is the shape no table may\nproduce.\n- The `returned` contract's four refusals, `FilterNode::needed_above`\n(set vs empty vs under a\njoin's branch vs a second pass), and four `.slt` answer cases — a\npredicate-only column, the same\ncolumn read above, a predicate the Filter still evaluates, and NULLs in\nboth.\n- `MemoryTable` and the io tests' `TestFilter` learned the narrowing and\nthe second shape, so the\nexec and io suites exercise the real path rather than opting out of it.\n\n## What the review round found\n\nA P0, five P1s and a P2, and **the P2 made the item measurably better**:\nthe branch that skips a\ncolumn's values still pushed one entry a row into a row list nothing\nreads — write-only, since no\narray is finished for such a column, and with the wrong base into the\nbargain. Dropping it moved Q19\nfrom 0.759× to **0.688×** and the 22 from 0.971× to **0.963×**.\n\n**A wrong-answer bug I found while the review ran**, and which nothing\nin the suite caught: ADR\n0029's key-filter column is named by position in the field list, which\nthis change reorders. The key\ncolumn is never dropped — the join reads it — but it *moves*, and the\nindex was passed unmapped. On\na VARCHAR column that is an error; on a second fixed-width one it would\nhash the wrong values and\ndrop rows the join must keep. Every field-naming path now goes through\none mapping, with a test that\nfails without it.\n\nThe rest: `find` rather than `binary_search` over a set whose order\nnothing verifies;\n`CloneWithFreshIds` drops `needed_above`, which the field's own comment\nnames as the hazard; the\npublic header no longer describes the `null`-type placeholder the code\nstopped using; and the oracle\nwrites its file on one thread, so the test adds no parallelism of its\nown.\n\n## Verification\n\n`pixi run check` cannot complete on this macOS host: its `lint` leg\nstops on 36 pre-existing\nshellcheck SC2218 findings in files this PR does not touch.\n\n```text\n$ pixi run test                      # Debug, the full hermetic suite\n100% tests passed out of 2908        ANTB1-TESTS: PASS\n\n$ pixi run release                   # RelWithDebInfo, -Werror\n100% tests passed out of 2909\n\n$ for s in 20261119 20261120; do ANTB1_DIFF_SEED=$s pixi run diff-random; done\nDIFF: PASS queries=2000 failed=0 unsupported=0     # both seeds\n\n$ pixi run static                    # the arm both measurements used\nstatic: OK: build/static/package\n\n$ pixi run lint\nevery linter Passed except shellcheck (36 pre-existing SC2218, none in a file this PR touches)\n\n$ ./.pixi/envs/default/bin/clang-tidy -p build/tidy --warnings-as-errors='*' <9 changed files>\nnothing in user code\n```\n\nclang-tidy caught several things in my own code, including two\n`bugprone-unchecked-optional-access`\nfindings it does not accept an `ASSERT_TRUE(has_value())` as a guard\nfor, and a\n`performance-no-automatic-move` on a returned const local.\n\n**The gate I would most want is one I cannot run.** `pixi run asan`\nfails at discovery on this host\non a pre-existing `curl_global_init` leak, and ASan is the leg most\nlikely to catch a lifetime\nmistake in a scan that stops filling a view buffer. CI is the gate for\nthat, and for `tidy` and\n`ci-gcc`/`codeql-build` (linux-64 only).\n\n## Type of change\n\n- [x] perf: performance improvement\n- [ ] feat / fix / refactor / docs / Breaking change\n\n## Checklist\n\n- [ ] `pixi run check` passes locally — unchecked for the reason above,\nnot because it was skipped\n- [x] Tests cover the change, each mutation-checked; what they cannot\nreach is stated above\n- [x] Docs updated: ADR 0020 gains the item with both configurations'\nmeasurements, the correction\nto its own earlier reasoning, why `#233`'s ceiling was right about what\nit measured, and what\nthe tests reach; `docs/sql-subset.md` documents the two new metrics\n- [x] No ClickBench-derived data is committed\n- [x] Nothing derived from TPC-H is committed: no query text, data,\nanswers or EXPLAIN output. The\nADR carries timings, ratios, query numbers and peak memory, which ADR\n0006 permits, and names\n      the host and scale factor as its Naming clause requires\n- [x] Changes to governance paths were agreed with a maintainer —\n**`docs/adr/0020-filter-pushdown.md`\nis CODEOWNERS and wants an explicit look**: the entry reverses a refusal\nthis ADR recorded a\nday earlier and corrects a statement of fact in it. @hor911 asked for\nthis item to be\n      prototyped, sized and built.\n\n## AI assistance\n\n- [x] AI-assisted. Tools and what they did: Claude Code (Opus 5) found\nthe item by measuring DuckDB\nand antb1 against each other on the same file — 1.01× versus 3.3× for a\nstring predicate\nagainst an equally selective numeric one — then read Arrow's source to\nestablish that this\nADR's recorded reason for not pursuing it was factually wrong. It\nprototyped both halves\nbefore writing anything, which is what showed that neither pays alone.\nIt also got the\nplaceholder type wrong in a way only a hardened build caught, wrote a\ntest whose field order\nmade it pass for the wrong reason, and reported a composed DuckDB ratio\nwithout noticing the\ntwo arms were different build flavours — each corrected above rather\nthan quietly. The\nproject's `reviewer` then found a P0, a dead per-row fill that was\ncosting real time, and that\nthe two `ScanValues` shapes were never distinguishable; a wrong-answer\nbug in ADR 0029's key\n      column was found by re-reading the diff while that review ran.\n- Accountable human (has read and understands the whole diff): @hor911\n\n---------\n\nCo-authored-by: Claude Opus 5 (1M context) <noreply@anthropic.com>",
          "timestamp": "2026-10-25T17:01:34+03:00",
          "tree_id": "c4778f6b76e87bf6b1891418a4e776b2a0e0376d",
          "url": "https://github.com/ydb-campus/antb1/commit/aaaa84a1ef005e39ad65b845a779632439267ab6"
        },
        "date": 1792937627024,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4892.6200832851555,
            "unit": "ns/iter",
            "extra": "iterations: 147685\ncpu: 4892.364227917527 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 28974.36390496111,
            "unit": "ns/iter",
            "extra": "iterations: 24369\ncpu: 28971.187697484518 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 51992.75336687942,
            "unit": "ns/iter",
            "extra": "iterations: 13514\ncpu: 51985.675521681216 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 230146.17104399548,
            "unit": "ns/iter",
            "extra": "iterations: 3046\ncpu: 230117.80564674988 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 503295.6610664509,
            "unit": "ns/iter",
            "extra": "iterations: 1369\ncpu: 503204.199415632 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1689495.8104264992,
            "unit": "ns/iter",
            "extra": "iterations: 422\ncpu: 1689278.9289099535 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 41.20295962500009,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 41.197119625 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 46.51456219999849,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.50647220000005 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 232.20217133333185,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 232.16240033333335 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.829729425532557,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.828175659574448 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 19.808644416665782,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.804441861111115 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 36.55229000000154,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.54873294736838 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.2654669119266404,
            "unit": "ms/iter",
            "extra": "iterations: 545\ncpu: 1.2651850752293552 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 145.52421519999825,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 145.50544999999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.93540958064458,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.932148354838702 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.90123989285767,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.898601214285712 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 53.64419446153499,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.63056192307689 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 124.46781360000614,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 124.44152559999964 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 316.57536349999305,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 316.4942244999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1411.7643019999946,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1411.6430069999985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.739510079998126,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.737435559999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.316290909090455,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.3141277272727 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 56.0040215000015,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.99660008333333 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 121.4650039999962,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 121.45713466666723 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 380.04474350000805,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 380.00050750000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1686.8140319999725,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1686.640122 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.16380500000181,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.1545463333332 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 59.488698000000305,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.4820933333331 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 90.79897437499795,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 90.78366662500015 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 201.91311533331677,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 201.90799699999926 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 500.9935539999901,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 500.81630500000074 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 2053.4731109999598,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2052.922491000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 66.71253672727124,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 66.69599790909137 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 69.68836329999704,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.68196509999984 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 107.97747085713841,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 107.9619521428573 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 246.17201433333472,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 246.11298233333181 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 547.3310739999988,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 547.2883210000034 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2213.1052650000242,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2212.8083349999984 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "4c931ec545d3e4a03a390c2c3e862c1e22e88ac3",
          "message": "chore: license antb1 under apache-2.0 and ship the static binary's sources (#241)\n\n## Summary\n\nPR 3a of the static-build plan: antb1 gets a licence, and every Linux\nstandalone package gets the sources that\nLGPL-2.1 section 6 asks for, because it links glibc 2.28 statically.\nReleases (`release.yml`) follow in PR 3b and\nreuse these assets. Decision record: [ADR\n0034](docs/adr/0034-licence-and-static-sources.md), Proposed.\n\n- **Apache-2.0.**\n- `LICENSE` is the official text, byte for byte (sha256 `cfc7749b…`, the\nsame as the copy in Arrow's LICENSE.txt).\n- `NOTICE` is two lines: `antb1` and `Copyright 2026 Aleksandr\nKhoroshilov`.\n- README.md and CONTRIBUTING.md state the licence. Contributions come in\nunder the same terms (section 5), with\n    no CLA and no sign-off.\n- `/LICENSE` and `/NOTICE` become governance paths, in GOVERNANCE_PATHS,\nCODEOWNERS, AGENTS.md, the settings\n    ask rules and the lint test fixture (R012).\n- **The package carries its licences.** `scripts/static_package.py\nnotices` writes LICENSE, NOTICE and\nTHIRD_PARTY_NOTICES.txt into both the package and the `.debug` tarball.\nThe `.debug` tarball now unpacks next to\n  the binary, under the same directory.\n- THIRD_PARTY_NOTICES.txt has one section per linked library, labelled\nwith the library and its version\n(`brotli 1.0.9: LICENSE`, `GNU C Library (glibc-2.28-236.el8_9.13):\nCOPYING.LIB`). Boost's licence is new, and\n    so is libc++'s on macOS.\n- On Linux it starts with the LGPL notice. The notice names the glibc\nsource package, says where the sources\nare and how to relink, and says that the GCC runtime is not among them.\n  - A new gate, `static_gates.py package`, checks the packed tarballs:\n    - members, file types and paths;\n    - LICENSE and NOTICE against the repository's copies;\n    - the notice sections each target needs;\n    - BUILDINFO.json and README.txt;\n    - SHA256SUMS.\n- BUILDINFO.json gains `license`, `source`, `sysroot` and\n`glibc_source`.\n- **glibc's source.** `cmake/static/glibc-source.lock` maps both locked\nsysroot builds to AlmaLinux's\n`glibc-2.28-236.el8_9.13.src.rpm` (18,177,129 bytes). Both builds\nrepackage AlmaLinux 8.9's RPMs unchanged.\n- The pin matches the vault's repodata (primary.xml). A second operator,\nmirrors.aliyun.com, serves the same\n    bytes.\n- `pixi run static` refuses to package with a sysroot build that is not\nin this lock. A lint test also fails a\n    lock refresh that brings one in.\n- **`pixi run static-sources`** (new task, network once, Linux) runs\nafter `pixi run static`. It writes\n`build/static/sources/antb1-<target>.sources.tar` (66 MB) and its\n`.sha256`. The tarball holds:\n  - the SRPM;\n- `antb1-source.tar.gz`: `git archive` of the packaged commit, refused\nwhen the tree is dirty or the commit\n    differs;\n  - the 12 Arrow pins;\n  - the CLI11 and Boost-headers conda archives;\n- a README with the rebuild and relink steps. It also says that\ninstalling env `static` installs TPC's dbgen\n    under the TPC EULA.\n\nDownloads reuse `scripts/static-sources.sh --fetch`, with one retried\npass. `--check-urls` checks every copy of\n  these files.\n- **static.yml:**\n- Every leg now runs `pixi run static-sources`; macOS does nothing. The\njob is renamed `static <target> (pixi run\n    static && pixi run static-sources)`.\n- On main and manual runs, a Linux leg first checks and uploads\n`antb1-<target>-sources`, then the package.\n    A package therefore never goes up without its sources.\n- The macOS failure artifact now carries the licence texts with the\nbinary.\n- The `urls` job also runs `static-sources --check-urls` when the glibc\nlock or the script changes. That is the\n    case in this PR.\n- **Docs:** ADR 0034 (new) and ADR 0033 (in place, still Proposed);\ndocs/ci.md (task, job, artifacts, Scorecard);\ndocs/benchmarks.md; the dependency-update recipe and skill (licence and\nsysroot steps); AGENTS.md:41 and :112 and\n  CLAUDE.md, both in place.\n\nThe Arrow prefix key and pixi.lock do not change: no `*.cmake` file\nunder cmake/static changes, and\n`scripts/static-arrow.sh` is untouched.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\nOn 88dd28c (main aaaa84a plus this change), linux-x86_64:\n\n```text\n$ pixi run check\nlint: PASS (201 tool tests; new: tools/ci/tests/test_static_package.py, more in test_static_sources.py)\n100% tests passed out of 2908\n\n$ pixi run static                       # existing prefix 1a64a30529ed3973, unchanged\nstatic-gates: OK (package: 27 checks, among them LICENSE/NOTICE equal, the LGPL notice first, every section)\n100% tests passed out of 91              # CLI goldens against the packaged binary\nstatic: OK: build/static/package\n\n$ pixi run static-sources\nstatic-sources: OK: build/static/sources/antb1-linux-x86_64.sources.tar (66 MB): glibc-2.28-236.el8_9.13.src.rpm,\n  antb1-source.tar.gz (88dd28c611ce) and 14 third-party files\n# sha256sum -c of the .sha256 and of the SHA256SUMS inside: OK; git get-tar-commit-id = the packaged commit;\n# entries owned by 0/0; a second run downloads nothing; a dirty tree and an unmapped sysroot build are refused\n\n$ pixi run static-sources --check-urls\nstatic-sources: OK: 6 of 6 URLs serve their pinned file\n```\n\nBoth reviews were run on the diff:\n- **The `reviewer` subagent:** five P1 findings.\n- **A five-lens review with an adversarial verifier per finding:** 15\nconfirmed, 2 rejected.\n\nBoth reviews' findings are fixed in bb405db, with one exception: a fully\nfaked hermetic test of static.sh's\npackaging mode. Instead, every real `pixi run static` runs the new\npackage gate, here and on all three CI legs.\n\nOn this PR, static.yml runs all three legs, with `static-sources`\nagainst the real hosts, and the `urls` job checks\nevery copy.\n\n## After the merge (maintainer)\n\n- **Delete the old Linux artifacts.** Main's `antb1-linux-*` artifacts\nfrom before this merge are still\ndownloadable, for up to 30 days. They carry neither a licence nor their\nsources. Once the first post-merge run has\n  uploaded a package with its sources:\n  - list the old artifacts:\n`gh api --paginate\n'repos/ydb-campus/antb1/actions/artifacts?per_page=100' --jq\n'.artifacts[] | select(.expired|not) | [.id,.name,.workflow_run.id] |\n@tsv'`;\n- delete every `antb1-linux-*` artifact whose run has no matching\n`-sources` artifact, and every old\n`failed-antb1-*`: `gh api -X DELETE\nrepos/ydb-campus/antb1/actions/artifacts/<id>`.\n- **Check the result:**\n- `gh api repos/ydb-campus/antb1 --jq .license.spdx_id` prints\n`Apache-2.0`;\n  - the next Scorecard run passes License.\n- **Not legal advice.** An open-source office or a lawyer should review\nthe LGPL route before the first public\nrelease (3b). ADR 0034 names the open question: whether the statically\nlinked GCC runtime archives count as the\ncompiler \"accompanying\" the executable. It also gives the fallback if\nthey do.\n\n## Not in this PR\n\n- **PR 3b:** release.yml, the tag rulesets, the `release` environment\n(both maintainers review, no self-review, no\nadmin bypass), immutable releases, SECURITY.md and the README install\nsection.\n- **Later:** the nightly `static-urls` check of the glibc lock;\nreproducible ownership in the binary tarballs; a\n  hermetic test of static.sh's packaging mode.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (tools/ci/tests: notices, sources, the\npackage gate, the repository's locks)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the approved plan for\n  PR 3a. The paths:\n- `pixi.toml`, `.github/workflows/static.yml` and `scorecard.yml`,\n`.github/CODEOWNERS`;\n  - `cmake/static/glibc-source.lock` and `licenses/{libcxx,Boost}-*`;\n- `tools/lint/check_repo.py` and its test fixture,\n`.claude/settings.json`, CLAUDE.md, `AGENTS.md:41` and `:112`;\n  - `docs/adr/0033`, `0034` and the ADR index;\n  - the dependency-update skill.\n- LICENSE and NOTICE themselves, with the copyright line chosen by the\nmaintainer.\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5)\n- A five-lens investigation with a critic, and a Plan-agent review of\nthe plan.\n- The pins (the Apache text, the SRPM against the vault's repodata and a\nmirror, the libc++ and Boost licences).\n  - The change and its tests.\n- The `reviewer` subagent and a five-lens review with verifiers. Their\nfindings are fixed here.\n- Accountable human (has read and understands the whole diff): @\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-25T19:44:17+03:00",
          "tree_id": "77dbe439302de2621061d310f3313cfdfd9f14dc",
          "url": "https://github.com/ydb-campus/antb1/commit/4c931ec545d3e4a03a390c2c3e862c1e22e88ac3"
        },
        "date": 1792947305073,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4077.9817899087134,
            "unit": "ns/iter",
            "extra": "iterations: 171059\ncpu: 4077.3439690399214 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 69362.62462582577,
            "unit": "ns/iter",
            "extra": "iterations: 10022\ncpu: 69354.73997206146 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 85373.20500305682,
            "unit": "ns/iter",
            "extra": "iterations: 8195\ncpu: 85357.08285539964 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 223577.59615999385,
            "unit": "ns/iter",
            "extra": "iterations: 3125\ncpu: 223537.01791999995 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 498133.7473309622,
            "unit": "ns/iter",
            "extra": "iterations: 1405\ncpu: 498026.6434163699 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1444006.4041236932,
            "unit": "ns/iter",
            "extra": "iterations: 485\ncpu: 1443791.6371134014 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 50.37752715384595,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 50.36619638461539 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 52.28641646154021,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.27346984615383 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 189.6288702500044,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 189.6145800000002 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.721051800000623,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.718358120000005 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 25.68115530769214,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 25.6793362307692 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 40.815376235293435,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.80809335294124 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 2.1745084606059417,
            "unit": "ms/iter",
            "extra": "iterations: 330\ncpu: 2.1742275696969657 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 66.6788429090895,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 66.66206654545448 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 34.42128644999798,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.41521569999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 35.00997175000009,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.004180799999936 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 56.0577420000025,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.05127241666663 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 90.79877424999694,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 90.78017537499994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 265.13354433332853,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 265.1014826666665 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1218.1057199999827,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1217.7221019999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 47.044264399998305,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 47.03474760000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 49.60248978571258,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.596477071428524 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 66.28288689999522,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 66.26739170000029 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 116.48743920000015,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 116.47514819999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 340.9255605000112,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 340.86220050000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1542.8475109999908,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1542.3755780000015 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 68.75911600000109,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.7483412999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 69.71634509999944,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.70114059999979 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 100.76675199999434,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 100.7579021428572 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 184.16606499999943,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 184.12969649999945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 436.60347350001416,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 436.52842599999883 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1968.9851690000069,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1968.5520019999956 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 92.46341937500091,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 92.4458953749996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 93.96213071428454,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 93.95319728571495 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 129.59246240000084,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 129.56629100000043 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 227.10298933333206,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 227.0509219999989 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 586.7325950000009,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 586.6041099999961 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2274.751876000039,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2273.7224440000005 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "11b031270ec4d9ada43cd2032b750704aab35661",
          "message": "perf(exec): take a key filter from any join of the pipeline (#242)\n\n## Summary\n\nA left-deep pipeline's scan is probed by **every** join above it, so\neach of them has a key filter to offer it\n([ADR 0029](docs/adr/0029-dynamic-join-filters.md)'s D1) — and D1 asked\nonly the deepest. Where that one's build is\ntoo large to publish anything (past `KeyFilter::kMaxBytes`, which a\n150,000,000-row build is), the scan ran\nunfiltered past a selective build sitting one level up.\n\n`ProbeKeyOf` now walks innermost-outwards. The innermost join's key\nwhere it published a filter, which keeps every\ndecision this made before; otherwise the first join above it whose\nfilter is worth reading.\n\n- **The key's position is the hazard, answered rather than avoided.** A\nkey from a join above the innermost is a\nposition in *that* join's probe input, not in the scan, so the walk\nsubtracts each intervening join's prefix on\nthe way down. It refuses a chain it cannot map: through a join that\nemits its build rows (dropping a probe row\nthere would *add* an output row), or into the right input of one whose\noutput holds its left input's columns\nalone. A key naming some other input's column lands outside the scan's\nfields and the two bounds refuse it.\n- **A direct-layout build is read here, which D1 refuses outright.**\nWhat a rejected row saves is then the work of\nevery join below the publisher and not one cheap probe — the same\ndistinction D5 turns on. Ungated that is\n**1.005x** over the 22 (five sub-second queries losing 4–6.5% to buy one\nquery's 15%), so it is gated on the\nshare of its own key range the build covers (`JoinTable::key_span`,\nnew): under an eighth it pays.\n- **`explain --physical` names every candidate now**, innermost first\n(`key filter candidate on v or k`), since\nwhich one a run reads follows from conditions no plan can settle. Naming\nthe first alone misreported the one\n  query this item changes.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n`explain --physical` prints an extra candidate column where a scan sits\nunder several joins. No `.slt`\nexpectation, golden or ratchet changes, and no answer changes.\n\n## For the maintainer: two things I did not do myself\n\n1. **`docs/adr/0029-dynamic-join-filters.md` is a CODEOWNERS path.** The\nD6 entry is in this diff and needs your\napproval; say the word and I will take it out and hand you the text\ninstead.\n2. **The ADR's Status line is now stale and I left it alone**, because\nchanging it is an ADR status change. It\nreads `Accepted (D0-D3 and D5 built and measured; ...)`. If you want it\nupdated, the minimal edit is\n   `D0-D3, D5 and D6 built and measured`.\n\n## Verification\n\nScope established from the engine's own plans rather than from timings:\n**one query of the 22 changes**, and its\n`lineitem` scan drops 567,376,329 of 600,037,902 rows. The filtered\nscans of the other 21 are byte-identical\nbetween the two binaries, so their timing differences are noise by\nconstruction. All 43 ClickBench physical plans\nare byte-identical — this ADR's required control, answered by plan\nidentity rather than a timing run.\n\nBoth arms `pixi run static` from the same tree, arms interleaved and\nrotated, answers compared in-process:\n\n| TPC-H SF 100 | base | this | ratio |\n| --- | ---: | ---: | ---: |\n| Q09, 14 threads, best of 5 | 5.443 s | **4.617 s** | **0.848x** |\n| Q09, 4 threads, best of 7 | 11.037 s | **9.825 s** | **0.890x** |\n| the other 21 | | | plans byte-identical |\n\nSix runs over the item's development span 0.842x to 0.861x. The 4-thread\nfigure is from one commit earlier, whose\nQ09 plan is identical to the final one by the same row counts.\n\n**Eight mutation checks**, each reverting one mechanism and watching the\nintended test fail: the direct-layout\nwaiver, the key-span gate, the walk past the innermost join, the prefix\nsubtraction, the candidate list, the two\nbounds that refuse a key naming another input's column, and D2's part\ntest on a non-innermost key. The bounds are\nwhat stand between this and a wrong answer, so each has a case of its\nown.\n\n```text\n$ pixi run check\n100% tests passed out of 2915\nlint: everything passes except 38 pre-existing shellcheck SC2218 findings in tools/github/apply-settings.sh,\n      which fail identically on clean main on this host (macOS)\n\n$ pixi run release\n100% tests passed out of 2914\n\n$ ANTB1_DIFF_SEED=20261025 pixi run diff-random\nDIFF: PASS seed=20261025 queries=2000 failed=0 unsupported=0\n\n$ clang-tidy -p build/tidy --warnings-as-errors='*' <each changed file, tests too>\nphysical_planner.cc clean; hash_join_test.cc's findings are the pre-existing macOS cluster, none in the\nadded ranges\n```\n\n**Not run on this host, and CI's:** `pixi run asan` (fails at discovery\non a pre-existing `curl_global_init`\nleak) — the gate most likely to catch a lifetime mistake, though this\nchange adds no allocation or ownership;\n`pixi run ci-gcc` and `codeql-build` (linux-64 only), so GCC-only\nwarnings reach me only through CI.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (6 new cases across `hash_join_test.cc`,\n`join_table_test.cc` and\n      `plan_report_test.cc`, every one mutation-checked)\n- [x] Docs updated where behavior, commands or architecture changed (ADR\n0029's D6, docs/sql-subset.md's\n      `explain --physical` section)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer — **see the two items\nabove: the ADR 0029 entry is in this diff and needs your approval**\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Opus 5) wrote\nthe change, the tests and the ADR entry,\nran every gate and measurement above, and reviewed its own diff with the\nrepository's `reviewer` subagent —\nwhose three P1 findings are addressed here (the two uncovered bounds and\nD2's new reach each got a\nmutation-checked test; the stale `KeyFilterCandidate` reference in the\npublic header is fixed).\n- Accountable human (has read and understands the whole diff): @hor911\n\n---------\n\nCo-authored-by: Claude Opus 5 (1M context) <noreply@anthropic.com>",
          "timestamp": "2026-10-26T01:10:08+03:00",
          "tree_id": "a45a4761d2529a7efadf0d155dd52673a5dea7e7",
          "url": "https://github.com/ydb-campus/antb1/commit/11b031270ec4d9ada43cd2032b750704aab35661"
        },
        "date": 1792966906357,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4735.215229958425,
            "unit": "ns/iter",
            "extra": "iterations: 148418\ncpu: 4735.017086876254 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 28875.183743254,
            "unit": "ns/iter",
            "extra": "iterations: 24273\ncpu: 28873.09850451119 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 51773.33631062151,
            "unit": "ns/iter",
            "extra": "iterations: 13547\ncpu: 51770.601166309854 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 234762.32677810235,
            "unit": "ns/iter",
            "extra": "iterations: 3051\ncpu: 234725.66142248438 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 494633.53710248653,
            "unit": "ns/iter",
            "extra": "iterations: 1415\ncpu: 494642.01837455883 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1496167.724731119,
            "unit": "ns/iter",
            "extra": "iterations: 465\ncpu: 1496170.109677419 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.54130905882403,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.538421176470536 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.17617459999883,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.17068913333334 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 197.1993783333422,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 197.1838246666664 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.7936872978724,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.792351255319138 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 19.122985000000387,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.122446305555535 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 35.79913357894707,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 35.79387005263154 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.2178479075043667,
            "unit": "ms/iter",
            "extra": "iterations: 573\ncpu: 1.2178068307155319 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 113.58440200000548,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 113.45574483333347 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.762353870968084,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.761753064516096 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.684663714286412,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.68238421428572 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 52.36731961538838,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.357950999999815 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 78.44804988889109,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 78.43725588888894 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 264.547612333331,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 264.5157176666674 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1298.203075999993,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1298.033286999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.62108659999967,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.619189480000017 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 30.843099363636384,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 30.83428809090918 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 53.77229776922832,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.77135246153841 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 98.95743657143286,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 98.9428008571426 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 376.5146019999861,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 376.50202300000046 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1552.3780279999642,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1552.1634609999992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.0175602307665,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 56.00636676923079 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 58.466826833334075,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.46527216666667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 84.11448287499468,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.09563412500009 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 151.11002160000453,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 151.09126499999945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 417.9321865000247,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 417.8683764999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1911.256428999991,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1910.974881999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 64.91358272727543,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 64.91025545454544 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.34524089999832,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.3296194999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 97.161271142852,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 97.15260142857167 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 171.26096700000915,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 171.24146699999977 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 440.40140050000787,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 440.3095609999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2074.6535499999936,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2074.3223749999943 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "6bbb26aaeb64e275cf7190cc8dbb6d1a0a70a7b3",
          "message": "build(static): verify the package on a fresh runner and in an empty container (#243)\n\n## Summary\n\nPR 3b-1 of the static-build plan. It adds one check of \"what the shipped\ntarball must do\" that runs where pixi,\nDuckDB and the LLVM tools are absent, and from now on it runs on every\nstatic.yml leg. PR 3b-2 (GitHub releases)\nruns the same check on the release's fresh runners.\n\n- **`scripts/static_gates.py`** (standard library only):\n- **`kit --env E --out DIR`** writes the smoke kit: DuckDB writes the\nsix codec files from `range()`, plus a\ntruncated copy, and `kit.json` holds their digests and DuckDB's own\nanswer. `pixi run static` writes it to\n    `build/static/smoke-kit`.\n- **`verify`** checks a package as a user gets it. It needs Python 3.11\nor newer with tarfile's data filter, and\n    exits 2 when it cannot check the package on this runner. It checks:\n    - the digests against SHA256SUMS, and the kit against `kit.json`;\n- each tarball before anything is unpacked: one directory, exactly the\npackage's members, regular files only,\n      no member twice, no extended attributes;\n- the unpacked files: BUILDINFO.json, LICENSE and NOTICE, and on macOS\n`codesign --verify --strict`;\n- the binary gate's runs, with an empty environment, in an empty\ndirectory, with stdin from /dev/null;\n- on Linux with `--container docker|podman`, the same runs again in an\nimage imported from a root file system\nof only `/antb1` and `/kit`: `--network none --read-only --cap-drop ALL\n--security-opt no-new-privileges\n--user 65534:65534 --pull never`, never `-v` or `--mount`, and the image\nis removed afterwards.\n- **The runtime checks are now one function,** shared by the binary gate\nand verify. It adds two checks: a parse\n    error exits 1, and an unknown option exits 2.\n- **The package gate** also checks that every tarball member is\nbyte-equal to the staged file that the binary\n    gate ran.\n- **DuckDB runs with `-no-init`,** so a `~/.duckdbrc` cannot change the\nkit's answers.\n- **`pixi run static-check antb1-<target>.tar.gz`** runs verify locally.\nIt needs SHA256SUMS next to the tarball,\nand on Linux the `.debug` tarball too. It uses docker only on Linux, and\nonly when `docker info` answers. The\n  binary form is unchanged.\n- **`static.yml`:**\n- after `pixi run static`, every leg runs verify with the runner's own\n`python3 -I`, and both Linux legs also run\n    it in a Docker container;\n  - the job name now also carries the verify step's repro:\n`static <target> (pixi run static && pixi run static-check\nbuild/static/package/antb1-<target>.tar.gz && pixi\n    run static-sources)`.\n- **Docs:** docs/ci.md, ADR 0033 (in place, still Proposed),\ndocs/benchmarks.md, plus the static-check\n  description in pixi.toml and its line in CLAUDE.md.\n\nNot in this PR (3b-2): release.yml and its release script, the tag\nrulesets, the `release` environment, immutable\nreleases, SECURITY.md, the README install section and ADR 0035.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\nOn 659e67c, linux-x86_64:\n\n```text\n$ pixi run check\nlint: PASS (253 tool tests; new: tools/ci/tests/test_static_verify.py, one more case in test_static_package.py)\n100% tests passed out of 2908\n\n$ pixi run static                       # existing prefix 1a64a30529ed3973, unchanged\nstatic-gates: ok   antb1-linux-x86_64.tar.gz holds the files that were checked in build/static/package/antb1-0.1.0-linux-x86_64\nstatic-gates: ok   antb1-linux-x86_64.debug.tar.gz holds the files that were checked in build/static/package/debug/antb1-0.1.0-linux-x86_64\nstatic-gates: ok   DuckDB answers 200000,19999900000,0,999,97 on the zstd file    # and on each other codec's file\nstatic-gates: ok   a parse error exits with 1\nstatic-gates: ok   an unknown option exits with 2\n100% tests passed out of 91              # CLI goldens against the packaged binary\nstatic: OK: build/static/package\n\n$ pixi run static-check build/static/package/antb1-linux-x86_64.tar.gz\nstatic-check: note: no container run (it needs docker on Linux); the binary runs on this host only\nstatic-gates: OK                         # 65 checks\n\n$ pixi run static-check <a copy with one byte flipped>/antb1-linux-x86_64.tar.gz\nstatic-gates: FAIL antb1-linux-x86_64.tar.gz matches SHA256SUMS: another digest     # exit 1, nothing unpacked\n```\n\nThis host has no Docker, so locally the container path ran only against\nthe test fake. It has since run for real in\nthis PR's static.yml run\n([40239069946](https://github.com/ydb-campus/antb1/actions/runs/40239069946)):\n- **linux-x86_64 and linux-aarch64:** verify passed its 18 runs on the\nrunner, and the same 18 in the container.\n  Docker imported the image, and verify removed it afterwards.\n- **macos-15:** verify passed with the image's own `python3`, so that\ninterpreter is 3.11 or newer with tarfile's\ndata filter. `codesign --verify --strict` passed on the unpacked binary.\n\nTwo reviews were run on the diff:\n- **The `reviewer` subagent:** four findings, fixed in 21a454d:\n  - the \"nothing was unpacked\" assertion could not fail;\n  - the container flags were compared with the module's own constant;\n  - four error paths had no test;\n  - the job name lacked the verify step's repro.\n\n  Its other four findings were the protected paths of the approved plan.\n- **A review workflow, four lenses (safety, runners, tests, docs), each\nwith a skeptic:** 9 findings confirmed and\n  fixed in 659e67c (7 after merging duplicates), 5 refuted:\n  - the `.debug` tarball's checks had no failing tests;\n  - two runtime checks had no failing tests either;\n  - `--version` was untested;\n  - DuckDB's init file could change the kit's answers;\n  - podman's docker banner on stderr;\n  - the Python floor;\n  - the tarball form's need for the `.debug` tarball.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (tools/ci/tests/test_static_verify.py, plus\na new case in test_static_package.py)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the approved 3b plan\n(`.github/workflows/static.yml`, `pixi.toml` (one description),\n`CLAUDE.md:27`, `docs/adr/0033`)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5)\n- A read-only design investigation (five lenses, a critic and a claims\nverifier), and a Plan-agent review of the\n    plan.\n  - The change and its tests.\n- The `reviewer` subagent and a review workflow with verifiers on the\ndiff.\n- Accountable human (has read and understands the whole diff): @\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-26T02:50:23+03:00",
          "tree_id": "c17aa095f6016ce6f2f2586c9bc3f2988d8362cb",
          "url": "https://github.com/ydb-campus/antb1/commit/6bbb26aaeb64e275cf7190cc8dbb6d1a0a70a7b3"
        },
        "date": 1792972694877,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2410.201414997625,
            "unit": "ns/iter",
            "extra": "iterations: 292580\ncpu: 2410.0485781666553 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 19460.079143079227,
            "unit": "ns/iter",
            "extra": "iterations: 36036\ncpu: 19458.726551226548 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 35841.26929206741,
            "unit": "ns/iter",
            "extra": "iterations: 19451\ncpu: 35837.31268315255 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 129380.18391883613,
            "unit": "ns/iter",
            "extra": "iterations: 5323\ncpu: 129367.75164380985 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 263141.27224334254,
            "unit": "ns/iter",
            "extra": "iterations: 2630\ncpu: 263133.98098859325 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 721525.6608606837,
            "unit": "ns/iter",
            "extra": "iterations: 976\ncpu: 721470.5409836067 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 21.286854939393322,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.28041463636362 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 26.689982269230175,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 26.687524999999994 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 138.71050899999773,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 138.693782 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 8.275081809523769,
            "unit": "ms/iter",
            "extra": "iterations: 84\ncpu: 8.274401999999984 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 9.643385680556252,
            "unit": "ms/iter",
            "extra": "iterations: 72\ncpu: 9.6415408888889 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 24.772951517242102,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.769815655172412 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 0.74820278980229,
            "unit": "ms/iter",
            "extra": "iterations: 961\ncpu: 0.748083505723205 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 73.00695033333366,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 72.99618011111106 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 13.606785219999438,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.605916019999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 16.194754363636125,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 16.19281200000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 31.714083954545398,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.71164259090905 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 55.94814581818045,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 55.93436500000011 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 212.10628599999382,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 212.08194133333316 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1151.8144370000414,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1151.525627999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 16.711380952381475,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.709615928571406 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 18.754333783784055,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.750615027027063 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 34.75497066666765,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 34.751229238095235 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 67.75761709999983,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 67.7438899000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 284.63178199999106,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 284.57642050000055 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1429.1631940000116,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1428.9351960000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 32.439896095238076,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.43353761904747 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 34.91500135000081,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.91097095000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 53.28966476923387,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.27288300000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 142.67713824999362,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 142.656047 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 369.4068135000066,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 369.34271550000017 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1674.2547220000006,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1673.9380460000034 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 38.55115216666465,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.53872599999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 41.36863817647321,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 41.366080941176286 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 68.05478772727466,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 68.04493100000028 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 146.61434060000147,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 146.6051240000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 378.93889750000653,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 378.8981124999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 1788.226452999993,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1787.930523 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "eff24ce27e35018000fee62fee38829d7352f033",
          "message": "perf(io): read a narrow DECIMAL at its own width from the reader (#244)\n\n## Summary\n\n[ADR 0021](docs/adr/0021-decimal-semantics.md)'s decimal64 item made the\nengine read a DECIMAL of 18 digits or\nfewer as **decimal64**, but Arrow's Parquet reader still produced\n**decimal128** and `io` narrowed every value in\na pass of its own. That item costed removing the pass as *\"decoding\nParquet without Arrow's Parquet-to-Arrow\ntransfer on the unfiltered path too, which is a larger change than this\nitem\"*.\n\n**Arrow's own reader does it.**\n`parquet::ArrowReaderProperties::set_smallest_decimal_enabled` makes the\ntransfer\nproduce decimal64 directly, so `ConversionFor` answers `kNone` and there\nis no pass to make cheap. The option was\nnamed in that item's own plan as something to set *\"at all three\ntype-inference and read sites\"* — and was set at\nnone of them: all three took\n`parquet::default_arrow_reader_properties()`.\n\nTwo consequences, both handled:\n\n- **Nine digits or fewer arrive as decimal32**, where the engine reads\ndecimal64, so they keep a conversion — and\nnearly lost their *filtered path*, since `FilteredColumnOf` admitted\ndecimal128 and decimal64 storage alone.\nIt now admits every decimal width of the right precision and scale,\nbecause that path reads the Parquet\ncolumn's own integer and the storage type says nothing about how the\nvalue travels.\n- **A BYTE_ARRAY decimal too wide for the narrow target now fails in the\nreader**, not in io's pass, and Arrow's\ntext names the decimal width and never the column. So io names the\ncolumns the read was asked for (four plus a\ncount; `no columns` for a scan of none), and the status kind moves\n`Invalid` → `IOError`, taking the CLI's exit\ncode for such a file **from 1 to 3**. `IOError` is the kind\n`parquet_table.h` already promises for every\nParquet read failure, so the new kind is the stated contract — but it is\nvisible, and neither side was pinned\n  before, so both the kind and the message are now asserted.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\nNot marked breaking, but **one user-visible change to call out**: the\nexit code for a malformed BYTE_ARRAY\ndecimal whose bytes exceed the narrow target goes 1 → 3, as described\nabove. No answer, `.slt` expectation,\ngolden or ratchet changes.\n\n## For the maintainer\n\n`docs/adr/0021-decimal-semantics.md` is a CODEOWNERS path and this diff\nadds an entry to it. Say the word and I\nwill take it out and hand you the text instead.\n\n## Verification\n\nBoth arms `pixi run static` from the same tree, 22 queries at SF 100,\nfour threads, best of three, interleaved\nand rotated, answers compared in-process:\n\n| | base | this | ratio |\n| --- | ---: | ---: | ---: |\n| geomean of the 22 | | | **0.992x** |\n| total | 90.28 s | 89.14 s | 0.987x |\n| the three that gain | | | **0.929x**, 0.971x, 0.972x |\n| the rest | | | inside the +-1.4% noise floor; nothing slower |\n\n**It confirms ADR 0021's own attribution.** That entry listed what the\nnarrowing pass cost — five queries at\n1.018x, 1.018x, 1.013x, 1.009x and 1.008x — and removing the pass brings\nall five back (to 0.971x, 0.997x,\n0.997x, 1.002x and 0.929x). A sampling profiler predicted five queries a\nweek ago and got five.\n\n**The mechanism is verified, not assumed.** `sample` over both arms on\none decimal query shows\n`(anonymous namespace)::Convert` in **11 samples of the old arm, each\nwith an `arrow::AllocateBuffer` beneath\nit** — a fresh buffer a batch a column — against **one** of the new arm,\nin that function's own prologue with\nnothing under it, which is what a dispatcher handing the array straight\nback looks like. Peak memory does not\nmove (12,003 MiB against 11,988 on the heaviest query, max of three\neach): the pass allocated and freed per\nbatch, so it was never held.\n\n```text\n$ pixi run check\n100% tests passed out of 2916\nlint: everything passes except 38 pre-existing shellcheck SC2218 findings in\n      tools/github/apply-settings.sh, which fail identically on clean main on this host (macOS)\n\n$ pixi run release\n100% tests passed out of 2917\n\n$ ANTB1_DIFF_SEED=20261026 pixi run diff-random\nDIFF: PASS seed=20261026 queries=2000 failed=0 unsupported=0\n\n$ clang-tidy -p build/tidy --warnings-as-errors='*' <each changed file, tests too>\n0 findings on all three\n```\n\n**Not run on this host, and CI's:** `pixi run asan` (fails at discovery\non a pre-existing `curl_global_init`\nleak) — relevant here, since this changes which buffers a scan\nallocates; `pixi run ci-gcc` and `codeql-build`\n(linux-64 only).\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: the p <= 9 filtered path is\nmutation-checked against the existing\n`DecimalColumnsReadAsTheUnfilteredScanReadsThem`, and the new error path\nhas a case of its own covering\n1, 3, 4, 5 and 6 columns, the zero-column read, and the status kind\n- [x] Docs updated where behavior, commands or architecture changed (ADR\n0021's decimal64 item)\n- [x] No ClickBench-derived data is committed (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer — **the ADR 0021\n      entry needs your approval, see above**\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Opus 5) found\nthe gap by measurement (a DECIMAL column\nread at 3.25x of DuckDB where a BIGINT column of the same INT64 storage\nis at parity), made the change,\nwrote the tests and the ADR entry, ran every gate and measurement above,\nand reviewed its own diff with the\nrepository's `reviewer` subagent — whose three findings are addressed\nhere: the ADR path is flagged for\nyou, `ColumnsRead`'s zero-field branch and the cap boundary now have\ncases, and the status-kind change is\nasserted and recorded. Its exit-code numbers were wrong (1 -> 3, not 2\n-> 3) and the corrected values are\n      from `cli.h`.\n- Accountable human (has read and understands the whole diff): @hor911\n\nCo-authored-by: Claude Opus 5 (1M context) <noreply@anthropic.com>",
          "timestamp": "2026-10-26T04:06:40+03:00",
          "tree_id": "2f75d8d6f7a6f4b2be04a5436030d0e74661e929",
          "url": "https://github.com/ydb-campus/antb1/commit/eff24ce27e35018000fee62fee38829d7352f033"
        },
        "date": 1792977478439,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4648.952107939076,
            "unit": "ns/iter",
            "extra": "iterations: 151048\ncpu: 4648.446533552248 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 36374.57359962942,
            "unit": "ns/iter",
            "extra": "iterations: 19477\ncpu: 36371.370488268214 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52291.659241110334,
            "unit": "ns/iter",
            "extra": "iterations: 13467\ncpu: 52272.78881710851 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 230664.47738854805,
            "unit": "ns/iter",
            "extra": "iterations: 3140\ncpu: 230658.3302547772 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 520275.421000008,
            "unit": "ns/iter",
            "extra": "iterations: 1000\ncpu: 520236.347 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1502505.4129032176,
            "unit": "ns/iter",
            "extra": "iterations: 465\ncpu: 1502284.9806451614 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.162545055554546,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 40.15814411111111 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.528067000001705,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.51931286666668 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 176.89467750000176,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 176.87922025000003 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.81351991489351,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.811991063829756 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 19.24128408108105,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 19.238458864864835 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 35.565510499998254,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.55995879999996 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.2219594092526422,
            "unit": "ms/iter",
            "extra": "iterations: 562\ncpu: 1.2218793950177966 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 79.28672062499942,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 79.27331099999991 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.64202174193535,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.640891838709685 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.54588617241441,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.544451931034466 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 52.53903738461601,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.53702653846153 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 75.28386288889048,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 75.27453333333354 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 252.8002860000053,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 252.7806186666659 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1248.69584999999,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1248.5841010000022 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.489878730770776,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.488496615384534 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.116198347824,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 31.11363026086955 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 53.6131433846138,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.61071138461536 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 90.25392762499962,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 90.24145612500023 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 304.3228104999969,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 304.313840999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1471.1341899999866,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1470.9395829999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.11318566666531,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.1077985833333 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 58.36236691666367,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.3603914166666 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 84.68517200000036,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.66583487500002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 141.0259042000007,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 141.00892579999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 395.32732499998247,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 395.29945449999815 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1855.657387000008,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1855.5428009999987 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 64.9598964545455,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 64.95331209090887 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.62887169999681,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.61886229999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 95.61306399999532,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.60661714285702 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 159.29779049999127,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 159.2807124999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 420.39682250000965,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 420.3120315 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2012.5377599999865,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2012.3279170000003 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "848c8a0d6e60053de8673b18f7342acbc60b67ad",
          "message": "ci: publish the standalone binary as tagged github releases (#245)\n\n## Summary\n\nPR 3b-2, the last step of the static-build plan: tagged GitHub releases\nof the standalone binary. 3b-1 (#243) added\nthe fresh-runner check that this workflow runs. Decision record: [ADR\n0035](docs/adr/0035-github-releases.md),\nProposed.\n\n- **`.github/workflows/release.yml`.** A release starts when an admin\npushes an annotated tag `v<VERSION>`\n(`v<VERSION>-rc.<N>` is a prerelease, never the latest) on a green\ncommit of `main`. A manual run is a dry run,\n  which never attests, drafts or publishes.\n\n  | Job | What it does |\n  | --- | --- |\n| `preflight` | Checks the tag, the version, that the commit is on\n`main`, its newest `CI OK`, that no published release has the tag, and\nthat the `release` environment and both tag rulesets equal\n`tools/github/*.json`. |\n| `build` ×3 | Cold, like nightly `static-cold`: no cache, ccache off,\nan empty Arrow directory. Runs `pixi run static`, then `pixi run\nstatic-sources`. |\n| `assemble` | Checks every file against its checksums, BUILDINFO.json\nagainst the commit and tag, and each sources tarball against the pins\nand the commit. Writes one SHA256SUMS and the notes. The attestation's\nsubjects travel as a job output. |\n| `verify` ×4 | 3b-1's verify on fresh runners: ubuntu-24.04 and -arm\n(both also in a container), macos-15 and macos-26. |\n| `attest` | `actions/attest`, SLSA build provenance. It runs no\nrepository code and is the only job with `id-token: write`. |\n  | `draft` | Uploads everything to a draft release. |\n| `publish` | Behind the `release` environment. It trusts no artifact:\nit re-downloads every asset and checks it against the API digest, the\njob output and the draft's own bundle. Then it publishes with an\nexplicit `make_latest` and requires an immutable release that `gh\nrelease verify` accepts. |\n| `check-published` ×3 | Runs README.md's install block as a user would,\nthen the documented verify commands. |\n\n  There are 9 assets, about 217 MB, with no version in their names:\n- the three packages, the two `.debug` tarballs and the two Linux\nsources tarballs;\n  - `SHA256SUMS`;\n  - `antb1.sigstore.json`, the provenance bundle.\n- **`.github/scripts/release.py`.** The workflow's logic: standard\nlibrary only, run with each runner's own\n`python3 -I`. Every GitHub call goes through `gh`, and the script fails\nclosed on any unexpected response.\n- **Settings as code** in `tools/github/`, applied with\n`apply-settings.sh --release` (a dry run unless `--apply`; the\n  read-back exits 1 on any difference):\n  - `ruleset-tags.json`: only admins create, move or delete a tag;\n- `ruleset-release-tags.json`: `v*` tags never move and are never\ndeleted (no bypass for anyone), and creating one\n    needs a green `CI OK`;\n- `environment-release.json`: both maintainers are reviewers, and the\nmaintainer who pushed the tag may approve\nit (self-approval). Admins cannot bypass it, and it accepts deployments\nfrom `v*` tags only;\n  - immutable releases turned on.\n- **Also in this PR:**\n- BUILDINFO.json records the tag on tag runs, so an rc package names its\nrelease;\n- `copilot-setup-steps.yml` no longer starts on tag pushes (path filters\ndo not apply to tags);\n- agents never release: AGENTS.md golden rule 4, and\n`.claude/settings.json` denies tag pushes,\n`gh release create`, `upload`, `edit` and `delete`, `gh workflow run\nrelease.yml` and deployment approvals;\n- docs: docs/ci.md#releases, README.md#install,\nSECURITY.md#verifying-a-release, CONTRIBUTING.md,\ndocs/benchmarks.md, docs/agents.md, the fix-ci-failure recipe and skill,\nthe bug form, and pointers in ADRs\n    0007, 0009, 0033 and 0034.\n\n## Before merging (maintainer)\n\nThe settings must be live before `release.yml` is on `main`. A workflow\nthat names a missing environment creates it\nwithout protection, and immutability covers only releases published\nafter it is turned on.\n\n1. `bash tools/github/apply-settings.sh --release`: a dry run that\nprints the five planned writes.\n2. `bash tools/github/apply-settings.sh --apply --release`, then paste\nthe read-back here. It must show `OK` five times\n   and no `DIFF`.\n3. Merge.\n\n## After merging (maintainer; each step is yours)\n\n1. CI OK goes green on the merge commit.\n2. `gh workflow run release.yml --ref main` (the dry run) passes.\n3. The LGPL review of ADR 0034, by an open-source office or a lawyer,\nbefore the first public release.\n4. `git tag -a v0.1.0-rc.1 -m \"...\" && git push origin v0.1.0-rc.1`,\npushed on its own. Check the draft, approve the\n   `release` environment, and wait for check-published to pass. Then\n`gh api -X GET repos/ydb-campus/antb1/rulesets/rule-suites -f\nref=refs/tags/v0.1.0-rc.1` shows how the CI OK rule\n   treated an annotated tag.\n5. `v0.1.0`, the same way.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\nOn the final tree (main 6bbb26a plus this PR), linux-x86_64:\n\n```text\n$ pixi run check\nlint: PASS (384 tool tests; new: test_release.py, test_release_workflow.py, test_github_settings.py,\n  test_release_docs.py, and more in test_static_package.py)\n100% tests passed out of 2915\n\n$ pixi run static                      # existing prefix 1a64a30529ed3973, unchanged\nstatic: OK: build/static/package       # BUILDINFO.json written by static_package.py buildinfo\n# Before that refactor, a tag run (GITHUB_REF_TYPE=tag GITHUB_REF_NAME=v0.1.0-rc.1 pixi run static) recorded\n# \"tag\": \"v0.1.0-rc.1\"; the helper's tag logic now has its own tests.\n\n$ gh run download 40246545600 -R ydb-campus/antb1 -p 'antb1-*' -D artifacts   # main's static run of 6bbb26a\n$ GITHUB_REPOSITORY=ydb-campus/antb1 RELEASE_TAG=v0.1.0 RELEASE_SHA=6bbb26aa... RELEASE_VERSION=0.1.0 \\\n  RELEASE_PRERELEASE=false RELEASE_PUBLISH=false \\\n  pixi run -e lint python -I .github/scripts/release.py assemble --in artifacts --out meta\nrelease: checked 7 tarballs; wrote SHA256SUMS and notes.md\n# all 7 tarballs against their checksums, BUILDINFO.json against the commit and pixi.lock, every member of both\n# sources tarballs against the pins, antb1-source.tar.gz against the commit\n```\n\nNothing here has touched real GitHub releases, attestations, the\nenvironment or the rulesets. The post-merge dry\nrun checks preflight's reads (the environment, the rulesets, ci.yml's\nruns) on the real API, and the\n`v0.1.0-rc.1` rehearsal checks the rest.\n\nTwo reviews were run on the diff, and their findings are fixed in the\nthird commit:\n\n- **The `reviewer` subagent:** five P1 findings, all fixed:\n- a refspec tag push (`git push origin HEAD:refs/tags/v…`) slipped past\nthe deny rules; `git push *:refs/tags/*`\n    now catches it;\n- check-published's `latest` run did not compare its download with this\nrelease's asset;\n  - docs/ci.md still said BUILDINFO.json records no tag;\n- check-published and the release-attestation check had no tests that\nmake them refuse;\n- the BUILDINFO `tag` field had no test. BUILDINFO.json now comes from\n`static_package.py buildinfo`, which the\n    tests cover.\n- **A review workflow with four lenses, each followed by a skeptic:** 13\nfindings confirmed and 3 refuted. Among the\n  fixes:\n- **CI OK could be forged.** Any workflow's token can create a check run\nnamed `CI OK`, and GitHub may file it in\nci.yml's check suite. preflight now reads ci.yml's own runs and their\n`CI OK` job through the Actions API and\nnever reads check runs. preflight's `checks: read` permission is\ndropped.\n- publish publishes first and makes the release the latest one only\nafter it is immutable and its attestation\nchecks out, so a failure never leaves an unverified release behind\n`releases/latest`.\n  - Runbook fixes:\n- the 30-day window: after a timeout the run can't be re-run, and the\ntag is spent;\n    - the rule-suite check needs two calls;\n- the recovery steps now tell a mutable failed release from an immutable\none;\n    - if publish's draft is gone, re-run the draft job.\n  - More tests:\n- the settings files are pinned (enforcement, conditions, the reviewer\nids);\n    - every difference in the environment and rulesets has a test;\n- every assemble check of the sources tarballs and BUILDINFO has a test;\n- the workflow's env and output wiring is checked against the script;\n    - guards ensure no test can reach the host's real gh, curl or uname.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change (tools/ci/tests: test_release.py,\ntest_release_workflow.py, test_github_settings.py,\n  test_release_docs.py, fake_gh.py)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the approved 3b plan\n(`.github/workflows/release.yml`, `.github/scripts/release.py`,\n`.github/workflows/copilot-setup-steps.yml`,\n`.github/ISSUE_TEMPLATE/bug.yml`, `tools/github/*`,\n`.claude/settings.json`, `CLAUDE.md`, `AGENTS.md:16`,\n`docs/adr/0035` (new), `0033`, `0034`, `0007`, `0009`, the ADR index,\nthe fix-ci-failure skill)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5)\n- A design investigation (five lenses, a critic, a verifier of 44\nclaims), and a Plan-agent review of the plan.\n  - The workflow, the release script, the settings and the tests.\n  - The docs, drafted by a workflow and cross-checked by a critic.\n- The `reviewer` subagent and a review workflow with verifiers on the\ndiff.\n- Accountable human (has read and understands the whole diff): @\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-27T01:58:30+03:00",
          "tree_id": "b724b08a8647cd68f05bbc55e0b5f405f41b645d",
          "url": "https://github.com/ydb-campus/antb1/commit/848c8a0d6e60053de8673b18f7342acbc60b67ad"
        },
        "date": 1793056059540,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3549.247606268572,
            "unit": "ns/iter",
            "extra": "iterations: 197495\ncpu: 3548.722853743132 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 25278.918340802178,
            "unit": "ns/iter",
            "extra": "iterations: 27676\ncpu: 25272.535771065188 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 46330.699741839955,
            "unit": "ns/iter",
            "extra": "iterations: 15107\ncpu: 46311.01734295361 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 191064.88981906115,
            "unit": "ns/iter",
            "extra": "iterations: 3703\ncpu: 191018.66594652992 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 380605.7771739021,
            "unit": "ns/iter",
            "extra": "iterations: 1840\ncpu: 380539.5353260873 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1172367.8207704744,
            "unit": "ns/iter",
            "extra": "iterations: 597\ncpu: 1172153.6264656608 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 33.406768238096774,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.39752542857145 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 38.37943944444393,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.36964294444442 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 194.20893350000767,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 194.16714574999983 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.545781066665958,
            "unit": "ms/iter",
            "extra": "iterations: 60\ncpu: 11.54395793333333 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 14.782668270833929,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.779111166666674 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 31.483945181817447,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.479069681818167 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 0.9725024937413496,
            "unit": "ms/iter",
            "extra": "iterations: 719\ncpu: 0.972316842837273 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 67.7402685555598,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 67.73153977777785 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 19.71361639999876,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.710353914285708 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 21.445646939393253,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.443982878787846 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 45.04661643749941,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 45.041646124999836 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 74.81325199999844,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 74.80907525000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 256.31421366667456,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 256.2694073333335 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1283.913288000008,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1283.7679100000016 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 23.580935466666613,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.577846900000072 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 26.541948407407713,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.53784322222221 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 46.49679513333164,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.49197693333349 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 97.10485257142766,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 97.08459857142871 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 296.634355000009,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 296.59976249999966 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1454.8821599999542,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1454.6733029999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 43.30001674999906,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.294710812499915 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 44.9814945,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 44.978364062500106 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 68.15499180000302,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.14423619999985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 136.55383500000653,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 136.5280826000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 371.0909959999924,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 371.0251145000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1759.74461200002,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1759.430897999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 51.325905785712884,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 51.31742300000042 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 53.903915846153545,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.89485092307704 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 80.1572703333338,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 80.1478888888888 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 150.74282780000203,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 150.71570100000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 392.20772150000016,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 392.14471749999905 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 1888.1811080000261,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1887.8294189999992 ms\nthreads: 1"
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
          "id": "b452b0bac11e4c56c5b41c008ed275c6e0072541",
          "message": "build(deps): update pixi.lock (#246)\n\n# Dependencies\n\n<details>\n<summary>Explicit dependencies</summary>\n\n|Dependency|Before|After|Change|Environments|\n|-|-|-|-|-|\n\n|[nodejs](https://prefix.dev/channels/conda-forge/packages/nodejs)|26.10.0|26.12.1|Minor\nUpgrade|lint on *all platforms*|\n\n|[clang](https://prefix.dev/channels/conda-forge/packages/clang)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[clang-format](https://prefix.dev/channels/conda-forge/packages/clang-format)|23.1.3|23.1.4|Patch\nUpgrade|lint on *all platforms*|\n\n|[clang-tools](https://prefix.dev/channels/conda-forge/packages/clang-tools)|23.1.3|23.1.4|Patch\nUpgrade|default on *all platforms*|\n\n|[clangxx](https://prefix.dev/channels/conda-forge/packages/clangxx)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[compiler-rt](https://prefix.dev/channels/conda-forge/packages/compiler-rt)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[lld](https://prefix.dev/channels/conda-forge/packages/lld)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[llvm-tools](https://prefix.dev/channels/conda-forge/packages/llvm-tools)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[python](https://prefix.dev/channels/conda-forge/packages/python)|3.14.8|3.14.9|Patch\nUpgrade|{default, lint, static} on *all platforms*|\n\n|[shellcheck](https://prefix.dev/channels/conda-forge/packages/shellcheck)|hecfb573_2|hff6ef18_2|Only\nbuild string|lint on osx-arm64|\n\n|[shellcheck](https://prefix.dev/channels/conda-forge/packages/shellcheck)|ha770c72_2|hfc2019e_2|Only\nbuild string|lint on linux-64|\n\n|[shellcheck](https://prefix.dev/channels/conda-forge/packages/shellcheck)|h8af1aa0_2|h22914b5_2|Only\nbuild string|lint on linux-aarch64|\n\n</details>\n\n<details>\n<summary>Implicit dependencies</summary>\n\n|Dependency|Before|After|Change|Environments|\n|-|-|-|-|-|\n\n|[tzdata](https://prefix.dev/channels/conda-forge/packages/tzdata)|2026c|2026e|Major\nUpgrade|{default, lint, static} on *all platforms*<br/>gcc on linux-64|\n\n|[nodeenv](https://prefix.dev/channels/conda-forge/packages/nodeenv)|1.10.0|1.11.0|Minor\nUpgrade|lint on *all platforms*|\n\n|[pycparser](https://prefix.dev/channels/conda-forge/packages/pycparser)|3.11|3.12|Minor\nUpgrade|lint on *all platforms*|\n\n|[setuptools](https://prefix.dev/channels/conda-forge/packages/setuptools)|84.0.0|84.2.1|Minor\nUpgrade|lint on *all platforms*|\n\n|[c-ares](https://prefix.dev/channels/conda-forge/packages/c-ares)|1.34.8|1.34.9|Patch\nUpgrade|{default, lint, static} on *all platforms*<br/>gcc on linux-64|\n\n|[ca-certificates](https://prefix.dev/channels/conda-forge/packages/ca-certificates)|2026.10.18|2026.10.23|Patch\nUpgrade|{default, lint, static} on *all platforms*<br/>gcc on linux-64|\n\n|[clang-23](https://prefix.dev/channels/conda-forge/packages/clang-23)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[clang-format](https://prefix.dev/channels/conda-forge/packages/clang-format)|23.1.3|23.1.4|Patch\nUpgrade|default on *all platforms*|\n\n|[clang-format-23](https://prefix.dev/channels/conda-forge/packages/clang-format-23)|23.1.3|23.1.4|Patch\nUpgrade|{default, lint} on *all platforms*|\n\n|[clang-scan-deps](https://prefix.dev/channels/conda-forge/packages/clang-scan-deps)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[clang_impl_linux-64](https://prefix.dev/channels/conda-forge/packages/clang_impl_linux-64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-64|\n\n|[clang_impl_linux-aarch64](https://prefix.dev/channels/conda-forge/packages/clang_impl_linux-aarch64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-aarch64|\n\n|[clang_impl_osx-arm64](https://prefix.dev/channels/conda-forge/packages/clang_impl_osx-arm64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on osx-arm64|\n\n|[clangxx_impl_linux-64](https://prefix.dev/channels/conda-forge/packages/clangxx_impl_linux-64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-64|\n\n|[clangxx_impl_linux-aarch64](https://prefix.dev/channels/conda-forge/packages/clangxx_impl_linux-aarch64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-aarch64|\n\n|[clangxx_impl_osx-arm64](https://prefix.dev/channels/conda-forge/packages/clangxx_impl_osx-arm64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on osx-arm64|\n\n|[compiler-rt23](https://prefix.dev/channels/conda-forge/packages/compiler-rt23)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[compiler-rt23_linux-64](https://prefix.dev/channels/conda-forge/packages/compiler-rt23_linux-64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-64|\n\n|[compiler-rt23_linux-aarch64](https://prefix.dev/channels/conda-forge/packages/compiler-rt23_linux-aarch64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-aarch64|\n\n|[compiler-rt23_osx-arm64](https://prefix.dev/channels/conda-forge/packages/compiler-rt23_osx-arm64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on osx-arm64|\n\n|[compiler-rt_linux-64](https://prefix.dev/channels/conda-forge/packages/compiler-rt_linux-64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-64|\n\n|[compiler-rt_linux-aarch64](https://prefix.dev/channels/conda-forge/packages/compiler-rt_linux-aarch64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on linux-aarch64|\n\n|[compiler-rt_osx-arm64](https://prefix.dev/channels/conda-forge/packages/compiler-rt_osx-arm64)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on osx-arm64|\n\n|[libbrotlicommon](https://prefix.dev/channels/conda-forge/packages/libbrotlicommon)|1.2.0|1.2.1|Patch\nUpgrade|{default, lint} on *all platforms*<br/>gcc on linux-64|\n\n|[libbrotlidec](https://prefix.dev/channels/conda-forge/packages/libbrotlidec)|1.2.0|1.2.1|Patch\nUpgrade|{default, lint} on *all platforms*<br/>gcc on linux-64|\n\n|[libbrotlienc](https://prefix.dev/channels/conda-forge/packages/libbrotlienc)|1.2.0|1.2.1|Patch\nUpgrade|{default, lint} on *all platforms*<br/>gcc on linux-64|\n\n|[libclang-cpp23.1](https://prefix.dev/channels/conda-forge/packages/libclang-cpp23.1)|23.1.3|23.1.4|Patch\nUpgrade|{default, lint, static} on *all platforms*|\n\n|[libclang13](https://prefix.dev/channels/conda-forge/packages/libclang13)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[libcompiler-rt](https://prefix.dev/channels/conda-forge/packages/libcompiler-rt)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[libcxx](https://prefix.dev/channels/conda-forge/packages/libcxx)|23.1.3|23.1.4|Patch\nUpgrade|{default, lint, static} on osx-arm64|\n\n|[libcxx-devel](https://prefix.dev/channels/conda-forge/packages/libcxx-devel)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on osx-arm64|\n\n|[libcxx-headers](https://prefix.dev/channels/conda-forge/packages/libcxx-headers)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on osx-arm64|\n\n|[libllvm23](https://prefix.dev/channels/conda-forge/packages/libllvm23)|23.1.3|23.1.4|Patch\nUpgrade|{default, lint, static} on *all platforms*|\n\n|[libpython](https://prefix.dev/channels/conda-forge/packages/libpython)|3.14.8|3.14.9|Patch\nUpgrade|{default, lint, static} on *all platforms*|\n\n|[llvm-openmp](https://prefix.dev/channels/conda-forge/packages/llvm-openmp)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[llvm-tools-23](https://prefix.dev/channels/conda-forge/packages/llvm-tools-23)|23.1.3|23.1.4|Patch\nUpgrade|{default, static} on *all platforms*|\n\n|[platformdirs](https://prefix.dev/channels/conda-forge/packages/platformdirs)|4.13.4|4.13.5|Patch\nUpgrade|lint on *all platforms*|\n\n|[python-discovery](https://prefix.dev/channels/conda-forge/packages/python-discovery)|1.6.3|1.6.4|Patch\nUpgrade|lint on *all platforms*|\n\n|[virtualenv](https://prefix.dev/channels/conda-forge/packages/virtualenv)|21.14.11|21.14.13|Patch\nUpgrade|lint on *all platforms*|\n\n|[libre2-11](https://prefix.dev/channels/conda-forge/packages/libre2-11)|h149037f_2|hf13fea9_2|Only\nbuild string|default on linux-aarch64|\n\n|[libre2-11](https://prefix.dev/channels/conda-forge/packages/libre2-11)|h5d15848_2|hc0bb115_2|Only\nbuild string|default on osx-arm64|\n\n|[libre2-11](https://prefix.dev/channels/conda-forge/packages/libre2-11)|h60473fc_2|hbb87c76_2|Only\nbuild string|{default, gcc} on linux-64|\n\n|[re2](https://prefix.dev/channels/conda-forge/packages/re2)|hfb2350b_2|he78ab50_2|Only\nbuild string|default on linux-aarch64|\n\n|[re2](https://prefix.dev/channels/conda-forge/packages/re2)|h94463f1_2|hbf548d4_2|Only\nbuild string|{default, gcc} on linux-64|\n\n|[re2](https://prefix.dev/channels/conda-forge/packages/re2)|h5d60da5_2|hb8e9f31_2|Only\nbuild string|default on osx-arm64|\n\n</details>\n\n[^1]: **Bold** means explicit dependency.\n[^2]: Dependency got downgraded.\n\n\nGenerated by `.github/workflows/pixi-lock-update.yml` with pixi 0.81.0.\nCI runs as for any PR;\na maintainer reviews the diff, approves and squash-merges.\n\nCo-authored-by: antb1-bot[bot] <334225516+antb1-bot[bot]@users.noreply.github.com>",
          "timestamp": "2026-10-28T12:40:44+03:00",
          "tree_id": "7de9aa877a9b5a0efd0dcaf4f50cd2e756a9a9ed",
          "url": "https://github.com/ydb-campus/antb1/commit/b452b0bac11e4c56c5b41c008ed275c6e0072541"
        },
        "date": 1793181568401,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3507.155578658925,
            "unit": "ns/iter",
            "extra": "iterations: 200066\ncpu: 3506.88291863685 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 25433.189386806665,
            "unit": "ns/iter",
            "extra": "iterations: 27626\ncpu: 25431.693115181348 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 46454.71172606072,
            "unit": "ns/iter",
            "extra": "iterations: 15069\ncpu: 46445.959121375025 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 187097.54890433387,
            "unit": "ns/iter",
            "extra": "iterations: 3742\ncpu: 187091.43666488517 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 373567.62146288296,
            "unit": "ns/iter",
            "extra": "iterations: 1873\ncpu: 373510.31393486384 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1183291.766891812,
            "unit": "ns/iter",
            "extra": "iterations: 592\ncpu: 1183272.2297297302 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 33.07319733332969,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.05528480952379 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 38.92246322221935,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.91127050000002 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 195.2092946666729,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 195.18116000000006 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.458243032786756,
            "unit": "ms/iter",
            "extra": "iterations: 61\ncpu: 11.457837737704917 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 14.445225520833086,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.443212416666647 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 31.12061891304462,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 31.12024826086954 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.0002101025640948,
            "unit": "ms/iter",
            "extra": "iterations: 702\ncpu: 1.0001214330484347 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 77.5219385555551,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 77.51465777777788 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 19.72693648571229,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.726030771428576 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 21.45018763636389,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.44778845454545 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 44.7193001250028,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 44.71333237499997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 75.30491244444369,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 75.29240166666658 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 264.93793333334753,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 264.91393633333377 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1295.4590450000296,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1295.3240940000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 23.54925169999736,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.546532999999908 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 26.166455777776584,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.16609007407407 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 47.008986800005914,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 47.00232940000007 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 92.06633062500202,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 92.06409800000026 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 325.42343300002585,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 325.3630824999991 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1533.6244999999735,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1533.3910270000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 43.24287793750159,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.239557062500154 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 45.657663200002695,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.655303533333345 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 69.82941790000723,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.82098410000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 135.8399282000164,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 135.83741539999963 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 386.89856249999366,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 386.8728934999979 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1810.3412930000786,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1810.091502000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 51.1384870000029,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 51.131816785714186 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 53.94439761537866,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.93738869230775 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 85.20137849998832,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.19467362499978 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 163.55672599999593,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 163.528950249999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 420.76944900003355,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 420.73009500000325 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 1968.8232459999426,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1968.5511610000005 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "49699333+dependabot[bot]@users.noreply.github.com",
            "name": "dependabot[bot]",
            "username": "dependabot[bot]"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "f20bf8ba104f20082d77ff451949ff4e9f4d10b2",
          "message": "ci(deps): bump the github-actions group across 1 directory with 6 updates (#247)\n\nBumps the github-actions group with 6 updates in the / directory:\n\n| Package | From | To |\n| --- | --- | --- |\n| [prefix-dev/setup-pixi](https://github.com/prefix-dev/setup-pixi) |\n`0.11.0` | `0.11.2` |\n|\n[anthropics/claude-code-action](https://github.com/anthropics/claude-code-action)\n| `1.0.248` | `1.0.257` |\n| [github/codeql-action/init](https://github.com/github/codeql-action) |\n`4.38.3` | `4.39.1` |\n|\n[github/codeql-action/analyze](https://github.com/github/codeql-action)\n| `4.38.3` | `4.39.1` |\n|\n[github/codeql-action/upload-sarif](https://github.com/github/codeql-action)\n| `4.38.3` | `4.39.1` |\n|\n[zizmorcore/zizmor-action](https://github.com/zizmorcore/zizmor-action)\n| `0.6.4` | `0.7.1` |\n\n\nUpdates `prefix-dev/setup-pixi` from 0.11.0 to 0.11.2\n<details>\n<summary>Release notes</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/prefix-dev/setup-pixi/releases\">prefix-dev/setup-pixi's\nreleases</a>.</em></p>\n<blockquote>\n<h2>v0.11.2</h2>\n<!-- raw HTML omitted -->\n<h2>What's Changed</h2>\n<h3>🤷🏻 Other changes</h3>\n<ul>\n<li>docs: Adjust workload-identity-audience section by <a\nhref=\"https://github.com/pavelzw\"><code>@​pavelzw</code></a> in <a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/pull/293\">prefix-dev/setup-pixi#293</a></li>\n<li>fix: Don't delete preinstalled pixi bin in cleanup by <a\nhref=\"https://github.com/pavelzw\"><code>@​pavelzw</code></a> in <a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/pull/297\">prefix-dev/setup-pixi#297</a></li>\n<li>fix(deps): bump undici, brace-expansion and handlebars by <a\nhref=\"https://github.com/wolfv\"><code>@​wolfv</code></a> in <a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/pull/295\">prefix-dev/setup-pixi#295</a></li>\n</ul>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/prefix-dev/setup-pixi/compare/v0.11.1...v0.11.2\">https://github.com/prefix-dev/setup-pixi/compare/v0.11.1...v0.11.2</a></p>\n<h2>v0.11.1</h2>\n<!-- raw HTML omitted -->\n<h2>What's Changed</h2>\n<h3>🤷🏻 Other changes</h3>\n<ul>\n<li>feat: Add auth-workload-identity inputs for OIDC login by <a\nhref=\"https://github.com/pavelzw\"><code>@​pavelzw</code></a> in <a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/pull/292\">prefix-dev/setup-pixi#292</a></li>\n</ul>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/prefix-dev/setup-pixi/compare/v0.11.0...v0.11.1\">https://github.com/prefix-dev/setup-pixi/compare/v0.11.0...v0.11.1</a></p>\n</blockquote>\n</details>\n<details>\n<summary>Commits</summary>\n<ul>\n<li><a\nhref=\"https://github.com/prefix-dev/setup-pixi/commit/ad1da6449d9e506fd127fd67ab2d054a769f6a94\"><code>ad1da64</code></a>\nfix(deps): bump undici, brace-expansion and handlebars (<a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/issues/295\">#295</a>)</li>\n<li><a\nhref=\"https://github.com/prefix-dev/setup-pixi/commit/05a7f7a5dd4ee746356507c0c52ada1463153ab9\"><code>05a7f7a</code></a>\nfix: Don't delete preinstalled pixi bin in cleanup (<a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/issues/297\">#297</a>)</li>\n<li><a\nhref=\"https://github.com/prefix-dev/setup-pixi/commit/f549d4661561febfea24180549434adf6853645f\"><code>f549d46</code></a>\nci: use pixi exec instead of choco to install yq (<a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/issues/294\">#294</a>)</li>\n<li><a\nhref=\"https://github.com/prefix-dev/setup-pixi/commit/4578022571fafe8be0488f5607781571b0b85c8f\"><code>4578022</code></a>\ndocs: Adjust workload-identity-audience section (<a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/issues/293\">#293</a>)</li>\n<li><a\nhref=\"https://github.com/prefix-dev/setup-pixi/commit/a2789fe3805b9ae22d6c25a15df2f2c624cd092e\"><code>a2789fe</code></a>\nfeat: Add auth-workload-identity inputs for OIDC login (<a\nhref=\"https://redirect.github.com/prefix-dev/setup-pixi/issues/292\">#292</a>)</li>\n<li>See full diff in <a\nhref=\"https://github.com/prefix-dev/setup-pixi/compare/9dabb60412d3d2d967a8d682f7ce01317af40514...ad1da6449d9e506fd127fd67ab2d054a769f6a94\">compare\nview</a></li>\n</ul>\n</details>\n<br />\n\nUpdates `anthropics/claude-code-action` from 1.0.248 to 1.0.257\n<details>\n<summary>Release notes</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/anthropics/claude-code-action/releases\">anthropics/claude-code-action's\nreleases</a>.</em></p>\n<blockquote>\n<h2>v1.0.257</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.256...v1.0.257\">https://github.com/anthropics/claude-code-action/compare/v1.0.256...v1.0.257</a></p>\n<h2>v1.0.256</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.255...v1.0.256\">https://github.com/anthropics/claude-code-action/compare/v1.0.255...v1.0.256</a></p>\n<h2>v1.0.255</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.254...v1.0.255\">https://github.com/anthropics/claude-code-action/compare/v1.0.254...v1.0.255</a></p>\n<h2>v1.0.254</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.253...v1.0.254\">https://github.com/anthropics/claude-code-action/compare/v1.0.253...v1.0.254</a></p>\n<h2>v1.0.253</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.252...v1.0.253\">https://github.com/anthropics/claude-code-action/compare/v1.0.252...v1.0.253</a></p>\n<h2>v1.0.252</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.251...v1.0.252\">https://github.com/anthropics/claude-code-action/compare/v1.0.251...v1.0.252</a></p>\n<h2>v1.0.251</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.250...v1.0.251\">https://github.com/anthropics/claude-code-action/compare/v1.0.250...v1.0.251</a></p>\n<h2>v1.0.250</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.249...v1.0.250\">https://github.com/anthropics/claude-code-action/compare/v1.0.249...v1.0.250</a></p>\n<h2>v1.0.249</h2>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/v1.0.248...v1.0.249\">https://github.com/anthropics/claude-code-action/compare/v1.0.248...v1.0.249</a></p>\n</blockquote>\n</details>\n<details>\n<summary>Commits</summary>\n<ul>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/aa8a7738e48bf30a40ee499ce818bef081c5ab5e\"><code>aa8a773</code></a>\nchore: bump Claude Code to 2.1.307 and Agent SDK to 0.3.307</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/a5a423f75c251dcbff8a74e99f83b2cb03c72bfa\"><code>a5a423f</code></a>\nchore: bump Claude Code to 2.1.306 and Agent SDK to 0.3.306</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/e4a6faad497d33dbc7144135c75e41f82f0bf6c0\"><code>e4a6faa</code></a>\nchore: bump Claude Code to 2.1.305 and Agent SDK to 0.3.305</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/db77d4d3c98f3f6380e7cd45ffba2342a3dbde3c\"><code>db77d4d</code></a>\nchore: bump Claude Code to 2.1.304 and Agent SDK to 0.3.304</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/5eb090f3c3afd701a0672cd32981197ca27688f6\"><code>5eb090f</code></a>\nchore: bump Claude Code to 2.1.303 and Agent SDK to 0.3.303</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/f27be6b65a4652f08a2ba3cff79114829b588967\"><code>f27be6b</code></a>\nchore: bump Claude Code to 2.1.302 and Agent SDK to 0.3.302</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/374f254f5efaf968abfe15d399fd90972e8138b9\"><code>374f254</code></a>\nchore: bump Claude Code to 2.1.301 and Agent SDK to 0.3.301</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/a26b07d8c778b0c43f64618e5fd29fd0f8af72e8\"><code>a26b07d</code></a>\nchore: bump Claude Code to 2.1.300 and Agent SDK to 0.3.300</li>\n<li><a\nhref=\"https://github.com/anthropics/claude-code-action/commit/34968b5981d5d170904281aa3df6f3c0d4a28855\"><code>34968b5</code></a>\nchore: bump Claude Code to 2.1.299 and Agent SDK to 0.3.299</li>\n<li>See full diff in <a\nhref=\"https://github.com/anthropics/claude-code-action/compare/1d6de8cb0c237e7c15e9e1bdf973826ebae490cc...aa8a7738e48bf30a40ee499ce818bef081c5ab5e\">compare\nview</a></li>\n</ul>\n</details>\n<br />\n\nUpdates `github/codeql-action/init` from 4.38.3 to 4.39.1\n<details>\n<summary>Release notes</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/github/codeql-action/releases\">github/codeql-action/init's\nreleases</a>.</em></p>\n<blockquote>\n<h2>v4.39.1</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.3\">2.27.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4256\">#4256</a></li>\n</ul>\n<h2>v4.39.0</h2>\n<ul>\n<li><em>Breaking change</em>: Bump the minimum required CodeQL bundle\nversion to 2.20.7. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4205\">#4205</a></li>\n<li>The <code>codeql-action/analyze</code> step now validates that a\ncorrect argument for the <code>checkout_path</code> input has been\nprovided. The <code>checkout_path</code> input must be set to the path\nwhere the repository that is being analyzed is checked out at on the\nrunner, if that path is something other than the workspace root. If the\npath does not point to a repository checkout or is different from the\npath that was used by <code>codeql-action/init</code>, then the CodeQL\nAction now produces a warning. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4148\">#4148</a></li>\n<li>Resolved an issue with third-party caching implementations that\nprevented caches from being restored correctly. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4206\">#4206</a></li>\n<li>The <code>codeql-action/start-proxy</code> step that is used in Code\nScanning Default Setup now finds CodeQL releases using the same logic as\n<code>codeql-action/init</code>. This primarily benefits customers who\nare using GitHub instances other than <code>github.com</code> where the\n<code>codeql-action/start-proxy</code> step may previously have been\nunable to locate recent releases and ended up using outdated versions of\nthe private registry authentication proxy as a result. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4198\">#4198</a></li>\n</ul>\n</blockquote>\n</details>\n<details>\n<summary>Changelog</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/github/codeql-action/blob/main/CHANGELOG.md\">github/codeql-action/init's\nchangelog</a>.</em></p>\n<blockquote>\n<h1>CodeQL Action Changelog</h1>\n<p>See the <a\nhref=\"https://github.com/github/codeql-action/releases\">releases\npage</a> for the relevant changes to the CodeQL CLI and language\npacks.</p>\n<h2>[UNRELEASED]</h2>\n<p>No user facing changes.</p>\n<h2>4.39.1 - 21 Oct 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.3\">2.27.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4256\">#4256</a></li>\n</ul>\n<h2>4.39.0 - 15 Oct 2026</h2>\n<ul>\n<li><em>Breaking change</em>: Bump the minimum required CodeQL bundle\nversion to 2.20.7. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4205\">#4205</a></li>\n<li>The <code>codeql-action/analyze</code> step now validates that a\ncorrect argument for the <code>checkout_path</code> input has been\nprovided. The <code>checkout_path</code> input must be set to the path\nwhere the repository that is being analyzed is checked out at on the\nrunner, if that path is something other than the workspace root. If the\npath does not point to a repository checkout or is different from the\npath that was used by <code>codeql-action/init</code>, then the CodeQL\nAction now produces a warning. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4148\">#4148</a></li>\n<li>Resolved an issue with third-party caching implementations that\nprevented caches from being restored correctly. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4206\">#4206</a></li>\n<li>The <code>codeql-action/start-proxy</code> step that is used in Code\nScanning Default Setup now finds CodeQL releases using the same logic as\n<code>codeql-action/init</code>. This primarily benefits customers who\nare using GitHub instances other than <code>github.com</code> where the\n<code>codeql-action/start-proxy</code> step may previously have been\nunable to locate recent releases and ended up using outdated versions of\nthe private registry authentication proxy as a result. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4198\">#4198</a></li>\n</ul>\n<h2>4.38.3 - 08 Oct 2026</h2>\n<ul>\n<li><em>Upcoming breaking change</em>: CodeQL version 2.21.2 and earlier\nwere discontinued on 24 September 2026 alongside GitHub Enterprise\nServer 3.17, and will be unsupported by the next minor release of the\nCodeQL Action. Added a deprecation warning for customers using these\nversions of CodeQL. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4188\">#4188</a></li>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.2\">2.27.2</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4203\">#4203</a></li>\n<li>Fixed a bug where the decision of whether to use a per-language\nbundle did not account for custom configurations that reference queries\noutside of compiled CodeQL packs. This issue was caught during internal\ntesting and did not affect any customer repositories. We will resume the\nroll out of per-language bundles in the coming weeks. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4184\">#4184</a></li>\n</ul>\n<h2>4.38.2 - 24 Sept 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.1\">2.27.1</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4160\">#4160</a></li>\n</ul>\n<h2>4.38.1 - 18 Sept 2026</h2>\n<ul>\n<li>The CodeQL Action now has experimental support for CodeQL releases\nfor which per-language bundles are available. Per-language bundles\nsupport analysis for a single language and are therefore smaller than\nthe combined bundles that allow analysis for all supported languages. As\na result, per-language bundles take up less space on disk and are faster\nto download. We expect to roll this change out to everyone in the coming\nweeks. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4146\">#4146</a></li>\n</ul>\n<h2>4.38.0 - 09 Sept 2026</h2>\n<ul>\n<li>On GitHub-hosted runners, the CodeQL Action now deletes unused\nCodeQL bundles from the toolcache before downloading a different bundle,\nwhich frees up disk space for the analysis. We expect to roll this\nchange out to everyone in September. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4124\">#4124</a></li>\n<li>The CodeQL Action now supports CodeQL releases that are compatible\nwith Linux Arm64 and downloads the native <code>linux-arm64</code>\nCodeQL bundle when available. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4072\">#4072</a></li>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.0\">2.27.0</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4129\">#4129</a></li>\n</ul>\n<h2>4.37.9 - 26 Aug 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.26.4\">2.26.4</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4106\">#4106</a></li>\n</ul>\n<h2>4.37.8 - 21 Aug 2026</h2>\n<p>No user facing changes.</p>\n<h2>4.37.7 - 13 Aug 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.26.3\">2.26.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4085\">#4085</a></li>\n</ul>\n<!-- raw HTML omitted -->\n</blockquote>\n<p>... (truncated)</p>\n</details>\n<details>\n<summary>Commits</summary>\n<ul>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/9b3254085494b55a0f7c4c2b3d2d52cc712ce370\"><code>9b32540</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4257\">#4257</a>\nfrom github/update-v4.39.1-c5a2f4b0f</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/b5e1ed9574dcfa81c88c318140f3b3e5c199bfb3\"><code>b5e1ed9</code></a>\nTrigger workflows</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/20f5ec39109073afe1789b098456b2be0164f4ee\"><code>20f5ec3</code></a>\nUpdate changelog for v4.39.1</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/c5a2f4b0fe28ca06eaaeb86fb05a5f612ae293f9\"><code>c5a2f4b</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4245\">#4245</a>\nfrom github/henrymercer/graduate-overlay-trap-caching</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/61a5a80dd98ad1b9679010141a46cace51065761\"><code>61a5a80</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4256\">#4256</a>\nfrom github/update-bundle/codeql-bundle-v2.27.3</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/3dd33f8b69588bd9591684b2dca169eb612f5ed5\"><code>3dd33f8</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4249\">#4249</a>\nfrom github/mbg/upload-improvements-for-4223</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/b96e3390db93dc46c2c3753a94b87c0d734b0b61\"><code>b96e339</code></a>\nChange comment for <code>getConfig</code> call in\n<code>upload-sarif</code></li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/7fa45180db8d657b23ded3636b5d8583aec6ddfe\"><code>7fa4518</code></a>\nUse <code>shouldSkipSarifUpload</code> in\n<code>analyze-action.ts</code></li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/a69c7cb332d56f616fcadef002884523c71e8d1c\"><code>a69c7cb</code></a>\nDocument that setting the env vars to <code>false</code> takes\nprecedence</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/74ddcd9f65683791ab07eccd074b4ba145d89ea6\"><code>74ddcd9</code></a>\nFix typo in comment</li>\n<li>Additional commits viewable in <a\nhref=\"https://github.com/github/codeql-action/compare/24c54180a607b1449ed407dd24f251e4e9147c8d...9b3254085494b55a0f7c4c2b3d2d52cc712ce370\">compare\nview</a></li>\n</ul>\n</details>\n<br />\n\nUpdates `github/codeql-action/analyze` from 4.38.3 to 4.39.1\n<details>\n<summary>Release notes</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/github/codeql-action/releases\">github/codeql-action/analyze's\nreleases</a>.</em></p>\n<blockquote>\n<h2>v4.39.1</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.3\">2.27.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4256\">#4256</a></li>\n</ul>\n<h2>v4.39.0</h2>\n<ul>\n<li><em>Breaking change</em>: Bump the minimum required CodeQL bundle\nversion to 2.20.7. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4205\">#4205</a></li>\n<li>The <code>codeql-action/analyze</code> step now validates that a\ncorrect argument for the <code>checkout_path</code> input has been\nprovided. The <code>checkout_path</code> input must be set to the path\nwhere the repository that is being analyzed is checked out at on the\nrunner, if that path is something other than the workspace root. If the\npath does not point to a repository checkout or is different from the\npath that was used by <code>codeql-action/init</code>, then the CodeQL\nAction now produces a warning. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4148\">#4148</a></li>\n<li>Resolved an issue with third-party caching implementations that\nprevented caches from being restored correctly. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4206\">#4206</a></li>\n<li>The <code>codeql-action/start-proxy</code> step that is used in Code\nScanning Default Setup now finds CodeQL releases using the same logic as\n<code>codeql-action/init</code>. This primarily benefits customers who\nare using GitHub instances other than <code>github.com</code> where the\n<code>codeql-action/start-proxy</code> step may previously have been\nunable to locate recent releases and ended up using outdated versions of\nthe private registry authentication proxy as a result. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4198\">#4198</a></li>\n</ul>\n</blockquote>\n</details>\n<details>\n<summary>Changelog</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/github/codeql-action/blob/main/CHANGELOG.md\">github/codeql-action/analyze's\nchangelog</a>.</em></p>\n<blockquote>\n<h1>CodeQL Action Changelog</h1>\n<p>See the <a\nhref=\"https://github.com/github/codeql-action/releases\">releases\npage</a> for the relevant changes to the CodeQL CLI and language\npacks.</p>\n<h2>[UNRELEASED]</h2>\n<p>No user facing changes.</p>\n<h2>4.39.1 - 21 Oct 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.3\">2.27.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4256\">#4256</a></li>\n</ul>\n<h2>4.39.0 - 15 Oct 2026</h2>\n<ul>\n<li><em>Breaking change</em>: Bump the minimum required CodeQL bundle\nversion to 2.20.7. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4205\">#4205</a></li>\n<li>The <code>codeql-action/analyze</code> step now validates that a\ncorrect argument for the <code>checkout_path</code> input has been\nprovided. The <code>checkout_path</code> input must be set to the path\nwhere the repository that is being analyzed is checked out at on the\nrunner, if that path is something other than the workspace root. If the\npath does not point to a repository checkout or is different from the\npath that was used by <code>codeql-action/init</code>, then the CodeQL\nAction now produces a warning. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4148\">#4148</a></li>\n<li>Resolved an issue with third-party caching implementations that\nprevented caches from being restored correctly. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4206\">#4206</a></li>\n<li>The <code>codeql-action/start-proxy</code> step that is used in Code\nScanning Default Setup now finds CodeQL releases using the same logic as\n<code>codeql-action/init</code>. This primarily benefits customers who\nare using GitHub instances other than <code>github.com</code> where the\n<code>codeql-action/start-proxy</code> step may previously have been\nunable to locate recent releases and ended up using outdated versions of\nthe private registry authentication proxy as a result. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4198\">#4198</a></li>\n</ul>\n<h2>4.38.3 - 08 Oct 2026</h2>\n<ul>\n<li><em>Upcoming breaking change</em>: CodeQL version 2.21.2 and earlier\nwere discontinued on 24 September 2026 alongside GitHub Enterprise\nServer 3.17, and will be unsupported by the next minor release of the\nCodeQL Action. Added a deprecation warning for customers using these\nversions of CodeQL. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4188\">#4188</a></li>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.2\">2.27.2</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4203\">#4203</a></li>\n<li>Fixed a bug where the decision of whether to use a per-language\nbundle did not account for custom configurations that reference queries\noutside of compiled CodeQL packs. This issue was caught during internal\ntesting and did not affect any customer repositories. We will resume the\nroll out of per-language bundles in the coming weeks. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4184\">#4184</a></li>\n</ul>\n<h2>4.38.2 - 24 Sept 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.1\">2.27.1</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4160\">#4160</a></li>\n</ul>\n<h2>4.38.1 - 18 Sept 2026</h2>\n<ul>\n<li>The CodeQL Action now has experimental support for CodeQL releases\nfor which per-language bundles are available. Per-language bundles\nsupport analysis for a single language and are therefore smaller than\nthe combined bundles that allow analysis for all supported languages. As\na result, per-language bundles take up less space on disk and are faster\nto download. We expect to roll this change out to everyone in the coming\nweeks. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4146\">#4146</a></li>\n</ul>\n<h2>4.38.0 - 09 Sept 2026</h2>\n<ul>\n<li>On GitHub-hosted runners, the CodeQL Action now deletes unused\nCodeQL bundles from the toolcache before downloading a different bundle,\nwhich frees up disk space for the analysis. We expect to roll this\nchange out to everyone in September. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4124\">#4124</a></li>\n<li>The CodeQL Action now supports CodeQL releases that are compatible\nwith Linux Arm64 and downloads the native <code>linux-arm64</code>\nCodeQL bundle when available. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4072\">#4072</a></li>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.0\">2.27.0</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4129\">#4129</a></li>\n</ul>\n<h2>4.37.9 - 26 Aug 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.26.4\">2.26.4</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4106\">#4106</a></li>\n</ul>\n<h2>4.37.8 - 21 Aug 2026</h2>\n<p>No user facing changes.</p>\n<h2>4.37.7 - 13 Aug 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.26.3\">2.26.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4085\">#4085</a></li>\n</ul>\n<!-- raw HTML omitted -->\n</blockquote>\n<p>... (truncated)</p>\n</details>\n<details>\n<summary>Commits</summary>\n<ul>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/9b3254085494b55a0f7c4c2b3d2d52cc712ce370\"><code>9b32540</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4257\">#4257</a>\nfrom github/update-v4.39.1-c5a2f4b0f</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/b5e1ed9574dcfa81c88c318140f3b3e5c199bfb3\"><code>b5e1ed9</code></a>\nTrigger workflows</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/20f5ec39109073afe1789b098456b2be0164f4ee\"><code>20f5ec3</code></a>\nUpdate changelog for v4.39.1</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/c5a2f4b0fe28ca06eaaeb86fb05a5f612ae293f9\"><code>c5a2f4b</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4245\">#4245</a>\nfrom github/henrymercer/graduate-overlay-trap-caching</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/61a5a80dd98ad1b9679010141a46cace51065761\"><code>61a5a80</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4256\">#4256</a>\nfrom github/update-bundle/codeql-bundle-v2.27.3</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/3dd33f8b69588bd9591684b2dca169eb612f5ed5\"><code>3dd33f8</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4249\">#4249</a>\nfrom github/mbg/upload-improvements-for-4223</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/b96e3390db93dc46c2c3753a94b87c0d734b0b61\"><code>b96e339</code></a>\nChange comment for <code>getConfig</code> call in\n<code>upload-sarif</code></li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/7fa45180db8d657b23ded3636b5d8583aec6ddfe\"><code>7fa4518</code></a>\nUse <code>shouldSkipSarifUpload</code> in\n<code>analyze-action.ts</code></li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/a69c7cb332d56f616fcadef002884523c71e8d1c\"><code>a69c7cb</code></a>\nDocument that setting the env vars to <code>false</code> takes\nprecedence</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/74ddcd9f65683791ab07eccd074b4ba145d89ea6\"><code>74ddcd9</code></a>\nFix typo in comment</li>\n<li>Additional commits viewable in <a\nhref=\"https://github.com/github/codeql-action/compare/24c54180a607b1449ed407dd24f251e4e9147c8d...9b3254085494b55a0f7c4c2b3d2d52cc712ce370\">compare\nview</a></li>\n</ul>\n</details>\n<br />\n\nUpdates `github/codeql-action/upload-sarif` from 4.38.3 to 4.39.1\n<details>\n<summary>Release notes</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/github/codeql-action/releases\">github/codeql-action/upload-sarif's\nreleases</a>.</em></p>\n<blockquote>\n<h2>v4.39.1</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.3\">2.27.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4256\">#4256</a></li>\n</ul>\n<h2>v4.39.0</h2>\n<ul>\n<li><em>Breaking change</em>: Bump the minimum required CodeQL bundle\nversion to 2.20.7. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4205\">#4205</a></li>\n<li>The <code>codeql-action/analyze</code> step now validates that a\ncorrect argument for the <code>checkout_path</code> input has been\nprovided. The <code>checkout_path</code> input must be set to the path\nwhere the repository that is being analyzed is checked out at on the\nrunner, if that path is something other than the workspace root. If the\npath does not point to a repository checkout or is different from the\npath that was used by <code>codeql-action/init</code>, then the CodeQL\nAction now produces a warning. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4148\">#4148</a></li>\n<li>Resolved an issue with third-party caching implementations that\nprevented caches from being restored correctly. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4206\">#4206</a></li>\n<li>The <code>codeql-action/start-proxy</code> step that is used in Code\nScanning Default Setup now finds CodeQL releases using the same logic as\n<code>codeql-action/init</code>. This primarily benefits customers who\nare using GitHub instances other than <code>github.com</code> where the\n<code>codeql-action/start-proxy</code> step may previously have been\nunable to locate recent releases and ended up using outdated versions of\nthe private registry authentication proxy as a result. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4198\">#4198</a></li>\n</ul>\n</blockquote>\n</details>\n<details>\n<summary>Changelog</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/github/codeql-action/blob/main/CHANGELOG.md\">github/codeql-action/upload-sarif's\nchangelog</a>.</em></p>\n<blockquote>\n<h1>CodeQL Action Changelog</h1>\n<p>See the <a\nhref=\"https://github.com/github/codeql-action/releases\">releases\npage</a> for the relevant changes to the CodeQL CLI and language\npacks.</p>\n<h2>[UNRELEASED]</h2>\n<p>No user facing changes.</p>\n<h2>4.39.1 - 21 Oct 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.3\">2.27.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4256\">#4256</a></li>\n</ul>\n<h2>4.39.0 - 15 Oct 2026</h2>\n<ul>\n<li><em>Breaking change</em>: Bump the minimum required CodeQL bundle\nversion to 2.20.7. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4205\">#4205</a></li>\n<li>The <code>codeql-action/analyze</code> step now validates that a\ncorrect argument for the <code>checkout_path</code> input has been\nprovided. The <code>checkout_path</code> input must be set to the path\nwhere the repository that is being analyzed is checked out at on the\nrunner, if that path is something other than the workspace root. If the\npath does not point to a repository checkout or is different from the\npath that was used by <code>codeql-action/init</code>, then the CodeQL\nAction now produces a warning. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4148\">#4148</a></li>\n<li>Resolved an issue with third-party caching implementations that\nprevented caches from being restored correctly. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4206\">#4206</a></li>\n<li>The <code>codeql-action/start-proxy</code> step that is used in Code\nScanning Default Setup now finds CodeQL releases using the same logic as\n<code>codeql-action/init</code>. This primarily benefits customers who\nare using GitHub instances other than <code>github.com</code> where the\n<code>codeql-action/start-proxy</code> step may previously have been\nunable to locate recent releases and ended up using outdated versions of\nthe private registry authentication proxy as a result. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4198\">#4198</a></li>\n</ul>\n<h2>4.38.3 - 08 Oct 2026</h2>\n<ul>\n<li><em>Upcoming breaking change</em>: CodeQL version 2.21.2 and earlier\nwere discontinued on 24 September 2026 alongside GitHub Enterprise\nServer 3.17, and will be unsupported by the next minor release of the\nCodeQL Action. Added a deprecation warning for customers using these\nversions of CodeQL. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4188\">#4188</a></li>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.2\">2.27.2</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4203\">#4203</a></li>\n<li>Fixed a bug where the decision of whether to use a per-language\nbundle did not account for custom configurations that reference queries\noutside of compiled CodeQL packs. This issue was caught during internal\ntesting and did not affect any customer repositories. We will resume the\nroll out of per-language bundles in the coming weeks. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4184\">#4184</a></li>\n</ul>\n<h2>4.38.2 - 24 Sept 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.1\">2.27.1</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4160\">#4160</a></li>\n</ul>\n<h2>4.38.1 - 18 Sept 2026</h2>\n<ul>\n<li>The CodeQL Action now has experimental support for CodeQL releases\nfor which per-language bundles are available. Per-language bundles\nsupport analysis for a single language and are therefore smaller than\nthe combined bundles that allow analysis for all supported languages. As\na result, per-language bundles take up less space on disk and are faster\nto download. We expect to roll this change out to everyone in the coming\nweeks. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4146\">#4146</a></li>\n</ul>\n<h2>4.38.0 - 09 Sept 2026</h2>\n<ul>\n<li>On GitHub-hosted runners, the CodeQL Action now deletes unused\nCodeQL bundles from the toolcache before downloading a different bundle,\nwhich frees up disk space for the analysis. We expect to roll this\nchange out to everyone in September. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4124\">#4124</a></li>\n<li>The CodeQL Action now supports CodeQL releases that are compatible\nwith Linux Arm64 and downloads the native <code>linux-arm64</code>\nCodeQL bundle when available. <a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4072\">#4072</a></li>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.27.0\">2.27.0</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4129\">#4129</a></li>\n</ul>\n<h2>4.37.9 - 26 Aug 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.26.4\">2.26.4</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4106\">#4106</a></li>\n</ul>\n<h2>4.37.8 - 21 Aug 2026</h2>\n<p>No user facing changes.</p>\n<h2>4.37.7 - 13 Aug 2026</h2>\n<ul>\n<li>Update default CodeQL bundle version to <a\nhref=\"https://github.com/github/codeql-action/releases/tag/codeql-bundle-v2.26.3\">2.26.3</a>.\n<a\nhref=\"https://redirect.github.com/github/codeql-action/pull/4085\">#4085</a></li>\n</ul>\n<!-- raw HTML omitted -->\n</blockquote>\n<p>... (truncated)</p>\n</details>\n<details>\n<summary>Commits</summary>\n<ul>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/9b3254085494b55a0f7c4c2b3d2d52cc712ce370\"><code>9b32540</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4257\">#4257</a>\nfrom github/update-v4.39.1-c5a2f4b0f</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/b5e1ed9574dcfa81c88c318140f3b3e5c199bfb3\"><code>b5e1ed9</code></a>\nTrigger workflows</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/20f5ec39109073afe1789b098456b2be0164f4ee\"><code>20f5ec3</code></a>\nUpdate changelog for v4.39.1</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/c5a2f4b0fe28ca06eaaeb86fb05a5f612ae293f9\"><code>c5a2f4b</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4245\">#4245</a>\nfrom github/henrymercer/graduate-overlay-trap-caching</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/61a5a80dd98ad1b9679010141a46cace51065761\"><code>61a5a80</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4256\">#4256</a>\nfrom github/update-bundle/codeql-bundle-v2.27.3</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/3dd33f8b69588bd9591684b2dca169eb612f5ed5\"><code>3dd33f8</code></a>\nMerge pull request <a\nhref=\"https://redirect.github.com/github/codeql-action/issues/4249\">#4249</a>\nfrom github/mbg/upload-improvements-for-4223</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/b96e3390db93dc46c2c3753a94b87c0d734b0b61\"><code>b96e339</code></a>\nChange comment for <code>getConfig</code> call in\n<code>upload-sarif</code></li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/7fa45180db8d657b23ded3636b5d8583aec6ddfe\"><code>7fa4518</code></a>\nUse <code>shouldSkipSarifUpload</code> in\n<code>analyze-action.ts</code></li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/a69c7cb332d56f616fcadef002884523c71e8d1c\"><code>a69c7cb</code></a>\nDocument that setting the env vars to <code>false</code> takes\nprecedence</li>\n<li><a\nhref=\"https://github.com/github/codeql-action/commit/74ddcd9f65683791ab07eccd074b4ba145d89ea6\"><code>74ddcd9</code></a>\nFix typo in comment</li>\n<li>Additional commits viewable in <a\nhref=\"https://github.com/github/codeql-action/compare/24c54180a607b1449ed407dd24f251e4e9147c8d...9b3254085494b55a0f7c4c2b3d2d52cc712ce370\">compare\nview</a></li>\n</ul>\n</details>\n<br />\n\nUpdates `zizmorcore/zizmor-action` from 0.6.4 to 0.7.1\n<details>\n<summary>Release notes</summary>\n<p><em>Sourced from <a\nhref=\"https://github.com/zizmorcore/zizmor-action/releases\">zizmorcore/zizmor-action's\nreleases</a>.</em></p>\n<blockquote>\n<h2>v0.7.1</h2>\n<h1>zizmor changes</h1>\n<h2>Bug Fixes 🐛🔗</h2>\n<ul>\n<li>Fixed a regression in 1.31.0 where some ignore\ncomments/configurations would no longer apply to the dangerous-triggers\naudit</li>\n</ul>\n<h1>action changes</h1>\n<p>The default version of zizmor is now 1.31.1.</p>\n<h2>What's Changed</h2>\n<ul>\n<li>Sync zizmor versions by <a\nhref=\"https://github.com/github-actions\"><code>@​github-actions</code></a>[bot]\nin <a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/pull/178\">zizmorcore/zizmor-action#178</a></li>\n</ul>\n<p><strong>Full Changelog</strong>: <a\nhref=\"https://github.com/zizmorcore/zizmor-action/compare/v0.7.0...v0.7.1\">https://github.com/zizmorcore/zizmor-action/compare/v0.7.0...v0.7.1</a></p>\n<h2>v0.7.0</h2>\n<p><a href=\"https://github.com/sponsors/woodruffw/\">Sponsorship is\nappreciated!</a></p>\n<h1>zizmor notes</h1>\n<h2>Enhancements 🌱<a\nhref=\"https://docs.zizmor.sh/release-notes/#enhancements\">🔗</a></h2>\n<ul>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#typosquat-uses\">typosquat-uses</a>\naudit now supports pre-commit config inputs (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2437\">#2437</a>)</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#self-hosted-runner\">self-hosted-runner</a>\naudit is now significantly more precise (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2350\">#2350</a>)</p>\n<p>Many thanks to <a\nhref=\"https://github.com/ubiratansoares\"><code>@​ubiratansoares</code></a>\nfor proposing and implementing this enhancement!</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#dangerous-triggers\">dangerous-triggers</a>\naudit now considers the issue_comment trigger fundamentally dangerous\n(<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2396\">#2396</a>)</p>\n</li>\n<li>\n<p>zizmor is now aware of the image_version workflow trigger (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2397\">#2397</a>)</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#dangerous-triggers\">dangerous-triggers</a>\naudit now produces more precise finding/diagnostic spans (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2402\">#2402</a>)</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#cache-poisoning\">cache-poisoning</a>\naudit is now aware of <a\nhref=\"https://github.com/pnpm/setup\">pnpm/setup</a> (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2409\">#2409</a>)</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#unpinned-images\">unpinned-images</a>\naudit now produces more precise findings when the user's workflow\ncontains an indirect matrix (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2395\">#2395</a>)</p>\n<p>Many thanks to <a\nhref=\"https://github.com/ubiratansoares\"><code>@​ubiratansoares</code></a>\nfor proposing and implementing this enhancement!</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#cache-poisoning\">cache-poisoning</a>\naudit is now aware of GitHub's new cache-mode: setting, and will skip\nany findings that would occur when a job has an effective cache-mode:\nnone (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2418\">#2418</a>)</p>\n</li>\n<li>\n<p>The <a\nhref=\"https://docs.zizmor.sh/audits/#cache-poisoning\">cache-poisoning</a>\naudit now flags usages of cache-mode: write and cache-mode: write-only\nwith dangerous triggers, i.e. triggers that give an attacker privileged\ncode execution (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2426\">#2426</a>)</p>\n</li>\n<li>\n<p>zizmor now uses a tree-sitter grammar for PowerShell that's more\nprecise/capable of capturing more non-trivial PowerShell expressions (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor/issues/2444\">#2444</a>)</p>\n</li>\n</ul>\n<h2>Bug Fixes 🐛<a\nhref=\"https://docs.zizmor.sh/release-notes/#bug-fixes\">🔗</a></h2>\n<!-- raw HTML omitted -->\n</blockquote>\n<p>... (truncated)</p>\n</details>\n<details>\n<summary>Commits</summary>\n<ul>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/85b2b9fb8e3325e7a23d7d0a9d7ba5ae0f999e3e\"><code>85b2b9f</code></a>\nSync zizmor versions (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/178\">#178</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/38c745a43764dcc8bcfd364e8295870e252e957c\"><code>38c745a</code></a>\nREADME: bump pins (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/177\">#177</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/f9597c869b2e67095f9b527070297a13a6efe928\"><code>f9597c8</code></a>\nSync zizmor versions (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/175\">#175</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/2181e51ed8d58b9304e4a561374bb36d5599af3a\"><code>2181e51</code></a>\nBump sync-zizmor-versions.py to Python 3.11 for locking (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/174\">#174</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/27d989be9329e6cff7f80baf450ca83bf4c94898\"><code>27d989b</code></a>\nchore(deps): bump the github-actions group with 2 updates (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/173\">#173</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/a91218c45134530ecc7aefc3eec3f997a279ad37\"><code>a91218c</code></a>\nBootstrap zizmor without Docker (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/169\">#169</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/ee3f1bf153a9caa7d162d7324ef8314ec3f91862\"><code>ee3f1bf</code></a>\nchore(deps): bump the github-actions group with 2 updates (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/170\">#170</a>)</li>\n<li><a\nhref=\"https://github.com/zizmorcore/zizmor-action/commit/8eacaa5f3f72b6638f4f87c3f66113bbc0f363dd\"><code>8eacaa5</code></a>\nREADME: bump pins (<a\nhref=\"https://redirect.github.com/zizmorcore/zizmor-action/issues/167\">#167</a>)</li>\n<li>See full diff in <a\nhref=\"https://github.com/zizmorcore/zizmor-action/compare/cc914d7f3750a2d13d75c7f184a1060aa0e9d482...85b2b9fb8e3325e7a23d7d0a9d7ba5ae0f999e3e\">compare\nview</a></li>\n</ul>\n</details>\n<br />\n\nSigned-off-by: dependabot[bot] <support@github.com>\nCo-authored-by: dependabot[bot] <49699333+dependabot[bot]@users.noreply.github.com>",
          "timestamp": "2026-10-28T14:06:16+03:00",
          "tree_id": "3c1411316a9666c916f39787ed83a11b6d5c8371",
          "url": "https://github.com/ydb-campus/antb1/commit/f20bf8ba104f20082d77ff451949ff4e9f4d10b2"
        },
        "date": 1793186307096,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4563.512656948383,
            "unit": "ns/iter",
            "extra": "iterations: 153315\ncpu: 4563.131839676483 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 28399.65089477582,
            "unit": "ns/iter",
            "extra": "iterations: 24643\ncpu: 28397.899646958576 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 51992.85334514986,
            "unit": "ns/iter",
            "extra": "iterations: 13542\ncpu: 51987.99630778319 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 232051.48373174138,
            "unit": "ns/iter",
            "extra": "iterations: 3012\ncpu: 232036.84395750327 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 497777.42206404684,
            "unit": "ns/iter",
            "extra": "iterations: 1405\ncpu: 497757.8370106759 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1495801.4707157607,
            "unit": "ns/iter",
            "extra": "iterations: 461\ncpu: 1495727.86550976 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.40582338888991,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 40.40437427777776 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 49.135587928568775,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.13432485714278 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 210.28183933333366,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 210.28137266666627 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.830372872340638,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.830166212765928 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 22.30621806451484,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.30495590322581 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 37.76923063157997,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.76568752631573 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.4877402355461051,
            "unit": "ms/iter",
            "extra": "iterations: 467\ncpu: 1.4876159657387558 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 128.75143300000028,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 128.73879916666672 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.777727000001022,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.776251258064512 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.797405464285912,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.79405024999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 54.54162061538717,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.537677307692434 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 82.99975537499904,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 82.99438299999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 293.7881579999839,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 293.7520324999987 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1333.8680150000073,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1333.7902249999977 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.57706588000019,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.57606300000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.172261045453936,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.170131772727316 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 55.5144294166657,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.51020266666681 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 105.6734442857094,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 105.66006285714278 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 366.61870700001487,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 366.6109320000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1625.0396710000246,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1624.9255249999983 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.43383449999817,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.42918600000021 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 59.37076525000388,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.36966675000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 86.56638212499956,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 86.55867087500013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 162.10657950000495,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 162.10062725000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 455.1630504999764,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 455.13632349999966 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 2124.856529999988,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2124.682573000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.21082272727405,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.20952754545432 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.58781220000196,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.5814533999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 98.48811285714224,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 98.47625814285672 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 184.83777700000323,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 184.8302026666673 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 493.0474390000086,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 492.99672199999736 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2137.3696429999995,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2137.173492999999 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "04fee4d89796a0cd8a144408f169ab5dbbecce16",
          "message": "fix(exec): fail every group's sum range error before a limit cuts the output (#252)\n\n## Summary\n\nA HUGEINT or DECIMAL(38,s) group `SUM` outside its 38-digit range was\ndetected only when `Finalize` reached that group, and GROUP BY output is\nfinalized lazily: `PartGroupAggregateOperator::BuildRows` builds\n`threads` partitions per wave (one under memory pressure), and the\nserial `GroupAggregateOperator` one `NextChunk` per `Next`. Under a\n`LIMIT` without `ORDER BY` the same query could therefore answer on 1\nthread and fail on 4 (against ADR 0013's thread-count independence; ADR\n0018 already treats \"an overflow in another group would no longer fail\nthe query\" as a bug).\n\nThe fix adds `GroupedAggregateState::CheckFinalize()` to the public\nheader (default OK, so other states are untouched). The HUGEINT/DECIMAL\nsum, its SUM alias and SUM of counts override it, sharing `Finalize`'s\nerror text. Operators call it on every table before their first output\nbatch:\n\n- the part operator, after the merge and before the first `BuildRows`\nwave;\n- the serial operator, after its input and before the first `NextChunk`;\n- the two-level operator's heavy table, a guard only:\n`TwoLevelAggregation` accepts sums of exact integers alone, which cannot\nleave the range, so no test can reach it today.\n\nThe check is one pass over the sums of the states that can fail; every\nother state returns at once.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [x] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n`exec.PartOperatorsTest.GroupRangeErrorsDoNotDependOnALimit` failed at\n`f20bf8b` (1 thread: OK, 4 threads: ExecutionError).\n`exec.GroupedAggregateTest.RangeErrorsFailBeforeTheFirstChunk` fails\nwith the serial operator's check removed.\n\n```text\n$ pixi run check\nlint: PASS\n100% tests passed out of 2919\n```\n\n`pixi run check-full` was not run: the diff touches no exec memory or\nownership code, CMake files or presets (AGENTS.md); CI runs its legs.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: the part operator on 1 and 4 threads under\na LIMIT, the serial operator with the failing group in its second chunk,\n`CheckFinalize` per state (HUGEINT, DECIMAL(38,2), AVG, an empty group,\nSUM of counts) and a SUM that reads an AVG's accumulator\n- [x] Docs updated: `docs/sql-subset.md` (SUM)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) confirmed the bug with a failing test, wrote the fix, tests and\ndocs under a plan approved in plan mode (public header), and ran the\n`reviewer` subagent (no P0/P1 findings; its two minor points are fixed).\n- Accountable human (has read and understands the whole diff): @Hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-28T15:59:34+03:00",
          "tree_id": "dfdc6f87e2548e6c748e5b7cd602d00cf983108f",
          "url": "https://github.com/ydb-campus/antb1/commit/04fee4d89796a0cd8a144408f169ab5dbbecce16"
        },
        "date": 1793193082789,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4728.845225057061,
            "unit": "ns/iter",
            "extra": "iterations: 147918\ncpu: 4728.533897159237 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 38971.39451734736,
            "unit": "ns/iter",
            "extra": "iterations: 17984\ncpu: 38965.40224644127 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52092.7047498696,
            "unit": "ns/iter",
            "extra": "iterations: 13453\ncpu: 52085.52129636512 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 232332.1835652538,
            "unit": "ns/iter",
            "extra": "iterations: 3018\ncpu: 232278.70642809803 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 497853.7492877616,
            "unit": "ns/iter",
            "extra": "iterations: 1404\ncpu: 497785.5925925926 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1504288.759825339,
            "unit": "ns/iter",
            "extra": "iterations: 458\ncpu: 1503953.5087336244 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 39.76956699999418,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 39.765977529411764 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 46.02614140000393,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.02463986666668 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 199.94349633335182,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 199.91656066666687 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.741761729164674,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.740344854166679 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 20.499900000001002,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.498060058823544 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 37.0625105263116,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.05793710526323 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.4189159290059716,
            "unit": "ms/iter",
            "extra": "iterations: 493\ncpu: 1.418811620689656 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 104.85398183334382,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 104.83781683333326 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.77029293103514,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 22.76867710344828 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.532056571428843,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.52872892857145 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 53.12639061538413,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.124624461538595 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 78.81485412501377,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 78.80268275000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 262.7516603333409,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 262.7459413333331 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1285.277280999935,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1285.0500779999975 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.428050079997774,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.42661267999992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 30.9776738695637,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.974192434782506 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 54.167946615384956,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.16663061538456 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 96.05002999999864,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.04375157142847 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 327.8990794999572,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 327.88276799999846 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1515.3414719999319,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1515.1219150000018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 55.727971692301885,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 55.72040515384612 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 59.09416500000475,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.09262416666679 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 84.93507449999527,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.92554612500003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 160.9248577500182,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 160.90034250000062 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 431.6453929999966,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 431.63681349999905 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1911.1001209998904,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1910.6778169999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 64.8885238181885,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 64.88621609090924 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.5520634999989,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.54697110000032 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 98.06251842855674,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 98.052708571429 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 173.67555275001223,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 173.67391424999923 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 437.12311849998287,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 437.0788380000015 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2084.413771999948,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2084.2302160000017 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "89f8afcaf42bf1258f965ced534c507e6c089477",
          "message": "perf(bench): step 0 of the group by key tables (#253)\n\n## Step 0 verdict: go for one BIGINT key and for a BIGINT plus an INT\n\nMeasured on a 2 × Xeon Gold 6338 (1.25 MiB L2 per core) and an Apple M4\nPro, against the budgeted grouper of the same run and the same ids. The\ngate is at least 1.3× at every count from 1000 to 1 Mi groups.\n\n- **Both shapes go, with the size estimate, on random ids (the realistic\ninput), on both hosts.**\n\n| `IndirectSlotOrder_Estimate`, minimum over 1000 to 1 Mi | BIGINT |\nBIGINT + INT |\n  | --- | --: | --: |\n  | Xeon | 1.60× | 1.50× |\n  | M4 Pro | 1.34× | 1.45× |\n\n- **Layout for step 1:** a 4-byte slot holding the group id alone, with\nthe size estimate. How the table grows becomes a template parameter,\ndefaulting to slot order (`IndirectSlotOrder`).\n- **Why slot order:** it never fell below the grouper on either host or\ninput. Its worst result is 1.07× on the Xeon, at 4 Mi groups with ids\nthat never repeat early.\n- **What it gives up:** on the M4 Pro, id-order growth (`Indirect`) is\n10-48% faster from 256 Ki groups up. But on the Xeon that same growth\nfalls to 0.79-0.87× of the grouper at 4 Mi on those ids.\n- **How it gets settled:** end to end in step 1, on real queries on both\nhosts. The choice cannot change any result, because growth only moves\ngroups between slots and never changes their ids.\n- **`Hash32` (8-byte slots) is out.** On the M4 Pro it fails the gate on\nrandom ids: 1.27-1.29× at 16 Ki for BIGINT, and 1.25× at 1 Mi for BIGINT\n+ INT without the estimate.\n- **The estimate fails small,** on data and on memory. It sizes the\ntable once, after the second batch, from a capture-recapture count of\nthe table's own rows, and:\n  - reserves nothing below 64 hits;\n  - is lowered by skew;\n  - never reserves more than 8× the groups already seen;\n  - never falls back to a row count;\n- when the budget refuses its reservation, is skipped while the table\nkeeps growing. A tight-budget run in the benchmark's own check covers\nthis; with the skip removed, that check fails.\n- **Known limit:** on the Xeon, BIGINT + INT with ids that never repeat\nearly (the permutation, standing in for sorted or clustered keys)\nreaches 1.25× at 1 Mi.\n- **Not covered by step 0:** NULL keys, DOUBLE normalization, string\nkeys (steps 3 and 4), several threads sharing caches, and the engine end\nto end. Step 1 wires the table into `GroupTable` under plan mode and is\ngated end to end.\n\n## Summary\n\nStep 0 of the GROUP BY key tables (follow-up of ADR 0019): benchmark a\nloop of our own against `arrow::compute::Grouper` before any engine\nchange, and stop the table work if none wins by 1.3×. Benchmarks only;\nno engine change. This PR keeps the loop step 0 chose and the arms that\ndecided it. The variants it dropped are in the branch history up to\n1bdca50, with their numbers below.\n\n- `bench/group_loops.h`: a minimal table for one BIGINT key or a BIGINT\nand an INT. It only numbers keys (first-seen dense ids, no NULLs) and\ncharges every byte to an `exec::MemoryBudget` before allocating it,\nnever per insert. Step 1 builds the engine's table from it and replaces\nthe file. It has:\n- a fixed-seed one-multiply hash using the mixing scheme of foldhash\n0.2.0's fast tier (a folded multiply). Its three constants are\nhexadecimal digits of pi, the ones foldhash uses;\n- SWAR tag groups of 8 with mirrored control bytes and a triangular\nprobe: hashbrown's design, written from its description, not its code.\nOrigins under ADR 0034: no code text is copied, so `NOTICE` is unchanged\n(confirmed by the maintainer);\n- a chunked loop: one room check and one hash pass per 256 rows, then\nthe probes, prefetching the buckets of the row 8 places ahead;\n- the group id alone in a 4-byte slot, growing either in id order\n(`Indirect`) or by walking the old slots in order and rehashing each key\n(`IndirectSlotOrder`);\n- an optional size estimate after the second batch: a capture-recapture\ncount of the table's own rows, reserving 9/8 of it. It fails small:\nbelow 64 hits it reserves nothing, it never reserves more than 8× the\ngroups seen, it never falls back to a row count, and a refused\nreservation is skipped.\n- `bench/micro_bench.cc`:\n- `BM_Group<shape>Budget[Random]`: the grouper with a `MemoryBudget` as\nits pool, the gate's baseline;\n- `BM_S0<shape>_<growth>_<Estimate | Presized>[_Random]` at 1000, 16 Ki,\n256 Ki, 1 Mi and 4 Mi groups: 70 benchmarks in all, with the baselines;\n- random ids drawn with splitmix64, so groups repeat early as in real\ndata. The other inputs use the existing permutation, where no id repeats\nuntil every group has been seen;\n- each variant's ids checked once against the input's true groups.\nEstimate variants are also checked under a tight budget, where their\nreservation must be refused and the table must carry on;\n- the grouping section's stale comments fixed, and counts derived from\nthe ClickBench dataset dropped.\n- `docs/benchmarks.md`: rows for the new benchmarks.\n\n### Results on Linux (2 × Xeon Gold 6338, 1.25 MiB L2 per core)\n\nStatic build, one core pinned (`numactl --physcpubind=8`),\n`MIMALLOC_PURGE_DELAY=-1`, 5 repetitions interleaved at random, the\nwhole host idle (about 1 busy core before and after). Speed-up over the\nbudgeted grouper of the same run and the same ids; the go is at least\n1.3× at every count from 1000 to 1 Mi, with 4 Mi reported only.\n\n**Random ids, with the estimate** (minimum over 1000 to 1 Mi, then 4 Mi)\n\n| | BIGINT min | BIGINT 4 Mi | BIGINT + INT min | BIGINT + INT 4 Mi |\n| --- | --: | --: | --: | --: |\n| `Indirect_Estimate` | 1.63× | 1.25× | 1.50× | 1.16× |\n| `IndirectSlotOrder_Estimate` | **1.60×** | **1.43×** | **1.50×** |\n**1.28×** |\n| `Hash32_Estimate` | 1.59× | 1.46× | 1.39× | 1.38× |\n\nWithout the estimate, on random ids, every layout drops to 1.13-1.46× at\n1 Mi. For example, BIGINT `Indirect` goes from 1.79× with the estimate\nto 1.13× without it.\n\n**Permuted ids** (no id repeats early, so the estimate sees too few hits\nand leaves the table alone)\n\n| | 256 Ki | 1 Mi | 4 Mi |\n| --- | --: | --: | --: |\n| BIGINT `Indirect` | 1.74× | 1.11× | 0.79× |\n| BIGINT `IndirectSlotOrder` | 1.77× | 1.31× | 1.07× |\n| BIGINT `Hash32` | 1.87× | 1.51× | 1.40× |\n| BIGINT `Indirect_Presized` | 2.32× | 2.45× | 3.00× |\n| BIGINT + INT `Indirect` | 1.57× | 1.10× | 0.87× |\n| BIGINT + INT `IndirectSlotOrder` | 1.59× | 1.25× | 1.10× |\n| BIGINT + INT `Hash32` | 1.61× | 1.36-1.40× | 1.37× |\n| BIGINT + INT `Indirect_Presized` | 2.05× | 2.21× | 3.13× |\n\n(256 Ki for BIGINT `IndirectSlotOrder` and BIGINT + INT `Indirect` is\nthe `_Estimate` arm, which runs the same loop on this input. The plain\narms varied 32% and 54% across repetitions in this run.)\n\n- **The gap at 1 Mi was growth.** Pre-sized, `Indirect` reaches\n2.21-2.45× there. The estimate recovers much of it where groups repeat.\n- **`IndirectSlotOrder` removes `Indirect`'s regression** when the\nestimate cannot fire (sorted or clustered keys, which the permutation\nimitates): at 4 Mi it reaches 1.07-1.10×, where `Indirect` falls to\n0.79-0.87×. `Hash32` is still the strongest there on this Xeon, at\n1.36-1.51× at 1 Mi. On the M4 Pro, `Hash32` fails at 16 Ki on random\nids.\n- **Run-to-run spread:** gated cells move by up to about 15% between\nruns that had only NUMA node 0 quiet and runs on a fully idle host.\nJudge the gate on an idle host, or across several runs.\n- **Measured and dropped:** a per-row loop (DataFusion's shape), the key\nor a 64-bit hash in the slot, prefetch distances of 16 and 32, load 1/2,\nand 4× growth; each lost to these.\n\n### macOS (M4 Pro), in the comments below\n\n- **Before the estimate (9f2d900):** every gated cell clears 1.3×, and\n`Indirect` beats `Hash32` at 1 Mi.\n- **d488610, random ids:** `Indirect` and `Indirect_Estimate` clear in\nboth shapes, with a minimum of 1.35× at 16 Ki for BIGINT. `Hash32` fails\nat 16 Ki for BIGINT (1.29×) and, without the estimate, at 1 Mi for\nBIGINT + INT (1.25×).\n- **8e03e9e:** `IndirectSlotOrder_Estimate` clears on random ids\n(minimum 1.34× and 1.45×), but slot-order growth is 10-48% slower than\nid-order growth from 256 Ki up. With ids that never repeat early, at 4\nMi, it reaches 1.17× and 1.14×, against `Indirect`'s 1.73× and 1.92×.\n`Hash32` fails again (1.27× and 1.25×).\n\n## Re-running it on macOS (arm64)\n\nThe useful number is the **speed-up over the budgeted grouper of the\nsame run and the same ids**, not ns/row.\n\n1. **Quiet machine:** on mains power, with messengers, browsers and\nvideo clients closed, and the screen kept awake (`caffeinate -dimsu` in\nanother terminal). Bursty load hurts more than steady load: random\ninterleaving absorbs a constant background, but a short burst skews\nwhichever arm it lands on. Write the output to a file, not the terminal.\n2. **Fetch and build.** The first `pixi run bench` builds the static\nArrow, which needs the network once.\n   ```bash\ngit fetch origin perf/step0-group-loops && git switch\nperf/step0-group-loops && git pull\n   pixi install --locked\npixi run bench --benchmark_list_tests=true | grep -cE 'Budget|S0' #\nbuilds; expect 70\n   ```\n3. **Run,** about 5 minutes:\n   ```bash\nMIMALLOC_PURGE_DELAY=-1 ANTB1_BENCH_OUT=step0-mac-4 pixi run bench\n--benchmark_filter='S0|Budget' \\\n--benchmark_repetitions=5 --benchmark_enable_random_interleaving=true \\\n     --benchmark_report_aggregates_only=true > step0-mac-4.txt 2>&1\n   ```\n   The JSON is `build/static/bench-results/step0-mac-4/micro.json`.\n4. **Summarize,** and paste the output here:\n   ```bash\n   python3 - build/static/bench-results/step0-mac-4/micro.json <<'PY'\n   import json, sys\n   COUNTS = [1000, 16384, 262144, 1048576, 4194304]\n   mean = {}\n   for b in json.load(open(sys.argv[1]))[\"benchmarks\"]:\n       if b.get(\"aggregate_name\") == \"mean\":\n           name, arg = b[\"run_name\"].rsplit(\"/\", 1)\n           mean[(name, int(arg))] = b[\"ns_per_row\"] * 1e9\n   for shape in [\"Int64\", \"Int64Int32\"]:\n       for ids in [\"\", \"Random\"]:\n           base = \"BM_Group%sBudget%s\" % (shape, ids)\nprint(\"\\n%s, %s ids (ns/row of the grouper, then speed-up)\" % (shape,\nids.lower() or \"permuted\"))\nprint(\"%-26s\" % \"grouper\" + \"\".join(\"%9.2f\" % mean[(base, c)] for c in\nCOUNTS))\nvariants = [\"Indirect_Estimate\", \"IndirectSlotOrder_Estimate\"]\n           if not ids:\n               variants += [\"Indirect_Presized\"]\n           for v in variants:\n               n = \"BM_S0%s_%s%s\" % (shape, v, \"_Random\" if ids else \"\")\nprint(\"%-26s\" % v + \"\".join(\"%8.2fx\" % (mean[(base, c)] / mean[(n, c)])\nfor c in COUNTS))\n   PY\ngrep -E '_cv ' step0-mac-4.txt | awk '{for(i=1;i<=NF;i++) if($i ~\n/^ns_per_row=/){split($i,a,\"=\");v=a[2];gsub(\"%\",\"\",v); if(v+0>3) print\n$1, a[2]}}'\n   ```\nThe last command lists any benchmark whose repetitions varied by more\nthan 3%. If it prints much, run step 3 again on a quieter machine.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [x] perf: performance improvement (benchmarks only; no engine change)\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check\nlint: PASS\n100% tests passed out of 2919\n\n$ pixi run release\n100% tests passed out of 2920   (bench.micro.smoke: Passed)\n```\n\nThe Linux benchmark runs were `numactl --physcpubind=8 pixi run bench\n--benchmark_filter='S0|Budget' --benchmark_repetitions=5\n--benchmark_enable_random_interleaving=true\n--benchmark_report_aggregates_only=true` with `MIMALLOC_PURGE_DELAY=-1`,\nwhile the host was idle (0.21-0.29 busy cores on NUMA node 0's other\nCPUs). `--benchmark_dry_run=true` runs every variant's id check.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests)\n- [x] Tests cover the change: benchmarks only; every step-0 variant\nchecks its ids against the input's groups before it is timed, and\n`bench.micro.smoke` runs them under `pixi run release`\n- [x] Docs updated: `docs/benchmarks.md`\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006); dataset-derived group counts\nwere removed from comments\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the loops, benchmarks and docs, and ran and summarized the\nmeasurements on the Linux host.\n- Accountable human (has read and understands the whole diff): @Hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-29T04:02:02+03:00",
          "tree_id": "104ce70a702e39739c78aadf02ed8028cdde8c23",
          "url": "https://github.com/ydb-campus/antb1/commit/89f8afcaf42bf1258f965ced534c507e6c089477"
        },
        "date": 1793236359438,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3458.3771724046887,
            "unit": "ns/iter",
            "extra": "iterations: 202483\ncpu: 3458.0401120094034 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 25759.54671167247,
            "unit": "ns/iter",
            "extra": "iterations: 27552\ncpu: 25755.867632113815 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 46364.94087233231,
            "unit": "ns/iter",
            "extra": "iterations: 15086\ncpu: 46359.06197799286 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 187683.3904072988,
            "unit": "ns/iter",
            "extra": "iterations: 3732\ncpu: 187661.1647909968 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 379136.6088124562,
            "unit": "ns/iter",
            "extra": "iterations: 1861\ncpu: 379128.46265448694 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1211373.9403747162,
            "unit": "ns/iter",
            "extra": "iterations: 587\ncpu: 1211321.4412265748 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 32.820829549999075,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 32.818449650000005 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 38.165538333332606,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.164237611111126 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 175.28801499999722,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 175.27086933333297 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.505181918032875,
            "unit": "ms/iter",
            "extra": "iterations: 61\ncpu: 11.504065196721314 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 14.372728306123193,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.372173897959192 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 31.97157727272652,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.967108318181833 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.1069637851500982,
            "unit": "ms/iter",
            "extra": "iterations: 633\ncpu: 1.1068414881516588 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 71.99014520000446,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 71.98001040000008 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 19.861169657143105,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.86013705714283 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 21.53421721212198,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.531876818181814 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 45.7731404999997,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 45.77126987500013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 73.64009629999941,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 73.62966960000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 249.1884316666718,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 249.17897200000047 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1306.89010399999,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1306.720981999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 23.843471793103898,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.84176955172414 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 26.32368666666741,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.321409407407366 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 48.36007933333425,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 48.35438146666675 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 86.29720037500022,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 86.29082000000032 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 323.7596090000068,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 323.7124824999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1556.333804000019,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1556.129526000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 43.73287725000097,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.728210625000095 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 46.15417320000006,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.151391933333485 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 69.3878294000001,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.37857830000027 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 158.82445050000626,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 158.79700750000046 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 417.25169149998464,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 417.1911339999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1918.6642160000247,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1918.475835999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 51.72373542857502,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 51.71803585714281 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 55.0841300769207,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 55.07995715384613 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 82.87256637500207,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 82.8655066249997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 182.16778824999835,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 182.14880274999956 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 437.2486065000203,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 437.1843764999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2003.4899369999835,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2003.3014419999945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 20.22783043749854,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 20.22705312500017 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 22.437112062499764,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 22.434670406249914 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 49.700005200001364,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 49.696401200000174 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 87.84564975000109,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 87.83015862499965 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 284.3185849999941,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 284.30565550000256 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 23.648525586205277,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.645954034482877 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 26.291356666667475,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.28737348148141 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 46.74775900000062,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.74218233333296 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 96.10106485714758,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.08906985714327 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 331.76897100000247,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 331.72126450000053 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 20.00594822857238,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 20.00324471428558 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 21.937877406250195,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 21.934557531249954 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 51.34297964285598,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 51.33726978571421 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 110.59162840000454,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 110.56515239999953 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 222.2325876666673,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 222.220870000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 23.94712544827567,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.944525551724077 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 28.12219740000046,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.118583920000045 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 53.19703815384331,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.18902715384647 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 120.61877100000136,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 120.60974999999985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 251.33081266665158,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 251.29824466666642 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 13.32368371698041,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.322931622641502 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 14.40797183673516,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.405902040816066 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 19.44150347222262,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.439193777777636 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 27.5895082499981,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 27.586481125000024 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 99.60258957142644,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 99.58662457142964 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 13.238081867924384,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.236777000000082 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 14.201003816325812,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.198911755102017 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 21.655990062500408,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 21.652820562500175 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 39.26736500000061,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 39.26438300000006 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 213.27281924999397,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 213.25540900000206 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 13.550713773585537,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.550010603773684 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 14.691352877550845,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.687255510203991 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 22.978483548386873,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.97615090322557 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 44.440578538460436,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 44.43785384615331 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 224.7033813333322,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 224.66389133333564 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 13.363666807692836,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.362918115384685 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 16.056216409091142,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 16.054430886363814 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 30.346902409090408,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 30.340096090908858 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 56.50495990909121,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 56.4973384545456 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 131.7759151666659,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 131.76426299999852 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 13.47497783018847,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.474046773584748 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 16.472056534883787,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.470760116279113 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 31.35499195454372,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.352158227272625 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 54.25407433332907,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 54.25015683333356 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 138.57425219999868,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 138.56859740000118 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 18.45911936111122,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 18.456339972222302 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 19.499175970587565,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 19.498111117647202 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 25.0194895000005,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.01638274999962 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 49.9511979230778,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 49.94747907692316 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 120.3024598333305,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 120.2853743333326 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 18.417451263156924,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.416200631578835 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 19.704659314285372,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.70228668571435 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 27.87388516000192,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.8727441999996 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 65.75435645454387,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.74669109090921 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 222.77875966667202,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 222.76829400000034 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 18.446850921052555,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.445072868421015 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 19.76631605714374,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.765459114285836 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 29.489370208331422,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.486226041666914 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 71.01742609999633,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 71.01035020000097 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 257.9082000000123,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 257.87593699999906 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 19.07252613888804,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.07130308333333 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 22.821241774192547,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.81886793548392 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 38.48168072222317,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.47768405555504 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 87.405234000002,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 87.39560875000052 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 214.31618375000028,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 214.3063377499992 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 19.163314702701747,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 19.159589594594507 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 23.046278766668138,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.043623800000052 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.95386688889049,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.94673266666694 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 93.15842950000312,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 93.15011616666642 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 223.2139282500043,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 223.1691707500012 ms\nthreads: 1"
          }
        ]
      },
      {
        "commit": {
          "author": {
            "email": "hor911@ydb.tech",
            "name": "Aleksandr Khoroshilov",
            "username": "Hor911"
          },
          "committer": {
            "email": "noreply@github.com",
            "name": "GitHub",
            "username": "web-flow"
          },
          "distinct": true,
          "id": "fe57fe1eb9be3d4ab1584d828f7add59e2895422",
          "message": "refactor(exec): put a group by's grouper behind a key-table seam (#255)\n\n## Summary\n\nStep 1a of the GROUP BY key tables (ADR 0019, amended in #254):\n`GroupTable` now numbers its key rows through an exec-private `KeyTable`\ninterface, instead of holding an `arrow::compute::Grouper` itself. Every\nkey shape still uses the Grouper, through `GrouperKeyTable`, so results\nmatch `main`. Step 1b puts a table of our own behind this seam for one\nor two fixed-width keys.\n\n- **`src/exec/key_table.h/.cc`:**\n  - `KeyTable` has `Consume`, `Uniques` and `num_groups`.\n- `Consume` returns `KeyIds`, which own their ids buffer. `GroupTable`\nholds them as the same call-local as before, so the ids are freed at the\nend of each `Consume`, `Merge` and `MergePartition` and memory use\nmatches `main`.\n- `MakeKeyTable(key_types, ExecContext*, KeyTableOptions)` allocates\nfrom the context's pool and takes no budget. `KeyTableKind` is `kAuto`,\nor a named kind for tests (only `kGrouper` in this PR).\n- `GroupTable::Make` gains a trailing `KeyTableOptions`. Operators keep\nthe default.\n- **`src/exec/pool_vector.h`:** an array in a buffer of an Arrow memory\npool. When that pool is the query's `MemoryBudget`, every allocation is\ncharged before it happens. Unused by the engine until 1b; unit-tested\nhere.\n- **Two deliberate differences from `main`:**\n- A fixed-width key column of only NULLs with no values buffer, which\nthe Arrow format allows, crashed Arrow's Grouper (a segfault in\n`Hashing32::HashInt`). `GrouperKeyTable` now hands the Grouper an\nall-NULL column with a zeroed values buffer. Nothing in the engine\nproduces such a column today, so this guards against a latent crash, not\na reachable bug.\n- A `std::bad_alloc` while making a key table or consuming keys becomes\n`OutOfMemory`. Before, `GroupAggregateOperator::Open` let it escape.\n- **`docs/architecture.md`:** the places that turn `std::bad_alloc` into\n`OutOfMemory`, now including the key table, and the Arrow places that\nstill end the process. That list grows to four with the Grouper's\n`GetUniques`: a heap failure inside it unwinds into a `noexcept` frame\nof Arrow's, the same on `main`.\n- **Small comment fixes in `group_table.{h,cc}`:** the duplicated\n`kPartitions` comment, and a note that lanes never read a shared part's\nkey table.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run check-full\nlint: PASS\nci (Clang Debug -Werror):      100% tests passed out of 2934\nasan (ASan+UBSan -Werror):     100% tests passed out of 2934\ntidy (-Werror):                PASS\ncoverage:                      100% tests passed out of 2934; Coverage gate: PASS\n                               exec 96.55% lines (floor 96.1%), 87.85% branches (floor 85.1%)\nfuzz-smoke:                    100% tests passed out of 2\nci-gcc (GCC 15 -Werror):       100% tests passed out of 2934\n\n$ pixi run release\n100% tests passed out of 2935   (bench.micro.smoke included)\n\n$ pixi run test-data\n100% tests passed out of 6\n```\n\n- **`key_table_test.cc`:** holds each kind to the contract any key table\nmust keep, against the keys' own bytes and not another table. A key's id\nnever changes, and each call's new keys take the new ids `[before,\nafter)`, one each. `Uniques` are of exactly the key types, valid after\nlater calls, and empty for a table with no groups. Every byte charged\ngoes back with the table. The cases:\n- NULLs across batches, and NULLs over arbitrary bytes in either of two\nkeys;\n  - columns with no values buffer, alone or beside another column;\n  - slices with NULLs at different offsets per column;\n  - every fixed-width key type, `decimal64` and `decimal128` included;\n  - two keys differing in one column;\n  - more than 64 Ki new groups in one call;\n  - `KeyHashes` of the uniques matching their rows;\n- a tiny budget failing with its own `OutOfMemory` and leaving 0 bytes;\n  - a full GROUP BY through part, lane and serial tables.\n\n  1b adds its kinds to the instantiation.\n- **`ColumnsWithoutAValuesBuffer` crashes on `main`'s Grouper path.** It\nis what found the latent crash.\n- **`bad_alloc_test.cc`:** sweeps every heap failure of `MakeKeyTable`\nand of `Consume` (including the all-NULL repair). Each gives\n`OutOfMemory`, never an exception, and leaves the budget at 0.\n- **Reviews:** a read-only adversarial review (46 agents; each finding\nchallenged by agents told to refute it) and the `reviewer` subagent.\nTheir confirmed findings are fixed:\n  - the `bad_alloc` guard now covers the whole `Consume`;\n  - a moved-from `PoolVector` is empty;\n  - the reference check is 4× faster;\n  - clang-tidy diagnostics and wording;\n  - the stale architecture paragraph.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests); `check-full` too\n- [x] Tests cover the change: `src/exec/tests/key_table_test.cc`,\n`pool_vector_test.cc` and `bad_alloc_test.cc`\n- [x] Docs updated: `docs/architecture.md` (`std::bad_alloc` handling)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer (none changed)\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) planned step 1 (a read-only adversarial plan review, approved by\nthe maintainer), wrote the seam, `PoolVector` and the tests, ran the\nreviews and the gates.\n- Accountable human (has read and understands the whole diff): @Hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-29T11:19:42+03:00",
          "tree_id": "3ddff55e7deed578b11e5a71ff6033c163407abb",
          "url": "https://github.com/ydb-campus/antb1/commit/fe57fe1eb9be3d4ab1584d828f7add59e2895422"
        },
        "date": 1793262522449,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2510.585147315162,
            "unit": "ns/iter",
            "extra": "iterations: 244306\ncpu: 2510.4781094201535 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 19777.67940668025,
            "unit": "ns/iter",
            "extra": "iterations: 32967\ncpu: 19776.512846179518 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 37676.999273368514,
            "unit": "ns/iter",
            "extra": "iterations: 19267\ncpu: 37675.62651165205 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 137687.0433339815,
            "unit": "ns/iter",
            "extra": "iterations: 5123\ncpu: 137679.47159867268 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 287820.9871582495,
            "unit": "ns/iter",
            "extra": "iterations: 2414\ncpu: 287790.41673570854 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 782170.0723981315,
            "unit": "ns/iter",
            "extra": "iterations: 884\ncpu: 782115.6029411766 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 22.172241451612756,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.17049077419352 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 29.668227874999786,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.66510770833334 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 132.59451249999188,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 132.58117349999998 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 8.51865775609756,
            "unit": "ms/iter",
            "extra": "iterations: 82\ncpu: 8.518255743902449 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 9.664495819443964,
            "unit": "ms/iter",
            "extra": "iterations: 72\ncpu: 9.663239625000017 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 24.89974257142689,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.897292678571453 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 0.8006835807201192,
            "unit": "ms/iter",
            "extra": "iterations: 861\ncpu: 0.8006061440185835 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 58.90433827273062,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 58.89775672727282 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 14.407997040817063,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.40709287755101 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 17.360116804877357,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.357833682926824 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 33.918532380950985,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.91602057142862 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 50.74618161538288,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 50.73520161538443 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 258.6549403333341,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 258.602561 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1177.6860489999876,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1177.5243800000012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 17.09234120000076,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.089592224999972 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 21.0409795714278,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 21.037585428571504 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 34.23669933333496,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 34.233689142857045 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 62.617835099996455,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 62.611988899999815 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 246.6006726666592,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 246.584418 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1343.4470709999573,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1343.2915879999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 32.59340809091007,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.590248136363634 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 35.68548889999761,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.683840399999944 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 50.19581330769212,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 50.19082746153841 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 97.51817014285636,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 97.50789542857115 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 309.0945460000114,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 309.0604950000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1703.1971750000139,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1702.9662809999984 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 43.82355349999835,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 43.8178525555557 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 42.11570494117509,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 42.11126294117662 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 65.30362391666245,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 65.29796475000005 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 125.53766683333833,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 125.51930883333284 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 321.9260639999959,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 321.91098749999725 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 1738.6074209999833,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1738.4445510000005 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 15.542976191489801,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 15.542138404255377 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 18.287243777777462,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 18.286300472222116 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 35.6642628000003,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.660036400000195 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 54.69085499999857,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.68782799999981 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 216.69491533333485,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 216.67090266666614 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 17.572759105261962,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 17.571929315789554 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 21.02611618749961,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 21.022799218749945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 34.887603850000914,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.88599360000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 68.53509844444261,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 68.52432222222245 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 284.0792959999969,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 284.0658546666669 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 14.51623626530586,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.513726632652949 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 17.3797826750004,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.37780089999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 36.16890399999875,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 36.162732900000094 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 69.0390232000027,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.0306636999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 152.95466675000569,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 152.9365905000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 17.504686825000704,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.503468899999852 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 24.74675350000055,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 24.7444119687501 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 37.71249827777802,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 37.70943261111112 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 78.66205987500052,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 78.65048674999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 175.26709499999527,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 175.24346050000082 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 10.10190554285779,
            "unit": "ms/iter",
            "extra": "iterations: 70\ncpu: 10.100247214285625 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 11.000046656249651,
            "unit": "ms/iter",
            "extra": "iterations: 64\ncpu: 10.981405390624976 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 13.411583921568388,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.410130490196165 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 18.277000828571285,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 18.271252171428543 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 91.6296521249933,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 91.61461749999944 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 10.979274188406489,
            "unit": "ms/iter",
            "extra": "iterations: 69\ncpu: 10.977706782608886 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 11.429095230768711,
            "unit": "ms/iter",
            "extra": "iterations: 65\ncpu: 11.428335907692363 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 14.934161791667103,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.932783395833482 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 26.876636592594085,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.872812296296637 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 162.05090624998775,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 162.04156675000192 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 10.136702901408471,
            "unit": "ms/iter",
            "extra": "iterations: 71\ncpu: 10.135796450704204 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 10.909998212121012,
            "unit": "ms/iter",
            "extra": "iterations: 66\ncpu: 10.90688839393919 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 16.241291953489572,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.23759983720931 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 30.873423052633754,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 30.863470789474203 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 220.11839074998818,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 220.08623050000153 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 10.045637492957738,
            "unit": "ms/iter",
            "extra": "iterations: 71\ncpu: 10.045157999999947 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 12.224479684211051,
            "unit": "ms/iter",
            "extra": "iterations: 57\ncpu: 12.217977771929835 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 21.734702031249498,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 21.733024406250134 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 50.54217191666529,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 50.5360170833337 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 135.96067383333357,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 135.94644683333476 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 9.79635939436673,
            "unit": "ms/iter",
            "extra": "iterations: 71\ncpu: 9.795442253521182 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 12.180041403509453,
            "unit": "ms/iter",
            "extra": "iterations: 57\ncpu: 12.178619982456327 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 23.730916233332284,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.730059166666706 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 69.6567272999971,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.64706520000021 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 149.28304659999867,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 149.24610680000114 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 13.476393301887601,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.474138547169739 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 14.269913632653456,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.26736222448971 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 18.177490666666984,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.174930846153977 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 33.88348605882223,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 33.87967776470609 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 102.11749114285696,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 102.1040565714283 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 13.920634750000168,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.919489826923071 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 14.056628760000649,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 14.05523488 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 19.958890914287103,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.957184771428565 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 52.767460571431,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 52.76221064285685 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 204.93950600000232,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 204.91856874999925 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 13.430600679245524,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.42980171698116 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 14.040314939999234,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 14.039202840000087 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 21.56822754545484,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.566943515151397 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 62.15498054545647,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 62.14550300000031 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 248.4708869999963,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 248.44798366666984 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 13.700761836734115,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 13.698742122449064 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 16.26849616279028,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.266854581395265 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 28.562856959999863,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.555740799999967 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 107.93035250000571,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 107.91248100000199 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 199.91907024999023,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 199.8868414999997 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 14.209031822223197,
            "unit": "ms/iter",
            "extra": "iterations: 45\ncpu: 14.207197622222148 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 16.765673261904197,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.763668333333097 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 29.930113826087958,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.927222913043316 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 97.00364133333512,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 96.99188966666601 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 196.77917625000418,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 196.75980749999766 ms\nthreads: 1"
          }
        ]
      }
    ]
  }
}