{ nixpkgs, nixbot }:
_args:
let
  pkgs = nixpkgs.legacyPackages.x86_64-linux;
  inherit (nixbot.lib.effects { inherit pkgs; }) mkEffect;
in
{
  # Daily bump of nix-git.json so API breakage in Nix master shows up here
  # before downstream flakes pull a newer nix. PRs carry the auto-merge label
  # and land via .github/workflows/auto-merge.yaml once CI is green.
  onSchedule.update-nix-git = {
    when = {
      hour = 4;
      minute = 17;
    };
    outputs.effects.update-nix-git = mkEffect {
      name = "effect-update-nix-git";
      checkout = true;
      inputs = [
        pkgs.git
        pkgs.gh
        pkgs.nix
        pkgs.jq
      ];
      secretsMap.git.type = "GitToken";
      effectScript = ''
        set -euo pipefail
        export NIX_CONFIG="experimental-features = nix-command flakes"
        GH_TOKEN=$(jq -r '.git.data.token' "$HERCULES_CI_SECRETS_JSON")
        export GH_TOKEN
        git config --global user.name "nix-tarmac-bot"
        git config --global user.email "nix-tarmac-bot@users.noreply.github.com"
        git config --global safe.directory '*'
        cd "$NIXBOT_EFFECT_CHECKOUT"

        ./scripts/update-nix-git.sh
        if git diff --quiet nix-git.json; then
          exit 0
        fi
        version=$(jq -r .version nix-git.json)
        branch=update-nix-git
        git checkout -b "$branch"
        git commit -m "nix-git: bump to $version" nix-git.json
        git push -f origin "$branch"
        if ! gh pr view "$branch" --json state -q .state 2>/dev/null | grep -qx OPEN; then
          gh pr create --head "$branch" --title "nix-git: bump to $version" \
            --body "Automated bump of nix-git.json to current NixOS/nix master." \
            --label auto-merge
        fi
      '';
    };
  };
}
