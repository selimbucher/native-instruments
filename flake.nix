{
  description = "Native Instruments software under Wine — Nix packaging for the ni-wine CLI";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      # NOT wineWow64Packages.yabridge (9.21): current Native Access's
      # Electron hits a deterministic CHECK crash (0x80000003, no window)
      # under it — verified on fresh prefixes, 2026-09.  Two Wine builds
      # cannot share a live prefix session ("wine client error: version
      # mismatch"), so for DAW cohabitation override *yabridge's* wine up
      # to this build (its package takes a `wine` argument) instead of
      # pinning ni-wine down.
      wine = pkgs.wineWow64Packages.staging;
      version = "2.4.0";

      # Tools ni-wine executes at runtime.
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

      # The forwarding msi.dll for the Kontakt installer (see shim/README.md),
      # cross-compiled from source with the 32-bit mingw-w64 toolchain.
      msi-shim = pkgs.pkgsCross.mingw32.stdenv.mkDerivation {
        pname = "ni-wine-msi-shim";
        inherit version;
        src = ./shim;
        nativeBuildInputs = [ pkgs.buildPackages.python3 ];
        buildPhase = "make CC=$CC";
        installPhase = "install -Dm644 msi_shim32.dll $out/msi_shim32.dll";
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
        '';

        makeWrapperArgs = [ "--prefix" "PATH" ":" runtimePath ];

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
        ];
        shellHook = ''
          export WINEPREFIX="$HOME/.wine-ni"
        '';
      };
    };
}
