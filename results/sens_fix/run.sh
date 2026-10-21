#!/bin/bash
SP=/tmp/claude-1001/-home-sejun000-dual-mapping/64054dd3-9fa1-4be8-986a-b7744c0b8ecf/scratchpad
xargs -P 24 -L 1 bash -c 'o=$0; t=$1; shift 1; mkdir -p $o; [ -s $o/$t.json ] && grep -q WAF $o/$t.json && exit 0; taskset -c 0-92:2 '$SP'/eval/ftlsim.bin "$@" tag=$t > $o/$t.json 2> $o/$t.log' < $SP/eval/jobs.txt
echo ALLDONE
