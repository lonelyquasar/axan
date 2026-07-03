# tomlplusplus — vendored single header (axan addition)

Not part of the frozen Windows Terminal snapshot (see `windows/VENDOR`). This is an
**axan-added** dependency for the portable-profile + startup-sessions TOML interchange
(decisions D18/D19; format spec in the root README, Configuration section): per-profile
`<uuid>.toml` mirrors plus the global `startup-sessions.toml`.

Used on the **parse/import** path only — the Startup-sessions editor importer
(`TerminalSettingsEditor/StartupSessionsViewModel.cpp`) is the one TU that includes
`toml.hpp`. The export path is hand-rolled (`TerminalApp/AxanProfileToml.cpp` plus the
shared wire-format header `src/inc/AxanLaunchEntryWire.h`) because toml++ drops comments
on serialize — same split the Linux build chose (`409c35d`).

- Upstream:   https://github.com/marzer/tomlplusplus
- Version:    v3.4.0
- License:    MIT (SPDX-License-Identifier embedded at the top of `toml.hpp`)
- File:       `toml.hpp` (single-header amalgamation, header-only — no .lib/.dll)
- Fetched:    2026-05-31 via
              `curl -fsSL https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp`

Header-only: consumed via `#include <toml.hpp>` with this directory on the include path
(`Microsoft.Terminal.Settings.Editor.vcxproj` AdditionalIncludeDirectories). Nothing to link.

## Updating

Only if a security/correctness fix in toml++ matters to the import path. Re-fetch the
single header at the new tag, update the version/date above. Keep it pinned otherwise —
axan is fork-and-freeze.
