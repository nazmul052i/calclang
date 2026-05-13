CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -Iinclude
BUILD := build
SRC := src

COMMON := $(SRC)/common.c
CALCLIB := $(SRC)/calclib.c
COMMON_FRONTEND := $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/symbol_table.c $(SRC)/codegen.c $(SRC)/type_infer.c

.PHONY: all clean example test

all: $(BUILD)/calcc $(BUILD)/calcasm $(BUILD)/calcld $(BUILD)/calcvm $(BUILD)/calcnat

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/calcc: $(BUILD) $(COMMON) $(CALCLIB) $(COMMON_FRONTEND) $(SRC)/calcc.c include/*.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(COMMON_FRONTEND) $(SRC)/calcc.c -o $@

$(BUILD)/calcasm: $(BUILD) $(COMMON) $(SRC)/calcasm.c include/common.h include/insn.h
	$(CC) $(CFLAGS) $(COMMON) $(SRC)/calcasm.c -o $@

$(BUILD)/calcld: $(BUILD) $(COMMON) $(SRC)/calcld.c include/common.h include/insn.h
	$(CC) $(CFLAGS) $(COMMON) $(SRC)/calcld.c -o $@

$(BUILD)/calcvm: $(BUILD) $(COMMON) $(CALCLIB) $(SRC)/calcvm.c include/common.h include/insn.h include/calclib.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(SRC)/calcvm.c -o $@

$(BUILD)/calcnat: $(BUILD) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/codegen_x64.c $(SRC)/type_infer.c $(SRC)/calcnat.c include/*.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/codegen_x64.c $(SRC)/type_infer.c $(SRC)/calcnat.c -o $@

example: all
	$(BUILD)/calcc examples/demo.calc $(BUILD)/demo.casm
	$(BUILD)/calcasm $(BUILD)/demo.casm $(BUILD)/demo.co
	$(BUILD)/calcld $(BUILD)/demo.co $(BUILD)/demo.cexe
	$(BUILD)/calcvm $(BUILD)/demo.cexe

# Build a native executable from a CalcLang source via the x86-64
# backend + runtime. Usage: `make native SRC=path/to/foo.calc`
NATIVE_SRC ?= tests/native_strings.calc
native: all
	$(BUILD)/calcnat $(NATIVE_SRC) $(BUILD)/native.s
	$(CC) $(BUILD)/native.s $(SRC)/runtime_x64.c -Iinclude -o $(BUILD)/native
	$(BUILD)/native

# Engineering library demos. The `lib/` modules compile once with
# `--lib` and the demos link against the resulting .s files. Each
# demo produces an SVG (if it plots) and prints a quick summary.

CALCLIB := $(BUILD)/calclib

.PHONY: libs sine_plot regression linsys numerical monte_carlo csv_demo demos

libs: all
	mkdir -p $(CALCLIB)
	$(BUILD)/calcnat --lib lib/linalg.calc  -o $(CALCLIB)/linalg.s
	$(BUILD)/calcnat --lib lib/stats.calc   -o $(CALCLIB)/stats.s
	$(BUILD)/calcnat --lib lib/plot.calc    -o $(CALCLIB)/plot.s
	$(BUILD)/calcnat --lib lib/numeric.calc -o $(CALCLIB)/numeric.s
	$(BUILD)/calcnat --lib lib/csv.calc     -o $(CALCLIB)/csv.s
	$(BUILD)/calcnat --lib lib/random.calc  -o $(CALCLIB)/random.s
	$(BUILD)/calcnat --lib lib/json.calc    -o $(CALCLIB)/json.s
	$(BUILD)/calcnat --lib lib/ode.calc     -o $(CALCLIB)/ode.s
	$(BUILD)/calcnat --lib lib/fft.calc     -o $(CALCLIB)/fft.s

sine_plot: libs
	$(BUILD)/calcnat examples/sine_plot.calc $(CALCLIB)/plot.s -o $(BUILD)/sine_plot
	$(BUILD)/sine_plot

regression: libs
	$(BUILD)/calcnat examples/regression.calc $(CALCLIB)/stats.s $(CALCLIB)/plot.s -o $(BUILD)/regression
	$(BUILD)/regression

linsys: libs
	$(BUILD)/calcnat examples/linsys.calc $(CALCLIB)/linalg.s -o $(BUILD)/linsys
	$(BUILD)/linsys

numerical: libs
	$(BUILD)/calcnat examples/numerical.calc $(CALCLIB)/numeric.s -o $(BUILD)/numerical
	$(BUILD)/numerical

monte_carlo: libs
	$(BUILD)/calcnat examples/monte_carlo.calc $(CALCLIB)/random.s $(CALCLIB)/stats.s -o $(BUILD)/monte_carlo
	$(BUILD)/monte_carlo

csv_demo: libs
	$(BUILD)/calcnat examples/csv_demo.calc $(CALCLIB)/csv.s $(CALCLIB)/stats.s $(CALCLIB)/plot.s -o $(BUILD)/csv_demo
	$(BUILD)/csv_demo

multi_plot: libs
	$(BUILD)/calcnat examples/multi_plot.calc $(CALCLIB)/plot.s -o $(BUILD)/multi_plot
	$(BUILD)/multi_plot

json_demo: libs
	$(BUILD)/calcnat examples/json_demo.calc $(CALCLIB)/json.s -o $(BUILD)/json_demo
	$(BUILD)/json_demo

ode_demo: libs
	$(BUILD)/calcnat examples/ode_demo.calc $(CALCLIB)/ode.s $(CALCLIB)/plot.s -o $(BUILD)/ode_demo
	$(BUILD)/ode_demo

fft_demo: libs
	$(BUILD)/calcnat examples/fft_demo.calc $(CALCLIB)/fft.s $(CALCLIB)/random.s $(CALCLIB)/plot.s -o $(BUILD)/fft_demo
	$(BUILD)/fft_demo

demos: sine_plot regression linsys numerical monte_carlo csv_demo \
       multi_plot json_demo ode_demo fft_demo

test: all
	sh tests/run_tests.sh

clean:
	rm -rf $(BUILD)
