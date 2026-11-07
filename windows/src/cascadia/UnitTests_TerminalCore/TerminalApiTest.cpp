// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include <WexTestClass.h>

#include "../cascadia/TerminalCore/Terminal.hpp"
#include "MockTermSettings.h"
#include "../renderer/inc/DummyRenderer.hpp"
#include "consoletaeftemplates.hpp"

using namespace winrt::Microsoft::Terminal::Core;
using namespace Microsoft::Terminal::Core;

using namespace WEX::Logging;
using namespace WEX::TestExecution;

namespace TerminalCoreUnitTests
{
#define WCS(x) WCSHELPER(x)
#define WCSHELPER(x) L## #x

    class TerminalApiTest
    {
        TEST_CLASS(TerminalApiTest);

        TEST_METHOD(SetColorTableEntry);

        TEST_METHOD(CursorVisibility);
        TEST_METHOD(CursorVisibilityViaStateMachine);

        // Terminal::_WriteBuffer used to enter infinite loops under certain conditions.
        // This test ensures that Terminal::_WriteBuffer doesn't get stuck when
        // PrintString() is called with more code units than the buffer width.
        TEST_METHOD(PrintStringOfSurrogatePairs);
        TEST_METHOD(CheckDoubleWidthCursor);

        TEST_METHOD(AddHyperlink);
        TEST_METHOD(AddHyperlinkCustomId);
        TEST_METHOD(AddHyperlinkCustomIdDifferentUri);

        TEST_METHOD(SetTaskbarProgress);
        TEST_METHOD(SetWorkingDirectory);

        // axan #29: OSC 7501
        TEST_METHOD(ProgramStatusQueryReply);
        TEST_METHOD(ProgramStatusRecords);
        TEST_METHOD(ProgramStatusRecordCap);

        // axan #30: OSC 133 command lifecycle
        TEST_METHOD(ShellCommandRunning);
    };
};

using namespace TerminalCoreUnitTests;

void TerminalApiTest::SetColorTableEntry()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto settings = winrt::make<MockTermSettings>(100, 100, 100);
    term.UpdateSettings(settings);

    VERIFY_NO_THROW(term._renderSettings.SetColorTableEntry(0, 100));
    VERIFY_NO_THROW(term._renderSettings.SetColorTableEntry(128, 100));
    VERIFY_NO_THROW(term._renderSettings.SetColorTableEntry(255, 100));

    VERIFY_THROWS(term._renderSettings.SetColorTableEntry(512, 100), std::exception);
}

// Terminal::_WriteBuffer used to enter infinite loops under certain conditions.
// This test ensures that Terminal::_WriteBuffer doesn't get stuck when
// PrintString() is called with more code units than the buffer width.
void TerminalApiTest::PrintStringOfSurrogatePairs()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 3, renderer);

    std::wstring text;
    text.reserve(600);

    for (size_t i = 0; i < 100; ++i)
    {
        text.append(L"𐐌𐐜𐐬");
    }

    struct Baton
    {
        HANDLE done;
        std::wstring text;
        Terminal* pTerm;
    } baton{
        CreateEventW(nullptr, TRUE, FALSE, L"done signal"),
        text,
        &term,
    };

    Log::Comment(L"Launching thread to write data.");
    const auto thread = CreateThread(
        nullptr,
        0,
        [](LPVOID data) -> DWORD {
            const auto& baton = *reinterpret_cast<Baton*>(data);
            Log::Comment(L"Writing data.");
            baton.pTerm->_stateMachine->ProcessString(baton.text);
            Log::Comment(L"Setting event.");
            SetEvent(baton.done);
            return 0;
        },
        (LPVOID)&baton,
        0,
        nullptr);

    Log::Comment(L"Waiting for the write.");
    switch (WaitForSingleObject(baton.done, 2000))
    {
    case WAIT_OBJECT_0:
        Log::Comment(L"Didn't get stuck. Success.");
        break;
    case WAIT_TIMEOUT:
        Log::Comment(L"Wait timed out. It got stuck.");
        Log::Result(WEX::Logging::TestResults::Failed);
        break;
    case WAIT_FAILED:
        Log::Comment(L"Wait failed for some reason. We didn't expect this.");
        Log::Result(WEX::Logging::TestResults::Failed);
        break;
    default:
        Log::Comment(L"Wait return code that no one expected. Fail.");
        Log::Result(WEX::Logging::TestResults::Failed);
        break;
    }

    TerminateThread(thread, 0);
    CloseHandle(baton.done);
    return;
}

