#pragma once
#include <cstdint>
#include <string>

namespace NativeIo {
struct Event { std::string operation, step, detail; bool success; };
bool Save(const std::string&, const std::string&);
bool Load(const std::string&, const std::string&);
bool PauseAndDrain(const std::string&);
bool Poll(Event&);
bool HasWorld();
bool Busy();
bool Loading();
bool SetActionsHeld(bool);
void WorkThreads(unsigned&, unsigned&);
}
namespace NativeControl {
void Start(const std::string& directory, bool supported);
void SignalShutdown();
}
