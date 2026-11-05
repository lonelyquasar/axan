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
$ancestor = Get-CimInstance Win32_Process -Filter "ProcessId=$PID"
while ($ancestor) {
  if ($ancestor.Name -match '^axan(-cli)?\.exe$') {
    Write-Error ("refusing to deploy: this shell is hosted by {0} (pid {1}). Deploying kills every axan " +
      "instance, including the one running this script. Run the deploy from stock Windows Terminal " +
      "or another non-axan console.") -f $ancestor.Name, $ancestor.ProcessId
    exit 3
  }
  $ancestor = if ($ancestor.ParentProcessId) { Get-CimInstance Win32_Process -Filter "ProcessId=$($ancestor.ParentProcessId)" } else { $null }
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
  $dcode = 1
  foreach ($attempt in 1..2) {
    Note "--- DeployAppRecipe.exe (register sh.axan.Axan) [attempt $attempt] ---"
    & $deploy $recipe.FullName | Tee-Object -FilePath $wrap -Append
    $dcode = $LASTEXITCODE
    Note ("=== deploy EXIT $dcode (attempt $attempt) ===")
    if ($dcode -eq 0) { break }
    if ($attempt -lt 2) { Start-Sleep -Seconds 5 }
  }
  # Gate success on the deploy's OWN exit code. Get-AppxPackage alone lies here: a
  # FAILED redeploy leaves the PREVIOUS registration in place, which used to make this
  # script report "PACKAGE DEPLOYED" over a stale build (seen live: layout copy failed
  # with ERROR_USER_MAPPED_FILE, exit 5, yet the old June package still answered).
  if ($dcode -ne 0) { Note "RESULT: DEPLOY FAILED ($dcode) - the previously registered build is still active"; exit $dcode }

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
