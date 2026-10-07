#pragma once

#include <string>
#include <vector>

namespace config {
	enum class BackgroundMode {
		Replace, // substitui o primeiro desenho em tela cheia do frame (fundo do cliente)
		Under,   // desenha antes do primeiro desenho do frame (fundo do cliente precisa ser transparente)
		Over,    // desenha por cima de tudo (apenas para testar as imagens)
	};

	// Um ponto de câmera sobre uma imagem de cena.
	struct View {
		int image = -1;      // índice em Settings::images
		float focusX = 0.5f; // centro da câmera na imagem (0..1)
		float focusY = 0.5f;
		float zoom = 1.0f;   // 1 = imagem cobre a tela inteira; >1 aproxima
		bool follow = false; // câmera seguindo um personagem (aplica o deslocamento de enquadramento)
	};

	struct Point {
		float x = 0.0f, y = 0.0f;
	};

	struct Settings {
		bool enabled = true;
		bool debug = false;
		BackgroundMode mode = BackgroundMode::Replace;
		float fullscreenThreshold = 0.95f; // fração da viewport para considerar um desenho "tela cheia"
		unsigned bgTextureWidth = 0;       // opcional: identifica o fundo pelo tamanho da textura
		unsigned bgTextureHeight = 0;

		unsigned transitionMs = 900;  // duração do movimento de câmera entre slots
		unsigned crossfadeMs = 500;   // duração do esmaecimento quando a imagem muda
		float pushIn = 0.06f;         // zoom extra na entrada de uma nova imagem
		float sway = 0.004f;          // amplitude do balanço ocioso (fração da imagem)
		float pullBack = 0.0f;        // quanto a câmera afasta no meio da troca de personagem (0 = não afasta)
		float smoothness = 0.11f;     // constante de tempo da mola da câmera (s); maior = mais lenta

		// Interface nova (fase 2)
		bool uiEnabled = true;
		unsigned codepage = 1252;     // nomes dos personagens
		float charScale = 1.8f;       // ampliação do personagem selecionado
		float charX = 0.5f;           // pés do personagem: fração da área livre entre os painéis
		float charY = 0.72f;          // pés do personagem: fração da altura da tela
		bool pixelated = true;        // amplia como pixel art (nítido, com shader de bordas suaves)
		float spriteSmooth = 0.3f;    // 0 = pixels nítidos, 1 = totalmente suavizado
		float spriteFootY = 146.0f;   // altura dos pés dentro do cartão nativo
		float dimOthers = 0.72f;      // brilho dos personagens não selecionados (1 = normal)
		float thumbScale = 0.9f;      // escala do personagem na miniatura da lista
		int selectedAction = 32;      // ação do boneco selecionado (32 = postura de combate; -1 = parado)
		int actionFrameMs = 110;      // duração de cada quadro dessa animação
		int fadeMs = 80;              // duração de cada fade da troca de mapa (original 255 ms; 0 = sem fade)
		int serverChoiceMs = 120000;  // tempo do login-server para escolher o servidor (AUTH_TIMEOUT)
		int attackAction = 40;        // golpe ao pré-selecionar (-1 = nunca)
		int attackEveryMs = 0;        // repetir o golpe a cada N ms (0 = só ao pré-selecionar)

		// Personagens no mapa (estilo "alojamento"): posição dos pés de cada slot na imagem (0..1)
		std::vector<Point> charPositions;
		// Enquadramento: onde o personagem selecionado fica na tela (fração a partir do centro)
		float frameX = -0.12f;
		float frameY = 0.16f;

