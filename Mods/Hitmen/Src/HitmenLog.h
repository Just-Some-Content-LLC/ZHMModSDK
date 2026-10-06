#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "Logging.h"

// Durable log for the Hitmen revival.
//
// The SDK's own file log buffers and is only flushed from a destructor that does not run at game
// exit, so anything logged after early startup is lost (glacier-relay M0_BASELINE.md, F3). This
// log does not go through it: every line is written with one unbuffered WriteFile to a per-process
// file outside the game directory, and to OutputDebugString for an attached debugger.
//
//   %LOCALAPPDATA%\GlacierRelay\Hitmen\hitmen-<process start, UTC>-<pid>.log
//
// It never mutates game state and never handles an exception.
namespace HitmenLog
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

    // Path of the log file as UTF-8, or an empty string if it could not be opened.
    std::string Path();

    // For DllMain only. Safe under the loader lock: file and debugger output only, no SDK calls.
    void ModuleAttached(void* p_Module);
    void ModuleDetaching(bool p_ProcessTerminating);
}
