#pragma once

#include <excpt.h>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

struct _EXCEPTION_POINTERS;

// Durable log for the Glacier Relay adapter. Derived from the Hitmen probe's HitmenLog
// (research/hitmen-revival, ff19bd57 and e748cdde), which was validated in the first runtime load.
//
// The SDK's own file log buffers and is only flushed from a destructor that never runs, because the
// game terminates its own process at exit (glacier-relay HITMEN_COMPILE_ARCHAEOLOGY.md, experiment
// 4, F9). This log does not go through it: every line is one unbuffered WriteFile to a per-process
// file outside the game directory, and goes to OutputDebugString for an attached debugger.
//
//   %LOCALAPPDATA%\GlacierRelay\Relay\relay-<process start, UTC>-<pid>.log
//   (or $GLACIERRELAY_LOG_DIR\relay-...log when that variable is set)
//
// This file depends on fmt and Win32 only, never on the SDK, so the engine-independent layers can
// use it and still be built and tested without SDK headers.
//
// It never mutates game state and never handles an exception: Guard only reports a fault that is
// already on its way out.
namespace RelayLog
{
    enum class Level
    {
        Info,
        Warn,
        Error,
    };

    void Write(Level p_Level, std::string_view p_Message);

    template <typename... Args>
    void Info(fmt::format_string<Args...> p_Format, Args&&... p_Args)
    {
        Write(Level::Info, fmt::format(p_Format, std::forward<Args>(p_Args)...));
    }

    template <typename... Args>
    void Warn(fmt::format_string<Args...> p_Format, Args&&... p_Args)
    {
        Write(Level::Warn, fmt::format(p_Format, std::forward<Args>(p_Args)...));
    }

    template <typename... Args>
    void Error(fmt::format_string<Args...> p_Format, Args&&... p_Args)
    {
        Write(Level::Error, fmt::format(p_Format, std::forward<Args>(p_Args)...));
    }

    // SEH filter for Guard. Logs the exception and always returns EXCEPTION_CONTINUE_SEARCH.
    int LogFault(const char* p_Where, _EXCEPTION_POINTERS* p_Info);

    // Runs p_Func. If a structured exception (access violation, C++ exception, ...) escapes it, the
    // fault is logged and then keeps propagating exactly as it would have without the guard.
    template <typename TFunc>
    void Guard(const char* p_Where, const TFunc& p_Func)
    {
        __try
        {
            p_Func();
        }
        __except (LogFault(p_Where, GetExceptionInformation()))
        {
        }
    }

    // Path of the log file as UTF-8, or an empty string if it could not be opened.
    std::string Path();

    // For DllMain only. Safe under the loader lock: file and debugger output only, no SDK calls.
    void ModuleAttached(void* p_Module);
    void ModuleDetaching(bool p_ProcessTerminating);
}
