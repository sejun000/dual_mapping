#include "app/global.h"

char *PlacementName = nullptr;
double GpThreshold = 0.091;               // for GC (how many invalid pages)
double OpRatio     = 0.10;                // over-provisioning ratio (physical = logical * (1 + OpRatio))
int LogicalSizeGb  = 8;                   // logical workload size in GB
char wk_name[1024]  = "/home/dogi-ae/dogi-prototype/prototype/app/workload/test-fio-small";
//char wk_name[1024]  = "/test-fio-small";
uint32_t NumGroup  = 6;
uint64_t BIR[10]   = {0,};
uint32_t naive_start = 1;
int APPLY_ML = 0;

const char kZnsDevicePath[] = "/dev/nvme0n1";
const char kZbdDeviceName[] = "nvme0n1";

const uint64_t kBlockBytes    = 4096;
#ifndef DOGI_SEG_BLOCKS
#define DOGI_SEG_BLOCKS 49152
#endif
const uint64_t kSegmentBlocks = DOGI_SEG_BLOCKS; // blocks per segment = ftlsim block P (build.sh SEG=<P>; default 192 MiB)
// Port: per-LBA tables are sized max(128 Mi, device pages) (artifact hard-coded 128 Mi = 512 GiB).
uint64_t gLbaCapacity = 128ull * 1024 * 1024;
// Port: strict physical budget (DOGI_NSEG > 0): at most NSEG segments holding data (sealed + written-open + GC victim);
// GC runs while free <= DOGI_GC_RESERVE, user writes stall meanwhile. 0 = artifact GP-threshold accounting.
int64_t gNumSegCap = 0;
int64_t gGcReserve = 2;
int gTrueLbaAll = 1;
const uint64_t kSegmentBytes  = kBlockBytes * kSegmentBlocks;
