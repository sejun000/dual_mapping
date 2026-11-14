#!/bin/bash
# (2026-11-13) ablation v3 first (urgent), then the rest of CoMap (finished runs skipped).
cd /home/sejun000/dual_mapping/results/nvmevirt
while kill -0 2946756 2>/dev/null; do sleep 30; done
bash q_32g_abl3.sh   >> q_32g_abl3.log    2>&1
bash q_32g_comap.sh  >> q_32g_comap2.log  2>&1
