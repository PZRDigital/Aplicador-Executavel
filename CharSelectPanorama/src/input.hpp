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
	// último aperto do botão direito entregue ao cliente (GetTickCount e ponto na viewport), se já foi solto,
	// e repetir o clique no mesmo ponto
	bool LastRightClick( unsigned& tick, Point& at );
	bool RightReleased();
	void NudgeMouse(); // movimento no ponto do último clique direito (o cliente só aceita clique com o mouse em cima)
	void RepeatRightClick( bool down ); // aperto e soltura em quadros diferentes (o cliente lê uma vez por quadro)
	// Chamado pelo RedirectFn no clique: o botão esquerdo não chega ao cliente até ser solto
	// (o clique é tratado pela interface nova; senão o jogo o toma como clique no mapa e anda)
	void SwallowClick();
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
