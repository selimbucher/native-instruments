{
  description = "Native Instruments Wine setup for NixOS";

  inputs.nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = nixpkgs.legacyPackages.${system};
    wine = pkgs.wineWow64Packages.staging;

    xvfb-dismiss = pkgs.writeShellScriptBin "xvfb-dismiss" ''
      DISPLAY_NUM=$1; shift
      WIN_NAME=$1; shift
      KEY=$1; shift

      ${pkgs.xvfb-run}/bin/xvfb-run -n "$DISPLAY_NUM" "$@" &
      CMD_PID=$!

      sleep 1
      while kill -0 "$CMD_PID" 2>/dev/null; do
        WID=$(DISPLAY=":$DISPLAY_NUM" ${pkgs.xdotool}/bin/xdotool search --name "$WIN_NAME" 2>/dev/null | head -1)
        if [ -n "$WID" ]; then
          DISPLAY=":$DISPLAY_NUM" ${pkgs.xdotool}/bin/xdotool key --window "$WID" "$KEY"
        fi
        sleep 0.5
      done

      wait "$CMD_PID"
    '';

    ni-setup = pkgs.writeShellScriptBin "ni-setup" ''
      export WINEPREFIX="$HOME/.wine-ni"
      export WINEARCH="win64"

      echo "==> Initializing Wine prefix (dismissing Mono installer)..."
      ${xvfb-dismiss}/bin/xvfb-dismiss 98 "Wine Mono Installer" Escape \
        ${wine}/bin/wineboot -i

      echo "==> Disabling winemenubuilder (no .desktop files)..."
      ${wine}/bin/wine reg add 'HKCU\Software\Wine\DllOverrides' \
        /v 'winemenubuilder.exe' /t REG_SZ /d "" /f 2>/dev/null

      echo "==> Removing home folder symlinks from Wine prefix..."
      for link in \
        "$WINEPREFIX/drive_c/users/$USER/Desktop" \
        "$WINEPREFIX/drive_c/users/$USER/Documents" \
        "$WINEPREFIX/drive_c/users/$USER/My Documents" \
        "$WINEPREFIX/drive_c/users/$USER/Downloads" \
        "$WINEPREFIX/drive_c/users/$USER/Music" \
        "$WINEPREFIX/drive_c/users/$USER/My Music" \
        "$WINEPREFIX/drive_c/users/$USER/Pictures" \
        "$WINEPREFIX/drive_c/users/$USER/My Pictures" \
        "$WINEPREFIX/drive_c/users/$USER/Videos" \
        "$WINEPREFIX/drive_c/users/$USER/My Videos" \
        "$WINEPREFIX/drive_c/users/$USER/Templates" \
      ; do
        if [ -L "$link" ]; then
          rm "$link"
          mkdir -p "$link"
        fi
      done

      echo "==> Installing vcrun2022..."
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${pkgs.winetricks}/bin/winetricks --unattended vcrun2022

      echo "==> Installing PowerShell..."
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${pkgs.winetricks}/bin/winetricks --unattended powershell

      echo "==> Installing Native Access..."
      NA_INSTALLER="$PWD/Native-Access_2.exe"
      if [ ! -f "$NA_INSTALLER" ]; then
        echo "Error: $NA_INSTALLER not found. Run ni-setup from the project directory." >&2
        exit 1
      fi
      ${xvfb-dismiss}/bin/xvfb-dismiss 98 "Warning" Return \
        ${wine}/bin/wine "$NA_INSTALLER"
      ${wine}/bin/wineserver -k || true

      echo "==> Installing NTKDaemon..."
      NTK_INSTALLER=$(ls "$HOME/.wine-ni/drive_c/Program Files/Native Instruments/Native Access/resources/daemon/win/NTKDaemon "*.exe 2>/dev/null | head -1)
      if [ -z "$NTK_INSTALLER" ]; then
        echo "Error: NTKDaemon installer not found" >&2
        exit 1
      fi
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum ${wine}/bin/wine "$NTK_INSTALLER" /s
      ${wine}/bin/wineserver -k || true

      echo "==> Done. Prefix ready at $WINEPREFIX"
    '';

  in {
    # Dev shell for manual setup / debugging
    devShells.${system}.default = pkgs.mkShell {
      packages = [
        wine
        pkgs.winetricks
        pkgs.xvfb-run
        pkgs.xdotool
        pkgs.curl
        ni-setup
        xvfb-dismiss
      ];

      shellHook = ''
        export WINEPREFIX="$HOME/.wine-ni"
        export WINEARCH="win64"
      '';
    };

    # Home-manager module
    homeManagerModules.default = { lib, ... }: {
      home.packages = [ wine ni-setup ];

      xdg.desktopEntries.native-access = {
        name = "Native Access";
        comment = "Native Access is your one-stop hub for easy product installation, registration, and updates.";
        startupWMClass = "native access.exe";
        exec = ''sh -c 'WINEPREFIX="$HOME/.wine-ni" wine "$HOME/.wine-ni/drive_c/Program Files/Common Files/Native Instruments/NTK/NTKDaemon.exe"; WINEPREFIX="$HOME/.wine-ni" wine "$HOME/.wine-ni/drive_c/users/$USER/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Native Access.lnk"' '';
      };

      home.activation.nativeInstruments = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
        MARKER="$HOME/.wine-ni/.setup-version"
        EXPECTED="${self.rev or "dirty"}"

        if [ ! -f "$MARKER" ] || [ "$(cat "$MARKER")" != "$EXPECTED" ]; then
          echo "==> Native Instruments: setting up Wine prefix (version $EXPECTED)..."
          ${ni-setup}/bin/ni-setup
          echo "$EXPECTED" > "$MARKER"
        fi
      '';
    };
  };
}
