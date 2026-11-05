// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#pragma once

// axan #30: the sidebar's per-session state model, as a pure function of plain inputs
// (no WinRT, no XAML) so it can be unit-tested outside the app. TerminalPage gathers the
// inputs from a tab's panes (TermControl::ProgramStatusRecords / ShellCommandRunning, the
// tab's bell indicator) and pushes the result onto the node's SessionNodeViewModel.
//
// States, strongest first (a tab with several panes shows its strongest pane):
//   needs-you    OSC 7501 blocked (permission / question / auth)
//   error        OSC 7501 error, not yet seen
//   done-unseen  OSC 7501 done not yet seen, or (plain shells) the tab's bell indicator
//   working      OSC 7501 working
//   running      OSC 133 C .. D/A: a shell command is executing
//   idle         everything else (incl. seen done/error, OSC 7501 idle)
//
// Sources per pane, in priority order:
//   1. OSC 7501 records. If a pane has any, they are authoritative for that pane: a Claude
//      Code session sitting at its own prompt reports `idle` while the shell around it is
//      still "running" claude, and idle is the truth there.
//   2. OSC 133 command lifecycle -> running / idle. A non-zero exit is NOT an error state:
//      a failed `ls` isn't something to flag in the tree.
// Then, tab-wide: if the strongest pane state is idle and the tab's bell indicator is up,
// the node is done-unseen with message "bell". WT already clears that indicator when the
// tab gains focus, which is the same "seen" rule the 7501 records follow, so the bell is
// a reasonable plain-shell "something finished / wants you" signal (`make; printf '\a'`).
//
// "Seen": done/error records are unread until the node is selected or its pane gains
// focus. The caller acknowledges by remembering, per control, the highest record Revision
// it has shown the user; a done/error record whose revision is <= that is `seen`.

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Axan::SessionState
{
    enum class State : uint8_t
    {
        Idle = 0,
        Running = 1,
        Working = 2,
        DoneUnseen = 3,
        Error = 4,
        NeedsYou = 5,
    };

    // Mirrors Microsoft.Terminal.Control.ProgramStatusState / ProgramStatusKind numerically.
    enum class RecordState : uint8_t
    {
        Idle = 0,
        Working = 1,
        Done = 2,
        Blocked = 3,
        Error = 4,
    };
    enum class RecordKind : uint8_t
    {
        None = 0,
        Permission = 1,
        Question = 2,
        Auth = 3,
    };

    struct RecordInput
    {
        bool isRoot{ false };
        RecordState state{ RecordState::Idle };
        RecordKind kind{ RecordKind::None };
        std::wstring_view msg;
        std::wstring_view title;
        uint64_t revision{ 0 };
        bool seen{ false };
    };

    struct PaneInput
    {
        std::vector<RecordInput> records;
        bool commandRunning{ false };
    };

    struct Result
    {
        State state{ State::Idle };
        std::wstring message; // 7501 msg (or title when msg is empty), "bell", or ""
        std::wstring kind; // "permission" / "question" / "auth" for needs-you, else ""
        std::string_view source{ "none" }; // "osc7501" / "osc133" / "bell" / "none"
    };

    constexpr int Rank(State s) noexcept
    {
        return static_cast<int>(s);
    }

    constexpr std::wstring_view Name(State s) noexcept
    {
        switch (s)
        {
        case State::Running:
            return L"running";
        case State::Working:
            return L"working";
        case State::DoneUnseen:
            return L"done-unseen";
        case State::Error:
            return L"error";
        case State::NeedsYou:
            return L"needs-you";
        case State::Idle:
        default:
            return L"idle";
        }
    }

    constexpr std::wstring_view KindName(RecordKind k) noexcept
    {
        switch (k)
        {
        case RecordKind::Permission:
            return L"permission";
        case RecordKind::Question:
            return L"question";
        case RecordKind::Auth:
            return L"auth";
        case RecordKind::None:
        default:
            return L"";
        }
    }

    constexpr State FromRecord(const RecordInput& r) noexcept
    {
        switch (r.state)
        {
        case RecordState::Blocked:
            return State::NeedsYou;
        case RecordState::Error:
            return r.seen ? State::Idle : State::Error;
        case RecordState::Done:
            return r.seen ? State::Idle : State::DoneUnseen;
        case RecordState::Working:
            return State::Working;
        case RecordState::Idle:
        default:
            return State::Idle;
        }
    }

    inline Result Derive(std::span<const PaneInput> panes, bool bellIndicator)
    {
        Result best;
        const RecordInput* bestRecord = nullptr;
        bool haveBest = false;

        for (const auto& pane : panes)
        {
            if (!pane.records.empty())
            {
                for (const auto& r : pane.records)
                {
                    const auto s = FromRecord(r);
                    // Stronger state wins; on a tie prefer the root record, then the most
                    // recently written one (highest revision) for the message.
                    const bool better = !haveBest || Rank(s) > Rank(best.state) ||
                                        (Rank(s) == Rank(best.state) && bestRecord &&
                                         ((r.isRoot && !bestRecord->isRoot) ||
                                          (r.isRoot == bestRecord->isRoot && r.revision > bestRecord->revision))) ||
                                        (Rank(s) == Rank(best.state) && !bestRecord);
                    if (better)
                    {
                        haveBest = true;
                        best.state = s;
                        best.source = "osc7501";
                        bestRecord = &r;
                    }
                }
            }
            else
            {
                const auto s = pane.commandRunning ? State::Running : State::Idle;
                if (!haveBest || Rank(s) > Rank(best.state))
                {
                    haveBest = true;
                    best.state = s;
                    best.source = pane.commandRunning ? "osc133" : "none";
                    bestRecord = nullptr;
                }
            }
        }

        if (bestRecord && best.state != State::Idle)
        {
            best.message.assign(!bestRecord->msg.empty() ? bestRecord->msg : bestRecord->title);
            if (best.state == State::NeedsYou)
            {
                best.kind.assign(KindName(bestRecord->kind));
            }
        }

        if (best.state == State::Idle && bellIndicator)
        {
            best.state = State::DoneUnseen;
            best.message = L"bell";
            best.kind.clear();
            best.source = "bell";
        }
        return best;
    }
}
