{
  description = "KMS/DRM based System Console";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      vmSystem =
        system:
        nixpkgs.lib.nixosSystem {
          modules = [
            "${nixpkgs}/nixos/modules/virtualisation/qemu-vm.nix"
            ./nix/vm.nix
            {
              nixpkgs.hostPlatform = system;
              nixpkgs.overlays = [ self.overlays.default ];
            }
          ];
        };
    in
    {
      overlays.default = final: prev: {
        kmscon = final.callPackage ./package.nix { };
      };

      packages = forAllSystems (
        pkgs:
        let
          inherit (pkgs.stdenv.hostPlatform) system;
        in
        rec {
          kmscon = pkgs.callPackage ./package.nix { };
          default = kmscon;
          vm = (vmSystem system).config.system.build.vm;
        }
      );

      nixosConfigurations.vm = vmSystem "x86_64-linux";

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.kmscon ];
          packages = with pkgs; [
            clang-tools
            gdb
            python3
          ];
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
    };
}
