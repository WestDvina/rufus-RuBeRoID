# Aggregates fresh Microsoft ISO links from all available sources and merges
# them into iso_links.json. Key rule: NEVER drop a key. If no fresh valid
# candidate is found for a key, the previous (last-known-good) URL is kept.
#
# Sources (any of them may fail; the freshest HEAD-valid URL wins per key):
#   A - current iso_links.json (last-known-good fallback)
#   B - ShiFER data.json (entries with is_valid + valid_until in the future)
#   C - Fido (pbatard/Fido master) fetched live, Sentinel-tolerant
#   D - bot leftovers are just what is already in the file (source A)
#
# Usage (PowerShell 5.1+ / pwsh):
#   pwsh -NoProfile -File tools\refresh_iso_links.ps1
param(
  [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
  [int]$MaxAttempts = 2,
  [int]$RetryDelaySec = 15,
  [string]$ShiferUrl = 'https://raw.githubusercontent.com/WestDvina/shifer/main/docs/data.json',
  [string]$FidoUrl = 'https://github.com/pbatard/Fido/raw/master/Fido.ps1'
)

$ErrorActionPreference = 'Stop'

function Get-UrlExpiry {
  # Approximate expiry of a signed prss.microsoft.com URL: P1 query = unix time.
  param([string]$Url)
  $m = [regex]::Match($Url, '[?&]P1=(\d+)')
  if ($m.Success) { return [long]$m.Groups[1].Value }
  return 0
}

function Test-IsoUrl {
  param([string]$Url)
  try {
    $r = Invoke-WebRequest -Uri $Url -Method Head -TimeoutSec 30 -UseBasicParsing
    return ([int]$r.StatusCode -eq 200)
  } catch { return $false }
}

function Get-FidoUrl {
  param([string]$Win, [string]$Rel, [string]$Ed, [string]$Lang, [string]$Arch, [string]$FidoPath)
  for ($i = 1; $i -le $MaxAttempts; $i++) {
    try {
      $u = & powershell -NoProfile -ExecutionPolicy Bypass -File $FidoPath `
        -Win $Win -Rel $Rel -Ed $Ed -Lang $Lang -Arch $Arch -GetUrl 2>&1 |
        Where-Object { $_ -match '^https://' } | Select-Object -First 1
      if ($u -match '^https://software\.download\.prss\.microsoft\.com/') { return $u.Trim() }
      Write-Warning "Fido attempt $i ($Win/$Rel/$Arch): no URL (Sentinel or empty)."
    } catch {
      Write-Warning "Fido attempt $i ($Win/$Rel/$Arch) failed: $($_.Exception.Message)"
    }
    if ($i -lt $MaxAttempts) { Start-Sleep -Seconds $RetryDelaySec }
  }
  return $null
}

function Get-ShiferCandidates {
  # Returns hashtable key -> array of @{ Url; Expires; Build } from ShiFER data.
  param([string]$Url)
  $result = @{}
  try {
    $data = Invoke-RestMethod -Uri $Url -TimeoutSec 60
  } catch {
    Write-Warning "ShiFER fetch failed: $($_.Exception.Message)"
    return $result
  }
  $nowUnix = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
  foreach ($e in $data) {
    try {
      if (-not $e.is_valid) { continue }
      # Compare as unix seconds: [datetime] vs [DateTimeOffset] comparison throws.
      $exp = [long]([DateTimeOffset][datetime]$e.valid_until).ToUnixTimeSeconds()
      if ($exp -le $nowUnix) { continue }
      $os = [string]$e.version.os
      $arch = [string]$e.version.arch
      $lang = [string]$e.version.lang
      if ($lang -ne 'Russian') { continue }
      $key = $null
      if ($os -eq 'win10' -and $arch -eq 'x64') { $key = '10_x64' }
      elseif ($os -eq 'win10' -and $arch -eq 'x86') { $key = '10_x86' }
      elseif ($os -eq 'win11' -and $arch -eq 'x64') { $key = '11_x64' }
      if (-not $key) { continue }
      if (-not $result.ContainsKey($key)) { $result[$key] = @() }
      $result[$key] += @{ Url = [string]$e.iso_url; Expires = $exp; Build = [string]$e.version.build; Source = 'shifer' }
    } catch { continue }
  }
  return $result
}

$jsonPath = Join-Path $RepoRoot 'iso_links.json'
$data = Get-Content $jsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
$links = @{}
foreach ($p in $data.links.PSObject.Properties) { $links[$p.Name] = $p.Value }

# --- Source B: ShiFER ---
$shifer = Get-ShiferCandidates -Url $ShiferUrl
foreach ($k in $shifer.Keys) {
  Write-Host "ShiFER candidates for ${k}: $(($shifer[$k] | ForEach-Object { "$($_.Build)~$($_.Expires)" }) -join ', ')"
}

# --- Source C: Fido ---
$fido = Join-Path ([IO.Path]::GetTempPath()) 'Fido.ps1'
try {
  Invoke-WebRequest -Uri $FidoUrl -OutFile $fido -UseBasicParsing
} catch {
  Write-Warning "Fido download failed: $($_.Exception.Message)"
  $fido = $null
}
$targets = @(
  @{ Key = '11_x64'; Win = 'Windows 11'; Rel = '26H2'; Ed = 'Windows 11 Home/Pro/Edu'; Lang = 'Russian'; Arch = 'x64' },
  @{ Key = '10_x64'; Win = 'Windows 10'; Rel = '22H2'; Ed = 'Windows 10 Home/Pro/Edu';   Lang = 'Russian'; Arch = 'x64' },
  @{ Key = '10_x86'; Win = 'Windows 10'; Rel = '22H2'; Ed = 'Windows 10 Home/Pro/Edu';   Lang = 'Russian'; Arch = 'x86' }
)
$fidoFound = @{}
if ($fido) {
  foreach ($t in $targets) {
    $url = Get-FidoUrl -Win $t.Win -Rel $t.Rel -Ed $t.Ed -Lang $t.Lang -Arch $t.Arch -FidoPath $fido
    if ($url) { $fidoFound[$t.Key] = $url; Write-Host "Fido candidate for $($t.Key): P1=$(Get-UrlExpiry $url)" }
    Start-Sleep -Seconds 5  # avoid Sentinel rate-limit between SKUs
  }
}

# --- Merge: freshest HEAD-valid URL wins per key; old URL never dropped ---
$buildRank = @{ '26H2' = 30; '25H2' = 25; '24H2' = 24; '23H2' = 23; '22H2' = 22 }
$updated = @()
$kept = @()
$winners = @('10_x64', '10_x86', '11_x64')
foreach ($k in ($links.Keys | ForEach-Object { $_ })) { if ($k -notin $winners) { $winners += $k } }
foreach ($key in $winners) {
  $pool = @()
  if ($links.ContainsKey($key) -and $links[$key]) {
    $pool += @{ Url = $links[$key]; Expires = (Get-UrlExpiry $links[$key]); Build = ''; Source = 'file' }
  }
  if ($shifer.ContainsKey($key)) { $pool += $shifer[$key] }
  if ($fidoFound.ContainsKey($key)) {
    $pool += @{ Url = $fidoFound[$key]; Expires = (Get-UrlExpiry $fidoFound[$key]); Build = ''; Source = 'fido' }
  }
  # Sort: freshest expiry first; tie-break by build rank for Win11.
  $pool = $pool | Sort-Object -Property `
    @{ Expression = { $_.Expires }; Descending = $true }, `
    @{ Expression = { if ($buildRank.ContainsKey($_.Build)) { $buildRank[$_.Build] } else { 0 } }; Descending = $true }
  $picked = $null
  foreach ($c in $pool) {
    if ($c.Url -and (Test-IsoUrl $c.Url)) { $picked = $c; break }
    Write-Warning "$key : candidate from $($c.Source) failed HEAD check, trying next."
  }
  if ($picked -and $picked.Url -ne $links[$key]) {
    $links[$key] = $picked.Url
    $updated += "$key($($picked.Source))"
  } elseif ($picked) {
    $kept += "$key($($picked.Source):unchanged)"
  } else {
    $kept += "$key(none-valid:kept-old)"
    Write-Warning "$key : no valid candidate, keeping old URL."
  }
}

$ordered = [ordered]@{}
foreach ($k in @('10_x64', '10_x86', '11_x64')) { if ($links.ContainsKey($k)) { $ordered[$k] = $links[$k] } }
foreach ($k in $links.Keys) { if (-not $ordered.Contains($k)) { $ordered[$k] = $links[$k] } }

$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$out = [ordered]@{ links = $ordered; published_at = $now; ttl_hours = 22 }
$out | ConvertTo-Json -Depth 5 | Set-Content $jsonPath -Encoding UTF8

# Self-verify (with -Raw: required on Windows PowerShell 5.1)
$check = Get-Content $jsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
Write-Host "Keys in file: $(($check.links.PSObject.Properties.Name) -join ', ')"
Write-Host "Updated: $($updated -join ', '); kept: $($kept -join ', ')"
exit 0
