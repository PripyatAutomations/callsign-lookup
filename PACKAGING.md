# Standalone callsign-lookup packaging

This is its own source project and package named **rustyrig-callsign-lookup**.
It depends on installed librustyaxe, not librrprotocol or rustyrig-fw.
The package owns /usr/bin/callsign-lookup and
/etc/rustyrig/callsign-lookup.cfg. Updating the older RustyRig-produced binary
package uses the same name at a newer version (1.0.3-1).

Build source or local packages independently:

```sh
make RUSTYRIG_FW_BUILD=false LIBRUSTYAXE_PROVIDER=system
make deb
make src-deb
# Arch: cd packaging && makepkg -s
# RPM: rpmbuild -ba packaging/rustyrig-callsign-lookup.spec
```

The QRZ XML client already uses libcurl HTTPS/TLS. This application does not
speak WebSocket; wslay is therefore not part of its runtime dependencies.
Do not add Mongoose as an SDK or public-header dependency.

Source installs support PREFIX/DESTDIR and make uninstall; use the system
package manager to remove packaged installations.
