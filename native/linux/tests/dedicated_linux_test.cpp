#include "dedicated_linux.h"
#include <cassert>
#include <fstream>
#include <iterator>
#include <unistd.h>

int main() {
    char temporary[] = "/tmp/tpf2mp-dedicated.XXXXXX";
    assert(mkdtemp(temporary));
    const std::string dir = std::string(temporary) + "/";
    const std::string flags = dir + "flags.txt";
    auto write = [&](const char* text) { std::ofstream f(flags); f << text; assert(f.good()); };
    assert(!dedicated::Read(flags).enabled);
    write("dedicated=1\r\ndedicated_lobby=Native test\n"
          "dedicated_save=mp_test\ndedicated_port=29473\n"
          "dedicated_empty_speed=4\ndedicated_pause_empty=1\n"
          "dedicated_fps=15\ndedicated_pin_batch=0\n");
    dedicated::Configure(flags, dir);
    const auto& s = dedicated::Get();
    assert(s.enabled && s.lobby == "Native test" && s.save == "mp_test");
    assert(s.port == 29473 && s.emptySpeed == 0 && s.fps == 15 && !s.pinBatch);
    std::ifstream input(dir + "mp_dedicated.txt");
    std::string body((std::istreambuf_iterator<char>(input)), {});
    assert(body == "dedicated=1\nempty_speed=0\npause_empty=1\npin_batch=0\n");
    write("dedicated=2\ndedicated_save=../unsafe\ndedicated_port=65536\n"
          "dedicated_fps=0\ndedicated_empty_speed=3junk\n");
    auto invalid = dedicated::Read(flags);
    assert(!invalid.enabled && invalid.save.empty() && invalid.port == 0);
    assert(invalid.fps == 30 && invalid.emptySpeed == 1);
    dedicated::Configure(flags, dir);
    assert(access((dir + "mp_dedicated.txt").c_str(), F_OK) != 0);
    assert(unlink(flags.c_str()) == 0 && rmdir(temporary) == 0);
}
