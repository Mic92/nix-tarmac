# nix-tarmac

Nix plugin that speeds up fetching of tarball flake inputs.

Nix unpacks every tarball input (nixpkgs, flake-utils, anything pinned to a `.tar.gz` URL) into its Git cache.
Git uses one zlib-compressed object per file, SHA-1 hashed and written one at a time.
For nixpkgs that's 50k+ files per revision.
The cache is never garbage collected.

nix-tarmac replaces the builtin tarball fetcher with
an [LMDB](https://en.wikipedia.org/wiki/Lightning_Memory-Mapped_Database)-indexed
append-only packfile, BLAKE3 hashes, and mmap-based reads.
Results are byte-identical (same `narHash`), so lock files keep working.

Fetching a nixpkgs tarball (51 MB, ~53k files) with Nix 2.35 on a
ZFS-backed server, average of 3 runs (`bench/e2e_bench.sh`):

| | builtin | nix-tarmac |
|---|---|---|
| first fetch (unpack + hash) | 11.4 s | 3.1 s |
| every eval after that | 44 ms | 31 ms |
| `firefox.drvPath` eval, warm | 3.1 s | 2.3 s |
| re-import into /nix/store | 10.5 s | 5.9 s |

The tarball comes from a local file, so the first-fetch row excludes
network time. With a real download the unpack and hash work overlaps
the transfer. The warm eval overhead is paid on every evaluation of
every tarball input.
Files shared between revisions are stored once: two nixpkgs
revisions 200 commits apart take 93 MB instead of 453 MB.

Over the network it also matters which compression the tarball uses.
nixpkgs channels are published as both xz- and zstd-compressed
tarballs (`nixexprs.tar.zst`); zstd decompresses fast enough to fully
overlap with the download, but the builtin fetcher's per-file import
cost hides most of that. Fetching nixpkgs-unstable from
channels.nixos.org (server in a datacenter, warm content cache, fresh
download each run, average of 3 runs, `bench/network_bench.sh`):

| | builtin | nix-tarmac |
|---|---|---|
| `channels.nixos.org/nixpkgs-unstable/nixexprs.tar.zst` | 3.1 s | 2.0 s |
| `channels.nixos.org/nixpkgs-unstable/nixexprs.tar.xz` | 3.6 s | 2.9 s |
| `github.com/NixOS/nixpkgs/archive/nixpkgs-unstable.tar.gz` | 3.9 s | 3.1 s |

## Installation

On NixOS, add the flake input and import the module:

```nix
inputs.nix-tarmac.url = "github:Mic92/nix-tarmac";
# in your configuration:
imports = [ inputs.nix-tarmac.nixosModules.default ];
```

For nix-darwin use `inputs.nix-tarmac.darwinModules.default`.

For a manual setup:

```sh
nix build github:Mic92/nix-tarmac
```

Add to `nix.conf`:

```
plugin-files = /path/to/result/lib/nix/plugins/nix-tarmac-loader.so
```

or pass `--plugin-files` on the command line.

The cache lives in `~/.cache/nix/tarmac`.
Inputs unused for 30 days are garbage collected automatically.

## Supported Nix versions

Nix 2.34+. The plugin is built against all versions provided by the pinned nixpkgs of the flake.
The plugin loader will match at runtime time a compatible version or skip with a warning.

## Development

```sh
nix develop -c meson setup build
nix develop -c ninja -C build
nix develop -c meson test -C build
./tests/e2e.sh        # fetch with real Nix, compare narHash against builtin
./bench/e2e_bench.sh  # builtin vs plugin benchmark
```

## License

MIT
