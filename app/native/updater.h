// DASK Browser - automatic update checker.
#ifndef DASK_BROWSER_UPDATER_H_
#define DASK_BROWSER_UPDATER_H_

#include <string>

namespace dask {

// manual=true skips the once-per-day throttle (Settings "check now").
void CheckForUpdate(bool manual);

}  // namespace dask

#endif  // DASK_BROWSER_UPDATER_H_
