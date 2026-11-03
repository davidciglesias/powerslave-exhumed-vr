#pragma once
#include <cstdarg>

namespace pvr {
void LogInit(const wchar_t *path);
void Log(const char *fmt, ...);
}  // namespace pvr

#define PVR_LOG(...) ::pvr::Log(__VA_ARGS__)
