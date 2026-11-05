// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #29: the per-terminal OSC 7501 Program Status record store.
//
// The adapter (terminal/adapter/ProgramStatus.hpp) owns the wire format and validation;
// this file owns the records and the spec's lifetime rules:
// - A report fully replaces its record (a missing key clears that value).
// - `clear` with an id removes that record and its subtree; without an id, everything.
// - OSC 133;A (new prompt) and connection close drop live (working/blocked) records.
//   done/error/idle survive both; "seen" acknowledgement is the consumer's job (#30).
// - RIS removes everything. DECSTR and screen switches don't touch records.
// - At most ProgramStatus::MaxRecords records; past that, the least recently updated
//   record is evicted.
//
// Feature gate: Terminal::_programStatusEnabled is the ONE switch. It defaults to on.
// Off means the `?` query goes unanswered (so a well-behaved program never sends
// reports) and any unsolicited reports are ignored. ControlCore/TermControl expose it so
// the app can turn it off; nothing else gates the feature.
//
// Every mutation runs under the terminal write lock (it arrives through the VT parser or
// from ControlCore holding the lock) and ends in _programStatusNotify(), which only calls
// the owner's callback. The owner raises its UI-facing event after releasing the lock.

#include "pch.h"
#include "Terminal.hpp"

#include "../../inc/AxanLog.h"

using namespace Microsoft::Terminal::Core;
using namespace Microsoft::Console::VirtualTerminal;

namespace
{
    constexpr std::string_view LogComponent{ "ProgramStatus" };

    std::string Utf8(std::wstring_view s)
    {
        return til::u16u8(s);
    }

    std::string IdForLog(std::wstring_view id)
    {
        return id.empty() ? std::string{ "<root>" } : Utf8(id);
    }

    // true if `key` is `id` itself or lies under it ("a" covers "a" and "a/...").
    bool IsInSubtree(std::wstring_view key, std::wstring_view id) noexcept
    {
        return key.size() >= id.size() && key.substr(0, id.size()) == id &&
               (key.size() == id.size() || key[id.size()] == L'/');
    }
}

void Terminal::SetProgramStatusEnabled(const bool enabled)
{
    if (_programStatusEnabled == enabled)
    {
        return;
    }
    _programStatusEnabled = enabled;
    Axan::Log::Info(LogComponent, enabled ? "feature enabled" : "feature disabled");
    // Turning the feature off drops what it collected, so nothing stale lingers.
    if (!enabled && !_programStatusRecords.empty())
    {
        _programStatusRecords.clear();
        _programStatusNotify();
    }
}

bool Terminal::IsProgramStatusEnabled() const noexcept
{
    return _programStatusEnabled;
}

void Terminal::SetProgramStatusChangedCallback(std::function<void()> pfn) noexcept
{
    _pfnProgramStatusChanged.swap(pfn);
}

void Terminal::_programStatusNotify()
{
    if (_pfnProgramStatusChanged)
    {
        _pfnProgramStatusChanged();
    }
}

bool Terminal::ProgramStatusQuery()
{
    _assertLocked();
    Axan::Log::Info(LogComponent, _programStatusEnabled ? "query answered" : "query not answered: feature off");
    return _programStatusEnabled;
}

void Terminal::ProgramStatusReport(const ProgramStatus::ParseResult& result)
{
    _assertLocked();

    if (!_programStatusEnabled)
    {
        Axan::Log::Debug(LogComponent, "report ignored: feature off");
        return;
    }

    if (result.outcome != ProgramStatus::Outcome::Report)
    {
        Axan::Log::Warn(LogComponent, "report discarded", { { "reason", std::string{ result.reason } } });
        return;
    }

    const auto& report = result.report;

    if (report.state == ProgramStatus::State::Clear)
    {
        size_t removed = 0;
        if (report.id.empty())
        {
            removed = _programStatusRecords.size();
            _programStatusRecords.clear();
        }
        else
        {
            // The subtree is contiguous in the ordered map: "a", then "a/...". Anything
            // else starting with "a" (e.g. "a-b", "ab") sorts outside or is skipped.
            for (auto it = _programStatusRecords.lower_bound(report.id); it != _programStatusRecords.end();)
            {
                if (IsInSubtree(it->first, report.id))
                {
                    it = _programStatusRecords.erase(it);
                    ++removed;
                }
                else if (it->first.size() >= report.id.size() && std::wstring_view{ it->first }.substr(0, report.id.size()) == report.id)
                {
                    ++it; // shares the prefix but isn't a descendant ("a-b"); keep scanning
                }
                else
                {
                    break;
                }
            }
        }
        Axan::Log::Info(LogComponent, "report accepted: clear", { { "id", report.id.empty() ? std::string{ "<all>" } : Utf8(report.id) }, { "removed", std::to_string(removed) } });
        _programStatusNotify();
        return;
    }

    auto it = _programStatusRecords.find(report.id);
    if (it == _programStatusRecords.end() && _programStatusRecords.size() >= ProgramStatus::MaxRecords)
    {
        // Evict the least recently updated record (lowest revision) to make room.
        auto oldest = _programStatusRecords.begin();
        for (auto scan = _programStatusRecords.begin(); scan != _programStatusRecords.end(); ++scan)
        {
            if (scan->second.revision < oldest->second.revision)
            {
                oldest = scan;
            }
        }
        Axan::Log::Info(LogComponent, "record evicted: cap reached", { { "id", IdForLog(oldest->first) } });
        _programStatusRecords.erase(oldest);
    }

    auto& record = _programStatusRecords[report.id];
    record.id = report.id;
    record.state = report.state;
    record.app = report.app;
    record.effectiveApp.clear(); // resolved at snapshot time
    record.kind = report.kind;
    record.progress = report.progress;
    record.title = report.title;
    record.msg = report.msg;
    record.revision = ++_programStatusRevision;

    Axan::Log::Info(LogComponent,
                    "report accepted",
                    { { "id", IdForLog(report.id) },
                      { "state", std::string{ ProgramStatus::StateName(report.state) } },
                      { "app", Utf8(report.app) },
                      { "kind", std::string{ ProgramStatus::KindName(report.kind) } },
                      { "progress", report.progress ? std::to_string(*report.progress) : std::string{} },
                      { "titleLen", std::to_string(report.title.size()) },
                      { "msgLen", std::to_string(report.msg.size()) },
                      { "skippedPairs", std::to_string(result.skippedPairs) },
                      { "records", std::to_string(_programStatusRecords.size()) } });
    _programStatusNotify();
}

