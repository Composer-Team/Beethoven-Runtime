#!/bin/bash
make install
vcs +v2k -kdb -debug_access -P $VERDI_HOME/share/PLI/VCS/LINUX64/novas.tab $VERDI_HOME/share/PLI/VCS/LINUX64/pli.a -sverilog +incdir+$BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/beethoven.build -full64 +vpi+1 -CFLAGS -std=c++17 -P $BEETHOVEN_ROOT/inc/tab.tab  +define+CLOCK_PERIOD=1 -f $BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/vcs_srcs.in libBeethovenRuntime.a $BEETHOVEN_ROOT/lib/libdramsim3.so -lrt -L/usr/local/lib64 -lbeethoven -CFLAGS -I$BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/ $BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/beethoven.build/BeethovenTopVCSHarness.v -o BeethovenTop
# if SDIR is not set, set it to $BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/beethoven.build
SDIR=${SDIR:-$BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/beethoven.build}
SLIST=${SLIST:-$BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/vcs_srcs.in}

vcs +v2k -kdb -debug_access -P $VERDI_HOME/share/PLI/VCS/LINUX64/novas.tab $VERDI_HOME/share/PLI/VCS/LINUX64/pli.a -sverilog +incdir+$SDIR -full64 +vpi+1 -CFLAGS -std=c++17 -P tab.tab  +define+CLOCK_PERIOD=1 -f $SLIST libBeethovenRuntime.a $BEETHOVEN_ROOT/lib/libdramsim3.so -lrt -L/usr/local/lib64 -lbeethoven -CFLAGS -I$BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/ $BEETHOVEN_ROOT/Beethoven-Hardware/vsim/generated-src/beethoven.build/BeethovenTopVCSHarness.v -o BeethovenTop
