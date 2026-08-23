CXX ?= $(HOME)/tools/zig-linux-x86_64-0.13.0/zig c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

all: ftlsim/ftlsim

ftlsim/ftlsim: ftlsim/ftlsim.cpp
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -f ftlsim/ftlsim
