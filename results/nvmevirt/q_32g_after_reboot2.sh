#!/bin/bash
# (2026-11-13) restart of q_32g_after_reboot.sh after the ablation app step failed (page4k module): wait for the Fig. 10
# U=64 70:30 replay already running (PID 34567), then ablation apps, rest of Fig. 10 (done runs skipped), CoMap.
cd /home/sejun000/dual_mapping/results/nvmevirt
while kill -0 34567 2>/dev/null; do sleep 30; done
bash q_32g_abl2_apps.sh >> q_32g_abl2.log   2>&1
bash q_32g_ns2.sh       >> q_32g_ns2b.log   2>&1
bash q_32g_comap.sh     >> q_32g_comap2.log 2>&1
