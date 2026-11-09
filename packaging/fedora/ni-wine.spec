Name:           ni-wine
Version:        2.5.0
Release:        1%{?dist}
Summary:        Native Instruments software under Wine
License:        GPL-3.0-or-later
URL:            https://github.com/selimbucher/native-instruments
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/ni-wine-%{version}.tar.gz

BuildRequires:  python3-devel
BuildRequires:  mingw32-gcc
BuildRequires:  mingw64-gcc
BuildRequires:  make
BuildRequires:  gcc
BuildRequires:  pkgconfig(alsa)
BuildRequires:  desktop-file-utils
BuildRequires:  python3-pytest
BuildRequires:  systemd-rpm-macros

Requires:       (wine >= 11 or winehq-staging >= 11 or winehq-stable >= 11 or winehq-devel >= 11)
Requires:       winetricks
Requires:       cabextract
Requires:       (7zip or p7zip-plugins)
Requires:       msitools
Requires:       alsa-lib
Requires:       procps-ng
Requires:       xorg-x11-server-Xvfb
Requires:       zenity
Requires:       xdg-utils
Requires:       desktop-file-utils
Requires:       hicolor-icon-theme
Recommends:     xdotool


%description
Sets up and runs Native Access and Native Instruments products under
Wine: a dedicated prefix with the required tweaks, a working Kontakt 8
install path, and diagnostics for the common failure modes.

%prep
%autosetup -n native-instruments-%{version}
%generate_buildrequires
%pyproject_buildrequires

%build
# the msi shim and the controller support have to be in the package data before the wheel is built
make -C shim CC=i686-w64-mingw32-gcc
cp shim/msi_shim32.dll src/ni_wine/data/msi_shim32.dll
make -C hardware install CC=x86_64-w64-mingw32-gcc
%set_build_flags
%make_build -C maschine
%pyproject_wheel

%install
%pyproject_install
%pyproject_save_files ni_wine
install -Dm755 maschine/ni-maschine %{buildroot}%{_bindir}/ni-maschine
# desktop file + icon by hand into %%{buildroot}%%{_datadir}/...
install -Dm644 src/ni_wine/data/native-access.desktop \
    %{buildroot}%{_datadir}/applications/native-access.desktop
install -Dm644 src/ni_wine/data/native-access.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/native-access.svg
install -Dm644 src/ni_wine/data/70-ni-wine.rules %{buildroot}%{_udevrulesdir}/70-ni-wine.rules

%check
%pyproject_check_import
%pytest
make -C maschine check
desktop-file-validate %{buildroot}%{_datadir}/applications/native-access.desktop

%files -f %{pyproject_files}
%license LICENSE
%doc README.md
%{_bindir}/ni
%{_bindir}/native-access
%{_bindir}/ni-maschine
%{_datadir}/applications/native-access.desktop
%{_datadir}/icons/hicolor/scalable/apps/native-access.svg
%{_udevrulesdir}/70-ni-wine.rules


%changelog
* Mon Nov 02 2026 Selim Bucher <me@selim.one> - 2.5.0-1
- Release 2.5.0

* Fri Oct 30 2026 Selim Bucher <me@selim.one> - 2.4.4-1
- Release 2.4.4

* Mon Oct 19 2026 Selim Bucher <me@selim.one> - 2.4.3-1
- Release 2.4.3

* Sun Sep 27 2026 Selim Bucher <me@selim.one> - 2.4.2-1
- Release 2.4.2

* Mon Sep 21 2026 Selim Bucher <me@selim.one> - 2.4.1-1
- Release 2.4.1

* Sat Sep 19 2026 Selim Bucher <me@selim.one> - 2.4.0-1
- Release 2.4.0

* Wed Sep 16 2026 Selim Bucher <me@selim.one> - 2.3.1-1
- Release 2.3.1

* Tue Sep 15 2026 Selim Bucher <me@selim.one> - 2.3.0-1
- Initial COPR packaging