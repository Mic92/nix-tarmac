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

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
store="local?root=$work/store"

run_eval() { # args: cache-dir plugin-args...
  local cache=$1
  shift
  XDG_CACHE_HOME=$cache "$nixbin" eval --store "$store" \
    --extra-experimental-features 'nix-command flakes' \
    --impure --raw "$@" >/dev/null
}

bench() { # args: label cache-dir reset(0/1) extra-args...
  local label=$1 cache=$2 reset=$3
  shift 3
  local total=0 t0 t1 dt
  for ((i = 0; i < runs; i++)); do
    [[ $reset == 1 ]] && rm -rf "$cache"
    t0=$(date +%s%N)
    run_eval "$cache" "$@"
    t1=$(date +%s%N)
    dt=$(((t1 - t0) / 1000000))
    total=$((total + dt))
  done
  printf '%-28s %6d ms\n' "$label" "$((total / runs))"
}

echo "== tarball: $tarball ($(du -h "$tarball" | cut -f1)), nix $version, $runs runs =="

bench "cold ingest builtin" "$work/b" 1 --expr "$expr"
bench "cold ingest plugin" "$work/p" 1 --plugin-files "$plugin" --expr "$expr"

run_eval "$work/b" --expr "$expr"
run_eval "$work/p" --plugin-files "$plugin" --expr "$expr"
bench "warm eval builtin" "$work/b" 0 --expr "$expr"
bench "warm eval plugin" "$work/p" 0 --plugin-files "$plugin" --expr "$expr"

fxexpr="(import (builtins.fetchTree { type = \"tarball\"; url = \"file://$tarball\"; }).outPath { config = { }; overlays = [ ]; }).firefox.drvPath"
bench "firefox eval builtin" "$work/b" 0 --expr "$fxexpr"
bench "firefox eval plugin" "$work/p" 0 --plugin-files "$plugin" --expr "$fxexpr"

for who in builtin plugin; do
  args=()
  cache=$work/b
  [[ $who == plugin ]] && {
    args=(--plugin-files "$plugin")
    cache=$work/p
  }
  total=0
  path=
  for ((i = 0; i < runs; i++)); do
    [[ -n $path ]] && "$nixbin" store delete --store "$store" "$path" >/dev/null 2>&1
    t0=$(date +%s%N)
    path=$(XDG_CACHE_HOME=$cache "$nixbin" eval --store "$store" \
      --extra-experimental-features 'nix-command flakes' \
      --impure --raw "${args[@]}" --expr "$outexpr")
    t1=$(date +%s%N)
    total=$((total + (t1 - t0) / 1000000))
  done
  printf '%-28s %6d ms\n' "materialize $who" "$((total / runs))"
done
