param(
  [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
  [string]$Toolset = 'v143',
  [string]$WindowsSDK = '10.0.26100.0',
  [switch]$Rebuild,
  [string]$ProjectDirectory = ''
)
$ErrorActionPreference = 'Stop'
if (!$ProjectDirectory) {
  $ProjectDirectory = $PSScriptRoot
  if (!(Test-Path -LiteralPath (Join-Path $ProjectDirectory 'Ishiiruka\Source\Dolphin.sln'))) {
    $ProjectDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
  }
}
if (!(Test-Path -LiteralPath (Join-Path $ProjectDirectory 'Ishiiruka\Source\Dolphin.sln'))) {
  throw 'Use the checkout layout in docs/LOCAL-TEAMS-BUILD.md or pass -ProjectDirectory'
}
# Use consistent C/C++ and Rust source locations for reproducible builds.
$buildDrive = @('Z:', 'Y:', 'X:', 'W:', 'V:', 'U:', 'T:', 'S:') |
  Where-Object { !(Test-Path -LiteralPath ($_ + '\')) } | Select-Object -First 1
if (!$buildDrive) { throw 'No free drive letter for the isolated build path' }
& subst.exe $buildDrive $ProjectDirectory
if ($LASTEXITCODE -ne 0) { throw 'Could not create the isolated build path' }
$buildRoot = $buildDrive + '\'
$previousRustFlags = $env:RUSTFLAGS
$previousEncodedRustFlags = $env:CARGO_ENCODED_RUSTFLAGS
$previousGitConfigCount = $env:GIT_CONFIG_COUNT
$gitConfigOffset = if ($previousGitConfigCount) { [int]$previousGitConfigCount } else { 0 }
$previousPath = $env:PATH
try {
  $rustMappings = @($buildRoot, $ProjectDirectory, $env:CARGO_HOME, $env:RUSTUP_HOME, [Environment]::GetFolderPath('UserProfile')) |
    Where-Object { $_ } | Select-Object -Unique
  $env:CARGO_ENCODED_RUSTFLAGS = ($rustMappings | ForEach-Object { '--remap-path-prefix=' + $_.TrimEnd('\') + '=/source' }) -join [char]31
  # The drive alias can have a different owner than the invoking account.
  # Trust only these two exact checkout paths for this build process.
  [Environment]::SetEnvironmentVariable(('GIT_CONFIG_KEY_' + $gitConfigOffset), 'safe.directory', 'Process')
  [Environment]::SetEnvironmentVariable(('GIT_CONFIG_VALUE_' + $gitConfigOffset), (Join-Path $ProjectDirectory 'Ishiiruka').Replace('\', '/'), 'Process')
  [Environment]::SetEnvironmentVariable(('GIT_CONFIG_KEY_' + ($gitConfigOffset + 1)), 'safe.directory', 'Process')
  [Environment]::SetEnvironmentVariable(('GIT_CONFIG_VALUE_' + ($gitConfigOffset + 1)), (Join-Path $buildRoot 'Ishiiruka').Replace('\', '/'), 'Process')
  $env:GIT_CONFIG_COUNT = [string]($gitConfigOffset + 2)
  $env:PATH = (Join-Path $buildRoot 'tools\gecko') + ';' + $env:PATH
Push-Location (Join-Path $buildRoot 'slippi-ssbm-asm')
try {
  & (Join-Path $buildRoot 'tools\gecko\gecko.exe') build -c netplay.json -defsym STG_EXIIndex=1 -batched
  if ($LASTEXITCODE -ne 0) { throw 'Melee codeset failed to assemble' }
  Copy-Item -Path 'Output\Netplay\*r2.ini' -Destination (Join-Path $buildRoot 'Ishiiruka\Data\Sys\GameSettings')
} finally { Pop-Location }
Push-Location $buildRoot
try {
  $buildTarget = if ($Rebuild) { '/t:Rebuild' } else { '/t:Build' }
  & $MSBuild 'Ishiiruka\Source\Dolphin.sln' $buildTarget /m:4 /p:Configuration=Release /p:Platform=x64 "/p:PlatformToolset=$Toolset" "/p:WindowsTargetPlatformVersion=$WindowsSDK" "/p:ForceImportAfterCppTargets=$PSScriptRoot\toolchain.props" "/p:LocalTeamsD3DX=$buildRoot\tools\d3dx" /v:minimal /nologo
  if ($LASTEXITCODE -ne 0) { throw 'Dolphin failed to build' }
  $runtime = Join-Path $buildRoot 'runtime'
  New-Item -ItemType Directory -Path $runtime -Force | Out-Null
  Copy-Item -Path 'Ishiiruka\Binary\x64\*' -Destination $runtime -Recurse -Force
  Copy-Item -Path 'tools\d3dx\build\native\release\bin\x64\*.dll' -Destination $runtime -Force
  Write-Output "Built: $runtime\Slippi Dolphin.exe"
} finally { Pop-Location }
} finally {
  $env:RUSTFLAGS = $previousRustFlags
  $env:CARGO_ENCODED_RUSTFLAGS = $previousEncodedRustFlags
  $env:GIT_CONFIG_COUNT = $previousGitConfigCount
  foreach ($index in @($gitConfigOffset, ($gitConfigOffset + 1))) {
    [Environment]::SetEnvironmentVariable(('GIT_CONFIG_KEY_' + $index), $null, 'Process')
    [Environment]::SetEnvironmentVariable(('GIT_CONFIG_VALUE_' + $index), $null, 'Process')
  }
  $env:PATH = $previousPath
  & subst.exe $buildDrive /D
}
