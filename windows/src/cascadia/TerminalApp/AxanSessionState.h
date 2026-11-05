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
//
// After seen (setting `sidebar.statusAfterSeen`): with `clear` a seen done/error record is
// simply idle. With `keep` (the default) it is still idle for every attention rule (#31
// de-emphasis, #33 badges, #34 Needs-you), but Result.retained carries it so the row keeps
// showing the last report, muted, while the node rests (idle / running). It goes away when
// the record is rewritten (a new report) or cleared, or the pane closes.
//
// Slot: Result.slot is the state the row's right-edge glyph slot shows (setting
// `sidebar.stateGlyphs`, keyed by SlotKey): blocked / error / done / working / running map
// straight from the node state; an idle node is `idle` only when some pane has an OSC 7501
// record (a status-reporting program such as Claude Code is present and at rest, including
// a done/error report already seen), and None otherwise (a plain shell at its prompt).

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
        std::wstring_view id; // "" = root; only used for Result::reportKey
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
        uint64_t paneId{ 0 }; // the control's ContentId; only used for Result::reportKey
    };

    struct Options
    {
        bool keepAfterSeen{ false }; // sidebar.statusAfterSeen == keep
    };

    // A seen done/error report kept on display (keep mode, node idle / running).
    // What the right-edge glyph slot shows (see the header comment). None = no slot state.
    enum class Slot : uint8_t
    {
        None = 0,
        Idle,
        Working,
        Blocked,
        Done,
        Error,
        Running,
    };

    // The sidebar.stateGlyphs key for a slot state; "" for None.
    constexpr std::wstring_view SlotKey(Slot s) noexcept
    {
        switch (s)
        {
        case Slot::Idle:
            return L"idle";
        case Slot::Working:
            return L"working";
        case Slot::Blocked:
            return L"blocked";
        case Slot::Done:
            return L"done";
        case Slot::Error:
            return L"error";
        case Slot::Running:
            return L"running";
        case Slot::None:
        default:
            return L"";
        }
    }

    struct Retained
    {
        bool present{ false };
        RecordState state{ RecordState::Done }; // Done or Error
        std::wstring message; // msg, else title, else ""
    };

    struct Result
    {
        State state{ State::Idle };
        std::wstring message; // 7501 msg (or title when msg is empty), "bell", or ""
        std::wstring kind; // "permission" / "question" / "auth" for needs-you, else ""
        std::string_view source{ "none" }; // "osc7501" / "osc133" / "bell" / "none"
        Slot slot{ Slot::None }; // the glyph slot's state (see the header comment)
        Retained retained; // keep mode only; set only while the node is idle / running
        // Identity of the done/error report the row's age counts from: "<paneId>:<id>:<state>"
        // for an unseen done/error that decided the state, or for the retained report; ""
        // otherwise. Stable across msg rewrites of the same state (done, then done + msg),
        // so the caller can keep "since" when it doesn't change, e.g. across acknowledgement.
        std::wstring reportKey;
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

    inline std::wstring ReportKey(const PaneInput& pane, const RecordInput& r)
    {
        std::wstring key = std::to_wstring(pane.paneId);
        key += L':';
        key.append(r.id);
        key += L':';
        key += std::to_wstring(static_cast<int>(r.state));
        return key;
    }

    inline Result Derive(std::span<const PaneInput> panes, bool bellIndicator, const Options& options = {})
    {
        Result best;
        const RecordInput* bestRecord = nullptr;
        const PaneInput* bestPane = nullptr;
        bool haveBest = false;
        bool anyRecords = false; // a status-reporting program is present (idle slot)

        for (const auto& pane : panes)
        {
            if (!pane.records.empty())
            {
                anyRecords = true;
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
                        bestPane = &pane;
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
                    bestPane = nullptr;
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
            if (best.state == State::DoneUnseen || best.state == State::Error)
            {
                best.reportKey = ReportKey(*bestPane, *bestRecord);
            }
        }

        if (best.state == State::Idle && bellIndicator)
        {
            best.state = State::DoneUnseen;
            best.message = L"bell";
            best.kind.clear();
            best.source = "bell";
        }

        // keep: while the node rests, show the strongest seen done/error report (error over
        // done, then root, then newest), same tie-breaks as the state pick.
        if (options.keepAfterSeen && (best.state == State::Idle || best.state == State::Running))
        {
            const RecordInput* kept = nullptr;
            const PaneInput* keptPane = nullptr;
            for (const auto& pane : panes)
            {
                for (const auto& r : pane.records)
                {
                    if (!r.seen || (r.state != RecordState::Done && r.state != RecordState::Error))
                    {
                        continue;
                    }
                    const bool better = !kept ||
                                        (r.state == RecordState::Error && kept->state != RecordState::Error) ||
                                        (r.state == kept->state &&
                                         ((r.isRoot && !kept->isRoot) ||
                                          (r.isRoot == kept->isRoot && r.revision > kept->revision)));
                    if (better)
                    {
                        kept = &r;
                        keptPane = &pane;
                    }
                }
            }
            if (kept)
            {
                best.retained.present = true;
                best.retained.state = kept->state;
                best.retained.message.assign(!kept->msg.empty() ? kept->msg : kept->title);
                best.reportKey = ReportKey(*keptPane, *kept);
            }
        }
        switch (best.state)
        {
        case State::NeedsYou:
            best.slot = Slot::Blocked;
            break;
        case State::Error:
            best.slot = Slot::Error;
            break;
        case State::DoneUnseen:
            best.slot = Slot::Done;
            break;
        case State::Working:
            best.slot = Slot::Working;
            break;
        case State::Running:
            best.slot = Slot::Running;
            break;
        case State::Idle:
        default:
            best.slot = anyRecords ? Slot::Idle : Slot::None;
            break;
        }
        return best;
    }
}
