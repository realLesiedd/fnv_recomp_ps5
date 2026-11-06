#!/usr/bin/env bash
# Zip a built PS5 app for a release.
#
#   tools/package_release.sh [APP_DIR]
#
# APP_DIR defaults to build-ps5/PPSA97701, the folder ps5/build.sh writes.
# The zip is written next to that folder as <title id>-<FNV_RELEASE>.zip.
# FNV_RELEASE is read from rt/main.c.
#
# ps5/build.sh copies LICENSE, LICENSES/, THIRD_PARTY_NOTICES.md and README.md
# from the repo root into the app folder. This script takes those copies from
# the app folder. It packs only the app: eboot.bin, sce_sys/, sce_module/, the
# license files, README.md, game/PUT_GAME_FILES_HERE.txt and an empty user/.
# Anything else in the app folder is left out. game/ with anything besides the
# placeholder, or a user/ that is not empty, stops the zip so game files and
# saves cannot be published.
set -euo pipefail

die() { echo "package_release: $*" >&2; exit 1; }

if [[ $# -gt 1 ]]; then
  die "usage: tools/package_release.sh [APP_DIR]"
fi

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
app=${1:-$root/build-ps5/PPSA97701}
[[ -d $app ]] || die "no app folder at $app"
app=$(cd -- "$app" && pwd)
title=$(basename -- "$app")

mainc=$root/rt/main.c
version=$(sed -nE 's/^#define[[:space:]]+FNV_RELEASE[[:space:]]+"(v[0-9]{1,3})".*/\1/p' "$mainc")
case $version in
  v[0-9]|v[0-9][0-9]|v[0-9][0-9][0-9]) ;;
  *) die "could not read FNV_RELEASE from $mainc (expected \"v\" and 1 to 3 digits)" ;;
esac
num=${version#v}
cv=$(printf '01.%03d.000' "$((10#$num))")

[[ -d $app/game ]] || die "missing game/ in $app"
[[ -d $app/user ]] || die "missing user/ in $app"

shopt -s nullglob dotglob
game_bad=()
saw_note=0
for p in "$app/game"/*; do
  name=$(basename -- "$p")
  if [[ $name == PUT_GAME_FILES_HERE.txt && -f $p && ! -L $p ]]; then
    saw_note=1
    continue
  fi
  game_bad+=("$name")
done
user_bad=()
for p in "$app/user"/*; do
  user_bad+=("$(basename -- "$p")")
done
mod=("$app/sce_module"/*)
lic=("$app/LICENSES"/*)
shopt -u nullglob dotglob

if ((${#game_bad[@]} > 0)); then
  die "refusing to zip: game/ contains more than PUT_GAME_FILES_HERE.txt: ${game_bad[*]}"
fi
if [[ $saw_note -ne 1 ]]; then
  die "refusing to zip: game/ is missing PUT_GAME_FILES_HERE.txt"
fi
if ((${#user_bad[@]} > 0)); then
  die "refusing to zip: user/ is not empty: ${user_bad[*]}"
fi

[[ -f $app/eboot.bin && ! -L $app/eboot.bin ]] || die "missing eboot.bin"
[[ -d $app/sce_sys && ! -L $app/sce_sys ]] || die "missing sce_sys/"
[[ -f $app/sce_sys/param.json && ! -L $app/sce_sys/param.json ]] || die "missing sce_sys/param.json"
[[ -d $app/sce_module && ! -L $app/sce_module ]] || die "missing sce_module/"
((${#mod[@]} > 0)) || die "sce_module/ is empty"
[[ -f $app/LICENSE && ! -L $app/LICENSE ]] || die "missing LICENSE (ps5/build.sh copies it from the repo root into the app folder)"
[[ -d $app/LICENSES && ! -L $app/LICENSES ]] || die "missing LICENSES/ (ps5/build.sh copies it from the repo root into the app folder)"
((${#lic[@]} > 0)) || die "LICENSES/ is empty"
[[ -f $app/THIRD_PARTY_NOTICES.md && ! -L $app/THIRD_PARTY_NOTICES.md ]] || die "missing THIRD_PARTY_NOTICES.md (ps5/build.sh copies it from the repo root into the app folder)"
[[ -f $app/README.md && ! -L $app/README.md ]] || die "missing README.md (ps5/build.sh copies it from the repo root into the app folder)"

got=$(sed -nE 's/.*"contentVersion"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' "$app/sce_sys/param.json" | tr -d '\r')
if [[ $got != "$cv" ]]; then
  die "refusing to zip: sce_sys/param.json contentVersion is '${got:-missing}', expected $cv from FNV_RELEASE $version. Rebuild with ps5/build.sh so the packaged param.json is stamped."
fi

command -v zip >/dev/null 2>&1 || die "need the zip command"

out=$(cd -- "$(dirname -- "$app")" && pwd)/${title}-${version}.zip
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dest=$stage/$title
mkdir -p "$dest/sce_sys" "$dest/sce_module" "$dest/LICENSES" "$dest/game" "$dest/user"
cp -p "$app/eboot.bin" "$app/LICENSE" "$app/THIRD_PARTY_NOTICES.md" "$app/README.md" "$dest/"
cp -a "$app/sce_sys/." "$dest/sce_sys/"
cp -a "$app/sce_module/." "$dest/sce_module/"
cp -a "$app/LICENSES/." "$dest/LICENSES/"
cp -p "$app/game/PUT_GAME_FILES_HERE.txt" "$dest/game/"

tmpzip=$stage/out.zip
( cd -- "$stage" && zip -r -q "$tmpzip" "$title" )
mv -f "$tmpzip" "$out"

bytes=$(wc -c < "$out" | tr -d '[:space:]')
if command -v sha256sum >/dev/null 2>&1; then
  digest=$(sha256sum -- "$out" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
  digest=$(shasum -a 256 -- "$out" | awk '{print $1}')
else
  die "need sha256sum or shasum to print SHA-256"
fi

echo "$out"
echo "size: $bytes bytes"
echo "sha256: $digest"
