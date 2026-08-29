{
  description = "Fast tarball fetcher cache plugin for Nix";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  inputs.nixbot.url = "git+https://github.com/Mic92/nixbot?shallow=1&ref=main";
  inputs.nixbot.inputs.nixpkgs.follows = "nixpkgs";

  outputs =
    {
      self,
      nixpkgs,
      nixbot,
    }:
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
        {
          options.nix-tarmac.package = lib.mkOption {
            type = lib.types.package;
            default =
              let
                scope = pkgs.callPackage ./packages.nix { };
              in
              scope.mkDispatcher (
                lib.optional (config.nix.package ? libs.nix-fetchers) (
                  pkgs.callPackage ./package.nix {
                    inherit (config.nix.package.libs) nix-fetchers nix-store nix-util;
                  }
                )
              );
            defaultText = "plugin built against config.nix.package";
          };
          config.nix.settings.plugin-files = [
            "${config.nix-tarmac.package}/lib/nix/plugins/nix-tarmac-loader${pkgs.stdenv.hostPlatform.extensions.sharedLibrary}"
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

      herculesCI = import ./effects.nix { inherit nixpkgs nixbot; };

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
