#!/usr/bin/env bash
# builtin vs plugin: fetching nixpkgs channel tarballs over the network.
# Channels are published as both xz- and zstd-compressed tarballs; zstd
# decompresses fast enough to overlap with the download, but the builtin
# fetcher's per-file import cost hides that. This measures how much of
# the zstd advantage the plugin recovers.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
channel=${1:-nixpkgs-unstable}
version=${2:-nix_2_35}
runs=${RUNS:-3}

nixbin=$(nix build --no-link --print-out-paths "nixpkgs#nixVersions.$version.out")/bin/nix
plugin=$(nix build --no-link --print-out-paths "$repo#plugin-$version")/lib/nix/plugins

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() { # args: cache-dir url plugin-args...
  local cache=$1 url=$2
  shift 2
  XDG_CACHE_HOME=$cache "$nixbin" eval \
    --extra-experimental-features 'nix-command flakes' \
    --option tarball-ttl 0 --impure --raw "$@" \
    --expr "(builtins.fetchTree { type = \"tarball\"; url = \"$url\"; }).narHash" \
    >/dev/null
}

bench() { # args: label cache-dir base-url extra-args...
  local label=$1 cache=$2 base=$3
  shift 3
  # prime the content cache so runs measure download + lookup,
  # not the one-time cold import
  fetch "$cache" "$base?bust=prime$RANDOM" "$@"
  local total=0 t0
  for ((i = 0; i < runs; i++)); do
    t0=$(date +%s%N)
    fetch "$cache" "$base?bust=$RANDOM$RANDOM" "$@"
    total=$((total + ($(date +%s%N) - t0) / 1000000))
  done
  printf '%-28s %6d ms\n' "$label" "$((total / runs))"
}

echo "== channel: $channel, nix $version, $runs runs (warm cache, fresh download) =="

for ext in zst xz; do
  url="https://channels.nixos.org/$channel/nixexprs.tar.$ext"
  bench "builtin tar.$ext" "$work/b" "$url" --plugin-files ""
  bench "plugin  tar.$ext" "$work/p" "$url" --plugin-files "$plugin"
done
