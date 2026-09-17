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
      palette = "custom";
      palette-black = "45475a";
      palette-red = "f38ba8";
      palette-green = "a6e3a1";
      palette-yellow = "f9e2af";
      palette-blue = "89b4fa";
      palette-magenta = "f5c2e7";
      palette-cyan = "94e2d5";
      palette-light-grey = "bac2de";
      palette-dark-grey = "585b70";
      palette-light-red = "f38ba8";
      palette-light-green = "a6e3a1";
      palette-light-yellow = "f9e2af";
      palette-light-blue = "89b4fa";
      palette-light-magenta = "f5c2e7";
      palette-light-cyan = "94e2d5";
      palette-white = "a6adc8";
      palette-foreground = "cdd6f4";
      palette-background = "1e1e2e";
      sb-size = 4096;
      scrollbar = true;
      cursor-style = "bar";
      cursor-color = "f5e0dc";
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
