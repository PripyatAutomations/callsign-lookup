obj/%.o: src/%.c .version $(wildcard include/*.h)
	@echo "[CC] $< -> $@"
	@${CC} ${CFLAGS} -DVERSION=\"$(VERSION)\" -o $@ -c $<
