{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  lmdb,
  libblake3,
  libarchive,
  openssl,
  # must be ABI-compatible with the nix that dlopen()s the plugin
  nix-fetchers ? null,
  nix-store ? null,
  nix-util ? null,
  plugin ? true,
  doCheck ? !plugin,
}:

stdenv.mkDerivation {
  pname = "nix-tarmac";
  version = "0.1.0";
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./meson.build
      ./meson.options
      ./src
      ./bench
      ./tests
      ./plugin
    ];
  };

  nativeBuildInputs = [
    meson
    ninja
    pkg-config
  ];

  buildInputs = [
    lmdb
    libblake3
    libarchive
    openssl
  ]
  ++ lib.optionals plugin [
    nix-fetchers
    nix-store
    nix-util
  ];

  mesonFlags = [ "-Dplugin=${if plugin then "enabled" else "disabled"}" ];
  inherit doCheck;

  postInstall = lib.optionalString (!plugin) "mkdir -p $out";

  env.NIX_CFLAGS_COMPILE = "-fno-omit-frame-pointer -g";
  dontStrip = true;

  meta = {
    description = "Fast tarball fetcher cache plugin for Nix";
    license = lib.licenses.mit;
  };
}
