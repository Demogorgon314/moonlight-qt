$ErrorActionPreference = 'Stop'

# FDK-AAC is kept outside the normal dependency wildcard so feature-off builds
# never deploy it. Pinning a commit and checksum makes the optional binary and
# its corresponding source archive reproducible.
$FdkVersion = '2.0.3'
$FdkCommit = '716f4394641d53f0d79c9ddac3fa93b03a49f278'
$FdkArchiveSha256 = 'C8DB4B4C335D0A6711EA82B0A577AB62FC1578187C391CFE02B2C64AB29F6D2A'
$FdkUrl = "https://github.com/mstorsjo/fdk-aac/archive/$FdkCommit.zip"
$TargetDir = Join-Path $PSScriptRoot '..\libs\windows\apple-screen-sharing\x64'

function Find-CMake {
    if ($env:CMAKE_EXE -and (Test-Path -LiteralPath $env:CMAKE_EXE)) {
        return (Resolve-Path -LiteralPath $env:CMAKE_EXE).Path
    }

    $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $vsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vsWhere) {
        $installation = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installation) {
            $candidate = Join-Path $installation 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    throw 'CMake from Visual Studio 2022 was not found. Set CMAKE_EXE to cmake.exe.'
}

$WorkRoot = Join-Path ([IO.Path]::GetTempPath()) ("moonlight-fdk-aac-$([Guid]::NewGuid().ToString('N'))")
$ArchivePath = Join-Path $WorkRoot "fdk-aac-$FdkVersion-source.zip"
$SourceRoot = Join-Path $WorkRoot "fdk-aac-$FdkCommit"
$BuildRoot = Join-Path $WorkRoot 'build'
$InstallRoot = Join-Path $WorkRoot 'install'

try {
    New-Item -ItemType Directory -Path $WorkRoot | Out-Null

    Write-Host "Downloading FDK-AAC $FdkVersion source..." -ForegroundColor Cyan
    & curl.exe -s -L -f -o $ArchivePath $FdkUrl
    if ($LASTEXITCODE -ne 0) {
        throw "FDK-AAC download failed with exit code $LASTEXITCODE"
    }

    $actualHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $ArchivePath).Hash
    if ($actualHash -ne $FdkArchiveSha256) {
        throw "FDK-AAC source checksum mismatch: expected $FdkArchiveSha256, got $actualHash"
    }

    Expand-Archive -LiteralPath $ArchivePath -DestinationPath $WorkRoot
    if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot 'CMakeLists.txt'))) {
        throw 'The FDK-AAC source archive has an unexpected layout.'
    }

    $cmake = Find-CMake
    & $cmake -S $SourceRoot -B $BuildRoot -G 'Visual Studio 17 2022' -A x64 `
        -DBUILD_SHARED_LIBS=ON `
        -DBUILD_PROGRAMS=OFF `
        -DFDK_AAC_INSTALL_PKGCONFIG_MODULE=OFF `
        -DFDK_AAC_INSTALL_CMAKE_CONFIG_MODULE=OFF `
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded `
        "-DCMAKE_INSTALL_PREFIX=$InstallRoot"
    if ($LASTEXITCODE -ne 0) {
        throw "FDK-AAC configure failed with exit code $LASTEXITCODE"
    }

    & $cmake --build $BuildRoot --config Release --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "FDK-AAC build failed with exit code $LASTEXITCODE"
    }
    & $cmake --install $BuildRoot --config Release
    if ($LASTEXITCODE -ne 0) {
        throw "FDK-AAC install failed with exit code $LASTEXITCODE"
    }

    New-Item -ItemType Directory -Path $TargetDir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $InstallRoot 'bin\fdk-aac.dll') `
        -Destination (Join-Path $TargetDir 'fdk-aac.dll') -Force
    Copy-Item -LiteralPath (Join-Path $SourceRoot 'NOTICE') `
        -Destination (Join-Path $TargetDir 'NOTICE.txt') -Force
    Copy-Item -LiteralPath $ArchivePath `
        -Destination (Join-Path $TargetDir "fdk-aac-$FdkVersion-source.zip") -Force

    Write-Host 'FDK-AAC Apple audio dependency deployed.' -ForegroundColor Green
}
finally {
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    $resolvedWork = [IO.Path]::GetFullPath($WorkRoot).TrimEnd('\')
    if ($resolvedWork.StartsWith($resolvedTemp + '\', [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedWork).StartsWith('moonlight-fdk-aac-', [StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force -ErrorAction SilentlyContinue
    }
}
