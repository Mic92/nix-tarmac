#!/usr/bin/env bash
# usage: store_add.sh NIX PLUGIN_DIR WORK_DIR
# The evaluator adds files to a tarmac:// store through addToStoreFromDump:
# builtins.toFile and a non-recursive builtins.path as flat dumps, a
# recursive builtins.path as a NAR. Each store path is the content address of
# what the plugin restored into memory, so it must equal the path a read-only
# evaluation computes, and the flat files must read back.
set -euo pipefail

nixbin=$1
plugin=$2
work=$3

mkdir -p "$work/tree/sub"
printf 'flat contents\n' >"$work/flat.txt"
printf 'nested\n' >"$work/tree/sub/data"
printf '#!/bin/sh\n' >"$work/tree/run"
chmod +x "$work/tree/run"
ln -s sub/data "$work/tree/link"

common=(--extra-experimental-features nix-command)
tarmac=("${common[@]}" --plugin-files "$plugin" --store "tarmac://$work/store")
evalargs=(--impure --raw)

declare -A exprs=(
  [toFile]='builtins.toFile "text.txt" "text contents\n"'
  [flat]="builtins.path { path = $work/flat.txt; recursive = false; }"
  [nar]="builtins.path { path = $work/tree; }"
)
declare -A contents=([toFile]='text contents' [flat]='flat contents')

for kind in toFile flat nar; do
  want=$("$nixbin" eval "${common[@]}" "${evalargs[@]}" --read-only --expr "${exprs[$kind]}")
  got=$("$nixbin" eval "${tarmac[@]}" "${evalargs[@]}" --expr "${exprs[$kind]}")
  [[ $got == "$want" ]] || {
    echo "FAIL $kind: tarmac=$got nix=$want" >&2
    exit 1
  }
  if [[ -n ${contents[$kind]:-} ]]; then
    [[ $("$nixbin" store cat "${tarmac[@]}" "$got") == "${contents[$kind]}" ]] || {
      echo "FAIL $kind: $got did not read back" >&2
      exit 1
    }
  fi
  echo "OK $kind: $got"
done
