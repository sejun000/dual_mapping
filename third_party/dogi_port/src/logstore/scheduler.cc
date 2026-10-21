#include <cstdlib>
#include "src/logstore/scheduler.h"
#include "app/global.h"

Scheduler::Scheduler(Manager *manager)
{
  mSelection = std::unique_ptr<Selection>(SelectionFactory::GetInstance(Config::GetInstance().selection));
  mWorker = std::thread(&Scheduler::scheduling, this, manager);
  mWorker.detach();
}

void Scheduler::scheduling(Manager *manager) {
  using namespace std::chrono_literals;
  struct timeval current_time;
  while (true) {
    if (gNumSegCap > 0) std::this_thread::sleep_for(0.00005s); else std::this_thread::sleep_for(0.05s);
    if (mShutdown) {
      break;
    }


//    while (manager->GetGp() >= 0.1737) {
    while (gNumSegCap > 0 ? manager->NeedGc() : manager->GetGp() >= GpThreshold) {
      //printf("GP: %.2f\n", manager->GetGp());
      // select a segment
      int segmentId = select(manager);
      {
        gettimeofday(&current_time, NULL);
 //       printf("GC start: %ld.%ld\n",
 //           current_time.tv_sec, current_time.tv_usec);
      }
      Segment segment = manager->ReadSegment(segmentId);
	  {
//		printf("GC Semgent Info: ClassId: %d, Valid Ratio: %.3f\n", segment.GetClassNum(), 1. - segment.GetGp()); 
	    gettimeofday(&current_time, NULL);
//        printf("GC finish read: %ld.%ld\n",
//            current_time.tv_sec, current_time.tv_usec);
      }
      // collect the segment
      collect(manager, segment);
      {
        gettimeofday(&current_time, NULL);
//        printf("GC finish rewrite: %ld.%ld\n",
//            current_time.tv_sec, current_time.tv_usec);
      }
    }
  }
}

int Scheduler::select(Manager *manager) {
  // prepare the segments_
  std::vector<Segment> segments;
  manager->GetSegments(segments);

  auto res = mSelection->Select(segments);
  int id = res[0].second;
  static const bool guard = gNumSegCap > 0 || (getenv("DOGI_GUARD") && atoi(getenv("DOGI_GUARD")));
  if (guard) {
    // port (strict budget; GP accounting only with DOGI_GUARD=1): liveness guard for DOGI's dead end. DogiSelect takes
    // an expired BIR-group segment by age (relocating a still-valid one is DOGI's design: it moves on to the next
    // group), otherwise the best cost-benefit segment of the remaining pool, and never a BIR-group segment before its
    // BIR age. Ages are counted in host writes, which do not advance while GC runs. If the pick comes from the
    // cost-benefit pool and is fully valid, that whole pool is fully valid (any garbage would score higher) and GC
    // would relocate full segments forever; only then take the sealed segment with the most invalid blocks instead.
    const Segment *chosen = nullptr, *best = nullptr;
    for (auto &s : segments) {
      if ((int)s.GetSegmentId() == id) chosen = &s;
      if (!best || s.GetTotalInvalidBlocks() > best->GetTotalInvalidBlocks()) best = &s;
    }
    // DOGI_GUARD_ANY=1 ("DOGI+" in results/dogi_v3): also replace fully-valid expired-BIR picks, i.e. never collect a
    // fully-valid segment while any sealed segment has garbage (stronger than the published policy).
    static const bool any = getenv("DOGI_GUARD_ANY") && atoi(getenv("DOGI_GUARD_ANY"));
    if (chosen && chosen->GetTotalInvalidBlocks() == 0 && (any || (uint32_t)chosen->GetClassNum() >= naive_start) &&
        best && best->GetTotalInvalidBlocks() > 0) {
      id = (int)best->GetSegmentId();
      manager->NoteEmergency();
    }
  }
  return id;
}

void Scheduler::collect(Manager *manager, Segment &segment) {
  uint64_t nRewriteBlocks = 0;
  manager->CollectSegment(segment.GetSegmentId());
  for (uint64_t i = 0; i < kSegmentBlocks; ++i) {
    off64_t blockAddr = segment.GetBlockAddr(i);
    if (blockAddr == UINT32_MAX) continue;
    off64_t oldPhyAddr = segment.GetPhyAddr(i);
    char* data = segment.GetBlockData(i);
    if (!manager->GcAppend(data, blockAddr, oldPhyAddr)) {
      nRewriteBlocks += 1;
    }
  }
  manager->RemoveSegment(segment.GetSegmentId(), segment.GetTotalInvalidBlocks() + nRewriteBlocks);
}
