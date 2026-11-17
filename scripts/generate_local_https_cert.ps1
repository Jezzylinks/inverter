param(
    # Supply the exact MAC-suffixed mDNS hostname shown by the inverter.
    # The resulting certificate will match https://<hostname>.
    [string]$DeviceHostname = ""
)

# Generate a locally trusted certificate chain for the inverter dashboard.
# Import certs/vonix_inverter_root_ca.crt into each client device's trusted
# CA store. The CA private key is kept outside SPIFFS and must never be shared.
# Requires OpenSSL on PATH. Run from the repository root.
$ErrorActionPreference = "Stop"

$root = Get-Location
$dataDir = Join-Path $root "data"
$certDir = Join-Path $root "certs"
New-Item -ItemType Directory -Force $dataDir | Out-Null
New-Item -ItemType Directory -Force $certDir | Out-Null

$key = Join-Path $dataDir "https_server.key"
$cert = Join-Path $dataDir "https_server.crt"
$caKey = Join-Path $certDir "vonix_inverter_root_ca.key"
$caCert = Join-Path $certDir "vonix_inverter_root_ca.crt"
$csr = Join-Path $dataDir "https_server.csr"
$ext = Join-Path $dataDir "https_server.ext"

$header = Get-Content (Join-Path $root "include/wifi/wifi_http_server.h") -Raw
$match = [regex]::Match($header, '#define\s+WIFI_HTTPS_DEFAULT_IP\s+"([^"]+)"')
if (-not $match.Success) {
    throw "WIFI_HTTPS_DEFAULT_IP was not found in wifi_http_server.h"
}
$provisioningIp = $match.Groups[1].Value

$sanEntries = @("IP:$provisioningIp")
if (-not [string]::IsNullOrWhiteSpace($DeviceHostname)) {
    if ($DeviceHostname -notmatch '^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.local$') {
        throw "DeviceHostname must be the exact local hostname, e.g. vonix-inverter-A1B2.local"
    }
    $sanEntries += "DNS:$DeviceHostname"
}

# Preserve an existing CA so previously installed client trust remains valid.
if ((Test-Path $caKey) -xor (Test-Path $caCert)) {
    throw "The local CA is incomplete. Restore both CA files from backup or remove both and regenerate, then reinstall the new CA certificate on clients."
}
if (-not (Test-Path $caKey)) {
    Write-Host "Creating Vonix local root CA..."
    $rootArgs = @(
        "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
        "-sha256", "-nodes", "-keyout", $caKey, "-out", $caCert, "-days", "3650",
        "-subj", "/CN=Vonix Inverter Local Root CA",
        "-addext", "basicConstraints=critical,CA:TRUE,pathlen:0",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign",
        "-addext", "subjectKeyIdentifier=hash"
    )
    & openssl @rootArgs
    if ($LASTEXITCODE -ne 0) { throw "OpenSSL failed to create the local root CA." }
}

# P-256 ECDSA keeps the device certificate and TLS handshake compact.
Write-Host "Creating device HTTPS key and certificate request..."
$csrArgs = @(
    "req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:prime256v1",
    "-sha256", "-nodes", "-keyout", $key, "-out", $csr, "-subj", "/CN=$provisioningIp"
)
& openssl @csrArgs
if ($LASTEXITCODE -ne 0) { throw "OpenSSL failed to create the device certificate request." }

$extensions = @(
    "basicConstraints=critical,CA:FALSE"
    "keyUsage=critical,digitalSignature"
    "extendedKeyUsage=serverAuth"
    "subjectAltName=$($sanEntries -join ',')"
    "subjectKeyIdentifier=hash"
    "authorityKeyIdentifier=keyid,issuer"
)
Set-Content -Path $ext -Value $extensions -Encoding ascii

Write-Host "Signing device certificate with the local root CA..."
$signArgs = @(
    "x509", "-req", "-in", $csr, "-CA", $caCert, "-CAkey", $caKey, "-CAcreateserial",
    "-out", $cert, "-days", "825", "-sha256", "-extfile", $ext
)
& openssl @signArgs
if ($LASTEXITCODE -ne 0) { throw "OpenSSL failed to sign the device certificate." }

Remove-Item -Force $csr, $ext -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Created device certificate: $cert"
Write-Host "Created device private key: $key"
Write-Host "Client trust certificate:   $caCert"
Write-Host ""
Write-Host "NEXT STEPS:"
Write-Host "1. Install $caCert as a trusted CA certificate on each phone/computer."
Write-Host "2. Open the dashboard using https://$provisioningIp/"
if ($DeviceHostname) { Write-Host "   Or use https://$DeviceHostname/" }
Write-Host "3. Upload the filesystem image: pio run -e esp32dev -t uploadfs"
Write-Warning "Never upload or commit the CA private key ($caKey) or device private key ($key)."
