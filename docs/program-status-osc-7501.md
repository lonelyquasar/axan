# Program Status Protocol (OSC 7501)

This is for anyone writing something that runs inside an axan pane and wants its row in the sidebar to show what it is doing: a shell integration, a build script, an agent runner, a coding agent. It covers the escape sequence, what axan accepts and rejects, how each report shows in the sidebar, and how to debug it. Everything here comes from the Windows code on the branch it was written against. The Linux build does not implement it yet.

The receiver is `windows/src/terminal/adapter/ProgramStatus.hpp` (parsing and validation) and `windows/src/cascadia/TerminalCore/TerminalProgramStatus.cpp` (the per-terminal records). How records become a sidebar state is `windows/src/cascadia/TerminalApp/AxanSessionState.h`.

**Claude Code already speaks this protocol.** It sends its own reports with `app=claude-code`: `working` during a turn, `blocked` with `kind=permission` or `kind=question` when it waits for you, `done` with a message when a turn ends, and `idle` at its prompt. You do not need a hook for it.

## 1. The two sequences

A program first asks whether the terminal understands the protocol, then sends reports. `ESC` is byte `0x1B` and `BEL` is `0x07`.

```
ESC ] 7501 ; ?  ESC \                     query
ESC ] 7501 ; key=value:key=value...  ESC \   report
```

