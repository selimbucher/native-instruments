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

      echo "==> Initializing Wine prefix..."
      WINEDLLOVERRIDES="mscoree,mshtml=" ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${wine}/bin/wineboot -i

      echo "==> Disabling winemenubuilder (no .desktop files)..."
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${wine}/bin/wine reg add 'HKCU\Software\Wine\DllOverrides' \
        /v 'winemenubuilder.exe' /t REG_SZ /d "" /f 2>/dev/null || true

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

      echo "==> Downloading Native Access installer..."
      NA_INSTALLER="/tmp/Native-Access_2.exe"
      CURL_ARGS="-L --progress-bar -o $NA_INSTALLER"
      [ -f "$NA_INSTALLER" ] && CURL_ARGS="$CURL_ARGS -z $NA_INSTALLER"
      ${pkgs.curl}/bin/curl $CURL_ARGS \
        "https://www.native-instruments.com/fileadmin/downloads/Native-Access_2.exe"

      echo "==> Installing Native Access..."
      ${xvfb-dismiss}/bin/xvfb-dismiss 98 "Warning" Return \
        ${wine}/bin/wine "$NA_INSTALLER"
      ${wine}/bin/wineserver -k || true

      echo "==> Installing NTKDaemon..."
      NTK_INSTALLER=$(ls "$WINEPREFIX/drive_c/Program Files/Native Instruments/Native Access/resources/daemon/win/NTKDaemon "*.exe 2>/dev/null | head -1)
      if [ -z "$NTK_INSTALLER" ]; then
        echo "Error: NTKDaemon installer not found" >&2
        exit 1
      fi
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum ${wine}/bin/wine "$NTK_INSTALLER" /s
      ${wine}/bin/wineserver -k || true

      echo "==> Fixing msvcp140 DLLs for Kontakt compatibility..."
      ${ni-install}/bin/ni-install --fix-msvcp140

      echo "==> Done. Prefix ready at $WINEPREFIX"
    '';

    ni-launch = pkgs.writeShellScriptBin "native-access" ''
      export WINEPREFIX="$HOME/.wine-ni"
      ${wine}/bin/wine "$WINEPREFIX/drive_c/Program Files/Common Files/Native Instruments/NTK/NTKDaemon.exe"
      ${wine}/bin/wine "$WINEPREFIX/drive_c/users/$USER/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Native Access.lnk"
    '';

    ni-install = pkgs.writeShellScriptBin "ni-install" ''
      usage() {
        echo "Usage: ni-install [--kontakt8 <url>] [--fix-msvcp140]"
        exit 1
      }

      [ $# -eq 0 ] && usage

      export WINEPREFIX="$HOME/.wine-ni"

      while [[ $# -gt 0 ]]; do
        case "$1" in
          --kontakt8)
            URL="$2"; shift 2
            ZIP="/tmp/Kontakt_8_Installer.zip"
            TMP_DRIVE="/tmp/k8_drive_c"

            echo "==> Downloading Kontakt 8..."
            ${pkgs.curl}/bin/curl -L --progress-bar -o "$ZIP" "$URL"

            echo "==> Extracting..."
            OUT="$TMP_DRIVE" PATH="${pkgs.lib.makeBinPath [ pkgs.p7zip pkgs.unzip ]}:$PATH" \
              bash ${./scripts/extract_kontakt8.sh} "$ZIP"

            echo "==> Copying to Wine prefix..."
            cp -r "$TMP_DRIVE/." "$WINEPREFIX/drive_c/"

            echo "==> Cleaning up..."
            rm -rf "$TMP_DRIVE" "$ZIP"

            echo "==> Kontakt 8 installed."
            ;;

          --fix-msvcp140)
            shift
            TMPDIR="$(mktemp -d)"
            trap 'rm -rf "$TMPDIR"' EXIT

            echo "==> Downloading VC++ 2022 x64 redistributable..."
            ${pkgs.curl}/bin/curl -L --progress-bar \
              "https://aka.ms/vs/17/release/vc_redist.x64.exe" \
              -o "$TMPDIR/vc_redist.x64.exe"

            echo "==> Extracting outer cabinet..."
            ${pkgs.cabextract}/bin/cabextract -d "$TMPDIR/stage1" "$TMPDIR/vc_redist.x64.exe" 2>/dev/null

            echo "==> Locating amd64 DLL cabinet..."
            INNER_CAB=""
            for f in "$TMPDIR/stage1"/a*; do
              if ${pkgs.cabextract}/bin/cabextract -l "$f" 2>/dev/null | grep -q "msvcp140.dll_amd64"; then
                INNER_CAB="$f"
                break
              fi
            done

            if [[ -z "$INNER_CAB" ]]; then
              echo "ERROR: Could not find inner cabinet with msvcp140.dll_amd64" >&2
              exit 1
            fi

            echo "==> Extracting DLLs..."
            ${pkgs.cabextract}/bin/cabextract -d "$TMPDIR/stage2" "$INNER_CAB" 2>/dev/null

            SYSTEM32="$WINEPREFIX/drive_c/windows/system32"

            copy_dll() {
              local src="$TMPDIR/stage2/$1"
              local dst="$SYSTEM32/$2"
              if [[ -f "$src" ]]; then
                echo "==> Installing $2"
                cp "$src" "$dst"
              else
                echo "WARNING: $1 not found, skipping"
              fi
            }

            copy_dll "msvcp140.dll_amd64"             "msvcp140.dll"
            copy_dll "msvcp140_1.dll_amd64"           "msvcp140_1.dll"
            copy_dll "msvcp140_2.dll_amd64"           "msvcp140_2.dll"
            copy_dll "msvcp140_atomic_wait.dll_amd64" "msvcp140_atomic_wait.dll"
            copy_dll "msvcp140_codecvt_ids.dll_amd64" "msvcp140_codecvt_ids.dll"
            copy_dll "concrt140.dll_amd64"            "concrt140.dll"
            copy_dll "vcruntime140.dll_amd64"         "vcruntime140.dll"
            copy_dll "vcruntime140_1.dll_amd64"       "vcruntime140_1.dll"
            copy_dll "vcruntime140_threads.dll_amd64" "vcruntime140_threads.dll"

            echo "==> Setting DLL overrides..."
            for dll in msvcp140 msvcp140_1 msvcp140_2 msvcp140_atomic_wait \
                       msvcp140_codecvt_ids concrt140 \
                       vcruntime140 vcruntime140_1 vcruntime140_threads; do
              ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
                ${wine}/bin/wine reg add \
                "HKCU\\Software\\Wine\\DllOverrides" \
                /v "$dll" /t REG_SZ /d "native,builtin" /f \
                2>/dev/null || true
            done

            echo "==> msvcp140 fix applied."
            ;;

          *) usage ;;
        esac
      done
    '';

    native-instruments = pkgs.symlinkJoin {
      name = "native-instruments";
      paths = [ ni-setup ni-launch ni-install xvfb-dismiss ];
      postBuild = ''
        mkdir -p $out/share/applications
        cp ${./data/native-access.desktop} $out/share/applications/native-access.desktop
      '';
    };

  in {
    packages.${system}.default = native-instruments;

    devShells.${system}.default = pkgs.mkShell {
      packages = [ native-instruments wine pkgs.winetricks pkgs.xvfb-run pkgs.xdotool pkgs.curl pkgs.p7zip pkgs.unzip pkgs.cabextract ];

      shellHook = ''
        export WINEPREFIX="$HOME/.wine-ni"
        export WINEARCH="win64"
      '';
    };
  };
}
