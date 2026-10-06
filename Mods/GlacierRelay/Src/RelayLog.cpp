#include "RelayLog.h"

#include <Windows.h>

#include <cstdio>

namespace
{
    using RelayLog::Level;

    SRWLOCK g_Lock = SRWLOCK_INIT;
    HANDLE g_File = INVALID_HANDLE_VALUE;
    bool g_OpenAttempted = false;
    wchar_t g_Path[MAX_PATH] = {};

    // The file is named after the process (start time and pid), not after this module, because
    // the SDK loads every mod dll once to read its ABI version, frees it, and loads it again if
    // the mod is enabled (observed in experiment 4). Both loads must append to the same file.
    void OpenLocked()
    {
        if (g_OpenAttempted)
            return;

        g_OpenAttempted = true;

        wchar_t s_Dir[MAX_PATH] = {};

        // GLACIERRELAY_LOG_DIR overrides the location (used by the test runner so test logs stay
        // out of the real directory). Otherwise %LOCALAPPDATA%\GlacierRelay\Relay, then %TEMP%.
        const DWORD s_OverrideLen = GetEnvironmentVariableW(L"GLACIERRELAY_LOG_DIR", s_Dir, MAX_PATH);
        const bool s_Override = s_OverrideLen != 0 && s_OverrideLen < MAX_PATH;

        if (!s_Override)
        {
            const DWORD s_Len = GetEnvironmentVariableW(L"LOCALAPPDATA", s_Dir, MAX_PATH);

            if (s_Len == 0 || s_Len >= MAX_PATH)
            {
                const DWORD s_TempLen = GetTempPathW(MAX_PATH, s_Dir);

                if (s_TempLen == 0 || s_TempLen >= MAX_PATH)
                {
                    OutputDebugStringA("[GlacierRelay] durable log unavailable: no LOCALAPPDATA or TEMP directory\n");
                    return;
                }
            }
        }

        FILETIME s_Creation = {}, s_Exit = {}, s_Kernel = {}, s_User = {};
        SYSTEMTIME s_Start = {};

        if (!GetProcessTimes(GetCurrentProcess(), &s_Creation, &s_Exit, &s_Kernel, &s_User) ||
            !FileTimeToSystemTime(&s_Creation, &s_Start))
        {
            GetSystemTime(&s_Start);
        }

        wchar_t s_Path[MAX_PATH] = {};
        wchar_t s_LogDir[MAX_PATH] = {};

        if (s_Override)
        {
            wcscpy_s(s_LogDir, s_Dir);
        }
        else
        {
            swprintf_s(s_Path, L"%s\\GlacierRelay", s_Dir);
            CreateDirectoryW(s_Path, nullptr);
            swprintf_s(s_LogDir, L"%s\\GlacierRelay\\Relay", s_Dir);
        }

        CreateDirectoryW(s_LogDir, nullptr);

        swprintf_s(
            s_Path, L"%s\\relay-%04u%02u%02u-%02u%02u%02u-%lu.log",
            s_LogDir, s_Start.wYear, s_Start.wMonth, s_Start.wDay, s_Start.wHour, s_Start.wMinute, s_Start.wSecond,
            GetCurrentProcessId()
        );

        g_File = CreateFileW(
            s_Path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr
        );

        if (g_File == INVALID_HANDLE_VALUE)
        {
            char s_Message[128];
            sprintf_s(s_Message, "[GlacierRelay] durable log unavailable: CreateFileW failed with error %lu\n", GetLastError());
            OutputDebugStringA(s_Message);
            return;
        }

        wcscpy_s(g_Path, s_Path);
    }

