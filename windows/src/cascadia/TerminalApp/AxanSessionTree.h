// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanSessionTree — axan's curated startup session tree (M4, retargeted by D19).
//
// Reads the GLOBAL launch-entry tree (GlobalAppSettings.StartupSessions in WT's
// settings.json — decision D19) and converts it into the Windows Terminal
// startup-action vector that TerminalPage::SetStartupActions consumes. The Windows
// evolution of the Linux `default-launch-entries` model: a curated, deterministic set
// of sessions that spawn on a plain launch — distinct from "restore whatever was open
// last time" (WT's persisted layout). See decisions D13/D19.
//
// The expansion logic here is deliberately kept in its own unit (not buried in the
// windowing glue) so the portable pieces can migrate to the shared core later (D4/D10).

#pragma once

#include <winrt/Microsoft.Terminal.Settings.Model.h>

namespace Axan
{
    // One curated startup session, in depth-first spawn order: the spawn action, its
    // optional label template (M5), and its place in the hierarchy (M6). The whole tree
    // is flattened DFS because WT's startup path spawns a flat list of tabs; the caller
    // re-applies the nesting to the sidebar nodes after the tabs exist, using parentIndex.
    struct StartupSession
    {
        winrt::Microsoft::Terminal::Settings::Model::ActionAndArgs action;
        winrt::hstring labelTemplate; // empty -> the node uses the default title/cwd label
        // axan #422: the source entry's stable Id, carried onto the spawned node so the sidebar
        // node editor's Save can write its edits back to this exact StartupSessions element.
        winrt::hstring id;
        // axan M13: the node's per-node icon override + recolor, restored onto the node's
        // view-model when the startup tab spawns. Empty override -> the node falls back to
        // the profile icon, else an auto-assigned glyph (the M7 resolution chain); empty
        // color -> the glyph paints with the theme foreground. Set interactively via the
        // node context-menu icon picker (M13) and round-tripped here so it survives a relaunch.
        winrt::hstring iconOverride;
        winrt::hstring iconColor;
        // axan M13 (design handoff): which surfaces the recolor paints — "icon", "text", or
        // "both" (default). Empty -> "both".
        winrt::hstring colorTarget;
        // Index (into the returned vector) of this node's parent, or -1 for a root-level
        // node. DFS order guarantees the parent precedes its children, so this is always a
        // smaller index than the node's own — the caller can reparent on the fly as each
        // tab spawns.
        int32_t parentIndex{ -1 };
    };

    // axan #3: one separator row from the startup tree. A separator spawns NOTHING — it is
    // never a StartupSession and never gets a spawn index — so it can't ride in the
    // index-aligned metadata vectors. It is described by position instead: the spawn index
    // of the session it nests under (parentSpawnIndex, -1 = root) and the spawn index of the
    // nearest PRECEDING sibling *session* in that same scope (afterSpawnIndex, -1 = first in
    // its scope). The sidebar inserts the separator node right after that sibling's node once
    // every startup session has been realized (TerminalPage::_RealizeStartupSeparators).
    struct StartupSeparator
    {
        winrt::hstring id; // the LaunchEntry Id, kept on the node so a capture round-trips it
        winrt::hstring style; // "line" | "space" (normalized; empty -> "line")
        double height{ 1.0 }; // in session-row units, already NormalizeSeparatorHeight'd
        winrt::hstring placement; // "inline" | "bottom" (normalized; empty -> "inline")
        int32_t parentSpawnIndex{ -1 };
        int32_t afterSpawnIndex{ -1 };
    };

    // axan D19: read the GLOBAL startup tree (GlobalAppSettings.StartupSessions) and flatten
    // it into StartupSessions. Each entry becomes a NewTab action under ITS OWN referenced
    // profile (LaunchEntry.Profile, a WT profile GUID; empty/unresolved -> the global default
    // profile), carrying the entry's StartingDirectory (leading "~" -> %USERPROFILE%) and,
    // when the entry has a command, a Commandline that runs that command under that profile's
    // shell *and survives its exit* (per-shell wrap: pwsh `-NoExit -Command`, cmd `/k`,
    // wsl/bash `... ; exec $SHELL`). The entry's Name rides as the label template and its
    // Icon as the per-node icon override; ParentId (a forward-reference-only GUID) resolves
    // to parentIndex (M6 nesting contract).
    //
    // There is no migration path: pre-public policy is that retired schemas (the per-profile
    // DefaultLaunchEntries of M14/D17, the standalone sessions.json of D15/D16) are simply
    // gone — the global tree in settings.json is the only source.
    //
    // axan #3: separator entries (Kind == "separator") produce NO action; they are returned
    // through `outSeparators`, positioned by their parent's / preceding sibling session's spawn
    // index (see StartupSeparator). A session that names a separator as its parent is
    // re-parented to the separator's own parent (a separator can't own sessions) with a warning.
    //
    // Returns an empty vector when there is no default profile, the global tree is empty,
    // or settings is null — the caller then falls back to WT's normal startup (a tree of
    // separators only also counts as empty: nothing to spawn, so nothing to divide).
    std::vector<StartupSession> LoadStartupTree(const winrt::Microsoft::Terminal::Settings::Model::CascadiaSettings& settings,
                                                std::vector<StartupSeparator>& outSeparators);
}
