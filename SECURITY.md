# Security policy

## Supported versions

antb1 is an experimental project without releases. Only the latest commit on the `main` branch is supported;
fixes land on `main` and are not backported.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub's private vulnerability reporting:
<https://github.com/ydb-campus/antb1/security/advisories/new> (repository "Security" tab, then
"Report a vulnerability").

Do not open public issues, pull requests or discussions for vulnerabilities, and do not include exploit details in
public commit messages before a fix is available.

A useful report contains:

- the affected commit (`git rev-parse HEAD`) and the output of `pixi run doctor --json`;
- the exact command, SQL text and a minimal input file that trigger the problem (synthetic data only, never
  ClickBench-derived data);
- what happens (crash, sanitizer report, wrong access, leaked secret) and what you expected.

The maintainers handle reports on a best-effort basis: they confirm receipt, discuss the fix in the private advisory
and credit reporters who want to be credited.

## Scope

In scope:

- memory-safety or undefined-behavior bugs reachable from SQL text or Parquet input passed to the `antb1` CLI;
- CI and supply-chain weaknesses in `.github/workflows/` (for example secrets reachable from fork pull requests or
  unpinned actions).

Out of scope: vulnerabilities in third-party packages (Apache Arrow, DuckDB, CLI11 and others; report those
upstream), and denial of service through queries or files that are simply very large.
