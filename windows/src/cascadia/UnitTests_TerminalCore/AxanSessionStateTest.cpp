// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #30: the sidebar session-state derivation (TerminalApp/AxanSessionState.h) is a pure
// function, so it is tested here rather than through the XAML app.

#include "pch.h"
#include <WexTestClass.h>

#include "../TerminalApp/AxanSessionState.h"

using namespace WEX::Logging;
using namespace Axan::SessionState;

namespace TerminalCoreUnitTests
{
    class AxanSessionStateTest
    {
        TEST_CLASS(AxanSessionStateTest);

        static RecordInput Rec(RecordState state, bool isRoot = true, uint64_t revision = 1, std::wstring_view msg = {}, RecordKind kind = RecordKind::None, bool seen = false)
        {
            RecordInput r;
            r.state = state;
            r.isRoot = isRoot;
            r.revision = revision;
            r.msg = msg;
            r.kind = kind;
            r.seen = seen;
            return r;
        }

        static void Expect(const Result& r, State state, std::wstring_view message = {}, std::wstring_view kind = {})
        {
            Log::Comment(WEX::Common::NoThrowString().Format(L"got %s msg='%s' kind='%s'", std::wstring{ Name(r.state) }.c_str(), r.message.c_str(), r.kind.c_str()));
            VERIFY_IS_TRUE(r.state == state);
            VERIFY_ARE_EQUAL(std::wstring{ message }, r.message);
            VERIFY_ARE_EQUAL(std::wstring{ kind }, r.kind);
        }

        TEST_METHOD(PlainShell)
        {
            std::vector<PaneInput> panes(1);
            Expect(Derive(panes, false), State::Idle);
            panes[0].commandRunning = true;
            Expect(Derive(panes, false), State::Running);
            panes[0].commandRunning = false;
            Log::Comment(L"The bell is a plain-shell attention signal");
            Expect(Derive(panes, true), State::DoneUnseen, L"bell");
            Log::Comment(L"...but a running command outranks it");
            panes[0].commandRunning = true;
            Expect(Derive(panes, true), State::Running);
        }

        TEST_METHOD(RecordsAreAuthoritativeForTheirPane)
        {
            std::vector<PaneInput> panes(1);
            panes[0].commandRunning = true; // the shell is "running" claude
            panes[0].records.push_back(Rec(RecordState::Idle));
            Expect(Derive(panes, false), State::Idle);
            panes[0].records[0] = Rec(RecordState::Working, true, 2, L"thinking");
            Expect(Derive(panes, false), State::Working, L"thinking");
        }

        TEST_METHOD(BlockedCarriesKindAndMessage)
        {
            std::vector<PaneInput> panes(1);
            panes[0].records.push_back(Rec(RecordState::Blocked, true, 3, L"Allow Bash?", RecordKind::Permission));
            Expect(Derive(panes, false), State::NeedsYou, L"Allow Bash?", L"permission");
        }

        TEST_METHOD(SeenDoneAndErrorAreIdle)
        {
            std::vector<PaneInput> panes(1);
            panes[0].records.push_back(Rec(RecordState::Done, true, 4, L"finished"));
            Expect(Derive(panes, false), State::DoneUnseen, L"finished");
            panes[0].records[0].seen = true;
            Expect(Derive(panes, false), State::Idle);
            panes[0].records[0] = Rec(RecordState::Error, true, 5, L"boom");
            Expect(Derive(panes, false), State::Error, L"boom");
            panes[0].records[0].seen = true;
            Expect(Derive(panes, false), State::Idle);
        }

        TEST_METHOD(RankAcrossRecordsAndPanes)
        {
            std::vector<PaneInput> panes(2);
            panes[0].records.push_back(Rec(RecordState::Working, true, 1, L"w"));
            panes[0].records.push_back(Rec(RecordState::Done, false, 2, L"d"));
            panes[1].commandRunning = true;
            Log::Comment(L"done-unseen > working > running");
            Expect(Derive(panes, false), State::DoneUnseen, L"d");
            panes[1].records.push_back(Rec(RecordState::Error, true, 1, L"e"));
            Log::Comment(L"error > done-unseen");
            Expect(Derive(panes, false), State::Error, L"e");
            panes[0].records.push_back(Rec(RecordState::Blocked, false, 3, L"q", RecordKind::Question));
            Log::Comment(L"needs-you > error");
            Expect(Derive(panes, false), State::NeedsYou, L"q", L"question");
        }

        TEST_METHOD(TieBreakPrefersRootThenNewest)
        {
            std::vector<PaneInput> panes(1);
            panes[0].records.push_back(Rec(RecordState::Working, false, 9, L"child"));
            panes[0].records.push_back(Rec(RecordState::Working, true, 1, L"root"));
            Expect(Derive(panes, false), State::Working, L"root");
            panes[0].records[1].state = RecordState::Idle;
            panes[0].records.push_back(Rec(RecordState::Working, false, 12, L"newer child"));
            Expect(Derive(panes, false), State::Working, L"newer child");
        }

        TEST_METHOD(TitleIsTheFallbackMessage)
        {
            std::vector<PaneInput> panes(1);
            auto r = Rec(RecordState::Working);
            r.title = L"Deploy";
            panes[0].records.push_back(r);
            Expect(Derive(panes, false), State::Working, L"Deploy");
        }

        TEST_METHOD(NoPanesIsIdle)
        {
            std::vector<PaneInput> panes;
            Expect(Derive(panes, false), State::Idle);
            Expect(Derive(panes, true), State::DoneUnseen, L"bell");
        }
    };
}
