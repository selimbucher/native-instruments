Name:           ni-wine
Version:        2.3.1
Release:        1%{?dist}
Summary:        Native Instruments software under Wine
License:        MIT
URL:            https://github.com/selimbucher/native-instruments
Source0:        %{url}/archive/refs/tags/v%{version}.tar.gz#/ni-wine-%{version}.tar.gz
BuildArch:      noarch

BuildRequires:  python3-devel
BuildRequires:  mingw32-gcc
BuildRequires:  make
BuildRequires:  desktop-file-utils

Requires:       wine >= 11
Requires:       winetricks
Requires:       cabextract
Requires:       (7zip or p7zip-plugins)
Requires:       msitools
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
# The Kontakt-installer msi shim is cross-compiled at build time rather
# than shipped as a binary; it must land in the package data before the
# wheel is built, because the wheel snapshots it.
make -C shim CC=i686-w64-mingw32-gcc
cp shim/msi_shim32.dll src/ni_wine/data/msi_shim32.dll
%pyproject_wheel

%install
%pyproject_install
%pyproject_save_files ni_wine
# desktop file + icon by hand into %%{buildroot}%%{_datadir}/...
install -Dm644 src/ni_wine/data/native-access.desktop \
    %{buildroot}%{_datadir}/applications/native-access.desktop
install -Dm644 src/ni_wine/data/native-access.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/native-access.svg

%check
%pyproject_check_import
desktop-file-validate %{buildroot}%{_datadir}/applications/native-access.desktop

%files -f %{pyproject_files}
%license LICENSE
%doc README.md
%{_bindir}/ni
%{_bindir}/native-access
%{_datadir}/applications/native-access.desktop
%{_datadir}/icons/hicolor/scalable/apps/native-access.svg


%changelog
* Wed Sep 16 2026 Selim Bucher <me@selim.one> - 2.3.1-1
- Release 2.3.1

* Tue Sep 15 2026 Selim Bucher <me@selim.one> - 2.3.0-1
- Initial COPR packaging