// hix.dll — shim between Bloody and Wine's hid.dll.
//  * logs every HID call Bloody makes (with buffers) to %HIXLOG% (default C:\hix.log)
//  * fills in HidD_GetIndexedString, which Wine's hidclass does not implement
// Bloody's import of HID.DLL is renamed to HIX.DLL so only its own calls land here.
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <hidsdi.h>
#include <hidpi.h>

static HANDLE log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION log_cs;

static void hixlog(const char *fmt, ...)
{
    char line[2048];
    DWORD written;
    va_list ap;
    int n;

    if (log_file == INVALID_HANDLE_VALUE) return;
    n = snprintf(line, sizeof(line), "%lu ", GetTickCount());
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
    line[n++] = '\n';
    EnterCriticalSection(&log_cs);
    WriteFile(log_file, line, n, &written, NULL);
    LeaveCriticalSection(&log_cs);
}

static void hex(char *out, size_t size, const void *buf, ULONG len)
{
    const unsigned char *p = buf;
    size_t n = 0;
    out[0] = 0;
    for (ULONG i = 0; i < len && n + 4 < size; i++) n += snprintf(out + n, size - n, " %02x", p[i]);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        char path[MAX_PATH];
        if (!GetEnvironmentVariableA("HIXLOG", path, sizeof(path))) strcpy(path, "C:\\hix.log");
        InitializeCriticalSection(&log_cs);
        log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        hixlog("hix loaded pid %lu", GetCurrentProcessId());
    }
    return TRUE;
}

void WINAPI hix_HidD_GetHidGuid(GUID *guid)
{
    HidD_GetHidGuid(guid);
}

BOOLEAN WINAPI hix_HidD_GetAttributes(HANDLE h, PHIDD_ATTRIBUTES attr)
{
    BOOLEAN ret = HidD_GetAttributes(h, attr);
    if (ret) hixlog("GetAttributes h=%p vid=%04x pid=%04x ver=%04x", h, attr->VendorID, attr->ProductID, attr->VersionNumber);
    else hixlog("GetAttributes h=%p FAILED err=%lu", h, GetLastError());
    return ret;
}

BOOLEAN WINAPI hix_HidD_GetPreparsedData(HANDLE h, PHIDP_PREPARSED_DATA *data)
{
    return HidD_GetPreparsedData(h, data);
}

BOOLEAN WINAPI hix_HidD_FreePreparsedData(PHIDP_PREPARSED_DATA data)
{
    return HidD_FreePreparsedData(data);
}

NTSTATUS WINAPI hix_HidP_GetCaps(PHIDP_PREPARSED_DATA data, PHIDP_CAPS caps)
{
    NTSTATUS ret = HidP_GetCaps(data, caps);
    hixlog("GetCaps ret=%08lx page=%04x usage=%04x in=%u out=%u feat=%u links=%u",
         ret, caps->UsagePage, caps->Usage, caps->InputReportByteLength,
         caps->OutputReportByteLength, caps->FeatureReportByteLength, caps->NumberLinkCollectionNodes);
    return ret;
}

BOOLEAN WINAPI hix_HidD_GetIndexedString(HANDLE h, ULONG index, PVOID buf, ULONG len)
{
    BOOLEAN ret = HidD_GetIndexedString(h, index, buf, len);
    const char *via = "native";

    if (!ret)
    {
        // Wine lacks IOCTL_HID_GET_INDEXED_STRING; USB devices conventionally
        // use 1/2/3 for manufacturer/product/serial.
        if (index == 1) ret = HidD_GetManufacturerString(h, buf, len), via = "manufacturer";
        else if (index == 2) ret = HidD_GetProductString(h, buf, len), via = "product";
        else if (index == 3) ret = HidD_GetSerialNumberString(h, buf, len), via = "serial";
        else via = "unmapped";
    }
    hixlog("GetIndexedString h=%p index=%lu ret=%d via=%s str=\"%ls\"", h, index, ret, via, ret ? (WCHAR *)buf : L"");
    return ret;
}

BOOLEAN WINAPI hix_HidD_GetFeature(HANDLE h, PVOID buf, ULONG len)
{
    char dump[1024];
    BOOLEAN ret = HidD_GetFeature(h, buf, len);
    hex(dump, sizeof(dump), buf, ret ? len : 1);
    hixlog("GET_FEATURE h=%p len=%lu ret=%d err=%lu%s", h, len, ret, ret ? 0 : GetLastError(), dump);
    return ret;
}

// Walk saved-EBP frames (Delphi code keeps them) to show who issued the call.
static void callers(char *out, size_t size)
{
    void **frame = __builtin_frame_address(0);
    size_t n = 0;
    MEMORY_BASIC_INFORMATION mbi;

    out[0] = 0;
    for (int depth = 0; depth < 12 && frame; depth++)
    {
        if (!VirtualQuery(frame, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) break;
        n += snprintf(out + n, size - n, " %p", frame[1]);
        if ((void **)frame[0] <= frame) break;
        frame = frame[0];
    }
}

BOOLEAN WINAPI hix_HidD_SetFeature(HANDLE h, PVOID buf, ULONG len)
{
    char dump[1024], stack[256];
    BOOLEAN ret;
    callers(stack, sizeof(stack));
    hixlog("SET_FEATURE callers:%s", stack);
    hex(dump, sizeof(dump), buf, len);
    ret = HidD_SetFeature(h, buf, len);
    hixlog("SET_FEATURE h=%p len=%lu ret=%d err=%lu%s", h, len, ret, ret ? 0 : GetLastError(), dump);
    return ret;
}
