#pragma once

#include <d3d9.h>

namespace d3dhooks {
	// Chamado pelo proxy logo após Direct3DCreate9/Direct3DCreate9Ex do d3d9.dll real.
	void OnDirect3DCreated( IDirect3D9* d3d, bool isEx );
	// Imagem da própria interface (DLL ou cópia embutida no executável): quadros dela são ignorados
	// ao identificar quem desenha (cursor).
	void SetSelfImage( void* base );
	void DebugDiagNow( int frames ); // registra os próximos quadros (debug)
}
