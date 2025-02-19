
CXX_FLAGS = --std=c++17 -fPIC \
			-I${BEETHOVEN_PATH}/build/ \
			-I/usr/local/include/iverilog/ \
			-Iinclude/ \
			-I/usr/local/include/beethoven \
			-IDRAMsim3/src/ \
			-IDRAMsim3/ext/headers/
PWD = $(shell pwd)
DRAMSIM3DIR = $(PWD)/DRAMsim3
LD_FLAGS = -shared -lbeethoven -L/usr/local/lib/ivl/ -LDRAMsim3 -ldramsim3 -undefined suppress -rpath /usr/local/lib -rpath $(DRAMSIM3DIR)

# DEBUG FLAGS
CXX_FLAGS += -O0 -g3
# RELEASE FLAGS
# CXX_FLAGS = $(CXX_FLAGS) -O2
FRONTBUS=axi
SIMULATOR=vpi
VPI_LOC = /usr/local/lib/ivl
VPI_FLAGS = $(VPI_LOC)/system.vpi
VERILOG_FLAGS = -DCLOCK_PERIOD=500 -DICARUS
VERILOG_SRCS = $(shell cat ${BEETHOVEN_PATH}/build/vcs_srcs.in) ${BEETHOVEN_PATH}/build/hw/BeethovenTopVCSHarness.v

# vpi
CXX_FLAGS += -DSIM=vcs

DRAMsim3/libdramsim3.dylib:
	cd DRAMsim3/ && mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j8

SRCS = 	src/data_server.o \
		src/cmd_server.o \
		src/mmio.o \
		src/sim/axi/front_bus_ctrl_axi.o \
		src/sim/axi/${SIMULATOR}_axi_frontend.o  \
		src/sim/tick.o \
		src/sim/mem_ctrl.o

lib_beethoven.o: ${BEETHOVEN_PATH}/build/beethoven_hardware.cc ${BEETHOVEN_PATH}/build/beethoven_hardware.h
	c++ -c $(CXX_FLAGS) -o$@ ${BEETHOVEN_PATH}/build/beethoven_hardware.cc

src/%.o: src/%.cc
	c++ -c ${CXX_FLAGS} -o$@ $^

sim_BeethovenRuntime.vpi: $(SRCS) lib_beethoven.o DRAMsim3/libdramsim3.dylib
	c++ $(LD_FLAGS) -o$@ $^
	#c++ -shared -lbeethoven -L/usr/local/lib/ivl/ -osim_BeethovenRuntime.vpi src/data_server.o src/cmd_server.o


beethoven.vvp: $(VERILOG_SRCS)
	iverilog $(VERILOG_FLAGS) -s BeethovenTopVCSHarness -o$@ $^

.PHONY: sim_icarus
sim_icarus: sim_BeethovenRuntime.vpi beethoven.vvp
	vvp -M. -msim_BeethovenRuntime beethoven.vvp -lDRAMsim3/libdramsim3.dylib

.PHONY: clean
clean:
	rm -f $(SRCS) beethoven.vvp DRAMsim3/libdramsim3.dylib sim_BeethovenRuntime.vpi
