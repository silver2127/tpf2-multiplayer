#include <cassert>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using Format = int (*)(char*, int, size_t, const char*, ...);
    const auto format = reinterpret_cast<Format>(dlsym(RTLD_DEFAULT, "__sprintf_chk"));
    assert(format);
    Dl_info info{};
    assert(dladdr(reinterpret_cast<void*>(format), &info) && info.dli_fname);
    assert(std::strcmp(info.dli_fname, argv[1]) == 0); // prove adapter was used
    char out[64];
    assert(format(out, 1, sizeof(out), "%+08.1f", 2.25) == 8);
    assert(std::strcmp(out, "+00002.2") == 0); // non-Lua retains glibc ties
    assert(format(out, 1, sizeof(out), "%s:%d:%.1f", "item", 42, -2.25) == 12);
    assert(std::strcmp(out, "item:42:-2.2") == 0);
    int consumed = -1;
    assert(format(out, 1, sizeof(out), "abc%n!", &consumed) == 4);
    assert(consumed == 3 && std::strcmp(out, "abc!") == 0);

    // The adapter must retain fortified buffer checking, not downgrade to
    // plain vsprintf. The child is only this tiny probe, never the game.
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        rlimit noCore{};
        setrlimit(RLIMIT_CORE, &noCore);
        close(STDERR_FILENO);
        format(out, 1, 4, "%s", "long value");
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    std::puts("boot formatter: nongame/unknown-build varargs and fortified bounds preserved");
}
