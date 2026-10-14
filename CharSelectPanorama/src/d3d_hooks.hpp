#pragma once

#include <d3d9.h>

namespace d3dhooks {
	// Chamado pelo proxy logo após Direct3DCreate9/Direct3DCreate9Ex do d3d9.dll real.
	void OnDirect3DCreated( IDirect3D9* d3d, bool isEx );
	// Imagem da própria interface (DLL ou cópia embutida no executável): quadros dela são ignorados
	// ao identificar quem desenha (cursor).
	void SetSelfImage( void* base );
	void DebugDiagNow( int frames ); // registra os próximos quadros (debug)
	// Ícones de buff deslocados (desenhados mais abaixo): sobre um deles, o alvo é o original (acima); no
	// lugar antigo (agora vazio, embaixo do nome do mapa), o alvo sai da coluna para o cliente não achar o ícone
	bool BuffRedirect( float x, float y, float& tx, float& ty );
}
