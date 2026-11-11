Name: rustyrig-callsign-lookup
Version: 1.0.4
Release: 1%{?dist}
Summary: Standalone RustyRig callsign lookup helper
License: MIT
URL: https://github.com/PripyatAutomations/callsign-lookup
Source0: %{name}-%{version}.tar.gz
BuildRequires: gcc, make, pkgconfig
BuildRequires: pkgconfig(librustyaxe), pkgconfig(glib-2.0)
BuildRequires: pkgconfig(libcurl), pkgconfig(sqlite3)
BuildRequires: libev-devel, libspatialite-devel, libbsd-devel, sqlite
Requires: librustyaxe

%description
Standalone FCC/ULS callsign lookup and optional QRZ XML HTTPS integration,
used by RustyRig and other applications.

%prep
%autosetup -n callsign-lookup-%{version}

%build
%make_build world RUSTYRIG_FW_BUILD=false LIBRUSTYAXE_PROVIDER=system

%install
%make_install RUSTYRIG_FW_BUILD=false LIBRUSTYAXE_PROVIDER=system PREFIX=/usr
install -Dpm0644 packaging/callsign-lookup.cfg %{buildroot}%{_sysconfdir}/rustyrig/callsign-lookup.cfg

%files
%{_bindir}/callsign-lookup
%config(noreplace) %{_sysconfdir}/rustyrig/callsign-lookup.cfg
%doc README etc/config.example.json