		// Mapa 3D de verdade ([mapa3d]); sem ele, usa as imagens de [cenas]
		bool map3d = false;
		std::wstring map3dDir;         // pasta com mapa.bin (tools\mapa3d.py)
		std::vector<Point> charCells;  // célula do GAT de cada slot
		std::vector<float> charYaw;    // direção da câmera de cada slot (NAN = automática)
		float camYaw = 0.0f;           // direção base da câmera (graus; 0 = olhando para o norte)
		float camPitch = 24.0f;        // inclinação (graus acima do horizonte): menor = mais "deitada"
		float camDist = 190.0f;        // distância até o personagem
		float camFov = 30.0f;          // campo de visão vertical
		float camHeight = 14.0f;       // altura do ponto observado acima dos pés
		float camOrbit = 0.35f;        // quanto a câmera gira para olhar o centro do mapa por trás do personagem
		float camArc = 0.30f;          // afastamento/subida no meio da troca (fração da distância percorrida)
		float camSwing = 7.0f;         // giro lateral durante a troca (graus)
		float camIdle = 1.0f;          // intensidade do movimento ocioso (0 = parada)
		float unitsPerPixel = 0.19f;   // tamanho do sprite no mundo (unidades por pixel do sprite)
		unsigned skyTop = 0xFF4F7FB8, skyHorizon = 0xFFB9D3EA;
		float fogStart = 700.0f, fogEnd = 2600.0f;
		bool water = false;
		int clouds = 40;
		float cloudLevel = -220.0f;
		unsigned cloudColor = 0xFFFFFFFF;
		int stars = 0;
		float particles = 1.0f;
		float glow = 1.0f;
		float supersample = 2.0f;

		// Janela nativa (UINewSelectCharWnd) e posições dos controles relativas a ela
		unsigned nativeWidth = 997, nativeHeight = 626;
		unsigned makeBgWidth = 1028, makeBgHeight = 768; // moldura da tela de criação (cópia da tela anterior)
		Point nativePlay{ 899, 465 };
		Point nativeDelete{ 895, 334 };       // "Deletar Personagem" (agenda a exclusão) / "Cancelar" quando já agendada
		Point nativeDeleteFinal{ 895, 363 };  // "Deletar" quando a exclusão já está agendada
		Point nativePrevPage{ 839, 573 };
		Point nativeNextPage{ 948, 573 };
		Point nativeClose{ 952, 14 };

		// Modo foto: pilhas de chamada (hash) dos desenhos escondidos (interface/personagem)
		std::vector<unsigned> photoHide;

		std::vector<std::wstring> images; // caminhos absolutos
		View defaultView;
		std::vector<View> slotViews;      // índice = slot absoluto; image == -1 usa defaultView
	};

	const std::wstring& BaseDir(); // pasta "charselect\" ao lado do executável
	void Load();
	const Settings& Get();
	View ViewForSlot( int slot ); // [cenas] do slot ou câmera seguindo a posição do personagem

	// Escolha do jogador: seleção personalizada (true) ou a original do Ragnarok.
	// Salva em charselect\preferencias.ini.
	bool CustomActive();

	// Alterações escolhidas no Aplicador Codex (gravadas no executável; DLL solta = todas)
	enum Feature : unsigned {
		FEAT_LOGIN = 1,   // telas de entrada (servidores, login, conectando)
		FEAT_SELECT = 2,  // seleção de personagem panorâmica
		FEAT_MAKE = 4,    // criação de personagem
		FEAT_TOGGLE = 8,  // chave Personalizada/Original para o jogador
		FEAT_COMBAT = 16, // postura e golpe de combate do selecionado
		FEAT_FADE = 32,   // escurecer/clarear mais rápido na troca de mapa e no @refresh
		FEAT_HUD = 64,    // barra de status no jogo (no lugar de "Informações básicas")
		FEAT_ITEMWND = 128, // descrição do item no tema, com comparação ao equipado
		FEAT_SKIN = 256,  // (desativada) janelas escuras
		FEAT_EQUIPWND = 512,  // janela de equipamentos nova
		FEAT_INVWND = 1024,   // inventário novo
		FEAT_SKILLWND = 2048, // habilidades novas
	};
	bool Feature( unsigned f );
	void SetCustomActive( bool on );
}
