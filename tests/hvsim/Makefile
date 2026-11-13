# vmdos's Hyper-V drivers (VMBus, storvsc, netvsc) against a simulated host,
# as a 32-bit Linux program (gcc-multilib). "make" builds and runs both hosts.
CXX      := g++
FLAGS    := -m32 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing
DRIVERS  := ../../src/vmbus.cpp ../../src/blk_storvsc.cpp ../../src/nic_netvsc.cpp

all: run

hvsim: host.cpp test.cpp linux_hv.h $(DRIVERS) ../../src/vmbus.hpp ../../src/net.hpp
	$(CXX) $(FLAGS) -std=gnu++17 -c host.cpp -o host.o
	$(CXX) $(FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -DVMBUS_SIM -c test.cpp -o test.o
	for f in $(DRIVERS); do $(CXX) $(FLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -fno-builtin -DVMBUS_SIM -c $$f -o $$(basename $$f .cpp).o || exit 1; done
	$(CXX) $(FLAGS) -o $@ host.o test.o vmbus.o blk_storvsc.o nic_netvsc.o

run: hvsim
	./hvsim new
	./hvsim old

clean:
	rm -f hvsim *.o
