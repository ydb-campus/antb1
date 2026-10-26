"""A fake `gh` for tools/ci/tests/test_release.py (installed by fakes.install as a `#!{python} -I` runner of this file):
exactly the gh surface that .github/scripts/release.py uses, answered from a JSON state file that it also updates, so
one test can run preflight, draft, publish and check-published in sequence and then inspect the "repository". Any
other argv exits 99 ("fake gh: unexpected ..."), so a new gh call in release.py fails its tests until the fake learns
it.

  gh api --include --method M PATH [-F k=true|false]... [-f k=v]...
  gh api -H "Accept: application/octet-stream" repos/R/releases/assets/ID          (bytes to stdout)
  gh release create TAG --repo R --verify-tag --draft --title T --notes-file F [--prerelease] FILE...
  gh release verify TAG --repo R --format json
  gh release verify-asset TAG FILE --repo R
  gh attestation verify FILE --repo R --signer-workflow W --source-ref REF --source-digest SHA
                         --deny-self-hosted-runners --format json [--bundle B]

The real behaviour it mirrors: `--include` prints `HTTP/2.0 <code> <reason>\\n`, header lines ending in \\r\\n, a
blank \\r\\n line and the body; an HTTP error exits 1 with `gh: <message> (HTTP <code>)` on stderr; a token without
push access (scope "read") sees no drafts in lists, gets 404 for a draft by id and 403 on writes; get-by-tag and
latest return published releases only; `release create --draft` leaves the draft with the assets uploaded so far
when an upload fails; publishing (PATCH draft=false) makes the release immutable only when the repository setting is
on; the release attestation names the tag's ref object (the tag object of an annotated tag) and every asset.

State ($FAKE_GH_STATE): repo, scope ("read"/"write"), immutable (the setting), refs {tag: {type, sha}}, tags {sha:
git tag object}, compare {"<base>...<head>": body}, workflow_runs [workflow runs], jobs {run id: [jobs]},
environments {name: body}, branch_policies {name: [policies]}, rulesets [full rulesets], releases [...], latest,
next_id, attestations [bundles the online lookup finds], fail {upload: asset name, corrupt: asset name, verify: n
(release verify fails n times first), attestation: "tag-object"|"extra-asset"|"predicate" (a release attestation
that names another tag object, one more asset or another predicate), verify_asset: true, create: "403" (create
refused), patch: 403 (PATCH refused), http: {"<METHOD> <path in the repo>": status}}. Asset bytes:
$FAKE_GH_DIR/<asset id>. Log: one JSON argv per call in $FAKE_GH_LOG. A fake bundle is JSON {"fake": "signed", repo,
workflow, ref, sha, hosted, subjects}; it verifies when every identity flag matches and the file's sha256 is a
subject, so a test models a forged or foreign bundle by changing one field.
"""

from __future__ import annotations

import hashlib
import json
import os
import sys
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

STATE = Path(os.environ["FAKE_GH_STATE"])
BLOBS = Path(os.environ["FAKE_GH_DIR"])
SLSA = "https://slsa.dev/provenance/v1"
RELEASE = "https://in-toto.io/attestation/release/v0.2"
REASONS = {200: "OK", 201: "Created", 204: "No Content", 403: "Forbidden", 404: "Not Found", 422: "Unprocessable"}


def unexpected(argv: list[str]) -> None:
    print(f"fake gh: unexpected arguments {argv}", file=sys.stderr)
    sys.exit(99)


def respond(status: int, body: object = None) -> None:
    data = b"" if body is None else json.dumps(body).encode()
    sys.stdout.buffer.write(f"HTTP/2.0 {status} {REASONS.get(status, '')}\n".encode())
    sys.stdout.buffer.write(b"Content-Type: application/json; charset=utf-8\r\n\r\n" + data)
    sys.stdout.flush()
    if status >= 400:
        message = body.get("message", "") if isinstance(body, dict) else ""
        print(f"gh: {message} (HTTP {status})", file=sys.stderr)
        sys.exit(1)
    sys.exit(0)


def options(args: list[str], flags: set[str], values: set[str]) -> tuple[dict[str, str | bool], list[str]]:
    """Strict option parsing: only the listed flags and valued options, each once; the rest are positionals."""
    seen: dict[str, str | bool] = {}
    rest: list[str] = []
    i = 0
    while i < len(args):
        arg = args[i]
        if arg.startswith("-"):
            if arg in seen or (arg not in flags and arg not in values) or (arg in values and i + 1 >= len(args)):
                unexpected(args)
            seen[arg] = True if arg in flags else args[i + 1]
            i += 1 if arg in flags else 2
        else:
            rest.append(arg)
            i += 1
    return seen, rest


