#!/usr/bin/env bash
# Check a published release's assets (M3): its state, then every pair UF2
# as a release build would check it (docs/pair-uf2.md).
#
#   bash keyboards/svalboard/tools/m3/check_release.sh TAG [OWNER/REPO]
#
# Downloads into ~/m3dry/TAG (kept: the hardware check uses these files).
# An x tag must be a published prerelease; a v tag must be a full release
# (a draft until someone publishes it, D41). Any other tag is refused. Every
# release must hold exactly 2 pair UF2 files, sval and blank (D34, D40).
set -euo pipefail
tag="${1:?usage: check_release.sh TAG [OWNER/REPO]}"
R="${2:-morganvenable/sval-qmk}"
here="$(cd "$(dirname "$0")" && pwd)"
dir="$HOME/m3dry/$tag"

state="$(gh release view "$tag" -R "$R" --json isDraft,isPrerelease \
  -q '"draft=" + (.isDraft|tostring) + " prerelease=" + (.isPrerelease|tostring)')"
echo "$tag: $state"
pre="${state##*prerelease=}"
if [[ "$tag" =~ ^x[0-9]+(\.[0-9]+)*$ ]]; then
  case "$state" in *draft=true*) echo "FAIL: an x tag's release is still a draft"; exit 1 ;; esac
  [ "$pre" = true ] || { echo "FAIL: an x tag must be a prerelease"; exit 1; }
elif [[ "$tag" =~ ^v[0-9]+(\.[0-9]+)*$ ]]; then
  [ "$pre" = false ] || { echo "FAIL: a v tag must not be a prerelease"; exit 1; }
  case "$state" in *draft=true*) echo "note: still a draft: publish it after this check" ;; esac
else
  echo "FAIL: $tag is not a release tag (v3, v3.1, x3, x3.1)"
  exit 1
fi
rm -rf "$dir"
mkdir -p "$dir"
gh release download "$tag" -R "$R" -D "$dir"
python3 -I "$here/check_release_assets.py" "$dir" "$tag"
