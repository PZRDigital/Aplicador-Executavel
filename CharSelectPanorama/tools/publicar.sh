#!/bin/bash
# Compila a interface, embute no Codex.exe e gera a char.grf; instala no cliente.
# Uso: tools/publicar.sh [--sem-grf]
#   base/Codex_warp.exe = executável gerado pelo WARP (sem a interface). Troque-o ao refazer o diff.
set -e
cd "$(dirname "$0")/.."
# A pasta fica em NexusRO\Aplicador de Executavel\CharSelectPanorama
CLIENTE="../../Cliente 2025"
EMULADOR="../../Emulador"
SAIDA=$(powershell.exe -ExecutionPolicy Bypass -File testar.ps1 compilar 2>&1 || true)
if echo "$SAIDA" | grep -qE " error |Falha na compilacao"; then
	echo "$SAIDA" | grep -E " error |Falha"
	echo "ERRO: compilação falhou; nada foi instalado"
	exit 1
fi
test -f bin/Release/d3d9.dll
powershell.exe -Command "Get-Process Codex -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1" || true
python3 tools/embutir.py base/Codex_warp.exe bin/Release/d3d9.dll dist/Codex.exe
cp dist/Codex.exe "$CLIENTE/Codex.exe"
if [ "$1" != "--sem-grf" ]; then
	# opções aleatórias e dados extras dos itens: GRFs do cliente e db do emulador
	rm -rf dist/charselect/skin
	python3 tools/opcoes.py "$CLIENTE" "$EMULADOR"
	python3 tools/itemextra.py "$EMULADOR"
	python3 tools/habilidades.py "$CLIENTE"
	python3 tools/criar_grf.py dist/charselect dist/char.grf
	cp dist/char.grf "$CLIENTE/char.grf"
fi
echo "OK: Codex.exe${1:+} e char.grf instalados no cliente"

# Aplicador de Executavel: recompila com a interface nova embutida e atualiza a char.grf dele
APLIC=".."
PROJ=$(wslpath -w "$APLIC/fonte/Aplicador.vcxproj")
powershell.exe -Command "& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe' '$PROJ' /p:Configuration=Release /p:Platform=Win32 /m /v:minimal /nologo" | grep -E " error |->" || true
[ -f dist/char.grf ] && cp dist/char.grf "$APLIC/char.grf"
echo "OK: Aplicador de Executavel atualizado"
