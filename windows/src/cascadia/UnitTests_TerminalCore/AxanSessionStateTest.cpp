// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

// axan #30: the sidebar session-state derivation (TerminalApp/AxanSessionState.h) is a pure
// function, so it is tested here rather than through the XAML app.

#include "pch.h"
#include <WexTestClass.h>

#include "../TerminalApp/AxanSessionState.h"
#include <AxanStateGlyphs.h> // axan: sidebar.stateGlyphs resolution
#include <AxanSidebarSizes.h> // axan: sidebar row sizes

#include <limits>
#include <map>
#include <optional>

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

            Log::Comment(L"Cleared (no records): nothing retained, no slot glyph");
            panes[0].records.clear();
            r = Derive(panes, false, keep);
            Expect(r, State::Idle);
            VERIFY_IS_FALSE(r.retained.present);
            VERIFY_IS_TRUE(r.slot == Slot::None);
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

        // ----- glyph slot state ----------------------------------------------------------

        TEST_METHOD(SlotIdleOnlyWithAProgramPresent)
        {
            std::vector<PaneInput> panes(2);
            VERIFY_IS_TRUE(Derive(panes, false).slot == Slot::None);
            Log::Comment(L"A plain running shell: running");
            panes[1].commandRunning = true;
            VERIFY_IS_TRUE(Derive(panes, false).slot == Slot::Running);

            Log::Comment(L"Claude Code at its prompt (idle record): idle");
            panes[1].commandRunning = false;
            panes[0].records.push_back(Rec(RecordState::Idle));
            auto r = Derive(panes, false);
            Expect(r, State::Idle);
            VERIFY_IS_TRUE(r.slot == Slot::Idle);

            Log::Comment(L"Done and seen: idle (clear and keep alike)");
            panes[0].records[0] = Rec(RecordState::Done, true, 2, L"ok", RecordKind::None, true);
            VERIFY_IS_TRUE(Derive(panes, false).slot == Slot::Idle);
            VERIFY_IS_TRUE(Derive(panes, false, Options{ .keepAfterSeen = true }).slot == Slot::Idle);
        }

        TEST_METHOD(SlotFollowsTheNodeState)
        {
            // The slot shows the node's derived state: working and running decide it over an
            // idle record elsewhere, and attention states map to their own keys.
            std::vector<PaneInput> panes(2);
            panes[0].records.push_back(Rec(RecordState::Idle));
            panes[1].commandRunning = true;
            auto r = Derive(panes, false);
            Expect(r, State::Running);
            VERIFY_IS_TRUE(r.slot == Slot::Running);

            panes[0].records[0] = Rec(RecordState::Working, true, 2, L"thinking");
            r = Derive(panes, false);
            Expect(r, State::Working, L"thinking");
            VERIFY_IS_TRUE(r.slot == Slot::Working);

            panes[0].records[0] = Rec(RecordState::Blocked, true, 3, L"Allow?", RecordKind::Permission);
            r = Derive(panes, false);
            Expect(r, State::NeedsYou, L"Allow?", L"permission");
            VERIFY_IS_TRUE(r.slot == Slot::Blocked);

            panes[0].records[0] = Rec(RecordState::Done, true, 4, L"ok");
            VERIFY_IS_TRUE(Derive(panes, false).slot == Slot::Done);
            panes[0].records[0] = Rec(RecordState::Error, true, 5, L"boom");
            VERIFY_IS_TRUE(Derive(panes, false).slot == Slot::Error);

            VERIFY_ARE_EQUAL(std::wstring{ L"blocked" }, std::wstring{ SlotKey(Slot::Blocked) });
            VERIFY_ARE_EQUAL(std::wstring{}, std::wstring{ SlotKey(Slot::None) });
        }

        // ---- axan: sidebar.stateGlyphs (src/inc/AxanStateGlyphs.h) ----

        static std::wstring GlyphFrom(std::wstring_view key, const std::map<std::wstring, std::wstring>& configured)
        {
            return Axan::StateGlyphs::GlyphFor(key, [&](std::wstring_view k) -> std::optional<std::wstring> {
                if (const auto it = configured.find(std::wstring{ k }); it != configured.end())
                {
                    return it->second;
                }
                return std::nullopt;
            });
        }

        TEST_METHOD(StateGlyphsDefaultWhenUnset)
        {
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE99A }), GlyphFrom(L"idle", {}));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE945 }), GlyphFrom(L"working", {}));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE768 }), GlyphFrom(L"running", {}));
            Log::Comment(L"blocked / done / error default to nothing (the attention dot carries them)");
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(L"blocked", {}));
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(L"done", {}));
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(L"error", {}));
            Log::Comment(L"No slot state (a plain shell at rest) shows nothing");
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(SlotKey(Slot::None), {}));
        }

        TEST_METHOD(StateGlyphsEmptyShowsNothingPerKey)
        {
            const std::map<std::wstring, std::wstring> cfg{ { L"idle", L"" }, { L"running", L"   " } };
            Log::Comment(L"Only the configured keys change; whitespace-only counts as empty");
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(L"idle", cfg));
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(L"running", cfg));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE945 }), GlyphFrom(L"working", cfg));
        }

        TEST_METHOD(StateGlyphsUseTheNodeIconVocabulary)
        {
            const std::map<std::wstring, std::wstring> cfg{
                { L"working", L"builtin:build" },
                { L"blocked", L"\U0001F64B" },
                { L"done", L" C:\\icons\\done.png " },
                { L"error", L"builtin:nope" },
                { L"idle", std::wstring(1, wchar_t{ 0xE768 }) },
            };
            Log::Comment(L"builtin:NAME maps to the registry glyph (build = wrench U+E90F)");
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE90F }), GlyphFrom(L"working", cfg));
            Log::Comment(L"An emoji (a surrogate pair) passes through");
            VERIFY_ARE_EQUAL(std::wstring{ L"\U0001F64B" }, GlyphFrom(L"blocked", cfg));
            Log::Comment(L"A path passes through, trimmed");
            VERIFY_ARE_EQUAL(std::wstring{ L"C:\\icons\\done.png" }, GlyphFrom(L"done", cfg));
            Log::Comment(L"An unknown builtin: name stays verbatim; a literal glyph passes through");
            VERIFY_ARE_EQUAL(std::wstring{ L"builtin:nope" }, GlyphFrom(L"error", cfg));
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE768 }), GlyphFrom(L"idle", cfg));
        }

        TEST_METHOD(StateGlyphsIgnoreUnknownKeys)
        {
            const std::map<std::wstring, std::wstring> cfg{ { L"Idle", L"" }, { L"presence", L"builtin:git" } };
            Log::Comment(L"Keys are exact (case-sensitive) and unknown ones don't leak into any state");
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE99A }), GlyphFrom(L"idle", cfg));
            VERIFY_ARE_EQUAL(std::wstring{}, GlyphFrom(L"presence", cfg));
            VERIFY_IS_FALSE(Axan::StateGlyphs::IsKey(L"presence"));
            Log::Comment(L"Every slot key is a stateGlyphs key");
            for (const auto slot : { Slot::Idle, Slot::Working, Slot::Blocked, Slot::Done, Slot::Error, Slot::Running })
            {
                VERIFY_IS_TRUE(Axan::StateGlyphs::IsKey(SlotKey(slot)));
            }
        }

        TEST_METHOD(StateGlyphsKindKeysOverrideBlocked)
        {
            const auto glyph = [](std::wstring_view kind, const std::map<std::wstring, std::wstring>& cfg) {
                return Axan::StateGlyphs::GlyphFor(L"blocked", kind, [&](std::wstring_view k) -> std::optional<std::wstring> {
                    if (const auto it = cfg.find(std::wstring{ k }); it != cfg.end())
                    {
                        return it->second;
                    }
                    return std::nullopt;
                });
            };
            const std::map<std::wstring, std::wstring> cfg{ { L"blocked", L"B" }, { L"question", L"Q" }, { L"auth", L"" } };
            VERIFY_ARE_EQUAL(std::wstring{ L"Q" }, glyph(L"question", cfg));
            Log::Comment(L"An empty kind value means nothing for that kind, not the blocked glyph");
            VERIFY_ARE_EQUAL(std::wstring{}, glyph(L"auth", cfg));
            Log::Comment(L"An unset kind falls back to blocked; no kind is blocked");
            VERIFY_ARE_EQUAL(std::wstring{ L"B" }, glyph(L"permission", cfg));
            VERIFY_ARE_EQUAL(std::wstring{ L"B" }, glyph(L"", cfg));
            Log::Comment(L"Nothing configured: blocked's default, i.e. nothing");
            VERIFY_ARE_EQUAL(std::wstring{}, glyph(L"question", {}));
            Log::Comment(L"A kind key never applies to another state");
            const std::map<std::wstring, std::wstring> onlyQ{ { L"question", L"Q" } };
            VERIFY_ARE_EQUAL(std::wstring(1, wchar_t{ 0xE945 }), Axan::StateGlyphs::GlyphFor(L"working", L"question", [&](std::wstring_view k) -> std::optional<std::wstring> {
                                 if (const auto it = onlyQ.find(std::wstring{ k }); it != onlyQ.end())
                                 {
                                     return it->second;
                                 }
                                 return std::nullopt;
                             }));
        }

        TEST_METHOD(StateWordTokens)
        {
            using Axan::StateGlyphs::WordToken;
            VERIFY_ARE_EQUAL(std::wstring{ L"idle" }, std::wstring{ WordToken(L"idle", L"") });
            VERIFY_ARE_EQUAL(std::wstring{ L"working" }, std::wstring{ WordToken(L"working", L"") });
            VERIFY_ARE_EQUAL(std::wstring{ L"running" }, std::wstring{ WordToken(L"running", L"") });
            VERIFY_ARE_EQUAL(std::wstring{ L"done" }, std::wstring{ WordToken(L"done", L"") });
            VERIFY_ARE_EQUAL(std::wstring{ L"error" }, std::wstring{ WordToken(L"error", L"") });
            VERIFY_ARE_EQUAL(std::wstring{ L"needs-permission" }, std::wstring{ WordToken(L"blocked", L"permission") });
            VERIFY_ARE_EQUAL(std::wstring{ L"needs-answer" }, std::wstring{ WordToken(L"blocked", L"question") });
            VERIFY_ARE_EQUAL(std::wstring{ L"needs-login" }, std::wstring{ WordToken(L"blocked", L"auth") });
            VERIFY_ARE_EQUAL(std::wstring{ L"needs-you" }, std::wstring{ WordToken(L"blocked", L"") });
            Log::Comment(L"No slot state, no word");
            VERIFY_ARE_EQUAL(std::wstring{}, std::wstring{ WordToken(L"", L"") });

            Log::Comment(L"End to end: a seen done in keep mode reads idle (the kept line says done)");
            std::vector<PaneInput> panes(1);
            panes[0].records.push_back(Rec(RecordState::Done, true, 2, L"ok", RecordKind::None, true));
            const auto r = Derive(panes, false, Options{ .keepAfterSeen = true });
            VERIFY_ARE_EQUAL(std::wstring{ L"idle" }, std::wstring{ WordToken(SlotKey(r.slot), r.kind) });
            panes[0].records[0] = Rec(RecordState::Blocked, true, 3, L"Tea or coffee?", RecordKind::Question);
            const auto q = Derive(panes, false);
            VERIFY_ARE_EQUAL(std::wstring{ L"needs-answer" }, std::wstring{ WordToken(SlotKey(q.slot), q.kind) });
        }

        TEST_METHOD(AgentNameWheneverRecordsExist)
        {
            std::vector<PaneInput> panes(2);
            panes[0].terminalTitle = L"pwsh";
            panes[1].terminalTitle = L"\x2733 exile-survival";
            Log::Comment(L"No records: a plain shell has no agent name");
            VERIFY_ARE_EQUAL(std::wstring{}, Derive(panes, false).agentName);

            Log::Comment(L"Idle record with no title: the agent pane's terminal title, spinner stripped");
            panes[1].records.push_back(Rec(RecordState::Idle));
            VERIFY_ARE_EQUAL(std::wstring{ L"exile-survival" }, Derive(panes, false).agentName);

            Log::Comment(L"A root record's title wins over the terminal title");
            panes[1].records[0].title = L"named-by-record";
            VERIFY_ARE_EQUAL(std::wstring{ L"named-by-record" }, Derive(panes, false).agentName);

            Log::Comment(L"Working: still derived (the row shows it when there is no message)");
            panes[1].records[0].state = RecordState::Working;
            VERIFY_ARE_EQUAL(std::wstring{ L"named-by-record" }, Derive(panes, false).agentName);

            Log::Comment(L"Blocked: derived too; the row lets the attention line win");
            panes[1].records[0].state = RecordState::Blocked;
            VERIFY_ARE_EQUAL(std::wstring{ L"named-by-record" }, Derive(panes, false).agentName);

            Log::Comment(L"Running beside an agent pane: derived");
            panes[1].records[0].state = RecordState::Idle;
            panes[1].records[0].title = {};
            panes[0].commandRunning = true;
            const auto running = Derive(panes, false);
            VERIFY_IS_TRUE(running.state == State::Running);
            VERIFY_ARE_EQUAL(std::wstring{ L"exile-survival" }, running.agentName);
            panes[0].commandRunning = false;

            Log::Comment(L"Seen done (keep): still at rest, so the name is derived (the row prefers the kept line)");
            panes[1].records[0] = Rec(RecordState::Done, true, 4, L"ok", RecordKind::None, true);
            VERIFY_ARE_EQUAL(std::wstring{ L"exile-survival" }, Derive(panes, false, Options{ .keepAfterSeen = true }).agentName);
        }

        TEST_METHOD(CleanTerminalTitleStripsLeadingSymbols)
        {
            VERIFY_ARE_EQUAL(std::wstring{ L"exile-survival" }, CleanTerminalTitle(L"\x2733 exile-survival"));
            VERIFY_ARE_EQUAL(std::wstring{ L"build bot" }, CleanTerminalTitle(L"\x2802  build bot  "));
            VERIFY_ARE_EQUAL(std::wstring{ L"Claude Code" }, CleanTerminalTitle(L"Claude Code"));
            VERIFY_ARE_EQUAL(std::wstring{ L"~/src" }, CleanTerminalTitle(L"~/src"));
            VERIFY_ARE_EQUAL(std::wstring{}, CleanTerminalTitle(L"\x2733 "));
        }

        TEST_METHOD(PreviewBaselineIsPerPane)
        {
            Log::Comment(L"Nothing new: the preview stays");
            const RevisionsByPane baseline{ { 1, 5 }, { 2, 9 } };
            VERIFY_IS_FALSE(HasNewReport(baseline, { { 1, 5 }, { 2, 9 } }));
            Log::Comment(L"A pane closed: its revisions leave, which must not mask a later report");
            VERIFY_IS_FALSE(HasNewReport(baseline, { { 1, 5 } }));
            VERIFY_IS_TRUE(HasNewReport(baseline, { { 1, 6 } })); // a sum (6 < 14) would have missed it
            Log::Comment(L"A new report in any pane clears it");
            VERIFY_IS_TRUE(HasNewReport(baseline, { { 1, 5 }, { 2, 10 } }));
            Log::Comment(L"A new pane counts from 0: quiet it changes nothing, reporting it clears");
            VERIFY_IS_FALSE(HasNewReport(baseline, { { 1, 5 }, { 2, 9 }, { 3, 0 } }));
            VERIFY_IS_TRUE(HasNewReport(baseline, { { 1, 5 }, { 2, 9 }, { 3, 1 } }));
            VERIFY_IS_TRUE(HasNewReport({}, { { 3, 1 } }));
        }

        TEST_METHOD(RowSizesTreatNaNAsTheDefault)
        {
            namespace Z = Axan::SidebarSizes;
            const auto nan = std::numeric_limits<double>::quiet_NaN();
            Log::Comment(L"NaN (a cleared NumberBox, JSON null) reads as the setting's default");
            VERIFY_ARE_EQUAL(Z::SanitizeSize(nan, Z::kLabelFontSize), 14.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(nan, Z::kStateLineFontSize), 11.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(nan, Z::kIconSize), 16.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(nan, Z::kStateGlyphSize), 12.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(nan, Z::kIdleLabelFontSize), 12.0); // axan #47
            VERIFY_ARE_EQUAL(Z::SanitizeSize(nan, Z::kIdleRowHeight), 24.0);
            Log::Comment(L"Anything else is clamped to the page's range");
            VERIFY_ARE_EQUAL(Z::SanitizeSize(4.0, Z::kIdleLabelFontSize), 8.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(8.0, Z::kIdleRowHeight), 16.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(100.0, Z::kIdleRowHeight), 64.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(20.0, Z::kIdleRowHeight), 20.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(2.0, Z::kLabelFontSize), 8.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(100.0, Z::kLabelFontSize), 32.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(100.0, Z::kIconSize), 48.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(20.0, Z::kStateGlyphSize), 20.0);
            VERIFY_ARE_EQUAL(Z::SanitizeSize(std::numeric_limits<double>::infinity(), Z::kIconSize), 48.0);
        }
    };
}
