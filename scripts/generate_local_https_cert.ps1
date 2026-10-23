# Generate a unique self-signed local HTTPS certificate for this inverter.
# Requires OpenSSL on PATH. Run from repository root.
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force data | Out-Null
$key = Join-Path (Get-Location) "data/https_server.key"
$cert = Join-Path (Get-Location) "data/https_server.crt"
openssl req -x509 -newkey rsa:2048 -sha256 -nodes `
  -keyout $key -out $cert -days 3650 `
  -subj "/CN=192.168.4.1" `
  -addext "subjectAltName=IP:192.168.4.1,DNS:solar-inverter.local"
if ($LASTEXITCODE -ne 0) {
    throw "OpenSSL failed to generate the certificate."
}
Write-Host "Created $cert and $key"
Write-Warning "Do not commit data/https_server.key. Upload filesystem with: pio run -e esp32dev -t uploadfs"