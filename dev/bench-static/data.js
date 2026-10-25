window.BENCHMARK_DATA = {
  "lastUpdate": 1792932072417,
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
      }
    ]
  }
}