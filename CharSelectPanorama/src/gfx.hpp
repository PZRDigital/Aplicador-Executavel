#pragma once

#include <d3d9.h>
#include <string>
#include <vector>

// Camada de desenho 2D da DLL (retângulos, imagens e texto) sobre o device do cliente.
namespace gfx {
	struct Image {
		IDirect3DTexture9* tex = nullptr;
		unsigned width = 0;
		unsigned height = 0;
	};

	enum class Font {
		Title,      // Cinzel, títulos grandes
		TitleSmall, // Cinzel, rótulos em destaque
		Body,       // Inter regular
		BodyBold,   // Inter semibold
		Small,      // Inter pequeno
		SmallBold,
		Button,     // Inter bold, botões
		Name,       // Inter bold grande, nomes na lista
		COUNT
	};

	enum Align { Left = 0, Center = 1, Right = 2 };

	void Init();                         // registra as fontes de charselect\fonts
	bool IsDrawing();                    // true enquanto a própria DLL desenha (hooks ignoram)
	void SetDrawing( bool on );          // marca desenhos da DLL feitos fora de Begin/End (mapa 3D)
	bool Begin( IDirect3DDevice9* dev ); // salva o estado do cliente e prepara o 2D
	void End();
	void OnDeviceLost();
	void SetUiScale( float scale );      // escala das fontes (resolução)

	// Pixel shader "sharp bilinear" para ampliar pixel art (sprites): pixels do mesmo tamanho e bordas
	// suavizadas, com interpolação ponderada pelo alfa (sem franja da cor transparente).
	// c0 = (largura, altura, 1/largura, 1/altura) da textura; c1.xy = pixels de tela por texel.
	// nullptr se o compilador de shader (d3dcompiler_47.dll) não estiver disponível.
	IDirect3DPixelShader9* SharpShader( IDirect3DDevice9* dev );

	// Carrega PNG/JPG/BMP em textura D3DPOOL_DEFAULT (limita ao tamanho máximo da GPU).
	bool LoadImageFile( IDirect3DDevice9* dev, const std::wstring& path, Image& out );
	void Release( Image& img );
	// Textura a partir de pixels BGRA (pitch = largura * 4)
	bool CreateImage( IDirect3DDevice9* dev, unsigned w, unsigned h, const void* bgra, Image& out );
	// Decodifica PNG/JPG/BMP em BGRA 32 bits (maxSize > 0 reduz imagens maiores que isso).
	// wicFactory: IWICImagingFactory* da thread que chama (nullptr = a da thread principal)
	bool DecodeImageFile( const std::wstring& path, std::vector<unsigned char>& bgra, unsigned& width, unsigned& height, unsigned maxSize = 0,
		void* wicFactory = nullptr );
	// Elipse luminosa branca (anel + preenchimento suave) para marcar o chão sob o personagem.
	bool CreateGlow( IDirect3DDevice9* dev, Image& out );
	// Engrenagem branca (selo dos ícones de classe) e disco branco suave, para tingir no desenho.
	bool CreateGear( IDirect3DDevice9* dev, Image& out );
	bool CreateDisc( IDirect3DDevice9* dev, Image& out );

	void Rect( float x, float y, float w, float h, D3DCOLOR color );
	void RectV( float x, float y, float w, float h, D3DCOLOR top, D3DCOLOR bottom );
	void RectH( float x, float y, float w, float h, D3DCOLOR left, D3DCOLOR right );
	void Frame( float x, float y, float w, float h, D3DCOLOR color, float thickness = 1.0f );
	void Triangle( float x1, float y1, float x2, float y2, float x3, float y3, D3DCOLOR color );
	void Line( float x1, float y1, float x2, float y2, D3DCOLOR color, float thickness = 1.0f );
	void Circle( float cx, float cy, float radius, D3DCOLOR color );
	// Retângulo de cantos arredondados (disc = textura de CreateDisc para os cantos)
	void RoundRect( float x, float y, float w, float h, float r, D3DCOLOR color, const Image& disc );
	void DrawImage( const Image& img, float x, float y, float w, float h, D3DCOLOR color = 0xFFFFFFFF,
		float u0 = 0.0f, float v0 = 0.0f, float u1 = 1.0f, float v1 = 1.0f );

	// Texto em UTF-16. Retorna a largura desenhada. maxWidth > 0 corta com reticências.
	float Text( Font font, const std::wstring& text, float x, float y, D3DCOLOR color,
		int align = Left, float maxWidth = 0.0f, bool shadow = true );
	float TextWidth( Font font, const std::wstring& text );
	float LineHeight( Font font );

	// Cores com alfa em 0..1
	inline D3DCOLOR Rgba( int r, int g, int b, float a ){
		int ai = (int)( a * 255.0f + 0.5f );
		return D3DCOLOR_ARGB( ai < 0 ? 0 : ( ai > 255 ? 255 : ai ), r, g, b );
	}
}
