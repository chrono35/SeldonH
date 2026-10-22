# SeldonH - compatibilite llama.cpp / MinGW-w64 < 12 (toolchain mingw1310 de Qt).
# Ses en-tetes declarent SetThreadInformation mais pas THREAD_POWER_THROTTLING_STATE, utilise par
# ggml-cpu.c (optimisation Windows 11 : eviter que les threads soient regroupes sur quelques coeurs).
# On ajoute la definition, seulement si elle manque, juste avant la fonction qui l'utilise.
# Appele par build_llama_win.bat ; sans effet si deja applique.
param([Parameter(Mandatory = $true)][string]$File)

$text = [IO.File]::ReadAllText($File)
if ($text.Contains('SeldonH : en-tetes MinGW-w64')) { Write-Host '    compatibilite MinGW deja appliquee'; exit 0 }

$anchor = 'static bool ggml_thread_apply_priority'
$i = $text.IndexOf($anchor)
if ($i -lt 0) { Write-Host "    ERREUR : '$anchor' introuvable dans $File"; exit 1 }

$nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }
$compat = @(
  '// SeldonH : en-tetes MinGW-w64 < 12 (toolchain mingw1310 de Qt) sans THREAD_POWER_THROTTLING_STATE',
  '#if defined(_WIN32) && !defined(THREAD_POWER_THROTTLING_CURRENT_VERSION)',
  'typedef struct _THREAD_POWER_THROTTLING_STATE {',
  '    ULONG Version;',
  '    ULONG ControlMask;',
  '    ULONG StateMask;',
  '} THREAD_POWER_THROTTLING_STATE;',
  '#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1',
  '#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1',
  '#endif',
  '', '') -join $nl

$text = $text.Insert($i, $compat)
[IO.File]::WriteAllText($File, $text, (New-Object Text.UTF8Encoding $false))
Write-Host '    compatibilite MinGW appliquee a ggml-cpu.c'
