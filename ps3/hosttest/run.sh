#!/bin/sh
# Builds and runs the host test. Run `make` in ps3/ first (the game objects
# are the PS3 ones). Usage: hosttest/run.sh <rom.z64> <frames> [frame:buttons ...]
set -e
cd "$(dirname "$0")/.."
O=${MK64_OBJ:-build/obj}   # MK64_OBJ=build/obj-test for the TEST=1 build
OBJS=$(find $O/game $O/jp $O/sup $O/gen -name '*.o')
powerpc64-linux-gnu-gcc -O1 -g -Wall -Isource -c hosttest/host_main.c -o build/host_main.o
powerpc64-linux-gnu-gcc -O2 -Wall -Isource -c hosttest/host_rsx.c -o build/host_rsx.o
powerpc64-linux-gnu-gcc -static -o build/mk64host build/host_main.o build/host_rsx.o $OBJS -lm -lpthread
exec qemu-ppc64 -cpu power9 build/mk64host "$@"
