# Build + deploy the CascadiaPackage (MSIX) for the axan fork of Windows Terminal.
# This is path A from the recovery: the project-sanctioned way to RUN a dev build.
# windows/doc/building.md — you cannot launch the loose app exe directly; you must deploy the
# packaged app. Post-M1 this registers axan (identity sh.axan.Axan), built with
# WindowsTerminalBranding=Release (clean single identity, real icons, no dev-test
# update hack; Debug *configuration* is retained independently for debugging).
#
# Mirrors .build-baseline.ps1's env setup + memory throttle (CL_MPCount=8 / m:6 to
# avoid TerminalSettingsModel's PCH commit blow-up), but targets the .wapproj, which
# produces the loose AppX layout + CascadiaPackage.build.appxrecipe. We then deploy
# that recipe via DeployAppRecipe.exe — the same thing VS does on F5.
#
# -Rebuild: pass on the FIRST build after any WindowsTerminalBranding change (or any
# other preprocessor-define change). The branding token (WT_BRANDING_RELEASE) is a
# compile define, so MSBuild's timestamp up-to-date check won't recompile on its own;
# stale Dev-branding objects would otherwise mismatch the Release manifest's CLSIDs.
param([switch]$Rebuild, [switch]$Launch)

# Refuse to run from inside axan. The kill step below force-stops every axan.exe, and if
# this script's own console is hosted by axan (a Claude Code session or a plain shell in an
# axan tab), that kill takes the script down with it: the build never starts and the
# driving session dies mid-turn. Happened 2026-09-17. Walk the parent chain and bail early.
#
# One Win32_Process query, then the walk runs over a PID map. A visited set and a 64-hop
# cap bound it: ParentProcessId is never cleared when a parent exits, so PID reuse can
# form a cycle. A "parent" created after its child is such a reused PID, not a real
# ancestor, so the walk stops there instead of following an unrelated process.
function Find-AxanAncestor([int]$StartPid) {
  $procs = @{}
  foreach ($p in Get-CimInstance Win32_Process -Property ProcessId, ParentProcessId, Name, CreationDate) {
    $procs[[int]$p.ProcessId] = $p
  }
  $visited = @{}
  $id = $StartPid
  $child = $null
  for ($hop = 0; $hop -lt 64; $hop++) {
    if (-not $id -or $visited.ContainsKey($id) -or -not $procs.ContainsKey($id)) { break }
    $p = $procs[$id]
    if ($child -and $p.CreationDate -and $child.CreationDate -and $p.CreationDate -gt $child.CreationDate) { break }
    if ($p.Name -match '^axan(-cli)?\.exe$') { return $p }
    $visited[$id] = $true
    $child = $p
    $id = [int]$p.ParentProcessId
  }
  return $null
}

$ancestor = Find-AxanAncestor $PID
if ($ancestor) {
  Write-Error (("refusing to deploy: this shell is hosted by {0} (pid {1}). Deploying kills every axan " +
    "instance, including the one running this script. Run the deploy from stock Windows Terminal " +
    "or another non-axan console.") -f $ancestor.Name, $ancestor.ProcessId)
  exit 3
}

$root = Join-Path $PSScriptRoot 'windows'
$wrap = Join-Path $PSScriptRoot '.deploy-cascadia.log'
$mlog = Join-Path $PSScriptRoot '.deploy-msbuild.log'
function Note($m) { Add-Content -Path $wrap -Value $m -Encoding utf8; Write-Host $m }

