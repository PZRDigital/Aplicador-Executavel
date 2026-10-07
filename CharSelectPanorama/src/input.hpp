#pragma once

#include <windows.h>
#include <string>

// Redirecionamento do mouse: a interface nova fica por cima da janela nativa escondida.
// Quando o jogador aponta para um botão novo, o cliente "enxerga" o cursor sobre o
// controle nativo equivalente, então o clique real aciona a lógica original.
namespace input {
	struct Point {
		float x = 0.0f, y = 0.0f;
	};

	// Recebe a posição real (coordenadas da viewport) e devolve true + alvo para redirecionar.
	using RedirectFn = bool ( * )( const Point& real, Point& target );

	void Install( HWND hwnd );
	void SetRedirect( RedirectFn fn );
	void SetViewport( float width, float height );

	Point RealCursor();       // posição real do mouse (viewport)
	Point CursorOffset();     // real - informado ao cliente (para mover o cursor desenhado)
	bool IsLeftDown();
	bool TakeDiagRequest();   // F11 (debug): registrar um quadro
	// ESC na seleção: a interface nova trata (o cliente não recebe a tecla)
	void SetEscapeCapture( bool on );
	bool TakeEscape();
	void QuitGame();          // fecha o cliente
	void PressKey( unsigned vk ); // envia uma tecla à janela do jogo
	// Chave de modo (viewport): cliques nela não chegam ao cliente. w = 0 desativa.
	void SetToggleRect( float x, float y, float w, float h );
	bool TakeToggleClick();
	bool PhotoMode();
	int TakeWheel();          // giro acumulado da roda do mouse desde a última leitura         // tecla Pause: esconde a interface do jogo (captura do mapa)
	// Campo de texto da interface: teclas vão para nós (\b = apagar, \r = enter, 27 = esc)
	void SetTextCapture( bool on );
	std::wstring TakeTyped();
}
