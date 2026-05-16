#!/bin/sh
set -e
make
bin/calcc examples/demo.calc build/demo.casm
bin/calcasm build/demo.casm build/demo.co
bin/calcld build/demo.co build/demo.cexe
bin/calcvm build/demo.cexe
