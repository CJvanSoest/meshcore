#!/usr/bin/env bash
# scripts/release.sh — cut a release on Gitea + GitHub in one go.
#
# Usage:   scripts/release.sh v2.3.0          # cut release
#          scripts/release.sh v2.3.0 --dry-run  # show plan, no side effects
#
# Steps (in order, fail fast):
#   1. Sanity:    semver tag, clean tree, on main, local==gitea/main,
#                 GitHub-side sync recent.
#   2. Notes:     CHANGELOG.md section for VERSION if present, else a
#                 categorized list of PR titles merged since last tag.
#   3. Confirm.
#   4. Build:     idf.py build for the tanmatsu target.
#   5. Tag Gitea: annotated tag on local main tip, push.
#   6. Tag GitHub: lightweight tag on github/main tip (the synced SHA,
#                  which differs from Gitea's due to devlog-strip).
#   7. Releases: Gitea + GitHub via REST API, application.bin attached.
#
# Secrets sourced from Infisical (see memory tanmatsu-meshcore-workflow):
#   GITEA_API_TOKEN   — env=dev, root
#   GITHUB_PAT_TANMATSU — env=dev, root

set -euo pipefail

# ---------- args ----------
VERSION="${1:-}"
DRY_RUN=0
[[ "${2:-}" == "--dry-run" ]] && DRY_RUN=1

[[ -z "$VERSION" ]] && { echo "usage: $0 vX.Y.Z [--dry-run]"; exit 1; }
[[ "$VERSION" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
    echo "error: version must be vX.Y.Z (got: $VERSION)"; exit 1;
}
VBARE="${VERSION#v}"   # strip leading "v"

# ---------- sanity ----------
echo ">> sanity"

[[ -z "$(git status --porcelain)" ]] || {
    echo "error: working tree dirty — commit or stash first"; exit 1;
}

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[[ "$BRANCH" == "main" ]] || { echo "error: not on main (on $BRANCH)"; exit 1; }

git fetch --quiet gitea main
git fetch --quiet github main

LOCAL="$(git rev-parse HEAD)"
GITEA_HEAD="$(git rev-parse gitea/main)"
GITHUB_HEAD="$(git rev-parse github/main)"

[[ "$LOCAL" == "$GITEA_HEAD" ]] || {
    echo "error: local main ($LOCAL) ≠ gitea/main ($GITEA_HEAD) — push or pull first"
    exit 1
}

# GitHub-side: warn but don't block — the dual-remote-divergence means
# github/main may legitimately lag if a devlog-only commit was the most
# recent push. Show the diff so the user can decide.
GITHUB_DIFF="$(git log --oneline "$GITHUB_HEAD..$LOCAL" -- ':(exclude)docs/devlog-*' | head -10)"
if [[ -n "$GITHUB_DIFF" ]]; then
    echo "warning: github/main is missing non-devlog commits:"
    echo "$GITHUB_DIFF" | sed 's/^/    /'
    echo "run the worktree-strip sync (see tanmatsu-meshcore-github-divergence memory) before continuing."
    [[ $DRY_RUN -eq 0 ]] && exit 1
fi

if git rev-parse "$VERSION" >/dev/null 2>&1; then
    echo "error: tag $VERSION already exists locally"; exit 1
fi

# ---------- notes ----------
echo ">> release notes"

NOTES=""
if [[ -f CHANGELOG.md ]]; then
    NOTES="$(awk -v v="$VBARE" '
        $0 ~ "^## \\[" v "\\]" { found=1; next }
        found && /^## \[/ { exit }
        found { print }
    ' CHANGELOG.md | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}')"
fi

if [[ -z "$NOTES" ]]; then
    PREV="$(git describe --tags --abbrev=0 HEAD 2>/dev/null || echo "")"
    RANGE="${PREV:+$PREV..}HEAD"
    MERGES="$(git log "$RANGE" --merges --format='%s' | sed -E 's/ \(#([0-9]+)\)$//' )"

    if [[ -z "$MERGES" ]]; then
        echo "error: no CHANGELOG section for $VERSION and no merged PRs since ${PREV:-repo start}"
        exit 1
    fi

    declare -A SECTIONS=(
        [feat]="### Added"
        [fix]="### Fixed"
        [docs]="### Changed"
        [refactor]="### Changed"
        [chore]="### Changed"
        [ci]="### Changed"
        [perf]="### Changed"
    )

    declare -A BUCKETS
    while IFS= read -r line; do
        [[ -z "$line" ]] && continue
        prefix="$(echo "$line" | sed -nE 's/^([a-z]+)(\([^)]*\))?:.*/\1/p')"
        section="${SECTIONS[$prefix]:-### Other changes}"
        BUCKETS[$section]+="- $line"$'\n'
    done <<< "$MERGES"

    for sect in "### Added" "### Changed" "### Fixed" "### Other changes"; do
        [[ -n "${BUCKETS[$sect]:-}" ]] && {
            NOTES+="$sect"$'\n'"${BUCKETS[$sect]}"$'\n'
        }
    done
fi