def visible(state: dict, release: dict) -> bool:
    return not release["draft"] or state["scope"] == "write"


def release_view(release: dict) -> dict:
    return {key: value for key, value in release.items() if key != "notes"}


def api(state: dict, args: list[str]) -> None:
    if args[:2] == ["-H", "Accept: application/octet-stream"] and len(args) == 3:
        asset_id = args[2].rsplit("/", 1)[-1]
        for release in state["releases"]:
            if visible(state, release) and any(str(a["id"]) == asset_id for a in release["assets"]):
                sys.stdout.buffer.write((BLOBS / asset_id).read_bytes())
                return
        print("gh: Not Found (HTTP 404)", file=sys.stderr)
        sys.exit(1)
    fields: dict[str, object] = {}
    rest = []
    i = 0
    while i < len(args):  # -F/-f repeat, so they are parsed apart from the strict options
        if args[i] in ("-F", "-f") and i + 1 < len(args) and "=" in args[i + 1]:
            key, value = args[i + 1].split("=", 1)
            fields[key] = {"true": True, "false": False}.get(value, value) if args[i] == "-F" else value
            i += 2
        else:
            rest.append(args[i])
            i += 1
    opts, positional = options(rest, {"--include"}, {"--method"})
    if "--include" not in opts or len(positional) != 1:
        unexpected(args)
    method = opts.get("--method", "GET")
    url = urlsplit(positional[0])
    query = {k: v[0] for k, v in parse_qs(url.query).items()}
    parts = url.path.strip("/").split("/")
    if parts[:3] != ["repos", *state["repo"].split("/")]:
        respond(404, {"message": "Not Found"})
    path = parts[3:]
    injected = state["fail"].get("http", {}).get(f"{method} {'/'.join(path)}")
    if injected:  # a status a test asks for, e.g. {"GET releases/tags/v0.1.0": 403}
        respond(int(injected), {"message": "injected"})
    if method != "GET" and state["scope"] != "write":
        respond(403, {"message": "Resource not accessible by integration"})
    page, per_page = int(query.get("page", "1")), int(query.get("per_page", "30"))

    def paged(items: list) -> list:
        return items[(page - 1) * per_page : page * per_page]

    if method == "GET" and path[:3] == ["git", "ref", "tags"] and len(path) == 4:
        ref = state["refs"].get(path[3])
        respond(200, {"ref": f"refs/tags/{path[3]}", "object": ref}) if ref else respond(404, {"message": "Not Found"})
    elif method == "GET" and path[:2] == ["git", "tags"] and len(path) == 3:
        tag = state["tags"].get(path[2])
        respond(200, tag) if tag else respond(404, {"message": "Not Found"})
    elif method == "GET" and path[:1] == ["compare"] and len(path) == 2:
        body = state["compare"].get(path[1])
        respond(200, body) if body else respond(404, {"message": "Not Found"})
    elif method == "GET" and path[:2] == ["actions", "workflows"] and path[3:] == ["runs"]:
        runs = [r for r in state["workflow_runs"] if r["path"].endswith("/" + path[2])
                and r["head_sha"] == query.get("head_sha", r["head_sha"])]  # fmt: skip
        respond(200, {"total_count": len(runs), "workflow_runs": paged(runs)})
    elif method == "GET" and path[:2] == ["actions", "runs"] and path[3:] == ["jobs"]:
        if query.get("filter") != "latest":
            unexpected(args)  # release.py reads the latest attempt's jobs
        jobs = state["jobs"].get(path[2], [])
        respond(200, {"total_count": len(jobs), "jobs": paged(jobs)})
    elif method == "GET" and path[:1] == ["environments"] and len(path) == 2:
        body = state["environments"].get(path[1])
        respond(200, body) if body else respond(404, {"message": "Not Found"})
    elif method == "GET" and path[:1] == ["environments"] and path[2:] == ["deployment-branch-policies"]:
        policies = state["branch_policies"].get(path[1], [])
        respond(200, {"total_count": len(policies), "branch_policies": paged(policies)})
    elif method == "GET" and path == ["rulesets"]:
        listed = [{k: r[k] for k in ("id", "name", "target", "enforcement", "source_type")} for r in state["rulesets"]]
        respond(200, paged(listed))
    elif method == "GET" and path[:1] == ["rulesets"] and len(path) == 2:
        found = [r for r in state["rulesets"] if str(r["id"]) == path[1]]
        respond(200, found[0]) if found else respond(404, {"message": "Not Found"})
    elif method == "GET" and path == ["releases"]:
        shown = [release_view(r) for r in sorted(state["releases"], key=lambda r: -r["id"]) if visible(state, r)]
        respond(200, paged(shown))
    elif method == "GET" and path[:2] == ["releases", "tags"] and len(path) == 3:
        found = [r for r in state["releases"] if r["tag_name"] == path[2] and not r["draft"]]
        respond(200, release_view(found[0])) if found else respond(404, {"message": "Not Found"})
    elif path[:1] == ["releases"] and len(path) == 2 and path[1].isdigit():
        found = [r for r in state["releases"] if r["id"] == int(path[1]) and visible(state, r)]
        if not found:
            respond(404, {"message": "Not Found"})
        release = found[0]
        if method == "GET":
            respond(200, release_view(release))
        elif method == "DELETE":
            if not release["draft"] and release["immutable"]:
                respond(422, {"message": "immutable release"})  # release.py never deletes a published release
            state["releases"].remove(release)
            save(state)
            respond(204)
        elif method == "PATCH" and set(fields) <= {"draft", "prerelease", "make_latest"}:
            if state["fail"].get("patch"):
                respond(int(state["fail"]["patch"]), {"message": "Resource not accessible by integration"})
            if fields.get("draft") is False and release["draft"]:
                if any(r["tag_name"] == release["tag_name"] and not r["draft"] for r in state["releases"]):
                    respond(422, {"message": "Validation Failed: tag_name already_exists"})
                release["draft"], release["immutable"] = False, state["immutable"]
            release["prerelease"] = fields.get("prerelease", release["prerelease"])
            if fields.get("make_latest", "true") == "true" and not release["prerelease"]:  # the API's default
                state["latest"] = release["id"]
            save(state)
            respond(200, release_view(release))
        else:
            unexpected(args)
    else:
        unexpected(args)


