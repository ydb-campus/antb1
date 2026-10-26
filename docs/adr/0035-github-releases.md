# 35. Tagged GitHub releases of the standalone binary

Date: 2026-10-26

## Status

Proposed

## Context

- **The binary exists, releases do not.** [ADR 0033](0033-standalone-static-binary.md) builds one standalone antb1 per
  target, and [ADR 0034](0034-licence-and-static-sources.md) licenses it and offers each Linux package with its sources
  from the same place. The only copies are `static.yml`'s artifacts: kept 30 days, built against a cached Arrow prefix,
  with checksums but no provenance.
- **Everyone with write access can publish:** the two admins, six writers, the GitHub Apps and every job with
  `contents: write` can push a tag and create, edit or delete a release; GitHub has no narrower permission. Only `main`
  has a ruleset, no tag is protected, releases are mutable, and a local agent runs with its maintainer's login.
- **A release is permanent.** Users fetch `releases/latest/download/<asset>`. An immutable release's assets and tag
  never change, its tag name is never reused, and its provenance stays in the public Sigstore log.
- **GitHub's rules:** a job that names a missing environment creates it unprotected; immutability covers only releases
  published after it is on; a job waiting for approval fails after 30 days; a push of more than three tags starts no
  workflow; the Releases API refuses `GITHUB_TOKEN` a release whose target commit changes `.github/workflows/` relative
  to the default branch; gh 2.101 and older match `--signer-workflow` as a prefix.

## Decision

- **A tag starts a release.** An admin pushes an annotated tag `v<VERSION>` (the project VERSION in `CMakeLists.txt`,
  which `pixi.toml` repeats) on its own, on a commit of `main` whose `CI OK` is green; the annotation opens the notes.
  Signed tags, which only preflight can require, wait for both maintainers' keys. `.github/workflows/release.yml` runs
  one release at a time, never cancelled; its logic is `.github/scripts/release.py` (under `/.github/`, a governance
  path) and `scripts/static_gates.py`, standard library only, run by the runner's `python3 -I` from the tagged commit. A
  manual run is a dry run: nothing is attested, drafted or published.
- **Release candidates:** `v<VERSION>-rc.<N>` is a prerelease, never the latest, whose binary reports the final version;
  its BUILDINFO.json names the tag (`scripts/static.sh` records it on tag runs, and `assemble` checks it). `v0.1.0-rc.1`
  rehearses the whole path in this repository after the settings, this
  change, a passing dry run and ADR 0034's LGPL review; `v0.1.0` follows ([docs/ci.md](../ci.md#releases)).
- **`preflight` refuses a tag push** (a dry run only warns): from a fork; for a tag that is not `v<VERSION>` or
  `v<VERSION>-rc.<N>`, is lightweight or does not peel to the run's commit; for a commit not on `main`; when the newest
  `ci.yml` run on it is missing or running, or its `CI OK` job is not green (read from the Actions API, never from check
  runs, which any workflow can create); when a published release has the tag; and when the
  `release` environment or the rulesets differ from `tools/github/`. It fails a dry run too when `CMakeLists.txt` and
  `pixi.toml` disagree. It only warns when `main` changed `.github/workflows/` since: the Releases API's rule should
  not apply to an existing tag, but the maintainers tag the head of `main` and merge no workflow change before
  publication.
- **Built and checked from scratch:** `build <target>` runs `pixi run static && pixi run static-sources` on
  `static.yml`'s runners like the nightly `static-cold`: no cache, ccache off, an empty Arrow directory. `assemble`
  checks each file against its checksums, BUILDINFO.json against the tag and each sources tarball against the pins and
  the commit, then writes the one `SHA256SUMS` and the notes. `verify` checks each package as
  `pixi run static-check antb1-<target>.tar.gz` does, without pixi and with an empty environment, on fresh ubuntu-24.04
  and ubuntu-24.04-arm runners (also in a FROM-scratch container without network), macos-15 and macos-26.
- **Provenance:** after every check, `attest` signs one SLSA build provenance attestation (`actions/attest`, keyless,
  public-good Sigstore) of the seven tarballs and `SHA256SUMS`; its bundle is the asset `antb1.sigstore.json`. Only it
  has `id-token: write`. It runs no repository code and takes the digests from `assemble`'s job output, which, unlike an
  artifact, no later job can replace.
- **Draft, approval, publication:** `draft` uploads every asset to a draft release for the approver to read. `publish`
  waits for the environment `release`, then trusts no artifact: it downloads each asset of the draft, checks it against
  the API digest, the job output and the draft's bundle, publishes it without making it the latest release, and
  requires an immutable release that `gh release verify` accepts. Only then does a stable version above every
  published one become the latest release, which README.md's install block downloads.
  `check-published` then runs README.md's install block and the documented checks on each target's runner.
