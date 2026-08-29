# nix-tarmac

Nix plugin with a content-addressed cache that speeds up two things:
fetching tarball flake inputs, and evaluation via a persistent
`tarmac://` eval store.

## Tarball fetcher

Nix unpacks every tarball input (nixpkgs, flake-utils, anything pinned to a `.tar.gz` URL) into a bare Git repository (`~/.cache/nix/tarball-cache-v2`).
Each file is SHA-1 hashed and zlib-compressed.
For nixpkgs that's 50k+ files per revision.
The cache is never garbage collected or repacked.

nix-tarmac replaces the builtin tarball fetcher with
an [LMDB](https://en.wikipedia.org/wiki/Lightning_Memory-Mapped_Database)-indexed
append-only packfile of LZ4-compressed blobs, BLAKE3 hashes, and mmap-based reads.
Results are byte-identical (same `narHash`), so lock files keep working.

Fetching a nixpkgs tarball (51 MB, ~53k files) with Nix 2.35 on a
ZFS-backed server, average of 3 runs (`bench/e2e_bench.sh`):

| | builtin | nix-tarmac |
|---|---|---|
| first fetch (unpack + hash) | 10.4 s | 2.4 s |
| `firefox.drvPath` eval, warm | 3.3 s | 2.4 s |
| re-import into /nix/store | 10.3 s | 8.8 s |

The tarball comes from a local file, so the first-fetch row excludes
network time. With a real download the unpack and hash work overlaps
the transfer.
Files shared between revisions are stored once.
One nixpkgs revision takes 102 MiB, a second one 200 commits later adds 5 MiB (`bench/size_bench.sh`).
Git needs 69 MiB for the same two revisions because zlib compresses better than LZ4.

Over the network it also matters which compression the tarball uses.
nixpkgs channels are published as both xz- and zstd-compressed
tarballs. zstd decompresses fast enough to fully overlap with the
download, but the builtin fetcher's per-file import cost hides most of
that. Fetching `channels.nixos.org/nixpkgs-unstable/nixexprs.tar.*`
from a datacenter server, fresh download each run, average of 3 runs
(`bench/network_bench.sh`). "First fetch" starts with a cold cache.
"Re-fetch" hits the content cache and only re-downloads:

| | builtin | nix-tarmac |
|---|---|---|
| `tar.zst`, first fetch | 10.2 s | 2.4 s |
| `tar.zst`, re-fetch | 1.6 s | **1.3 s** |
| `tar.xz`, first fetch | 10.6 s | 3.0 s |
| `tar.xz`, re-fetch | 2.3 s | 2.3 s |

With the plugin a cold fetch costs barely more than a re-fetch: ingest
and a single hash pass overlap the download.

## The cache

Everything lives in one content-addressed packfile in
`~/.cache/nix/tarmac`, shared by the fetcher and the eval store.
Identical content is stored once, no matter which feature wrote it.
Entries unused for 30 days are garbage collected automatically. If
the cache is detected as corrupt it is wiped and everything
regenerates on the next run.

## Eval store

The plugin also registers a `tarmac://` store scheme, meant as an
`--eval-store`. It keeps evaluation outputs (derivations, `writeText`
files) in the same packfile format instead of writing them to
/nix/store with SQLite registration. Unlike `dummy://` it is
persistent, so restarted evaluator workers and repeated runs skip all
writes.

Evaluating the NixOS release-small jobset with nix-eval-jobs and 4
workers on a 16-core server (`bench/eval_bench.sh`):

| | wall time |
|---|---|
| /nix/store | 207 s |
| `tarmac://`, cold | 164 s |
| `tarmac://`, warm | 140 s |

The whole jobset output fits in a 112 MB store directory, and a warm
run issues a single fsync.

```console
$ nix-eval-jobs --workers 4 --eval-store tarmac:// \
    --gc-roots-dir /tmp/roots nixos/release-small.nix
```

Building works: Nix copies the derivations out to the build store on
demand, so `nix build --eval-store tarmac://...` behaves like a
normal build. The store itself never holds build outputs and signs
nothing.

Without a path, `tarmac://` uses the shared cache described above.
Eval records survive as long as something reads them. Pass an
explicit path like `tarmac:///var/cache/evalstore` for an isolated
store with its own retention.

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

## Supported Nix versions

Nix 2.34+. The plugin is built against all versions provided by the pinned nixpkgs of the flake.
The loader picks the build whose libnixstore SONAME is already loaded in the process and otherwise skips with a warning.

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
