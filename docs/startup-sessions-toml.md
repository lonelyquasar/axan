# Authoring `startup-sessions.toml`

This guide is for anyone, human or AI agent, who has to write an axan startup tree by hand on a Windows machine, usually from a list of repositories, and import it through **Settings → Startup sessions → Import from TOML…**. It assumes you cannot see axan's UI while you write. Everything here is taken from the code on the branch it was written against. Where the root [README](../README.md) says something different, this guide follows the code.

If you are an agent: read sections 1 to 4 once, pick icons with section 5, copy the shape of the example in section 7, then run the checks in section 8.

## 1. What the file is and what Import does

axan's startup tree is the list of sessions the sidebar spawns when axan launches. The canonical copy lives in Windows Terminal's `settings.json` under the root key `startupSessions`. The TOML file is a portable copy of it.

- **Where axan writes it.** After every successful settings load, axan writes `startup-sessions.toml` to `%LOCALAPPDATA%\Packages\sh.axan.Axan_<publisher-id>\LocalState\profiles\`, next to one `<profile-guid>.toml` per profile. That copy is regenerated each time, so comments are not preserved. Write your own file anywhere else and import it.
- **Import replaces the whole tree.** It does not merge by `id`. The current list on the Startup sessions page is swapped for the rows in the file, in file order, and the first row is selected.
- **Then press Save.** Import changes the Settings editor's copy, like any other edit on that page. Nothing reaches `settings.json` until you press **Save** in the Settings window. The sidebar builds the tree at the next launch of axan. It is used only for a plain launch: started with any command-line arguments, axan does what the arguments say instead.
- **A failed import changes nothing.** The page shows an info bar saying the import failed and the list was left untouched. The reason is in the log (section 8).

What counts as a failure, and what is only a warning:

| Situation | Result |
|---|---|
| File cannot be opened or read | Import fails |
| File is not valid TOML 1.0 | Import fails; the parser's message is logged |
| `[meta] axan-toml-version` is greater than `2` | Import fails |
| No `[[startup-sessions]]` array | Import fails |
| The array produces zero rows | Import fails, so an empty file can never wipe your tree |
| `[meta]` missing, or `axan-toml-version` missing or not an integer | Accepted, read as version 1 |
| A row with an unknown `kind` | That row is skipped, with a warning |
| A row with no `id` (or `id = ""`) | A fresh id is generated for it, silently |
| Two rows with the same `id` | Both are imported. Anything that looks a row up by id finds the later one. Do not do this |
| `parent-id` names a row that comes later in the file, or no row at all | Imported as written. The row shows at root depth, and at launch it spawns as a root session with a warning |
| `parent-id` names a separator | At launch the row is placed under the separator's own parent, with a warning |
| `profile` GUID not on this machine | Re-matched by `profile-name` if one is given; otherwise kept, with a warning, and the session spawns under the default profile |
| `icon = "builtin:NAME"` with an unknown NAME | The icon is dropped (default placeholder), with a warning |
| Unknown names inside `hoist` | Dropped, with a warning. If none are left, the row becomes `"never"` |
| Unknown keys | Ignored silently. The README says they are logged; the code does not log them |
| A known key with the wrong TOML type (`collapsed = "true"`, `name = 5`) | Ignored silently; the field takes its default |

The parser is [toml++](https://github.com/marzer/tomlplusplus) v3.4.0 in strict TOML 1.0 mode. TOML 1.1 additions such as `\x41` and `\e` escapes are rejected as parse errors. Save the file as UTF-8.

## 2. Key reference

The file has an optional `[meta]` table and one `[[startup-sessions]]` table per row.

### `[meta]`

| Key | Type | Meaning |
|---|---|---|
| `axan-toml-version` | integer | `1` for a tree without separators, `2` if it has any. Import accepts any value up to `2` (it does not check that separators came with `2`), so writing `2` is always safe |
| `exported-by` | string | Provenance written by the exporter (`"windows"`). Not read on import |

### Order and nesting

- **List order is sidebar order.** Rows appear top to bottom exactly as they appear in the file, within their parent.
- **A parent must come before its children.** `parent-id` refers back to an earlier row's `id`. `parent-id = ""` (or omitting it) puts the row at the root.
- **Nest under sessions, not separators.** A separator can itself be nested under a session. A row should not name a separator as its parent; axan rehomes it at launch.
- **Ids** are any unique string, but write a UUID: lowercase, hyphenated, no braces, from any generator (`python -c "import uuid; print(uuid.uuid4())"`, PowerShell `[guid]::NewGuid()`). They only need to be unique within the file. They must stay stable if you want other settings that refer to a row, such as its hoist override, to keep pointing at it.

### Session rows

`kind` absent (or `"session"`) makes a session: one terminal tab that axan spawns at launch.

| Key | Type | Default when omitted | Meaning |
|---|---|---|---|
| `id` | string | a generated UUID | The row's identity |
| `parent-id` | string | `""` (root) | The `id` of an earlier session row |
| `profile` | string | the default profile | Bare GUID of the Windows Terminal profile to spawn under (`61c54bbd-c2c6-5271-96e7-009a87ff44bf`; braces are tolerated). This picks the shell: PowerShell, cmd, a WSL distro |
| `profile-name` | string | none | The profile's display name, used only when `profile` is present but not found. See section 3 |
| `name` | string | `""` | The row's label, a template (section 4). `""` means the terminal title, else the folder name, else `shell` |
| `directory` | string | `""` | Starting directory. `"~"` or a path starting `~\` or `~/` expands to `%USERPROFILE%`; `%VAR%` references are expanded by Windows Terminal. `""` means the directory axan was launched from |
| `command` | string | `""` | A command to run inside the profile's shell. `""` means just the shell. See below |
| `icon` | string | `""` | The row's icon (section 5). `""` means the profile's icon, else an automatic glyph |
| `color` | string | `""` | Tint: `red`, `orange`, `yellow`, `green`, `blue`, `purple` (case-insensitive; each has a light-theme and dark-theme shade), or `"#RRGGBB"` / `"#AARRGGBB"`. Any other name means no tint |
| `color-target` | string | `"both"` | What `color` tints: `"icon"`, `"text"` or `"both"`. Lowercase only |
| `hoist` | string or array | follow the global `sidebar.hoistStates` | `"never"`, or a list of state names (lowercase only): `idle`, `working`, `blocked`, `done`, `error`, `running`, `permission`, `question`, `auth`. An empty list `[]` means `"never"` |

**How `command` runs.** The command is wrapped so the tab survives its exit, according to the profile's shell. PowerShell runs `<shell> -NoExit -Command "<command>"`, cmd runs `<shell> /k "<command>"`, and WSL runs `<shell> -- bash -ic "<command>; exec bash -i"`. For any other shell, such as Git Bash, the command replaces the shell, so the tab closes when the command exits; that case is logged. Double quotes inside `command` are not escaped, so keep them out: `command = "claude"` and `command = "npm run dev"` are fine, a command containing `"` is not.