bool Terminal::_programStatusDropLive(std::string_view reason)
{
    size_t removed = 0;
    for (auto it = _programStatusRecords.begin(); it != _programStatusRecords.end();)
    {
        if (ProgramStatus::IsLive(it->second.state))
        {
            it = _programStatusRecords.erase(it);
            ++removed;
        }
        else
        {
            ++it;
        }
    }
    if (removed != 0)
    {
        Axan::Log::Info(LogComponent, "live records dropped", { { "reason", std::string{ reason } }, { "removed", std::to_string(removed) } });
    }
    return removed != 0;
}

void Terminal::ProgramStatusPromptStarted()
{
    _assertLocked();
    if (_programStatusDropLive("prompt-start"))
    {
        _programStatusNotify();
    }
}

void Terminal::ProgramStatusDropLive(std::string_view reason)
{
    _assertLocked();
    if (_programStatusDropLive(reason))
    {
        _programStatusNotify();
    }
}

void Terminal::ProgramStatusReset()
{
    _assertLocked();
    if (_programStatusRecords.empty())
    {
        return;
    }
    Axan::Log::Info(LogComponent, "records cleared: reset", { { "removed", std::to_string(_programStatusRecords.size()) } });
    _programStatusRecords.clear();
    _programStatusNotify();
}

std::vector<Terminal::ProgramStatusRecord> Terminal::GetProgramStatusRecords() const
{
    _assertLocked();
    std::vector<ProgramStatusRecord> out;
    out.reserve(_programStatusRecords.size());
    for (const auto& [id, record] : _programStatusRecords)
    {
        auto& copy = out.emplace_back(record);
        if (!copy.app.empty())
        {
            copy.effectiveApp = copy.app;
            continue;
        }
        // Inherit from the nearest ancestor that has an app: "a/b/c" -> "a/b" -> "a" -> root.
        std::wstring_view ancestor{ id };
        while (!ancestor.empty())
        {
            const auto slash = ancestor.rfind(L'/');
            ancestor = slash == std::wstring_view::npos ? std::wstring_view{} : ancestor.substr(0, slash);
            const auto found = _programStatusRecords.find(ancestor);
            if (found != _programStatusRecords.end() && !found->second.app.empty())
            {
                copy.effectiveApp = found->second.app;
                break;
            }
        }
    }
    return out;
}

// axan #30: OSC 133 command lifecycle -> "is a shell command running". A plain shell's
// sidebar state is running/idle from this; a non-zero exit is logged but deliberately
// not surfaced as an error state (a failed `ls` isn't something to flag in the tree).
bool Terminal::IsShellCommandRunning() const noexcept
{
    return _shellCommandRunning;
}

void Terminal::SetShellCommandChangedCallback(std::function<void()> pfn) noexcept
{
    _pfnShellCommandChanged.swap(pfn);
}

void Terminal::ShellCommandMark(const wchar_t mark, const std::optional<unsigned int> exitCode)
{
    _assertLocked();
    const bool running = mark == L'C';
    if (running == _shellCommandRunning)
    {
        return;
    }
    _shellCommandRunning = running;
    Axan::Log::Debug(LogComponent,
                     running ? "shell command started" : "shell command ended",
                     { { "mark", std::string(1, static_cast<char>(mark)) },
                       { "exitCode", exitCode ? std::to_string(*exitCode) : std::string{} } });
    if (_pfnShellCommandChanged)
    {
        _pfnShellCommandChanged();
    }
}
