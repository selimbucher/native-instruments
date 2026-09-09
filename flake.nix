{
  description = "Native Instruments software under Wine — Nix packaging for the ni-wine CLI";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      wine = pkgs.wineWow64Packages.staging;
      version = "2.2.0";

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