### Separator rows

`kind = "separator"` makes a divider that spawns nothing. A labelled separator is a collapsible section header. Only these keys apply; session keys on a separator are ignored.

| Key | Type | Default when omitted | Meaning |
|---|---|---|---|
| `id`, `parent-id` | string | as for sessions | A separator nests and orders like a session |
| `kind` | string | | `"separator"` |
| `name` | string | `""` | Section header label. `""` is a plain divider |
| `collapsed` | boolean | `false` | Start the section collapsed. Only takes effect with a non-empty `name` |
| `style` | string | `"line"` | `"line"` draws a rule; `"space"` leaves a gap |
| `height` | float or integer | `1.0` | Height in session-row units, clamped to `0.1`–`10` and rounded to tenths |
| `placement` | string | `"inline"` | `"inline"` keeps its list position; `"bottom"` pins it below its siblings |

A labelled section covers the rows after it, up to the next separator at the same depth.

## 3. Profiles

`profile-name` is only consulted when `profile` is also present and that GUID is not installed. A row with only `profile-name` spawns under the default profile. The name must match the profile's display name exactly, including case.

In order of preference:

1. **Use the real GUID.** On the target machine, the file names in `LocalState\profiles\` are the profile GUIDs, and each `<guid>.toml` has the profile's name inside it. The `guid` fields in `settings.json` work too.
2. **Use a stock GUID.** Windows Terminal generates these two the same way on every machine: Windows PowerShell is `61c54bbd-c2c6-5271-96e7-009a87ff44bf` and Command Prompt is `0caa0dad-35be-5f56-a8ff-afceeeaa6101`.
3. **Use a placeholder GUID plus the name** when you know the name but not the GUID, typical for PowerShell 7 (`"PowerShell"`) and WSL distros (`"Ubuntu"`): `profile = "00000000-0000-0000-0000-000000000000"` with `profile-name = "Ubuntu"`. Import swaps in the real GUID when the name matches. If it does not match, the placeholder is kept, a warning is logged, and the row spawns under the default profile.
4. **Omit both** to use whatever the default profile is.

## 4. Labels

`name` is a template, expanded live in the sidebar. Unknown variables are shown literally, so typos are visible. Names are case-insensitive, and `-`/`_` inside them are ignored.

| Variable | Expands to |
|---|---|
| `$repo`, `$gitrepo` | Folder name of the enclosing git repository |
| `$branch`, `$gitbranch` | Current git branch |
| `$dir` | Folder name of the current directory |
| `$pwd`, `$cwd` | Full current directory |
| `$~` | Current directory with your profile folder shortened to `~` |
| `$title`, `$cmd` | The terminal title set by the running program |
| `$shell` | The shell's executable name, such as `pwsh.exe` |
| `$user`, `$host`, `$hostname` | Windows user name, computer name |
| `${file:path}` | First line of a file (`~` allowed) |

The directory-based variables depend on the shell reporting its directory, which axan sets up automatically for a **plain** PowerShell, cmd or WSL shell. They stay empty until the first prompt, outside a git repo (for `$repo` and `$branch`), and always for a PowerShell or WSL row with a `command`, because axan does not inject its reporting into a shell started for a command.

Recommendation: **use a literal name** (`name = "billing-api"`) for any row with a `command`, and wherever you want the label fixed. `"$repo"` or `"$repo $branch"` suit plain shell rows that move between repos. The Startup sessions list shows each row as `name · folder`, using the last segment of `directory` and leaving it off when it equals the name, so a literal repo name stays readable there.

## 5. Icons

### What the `icon` field accepts

| Form | Example | Notes |
|---|---|---|
| `""` | `""` | The profile's icon, else an automatic glyph |
| `builtin:NAME` | `"builtin:git"` | One of eleven portable names, **lowercase only**: `terminal`, `ssh`, `server`, `database`, `package`, `container`, `git`, `editor`, `logs`, `build`, `test`. An unknown name becomes the placeholder with a warning. The only form the Linux build also understands |
| A Segoe Fluent Icons glyph name | `"robot"` | About 1,400 names. Case-insensitive; spaces and underscores read as hyphens (`"Chat Bubbles"` = `"chat-bubbles"`). The eleven builtin names also work bare (`"git"`). **An invalid name is not reported**: it silently renders as the placeholder |
| One character | `"A"`, `"🦀"`, `"𓆣"`, `"ᚠ"` | Exactly one user-perceived character: a letter, digit, symbol, emoji (including joined emoji and flags) or historic-script sign. Two or more characters render as the placeholder |
| An image path | `'C:\icons\api.png'` | Absolute path to PNG, ICO or SVG, or an exe/dll to take its icon. Windows only, not portable |

**Code-point text is not accepted.** `"U+131A3"`, `"0x131A3"` and `"131A3"` render as the placeholder. Those forms only work in the icon picker's search box. In the file, write the character itself, either pasted or escaped.

### Writing characters in TOML

- **Pasting is fine.** `icon = "🦀"` and `icon = "𓆣"` work in a basic (double-quoted) string, provided the file is UTF-8.
- **Escapes are safer for an agent**, since you cannot see whether a pasted glyph survived. In a basic string use `\uXXXX` (exactly 4 hex digits) for U+0000–U+FFFF and `\UXXXXXXXX` (exactly 8 hex digits) above that: `"\u16A0"` is ᚠ, `"\U000131A3"` is 𓆣, `"\U0001F980"` is 🦀. toml++ supports both; surrogate values `\uD800`–`\uDFFF` are a parse error.
- **Literal strings take no escapes.** `'\U000131A3'` is ten literal characters, which renders as the placeholder. Use single quotes only for Windows paths, such as `directory = 'C:\src\api'`.

### What Windows 11 can draw

axan draws one-character icons in Segoe UI, or Segoe Fluent Icons for the private-use range, and Windows falls back to other installed fonts for anything Segoe UI lacks. A character no installed font covers draws as an empty box, and that cannot be detected from the file. Stay inside the fonts below, all of which ship with Windows 10 and 11. Coverage was checked against the font files' character maps on a Windows 11 machine.

**Segoe Fluent Icons** (`SegoeIcons.ttf`; [Microsoft docs](https://learn.microsoft.com/en-us/windows/apps/design/style/segoe-fluent-icons-font)). Use it by name. Monochrome, takes the tint. The full list of valid names is `windows/src/inc/AxanSegoeFluentGlyphs.h` in this repository, one `{ L"name", 0xCODE, L"Display Name" }` line per glyph, so grep it (`grep -i rocket AxanSegoeFluentGlyphs.h`) before using any name not listed here. These are all verified:

| Theme | Names |
|---|---|
| Code and build | `code`, `bug`, `developer-tools`, `command-prompt`, `repair`, `construction`, `beta`, `diagnostic`, `package`, `puzzle` |
| Infrastructure | `cloud`, `network`, `media-storage-tower`, `storage-optical`, `sync`, `vpn`, `lightning-bolt` |
| Web | `globe`, `world`, `website`, `link`, `search` |
| Security | `shield`, `lock`, `certificate`, `fingerprint`, `permissions` |
| Docs and knowledge | `library`, `document`, `bookmarks`, `folder`, `edit`, `calligraphy-pen`, `education`, `lightbulb` |
| Agents and people | `robot`, `chat-bubbles`, `people`, `mail`, `calendar` |
| Business | `work`, `bank`, `shop`, `payment-card`, `area-chart`, `view-dashboard`, `calculator` |
| Media and design | `color`, `photo`, `movies`, `music-note`, `game`, `font` |
| Misc | `home`, `settings`, `flag`, `heart`, `favorite-star`, `leaf`, `health`, `airplane`, `car`, `cafe` |

Common guesses that do **not** exist: `rocket`, `database`, `server`, `terminal`, `briefcase`, `map`, `compass`, `key`. Use `builtin:database`, `builtin:server` and `builtin:terminal` for those three.

**Segoe UI Emoji** (`seguiemj.ttf`). Colour emoji, the usual Unicode set: 🦀 U+1F980, 🐍 U+1F40D, 🐳 U+1F433, 🧪 U+1F9EA, 🧠 U+1F9E0, 🌐 U+1F310, 📦 U+1F4E6, 🔧 U+1F527, 📚 U+1F4DA, 🧭 U+1F9ED, 🎯 U+1F3AF, 🦊 U+1F98A. Colour glyphs ignore the `color` tint.

**Segoe UI Symbol** (`seguisym.ttf`; [Microsoft docs](https://learn.microsoft.com/en-us/typography/font-list/segoe-ui-symbol)). Monochrome symbols. Characters found only here take the tint: ⌘ U+2318, ⎇ U+2387 (alternative key, a branch-like mark), ★ U+2605, ♔ U+2654, ♞ U+265E, 𝄞 U+1D11E, ䷀ U+4DC0 (I Ching hexagrams U+4DC0–4DFF), 🜁 U+1F701 (alchemical symbols U+1F700–1F77F). Braille (U+2800–28FF), musical symbols (U+1D100–1D1FF), mahjong, domino and playing-card blocks (U+1F000–1F0FF) and Deseret (U+10400–1044F) are here too. Characters also in Segoe UI Emoji, such as ⚙ U+2699 or ⚡ U+26A1, may come out in colour; check them after import.

**Segoe UI Historic** (`seguihis.ttf`; [Microsoft docs](https://learn.microsoft.com/en-us/typography/font-list/segoe-ui-historic), which lists the scripts by tag). Monochrome, takes the tint. Runes and hieroglyphs are scaled up 1.35× by axan to match other icons; the other scripts draw at normal size and may look small.

| Block | Range | Examples |
|---|---|---|
| Egyptian Hieroglyphs | U+13000–1342F | 𓆣 U+131A3 scarab, 𓂀 U+13080 eye of Horus, 𓅓 U+13153 owl, 𓃭 U+130ED lion, 𓅱 U+13171 quail chick, 𓂝 U+1309D arm |
| Runic | U+16A0–16FF | ᚠ U+16A0 fehu, ᚱ U+16B1 raido, ᛉ U+16C9 algiz, ᛗ U+16D7 mannaz, ᛟ U+16DF othala, ᚨ U+16A8 ansuz |
| Cuneiform | U+12000–123FF | 𒀭 U+1202D dingir (god, star), 𒀀 U+12000 a, 𒄑 U+12111 gish (wood) |
| Gothic | U+10330–1034F | 𐌰 U+10330 ahsa |
| Old Italic | U+10300–1032F | 𐌀 U+10300 a |
| Phoenician | U+10900–1091F | 𐤀 U+10900 alf |
| Glagolitic | U+2C00–2C5F | Ⰰ U+2C00 azu |
| Ogham | U+1680–169F | ᚁ U+1681 beith |
| Ugaritic | U+10380–1039F | 𐎀 U+10380 alpa |
| Old Persian | U+103A0–103DF | 𐎠 U+103A0 a |
| Old South Arabian | U+10A60–10A7F | 𐩠 U+10A60 he |
| Shavian | U+10450–1047F | 𐑐 U+10450 peep |

The font also covers Syriac, Lycian, Carian, Lydian, Cypriot, Imperial Aramaic, Meroitic Cursive, Kharoshthi, Inscriptional Parthian and Pahlavi, Old Turkic and Brahmi.

**Avoid Linear B** (U+10000–100FF). It is not in any of the Segoe fonts. On the checked machine only `SansSerifCollection.ttf` covered it, which has not been tested with axan.

The icon picker in axan offers two named sets, **Segoe Fluent Icons** and **Letters and digits**. Any other character is picked by typing its code point into the picker's search, and is stored as the character itself, exactly as described above.

### Choosing glyphs for a list of repos

1. **Match by meaning first.** Pick from the repo's language (🐍 Python, 🦀 Rust), domain (`bank` payments, `shield` auth, `cloud` infra, `globe` web front end, `robot` an agent), or what you do there (`builtin:build`, `builtin:test`, `builtin:logs`).
2. **Make siblings distinct.** Rows under the same parent or section should differ in shape at a glance, not just in tint.
3. **Keep one family per section.** For example, Fluent glyphs for the work section and runes for personal tools, so each section reads as a unit.
4. **Use emoji sparingly.** They are the only colour icons, so they stand out. Save them for the rows you want to find first.
5. **Use the tint for grouping.** Give a whole section one `color` and use different shapes inside it. Tints do nothing on colour emoji.
6. **Fall back to a letter.** When nothing fits, the repo's initial (`"B"`) is always legible.
7. **Use `builtin:` for files meant to travel**, if the same tree may be used on Linux later.

## 6. Cross-platform note

`builtin:NAME` is the only icon form the Windows and Linux builds share. The Linux build does not import `startup-sessions.toml` yet: it keeps its tree per profile and fails cleanly on this file. Glyph names, emoji, historic characters and image paths are Windows-only. See [The portable TOML](../README.md#the-portable-toml) and [Icons](../README.md#icons) in the root README.

## 7. Worked example

Two labelled sections, eight sessions, one level of nesting. It parses with Python's `tomllib`, a strict TOML 1.0 parser like toml++.

```toml
[meta]                                                    # optional version block
axan-toml-version = 2                                     # 2 because the file has separators
exported-by       = "hand-written"                        # provenance only; not read on import