def release_create(state: dict, args: list[str]) -> None:
    opts, positional = options(args, {"--verify-tag", "--draft", "--prerelease"}, {"--repo", "--title", "--notes-file"})
    required = {"--verify-tag", "--draft", "--repo", "--title", "--notes-file"}
    if not required <= set(opts) or opts["--repo"] != state["repo"] or len(positional) < 2:
        unexpected(args)  # no --target, --latest, --generate-notes or --notes-from-tag
    if state["scope"] != "write" or state["fail"].get("create"):
        print(f"HTTP {state['fail'].get('create', '403')}: Resource not accessible by integration", file=sys.stderr)
        sys.exit(1)
    tag, files = positional[0], [Path(f) for f in positional[1:]]
    if tag not in state["refs"]:
        print(
            f"tag {tag} doesn't exist in the repo {state['repo']}, aborting due to --verify-tag flag", file=sys.stderr
        )
        sys.exit(1)
    release = {"id": state["next_id"], "tag_name": tag, "draft": True, "prerelease": "--prerelease" in opts,
               "immutable": False, "name": opts["--title"], "body": Path(str(opts["--notes-file"])).read_text(),
               "target_commitish": "main", "assets": [],
               "html_url": f"https://github.com/{state['repo']}/releases/tag/untagged-{state['next_id']}"}  # fmt: skip
    state["next_id"] += 1
    state["releases"].append(release)
    for file in files:
        if state["fail"].get("upload") == file.name:
            save(state)
            print(f"HTTP 502: upload of {file.name} failed", file=sys.stderr)
            sys.exit(1)  # like gh: the --draft release stays with the assets uploaded so far
        data = file.read_bytes() + (b"x" if state["fail"].get("corrupt") == file.name else b"")
        asset_id = state["next_id"]
        state["next_id"] += 1
        (BLOBS / str(asset_id)).write_bytes(data)
        release["assets"].append({"id": asset_id, "name": file.name, "size": len(data), "state": "uploaded",
                                  "digest": "sha256:" + hashlib.sha256(data).hexdigest()})  # fmt: skip
    save(state)
    print(release["html_url"])


