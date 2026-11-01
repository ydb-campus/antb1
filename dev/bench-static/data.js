window.BENCHMARK_DATA = {
  "lastUpdate": 1793528113484,
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
          "id": "87104e4c32d4cfef311b9af19d3963fef7fcdd04",
          "message": "fix(sql): refuse a window aggregate over an aggregate as unsupported (#256)\n\n## Summary\n\n`SUM(SUM(x)) OVER (...)`, a window function over an aggregate, is valid\nDuckDB. antb1 reported the inner call as a\nsyntax error (exit code 1) before it reached `OVER`, rather than\nrefusing the window function as unsupported (exit\ncode 4). The parser now decides when it meets an aggregate call inside\nan aggregate's argument. It looks ahead,\nwithout consuming anything, to the `)` that closes the outer call:\n\n- If `OVER` follows, parsing goes on, and the `OVER` is refused after\nthe call as every `OVER` is.\n- Otherwise the parser reports the same syntax error, at the same span,\nas `main`. This also holds when the argument\ngoes on to something unsupported (`SUM(AVG(DISTINCT a))`) or to a lexer\nerror (`SUM(COUNT(a) + 0x10)`).\n\n`Take()` counts consumed parentheses for the lookahead. It runs at most\nonce per aggregate call, so parsing stays\nlinear. A lexer error that the lookahead meets is dropped, because the\ncaller then fails before it consumes that far.\n\nWhy now: this is the first step of adding the queries derived from\nTPC-DS, a ratchet like the TPC-H-derived one. That\nratchet requires every query antb1 does not answer to fail with\nUnsupported. Nine of the 99 queries use this form and\nwould fail it with a parse error. They are referred to by number only\n(ADR 0006); nothing derived from TPC-DS is in\nthis PR.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [x] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run test -R '^sql\\.'\nANTB1-TESTS: PASS preset=dev\n\n$ pixi run ci\n100% tests passed out of 2948\n\n$ pixi run lint\nevery check passes except shellcheck: SC2218 only, in tools/github/apply-settings.sh, scripts/agent-setup.sh and\nscripts/static-sources.sh, which this PR does not touch (macOS shellcheck; the same on main on this host)\n\n$ clang-tidy -p build/tidy --warnings-as-errors='*' src/sql/parser.cc src/sql/tests/parser_test.cc\nno findings\n```\n\n- The expected outcome of every new reject case was checked against\nDuckDB 1.5.6: it accepts the window-over-aggregate\nforms and reports \"aggregate function calls cannot be nested\" for the\nnested ones.\n- Parse time on `SUM(a IN (COUNT(a), ... n times)) OVER ()` is linear:\n0.23 s, 0.39 s and 0.74 s for n = 20k, 40k\n  and 80k (dev build, whole CLI run).\n- Local, unpublished check: on the TPC-DS-derived queries (generated\noutside the repository, output redacted), the\nnine queries now exit with Unsupported. A ratchet of today's 29 passing\nqueries passes at SF 0.01, 0.1 and 1, and on\n  4 threads against 1.\n- **Not run locally** (macOS host): `pixi run asan`, `pixi run ci-gcc`,\nand `pixi run tidy` over the whole tree. CI\n  runs them.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint passes apart\n      from the macOS-only shellcheck findings listed above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed): 12 new reject cases\n      in `src/sql/tests/parser_test.cc`\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed:\n      `docs/sql-subset.md`\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: none touched\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code found the issue\nin a local survey of the TPC-DS-derived\nqueries, wrote the change and the tests, and ran the gates. Two rounds\nwith a read-only reviewer agent found\n      three real problems, all fixed before this PR:\n- deferring the error let an unsupported construct later in the argument\nwin (exit 4 instead of 1);\n- the lookahead could surface a lexer error in place of the syntax\nerror;\n      - one lookahead per nested call made parsing quadratic.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T02:25:10+03:00",
          "tree_id": "7f40adfdd20a7b4806ce45a8f9150cd51caefba7",
          "url": "https://github.com/ydb-campus/antb1/commit/87104e4c32d4cfef311b9af19d3963fef7fcdd04"
        },
        "date": 1793317082260,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4742.706394737086,
            "unit": "ns/iter",
            "extra": "iterations: 147293\ncpu: 4742.067117921421 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 28709.470691748873,
            "unit": "ns/iter",
            "extra": "iterations: 23867\ncpu: 28703.60367871957 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52409.11805917603,
            "unit": "ns/iter",
            "extra": "iterations: 13417\ncpu: 52393.91190280985 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 235237.29529760565,
            "unit": "ns/iter",
            "extra": "iterations: 3041\ncpu: 235226.45741532402 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 496359.0184396988,
            "unit": "ns/iter",
            "extra": "iterations: 1410\ncpu: 496181.443262411 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1486366.7710582982,
            "unit": "ns/iter",
            "extra": "iterations: 463\ncpu: 1486234.773218142 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 39.33951722222181,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.33460849999997 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.09825646666741,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.09300759999999 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 186.56670966665465,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 186.54125333333363 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.93621115999963,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.934888060000006 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 24.8396724285718,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.835706535714255 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 36.206794684209115,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.20483168421049 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.3468533918918573,
            "unit": "ms/iter",
            "extra": "iterations: 518\ncpu: 1.3467893223938228 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 95.92152585714432,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.91635914285719 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.688915774194225,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.687879096774186 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.54181634482799,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.54093024137929 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 52.19668530769293,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.195362230769156 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 74.87452288888916,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 74.8677194444443 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 253.95333433332704,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 253.9394633333328 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1260.5711269999915,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1260.473601000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.467168230769037,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.46481246153848 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 30.825837434782027,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.82408408695653 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 53.62610807692538,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.623062153845936 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 92.33905600000624,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 92.33460485714318 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 313.28608850000705,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 313.280047000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1493.9703640000062,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1493.8814739999984 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.101237500001844,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.09736516666667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 59.24956233333489,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.247493083333325 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 84.67920624999437,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.67531574999975 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 145.0175483999942,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 145.0017301999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 408.5595195000167,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 408.53035299999976 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1833.1535260000464,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1833.0353509999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.49117572727296,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.48791318181848 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.94516469999985,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.94219100000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 96.72939771428446,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.72235257142907 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 164.56688175000522,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 164.55558224999933 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 427.18660349999027,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 427.1525714999989 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2015.1886880000234,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2015.0677099999966 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 22.82173486206955,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 22.82079920689641 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 24.529047068965767,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.52805131034489 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 52.57631900000293,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.57312376923114 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 76.81416755555246,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 76.8060512222222 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 258.0597150000017,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 258.0400103333318 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 27.542305960000704,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.541174480000162 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 30.720800260869805,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.719297913043608 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 53.801019307693075,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.796720307692375 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 92.14644885714408,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 92.13914714285717 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 314.64778500000534,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 314.63275349999975 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 22.81186003225714,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.810153419354933 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 24.931101821426946,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.92977432142851 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 59.49110141666836,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.487421916666484 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 92.35521771427914,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 92.34709999999983 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 194.30864449999774,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 194.29431799999895 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 27.34185651999951,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.340255759999938 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 33.648818849999884,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 33.64548985000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 61.23048218181669,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 61.22660390909054 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 118.03872933333537,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 118.02774533333367 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 228.72298366665214,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 228.7090723333331 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.112470585365227,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.11159180487802 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.548891921052533,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.547847105263465 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 23.25172170000087,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.250874466666478 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 29.75220013043465,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.750263652174354 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 96.71711500000616,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.71297957142956 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.221110341463316,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.220365414634283 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.40830342105224,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.407753921052677 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 27.030264230769664,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.02927380769237 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 48.93759349999982,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 48.93126149999968 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 228.42766133332285,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 228.39714399999878 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.39607924999973,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.389839850000044 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.74493262162074,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.743096594594565 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 28.40307952000103,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.39992931999973 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 52.172782454544055,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 52.16989927272664 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 233.90276866666682,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 233.89605833333368 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.287246024389933,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.28620446341445 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 21.39889875757655,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.397795575757847 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 36.33485915789654,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.33119605263116 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 55.65664360000255,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 55.653681699999424 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 139.52154319999863,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 139.4988466000001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.516920512819596,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 17.515143794871808 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 22.02114968750024,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 22.01846765624982 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 37.32433689473761,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.322654263158185 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 53.71112272727239,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 53.70371509090949 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 142.63949999999568,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 142.6238082000026 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 23.48101296666793,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.478359233333396 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 24.863244928572012,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.86046089285716 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 29.823516173913188,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.82074126087011 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 48.24763030769383,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 48.24139330769128 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 119.10880800000238,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 119.09293616666616 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 23.50947713333274,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.50707310000028 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 25.127081444443995,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 25.124141074074288 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.6966234000007,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.69311260000012 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 71.51752722222051,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 71.50795911111156 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 262.6749523333274,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 262.64826900000077 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 22.859811290321694,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.856761548387148 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 24.501431344827417,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.50017548275843 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 34.93749575000038,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.934710750000164 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 74.59353922222236,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 74.58977833333336 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 276.22632266667324,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 276.204823333335 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 24.661498107144148,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.65976225000028 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 32.94412738095042,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.940502285714366 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 45.02451381249983,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 45.019512125000105 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 85.64001437499513,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.62757125000076 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 213.6486907500057,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 213.62453575000018 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 23.762637000000442,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.759772206896276 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 31.892567772729308,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.889682272727462 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 45.14838213333405,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.14496733333336 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 85.96429433332749,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 85.95798666666592 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 218.85739999999032,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 218.83852366666665 ms\nthreads: 1"
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
          "id": "6d51c3f34774d437c3178a1901066d7d5138fa20",
          "message": "build(deps): add duckdb-extension-tpcds for TPC-DS-derived test data (#257)\n\n## Summary\n\nAdds the conda-forge package `duckdb-extension-tpcds` (DuckDB's tpcds\nextension, which contains TPC's dsdgen under the\nTPC EULA) as the source of the data derived from TPC-DS, the third\nworkload. This is the first harness PR of the\nTPC-DS work and mirrors #71 for TPC-H. #256 was its prerequisite: every\nquery antb1 does not answer must fail with\nUnsupported.\n\n- **pixi.toml and pixi.lock.** `duckdb-extension-tpcds = \"1.5.*\"` sits\nnext to `duckdb-extension-tpch` in feature\n`base`, on Linux only (linux-64, linux-aarch64), for the reason already\nrecorded for tpch: pixi cannot install the\nosx-arm64 build. `pixi lock` (pixi 0.81.0) adds only the extension,\n1.5.6, nothing else moves:\n- linux-64 `hd2095e1_0`, which depends on the locked libduckdb\n`hcfde7ca_0`;\n- linux-aarch64 `h4154aff_0`, which depends on the locked `he694f2e_0`.\n\nThe extension is in the `default`, `gcc` and `static` environments, the\nsame places as tpch.\n- **cmake/Antb1Dependencies.cmake.** It finds exactly one tpcds\nextension in the prefix that holds libduckdb and sets\n  `ANTB1_HAVE_TPCDS`. It is a line-for-line mirror of the tpch block:\n- with `ANTB1_WITH_DUCKDB=ON`, configure fails on Linux when the\nextension is missing;\n  - on macOS it prints a status line and registers no TPC-DS tests.\n- **tests/tpcds/ (new): `harness.tpcds.extension`.** The smoke test runs\nwith the same lockdown as\n`harness.tpch.extension`: `-unsigned -no-init`, one thread, no\nauto-install or auto-load, LOAD by absolute path. It\nchecks that the extension provides exactly 99 query texts and prints\nonly the count.\n- **scripts/doctor.sh.** It reports `tpcds extension` in the text output\nand `tpcds_extension` in the JSON output.\n- **ADR 0006 amendment, \"Data policy for the data derived from\nTPC-DS\".** The TPC-H policy applies, with these\n  differences:\n- **No stored answers.** In 1.5.6, `tpcds_answers()` covers SF 1 and 10,\nhas answers only for 0.01 and 1, and builds\nevery row at once, so each scan fails with \"Don't have TPC-DS answers\nfor SF 10\". The live DuckDB oracle on the\n    same Parquet files is the only judge.\n- **The corpus is the pinned `tpcds_queries()`.** A change to it, such\nas DuckDB 2.0's TPC-DS v4 texts, is a new\n    baseline.\n  - **Its own naming and disclaimer**, and no TPC-DS metrics.\n\n  Maintainer decisions recorded, by @hor911 on 2026-10-30:\n- accept the dsdgen EULA exposure of installing the Linux environments,\nas for dbgen;\n- allow timings of the TPC-DS-derived queries in the same public places\nas the TPC-H-derived ones.\n- **Wording.** CONTRIBUTING.md, the static package README\n(`scripts/static_package.py`), ADR 0034, the\ndependency-update skill and docs/testing.md and tests/README.md now name\ndsdgen next to dbgen.\n\nLater PRs, per the plan:\n- lint and governance for TPC-DS material: R009 patterns for dsdgen\nartefacts, the status file as a governance path,\n  TPC-DS in AGENTS.md rule 3 and the review prompts;\n- fixture generation;\n- the `antb1-slt tpcds` ratchet, starting at the 29 queries that pass\ntoday.\n\nNo TPC-DS-derived text, data or answers are in this PR, and none will be\nin later ones.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi lock\n+ (conda) duckdb-extension-tpcds 1.5.6 hd2095e1_0 (linux-64: default, gcc, static), h4154aff_0 (linux-aarch64: default, static)\npixi.lock | 30 +++ (nothing else changes)\n\n$ pixi run test -R '^harness\\.tpc'        # macOS, extension placed by hand as for tpch\nharness.tpcds.extension ... Passed\n\n$ pixi run doctor\ntpcds extension present (.../.pixi/envs/default/duckdb/extensions/v1.5.6/osx_arm64/tpcds.duckdb_extension)\n\n$ pixi run ci\n100% tests passed out of 2949\n\n$ pixi run lint\nevery check passes except shellcheck: SC2218 only, in tools/github/apply-settings.sh, scripts/agent-setup.sh and\nscripts/static-sources.sh, which this PR does not touch (macOS shellcheck; the same on main on this host)\n```\n\nTwo things are verified only on macOS:\n\n- **The conda package's install.** This macOS host cannot install the\nLinux package. Here, the smoke test ran with\nDuckDB's signed osx_arm64 tpcds 1.5.6 extension placed by hand in the\nenv, the same way TPC-H runs on this host.\n  The Linux CI legs install and load the conda-forge build itself.\n- **The other gates.** `pixi run asan`, `ci-gcc` and `tidy` were not run\nlocally; CI runs them.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint passes apart\n      from the macOS-only shellcheck findings listed above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed): `harness.tpcds.extension`\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H is committed: no query text or\nfragments, data, answers or TPC tools (ADR-0006); nor\n      anything derived from TPC-DS\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: pixi.toml, pixi.lock,\ncmake/, docs/adr/, .agents/ and .claude/ (skill copy), approved by\n@hor911 on 2026-10-30\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code confirmed that\nthe package's builds match the locked libduckdb\nand ran a local survey of the TPC-DS-derived queries. The survey printed\ncounts and query numbers only, and its\noutput stays outside the repository. Claude Code then wrote the change\nand ran the gates. A read-only reviewer\nagent found no correctness problems. It suggested a more precise ADR\nwording about `tpcds_answers()`, applied,\nand flagged the two policy decisions above for explicit maintainer\nconfirmation.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T02:59:41+03:00",
          "tree_id": "c4c5e7c775b9340b7b85a5065ccf8fb7a3d1de7e",
          "url": "https://github.com/ydb-campus/antb1/commit/6d51c3f34774d437c3178a1901066d7d5138fa20"
        },
        "date": 1793319673224,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4720.894683013229,
            "unit": "ns/iter",
            "extra": "iterations: 148618\ncpu: 4720.6101212504545 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 37961.38428231235,
            "unit": "ns/iter",
            "extra": "iterations: 18476\ncpu: 37958.53474778092 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52271.93443608956,
            "unit": "ns/iter",
            "extra": "iterations: 13300\ncpu: 52269.08819548873 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 233250.60159893762,
            "unit": "ns/iter",
            "extra": "iterations: 3002\ncpu: 233238.82511658882 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 496354.8962331235,
            "unit": "ns/iter",
            "extra": "iterations: 1407\ncpu: 496331.30277185503 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1500642.6630434261,
            "unit": "ns/iter",
            "extra": "iterations: 460\ncpu: 1500552.1934782614 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.33698782352664,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.33521758823527 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.432182733331196,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.42949000000001 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 207.79350033334745,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 207.78057466666647 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.982406239999818,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.981048899999992 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 26.06575470370579,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.064129925925883 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 37.80279842105397,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.799363263157915 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.490721688034107,
            "unit": "ms/iter",
            "extra": "iterations: 468\ncpu: 1.4906460876068361 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 132.66946080000253,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 132.65278380000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.96551303225941,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.963740838709708 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.84849635714324,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.846494214285705 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 53.04683215384697,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.04291492307693 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 81.87225311111332,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 81.8665615555554 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 281.15296499998976,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 281.13624999999945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1355.7050299999673,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1355.6045780000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.57356928000263,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.572335159999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.458114318184016,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.454526818181737 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 54.472635461540726,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.46896676923065 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 103.48494028572271,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 103.47475157142836 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 341.6984445000253,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 341.6761064999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1661.6765199999008,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1661.5531040000028 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.435624166662514,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.43134133333334 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 59.41076641666617,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.40434075000015 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 85.57274174999918,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.56049749999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 154.31979025001397,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 154.30444574999936 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 431.6739294999934,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 431.64155999999923 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 2022.4749569999858,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2022.3066919999937 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 66.0208982727268,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 66.01662400000018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 69.72026449999476,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.71048590000066 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 99.43932185714337,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 99.427692428572 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 176.450417500007,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 176.43629099999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 459.8849705000134,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 459.83729900000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2192.843119000031,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2192.651640000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 23.151528733334697,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.14980126666673 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 24.871901892855217,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.86953653571433 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 53.74627323076835,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.74173438461527 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 83.65289924999786,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 83.6473293749993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 281.78763099998605,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 281.74892699999884 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 27.846582600000147,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.843821319999908 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 31.51625395454713,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.513027318182004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 55.15104291667209,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.14600883333346 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 100.74813114285267,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 100.73584585714254 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 344.092577500021,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 344.074753000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 22.93508389999867,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 22.93290499999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 25.236946464283555,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.235375928571408 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 58.430251666663935,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.42256441666649 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 105.53878916666311,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 105.52956216666705 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 234.3878853333384,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 234.3599543333331 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 27.562852559999556,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.560260920000133 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 34.59613275000493,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.59066849999992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 62.37605599998956,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 62.372475272727385 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 135.729041400009,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 135.70708480000064 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 258.3176566667286,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 258.29185800000215 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.193892585365997,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.191949170731878 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.313119657896575,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.312221368420836 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 24.348584689654484,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.346791241378956 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 32.21516980953051,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.211658666666736 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 108.45927842858926,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 108.44969614285763 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.160793195122405,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.159998292683206 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.60185447368454,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.600204342105467 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 27.473653519991785,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.472430799999756 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 49.91956121429147,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.915035857142904 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 243.8292870000017,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 243.80095766666918 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.379773900000828,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.378841400000056 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.886604675676484,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.885734648649006 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 28.797095500001053,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 28.794507833333444 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 52.877898499991716,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 52.872958100000744 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 247.34257600001305,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 247.33757499999834 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.29276692682971,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.291031170731856 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 22.444899000000138,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.443756451612593 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 37.34795457894267,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.34407026315753 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 52.94035063634924,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 52.936345363637436 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 150.4268026000318,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 150.40756460000182 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.55333919999771,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.551856425000167 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 22.890192966671446,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 22.888311866666793 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.204008055559925,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.20219077777804 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 56.248832363632,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 56.242017909091466 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 149.33941799999957,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 149.32485219999876 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 23.483061366672093,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.47957230000001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 24.93734707142689,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.93611503571407 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 30.338297913045686,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.335426869564973 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 49.33984933334538,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 49.33509933333369 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 124.65338819997669,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 124.63679239999976 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 23.45402316666423,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.45251693333334 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 25.45729139285413,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.454522285714226 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.95246659999793,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.950915399999616 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 72.96765622224181,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 72.95947088888965 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 273.18725133333527,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 273.16344500000395 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 22.61747764516493,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.615451612903456 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 24.61946578571477,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.617979785714134 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 35.14299045000371,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.13877795000013 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 78.02134800001821,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 78.01578555555484 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 290.9170745000438,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 290.8859869999958 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 24.375749206902928,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.374418413793133 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 32.95585390476065,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.953395904761976 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 45.683472866661155,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.67940086666624 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 88.56280328570521,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 88.55799542857109 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 224.68195200000687,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 224.66675766666336 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 23.471016533335387,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.470012899999897 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 32.297436590903835,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.294702545454804 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 45.76032066667418,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.75590360000016 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 95.09849733331066,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 95.08430283333287 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 230.47957000001892,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 230.4652196666647 ms\nthreads: 1"
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
          "id": "c8c0b993f373dd1d1a86e2238c202b6fbd382491",
          "message": "feat(plan): constant integer expressions in IN lists (#261)\n\n## Summary\n\nAn `IN` list value may now be a constant integer expression, written as\ninteger literals under unary `-`, `+`, `-` and\n`*` (`c IN (10, 10 + 1)`). A side of a comparison and a `BETWEEN` bound\nalready may be one.\n\n- **Binding.** The binder folds such a value to the literal of its value\nin DuckDB's types (`FoldConstant`, as for\ncomparisons), then types and folds it like any other value of the list.\nSo an out-of-range value is dropped, and a\nDOUBLE-typed number still makes the whole list DOUBLE. An overflow is\nthe comparison's bind error (divergence D17,\nnow also listed for `IN`). Other non-literal values stay unsupported\n(exit code 4), with a message that names both\n  kinds.\n- **OR factoring fix: a wrong answer this change made reachable.**\n- `ValuesOf` took the literals that came before the first non-literal\nvalue of an `IN` list and then gave up, but\nleft those literals behind. It then derived an implied `IN` list that\ndropped rows.\n- Example: `i16 IN (1, 1 + 6) OR i16 = 100` returned the rows for 1 and\n100 but not the row for 7.\n- The path was unreachable while `IN` lists held literals only. A list\nwith a non-literal value now contributes no\n    values at all.\n- The reviewer found it. A unit test and three `.slt` records pin it,\nand all of them fail without the fix.\n- **TPC-DS ratchet.** Queries 29, 46, 68 and 79 derived from TPC-DS need\nonly this and now pass: the ratchet goes from\n29 to 33. Their table rows are updated, and this need is removed from\nthe five other rows that listed it.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run test -R '^(slt|oracle)\\.in\\.'       # new records written by pixi run slt-complete from DuckDB\n100% tests passed out of 5\n\n$ git stash push src/plan/or_factoring.cc && pixi run test -R '^(slt\\.in\\.in|plan\\.OrFactoringTest\\.DerivesNothing.*)$'\n2 tests failed out of 3                         # the regression tests catch the bug without the fix\n\n$ pixi run test -R 'tpcds\\.status'\ntpcds.status.sf0_1, tpcds.status.sf1, parallel.tpcds.status.sf0_1: Passed   (33 queries pass)\n\n$ pixi run ci\n100% tests passed out of 2963\n\n$ pixi run lint\nevery check passes except shellcheck (SC2218, macOS only, in three scripts this PR does not touch)\n\n$ clang-tidy -p build/tidy --warnings-as-errors='*' src/plan/binder.cc src/plan/or_factoring.cc src/plan/tests/binder_test.cc src/plan/tests/or_factoring_test.cc\nno findings\n```\n\nNot run locally (macOS host): `pixi run asan`, `ci-gcc` and the\nwhole-tree `tidy`.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed):\n- `plan.BinderTest.InListFoldsConstantIntegerExpressions`: a folded list\nbinds like the literal one, in WHERE and\n    HAVING, with a DOUBLE list, out of range, and on overflow;\n- error cases for constants that do not fold, overflow and more than 38\ndigits;\n  - the OR-factoring case;\n  - `.slt` records in `tests/slt/cases/in/in.slt`.\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed:\n      docs/sql-subset.md (summary, conditions, IN semantics, D17)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n(ADR-0006). The example values were written from scratch, and the\nreviewer asked to replace an earlier set that\n      looked like the TPC-DS lists.\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `tests/data/tpcds_status.json`\n(+29, 46, 68, 79) was edited with @hor911's per-edit approval; please\nconfirm it in review\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand its tests and ran the gates. A read-only\n      reviewer agent found:\n  - **P0:** the OR-factoring wrong answer above, now fixed and pinned;\n- **P1:** the missing error-path tests (added), the D17 row (updated),\nand example values that resembled the\n    TPC-DS-derived lists (replaced).\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T16:12:52+03:00",
          "tree_id": "3cb7e3b794f7977046e5ae9b192f91efb875dfd5",
          "url": "https://github.com/ydb-campus/antb1/commit/c8c0b993f373dd1d1a86e2238c202b6fbd382491"
        },
        "date": 1793366646724,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3838.214942345952,
            "unit": "ns/iter",
            "extra": "iterations: 182207\ncpu: 3838.3759679924474 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 46004.019193736945,
            "unit": "ns/iter",
            "extra": "iterations: 15578\ncpu: 46007.449159070486 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 87339.36354618627,
            "unit": "ns/iter",
            "extra": "iterations: 8065\ncpu: 87340.57805331677 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 228898.02107653042,
            "unit": "ns/iter",
            "extra": "iterations: 3084\ncpu: 228912.97568093392 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 455586.21774701355,
            "unit": "ns/iter",
            "extra": "iterations: 1589\ncpu: 455611.8842039021 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1252197.8698751808,
            "unit": "ns/iter",
            "extra": "iterations: 561\ncpu: 1250139.3368983956 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 44.87437493333649,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 44.87796119999997 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 43.0625238124982,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.0674981875 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 185.07005666666979,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 185.08435433333344 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 12.581139017857115,
            "unit": "ms/iter",
            "extra": "iterations: 56\ncpu: 12.582285000000011 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 18.866400166667557,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 18.866368194444462 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 35.48367915789485,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 35.486124578947404 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 2.01528198270894,
            "unit": "ms/iter",
            "extra": "iterations: 347\ncpu: 2.01531167723343 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 33.255675190476026,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.25848752380952 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 11.533001779660758,
            "unit": "ms/iter",
            "extra": "iterations: 59\ncpu: 11.532803491525407 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 13.305233566038108,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.305130283018839 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 35.9916319000007,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.99293864999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 61.638592600002085,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 61.641540100000114 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 178.71181425002192,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 178.71043700000033 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1003.472309000017,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1003.5439850000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 14.460165382978571,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.460215340425508 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 17.923979200000417,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.923550425000023 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 36.50238344444764,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 36.500998000000074 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 74.78168300000713,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 74.78808755555575 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 231.2411530000039,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 231.24205833333278 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1246.4804040000672,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1246.595555999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 46.24967460000183,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.25337899999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 49.122335416673955,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 49.12354591666664 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 76.4543198888911,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 76.46110522222216 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 123.67901460002031,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 123.68062900000041 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 330.2865780000275,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 330.2862014999981 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1738.1099399999584,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1738.2172849999974 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 56.311523583332246,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.31683100000018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 58.37105125000145,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.372680750000086 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 89.96460814286625,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 89.9644995714287 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 156.75212249999504,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 156.75976900000066 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 393.23087249999844,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 393.253069 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2004.3718030000264,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2004.4843069999984 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 11.337272419354811,
            "unit": "ms/iter",
            "extra": "iterations: 62\ncpu: 11.338019741935518 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 13.081350403845923,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.082335961538405 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 36.26792873683969,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.269435789473675 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 60.79644269999562,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 60.799505700000374 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 185.48541674999797,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 185.48426300000108 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 14.706084799998584,
            "unit": "ms/iter",
            "extra": "iterations: 45\ncpu: 14.706940155555673 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 17.515186857143103,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 17.515784833333402 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 37.35751194444169,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 37.3593891111111 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 75.62016399999214,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 75.621350111111 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 244.34384099996956,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 244.35166566666547 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 11.516681413793556,
            "unit": "ms/iter",
            "extra": "iterations: 58\ncpu: 11.516503017241465 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 13.640140346154794,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.640312346153857 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 44.02405399999765,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 44.024928500000286 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 71.868707874998,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 71.87105875000022 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 140.48756160000266,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 140.49199520000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 14.966745568182722,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 14.967188318181808 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 18.812769371428267,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 18.81330897142866 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 47.51229153333346,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 47.51439453333338 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 89.31487228570793,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 89.31115128571423 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 167.48869200000627,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 167.488864500001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 14.513625595743585,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.513452574468094 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 16.05942625581541,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.060788767442034 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 23.052171399998162,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.053334700000033 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 31.974535350002498,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 31.974646150000297 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 74.83712812499732,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 74.8418776249995 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 14.50088367346789,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.501568816326618 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 15.991978363636214,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.9921460681819 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 24.693621689654467,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.694440586207303 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 54.27156409999725,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 54.27244640000026 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 210.64573500001416,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 210.646928500001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 14.192888020002101,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 14.193391320000046 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 15.850097681819038,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.850037068181939 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 26.789503185185552,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.790613185185023 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 55.63516333333496,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.63779833333271 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 266.3885100000319,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 266.38717033333137 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 13.976533857142998,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 13.976407836734921 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 17.436822341462573,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.436762658536786 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 38.810896666663865,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.81400111111175 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 57.497118666664925,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 57.49648683333319 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 125.12339140000677,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 125.13102239999796 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 13.611735529413155,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.612862941176543 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 17.600481950000813,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.601939349999896 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 40.48093305882236,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.481537705882516 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 59.76405866666331,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.76611641666688 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 139.45195639998929,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 139.45160279999982 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 16.684682813952136,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.685729348837267 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 18.169567564101296,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.16955858974358 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 28.578683399996407,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.580370959999755 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 51.00159075000003,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 51.003367750000216 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 103.00064883333941,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 103.0071346666664 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 16.757752194444542,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 16.758408444444466 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 18.42181182051333,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.422495974359066 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 32.9983873636324,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.99859872727301 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 75.64878500001271,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 75.65179500000063 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 230.61595766667628,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 230.6373203333294 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 16.679785142858215,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.68013597619038 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 18.672045135134965,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.673351540540637 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 32.528693380955005,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.53135533333374 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 69.03804662499624,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 69.04433949999955 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 276.2889313333214,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 276.2892959999969 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 17.118885487806015,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.11987507317065 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 20.21629426470848,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.217625294117422 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 47.222800250002216,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 47.22124899999969 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 82.63644920000388,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 82.63839299999916 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 190.2155122500062,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 190.22863400000034 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 16.56574952381193,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.565505809523984 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 21.80166212500012,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 21.801638374999932 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 56.82976219999318,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 56.82917049999929 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 82.4080282500006,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 82.40933875000067 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 226.22177849999048,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 226.22132849999943 ms\nthreads: 1"
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
          "id": "2cf38bb7bfe8b81f312641c6b7a80390209825dd",
          "message": "feat(sql): read ELSE NULL as a CASE without ELSE (#262)\n\n## Summary\n\n`CASE ... ELSE NULL END` now parses as the `CASE` without its `ELSE`. In\nDuckDB the two forms are the same thing too:\nDuckDB gives a `CASE` without `ELSE` the same NULL constant that an\nexplicit `ELSE NULL` becomes. So the change keeps\nDuckDB's answers in every respect:\n\n- the NULL for each row no branch takes;\n- the type, which folds from that NULL, so divergence D20 and ADR 0021's\nrule 10 behave as for no `ELSE`;\n- the name `... ELSE NULL END`, which antb1 already wrote for a `CASE`\nwithout `ELSE`.\n\nThe change is in the parser only, in `ParseCase`: a bare `ELSE NULL`\ndirectly followed by `END` leaves `otherwise`\nempty. NULL anywhere else is still an unsupported NULL literal (exit\ncode 4): as a `THEN` value, as an operand of the\n`ELSE` value (`ELSE NULL + 1`, `ELSE NULL::INT`) or in parentheses.\n\nTPC-DS ratchet: queries 31, 34, 43, 59 and 73 derived from TPC-DS use\nNULL only this way and now pass, so the ratchet\ngoes from 33 to 38. In the status table, nine other queries no longer\nlist \"NULL literals\" among their needs. Only\n27, 36, 39 and 90 still do: the survey found NULL as a `THEN` value or a\nselect item in exactly those four.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run test -R '^sql\\.'\n100% tests passed out of 779\n\n$ pixi run test -R '(slt|oracle)\\.expressions\\.case'   # new records written by pixi run slt-complete from DuckDB\n100% tests passed out of 3\n\n$ pixi run test -R 'tpcds\\.status'\ntpcds.status.sf0_1, tpcds.status.sf1, parallel.tpcds.status.sf0_1: Passed   (38 queries pass)\n\n$ pixi run ci\n100% tests passed out of 2967\n\n$ pixi run lint\nevery check passes except shellcheck (SC2218, macOS only, in three scripts this PR does not touch)\n\n$ clang-tidy -p build/tidy --warnings-as-errors='*' src/sql/parser.cc src/sql/tests/parser_test.cc\nno findings\n```\n\nNot run locally (macOS host): `pixi run asan`, `ci-gcc` and the\nwhole-tree `tidy`.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed):\n- `sql.ParserTest.ElseNullIsACaseWithoutElse` checks the same tree and\nthe same `ToSql` in the select list, in\n    aggregates, in WHERE and for the simple form;\n- three reject cases cover `THEN NULL`, `ELSE NULL + 1` and `ELSE\n(NULL)`;\n  - `.slt` records over NULL-bearing fixtures.\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed:\n      docs/sql-subset.md (grammar, unsupported list, CASE)\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `tests/data/tpcds_status.json`\n(+31, 34, 43, 59, 73) was edited with @hor911's per-edit approval;\nplease confirm it in review\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code wrote the change\nand its tests and ran the gates. A read-only\n      reviewer agent:\n- checked DuckDB's transformer and found no semantic, typing or naming\ndifference;\n- checked the parser's lookahead and the edge cases above, and the\nround-trip and fuzz properties;\n- ran 789 related tests and 245 fuzz-replay, CLI and metamorphic tests;\n- found only the protected-file approval and a misplaced test comment\n(fixed).\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T18:35:58+03:00",
          "tree_id": "8eacc29a389730283450dd6b5f0746886c2f9409",
          "url": "https://github.com/ydb-campus/antb1/commit/2cf38bb7bfe8b81f312641c6b7a80390209825dd"
        },
        "date": 1793375300777,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4478.9190298987605,
            "unit": "ns/iter",
            "extra": "iterations: 155860\ncpu: 4478.2706916463485 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 32656.528734560314,
            "unit": "ns/iter",
            "extra": "iterations: 21455\ncpu: 32652.18639011885 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 58978.524664811615,
            "unit": "ns/iter",
            "extra": "iterations: 11859\ncpu: 58976.8268825365 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 238107.44160707272,
            "unit": "ns/iter",
            "extra": "iterations: 2937\ncpu: 238100.94484167523 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 481528.6359189168,
            "unit": "ns/iter",
            "extra": "iterations: 1431\ncpu: 481498.3060796645 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1395894.4741036014,
            "unit": "ns/iter",
            "extra": "iterations: 502\ncpu: 1395771.8525896424 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.71157011764746,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.70717217647058 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 48.419301199999154,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 48.41670299999995 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 204.2028303333344,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 204.17754699999986 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.757103125000745,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.75571535416668 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 18.038564923075775,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.036988333333323 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 38.929724666666566,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.927263944444434 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.383122921104522,
            "unit": "ms/iter",
            "extra": "iterations: 507\ncpu: 1.3829211065088753 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 63.885741545458465,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 63.87568927272725 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 24.844533857142487,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.840537428571427 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 27.05838984615219,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.054276115384603 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 56.28620941666895,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.27795916666667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 84.42178025000402,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.41150312500012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 273.73438133332684,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 273.69382966666694 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1378.3343489999993,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1378.1861370000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 29.66603287499936,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.663126124999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 33.213239904761224,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.209402809523866 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 58.39737841666685,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.390429499999875 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 103.18676100000143,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 103.17769028571432 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 329.37665750000633,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 329.3391230000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1609.3633530000488,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1609.116986 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 55.1195497692316,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 55.11233938461539 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 62.134853583335335,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 62.129751833333614 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 86.89894049999936,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 86.88802812500018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 155.37676839999222,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 155.34804939999987 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 420.20748049998247,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 420.1623909999981 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1954.254418000005,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1954.0665419999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.08748429999969,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 65.08160759999981 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.18189599999869,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.17813050000012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 99.87194357142926,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 99.85926085714222 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 174.87821825000083,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 174.8611592499998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 448.2019520000051,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 448.16806049999866 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2102.8518640000016,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2102.681311000005 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 25.355172250000027,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.35339250000008 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 27.891217538461014,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.88843973076911 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 56.73533008333701,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.730844083332954 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 88.91174737500052,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 88.90757799999972 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 280.1597060000063,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 280.13833449999834 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 30.203780652174295,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.200598956521922 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 33.21050376190388,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.20828361904761 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 58.717773250000015,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.71327000000016 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 108.54661657142677,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 108.53924228571441 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 328.52971849999335,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 328.5007554999986 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 24.8344308928584,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.83320792857135 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 28.04760299999998,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.045667640000147 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 62.857959272724365,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 62.85252445454535 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 104.38687957143397,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 104.37527285714258 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 213.40960533332995,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 213.39812466666785 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 29.853459347825027,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.851523739130332 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 35.410244649997935,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.405748499999845 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 67.5339111000028,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 67.5231776000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 130.0755105999997,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 130.06618040000149 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 244.34129166667162,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 244.31585100000083 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.032456487804556,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.030864658536522 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.10055020512763,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.09852405128181 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 23.508684655171002,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.507384310344857 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 30.41302434782823,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.41013208695631 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 106.38144366666324,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 106.37059716666641 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.40827321951218,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.406857000000215 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.269129131579717,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.267840184210485 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 26.45592626923041,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 26.452656076923102 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 43.95476737499848,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.95150199999964 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 218.880697000003,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 218.81968699999987 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 16.96913539024358,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 16.968414609756103 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.319780552631254,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.3185248157894 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 27.925817919999645,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.92361844000027 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 51.26173429999881,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 51.25470770000078 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 252.68054166667753,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 252.66198433332931 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.240886268292265,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.239285853658362 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 20.711532382354306,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.704502264705713 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 38.28170049999989,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.277608111111284 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 63.305033799997545,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 63.29884290000081 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 156.722911199995,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 156.70243759999778 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.405727756098013,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.39920390243884 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 20.796137147058023,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.79424852941175 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.84366450000066,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.84131988888918 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 61.1856328999977,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 61.17854200000039 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 154.48648900000705,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 154.4755207499975 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 23.65587379310243,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.6539486896556 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 24.87153260714235,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.869789464285624 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 31.239675086955348,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 31.235298652174244 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 82.04338624999963,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 82.0251486250001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 138.86485325001274,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 138.84228374999807 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 23.694778899999847,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.691564666666903 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 25.080202428571038,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.075789357143005 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.922672350000994,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.91899564999983 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 74.15251022222138,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 74.14110833333294 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 258.87749633333596,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 258.84238666666687 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 23.681750233333787,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.678837033333384 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 25.225112535713784,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.223018142857054 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 36.59327584210515,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.58920847368413 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 82.25381677777932,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 82.24713600000024 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 292.47084499999687,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 292.44365050000454 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 24.579168793103424,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.577384482758337 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 29.37553475000006,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.373339750000227 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 50.074617071430794,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 50.07067607142903 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 100.86337699999604,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 100.85441357142803 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 221.9073939999987,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 221.88370025000026 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 24.624998714285443,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.61389321428545 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 29.241807625001098,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.238653458332802 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 49.94919930769234,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 49.94328323076922 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 108.36885133333378,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 108.36366849999972 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 237.1420196666539,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 237.12048433333166 ms\nthreads: 1"
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
          "id": "5f0996c73341d701883e67180f1be88f738dba83",
          "message": "feat(plan,exec): an n-ary union all node and its operator (#263)\n\n## Summary\n\nThis is the first of three PRs for UNION ALL ([ADR\n0036](docs/adr/0036-union-all.md), new): the plan node and its\noperator, with no SQL change.\n\n- **What comes next.** The second PR adds the grammar (parsed, then\nrefused with exit code 4). The third binds UNION\nALL and raises the TPC-DS ratchet by eight queries (4, 11, 33, 56, 60,\n66, 71 and 74).\n- **The tests build plans by hand.** No query reaches the node yet.\n\nWhat changes:\n\n- **`UnionAllNode`.** It is N-ary: a run of `UNION ALL`s is one node\nwith its inputs in written order, not a chain\nof binary nodes. It holds per-input column lists and new output ids,\nsince no input defines a union's rows. Every\n  exhaustive visitor has an arm:\n  - names and inputs;\n  - output, read and defined ids;\n- the position resolver: each input resolved against itself, one type\nper position, ids defined after the inputs;\n  - `CloneWithFreshIds`;\n- pruning: every output is kept, as a Project's is, and each input is\nasked for the columns the union maps;\n  - `EstimateRows`: the saturated sum, unknown where any input's is;\n- the shared sub-plans' origins and keys, keyed by the inputs in order;\n  - EXPLAIN.\n- **Shared sub-plans: nothing under a union is ever a member.** A union\nrepeated whole is one group, and\n`WorthSharing` refuses it. Below a union, `SharedSubPlans` neither\ncollects nor reports anything. Otherwise an\naggregate-topped branch could be held on its own and outgrow\n`MaterializedTable::kMaxBytes`, which fails the query.\n  Two reviewer findings shaped this rule:\n- copies that lack a key or list their branches in another order let\nbranches be held one by one;\n- a copy under a union could report a group nested wholly in another\ngroup.\n\n  Groups above a union are still found.\n- **The semi-join reduction's gate.** `ReadsAJoin` becomes\n`ReadsAJoinOrUnion`, because D5's offer does not cross a\n  union either. ADR 0023 gets an update line.\n- **`UnionAllOperator`, serial.**\n- Input k is opened when the union first needs it, and closed before k+1\nopens: one input's resources at a time,\n    and nothing of an input a LIMIT never reaches.\n- It runs on the consumer thread; each branch keeps its own part\npipelines.\n- Each batch is rebuilt under the union's schema (input 0's names,\nnullable fields), with its selection kept and no\n    row copied.\n  - The first failing input decides the error.\n- The Builder arm plans each input on its own and drops the offer from\nabove. A malformed union is `Invalid`,\n    including columns of two types.\n- **Docs:** ADR 0036 (Proposed, with roadmap X1 to X7), the ADR index,\ndocs/architecture.md and the header comments.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run ci\n100% tests passed out of 2996   (1 skipped: the window-function canary, as on main)\n\n$ pixi run lint\nevery check passes except shellcheck (SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' <every changed .cc file>\nno findings in changed code (two pre-existing bugprone-exception-escape findings in untouched lambdas of\nsrc/exec/tests/parallel_test.cc, seen with the dev compile database)\n\n# the sharing tests catch the bugs they guard: with each fix disabled in turn\n$ pixi run test -R 'AreNotGrouped|NothingUnderAUnionWithoutAKeyIsAMember|HoldsNothingUnderAUnion'\n3 tests failed out of 3          # without collect's stop at a union\n$ pixi run test -R 'ACopyUnderAUnionReportsNoNestedGroup'\n1 test failed out of 1           # without emit's stop at a union\n```\n\nNot run locally (macOS host): `pixi run asan`, `ci-gcc`, the whole-tree\n`tidy`, `coverage` and `fuzz-smoke`. This PR\ntouches exec ownership code and a CMake file, so CI's `check-full` legs\nare the gate for those.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed):\n  - plan:\n- `column_ids_test`: output ids, per-input resolution, every\nbroken-union message, cloning.\n- `logical_plan_test`: names, spans, inputs, and a `WithInputs` death\ntest.\n    - `join_order_test`, `explain_test`.\n    - `optimizer_test`: pruning, and the reduction over a union.\n    - `shared_sub_plans_test`: five cases.\n  - exec:\n- `union_all_test`, eleven cases: order, schema rebuild without copy,\nselections, lazy open and close, LIMIT,\nerrors in input order, failed Open and Close, empty inputs, Invalid\nplans.\n- `physical_planner_test`: under every consumer, as a join's build and\nas its probe, with a profile, and\n      malformed unions.\n    - `parallel_test`: the same rows on 1 and 4 threads, LIMIT, errors.\n- `aggregate_filter_test`: the offer does not cross a union, with a\ncontrol that shows it does over a scan.\n- `shared_builds_test`: nothing under a union is held, with a control\nthat shows sharing fires outside one.\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n(ADR-0006). ADR 0036 cites query numbers and counts only, with the\nTPC-DS disclaimer.\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `docs/adr/` (the new\nADR 0036, its index entry, and an update line in ADR 0023), each edit\napproved in the session.\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the design (with three design\nsubagents), the code, the tests and the docs. The `reviewer` subagent\nreviewed the diff twice; its three P1\nfindings (two on shared sub-plans, one on untested Close-failure paths)\nare fixed and tested.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T19:47:48+03:00",
          "tree_id": "9262357dbb3e5a973ddae63f2ee709398e76288a",
          "url": "https://github.com/ydb-campus/antb1/commit/5f0996c73341d701883e67180f1be88f738dba83"
        },
        "date": 1793379580247,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4012.8383118347465,
            "unit": "ns/iter",
            "extra": "iterations: 173111\ncpu: 4012.962330527813 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 45886.71819059652,
            "unit": "ns/iter",
            "extra": "iterations: 13463\ncpu: 45886.86919705861 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 87751.41086544677,
            "unit": "ns/iter",
            "extra": "iterations: 7915\ncpu: 87742.97397346808 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 229156.9215429787,
            "unit": "ns/iter",
            "extra": "iterations: 3059\ncpu: 229153.7378228179 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 434934.63902742753,
            "unit": "ns/iter",
            "extra": "iterations: 1604\ncpu: 434921.67394015 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1296993.173112419,
            "unit": "ns/iter",
            "extra": "iterations: 543\ncpu: 1296994.2486187837 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 45.809017266666764,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.80701586666667 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.646124133331035,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.645148066666685 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 206.25683066667003,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 206.25865600000043 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.620591999999371,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 13.620956292682935 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 22.34566950000006,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 22.344146099999957 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 43.524563249999204,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.525397999999996 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.9429498156424534,
            "unit": "ms/iter",
            "extra": "iterations: 358\ncpu: 1.9429335586592178 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 64.6043882000015,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 64.60364070000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 13.960303779999776,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.959677940000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 15.779168909091368,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.77900965909091 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 38.150979833334176,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.149538944444494 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 71.44796577778025,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 71.44640055555563 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 236.71385500000497,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 236.70388266666723 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1213.0217470000275,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1213.0169200000012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 18.193222743590443,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.193268948717918 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 21.3971575151511,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.397082939393922 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 41.40595658823459,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 41.404930235294174 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 86.17974099999921,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 86.18047399999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 303.69141199997784,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 303.68360049999944 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1508.6091609999812,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1508.5750309999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 47.85439620000034,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 47.85297766666664 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 49.88073642857265,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.87991978571443 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 80.89705737499742,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 80.89164637500002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 163.32331699999258,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 163.31904975000012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 433.61803799999166,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 433.6073409999983 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1965.3071789999785,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1965.2577539999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 57.640460250003876,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 57.63996183333298 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 59.30962158332894,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.30578824999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 96.8026645714336,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.79349271428656 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 175.686531249994,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 175.68609899999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 506.0153250000212,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 505.99217200000624 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2214.1298649999612,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2214.0923170000024 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 14.034552839999606,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 14.033966599999985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 15.600959133333467,
            "unit": "ms/iter",
            "extra": "iterations: 45\ncpu: 15.59997271111118 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 38.66553277777661,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.66401749999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 71.53072366666417,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 71.52628355555557 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 240.35530900001353,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 240.35817033333254 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 17.8534039230767,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 17.852804076923146 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 21.09357639393879,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.09312496969702 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 41.26907888235444,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 41.26521682352922 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 89.00307000000396,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 89.00101950000038 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 304.86153199998967,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 304.8394845000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 13.994005700000116,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.993852840000045 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 15.804208795454532,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.803280431818063 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 45.594978733333846,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.59269426666693 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 85.6204016250004,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.61544974999968 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 179.926392750005,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 179.92316425000075 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 18.005996974358286,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.005223205128264 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 22.504119000000028,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.502951838709627 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 48.96445853846059,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 48.9609933846155 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 98.49593314285292,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 98.49068099999921 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 204.03583233333697,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 204.03997733333054 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 16.737751880953056,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.73792719047606 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.00929482051317,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.008925769230736 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 30.73158734782589,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.73148460869576 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 52.745360083330674,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 52.743781833333024 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 95.3529904285639,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.3444554285718 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 16.717691976191688,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.717139833333356 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.082977871795965,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.08095148717971 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 32.930684952380794,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.929462095238264 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 72.02835759999857,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 72.02120960000116 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 221.1787843333468,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 221.1692580000033 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 16.92595447619012,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.925002476190528 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.089216315789407,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.088836289473868 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 34.46376389999841,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.46116205000038 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 79.32814149999956,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 79.33023400000039 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 259.24395699998587,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 259.2343906666675 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 16.667316285713966,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.667122547619194 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 20.262966885715223,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 20.262092085714503 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 45.910965533335,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.87917626666638 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 74.2039516249946,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 74.19897662499864 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 155.69659719999436,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 155.6926489999995 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 16.66981680952382,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.66938188095269 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 20.342694117647063,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.342791323529205 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 46.7437370666668,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.73936933333304 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 77.12054162499982,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 77.1201855000001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 167.34669724999662,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 167.34501075000097 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 19.25087613513468,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 19.251437702702813 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 21.193637999999737,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.193296909090847 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 38.802770888891146,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.80055027777729 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 71.00994422222584,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 71.00539811111162 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 121.75750116666488,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 121.75936433333352 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 19.340678666665667,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.338047250000063 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 21.34185806060755,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.34101618181784 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 43.04300312499976,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.04051806249998 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 90.7949867142861,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 90.7970107142871 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 262.99904533334484,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 262.9909433333353 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 18.971118837836567,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.971138378378246 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 21.344986393938655,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.344075181818052 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 44.988049687500364,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 44.98800893749966 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 95.05039714285398,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.0433608571432 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 299.1197450000129,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 299.11848549999576 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 19.930671857142507,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.92920485714283 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 24.154882758621405,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.15424803448302 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 58.399213545452646,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 58.39446018181863 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 107.41777083332711,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 107.41497416666827 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 216.20095899999114,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 216.19632500000208 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 19.752962500000926,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.75256155555548 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 24.101268758619394,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.10028055172414 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 59.05528289999893,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 59.051280900000336 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 108.28661559999091,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 108.28871020000008 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 228.7291420000012,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 228.72849899999892 ms\nthreads: 1"
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
          "id": "f66692b10826d6d0eecefd5c2e1eb121e1cabd69",
          "message": "fix(plan): tell a self-join's two sides apart in shared sub-plans (#264)\n\n## Summary\n\nThis fixes a wrong answer from shared sub-plans ([ADR\n0030](docs/adr/0030-shared-sub-plans.md)'s E2). The PR #263\nreviewer spotted it while reading `shared_sub_plans.cc`.\n\n**Repro.** Two derived tables over one self-join: one groups by `a.k`,\nthe other by `b.k`, with a filter on `b`.\nWhen the table is large enough to pass the sharing gate (at least 10M\nrows read), they keyed as one sub-plan, and E2\nanswered the second from the first's materialized result.\n\n- `SUM(s2.n)` came out **72**; the correct answer, without sharing, is\n**48**.\n- The test fakes the size with a claimed row count. Real data of that\nsize could hit it, with no test noticing.\n\n**Root cause.** A column reference was keyed by its *origin* alone:\n`table#field` for a scanned column, or a\nstructure built on such origins. An origin cannot tell apart:\n\n- **the two sides of a self-join:** `a.k` and `b.k` are both `t#k`;\n- **the `COUNT(*)`s of two different aggregations:** a call without an\nargument has one origin, whatever it counts.\n\nSo two different sub-plans could get equal keys. Separately, `GroupOf`\nmatched members' output columns by origin\nalone, which could also map a member's `b.k` onto `a.k`'s position.\n\n**Fix.** Every reference now also carries its *path*: the inputs it is\nread through, from the node that reads it\ndown to the node that defines it.\n\n- `Origins::Trace` records each node's output paths.\n- The path is applied in node keys (`NodeKey`, `PredicateKey`,\n`ExprKey`), inside the origins of columns defined\nfrom references (Project, GroupAggregate, Aggregate, Compute), and in\n`GroupOf`'s output matching.\n- Equivalent sub-plans have one shape and so the same paths, so every\nsharing ADR 0030 measured is kept: two CTE\n  references, and repeated joins whose scans read different fields.\n- A node with a missing input, or below one, gets no paths. That is a\nmalformed plan, which the physical planner\nthen rejects as before. Without this guard, two malformed-plan tests\nsegfaulted.\n\n**Docs.** An update line in ADR 0030, and the matching rule in\n`shared_sub_plans.h`.\n\n**On top of #263.** This branch is rebased on #263, and its union arms\nkey their reads with paths too: a union's\ncolumn `i` is read through input `k` as `/k…`.\n\n**Cost.** Bounded by the 256-relation limit. The reviewer timed an\nextreme plan, a 256-relation self-join carrying\n2304 columns, at 0.83 s for the whole EXPLAIN in a Debug build. For\nrealistic plans the cost is negligible.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [x] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run test -R 'TheSidesOfASelfJoinAreNotOnePlace|TwoAggregationsCountsAreNotOneColumn|AnswersTwoSidesOfASelfJoin'\n100% tests passed out of 3\n# with the fix stashed: 0% tests passed, 3 tests failed out of 3 (the exec one shows SUM 72 against 48)\n\n$ pixi run ci\n100% tests passed out of 2999   (1 skipped: the window-function canary, as on main)\n\n$ pixi run lint\nevery check passes except shellcheck (SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' src/plan/shared_sub_plans.cc src/plan/tests/shared_sub_plans_test.cc src/exec/tests/shared_builds_test.cc\nno findings\n```\n\nNot run locally (macOS host): `pixi run asan`, `ci-gcc`, the whole-tree\n`tidy`, and the TPC-H- and TPC-DS-derived\nstatus tests (Linux only), which CI runs. The TPC-H-derived queries ADR\n0030 measured have no self-join, so their\ngroups are unchanged.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed):\n- `exec.SharedBuildsTest.AnswersTwoSidesOfASelfJoinEachFromItsOwnRows`:\nthe wrong answer.\n- `plan.SharedSubPlansTest.TheSidesOfASelfJoinAreNotOnePlace` and\n`TwoAggregationsCountsAreNotOneColumn`: the two\n    collision classes.\n\nAll three fail without the fix, and the existing sharing tests still\npass.\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `docs/adr/0030` (an\n      update line), approved in the session\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5). The `reviewer` subagent on #263 pointed\nat the self-join keying; Claude reproduced the wrong answer, found the\nCOUNT(*) case, and wrote the fix, the\ntests and the docs. A second `reviewer` subagent reviewed this diff.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T21:11:01+03:00",
          "tree_id": "f44c4d01d9aec198a1891310f26047f7a696bd29",
          "url": "https://github.com/ydb-campus/antb1/commit/f66692b10826d6d0eecefd5c2e1eb121e1cabd69"
        },
        "date": 1793384632203,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4681.963086934432,
            "unit": "ns/iter",
            "extra": "iterations: 149676\ncpu: 4681.79700152329 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 31018.89867597581,
            "unit": "ns/iter",
            "extra": "iterations: 23489\ncpu: 30974.563838392438 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52500.660175271216,
            "unit": "ns/iter",
            "extra": "iterations: 13351\ncpu: 52500.26522357876 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 221382.0683977073,
            "unit": "ns/iter",
            "extra": "iterations: 3158\ncpu: 221372.7973400887 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 498274.7703281193,
            "unit": "ns/iter",
            "extra": "iterations: 1402\ncpu: 498254.91654778895 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1501950.9827214114,
            "unit": "ns/iter",
            "extra": "iterations: 463\ncpu: 1501903.4082073418 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 39.861399944442155,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.858820222222214 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 44.89757556249785,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 44.896622437500035 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 181.62115733332485,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 181.60318933333326 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.705094208333472,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.704803333333311 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 24.8782216071431,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.87638285714284 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 36.83377447368373,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.83111857894741 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.4632007878151303,
            "unit": "ms/iter",
            "extra": "iterations: 476\ncpu: 1.463080586134454 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 90.24783242857178,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 90.2447467142858 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.665246774194305,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.663884225806438 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.480503137930288,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.47989037931029 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 53.14245830769201,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.138514923076926 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 75.35441555555482,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 75.34257411111116 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 256.4390426666705,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 256.42068266666604 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1254.0941450000105,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1253.9715099999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.528535680000914,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.526933199999917 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 30.973430130433393,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.970673043478133 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 54.51844584615628,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.51633084615373 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 94.93107471428986,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 94.926171571429 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 311.4335214999926,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 311.4116350000007 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1511.7154989999904,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1511.6239169999978 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 55.8031050833326,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.798636166666846 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 58.7592259166693,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.75712574999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 85.09796825000393,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.09562425000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 147.21638319999784,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 147.20381659999973 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 402.35228099999176,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 402.3337819999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1869.054312000003,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1868.946721999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.12573018181911,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.12168481818166 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.88292549999733,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.88129920000026 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 97.38037671428401,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 97.36551671428576 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 167.69315425000286,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 167.67503724999955 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 436.0070580000013,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 435.96273350000206 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2014.0919640000448,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2013.8779769999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 23.243016033332726,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.241614133333382 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 24.499901413791797,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.4971363103448 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 55.961409333330415,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.95674308333364 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 74.98971477778267,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 74.9857487777782 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 256.491978666664,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 256.47979999999865 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 27.466510560000188,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.46531207999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 30.937110565218973,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.934723260869788 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 54.64678969230783,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.64454653846163 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 101.52614699999926,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 101.52145857142817 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 327.8655430000015,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 327.8567465000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 22.870560483870722,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.870185903225742 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 25.05380560714343,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.052287392857206 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 58.35462772727272,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 58.34996327272729 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 107.3887754999987,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 107.37863816666741 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 218.54460566667436,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 218.51349033333395 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 27.418254000001525,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.41576948000016 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 34.04955357142994,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 34.04558866666666 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 62.48846500000127,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 62.479134636363526 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 117.56072033333946,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 117.54880949999938 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 225.4374956666728,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 225.42388433333352 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.063239585365586,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.061635073170716 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.19785715789342,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.197265315789487 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 23.919861965517722,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.917873275862163 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 31.492369636362643,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.4913557727275 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 101.47771828571551,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 101.46934699999933 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.118037634146475,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.117672658536677 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.41867999999935,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.41665760526318 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 27.32980157692238,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.328677923076857 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 50.86396453845929,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 50.860578076922664 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 249.89017033334449,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 249.88286800000026 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.475257774999875,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.47415729999986 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.742922216216737,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.74240072972983 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 28.794357708333013,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 28.793053708333478 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 51.816786500000944,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 51.81279839999888 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 232.92132533333643,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 232.8992036666667 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.173307000000367,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.171096560975744 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 21.326871272726816,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.32130760606048 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 36.73521115789706,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.733388157894126 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 51.256688300009046,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 51.25374140000076 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 138.30873499998688,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 138.28929399999765 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.541070724999486,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.539196999999973 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 21.829627875000313,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 21.82743531250031 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.06491833333641,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.06199316666673 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 54.06706918181085,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 54.05739572727245 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 142.57584560000396,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 142.56328719999942 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 23.493627266668682,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.490968500000047 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 24.785472035712733,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.78367828571442 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 30.16121369565237,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.157746652174243 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 47.98919792307725,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 47.98584469230714 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 116.76087116666167,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 116.75613400000155 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 23.43291269999857,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.431261266667036 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 24.997568250000768,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.994630071428706 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.47059330000002,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.46707095000008 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 71.35042044443354,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 71.34262355555639 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 260.5725463333404,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 260.5489719999999 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 22.610310806451327,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.602320225806512 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 24.258377172416125,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.25627206896567 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 34.79146789999845,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.786675299999814 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 76.97062977778539,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 76.96425888888807 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 271.5529790000346,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 271.5106106666667 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 24.463384862068278,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.459969413793058 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 32.95635814285582,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.95477423809516 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 47.41101373332791,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 47.40443666666655 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 97.78667400000056,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 97.78300316666606 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 216.32282125000302,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 216.3112282499995 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 23.50305863333612,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.501125099999836 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 31.68993100000121,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.684417590909675 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 46.40840614285935,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 46.404685785713944 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 86.91495342856926,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 86.91145242857041 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 214.8719216666374,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 214.85616800000193 ms\nthreads: 1"
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
          "id": "76291c6f3f104502c83662e82e0392b8876a8e9f",
          "message": "feat(sql): parse union all, refused until the binder answers it (#265)\n\n## Summary\n\nThis is the second of three UNION ALL PRs: item X2 of [ADR\n0036](docs/adr/0036-union-all.md), after #263's plan node\nand operator. `UNION ALL` now **parses** wherever a query stands: the\nwhole statement, a derived table, a CTE body,\nor the query of a subquery. The binder **refuses every compound with\nexit code 4** at its first `UNION ALL` until\nthe next PR (X3) binds it. The one exception is a compound in a CTE that\nnothing references: like any such CTE, it is\nnever checked or bound, so that query is answered (it was exit 4 at\nparse time before).\n\n**AST.**\n- A compound keeps its blocks in a flat list, `set_operation` on\n`sql::SelectStatement` (`SetBranch{query, op_span}`).\n  The roughly 55 `Box<SelectStatement>` sites stay unchanged.\n- Its own block fields stay empty. The `WITH` list before the blocks and\nthe `ORDER BY`, `LIMIT` and `OFFSET` after\n  the last one belong to the compound, never to the last block.\n- Each block is one level below the compound for the depth limit.\n\n**Tripwire.** `kSelectStatementFields` (19) is checked by a structured\nbinding of every field in `ast.cc`, and every\nwalk over a statement `static_assert`s it. The next new field fails the\nbuild at each walker until it is revisited.\n\n**Parser.** `ParseBlock` is split into a core (SELECT through HAVING),\nthe compound loop and a tail.\n- Exit 4: `UNION` without `ALL` (`UNION DISTINCT` too), `BY NAME`,\n`INTERSECT`, `EXCEPT`, and a block in parentheses,\n  a `VALUES` list, a FROM-first query or `TABLE`.\n- Exit 1, as in DuckDB: `ORDER BY`, `LIMIT` or `OFFSET` before a `UNION\nALL`, and a `UNION ALL` that no block\n  follows.\n\n**Walkers.**\n- `EqualIgnoringSpans`, `Depth` and the unparser.\n- The relation count: the limit counts every block, and reports the span\nof the block item that crosses it.\n- The binder's static checks and its date-cast fold.\n\nThe binder refuses a compound before any check that reads a block's\nclauses; otherwise a compound would reach\n`CheckFromList`'s assertion (exit 70). The `Binder` constructor asserts\nthat it never sees one.\n\n**Harness.**\n- `MakeOrderedQuery` declines a compound, and `UnlimitedSql` already\nhandles one.\n- The random generator writes compounds (2 or 3 copies of a nested\nblock) from an eighth salted stream, as a feature\nthat is generated but not yet supported (`kUnionAll`). Queries without a\ncompound are byte-identical to before, and\n  diff samples with one expect exit 4.\n- Three pending metamorphic relations wait for X3.\n\n**Fuzzing.** Dictionary tokens and six seeds; the replay test checks the\nround trip on them.\n\n**Docs.**\n- `docs/sql-subset.md`: the grammar, a new \"Compounds\" paragraph\n(placements, refusals, syntax errors, depth), the\n  canonical form and the exit codes.\n- `docs/architecture.md` (parse and bind steps) and the walker note in\nthe add-sql-feature recipe.\n- ADR 0036: progress only. X2 is now \"Built\", and Decision 4 says the\nbinder checks no unreferenced CTE. Its status\n  stays Proposed.\n\n**TPC-DS-derived queries.** A local run of the dev build over all 99 at\nSF 0.01 gives:\n- 38 exit 0, as the ratchet;\n- 61 exit 4;\n- no query moved to exit 1.\n\n\"UNION ALL is not supported\" is now the first error of exactly queries\n4, 11, 33, 56, 60, 66, 71 and 74, the eight\nthe next PR targets.\n\nThis workload is derived from the TPC-DS Benchmark and is not comparable\nto published TPC-DS Benchmark results, as\nthis implementation does not comply with all requirements of the TPC-DS\nBenchmark.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run ci\n100% tests passed out of 3048   (4 skipped: the 3 new pending union relations, which wait for X3,\n                                 and the window-function canary, as on main)\n\n$ pixi run lint\nevery check passes except shellcheck (50 x SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' <the 16 changed .cc files, tests included>\nno findings\n```\n\nMutation check: with the date-cast fold over a compound's blocks\nremoved, the case\n`UnionAllLastBlock/...CAST('2020-01-01' AS DATE)` fails, because the\ncast is then refused before the compound.\n\nNot run locally (macOS host), so CI's legs are the check:\n- `pixi run asan`, which `docs/testing.md` asks for when the generator\nor `supported_features.h` changes (DuckDB leak\n  check on the seeds' new queries);\n- `ci-gcc`;\n- the whole-tree `tidy`.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed):\n- `parser_test`: accepted forms with spans, the refusal and syntax\ntables, depth limits, and 10,000 blocks.\n- `unparse_test`, `parser_property_test` and the fuzz seeds: round\ntrips.\n- `relation_count_test`; `binder_test`'s `UnionAll` (every placement)\nand `UnionAllLastBlock` (each construct only\n    in the last of three blocks).\n- `query_gen_test` (stream isolation), the harness rewrites, and the CLI\ngolden `unsupported_union_all`.\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [ ] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: `docs/adr/0036` (progress\nand one precision in Decision 4, status unchanged), for the maintainer's\nreview here\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the code, tests and docs and ran the\nchecks above. A `reviewer` subagent reviewed the diff: no P0, and its\nthree P1s are fixed in the second commit\n(the unreferenced-CTE case, the depth wording with a check that could\nnever fire, and the ADR edit flagged here).\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-30T22:30:56+03:00",
          "tree_id": "c39cfa1a0c1336f9ee3b4fa2eead037605d3b6b8",
          "url": "https://github.com/ydb-campus/antb1/commit/76291c6f3f104502c83662e82e0392b8876a8e9f"
        },
        "date": 1793389174691,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 2672.875472862497,
            "unit": "ns/iter",
            "extra": "iterations: 262497\ncpu: 2672.716693905073 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 20221.111544011754,
            "unit": "ns/iter",
            "extra": "iterations: 34650\ncpu: 20219.292929292933 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 37166.59802771902,
            "unit": "ns/iter",
            "extra": "iterations: 18760\ncpu: 37164.18342217484 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 133892.60992366433,
            "unit": "ns/iter",
            "extra": "iterations: 5240\ncpu: 133881.2269083969 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 280488.80513231514,
            "unit": "ns/iter",
            "extra": "iterations: 2494\ncpu: 280477.15156375285 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 787308.4490481415,
            "unit": "ns/iter",
            "extra": "iterations: 893\ncpu: 787235.2094064953 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 22.593397035714297,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 22.592581964285724 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 29.50938212500039,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.505623750000026 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 134.5202180000001,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 134.50838419999994 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 8.695176375000102,
            "unit": "ms/iter",
            "extra": "iterations: 80\ncpu: 8.694493687499994 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 9.79704579411756,
            "unit": "ms/iter",
            "extra": "iterations: 68\ncpu: 9.796301102941175 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 24.950538535714525,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.94846621428572 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 0.8267605381008336,
            "unit": "ms/iter",
            "extra": "iterations: 853\ncpu: 0.8266631875732694 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 55.052125000000274,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 55.04742639999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 14.13624435294109,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 14.134037098039185 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 16.750592634146763,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 16.749323975609755 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 32.654238952380595,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.64912785714289 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 49.832620285714924,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.829755214285804 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 202.6782373333257,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 202.6574046666667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1117.9169720000175,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1117.7828299999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 16.944714071428955,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.94315590476194 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 20.381877611110927,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 20.380292166666653 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 34.70059795238204,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 34.6954158095238 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 61.582648499995685,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 61.57592879999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 236.27726333332552,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 236.26367633333226 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1394.7924130000047,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1394.6381870000018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 33.08721038095406,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.08370795238094 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 35.03624449999734,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.03184785000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 50.532626199998276,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 50.52800160000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 96.67583771428261,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.66912699999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 314.7385874999884,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 314.7137975000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1647.5757840000256,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1647.360531999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 39.264160055555266,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.26044733333356 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 42.00987411764559,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 42.005479176470786 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 60.99657154545536,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 60.989004727273056 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 113.8043816666728,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 113.78557716666649 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 333.03269999998975,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 333.0087800000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 1711.2113129999784,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1711.0345740000027 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 13.825415509803834,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.823598058823531 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 16.61691966666689,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.614842666666586 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 32.55079545454526,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.54609604545454 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 50.70244369230837,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 50.69422315384633 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 205.51304433333448,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 205.4813193333326 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 17.02313853658458,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.021294731707346 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 19.893840800000362,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.89127805714292 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 33.172590619047,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.169727333333164 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 66.12634836363378,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 66.12001881818165 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 238.42884000000217,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 238.39920833333395 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 14.105966666666822,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 14.10519941176463 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 16.18742481395371,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.18581497674415 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 34.86494210000046,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.862194849999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 58.83186241666275,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.8241617500002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 147.43299660000275,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 147.41766920000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 16.726387928571285,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.723683023809397 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 20.881140970588188,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.85038888235293 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 38.420050421052565,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 38.41683173684218 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 76.45925575000234,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 76.44859112500058 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 174.5337627500021,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 174.50435249999964 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 10.113820492753565,
            "unit": "ms/iter",
            "extra": "iterations: 69\ncpu: 10.112373594202886 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 10.807627292307679,
            "unit": "ms/iter",
            "extra": "iterations: 65\ncpu: 10.805933169230833 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 13.252654150943393,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.251454075471742 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 16.852129435898032,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 16.85080256410269 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 81.04961575000402,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 81.04193074999966 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 10.119822308823245,
            "unit": "ms/iter",
            "extra": "iterations: 68\ncpu: 10.118720000000096 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 10.921941468749807,
            "unit": "ms/iter",
            "extra": "iterations: 64\ncpu: 10.920255203124896 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 14.658274562499685,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.656162125000094 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 25.372801347825572,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 25.36699717391299 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 151.86208719999286,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 151.84747859999845 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 10.211361695652336,
            "unit": "ms/iter",
            "extra": "iterations: 69\ncpu: 10.208034956521749 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 10.868830281250297,
            "unit": "ms/iter",
            "extra": "iterations: 64\ncpu: 10.86804149999998 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 15.839231386363197,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.83830731818187 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 30.992256043478136,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.98922073913002 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 186.65507200000775,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 186.63285999999957 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 9.864635647887445,
            "unit": "ms/iter",
            "extra": "iterations: 71\ncpu: 9.860223957746554 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 12.574101803571532,
            "unit": "ms/iter",
            "extra": "iterations: 56\ncpu: 12.572992392857126 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 22.117321437500337,
            "unit": "ms/iter",
            "extra": "iterations: 32\ncpu: 22.113356062499978 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 35.23053909523945,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 35.227169857142556 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 107.03325485714491,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 107.01125285714242 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 10.104908071429008,
            "unit": "ms/iter",
            "extra": "iterations: 70\ncpu: 10.099844014285761 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 12.683437654545122,
            "unit": "ms/iter",
            "extra": "iterations: 55\ncpu: 12.681574872727259 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 22.739878096775012,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.73720829032254 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 35.934241550000934,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 35.92597594999987 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 118.64909900000005,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 118.63692166666577 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 13.881250538461769,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.877929076922868 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 14.176011795917262,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.17491406122442 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 17.791752075000034,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.788525525000054 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 31.05918939999981,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 31.056158999999894 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 93.3357961428588,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 93.32352128571537 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 14.12072994339552,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 14.118950641509315 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 14.337778857142839,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.336802285714379 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 19.827518514284748,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.826221257142556 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 44.68690956250043,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 44.67973037499995 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 174.11614674999498,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 174.1005874999999 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 13.723316509803155,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.72185584313726 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 14.752898808510848,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.751678234042533 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 21.372366575757372,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.371214454545093 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 54.96965166666712,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 54.96071441666691 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 230.26126833332228,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 230.23950800000156 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 13.879177058823625,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.874953313725602 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 16.377125651162217,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.375676837209234 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 28.182407166665275,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 28.177230374999713 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 68.47491488888915,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 68.4674196666669 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 176.93461725001214,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 176.90740400000138 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 14.04195295918373,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.040363387755232 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 17.904160857142415,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 17.901119523809513 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 29.780226583334013,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.778815416666642 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 72.05152349999366,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 72.04833412499845 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 185.63081649999447,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 185.60347525000154 ms\nthreads: 1"
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
          "id": "b3e73ef7b5ab5778c4388d02b6a49dd926ec3b74",
          "message": "fix(sql): close the union all grammar's gaps (#266)\n\n## Summary\n\nThis follow-up to #265 closes the gaps that an audit of #265 found\nagainst its plan (ADR 0036's X2, the UNION ALL\ngrammar). Nine read-only agents checked each plan item against the\nmerged diff, and a second agent confirmed every\ngap. They found no wrong answer. The gaps are missing tests, one\nexit-code inconsistency, a hint, a few tripwire spots\nand docs. #265 is still refused by the binder in every placement, so\nthis changes no answer.\n\n**Behavior (the parser):**\n- **Parenthesized first block.** In `x IN ((SELECT …) UNION ALL SELECT\n…)`, `x = ((SELECT …) UNION ALL SELECT …)`\nand before a query's tail (`x IN ((SELECT …) LIMIT 1)`, `ORDER BY`,\n`OFFSET`), a parenthesized first block is now\nunsupported (exit 4, \"parenthesized queries are not supported\", at its\n`(`), as the Compounds docs promise. It was\na syntax error (exit 1), which predates #265. DuckDB answers every one\nof these forms.\n- Checked against DuckDB: after a later `IN` value or inside a larger\noperand\n(`(SELECT …) + 1 UNION ALL …`), a set operation stays a syntax error, as\nit is in DuckDB.\n- **Hint.** The \"expected …\" hint after a block now offers `UNION ALL`\nwherever one may follow.\n\n**Tests the plan listed that #265 lacked:**\n- **Parser spans** for compounds as a derived table's, a CTE's and an\nIN, EXISTS or scalar subquery's query. This\ncovers a compound tail with ORDER BY, LIMIT and OFFSET, and the\ncompound's `WITH` and `ORDER BY` spans.\n- **Three-block depth case:** a later block's depth error lands inside\nit.\n- **Last-of-three-blocks matrix for the SQL walks** (ADR 0036 Decision\n3's second check): each construct only in the\nthird block, with exact `Depth`, inequality against a changed copy,\nround trip and `CheckParse`.\n- **Property-test pools:** `UNION ALL` in the structure pool;\n`INTERSECT` and `EXCEPT` among the others.\n- **Relation count:** a compound's own ORDER BY subquery, a CTE whose\nquery is a compound, three blocks, and the\nspans `Top` reports for a block's subquery and for the compound's ORDER\nBY subquery.\n- **Generator:**\n- a compound's row bound is copies × the table's rows; deleting that\nmultiplication failed no test before;\n- a SELECT * star guard over a 20-row table (3 copies need a LIMIT, 2 do\nnot);\n- `NestedBlocksProjectColumnsUnderTheirOwnNames` now reaches its\ncompound branch;\n- `DeepSubqueries` walks a compound's blocks and carries the field-count\ntripwire.\n- **Oracle:** `QueryGenOracle` counts the compounds it checks. A fixed\nlist of compound shapes the generator never\nwrites (top level with WITH and a tail, a derived tail, IN, EXISTS and\nscalar) is read by DuckDB in its original and\n  canonical forms.\n\n**Tripwire and docs:**\n- **Tripwire:** `BlockSql` and `ParseBlockEnd` now assert\n`kSelectStatementFields`; the recipe lists them, plus the\n  harness walks and rewrites to revisit.\n- **Docs:** `UNION BY NAME` is reported at `UNION` and `UNION ALL BY\nNAME` at `BY`; the docs had said otherwise. The\n  docs now also name the parenthesized first-block forms.\n- **Comments:** `parser.h`, `ast.h` and `relation_count.h`.\n\n**#265's own verification, now run:** `pixi run diff-random` with 20,000\nqueries (seed 2973530691):\n\n```text\nantb1-slt diff: seed=2973530691 queries=20000 supported=19790 target=4948 compared=19790 unsupported=210 rejected=0 answered_outside_supported=0 failed=0\nantb1-slt diff: Unsupported answers (not failures) by feature outside the supported set: union_all=210\nDIFF: PASS seed=2973530691 queries=20000 failed=0 unsupported=210\n```\n\nEvery generated union got exit 4, and none got exit 1.\n\n**Out of scope:** the order of errors for a compound as the query of a\nsubquery belongs to the next PR (X3), which\nreplaces that refusal.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [x] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run ci\n100% tests passed out of 3074   (4 skipped: the 3 union relations, pending until X3 declares the\n                                 feature, and the window-function canary, as on main)\n\n$ pixi run lint\nevery check passes except shellcheck (50 x SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' <the 7 changed .cc files, tests included>\nno findings\n```\n\nThe DuckDB claims behind the parser change were checked on DuckDB 1.5.6,\nthrough the oracle on a scratch `.slt` file:\n- both forms that stay syntax errors are Parser Errors there;\n- both forms that now exit 4 are answered there.\n\nNot run locally (macOS host): `pixi run asan`, `ci-gcc` and the\nwhole-tree `tidy`, which CI runs.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: none\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5). A workflow of nine read-only agents\naudited #265 against its plan, and four agents drafted the fixes in\nseparate worktrees. Claude integrated them,\nran every expectation, checked the DuckDB claims and wrote the docs. A\n`reviewer` subagent reviewed the diff: no P0. Its one P1 (the EXCEPT\nform had no test) and its\nsuggestion (the same form before ORDER BY, LIMIT or OFFSET gave exit 1)\nare fixed in the last commit.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-31T00:33:26+03:00",
          "tree_id": "f7ddcf9e8ac81a7141ec4a0d8f0bb3fd16f8f6da",
          "url": "https://github.com/ydb-campus/antb1/commit/b3e73ef7b5ab5778c4388d02b6a49dd926ec3b74"
        },
        "date": 1793396823551,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4761.4335021394245,
            "unit": "ns/iter",
            "extra": "iterations: 147704\ncpu: 4761.194564805287 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 32322.458747493292,
            "unit": "ns/iter",
            "extra": "iterations: 22435\ncpu: 32320.894094049476 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52410.909348651745,
            "unit": "ns/iter",
            "extra": "iterations: 13403\ncpu: 52403.539804521395 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 233204.97070571285,
            "unit": "ns/iter",
            "extra": "iterations: 3004\ncpu: 233170.89380825573 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 509467.01362008136,
            "unit": "ns/iter",
            "extra": "iterations: 1395\ncpu: 509399.2974910395 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1545583.2831460694,
            "unit": "ns/iter",
            "extra": "iterations: 445\ncpu: 1545236.164044944 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 42.365037437502195,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 42.35853300000003 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 48.17777520000087,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 48.172511866666724 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 246.76801166666942,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 246.72393933333342 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 14.313601693877862,
            "unit": "ms/iter",
            "extra": "iterations: 49\ncpu: 14.3116934285714 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 35.49978194736899,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 35.49567926315791 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 37.945587052631346,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.94410426315792 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.4967576025640514,
            "unit": "ms/iter",
            "extra": "iterations: 468\ncpu: 1.4966292799145309 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 161.01049474998774,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 161.00089375000027 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 23.393912500000624,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.391482933333364 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 25.292687249999194,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.2915915357143 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 56.0872448461527,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 56.07919161538451 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 166.98475850000705,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 166.95843599999984 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 332.51482500000407,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 332.45686349999914 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1402.3265440000046,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1402.165556 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 28.11656535999873,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.114545880000037 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.637839863637137,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.635403772727376 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 58.53364541666887,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.529559916666564 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 158.66052799999864,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 158.65762500000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 389.40527150001003,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 389.3688574999992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1702.5019520000342,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1702.3873439999982 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.983358083333734,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.981241499999946 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 60.33960666666852,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 60.33383699999994 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 103.87978416666972,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 103.87138699999987 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 232.5466549999836,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 232.5322203333329 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 479.1854315000137,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 479.15442500000086 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 2055.779813000015,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2055.6470329999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 66.08538127272823,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 66.08221536363666 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 69.90914090000047,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.90513849999971 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 112.46651833333014,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 112.46300800000006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 233.23489666665864,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 233.21146733333364 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 541.1308930000018,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 541.0354089999955 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2200.3172920000225,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2200.1487610000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 23.552329700000502,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.5504668000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 24.9756458571438,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.973014107142887 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 55.54722076922977,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 55.54320215384606 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 134.64956183332788,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 134.6434495000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 331.9197220000092,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 331.89231599999755 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 28.208196319999388,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.20613307999992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 31.789051863636463,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.785794272727358 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 57.67279341666646,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 57.67217874999986 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 155.91844800000842,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 155.9049409999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 377.71232349999195,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 377.7044744999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 23.48215203333363,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.479165833333347 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 25.62003696296297,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 25.618253296296228 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 66.91702379999924,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 66.90752900000021 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 173.11135280000371,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 173.08525739999965 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 278.19799666665784,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 278.1829800000016 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 28.12279220000164,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.121658279999906 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 34.88433625000198,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.88100779999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 73.2915984444465,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 73.28891622222184 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 187.9443989999885,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 187.92774275000212 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 299.0616459999842,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 299.0472899999972 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.187059878048462,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.185233756097357 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.372296421052614,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.37183331578947 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 24.84944768965416,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.846256862068802 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 47.91106882352966,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 47.90744441176454 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 125.56819966666428,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 125.55534566666655 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.192811682926877,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.19139478048768 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.379703763158478,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.37747121052608 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 28.106610280001405,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.10155392000013 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 63.24442109091292,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 63.23473936363655 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 308.0093840000018,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 307.9752729999967 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.440846574999114,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.433563475000113 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.86234002702659,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.855816972972733 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 31.022681043477565,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 31.018261086956663 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 72.23932950000034,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 72.2396778750003 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 283.80540299999285,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 283.7867989999978 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.28380407500083,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.27249072500001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 23.702981999999412,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.697395933333592 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 38.76753594444595,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.76545444444446 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 112.76077699999405,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 112.75389479999944 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 200.47928374999913,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 200.47556100000108 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.57512151282124,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 17.56771766666641 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 24.16260375862137,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.16031568965495 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 40.366193055553545,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 40.35917938888881 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 117.43568800000048,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 117.42386960000033 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 201.03695225000706,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 201.01448074999695 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 23.707536133332496,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.704989066666826 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 25.059531499999416,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.051077285714335 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 32.25622345454388,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.25380918181871 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 70.4325945555537,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 70.42626655555593 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 151.95822300000827,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 151.94960240000057 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 23.58816724137898,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.585255448275948 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 25.13819571428704,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.136135928571587 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 36.91705142105212,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.91325931578897 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 90.45179757142867,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 90.44476871428587 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 378.8554660000045,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 378.8276599999989 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 22.698245903225803,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.696188548387166 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 24.493499965517223,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.491775689655086 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 36.88435289473689,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.88163231578974 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 101.91820714285475,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 101.9153504285697 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 314.26495599998816,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 314.23577600000385 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 24.541377000001173,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.540812344827735 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 33.50355028571595,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.500807380952274 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 57.80183854545323,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 57.80219327272753 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 156.35566174999838,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 156.33862924999775 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 263.04073299998737,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 263.03050200000183 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 23.60171229999916,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.59880866666657 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 33.31199414285601,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.308815904761886 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 54.75207041666863,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 54.74877008333342 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 148.7545885999907,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 148.75151500000072 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 256.72852633336635,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 256.69242233333495 ms\nthreads: 1"
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
          "id": "56c4661e070f8ecf1f6a07e6a806111d816d5636",
          "message": "feat(plan): answer union all in derived tables, ctes and statements (#267)\n\n## Summary\n\nThis is the third of the three UNION ALL PRs: X3 of [ADR\n0036](docs/adr/0036-union-all.md). antb1 now **answers**\n`UNION ALL` of two or more blocks as the whole statement, a derived\ntable's query or a CTE's query.\n- The TPC-DS ratchet goes from **38 to 46**: queries 4, 11, 33, 56, 60,\n66, 71 and 74.\n- ADR 0036 moves to **Accepted**, as agreed in the session; X4 to X7\nstay on its Plan.\n\n**Binding** (`src/plan/binder.cc`, `BindUnionAll`):\n- **Scope.** Each block binds as a block of its own, in written order,\nin the compound's CTE frame and scope:\n  - its `WITH` list is every block's;\n- a name that only an enclosing FROM item has is a lateral reference\n(exit 4);\n  - a block never sees another block's names.\n- **Counts before types, as in DuckDB.** Every block binds before the\ncolumn counts are compared. Checked with\nDuckDB: a bind error in a third block wins over a count mismatch at the\nsecond. A count that differs is DuckDB's\nbind error (exit 1), at the `UNION ALL` before the first block that\ndiffers.\n- **Types: DuckDB's left fold in written order** (`UnionAllType`):\n  - equal types stay;\n- integers combine by CASE's integer rule, now shared as\n`CommonInteger`;\n- an integer or DECIMAL with a DECIMAL goes through `CaseDecimal`, so\nthe order of the blocks can change the\n    type;\n- FLOAT ∪ FLOAT is FLOAT and FLOAT ∪ DOUBLE is DOUBLE; the column's\nFLOAT flag is the AND over its blocks;\n  - every other mix exits 4;\n- every block must then widen exactly to the result type\n(`IsExactWidening`).\n\nThe refusal reads \"UNION ALL of T1 and T2 values is not supported\", at\nthe first select item in the query that\n  fails, or at its block's `*`.\n- **Plan.** One `UnionAll` node. A block of another type is cast below\nit, once, straight to the final type. Its ids\n  are new; its names are the first block's.\n- **Still exit 4:**\n- a compound's own `ORDER BY`, `LIMIT` or `OFFSET` (X5), at the first of\nthem. `CheckBlock` returns before\n    `CheckFromList`, whose assertion a compound must never reach.\n- a compound as the query of IN, EXISTS or a scalar subquery (X6), at\nthe subquery's operator, once the other side\nof its conjunct is checked. That restores the query order which the\naudit of #265 found broken.\n- **Unreferenced CTEs:** a compound in a CTE that nothing references is\nnever checked or bound, as before.\n\n**New divergence D26.** Under a `LIMIT` without `ORDER BY`, antb1 never\nruns the blocks it does not need, so their\nerrors are never reported, where DuckDB fails. This was checked with an\noverflow in a second block. With `ORDER BY`\nboth engines fail.\n\n**Tests:**\n- **`binder_test`:**\n  - refusals and their carets;\n  - binding order and lateral references;\n  - every type pair and the order-dependent folds;\n  - exactness;\n  - the plan's shape, casts and fresh ids;\n  - names (top level vs derived and CTE);\n  - the FLOAT flag;\n  - each construct only in the last of three blocks.\n- **`optimizer_test`:** unions in the idempotence, column-id and\nposition lists.\n- **slt, expected results written by DuckDB through `pixi run\nslt-complete`, also run on 4 threads as\n  `parallel.setops.*`:**\n- `tests/slt/cases/setops/union_all.slt` (48 records): placements;\nblocks with aggregates, joins and subqueries;\nevery consumer; joins on either side; type folds at their extremes;\nFLOAT; NULLs; empty blocks; split files.\n- `tests/slt/cases/setops/errors.slt`: bind errors in both engines; the\nX4, X5 and X6 forms as `pending` records,\nwhich DuckDB answers and antb1 must still refuse; type mixes,\nDuckDB-only; D26.\n- **CLI goldens:** EXPLAIN, `--physical`, the X5 refusal, a type refusal\nand the count error. PR 2's\n  `unsupported_union_all` is retired.\n- **Harness:**\n- `kUnionAll` is declared supported, so generated unions are now\ncompared with DuckDB;\n  - the three metamorphic union relations hold;\n  - a nested compound's row bound is copies × the table's rows.\n\n**TPC-DS-derived queries (local, dev build):**\n- All 8 pass at SF 0.01, 0.1 and 1, equal to DuckDB, and 4 threads give\nbyte-identical answers to 1 thread at SF 1.\n- The other 15 queries with set operations fail first on other features\n(exit 4); none moved to exit 1.\n\n**Why the SF 1 ratchet is sharded.**\n- **Cost.** Queries 4, 11 and 74 recompute their union CTE once per\nreference. That takes the full SF 1 run from\n48 s to 90 s in the dev build, which projects to about 846 s of the ASan\nleg's 870 s limit.\n- **Fix chosen.** As agreed in the session, `tpcds.status.sf1` becomes\nfour shards: `tpcds.status.sf1.1of4` to\n  `4of4`, which together are the whole ratchet.\n- They run through a new `antb1-slt --shard K/N` option, with a unit\ntest; each shard runs every fourth query.\n- Locally the shards take 14 to 41 s, projecting to about 390 s under\nASan.\n- **Real fix later:** filter pushdown into the blocks (ADR 0036's X7),\nrecorded in its Consequences.\n\nThis workload is derived from the TPC-DS Benchmark and is not comparable\nto published TPC-DS Benchmark results, as\nthis implementation does not comply with all requirements of the TPC-DS\nBenchmark.\n\n**Docs:**\n- `docs/sql-subset.md`: what works, the binder's list, Compounds, a\nBinding bullet, logical plans, EXPLAIN and\n  `--physical`, semantics, D11, D26, exit codes and the TPC-DS table.\n- `docs/architecture.md`, `docs/testing.md` and `tests/slt/README.md`.\n- ADR 0036 (Accepted; Decisions 4 and 5 for X3; Consequences) and the\nADR index.\n\n**Includes main after #266** (merged in, no conflicts; the Compounds\nparagraph carries both changes).\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run ci\n100% tests passed out of 3120   (1 skipped: the window-function canary, as on main; the three union\n                                 relations now run and hold)\n\n$ pixi run lint\nevery check passes except shellcheck (50 x SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' <the 8 changed .cc files, tests included>\nno findings\n\n$ ANTB1_DIFF_COUNT=20000 pixi run diff-random\nantb1-slt diff: seed=4047273273 queries=20000 supported=20000 target=5029 compared=20000 unsupported=0 rejected=0 answered_outside_supported=0 failed=0\nDIFF: PASS seed=4047273273 queries=20000 failed=0 unsupported=0\n\n$ pixi run slt-complete   (then git diff tests/slt: only the two new setops files change)\n$ pixi run test -R 'tpcds\\.status'\n100% tests passed out of 10   (sf0_1, the four sf1 shards, parallel sf0_1, harness and fixtures)\n```\n\nNot run locally (macOS host): `pixi run asan`, `ci-gcc` and the\nwhole-tree `tidy`, which CI runs. The ASan\nprojection above comes from #265's ASan log and local timings.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): `ci` passes; lint as above\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ratchet\n`tests/data/tpcds_status.json` (+8), ADR 0036's status (Accepted) and\nthe SF 1 sharding, all agreed in the\n      session\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5).\n- A read-only workflow of five agents mapped the code this change\ntouches.\n- Claude wrote the binder change, the tests, the slt SQL (DuckDB wrote\nthe expected results) and the docs, and\n    measured the ASan budget.\n- A `reviewer` subagent reviewed the diff: no P0. Two of its P1s are\nfixed in the last commit: the docs' DECIMAL\nrule for an integer, and a unit test for the `--shard` parsing. Its\nother two ask a maintainer to confirm the\nratchet edit and ADR 0036's status change, which were agreed in the\nsession and are listed in the checklist.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-31T01:55:46+03:00",
          "tree_id": "a40cc25d0702a3e9ff8ec6a8d48086517cf5540a",
          "url": "https://github.com/ydb-campus/antb1/commit/56c4661e070f8ecf1f6a07e6a806111d816d5636"
        },
        "date": 1793401602652,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 3846.042837563735,
            "unit": "ns/iter",
            "extra": "iterations: 179492\ncpu: 3845.914486439507 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 43617.28717093241,
            "unit": "ns/iter",
            "extra": "iterations: 16182\ncpu: 43616.28272154246 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 81618.70467199586,
            "unit": "ns/iter",
            "extra": "iterations: 8262\ncpu: 81615.54829339146 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 216204.11280486552,
            "unit": "ns/iter",
            "extra": "iterations: 3280\ncpu: 216195.59786585363 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 426460.25719533453,
            "unit": "ns/iter",
            "extra": "iterations: 1633\ncpu: 426431.54378444573 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1213922.9896906705,
            "unit": "ns/iter",
            "extra": "iterations: 582\ncpu: 1213852.357388316 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.02462155555501,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 40.02244333333331 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 40.262466055556466,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 40.2615207777778 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 160.41047099999162,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 160.39943774999978 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 11.934280306451594,
            "unit": "ms/iter",
            "extra": "iterations: 62\ncpu: 11.933959209677422 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 17.682484605263415,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 17.68170781578945 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 32.549398380952184,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.54589266666672 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.8904313087431523,
            "unit": "ms/iter",
            "extra": "iterations: 366\ncpu: 1.890255448087434 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 31.48663259090964,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.48454704545452 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 10.37528389230759,
            "unit": "ms/iter",
            "extra": "iterations: 65\ncpu: 10.374361323076926 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 12.032492896552107,
            "unit": "ms/iter",
            "extra": "iterations: 58\ncpu: 12.031615741379273 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 33.554943428571605,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.55307890476193 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 55.45706372727595,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 55.45544981818198 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 168.15605575000347,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 168.15542974999966 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 941.8605999999841,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 941.8130240000017 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 13.210324799999853,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.210127500000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 16.28326199999916,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 16.28217002272724 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 33.64044530000001,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 33.640165299999936 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 66.41101019999951,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 66.40539280000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 205.32285699999875,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 205.31702033333352 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1116.9640049999998,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1116.9078259999985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 42.889740312499924,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 42.88723031250008 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 45.27165862499771,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 45.27066756249987 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 73.25342209999803,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 73.2502052000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 113.32159566666178,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 113.31601950000032 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 328.4496334999858,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 328.44486350000324 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1606.1074599999756,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1606.0006340000043 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 53.69402015384576,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.68983315384613 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 52.702462769231644,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.70025453846144 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 87.00308762499986,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 87.00042212499959 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 140.0477539999997,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 140.03568880000046 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 369.7004324999966,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 369.6872420000012 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 1879.0456920000338,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1878.925144 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 10.43280565079295,
            "unit": "ms/iter",
            "extra": "iterations: 63\ncpu: 10.432633793650817 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 11.696213949152233,
            "unit": "ms/iter",
            "extra": "iterations: 59\ncpu: 11.695315559321998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 33.1935573809525,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.192621476190304 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 55.2691869090929,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 55.263348818181676 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 164.90462024999886,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 164.88397475000127 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 13.248046358489988,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.246915301886744 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 16.078706348837333,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.077201372093054 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 34.076883714287,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 34.074212952380854 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 69.58608133333479,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 69.57525844444412 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 207.88609700000885,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 207.88474733333115 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 10.907762985294266,
            "unit": "ms/iter",
            "extra": "iterations: 68\ncpu: 10.907441779411771 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 12.324973196428905,
            "unit": "ms/iter",
            "extra": "iterations: 56\ncpu: 12.324943642857253 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 39.887245555553996,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.882263666666596 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 68.10124930000256,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.09911150000048 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 122.70884959999648,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 122.70442639999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 13.190820039215744,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.189715666666675 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 17.36863349999993,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.36718697500006 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 41.7941762500007,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 41.79310950000037 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 78.69909699999766,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 78.69215737500035 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 148.77798575000156,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 148.76035800000054 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 13.22232774074063,
            "unit": "ms/iter",
            "extra": "iterations: 54\ncpu: 13.221801314814899 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 14.373712020834262,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.372995812499994 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 21.38342539394051,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.383143727273037 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 29.844159478260657,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.842520565217658 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 68.48101266666617,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 68.47935466666715 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 13.289095269231465,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.288572788461568 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 16.455269434783055,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 16.45475828260879 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 23.824555566667033,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.823460799999907 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 45.16530775000168,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 45.16124616666654 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 180.7857487499973,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 180.77149275000082 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 13.410031529412093,
            "unit": "ms/iter",
            "extra": "iterations: 51\ncpu: 13.408726215686226 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 14.968666543478301,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 14.967326630434556 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 24.705012035714244,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.703810750000038 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 45.246220928569464,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 45.244240000000346 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 190.78671625000254,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 190.7777412499989 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 13.59348371153802,
            "unit": "ms/iter",
            "extra": "iterations: 52\ncpu: 13.593218884615236 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 16.47237604651293,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.468378209302422 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 37.940146888887384,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 37.93854488888826 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 52.37199059999966,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 52.37039400000043 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 104.43328049999916,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 104.42911733333204 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 13.392226018868957,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 13.391843226415022 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 17.03096075000019,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.03058252500007 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.45811605555759,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.45637944444431 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 55.38332529999934,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 55.38137309999911 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 105.64116966666386,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 105.63954466666559 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 15.84166981818152,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.840088727272736 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 17.504607424999108,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.503152075000017 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 26.536746925924287,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 26.534884518518446 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 44.18751714285918,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 44.182428642856614 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 89.47987871428202,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 89.47080428571448 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 16.02289472093035,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.02169965116284 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 18.252478325000254,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 18.250624450000075 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 29.887903565217446,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.88540769565193 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 65.48320588888777,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 65.47867744444444 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 204.24489966666215,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 204.2452790000008 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 15.982108604651616,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 15.981458372092884 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 18.639267205127197,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 18.63886192307718 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 32.13999850000123,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.13867449999955 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 67.90933655555085,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 67.90879700000119 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 230.1379823333415,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 230.10788399999646 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 17.07004584615427,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 17.068784435897427 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 20.102799599999344,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 20.101171799999765 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 47.26660008333283,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 47.26440724999984 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 67.33866540000122,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 67.33380370000077 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 164.10526639999716,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 164.10306399999968 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 16.270260093023786,
            "unit": "ms/iter",
            "extra": "iterations: 43\ncpu: 16.26953093023274 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 20.08420076470643,
            "unit": "ms/iter",
            "extra": "iterations: 34\ncpu: 20.083787852941104 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 46.63794407692352,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 46.63572230769284 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 64.88516999999919,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 64.88472454545435 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 160.19959874999756,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 160.19344949999947 ms\nthreads: 1"
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
          "id": "6621eccbb0fca057ff2266f79de779ee2eb05654",
          "message": "feat(plan): cross products of one-row inputs (#268)\n\n## Summary\n\nantb1 now answers a cross product when every relation that no join key\nconnects is **one row**. Such a relation is a\nderived table or a CTE reference whose query is an aggregate without\n`GROUP BY`, `HAVING`, `LIMIT` or `OFFSET`, and\nthat no key, residual or condition of its own reads. A subquery conjunct\nmay read it.\n- The TPC-DS ratchet goes from **46 to 48**: queries 28 and 88.\n- Every other cross product stays refused with exit 4, with the same\nmessage: \"a cross product is not supported: no\njoin key connects 'x' to 'y'\". General cross products remain ADR 0022's\ndeferred item.\n\n**Binding** (`src/plan/binder.cc`):\n- **`JoinsAsOneRow`.** A relation joins as one row only when all of\nthese hold:\n  - it is not a LEFT JOIN unit;\n- it has no routed condition (scan filter, computed filter), no join-key\nexpression and no pushed-down semi join;\n  - no edge and no residual of its block reads it;\n- its sub-plan is an `Aggregate` under `Project`/`Compute` only\n(`ExactlyOneRow`). `GROUP BY`, `HAVING`,\n    `LIMIT`, `OFFSET` and `UNION ALL` all put another node on top.\n\nA condition on the relation alone stays refused, since it could leave\nthe join's right input with no row. The\n`ONE-ROW` join would then report that as a planner bug (runtime\n`Invalid`) instead of returning no rows. A subquery\nconjunct that reads it (`IN`, `NOT IN`, a comparison with a scalar\nsubquery) is answered: it joins above the\none-row join, as above every join of its block, and filters the joined\nrows.\n- **`OrderBlock`.** A connected graph is ordered exactly as before.\nOtherwise:\n- the core is what the keys connect to the first relation that is not\none-row (or to the first relation, when all\n    are);\n- every relation outside the core must join as one row, or the existing\nrefusal names it against the core's\n    first relation;\n- the core alone goes to `OrderJoins`, renumbered, and its steps are\nmapped back to the block's indices.\n- **`JoinTree`.** The core's tree (`CoreTree`, the former step loop)\ngets one `JoinKind::kOneRow` join per one-row\nrelation above it, in FROM order: no keys, built on the right, with the\ncore's estimate. This is the existing join\n  of scalar subqueries; no exec change.\n- **`PushableSemiJoin`.** A semi join over a one-row relation's column\nstays above its one-row join, so it filters\n  the joined rows rather than the relation's one row.\n\n**Tests:**\n- **`binder_test`:**\n  - EXPLAIN shapes:\n    - two and three one-row inputs;\n    - a one-row input first in FROM (the core still prints first);\n    - a CTE reference;\n    - a keyed one-row relation (an ordinary inner join);\n    - a semi join above the one-row join;\n- a one-row input first before a core of three with a residual, and one\nin the middle of FROM. These pin the\ncore's renumbering: with the mapping back to block indices removed, the\ntest fails (checked).\n  - Refusals, with their carets:\n    - a condition or a residual on the one-row relation;\n    - `HAVING`, `LIMIT` and `GROUP BY` sub-plans;\n    - a `UNION ALL` of aggregates;\n- a one-row input first in FROM with a table no key connects (named\nagainst the core's first relation).\n- **slt, `tests/slt/cases/joins/one_row.slt` (25 records):** expected\nresults written by DuckDB through\n`pixi run slt-complete`, also run on 4 threads as\n`parallel.joins.one_row`. It covers:\n  - one-row inputs alone;\n- next to tables: first, in the middle, last, and before a core of three\nwith a residual;\n  - next to a join with `GROUP BY`;\n  - inside and next to a `LEFT JOIN`;\n  - a CTE reference;\n  - subquery conjuncts that read a one-row input;\n- one-row inputs in the blocks of a `UNION ALL`, and the aggregate of a\nunion as one;\n  - inputs with no rows;\n  - a DuckDB-only section for the forms that stay refused.\n\n**TPC-DS-derived queries (local, dev build):**\n- Queries 28 and 88 pass at SF 0.01, 0.1 and 1, equal to DuckDB. At SF\n1, 4 threads give byte-identical answers to\n  1 thread.\n- Queries 61 and 90 used one-row cross products too; they now fail first\non general CAST (exit 4, as before).\n- **ASan budget.**\n- Both queries fall in shard `tpcds.status.sf1.4of4` (every fourth\nquery). They take 5.4 s and 5.1 s at SF 1 in the\n    dev build, so the shard goes from about 41 s to 52.3 s locally.\n- Measured under ASan (the `ci-asan` build, run directly on this Mac):\n15.6 s and 13.4 s, which is 18% of the\n    shard's 160.8 s of queries.\n- On #267's ASan run, shard 4 took 390 s against the test's 870 s limit.\nScaled by that share, it projects to about\n    475 s.\n\nThis workload is derived from the TPC-DS Benchmark and is not comparable\nto published TPC-DS Benchmark results, as\nthis implementation does not comply with all requirements of the TPC-DS\nBenchmark.\n\n**Docs:**\n- `docs/sql-subset.md`:\n  - what works;\n  - the exit-4 list;\n- the logical-plan rule for `ONE-ROW` joins, including the subquery\nconjuncts that may read a one-row input;\n- the TPC-DS table: Q28 and Q88 pass, and \"cross products of one-row\ninputs\" leaves the needs of Q61 and Q90.\n- `docs/architecture.md`: the binder paragraph's cross-product sentence\nnames the exception.\n- ADR 0022: one update line next to the scalar-subquery exemption from\nthe connectivity rule.\n- `src/plan/join_order.{h,cc}`: four comments no longer say the binder\nrefuses every disconnected graph.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run ci\n100% tests passed out of 3131   (1 skipped: the window-function canary, as on main)\n\n$ pixi run lint\nevery check passes except shellcheck (50 x SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' src/plan/binder.cc src/plan/join_order.cc src/plan/tests/binder_test.cc\nno findings\n\n$ pixi run slt-complete   (then git diff tests/slt: only tests/slt/cases/joins/one_row.slt changes)\n\n$ pixi run test -R 'tpcds'\n100% tests passed out of 15   (sf0_1, the four sf1 shards, parallel sf0_1, harness and fixtures)\n\n$ ANTB1_TPCDS_TIMES=1 pixi run test -R '^tpcds\\.status\\.sf1\\.4of4$' -V\nQ28: pass (5.356 s)\nQ88: pass (5.143 s)\nTPCDS: PASS queries=24 ... unsupported=11 rejected=0 failed=0\n\n$ pixi run asan   (builds build/ci-asan; ctest then stops at discovery on macOS's curl_global_init leak)\n$ ASAN_OPTIONS=detect_leaks=0 ANTB1_TPCDS_TIMES=1 build/ci-asan/bin/antb1-slt tpcds ... --shard 4/4\nQ28: pass (15.618 s)\nQ88: pass (13.426 s)\nTPCDS: PASS queries=24 ... unsupported=11 rejected=0 failed=0   (160.8 s of queries, no ASan report)\n\nMutation check: with OrderBlock's mapping of the core's step operands back to block indices removed,\nplan.BinderTest.OneRowRelationsJoinAsOneRow fails; restored, it passes.\n```\n\nNot run locally (macOS host): the ASan test suite as a whole, `ci-gcc`\nand the whole-tree `tidy`, which CI runs.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests), but for shellcheck's\n      macOS-only findings in scripts this PR does not touch\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ADR 0022 update\n      line and the ratchet, each approved in the session\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the change, its tests and docs, and ran\nthe verification above. Its `reviewer` subagent reviewed the diff and\nfound no wrong answers. It found four test\n  and docs gaps, all fixed here.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-31T03:13:28+03:00",
          "tree_id": "4ef12be91309b981822558ce5e53015a73852ab8",
          "url": "https://github.com/ydb-campus/antb1/commit/6621eccbb0fca057ff2266f79de779ee2eb05654"
        },
        "date": 1793406311158,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 4153.997876920625,
            "unit": "ns/iter",
            "extra": "iterations: 168623\ncpu: 4153.970039674303 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 46140.836475086544,
            "unit": "ns/iter",
            "extra": "iterations: 15172\ncpu: 46138.87997627207 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 89572.74263009458,
            "unit": "ns/iter",
            "extra": "iterations: 7802\ncpu: 89573.06395795953 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 232956.06123128178,
            "unit": "ns/iter",
            "extra": "iterations: 3005\ncpu: 232942.41863560732 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 444040.4974619315,
            "unit": "ns/iter",
            "extra": "iterations: 1576\ncpu: 444041.4866751268 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1299655.306691498,
            "unit": "ns/iter",
            "extra": "iterations: 538\ncpu: 1299579.977695167 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 46.493677599998286,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.49177926666663 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 45.421748666668314,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.417316333333346 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 187.05637400000796,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 187.05006900000024 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.028488277778175,
            "unit": "ms/iter",
            "extra": "iterations: 54\ncpu: 13.027892240740739 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 20.929866272726112,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 20.929633090909093 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 41.92271464706089,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 41.91838564705881 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.9628174772726614,
            "unit": "ms/iter",
            "extra": "iterations: 352\ncpu: 1.9627393267045434 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 43.71884142857344,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 43.71522199999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 12.647306618182277,
            "unit": "ms/iter",
            "extra": "iterations: 55\ncpu: 12.64667158181816 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 14.963114489361526,
            "unit": "ms/iter",
            "extra": "iterations: 47\ncpu: 14.96235157446809 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 38.295975944445594,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.295385388888945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 67.69280333333603,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 67.68912566666667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 228.3535103333255,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 228.34678466666603 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1137.392454999997,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1136.6680420000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 16.623154095238338,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.62256609523807 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 19.918094914286094,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.91554991428574 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 40.76853094117643,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.76562211764688 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 84.68101025000152,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.67276825000036 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 273.90994450001926,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 273.8784540000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1429.4619300000022,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1429.3272859999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 49.17031964285862,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.16482785714292 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 50.97793749999774,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 50.973177928571495 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 81.00430574999962,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 80.99714499999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 144.08769760000268,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 144.07939420000062 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 403.9894689999812,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 403.9503975000009 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1876.7238870000256,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1876.5741759999983 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 58.80146233333031,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.7943257500001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 60.08885558333077,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 60.08564333333351 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 95.37400442857395,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.36988585714263 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 169.70603374998916,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 169.70389475000047 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 437.57055249997734,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 437.5496150000018 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2106.5791970000305,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2106.489466999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 12.752201641509352,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 12.751639830188767 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 14.625976979165975,
            "unit": "ms/iter",
            "extra": "iterations: 48\ncpu: 14.625412416666705 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 38.68121983333279,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.6794034444446 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 69.01419677777514,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 69.01057311111143 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 229.86649666665926,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 229.8577306666682 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 16.605890024390824,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 16.605305780487875 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 19.748724571427697,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.747720285714365 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 40.71774117646916,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.7156404117648 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 82.38035987499615,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 82.37760950000083 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 282.6275945000134,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 282.6223739999989 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 12.47946501886855,
            "unit": "ms/iter",
            "extra": "iterations: 53\ncpu: 12.478818301886731 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 14.796989369566074,
            "unit": "ms/iter",
            "extra": "iterations: 46\ncpu: 14.796935630434898 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 45.53159313333405,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.5291492666665 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 83.16006750000327,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 83.16000449999983 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 155.209993750006,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 155.19858050000045 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 16.249295261905857,
            "unit": "ms/iter",
            "extra": "iterations: 42\ncpu: 16.249037071428603 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 21.5797692727275,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.578289424242403 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 49.057383538459256,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 49.05596692307658 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 94.30480557142573,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 94.2993308571433 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 192.01238475000082,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 191.989465999999 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 15.497882044443637,
            "unit": "ms/iter",
            "extra": "iterations: 45\ncpu: 15.495844244444218 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 17.108896384615655,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 17.10685071794871 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 27.10385992000056,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.10113607999972 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 41.35533388235297,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 41.35186429411754 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 88.06791071428474,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 88.05862042857119 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 15.337558704545854,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.33621495454537 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.380665924999562,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 18.37934552500009 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 30.07034479166748,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 30.06669695833312 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 66.76743640000495,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 66.7663263999998 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 215.83941066667953,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 215.82896233333315 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 15.610426666666577,
            "unit": "ms/iter",
            "extra": "iterations: 45\ncpu: 15.609964400000031 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.005651925000166,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 18.002661024999966 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 33.398637549998966,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 33.3960452999996 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 70.29633562500237,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 70.28979087499998 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 256.8043126666642,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 256.8027193333317 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 15.59325713636316,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 15.59229527272747 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 19.84542397142864,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.84527108571399 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 48.723405466663884,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 48.717389933333756 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 73.45469500000235,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 73.45427762500023 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 143.93750300000647,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 143.92772520000108 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 16.14369570454502,
            "unit": "ms/iter",
            "extra": "iterations: 44\ncpu: 16.143063363636337 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 20.298710800000208,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 20.297795600000004 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 46.547722866667606,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.54801400000016 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 74.00197966666786,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 73.99734555555584 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 154.38943740000468,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 154.38388020000104 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 18.926891459458957,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.925889891891675 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 21.296946060605286,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.297004060605992 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 36.02763824999897,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 36.02513219999963 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 60.44766299999599,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 60.44741427272852 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 110.4143618333353,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 110.40903883333424 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 19.00534883783793,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 19.005225378378626 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 21.13401360606021,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.13320193939389 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 39.80153735294072,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 39.79873547058834 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 83.98135499999881,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 83.9767389999988 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 254.95987633333547,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 254.95782000000133 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 18.88990154054067,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.887967081081026 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 21.39700506060735,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.39579675757547 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 42.91533512499868,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 42.9059623125001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 88.72153566665968,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 88.72166150000045 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 279.61663166666995,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 279.5829523333329 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 19.63393325714183,
            "unit": "ms/iter",
            "extra": "iterations: 35\ncpu: 19.632617599999744 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 24.09792131034441,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.094652310344795 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 56.99637854545162,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 56.99255372727211 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 97.03685028571434,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 97.03058857143024 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 205.19002874999615,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 205.17411850000045 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 19.308816666667084,
            "unit": "ms/iter",
            "extra": "iterations: 36\ncpu: 19.306538749999746 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 24.262132275862704,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.259706620689524 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 57.06323659999839,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 57.06144669999986 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 97.16680449999633,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 97.15950133333469 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 207.71938675,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 207.71106299999786 ms\nthreads: 1"
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
          "id": "e12aeac72e0086b7ca1ef7da868f5832047270c6",
          "message": "feat(sql,plan): stars next to other select items (#269)\n\n## Summary\n\nantb1 now answers stars among the select items, with DuckDB 1.5's\nsemantics: `*` and `t.*` anywhere in the list, any\nnumber of times, next to columns, expressions, constants and a grouped\nquery's aggregates.\n- The TPC-DS ratchet goes from **48 to 50**: queries 23 and 62. Query 99\nnow needs only upper and lower.\n- This is PR 1 of 2, as agreed. PR 2 teaches the random generator to\nwrite stars; until then `star_items` and\n  `qualified_star` wait in `kGeneratorPending`.\n\n**Behavior**\n\nAnswered, as in DuckDB:\n- **Expansion in place.** `*` gives every FROM item's columns in FROM\norder, `t.*` its one FROM item's. Columns keep\ntheir declared names, so duplicates stay at the top level; a derived\ntable or CTE makes them unique (`a_1`) after\n  its alias list.\n- **Counting and keys.** Positions count a star's columns. Every star\ncolumn must be a GROUP BY key. The\naggregate-mixing error points at the first plain column in select order,\na star's included.\n- **Other places.** UNION ALL widths and type refusals; IN and scalar\ncolumn counts; LEFT JOIN NULL padding.\n\n`t.*` (new `Scope::StarBinding`):\n- **The one FROM item named t, in its own block.** None is a bind error,\nalso when only an enclosing block has the\nname: a star never correlates (DuckDB: \"Referenced table not found\").\nTwo are a bind error whatever their columns.\n- **A STRUCT column of that name** exits 4, since DuckDB expands its\nfields.\n\nORDER BY names, DuckDB's three steps:\n1. **Aliases.** DuckDB counts a plain column written otherwise than\ndeclared (`t.K` for `k`) as an alias of the\ndeclared name. It and the written aliases match in any case, and the\nlast wins. This fixes a wrong row order that\npredates this PR: `SELECT x AS k, K FROM v ORDER BY k` sorted by `x`;\nDuckDB sorts by `k`.\n2. **The select-list step.** A name that FROM finds ambiguous but\nexactly one un-aliased column item carries as\ndeclared, a star's column included, exits 4 until a follow-up PR answers\nit, as you chose.\n3. **FROM.** Otherwise the name is a FROM column.\n\nExit 4:\n- star modifiers after any star (EXCLUDE, REPLACE, RENAME, and LIKE,\nILIKE, GLOB or SIMILAR TO, also after NOT,\n  before a string);\n- an alias after a star before `,` or FROM (`* AS x`, `* AS ''`), which\nDuckDB gives every column;\n- an ORDER BY position over an EXISTS block's star, which ADR 0023\nchecks but does not expand;\n- `t.*` inside an expression (unchanged).\n\nExit 1, as in DuckDB:\n- an operator after a star (`* + 1`, `t.* + 1`; `t.* + 1` exited 4\nbefore);\n- ISNULL, NOTNULL and AT TIME ZONE after a star;\n- a star with no FROM;\n- every malformed modifier or alias (`t.* u.*`, `* LIKE b`, `* NOT\nSIMILAR TO b`).\n\nError order: every star's name is checked first, as DuckDB expands its\nstars before anything else.\n\n**Design**\n\n- **AST** (`ast.h`, a public header; designed in plan mode):\n- a star is a kind of select item: `SelectItem::value` is\n`std::variant<SelectExpr, SelectStar>`, read through\n    `expr()` and `star()`;\n- `SelectStatement::star` and `star_span` are gone\n(`kSelectStatementFields` goes from 19 to 17), so every reader of\n    an item had to be revisited;\n- a new tripwire, `kSelectItemKinds`, is asserted at every walk that\ntells the kinds apart.\n- **Parser:** `StartsStar` and `ParseStar`. A star's qualifier follows a\ncolumn's qualifier rules, as DuckDB's does.\n  Modifiers and aliases are checked within the three-token lookahead.\n- **Binder:**\n- `BindSelectList` is one loop, after a pre-pass over every star's name;\n  - an EXISTS block records each star's range and its columns' names;\n- `SelectList` gains per-column spans, which `BoundBlock` carries, so\nthe UNION ALL caret finds the star a column\n    expands from;\n- `FindOrderAlias` and `OnlyTheSelectListResolves` implement the ORDER\nBY steps.\n- **Harness:** `MakeOrderedQuery` declines a position at or after a star\nand builds the augmented query through\n  `ToSql`.\n\n**Tests**\n- **`parser_test`:** accepted forms with spans, and the modifier, alias,\noperator and FROM-less cases.\n- **`unparse_test`:** canonical forms, and `EqualIgnoringSpans`\ndifferences.\n- **`parser_property_test`:** it generates stars among items, and its\ntoken accounting counts them.\n- **`binder_test`:** the `StarItems` error suite (`t.*` lookup, STRUCT,\nerror order, aggregate mixing, GROUP BY,\npositions, EXISTS ranges, ORDER BY steps), in-place expansion and ids,\nthe UNION ALL caret, EXISTS, and implicit\n  aliases.\n- **`scope_test`:** `StarBinding`.\n- **`ordered_compare_test`, `limit_compare_test` and\n`query_gen_oracle_test`:** DuckDB reads the star canonical forms\n  alike.\n- **`table_alias_oracle_test`:** for every DuckDB keyword, `kw.*` is\nclassified as `kw.x` is.\n- **slt:**\n- `tests/slt/cases/projection/star_items.slt` (36 records, written by\nDuckDB through `pixi run slt-complete`, also\n    run on 4 threads), with DuckDB-only sections for the refused forms;\n  - two records in `orderby/order.slt` for implicit aliases.\n- **CLI goldens:** names in table, csv and json (json repeats a\nduplicate key, as DuckDB's CLI does); the alias\n  refusal; a `t.*` bind error.\n- **Fuzz:** six seeds; `.*` moved into the subset part of\n`fuzz/sql.dict`.\n\n**Review**\n- **Plan.** A read-only workflow mapped every site. Three adversarial\ncritics reviewed the plan before any code was\n  written.\n- **Code.** A review workflow ran four independent finders: the repo's\n`reviewer`, a DuckDB differential prober\n(about 3,000 queries), a code reader and a tests-and-docs auditor. A\nskeptic then tried to refute each finding.\n23 of 27 findings held up, and the second commit fixes every one that\nconcerns this PR.\n- **Re-verification.** Two agents re-checked the fixes over about 28,000\nqueries. Wherever both engines answered,\nrows and order matched DuckDB, and the documented ORDER BY rule held\neverywhere. They found four more edges, and\nthe third commit fixes three of them: a pattern in parentheses, a\ndollar-quoted alias, and the harness's ORDER BY\nkeys next to implicit aliases. The fourth follows antb1's rule that an\nunsupported construct is reported before\na later error: `SELECT * AS x, 1` without FROM exits 4, where DuckDB\nreports the missing FROM.\n- **Found but not fixed here (each predates this PR, and stars only make\nthem easier to reach); follow-up PRs:**\n- GROUP BY, HAVING and an ORDER BY expression fall back to a select\nalias when a name is ambiguous in FROM, and\n    then answer where DuckDB reports the ambiguity;\n- a select alias used in WHERE is a bind error (exit 1), which DuckDB\nanswers;\n- an alias written as a dollar-quoted string (`k AS $$x$$`, `* AS\n$$x$$`), or `k AS E'x'`, is a syntax error, which\n    DuckDB answers.\n\n**TPC-DS-derived queries (local, dev build)**\n- Queries 23 and 62 pass at SF 0.1 and 1, equal to DuckDB, with 4\nthreads equal to 1 thread at SF 0.1.\n- Every other query is unchanged: still passing, or still exit 4. Query\n99 now fails first on upper and lower.\n- **ASan budget:**\n- At SF 1 in the dev build, query 62 takes 0.5 s and query 23 11.9 s, in\nshard `tpcds.status.sf1.3of4`.\n- Measured under ASan (the `ci-asan` build, run directly here): query 23\ntakes 38.0 s, 24% of its shard's 158.0 s\n    of queries, with no ASan report.\n- On #267's ASan run that shard took 380 s, so it projects to about 500\ns of the test's 870 s limit.\n\nThis workload is derived from the TPC-DS Benchmark and is not comparable\nto published TPC-DS Benchmark results, as\nthis implementation does not comply with all requirements of the TPC-DS\nBenchmark.\n\n**Docs**\n- **`docs/sql-subset.md`:**\n  - what works, the grammar, qualified names and the canonical form;\n  - the EXISTS rules, the exit-4 list and the Known gaps;\n  - the error order;\n- the Names, Select list, Positions, GROUP BY, Result names and ORDER BY\nbullets;\n  - the UNION ALL caret, the `Project` line and json's repeated keys;\n  - D21;\n  - the TPC-DS rows 23, 62 and 99.\n- **ADR update notes** (protected, each approved in the session): 0022\n(rules 3, 5 and 9, the exit-4 and bind-error\n  lists), 0023 (stars in EXISTS) and 0036 (the caret).\n- **Elsewhere:** `docs/architecture.md`,\n`docs/recipes/add-sql-feature.md` (the new tripwire), `binder.h`'s\n`Bind()`\ncomment, `unparse.h`'s canonical form, and `tests/slt/README.md`\n(`kGeneratorPending`).\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run ci                      (on 87df934)\n100% tests passed out of 3226   (1 skipped: the window-function canary, as on main)\n                                (tpcds.status.sf0_1, the four sf1 shards and parallel.tpcds.status.sf0_1\n                                 among them; queries 23 and 62 pass)\n\n$ pixi run lint\nevery check passes except shellcheck (50 x SC2218, macOS only, in scripts this PR does not touch)\n\n$ clang-tidy -p build/dev --warnings-as-errors='*' <the 17 changed .cc files, tests included>\nno findings\n\n$ pixi run slt-complete   (then git diff tests/slt: only star_items.slt and orderby/order.slt change)\n\n$ pixi run diff-random\nDIFF: PASS seed=195109699 queries=2000 failed=0 unsupported=0\n\n$ pixi run fuzz-smoke\n200000 runs, no crash and no artifact; LeakSanitizer then reports libFuzzer's own RSS thread at exit\n(fuzzer::StartRssThread, no antb1 frame), a gap of this macOS host\n\n$ pixi run asan   (builds build/ci-asan; ctest then stops at discovery on macOS's curl_global_init leak)\n$ ASAN_OPTIONS=detect_leaks=0 ANTB1_TPCDS_TIMES=1 build/ci-asan/bin/antb1-slt tpcds ... --shard 3/4\nQ23: pass (38.016 s)\nTPCDS: PASS queries=25 ... unsupported=10 rejected=0 failed=0   (158.0 s of queries, no ASan report)\n```\n\nNot run locally (macOS host): the ASan test suite as a whole, `ci-gcc`\nand the whole-tree `tidy`, which CI runs.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests), but for shellcheck's\n      macOS-only findings in scripts this PR does not touch\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ADR update notes\n      and the ratchet, each approved in the session\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the change, its tests and docs, and ran\nthe verification above. Its workflows reviewed the design and the code,\nand its `reviewer` subagent reviewed the\n  diff.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\n---------\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-31T12:41:00+03:00",
          "tree_id": "57df6ef265dd8b7add396549dcd2365c30d25ec7",
          "url": "https://github.com/ydb-campus/antb1/commit/e12aeac72e0086b7ca1ef7da868f5832047270c6"
        },
        "date": 1793440438569,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 5151.689669631221,
            "unit": "ns/iter",
            "extra": "iterations: 135939\ncpu: 5151.239688389646 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 30548.504782099786,
            "unit": "ns/iter",
            "extra": "iterations: 23107\ncpu: 30546.22252996928 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52143.70316938091,
            "unit": "ns/iter",
            "extra": "iterations: 13378\ncpu: 52139.44931977873 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 233007.02480979823,
            "unit": "ns/iter",
            "extra": "iterations: 3023\ncpu: 233005.2778696659 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 499403.7435714306,
            "unit": "ns/iter",
            "extra": "iterations: 1400\ncpu: 499370.8021428568 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1507787.157205296,
            "unit": "ns/iter",
            "extra": "iterations: 458\ncpu: 1507749.7074235808 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 39.69326466666543,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.69023111111111 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 46.467045066663104,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.465531800000015 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 197.34258033332708,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 197.33784200000017 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.907529419999491,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.90720481999999 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 24.210262862068483,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.20845475862074 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 35.962092684210226,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 35.96112694736834 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.3910475009980499,
            "unit": "ms/iter",
            "extra": "iterations: 501\ncpu: 1.3909523193612767 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 98.6283085000063,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 98.62694133333338 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.862372967740757,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.816988064516153 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.710398142858025,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.709911178571417 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 53.54685053846162,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.54034907692305 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 78.98145600000186,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 78.97840100000013 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 251.03864833333014,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 251.01885333333368 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1244.2342600000416,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1244.1773629999986 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.47913376923022,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.47712934615388 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 30.94644439130434,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.94612578260874 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 54.059506153844964,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.05619592307705 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 93.56855842857418,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 93.56709414285699 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 314.75354050002124,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 314.71509600000047 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1490.2015010000014,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1490.1279559999984 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.00685724999721,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.00367741666664 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 58.65111649999714,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.64642516666674 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 85.03228049999478,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.02919799999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 145.37895300001082,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 145.3705706000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 414.1724369999906,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 414.1167500000016 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1866.2744140000314,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1866.14728 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.18293518182232,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.17595236363667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.53308690000404,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.53051940000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 96.80274342857079,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.79918300000067 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 169.69450324999968,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 169.6849117500001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 425.0078260000123,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 424.9987735000005 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2007.5991339999746,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2007.4754970000015 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 22.90749187096708,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.90631825806439 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 24.5075182758624,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.506246137930972 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 55.931283416668975,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.92935941666669 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 76.67250933333334,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 76.66275388888897 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 253.99270266666463,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 253.97203566666585 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 27.500413576920945,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.498363576923225 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 31.310737590907614,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.310094272727312 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 53.93966115384676,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 53.93392184615373 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 94.63983399999572,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 94.634695571429 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 321.4587184999971,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 321.42722500000076 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 22.788367741934362,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.787258451613056 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 25.27436592856905,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.271701571428554 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 58.28604100000234,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.28379791666668 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 96.37522242857293,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.37150385714322 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 200.26374533332122,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 200.258256666667 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 27.401141269229775,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.400121538461576 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 33.8205443999982,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 33.81952129999988 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 61.77278563637339,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 61.77070945454556 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 118.51375566667836,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 118.50665783333268 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 225.2359850000024,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 225.22975566666523 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.335274850000815,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.33414147500021 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.454445684211038,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.453974657894516 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 23.73392590000094,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.732662099999875 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 31.59114563636029,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.590388454545668 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 98.74879571428404,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 98.74242514285721 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.32820537500004,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.327239349999957 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.55287018421031,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.55154194736816 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 27.502317807694418,
            "unit": "ms/iter",
            "extra": "iterations: 26\ncpu: 27.50210219230754 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 49.2942757142844,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 49.28960950000061 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 239.46991066668488,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 239.45879066666956 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.358735414634836,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.344412731707123 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.501037026315938,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.499778789473595 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 28.448035040000832,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.445874999999887 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 50.89392129999624,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 50.88727239999997 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 234.79158833333713,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 234.76028566666685 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.5018477750001,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.498323450000086 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 23.92173446428712,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 23.918255928571394 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 36.994788842105805,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.966879210526685 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 53.22271690000662,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 53.21665440000061 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 141.8302229999881,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 141.8124014 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.299113249998754,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.297272425000187 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 23.88926458620674,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 23.887211448276236 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 37.44469452631524,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.44081852631615 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 55.58217199999986,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 55.58045918181811 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 147.69952839999405,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 147.69843280000146 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 22.657796935486513,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.6566405806453 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 24.05855717241311,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.05674941379316 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 29.451412166665136,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.449289458333066 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 48.9233714166725,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 48.92192599999964 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 118.34826983332884,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 118.33949433333402 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 22.654608258064048,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.65407645161267 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 24.275747965518725,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.27437637931056 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.06162099999966,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 34.058790999999985 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 70.79283570000143,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 70.78881090000095 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 264.55095533333406,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 264.52951533333174 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 23.647276433333293,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.645292400000056 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 25.605737962960287,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 25.60321340740704 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 36.58588494736729,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.58304600000032 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 76.36712988889687,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 76.35640688888954 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 273.24264433332246,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 273.2234386666666 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 23.68966176666542,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.685187100000366 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 32.89955295237979,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.89784171428575 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 43.898520812497566,
            "unit": "ms/iter",
            "extra": "iterations: 16\ncpu: 43.89636950000053 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 89.02142299999534,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 89.01848114285826 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 217.36170024999524,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 217.3416235000012 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 24.528334758621174,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.52751996551732 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 33.886467428568984,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.88515285714296 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 46.467438642853914,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 46.46600257142808 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 89.73265057142855,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 89.72432185714142 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 226.4052383333516,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 226.39888666666744 ms\nthreads: 1"
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
          "id": "148b49139ff63c9a1c01177a6feee334b2bfe8b2",
          "message": "ci(tpcds): run scale factor 1 under sanitizers only in a nightly asan job (#272)\n\n## Summary\n\nThe ratchet of the queries derived from TPC-DS at scale factor 1 runs as\neight shards, `tpcds.status.sf1.1of8` to\n`8of8`. It leaves the two sanitizer legs that run every hermetic test:\n\n- the PR leg `clang-asan (pixi run asan)`;\n- the nightly `tsan (pixi run tsan)`.\n\nA new nightly job, `asan-tpc-sf1 (pixi run asan-tpc-sf1)`, runs it under\nASan and UBSan instead.\n\nRefs #271: its `tsan` failure. Its `static-urls` failure is unrelated.\n\n**Why:**\n\n- **ASan on every PR.** #270's run was the first with eight shards.\nUnder ASan, the shard holding query 4 took 686 s of\nits 870 s limit (79%), where I had projected 66%. Query 4 alone is most\nof that, so more shards cannot help, and each\nnewly passing heavy query adds more. The eight shards were 35% of the\njob's summed test time.\n- **TSan nightly.** The nightly of 2026-10-31, still on four shards,\nfailed in `tsan` (#271): shard `4of4` hit the\nlimit, and `3of4` took 867 s. With eight shards, the shard holding query\n4 would still take roughly 90% of the limit\nunder TSan. The shards run one engine thread, so TSan learns little from\nthem.\n\n**What still runs:**\n\n- `gcc-compat` and `clang-coverage-fuzz` run the eight shards on every\nPR (16 to 143 s each on #270), so the answers at\n  scale factor 1 are still checked before a merge.\n- On every PR, `clang-asan` still runs all 99 queries at scale factor\n0.1 under ASan and UBSan, and also their 4-thread\n  run (`parallel.tpcds.status.sf0_1`). `tsan` runs both nightly.\n- What moves to after the merge is ASan and UBSan over the larger data\nof scale factor 1. A finding there opens the\n  `nightly-failure` issue instead of failing the PR.\n\n**The change:**\n\n- `tests/tpcds/CMakeLists.txt`: the shards' label is `tpc-sf1` instead\nof `oracle` (every test has exactly one label).\n- `CMakePresets.json`: the `ci-asan` and `tsan` test presets leave\n`tpc-sf1` out. New `asan-tpc-sf1` test and workflow\npresets run only that label on the `ci-asan` build, four at a time (one\nper runner core).\n- `pixi.toml`: the `asan-tpc-sf1` task.\n- `.github/workflows/nightly.yml`: the `asan-tpc-sf1` job, modelled on\n`asan-data`, and its entry in the failure report.\n- `.claude/settings.json`: pre-approves the task, as it does\n`asan-data`.\n- Docs:\n- the label in `cmake/Antb1Testing.cmake`, `docs/testing.md`, AGENTS.md\nand `tests/README.md`;\n  - `docs/ci.md`;\n- update notes in ADR 0006 (sanitizers on every PR) and ADR 0036 (the\nshard timings);\n- the TPC-DS disclaimer in the TPC-DS section of `docs/testing.md` and\nin `tests/tpcds/CMakeLists.txt`, which present\n    the workload's numbers.\n\n**Expected effect:**\n\n- `clang-asan` should drop from about 26 to about 18 minutes. That is an\nestimate from #270's log: a 4-minute build and\n  1331 s of tests. This PR's own run gives the real number.\n- The new nightly job should take 20 to 25 minutes of its 60. Its first\nrun is the nightly after the merge, or a manual\n  run.\n\nProtected paths, each edited with the maintainer's approval in the\nsession:\n\n- `CMakePresets.json`, `pixi.toml`, `.github/workflows/nightly.yml` and\n`.claude/settings.json`;\n- AGENTS.md and `cmake/Antb1Testing.cmake`;\n- the ADR notes (CODEOWNERS).\n\nThis workload is derived from the TPC-DS Benchmark and is not comparable\nto published TPC-DS Benchmark results, as\nthis implementation does not comply with all requirements of the TPC-DS\nBenchmark.\n\n## Type of change\n\n- [ ] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [x] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run lint\nevery check passes except shellcheck (47 x SC2218, macOS only, in scripts this PR does not touch)\n\n$ pixi run test -N -L '^tpc-sf1$'\nTotal Tests: 9   (tpcds.status.sf1.1of8 to 8of8, plus fixtures.tpcds, which ctest adds as their fixture)\n\n$ pixi run test -N -L '^oracle$'\ntpcds.status.sf0_1 among them; no tpcds.status.sf1.* test\n\n$ pixi run ci\n100% tests passed out of 3230   (1 skipped: the window-function canary, as on main)\n\n$ pixi run coverage   (run before the review fixes, which changed only docs, comments and the tsan preset)\n100% tests passed out of 3230; Coverage gate: PASS\n\n$ pixi run asan-tpc-sf1\nnot runnable on this Mac: ctest stops at gtest discovery on the known macOS-only curl_global_init leak under\nLeakSanitizer, as pixi run asan does\n```\n\nNot run here:\n\n- `pixi run asan` and `pixi run asan-tpc-sf1`: blocked by the macOS\ndiscovery leak above. This PR's `clang-asan` job\nshows the exclusion; the first nightly, or a manual run of `Nightly`,\nshows the new job.\n- `pixi run tsan`: its filter is the same as `ci-asan`'s; the next\nnightly shows it.\n- `pixi run ci-gcc`: the `gcc` environment is linux-64 only.\n- `pixi run tidy` and `pixi run fuzz-smoke`: no C++ or parser change.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests), except shellcheck's macOS-only\n      findings in scripts this PR does not touch\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed): no new test. The label\nselection was checked with `pixi run test -N -L`, and this PR's\n`clang-asan` run shows the exclusion.\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer, in the session\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did: Claude Code (Claude Opus\n5.5) wrote the change and ran the verification\nabove. Two review workflows checked it: the `reviewer` subagent,\nmechanics, docs, policy and completeness lenses, and\nan adversarial check of each finding. The first found the missing TPC-DS\ndisclaimer and the `tsan` timeout; both are\n  fixed here.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-10-31T17:18:08+03:00",
          "tree_id": "e1303e0726b94aae176e9c9fb8901469da857eef",
          "url": "https://github.com/ydb-campus/antb1/commit/148b49139ff63c9a1c01177a6feee334b2bfe8b2"
        },
        "date": 1793457078352,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 5223.523901460024,
            "unit": "ns/iter",
            "extra": "iterations: 133632\ncpu: 5222.480087104885 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 28501.683601549255,
            "unit": "ns/iter",
            "extra": "iterations: 24545\ncpu: 28497.15844367488 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 51923.439189696146,
            "unit": "ns/iter",
            "extra": "iterations: 13427\ncpu: 51911.16541297387 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 222957.57894737492,
            "unit": "ns/iter",
            "extra": "iterations: 3154\ncpu: 222884.0881420419 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 501451.1733899719,
            "unit": "ns/iter",
            "extra": "iterations: 1413\ncpu: 501312.0198159944 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1503731.5404814857,
            "unit": "ns/iter",
            "extra": "iterations: 457\ncpu: 1503457.8512035015 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 39.96903305555735,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 39.96712644444446 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 47.25631773333134,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 47.256207066666654 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 197.0127733333508,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 196.99774600000038 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.966980699999567,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.96602657999999 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 24.96836307143115,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.965321249999963 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 36.58370515789126,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.58303726315787 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.4052330841682825,
            "unit": "ms/iter",
            "extra": "iterations: 499\ncpu: 1.405140583166332 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 119.3490423333401,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 119.34350516666647 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.773201000001293,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.770693645161295 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.8958771071435,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.89534592857139 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 52.47614238462225,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.47284976923069 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 83.13553224999737,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 83.12896662499992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 278.2177845000433,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 278.20194600000116 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1388.5993850000204,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1388.5050139999996 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.598163520001435,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.596402799999993 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.05063863636236,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.0497548181817 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 54.01428853846168,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.010127461538374 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 99.6147351428525,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 99.6086575714286 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 327.56149800002277,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 327.5313410000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1553.566821000004,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1553.4451360000005 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 55.81918807691776,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 55.81536123076921 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 58.61660916666741,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.61166124999991 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 84.46474312499674,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 84.45924162499985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 145.79611340000156,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 145.77917860000014 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 415.8864185000084,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 415.8571349999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1913.6760300000333,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1913.5025440000036 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.00618099999593,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.00310681818178 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 69.30968870000243,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 69.3079505 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 96.08234228570771,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 96.0732355714293 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 171.69948700001214,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 171.68223250000025 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 452.6507500000321,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 452.61591950000127 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2117.89669999996,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2117.6598670000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 22.902605033334567,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 22.900653866666687 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 24.62315735714274,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.62087178571437 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 55.81721841666839,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 55.81425683333367 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 78.0032397777859,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 77.99775811111134 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 266.7745553333134,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 266.7622320000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 27.587771199996496,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.58480036000009 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 30.948023304344844,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 30.945682869565072 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 54.12655546154628,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.121234615384246 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 95.4040131428623,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 95.40058399999955 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 327.014872999996,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 326.9699035000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 22.80501403225982,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.80410648387098 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 25.077680464282334,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.07583939285724 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 58.13466691666728,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.131480833333534 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 97.21468814286384,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 97.20621957142787 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 211.416674333312,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 211.3991036666671 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 27.47146212000189,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.46960864000016 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 33.86614414285391,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.864233571428656 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 61.29702818181861,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 61.28810054545461 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 122.11385999999418,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 122.10538200000087 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 236.56210899999527,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 236.53052233333224 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.327682560975763,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.32593143902419 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.452169789473995,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.450345789473786 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 24.055070464287706,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.053999499999787 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 31.24032991304474,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 31.23798678260886 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 105.27742485713328,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 105.26992242857196 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.276390707318726,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.27458985365858 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.95524364864913,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.95439589189223 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 28.090848120000373,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 28.08815604000017 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 50.11582635714668,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 50.11091964285746 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 242.11190533333138,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 242.0809403333332 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.144050756098473,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.142423585366096 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.712496054054345,
            "unit": "ms/iter",
            "extra": "iterations: 37\ncpu: 18.705782027026885 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 28.960248166664353,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 28.958466666666755 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 52.967717400008496,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 52.9630056000002 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 253.84740166668962,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 253.81689766666682 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.465799174999574,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.46482652499992 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 23.665035166667774,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.65706526666666 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 37.207295789477556,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.20449800000013 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 55.83312088889316,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 55.8254473333335 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 151.65283899998485,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 151.64268319999792 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.283338000001436,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.281501000000077 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 23.67083136666679,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.669831100000255 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.452062444441204,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.44801627777785 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 74.41272162499502,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 74.41098812499902 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 160.08528700001534,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 160.07505050000148 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 22.63275254838684,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.63194051612886 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 24.18093251724023,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.178885344827584 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 29.449046249998407,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 29.44800383333342 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 52.22405925000354,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 52.22068533333394 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 126.35689950000521,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 126.34405299999922 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 22.65316635483824,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.650101483870987 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 24.3843427586218,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.382342379310224 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.33815740000341,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.33449639999964 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 76.1244110000108,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 76.11841844444535 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 275.7188276666511,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 275.6794073333329 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 23.557487733330618,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.5556840000001 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 25.86231411111452,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 25.860489222222537 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 36.61158578947116,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.6074490526312 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 78.33648087500933,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 78.32516699999914 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 291.4293190000308,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 291.40722449999856 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 23.563201966665776,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.55778096666654 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 32.84515476190379,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.842621761904496 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 45.1290796666702,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 45.126493133333646 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 98.01668499999649,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 98.00887671428679 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 225.64949599999787,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 225.63416233333555 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 24.467955310346323,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.465880068965593 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 33.39503519047563,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 33.39404271428594 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 47.29429385714314,
            "unit": "ms/iter",
            "extra": "iterations: 14\ncpu: 47.29127485714254 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 97.37904366664907,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 97.36716699999921 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 230.9158006666697,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 230.89562833333352 ms\nthreads: 1"
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
          "id": "fc101290a0ae69babc48e45f4bf2b2e0abf052b4",
          "message": "feat(engine): report each query's peak memory (#277)\n\n## Summary\n\nEvery query now reports its peak memory. `engine::QueryResult` gains\n`peak_memory`: the most bytes the session's\nmemory budget held from the start of the query until its result was\nbuilt, the result included. That is the count\n`--memory-limit` checks.\n\n`Session::Execute` restarts the budget's peak from the bytes held when\nit starts (`MemoryBudget::ResetPeak`), as\n`ExplainAnalyze` already did for profiled runs. Until now `Execute`\nnever reset it, so the only per-query peak came\nfrom `explain --analyze`.\n\n**What the number holds**, as the header says:\n- With one thread and no earlier result alive, it is the query's own\npeak, the same on every run.\n- A result the caller still holds is a floor under the next query's\npeak.\n- With more than one thread, so are buffers the previous query's workers\nhad not yet released.\n\nNo caller depends on the old meaning, the session's peak since it began.\nNothing in `src/`, `tests/`, `bench/` or the\nCLI read the budget's peak after `Execute`.\n\nThis is the first of three PRs of [ADR\n0037](docs/adr/0037-tpcds-roadmap.md)'s HC3, as planned. The second\nmakes the\nTPC-H and TPC-DS ratchets print each query's seconds and this peak in\nevery CI leg, for Decision 6's SF 1 budget. The\nthird adds a local report of each refusal.\n\n## Type of change\n\n- [x] feat: new SQL, CLI or engine capability\n- [ ] fix: bug fix\n- [ ] perf: performance improvement\n- [ ] refactor, test, docs, build, ci or chore\n- [ ] Breaking change (CLI, output format or semantics); also add the\n`breaking-change` label\n\n## Verification\n\n```text\n$ pixi run test -R '^engine\\.SessionTest\\.'\n100% tests passed out of 19\n\n$ pixi run ci\n100% tests passed out of 3247\n\n$ pixi run lint\nevery hook Passed except shellcheck: macOS-only SC2218 findings in tools/github/apply-settings.sh, untouched\n\n$ .pixi/envs/default/bin/clang-tidy -p build/tidy --warnings-as-errors='*' src/engine/session.cc \\\n    src/engine/tests/session_test.cc\nno findings\n```\n\n`engine.SessionTest.ExecuteReportsThePeakMemoryOfTheQuery` pins the\nsemantics:\n- the peak is positive and within the limit;\n- it is equal on a second run of the same query, and equal to\n`ExplainAnalyze`'s;\n- it is above the bytes the result still holds afterwards: the peak, not\nthe end;\n- it is smaller for a smaller query run afterwards;\n- with an earlier result still held, it is exactly that result's bytes\nplus the query's own peak.\n\nThe test fails with the reset removed, and fails when `Execute` reports\nthe bytes held at the end instead of the peak.\n\nNot run here: `pixi run asan` stops at gtest discovery on this Mac (a\nknown macOS-only `curl_global_init` leak), and\n`pixi run ci-gcc` is linux-64 only; CI runs both. `pixi run tidy` fails\non main on this Mac (a libc++-only finding in\n`profile.h`), so clang-tidy ran on the changed files.\n\n## Checklist\n\n- [x] `pixi run check` passes locally (lint + clang Debug -Werror +\nhermetic tests): as `pixi run lint` and\n`pixi run ci`, except shellcheck's macOS-only findings in a script this\nPR does not touch\n- [x] Tests cover the change (unit tests under `src/<module>/tests/`, or\nwhy none are needed)\n- [x] Docs updated where behavior, commands or architecture changed\n(AGENTS.md, `docs/`, an ADR), or not needed:\n`docs/architecture.md`'s memory paragraph and an ADR 0015 update note\n- [x] No ClickBench-derived data is committed: no Parquet files, query\nanswers or values from `hits` (ADR-0006)\n- [x] Nothing derived from TPC-H or TPC-DS is committed: no query text\nor fragments, data, answers or TPC tools\n      (ADR-0006)\n- [x] Changes to governance paths (see `.github/CODEOWNERS`) were agreed\nwith a maintainer: the ADR 0015 note, in\n      the approved HC3 plan\n\n## AI assistance\n\n- [ ] No AI assistance\n- [x] AI-assisted. Tools and what they did:\n- **Design.** Claude Code (Claude Opus 5.5) mapped the harness and the\nengine's memory budget with a read-only\nworkflow. Three critics (correctness, CI conventions, data policy)\nreviewed the HC3 design, which was approved in\n    plan mode, the public header included.\n- **Implementation and review.** It wrote this change and its test, and\nran the verification above. A review\nworkflow checked it: the `reviewer` subagent and a memory-semantics\nlens, with an adversarial check of each\nfinding. One finding was confirmed (P2) and is fixed: the test would\nalso have passed if `Execute` reported the\nbytes held at the end (the result's) instead of the peak, so it now also\nchecks the peak against the held bytes\n    and against `ExplainAnalyze`'s.\n- Accountable human (has read and understands the whole diff): @hor911\n\n🤖 Generated with [Claude Code](https://claude.com/claude-code)\n\nCo-authored-by: Claude Opus 5.5 <noreply@anthropic.com>",
          "timestamp": "2026-11-01T13:02:07+03:00",
          "tree_id": "d33e83cf4391da79376ff74c3027b7a363113ea0",
          "url": "https://github.com/ydb-campus/antb1/commit/fc101290a0ae69babc48e45f4bf2b2e0abf052b4"
        },
        "date": 1793528112422,
        "tool": "googlecpp",
        "benches": [
          {
            "name": "BM_ParseSmallAggQuery",
            "value": 5217.392191064884,
            "unit": "ns/iter",
            "extra": "iterations: 135870\ncpu: 5216.881062780599 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_Exact",
            "value": 30916.539190866886,
            "unit": "ns/iter",
            "extra": "iterations: 21727\ncpu: 30913.195148893086 ns\nthreads: 1"
          },
          {
            "name": "BM_SumInt16_ArrowKernel",
            "value": 52800.002790765786,
            "unit": "ns/iter",
            "extra": "iterations: 13258\ncpu: 52796.89613818071 ns\nthreads: 1"
          },
          {
            "name": "BM_NotEqualTrueCount",
            "value": 222011.35898271712,
            "unit": "ns/iter",
            "extra": "iterations: 3067\ncpu: 221913.06651450932 ns\nthreads: 1"
          },
          {
            "name": "BM_Int128AvgAccumulate",
            "value": 522400.3229999994,
            "unit": "ns/iter",
            "extra": "iterations: 1000\ncpu: 522369.3709999999 ns\nthreads: 1"
          },
          {
            "name": "BM_ScanColumn",
            "value": 1511234.7013275083,
            "unit": "ns/iter",
            "extra": "iterations: 452\ncpu: 1511190.5619469034 ns\nthreads: 1"
          },
          {
            "name": "BM_StringFilterAfterScan",
            "value": 40.12040729411544,
            "unit": "ms/iter",
            "extra": "iterations: 17\ncpu: 40.11623505882354 ms\nthreads: 1"
          },
          {
            "name": "BM_StringFilterInScan",
            "value": 46.6757173333311,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.670871866666694 ms\nthreads: 1"
          },
          {
            "name": "BM_SortRows",
            "value": 201.61514399999683,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 201.60280266666675 ms\nthreads: 1"
          },
          {
            "name": "BM_TopNRows",
            "value": 13.918606880000652,
            "unit": "ms/iter",
            "extra": "iterations: 50\ncpu: 13.91790278000002 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/0",
            "value": 24.651383413791745,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.64835972413796 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableBuild/1",
            "value": 36.62067236841961,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 36.61815057894735 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/0",
            "value": 1.452801436590541,
            "unit": "ms/iter",
            "extra": "iterations: 481\ncpu: 1.4526522598752603 ms\nthreads: 1"
          },
          {
            "name": "BM_JoinTableProbe/1",
            "value": 123.92404883333559,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 123.91425649999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1000",
            "value": 22.75688441935406,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.7542612580645 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16384",
            "value": 24.652892964285552,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 24.650991499999975 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/262144",
            "value": 52.94660530769044,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 52.939714076923146 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/1048576",
            "value": 80.94148544444755,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 80.93557122222232 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/4194304",
            "value": 267.5394423333349,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 267.5169120000002 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64/16777216",
            "value": 1321.2522130000366,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1321.18221 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1000",
            "value": 27.568731400001525,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.566806479999997 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16384",
            "value": 31.340407818181472,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.334283272727408 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/262144",
            "value": 54.482143384614815,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 54.47404869230774 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/1048576",
            "value": 103.01279528571935,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 103.004850142857 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/4194304",
            "value": 334.06280350001794,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 334.0196859999995 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32/16777216",
            "value": 1561.8053970000005,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1561.608992 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1000",
            "value": 56.04324391666656,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 56.03825775000004 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16384",
            "value": 59.314661500001385,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 59.30778524999999 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/262144",
            "value": 85.35714387500093,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 85.34223362500003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/1048576",
            "value": 147.41032150000422,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 147.38912049999976 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/4194304",
            "value": 436.53476549999937,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 436.46624100000156 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupString/16777216",
            "value": 1948.4188760000052,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 1948.1562940000003 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1000",
            "value": 65.12308027272825,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 65.11446509090939 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16384",
            "value": 68.83754940000131,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 68.82531919999977 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/262144",
            "value": 99.69843585714345,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 99.68921799999973 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/1048576",
            "value": 180.53971224999543,
            "unit": "ms/iter",
            "extra": "iterations: 4\ncpu: 180.5142945 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/4194304",
            "value": 444.24244299997895,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 444.1744579999991 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64String/16777216",
            "value": 2091.4020759999516,
            "unit": "ms/iter",
            "extra": "iterations: 1\ncpu: 2091.233199000001 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1000",
            "value": 22.97319554838667,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.96900354838707 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/16384",
            "value": 25.276874821429374,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.274809607142807 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/262144",
            "value": 58.96660025000244,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.95853833333339 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/1048576",
            "value": 86.28320571428308,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 86.2785368571427 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Budget/4194304",
            "value": 269.33451799999375,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 269.30644899999834 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1000",
            "value": 27.564974519998486,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.563672999999934 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/16384",
            "value": 31.647427863635937,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 31.644462545454385 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/262144",
            "value": 54.571183416669555,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 54.56743383333359 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/1048576",
            "value": 100.0142271428542,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 100.0022072857141 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32Budget/4194304",
            "value": 336.9221080000102,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 336.8777344999998 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1000",
            "value": 22.92762332258059,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.924796903225797 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/16384",
            "value": 25.278027535713527,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 25.275912714285706 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/262144",
            "value": 58.68049083333441,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 58.67547724999985 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/1048576",
            "value": 106.15451685714561,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 106.1392684285717 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64BudgetRandom/4194304",
            "value": 217.64343033332048,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 217.62538999999967 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1000",
            "value": 27.468172840001444,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.46561179999986 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/16384",
            "value": 34.471796199997584,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.4668145 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/262144",
            "value": 62.2101944545441,
            "unit": "ms/iter",
            "extra": "iterations: 11\ncpu: 62.197533090908834 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/1048576",
            "value": 128.6870668000006,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 128.67226580000022 ms\nthreads: 1"
          },
          {
            "name": "BM_GroupInt64Int32BudgetRandom/4194304",
            "value": 242.87857233332488,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 242.8509863333351 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1000",
            "value": 17.282644219512914,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.28137580487806 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/16384",
            "value": 18.49832215789529,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.495627526315904 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/262144",
            "value": 24.332396172412544,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.330102172413604 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/1048576",
            "value": 32.26015386363718,
            "unit": "ms/iter",
            "extra": "iterations: 22\ncpu: 32.25708940909106 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Presized/4194304",
            "value": 104.56440628571174,
            "unit": "ms/iter",
            "extra": "iterations: 7\ncpu: 104.55625099999922 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1000",
            "value": 17.564749384615215,
            "unit": "ms/iter",
            "extra": "iterations: 39\ncpu: 17.562664153846168 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/16384",
            "value": 18.61350439473672,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.611928026315937 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/262144",
            "value": 27.756210959998953,
            "unit": "ms/iter",
            "extra": "iterations: 25\ncpu: 27.752676559999827 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/1048576",
            "value": 51.614740846153275,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 51.60824215384707 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate/4194304",
            "value": 256.0110646666658,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 255.96864366666713 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1000",
            "value": 17.16381012195226,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.162072414634036 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/16384",
            "value": 18.559488710526228,
            "unit": "ms/iter",
            "extra": "iterations: 38\ncpu: 18.55616757894745 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/262144",
            "value": 28.79378000000088,
            "unit": "ms/iter",
            "extra": "iterations: 24\ncpu: 28.79054083333325 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/1048576",
            "value": 55.64060349999522,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 55.6313270000004 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate/4194304",
            "value": 255.57774433334166,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 255.54133433333467 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1000",
            "value": 17.447987424999667,
            "unit": "ms/iter",
            "extra": "iterations: 40\ncpu: 17.44514379999984 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/16384",
            "value": 21.14687472727162,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.144857090909206 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/262144",
            "value": 37.96174399999891,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.95656499999953 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/1048576",
            "value": 63.55372777777448,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 63.545835000000906 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_Indirect_Estimate_Random/4194304",
            "value": 156.96169600000758,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 156.93619559999945 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1000",
            "value": 17.27768070731749,
            "unit": "ms/iter",
            "extra": "iterations: 41\ncpu: 17.27603634146339 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/16384",
            "value": 21.050662363636135,
            "unit": "ms/iter",
            "extra": "iterations: 33\ncpu: 21.04733524242401 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/262144",
            "value": 38.795395777779305,
            "unit": "ms/iter",
            "extra": "iterations: 18\ncpu: 38.79093300000057 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 64.66851539999539,
            "unit": "ms/iter",
            "extra": "iterations: 10\ncpu: 64.65739589999941 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 162.16903740000816,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 162.1508786000021 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1000",
            "value": 22.560225451612833,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.556856645161087 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/16384",
            "value": 23.956485071427178,
            "unit": "ms/iter",
            "extra": "iterations: 28\ncpu: 23.953880678571387 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/262144",
            "value": 29.874518956521865,
            "unit": "ms/iter",
            "extra": "iterations: 23\ncpu: 29.86890630434773 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/1048576",
            "value": 52.10248633333473,
            "unit": "ms/iter",
            "extra": "iterations: 12\ncpu: 52.09641225000065 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Presized/4194304",
            "value": 128.46004860000448,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 128.43495779999898 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1000",
            "value": 22.59819622580681,
            "unit": "ms/iter",
            "extra": "iterations: 31\ncpu: 22.59509474193525 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/16384",
            "value": 24.655004206896717,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.6511863103448 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/262144",
            "value": 34.443419549998566,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.43996434999974 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/1048576",
            "value": 77.19432811110967,
            "unit": "ms/iter",
            "extra": "iterations: 9\ncpu: 77.18185477777695 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate/4194304",
            "value": 285.24909550000643,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 285.20549699999975 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1000",
            "value": 23.517472499999787,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.513490000000086 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/16384",
            "value": 25.906010740742303,
            "unit": "ms/iter",
            "extra": "iterations: 27\ncpu: 25.90332162962949 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/262144",
            "value": 37.295772421053265,
            "unit": "ms/iter",
            "extra": "iterations: 19\ncpu: 37.28980478947326 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/1048576",
            "value": 83.09282162500153,
            "unit": "ms/iter",
            "extra": "iterations: 8\ncpu: 83.08271937499967 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate/4194304",
            "value": 293.1094359999804,
            "unit": "ms/iter",
            "extra": "iterations: 2\ncpu: 293.0668804999996 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1000",
            "value": 23.525761766668058,
            "unit": "ms/iter",
            "extra": "iterations: 30\ncpu: 23.51919826666678 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/16384",
            "value": 32.53020214285535,
            "unit": "ms/iter",
            "extra": "iterations: 21\ncpu: 32.525943285714035 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/262144",
            "value": 46.05813800000078,
            "unit": "ms/iter",
            "extra": "iterations: 15\ncpu: 46.054223599999965 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/1048576",
            "value": 111.02983649999487,
            "unit": "ms/iter",
            "extra": "iterations: 6\ncpu: 111.01669433333446 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_Indirect_Estimate_Random/4194304",
            "value": 230.2331153333436,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 230.21730466666668 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1000",
            "value": 24.496690586208214,
            "unit": "ms/iter",
            "extra": "iterations: 29\ncpu: 24.494023344827504 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/16384",
            "value": 34.685392700001216,
            "unit": "ms/iter",
            "extra": "iterations: 20\ncpu: 34.68197170000025 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/262144",
            "value": 48.602851923069814,
            "unit": "ms/iter",
            "extra": "iterations: 13\ncpu: 48.59627246153873 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/1048576",
            "value": 114.09707239999989,
            "unit": "ms/iter",
            "extra": "iterations: 5\ncpu: 114.09101540000108 ms\nthreads: 1"
          },
          {
            "name": "BM_S0Int64Int32_IndirectSlotOrder_Estimate_Random/4194304",
            "value": 237.01043299998523,
            "unit": "ms/iter",
            "extra": "iterations: 3\ncpu: 236.98516299999994 ms\nthreads: 1"
          }
        ]
      }
    ]
  }
}