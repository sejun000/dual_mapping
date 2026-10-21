#!/bin/bash
# Load each policy's nvmev.ko in turn and run a test script on it.
#   tools/nvmev_cycle.sh <test-script> <suffix> policy[:dm_params] ...
#   dm_params: comma-separated key=value; tokens B18 / B12 expand to 18.75% / 12.5% of the page map in bytes (L x 4 x r).
set -u
TEST=$1; SUF=$2; shift 2
MEM="memmap_start=256G memmap_size=${NVMEV_MEMMAP_SIZE:-96G} cpus=94,95"   # NVMEV_MEMMAP_SIZE: optional override (<= 96G reserved)
load() {   # $1 folder, $2 params
    lsmod | grep -q '^nvmev ' && { sudo rmmod nvmev; sleep 2; }
    sudo insmod ~/nvmevirt_$1/nvmev.ko $MEM ${2:+dm_params=$2} || return 1
    for i in $(seq 30); do lsblk -dn -o MODEL | grep -q CSL_Virt && return 0; sleep 1; done; return 1
}
for spec in "$@"; do
    pol=${spec%%:*}; par=""; [[ "$spec" == *:* ]] && par=${spec#*:}
    if [[ "$par" == *B18* || "$par" == *B12* ]]; then
        load page4k "" || { echo "load page4k for L failed"; exit 1; }
        L=$(sudo dmesg | grep "dm_ftl policy" | tail -1 | sed -E 's/.*logical ([0-9]+) pages.*/\1/')
        B=$((L * 4 * 1875 / 10000)); par=${par//B18/$B}
        B=$((L * 4 * 1250 / 10000)); par=${par//B12/$B}    # 12.5% (1/8) of the page map
    fi
    echo "=== $pol params='$par'"
    load $pol "$par" || { echo "load $pol failed"; continue; }
    sudo dmesg | grep "dm_ftl policy" | tail -1
    $TEST ${pol}${SUF}
done
lsmod | grep -q '^nvmev ' && sudo rmmod nvmev
sudo dmesg | grep "\[dm final\]" | tail -4
