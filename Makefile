CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -Iinclude
BUILD := build
SRC := src

COMMON := $(SRC)/common.c
CALCLIB := $(SRC)/calclib.c
COMMON_FRONTEND := $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/symbol_table.c $(SRC)/codegen.c $(SRC)/type_infer.c $(SRC)/optimizer.c

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

$(BUILD)/calcnat: $(BUILD) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/codegen_x64.c $(SRC)/type_infer.c $(SRC)/optimizer.c $(SRC)/calcnat.c include/*.h
	$(CC) $(CFLAGS) $(COMMON) $(CALCLIB) $(SRC)/lexer.c $(SRC)/ast.c $(SRC)/parser.c $(SRC)/codegen_x64.c $(SRC)/type_infer.c $(SRC)/optimizer.c $(SRC)/calcnat.c -o $@

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

# Engineering library demos. Each example pulls in its libraries via
# `import "name";` — no separate --lib step or .s files on the link
# line. One command per demo.

.PHONY: math_demo sine_plot regression linsys numerical monte_carlo csv_demo
.PHONY: multi_plot json_demo ode_demo fft_demo
.PHONY: nr_demo nr_demo2 nr_demo3 nr_demo4 nr_demo5
.PHONY: nr_demo6 nr_demo7 nr_demo8 nr_demo9 nr_demo10
.PHONY: demos

# One-liner pattern: build the demo from the .calc, then run it.
define DEMO_template
$(1): all
	$$(BUILD)/calcnat examples/$(1).calc -o $$(BUILD)/$(1)
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

clean:
	rm -rf $(BUILD)
