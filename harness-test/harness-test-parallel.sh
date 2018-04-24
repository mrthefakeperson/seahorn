#!/bin/sh

#BASE="/ag/git/sv-benchmarks/c"
#BENCHMARK_FILE="$BASE/DeviceDriversLinux64.false-unreach-call.set"
cd harnesses/$1

BENCHMARK_FILE="../../files.txt"

parallel -a $BENCHMARK_FILE --max-procs 16 ./verify-harness.sh {}
