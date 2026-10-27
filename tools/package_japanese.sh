#!/bin/bash
# Shared payload for both release bundles; retain every supplied artifact.
set -eu
cd "$(dirname "$0")/.."
dest=${1:?usage: tools/package_japanese.sh PACKAGE_DIR BUILD_DIR}
build_dir=${2:?build directory required}
profile=$(cat "$build_dir/bbhost-edition.txt")
case "$profile" in jp-only|eu-personal) ;; *) echo "invalid edition profile: $profile" >&2; exit 1;; esac
exe=$dest/bbhost; plugin=$dest/plugins/region_switch.so
if [ -f "$dest/bbhost.exe" ]; then
    [ "$profile" = jp-only ] || { echo "Windows must be Japanese-only" >&2; exit 1; }
    exe=$dest/bbhost.exe; plugin=$dest/plugins/region_switch.dll
fi
strings -a "$exe" | grep -qxF "BBHOST_EDITION_PROFILE=$profile" || { echo "executable edition does not match build profile" >&2; exit 1; }
strings -a "$plugin" | grep -qxF "BBHOST_REGION_PLUGIN_PROFILE=$profile" || { echo "plugin edition does not match build profile" >&2; exit 1; }
cp "$build_dir/bbhost-edition.txt" "$dest/edition.txt"
(cd compat/jp-eu-v020 && sha256sum -c SHA256SUMS.txt >/dev/null)
mkdir -p "$dest/compat" "$dest/licenses/mplus"
cp -a compat/jp-eu-v020 "$dest/compat/"
cp third_party/mplus/LICENSE "$dest/licenses/mplus/LICENSE"
guide=docs/japanese-release.md
[ "$profile" = jp-only ] || guide=docs/personal-eu-release.md
cp "$guide" "$dest/導入手順_日本語.md"

cp docs/trophies.md "$dest/トロフィー.md"
cp docs/online-diagnostics.md "$dest/オンライン診断.md"
