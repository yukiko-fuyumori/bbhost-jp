#!/bin/bash
# Shared payload for both release bundles; retain every supplied artifact.
set -eu
cd "$(dirname "$0")/.."
dest=${1:?usage: tools/package_japanese.sh PACKAGE_DIR}
(cd compat/jp-eu-v020 && sha256sum -c SHA256SUMS.txt >/dev/null)
mkdir -p "$dest/compat" "$dest/licenses/mplus"
cp -a compat/jp-eu-v020 "$dest/compat/"
cp third_party/mplus/LICENSE "$dest/licenses/mplus/LICENSE"
cp docs/japanese-release.md "$dest/導入手順_日本語.md"
