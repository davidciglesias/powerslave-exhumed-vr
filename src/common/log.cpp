#include "common/log.h"

#include <windows.h>
#include <cstdio>
#include <share.h>
#include <mutex>

namespace pvr {
static FILE *g_log = nullptr;
static std::mutex g_mtx;
static LARGE_INTEGER g_t0, g_freq;

void LogInit(const wchar_t *path) {
    g_log = _wfsopen(path, L"w", _SH_DENYWR);  // readable while the game runs
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
}

void Log(const char *fmt, ...) {
    if (!g_log) return;
    std::lock_guard<std::mutex> lk(g_mtx);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double s = double(t.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    fprintf(g_log, "[%9.3f] ", s);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}
}  // namespace pvr
