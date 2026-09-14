#pragma once
#include <string>
namespace NativeControl {
void Start(const std::wstring& directory,bool supported);
void SignalShutdown();
}
