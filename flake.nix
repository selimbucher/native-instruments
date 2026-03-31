{
  description = "Native Instruments Wine setup for NixOS";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};
    wine = pkgs.wineWow64Packages.staging;

    native-instruments = pkgs.stdenv.mkDerivation {
      pname = "native-instruments";
      version = "1.0.0";
      src = ./.;

      nativeBuildInputs = [ pkgs.makeShellWrapper ];

      buildInputs = [ wine pkgs.winetricks pkgs.xvfb-run pkgs.xdotool pkgs.curl ];

      installPhase = ''
        mkdir -p $out/bin $out/share/applications

        # xvfb-dismiss helper
        makeShellWrapper ${pkgs.bash}/bin/bash $out/bin/xvfb-dismiss \
          --add-flags "$src/scripts/xvfb-dismiss.sh" \
          --prefix PATH : ${pkgs.lib.makeBinPath [ pkgs.xvfb-run pkgs.xdotool ]}

        # ni-setup
        makeShellWrapper ${pkgs.bash}/bin/bash $out/bin/ni-setup \
          --add-flags "$src/scripts/ni-setup.sh" \
          --prefix PATH : ${pkgs.lib.makeBinPath [ wine pkgs.winetricks pkgs.xvfb-run pkgs.curl ]} \
          --set-default WINEPREFIX "$HOME/.wine-ni" \
          --set-default WINEARCH win64

        # ni-launch
        makeShellWrapper ${pkgs.bash}/bin/bash $out/bin/native-access \
          --add-flags "$src/scripts/ni-launch.sh" \
          --prefix PATH : ${pkgs.lib.makeBinPath [ wine ]} \
          --set-default WINEPREFIX "$HOME/.wine-ni"

        cp data/native-access.desktop $out/share/applications/native-access.desktop
      '';
    };

  in {
    packages.${system}.default = native-instruments;

    homeManagerModules.default = { lib, ... }: {
      home.packages = [ native-instruments ];

      home.activation.nativeInstruments = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        MARKER="$HOME/.wine-ni/.setup-version"
        EXPECTED="${self.rev or "dirty"}"

        if [ ! -f "$MARKER" ] || [ "$(cat "$MARKER")" != "$EXPECTED" ]; then
          echo "==> Native Instruments: setting up Wine prefix..."
          ${native-instruments}/bin/ni-setup
          echo "$EXPECTED" > "$MARKER"
        fi
      '';
    };

    devShells.${system}.default = pkgs.mkShell {
      packages = [ native-instruments wine pkgs.winetricks pkgs.xvfb-run pkgs.xdotool pkgs.curl ];

      shellHook = ''
        export WINEPREFIX="$HOME/.wine-ni"
        export WINEARCH="win64"
      '';
    };
  };
}
