#!/bin/bash
# (2026-11-13, user) U=32 SPFresh rerun ahead of the rest of CoMap: wait for the running CoMap replay (PID 2946756),
# then the rerun, then q_32g_comap.sh (finished runs skipped).
cd /home/sejun000/dual_mapping/results/nvmevirt
while kill -0 2946756 2>/dev/null; do sleep 30; done
bash q_32g_u32sp.sh  >> q_32g_u32sp.log   2>&1
bash q_32g_comap.sh  >> q_32g_comap2.log  2>&1
