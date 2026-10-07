#pragma once

#include <d3d9.h>
#include <string>

// Mapa 3D de verdade (malha gerada por tools\mapa3d.py) desenhado com uma câmera livre.
namespace world3d {
	struct Camera {
		float tx = 0, ty = 0, tz = 0; // ponto observado (mundo)
		float yaw = 0;                // graus; 0 = olhando para o norte do mapa
		float pitch = 30;             // graus acima do horizonte
		float dist = 300;             // distância até o ponto observado
		float fov = 30;               // campo de visão vertical (graus)
		float shiftX = 0, shiftY = 0; // deslocamento da imagem (fração da tela, a partir do centro)
	};

	struct Sky {
		unsigned top = 0xFF4F7FB8, horizon = 0xFFB9D3EA; // degradê do céu
		float fogStart = 700, fogEnd = 2600;             // névoa (distância da câmera)
		bool water = false;                               // plano de água do mapa
		int clouds = 40;                                  // nuvens em volta da ilha
		float cloudLevel = -220;                          // altura média das nuvens
		unsigned cloudColor = 0xFFFFFFFF;                 // tom das nuvens (alfa = opacidade máxima)
		int stars = 0;                                    // estrelas no céu
		float particles = 1.0f;                           // brilhos nos emissores do mapa (0 desliga)
		float glow = 1.0f;                                // halo dos modelos que brilham (0 desliga)
		float supersample = 2.0f;                         // renderiza em N vezes a resolução e reduz (1 = desliga)
	};

	bool Load( const std::wstring& dir );  // pasta com mapa.bin; carrega as texturas no primeiro Draw
	// Pré-carga em segundo plano (lê e decodifica tudo); chamar cedo, com o céu já definido.
	void Preload( const std::wstring& dir );
	// Fora da seleção: sobe as texturas pré-carregadas para a placa (true = pronto)
	bool Warmup( IDirect3DDevice9* dev );
	bool Loaded();
	void SetSky( const Sky& sky );
	bool Draw( IDirect3DDevice9* dev, const Camera& cam, double timeMs );
	void OnDeviceLost();

	// Pela câmera do último Draw: posição na tela e pixels por unidade do mundo naquela profundidade.
	bool Project( float x, float y, float z, float& sx, float& sy, float& pixelsPerUnit );
	// Centro de uma célula do GAT (coordenadas do servidor) em coordenadas do mundo.
	bool CellToWorld( float cellX, float cellY, float& x, float& y, float& z );
	bool CellWalkable( int cellX, int cellY );
	// Colisão da câmera: fração (0..1] da distância livre do alvo até o olho (1 = sem obstáculo).
	float ClearFraction( const Camera& cam );
	void MapCenter( float& x, float& z );
}
