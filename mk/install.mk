install: bin/callsign-lookup
	@for i in ${real_bins}; do \
		install -m 0755 $$i ${bin_install_path}/$$i; \
	done
