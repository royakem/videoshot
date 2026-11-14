Name:           videoshot
Version:        0.1.0
Release:        1%{?dist}
Summary:        Screenshots and short video clips of your screen

License:        GPL-2.0-or-later
URL:            https://github.com/royakem/videoshot
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  meson
BuildRequires:  gettext
BuildRequires:  desktop-file-utils
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(libhandy-1) >= 1.5.0
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xext)

# The recorder runs the ffmpeg program. Any package that provides it will do.
Requires:       /usr/bin/ffmpeg

%description
Videoshot takes pictures and short video clips of the whole screen, of a
window or of a selected area, and saves them to a file or copies them to
the clipboard. Video clips are saved as MP4, WebM, Matroska or animated GIF.

Videoshot is a fork of GNOME Screenshot as shipped by Linux Mint. Recording
needs an X11 session.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install
%find_lang %{name}

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/com.royake.Videoshot.desktop

%files -f %{name}.lang
%license COPYING
%doc README.md NEWS AUTHORS.md
%{_bindir}/videoshot
%{_datadir}/applications/com.royake.Videoshot.desktop
%{_datadir}/dbus-1/services/com.royake.Videoshot.service
%{_datadir}/glib-2.0/schemas/com.royake.videoshot.gschema.xml
%{_datadir}/icons/hicolor/*/apps/com.royake.Videoshot*.svg
%{_datadir}/metainfo/com.royake.Videoshot.metainfo.xml
%{_mandir}/man1/videoshot.1*

%changelog
* Sat Nov 14 2026 Roy-Åke Martinsson - 0.1.0-1
- First release
