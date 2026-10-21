# Builds SETTINGS.cfg from the player's own DUMMYS.DAT (same logic as
# tools/make_settings.py), so no game data has to ship with the installer.
#   make_settings.ps1 -Dummys <Common\Settings\DUMMYS.DAT> -Mode steam|lan -Out <file>
param(
    [Parameter(Mandatory)] [string] $Dummys,
    [Parameter(Mandatory)] [ValidateSet('steam', 'lan')] [string] $Mode,
    [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = 'Stop'
$latin1 = [Text.Encoding]::GetEncoding(28591)

$key = [Text.Encoding]::ASCII.GetBytes('14F5FsyDFFUC4NVANPpYggyakreWkJfy')
$data = [IO.File]::ReadAllBytes($Dummys)
for ($i = 0; $i -lt $data.Length; $i++) { $data[$i] = $data[$i] -bxor $key[$i % $key.Length] }
$text = $latin1.GetString($data)

# The last few bytes are not covered by the XOR key (they decode to garbage) and
# hold the closing brace of the final table. Drop them and close what is open.
$text = $text.Substring(0, $text.LastIndexOf('}') + 1)

function Get-OpenBraces([string] $lua) {
    $lua = [regex]::Replace($lua, '--[^\n]*', '')
    $lua = [regex]::Replace($lua, '"(?:\\.|[^"\\])*"', '')
    ([regex]::Matches($lua, '\{')).Count - ([regex]::Matches($lua, '\}')).Count
}
$text += "`r`n}" * (Get-OpenBraces $text) + "`r`n"
if ((Get-OpenBraces $text) -ne 0) { throw 'unbalanced braces after repair' }

if ($Mode -eq 'lan') {
    $overrides = , @("`tUseLanNetwork = false,", "`tUseLanNetwork = true, -- wrc10mp")
} else {
    $overrides = , @("`tUseKtOS = true,`r`n}", "`tUseKtOS = true,`r`n`tUseKtOSMultiplayer = false, -- wrc10mp`r`n}")
}
foreach ($o in $overrides) {
    $count = ([regex]::Matches($text, [regex]::Escape($o[0]))).Count
    if ($count -ne 1) { throw "override anchor found $count times: $($o[0])" }
    $text = $text.Replace($o[0], $o[1])
}
[IO.File]::WriteAllBytes($Out, $latin1.GetBytes($text))
