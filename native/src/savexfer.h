// Bulk save-file transfer, host -> joiner.
//
// Deliberately NOT on the event channel: that is a line-oriented reliable-UDP
// protocol with 1KB chunks and a 32-entry ack window, and a Transport Fever
// save is ~178 MB. TCP is the right tool for a one-shot bulk copy.
//
// Legacy: the lobby transfers saves now, and the bridge starts this server only
// when its cfg says save_server=1.
#pragma once
#include <cstdint>
#include <string>

typedef void (*XferLogFn)(const char* fmt, ...);

// Best-effort discovery of "<Steam>/userdata/<id>/1066780/local/save".
// Empty string if it cannot be found; override with save_dir= in the cfg.
std::string Save_FindSaveDir();

// Host side: serve one save (name without extension; empty = newest .sav) to
// peerIp only. A loopback peerIp binds the listener to 127.0.0.1; any other
// binds all interfaces. A connection from any other address is closed unserved.
void Save_StartServer(uint16_t port, const std::string& saveDir,
                      const std::string& shareName, const char* peerIp,
                      XferLogFn log);

// Joiner side: pull the host's save into saveDir. Files are written with an
// "mp_host_" prefix so a transfer can never overwrite the player's own saves.
bool Save_Pull(const char* hostIp, uint16_t port, const std::string& saveDir,
               XferLogFn log);
