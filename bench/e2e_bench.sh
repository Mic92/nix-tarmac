#!/usr/bin/env bash
# builtin vs plugin: cold ingest, warm eval, store-path materialization
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
tarball=${1:-/tmp/casbench-tarballs/nixpkgs-a.tar.gz}
version=${2:-nix_2_35}
runs=${RUNS:-3}

nixbin=$(nix build --no-link --print-out-paths "nixpkgs#nixVersions.$version.out")/bin/nix
plugin=$(nix build --no-link --print-out-paths "$repo#plugin-$version")/lib/nix/plugins
expr="(builtins.fetchTree { type = \"tarball\"; url = \"file://$tarball\"; }).narHash"
outexpr="(builtins.fetchTree { type = \"tarball\"; url = \"file://$tarball\"; }).outPath"
fxexpr="(import (builtins.fetchTree { type = \"tarball\"; url = \"file://$tarball\"; }).outPath { config = { }; overlays = [ ]; }).firefox.drvPath"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

run_eval() { # args: cache-dir plugin-args...
  local cache=$1
  shift
  XDG_CACHE_HOME=$cache "$nixbin" eval --store "local?root=$work/store" \
    --extra-experimental-features 'nix-command flakes' \
    --impure --raw "$@" >/dev/null
}

bench() { # args: label cache-dir reset(cache|store|-) extra-args...
  local label=$1 cache=$2 reset=$3
  shift 3
  local total=0 t0
  for ((i = 0; i < runs; i++)); do
    [[ $reset == cache ]] && rm -rf "$cache"
    [[ $reset == store ]] && rm -rf "$work/store"
    t0=$(date +%s%N)
    run_eval "$cache" "$@"
    total=$((total + ($(date +%s%N) - t0) / 1000000))
  done
  printf '%-28s %6d ms\n' "$label" "$((total / runs))"
}

echo "== tarball: $tarball ($(du -h "$tarball" | cut -f1)), nix $version, $runs runs =="

bench "cold ingest builtin" "$work/b" cache --plugin-files "" --expr "$expr"
bench "cold ingest plugin" "$work/p" cache --plugin-files "$plugin" --expr "$expr"

bench "warm eval builtin" "$work/b" - --plugin-files "" --expr "$expr"
bench "warm eval plugin" "$work/p" - --plugin-files "$plugin" --expr "$expr"

bench "firefox eval builtin" "$work/b" - --plugin-files "" --expr "$fxexpr"
bench "firefox eval plugin" "$work/p" - --plugin-files "$plugin" --expr "$fxexpr"

bench "materialize builtin" "$work/b" store --plugin-files "" --expr "$outexpr"
bench "materialize plugin" "$work/p" store --plugin-files "$plugin" --expr "$outexpr"
