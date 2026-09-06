# WrestleFest native engine — self-contained build (run from this directory).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
DEFS    :=
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
SRC := $(wildcard src/*.c) $(wildcard src/vendor/*.c) $(wildcard tools/*.c)
wfengine: $(SRC) src/engine.h src/wf.h src/rules.def
	$(CC) $(CFLAGS) $(DEFS) $(SDL_CFLAGS) -Isrc -o $@ $(SRC) -lm -lpng $(SDL_LIBS)
# Runtime paks (build/, git-ignored) from the data tree; build.sh runs this.
pack: wfengine
	@mkdir -p build
	@./wfengine --pack data/tables build/base.pak 2>&1 | grep -E "tables ->|wrestlers ->|gfx ->|pack-weapons|FAIL|failed" | grep -v " 0 failed" || true
regress: wfengine
	@test -f build/base.pak || $(MAKE) -s pack
	@./wfengine --selftest --frames 1200 2>/dev/null | tee /tmp/wfe-selftest.txt
	@diff -q baseline.txt /tmp/wfe-selftest.txt && echo "engine-regress: OK"
baseline: wfengine
	./wfengine --selftest --frames 1200 2>/dev/null > baseline.txt
	@cat baseline.txt
clean:
	rm -f wfengine
	rm -rf build
.PHONY: regress baseline clean pack
