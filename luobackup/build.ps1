param([string] $BungeeJar = (Join-Path $PSScriptRoot '../../../core/BungeeCord.jar'))
$ErrorActionPreference = 'Stop'
$bungee = (Resolve-Path -LiteralPath $BungeeJar).Path
$classes = Join-Path $PSScriptRoot 'build/classes'
New-Item -ItemType Directory -Path $classes -Force | Out-Null
javac --release 8 -encoding UTF-8 -cp $bungee -d $classes (Join-Path $PSScriptRoot 'src/top/luotiany1/backup/LuoBackup.java')
if ($LASTEXITCODE -ne 0) { throw 'LuoBackup Java compilation failed' }
jar cf (Join-Path $PSScriptRoot 'build/LuoBackup.jar') -C $classes . -C (Join-Path $PSScriptRoot 'resources') .
if ($LASTEXITCODE -ne 0) { throw 'LuoBackup JAR creation failed' }
