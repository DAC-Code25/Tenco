param(
    [ValidateSet('release')][string]$Preset = 'release',
    [string]$BuildDir = '',
    [string]$OutputDir = '',
    [string]$QtPrefix = 'D:/Qt/6.5.3/mingw_64',
    [string]$CompilerDir = 'D:/Qt/Tools/mingw1120_64',
    [string]$CMakeExe = 'D:/Qt/Tools/CMake_64/bin/cmake.exe',
    [string]$NinjaExe = 'D:/Qt/Tools/Ninja/ninja.exe',
    [string]$PythonExe = 'python',
    [string]$PostgreSqlArchive = '',
    [string]$PostgreSqlArchiveSha256 = '',
    [string]$PostgreSqlSourceUrl = '',
    [string]$MsvcRuntimeDir = '',
    [int]$Parallel = 4,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repoDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $BuildDir) { $BuildDir = Join-Path $repoDir 'build/preset-release-share' }
if (-not $OutputDir) { $OutputDir = Join-Path $repoDir ('../tmp/releases/windows-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
$QtPrefix = [IO.Path]::GetFullPath($QtPrefix)
$CompilerDir = [IO.Path]::GetFullPath($CompilerDir)
$runtimeDir = Join-Path $OutputDir 'runtime'
$packageName = 'Tenco-Windows-x64-Release'
$stageDir = Join-Path (Join-Path $OutputDir 'stage') $packageName
$archive = Join-Path $OutputDir ($packageName + '.zip')
$qtRoot = [IO.Path]::GetFullPath((Join-Path $QtPrefix '../..'))
$qtVersion = Split-Path (Split-Path $QtPrefix -Parent) -Leaf
$qtDocs = Join-Path $qtRoot ('Docs/Qt-' + $qtVersion)
$originalPath = $env:PATH
$dependencyCollector = Join-Path $PSScriptRoot 'windows_runtime_dependencies.py'

function Invoke-Checked([string]$Tool, [string[]]$Arguments, [string]$LogName) {
    & $Tool @Arguments 2>&1 | Tee-Object -FilePath (Join-Path $OutputDir $LogName)
    if ($LASTEXITCODE -ne 0) { throw "$Tool failed with exit code $LASTEXITCODE ($LogName)" }
}
function Copy-Required([string]$Source, [string]$Destination) {
    if (-not (Test-Path -LiteralPath $Source)) { throw "Required package input is missing: $Source" }
    New-Item -ItemType Directory -Path (Split-Path $Destination -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse
}

if (-not $PostgreSqlArchive -or $PostgreSqlArchiveSha256 -notmatch '^[0-9a-fA-F]{64}$' -or -not $PostgreSqlSourceUrl) {
    throw 'A verified official PostgreSQL Windows x64 archive, SHA256 and source URL are required for the QPSQL driver.'
}
$PostgreSqlArchive = [IO.Path]::GetFullPath($PostgreSqlArchive)
if (-not (Get-Command $PythonExe -ErrorAction SilentlyContinue)) { throw 'Python 3 is required by the PE dependency collector.' }
if (-not (Get-Command 'pwsh.exe' -ErrorAction SilentlyContinue)) { throw 'PowerShell 7 is required to verify Microsoft runtime Authenticode signatures.' }
foreach ($required in @($CMakeExe, $NinjaExe, $dependencyCollector, $PostgreSqlArchive, (Join-Path $QtPrefix 'bin/windeployqt.exe'),
        (Join-Path $CompilerDir 'bin/g++.exe'), (Join-Path $qtDocs 'qtdoc/gpl.html'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required tool or license is missing: $required" }
}
if ((Get-FileHash -LiteralPath $PostgreSqlArchive -Algorithm SHA256).Hash -ne $PostgreSqlArchiveSha256) {
    throw 'The PostgreSQL archive does not match the provided official SHA256.'
}
if ($MsvcRuntimeDir) { $MsvcRuntimeDir = [IO.Path]::GetFullPath($MsvcRuntimeDir) }
if ($Parallel -lt 1) { throw 'Parallel must be positive.' }
foreach ($reserved in @($stageDir, $archive, (Join-Path $OutputDir 'release-manifest.json'))) {
    if (Test-Path -LiteralPath $reserved) { throw "Refusing to overwrite an existing package: $reserved" }
}
$normalBin = [IO.Path]::GetFullPath((Join-Path $repoDir '../bin'))
foreach ($directory in @($BuildDir, $OutputDir)) {
    if ($directory.TrimEnd('\', '/') -eq $normalBin -or
        $directory.StartsWith($normalBin + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Packaging must not use the current bin directory.'
    }
}
if (Test-Path -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt')) {
    $homeEntry = Select-String -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$'
    if (-not $homeEntry -or [IO.Path]::GetFullPath($homeEntry.Matches[0].Groups[1].Value) -ne $repoDir) {
        throw 'Build cache belongs to another source directory.'
    }
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
Push-Location $repoDir
try {
    $env:PATH = (Join-Path $QtPrefix 'bin') + ';' + (Join-Path $CompilerDir 'bin') + ';' +
        (Split-Path $NinjaExe -Parent) + ';' + $originalPath
    Write-Host '[1/6] Configure isolated Release'
    Invoke-Checked $CMakeExe @('--preset', $Preset, '-B', $BuildDir,
        "-DCMAKE_MAKE_PROGRAM:FILEPATH=$NinjaExe", "-DCMAKE_PREFIX_PATH:PATH=$QtPrefix",
        "-DCMAKE_CXX_COMPILER:FILEPATH=$(Join-Path $CompilerDir 'bin/g++.exe')",
        '-DCMAKE_BUILD_TYPE:STRING=Release', '-DCMAKE_CXX_FLAGS:STRING=',
        '-DBUILD_TESTING:BOOL=ON', '-DTENCO_BUILD_TESTS:BOOL=ON',
        '-DTENCO_ENABLE_ABOUT_3D:BOOL=ON', '-DTENCO_ENABLE_OAK_CAMERA:BOOL=OFF',
        '-DTENCO_DEPLOY_WINDOWS_RUNTIME:BOOL=ON', "-DTENCO_RUNTIME_OUTPUT_DIRECTORY:PATH=$runtimeDir") 'configure.log'
    Write-Host '[2/6] Build complete project and deploy selected Qt runtime'
    Invoke-Checked $CMakeExe @('--build', $BuildDir, '--parallel', "$Parallel") 'build.log'
    if (-not (Select-String -LiteralPath (Join-Path $BuildDir 'build.ninja') -Pattern 'TENCO_ENABLE_ABOUT_3D')) {
        throw 'Release requires the About 3D backend; the selected Qt kit did not enable it.'
    }
    Write-Host '[3/6] Run CTest serially'
    if ($SkipTests) { Write-Warning 'Tests skipped explicitly; this is not a fully verified release.' }
    else { Invoke-Checked (Join-Path (Split-Path $CMakeExe -Parent) 'ctest.exe') @('--test-dir', $BuildDir,
        '--output-on-failure', '--parallel', '1') 'ctest.log' }

    Write-Host '[4/6] Stage clean runtime, safe configuration and license texts'
    $unexpected = Get-ChildItem -LiteralPath $runtimeDir -Recurse -File | Where-Object {
        $_.Extension -in @('.log', '.db', '.sqlite', '.sqlite3', '.bak', '.pem', '.key') -or
        $_.FullName -match '[/\\](logs|maps|Capture|backups)[/\\]'
    }
    if ($unexpected) { throw 'Runtime contains operator data; use a clean output directory.' }
    New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
    # Start from the executable and selected kit only; never copy arbitrary runtime files.
    Copy-Required (Join-Path $runtimeDir 'Tenco.exe') (Join-Path $stageDir 'Tenco.exe')
    Invoke-Checked (Join-Path $QtPrefix 'bin/windeployqt.exe') @('--qtpaths', (Join-Path $QtPrefix 'bin/qtpaths.exe'),
        '--qmldir', (Join-Path $repoDir 'qml'), '--compiler-runtime', '--no-translations', '--verbose', '0',
        (Join-Path $stageDir 'Tenco.exe')) 'deploy-package.log'
    Copy-Required (Join-Path $repoDir 'src/carmodel_about_centered.glb') (Join-Path $stageDir 'models/carmodel_about_centered.glb')
    $config = Get-Content -LiteralPath (Join-Path $repoDir 'config.json') -Raw | ConvertFrom-Json
    $config.network.websocketUrl = 'ws://127.0.0.1:1'
    $config.network.statusReadUrl = 'http://127.0.0.1:1/table/reads'
    $config.network.authToken = ''
    $config.network.chassisAutoReconnect = $false
    $config.video.backend = 'mjpeg_http'
    $config.video.streamUrl = 'http://127.0.0.1:1/camera/stream.mjpeg'
    $config.video.controlBaseUrl = 'http://127.0.0.1:1'
    $config.video.deviceId = ''
    $config.video.autoStart = $false
    foreach ($option in $config.video.streamOptions) { $option.url = 'http://127.0.0.1:1/stream' }
    $config.gimbal.enabled = $false
    $config.gimbal.plcHost = '127.0.0.1'
    foreach ($section in @('poseSource', 'tracking')) {
        $config.$section.enabled = $false
        $config.$section.baseUrl = 'http://127.0.0.1:1/api/v1'
    }
    $config.database.backend = 'sqlite'
    $config.database.sqliteFilePath = ''
    $config.database.host = ''
    $config.database.userName = ''
    $config.database.password = ''
    $config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $stageDir 'config.json') -Encoding UTF8
    foreach ($doc in @('README.md', 'CHANGELOG.md')) { Copy-Required (Join-Path $repoDir $doc) (Join-Path $stageDir $doc) }
    $licenses = Join-Path $stageDir 'licenses'
    foreach ($name in @('gpl.html', 'lgpl.html', 'fdl.html', 'licensing.html', 'licenses-used-in-qt.html')) {
        Copy-Required (Join-Path $qtDocs "qtdoc/$name") (Join-Path $licenses "Qt/$name")
    }
    # Include the installed version's attribution set, covering transitive QML imports too.
    foreach ($moduleDirectory in (Get-ChildItem -LiteralPath $qtDocs -Directory)) {
        $module = $moduleDirectory.Name
        $modulePath = $moduleDirectory.FullName
        Get-ChildItem -LiteralPath $modulePath -File | Where-Object {
            $_.Name -like '*-attribution-*.html' -or $_.Name -eq "$module-index.html"
        } | ForEach-Object { Copy-Required $_.FullName (Join-Path $licenses "Qt/$module/$($_.Name)") }
    }
    Copy-Required (Join-Path $qtRoot 'Licenses/LICENSE') (Join-Path $licenses 'Qt/LICENSE.txt')
    foreach ($name in @('gcc', 'winpthreads', 'mingw-w64')) {
        Copy-Required (Join-Path $CompilerDir "licenses/$name") (Join-Path $licenses "MinGW/$name")
    }
    $installerResources = Join-Path $qtRoot 'installerResources/qt.qt6.653.win64_mingw'
    if (Test-Path -LiteralPath $installerResources) {
        Get-ChildItem -LiteralPath $installerResources -File | Where-Object Name -match '(MinGW|d3dcompiler|opengl32sw)' |
            ForEach-Object { Copy-Required $_.FullName (Join-Path $licenses "Qt-installer/$($_.Name)") }
    }
    @'
Qt 6.5.3 and bundled runtime notices
===================================
This directory preserves official Qt license texts, module attribution pages,
MinGW runtime notices, and Qt installer runtime origin records.
Qt Quick3D in this OpenSource kit is Commercial OR GPL-3.0-only, not LGPL.
These notices do not replace any applicable source availability obligations.
No license for Tenco or its model/image assets is granted by this tool.
The project owner must establish redistribution rights before distributing.
DLLs remain replaceable; no static Qt linking is used.
'@ | Set-Content -LiteralPath (Join-Path $licenses 'README.txt') -Encoding UTF8
    @'
Tenco Windows x64 Release
========================
Extract the entire ZIP into a writable folder, then double-click Tenco.exe.
Keep its DLLs, plugin/QML folders, models and config.json together.
No Qt, MinGW, Python or developer PATH is needed to run the application.
Initial configuration is offline: loopback endpoints, empty credentials,
and disabled automatic video, PLC, positioning and navigation connections.
Set your own endpoints and credentials in Maintenance before connecting.
Logs, configuration backups and SQLite data are created locally on first use.
No saved maps, recordings, business database or field secrets are included.
See README.md for operation and licenses/README.txt for redistribution notices.
'@ | Set-Content -LiteralPath (Join-Path $stageDir 'START-HERE.txt') -Encoding UTF8
    foreach ($required in @('Tenco.exe', 'Qt6Core.dll', 'Qt6Quick3D.dll', 'Qt6Quick3DAssetUtils.dll',
            'platforms/qwindows.dll', 'sqldrivers/qsqlite.dll', 'qml/QtQuick3D/qmldir',
            'qml/QtQuick3D/AssetUtils/qmldir', 'models/carmodel_about_centered.glb',
            'libgcc_s_seh-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        if (-not (Test-Path -LiteralPath (Join-Path $stageDir $required) -PathType Leaf)) { throw "Missing dependency: $required" }
    }
    $dependencyArguments = @('-B', $dependencyCollector, '--stage', $stageDir,
        '--postgresql-archive', $PostgreSqlArchive, '--postgresql-sha256', $PostgreSqlArchiveSha256,
        '--postgresql-url', $PostgreSqlSourceUrl)
    if ($MsvcRuntimeDir) { $dependencyArguments += @('--msvc-runtime-dir', $MsvcRuntimeDir) }
    Invoke-Checked $PythonExe $dependencyArguments 'deploy-postgresql.log'
    Write-Host '[5/6] Write provenance and per-file SHA256 inventory'
    $files = @(Get-ChildItem -LiteralPath $stageDir -File -Recurse | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = $_.FullName.Substring($stageDir.Length + 1).Replace('\', '/'); bytes = $_.Length;
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    $sourceCommit = (& git -C $repoDir rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record source commit.' }
    $sourceChanges = @(& git -C $repoDir diff --name-only)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot record source changes.' }
    $manifest = [ordered]@{ format = 1; package = $packageName; createdUtc = [DateTime]::UtcNow.ToString('o');
        sourceCommit = $sourceCommit; sourceModifiedFiles = $sourceChanges; buildType = 'Release';
        architecture = 'x86_64'; qtVersion = $qtVersion; testsSkipped = [bool]$SkipTests;
        safeOfflineConfiguration = $true; redistributionLicenseReviewRequired = $true;
        postgresRuntime = 'postgresql-runtime.json'; files = $files }
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $stageDir 'release-manifest.json') -Encoding UTF8
    Write-Host '[6/6] Create ZIP without logs, tests or field data'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::CreateFromDirectory($stageDir, $archive, [IO.Compression.CompressionLevel]::Optimal, $true)
    $archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$archiveHash  $([IO.Path]::GetFileName($archive))" | Set-Content -LiteralPath ($archive + '.sha256') -Encoding ASCII
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDir 'release-manifest.json') -Encoding UTF8
    Write-Host "Package: $archive"
    Write-Host "SHA256:  $archiveHash"
} finally {
    $env:PATH = $originalPath
    Pop-Location
}
