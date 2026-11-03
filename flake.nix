{
  description = "Native Instruments software under Wine on Linux";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      # not wineWow64Packages.yabridge (9.21): Native Access's Electron hits
      # a CHECK crash (0x80000003, no window) under it, verified 2026-09.
      # Two Wine builds can't share a prefix session ("version mismatch"),
      # so override yabridge's wine up to this one (overlays.yabridge below).
      wine = pkgs.wineWow64Packages.staging;
      version = "2.5.0";

      # tools ni-wine runs
      runtimePath = pkgs.lib.makeBinPath [
        wine
        pkgs.winetricks
        pkgs.xorg.xorgserver # Xvfb
        pkgs.cabextract
        pkgs.p7zip
        pkgs.msitools
        pkgs.procps
        pkgs.yad
        pkgs.xdg-utils
        pkgs.desktop-file-utils
      ];

      # the forwarding msi.dll for the Kontakt installer (shim/README.md)
      msi-shim = pkgs.pkgsCross.mingw32.stdenv.mkDerivation {
        pname = "ni-wine-msi-shim";
        inherit version;
        src = ./shim;
        nativeBuildInputs = [ pkgs.buildPackages.python3 ];
        buildPhase = "make CC=$CC";
        installPhase = "install -Dm644 msi_shim32.dll $out/msi_shim32.dll";
        dontStrip = true;
      };

      # NI controller support (hardware/README.md)
      hardware = pkgs.pkgsCross.mingwW64.stdenv.mkDerivation {
        pname = "ni-wine-hardware";
        inherit version;
        src = ./hardware;
        buildPhase = "make CC=$CC";
        installPhase = "mkdir -p $out && make install CC=$CC DATADIR=$out";
        dontStrip = true;
      };

      ni-wine = pkgs.python3Packages.buildPythonApplication {
        pname = "ni-wine";
        inherit version;
        pyproject = true;
        src = ./.;
        build-system = [ pkgs.python3Packages.setuptools ];

        postPatch = ''
          cp ${msi-shim}/msi_shim32.dll src/ni_wine/data/msi_shim32.dll
          cp ${hardware}/* src/ni_wine/data/
        '';

        makeWrapperArgs = [ "--prefix" "PATH" ":" runtimePath ];

        nativeCheckInputs = [ pkgs.python3Packages.pytestCheckHook ];

        postInstall = ''
          install -Dm644 src/ni_wine/data/native-access.desktop \
            $out/share/applications/native-access.desktop
          install -Dm644 src/ni_wine/data/native-access.svg \
            $out/share/icons/hicolor/scalable/apps/native-access.svg
        '';

        meta = {
          description = "Native Instruments software under Wine on Linux";
          mainProgram = "ni";
        };
      };
    in
    {
      packages.${system} = {
        default = ni-wine;
        msi-shim = msi-shim;
        hardware = hardware;
      };

      # yabridge's last release (5.1.1) is built against Wine 9.21, which
      # Native Access doesn't run on. This builds yabridge master (Wine 10+
      # support) against ni-wine's Wine, so plugins and Native Access can
      # share the prefix.
      overlays.yabridge = final: prev:
        let
          wineWow64Packages = prev.wineWow64Packages // {
            yabridge = final.wineWow64Packages.staging;
          };
        in
        {
          yabridge = (prev.yabridge.override { inherit wineWow64Packages; }).overrideAttrs (old: {
            version = "5.1.1-unstable-2026-08-02";
            src = final.fetchFromGitHub {
              owner = "robbert-vdh";
              repo = "yabridge";
              rev = "b580a9f7fc46509767ca156d4f92872552b9e571";
              hash = "sha256-TiKiyE3GZYCX1+vooHdD03fAhNQPAA1IzTfkG++I7TY=";
            };
            # master dropped the 32-bit build itself
            patches = builtins.filter
              (p: !final.lib.hasSuffix "libyabridge-drop-32-bit-support.patch" (toString p))
              old.patches;
          });
          yabridgectl = prev.yabridgectl.override { inherit wineWow64Packages; };
        };

      devShells.${system}.default = pkgs.mkShell {
        packages = [
          ni-wine
          pkgs.ruff
          pkgs.python3
          pkgs.python3Packages.pytest
        ];
        shellHook = ''
          export WINEPREFIX="$HOME/.wine-ni"
        '';
      };
    };
}
