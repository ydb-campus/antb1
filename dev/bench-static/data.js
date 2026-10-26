window.BENCHMARK_DATA = {
  "lastUpdate": 1792977479540,
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
      }
    ]
  }
}