# Generate a unique self-signed local HTTPS certificate for this inverter.
# Requires OpenSSL on PATH. Run from repository root.
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force data | Out-Null
$key = Join-Path (Get-Location) "data/https_server.key"
$cert = Join-Path (Get-Location) "data/https_server.crt"
# P-256 ECDSA keeps the certificate and handshake smaller than RSA-2048.
# The IP SAN matches the default provisioning AP; no misleading .local DNS SAN.
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -sha256 -nodes `
  -keyout $key -out $cert -days 825 `
  -subj "/CN=192.168.4.1" `
  -addext "subjectAltName=IP:192.168.4.1" `
  -addext "basicConstraints=critical,CA:FALSE" `
  -addext "keyUsage=critical,digitalSignature" `
  -addext "extendedKeyUsage=serverAuth"
if ($LASTEXITCODE -ne 0) {
    throw "OpenSSL failed to generate the certificate."
}
Write-Host "Created $cert and $key"
Write-Warning "Do not commit data/https_server.key. Upload filesystem with: pio run -e esp32dev -t uploadfs"
