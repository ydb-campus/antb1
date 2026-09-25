# AI agents

Humans and AI coding agents (Claude Code, OpenAI Codex, GitHub Copilot) work on antb1 side by side. Every agent follows
[AGENTS.md](../AGENTS.md), uses the same `pixi run <task>` commands as humans and CI, and passes the same gates. This
page covers what runs where, the one-time setup (including every step in a web UI), the secrets and who can reach
them, and what each person needs. The decisions are recorded in [ADR 0007](adr/0007-ai-agents.md); workflow
hardening is described in [docs/ci.md](ci.md).

## At a glance

| Agent | Where it runs | Reads | Setup |
| --- | --- | --- | --- |
| Claude Code | your terminal; cloud sessions on claude.ai/code | CLAUDE.md, which imports AGENTS.md | [Claude Code cloud environment](#claude-code-cloud-environment) |
| Claude in GitHub Actions | `@claude` on issues and PRs; one automatic review per PR | the same | [Claude GitHub App and token](#claude-github-app-and-token) |
| Codex | Codex CLI; ChatGPT Codex cloud tasks; `@codex review` on PRs | AGENTS.md | [Codex](#codex) |
| Codex harness audit | GitHub Actions, one audit per PR | base-commit prompt and AGENTS.md | [Codex harness audit and API key](#codex-harness-audit-and-api-key) |
| Copilot | coding agent and code review on github.com | `.github/copilot-instructions.md`, AGENTS.md | [Copilot](#copilot) |
| antb1-bot | GitHub Actions, weekly | none (no model) | [Bot App antb1-bot](#bot-app-antb1-bot) |

## Workflows

All of these are advisory: none is a required check or part of `CI OK`, so a failed or missing run never blocks a
merge. They follow the workflow security rules in [docs/ci.md](ci.md).

| Workflow | Triggers | What it does | Secret or variable |
| --- | --- | --- | --- |
| `Claude` ([claude.yml](../.github/workflows/claude.yml)) | `@claude` in an issue comment, PR comment, review or review comment; an issue opened with `@claude`; the issue label `claude` | Claude Code answers or works on the issue or PR, runs allowed tasks, pushes a branch and keeps one reply comment up to date | `CLAUDE_CODE_OAUTH_TOKEN` |
| `Claude review` ([claude-review.yml](../.github/workflows/claude-review.yml)) | PR opened, ready for review or reopened (same repository, not a draft, no bots except Copilot) | Anthropic's code-review plugin posts inline comments for high-confidence problems | `CLAUDE_CODE_OAUTH_TOKEN` |
| `Codex harness audit` ([codex-review.yml](../.github/workflows/codex-review.yml)) | PR opened, ready for review or reopened (same repository, not a draft, no bots); the label `codex-review` | Codex CLI 0.155.1, read-only and without sudo, checks the diff against the harness rules and posts one sticky comment | `OPENAI_API_KEY` |
| `Copilot Setup Steps` ([copilot-setup-steps.yml](../.github/workflows/copilot-setup-steps.yml)) | before every Copilot coding-agent task; manual; PRs and pushes that change it, `pixi.toml` or `pixi.lock` | installs the locked `default` and `lint` environments and builds `build/dev` | none |
| `pixi.lock update` ([pixi-lock-update.yml](../.github/workflows/pixi-lock-update.yml)) | Mondays 05:00 UTC; manual | `pixi update` with pixi 0.81.0, then a PR `build(deps): update pixi.lock` from antb1-bot | `BOT_CLIENT_ID` (variable), `BOT_PRIVATE_KEY` (environment `automation`) |

The workflow token (`GITHUB_TOKEN`) is read-only in every job that runs a model. The Claude jobs write through the
Claude GitHub App's token, which the action obtains with the job's OIDC token. The Codex audit job writes nothing;
a separate job with only `pull-requests: write` posts its comment. The lock update writes through antb1-bot's token.

## How AI reviews work

- Three AI reviewers can comment on a PR: Claude (an automatic review once per PR), Codex (the automatic harness
  audit once per PR, and a full review on `@codex review`) and Copilot (only when requested). None of them runs
  again on every push.
- All of them apply the AGENTS.md "Code Review Rules": concrete, high-confidence problems with a failure scenario, no
  formatting comments, no ClickBench data values.
- They are advisory and never approve. Claude and Codex post comments, Copilot's reviews are comment reviews, and
  the repository does not let GitHub Actions approve pull requests. Merging still needs `CI OK`, `PR title` and one
  human approval.
- The ruleset requires every review thread to be resolved, AI threads included. Fix the problem, or reply why the
  comment does not apply, then resolve the thread.
- Ask for another review with `@claude review` (or a narrower request such as `@claude review the io changes`),
  `@codex review` or `@codex review for <focus>`, by adding the label `codex-review` (remove it first if it is
  already there), or by requesting Copilot as a reviewer.

## Using @claude

- Who: someone with write access to antb1, on an issue or PR whose author also has write access (Copilot's PRs are
  accepted). Pull requests from forks are refused. Anything else fails at the workflow's first step with an error
  that names the user without write access.
- How: mention `@claude` in an issue comment, a PR comment, a review or an inline review comment; open an issue
  whose title or body mentions `@claude`; or add the label `claude` to an issue.
- What Claude can do there: read the repository and the PR, edit files, commit and push to a branch (a new
  `claude/...` branch for issues, the PR's branch for PRs), read CI runs, and keep one reply comment up to date. The
  only shell commands it may run are `pixi run configure`, `pixi run build`, `pixi run test` (with ctest arguments),
  `pixi run ci`, `pixi run asan`, `pixi run lint`, `pixi run fmt`, `pixi run check`, `pixi run doctor`,
  `pixi run antb1 ...`, and read-only `git` and `gh` commands. Hooks are disabled. The `gcc` environment and
  `pixi run tidy` are left to CI.
- Claude's commits go through a PR, CI and a human approval like everyone else's; the Claude App cannot push to
  `main`. Paths under "Ask a human first" in AGENTS.md still need a maintainer's agreement.
- Every run draws on the Claude subscription behind `CLAUDE_CODE_OAUTH_TOKEN`; keep requests focused.

## Setup order

A repository admin does this once, in this order (the steps are detailed below):

1. Install the Claude GitHub App and set `CLAUDE_CODE_OAUTH_TOKEN`.
2. Create a spend-capped OpenAI project key and set `OPENAI_API_KEY`.
3. After the AI workflows are on `main`: set up the Codex connection, cloud environment and review; the Claude Code
   cloud environment; and Copilot, if the organization has seats.
4. Create the antb1-bot App, set `BOT_CLIENT_ID` and `BOT_PRIVATE_KEY`, then run `pixi.lock update` once by hand.
5. Confirm that Copilot runs are not blocked by the repository's "Require actions to be pinned to a full-length
   commit SHA" setting (see [Copilot](#copilot)).

Each collaborator then completes the [per-person prerequisites](#per-person-prerequisites).

## Claude GitHub App and token

1. An organization owner opens <https://github.com/apps/claude>, chooses **Install** (or **Configure**), selects
   the `ydb-campus` organization, picks **Only select repositories**, selects `antb1` and confirms. Do not use
   `/install-github-app` in Claude Code for this: it opens a PR with its own workflow files, and ours already exist.
2. On a machine with Claude Code, the owner of the Claude subscription that pays for the automation runs
   `claude setup-token`, signs in in the browser, and copies the printed token. It is valid for about one year.
3. Store it without echoing it to the terminal history:

   ```bash
   gh secret set CLAUDE_CODE_OAUTH_TOKEN -R ydb-campus/antb1   # paste the token at the prompt
   ```

4. Put a renewal reminder in the calendar about 11 months ahead; renewal repeats steps 2 and 3. An expired token
   makes both Claude workflows fail with an authentication error.
5. Both Claude workflows work only once they are on `main`: before that (and on the PR that adds them) the token
   exchange fails with `workflow_not_found_on_default_branch`.

One subscription serves all members' `@claude` requests and the automatic reviews. If its rate limits or policy
become a problem, switch to API billing:

1. Create an API key in the Anthropic Console, in a workspace with a spend limit.
2. Store it with `gh secret set ANTHROPIC_API_KEY -R ydb-campus/antb1`.
3. In both Claude workflows, replace the input `claude_code_oauth_token` with
   `anthropic_api_key: ${{ secrets.ANTHROPIC_API_KEY }}` (a `.github/` change, reviewed like any other).

## Claude Code cloud environment

Claude Code on the web (<https://claude.ai/code>) runs each session in a fresh Linux container that clones the
repository. Its environment needs pixi and the locked environments; the setup script below provides them.

1. Open <https://claude.ai/code>, connect GitHub if asked, and select `ydb-campus/antb1`.
2. Open the environment selector and choose **Add environment** (or edit the existing one):
   - **Name:** `antb1`.
   - **Network access:** **Trusted**. Its allowlist includes `conda.anaconda.org` (pixi and every conda package)
     and `raw.githubusercontent.com` (main's `pixi.toml` and `pixi.lock`), which is all the setup needs.
   - **Environment variables:** `PIXI_CACHE_DIR=/opt/antb1/pixi-cache`.
   - **Setup script:** the script below, unchanged.
3. Start a session. The setup script runs as root before Claude starts; then Claude's session-start hook runs
   `bash scripts/agent-setup.sh --envs "default lint"`, which finds `/usr/local/bin/pixi` and installs the locked
   environments from the warm package cache. If that hook is not active, ask Claude to run the same command.

```bash
#!/bin/bash
# antb1 Claude cloud setup (root, before the session). pixi from conda-forge (GitHub release assets of unattached repos
# return 403 here), then warm the package cache from main's lock. Never fails the setup.
set -uo pipefail
V=0.81.0; F=linux-64/pixi-0.81.0-hf01adef_0.conda; S=691c4f465b27b9ed0aeee0849c5a1f8b234f1ef02d4c7d7572855bcca84d3e10
export PIXI_CACHE_DIR=${PIXI_CACHE_DIR:-/opt/antb1/pixi-cache}; mkdir -p "$PIXI_CACHE_DIR"
if [ "$(pixi --version 2>/dev/null | awk '{print $2}')" != "$V" ]; then
  command -v zstd >/dev/null || { timeout 40 apt-get update -qq && timeout 40 apt-get install -y -qq zstd; } || true
  t=$(mktemp -d)
  timeout 40 curl -fsSL --retry 2 -o "$t/p.conda" "https://conda.anaconda.org/conda-forge/$F" \
    && echo "$S  $t/p.conda" | sha256sum -c - \
    && python3 -c 'import sys,zipfile;z=zipfile.ZipFile(sys.argv[1]);n=next(x for x in z.namelist() if x.startswith("pkg-"));sys.stdout.buffer.write(z.read(n))' "$t/p.conda" \
       | zstd -dc | tar -x -C "$t" bin/pixi \
    && "$t/bin/pixi" --version \
    && install -m 0755 "$t/bin/pixi" /usr/local/bin/pixi
fi
w=/opt/antb1/warm; mkdir -p "$w"
timeout 10 curl -fsSL -o "$w/pixi.toml" https://raw.githubusercontent.com/ydb-campus/antb1/main/pixi.toml \
  && timeout 10 curl -fsSL -o "$w/pixi.lock" https://raw.githubusercontent.com/ydb-campus/antb1/main/pixi.lock \
  && (cd "$w" && timeout 150 pixi install --locked -e default -e lint; rm -rf "$w/.pixi") || true
chmod -R a+rwX /opt/antb1 || true      # the session user may not be root
exit 0
```

Why it looks like this:

- It is self-contained because it runs before Claude, from an unknown working directory, and it must never fail
  the session: every step has a timeout and the script always exits 0. Its worst case is about 290 seconds, within
  the setup time limit of about 5 minutes.
- GitHub release downloads of other projects are blocked (HTTP 403) in cloud sessions, so it installs pixi 0.81.0
  from conda-forge and checks the file's sha256 (the same pin as `scripts/agent-setup.sh`). The conda-forge build
  needs glibc and OpenSSL 3, which the cloud image has.
- It warms only the package cache (`PIXI_CACHE_DIR`), from `main`'s lock file. A branch with a different lock file
  downloads just the difference. The cache may be on another file system than the checkout; pixi then copies
  instead of hard-linking, which is slower but correct.
- If the Trusted allowlist ever stops covering these hosts, switch **Network access** to **Custom** and allow
  `conda.anaconda.org`, `raw.githubusercontent.com` and `pixi.prefix.dev` in addition to the defaults.
- The `gcc` environment is not installed. `pixi run ci-gcc` and `pixi run check-full` need it: run
  `bash scripts/agent-setup.sh --envs "gcc"` in a session with network access first.

## Codex

### Connecting Codex to GitHub

1. An organization owner opens ChatGPT Codex (<https://chatgpt.com/codex>), connects GitHub, and installs the
   ChatGPT Codex Connector GitHub App on `ydb-campus` with access to `antb1` only.
2. Each person who uses Codex cloud tasks or `@codex` signs in to ChatGPT with a plan that includes Codex and
   connects their own GitHub account in the Codex settings.

### Codex cloud environment

In ChatGPT Codex, open **Settings** → **Environments** → **Create environment** and set:

| Setting | Value |
| --- | --- |
| Repository | `ydb-campus/antb1` |
| Container image | universal |
| Setup script | `bash scripts/agent-setup.sh --envs "default lint" --persist-bashrc` |
| Maintenance script | the same command (it is idempotent) |
| Environment variables and secrets | none |
| Agent internet access | Off |

- The setup script runs with network access. It installs the pinned, sha256-verified pixi 0.81.0 from the GitHub
  release and the locked `default` (Clang 23) and `lint` environments. `--persist-bashrc` puts pixi on `PATH` in the
  first line of `~/.bashrc` and `~/.profile` (before the early return for non-interactive shells) and, as root,
  links `/usr/local/bin/pixi`.
- The agent itself runs offline. Every task works offline once the environments exist.
- Add `gcc` to `--envs` if Codex should run `pixi run ci-gcc` or `pixi run check-full`; it makes the setup bigger.

### Native Codex review

In ChatGPT Codex, open **Settings** → **Code review**, enable review for `ydb-campus/antb1` and keep
**Automatic reviews** off. Reviews then run only when someone comments `@codex review` (or
`@codex review for <focus>`) on a PR. Codex applies the AGENTS.md "Code Review Rules". The person who comments
needs a linked ChatGPT account.

### Codex harness audit and API key

[codex-review.yml](../.github/workflows/codex-review.yml) runs Codex CLI in GitHub Actions once per PR. It takes
its prompt ([.github/codex/review-prompt.md](../.github/codex/review-prompt.md)) and AGENTS.md from the PR's base
commit, so a PR cannot rewrite its own instructions, and runs Codex with the `:read-only` permission profile and
without sudo. The answer (a harness checklist plus P0/P1 findings) is posted as one sticky comment. The audit is
skipped with a notice when the base commit has no prompt yet, and when the `OPENAI_API_KEY` secret is not set, so
the workflow stays inert until the key exists. Codex CLI is pinned to 0.155.1; bump it by hand to a release that is
at least 7 days old.

To set up the key:

1. In the OpenAI Platform (<https://platform.openai.com>), create a project for this repository, for example
   `antb1-ci`.
2. In the project's **Limits**, set a monthly budget (the spend cap for a leaked key or a runaway workflow) and a
   notification threshold.
3. In the project's **API keys**, create a new secret key for a service account of that project.
4. Store it:

   ```bash
   gh secret set OPENAI_API_KEY -R ydb-campus/antb1   # paste the key at the prompt
   ```

## Copilot

Copilot needs organization seats and policy; skip this section if the organization has none.

1. An organization owner assigns Copilot seats to the members who will use it (**Organization settings** →
   **Copilot** → **Access**) and enables the Copilot coding agent for `antb1` under **Copilot** → **Policies**.
2. In the repository settings, under **Copilot** → **Coding agent**, keep the agent enabled and its firewall on with
   the recommended allowlist. The setup steps run outside the firewall.
3. [copilot-setup-steps.yml](../.github/workflows/copilot-setup-steps.yml) prepares every Copilot task: it installs
   the locked `default` and `lint` environments and builds `build/dev`, so the agent can run `pixi run test`,
   `pixi run lint` and `pixi run check` without network access. After changing it, run it once from the Actions tab
   (**Copilot Setup Steps** → **Run workflow**); PRs that change it, `pixi.toml` or `pixi.lock` run it automatically.
4. Keep the defaults that make a human the gate: a maintainer approves workflow runs on Copilot's PRs before CI
   runs, and the Actions setting "Allow GitHub Actions to create and approve pull requests" stays off
   (`tools/github/apply-settings.sh` enforces it).
5. Copilot code review is on request only (add Copilot as a reviewer). Automatic Copilot review stays off, so a PR
   does not get three automatic reviews.
6. The repository requires every action to be pinned to a full commit SHA. After the first Copilot task, check that
   its setup-steps run was not blocked by that setting. If it was, a repository admin turns the setting off; the
   lint checks (`pixi run lint`: R010, R015, zizmor) still enforce pinning for this repository's workflows.

To give Copilot a task, assign an issue to Copilot (the `agent-task` issue form fits) or ask it from Copilot Chat.
It works on a `copilot/...` branch, opens a draft PR and asks for your review when done. Claude review runs when
the PR is marked ready for review (the Codex audit skips bot PRs); like every PR, it needs CI and one human approval.

## Bot App antb1-bot

[pixi-lock-update.yml](../.github/workflows/pixi-lock-update.yml) refreshes `pixi.lock` weekly through a
dedicated organization GitHub App. A PR opened with the App's token triggers CI like any member's PR (one opened
with `GITHUB_TOKEN` would not) and still needs one human approval. The job is skipped until `BOT_CLIENT_ID` exists.

1. An organization owner opens **Organization settings** → **Developer settings** → **GitHub Apps** →
   **New GitHub App** and sets:
   - **GitHub App name:** `antb1-bot` (App names are global; if it is taken, pick another, since the workflows only
     use the client ID);
   - **Homepage URL:** `https://github.com/ydb-campus/antb1`;
   - **Webhook:** clear **Active** (no webhook);
   - **Repository permissions:** **Contents: Read and write**, **Pull requests: Read and write** (Metadata:
     Read-only is added automatically); nothing else;
   - **Where can this GitHub App be installed?** **Only on this account**.
2. Choose **Create GitHub App**, copy the **Client ID** from the App's page, and under **Private keys** choose
   **Generate a private key** (a `.pem` file is downloaded).
3. Choose **Install App** → `ydb-campus` → **Only select repositories** → `antb1` → **Install**.
4. Store the client ID as a variable and the key as a secret of the `automation` environment (created by
   `tools/github/apply-settings.sh`; deployable from `main` only), then delete the downloaded key file:

   ```bash
   key=~/Downloads/antb1-bot.2026-09-25.private-key.pem   # the file you downloaded
   gh variable set BOT_CLIENT_ID -R ydb-campus/antb1 --body "<client ID>"
   gh secret set BOT_PRIVATE_KEY -R ydb-campus/antb1 --env automation < "$key"
   rm -- "$key"
   ```

5. Run the workflow once and check the result: `gh workflow run pixi-lock-update.yml -R ydb-campus/antb1`, then
   `gh run list -R ydb-campus/antb1 --workflow pixi-lock-update.yml`. When the lock changed, a PR from
   `bot/pixi-lock-update` appears with the labels `dependencies` and `pixi`, a signed commit and the package diff.

The job regenerates the lock with pixi 0.81.0 only (the lock format must stay `version: 7`). If a later run finds
no changes while the PR is still open, the PR is closed and its branch deleted.

## Secrets and exposure model

| Name | Kind | Used by | Reachable from |
| --- | --- | --- | --- |
| `CLAUDE_CODE_OAUTH_TOKEN` | repository secret | Claude, Claude review | workflows of this repository's events and same-repo branches; never fork PRs |
| `OPENAI_API_KEY` | repository secret | Codex harness audit | the same |
| `BOT_PRIVATE_KEY` | secret of the environment `automation` | pixi.lock update | only jobs that run on `main` and name the environment |
| `BOT_CLIENT_ID` | repository variable (not secret) | pixi.lock update | any workflow; useless without the key |

- GitHub does not pass secrets to workflows that pull requests from forks trigger, runs from external contributors'
  forks need a maintainer's approval, and every AI job also skips fork PRs.
- Anyone with write access can push a branch whose workflow change reads a repository secret in a PR run. That is
  inherent to repository secrets; the mitigations are that only organization members have write access,
  `.github/` changes request review from the code owners, the OpenAI key is spend-capped, the Claude token only
  draws on a subscription, and every secret can be rotated in minutes. `BOT_PRIVATE_KEY` is out of reach for PR
  branches because the `automation` environment deploys from `main` only.
- Prompt injection (instructions hidden in issues, PRs, comments or files) is handled in depth: `@claude` requires
  write access for both the sender and the author and refuses forks; the review workflows run only on same-repo,
  non-draft PRs; Claude runs with hooks disabled, a fixed tool allowlist and `.claude/` and CLAUDE.md restored from
  the base branch; the Codex audit reads its prompt and rules from the base commit, does not load the PR's AGENTS.md
  as instructions, and runs read-only without sudo, with no write token in its job; and all AI output is advisory,
  behind CI and a human approval.
- What remains: the `@claude` job builds and tests the branch, so code in the branch runs with the job's credentials
  (the Claude token and the Claude App token); the tool allowlist limits Claude's commands, not what the code it
  builds does. That is why both the sender and the author need write access; on a Copilot PR, the person who
  mentions `@claude` vouches for Copilot's code. Claude also reads every comment in the thread, including comments
  from people without write access: read them before you mention `@claude` in a thread that outsiders commented on.
- The workflows never print secrets. codex-action keeps the OpenAI key in a local proxy process that Codex cannot
  read; claude-code-action does not print Claude's full output to the log unless `show_full_output` is set (it is
  not).

To rotate a secret (and immediately after any suspected leak):

- `CLAUDE_CODE_OAUTH_TOKEN`: run `claude setup-token` again and store the new token with
  `gh secret set CLAUDE_CODE_OAUTH_TOKEN -R ydb-campus/antb1`. If the old token may have leaked, revoke it through
  the Claude account that created it and watch that subscription's usage.
- `OPENAI_API_KEY`: create a new key in the OpenAI project, store it with `gh secret set`, then delete the old key
  in the project and check the project's usage.
- `BOT_PRIVATE_KEY`: generate a new private key on the App's page, store it with
  `gh secret set BOT_PRIVATE_KEY -R ydb-campus/antb1 --env automation`, then delete the old key on the App's page.

## Per-person prerequisites

| To | You need |
| --- | --- |
| Build, test or run any agent locally | Linux: `bash scripts/agent-setup.sh` (installs pixi 0.81.0 if missing and the locked environments). macOS: pixi 0.81.0 or newer from <https://pixi.prefix.dev> |
| Mention `@claude` | write access to antb1; nothing else (the shared token pays) |
| Use Claude Code locally or on claude.ai/code | a Claude account with Claude Code; for the cloud, the environment above |
| Use Codex (CLI, cloud tasks, `@codex review`) | a ChatGPT plan with Codex and your GitHub account connected in Codex |
| Use Copilot (coding agent, review) | a Copilot seat from the organization |
| Approve and merge | write access; the approving human is accountable for the whole diff |

Local notes:

- Install the environments before you start Codex CLI: its sandbox blocks network access.
- On Ubuntu 20.04 hosts `scripts/agent-setup.sh` uses the static GitHub build of pixi; the conda-forge build needs
  OpenSSL 3.
- Claude Code reads CLAUDE.md, which imports AGENTS.md; Codex and Copilot read AGENTS.md directly.

## Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `@claude` run fails in its first step | the sender or the author lacks write access, the PR comes from a fork, or `CLAUDE_CODE_OAUTH_TOKEN` is not set; the error says which |
| Claude review ends with a notice and no comments | `CLAUDE_CODE_OAUTH_TOKEN` is not set |
| Claude workflows fail with `workflow_not_found_on_default_branch` | the workflow is not on `main` yet |
| Claude workflows fail to authenticate | `CLAUDE_CODE_OAUTH_TOKEN` expired or is missing; renew it |
| Codex audit ends with a notice and no comment | `OPENAI_API_KEY` is not set, or the base commit predates the prompt |
| The audit did not re-run after adding `codex-review` | the label was already present; remove it and add it again |
| `pixi.lock update` is skipped | the variable `BOT_CLIENT_ID` is not set |
| `pixi.lock update` fails at the App token step | wrong client ID, wrong key, or the App is not installed on antb1 |