void TerminalApiTest::CursorVisibility()
{
    // GH#3093 - Cursor Visibility and On states shouldn't affect each other
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsVisible());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsOn());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsBlinkingAllowed());

    term.SetCursorOn(false);
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsVisible());
    VERIFY_IS_FALSE(term._mainBuffer->GetCursor().IsOn());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsBlinkingAllowed());

    term.SetCursorOn(true);
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsVisible());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsOn());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsBlinkingAllowed());

    auto& textBuffer = term.GetBufferAndViewport().buffer;
    textBuffer.GetCursor().SetIsVisible(false);
    VERIFY_IS_FALSE(term._mainBuffer->GetCursor().IsVisible());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsOn());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsBlinkingAllowed());

    term.SetCursorOn(false);
    VERIFY_IS_FALSE(term._mainBuffer->GetCursor().IsVisible());
    VERIFY_IS_FALSE(term._mainBuffer->GetCursor().IsOn());
    VERIFY_IS_TRUE(term._mainBuffer->GetCursor().IsBlinkingAllowed());
}

void TerminalApiTest::CursorVisibilityViaStateMachine()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestCursorIsOn, adapted for the Terminal
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);
    auto& cursor = tbi.GetCursor();

    stateMachine.ProcessString(L"Hello World");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_TRUE(cursor.IsBlinkingAllowed());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12l");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_FALSE(cursor.IsBlinkingAllowed());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12h");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_TRUE(cursor.IsBlinkingAllowed());
    VERIFY_IS_TRUE(cursor.IsVisible());

    cursor.SetIsOn(false);
    stateMachine.ProcessString(L"\x1b[?12l");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_FALSE(cursor.IsBlinkingAllowed());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12h");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_TRUE(cursor.IsBlinkingAllowed());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?25l");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_TRUE(cursor.IsBlinkingAllowed());
    VERIFY_IS_FALSE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?25h");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_TRUE(cursor.IsBlinkingAllowed());
    VERIFY_IS_TRUE(cursor.IsVisible());

    stateMachine.ProcessString(L"\x1b[?12;25l");
    VERIFY_IS_TRUE(cursor.IsOn());
    VERIFY_IS_FALSE(cursor.IsBlinkingAllowed());
    VERIFY_IS_FALSE(cursor.IsVisible());
}

void TerminalApiTest::CheckDoubleWidthCursor()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);
    auto& cursor = tbi.GetCursor();

    // Lets stuff the buffer with single width characters,
    // but leave the last two columns empty for double width.
    std::wstring singleWidthText;
    singleWidthText.reserve(98);
    for (size_t i = 0; i < 98; ++i)
    {
        singleWidthText.append(L"A");
    }
    stateMachine.ProcessString(singleWidthText);
    VERIFY_IS_TRUE(cursor.GetPosition().x == 98);

    // Stuff two double width characters.
    std::wstring doubleWidthText{ L"我愛" };
    stateMachine.ProcessString(doubleWidthText);

    // The last 'A'
    cursor.SetPosition({ 97, 0 });
    VERIFY_IS_FALSE(term.IsCursorDoubleWidth());

    // This and the next CursorPos are taken up by '我‘
    cursor.SetPosition({ 98, 0 });
    VERIFY_IS_TRUE(term.IsCursorDoubleWidth());
    cursor.SetPosition({ 99, 0 });
    VERIFY_IS_TRUE(term.IsCursorDoubleWidth());

    // This and the next CursorPos are taken up by ’愛‘
    cursor.SetPosition({ 0, 1 });
    VERIFY_IS_TRUE(term.IsCursorDoubleWidth());
    cursor.SetPosition({ 1, 1 });
    VERIFY_IS_TRUE(term.IsCursorDoubleWidth());
}

void TerminalCoreUnitTests::TerminalApiTest::AddHyperlink()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestAddHyperlink, adapted for the Terminal

    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);

    // Process the opening osc 8 sequence
    stateMachine.ProcessString(L"\x1b]8;;test.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");

    // Send any other text
    stateMachine.ProcessString(L"Hello World");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");

    // Process the closing osc 8 sequences
    stateMachine.ProcessString(L"\x1b]8;;\x1b\\");
    VERIFY_IS_FALSE(tbi.GetCurrentAttributes().IsHyperlink());
}

