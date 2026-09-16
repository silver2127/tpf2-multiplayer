// Actual control-file reset path with temporary files and a loopback transport.
// No engine patches, startup threads or game files are touched.
#define TPF2MP_BRIDGE_TEST
#include "../src/bridge_linux.cpp"
#include <cassert>
#include <filesystem>

bool SpeedHook_Install(SpeedLogFn) { assert(false); return false; }
void SpeedHook_SetTarget(double) { assert(false); }
double SpeedHook_Target() { assert(false); return 0; }
bool SetPlayerPatch_Install(SetPlayerLogFn) { assert(false); return false; }

static void Write(const std::string& path, const std::string& body) {
    FILE* f = fopen(path.c_str(), "wb"); assert(f);
    assert(fwrite(body.data(), 1, body.size(), f) == body.size());
    assert(fclose(f) == 0);
}
int main() {
    char temporary[] = "/tmp/tpf2mp-bridge-epoch.XXXXXX";
    assert(mkdtemp(temporary)); S().dataDir = std::string(temporary) + "/";
    S().rt.instance = "b";
    SetTailPath(CapturePathFor("b"));
    const auto events = S().dataDir + "tpf2_events_b.txt";
    Write(events, "old received command\n");
    Write(S().tailPath, "old sent command\n");
    assert(Net_Init(0, "127.0.0.1", 1, OnPeerLine));
    const std::string lobby(32, 'a'), epoch(32, 'b');
    const std::string control = "pid=" + std::to_string(getpid()) + "\nlobby=" + lobby + "\npeer=127.0.0.1:1\n";
    ApplyControl("pid=2147483647\nlobby=" + lobby + "\npeer=127.0.0.1:1\n");
    assert(Net_WorldEpoch() == std::string(32, '0'));
    ApplyControl(control);
    assert(Net_WorldEpoch() == lobby && S().tailEpoch == lobby && S().tailFromZero);
    std::string text;
    assert(ReadSmallFile(events, text) && text.empty());
    assert(ReadSmallFile(S().tailPath, text) && text.empty());
    assert(ReadSmallFile(S().dataDir + "tpf2_epoch_ready.txt", text));
    assert(text == "epoch=" + lobby + "\nok=1\npid=" + std::to_string(getpid()) + "\n");
    Write(S().tailPath, "new command\n");
    const auto generation = S().tailGen;
    ApplyControl(control);
    assert(S().tailGen == generation);
    assert(ReadSmallFile(S().tailPath, text) && text == "new command\n");
    ApplyControl("epoch=" + epoch + "\n");
    assert(S().tailGen == generation + 1 && S().tailEpoch == epoch);
    ApplyControl(control); // Roster refresh cannot undo the recovered world.
    assert(Net_WorldEpoch() == epoch && S().tailGen == generation + 1);
    ApplyControl("epoch=bad\n");
    assert(Net_WorldEpoch() == epoch);
    // A failed local reset must publish failure, never acknowledge readiness.
    S().tailPath = S().dataDir + "missing/capture.txt";
    ApplyControl("epoch=" + std::string(32, 'c') + "\n");
    assert(ReadSmallFile(S().dataDir + "tpf2_epoch_ready.txt", text));
    assert(text.find("\nok=0\n") != std::string::npos);
    Net_Shutdown(); fclose(S().events); S().events = nullptr;
    std::filesystem::remove_all(temporary);
    puts("PASS: bridge epoch reset, owner PID, local files, readiness, idempotence and failed reset");
}
