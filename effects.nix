{ nixpkgs }:
_args:
let
  pkgs = nixpkgs.legacyPackages.x86_64-linux;
in
{
  # Weekly bump of nix-git.json so API breakage in Nix master shows up here
  # before downstream flakes pull a newer nix. PRs carry the auto-merge label
  # and land via .github/workflows/auto-merge.yaml once CI is green.
  onSchedule.update-nix-git = {
    when = {
      dayOfWeek = [ "Mon" ];
      hour = 4;
      minute = 17;
    };
    # Plain derivation instead of nixbot's mkEffect to avoid the flake input.
    # nixbot reads secretsMap and __nixbot_effect_checkout, see its
    # docs/EFFECTS.md.
    outputs.effects.update-nix-git =
      pkgs.runCommand "effect-update-nix-git"
        {
          nativeBuildInputs = [
            pkgs.cacert
            pkgs.git
            pkgs.gh
            pkgs.nix
            pkgs.jq
          ];
          secretsMap = builtins.toJSON { git.type = "GitToken"; };
          __nixbot_effect_checkout = true;
        }
        ''
          export HOME=/build/home
          mkdir -p "$HOME"
          export NIX_CONFIG="experimental-features = nix-command flakes"
          GH_TOKEN=$(jq -r '.git.data.token' "$HERCULES_CI_SECRETS_JSON")
          export GH_TOKEN
          git config --global user.name "nix-tarmac-bot"
          git config --global user.email "nix-tarmac-bot@users.noreply.github.com"
          git config --global safe.directory '*'
          cd "$NIXBOT_EFFECT_CHECKOUT"

          bash scripts/update-nix-git.sh
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
}
