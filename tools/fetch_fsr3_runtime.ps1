param([string]$Destination = 'build/fsr3-runtime')
$ErrorActionPreference = 'Stop'
$revision = '7539ae8b6568ad29ed78a753404351daf3b5cca7'
$source = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/$revision/Kits/FidelityFX/signedbin"
$files = [ordered]@{
    'amd_fidelityfx_loader_vk.dll' = '38A56A01FB3565C0757D9F5C1E4E69DE4B65E5F2E5769B323B200DC1F142BAB9'
    'amd_fidelityfx_upscaler_vk.dll' = '4A7E895AF6E0B615D40A84D59EB8C2B1054AD91CDD0C739567940270D51BDAB7'
}
$target = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $target | Out-Null
foreach ($entry in $files.GetEnumerator()) {
    $file = Join-Path $target $entry.Key
    if ((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $entry.Value) { continue }
    $temporary = Join-Path $target ($entry.Key + '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        Invoke-WebRequest -Uri "$source/$($entry.Key)" -OutFile $temporary
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $entry.Value) { throw "Hash mismatch: $($entry.Key)" }
        $signature = Get-AuthenticodeSignature -LiteralPath $temporary
        if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Advanced Micro Devices') {
            throw "AMD signature check failed: $($entry.Key)"
        }
        Move-Item -LiteralPath $temporary -Destination $file -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../third_party/fsr3_sdk/LICENSE-SDK.md') -Destination (Join-Path $target 'AMD-FSR-SDK-LICENSE.md')
Write-Output "AMD FSR SDK Vulkan runtime ready in $target"
