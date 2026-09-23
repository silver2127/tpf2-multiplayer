// Native-only cancellation-hook readiness shared by the slice and lobby.
// This says that the required hooks installed, not that every action is covered.
#pragma once
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

inline uint64_t SliceReadyProcessStart()
{
    const int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    char body[4096];
    ssize_t n;
    do { n = read(fd, body, sizeof(body)-1); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n <= 0 || size_t(n) == sizeof(body)-1) return 0;
    body[n] = 0;
    // comm (field 2) can contain spaces and parentheses. Its last ')' is
    // followed by state (field 3); starttime is field 22 in clock ticks.
    const char* p = std::strrchr(body, ')');
    if (!p || p[1] != ' ') return 0;
    p += 2;
    for (int field=3; field<22; ++field) {
        p = std::strchr(p, ' ');
        if (!p) return 0;
        while (*p == ' ') ++p;
    }
    if (*p < '0' || *p > '9') return 0;
    char* end = nullptr;
    errno = 0;
    const auto start = std::strtoull(p, &end, 10);
    return !errno && end && (*end == ' ' || *end == '\n' || !*end) ? uint64_t(start) : 0;
}

inline bool SliceReadyPath(const char* dataDir, char* path, size_t cap)
{
    if (!dataDir || !*dataDir) return false;
    const size_t n = std::strlen(dataDir);
    const int wrote = std::snprintf(path, cap, "%s%stpf2_slice_ready.txt", dataDir, dataDir[n-1]=='/' ? "" : "/");
    return wrote > 0 && size_t(wrote) < cap;
}

// Publish only after this slice has acquired its data-directory ownership.
// A temporary regular file and rename keep readers from accepting a prefix.
inline bool SlicePublishReady(const char* dataDir, bool ready)
{
    const uint64_t start = SliceReadyProcessStart();
    char path[4096], temporary[4160], body[128];
    if (!start || !SliceReadyPath(dataDir, path, sizeof(path))) return false;
    const int length = std::snprintf(body, sizeof(body), "pid=%ld\nstart=%llu\nready=%d\n",
        long(getpid()), static_cast<unsigned long long>(start), ready ? 1 : 0);
    const int tn = std::snprintf(temporary, sizeof(temporary), "%s.%ld.XXXXXX", path, long(getpid()));
    if (length <= 0 || size_t(length) >= sizeof(body) || tn <= 0 || size_t(tn) >= sizeof(temporary)) return false;
    const int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0) return false;
    size_t wrote = 0;
    while (wrote < size_t(length)) {
        const ssize_t n = write(fd, body+wrote, size_t(length)-wrote);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        wrote += size_t(n);
    }
    const bool closed = close(fd) == 0;
    if (wrote == size_t(length) && closed && rename(temporary, path) == 0) return true;
    unlink(temporary);
    return false;
}

inline bool SliceHooksReady(const char* dataDir, const char** reason = nullptr)
{
    const auto refuse = [reason](const char* why) { if (reason) *reason=why; return false; };
    char path[4096], body[128], expected[128];
    const uint64_t start = SliceReadyProcessStart();
    if (!start || !SliceReadyPath(dataDir, path, sizeof(path))) return refuse("process identity unavailable");
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
    if (fd < 0) return refuse("readiness file missing or unreadable");
    struct stat st{};
    const bool regular = fstat(fd, &st)==0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < off_t(sizeof(body));
    ssize_t n = -1;
    if (regular) do { n = read(fd, body, sizeof(body)); } while (n < 0 && errno == EINTR);
    close(fd);
    if (!regular || n != st.st_size) return refuse("readiness file incomplete or invalid");
    const int want = std::snprintf(expected, sizeof(expected), "pid=%ld\nstart=%llu\nready=1\n",
        long(getpid()), static_cast<unsigned long long>(start));
    if (want <= 0 || n != want || std::memcmp(body, expected, size_t(want)))
        return refuse("cancellation hooks are not ready for this game process");
    if (reason) *reason = "";
    return true;
}
