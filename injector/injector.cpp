// Classic LoadLibrary injector (x64). Usage:
//   injector.exe <process-name-or-pid> <full-path-to-dll>
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static DWORD FindPid(const char* name)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    wchar_t wname[MAX_PATH];
    mbstowcs(wname, name, MAX_PATH);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, wname) == 0) { pid = pe.th32ProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: injector.exe <process-name-or-pid> <dll-path>\n");
        return 1;
    }
    DWORD pid = (DWORD)strtoul(argv[1], nullptr, 10);
    if (pid == 0) pid = FindPid(argv[1]);
    if (pid == 0) { fprintf(stderr, "process not found: %s\n", argv[1]); return 1; }

    char full[MAX_PATH];
    GetFullPathNameA(argv[2], MAX_PATH, full, nullptr);

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) { fprintf(stderr, "OpenProcess failed: %lu\n", GetLastError()); return 1; }

    size_t len = strlen(full) + 1;
    void* mem = VirtualAllocEx(proc, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { fprintf(stderr, "VirtualAllocEx failed: %lu\n", GetLastError()); return 1; }
    WriteProcessMemory(proc, mem, full, len, nullptr);

    HANDLE th = CreateRemoteThread(proc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"),
        mem, 0, nullptr);
    if (!th) { fprintf(stderr, "CreateRemoteThread failed: %lu\n", GetLastError()); return 1; }
    WaitForSingleObject(th, 10000);
    DWORD rc = 0;
    GetExitCodeThread(th, &rc);
    printf("injected into pid %lu, LoadLibrary returned 0x%lx\n", pid, rc);
    CloseHandle(th);
    CloseHandle(proc);
    return rc ? 0 : 1;
}
