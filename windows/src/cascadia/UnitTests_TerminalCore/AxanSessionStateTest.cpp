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

        // ----- sidebar.statusAfterSeen (keep / clear) --------------------------------------

        static void ExpectRetained(const Result& r, RecordState state, std::wstring_view message)
        {
            Log::Comment(WEX::Common::NoThrowString().Format(L"retained=%d msg='%s' key='%s'", r.retained.present, r.retained.message.c_str(), r.reportKey.c_str()));
            VERIFY_IS_TRUE(r.retained.present);
            VERIFY_IS_TRUE(r.retained.state == state);
            VERIFY_ARE_EQUAL(std::wstring{ message }, r.retained.message);
        }

        TEST_METHOD(ClearDropsASeenReport)
        {
            std::vector<PaneInput> panes(1);
            panes[0].records.push_back(Rec(RecordState::Done, true, 4, L"finished", RecordKind::None, true));
            const auto r = Derive(panes, false, Options{ .keepAfterSeen = false });
            Expect(r, State::Idle);
            VERIFY_IS_FALSE(r.retained.present);
            VERIFY_ARE_EQUAL(std::wstring{}, r.reportKey);
            Log::Comment(L"The default options are clear (the pre-setting behaviour)");
            VERIFY_IS_FALSE(Derive(panes, false).retained.present);
        }

        TEST_METHOD(KeepRetainsASeenReportButStaysIdle)
        {
            std::vector<PaneInput> panes(1);
            panes[0].paneId = 7;
            panes[0].records.push_back(Rec(RecordState::Done, true, 4, L"finished"));
            const Options keep{ .keepAfterSeen = true };

            Log::Comment(L"Unseen: done-unseen as before, nothing retained, the key names the report");
            const auto unseen = Derive(panes, false, keep);
            Expect(unseen, State::DoneUnseen, L"finished");
            VERIFY_IS_FALSE(unseen.retained.present);
            VERIFY_ARE_EQUAL(std::wstring{ L"7::2" }, unseen.reportKey);

            Log::Comment(L"Seen: the state is idle (no attention), the report is retained under the same key");
            panes[0].records[0].seen = true;
            const auto seen = Derive(panes, false, keep);
            Expect(seen, State::Idle);
            ExpectRetained(seen, RecordState::Done, L"finished");
            VERIFY_ARE_EQUAL(unseen.reportKey, seen.reportKey);

            Log::Comment(L"A seen error is retained as an error; its title stands in for an empty msg");
            panes[0].records[0] = Rec(RecordState::Error, true, 5, {}, RecordKind::None, true);
            panes[0].records[0].title = L"Build";
            const auto err = Derive(panes, false, keep);
            Expect(err, State::Idle);
            ExpectRetained(err, RecordState::Error, L"Build");
            VERIFY_ARE_EQUAL(std::wstring{ L"7::4" }, err.reportKey);
        }

        TEST_METHOD(KeepIsReplacedByANewReportAndGoneWhenCleared)
        {
            std::vector<PaneInput> panes(1);
            panes[0].records.push_back(Rec(RecordState::Done, true, 4, L"finished", RecordKind::None, true));
            const Options keep{ .keepAfterSeen = true };
            ExpectRetained(Derive(panes, false, keep), RecordState::Done, L"finished");

            Log::Comment(L"A new report (working) replaces the record: shown live, nothing retained");
            panes[0].records[0] = Rec(RecordState::Working, true, 5, L"thinking");
            auto r = Derive(panes, false, keep);
            Expect(r, State::Working, L"thinking");
            VERIFY_IS_FALSE(r.retained.present);
            VERIFY_ARE_EQUAL(std::wstring{}, r.reportKey);

            Log::Comment(L"An idle report replaces it too");
            panes[0].records[0] = Rec(RecordState::Idle, true, 6);
            VERIFY_IS_FALSE(Derive(panes, false, keep).retained.present);

            Log::Comment(L"Cleared (no records): nothing retained, no presence");
            panes[0].records.clear();
            r = Derive(panes, false, keep);
            Expect(r, State::Idle);
            VERIFY_IS_FALSE(r.retained.present);
            VERIFY_IS_FALSE(r.agentPresent);
        }

        TEST_METHOD(KeepOnlyShowsWhileTheNodeRests)
        {
            std::vector<PaneInput> panes(2);
            panes[0].records.push_back(Rec(RecordState::Done, true, 4, L"finished", RecordKind::None, true));
            const Options keep{ .keepAfterSeen = true };

            Log::Comment(L"Another pane running a command: the node is running, the report is still kept");
            panes[1].commandRunning = true;
            auto r = Derive(panes, false, keep);
            Expect(r, State::Running);
            ExpectRetained(r, RecordState::Done, L"finished");

            Log::Comment(L"Another pane needing you: that wins and nothing is retained");
            panes[1].records.push_back(Rec(RecordState::Blocked, true, 2, L"Allow?", RecordKind::Permission));
            r = Derive(panes, false, keep);
            Expect(r, State::NeedsYou, L"Allow?", L"permission");
            VERIFY_IS_FALSE(r.retained.present);

            Log::Comment(L"A bell on an otherwise idle node is attention too");
            panes[1].records.clear();
            panes[1].commandRunning = false;
            r = Derive(panes, true, keep);
            Expect(r, State::DoneUnseen, L"bell");
            VERIFY_IS_FALSE(r.retained.present);
        }

        TEST_METHOD(KeepPrefersErrorThenRootThenNewest)
        {
            std::vector<PaneInput> panes(1);
            const Options keep{ .keepAfterSeen = true };
            panes[0].records.push_back(Rec(RecordState::Done, false, 9, L"child done", RecordKind::None, true));
            panes[0].records.push_back(Rec(RecordState::Done, true, 3, L"root done", RecordKind::None, true));
            ExpectRetained(Derive(panes, false, keep), RecordState::Done, L"root done");
            panes[0].records.push_back(Rec(RecordState::Error, false, 2, L"child error", RecordKind::None, true));
            ExpectRetained(Derive(panes, false, keep), RecordState::Error, L"child error");
        }

        TEST_METHOD(ReportKeyIsStableAcrossMessageRewrites)
        {
            // Claude Code reports done, then done again with the msg ~1s later: same key, so
            // the row's age doesn't restart.
            std::vector<PaneInput> panes(1);
            panes[0].paneId = 3;
            panes[0].records.push_back(Rec(RecordState::Done, true, 10));
            const auto first = Derive(panes, false, Options{ .keepAfterSeen = true });
            panes[0].records[0] = Rec(RecordState::Done, true, 11, L"replied");
            const auto second = Derive(panes, false, Options{ .keepAfterSeen = true });
            VERIFY_ARE_EQUAL(first.reportKey, second.reportKey);
            Log::Comment(L"A different pane is a different report");
            panes[0].paneId = 4;
            VERIFY_ARE_NOT_EQUAL(first.reportKey, Derive(panes, false).reportKey);
        }

        // ----- agent presence -----------------------------------------------------------

        TEST_METHOD(PresenceWithIdleAndSeenRecords)
        {
            std::vector<PaneInput> panes(2);
            VERIFY_IS_FALSE(Derive(panes, false).agentPresent);
            Log::Comment(L"A plain running shell is not an agent");
            panes[1].commandRunning = true;
            VERIFY_IS_FALSE(Derive(panes, false).agentPresent);

            Log::Comment(L"Claude Code at its prompt (idle record): present, idle");
            panes[1].commandRunning = false;
            panes[0].records.push_back(Rec(RecordState::Idle));
            auto r = Derive(panes, false);
            Expect(r, State::Idle);
            VERIFY_IS_TRUE(r.agentPresent);

            Log::Comment(L"Done and seen: present, idle (clear and keep alike)");
            panes[0].records[0] = Rec(RecordState::Done, true, 2, L"ok", RecordKind::None, true);
            VERIFY_IS_TRUE(Derive(panes, false).agentPresent);
            VERIFY_IS_TRUE(Derive(panes, false, Options{ .keepAfterSeen = true }).agentPresent);
        }

        TEST_METHOD(PresenceDoesNotChangeStatePrecedence)
        {
            // Presence is a flag beside the state, never a state: working and running still
            // decide the node (and so own the glyph slot), and attention states still win.
            std::vector<PaneInput> panes(2);
            panes[0].records.push_back(Rec(RecordState::Idle));
            panes[1].commandRunning = true;
            auto r = Derive(panes, false);
            Expect(r, State::Running);
            VERIFY_IS_TRUE(r.agentPresent);

            panes[0].records[0] = Rec(RecordState::Working, true, 2, L"thinking");
            r = Derive(panes, false);
            Expect(r, State::Working, L"thinking");
            VERIFY_IS_TRUE(r.agentPresent);

            panes[0].records[0] = Rec(RecordState::Blocked, true, 3, L"Allow?", RecordKind::Permission);
            r = Derive(panes, false);
            Expect(r, State::NeedsYou, L"Allow?", L"permission");
            VERIFY_IS_TRUE(r.agentPresent);
        }
    };
}