Either sequence may end with `BEL` instead of `ESC \`.

### The handshake

axan answers the query immediately, on the program's input:

```
ESC ] 7501;?:states=idle,working,done,blocked,error:kinds=permission,question,auth ESC \
```

If the program has switched the terminal to 8-bit controls, the reply uses `0x9D` and `0x9C` instead of `ESC ]` and `ESC \`. No reply means no support, and a well-behaved program then sends no reports. Whitespace around the `?` is tolerated.

The handshake matters for programs that might run in other terminals, which could print an unknown sequence. Inside axan the feature is always on: there is no setting to turn it off, so a report sent without asking first is still accepted. A script that only ever runs in axan can skip the query.

## 2. Report grammar

A report is a list of `key=value` pairs separated by `:`. Spaces and tabs around a key or value are trimmed. Keys are lowercase ASCII letters. Values may contain only `A-Z a-z 0-9 _ . , + / = -`, which is why free text travels as base64.

| Key | Required | Values | Meaning |
|---|---|---|---|
| `state` | yes | `idle`, `working`, `done`, `blocked`, `error`, `clear` | The record's state. `clear` removes records instead of setting one (section 3) |
| `id` | no | a path, see below | Which record this report is about. Absent means the root record |
| `app` | no | 1–32 bytes of `A-Z a-z 0-9 _ . + -` | Name of the reporting program. A child record without one inherits its nearest ancestor's. Stored but not displayed |
| `kind` | no | `permission`, `question`, `auth` | Why a `blocked` program is waiting. Ignored for other states |
| `progress` | no | integer `0`–`100` | Percent done, for `working` or `blocked`. Stored but not displayed |
| `msg` | no | base64 of UTF-8 | The line of text the row shows, such as "Tests failed" or "Allow Bash: rm -rf build?" |
| `title` | no | base64 of UTF-8 | A short name for the task or agent. Shown where `msg` would be when there is no `msg`; a root record's `title` is also the agent name (section 5) |

**Ids.** An id is one to eight segments joined by `/`, such as `build` or `agent/sub1`. Each segment is 1–32 characters of `A-Z a-z 0-9 _ . + -`, and the whole id is at most 128 bytes. Ids form a tree: `agent/sub1` is under `agent`. A program with one thing to report should leave `id` out and use the root record.

**Text fields.** `msg` and `title` use the standard base64 alphabet (`+` and `/`); `=` padding is optional but must be correct if present. The decoded bytes must be valid UTF-8 with no control characters: no C0 controls (which includes newline and tab), no DEL, no C1 controls. Bidirectional formatting characters (U+061C, U+200E, U+200F, U+202A–202E, U+2066–2069) are silently removed. An empty value means absent.

### Limits

| What | Limit |
|---|---|
| Whole sequence, `ESC ]` through the terminator | 4096 bytes (a payload after `7501;` of at most 4088 characters) |
| A key | 16 bytes |
| `app` | 32 bytes |
| `id` | 128 bytes, 8 segments, 32 bytes per segment |
| `msg` | 2732 base64 characters, 2048 bytes decoded |
| `title` | 256 base64 characters, 192 bytes decoded |
| Records per terminal | 256 |

### What is rejected

axan is strict about the parts that address and bound a report, and lenient about the rest.

**The whole report is discarded, and nothing changes,** when:
- the payload is over the sequence limit;
- a lowercase key is longer than 16 bytes;
- `app` is longer than 32 bytes;
- `state` is missing or not one of the six names;
- any `id` pair is invalid, even if a later `id` pair is valid. This includes an `id` value with a character outside the value alphabet and a bare `id` segment with no `=`, and it applies to `state=clear` too, so a malformed id can never fall back to "clear everything";
- `msg` or `title` is over its limit, is not valid base64, is not valid UTF-8, or decodes to a control character.

**Only the pair is skipped,** and the rest still applies, when:
- it has no `=` and is not a bare `id`, including an empty segment from a doubled or trailing `:`;
- its key is empty or contains anything but `a-z`;
- its value (other than `id`'s) contains a character outside the value alphabet.

**These are treated as absent:** an `app` with characters outside its alphabet; a `kind` that is not one of the three names or comes with a state other than `blocked`; a `progress` that is not an integer from 0 to 100, or comes with a state other than `working` or `blocked`.

Unknown keys are ignored. If a key repeats, the last value wins.

## 3. Records and their lifetime

Each terminal (each pane) keeps its own set of records, keyed by `id`.

- **A report replaces its record completely.** A key you leave out is cleared, not kept from the previous report. To change only the message, resend the state and every key you still want.
- **`state=clear` removes records.** With an `id`, it removes that record and everything under it: `id=a` removes `a` and `a/b`, but not `a-b` or `ab`. Without an `id`, it removes every record in the terminal.
- **Live records end with the prompt.** `working` and `blocked` records are dropped when the shell starts a new prompt (OSC 133;A, section 6), when the pane's process exits, or when output arrives after the connection closed. A crashed program therefore cannot leave a row stuck on "working". `done`, `error` and `idle` survive all three.
- **A full reset clears everything.** RIS (`ESC c`) removes every record and also ends any shell command marked as running. DECSTR soft reset and switching to or from the alternate screen leave records alone.
- **The 256th record evicts.** When a terminal already has 256 records and a report names a new id, the least recently updated record is removed first.
- **Revisions order everything.** Each accepted report stamps its record with a number that only increases. axan uses it to break ties and to remember what you have seen.

## 4. How a pane's records become a row

A sidebar row is one tab, and a tab can hold several panes. axan works out a state per pane and then shows the strongest state of the tab.

**Per pane, in priority order:**
1. **OSC 7501 records.** If the pane has any records, they decide the pane's state, and OSC 133 is ignored for that pane. Claude Code at its own prompt reports `idle` while the shell around it is technically still running `claude`, and idle is the truth there.
2. **OSC 133 command lifecycle.** A command between OSC 133;C and the next D or A is *running*; otherwise the pane is idle. A non-zero exit code is not an error state, so a failed `ls` doesn't flag the row.

**Per tab:** if the strongest state is idle and the tab's bell indicator is on, the row becomes *done, not yet seen* with the message `bell`. Windows Terminal clears that indicator when the tab gets focus, so `make; printf '\a'` works as a "finished" signal from a plain shell.

**Record state to row state:**

| Record | Row state |
|---|---|
| `blocked` | needs you (with its `kind`) |
| `error`, not yet seen | error |
| `done`, not yet seen | done-unseen |
| `working` | working |
| `error` or `done` already seen, or `idle` | idle |

**Strongest first:** needs you, error, done-unseen, working, running, idle. Among records with the same state, the root record wins, then the most recently written one. The winning record's `msg`, or its `title` if `msg` is empty, becomes the row's message. So a hierarchy such as `agent` and `agent/sub1` collapses into one row showing the most urgent thing happening.

### Seen

Only `done` and `error` can be unread. A `blocked` record stays lit for as long as the program keeps it `blocked`.

A done or error report counts as seen when you select its tab, when one of the tab's panes gets focus, or when the window is activated while that tab is the focused one. A report that arrives while its tab is focused in the active window is seen at once and never shows the unread dot. To see the dot in a test, report from a tab you aren't looking at: `sleep 5; tools/axan-status.sh done --msg Finished`, then switch tabs.

axan remembers, per pane, the highest revision you have seen. A new report gets a new revision, so it is unread again even if it repeats the previous text.

## 5. What the row shows

A row can show a dot on the corner of its icon, a second line under the label, and a glyph with a short word at its right edge (the "slot").

| Row state | Dot | Second line | Slot glyph (default) | Slot word |
|---|---|---|---|---|
| needs you | Full-strength dot in the theme's text colour, with a ring | `message · age`; with no message, the kind (`permission · 3m`) or `needs you` | none | `needs permission`, `needs answer`, `needs login`, or `needs you` without a kind |
| error, unseen | Red dot | `message · age`, or `error · age` | none | `error` |
| done-unseen | Half-strength dot | `message · age`, or `done · age`; `bell · age` for the bell | none | `done` |
| working | none | the message, with no age; the agent name if there is no message | lightning bolt | `working` |
| running (OSC 133) | none | a kept report (see below), else the agent name if a reporting program shares the tab | play | `running` |
| idle, with records | none | a kept report (see below), else the agent name | robot | `idle` |
| idle, no records | none | nothing | none | none |

- **Ages** read `<1m`, `5m`, `2h`, `3d`, counted from when the state began. The age is never trimmed; a long message is cut to fit and the full text is in the row's tooltip, with the state, kind and the local time it began.
- **Kept reports.** With `sidebar.statusAfterSeen` set to `keep`, the default, a seen done or error report stays on the second line, muted, with its age, while the row is idle or running. It goes away when the record is replaced or cleared, or the pane closes. With `clear`, a seen report simply leaves the row idle. Either way, a seen report no longer counts as needing you.
- **Agent name.** When any pane in the tab has records and nothing else owns the second line, the line shows the agent's name, muted. That is the root record's `title` if one was sent, else the pane's terminal title with any leading spinner or status symbol removed. Claude Code sets its terminal title from `claude --name`, so the row shows the name other agents use for it. Controlled by `sidebar.showAgentName`.
- **State word.** The slot word is shown when `sidebar.stateLabels` is on, the default. It is always part of the row's accessible name ("billing-api, needs answer").
- **Glyphs** come from `sidebar.stateGlyphs`, keyed `idle`, `working`, `blocked`, `done`, `error` and `running`, plus optional `permission`, `question` and `auth` that override `blocked` for that kind. Each value uses the node icon vocabulary; `""` means no glyph.
- **De-emphasis.** A row that has been idle, with no input or output, for `sidebar.idleAfterMinutes` (default 15) is dimmed and drawn smaller, per `sidebar.idleEffects`. The focused tab's row never is.
- **Hoisted group.** With `sidebar.needsYouGroup` on, a list above the tree repeats every row whose state is in `sidebar.hoistStates`, longest-waiting first. The default list is `blocked`, `error`, `done`. `blocked` matches any kind; `permission`, `question` and `auth` match only that kind; `done` means unseen, including the bell; `idle` matches only a row with records. A session can override the list with Default, Never or Custom under **Hoisted group**, on the Startup sessions page or in the sidebar's **Edit node** card; it is stored as the startup entry's `hoist` ([TOML guide](startup-sessions-toml.md#session-rows)). The group is titled "Needs you" while the list holds only attention states, and "Hoisted" otherwise.
- **Section headers.** A collapsed section header shows a count of the rows it hides and the most urgent state among them.

Nothing in the sidebar animates, except a long second line when `sidebar.stateLineMarquee` is on.

To see how each state looks without a program, right-click a session row and pick a state under **Preview state**. The row draws as if that report had arrived two minutes ago. It is display only: the hoisted group, section badges and idle handling keep using the real state. With `sidebar.hoistPreview` on, the hoisted group uses the previewed state instead, so you can try out `sidebar.hoistStates` and a session's own hoist setting. **Clear preview**, a new report from the session, or a real state change ends it, and it is never saved.

## 6. OSC 133 and the running state

axan injects shell integration into plain PowerShell, cmd and WSL shells at spawn. It does not inject it into a shell started with a command of its own: PowerShell with `-Command`, `-File` or `-EncodedCommand`, cmd with `/c`, WSL with `--`, `-e` or `--exec`.

| Shell | Emits |
|---|---|
| PowerShell | `133;D;<exit>` for the previous command, cwd (OSC 9;9), `133;A`, the user's prompt, `133;B`; and `133;C` when PSReadLine accepts a non-empty line |
| bash in WSL | `133;D;<exit>`, `133;A` and the cwd from `PROMPT_COMMAND`; `133;B` appended to `PS1`; `133;C` from a `DEBUG` trap. The 133 marks are skipped if a `DEBUG` trap already exists, so tools like bash-preexec keep it |
| cmd | cwd only, through `%PROMPT%`. No OSC 133, so a cmd row never shows *running* |

What each mark does in axan:
- `133;C` marks a command as running.
- `133;D` and `133;A` end it. `133;A` also drops every `working` and `blocked` OSC 7501 record in that terminal.
- `133;B` changes nothing.
- The running flag also ends, without a D mark, when the pane's process exits, when output arrives after the connection closed, and on RIS.

A shell integration you write yourself only needs to emit `133;C` before a command runs and `133;D` (or `133;A`) when it finishes. To report more than running or idle, use OSC 7501.

## 7. Emitting reports

Write the sequence to the terminal: the program's own stdout or console. A process whose stdout is piped or captured, such as a hook run by another program, does not reach the terminal by writing to stdout.

### bash, zsh, Git Bash, WSL

```bash
axan_status() {  # axan_status <state> [message] [kind]
  local p="state=$1:app=mybuild"
  [ -n "${3:-}" ] && p="$p:kind=$3"
  [ -n "${2:-}" ] && p="$p:msg=$(printf '%s' "$2" | base64 | tr -d '\n')"
  printf '\033]7501;%s\033\\' "$p"
}

