{
  lib,
  stdenv,
  meson,
  libtsm,
  systemdLibs,
  libxkbcommon,
  libdrm,
  libGLU,
  libGL,
  freetype,
  fontconfig,
  zlib,
  pango,
  pkg-config,
  docbook_xsl,
  docbook_xml_dtd_42,
  libxslt,
  libgbm,
  ninja,
  check,
  dbus,
  bash,
  inotify-tools,
  buildPackages,
}:
stdenv.mkDerivation {
  pname = "kmscon";
  version = "10.0.3";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./docs
      ./meson.build
      ./meson.options
      ./scripts
      ./src
      ./subprojects
      ./tests
      ./tools
    ];
  };

  strictDeps = true;
  __structuredAttrs = true;

  depsBuildBuild = [
    buildPackages.stdenv.cc
  ];

  buildInputs = [
    libGLU
    libGL
    libdrm
    libtsm
    libxkbcommon
    freetype
    fontconfig
    zlib
    pango
    systemdLibs
    libgbm
    check
    dbus
    bash
  ];

  nativeBuildInputs = [
    meson
    ninja
    docbook_xsl
    pkg-config
    libxslt
    docbook_xml_dtd_42
  ];

  env.PKG_CONFIG_SYSTEMD_SYSTEMDSYSTEMUNITDIR = "${placeholder "out"}/lib/systemd/system";

  outputs = [
    "out"
    "man"
  ];

  postFixup = ''
    substituteInPlace $out/bin/kmscon-launch-gui \
      --replace-fail "inotifywait" "${lib.getExe' inotify-tools "inotifywait"}"
  '';

  meta = {
    description = "KMS/DRM based System Console";
    mainProgram = "kmscon";
    homepage = "https://www.freedesktop.org/wiki/Software/kmscon/";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
  };
}
