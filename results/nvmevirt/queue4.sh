#!/bin/bash
# after queue3: IU+RMW full random write (same as *_op7 runs)
cd /home/sejun000/dual_mapping
while pgrep -f "[q]ueue3.sh" >/dev/null || pgrep -f "[n]vmev_cycle.sh" >/dev/null || pgrep -f "[r]un_all.sh" >/dev/null || lsmod | grep -q '^nvmev '; do sleep 60; done
tools/nvmev_cycle.sh tools/nvmev_randwrite.sh _op7 iurmw
echo "$(date) done"
