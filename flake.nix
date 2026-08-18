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

      darwinModules.default =
        { pkgs, ... }:
        {
          nix.settings.plugin-files = [
            "${
              self.packages.${pkgs.stdenv.hostPlatform.system}.plugin-dispatcher
            }/lib/nix/plugins/nix-tarmac-loader.dylib"
          ];
        };

      nixosModules.default =
        { pkgs, ... }:
        {
          nix.settings.plugin-files = [
            "${
              self.packages.${pkgs.stdenv.hostPlatform.system}.plugin-dispatcher
            }/lib/nix/plugins/nix-tarmac-loader.so"
          ];
        };

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
