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
    # nixpkgs' nixVersions.git lags behind master; track it closer so API
    # breakage shows up here before downstream flakes pull a newer nix.
    # Bumped daily by the update-nix-git effect (effects.nix).
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
    libsFor = version: if version == "git" then nixGit.libs else nixVersions.${version}.libs;

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
          self.callPackage ./package.nix {
            inherit (libsFor version) nix-fetchers nix-store nix-util;
          }
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