void TerminalCoreUnitTests::TerminalApiTest::AddHyperlinkCustomId()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestAddHyperlinkCustomId, adapted for the Terminal

    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);

    // Process the opening osc 8 sequence
    stateMachine.ProcessString(L"\x1b]8;id=myId;test.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"test.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    // Send any other text
    stateMachine.ProcessString(L"Hello World");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"test.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    // Process the closing osc 8 sequences
    stateMachine.ProcessString(L"\x1b]8;;\x1b\\");
    VERIFY_IS_FALSE(tbi.GetCurrentAttributes().IsHyperlink());
}

void TerminalCoreUnitTests::TerminalApiTest::AddHyperlinkCustomIdDifferentUri()
{
    // This is a nearly literal copy-paste of ScreenBufferTests::TestAddHyperlinkCustomId, adapted for the Terminal

    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& tbi = *(term._mainBuffer);
    auto& stateMachine = *(term._stateMachine);

    // Process the opening osc 8 sequence
    stateMachine.ProcessString(L"\x1b]8;id=myId;test.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"test.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"test.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    const auto oldAttributes{ tbi.GetCurrentAttributes() };

    // Send any other text
    stateMachine.ProcessString(L"\x1b]8;id=myId;other.url\x1b\\");
    VERIFY_IS_TRUE(tbi.GetCurrentAttributes().IsHyperlink());
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(tbi.GetCurrentAttributes().GetHyperlinkId()), L"other.url");
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkId(L"other.url", L"myId"), tbi.GetCurrentAttributes().GetHyperlinkId());

    // This second URL should not change the URL of the original ID!
    VERIFY_ARE_EQUAL(tbi.GetHyperlinkUriFromId(oldAttributes.GetHyperlinkId()), L"test.url");
    VERIFY_ARE_NOT_EQUAL(oldAttributes.GetHyperlinkId(), tbi.GetCurrentAttributes().GetHyperlinkId());
}

void TerminalCoreUnitTests::TerminalApiTest::SetTaskbarProgress()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& stateMachine = *(term._stateMachine);

    // Initial values for taskbar state and progress should be 0
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Set some values for taskbar state and progress through state machine
    stateMachine.ProcessString(L"\x1b]9;4;1;50\x1b\\");
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(1));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(50));

    // Reset to 0
    stateMachine.ProcessString(L"\x1b]9;4;0;0\x1b\\");
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Set an out of bounds value for state
    stateMachine.ProcessString(L"\x1b]9;4;5;50\x1b\\");
    // Nothing should have changed (dispatch should have returned false)
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Set an out of bounds value for progress
    stateMachine.ProcessString(L"\x1b]9;4;1;999\x1b\\");
    // Progress should have been clamped to 100
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(1));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(100));

    // Don't specify any params
    stateMachine.ProcessString(L"\x1b]9;4\x1b\\");
    // State and progress should both be reset to 0
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    // Specify additional params
    stateMachine.ProcessString(L"\x1b]9;4;1;80;123\x1b\\");
    // Additional params should be ignored, state and progress still set normally
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(1));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(80));

    // Edge cases + trailing semicolon testing
    stateMachine.ProcessString(L"\x1b]9;4;2;\x1b\\");
    // String should be processed correctly despite the trailing semicolon,
    // taskbar progress should remain unchanged from previous value
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(2));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(80));

    stateMachine.ProcessString(L"\x1b]9;4;3;75\x1b\\");
    // Given progress value should be ignored because this is the indeterminate state,
    // so the progress value should remain unchanged
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(3));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(80));

    stateMachine.ProcessString(L"\x1b]9;4;0;50\x1b\\");
    // Taskbar progress should be 0 (the given value should be ignored)
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(0));
    VERIFY_ARE_EQUAL(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));

    stateMachine.ProcessString(L"\x1b]9;4;2;\x1b\\");
    // String should be processed correctly despite the trailing semicolon,
    // taskbar progress should be set to a 'minimum', non-zero value
    VERIFY_ARE_EQUAL(term.GetTaskbarState(), gsl::narrow<size_t>(2));
    VERIFY_IS_GREATER_THAN(term.GetTaskbarProgress(), gsl::narrow<size_t>(0));
}

