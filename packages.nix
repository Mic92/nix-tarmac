# One plugin per supported Nix version plus a dispatcher bundle.
{
  lib,
  newScope,
  fetchFromGitHub,
  nixVersions,
  symlinkJoin,
}:
lib.makeScope newScope (
  self:
  let
    # nixpkgs' nixVersions.git lags behind master. Track it closer so API
    # breakage shows up here before downstream flakes pull a newer nix.
    # Bumped weekly by the update-nix-git effect (effects.nix).
    nixGitPin = lib.importJSON ./nix-git.json;
    nixGitSrc = fetchFromGitHub {
      inherit (nixGitPin)
        owner
        repo
        rev
        hash
        ;
    };
    nixGit =
      ((nixVersions.nixComponents_git.overrideSource nixGitSrc).overrideScope (
        _final: _prev: { inherit (nixGitPin) version; }
      )).nix-everything;
    nixFor = version: if version == "git" then nixGit else nixVersions.${version};

    supportedNixVersions = builtins.filter (
      name:
      builtins.match "nix_[0-9]+_[0-9]+" name != null
      && (builtins.tryEval (
        lib.versionAtLeast nixVersions.${name}.version "2.34"
        && (nixVersions.${name}.libs or { }) ? nix-fetchers
      )).value or false
    ) (builtins.attrNames nixVersions);
  in
  {
    tests = self.callPackage ./package.nix { plugin = false; };

    default = self.callPackage ./package.nix {
      inherit (nixVersions.latest.libs) nix-fetchers nix-store nix-util;
    };

    versionPlugins = lib.listToAttrs (
      map (
        version:
        lib.nameValuePair "plugin-${version}" (
          (self.callPackage ./package.nix {
            inherit ((nixFor version).libs) nix-fetchers nix-store nix-util;
          }).overrideAttrs
            (old: {
              # tests/e2e.sh runs the plugin under exactly this nix
              passthru = (old.passthru or { }) // {
                nix = nixFor version;
              };
            })
        )
      ) (supportedNixVersions ++ [ "git" ])
    );

    # The loader dlopen()s the build matching the running Nix version.
    # extraPlugins come first so exact-match builds win over nixpkgs ones.
    mkDispatcher =
      extraPlugins:
      symlinkJoin {
        name = "nix-tarmac-dispatcher";
        paths =
          extraPlugins
          ++ [ self.default ]
          ++ map (version: self.versionPlugins."plugin-${version}") (supportedNixVersions ++ [ "git" ]);
        # dladdr() resolves symlinks, so the loader must be a real file here
        postBuild = ''
          rm -f "$out"/lib/nix/plugins/nix-tarmac-loader.*
          cp -L ${self.default}/lib/nix/plugins/nix-tarmac-loader.* "$out"/lib/nix/plugins/
        '';
      };

    plugin-dispatcher = self.mkDispatcher [ ];
  }
)
