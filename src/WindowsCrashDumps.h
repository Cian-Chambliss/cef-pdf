#ifndef WINDOWS_CRASH_DUMPS_H_
#define WINDOWS_CRASH_DUMPS_H_

#include <string>

namespace cefpdf {

void ConfigureWindowsCrashDumps(const std::string& dumpFilePrefix, int maxDumpFiles = 5);

} // namespace cefpdf

#endif // WINDOWS_CRASH_DUMPS_H_
