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

      UI=false
      [[ ''${1:-} == --ui ]] && UI=true

      if $UI; then
        PIPE=$(mktemp -u)
        mkfifo "$PIPE"
        CSS=$(mktemp --suffix=.css)
        cat > "$CSS" << 'CSSEOF'
window, .dialog {
  background-color: #1a1a1a;
  color: #ffffff;
}
label {
  color: #ffffff;
  margin-bottom: 8px;
}
progressbar trough {
  background-color: #333333;
  border-radius: 4px;
}
progressbar progress {
  background-color: #ffffff;
  border-radius: 4px;
}
.dialog-action-area {
  margin: 0;
  padding: 0;
}
CSSEOF
        ${pkgs.yad}/bin/yad \
          --progress \
          --title="Native Instruments Setup" \
          --text="Native Access Setup" \
          --percentage=0 \
          --auto-close \
          --auto-kill \
          --center \
          --width=480 \
          --no-buttons \
          --borders=16 \
          --gtkrc="$CSS" \
          < "$PIPE" &
        exec 3>"$PIPE"
        trap "exec 3>&-; rm -f '$PIPE' '$CSS'" EXIT
      fi

      step() {
        echo "==> $1"
        if $UI; then
          echo "# $1" >&3
          echo "$2"   >&3
        fi
      }

      mkdir -p "$WINEPREFIX/drive_c"

      step "Initializing Wine prefix..." 5
      WINEDLLOVERRIDES="mscoree,mshtml=" ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${wine}/bin/wineboot -i

      step "Disabling winemenubuilder..." 12
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${wine}/bin/wine reg add 'HKCU\Software\Wine\DllOverrides' \
        /v 'winemenubuilder.exe' /t REG_SZ /d "" /f 2>/dev/null || true

      step "Cleaning up home folder symlinks..." 18
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
      mkdir -p "$WINEPREFIX/drive_c/users/Public/Downloads"

      step "Installing vcrun2022..." 25
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${pkgs.winetricks}/bin/winetricks --unattended vcrun2022

      step "Installing PowerShell..." 40
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum \
        ${pkgs.winetricks}/bin/winetricks --unattended powershell

      step "Downloading Native Access..." 55
      NA_INSTALLER="/tmp/Native-Access_2.exe"
      CURL_ARGS="-L --progress-bar -o $NA_INSTALLER"
      [ -f "$NA_INSTALLER" ] && CURL_ARGS="$CURL_ARGS -z $NA_INSTALLER"
      ${pkgs.curl}/bin/curl $CURL_ARGS \
        "https://www.native-instruments.com/fileadmin/downloads/Native-Access_2.exe"

      step "Installing Native Access..." 65
      ${xvfb-dismiss}/bin/xvfb-dismiss 98 "Warning" Return \
        ${wine}/bin/wine "$NA_INSTALLER"
      ${wine}/bin/wineserver -k || true

      step "Installing NTKDaemon..." 78
      NTK_INSTALLER=$(ls "$WINEPREFIX/drive_c/Program Files/Native Instruments/Native Access/resources/daemon/win/NTKDaemon "*.exe 2>/dev/null | head -1)
      if [ -z "$NTK_INSTALLER" ]; then
        echo "Error: NTKDaemon installer not found" >&2
        exit 1
      fi
      ${pkgs.xvfb-run}/bin/xvfb-run --auto-servernum ${wine}/bin/wine "$NTK_INSTALLER" /s
      ${wine}/bin/wineserver -k || true

      step "Fixing msvcp140 DLLs..." 90
      ${ni-install}/bin/ni-install --fix-msvcp140

      step "Done!" 100
    '';

    ni-launch = pkgs.writeShellScriptBin "native-access" ''
      export WINEPREFIX="$HOME/.wine-ni"

      NTK_EXE="$WINEPREFIX/drive_c/Program Files/Common Files/Native Instruments/NTK/NTKDaemon.exe"
      if [ ! -f "$NTK_EXE" ]; then
        echo "==> Native Access not installed. Running setup..."
        ${ni-setup}/bin/ni-setup --ui
      fi

      if ! pgrep -f "NTKDaemon.exe" > /dev/null 2>&1; then
        ${wine}/bin/wine "$NTK_EXE"
      fi
      ${wine}/bin/wine "$WINEPREFIX/drive_c/users/$USER/AppData/Roaming/Microsoft/Windows/Start Menu/Programs/Native Access.lnk"
    '';

    ni-url-server = pkgs.writeText "ni-url-server.py" ''
      import http.server, sys, os
      url_file = sys.argv[1]
      port     = int(sys.argv[2])
      class Handler(http.server.BaseHTTPRequestHandler):
          def do_OPTIONS(self):
              self.send_response(200)
              self.send_header('Access-Control-Allow-Origin', '*')
              self.send_header('Access-Control-Allow-Methods', 'POST')
              self.send_header('Access-Control-Allow-Headers', 'Content-Type')
              self.end_headers()
          def do_POST(self):
              url = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode().strip()
              self.send_response(200)
              self.send_header('Access-Control-Allow-Origin', '*')
              self.end_headers()
              open(url_file, 'w').write(url)
              os._exit(0)
          def log_message(self, *args): pass
      class ReuseServer(http.server.HTTPServer):
          allow_reuse_address = True
      ReuseServer(('localhost', port), Handler).serve_forever()
    '';

    ni-install = pkgs.writeShellScriptBin "ni-install" ''
      usage() {
        echo "Usage: ni-install [--kontakt8 [<url>]] [--update-kontakt8 [<url>]] [--uninstall-kontakt8] [--fix-msvcp140]"
        exit 1
      }

      [ $# -eq 0 ] && usage

      export WINEPREFIX="$HOME/.wine-ni"

      while [[ $# -gt 0 ]]; do
        case "$1" in
          --kontakt8)
            shift
            K8_EXE="$WINEPREFIX/drive_c/Program Files/Native Instruments/Kontakt 8/Kontakt 8.exe"
            if [ -f "$K8_EXE" ]; then
              echo "Kontakt 8 is already installed. Use ni-install --update-kontakt8 to update."
              exit 0
            fi

            if [[ $# -gt 0 && "$1" != --* ]]; then
              URL="$1"; shift
            else
              PORT=19876
              URL_FILE=$(mktemp)
              EXT_DIR=$(mktemp -d)

              cat > "$EXT_DIR/manifest.json" << 'JSON'
{
  "manifest_version": 3,
  "name": "NI URL Capture",
  "version": "1.0",
  "content_scripts": [{
    "matches": ["https://www.native-instruments.com/*/account/downloads/*"],
    "js": ["capture.js"],
    "run_at": "document_idle"
  }]
}
JSON

              cat > "$EXT_DIR/capture.js" << JSEOF
(function poll() {
  var links = document.querySelectorAll('a[href*="Kontakt_8_Installer.zip"]');
  if (!links.length) { setTimeout(poll, 1000); return; }
  fetch('http://localhost:${toString 19876}', { method: 'POST', body: links[0].href });
})();
JSEOF

              fuser -k "$PORT/tcp" 2>/dev/null || true
              ${pkgs.python3}/bin/python3 ${ni-url-server} "$URL_FILE" "$PORT" &
              SERVER_PID=$!

              echo "==> Log in to Native Instruments in the popup window."

              ${pkgs.chromium}/bin/chromium \
                --app="https://www.native-instruments.com/en/account/downloads/0e504595-40d8-4982-978e-a242f036912d" \
                --load-extension="$EXT_DIR" \
                --disable-extensions-except="$EXT_DIR" \
                --no-first-run \
                --no-default-browser-check \
                2>/dev/null &
              BROWSER_PID=$!

              while kill -0 "$BROWSER_PID" 2>/dev/null; do
                [ -s "$URL_FILE" ] && break
                sleep 1
              done

              kill "$BROWSER_PID" 2>/dev/null || true
              kill "$SERVER_PID" 2>/dev/null || true
              rm -rf "$EXT_DIR"

              URL=$(cat "$URL_FILE")
              rm -f "$URL_FILE"

              if [ -z "$URL" ]; then
                echo "==> Browser closed before URL was received. Aborting." >&2
                exit 1
              fi
              echo "==> URL captured."
            fi

            ZIP="/tmp/Kontakt_8_Installer.zip"
            TMP_DRIVE="/tmp/k8_drive_c"

            echo "==> Downloading Kontakt 8..."
            ${pkgs.curl}/bin/curl -L --progress-bar -o "$ZIP" "$URL"

            echo "==> Extracting..."
            OUT="$TMP_DRIVE" PATH="${pkgs.lib.makeBinPath [ pkgs.p7zip pkgs.unzip pkgs.msitools ]}:$PATH" \
              ${pkgs.python3}/bin/python3 ${./scripts/extract_kontakt8.py} "$ZIP"

            if [[ ! -d "$TMP_DRIVE" ]]; then
              echo "Error: extraction failed, aborting copy." >&2
              exit 1
            fi

            echo "==> Copying to Wine prefix..."
            mkdir -p "$WINEPREFIX/drive_c"
            cp -r "$TMP_DRIVE/." "$WINEPREFIX/drive_c/"

            echo "==> Cleaning up..."
            rm -rf "$TMP_DRIVE" "$ZIP"

            echo "==> Kontakt 8 installed."
            ;;

          --fix-msvcp140)
            shift
            if [ ! -d "$WINEPREFIX/drive_c/windows/system32" ]; then
              echo "Error: Wine prefix not initialized. Run ni-setup first." >&2
              exit 1
            fi

            TMPDIR="$(mktemp -d)"
            trap 'rm -rf "$TMPDIR"' EXIT

            echo "==> Downloading VC++ 2022 x64 redistributable..."
            ${pkgs.curl}/bin/curl -L --progress-bar \
              "https://aka.ms/vs/17/release/vc_redist.x64.exe" \
              -o "$TMPDIR/vc_redist.x64.exe"

            echo "==> Extracting cabinet..."
            ${pkgs.cabextract}/bin/cabextract -d "$TMPDIR/stage1" "$TMPDIR/vc_redist.x64.exe" 2>/dev/null

            INNER_CAB=""
            for f in "$TMPDIR/stage1"/a*; do
              if ${pkgs.cabextract}/bin/cabextract -l "$f" 2>/dev/null | grep -q "msvcp140.dll_amd64"; then
                INNER_CAB="$f"; break
              fi
            done

            if [[ -z "$INNER_CAB" ]]; then
              echo "ERROR: Could not find msvcp140.dll_amd64 cabinet" >&2
              exit 1
            fi

            ${pkgs.cabextract}/bin/cabextract -d "$TMPDIR/stage2" "$INNER_CAB" 2>/dev/null

            SYSTEM32="$WINEPREFIX/drive_c/windows/system32"
            copy_dll() {
              local src="$TMPDIR/stage2/$1" dst="$SYSTEM32/$2"
              [[ -f "$src" ]] && { echo "==> Installing $2"; cp "$src" "$dst"; } || echo "WARNING: $1 not found"
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
                ${wine}/bin/wine reg add "HKCU\\Software\\Wine\\DllOverrides" \
                /v "$dll" /t REG_SZ /d "native,builtin" /f 2>/dev/null || true
            done

            echo "==> msvcp140 fix applied."
            ;;

          --update-kontakt8)
            shift
            if [[ $# -gt 0 && "$1" != --* ]]; then
              URL="$1"; shift
            else
              URL=""
            fi

            ZIP="/tmp/Kontakt_8_Installer.zip"

            if [[ -n "$URL" ]]; then
              echo "==> Downloading Kontakt 8..."
              ${pkgs.curl}/bin/curl -L --progress-bar -o "$ZIP" "$URL"
            elif [[ ! -f "$ZIP" ]]; then
              PORT=19876
              URL_FILE=$(mktemp)
              EXT_DIR=$(mktemp -d)

              cat > "$EXT_DIR/manifest.json" << 'JSON'
{
  "manifest_version": 3,
  "name": "NI URL Capture",
  "version": "1.0",
  "content_scripts": [{
    "matches": ["https://www.native-instruments.com/*/account/downloads/*"],
    "js": ["capture.js"],
    "run_at": "document_idle"
  }]
}
JSON

              cat > "$EXT_DIR/capture.js" << JSEOF
(function poll() {
  var links = document.querySelectorAll('a[href*="Kontakt_8_Installer.zip"]');
  if (!links.length) { setTimeout(poll, 1000); return; }
  fetch('http://localhost:${toString 19876}', { method: 'POST', body: links[0].href });
})();
JSEOF

              fuser -k "$PORT/tcp" 2>/dev/null || true
              ${pkgs.python3}/bin/python3 ${ni-url-server} "$URL_FILE" "$PORT" &
              SERVER_PID=$!

              echo "==> Log in to Native Instruments in the popup window."

              ${pkgs.chromium}/bin/chromium                 --app="https://www.native-instruments.com/en/account/downloads/0e504595-40d8-4982-978e-a242f036912d"                 --load-extension="$EXT_DIR"                 --disable-extensions-except="$EXT_DIR"                 --no-first-run                 --no-default-browser-check                 2>/dev/null &
              BROWSER_PID=$!

              while kill -0 "$BROWSER_PID" 2>/dev/null; do
                [ -s "$URL_FILE" ] && break
                sleep 1
              done

              kill "$BROWSER_PID" 2>/dev/null || true
              kill "$SERVER_PID" 2>/dev/null || true
              rm -rf "$EXT_DIR"

              URL=$(cat "$URL_FILE")
              rm -f "$URL_FILE"

              if [ -z "$URL" ]; then
                echo "==> Browser closed before URL was received. Aborting." >&2
                exit 1
              fi
              echo "==> URL captured."
              echo "==> Downloading Kontakt 8..."
              ${pkgs.curl}/bin/curl -L --progress-bar -o "$ZIP" "$URL"
            fi

            TMP_DRIVE="/tmp/k8_drive_c"
            rm -rf "$TMP_DRIVE"

            echo "==> Extracting (update, overwrite enabled)..."
            K8_UPDATE=1 OUT="$TMP_DRIVE" \
              PATH="${pkgs.lib.makeBinPath [ pkgs.p7zip pkgs.unzip pkgs.msitools ]}:$PATH" \
              ${pkgs.python3}/bin/python3 ${./scripts/extract_kontakt8.py} "$ZIP"

            if [[ ! -d "$TMP_DRIVE" ]]; then
              echo "Error: extraction failed, aborting copy." >&2
              exit 1
            fi

            echo "==> Removing old Kontakt 8 files..."
            rm -rf "$WINEPREFIX/drive_c/Program Files/Native Instruments/Kontakt 8"
            rm -rf "$WINEPREFIX/drive_c/Program Files/Common Files/Native Instruments/Kontakt 8"
            rm -rf "$WINEPREFIX/drive_c/Program Files/Common Files/VST3/Kontakt 8.vst3"

            echo "==> Copying to Wine prefix..."
            mkdir -p "$WINEPREFIX/drive_c"
            cp -r "$TMP_DRIVE/." "$WINEPREFIX/drive_c/"

            echo "==> Cleaning up..."
            rm -rf "$TMP_DRIVE" "$ZIP"

            echo "==> Kontakt 8 updated."
            ;;

          --uninstall-kontakt8)
            shift
            echo "==> Removing Kontakt 8 files from Wine prefix..."

            rm -rf "$WINEPREFIX/drive_c/Program Files/Native Instruments/Kontakt 8"
            rm -rf "$WINEPREFIX/drive_c/Program Files/Common Files/Native Instruments/Kontakt 8"
            rm -rf "$WINEPREFIX/drive_c/Program Files/Common Files/VST3/Kontakt 8.vst3"
            rm -f  "$WINEPREFIX/drive_c/users/Public/Documents/Native Instruments/installed_products/Kontakt 8.json"

            echo "==> Kontakt 8 uninstalled."
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
        mkdir -p $out/share/icons/hicolor/scalable/apps
        cp ${./data/native-access.svg} $out/share/icons/hicolor/scalable/apps/native-access.svg
      '';
    };

  in {
    packages.${system}.default = native-instruments;

    devShells.${system}.default = pkgs.mkShell {
      packages = [
        native-instruments
        wine
        pkgs.winetricks
        pkgs.xvfb-run
        pkgs.xdotool
        pkgs.curl
        pkgs.p7zip
        pkgs.unzip
        pkgs.cabextract
        pkgs.xdg-utils
        pkgs.python3
        pkgs.chromium
        pkgs.yad
        pkgs.msitools
      ];

      shellHook = ''
        export WINEPREFIX="$HOME/.wine-ni"
        export WINEARCH="win64"
      '';
    };
  };
}