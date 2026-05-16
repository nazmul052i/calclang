CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -Iinclude
BIN := bin
BUILD := build
SRC := src

COMMON := $(SRC)/common.c
CALCLIB := $(SRC)/calclib.c
COMMON_FRONTEND := $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/symbol_table.c $(SRC)/codegen.c $(SRC)/type_infer.c $(SRC)/optimizer.c

.PHONY: all clean example test install uninstall

# Install layout: a self-contained subtree at $(PREFIX)/calclang/.
# `clc` (and the six sibling tools) live in <subtree>/bin/. At runtime
# they derive CALC_HOME = <bin_dir>/.., which makes the rest of the
# subtree (lib/, src/, include/, third_party/) discoverable.
PREFIX ?= /usr/local
CALCLANG_PREFIX := $(PREFIX)/calclang

all: $(BIN)/calcc $(BIN)/calcasm $(BIN)/calcld $(BIN)/calcvm $(BIN)/calcnat $(BIN)/calcwasm $(BIN)/clc

$(BIN):
	mkdir -p $(BIN)

$(BUILD):
	mkdir -p $(BUILD)

$(BIN)/calcc: $(BIN) $(COMMON) $(CALCLIB) $(COMMON_FRONTEND) $(SRC)/calcc.c include/*.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(COMMON_FRONTEND) $(SRC)/calcc.c -o $@

$(BIN)/calcasm: $(BIN) $(COMMON) $(SRC)/calcasm.c include/common.h include/insn.h
	$(CC) $(CFLAGS) $(COMMON) $(SRC)/calcasm.c -o $@

$(BIN)/calcld: $(BIN) $(COMMON) $(SRC)/calcld.c include/common.h include/insn.h
	$(CC) $(CFLAGS) $(COMMON) $(SRC)/calcld.c -o $@

$(BIN)/calcvm: $(BIN) $(COMMON) $(CALCLIB) $(SRC)/calcvm.c include/common.h include/insn.h include/calclib.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(SRC)/calcvm.c -o $@

$(BIN)/calcnat: $(BIN) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/codegen_x64.c $(SRC)/type_infer.c $(SRC)/optimizer.c $(SRC)/calcnat.c include/*.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/codegen_x64.c $(SRC)/type_infer.c $(SRC)/optimizer.c $(SRC)/calcnat.c -o $@

# Unified gcc-style driver. Spawns the six sibling compiler tools and
# hosts the package manager (clc init / clc install). Uses common.c for
# string helpers and the install-paths seam.
$(BIN)/clc: $(BIN) $(COMMON) $(SRC)/clc.c include/version.h include/common.h
	$(CC) $(CFLAGS) $(COMMON) $(SRC)/clc.c -o $@

# WebAssembly backend (Stage 1, numeric subset). Output is .wat text.
# Run with `python tools/wasm_host.py foo.wat` (needs `pip install wasmtime`).
$(BIN)/calcwasm: $(BIN) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/symbol_table.c $(SRC)/codegen_wasm.c $(SRC)/type_infer.c $(SRC)/optimizer.c $(SRC)/calcwasm.c include/*.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/symbol_table.c $(SRC)/codegen_wasm.c $(SRC)/type_infer.c $(SRC)/optimizer.c $(SRC)/calcwasm.c -o $@

example: all $(BUILD)
	$(BIN)/calcc examples/demo.clc $(BUILD)/demo.casm
	$(BIN)/calcasm $(BUILD)/demo.casm $(BUILD)/demo.co
	$(BIN)/calcld $(BUILD)/demo.co $(BUILD)/demo.cexe
	$(BIN)/calcvm $(BUILD)/demo.cexe

# Build a native executable from a CalcLang source via the x86-64
# backend + runtime. Usage: `make native SRC=path/to/foo.clc`
NATIVE_SRC ?= tests/native_strings.clc
native: all $(BUILD)
	$(BIN)/calcnat $(NATIVE_SRC) $(BUILD)/native.s
	$(CC) $(BUILD)/native.s $(SRC)/runtime_x64.c -Iinclude -o $(BUILD)/native
	$(BUILD)/native

# Engineering library demos. Each example pulls in its libraries via
# `import "name";` — no separate --lib step or .s files on the link
# line. One command per demo.

.PHONY: math_demo sine_plot regression linsys numerical monte_carlo csv_demo
.PHONY: multi_plot json_demo ode_demo fft_demo
.PHONY: nr_demo nr_demo2 nr_demo3 nr_demo4 nr_demo5
.PHONY: nr_demo6 nr_demo7 nr_demo8 nr_demo9 nr_demo10
.PHONY: demos

# One-liner pattern: build the demo from the .clc, then run it.
define DEMO_template
$(1): all $$(BUILD)
	$$(BIN)/calcnat examples/$(1).clc -o $$(BUILD)/$(1)
	$$(BUILD)/$(1)
endef

# Apply the template to every demo.
$(eval $(call DEMO_template,math_demo))
$(eval $(call DEMO_template,sine_plot))
$(eval $(call DEMO_template,regression))
$(eval $(call DEMO_template,linsys))
$(eval $(call DEMO_template,numerical))
$(eval $(call DEMO_template,monte_carlo))
$(eval $(call DEMO_template,csv_demo))
$(eval $(call DEMO_template,multi_plot))
$(eval $(call DEMO_template,json_demo))
$(eval $(call DEMO_template,ode_demo))
$(eval $(call DEMO_template,fft_demo))
$(eval $(call DEMO_template,nr_demo))
$(eval $(call DEMO_template,nr_demo2))
$(eval $(call DEMO_template,nr_demo3))
$(eval $(call DEMO_template,nr_demo4))
$(eval $(call DEMO_template,nr_demo5))
$(eval $(call DEMO_template,nr_demo6))
$(eval $(call DEMO_template,nr_demo7))
$(eval $(call DEMO_template,nr_demo8))
$(eval $(call DEMO_template,nr_demo9))
$(eval $(call DEMO_template,nr_demo10))

demos: math_demo sine_plot regression linsys numerical monte_carlo csv_demo \
       multi_plot json_demo ode_demo fft_demo \
       nr_demo nr_demo2 nr_demo3 nr_demo4 nr_demo5 \
       nr_demo6 nr_demo7 nr_demo8 nr_demo9 nr_demo10

test: all
	sh tests/run_tests.sh

# Install the toolchain to $(CALCLANG_PREFIX). Default prefix is
# /usr/local — override with `make install PREFIX=/somewhere/else`.
# On Windows, e.g. `make install PREFIX="$PROGRAMFILES/CalcLang"`.
# After installing, add $(CALCLANG_PREFIX)/bin to your PATH.
install: all
	mkdir -p $(CALCLANG_PREFIX)/bin
	mkdir -p $(CALCLANG_PREFIX)/lib
	mkdir -p $(CALCLANG_PREFIX)/src
	mkdir -p $(CALCLANG_PREFIX)/include
	mkdir -p $(CALCLANG_PREFIX)/third_party
	cp -f $(BIN)/* $(CALCLANG_PREFIX)/bin/
	cp -rf lib/. $(CALCLANG_PREFIX)/lib/
	cp -f $(SRC)/runtime_x64.c $(SRC)/regex.c $(SRC)/runtime_gui_sdl2.c $(CALCLANG_PREFIX)/src/
	cp -rf include/. $(CALCLANG_PREFIX)/include/
	@if [ -d third_party/stb ]; then \
		cp -rf third_party/stb $(CALCLANG_PREFIX)/third_party/; \
	fi
	@if [ -d third_party/SDL2-2.30.10 ]; then \
		cp -rf third_party/SDL2-2.30.10 $(CALCLANG_PREFIX)/third_party/; \
		echo "Bundled vendored SDL2 + stb (GUI demos will work post-install)."; \
	else \
		echo "Note: third_party/SDL2-2.30.10 not present — run tools/setup_sdl2.sh first if you need GUI."; \
	fi
	@echo ""
	@echo "Installed to $(CALCLANG_PREFIX)"
	@echo "Add $(CALCLANG_PREFIX)/bin to your PATH, then 'clc foo.clc' works from any directory."

uninstall:
	rm -rf $(CALCLANG_PREFIX)
	@echo "Removed $(CALCLANG_PREFIX)"

clean:
	rm -rf $(BIN) $(BUILD)
