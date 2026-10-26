# Security policy

## Supported versions

| Version | Supported |
| --- | --- |
| The latest [release](https://github.com/ydb-campus/antb1/releases/latest) | yes |
| The `main` branch | yes |
| Older releases and release candidates | no: use the latest release |

antb1 is experimental, and its releases are 0.x versions. Fixes land on `main` and ship with the next release;
nothing is backported. A published release keeps its assets and its tag for good (GitHub's immutable releases), so
a fix always comes as a new version.

The release binaries link their libraries statically: Apache Arrow and the libraries it bundles, CLI11, the C++
standard library and, on Linux, the GNU C Library 2.28 (each package's `THIRD_PARTY_NOTICES.txt` lists them). A fix
in one of them reaches the binaries only with a new antb1 release, and no vulnerability alert covers these pins: tell
the maintainers privately when such a fix matters.

## Reporting a vulnerability

Please report vulnerabilities privately through GitHub's private vulnerability reporting:
<https://github.com/ydb-campus/antb1/security/advisories/new> (repository "Security" tab, then
"Report a vulnerability").

Do not open public issues, pull requests or discussions for vulnerabilities, and do not include exploit details in
public commit messages before a fix is available.

A useful report contains:

- what you ran: for a release binary, the release tag, the asset name and the output of `antb1 version --verbose`;
  for a checkout, the commit (`git rev-parse HEAD`) and the output of `pixi run doctor --json`;
- the exact command, SQL text and a minimal input file that trigger the problem (synthetic data only, never
  ClickBench-derived data);
- what happens (crash, sanitizer report, wrong access, leaked secret) and what you expected.

The maintainers handle reports on a best-effort basis: they confirm receipt, discuss the fix in the private advisory
and credit reporters who want to be credited.

## Verifying a release

Each check answers its own question. The examples check `antb1-linux-x86_64.tar.gz` of `v0.1.0`: give your file and
its release's tag.

1. **The download is intact.** `SHA256SUMS` in the same release lists its seven tarballs:

   ```sh
   sha256sum --ignore-missing -c SHA256SUMS   # macOS: shasum -a 256 --ignore-missing -c SHA256SUMS
   ```

   `SHA256SUMS` comes from the same place as the file, so it shows that the download is complete and unchanged, not
   who made it.

2. **`release.yml` built it from the release's tag, on a GitHub-hosted runner.** This is the check that matters.
   The release workflow signs SLSA build provenance for the seven tarballs and `SHA256SUMS`, keyless through
   Sigstore's public-good instance. It needs [gh](https://cli.github.com), logged in:

   ```sh
   gh attestation verify antb1-linux-x86_64.tar.gz --repo ydb-campus/antb1 \
     --signer-workflow ydb-campus/antb1/.github/workflows/release.yml \
     --source-ref refs/tags/v0.1.0 --deny-self-hosted-runners
   ```

   - **Always give `--source-ref`.** `--signer-workflow` names the workflow file, not the ref it ran from, so on its
     own it also accepts what a copy of `release.yml` on any branch signed. gh 2.101 and older also match it as a
     prefix, which lets through a workflow file whose name merely starts with `release.yml`. With
     `--source-ref refs/tags/<tag>` only a run that the release's tag started passes: only admins can create a tag,
     and nobody can move or delete a `v*` tag.
   - `--deny-self-hosted-runners` refuses provenance made on a self-hosted runner.
   - `SHA256SUMS` is attested as well: once `SHA256SUMS` itself passes this check, step 1 covers every tarball it
     lists.
   - **Offline**, give the release's `antb1.sigstore.json` (its provenance bundle) and a trusted root that
     `gh attestation trusted-root` wrote while online. No login is needed then:

     ```sh
     gh attestation trusted-root >trusted_root.jsonl   # once, online
     gh attestation verify antb1-linux-x86_64.tar.gz --repo ydb-campus/antb1 \
       --signer-workflow ydb-campus/antb1/.github/workflows/release.yml \
       --source-ref refs/tags/v0.1.0 --deny-self-hosted-runners \
       --bundle antb1.sigstore.json --custom-trusted-root trusted_root.jsonl
     ```

3. **It is an asset of that published release.** When a release is published, GitHub signs a release attestation of
   its tag and every asset, and the release is immutable from then on. With gh logged in:

   ```sh
   gh release verify v0.1.0 --repo ydb-campus/antb1
   gh release verify-asset v0.1.0 antb1-linux-x86_64.tar.gz --repo ydb-campus/antb1
   ```

The `static` workflow's artifacts of `main` ([docs/benchmarks.md](docs/benchmarks.md)) are not releases: their
`SHA256SUMS` comes from the same run, they carry no provenance, and they expire after 30 days.

## Scope

In scope:

- memory-safety or undefined-behavior bugs reachable from SQL text or Parquet input passed to the `antb1` CLI, in a
  release binary or a build of `main`;
- how a release is built and published: `.github/workflows/release.yml`, `.github/scripts/release.py`, the tag
  rulesets (`tools/github/ruleset-tags.json`, `tools/github/ruleset-release-tags.json`) and the `release` environment
  (`tools/github/environment-release.json`). For example a way to publish an asset that `release.yml` did not build
  from the release's tag, to change a published release, or to move or delete a `v*` tag;
- CI and supply-chain weaknesses in `.github/workflows/` (for example secrets reachable from fork pull requests or
  unpinned actions).

Known and accepted: a maintainer with admin rights can release alone, by pushing the tag and approving the
`release` environment ([docs/ci.md](docs/ci.md#releases)).

Out of scope: vulnerabilities in third-party packages (Apache Arrow, DuckDB, CLI11 and others; report those upstream,
and tell the maintainers privately as well when the release binaries link the affected code), and denial of service
through queries or files that are simply very large.
