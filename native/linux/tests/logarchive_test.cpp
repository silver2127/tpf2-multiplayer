#include "logarchive_linux.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
namespace fs = std::filesystem;
static void put(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary); f << s; assert(f.good()); }
static std::string get(const fs::path& p) { std::ifstream f(p, std::ios::binary); assert(f.good()); std::ostringstream s; s << f.rdbuf(); return s.str(); }
static int count(const fs::path& root, int kind) { int n=0; for (auto& e: fs::directory_iterator(root)) if (e.is_directory() && LaArchiveKind(e.path().filename().c_str())==kind) ++n; return n; }
int main(int argc, char** argv) {
    if (argc == 2) {
        int fd = open(argv[1], O_RDONLY); assert(fd >= 0);
        char id[129]; LaBuildId(fd, id, sizeof(id)); close(fd);
        puts(id); return id[0] ? 0 : 1;
    }
    char temp[] = "./logarchive-test-XXXXXX";
    assert(mkdtemp(temp));
    fs::path root = fs::absolute(temp), xdg=root/"share", mod=xdg/"tpf2mp", data=mod/"data", game=root/"game", net=mod/"netpunch";
    setenv("HOME", root.c_str(), 1); setenv("XDG_DATA_HOME", xdg.c_str(), 1);
    setenv("TPF2MP_DATADIR", data.c_str(), 1);
    unsetenv("STEAM_COMPAT_CLIENT_INSTALL_PATH");
    setenv("PRESSURE_VESSEL_RUNTIME", "fixture-soldier", 1);
    put(mod/"tpf2mp_install.txt", "version\t0.7.0.3\ngame\t/private/path\n");
    put(data/"tpf2_bridge.log", "bridge\n");
    put(data/"terrain_trace.log", "terrain diagnostics\n");
    const std::string ctl="instance=b\nlobby=0123456789abcdef\n";
    put(data/"tpf2_bridge_ctl.txt", ctl);
    std::string stream(9u<<20, 'x'); stream.replace(0,5,"FIRST"); stream.replace(stream.size()-4,4,"LAST");
    put(data/"lockstep_inject_b.txt", stream);
    put(data/"terrain_a.bin", "excluded"); put(net/"incoming_save.txt", "excluded"); put(net/"incoming_save.sav", "excluded");
    put(game/"tpf2_menu_flags.txt", "autoload=1\n"); put(game/"tpf2_slice.cfg", "trace=1\n");
    put(mod/"plugins/test.cfg", "enabled=1\n"); put(game/"plugins/test.cfg", "enabled=0\n");
    put(game/"tpf2mp_version.txt", "fallback\n");
    fs::copy_file("/proc/self/exe", game/"TransportFever2");
    fs::copy_file("/proc/self/exe", mod/"tpf2_menu.so");
    fs::create_directories(mod/"boot");
    fs::copy_file("/proc/self/exe", mod/"boot/libtpf2mp_boot.so");
    fs::copy_file("/proc/self/exe", mod/"plugins/test.so");
    fs::copy_file("/proc/self/exe", net/"netpunch");
    const std::string state="{\"code\":\"ABC\\\"DEF\",\"cross_code\": \"JOIN\",\"steam\":\"7654\",\"password\":\"hidden\",\"players\":[\"A\"],\"crossplay\":true,\"type\":\"code\"}\n";
    put(net/"lobby_state.json", state); put(net/"lobby_out.jsonl", state); put(net/"lobby_proc.log", "lobby\n");
    put(game/"netpunch/lobby_in.jsonl", "{\"secret\":\n\"xyz\",\"pass\":false}\n");
    std::string large="{\"code\":\""+std::string(9u<<20,'S')+"\"}\n"+state;
    put(net/"large.jsonl", large);
    put(xdg/"Steam/userdata/42/1066780/local/crash_dump/stdout.txt", "game stdout\n");
    for(int k=0;k<2;++k) for(int i=0;i<6;++i) put(mod/"logs"/("2020010"+std::to_string(i)+"-000000-"+(k?"now":"previous"))/"about.txt","old");
    const std::string gameDir=game.string()+"/";
    Tpf2mpLogArchive a;
    assert(Tpf2mpArchiveLogsSafe(false, gameDir.c_str(), &a));
    fs::path out=a.folder;
    auto about=get(out/"about.txt");
    assert(about.find("Steam Runtime fixture-soldier")!=std::string::npos);
    assert(about.find("System ")!=std::string::npos && about.find("mod/boot/libtpf2mp_boot.so")!=std::string::npos);
    assert(about.find("version 0.7.0.3")!=std::string::npos && about.find("Linux")!=std::string::npos && about.find("UTC")!=std::string::npos);
    assert(about.find("GNU build-id unavailable")==std::string::npos);
    assert(about.find("mod/plugins/test.so")!=std::string::npos && about.find("netpunch/netpunch")!=std::string::npos);
    assert(get(out/"game_stdout.txt")=="game stdout\n");
    assert(get(out/"terrain_trace.log")=="terrain diagnostics\n");
    assert(get(out/"state_tpf2_bridge_ctl.txt")==ctl && get(data/"tpf2_bridge_ctl.txt")==ctl);
    auto tail=get(out/"state_lockstep_inject_b.txt"); assert(tail.size()==(8u<<20) && tail.substr(tail.size()-4)=="LAST" && tail.find("FIRST")==std::string::npos);
    assert(about.find("last 8 MB kept")!=std::string::npos);
    auto masked=get(out/"netpunch_lobby_state.json"); assert(masked.size()==state.size());
    for(auto secret:{"ABC","DEF","JOIN","7654","hidden"}) assert(masked.find(secret)==std::string::npos);
    assert(masked.find("\"type\":\"code\"")!=std::string::npos && masked.find("\"crossplay\":true")!=std::string::npos);
    assert(get(net/"lobby_state.json")==state);
    assert(get(out/"netpunch_large.jsonl").find('S')==std::string::npos);
    assert(get(out/"game_netpunch_lobby_in.jsonl").find("xyz")==std::string::npos);
    for(auto name:{"game_tpf2_menu_flags.txt","game_tpf2_slice.cfg","mod_plugins_test.cfg","game_plugins_test.cfg","game_tpf2mp_version.txt","netpunch_lobby_proc.log"}) assert(fs::exists(out/name));
    for(auto name:{"netpunch_incoming_save.txt","netpunch_incoming_save.sav","state_terrain_a.bin"}) assert(!fs::exists(out/name));
    assert(count(mod/"logs",1)==5 && count(mod/"logs",0)==6);
    assert(Tpf2mpArchiveLogsSafe(true,gameDir.c_str(),&a));
    assert(!fs::exists(data/"tpf2_bridge.log") && get(fs::path(a.folder)/"tpf2_bridge.log")=="bridge\n");
    assert(get(data/"tpf2_bridge_ctl.txt")==ctl && get(fs::path(a.folder)/"state_tpf2_bridge_ctl.txt")==ctl);
    assert(count(mod/"logs",0)==5);
    fs::remove(mod/"tpf2mp_install.txt");
    put(data/"tpf2_bridge.log","live\n");
    // The previous Safe call holds a shared liveness lock: a second caller copies.
    assert(Tpf2mpArchiveLogsSafe(true,gameDir.c_str(),&a)); assert(get(data/"tpf2_bridge.log")=="live\n");
    assert(get(fs::path(a.folder)/"about.txt").find("version fallback")!=std::string::npos);
    put(data/"tpf2mp_keep_logs.txt", "1\n");
    assert(Tpf2mpArchiveLogsSafe(true,gameDir.c_str(),&a));
    assert(count(mod/"logs",0)==6 && get(data/"tpf2_bridge.log")=="live\n");
    // Empty/malformed ELF input is safe and produces no invented identity.
    put(root/"bad", "not an ELF"); int fd=open((root/"bad").c_str(),O_RDONLY); char id[129]; LaBuildId(fd,id,sizeof(id)); close(fd); assert(!id[0]);
    fs::remove_all(root);
    puts("PASS: native identities, state, tails, redaction, exclusion, retention, startup/liveness");
}
