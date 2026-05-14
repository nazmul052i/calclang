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

# Numerical-Recipes-style algorithms (branch `numerical-library`).
nr_libs: all
	mkdir -p $(CALCLIB)
	$(BUILD)/calcnat --lib lib/nr/brent.calc   -o $(CALCLIB)/nr_brent.s
	$(BUILD)/calcnat --lib lib/nr/spline.calc  -o $(CALCLIB)/nr_spline.s
	$(BUILD)/calcnat --lib lib/nr/special.calc -o $(CALCLIB)/nr_special.s
	$(BUILD)/calcnat --lib lib/nr/eigen.calc   -o $(CALCLIB)/nr_eigen.s
	$(BUILD)/calcnat --lib lib/nr/lu.calc      -o $(CALCLIB)/nr_lu.s
	$(BUILD)/calcnat --lib lib/nr/romberg.calc -o $(CALCLIB)/nr_romberg.s
	$(BUILD)/calcnat --lib lib/nr/rk45.calc     -o $(CALCLIB)/nr_rk45.s
	$(BUILD)/calcnat --lib lib/nr/poly.calc     -o $(CALCLIB)/nr_poly.s
	$(BUILD)/calcnat --lib lib/nr/minimize.calc    -o $(CALCLIB)/nr_minimize.s
	$(BUILD)/calcnat --lib lib/nr/sort.calc        -o $(CALCLIB)/nr_sort.s
	$(BUILD)/calcnat --lib lib/nr/diff.calc        -o $(CALCLIB)/nr_diff.s
	$(BUILD)/calcnat --lib lib/nr/random_dist.calc -o $(CALCLIB)/nr_random_dist.s
	$(BUILD)/calcnat --lib lib/nr/newton.calc      -o $(CALCLIB)/nr_newton.s
	$(BUILD)/calcnat --lib lib/nr/fitnl.calc       -o $(CALCLIB)/nr_fitnl.s
	$(BUILD)/calcnat --lib lib/nr/qr.calc          -o $(CALCLIB)/nr_qr.s
	$(BUILD)/calcnat --lib lib/nr/svd.calc         -o $(CALCLIB)/nr_svd.s
	$(BUILD)/calcnat --lib lib/nr/polyroots.calc   -o $(CALCLIB)/nr_polyroots.s
	$(BUILD)/calcnat --lib lib/nr/conv.calc        -o $(CALCLIB)/nr_conv.s
	$(BUILD)/calcnat --lib lib/nr/cheb.calc        -o $(CALCLIB)/nr_cheb.s
	$(BUILD)/calcnat --lib lib/nr/savgol.calc      -o $(CALCLIB)/nr_savgol.s
	$(BUILD)/calcnat --lib lib/nr/kalman.calc      -o $(CALCLIB)/nr_kalman.s
	$(BUILD)/calcnat --lib lib/nr/pde.calc         -o $(CALCLIB)/nr_pde.s
	$(BUILD)/calcnat --lib lib/nr/cholesky.calc    -o $(CALCLIB)/nr_cholesky.s
	$(BUILD)/calcnat --lib lib/nr/cg.calc          -o $(CALCLIB)/nr_cg.s
	$(BUILD)/calcnat --lib lib/nr/anneal.calc      -o $(CALCLIB)/nr_anneal.s
	$(BUILD)/calcnat --lib lib/nr/mcmc.calc        -o $(CALCLIB)/nr_mcmc.s
	$(BUILD)/calcnat --lib lib/nr/welch.calc       -o $(CALCLIB)/nr_welch.s
	$(BUILD)/calcnat --lib lib/nr/wavelet.calc     -o $(CALCLIB)/nr_wavelet.s
	$(BUILD)/calcnat --lib lib/nr/toeplitz.calc    -o $(CALCLIB)/nr_toeplitz.s
	$(BUILD)/calcnat --lib lib/nr/simplex_lp.calc  -o $(CALCLIB)/nr_simplex_lp.s
	$(BUILD)/calcnat --lib lib/nr/fft2d.calc       -o $(CALCLIB)/nr_fft2d.s
	$(BUILD)/calcnat --lib lib/nr/quad2d.calc      -o $(CALCLIB)/nr_quad2d.s
	$(BUILD)/calcnat --lib lib/nr/power_eigen.calc -o $(CALCLIB)/nr_power_eigen.s
	$(BUILD)/calcnat --lib lib/nr/bspline.calc     -o $(CALCLIB)/nr_bspline.s

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

