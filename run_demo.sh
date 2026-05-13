#!/bin/sh
set -e
make
build/calcc examples/demo.calc build/demo.casm
build/calcasm build/demo.casm build/demo.co
build/calcld build/demo.co build/demo.cexe
build/calcvm build/demo.cexe
