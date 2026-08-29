#!/usr/bin/env bash
# Bump nix-git.json to the latest NixOS/nix master commit.
set -euo pipefail

cd "$(dirname "$0")/.."
pin=nix-git.json

owner=$(jq -r .owner "$pin")
repo=$(jq -r .repo "$pin")
old_rev=$(jq -r .rev "$pin")

new_rev=$(git ls-remote "https://github.com/$owner/$repo" refs/heads/master | cut -f1)
if [[ "$new_rev" == "$old_rev" ]]; then
  echo "nix-git already at $new_rev"
  exit 0
fi

prefetch=$(nix flake prefetch --json "github:$owner/$repo/$new_rev")
hash=$(jq -r .hash <<<"$prefetch")
store_path=$(jq -r .storePath <<<"$prefetch")
last_modified=$(jq -r .locked.lastModified <<<"$prefetch")

base_version=$(tr -d '\n' <"$store_path/.version")
date=$(date -u -d "@$last_modified" +%Y%m%d)
version="${base_version}pre${date}_${new_rev:0:8}"

jq -n \
  --arg owner "$owner" --arg repo "$repo" --arg rev "$new_rev" \
  --arg hash "$hash" --arg version "$version" \
  '{owner: $owner, repo: $repo, rev: $rev, hash: $hash, version: $version}' \
  >"$pin.tmp"
mv "$pin.tmp" "$pin"

echo "nix-git: $old_rev -> $new_rev ($version)"
