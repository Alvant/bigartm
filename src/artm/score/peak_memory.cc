// Copyright 2018, Additive Regularization of Topic Models.

// Author: Alexander Frey (sashafrey@gmail.com)

#include <cmath>

#if defined(WIN32)
#include "windows.h"  // NOLINT
#include "psapi.h"    // NOLINT
#endif
#undef ERROR

#if defined(__linux__) || defined(__APPLE__)
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "artm/core/common.h"
#include "artm/core/exceptions.h"
#include "artm/core/protobuf_helpers.h"

#include "artm/score/peak_memory.h"

namespace artm {
namespace score {

std::shared_ptr<Score> PeakMemory::CalculateScore(const artm::core::PhiMatrix& p_wt) {
  PeakMemoryScore* peak_memory_score = new PeakMemoryScore();
  std::shared_ptr<Score> retval(peak_memory_score);

#if defined(WIN32)
  PROCESS_MEMORY_COUNTERS info;
  GetProcessMemoryInfo( GetCurrentProcess( ), &info, sizeof(info) );
  peak_memory_score->set_value((size_t)info.PeakWorkingSetSize);
#elif defined(__linux__) || defined(__APPLE__)
  rusage info;
  if (!getrusage(RUSAGE_SELF, &info)) {
#if defined(__linux__)
    // For linux `ru_maxrss` field contains kilobytes
    peak_memory_score->set_value((size_t) info.ru_maxrss * (int64_t) 1024);
#else
    // For MacOS `ru_maxrss` field contains bytes
    peak_memory_score->set_value((size_t) info.ru_maxrss);
#endif
  } else {
    peak_memory_score->set_value(0);
  }
#else
  // Unimplemented for other systems
  peak_memory_score->set_value(0);
#endif

  return retval;
}

}  // namespace score
}  // namespace artm
