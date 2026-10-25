window.BENCHMARK_DATA = {
  "lastUpdate": 1792937627863,
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
      }
    ]
  }
}