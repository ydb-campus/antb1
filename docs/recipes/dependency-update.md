# Recipe: update a dependency

The whole toolchain and every library come from conda-forge through pixi: `pixi.toml` declares them, `pixi.lock`
pins every package for every platform ([ADR 0002](../adr/0002-toolchain-pixi-conda-forge.md)). A dependency change
therefore changes what every developer, agent and CI job builds with. The skill
`.agents/skills/dependency-update/SKILL.md` is the short version.

## 1. Ask a human first

`pixi.toml`, `pixi.lock`, `cmake/`, `CMakePresets.json`, `.github/` and `tools/github/` are "Ask a human first"
paths, and in Claude Code `pixi add`, `pixi remove`, `pixi update`, `pixi upgrade` and `pixi lock` always prompt.
Before changing anything, hand off a proposal to a maintainer:

- the package and version constraint, and the feature and environments it goes into;
- why it is needed, and what you considered instead (often Arrow or the standard library already has it);
- for a library: which module links it (this also needs `cmake/Antb1Modules.cmake` and an ADR);
- for an upgrade: the changelog items that matter here (ABI, deprecations, behavior changes).

Continue only with explicit approval, and only with the approved change.

## 2. Where things go in `pixi.toml`

- conda's Arrow (`libarrow`, `libarrow-compute`, `libparquet`) of the `default` and `gcc` environments: the default
  feature (`[dependencies]`). Env `static` builds its own Arrow from `cmake/static/arrow-sources.lock` instead.
- Build and test tools and libraries of `default`, `gcc` and `static`: feature `base`. The Clang toolchain of
  `default` and `static`: feature `llvm` (`default` and `static` share a solve group, so they get the same versions).
- Other compiler-specific tools: the environment's own feature (`clang` for `default`, `gcc` for `gcc`, `static`).
- Formatters and linters: the `lint` feature (no C++ dependencies there).
- DuckDB is a test oracle only (never linked into `src/`); new test-only packages follow the same rule.
- Pin the minor version, for example `"25.0.*"`, and keep lockstep groups together:
  - `libarrow`, `libarrow-compute` and `libparquet` (always the CPU builds), and with them the static build's pins:
    an Arrow upgrade replaces every line of `cmake/static/arrow-sources.lock`, then `pixi run static` must pass.
    - The pins come only from the ASF release. Check `apache-arrow-<v>.tar.gz` against its `.sha512` and run
      `gpg --verify` on its `.asc` with <https://downloads.apache.org/arrow/KEYS>. Every other pin comes from that
      tarball's `cpp/thirdparty/versions.txt`. Never take a pin from a mirror: `--check-urls` checks each URL
      against the pin, not the pin itself.
    - Then list each file's byte-identical copies, at least two operators each, in the lock header's order. Download
      each one and compare it with the pin; a file name proves nothing. `pixi run static-arrow --check-urls` must pass;
  - `clang`, `clangxx`, `clang-tools`, `llvm-tools`, `lld` and `compiler-rt`;
  - `libstdcxx-devel_*` and `libgcc-devel_*` (never `-stdlib=libc++` on Linux: conda Arrow uses libstdc++).
- Tasks: a new task lives in exactly one environment and is documented in the AGENTS.md command table (lint checks
  both).
- Licences ([ADR 0034](../adr/0034-licence-and-static-sources.md)): a library linked into antb1 needs a licence
  compatible with Apache-2.0. Its licence goes into the package's notices (`scripts/static_package.py`) and its
  source into the sources of the static binary: a pin in `cmake/static/arrow-sources.lock`, or, for a header-only
  conda package, `COMPILED_PACKAGES` in `scripts/static_package.py`.

## 3. Lock and install

```bash
pixi lock                          # after editing pixi.toml by hand (prompts in Claude Code)
pixi install --locked              # the default environment
pixi install --locked -e gcc       # every other environment you changed (gcc is linux-64 only)
pixi install --locked -e lint
```

- Lock only with pixi 0.81.0; the lock file must keep `version: 7` on its first line. Never edit `pixi.lock` by
  hand, and never let `pixi run` rewrite it silently (CI runs with `--frozen`).
- Review the lock diff: only the packages you meant to change (and their required dependencies) should move.
- A new sysroot build (`sysroot_linux-64` or `sysroot_linux-aarch64`, for example in the weekly lock refresh) fails
  `tools/ci/tests/test_static_sources.py` until `cmake/static/glibc-source.lock` maps it to the glibc source RPM it
  was built from: read the AlmaLinux version in the package's `info/recipe/meta.yaml`, take that version's source
  RPM from the AlmaLinux vault, check its sha256 against the vault's `repodata`, and add the line (ADR 0034).

## 4. Verify

- `pixi run check-full` on Linux: lint, Clang Debug with tests, ASan/UBSan, clang-tidy and the GCC leg. New compiler
  versions often bring new warnings; fix them in the code, never by silencing them.
- The macOS leg (`pixi run release`) runs in CI, or locally on a Mac.
- `pixi run doctor` shows the resolved compiler, CMake and Arrow versions.
- Update the docs that name versions: AGENTS.md, CONTRIBUTING.md, docs/architecture.md, and an ADR for a major
  upgrade (for example the next Arrow major).

## 5. Open the PR

- Title: `build(deps): <lowercase subject>`, for example `build(deps): update arrow to 26.0`.
- Body: the approved proposal, a summary of the lock diff and the verification commands with their results.

## `pixi.lock` conflicts

Never resolve a `pixi.lock` conflict by hand.

1. Rebase your branch on `origin/main` and resolve the conflict in `pixi.toml` only.
2. Regenerate the lock with `pixi lock` (it prompts), then `pixi install --locked` and the checks above.
3. If the branch did not change `pixi.toml` at all, take the lock from `main` instead of regenerating it.

A scheduled bot PR refreshes `pixi.lock` within the existing constraints every Monday (`pixi.lock update`,
[ci.md](../ci.md#workflows)); it goes through CI and a human review like any other PR.

## GitHub Actions

Actions are pinned to a full commit SHA with a `# vX.Y.Z` comment, and only actions allowed by
`tools/github/allowed-actions.json` may run ([ci.md](../ci.md#workflow-security-rules)). A new or bumped action is a
`.github/` change: propose the action, the version, the SHA and why, and let a maintainer apply it.
