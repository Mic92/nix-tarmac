#!/usr/bin/env bash
# fetch a tarball flake input with the builtin fetcher and with the plugin;
# narHash must match and the plugin must actually populate its store
set -euo pipefail

repo=$(cd "$(dirname "$0")/.." && pwd)
system=$(nix eval --raw --impure --expr builtins.currentSystem)
if [[ $# -gt 0 ]]; then
  versions=$*
else
  versions=$(nix eval --json "$repo#packages.$system" --apply builtins.attrNames |
    jq -r '.[] | select(startswith("plugin-")) | ltrimstr("plugin-") | select(. != "dispatcher")')
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/src/sub"
echo hello >"$work/src/file.txt"
echo world >"$work/src/sub/nested.txt"
ln -s file.txt "$work/src/link"
tar -C "$work" -czf "$work/src.tar.gz" src
url="file://$work/src.tar.gz"
expr="(builtins.fetchTree { type = \"tarball\"; url = \"$url\"; }).narHash"

for v in $versions; do
  nixbin=$(nix build --no-link --print-out-paths "nixpkgs#nixVersions.$v.out")/bin/nix
  plugin=$(nix build --no-link --print-out-paths "$repo#plugin-$v")/lib/nix/plugins
  common=(--extra-experimental-features 'nix-command flakes' --impure --raw)

  export XDG_CACHE_HOME="$work/cache-builtin-$v"
  builtin_hash=$("$nixbin" eval "${common[@]}" --expr "$expr")

  export XDG_CACHE_HOME="$work/cache-plugin-$v"
  plugin_hash=$("$nixbin" eval "${common[@]}" --plugin-files "$plugin" --expr "$expr")

  [[ -e "$XDG_CACHE_HOME/nix/tarmac/pack.0" ]] || {
    echo "FAIL $v: plugin store not populated" >&2
    exit 1
  }
  [[ $builtin_hash == "$plugin_hash" ]] || {
    echo "FAIL $v: narHash mismatch: builtin=$builtin_hash plugin=$plugin_hash" >&2
    exit 1
  }

  # warm hit: served from the tarmac store without touching the tarball
  mv "$work/src.tar.gz" "$work/src.tar.gz.hidden"
  hit_hash=$("$nixbin" eval "${common[@]}" --plugin-files "$plugin" --expr "$expr")
  mv "$work/src.tar.gz.hidden" "$work/src.tar.gz"
  [[ $hit_hash == "$builtin_hash" ]] || {
    echo "FAIL $v: cache hit narHash mismatch" >&2
    exit 1
  }

  echo "OK $v: $builtin_hash"
done

# unsupported Nix: the dispatcher must fall back to the builtin fetcher
nixbin=$(nix build --no-link --print-out-paths "nixpkgs#nixVersions.nix_2_31.out")/bin/nix
plugin=$(nix build --no-link --print-out-paths "$repo#plugin-dispatcher")/lib/nix/plugins
export XDG_CACHE_HOME="$work/cache-degrade"
ref_hash=$("$nixbin" eval --extra-experimental-features 'nix-command flakes' \
  --impure --raw --expr "$expr")
out=$("$nixbin" eval --extra-experimental-features 'nix-command flakes' \
  --impure --raw --plugin-files "$plugin" --expr "$expr" 2>"$work/degrade.log")
grep -q 'falling back to the builtin fetcher' "$work/degrade.log" || {
  echo "FAIL degrade: expected fallback warning" >&2
  exit 1
}
[[ $out == "$ref_hash" ]] || {
  echo "FAIL degrade: narHash mismatch" >&2
  exit 1
}
echo "OK degrade: builtin fallback on nix_2_31"
