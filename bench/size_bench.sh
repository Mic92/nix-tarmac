#!/usr/bin/env bash
# On-disk cache size, builtin git tarball cache vs plugin, after ingesting
# two (ideally nearby) revisions of the same tree. Reports logical bytes so
# the result does not depend on filesystem compression (ZFS, btrfs).
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
if [[ $# -lt 2 ]]; then
  echo "usage: $0 first.tar.gz second.tar.gz [nix_version]" >&2
  exit 1
fi
version=${3:-nix_2_35}

nixbin=$(nix build --no-link --print-out-paths "nixpkgs#nixVersions.$version.out")/bin/nix
plugin=$(nix build --no-link --print-out-paths "$repo#plugin-$version")/lib/nix/plugins

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() { # args: cache-dir plugin-files tarball
  XDG_CACHE_HOME=$1 "$nixbin" eval --store "local?root=$work/store" \
    --extra-experimental-features 'nix-command flakes' \
    --impure --raw --plugin-files "$2" \
    --expr "(builtins.fetchTree { type = \"tarball\"; url = \"file://$3\"; }).narHash" >/dev/null 2>&1
}

mib() { # logical size of the given paths in MiB
  du -smc --apparent-size "$@" | tail -1 | cut -f1
}

builtin_size() { mib "$work/b/nix/tarball-cache-v2/objects"; }

# LMDB's data.mdb is a sparse file the size of the map (32 GiB), so count its
# allocated blocks instead of its length. The pack file itself is not sparse.
plugin_size() {
  local pack index
  pack=$(mib "$work/p/nix/tarmac"/pack.*)
  index=$(du -sm "$work/p/nix/tarmac/index/data.mdb" | cut -f1)
  echo $((pack + index))
}

unpacked_size() {
  local dir=$work/unpacked
  rm -rf "$dir" && mkdir -p "$dir"
  tar -xf "$1" -C "$dir"
  mib "$dir"
}

printf '%-12s %14s %14s %14s\n' "" "unpacked" "builtin" "nix-tarmac"
for tarball in "$1" "$2"; do
  tarball=$(realpath "$tarball")
  fetch "$work/b" "" "$tarball"
  fetch "$work/p" "$plugin" "$tarball"
  printf '%-12s %11s MiB %11s MiB %11s MiB\n' \
    "+$(basename "$tarball")" "$(unpacked_size "$tarball")" "$(builtin_size)" "$(plugin_size)"
done
echo "(unpacked column is per tarball; cache columns are cumulative)"
