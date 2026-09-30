# 17. The allocator keeps the memory a query frees

Date: 2026-09-30

## Status

Proposed

## Context

- **The allocator:** Arrow's default memory pool is mimalloc, bundled into conda-forge's `libarrow`. Every Arrow
  buffer of a query comes from it: Parquet pages, decoded columns, hash table rows, aggregate states.
- **Freed memory goes back to the system:** mimalloc returns ("purges") freed memory to the OS after a delay, 1 s in
  this build, and the next allocation faults it back in. With 128 threads, those page faults contend in the kernel.
- **The measurement** (full ClickBench data, 128 threads, the same binary with `MIMALLOC_PURGE_DELAY=-1` against the
  default, three runs):
  - The kernel CPU of a run of the 43 queries halves (about 480 s to 220–245 s).
  - The total falls 5–5.5% (26.0 s to 24.6 s). Q36 is 22% faster, Q32 and Q39 about 14%. No query is more than 5%
    slower.
- **Other allocators are worse** under the same load:
  - Arrow's jemalloc and system (glibc) pools spend 20–100 s of kernel CPU on single queries (Q28, Q32, Q34) and run
    up to 2.7× slower.
  - gperftools tcmalloc (8 KiB pages; 256 KiB pages with a 4 GiB thread cache) wins some high-cardinality GROUP BYs
    but loses 15–18% on scan-heavy queries. Its totals are 2–9% above mimalloc without purging.
- **A process cannot set it for itself:** mimalloc reads its options when `libarrow` is loaded, before `main`. Arrow
  does not export mimalloc's `mi_option_set`. An executable's `.preinit_array` runs early enough, but glibc resets the
  environment after it.

## Decision

- **On Linux, the `antb1` executable restarts itself once with `MIMALLOC_PURGE_DELAY=-1`** (`cli::RestartForAllocator`,
  first thing in `main`): `execv("/proc/self/exe", argv)`, the same arguments and open files (stdin included).
- **When it restarts:** only when the process was started as `antb1` itself (`/proc/self/exe` and `argv[0]` have the
  same file name; through the dynamic loader or an emulator they do not), when `MIMALLOC_PURGE_DELAY` is unset (a value the user set wins; the restarted process has
  it, so it never restarts twice) and Arrow's default pool is mimalloc (`ARROW_DEFAULT_MEMORY_POOL` unset or
  `mimalloc`).
- **Setting it first:** the variable is set before the exec, so a failed exec never loops. The process goes on with
  mimalloc's defaults.
- **Where it does not apply:** the library modules, the test binaries and other platforms are unchanged.

## Consequences

- **Faster queries:** 5% on the 43 queries, most on high-cardinality GROUP BYs and short GROUP BYs.
- **Start-up:** the CLI starts one more time, about 26 ms. `antb1 bench` times queries inside the process, so its
  numbers do not include it.
- **Resident memory:** the process keeps its peak memory until it exits, rather than returning it between queries.
  `--memory-limit` is unchanged: the budget counts Arrow allocations, not resident pages. Set
  `MIMALLOC_PURGE_DELAY` (e.g. to `1000`) to get mimalloc's behavior back.
- **Tests:** the decisions are pure functions with unit tests. The CLI golden tests run through the restart (except
  under the sanitizer presets, which use the system pool): they pass arguments and stdin.
- **Later:** if Arrow exposes mimalloc's options, or antb1 gets its own pool, an in-process setting replaces the
  restart.
