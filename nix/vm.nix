{ lib, pkgs, ... }:
{
  users.users.demo = {
    isNormalUser = true;
    password = "demo";
    extraGroups = [
      "wheel"
      "video"
      "input"
    ];
  };
  users.users.root.password = "root";
  services.getty.autologinUser = "demo";

  hardware.graphics.enable = true;

  fonts = {
    fontconfig.enable = true;
    packages = [ pkgs.nerd-fonts.jetbrains-mono ];
  };

  services.kmscon = {
    enable = true;
    useXkbConfig = true;
    config = {
      font-name = "JetBrainsMono Nerd Font";
      font-size = 14;
      hwaccel = true;
      term = "kmscon";
      palette = "solarized-black";
      sb-size = 4096;
      xkb-layout = "us";
    };
  };

  environment.systemPackages = with pkgs; [
    vttest
    htop
    unicode-emoji
  ];

  virtualisation = {
    memorySize = 2048;
    cores = 2;
    diskSize = 4096;
    graphics = true;
    useNixStoreImage = true;
    writableStore = false;
    sharedDirectories = lib.mkForce { };
    qemu.options = [ "-device virtio-gpu-pci" ];
  };

  boot.initrd.kernelModules = [ "virtio_gpu" ];
  boot.kernelParams = [
    "console=tty0"
    "console=ttyS0,115200"
  ];

  documentation.enable = false;
  system.stateVersion = "26.11";
}