# ---- Section 1: work repos ------------------------------------------------
[[startup-sessions]]                                      # a labelled separator = section header
id        = "aafbc58d-628f-428a-8970-88d92d74c14d"        # unique row id (uuid4)
parent-id = ""                                            # root level
kind      = "separator"                                   # spawns nothing
name      = "Contoso"                                     # header label; makes it collapsible

[[startup-sessions]]                                      # session: an agent working on the API
id           = "778d428f-964d-4338-83b6-18157bca226e"     # referenced by its child below
parent-id    = ""                                         # root level, under the Contoso header
profile      = "61c54bbd-c2c6-5271-96e7-009a87ff44bf"     # stock Windows PowerShell GUID
profile-name = "Windows PowerShell"                       # name fallback if the GUID is missing
name         = "billing-api"                              # literal label: rows with a command get no live cwd
directory    = '~\src\billing-api'                        # literal string, so backslashes stay as typed
command      = "claude"                                   # runs as: powershell -NoExit -Command "claude"
icon         = "robot"                                    # Segoe Fluent Icons glyph name
color        = "blue"                                     # palette tint for the section
color-target = "icon"                                     # tint the icon only, keep the label plain
hoist        = ["blocked", "error"]                       # list it in the hoisted group only for these

[[startup-sessions]]                                      # session nested under billing-api
id        = "43b0dca1-c036-4330-aa0e-b4910f2a7e40"        # unique row id
parent-id = "778d428f-964d-4338-83b6-18157bca226e"        # parent comes earlier in the file
profile   = "0caa0dad-35be-5f56-a8ff-afceeeaa6101"        # stock Command Prompt GUID
name      = "$repo $branch"                               # plain cmd shell, so live git labels work
directory = '~\src\billing-api'                           # same checkout as its parent
command   = ""                                            # just the shell
icon      = "builtin:test"                                # portable builtin name (lowercase)
color     = "blue"                                        # same tint as its parent

