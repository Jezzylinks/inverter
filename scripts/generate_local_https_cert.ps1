param(
    # Supply the exact MAC-suffixed hostname shown on the inverter LCD/API to
    # make the certificate SAN match https://<hostname>.local.
    [string]$DeviceHostname = ""
)

# Generate a self-signed local HTTPS certificate for this inverter.
# Requires OpenSSL on PATH. Run from repository root.
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force data | Out-Null
$key = Join-Path (Get-Location) "data/https_server.key"
$header = Get-Content (Join-Path (Get-Location) "include/wifi/wifi_http_server.h") -Raw
$match = [regex]::Match($header, '#define\s+WIFI_HTTPS_DEFAULT_IP\s+"([^"]+)"')
if (-not $match.Success) { throw "WIFI_HTTPS_DEFAULT_IP was not found in wifi_http_server.h" }
$provisioningIp = $match.Groups[1].Value
$cert = Join-Path (Get-Location) "data/https_server.crt"
$san = "IP:$provisioningIp"
if (-not [string]::IsNullOrWhiteSpace($DeviceHostname)) {
    if ($DeviceHostname -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.localif ($LASTEXITCODE -ne 0) {
    throw "OpenSSL failed to generate the certificate."
}
Write-Host "Created $cert and $key"
Write-Warning "Do not commit data/https_server.key. Upload filesystem with: pio run -e esp32dev -t uploadfs"
) {
        throw "DeviceHostname must be the exact local hostname, for example solar-inverter-A1B2.local"
    }
    $san += ",DNS:$DeviceHostname"
}
# P-256 ECDSA keeps the certificate and handshake smaller than RSA-2048.
# The IP SAN matches the provisioning AP; the optional DNS SAN matches the
# exact device hostname for the normal dashboard.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -sha256 -nodes `
  -keyout $key -out $cert -days 825 `
  -subj "/CN=$provisioningIp" `
  -addext "subjectAltName=$san" `
  -addext "basicConstraints=critical,CA:FALSE" `
  -addext "keyUsage=critical,digitalSignature" `
  -addext "extendedKeyUsage=serverAuth"
if ($LASTEXITCODE -ne 0) {
    throw "OpenSSL failed to generate the certificate."
}
Write-Host "Created $cert and $key"
Write-Warning "Do not commit data/https_server.key. Upload filesystem with: pio run -e esp32dev -t uploadfs"
