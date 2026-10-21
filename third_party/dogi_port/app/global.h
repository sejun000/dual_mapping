#ifndef GLOBALS_H
#define GLOBALS_H

#include <stdint.h>  // uint32_t, uint64_t
#include <stdio.h>   // FILE*

// Global configuration knobs shared across app components.
extern char *PlacementName;
extern double GpThreshold;
extern double OpRatio;
extern int LogicalSizeGb;
extern char wk_name[1024];
extern uint32_t NumGroup;
extern uint32_t naive_start;
extern uint64_t BIR[10];
extern int APPLY_ML;

// Device paths/names for zoned backend.
extern const char kZnsDevicePath[];
extern const char kZbdDeviceName[];

// Segment/Block size constants (configured in global.cc)
extern const uint64_t kBlockBytes;
extern const uint64_t kSegmentBlocks;         // blocks per segment
extern const uint64_t kSegmentBytes;          // bytes per segment
extern uint64_t gLbaCapacity;                // per-LBA table size (port)
extern int64_t gNumSegCap;                   // strict physical segment budget (port; 0 = off)
extern int gTrueLbaAll;                      // port: post-sampling true-LBA logging of all writes
extern int64_t gGcReserve;                   // free segments kept for GC in strict mode (port)

// Derived thresholds based on logical size.
inline uint64_t GetPassTimeBlocks() {
  return static_cast<uint64_t>(LogicalSizeGb) * 1000ull * 1000ull * 1000ull / kBlockBytes;
}
#endif // GLOBALS_H
