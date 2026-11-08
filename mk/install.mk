# DESTDIR permits staged installs and uninstall without changing the host.
.PHONY: install uninstall
install: ${CALLSIGN_BIN_DIR}/callsign-lookup
	install -Dm755 "${CALLSIGN_BIN_DIR}/callsign-lookup" "$(DESTDIR)$(PREFIX)/bin/callsign-lookup"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/callsign-lookup"
