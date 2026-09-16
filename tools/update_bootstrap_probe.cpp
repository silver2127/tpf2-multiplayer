#include "../native/src/update_bootstrap.h"
int main()
{
    Tpf2mpPinRelease();
    wchar_t root[MAX_PATH] = {};
    GetEnvironmentVariableW(L"TPF2MP_RELEASE_ROOT", root, MAX_PATH);
    wprintf(L"%s", root);
    return 0;
}
