#!/usr/bin/env bash
# Create or refresh a fixed-tag GitHub Release and upload assets (clobber).
#
# Usage:
#   ci/publish/fixed-release.sh \
#     --tag TAG --title TITLE --body TEXT [--latest] [--] FILE [FILE...]
#
# Env:
#   GH_TOKEN     required
#   REPO         owner/name (default: from gh)
#   MAKE_LATEST  true|false (default false; --latest sets true)
#   TARGET_REF   commitish for new tags (default: GITHUB_SHA or HEAD)
set -euo pipefail

TAG=""
NAME=""
BODY=""
MAKE_LATEST="${MAKE_LATEST:-false}"
REPO="${REPO:-}"
TARGET_REF="${TARGET_REF:-${GITHUB_SHA:-HEAD}}"
FILES=()

while [ $# -gt 0 ]; do
  case "$1" in
    --tag) TAG="${2:?}"; shift 2 ;;
    --name|--title) NAME="${2:?}"; shift 2 ;;
    --body) BODY="${2:?}"; shift 2 ;;
    --latest) MAKE_LATEST=true; shift ;;
    --files) shift; FILES+=("$@"); break ;;
    --) shift; FILES+=("$@"); break ;;
    -*) echo "unknown flag: $1" >&2; exit 2 ;;
    *) FILES+=("$1"); shift ;;
  esac
done

: "${TAG:?--tag required}"
: "${NAME:?--title/--name required}"
: "${GH_TOKEN:?GH_TOKEN required}"
[ "${#FILES[@]}" -gt 0 ] || { echo "no files to upload" >&2; exit 2; }

if [ -z "$REPO" ]; then
  REPO="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
fi

shopt -s nullglob
EXPANDED=()
for f in "${FILES[@]}"; do
  # shellcheck disable=SC2206
  matches=($f)
  if [ ${#matches[@]} -eq 0 ]; then
    echo "missing file: $f" >&2
    exit 1
  fi
  EXPANDED+=("${matches[@]}")
done
FILES=("${EXPANDED[@]}")
shopt -u nullglob

for f in "${FILES[@]}"; do
  [ -f "$f" ] || { echo "missing file: $f" >&2; exit 1; }
done

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  gh release edit "$TAG" --repo "$REPO" --title "$NAME" --notes "$BODY"
else
  gh release create "$TAG" --repo "$REPO" --title "$NAME" --notes "$BODY" \
    --target "$TARGET_REF"
fi

gh release upload "$TAG" "${FILES[@]}" --repo "$REPO" --clobber

if [ "$MAKE_LATEST" = "true" ]; then
  gh release edit "$TAG" --repo "$REPO" --latest
fi

echo "Published $REPO@$TAG ← ${FILES[*]}"
