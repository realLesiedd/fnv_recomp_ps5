#!/usr/bin/env bash
# Builds Fallout: New Vegas (recompiled) as a jailbroken-PS5 home-screen app.
#
#   ps5/build.sh [TRIANGLE_DIR] [DXVK_DIR]
#
# TRIANGLE_DIR: a built checkout of Nazky's ps5-plume-triangle (its `make` provides the
#               PS5 payload SDK, PS5_Mesa's RADV, the app CRT, PacBrew's SDL2 and the signer)
# DXVK_DIR:     DXVK v2.7.1 with ps5/dxvk/dxvk-ps5.patch applied, configured into build.ps5
#               (see ps5/README.md)
# gen/ must hold the C generated from the player's own FalloutNV.exe (tools/recomp.py).
# Output: build-ps5/PPSA97701 -> copy to /data/homebrew/PPSA97701 and put the game in game/.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tri=$(realpath "${1:-$root/../ext/ps5-plume-triangle}")
dxvk=$(realpath "${2:-$root/../ext/dxvk}")
title=PPSA97701
sdk=$tri/.deps/native/ps5-payload-sdk
pac=$tri/.deps/pacbrew/v0.40.2/sysroot
radv=$tri/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a
tool=$tri/build/host/ps5-native-tool
out=$root/build-ps5
obj=$out/obj
jobs=${JOBS:-$(nproc)}
for f in "$sdk/bin/prospero-lld" "$radv" "$tool" "$tri/build/obj/app_crt.o" "$tri/build/obj/app_cpp_runtime.o" \
         "$tri/build/stubs/libSceAgc.so" "$pac/user/homebrew/lib/libSDL2.a" "$dxvk/build.ps5/src/d3d9/libdxvk_d3d9.a" \
         "$root/gen/table.c"; do
  [[ -e $f ]] || { echo "missing $f" >&2; exit 2; }
done
export PS5_PAYLOAD_SDK=$sdk
cc() { sh "$tri/tooling/prospero-clang18" "$@"; }
export -f cc; export tri
mkdir -p "$obj/gen" "$obj/rt"

