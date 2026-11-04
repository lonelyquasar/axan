# Build and run the axan fork's (axan/windows) TAEF unit-test DLLs that cover the VT
# parser, the adapter, and TerminalCore - the layers most axan protocol work touches.
# Same env setup and memory throttle as .build-baseline.ps1 (CL_MPCount=8 / m:6, Debug|x64,
# WindowsTerminalBranding=Release so the shared static libs aren't rebuilt per branding).
#
#   .build-tests.ps1                      build all three, run all three
#   .build-tests.ps1 -Name '*ProgramStatus*'   run only matching tests (TAEF /name:)
#   .build-tests.ps1 -NoBuild             run the existing DLLs without building
#
# Logs: .build-tests.log (wrapper + TE output), .build-tests-msbuild.log (msbuild).
# TE.exe comes from the restored Microsoft.Taef NuGet package.
param([string]$Name = '', [switch]$NoBuild)

$root = Join-Path $PSScriptRoot 'windows'
$wrap = Join-Path $PSScriptRoot '.build-tests.log'
$mlog = Join-Path $PSScriptRoot '.build-tests-msbuild.log'
function Note($m) { Add-Content -Path $wrap -Value $m -Encoding utf8; Write-Host $m }

$projects = @(
  'src\terminal\parser\ut_parser\Parser.UnitTests.vcxproj',
  'src\terminal\adapter\ut_adapter\Adapter.UnitTests.vcxproj',
  'src\cascadia\UnitTests_TerminalCore\UnitTests.vcxproj'
)
$dlls = @(
  'bin\x64\Debug\ConParser.Unit.Tests.dll',
  'bin\x64\Debug\ConAdapter.Unit.Tests.dll',
  'bin\x64\Debug\UnitTests_TerminalCore\Terminal.Core.Unit.Tests.dll'
)

Set-Content -Path $wrap -Value "=== axan unit tests start ===" -Encoding utf8
try {
  $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
  Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
  Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
  Set-Location $root

  if (-not $NoBuild) {
    & .\dep\nuget\nuget.exe restore .\OpenConsole.sln -Verbosity quiet
    $soldir = ($root -replace '\\','/') + '/'
    Set-Content -Path $mlog -Value '' -Encoding utf8
    foreach ($proj in $projects) {
      Note "--- msbuild $proj Debug|x64 ---"
      & msbuild $proj /t:Build `
          /p:Configuration=Debug /p:Platform=x64 "/p:SolutionDir=$soldir" `
          /p:WindowsTerminalBranding=Release `
          /m:6 /p:CL_MPCount=8 /v:minimal /clp:Summary `
          "/flp:logfile=$mlog;verbosity=normal;summary;append"
      $code = $LASTEXITCODE
      Note ("=== msbuild EXIT $code ($proj) ===")
      if ($code -ne 0) { Note "RESULT: TEST BUILD FAILED ($proj)"; exit $code }
    }
  }

  $te = Get-ChildItem -Path .\packages -Recurse -Filter 'TE.exe' -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -like '*\x64\TE.exe' } | Select-Object -First 1
  if (-not $te) { Note "RESULT: TE.exe NOT FOUND under windows\packages"; exit 1 }
  Note ("TE: " + $te.FullName)

  $teArgs = @($dlls | ForEach-Object { Join-Path $root $_ })
  if ($Name) { $teArgs += "/name:$Name" }
  Note ("--- TE " + ($teArgs -join ' ') + " ---")
  & $te.FullName @teArgs 2>&1 | Tee-Object -FilePath $wrap -Append
  $tcode = $LASTEXITCODE
  Note ("=== TE EXIT $tcode ===")
  if ($tcode -eq 0) { Note "RESULT: TESTS PASSED" } else { Note "RESULT: TESTS FAILED ($tcode)" }
  exit $tcode
} catch {
  Note ("=== WRAPPER ERROR: $_ ===")
  Note "RESULT: TEST RUN FAILED (setup)"
  exit 1
}
