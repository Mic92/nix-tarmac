#!/usr/bin/env bash
# real /nix/store vs tarmac:// as --eval-store: nix-eval-jobs over the
# NixOS release-small jobset. The tarmac store skips SQLite
# registration and store I/O for eval outputs, and a warm store turns
# repeated drv writes into LMDB point reads.
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
workers=${WORKERS:-4}

nej=$(nix build --no-link --print-out-paths --inputs-from "$repo" nixpkgs#nix-eval-jobs)/bin/nix-eval-jobs
plugin=$(nix build --no-link --print-out-paths "$repo#plugin-nix_2_35")/lib/nix/plugins
nixpkgs=$(nix flake metadata --inputs-from "$repo" nixpkgs --json | jq -r .path)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

bench() { # args: label extra-args...
  local label=$1
  shift
  local t0
  t0=$(date +%s%N)
  "$nej" --workers "$workers" --force-recurse \
    --gc-roots-dir "$work/roots" "$@" \
    "$nixpkgs/nixos/release-small.nix" >"$work/jobs.jsonl" 2>/dev/null
  local ms=$((($(date +%s%N) - t0) / 1000000))
  local outpaths
  outpaths=$(jq -r 'select(.error == null) | .outputs[]?' "$work/jobs.jsonl" |
    sort -u | wc -l)
  printf '%-24s %6d ms   %s outpaths\n' "$label" "$ms" "$outpaths"
}

tarmac=(--option plugin-files "$plugin" --eval-store "tarmac://$work/evalstore")

bench "/nix/store"
bench "tarmac cold" "${tarmac[@]}"
bench "tarmac warm" "${tarmac[@]}"
du -sh "$work/evalstore" | awk '{print "store size: " $1}'