void TerminalCoreUnitTests::TerminalApiTest::SetWorkingDirectory()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);

    auto& stateMachine = *(term._stateMachine);

    // Test setting working directory using OSC 9;9
    // Initial CWD should be empty
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    // Invalid sequences should not change CWD
    stateMachine.ProcessString(L"\x1b]9;9\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9\"C:\\\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b"
                               LR"(]9;9;"C:\invalid path "with" quotes")"
                               L"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    // These OSC 9;9 sequences will result in invalid CWD. It should end up empty, like above.
    stateMachine.ProcessString(L"\x1b]9;9;\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;\"\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;\"\"\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;\"\"\"\"\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    stateMachine.ProcessString(L"\x1b]9;9;No quotes \"until\" later\x1b\\");
    VERIFY_IS_TRUE(term.GetWorkingDirectory().empty());

    // Valid sequences should change CWD
    stateMachine.ProcessString(L"\x1b]9;9;\"C:\\\"\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\");

    stateMachine.ProcessString(L"\x1b]9;9;\"C:\\Program Files\"\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\Program Files");

    stateMachine.ProcessString(L"\x1b]9;9;\"D:\\中文\"\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"D:\\中文");

    // Test OSC 9;9 sequences without quotation marks
    stateMachine.ProcessString(L"\x1b]9;9;C:\\\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\");

    stateMachine.ProcessString(L"\x1b]9;9;C:\\Program Files\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"C:\\Program Files");

    stateMachine.ProcessString(L"\x1b]9;9;D:\\中文\x1b\\");
    VERIFY_ARE_EQUAL(term.GetWorkingDirectory(), L"D:\\中文");
}

// axan #29: OSC 7501 Program Status ---------------------------------------------------

namespace
{
    using ::Microsoft::Console::VirtualTerminal::ProgramStatus::Kind;
    using ::Microsoft::Console::VirtualTerminal::ProgramStatus::State;

    // Find a record by id in a snapshot; nullptr if absent.
    const Terminal::ProgramStatusRecord* FindRecord(const std::vector<Terminal::ProgramStatusRecord>& records, std::wstring_view id)
    {
        for (const auto& r : records)
        {
            if (r.id == id)
            {
                return &r;
            }
        }
        return nullptr;
    }

    std::wstring Report(std::wstring_view payload)
    {
        return std::wstring{ L"\x1b]7501;" }.append(payload).append(L"\x1b\\");
    }
}

void TerminalCoreUnitTests::TerminalApiTest::ProgramStatusQueryReply()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);
    std::wstring responses;
    term.SetWriteInputCallback([&](std::wstring_view s) { responses.append(s); });
    auto& stateMachine = *(term._stateMachine);

    constexpr std::wstring_view reply{ L"\x1b]7501;?:states=idle,working,done,blocked,error:kinds=permission,question,auth\x1b\\" };

    Log::Comment(L"Enabled by default: the query is answered ahead of DA1");
    VERIFY_IS_TRUE(term.IsProgramStatusEnabled());
    stateMachine.ProcessString(L"\x1b]7501;?\x1b\\\x1b[c");
    VERIFY_IS_TRUE(responses.starts_with(reply));
    VERIFY_ARE_EQUAL(reply.size(), responses.find(L"\x1b[?61;"));

    Log::Comment(L"Gate off: no reply to the query, and reports are ignored");
    responses.clear();
    term.SetProgramStatusEnabled(false);
    stateMachine.ProcessString(L"\x1b]7501;?\x07\x1b[c");
    VERIFY_ARE_EQUAL(std::wstring::npos, responses.find(L"7501"));
    VERIFY_IS_TRUE(responses.starts_with(L"\x1b[?61;"));
    stateMachine.ProcessString(Report(L"state=working:app=claude"));
    VERIFY_ARE_EQUAL(0u, term.GetProgramStatusRecords().size());
}

void TerminalCoreUnitTests::TerminalApiTest::ProgramStatusRecords()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);
    size_t notifications = 0;
    term.SetProgramStatusChangedCallback([&]() { ++notifications; });
    auto& stateMachine = *(term._stateMachine);

    Log::Comment(L"A report creates a record; fields round-trip");
    stateMachine.ProcessString(Report(L"state=blocked:kind=auth:app=brew:progress=10:msg=UGFzc3dvcmQ="));
    auto records = term.GetProgramStatusRecords();
    VERIFY_ARE_EQUAL(1u, records.size());
    VERIFY_ARE_EQUAL(1u, notifications);
    auto root = FindRecord(records, L"");
    VERIFY_IS_NOT_NULL(root);
    VERIFY_ARE_EQUAL(State::Blocked, root->state);
    VERIFY_ARE_EQUAL(Kind::Auth, root->kind);
    VERIFY_ARE_EQUAL(L"brew", root->app);
    VERIFY_ARE_EQUAL(L"brew", root->effectiveApp);
    VERIFY_ARE_EQUAL(10, static_cast<int>(root->progress.value_or(255)));
    VERIFY_ARE_EQUAL(L"Password", root->msg);
    const auto firstRevision = root->revision;

    Log::Comment(L"A report fully replaces its record: missing keys clear their values");
    stateMachine.ProcessString(Report(L"state=working"));
    records = term.GetProgramStatusRecords();
    root = FindRecord(records, L"");
    VERIFY_ARE_EQUAL(State::Working, root->state);
    VERIFY_ARE_EQUAL(Kind::None, root->kind);
    VERIFY_ARE_EQUAL(L"", root->app);
    VERIFY_ARE_EQUAL(L"", root->msg);
    VERIFY_IS_FALSE(root->progress.has_value());
    VERIFY_IS_GREATER_THAN(root->revision, firstRevision);

    Log::Comment(L"Children inherit app from the nearest ancestor that has one");
    stateMachine.ProcessString(Report(L"state=working:app=deploy"));
    stateMachine.ProcessString(Report(L"state=working:id=us-east"));
    stateMachine.ProcessString(Report(L"state=working:id=us-east/web:app=nginx"));
    stateMachine.ProcessString(Report(L"state=done:id=us-east/web/a"));
    stateMachine.ProcessString(Report(L"state=done:id=eu/x/y"));
    stateMachine.ProcessString(Report(L"state=idle:id=us-east-2"));
    records = term.GetProgramStatusRecords();
    VERIFY_ARE_EQUAL(6u, records.size());
    VERIFY_ARE_EQUAL(L"", records[0].id); // sorted, root first
    VERIFY_ARE_EQUAL(L"deploy", FindRecord(records, L"us-east")->effectiveApp);
    VERIFY_ARE_EQUAL(L"", FindRecord(records, L"us-east")->app);
    VERIFY_ARE_EQUAL(L"nginx", FindRecord(records, L"us-east/web/a")->effectiveApp);
    VERIFY_ARE_EQUAL(L"deploy", FindRecord(records, L"eu/x/y")->effectiveApp); // no parent records needed

    Log::Comment(L"An invalid id never touches anything, even with state=clear");
    const auto before = notifications;
    stateMachine.ProcessString(Report(L"state=clear:id=bad!"));
    stateMachine.ProcessString(Report(L"state=clear:id=bad!:id=us-east"));
    stateMachine.ProcessString(Report(L"state=idle:id=a//b"));
    VERIFY_ARE_EQUAL(6u, term.GetProgramStatusRecords().size());
    VERIFY_ARE_EQUAL(before, notifications);

    Log::Comment(L"clear with an id removes the record and its subtree, not prefix-siblings");
    stateMachine.ProcessString(Report(L"state=clear:id=us-east"));
    records = term.GetProgramStatusRecords();
    VERIFY_ARE_EQUAL(3u, records.size());
    VERIFY_IS_NULL(FindRecord(records, L"us-east"));
    VERIFY_IS_NULL(FindRecord(records, L"us-east/web"));
    VERIFY_IS_NULL(FindRecord(records, L"us-east/web/a"));
    VERIFY_IS_NOT_NULL(FindRecord(records, L"us-east-2"));
    VERIFY_IS_NOT_NULL(FindRecord(records, L""));

    Log::Comment(L"OSC 133;A drops live (working/blocked) records only");
    stateMachine.ProcessString(Report(L"state=blocked:id=ask:kind=question"));
    stateMachine.ProcessString(Report(L"state=error:id=failed"));
    stateMachine.ProcessString(L"\x1b]133;A\x07");
    records = term.GetProgramStatusRecords();
    VERIFY_IS_NULL(FindRecord(records, L"")); // root was working
    VERIFY_IS_NULL(FindRecord(records, L"ask"));
    VERIFY_IS_NOT_NULL(FindRecord(records, L"eu/x/y")); // done
    VERIFY_IS_NOT_NULL(FindRecord(records, L"failed")); // error
    VERIFY_IS_NOT_NULL(FindRecord(records, L"us-east-2")); // idle

    Log::Comment(L"Connection close drops live records too (same rule, explicit entry point)");
    stateMachine.ProcessString(Report(L"state=working:id=job"));
    term.ProgramStatusDropLive("test");
    VERIFY_IS_NULL(FindRecord(term.GetProgramStatusRecords(), L"job"));

    Log::Comment(L"A prompt with nothing live to drop doesn't notify");
    const auto quiet = notifications;
    stateMachine.ProcessString(L"\x1b]133;A\x07");
    VERIFY_ARE_EQUAL(quiet, notifications);

    Log::Comment(L"DECSTR keeps records; RIS removes them all");
    stateMachine.ProcessString(L"\x1b[!p");
    VERIFY_ARE_EQUAL(3u, term.GetProgramStatusRecords().size());
    stateMachine.ProcessString(L"\x1b"
                               L"c");
    VERIFY_ARE_EQUAL(0u, term.GetProgramStatusRecords().size());

    Log::Comment(L"clear without an id removes everything");
    stateMachine.ProcessString(Report(L"state=done:id=a"));
    stateMachine.ProcessString(Report(L"state=done:id=b"));
    stateMachine.ProcessString(Report(L"state=clear"));
    VERIFY_ARE_EQUAL(0u, term.GetProgramStatusRecords().size());
}