echo "---"
echo "$NOTES"
echo "---"

# ---------- confirm ----------
if [[ $DRY_RUN -eq 1 ]]; then
    echo ">> DRY RUN — would now: build, tag $VERSION on gitea ($LOCAL) and github ($GITHUB_HEAD), create releases."
    exit 0
fi

read -r -p "Proceed with release $VERSION? [y/N] " confirm
[[ "$confirm" == "y" || "$confirm" == "Y" ]] || { echo "aborted"; exit 0; }

# ---------- secrets ----------
echo ">> fetching tokens from Infisical"
INF_TOKEN="$(curl -s -X POST "$INFISICAL_HOST/api/v1/auth/universal-auth/login" \
    -H "Content-Type: application/json" \
    -d "{\"clientId\":\"$INFISICAL_CLIENT_ID\",\"clientSecret\":\"$INFISICAL_CLIENT_SECRET\"}" \
    | python3 -c "import sys,json; print(json.load(sys.stdin)['accessToken'])")"

fetch_secret() {
    curl -s -G -H "Authorization: Bearer $INF_TOKEN" \
        "$INFISICAL_HOST/api/v3/secrets/raw" \
        --data-urlencode "workspaceId=$INFISICAL_PROJECT_ID" \
        --data-urlencode "environment=dev" \
        --data-urlencode "secretPath=/" \
        | python3 -c "import sys,json,os; k=os.environ['K']; [print(s['secretValue']) for s in json.load(sys.stdin)['secrets'] if s['secretKey']==k]"
}
GITEA_PAT="$(K=GITEA_API_TOKEN fetch_secret)"
GITHUB_PAT="$(K=GITHUB_PAT_TANMATSU fetch_secret)"

[[ -n "$GITEA_PAT" && -n "$GITHUB_PAT" ]] || { echo "error: missing tokens"; exit 1; }

# ---------- build ----------
echo ">> build"
make build DEVICE=tanmatsu
BIN="build/tanmatsu/application.bin"
[[ -f "$BIN" ]] || { echo "error: $BIN not produced"; exit 1; }

# ---------- tags ----------
echo ">> tag gitea ($VERSION on $LOCAL)"
git tag -a "$VERSION" -m "Release $VERSION" "$LOCAL"
git push gitea "$VERSION"

echo ">> tag github (lightweight, on synced $GITHUB_HEAD)"
git push github ":refs/tags/$VERSION" 2>/dev/null || true   # delete if exists
git push github "$GITHUB_HEAD:refs/tags/$VERSION"

# ---------- releases ----------
NOTES_JSON="$(python3 -c "import json,sys; print(json.dumps(sys.stdin.read()))" <<<"$NOTES")"

echo ">> create Gitea release"
GITEA_REL="$(curl -s -X POST -H "Authorization: token $GITEA_PAT" \
    -H "Content-Type: application/json" \
    "http://192.168.2.25:3000/api/v1/repos/CJ/meshcore/releases" \
    -d "{\"tag_name\":\"$VERSION\",\"name\":\"$VERSION\",\"body\":$NOTES_JSON,\"draft\":false,\"prerelease\":false}")"
GITEA_REL_ID="$(echo "$GITEA_REL" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))")"
[[ -n "$GITEA_REL_ID" ]] || { echo "error: Gitea release create failed: $GITEA_REL"; exit 1; }

echo ">> upload $BIN to Gitea release $GITEA_REL_ID"
curl -s -X POST -H "Authorization: token $GITEA_PAT" \
    "http://192.168.2.25:3000/api/v1/repos/CJ/meshcore/releases/$GITEA_REL_ID/assets?name=meshcore-$VERSION-tanmatsu.bin" \
    -F "attachment=@$BIN" > /dev/null

echo ">> create GitHub release"
GH_REL="$(curl -s -X POST -H "Authorization: token $GITHUB_PAT" \
    -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/CJvanSoest/meshcore/releases" \
    -d "{\"tag_name\":\"$VERSION\",\"name\":\"$VERSION\",\"body\":$NOTES_JSON,\"draft\":false,\"prerelease\":false}")"
GH_REL_ID="$(echo "$GH_REL" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))")"
GH_UPLOAD="$(echo "$GH_REL" | python3 -c "import sys,json; print(json.load(sys.stdin).get('upload_url','').split('{')[0])")"
[[ -n "$GH_REL_ID" ]] || { echo "error: GitHub release create failed: $GH_REL"; exit 1; }

echo ">> upload $BIN to GitHub release $GH_REL_ID"
curl -s -X POST -H "Authorization: token $GITHUB_PAT" \
    -H "Content-Type: application/octet-stream" \
    "$GH_UPLOAD?name=meshcore-$VERSION-tanmatsu.bin" \
    --data-binary @"$BIN" > /dev/null

echo
echo "✅ release $VERSION done"
echo "   Gitea:  http://192.168.2.25:3000/CJ/meshcore/releases/tag/$VERSION"
echo "   GitHub: https://github.com/CJvanSoest/meshcore/releases/tag/$VERSION"
