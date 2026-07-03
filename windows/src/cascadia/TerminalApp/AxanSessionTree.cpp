// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.

#include "pch.h"
#include "AxanSessionTree.h"
#include "AxanLog.h"
#include "AxanShellIntegration.h" // M14: shell-family detection for command-survival wrapping

#include <unordered_map>

#include "../../types/inc/utils.hpp" // GuidToString / GuidFromString

using namespace winrt::Microsoft::Terminal::Settings::Model;
namespace Model = winrt::Microsoft::Terminal::Settings::Model;

namespace
{
    // M14: expand a leading "~" to %USERPROFILE% (Linux parity: leading ~ -> $HOME). An
    // absolute path or a "%VAR%" reference passes through unchanged — WT evaluates env vars in
    // a NewTerminalArgs StartingDirectory itself. Empty stays empty ("the launch directory").
    std::wstring _expandDirectory(std::wstring_view dir)
    {
        if (dir.empty())
        {
            return {};
        }
        if (dir.front() == L'~' && (dir.size() == 1 || dir[1] == L'\\' || dir[1] == L'/'))
        {
            wchar_t home[MAX_PATH]{};
            const auto len = ::GetEnvironmentVariableW(L"USERPROFILE", home, ARRAYSIZE(home));
            if (len > 0 && len < ARRAYSIZE(home))
            {
                return std::wstring{ home, len } + std::wstring{ dir.substr(1) };
            }
        }
        return std::wstring{ dir };
    }

    // M14: build a commandline that runs `command` under `shellCmdline` (the active profile's
    // shell) AND survives the command's exit — the Windows analogue of the Linux
    // `$SHELL -c "cmd; exec $SHELL"` wrap (commit f88b566). Each shell family keeps the tab
    // interactive differently. An unrecognized shell family can't be wrapped, so the command
    // runs as the bare commandline (literal semantics, no survival); the caller logs that.
    //
    // Quoting is best-effort: simple commands (claude, ssh user@host, wsl, pwsh) carry no
    // embedded quotes and pass through cleanly. A command containing its own double-quotes is
    // not escaped here — a known limitation tracked for a later pass.
    std::wstring _wrapCommandForSurvival(std::wstring_view shellCmdline, std::wstring_view command)
    {
        const std::wstring shell{ shellCmdline };
        const std::wstring cmd{ command };
        switch (Axan::DetectShellFamily(shellCmdline))
        {
        case Axan::ShellFamily::PowerShell:
            // -NoExit keeps the session interactive after -Command finishes.
            return shell + L" -NoExit -Command \"" + cmd + L"\"";
        case Axan::ShellFamily::Cmd:
            // cmd /k runs the command then stays at the prompt.
            return shell + L" /k \"" + cmd + L"\"";
        case Axan::ShellFamily::Wsl:
            // Run the command in an interactive bash, then exec a fresh login shell so
            // Ctrl+C / normal exit lands at a prompt instead of closing the tab.
            return shell + L" -- bash -ic \"" + cmd + L"; exec bash -i\"";
        default:
            return cmd;
        }
    }

    // axan D19: resolve the WT profile an entry should spawn under. `profileGuid` is the entry's
    // stored profile GUID; empty or unresolved falls back to the global default profile.
    Model::Profile _resolveProfile(const CascadiaSettings& settings, const winrt::hstring& profileGuid, const Model::Profile& fallback)
    {
        if (!profileGuid.empty())
        {
            try
            {
                const auto g = ::Microsoft::Console::Utils::GuidFromString(profileGuid.c_str());
                if (const auto p = settings.FindProfile(winrt::guid{ g }))
                {
                    return p;
                }
            }
            catch (...)
            {
            }
        }
        return fallback;
    }
}

