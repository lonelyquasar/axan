# Release packaging build for the axan fork (axan/windows) — produces a SIGNED
# sideload .msix under .release/ for distribution via GitHub Release assets.
# Unlike .build-baseline.ps1 (Debug app-only loop) this builds the full
# CascadiaPackage.wapproj dependency graph at Release|x64, so expect a long build.
#
# Signing: uses a self-signed "CN=Lonely Quasar" code-signing cert from the
# CurrentUser\My store (machine-local; NOT in the repo). Pass -Thumbprint if the
# cert is reissued. The matching public .cer ships as a release asset so target
# machines can trust the package (import to LocalMachine\Trusted People, admin).
#
# Same environment notes as .build-baseline.ps1: VS dev shell via DevShell.dll,
# nuget restore first, CL_MPCount=8 / m:6 to keep the C++/WinRT PCH fan-out from
# exhausting commit on this 32-core box.
#
# Known flake: a COLD Release tree can lose WT's MIDL codegen race (C1083:
# ITerminalHandoff.h not found) even on a plain /t:Build, not just /t:Rebuild
# (observed 2026-06-10 building v0.1.0). The failed pass leaves the generated
# header behind, so simply rerunning this script completes the build.
param(
  [string]$Thumbprint = '7144348F84B8AEF5B66213715CF4B042018AF9BA',
  [switch]$Rebuild
)

$root = Join-Path $PSScriptRoot 'windows'
$outdir = Join-Path $PSScriptRoot '.release'
$wrap = Join-Path $PSScriptRoot '.build-release.log'
$mlog = Join-Path $PSScriptRoot '.build-release-msbuild.log'
function Note($m) { Add-Content -Path $wrap -Value $m -Encoding utf8; Write-Host $m }

Set-Content -Path $wrap -Value "=== release package build start (CascadiaPackage Release|x64, signed sideload) ===" -Encoding utf8
try {
  if (-not (Get-ChildItem "Cert:\CurrentUser\My\$Thumbprint" -ErrorAction SilentlyContinue)) {
    Note "RESULT: RELEASE BUILD FAILED (signing cert $Thumbprint not in CurrentUser\My)"
    exit 1
  }
  New-Item -ItemType Directory -Force $outdir | Out-Null

  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
  Set-Location $root
  Note ("msbuild: " + (Get-Command msbuild).Source)

  Note "--- nuget restore (no-op if already restored) ---"
  & .\dep\nuget\nuget.exe restore .\OpenConsole.sln -Verbosity quiet
  & .\dep\nuget\nuget.exe restore .\dep\nuget\packages.config -Verbosity quiet
  Note ("nuget restore exit: " + $LASTEXITCODE)

  # Forward slashes + trailing slash: dodge the trailing-backslash quoting trap.
  $soldir = ($root -replace '\\','/') + '/'
  $pkgdir = ($outdir -replace '\\','/') + '/'

  if ($Rebuild) {
    Note "--- msbuild CascadiaPackage.wapproj /t:Clean ---"
    & msbuild .\src\cascadia\CascadiaPackage\CascadiaPackage.wapproj `
        /t:Clean `
        /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$soldir" `
        /p:WindowsTerminalBranding=Release /v:minimal
    Note ("=== clean EXIT $LASTEXITCODE ===")
  }

  Note "--- msbuild CascadiaPackage.wapproj Release|x64 branding=Release sideload+signed ---"
  & msbuild .\src\cascadia\CascadiaPackage\CascadiaPackage.wapproj `
      /t:Build `
      /p:Configuration=Release /p:Platform=x64 "/p:SolutionDir=$soldir" `
      /p:WindowsTerminalBranding=Release `
      /p:UapAppxPackageBuildMode=SideloadOnly `
      /p:AppxBundle=Never `
      /p:GenerateAppxPackageOnBuild=true `
      /p:AppxPackageSigningEnabled=true `
      "/p:PackageCertificateThumbprint=$Thumbprint" `
      "/p:AppxPackageDir=$pkgdir" `
      /m:6 /p:CL_MPCount=8 /v:minimal /clp:Summary `
      "/flp:logfile=$mlog;verbosity=normal;summary"
  $code = $LASTEXITCODE
  Note ("=== msbuild EXIT $code ===")
  if ($code -eq 0) {
    Get-ChildItem -Recurse $outdir -Filter *.msix | ForEach-Object { Note ("artifact: " + $_.FullName) }
    Note "RESULT: RELEASE BUILD SUCCEEDED"
  } else {
    Note "RESULT: RELEASE BUILD FAILED ($code)"
  }
} catch {
  Note ("=== WRAPPER ERROR: $_ ===")
  Note "RESULT: RELEASE BUILD FAILED (setup)"
}
