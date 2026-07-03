// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanShellIntegration — shell-integration injection (milestone M10, "OSC emit").
//
// M5 built the label engine and wired the consume side (WT's ControlCore turns an
// OSC 9;9 into ICoreState::CurrentWorkingDirectory, which the sidebar reads), but the
// stock shells WT spawns do not emit that sequence by default, so a `cd` never moved a
// label. M10 is the missing emit half: when axan spawns a recognized shell, it rewrites
// the spawn so the shell reports its cwd/title each prompt.
//
// This is the Windows port of the Linux bash-rcfile injection
// (linux/data/shell-integration/bash/axan-rcfile.sh + the argv rewrite in the GTK fork,
// lonelyquasar/axan#405). The seam is the same idea — modify the spawn so the shell loads
// our integration — realized at TerminalPage::_CreateConnectionFromSettings, just before
// the commandline becomes a ConptyConnection. Three shell families, three mechanisms:
//
//   PowerShell (pwsh / powershell.exe) — argv rewrite:
//       <exe> -NoExit -Command ". '<LocalState>\shell-integration\pwsh\...ps1'"
//     The script is dot-sourced AFTER the user's $PROFILE (profiles load by default; only
//     -NoProfile suppresses them), so it wraps the user's prompt rather than replacing it.
//
//   cmd.exe — environment (%PROMPT%): no argv munging. cmd reads %PROMPT% at startup; we
//     set it to a string whose $E (ESC) + $P (cwd) codes emit OSC 9;9 each prompt. Best
//     effort (a user's /k that re-sets PROMPT wins); falls back to spawn-cwd silently.
//
//   WSL/bash (wsl.exe ...) — argv rewrite: `... -- bash --rcfile <wrapper> -i`. The wrapper
//     re-sources ~/.bashrc then installs a PROMPT_COMMAND that emits OSC 9;9. Unlike the
//     Linux origin it does NOT source VTE's profile.d hooks (absent in a headless WSL
//     distro) — it emits directly, and uses `wslpath -w` to report a \\wsl.localhost path
//     when available so M5's $branch/$repo git-walk resolves across the \\wsl$ boundary.
//
// The integration scripts live in the package LocalState (reachable from WSL as /mnt/c/...,
// unlike the appx install dir's restrictive ACLs) and are materialized on demand. The
// emit target is fixed by the consume side: OSC 9;9;"<path>" ST, a path that must pass
// til::is_legal_path (adaptDispatch.cpp's SetWorkingDirectory).
//
// Detection/policy is deliberately a small, separable unit (like AxanLabelTemplate) so the
// portable parts can migrate to the shared core later (D4/D10); the LocalState
// materialization is the one Windows-specific piece, mirroring AxanSessionTree.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace Axan
{
    // The shell families axan knows how to inject. Unknown = leave the spawn untouched.
    enum class ShellFamily
    {
        Unknown,
        PowerShell, // pwsh.exe (7+) and powershell.exe (Windows PowerShell 5.1)
        Cmd, // cmd.exe
        Wsl, // wsl.exe (the default-distro launcher WT's WSL profiles use)
    };

    // Classify a spawn commandline by the basename of its first token (the exe). Handles a
    // quoted exe path and a bare first token, matching M5's _shellBasenameFromCommandline.
    ShellFamily DetectShellFamily(std::wstring_view commandline);

    // The result of computing shell-integration injection for one spawn.
    struct ShellIntegration
    {
        // True when we changed the spawn (rewrote the commandline and/or produced a prompt
        // env). When false, `commandline` echoes the input unchanged and `promptEnv` is empty
        // — the caller spawns exactly as it would have.
        bool applied{ false };

        // The commandline to actually spawn (rewritten for PowerShell/WSL; unchanged for cmd,
        // whose injection rides the environment instead).
        std::wstring commandline;

        // For cmd: the value to set as %PROMPT% in the spawn environment so cmd emits OSC 9;9.
        // Empty/absent for every other family.
        std::optional<std::wstring> promptEnv;
    };

    // Compute shell-integration injection for `commandline`. When `enabled` and the
    // commandline is a recognized shell launched *plainly* (no custom -Command/-File/-c for
    // PowerShell, no /c one-shot for cmd, no `--`/`-e` one-shot for WSL — so we never clobber
    // a user's own command), this materializes the needed script under LocalState and returns
    // a rewritten commandline (PowerShell/WSL) or a cmd %PROMPT% value, with applied=true.
    // Otherwise it returns the input unchanged with applied=false. Safe to call on every
    // spawn: re-materializing a script is idempotent (write-if-missing-or-stale).
    ShellIntegration ComputeShellIntegration(std::wstring_view commandline, bool enabled);
}