[[startup-sessions]]                                      # session: the web front end
id        = "008ab6da-dd5f-4deb-a6fd-ea5253ec4f44"        # unique row id
parent-id = ""                                            # root level
name      = "storefront"                                  # literal label
directory = '~\src\storefront'                            # starting directory
command   = "npm run dev"                                 # no double quotes inside commands
icon      = "\U0001F310"                                  # globe with meridians emoji, escaped
                                                          # (no profile key: uses the default profile)

[[startup-sessions]]                                      # session: infrastructure repo in WSL
id           = "ce83343c-423d-4b36-ab32-29e760cdbd47"     # unique row id
parent-id    = ""                                         # root level
profile      = "00000000-0000-0000-0000-000000000000"     # placeholder GUID: resolve by name instead
profile-name = "Ubuntu"                                   # exact WSL profile display name
name         = "$repo"                                    # plain WSL shell, so $repo resolves
directory    = '\\wsl.localhost\Ubuntu\home\me\infra'     # a WSL path, written as a literal string
icon         = "cloud"                                    # Segoe Fluent Icons glyph name
color        = "#2E8B57"                                  # a literal hex tint instead of a palette name

[[startup-sessions]]                                      # session: data pipeline, a hieroglyph icon
id        = "126ecb85-fb87-4c16-983b-d9f332d1d77f"        # unique row id
parent-id = ""                                            # root level
name      = "ledger-etl"                                  # literal label
directory = '~\src\ledger-etl'                            # starting directory
icon      = "\U000131A3"                                  # U+131A3 scarab, Segoe UI Historic
color     = "orange"                                      # takes the tint: the font is monochrome

