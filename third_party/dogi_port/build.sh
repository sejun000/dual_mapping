#!/bin/bash
# Build the DOGI port. SEG=<pages per segment> (= ftlsim block P; default 49152 = 192 MiB), OUT=<binary>.
cd "$(dirname "$0")"
ZIG=$HOME/tools/zig-linux-x86_64-0.13.0/zig
SEG=${SEG:-49152}
OUT=${OUT:-dogi_sim}
SRCS="app/main.cc app/global.cc app/classifier.cc app/freq_features.cc app/group_config.cc \
app/group_optimizer.cc app/mlp_inference.cc app/model_train.cc \
src/placement/dogi.cc src/selection/dogiselect.cc src/selection/costbenefit.cc src/selection/greedy.cc \
src/logstore/logstore.cc src/logstore/manager.cc src/logstore/segment.cc src/logstore/scheduler.cc \
src/indexmap/array.cc src/indexmap/filter.cc src/indexmap/hashmap.cc \
src/storage_adapter/local_adapter.cc src/buse/buseOperations.cpp src/buse/diskStats.cpp \
cblas_stub.cpp"
rm -f "$OUT"
$ZIG c++ -O2 -std=c++17 -DDOGI_SEG_BLOCKS=$SEG -I. -Ifakeinc_cblas -Ifakeinc -o "$OUT" $SRCS -ldl -pthread 2>&1 | grep -vE "warning:|note:|\^|~|^ +\||^ +[0-9]+ \|" | head -30
[ -x "$OUT" ] && echo "BUILT $OUT (SEG=$SEG)"
