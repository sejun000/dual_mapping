#!/bin/bash
# (2026-11-13) after the reboot that clears the stuck nvmev module: reordered ablation, then Fig. 10 without spill
# (U = 64 / 128 + apps), then the CoMap configs.
cd /home/sejun000/dual_mapping/results/nvmevirt
bash q_32g_abl2.sh   >> q_32g_abl2.log  2>&1
bash q_32g_ns2.sh    >> q_32g_ns2b.log  2>&1
bash q_32g_comap.sh  >> q_32g_comap2.log 2>&1
