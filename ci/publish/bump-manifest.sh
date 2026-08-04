#!/usr/bin/env bash
# Bump one pin in amphora-dev/content_manifest and push to main.
#
# Adapted from amphora-dev/imagefs/ci/publish/bump-manifest.sh with wine support.
#
# Required env:
#   GH_TOKEN              — PAT with write access to content_manifest
#   COMPONENT | RUNTIME_ASSET  (exactly one)
#   SHA256 / SIZE
#
# Optional: ASSET_PATH, VER_NAME, VER_CODE, REMOTE_URL, KIND, CONTENT_TYPE,
#           COMMIT_SUBJECT, BOT_NAME, BOT_EMAIL
set -euo pipefail

if [ -z "${GH_TOKEN:-}" ]; then
  echo "FAIL: GH_TOKEN not set; refusing to publish without updating content_manifest" >&2
  exit 1
fi

: "${SHA256:?SHA256 required}"
: "${SIZE:?SIZE required}"

if [ -n "${COMPONENT:-}" ] && [ -n "${RUNTIME_ASSET:-}" ]; then
  echo "FAIL: set COMPONENT or RUNTIME_ASSET, not both" >&2
  exit 1
fi
if [ -z "${COMPONENT:-}" ] && [ -z "${RUNTIME_ASSET:-}" ]; then
  echo "FAIL: one of COMPONENT / RUNTIME_ASSET is required" >&2
  exit 1
fi

BOT_NAME="${BOT_NAME:-proton-wine-bot}"
BOT_EMAIL="${BOT_EMAIL:-41898282+github-actions[bot]@users.noreply.github.com}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PIN_SKIP="$WORK/pin-skip"
export PIN_SKIP

git clone --depth 1 \
  "https://x-access-token:${GH_TOKEN}@github.com/amphora-dev/content_manifest.git" \
  "$WORK/content_manifest"
MANIFEST="$WORK/content_manifest/content_manifest.json"
test -f "$MANIFEST"

python3 - "$MANIFEST" <<'PY'
import json, os, sys

path = sys.argv[1]
component = os.environ.get("COMPONENT") or None
runtime_asset = os.environ.get("RUNTIME_ASSET") or None
sha = os.environ["SHA256"]
size = int(os.environ["SIZE"])
asset = os.environ.get("ASSET_PATH") or None
ver_name = os.environ.get("VER_NAME") or None
remote = os.environ.get("REMOTE_URL") or None
kind = os.environ.get("KIND") or None
content_type = os.environ.get("CONTENT_TYPE") or None

with open(path, encoding="utf-8") as f:
    data = json.load(f)


def skip(message):
    print(message)
    open(os.environ["PIN_SKIP"], "w").write("1")
    raise SystemExit(0)


if runtime_asset is not None:
    entries = data.get("runtimeAssets")
    if not isinstance(entries, list):
        raise SystemExit("manifest has no runtimeAssets[]")
    entry = next((e for e in entries if e.get("assetPath") == runtime_asset), None)
    if entry is None:
        raise SystemExit(f"runtimeAssets[] has no entry for {runtime_asset}")

    unchanged = entry.get("sha256") == sha and int(entry.get("size") or 0) == size
    if remote is not None:
        unchanged = unchanged and entry.get("remoteUrl") == remote
    if unchanged:
        skip(f"{runtime_asset} pin already up to date; nothing to commit")

    entry["sha256"] = sha
    entry["size"] = size
    if remote is not None:
        entry["remoteUrl"] = remote
    print(f"bumped runtimeAssets[{runtime_asset}] sha={sha} size={size}")
else:
    entry = data["components"][component]

    unchanged = entry.get("sha256") == sha and int(entry.get("size") or 0) == size
    if asset is not None:
        unchanged = unchanged and entry.get("assetPath") == asset
    if remote is not None:
        unchanged = unchanged and entry.get("remoteUrl") == remote
    if unchanged:
        skip(f"{component} pin already up to date; nothing to commit")

    entry["sha256"] = sha
    entry["size"] = size
    if asset is not None:
        entry["assetPath"] = asset
    if remote is not None:
        entry["remoteUrl"] = remote
    if kind is not None:
        entry["kind"] = kind
    if content_type is not None:
        entry["contentType"] = content_type

    if component == "wine":
        if ver_name is None:
            raise SystemExit("VER_NAME required for wine")
        entry["verName"] = ver_name
        entry["verCode"] = int(os.environ.get("VER_CODE") or 0)
        entry["version"] = f"Proton-{ver_name}-{entry['verCode']}"
        entry.setdefault("kind", "WCP")
        entry.setdefault("contentType", "Proton")
        print(f"bumped wine pin -> {entry.get('assetPath')} version={entry['version']} sha={sha}")
    elif component == "box64":
        if ver_name is None:
            raise SystemExit("VER_NAME required for box64")
        entry["verName"] = ver_name
        entry["verCode"] = int(os.environ.get("VER_CODE") or 0)
        entry["version"] = f"Box64-{ver_name}-{entry['verCode']}"
        entry.setdefault("kind", "WCP")
        entry.setdefault("contentType", "Box64")
        print(f"bumped box64 pin -> {entry.get('assetPath')} sha={sha} size={size}")
    elif component == "rootfs":
        old_ver = int(str(entry.get("version", "0")))
        entry["version"] = str(old_ver + 1)
        print(f"bumped rootfs pin v{old_ver} -> v{old_ver + 1} sha={sha} size={size}")
    else:
        if ver_name is not None:
            entry["verName"] = ver_name
            entry["version"] = ver_name
        print(f"bumped {component} pin sha={sha} size={size}")

with open(path, "w", encoding="utf-8") as f:
    json.dump(data, f, indent=2)
    f.write("\n")
PY

if [ -f "$PIN_SKIP" ]; then
  exit 0
fi

cd "$WORK/content_manifest"
python3 validate_manifest.py content_manifest.json
git config user.name "$BOT_NAME"
git config user.email "$BOT_EMAIL"
git add content_manifest.json

if [ -n "${COMMIT_SUBJECT:-}" ]; then
  SUBJECT="$COMMIT_SUBJECT"
elif [ -n "${RUNTIME_ASSET:-}" ]; then
  SUBJECT="chore: pin ${RUNTIME_ASSET##*/} ${VER_NAME:-$SHA256}"
elif [ "${COMPONENT}" = "wine" ]; then
  SUBJECT="chore: pin Proton ${VER_NAME}"
elif [ "$COMPONENT" = "box64" ]; then
  SUBJECT="chore: pin Box64 ${VER_NAME}"
elif [ "$COMPONENT" = "rootfs" ]; then
  NEW_VER="$(python3 -c 'import json;print(json.load(open("content_manifest.json"))["components"]["rootfs"]["version"])')"
  SUBJECT="chore: pin imagefs rootfs v${NEW_VER}"
else
  SUBJECT="chore: pin ${COMPONENT}"
fi

git commit -m "$(cat <<EOF
${SUBJECT}

Published by amphora-dev/proton-wine CI.
EOF
)"
git push origin HEAD:main
