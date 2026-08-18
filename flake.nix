{
  description = "Fast tarball fetcher cache plugin for Nix";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
      scopeFor = system: nixpkgs.legacyPackages.${system}.callPackage ./packages.nix { };
      tarmacModule =
        {
          pkgs,
          lib,
          config,
          ...
        }:
        let
          suffix = if pkgs.stdenv.hostPlatform.isDarwin then "dylib" else "so";
        in
        {
          options.nix-tarmac.package = lib.mkOption {
            type = lib.types.package;
            default = (pkgs.callPackage ./packages.nix { }).plugin-dispatcher;
            description = "Plugin build to load. Override to build against a custom Nix.";
          };
          config.nix.settings.plugin-files = [
            "${config.nix-tarmac.package}/lib/nix/plugins/nix-tarmac-loader.${suffix}"
          ];
        };
    in
    {
      packages = forAllSystems (
        system:
        let
          scope = scopeFor system;
        in
        {
          inherit (scope) default plugin-dispatcher;
        }
        // scope.versionPlugins
      );

      nixosModules.default = tarmacModule;
      darwinModules.default = tarmacModule;

      checks = forAllSystems (
        system:
        let
          scope = scopeFor system;
        in
        {
          inherit (scope) tests;
          inherit (scope) plugin-dispatcher;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ (scopeFor system).default ];
            packages =
              with pkgs;
              [
                clang-tools
                llvmPackages_latest.clang
              ]
              ++ lib.optionals stdenv.hostPlatform.isLinux [ perf ];
          };
        }
      );
    };
}
