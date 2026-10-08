# Checks that an unpacked or installed Windows package runs on a PC with nothing else installed:
#  1. every DLL imported by a shipped .exe/.dll is shipped too (next to the importer or in bin/)
#     or is part of Windows. The VC++ runtime is not part of Windows even though build machines
#     have it in System32, so it must be shipped.
#  2. with -Clip, scvp.exe opens the clip and saves a frame, with PATH reduced to Windows itself
#     (so no Qt or FFmpeg DLL can come from the build machine), and the colours in that frame
#     tell whether the video was drawn (software OpenGL on runners without a GPU).
#
#   scripts/check-windows-bundle.ps1 -Dir <package folder> [-Clip <video>]
# Needs dumpbin (a Visual Studio developer shell).
param(
    [Parameter(Mandatory = $true)] [string] $Dir,
    [string] $Clip
)
$ErrorActionPreference = 'Stop'
$Dir = (Resolve-Path $Dir).Path
$bin = Join-Path $Dir 'bin'
$system = Join-Path $env:WINDIR 'System32'
$vcRuntime = '^(vcruntime140(_1)?|msvcp140(_\d|_atomic_wait|_codecvt_ids)?|concrt140|vcomp140)\.dll$'

$names = @{}
Get-ChildItem $bin -File | ForEach-Object { $names[$_.Name.ToLowerInvariant()] = $true }
$problems = @()
$binaries = Get-ChildItem $Dir -Recurse -File -Include *.exe, *.dll
foreach ($f in $binaries) {
    $local = @{}
    Get-ChildItem $f.DirectoryName -File | ForEach-Object { $local[$_.Name.ToLowerInvariant()] = $true }
    $imports = dumpbin /nologo /dependents $f.FullName |
        Where-Object { $_ -match '^\s+(\S+\.dll)\s*$' } | ForEach-Object { $Matches[1].ToLowerInvariant() }
    foreach ($dll in $imports | Sort-Object -Unique) {
        if ($names[$dll] -or $local[$dll]) { continue }
        if ($dll -match $vcRuntime) {
            $problems += "$($f.Name) imports $dll (VC++ runtime, not shipped)"
        } elseif (-not ($dll -like 'api-ms-win-*' -or $dll -like 'ext-ms-*' -or (Test-Path (Join-Path $system $dll)))) {
            $problems += "$($f.Name) imports $dll (not shipped, not part of Windows)"
        }
    }
}
if (Get-ChildItem $Dir -Recurse -File -Filter 'vc_redist*.exe') {
    $problems += 'vc_redist installer shipped: the runtime DLLs belong next to scvp.exe instead'
}
if ($problems) {
    $problems | ForEach-Object { Write-Host "missing: $_" }
    throw "$($problems.Count) unresolved import(s) in $Dir"
}
Write-Host "imports: all $($binaries.Count) binaries resolve inside the package or Windows"

if ($Clip) {
    $Clip = (Resolve-Path $Clip).Path
    $out = Join-Path ([IO.Path]::GetTempPath()) "scvp-check-$PID"
    New-Item -ItemType Directory -Force $out | Out-Null
    $png = Join-Path $out 'grab.png'
    # A clean PC: only Windows on PATH, no Qt variables from the build machine, settings kept apart.
    $saved = @{}
    foreach ($v in 'PATH', 'QT_PLUGIN_PATH', 'QML2_IMPORT_PATH', 'QT_ROOT_DIR', 'APPDATA', 'LOCALAPPDATA') {
        $saved[$v] = [Environment]::GetEnvironmentVariable($v)
    }
    try {
        $env:PATH = "$system;$env:WINDIR;$system\Wbem"
        Remove-Item Env:QT_PLUGIN_PATH, Env:QML2_IMPORT_PATH, Env:QT_ROOT_DIR -ErrorAction SilentlyContinue
        $env:APPDATA = Join-Path $out 'appdata'
        $env:LOCALAPPDATA = Join-Path $out 'localappdata'
        # Runners have no GPU driver: Qt's bundled software OpenGL (opengl32sw.dll). Qt's
        # warnings go to stderr, not to a debugger nobody attaches.
        $env:QT_OPENGL = 'software'
        $env:QT_FORCE_STDERR_LOGGING = '1'
        $p = Start-Process -FilePath (Join-Path $bin 'scvp.exe') -PassThru -NoNewWindow `
            -ArgumentList @("`"$Clip`"", '--debug-size', '700x600', '--debug-grab', "`"$png`"") `
            -RedirectStandardError (Join-Path $out 'stderr.txt') -RedirectStandardOutput (Join-Path $out 'stdout.txt')
        $null = $p.Handle  # keeps ExitCode readable after the process ends
        if (-not $p.WaitForExit(120000)) { $p.Kill(); throw 'scvp.exe did not finish within 120 s' }
        $code = $p.ExitCode
    } finally {
        foreach ($v in $saved.Keys) { [Environment]::SetEnvironmentVariable($v, $saved[$v]) }
        Remove-Item Env:QT_OPENGL, Env:QT_FORCE_STDERR_LOGGING -ErrorAction SilentlyContinue
    }
    Get-Content (Join-Path $out 'stdout.txt'), (Join-Path $out 'stderr.txt') -ErrorAction SilentlyContinue | Write-Host
    if ($code -ne 0) { throw "scvp.exe exited with $code" }
    if (-not (Test-Path $png)) { throw 'scvp.exe saved no frame' }
    # A drawn test picture has many colours; an empty view has one or two.
    Add-Type -AssemblyName System.Drawing
    $bmp = [System.Drawing.Bitmap]::new($png)
    $colours = @{}
    for ($y = 0; $y -lt $bmp.Height; $y += 4) {
        for ($x = 0; $x -lt $bmp.Width; $x += 4) { $colours[$bmp.GetPixel($x, $y).ToArgb()] = $true }
    }
    $bmp.Dispose()
    Write-Host "run: scvp.exe opened $(Split-Path $Clip -Leaf) and saved a frame ($($colours.Count) colours)"
    if ($colours.Count -lt 16) { Write-Warning 'the saved view looks empty: the video was decoded but not drawn' }
}
