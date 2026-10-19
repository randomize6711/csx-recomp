// Xbox Live behaviour for a console with no stored Live accounts. The runtime's XOnlineStartup succeeds without
// building the library's state (global 0x3A5698 stays null), so the recompiled XOnline calls report
// XONLINE_E_NOT_INITIALIZED (0x80150005). The UIX front end (0x208817 -> 0x2084D6 -> 0x207E0B) treats that as
// fatal and the title shows its error screen; on the console XOnlineStartup succeeds offline and the account
// enumeration simply finds no accounts.
#include <cstring>

#include "xdk/hle.h"

using namespace kernel;

// XOnlineGetUsers(PXONLINE_USER pUsers, DWORD* pcUsers), .XTLID 0x30005. As its body does first (0x27AB3D..0x27AB4C):
// the 0x1C0 dword user array cleared and the count zeroed; then no account records are read, so none are returned
HLE_STDCALL(XOnlineGetUsers, 2) {
    if (a[0]) std::memset(HostPtr(a[0]), 0, 0x1C0 * 4);
    if (a[1]) W32(a[1], 0);
    return 0;
}
