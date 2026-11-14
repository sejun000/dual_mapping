#!/bin/bash
# (2026-11-13, user) U=32 SPFresh rerun cancelled: wait for the running CoMap replay (PID 2946756), then the rest of
# q_32g_comap.sh (finished runs skipped).
cd /home/sejun000/dual_mapping/results/nvmevirt
while kill -0 2946756 2>/dev/null; do sleep 30; done
bash q_32g_comap.sh  >> q_32g_comap2.log  2>&1
