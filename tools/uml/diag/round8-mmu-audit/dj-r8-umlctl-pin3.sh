#!/bin/bash
# Round 8 (1)/(4): pin umlctl + ALL forked children to host CPU 3.
# Under kvm-v2's one-vCPU-per-host-CPU pool, this forces every dispatch
# onto vCPUs[3] — no cross-vCPU dispatch, no cross-host-CPU iTLB churn.
# Dispositive for pool-share/icache hypotheses for Django flake.
exec taskset -c 3 /home/mjbommar/projects/personal/linux/tools/uml/uml-launcher/target/release/umlctl "$@"
