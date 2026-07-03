// Copyright (c) Lonely Quasar.
// Licensed under the MIT license.
//
// AxanProfileToml — the Windows EXPORT half of the cross-platform portable-profile TOML
// interchange (decisions D18/D19; spec: root README, Configuration). The Linux side is
// linux/src/profile-toml.cc; the Windows IMPORT side is the Startup-sessions editor
// (TerminalSettingsEditor/StartupSessionsViewModel::ImportFromToml), which applies the
// portable tree through the live settings model rather than rewriting settings.json.
//
// Model: each platform's native config is canonical (D15). On Windows that is WT's
// settings.json, read here directly as JSON (off the WinRT projection boundary, the same
// shape as the Linux engine reading dconf). Each settings reload refreshes a portable
// mirror in <LocalState>\profiles\:
//   * <uuid>.toml         — one per profile: [meta] + [profile] identity + the verbatim
//                           [profile.native.windows] passthrough. Post-D19 a profile
//                           carries no launch entries, so this is identity + appearance.
//   * startup-sessions.toml — THE global startup tree (GlobalAppSettings.StartupSessions),
//                           one per workstation, as a top-level [[startup-sessions]]
//                           array-of-tables with a per-entry profile GUID + profile-name
//                           fallback (D19). Schema: src/inc/AxanLaunchEntryWire.h.
// Stale <uuid>.toml mirrors for deleted profiles are pruned (only files matching the
// bare-uuid name pattern axan itself writes — never anything else).
//
// Lossy by design: a JSON `null` profile value (WT uses null meaningfully, e.g.
// "tabColor": null) has no TOML analogue, so null-valued keys are SKIPPED on export —
// a re-import leaves them untouched (sparse contract) instead of stamping "".
//
// The wire format (kebab portable core, builtin: icon vocabulary, sparse color rules) is
// single-sourced in src/inc/AxanLaunchEntryWire.h. Errors are returned to the caller (a
// string out-param), which logs them through Axan::Log — this unit takes no logging
// dependency so it can move toward core/ later.

#pragma once

#include <filesystem>
#include <string>

namespace Axan::ProfileToml
{
    // Refresh the portable mirror at `outDir` from the settings.json at `settingsPath`:
    // write <outDir>\<uuid>.toml per profile plus <outDir>\startup-sessions.toml for the
    // global startup tree, then prune stale <uuid>.toml files whose profile is gone.
    // Creates outDir if needed. Returns the count of profile files written, or -1 if
    // settings.json couldn't be read/parsed or outDir couldn't be created; `error` holds
    // the first failure encountered (the export continues past individual file failures,
    // so `error` can be non-empty even when the return value is >= 0 — callers should
    // log it whenever it is non-empty).
    int ExportAllProfiles(const std::filesystem::path& settingsPath,
                          const std::filesystem::path& outDir,
                          std::string& error);
}