nr_demo: nr_libs
	$(BUILD)/calcnat examples/nr_demo.calc \
	    $(CALCLIB)/nr_brent.s $(CALCLIB)/nr_spline.s \
	    $(CALCLIB)/nr_special.s $(CALCLIB)/nr_eigen.s -o $(BUILD)/nr_demo
	$(BUILD)/nr_demo

nr_demo2: nr_libs
	$(BUILD)/calcnat examples/nr_demo2.calc \
	    $(CALCLIB)/nr_lu.s $(CALCLIB)/nr_romberg.s \
	    $(CALCLIB)/nr_rk45.s $(CALCLIB)/nr_poly.s -o $(BUILD)/nr_demo2
	$(BUILD)/nr_demo2

nr_demo3: nr_libs
	$(BUILD)/calcnat examples/nr_demo3.calc \
	    $(CALCLIB)/nr_minimize.s $(CALCLIB)/nr_sort.s -o $(BUILD)/nr_demo3
	$(BUILD)/nr_demo3

nr_demo4: nr_libs libs
	$(BUILD)/calcnat examples/nr_demo4.calc \
	    $(CALCLIB)/nr_diff.s $(CALCLIB)/nr_random_dist.s \
	    $(CALCLIB)/nr_newton.s $(CALCLIB)/nr_fitnl.s \
	    $(CALCLIB)/nr_lu.s $(CALCLIB)/random.s -o $(BUILD)/nr_demo4
	$(BUILD)/nr_demo4

nr_demo5: nr_libs libs
	$(BUILD)/calcnat examples/nr_demo5.calc \
	    $(CALCLIB)/nr_qr.s $(CALCLIB)/nr_svd.s \
	    $(CALCLIB)/nr_polyroots.s $(CALCLIB)/nr_conv.s \
	    $(CALCLIB)/nr_eigen.s $(CALCLIB)/fft.s -o $(BUILD)/nr_demo5
	$(BUILD)/nr_demo5

nr_demo6: nr_libs libs
	$(BUILD)/calcnat examples/nr_demo6.calc \
	    $(CALCLIB)/nr_cheb.s $(CALCLIB)/nr_savgol.s \
	    $(CALCLIB)/nr_kalman.s $(CALCLIB)/nr_pde.s \
	    $(CALCLIB)/nr_lu.s $(CALCLIB)/random.s -o $(BUILD)/nr_demo6
	$(BUILD)/nr_demo6

nr_demo7: nr_libs libs
	$(BUILD)/calcnat examples/nr_demo7.calc \
	    $(CALCLIB)/nr_cholesky.s $(CALCLIB)/nr_cg.s \
	    $(CALCLIB)/nr_anneal.s $(CALCLIB)/nr_mcmc.s \
	    $(CALCLIB)/random.s -o $(BUILD)/nr_demo7
	$(BUILD)/nr_demo7

nr_demo8: nr_libs libs
	$(BUILD)/calcnat examples/nr_demo8.calc \
	    $(CALCLIB)/nr_welch.s $(CALCLIB)/nr_wavelet.s \
	    $(CALCLIB)/nr_toeplitz.s $(CALCLIB)/nr_simplex_lp.s \
	    $(CALCLIB)/fft.s $(CALCLIB)/random.s -o $(BUILD)/nr_demo8
	$(BUILD)/nr_demo8

nr_demo9: nr_libs libs
	$(BUILD)/calcnat examples/nr_demo9.calc \
	    $(CALCLIB)/nr_fft2d.s $(CALCLIB)/nr_quad2d.s \
	    $(CALCLIB)/nr_power_eigen.s $(CALCLIB)/nr_bspline.s \
	    $(CALCLIB)/nr_lu.s $(CALCLIB)/nr_qr.s $(CALCLIB)/fft.s \
	    -o $(BUILD)/nr_demo9
	$(BUILD)/nr_demo9

demos: sine_plot regression linsys numerical monte_carlo csv_demo \
       multi_plot json_demo ode_demo fft_demo nr_demo nr_demo2 nr_demo3 nr_demo4 nr_demo5 nr_demo6 nr_demo7 nr_demo8 nr_demo9

test: all
	sh tests/run_tests.sh

clean:
	rm -rf $(BUILD)
