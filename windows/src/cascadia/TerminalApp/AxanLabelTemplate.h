// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanLabelTemplate — the templated-label engine (milestone M5, the "terminal
// contract"). A session-tree node's label is a live template expanded against the
// running shell's cwd/title: e.g. "$dir $branch" -> "axan main".
//
// This is the Windows port of the Linux sidebar template engine
// (linux/src/terminal-sidebar.cc: sidebar_expand_name_template /
// sidebar_lookup_template_var / the git walk / ${file:path}). It is deliberately
// GUI-free and WinRT-free — pure string + std::filesystem + a couple of Win32
// environment lookups — so it can migrate to the shared C++ core (core/, D4/D10)
// unchanged when extraction happens. The WinUI wiring that feeds it cwd/title and
// pushes the result into the TreeView node lives in TerminalPage; this unit only
// computes text.
//
// Supported variables (case-insensitive; '_' and '-' are stripped, so $host-name,
// $host_name and $hostname all resolve the same):
//   $pwd, $cwd        full current working directory
//   $dir              basename of the current working directory
//   $~                cwd with the user profile collapsed to "~"
//   $user             USERNAME
//   $host, $hostname  COMPUTERNAME (short host)
//   $shell            basename of the spawn command (e.g. "pwsh.exe")
//   $cmd, $title      the window title (OSC 0/2)
//   $branch, $gitbranch   git branch via a .git/HEAD walk-up from cwd
//   $repo, $gitrepo       basename of the git toplevel
//   ${file:path}      first line of a file (the only brace form today; '~' -> profile)
//
// Unknown names are emitted literally (including the '$') so typos stay visible.

#pragma once

#include <string>
#include <string_view>

namespace Axan
{
    // The live runtime context a template expands against. cwd and title come from the
    // active TermControl (ICoreState::CurrentWorkingDirectory / Title); shell is the
    // basename of the active profile's command, supplied by the caller (the control has
    // no notion of "the shell"). user/host are resolved from the environment inside the
    // engine so callers don't have to.
    struct LabelContext
    {
        std::wstring cwd; // plain filesystem path (WT's OSC 9;9). May be empty before the
                          // shell reports one, or a "\\wsl$\<distro>\..." UNC for WSL.
        std::wstring title; // window title (OSC 0/2)
        std::wstring shell; // basename of the spawn command, e.g. L"pwsh.exe"
    };

    // Expand $-variable references in `tmpl` against `ctx`. Unknown names emit literally.
    std::wstring ExpandLabelTemplate(std::wstring_view tmpl, const LabelContext& ctx);

    // The fallback used when a node carries no custom template: the window title, else
    // the basename of the cwd, else the literal "shell" so a row is never blank. Mirrors
    // the Linux sidebar_compute_row_label_text precedence.
    std::wstring DefaultLabel(const LabelContext& ctx);

    // If `tmpl` is non-empty, expand it; otherwise DefaultLabel(ctx). This is the single
    // entry point the sidebar calls per node.
    std::wstring ComputeLabel(std::wstring_view tmpl, const LabelContext& ctx);
}
