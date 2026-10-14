#pragma once

#include <excpt.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

struct _EXCEPTION_POINTERS;

// Native log for the Glacier Relay adapter. Derived from the Hitmen probe's HitmenLog
// (research/hitmen-revival, ff19bd57 and e748cdde), which was validated in the first runtime load.
//
// The SDK's own file log buffers and is only flushed from a destructor that never runs, because the
// game terminates its own process at exit (glacier-relay HITMEN_COMPILE_ARCHAEOLOGY.md, experiment
// 4, F9). This log does not go through it: every line is one unbuffered WriteFile to a per-process
// file outside the game directory, and goes to OutputDebugString for an attached debugger. A line
// WriteFile accepted is in the operating system's file cache: it survives this process ending in
// any way, but nothing here flushes it to the disk (no FlushFileBuffers, no write-through), so it
// is not durable across a power loss or an operating-system crash (M5 design section 29.5).
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

    // M5 C-2B correction B (M5 design section 30): the outcome of one line's write, for the one
    // line that must be in the file before an engine write is asked for (OutfitApply's "applying"
    // line). Written means the file was open and WriteFile returned success having accepted every
    // byte of the line: a successful submission to the operating system, which is not a flush, not
    // power-loss durability, and says nothing about what happens after it.
    enum class WriteStatus
    {
        Written,         // the whole line was accepted by the operating system
        FileUnavailable, // the log file could not be opened (no directory, CreateFileW failed) or is closed
        WriteFailed,     // WriteFile returned failure
        ShortWrite,      // WriteFile returned success with fewer bytes than the line
    };

    const char* WriteStatusText(WriteStatus p_Status);

    struct WriteOutcome
    {
        WriteStatus status = WriteStatus::FileUnavailable;
        uint32_t requested = 0; // bytes of the line (prefix, message, newline)
        uint32_t written = 0;   // bytes WriteFile reported accepted
        uint32_t error = 0;     // GetLastError after a failed WriteFile, or of the failed open when
                                // the file never opened (FileUnavailable); 0 when no code applies
    };

    // Writes one line like Write and reports how the write went. Ordinary diagnostics keep using
    // Info/Warn/Error, whose result is still ignored.
    WriteOutcome WriteChecked(Level p_Level, std::string_view p_Message);

    template <typename... Args>
    WriteOutcome InfoChecked(fmt::format_string<Args...> p_Format, Args&&... p_Args)
    {
        return WriteChecked(Level::Info, fmt::format(p_Format, std::forward<Args>(p_Args)...));
    }

    // The raw file write beneath every line (the one WriteFile, under the lock, after the lazy
    // open) as a replaceable function, so the engine-independent tests can script an unopened
    // file, a failed write and a short write without a real file. Null, the default, is the file
    // write. Tests only; the DLL never sets it. Returns the previous writer.
    struct RawWrite
    {
        bool file_open = false;
        bool succeeded = false;
        uint32_t written = 0;
        uint32_t error = 0;
    };

    using RawWriter = RawWrite (*)(const char* p_Data, uint32_t p_Size);
    RawWriter SetRawWriterForTests(RawWriter p_Writer);

    // SEH filter for Guard. Logs the exception and always returns EXCEPTION_CONTINUE_SEARCH.
    int LogFault(const char* p_Where, _EXCEPTION_POINTERS* p_Info);

    // M5 C-2B: how many faults the filter has seen in this process (any guard, any thread). The
    // effectful outfit path refuses to run once this is non-zero: a process that survived a fault
    // (an outer handler resumed it) is not a process this adapter will ask for an engine write.
    uint32_t FaultsLogged();

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
