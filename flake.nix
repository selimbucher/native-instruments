{
  description = "Native Instruments software under Wine — Nix packaging for the ni-wine CLI";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      wine = pkgs.wineWow64Packages.staging;

      # Tools ni-wine executes at runtime.  The user's own browser (earlier
      # on PATH) is preferred by the probe — chromium- and firefox-family
      # both work for the download-URL capture; firefox is the guaranteed
      # fallback.
      runtimePath = pkgs.lib.makeBinPath [
        wine
        pkgs.winetricks
        pkgs.xorg.xorgserver # Xvfb
        pkgs.xdotool
        pkgs.cabextract
        pkgs.p7zip
        pkgs.msitools
        pkgs.procps
        pkgs.yad
        pkgs.xdg-utils
        pkgs.desktop-file-utils
        pkgs.firefox
      ];

      ni-wine = pkgs.python3Packages.buildPythonApplication {
        pname = "ni-wine";
        version = "2.1.2";
        pyproject = true;
        src = ./.;
        build-system = [ pkgs.python3Packages.setuptools ];

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
      packages.${system}.default = ni-wine;

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
