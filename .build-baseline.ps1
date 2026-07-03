# App-only build of the axan fork (axan/windows) — WindowsTerminal.vcxproj + its
# dependency graph, WITHOUT the packaging/deploy step. Faster loop for C++ iteration;
# use .deploy-cascadia.ps1 when you need a runnable registered package.
# (Originally the UNMODIFIED-WT baseline-verification script; baseline is verified.)
# Builds with WindowsTerminalBranding=Release to stay consistent with the deploy
# script — otherwise stale Dev-branding objects would mismatch the Release manifest.
# -Rebuild: pass on the first build after a branding/preprocessor-define change.
# Sets up the VS dev env in PS 5.1 (OpenConsole.psm1 needs PS7), then mirrors
# Invoke-OpenConsoleBuild: nuget restore -> msbuild Debug|x64.
param([switch]$Rebuild)

# ROOT CAUSE (confirmed): WT already pins PreferredToolArchitecture=x64
# (src/common.build.pre.props) and enables MultiProcessorCompilation, so on this
# 32-core box TerminalSettingsModel's huge C++/WinRT PCH fans out to ~32 cl.exe,
# each reserving multi-GB -> commit exhaustion -> C1076 / C3859 ("failed to create
# virtual memory for PCH"). FIX: cap the per-project /MP count via CL_MPCount (the
# critical knob; /m alone can't help since the blow-up is within one project).
# Build ONLY the app target (WindowsTerminal.exe + its dependency graph), not the
# whole ~80-project solution (skips conhost-standalone, the MSIX wapproj, the TAEF
# tests, the WPF control, samples). CL_MPCount=8 / m:6 keeps peak commit well under
# this box's 56 GB budget and is faster overall; scale to your own RAM.
$root = Join-Path $PSScriptRoot 'windows'
$wrap = Join-Path $PSScriptRoot '.build-baseline.log'
$mlog = Join-Path $PSScriptRoot '.build-msbuild.log'
function Note($m) { Add-Content -Path $wrap -Value $m -Encoding utf8; Write-Host $m }

Set-Content -Path $wrap -Value "=== baseline build start (unmodified WT v1.24.11321.0, x64 host compiler) ===" -Encoding utf8
try {
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
  Set-Location $root
  Note ("msbuild: " + (Get-Command msbuild).Source)

  Note "--- nuget restore (no-op if already restored) ---"
  & .\dep\nuget\nuget.exe restore .\OpenConsole.sln -Verbosity quiet
  & .\dep\nuget\nuget.exe restore .\dep\nuget\packages.config -Verbosity quiet
  Note ("nuget restore exit: " + $LASTEXITCODE)

  # Build the app project file DIRECTLY (not `sln /t:WindowsTerminal`, which makes
  # MSBuild run a target by that name on every project). A .vcxproj build pulls in
  # its ProjectReference deps automatically. SolutionDir is passed with forward
  # slashes to dodge the trailing-backslash native-arg quoting trap.
  $soldir = ($root -replace '\\','/') + '/'

  # -Rebuild: Clean as its own pass, then /t:Build. NOT /t:Rebuild — that races WT's
  # MIDL codegen under parallel build (ITerminalHandoff.h C1083). See .deploy-cascadia.ps1.
  if ($Rebuild) {
    Note "--- msbuild WindowsTerminal.vcxproj /t:Clean (force full branding recompile) ---"
    & msbuild .\src\cascadia\WindowsTerminal\WindowsTerminal.vcxproj `
        /t:Clean `
        /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=$soldir" `
        /p:WindowsTerminalBranding=Release /v:minimal
    Note ("=== clean EXIT $LASTEXITCODE ===")
  }

  Note "--- msbuild WindowsTerminal.vcxproj Debug|x64 branding=Release /t:Build (CL_MPCount=8, /m:6) ---"
  & msbuild .\src\cascadia\WindowsTerminal\WindowsTerminal.vcxproj `
      /t:Build `
      /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=$soldir" `
      /p:WindowsTerminalBranding=Release `
      /m:6 /p:CL_MPCount=8 /v:minimal /clp:Summary `
      "/flp:logfile=$mlog;verbosity=normal;summary"
  $code = $LASTEXITCODE
  Note ("=== msbuild EXIT $code ===")
  if ($code -eq 0) { Note "RESULT: BASELINE BUILD SUCCEEDED" } else { Note "RESULT: BASELINE BUILD FAILED ($code)" }
} catch {
  Note ("=== WRAPPER ERROR: $_ ===")
  Note "RESULT: BASELINE BUILD FAILED (setup)"
}