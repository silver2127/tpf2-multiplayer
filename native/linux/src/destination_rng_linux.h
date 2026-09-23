#pragma once
#include <cstdint>

// Runs only before game main/worker creation. Requires the exact ELF build-id
// and complete original bytes of the boost::mt19937 integer wrappers. Every
// valid call gets Windows interval semantics; invalid ranges retain the native
// assertion path. Other engine and distribution overloads are unaffected.
bool Tpf2mpInstallDestinationRng(uintptr_t imageBase, const char* buildId);

// The same startup/build verification rule, for all ten verified SimPersonSystem
// MT-seed calls. Other callers retain their original seed and constructor.
bool Tpf2mpInstallPersonSeeds(uintptr_t imageBase, const char* buildId);
