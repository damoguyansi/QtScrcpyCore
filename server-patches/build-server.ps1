param(
    [Parameter(Mandatory=$true)][string]$SourceDir,
    [Parameter(Mandatory=$true)][string]$AndroidJar,
    [Parameter(Mandatory=$true)][string]$FrameworkAidl,
    [Parameter(Mandatory=$true)][string]$BuildToolsDir,
    [Parameter(Mandatory=$true)][string]$BuildDir
)
$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SourceDir).Path.Replace('\', '/')
$revision = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne '2926c06c5dc3064ae6d8db706f1a98a37cfcf3f0') {
    throw 'SourceDir must be a checkout of upstream scrcpy v4.1.'
}
$patch = Join-Path $PSScriptRoot 'manual-clipboard.patch'
& git -C $source apply --reverse --check $patch 2>$null
if ($LASTEXITCODE -ne 0) {
    & git -C $source apply --check $patch
    if ($LASTEXITCODE -ne 0) { throw 'Clipboard patch does not apply cleanly.' }
    & git -C $source apply $patch
    if ($LASTEXITCODE -ne 0) { throw 'Clipboard patch failed.' }
}
$build = [IO.Path]::GetFullPath($BuildDir).Replace('\', '/')
if (Test-Path -LiteralPath $build) { throw 'Use a new, empty BuildDir for reproducible server builds.' }
$classes = Join-Path $build 'classes'
$generated = Join-Path $build 'gen'
New-Item -ItemType Directory -Path $classes,"$generated/com/genymobile/scrcpy" -Force | Out-Null
[IO.File]::WriteAllText("$generated/com/genymobile/scrcpy/BuildConfig.java", @'
package com.genymobile.scrcpy;
public final class BuildConfig {
    public static final boolean DEBUG = false;
    public static final String VERSION_NAME = "4.1";
}
'@)
# Windows aidl compares import roots literally; use absolute native paths throughout.
$aidlSource = (Resolve-Path "$source/server/src/main/aidl").Path
$aidlGenerated = (Resolve-Path $generated).Path
& "$BuildToolsDir/aidl.exe" "-o$aidlGenerated" "-I$aidlSource" "$aidlSource\android\content\IOnPrimaryClipChangedListener.aidl"
if ($LASTEXITCODE -ne 0) { throw 'Clipboard AIDL generation failed.' }
& "$BuildToolsDir/aidl.exe" "-o$aidlGenerated" "-I$aidlSource" -p $FrameworkAidl "$aidlSource\android\view\IDisplayWindowListener.aidl"
if ($LASTEXITCODE -ne 0) { throw 'Display AIDL generation failed.' }
$sources = Get-ChildItem "$source/server/src/main/java",$generated -Recurse -Filter '*.java'
$javaArguments = @('-encoding','UTF-8','-bootclasspath',$AndroidJar,
    '-cp',"$BuildToolsDir/core-lambda-stubs.jar;$generated",'-d',$classes,'-source','1.8','-target','1.8')
& "$env:JAVA_HOME/bin/javac.exe" @javaArguments @($sources.FullName)
if ($LASTEXITCODE -ne 0) { throw 'Server Java compilation failed.' }
# Feed D8 a jar so Windows command-line limits cannot truncate the class list.
& "$env:JAVA_HOME/bin/jar.exe" cf "$build/classes.jar" -C $classes .
if ($LASTEXITCODE -ne 0) { throw 'Class archiving failed.' }
& "$BuildToolsDir/d8.bat" --classpath $AndroidJar --output "$build/classes.zip" "$build/classes.jar"
if ($LASTEXITCODE -ne 0) { throw 'Server dex compilation failed.' }
Copy-Item -LiteralPath "$build/classes.zip" -Destination "$PSScriptRoot/../src/third_party/scrcpy-server" -Force
Get-FileHash "$PSScriptRoot/../src/third_party/scrcpy-server" -Algorithm SHA256