- **Nine assets, about 217 MB,** without the version in their names, so the latest release's URLs never change:
  `antb1-<target>.tar.gz` per target, `antb1-<target>.debug.tar.gz` and `antb1-<target>.sources.tar` per Linux target,
  `SHA256SUMS` of the seven tarballs, and the bundle, which is neither a subject nor in `SHA256SUMS`.
- **Release notes:** the annotation without a signature, an rc note for a prerelease, then fixed sections: assets and
  CPU floors, install and verify commands, licences and sources. No PR list, no CHANGELOG, no measured figures.
- **Users verify** the digest (`sha256sum --ignore-missing -c SHA256SUMS`), the provenance (offline: add
  `--bundle antb1.sigstore.json` and `--custom-trusted-root`; `--source-ref` always, as gh 2.101 and older match
  `--signer-workflow` as a prefix) and GitHub's release attestation:

  ```sh
  gh attestation verify antb1-<target>.tar.gz --repo ydb-campus/antb1 --signer-workflow ydb-campus/antb1/.github/workflows/release.yml --source-ref refs/tags/<tag> --deny-self-hosted-runners
  gh release verify <tag> --repo ydb-campus/antb1
  gh release verify-asset <tag> antb1-<target>.tar.gz --repo ydb-campus/antb1
  ```

- **Settings as code** in `tools/github/` ([ADR 0009](0009-ci-and-governance.md)), applied by an admin before this
  change merges, so that no tag meets a missing environment or a mutable release.
  `bash tools/github/apply-settings.sh --release` is the dry run; `--apply --release` applies them, and its read-back,
  which must be clean, goes into the PR. Bypass is per ruleset, hence two tag rulesets:
  - `tags` (`tools/github/ruleset-tags.json`): only admins create, update or delete a tag, so nobody else can start a
    release or put one on a new tag;
  - `release-tags` (`tools/github/ruleset-release-tags.json`, no bypass): nobody moves or deletes a `v*` tag, and a new
    one needs `CI OK` on its commit;
  - the environment `release` (`tools/github/environment-release.json`): its reviewers are both maintainers, and the
    approval of either is enough, the one who pushed the tag included (`prevent_self_review: false`); admins cannot
    bypass it, and only `v*` tags deploy to it;
  - immutable releases: GitHub locks the assets and the tag at publication and signs a release attestation.
- **Agents never release** ([ADR 0007](0007-ai-agents.md)): no agent pushes a tag, starts `release.yml`, or creates,
  approves, edits or publishes a release (AGENTS.md golden rule 4). The `tags` ruleset stops the Apps; for local agents
  `.claude/settings.json` denies tag pushes, `gh release` writes, `gh workflow run release.yml` and deployment
  approvals.

## Consequences

- **One maintainer can release alone:** an admin who pushes a tag may approve it, and through `main`'s pull-request
  bypass can change `release.yml` or `.github/scripts/release.py` first; this change itself may merge that way. What
  remains is the audit log, the tag's rule suite, immutability and provenance that names the workflow and commit of
  every file. Two-person approval can return with a settings PR (`prevent_self_review: true`).
- **What the settings cannot stop:** a writer, an App or a job with `contents: write` can edit a waiting draft, or
  publish a release of its own on a pushed tag. `publish` re-checks every asset right before publishing and refuses a
  tag with a published release, and the documented checks reject files that `release.yml` did not build from the tag.
- **A mistake costs a version:** a `v*` tag is never moved or deleted, an immutable release's tag is never reused, and a
  pushed tag starts a build and a permanent attestation even if nobody approves it. Fix forward with the next version.
  Before publication re-run only the failed jobs: re-running all jobs packs other bytes and attests them again.
- **Each release costs** about 217 MB of assets, a cold build of three targets that downloads every pinned source, and
  checks on four fresh runners. Artifacts are kept 30 days for a tag, 7 for a dry run.
- **The LGPL review comes first:** ADR 0034 asks an open-source office or a lawyer to review the LGPL route before the
  first public release. An rc is public and immutable too, so the review precedes `v0.1.0-rc.1`; no code enforces it.
- **Left for later:** the glibc source RPM as an asset; notes generated from the previous tag; verify legs on
  ubuntu-26.04 (after an actionlint update); reproducible tarballs; signed tags; notarization; verify in the nightly
  `static-cold`; glibc's `--check-urls` nightly; a debuglink and build-id check; a stricter `main` ruleset and
  two-person approval; a lint rule for tag-triggered workflows; scheduled re-verification; one version source.
- **Alternatives considered:**
  - Two-person approval (`prevent_self_review: true`): it stalls while one maintainer is away; an admin could undo it.
  - One `v*` ruleset with an admin bypass: admins could move release tags, and writers could create other tags.
  - A throwaway repository for the rehearsal: public Sigstore entries too, without this repository's settings.
  - The glibc source RPM as an asset: both sources tarballs hold it, and it would be the one unattested asset.
  - Notes generated from the merged PRs: for `v0.1.0` they would list every PR since the first.
  - One merged release-assets artifact: 217 MB uploaded twice, and integrity resting on an overwritable artifact.