axan_status working "Compiling"
make && axan_status done "Build finished" || axan_status error "Build failed"
```

`tr -d '\n'` matters: GNU `base64` wraps long output at 76 characters, and a newline inside the sequence is outside the value alphabet.

### PowerShell

```powershell
function Send-AxanStatus {
    param([string]$State, [string]$Msg, [string]$Kind, [string]$Id, [string]$App = 'mytool')
    $pairs = @("state=$State", "app=$App")
    if ($Id)   { $pairs += "id=$Id" }
    if ($Kind) { $pairs += "kind=$Kind" }
    if ($Msg)  { $pairs += 'msg=' + [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Msg)) }
    [Console]::Write("$([char]27)]7501;$($pairs -join ':')$([char]27)\")
}

Send-AxanStatus -State blocked -Kind question -Msg 'Which branch?'
```

Use `[Console]::Write`, not `Write-Output`, so the sequence isn't captured by the pipeline.

### cmd

cmd has no escape for `ESC`, so a batch file builds one first:

```bat
@echo off
for /f %%a in ('echo prompt $E^| cmd /q') do set "ESC=%%a"
<nul set /p "=%ESC%]7501;state=working:app=build%ESC%\"
```

A `msg` needs base64, which cmd cannot compute itself. Call PowerShell for that, or use the scripts below.

### Claude Code and other agents

Claude Code emits OSC 7501 itself; add nothing. For another agent CLI that doesn't, the usual place to report from is its hook mechanism, but hooks normally run with stdout captured by the agent, so `printf` from a hook goes to the agent and not to the terminal. A hook would have to open the terminal device directly (`/dev/tty` on Unix-like systems, `CONOUT$` on Windows), which only works if the hook process shares the agent's console. This has not been tested with axan. Prefer a wrapper that the agent's own output passes through, or a change to the agent itself.

### Test scripts

`tools/axan-status.sh` (bash, Git Bash, WSL) and `tools/axan-status.ps1` (PowerShell) send one report from the pane they run in, and take the same flags:

```
axan-status.sh <state> [--kind permission|question|auth] [--msg "text"] [--title "text"]
               [--progress N] [--id a/b] [--app name]
