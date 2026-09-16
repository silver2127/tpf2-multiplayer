// Run under the actual preload loader, named TransportFever2. The constructor
// publishes its environment; exec then passes it to the unchanged Lua checks.
#include <cerrno>
#include <cstdio>
#include <unistd.h>
int main(int argc, char** argv)
{
    if (argc < 2) return 2;
    execvp(argv[1], argv + 1);
    std::perror("boot environment probe exec");
    return errno ? errno : 1;
}
