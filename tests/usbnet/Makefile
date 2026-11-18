# Tests for src/usbnet_frame.h: make -C tests/usbnet
usbnet_test: test.cpp ../../src/usbnet_frame.h
	g++ -std=c++17 -Wall -Wextra -fsanitize=address,undefined -g -o $@ test.cpp
	./$@
.PHONY: clean
clean:
	rm -f usbnet_test