echo "==> [compile] recompiled game code (gen/, the long part)"
gflags="-O1 -g0 -w -fno-strict-aliasing -fwrapv -ffunction-sections -I$root/rt"
# The files in tools/hot_gen_files.txt (most of the main thread's game-code time) are built at -O2; a file taken
# off that list is rebuilt at -O1 only when it changes or its object is deleted.
hot=$root/tools/hot_gen_files.txt
ls "$root"/gen/*.c | xargs -P "$jobs" -I{} sh -c '
  o="'"$obj"'/gen/$(basename "{}" .c).o"; fl="'"$gflags"'"; dep=/dev/null
  if grep -qx "$(basename "{}")" "'"$hot"'"; then fl=$(echo "$fl" | sed "s/-O1/-O2/"); dep="'"$hot"'"; fi
  [ "$o" -nt "{}" ] && [ "$o" -nt "'"$root"'/rt/x86.h" ] && { [ "$dep" = /dev/null ] || [ "$o" -nt "$dep" ]; } ||
    sh "$tri/tooling/prospero-clang18" -c $fl "{}" -o "$o" || exit 255'

echo "==> [compile] runtime"
rflags="-O2 -g -Wall -Wno-unused-function -Wno-unknown-pragmas -Wno-unreachable-code-generic-assoc -fno-strict-aliasing -fwrapv -fno-omit-frame-pointer -I$root/rt -I$pac/user/homebrew/include -I$sdk/target/include"
for f in "$root"/rt/*.c; do
  case $(basename "$f") in platform_null.c|hostd3d9_null.c) continue;; esac
  cc -std=gnu11 $rflags -c "$f" -o "$obj/rt/$(basename "$f" .c).o"
done
python3 "$root/tools/embed_assets.py" "$root/assets" "$obj/assets_data.c"   # button prompts, built in
cc -std=gnu11 -O1 -c "$obj/assets_data.c" -o "$obj/rt/assets_data.o"
cc -std=c++17 $rflags -Wno-unused-variable -I"$dxvk/include/native/directx" -I"$dxvk/include/native/windows" -I"$dxvk/include/native" \
   -c "$root/rt/hostd3d9.cpp" -o "$obj/rt/hostd3d9.o"

echo "==> [link] game + runtime + DXVK + RADV into a PS5 app"
ln -sfn "$tri/tooling" "$out/tooling"; ln -sfn "$tri/vendor" "$out/vendor"; mkdir -p "$out/build"
source "$tri/tools/radv-link.sh"
radv_link_recipe "$out" "$sdk" "$radv"
dxvk_libs=$(find "$dxvk/build.ps5" -name "*.a" | sort)
"$sdk/bin/prospero-lld" "${radv_linker_script[@]}" --eh-frame-hdr "${radv_link_flags[@]}" \
  --version-script "$tri/tooling/native/app-symbols.map" --exclude-libs=ALL \
  -e _start -o "$out/llvm-pie.elf" \
  "$tri/build/obj/app_crt.o" "$tri/build/obj/app_cpp_runtime.o" \
  "$obj"/rt/*.o "$obj"/gen/*.o \
  --start-group $dxvk_libs "$pac/user/homebrew/lib/libSDL2.a" "$pac/user/homebrew/lib/libiconv.a" \
  "$pac/user/homebrew/lib/libavcodec.a" "$pac/user/homebrew/lib/libavutil.a" --end-group \
  "$tri/build/stubs/libSceAgc.so" "$tri/build/stubs/libSceAgcDriver.so" \
  "${radv_link_inputs[@]}" \
  --as-needed "$sdk"/target/lib/*.so

echo "==> [package] eboot.bin for $title"
"$tool" link --in "$out/llvm-pie.elf" --out "$out/eboot.elf" \
  --stub-dir "$sdk/target/lib" --stub "$tri/build/stubs/libSceAgc.so" --stub "$tri/build/stubs/libSceAgcDriver.so" \
  --module-sdk 0x02000009 --companion-sdk 0x08050001 --file-name eboot.elf
app=$out/$title
rm -rf "$app"; mkdir -p "$app/sce_sys" "$app/sce_module" "$app/game" "$app/user"
"$tool" self --sign --in "$out/eboot.elf" --out "$app/eboot.bin" --magic 0x1D3D154F
cp "$root/ps5/sce_sys/param.json" "$app/sce_sys/"
# The source param.json is left as it is. Only this copy is stamped.
# contentVersion is NN.NNN.NNN (PSDevWiki's example is 01.000.000; SharpProspero checks
# that shape and says to raise it for an update). FNV_RELEASE vN becomes 01.NNN.000.
# masterVersion stays 01.00: that field is the compatibility baseline (NN.NN),
# not the version of this build.
rel=$(sed -nE 's/^#define[[:space:]]+FNV_RELEASE[[:space:]]+"v([0-9]{1,3})".*/\1/p' "$root/rt/main.c")
case $rel in
  [0-9]|[0-9][0-9]|[0-9][0-9][0-9]) ;;
  *) echo "could not read FNV_RELEASE from $root/rt/main.c (expected \"v\" and 1 to 3 digits)" >&2; exit 2 ;;
esac
cv=$(printf '01.%03d.000' "$((10#$rel))")
param=$app/sce_sys/param.json
if ! grep -Eq '"contentVersion"[[:space:]]*:[[:space:]]*"[0-9]{2}\.[0-9]{3}\.[0-9]{3}"' "$param"; then
  echo "packaged param.json has no contentVersion in NN.NNN.NNN form" >&2
  exit 2
fi
sed -i -E 's/("contentVersion"[[:space:]]*:[[:space:]]*")[0-9]{2}\.[0-9]{3}\.[0-9]{3}"/\1'"$cv"'"/' "$param"
if ! grep -Fq "\"contentVersion\": \"$cv\"" "$param"; then
  echo "failed to write contentVersion $cv into $param" >&2
  exit 2
fi
echo "contentVersion $cv (from FNV_RELEASE v$rel); masterVersion unchanged"
cp "$root/ps5/sce_sys/icon0.png" "$app/sce_sys/"
cp "$tri/runtime/libc.prx" "$app/sce_module/"
cp "$root/LICENSE" "$root/THIRD_PARTY_NOTICES.md" "$root/README.md" "$app/"; cp -r "$root/LICENSES" "$app/"
echo "Put your New Vegas folder (FalloutNV.exe 1.4.0.525 + Data) here, then launch the app." > "$app/game/PUT_GAME_FILES_HERE.txt"
echo "Built: $app ($(du -sh "$app/eboot.bin" | cut -f1) eboot.bin)"
