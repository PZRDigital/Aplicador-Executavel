# Ferramentas de teste da seleção: compilar, abrir o cliente, capturar a tela e clicar.
# Uso: powershell -ExecutionPolicy Bypass -File testar.ps1 <acao> [args]
#   compilar            compila Release|Win32 (bin\Release\d3d9.dll); tools/publicar.sh embute no Codex.exe
#   abrir               fecha o Codex (se aberto) e abre pelo #Start.bat
#   captura <nome>      salva a área do cliente em CharSelectPanorama\testes\<nome>.png
#   clique <x> <y>      clique esquerdo em coordenadas do cliente
#   duplo <x> <y>       clique duplo
#   tecla <vk>          pressiona uma tecla (código virtual, ex.: 27 = Esc)
#   digitar <texto>     envia texto (sintaxe SendKeys: {ENTER}, etc.)
#   rajada <x,y> <n>    clica em x,y e captura n quadros seguidos (reduzidos) em testes\rajada\
#   fechar              encerra o Codex
param(
	[Parameter(Mandatory = $true)][string]$Acao,
	[string]$A,
	[string]$B
)

$ErrorActionPreference = 'Stop'
$projeto = $PSScriptRoot
$cliente = Join-Path (Split-Path (Split-Path $projeto -Parent) -Parent) 'Cliente 2025' # NexusRO\Cliente 2025
$saida = Join-Path $projeto 'testes'
New-Item -ItemType Directory -Force -Path $saida | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class W {
	[StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
	[StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
	[DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
	[DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
	[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
	[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
	[DllImport("user32.dll")] public static extern void mouse_event(int f, int x, int y, int d, int e);
	[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, int f, int e);
}
'@

function Janela {
	$p = Get-Process Codex -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
	if (-not $p) { throw 'Janela do Codex nao encontrada' }
	return $p.MainWindowHandle
}

function Mover([int]$x, [int]$y) {
	$h = Janela
	[W]::SetForegroundWindow($h) | Out-Null
	$o = New-Object W+POINT; $o.X = $x; $o.Y = $y
	[W]::ClientToScreen($h, [ref]$o) | Out-Null
	[W]::SetCursorPos($o.X, $o.Y) | Out-Null
	Start-Sleep -Milliseconds 300
}

function Clicar { [W]::mouse_event(0x02, 0, 0, 0, 0); Start-Sleep -Milliseconds 180; [W]::mouse_event(0x04, 0, 0, 0, 0) }

switch ($Acao) {
	'compilar' {
		$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
		$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
		& $msbuild (Join-Path $projeto 'CharSelectPanorama.vcxproj') /p:Configuration=Release /p:Platform=Win32 /m /v:minimal /nologo
		if ($LASTEXITCODE -ne 0) { throw "Falha na compilacao ($LASTEXITCODE)" }
		if (Get-Process Codex -ErrorAction SilentlyContinue) { Get-Process Codex | Stop-Process -Force; Start-Sleep -Seconds 1 }
		Write-Host 'OK: compilado (bin\Release\d3d9.dll); use tools/publicar.sh para embutir no Codex.exe'
	}
	'abrir' {
		Get-Process Codex -ErrorAction SilentlyContinue | Stop-Process -Force
		Start-Process -FilePath (Join-Path $cliente '#Start.bat') -WorkingDirectory $cliente
		Write-Host 'OK: cliente iniciado'
	}
	'captura' {
		$h = Janela
		$r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
		$o = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o) | Out-Null
		$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
		$g = [System.Drawing.Graphics]::FromImage($bmp)
		$g.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
		$arquivo = Join-Path $saida "$A.png"
		$bmp.Save($arquivo); $g.Dispose(); $bmp.Dispose()
		Write-Host "OK: $arquivo ($($r.R)x$($r.B))"
	}
	'clique' { Mover ([int]$A) ([int]$B); Clicar; Write-Host "OK: clique $A,$B" }
	'duplo' { Mover ([int]$A) ([int]$B); Clicar; Start-Sleep -Milliseconds 90; Clicar; Write-Host "OK: duplo $A,$B" }
	'mover' { Mover ([int]$A) ([int]$B); Write-Host "OK: mouse em $A,$B" }
	'tecla' {
		[W]::SetForegroundWindow((Janela)) | Out-Null
		[W]::keybd_event([byte][int]$A, 0, 0, 0); Start-Sleep -Milliseconds 60; [W]::keybd_event([byte][int]$A, 0, 2, 0)
		Write-Host "OK: tecla $A"
	}
	'roda' {
		# gira a roda do mouse N vezes (negativo = afastar)
		Mover 800 450
		$n = [int]$A
		for ($i = 0; $i -lt [Math]::Abs($n); $i++) {
			[W]::mouse_event(0x0800, 0, 0, [Math]::Sign($n) * 120, 0); Start-Sleep -Milliseconds 120
		}
		Write-Host "OK: roda $n"
	}
	'digitar' {
		Add-Type -AssemblyName System.Windows.Forms
		[W]::SetForegroundWindow((Janela)) | Out-Null
		Start-Sleep -Milliseconds 200
		[System.Windows.Forms.SendKeys]::SendWait($A)
		Write-Host "OK: digitado $A"
	}
	'rajada' {
		# clique + sequência de capturas com o tempo de cada uma (para ver a transição da câmera)
		$h = Janela
		$r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
		$o = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o) | Out-Null
		$w = $r.R - $r.L; $hh = $r.B - $r.T
		$dir = Join-Path $saida 'rajada'
		New-Item -ItemType Directory -Force -Path $dir | Out-Null
		Get-ChildItem $dir -Filter *.png | Remove-Item
		$xy = $A.Split(','); $n = [int]$B
		Mover ([int]$xy[0]) ([int]$xy[1])
		$bmp = New-Object System.Drawing.Bitmap $w, $hh
		$g = [System.Drawing.Graphics]::FromImage($bmp)
		$small = New-Object System.Drawing.Bitmap ([int]($w / 2)), ([int]($hh / 2))
		$gs = [System.Drawing.Graphics]::FromImage($small)
		$sw = [System.Diagnostics.Stopwatch]::StartNew()
		Clicar
		$tempos = @()
		for ($i = 0; $i -lt $n; $i++) {
			$g.CopyFromScreen($o.X, $o.Y, 0, 0, $bmp.Size)
			$t = $sw.ElapsedMilliseconds
			$gs.DrawImage($bmp, 0, 0, $small.Width, $small.Height)
			$small.Save((Join-Path $dir ('{0:D3}.png' -f $i)))
			$tempos += "$i $t"
		}
		$tempos | Set-Content (Join-Path $dir 'tempos.txt')
		$g.Dispose(); $gs.Dispose(); $bmp.Dispose(); $small.Dispose()
		Write-Host "OK: $n quadros em $dir"
	}
	'fechar' { Get-Process Codex -ErrorAction SilentlyContinue | Stop-Process -Force; Write-Host 'OK: cliente fechado' }
	default { throw "Acao desconhecida: $Acao" }
}