void TerminalCoreUnitTests::TerminalApiTest::ProgramStatusRecordCap()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);
    auto& stateMachine = *(term._stateMachine);

    constexpr auto cap = ::Microsoft::Console::VirtualTerminal::ProgramStatus::MaxRecords;
    for (size_t i = 0; i < cap; ++i)
    {
        stateMachine.ProcessString(Report(L"state=done:id=r" + std::to_wstring(i)));
    }
    VERIFY_ARE_EQUAL(cap, term.GetProgramStatusRecords().size());

    Log::Comment(L"Touch r0 so r1 becomes the least recently updated, then overflow");
    stateMachine.ProcessString(Report(L"state=idle:id=r0"));
    stateMachine.ProcessString(Report(L"state=done:id=overflow"));
    const auto records = term.GetProgramStatusRecords();
    VERIFY_ARE_EQUAL(cap, records.size());
    VERIFY_IS_NOT_NULL(FindRecord(records, L"r0"));
    VERIFY_IS_NULL(FindRecord(records, L"r1"));
    VERIFY_IS_NOT_NULL(FindRecord(records, L"overflow"));

    Log::Comment(L"Updating an existing record at the cap evicts nothing");
    stateMachine.ProcessString(Report(L"state=error:id=r2"));
    VERIFY_ARE_EQUAL(cap, term.GetProgramStatusRecords().size());
    VERIFY_IS_NOT_NULL(FindRecord(term.GetProgramStatusRecords(), L"r3"));
}