# ---- Section 2: tools, starts collapsed ----------------------------------
[[startup-sessions]]                                      # second section header
id        = "3ea4aac9-6286-443b-80ea-0f4f23475d3e"        # unique row id
parent-id = ""                                            # root level
kind      = "separator"                                   # spawns nothing
name      = "Tools"                                       # header label
collapsed = true                                          # section starts folded

[[startup-sessions]]                                      # session: dotfiles, a rune icon
id        = "d95d2574-f21d-4710-b4b4-18f926b64633"        # unique row id
parent-id = ""                                            # root level, inside the Tools section
name      = "dotfiles"                                    # literal label
directory = '~\dotfiles'                                  # starting directory
icon      = "\u16A0"                                      # U+16A0 runic fehu (BMP: 4-digit escape)
color     = "purple"                                      # one tint for the Tools section
hoist     = "never"                                       # never show in the hoisted group

[[startup-sessions]]                                      # session: release scripts
id        = "fa997a04-0ae1-43fb-9b6b-9002c4e9342f"        # unique row id
parent-id = ""                                            # root level
name      = "release"                                     # literal label
directory = '~\src\release-tools'                         # starting directory
icon      = "builtin:package"                             # portable builtin name
color     = "purple"                                      # same tint as the section

[[startup-sessions]]                                      # session: scratch shell at home
id        = "b4384f41-d7ee-41f6-9c8d-82637904ace0"        # unique row id
parent-id = ""                                            # root level
name      = "$dir"                                        # follows wherever you cd
directory = "~"                                           # %USERPROFILE%
icon      = "S"                                           # a single letter always renders
```

## 8. Checking the result

Before importing, validate the TOML syntax:

```
python -I -c "import tomllib,sys; tomllib.load(open(sys.argv[1],'rb'))" startup-sessions.toml
```

Then grep the Segoe names you used against `AxanSegoeFluentGlyphs.h`, because a wrong name is never reported.

After **Import from TOML…**:

1. **Check the list.** The Startup sessions list should show every row in file order, nested as intended, with the right icon and `name · folder`. A row with no icon at all means its icon string did not resolve. A hollow box in place of a glyph means no installed font has that character.
2. **Save, then relaunch axan** and look at the sidebar. Headers show their label and a chevron; the Tools section should start collapsed.
3. **Read the log** at `%LOCALAPPDATA%\Packages\sh.axan.Axan_<publisher-id>\LocalState\logs\axan.log`. Import lines come from the `StartupSessionsViewModel` component; launch lines come from `AxanSessionTree`. A clean import logs `import: replaced the startup tree from TOML` with the row and separator counts. Search for these warnings:

```
import: TOML parse failed
import: file declares a newer axan-toml-version
import: no [[startup-sessions]] array
import: file yielded no entries
import: unrecognized row kind
import: unknown builtin: icon
import: unknown hoist state name(s) ignored
import: profile GUID not found; re-matched by name        (informational: the fallback worked)
import: entry references a profile this machine doesn't have
LoadStartupTree: parentId not seen before its child
LoadStartupTree: entry names a separator as its parent
LoadStartupTree: unknown shell family; command runs without a survival wrap
```

4. **Compare against the export.** After Save, axan rewrites `LocalState\profiles\startup-sessions.toml` from what it stored. Comparing that file with yours shows exactly what was kept, normalized and dropped. Icons that denote one of the eleven builtin glyphs come back as `builtin:NAME`, even when written as a glyph name such as `command-prompt`. All other icons appear as written.
