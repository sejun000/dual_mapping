#!/bin/bash
# build ftlsim_cgc (pinned zig, same flags as ./build.sh), atomically replacing the binary used by running jobs
cd /home/sejun000/dual_mapping
~/tools/zig-linux-x86_64-0.13.0/zig c++ -O2 -std=c++17 -Wall -Wextra ftlsim/ftlsim.cpp -o ftlsim/ftlsim_cgc.new && mv ftlsim/ftlsim_cgc.new ftlsim/ftlsim_cgc && echo built
