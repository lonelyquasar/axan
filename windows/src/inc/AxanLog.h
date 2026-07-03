// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <winrt/Windows.Storage.h>

// axan M12 (B.13): file-only structured logging for the axan frontend seams.
//
// The global workflow.md logging preference asks for structured, file-only logs (never
// stdout) on system-complexity projects, recording function entry/exit, backend command
// execution, decision branches, resolved config values, file paths read/written, and
// error details. WT's own telemetry goes through TraceLogging (ETW) — that is not a file
// you can `tail`, so this is a separate, additive sink.
//
// Header-only and placed in the shared src/inc (the AxanIconRegistry.h precedent) so BOTH
// the TerminalApp binary and the Microsoft.Terminal.Settings.Editor binary log through the
// same one implementation — the editor-side TOML importer (#431/#435) needs to log and the
// two DLLs share no axan symbols. Each module inlines its own copy; all copies append to
// the same file with open-append-close per line, so cross-module interleaving is no worse
// than the preexisting cross-process case. Expects the Win32 basics (GetCurrentProcessId,
// GetSystemTime, sprintf_s) from the consumer's pch, which every cascadia project provides.
//
// One RFC 5424-inspired line per call lands in <LocalState>\logs\axan.log (rolled to
// axan.log.1 when it passes ~5 MB; one generation of history is kept):
//   2026-06-02T14:33:01.123Z INFO  AxanSessionTree [axan@0 pid="1234" tid="56" k="v"] message
//
// The bracketed group is an RFC 5424 STRUCTURED-DATA element (SD-ID "axan@0") carrying
// pid/tid plus any caller-supplied key/value fields. Logging never throws: an unpackaged
// process (no LocalState) or a write failure is swallowed so a log call can't take down
// the terminal. Thread-safe.
namespace Axan::Log
{
    enum class Level
    {
        Debug,
        Info,
        Warn,
        Error,
    };

    // A single structured-data field. The value is an owning string so callers can pass
    // std::to_string(...) results and other temporaries without lifetime worries.
    using Field = std::pair<std::string_view, std::string>;

    namespace details
    {
        // <LocalState>\logs\axan.log — beside settings.json / state.json (D15). The same
        // package-identity store the rest of axan persists into. ApplicationData::Current()
        // throws for an unpackaged process; Write() guards the whole call.
        inline std::filesystem::path _logPath()
        {
            const auto localFolder = winrt::Windows::Storage::ApplicationData::Current().LocalFolder();
            return std::filesystem::path{ localFolder.Path().c_str() } / L"logs" / L"axan.log";
        }

        // Fixed-width so columns line up when scanning the file by eye.
        inline const char* _levelName(Level level) noexcept
        {
            switch (level)
            {
            case Level::Debug:
                return "DEBUG";
            case Level::Warn:
                return "WARN ";
            case Level::Error:
                return "ERROR";
            case Level::Info:
            default:
                return "INFO ";
            }
        }

        // ISO 8601 UTC with milliseconds, e.g. 2026-06-02T14:33:01.123Z.
        inline std::string _timestamp()
        {
            SYSTEMTIME st{};
            GetSystemTime(&st);
            char buf[40]{};
            sprintf_s(buf,
                      "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                      st.wYear,
                      st.wMonth,
                      st.wDay,
                      st.wHour,
                      st.wMinute,
                      st.wSecond,
                      st.wMilliseconds);
            return std::string{ buf };
        }

        // Escape an SD-PARAM value per RFC 5424 §6.3.3: ", \, and ] must be backslash-escaped.
        inline std::string _escape(std::string_view value)
        {
            std::string out;
            out.reserve(value.size());
            for (const auto ch : value)
            {
                if (ch == '"' || ch == '\\' || ch == ']')
                {
                    out.push_back('\\');
                }
                out.push_back(ch);
            }
            return out;
        }

        // Serializes writers and the lazy directory-create within this module. axan log
        // volume is low (tree load/save, TOML round-trips, sidebar/theme decisions — not
        // hot loops), so an open-append-close per line is cheaper than the bookkeeping of
        // a kept-open handle and guarantees each line is flushed to disk.
        inline std::mutex& _logMutex()
        {
            static std::mutex mutex;
            return mutex;
        }

        // Size-based rotation (#436 item 10): Info lines fire on every shell spawn and
        // settings reload, and nothing ever pruned the file. When axan.log exceeds ~5 MB
        // at write time (each Write reopens the file, so "at open time" is every line),
        // roll it to axan.log.1 — replacing any previous .1 — and let the write below
        // start a fresh file. One generation of history is enough for "what happened
        // before the restart". Best-effort by design: every filesystem call takes a
        // std::error_code, and a failed roll (e.g. another process still has the file
        // open without delete sharing) just means we keep appending and retry on the
        // next Write. Rotation must never break logging: the filesystem calls can't throw
        // (error_code overloads) and anything else (path-copy bad_alloc) propagates into
        // Write's catch-all — deliberately not noexcept, which would terminate instead.
        // Caller holds _logMutex.
        inline constexpr std::uintmax_t _maxLogBytes = 5u * 1024u * 1024u;

        inline void _rotateIfNeeded(const std::filesystem::path& path)
        {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size <= _maxLogBytes)
            {
                return; // missing file, unreadable size, or still small: nothing to do
            }
            auto rolled = path;
            rolled += L".1";
            std::filesystem::remove(rolled, ec);
            ec.clear();
            std::filesystem::rename(path, rolled, ec);
        }
    }

    inline void Write(Level level, std::string_view component, std::string_view message, std::initializer_list<Field> fields = {})
    try
    {
        std::string line{ details::_timestamp() };
        line += ' ';
        line += details::_levelName(level);
        line += ' ';
        line.append(component.data(), component.size());

        // STRUCTURED-DATA element: SD-ID "axan@0" (the "@0" private-enterprise form RFC
        // 5424 §6.3.2 allows) carrying pid/tid plus the caller's fields.
        line += " [axan@0 pid=\"";
        line += std::to_string(static_cast<unsigned long>(GetCurrentProcessId()));
        line += "\" tid=\"";
        line += std::to_string(static_cast<unsigned long>(GetCurrentThreadId()));
        line += '"';
        for (const auto& [key, value] : fields)
        {
            line += ' ';
            line.append(key.data(), key.size());
            line += "=\"";
            line += details::_escape(value);
            line += '"';
        }
        line += "] ";
        line.append(message.data(), message.size());
        line += '\n';

        const auto path = details::_logPath();

        std::scoped_lock guard{ details::_logMutex() };
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        details::_rotateIfNeeded(path);
        std::ofstream stream{ path, std::ios::app | std::ios::binary };
        if (stream)
        {
            stream << line;
        }
    }
    catch (...)
    {
        // Logging must never take down the terminal. A failure here (unpackaged process
        // with no LocalState, disk full, locked file) is intentionally swallowed.
    }

    inline void Debug(std::string_view component, std::string_view message, std::initializer_list<Field> fields = {})
    {
        Write(Level::Debug, component, message, fields);
    }
    inline void Info(std::string_view component, std::string_view message, std::initializer_list<Field> fields = {})
    {
        Write(Level::Info, component, message, fields);
    }
    inline void Warn(std::string_view component, std::string_view message, std::initializer_list<Field> fields = {})
    {
        Write(Level::Warn, component, message, fields);
    }
    inline void Error(std::string_view component, std::string_view message, std::initializer_list<Field> fields = {})
    {
        Write(Level::Error, component, message, fields);
    }
}