namespace Axan
{
    std::vector<StartupSession> LoadStartupTree(const CascadiaSettings& settings)
    {
        if (!settings)
        {
            Axan::Log::Warn("AxanSessionTree", "LoadStartupTree: null settings; skipping");
            return {};
        }

        // D19: the curated tree is GLOBAL (GlobalAppSettings.StartupSessions). The default profile
        // is only the fallback shell for entries with no/unresolved profile ref.
        Model::Profile defaultProfile{ nullptr };
        try
        {
            defaultProfile = settings.FindProfile(settings.GlobalSettings().DefaultProfile());
        }
        catch (...)
        {
        }
        if (!defaultProfile)
        {
            Axan::Log::Warn("AxanSessionTree", "LoadStartupTree: no default profile resolved; WT startup");
            return {};
        }

        const auto entries = settings.GlobalSettings().StartupSessions();
        if (!entries || entries.Size() == 0)
        {
            Axan::Log::Info("AxanSessionTree", "LoadStartupTree: no global startup sessions; WT startup");
            return {};
        }

        std::vector<StartupSession> result;
        result.reserve(entries.Size());
        // id -> emitted index, so a ParentId (forward-reference-only) resolves to a parentIndex
        // that is always smaller than the child's own — the M6 reparent-on-spawn contract.
        std::unordered_map<winrt::hstring, int32_t> indexById;

        for (const auto& entry : entries)
        {
            // D19: each entry spawns under ITS OWN referenced profile (which shell); that profile's
            // commandline drives the survival wrap. Empty/unresolved -> the default profile.
            const auto targetProfile = _resolveProfile(settings, entry.Profile(), defaultProfile);
            const std::wstring profileShell{ targetProfile.Commandline() };
            const auto shellFamily = Axan::DetectShellFamily(profileShell);
            const auto targetGuid = winrt::hstring{ ::Microsoft::Console::Utils::GuidToString(targetProfile.Guid()) };

            StartupSession session{};

            NewTerminalArgs ntArgs{};
            ntArgs.Profile(targetGuid);

            if (const auto dir = _expandDirectory(std::wstring_view{ entry.Directory() }); !dir.empty())
            {
                ntArgs.StartingDirectory(winrt::hstring{ dir });
            }

            if (const std::wstring command{ entry.Command() }; !command.empty())
            {
                ntArgs.Commandline(winrt::hstring{ _wrapCommandForSurvival(profileShell, command) });
                if (shellFamily == Axan::ShellFamily::Unknown)
                {
                    Axan::Log::Warn("AxanSessionTree", "LoadStartupTree: unknown shell family; command runs without a survival wrap", { { "shell", winrt::to_string(profileShell) } });
                }
            }

            NewTabArgs tabArgs{ ntArgs };
            session.action = ActionAndArgs{ ShortcutAction::NewTab, tabArgs };
            session.id = entry.Id();
            session.labelTemplate = entry.Name();
            session.iconOverride = entry.Icon();
            // axan #422: restore the per-node recolor token + target so a colored session
            // survives a restart. The token is a palette name resolved per theme at render time.
            session.iconColor = entry.Color();
            session.colorTarget = entry.ColorTarget();

            int32_t parentIndex = -1;
            if (const auto parentId = entry.ParentId(); !parentId.empty())
            {
                if (const auto it = indexById.find(parentId); it != indexById.end())
                {
                    parentIndex = it->second;
                }
                else
                {
                    Axan::Log::Warn("AxanSessionTree", "LoadStartupTree: parentId not seen before its child (forward-reference violated); treating as root", { { "parentId", winrt::to_string(parentId) } });
                }
            }
            session.parentIndex = parentIndex;

            const auto myIndex = static_cast<int32_t>(result.size());
            if (const auto id = entry.Id(); !id.empty())
            {
                indexById[id] = myIndex;
            }
            result.push_back(std::move(session));
        }

        TraceLoggingWrite(
            g_hTerminalAppProvider,
            "AxanSessionTreeLoaded",
            TraceLoggingDescription("Emitted when axan loads its curated startup session tree"),
            TraceLoggingValue(static_cast<uint64_t>(result.size()), "sessionCount"),
            TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
            TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

        Axan::Log::Info("AxanSessionTree", "LoadStartupTree: expanded global startup sessions", { { "entryCount", std::to_string(result.size()) } });
        return result;
    }
}