void TerminalCoreUnitTests::TerminalApiTest::ShellCommandRunning()
{
    Terminal term{ Terminal::TestDummyMarker{} };
    DummyRenderer renderer{ &term };
    term.Create({ 100, 100 }, 0, renderer);
    size_t notifications = 0;
    term.SetShellCommandChangedCallback([&]() { ++notifications; });
    auto& stateMachine = *(term._stateMachine);

    VERIFY_IS_FALSE(term.IsShellCommandRunning());
    stateMachine.ProcessString(L"\x1b]133;A\x07\x1b]133;B\x07");
    VERIFY_IS_FALSE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(0u, notifications);

    stateMachine.ProcessString(L"\x1b]133;C\x07");
    VERIFY_IS_TRUE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(1u, notifications);

    Log::Comment(L"A non-zero exit just ends the command");
    stateMachine.ProcessString(L"\x1b]133;D;1\x07");
    VERIFY_IS_FALSE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(2u, notifications);

    Log::Comment(L"A new prompt without D also ends it");
    stateMachine.ProcessString(L"\x1b]133;C\x07\x1b]133;A\x07");
    VERIFY_IS_FALSE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(4u, notifications);

    Log::Comment(L"Repeated marks don't re-notify");
    stateMachine.ProcessString(L"\x1b]133;D\x07\x1b]133;A\x07");
    VERIFY_ARE_EQUAL(4u, notifications);

    Log::Comment(L"The shell exiting mid-command (connection closed, no D) ends it");
    stateMachine.ProcessString(L"\x1b]133;C\x07");
    VERIFY_IS_TRUE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(5u, notifications);
    term.ShellCommandEnded("test");
    VERIFY_IS_FALSE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(6u, notifications);
    term.ShellCommandEnded("test");
    VERIFY_ARE_EQUAL(6u, notifications);

    Log::Comment(L"RIS ends a running command too");
    stateMachine.ProcessString(L"\x1b]133;C\x07");
    VERIFY_IS_TRUE(term.IsShellCommandRunning());
    stateMachine.ProcessString(L"\x1b"
                               L"c");
    VERIFY_IS_FALSE(term.IsShellCommandRunning());
    VERIFY_ARE_EQUAL(8u, notifications);
}