def published(state: dict, tag: str) -> dict | None:
    found = [r for r in state["releases"] if r["tag_name"] == tag and not r["draft"]]
    return found[0] if found and found[0]["immutable"] else None


def release_verify(state: dict, args: list[str]) -> None:
    opts, positional = options(args, set(), {"--repo", "--format"})
    if opts != {"--repo": state["repo"], "--format": "json"} or len(positional) != 1:
        unexpected(args)
    release = published(state, positional[0])
    if state["fail"].get("verify", 0) > 0:
        state["fail"]["verify"] -= 1
        save(state)
        release = None
    if release is None:
        print(f"no attestations found for release {positional[0]}", file=sys.stderr)
        sys.exit(1)
    ref, fault = state["refs"][positional[0]], state["fail"].get("attestation", "")
    subjects = [{"uri": f"pkg:github/{state['repo']}@{release['tag_name']}",
                 "digest": {"sha1": "0" * 40 if fault == "tag-object" else ref["sha"]}}]  # fmt: skip
    subjects += [{"name": a["name"], "digest": {"sha256": a["digest"].removeprefix("sha256:")}}
                 for a in release["assets"]]  # fmt: skip
    if fault == "extra-asset":
        subjects.append({"name": "extra.txt", "digest": {"sha256": "0" * 64}})
    predicate = {"tag": release["tag_name"], "repository": state["repo"], "databaseId": str(release["id"])}
    kind = "https://example.invalid/other" if fault == "predicate" else RELEASE
    print(json.dumps({"verificationResult": {"statement": {"predicateType": kind, "subject": subjects,
                                                           "predicate": predicate}}}))  # fmt: skip


def release_verify_asset(state: dict, args: list[str]) -> None:
    opts, positional = options(args, set(), {"--repo"})
    if opts != {"--repo": state["repo"]} or len(positional) != 2:
        unexpected(args)
    release = published(state, positional[0])
    digest = "sha256:" + hashlib.sha256(Path(positional[1]).read_bytes()).hexdigest()
    listed = release is not None and any(a["digest"] == digest for a in release["assets"])
    if not listed or state["fail"].get("verify_asset"):
        print(f"no attestation for {positional[1]} in release {positional[0]}", file=sys.stderr)
        sys.exit(1)
    print(f"Verified asset {Path(positional[1]).name} of {positional[0]}")


def attestation_verify(state: dict, args: list[str]) -> None:
    opts, positional = options(
        args,
        {"--deny-self-hosted-runners"},
        {"--repo", "--signer-workflow", "--source-ref", "--source-digest", "--format", "--bundle"},
    )
    if len(positional) != 1 or opts.get("--format") != "json" or "--deny-self-hosted-runners" not in opts:
        unexpected(args)
    digest = hashlib.sha256(Path(positional[0]).read_bytes()).hexdigest()
    bundles = [json.loads(Path(str(opts["--bundle"])).read_text())] if "--bundle" in opts else state["attestations"]
    want = {"repo": opts.get("--repo"), "workflow": opts.get("--signer-workflow"), "ref": opts.get("--source-ref"),
            "sha": opts.get("--source-digest"), "hosted": True, "fake": "signed"}  # fmt: skip
    good = [b for b in bundles if all(b.get(k) == v for k, v in want.items())
            and any(s["digest"].get("sha256") == digest for s in b["subjects"])]  # fmt: skip
    if not good:
        print(f"Error: no matching attestation for sha256:{digest}", file=sys.stderr)
        sys.exit(1)
    print(json.dumps([{"verificationResult": {"statement": {"predicateType": SLSA, "subject": b["subjects"]}}}
                      for b in good]))  # fmt: skip


def save(state: dict) -> None:
    STATE.write_text(json.dumps(state))


def main() -> None:
    argv = sys.argv[1:]
    with open(os.environ["FAKE_GH_LOG"], "a") as log:
        log.write(json.dumps(argv) + "\n")
    state = json.loads(STATE.read_text())
    if argv[:1] == ["api"]:
        api(state, argv[1:])
    elif argv[:2] == ["release", "create"]:
        release_create(state, argv[2:])
    elif argv[:2] == ["release", "verify"]:
        release_verify(state, argv[2:])
    elif argv[:2] == ["release", "verify-asset"]:
        release_verify_asset(state, argv[2:])
    elif argv[:2] == ["attestation", "verify"]:
        attestation_verify(state, argv[2:])
    else:
        unexpected(argv)


if __name__ == "__main__":
    main()