Set-Content -Path $wrap -Value "=== axan CascadiaPackage build+deploy start ===" -Encoding utf8
try {
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
  Set-Location $root
  Note ("msbuild: " + (Get-Command msbuild).Source)

  # Stop any running axan instances first. A live instance locks the AppX payload
  # (axan.exe / TerminalApp.dll), which makes the layout copy and/or DeployAppRecipe
  # fail with exit 5 (access denied).
  #
  # Force-kill directly. A graceful CloseMainWindow() was tried and verified NOT to work:
  # axan ignores WM_CLOSE (blocked by its close-confirmation path), so it only added an 8s
  # wait before the same force-kill. Force-killing axan is harmless to OTHER terminals (no
  # orphaned-shell crashes in testing); the only way it takes the driving session down is
  # if that session is itself inside axan, which the ancestry guard at the top refuses.
  # Beyond that, the protection is the DETACHED relaunch below (axan is never a child of
  # the deploy console) plus running this whole script backgrounded. Note that with axan as
  # the daily driver, this step closes every open axan session, so the caller must have
  # confirmed nothing live is in them.
  $running = Get-Process axan,axan-cli -ErrorAction SilentlyContinue
  if ($running) {
    Note ("stopping running axan instances: " + ($running.Id -join ', '))
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
  }
  # Also stop anything else still EXECUTING FROM the layout (orphaned axan-console conpty
  # hosts, elevate-shim). Path-scoped so stock Windows Terminal's OpenConsole survives.
  # A mapped layout file fails the copy step with 0x800704C8 ERROR_USER_MAPPED_FILE.
  $layoutDir = Join-Path $root 'src\cascadia\CascadiaPackage\bin\x64\Debug\AppX'
  $holders = Get-Process | Where-Object { $_.Path -like "$layoutDir*" }
  if ($holders) {
    Note ("stopping processes running from the layout: " + (($holders | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ', '))
    $holders | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
  }

  Note "--- nuget restore (no-op if already restored) ---"
  & .\dep\nuget\nuget.exe restore .\OpenConsole.sln -Verbosity quiet
  & .\dep\nuget\nuget.exe restore .\dep\nuget\packages.config -Verbosity quiet
  Note ("nuget restore exit: " + $LASTEXITCODE)

  # Build the packaging project DIRECTLY. It pulls in the app graph (already built,
  # so this is mostly the packaging steps: PRI merge, loose-layout staging, recipe
  # gen). SolutionDir passed with forward slashes to dodge the trailing-backslash
  # native-arg quoting trap (same as the baseline script).
  $soldir = ($root -replace '\\','/') + '/'

  # -Rebuild: do Clean as its OWN msbuild pass, then a normal /t:Build. NOT /t:Rebuild:
  # Rebuild cleans+builds in one parallel pass and races WT's MIDL codegen (the
  # consuming .cpp compiles before ITerminalHandoff.h etc. are regenerated -> C1083).
  # A from-clean /t:Build is dependency-ordered correctly (it's what the cold build was).
  if ($Rebuild) {
    Note "--- msbuild CascadiaPackage.wapproj /t:Clean (force full branding recompile) ---"
    & msbuild .\src\cascadia\CascadiaPackage\CascadiaPackage.wapproj `
        /t:Clean `
        /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=$soldir" `
        /p:WindowsTerminalBranding=Release /v:minimal
    Note ("=== clean EXIT $LASTEXITCODE ===")
  }

  Note "--- msbuild CascadiaPackage.wapproj Debug|x64 branding=Release /t:Build (CL_MPCount=8, /m:6) ---"
  & msbuild .\src\cascadia\CascadiaPackage\CascadiaPackage.wapproj `
      /t:Build `
      /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=$soldir" `
      /p:WindowsTerminalBranding=Release `
      /p:AppxBundle=Never /p:AppxPackageSigningEnabled=false `
      /m:6 /p:CL_MPCount=8 /v:minimal /clp:Summary `
      "/flp:logfile=$mlog;verbosity=normal;summary"
  $code = $LASTEXITCODE
  Note ("=== msbuild EXIT $code ===")
  if ($code -ne 0) { Note "RESULT: PACKAGE BUILD FAILED ($code)"; exit $code }

  # Locate the appxrecipe the wapproj just produced.
  $recipe = Get-ChildItem -Path .\src\cascadia\CascadiaPackage\bin -Recurse -Filter '*.build.appxrecipe' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (-not $recipe) { Note "RESULT: NO APPXRECIPE PRODUCED (cannot deploy)"; exit 1 }
  Note ("appxrecipe: " + $recipe.FullName)

  # Deploy exactly as VS F5 does: register the loose layout from the recipe.
  $deploy = Join-Path $vs 'Common7\IDE\DeployAppRecipe.exe'
  if (-not (Test-Path $deploy)) { Note "RESULT: DeployAppRecipe.exe not found at $deploy"; exit 1 }
  # The layout copy can transiently fail with 0x800704C8 ERROR_USER_MAPPED_FILE:
  # something (typically the shell, briefly) still has the just-killed registered
  # package's resources.pri mapped. Observed twice on 2026-07-02, and an immediate
  # manual rerun succeeded both times — so retry once after a short pause before
  # declaring failure.
  # Issue #43 workaround. Registration of the SAME full package name can be refused with
  # 0x80070020 (the package's activation store on the system side is stuck; only a reboot
  # clears it), but a NEW full name registers fine. The full name includes the version, so
  # bumping the 4th field in the LAYOUT manifest (never the source manifest) sidesteps it.
  # Verified 2026-11-06: 0.1.2.0 refused, 0.1.2.1 registered on the first try, no reboot.
  # The bump only matters on that fallback path: DeployAppRecipe UNREGISTERS the current
  # package and recreates the layout from the recipe before registering (observed
  # 2026-11-07: 0.1.2.1 registered, plain deploy of 0.1.2.0 went through on attempt 1 with
  # no downgrade refusal), so there is no pre-bump before it — one would be overwritten
  # anyway. The fallback's bump is a full four-part [version] compare: a registered build
  # newer in Major/Minor/Build (deployed from another branch) is only beaten by taking ITS
  # Major.Minor.Build, not by bumping the layout's own revision.
  $layoutManifest = Join-Path $root 'src\cascadia\CascadiaPackage\bin\x64\Debug\AppX\AppxManifest.xml'
  function Get-LayoutVersion { [version](([xml](Get-Content $layoutManifest -Raw)).Package.Identity.Version) }
  # A version strictly greater than both inputs: the higher of the two, revision + 1.
  # $Registered may be $null (nothing registered). AppX version fields are 16-bit.
  function Get-FreshVersion([version]$Layout, [version]$Registered) {
    $base = $Layout
    if ($Registered -and $Registered -ge $Layout) { $base = $Registered }
    $rev = [Math]::Max($base.Revision, 0) + 1
    if ($rev -gt 65535) { throw "cannot raise version $base above itself: the revision field is already 65535" }
    [version]('{0}.{1}.{2}.{3}' -f $base.Major, $base.Minor, $base.Build, $rev)
  }
  # Rewrite Version= inside the <Identity> element only (attribute order independent), then
  # re-parse and confirm the manifest really carries the new version. Fails loudly instead
  # of reporting a change that did not happen. Preserves a UTF-8 BOM if the file had one.
  function Set-LayoutVersion([version]$New, [string]$Why) {
    $bytes = [IO.File]::ReadAllBytes($layoutManifest)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    $raw = [IO.File]::ReadAllText($layoutManifest)
    $idRx = [regex]'<Identity\b[^>]*>'
    $ids = $idRx.Matches($raw)
    if ($ids.Count -ne 1) { throw "layout manifest ${layoutManifest}: expected exactly one <Identity> element, found $($ids.Count); version NOT changed" }
    $verRx = [regex]'(\sVersion\s*=\s*")[^"]*(")'
    if (-not $verRx.IsMatch($ids[0].Value)) { throw "layout manifest ${layoutManifest}: <Identity> has no Version attribute; version NOT changed" }
    $newId = $verRx.Replace($ids[0].Value, ('${1}' + $New.ToString() + '${2}'), 1)
    $raw = $raw.Substring(0, $ids[0].Index) + $newId + $raw.Substring($ids[0].Index + $ids[0].Length)
    [IO.File]::WriteAllText($layoutManifest, $raw, (New-Object Text.UTF8Encoding($hasBom)))
    $actual = Get-LayoutVersion
    if ($actual -ne $New) { throw "layout manifest ${layoutManifest}: wrote version $New but it reads back as $actual" }
    Note "layout manifest version set to $New ($Why; the source manifest is untouched)"
    $New
  }
  $registered = Get-AppxPackage -Name 'sh.axan.Axan' -ErrorAction SilentlyContinue
  Note ("registered before deploy: " + $(if ($registered) { $registered.PackageFullName } else { '<none>' }))

  $dcode = 1
  $deployOut = ''
  foreach ($attempt in 1..2) {
    Note "--- DeployAppRecipe.exe (register sh.axan.Axan) [attempt $attempt] ---"
    # Tee to the log and the console live; -Variable keeps a copy for the diagnostics below.
    & $deploy $recipe.FullName | Tee-Object -FilePath $wrap -Append | Tee-Object -Variable out | Out-Host
    $dcode = $LASTEXITCODE
    $deployOut += ($out | Out-String)
    Note ("=== deploy EXIT $dcode (attempt $attempt) ===")
    if ($dcode -eq 0) { break }
    if ($attempt -lt 2) { Start-Sleep -Seconds 5 }
  }
  # Gate success on the deploy's OWN exit code. Get-AppxPackage alone lies here: a
  # FAILED redeploy leaves the PREVIOUS registration in place, which used to make this
  # script report "PACKAGE DEPLOYED" over a stale build (seen live: layout copy failed
  # with ERROR_USER_MAPPED_FILE, exit 5, yet the old June package still answered).
  if ($dcode -ne 0) {
    # Issue #43: DEP0700 0x80073CF6 with inner 0x80070020 (sharing violation while
    # registering the activatableClass collector) is NOT a running instance. Nothing
    # holds the layout; the package's activation store on the system side is stuck,
    # the OLD registration is gone too, and only a reboot has cleared it (elevation
    # does not). Say so instead of the generic "still active" line, which is wrong here.
    # Only DeployAppRecipe's own output is inspected, never the whole log: this script's
    # own notes mention "downgrade" and must not be mistaken for the deploy's error.
    # Downgrade: 0x80073D06 ERROR_INSTALL_PACKAGE_DOWNGRADE ("a higher version ... is
    # already installed"). 0x80073CFB ERROR_PACKAGE_ALREADY_EXISTS is a same-name refusal.
    $downgradeRx = '0x80073D06|downgrade|higher version'
    $nowReg = Get-AppxPackage -Name 'sh.axan.Axan' -ErrorAction SilentlyContinue
    $nowRegText = if ($nowReg) { "registered now: $($nowReg.Version)" } else { 'NO axan build is registered now' }
    if ($deployOut -match '0x80070020') {
      $nowRegVersion = if ($nowReg) { [version]$nowReg.Version } else { $null }
      $new = Set-LayoutVersion (Get-FreshVersion (Get-LayoutVersion) $nowRegVersion) '#43 workaround: fresh full name after 0x80070020'
      Note "--- 0x80070020 (issue #43): Add-AppxPackage -Register under the fresh full name $new ---"
      try {
        Add-AppxPackage -Register $layoutManifest -ForceApplicationShutdown -ErrorAction Stop
        $dcode = 0
      } catch {
        $msg = $_.Exception.Message -replace '\s+', ' '
        $hr = '0x{0:X8}' -f $_.Exception.HResult
        Note ("fallback register failed (HRESULT $hr): " + $msg)
        $after = Get-AppxPackage -Name 'sh.axan.Axan' -ErrorAction SilentlyContinue
        $afterText = if ($after) { "registered now: $($after.Version)" } else { 'the package is now UNREGISTERED' }
        if ("$hr $msg" -match $downgradeRx) {
          Note "RESULT: DEPLOY FAILED ($dcode) - the fallback registration of $new was refused as a DOWNGRADE ($afterText). A newer sh.axan.Axan is registered than this layout; deploy from that build's branch or remove it (Remove-AppxPackage) first."
        } elseif ("$hr $msg" -match '0x80070020') {
          Note "RESULT: DEPLOY FAILED ($dcode) - registration refused with 0x80070020 even under a fresh version (issue #43); $afterText. Reboot, then rerun this script. Before rebooting, from an elevated shell, 'handle.exe -a sh.axan.Axan' would identify the holder."
        } else {
          Note "RESULT: DEPLOY FAILED ($dcode) - the fallback registration of $new failed with $hr (see the message above, not 0x80070020); $afterText."
        }
        exit $dcode
      }
    } elseif ($deployOut -match $downgradeRx) {
      Note "RESULT: DEPLOY FAILED ($dcode) - refused as a DOWNGRADE: layout $(Get-LayoutVersion) vs $nowRegText. The registered build is newer than this checkout's; deploy from that build's branch or remove it (Remove-AppxPackage) first."
      exit $dcode
    } elseif ($deployOut -match '0x80073CFB') {
      Note "RESULT: DEPLOY FAILED ($dcode) - 0x80073CFB: a package with this exact full name ($(Get-LayoutVersion)) is already installed and reinstall was blocked; $nowRegText"
      exit $dcode
    } elseif ($nowReg) {
      Note "RESULT: DEPLOY FAILED ($dcode) - the previously registered build is still active ($nowRegText)"
      exit $dcode
    } else {
      Note "RESULT: DEPLOY FAILED ($dcode) - $nowRegText; see the DeployAppRecipe output above"
      exit $dcode
    }
  }

  $pkg = Get-AppxPackage -Name 'sh.axan.Axan*'
  if ($pkg) {
    Note ("REGISTERED: " + $pkg.PackageFullName)
    Note ("PackageFamilyName: " + $pkg.PackageFamilyName)
    Note ("InstallLocation: " + $pkg.InstallLocation)
    Note "RESULT: PACKAGE DEPLOYED"

    # -Launch: start axan DETACHED via the shell, never as a child of the console that
    # ran this deploy. A child launch shares this process's console; if axan's startup
    # perturbs that console the driving shell can be redirected and crash (see the
    # graceful-close note above). shell:AppsFolder hands axan its own process + window,
    # fully decoupled. Invoke this whole script in the background too, so the deploy's
    # own console is never the one in the blast radius.
    if ($Launch) {
      $appid = "$($pkg.PackageFamilyName)!App"
      Note ("--- launching axan detached: shell:AppsFolder\$appid ---")
      Start-Process "shell:AppsFolder\$appid"
      Note "RESULT: PACKAGE DEPLOYED + LAUNCHED (detached)"
    }
  } else {
    Note "RESULT: DEPLOY RAN BUT sh.axan.Axan NOT REGISTERED"
  }
} catch {
  Note ("=== WRAPPER ERROR: $_ ===")
  Note "RESULT: PACKAGE BUILD/DEPLOY FAILED (setup)"
}