    void WriteLine(Level p_Level, std::string_view p_Message, bool p_TakeLock)
    {
        SYSTEMTIME s_Now;
        GetSystemTime(&s_Now);

        const char* s_LevelName = "INFO ";

        if (p_Level == Level::Warn)
            s_LevelName = "WARN ";
        else if (p_Level == Level::Error)
            s_LevelName = "ERROR";

        char s_Prefix[64];
        const int s_PrefixLen = sprintf_s(
            s_Prefix, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ %s [tid %5lu] ",
            s_Now.wYear, s_Now.wMonth, s_Now.wDay, s_Now.wHour, s_Now.wMinute, s_Now.wSecond, s_Now.wMilliseconds,
            s_LevelName, GetCurrentThreadId()
        );

        std::string s_Line;
        s_Line.reserve(s_PrefixLen + p_Message.size() + 1);
        s_Line.append(s_Prefix, s_PrefixLen);
        s_Line.append(p_Message);
        s_Line.push_back('\n');

        if (p_TakeLock)
            AcquireSRWLockExclusive(&g_Lock);

        OpenLocked();

        if (g_File != INVALID_HANDLE_VALUE)
        {
            DWORD s_Written = 0;
            WriteFile(g_File, s_Line.data(), static_cast<DWORD>(s_Line.size()), &s_Written, nullptr);
        }

        if (p_TakeLock)
            ReleaseSRWLockExclusive(&g_Lock);

        OutputDebugStringA(("[GlacierRelay] " + s_Line).c_str());
    }
}

void RelayLog::Write(Level p_Level, std::string_view p_Message)
{
    WriteLine(p_Level, p_Message, true);
}

int RelayLog::LogFault(const char* p_Where, EXCEPTION_POINTERS* p_Info)
{
    static volatile LONG s_Logged = 0;

    const auto* s_Record = p_Info->ExceptionRecord;

    // On a stack overflow there is no stack left to format a message with. A fault that some outer
    // handler resumes from could repeat every frame, so stop reporting after a few.
    if (s_Record->ExceptionCode == EXCEPTION_STACK_OVERFLOW || InterlockedIncrement(&s_Logged) > 16)
        return EXCEPTION_CONTINUE_SEARCH;

    HMODULE s_Module = nullptr;
    char s_ModulePath[MAX_PATH] = "unknown module";

    if (GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        static_cast<LPCSTR>(s_Record->ExceptionAddress), &s_Module
    ))
    {
        GetModuleFileNameA(s_Module, s_ModulePath, MAX_PATH);
    }

    std::string s_Access;

    if (s_Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && s_Record->NumberParameters >= 2)
    {
        const auto s_Kind = s_Record->ExceptionInformation[0];

        s_Access = fmt::format(
            ", {} address {:#x}",
            s_Kind == 0 ? "reading" : s_Kind == 1 ? "writing" : "executing", s_Record->ExceptionInformation[1]
        );
    }

    Error(
        "FAULT in {}: exception {:#010x} at {} ('{}' base {}, offset {:#x}){}. Not handled here; it continues to propagate.",
        p_Where, static_cast<uint32_t>(s_Record->ExceptionCode), fmt::ptr(s_Record->ExceptionAddress), s_ModulePath,
        fmt::ptr(s_Module),
        reinterpret_cast<uintptr_t>(s_Record->ExceptionAddress) - reinterpret_cast<uintptr_t>(s_Module), s_Access
    );

    return EXCEPTION_CONTINUE_SEARCH;
}

std::string RelayLog::Path()
{
    AcquireSRWLockExclusive(&g_Lock);
    OpenLocked();

    char s_Path[MAX_PATH * 3] = {};
    WideCharToMultiByte(CP_UTF8, 0, g_Path, -1, s_Path, sizeof(s_Path), nullptr, nullptr);

    ReleaseSRWLockExclusive(&g_Lock);

    return s_Path;
}

void RelayLog::ModuleAttached(void* p_Module)
{
    char s_ModulePath[MAX_PATH] = {};
    GetModuleFileNameA(static_cast<HMODULE>(p_Module), s_ModulePath, MAX_PATH);

    Info(
        "module attached: base {}, pid {}, path '{}', built " __DATE__ " " __TIME__,
        fmt::ptr(p_Module), GetCurrentProcessId(), s_ModulePath
    );
}

void RelayLog::ModuleDetaching(bool p_ProcessTerminating)
{
    // At process termination every other thread is already gone. One of them may have been
    // killed while holding the lock, so it must not be taken here.
    if (p_ProcessTerminating)
    {
        WriteLine(Level::Info, "module detaching: process is terminating", false);
        return;
    }

    WriteLine(Level::Info, "module detaching: FreeLibrary", true);

    // The statics die with the module, so release the handle. A later load reopens the same file.
    AcquireSRWLockExclusive(&g_Lock);

    if (g_File != INVALID_HANDLE_VALUE)
        CloseHandle(g_File);

    g_File = INVALID_HANDLE_VALUE;
    g_OpenAttempted = false;

    ReleaseSRWLockExclusive(&g_Lock);
}
