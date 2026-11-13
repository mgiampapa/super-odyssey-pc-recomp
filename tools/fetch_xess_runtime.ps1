param([string]$Destination = 'build/xess-runtime')
$ErrorActionPreference = 'Stop'
$revision = 'de0fb9c1c510661c571164e1418ceca8101dab69'
$expected = '251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7'
$target = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $target | Out-Null
$file = Join-Path $target 'libxess.dll'
if (!(Test-Path -LiteralPath $file) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) {
    $temporary = Join-Path $target ('libxess.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Invoke-WebRequest -Uri "https://raw.githubusercontent.com/intel/xess/$revision/bin/libxess.dll" -OutFile $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $expected) { throw 'XeSS hash mismatch' }
        $signature = Get-AuthenticodeSignature -LiteralPath $temporary
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Intel Corporation') {
            throw 'Intel signature check failed'
        }
        Move-Item -LiteralPath $temporary -Destination $file -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}
$licences = Join-Path $PSScriptRoot '../third_party/xess'
Copy-Item -LiteralPath (Join-Path $licences 'LICENSE-RUNTIME.txt') -Destination (Join-Path $target 'Intel-XeSS-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $licences 'third-party-programs.txt') -Destination (Join-Path $target 'Intel-XeSS-THIRD-PARTY.txt')
Write-Output "Intel XeSS runtime ready in $target"
