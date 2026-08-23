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

# eval store: drvs land in the pack CAS, building copies them out to
# the real store on demand
nixbin=$(nix build --no-link --print-out-paths "nixpkgs#nixVersions.nix_2_35.out")/bin/nix
plugin=$(nix build --no-link --print-out-paths "$repo#plugin-nix_2_35")/lib/nix/plugins
export XDG_CACHE_HOME="$work/cache-evalstore"
evalstore="tarmac://$work/evalstore"
buildexpr="derivation {
  name = \"tarmac-e2e\";
  system = builtins.currentSystem;
  builder = \"/bin/sh\";
  args = [ \"-c\" \"echo ok > \$out\" ];
  passthru.note = builtins.toFile \"note.txt\" \"hello\";
}"
common=(--extra-experimental-features 'nix-command flakes' --impure
  --plugin-files "$plugin" --eval-store "$evalstore")

drv=$("$nixbin" eval "${common[@]}" --raw --expr "($buildexpr).drvPath")
[[ -e $work/evalstore/pack.0 ]] || {
  echo "FAIL evalstore: store not populated" >&2
  exit 1
}
[[ -e $drv ]] && {
  echo "FAIL evalstore: drv leaked into /nix/store" >&2
  exit 1
}
# hash-part lookup must find the drv in the meta table
"$nixbin" path-info "${common[@]}" "${drv:11:32}" >/dev/null || {
  echo "FAIL evalstore: queryPathFromHashPart" >&2
  exit 1
}
out=$("$nixbin" build "${common[@]}" --no-link --print-out-paths --expr "$buildexpr")
[[ $(cat "$out") == ok ]] || {
  echo "FAIL evalstore: build output mismatch" >&2
  exit 1
}
echo "OK evalstore: eval + build via $evalstore"
