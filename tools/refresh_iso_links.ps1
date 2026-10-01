# Refreshes iso_links.json using Fido (pbatard/Fido master).
# Key rule: NEVER drop a key. On Sentinel reject / error the old URL is kept.
# Usage (Windows, PowerShell 5.1+):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\refresh_iso_links.ps1
param(
  [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
  [int]$MaxAttempts = 3,
  [int]$RetryDelaySec = 15
)

$ErrorActionPreference = 'Stop'

function Get-FidoUrl {
  param([string]$Win, [string]$Rel, [string]$Ed, [string]$Lang, [string]$Arch, [string]$FidoPath)
  for ($i = 1; $i -le $MaxAttempts; $i++) {
    try {
      $u = & powershell -NoProfile -ExecutionPolicy Bypass -File $FidoPath `
        -Win $Win -Rel $Rel -Ed $Ed -Lang $Lang -Arch $Arch -GetUrl 2>&1 |
        Where-Object { $_ -match '^https://' } | Select-Object -First 1
      if ($u -match '^https://software\.download\.prss\.microsoft\.com/') { return $u.Trim() }
      Write-Warning "Attempt $i ($Win/$Rel/$Arch): no URL (Sentinel or empty). Output: $u"
    } catch {
      Write-Warning "Attempt $i ($Win/$Rel/$Arch) failed: $($_.Exception.Message)"
    }
    if ($i -lt $MaxAttempts) { Start-Sleep -Seconds $RetryDelaySec }
  }
  return $null
}

function Test-IsoUrl {
  param([string]$Url)
  try {
    $r = Invoke-WebRequest -Uri $Url -Method Head -TimeoutSec 30 -UseBasicParsing
    $len = ($r.Headers['Content-Length'] -join ',')
    return ([int]$r.StatusCode -eq 200)
  } catch { return $false }
}

$jsonPath = Join-Path $RepoRoot 'iso_links.json'
$data = Get-Content $jsonPath -Raw -Encoding UTF8 | ConvertFrom-Json
# Hashtable for merge (preserve existing keys)
$links = @{}
foreach ($p in $data.links.PSObject.Properties) { $links[$p.Name] = $p.Value }

$fido = Join-Path ([IO.Path]::GetTempPath()) 'Fido.ps1'
Invoke-WebRequest -Uri 'https://github.com/pbatard/Fido/raw/master/Fido.ps1' -OutFile $fido -UseBasicParsing

$targets = @(
  @{ Key = '11_x64'; Win = 'Windows 11'; Rel = '26H2'; Ed = 'Windows 11 Home/Pro/Edu'; Lang = 'Russian'; Arch = 'x64' },
  @{ Key = '10_x64'; Win = 'Windows 10'; Rel = '22H2'; Ed = 'Windows 10 Home/Pro/Edu';   Lang = 'Russian'; Arch = 'x64' },
  @{ Key = '10_x86'; Win = 'Windows 10'; Rel = '22H2'; Ed = 'Windows 10 Home/Pro/Edu';   Lang = 'Russian'; Arch = 'x86' }
)

$updated = @()
$kept = @()
foreach ($t in $targets) {
  $url = Get-FidoUrl -Win $t.Win -Rel $t.Rel -Ed $t.Ed -Lang $t.Lang -Arch $t.Arch -FidoPath $fido
  if ($url -and (Test-IsoUrl $url)) {
    $links[$t.Key] = $url
    $updated += $t.Key
  } else {
    $kept += $t.Key  # old URL preserved, key never deleted
    Write-Warning "$($t.Key): keeping old URL (fresh fetch failed)"
  }
  Start-Sleep -Seconds 5  # avoid Sentinel rate-limit between SKUs
}

# Rebuild ordered object: 10_x64, 10_x86, 11_x64 (only keys we know + any extras preserved)
$ordered = [ordered]@{}
foreach ($k in @('10_x64', '10_x86', '11_x64')) { if ($links.ContainsKey($k)) { $ordered[$k] = $links[$k] } }
foreach ($k in $links.Keys) { if (-not $ordered.Contains($k)) { $ordered[$k] = $links[$k] } }

$now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$out = [ordered]@{ links = $ordered; published_at = $now; ttl_hours = 22 }
$out | ConvertTo-Json -Depth 5 | Set-Content $jsonPath -Encoding UTF8

Write-Host "Updated: $($updated -join ', '); kept old: $($kept -join ', ')"