axan-status.sh --query     # send the handshake; the reply arrives on the shell's input
axan-status.sh --demo      # working, then blocked (question), then done, 3 seconds apart

tools/axan-status.sh blocked --kind permission --msg "Run rm -rf build?"
tools/axan-status.sh working --id agent/sub1 --msg "Sub-task"
tools/axan-status.sh clear --id agent        # removes agent and agent/sub1
tools/axan-status.sh clear                   # removes every record in this pane
```

Each script writes only the escape sequence to stdout, so `axan-status.sh done > file` captures exactly what the terminal would receive.

## 8. Settings

All are in `settings.json` at the root, next to `startupSessions`, and on the **Sidebar** page in the Settings **Sessions** group. `sidebar.needsYouGroup` and `sidebar.statusAfterSeen` are also in the right-click menu of empty sidebar space.

| Key | Default | Meaning |
|---|---|---|
| `sidebar.idleAfterMinutes` | `15` | Minutes idle, with no input or output, before a row is de-emphasised. `0` turns it off |
| `sidebar.idleEffects` | `["dim", "smaller"]` | Any of `dim`, `smaller`, `gray` (a coloured row turns gray), `collapse` (idle rows under an expanded section header or parent row are tucked away while active ones stay; a hollow dot with the hidden count shows in the chevron slot; clicking cycles idle-collapsed → fully collapsed → expanded, and a manual expand holds the node open until it is collapsed again). `[]` = no visible effect |
| `sidebar.needsYouGroup` | `false` | Show the hoisted group above the tree |
| `sidebar.hoistStates` | `["blocked", "error", "done"]` | States that put a row in the hoisted group: `idle`, `working`, `blocked`, `done`, `error`, `running`, `permission`, `question`, `auth` |
| `sidebar.hoistPreview` | `false` | A previewed row is hoisted by its previewed state instead of its real one (**Preview state** menu) |
| `sidebar.stateLineMarquee` | `false` | A second line too long for its row scrolls slowly back and forth instead of ending in "…", in the hoisted group and on the selected row. Never when Windows animation effects or `disableAnimations` turn animation off |
| `sidebar.statusAfterSeen` | `"keep"` | `"keep"` or `"clear"`: what a seen done or error report does |
| `sidebar.stateGlyphs` | robot, bolt, play for `idle`, `working`, `running`; none for the rest | Per-state slot glyphs |
| `sidebar.stateLabels` | `true` | Show the slot word |
| `sidebar.showAgentName` | `true` | Show the agent name as the muted second line |
| `sidebar.stateGlyphSize` | `12` | Slot glyph size in pixels, 8–48 |
| `sidebar.stateLineFontSize` | `11` | Font size of the second line and the slot word, 8–32 |
| `sidebar.labelFontSize` | `14` | Font size of a row's name, 8–32 |
| `sidebar.iconSize` | `16` | Size of a row's icon box in pixels, 8–48. The glyph is drawn at 3/4 of it |
| `sidebar.iconFontScale` | unset | Per-font-family icon scale, for example `{ "Segoe UI Historic": 1.5 }`. A family left out keeps its built-in scale. Values are clamped to 0.5–3 |
| `sidebar.iconFontWeight` | unset | Per-font-family icon weight, `"normal"`, `"semibold"` or `"bold"`, for example `{ "Segoe UI Historic": "bold" }`. A font with no bold face, such as Segoe UI Historic, gets a synthesised one. A family left out, or an unknown value, draws at normal |
| `sidebar.iconOverflow` | `false` | Draw a glyph larger than its icon box whole, centred on the box, instead of cutting it at the box edge. The row height is unchanged; the glyph may overlap the row's padding |
| `sidebar.idleLabelFontSize` | `12` | Font size of an idle row's name while the `smaller` idle effect is on, 8–32. This is an absolute size and is not clamped to the active row size |
| `sidebar.idleRowHeight` | `24` | Minimum height in pixels of an idle row while the `smaller` idle effect is on, 16–64. This is an absolute size and is not clamped to the active row size |

## 9. Troubleshooting

The log is `%LOCALAPPDATA%\Packages\sh.axan.Axan_<publisher-id>\LocalState\logs\axan.log`. Every level is written, including DEBUG. The receiver logs under the component `ProgramStatus`; the sidebar logs under `SessionState` and `TerminalPage`.

| Symptom | Look for |
|---|---|
| Nothing happens at all | No `ProgramStatus` line at your report's time means the sequence never reached axan's terminal. Check that you are writing to the terminal and not to a pipe, and that the pane is an axan pane |
| A report is ignored | `report discarded` with a `reason`: `sequence-too-long`, `key-too-long`, `invalid-id`, `bare-id`, `missing-state`, `unknown-state`, `app-too-long`, `text-too-long`, `bad-base64`, `bad-utf8`, `control-char` |
| A key seems to be ignored | `report accepted` with `skippedPairs` above 0: a pair was malformed (section 2) |
| The handshake gets no reply | `query answered` should appear. If it does, check that your program reads its input. The other outcome, `query not answered: feature off`, needs the feature switched off, which no setting in this build does |
| A working or blocked row goes idle by itself | `live records dropped` with `reason` `prompt-start` (the shell drew a prompt), `connection-closed` or `output-after-close` |
| Everything cleared at once | `records cleared: reset` (the program sent RIS) or `report accepted: clear` with `id="<all>"` |
| A record vanished among many | `record evicted: cap reached` |
| A row never shows *running* | `shell command started` should appear when a command runs. cmd never sends it; a PowerShell or WSL row started with a command has no shell integration; a bash with its own `DEBUG` trap has no `133;C` |
| *Running* never ends | `shell command ended: no D mark` shows the fallbacks. If neither that nor a D or A mark arrives, the shell isn't emitting them |
| A done row never loses its dot | `node acknowledged` should appear when you focus the tab |
| The row shows the wrong state | `node state changed` gives the resulting `state`, `kind` and `source` (`osc7501`, `osc133`, `bell`, `none`) |

Each accepted report logs `report accepted` at DEBUG with its id, state, app, kind and the lengths of `msg` and `title`. A state change also logs `record state changed` at INFO with `from` and `to`. Text content is never logged.
