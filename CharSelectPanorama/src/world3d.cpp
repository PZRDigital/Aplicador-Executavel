#include "world3d.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <vector>

#include "gfx.hpp"
#include "log.hpp"
#include "vfs.hpp"
#include <objbase.h>
#include <wincodec.h>

namespace {
	constexpr float PI = 3.14159265f;

	// Formato de mapa.bin (tools\mapa3d.py)
	struct MapVertex {
		float x, y, z;
		D3DCOLOR color;
		float u, v;
		float u2, v2;
	};
	constexpr DWORD MAP_FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;

	struct SkyVertex {
		float x, y, z, rhw;
		D3DCOLOR color;
	};
	constexpr DWORD SKY_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

	struct BillboardVertex {
		float x, y, z;
		D3DCOLOR color;
		float u, v;
	};
	constexpr DWORD BILLBOARD_FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

	enum Kind { KIND_GROUND = 0, KIND_MODEL = 1, KIND_BLEND = 2 };

	struct Batch {
		int tex, kind, cull, start, count;
	};

	struct Cloud {
		float x, y, z, size, speed, alpha;
		int tex;
	};

	struct Mat {
		float m[4][4];
	};

	struct PointLight {
		float x, y, z, r, g, b, range;
	};
	struct Emitter {
		float x, y, z;
		int id;
	};
	struct Glow {
		float x, y, z, r, g, b, size;
	};

	// ---------------------------------------------------------------- dados (independentes do device)
	std::wstring g_dir;
	bool g_parsed = false;
	int g_gndW = 0, g_gndH = 0;
	bool g_hasWater = false;
	float g_waterLevel = 0, g_waterAmp = 0, g_waterSpeed = 0, g_waterPitch = 0, g_waterAnim = 3;
	int g_waterFrames = 0;
	int g_texCount = 0;
	std::vector<Batch> g_batches;
	std::vector<MapVertex> g_vertices;
	int g_gatW = 0, g_gatH = 0;
	std::vector<float> g_gatHeight;
	std::vector<unsigned char> g_gatType;
	std::vector<Cloud> g_clouds;
	std::vector<PointLight> g_lights;
	std::vector<Emitter> g_emitters;
	std::vector<Glow> g_glows;
	world3d::Sky g_sky;

	// Ocupação da geometria opaca (colisão da câmera): altura mínima/máxima por célula
	constexpr float OCC_CELL = 4.0f;
	int g_occW = 0, g_occH = 0;
	std::vector<float> g_occLo, g_occHi;

	// ---------------------------------------------------------------- recursos do device
	IDirect3DDevice9* g_dev = nullptr;
	bool g_gpuTried = false, g_gpuOk = false;
	IDirect3DVertexBuffer9* g_vb = nullptr;
	std::vector<IDirect3DTexture9*> g_textures;
	IDirect3DTexture9* g_lightmap = nullptr;
	std::vector<IDirect3DTexture9*> g_water;
	std::vector<IDirect3DTexture9*> g_cloudTex;
	IDirect3DTexture9* g_glowTex = nullptr;  // halo radial (procedural)
	IDirect3DTexture9* g_sparkTex = nullptr; // brilho em cruz (procedural)
	IDirect3DSurface9* g_depth = nullptr;
	UINT g_depthW = 0, g_depthH = 0;
	D3DMULTISAMPLE_TYPE g_depthMs = D3DMULTISAMPLE_NONE;
	DWORD g_depthMsQ = 0;
	IDirect3DStateBlock9* g_state = nullptr;
	IDirect3DTexture9* g_ssTex = nullptr;  // alvo da superamostragem
	UINT g_ssW = 0, g_ssH = 0;

	// Câmera do último quadro
	bool g_viewValid = false;
	Mat g_viewProj;
	float g_projScaleY = 1;
	D3DVIEWPORT9 g_vp;

	Mat Identity(){
		Mat r = {};
		for( int i = 0; i < 4; i++ ) r.m[i][i] = 1;
		return r;
	}

	Mat Mul( const Mat& a, const Mat& b ){
		Mat r = {};
		for( int i = 0; i < 4; i++ )
			for( int j = 0; j < 4; j++ )
				for( int k = 0; k < 4; k++ )
					r.m[i][j] += a.m[i][k] * b.m[k][j];
		return r;
	}

	D3DMATRIX ToD3D( const Mat& m ){
		D3DMATRIX r;
		memcpy( &r, m.m, sizeof( r ) );
		return r;
	}

	struct Vec3 {
		float x, y, z;
	};
	Vec3 Sub( Vec3 a, Vec3 b ){ return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	Vec3 Cross( Vec3 a, Vec3 b ){ return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float Dot( Vec3 a, Vec3 b ){ return a.x * b.x + a.y * b.y + a.z * b.z; }
	Vec3 Norm( Vec3 a ){
		float l = sqrtf( Dot( a, a ) );
		return l > 0 ? Vec3{ a.x / l, a.y / l, a.z / l } : Vec3{ 0, 0, 0 };
	}

	// Vista "mão esquerda" sobre o mundo do BrowEdit: x = direita, y = cima, z = para frente.
	// (espelha só o eixo de profundidade, então a imagem é a mesma de uma câmera comum)
	Vec3 g_camRight, g_camUp, g_camForward, g_camEye;

	Mat LookAt( Vec3 eye, Vec3 at ){
		Vec3 f = Norm( Sub( at, eye ) );
		Vec3 r = Norm( Cross( f, { 0, 1, 0 } ) );
		Vec3 u = Cross( r, f );
		g_camRight = r;
		g_camUp = u;
		g_camForward = f;
		g_camEye = eye;
		Mat m = Identity();
		m.m[0][0] = r.x; m.m[0][1] = u.x; m.m[0][2] = f.x;
		m.m[1][0] = r.y; m.m[1][1] = u.y; m.m[1][2] = f.y;
		m.m[2][0] = r.z; m.m[2][1] = u.z; m.m[2][2] = f.z;
		m.m[3][0] = -Dot( r, eye ); m.m[3][1] = -Dot( u, eye ); m.m[3][2] = -Dot( f, eye );
		return m;
	}

	Mat Perspective( float fovDeg, float aspect, float zn, float zf, float shiftX, float shiftY ){
		float ys = 1.0f / tanf( fovDeg * PI / 360.0f );
		float xs = ys / aspect;
		Mat m = {};
		m.m[0][0] = xs;
		m.m[1][1] = ys;
		m.m[2][0] = shiftX * 2.0f;  // desloca a imagem (NDC) sem mudar a perspectiva
		m.m[2][1] = -shiftY * 2.0f;
		m.m[2][2] = zf / ( zf - zn );
		m.m[2][3] = 1;
		m.m[3][2] = -zn * zf / ( zf - zn );
		return m;
	}

	// ---------------------------------------------------------------- leitura dos arquivos
	bool ReadFile( const std::wstring& path, std::vector<unsigned char>& out ){
		return vfs::Read( path, out );
	}

	bool ReadFileDisk( const std::wstring& path, std::vector<unsigned char>& out ){
		FILE* f = _wfopen( path.c_str(), L"rb" );
		if( f == nullptr ) return false;
		fseek( f, 0, SEEK_END );
		long n = ftell( f );
		fseek( f, 0, SEEK_SET );
		out.resize( n > 0 ? (size_t)n : 0 );
		bool ok = n > 0 && fread( out.data(), 1, (size_t)n, f ) == (size_t)n;
		fclose( f );
		return ok;
	}

	struct Reader {
		const unsigned char* p;
		const unsigned char* end;
		bool ok = true;
		template <typename T> T Get(){
			T v{};
			if( p + sizeof( T ) > end ){ ok = false; return v; }
			memcpy( &v, p, sizeof( T ) );
			p += sizeof( T );
			return v;
		}
	};

	bool ParseMap( const std::wstring& dir ){
		std::vector<unsigned char> d;
		if( !ReadFile( dir + L"mapa.bin", d ) || d.size() < 8 || memcmp( d.data(), "CSM3", 4 ) != 0 ){
			logger::Write( "ERRO: mapa 3D invalido ou ausente: %lsmapa.bin", dir.c_str() );
			return false;
		}
		Reader r{ d.data() + 8, d.data() + d.size() };
		g_gndW = r.Get<int>();
		g_gndH = r.Get<int>();
		r.Get<float>(); r.Get<float>();
		g_hasWater = r.Get<int>() != 0;
		g_waterLevel = r.Get<float>();
		g_waterAmp = r.Get<float>();
		g_waterSpeed = r.Get<float>();
		g_waterPitch = r.Get<float>();
		g_waterAnim = r.Get<float>();
		g_waterFrames = r.Get<int>();
		for( int i = 0; i < 6; i++ ) r.Get<float>(); // luz (já aplicada nos vértices)
		g_texCount = r.Get<int>();
		int batchCount = r.Get<int>();
		for( int i = 0; i < g_texCount; i++ ){ r.Get<int>(); r.Get<int>(); }
		g_batches.resize( std::max( 0, batchCount ) );
		for( Batch& b : g_batches ){
			b.tex = r.Get<int>(); b.kind = r.Get<int>(); b.cull = r.Get<int>(); b.start = r.Get<int>(); b.count = r.Get<int>();
		}
		int vertexCount = r.Get<int>();
		if( !r.ok || vertexCount <= 0 || r.p + (size_t)vertexCount * sizeof( MapVertex ) > r.end ){
			logger::Write( "ERRO: mapa.bin truncado" );
			return false;
		}
		g_vertices.resize( vertexCount );
		memcpy( g_vertices.data(), r.p, (size_t)vertexCount * sizeof( MapVertex ) );
		r.p += (size_t)vertexCount * sizeof( MapVertex );

		// versão 2: luzes, emissores e halos
		g_lights.clear();
		g_emitters.clear();
		g_glows.clear();
		unsigned version = 0;
		memcpy( &version, d.data() + 4, 4 );
		if( version >= 2 ){
			int n = r.Get<int>();
			for( int i = 0; i < n && r.ok; i++ ){
				PointLight l;
				l.x = r.Get<float>(); l.y = r.Get<float>(); l.z = r.Get<float>();
				l.r = r.Get<float>(); l.g = r.Get<float>(); l.b = r.Get<float>(); l.range = r.Get<float>();
				g_lights.push_back( l );
			}
			n = r.Get<int>();
			for( int i = 0; i < n && r.ok; i++ ){
				Emitter e;
				e.x = r.Get<float>(); e.y = r.Get<float>(); e.z = r.Get<float>(); e.id = r.Get<int>();
				g_emitters.push_back( e );
			}
			n = r.Get<int>();
			for( int i = 0; i < n && r.ok; i++ ){
				Glow g;
				g.x = r.Get<float>(); g.y = r.Get<float>(); g.z = r.Get<float>();
				g.r = r.Get<float>(); g.g = r.Get<float>(); g.b = r.Get<float>(); g.size = r.Get<float>();
				g_glows.push_back( g );
			}
		}

		// mapa de ocupação
		g_occW = (int)( 10.0f * g_gndW / OCC_CELL ) + 4;
		g_occH = (int)( ( 10.0f * g_gndH + 20.0f ) / OCC_CELL ) + 4;
		g_occLo.assign( (size_t)g_occW * g_occH, 1e9f );
		g_occHi.assign( (size_t)g_occW * g_occH, -1e9f );
		for( const Batch& b : g_batches ){
			if( b.kind == 2 ) continue; // semitransparente não bloqueia
			for( int i = b.start; i + 2 < b.start + b.count && i + 2 < vertexCount; i += 3 ){
				const MapVertex* v = &g_vertices[i];
				float x0 = std::min( { v[0].x, v[1].x, v[2].x } ), x1 = std::max( { v[0].x, v[1].x, v[2].x } );
				float z0 = std::min( { v[0].z, v[1].z, v[2].z } ), z1 = std::max( { v[0].z, v[1].z, v[2].z } );
				float y0 = std::min( { v[0].y, v[1].y, v[2].y } ), y1 = std::max( { v[0].y, v[1].y, v[2].y } );
				int a = std::max( 0, (int)( x0 / OCC_CELL ) ), bb = std::min( g_occW - 1, (int)( x1 / OCC_CELL ) );
				int c = std::max( 0, (int)( z0 / OCC_CELL ) ), e = std::min( g_occH - 1, (int)( z1 / OCC_CELL ) );
				for( int zz = c; zz <= e; zz++ ){
					for( int xx = a; xx <= bb; xx++ ){
						size_t k = (size_t)zz * g_occW + xx;
						g_occLo[k] = std::min( g_occLo[k], y0 );
						g_occHi[k] = std::max( g_occHi[k], y1 );
					}
				}
			}
		}

		std::vector<unsigned char> gat;
		if( ReadFile( dir + L"gat.bin", gat ) && gat.size() >= 8 ){
			Reader g{ gat.data(), gat.data() + gat.size() };
			g_gatW = g.Get<int>();
			g_gatH = g.Get<int>();
			size_t n = (size_t)std::max( 0, g_gatW ) * std::max( 0, g_gatH );
			g_gatHeight.resize( n );
			g_gatType.resize( n );
			for( size_t i = 0; i < n && g.ok; i++ ){
				g_gatHeight[i] = g.Get<float>();
				g_gatType[i] = g.Get<unsigned char>();
			}
			if( !g.ok ){
				g_gatW = g_gatH = 0;
			}
		}

		logger::Write( "Mapa 3D: %dx%d, %d vertices, %d lotes, %d texturas, agua=%d (%d quadros), gat %dx%d, %u luzes, %u emissores, %u halos",
			g_gndW, g_gndH, vertexCount, batchCount, g_texCount, g_hasWater, g_waterFrames, g_gatW, g_gatH,
			(unsigned)g_lights.size(), (unsigned)g_emitters.size(), (unsigned)g_glows.size() );
		return true;
	}

	void MakeClouds(){
		g_clouds.clear();
		float cx = 5.0f * g_gndW + 10.0f, cz = 5.0f * g_gndH + 10.0f;
		unsigned seed = 12345;
		auto rnd = [&seed](){ seed = seed * 1103515245u + 12345u; return ( ( seed >> 8 ) & 0xFFFF ) / 65535.0f; };
		int count = std::max( 0, g_sky.clouds );
		int texCount = std::max<int>( 1, (int)g_cloudTex.size() );
		for( int i = 0; i < count; i++ ){
			Cloud c;
			float ang = rnd() * 2 * PI;
			float rad = 180.0f + rnd() * 1400.0f;
			c.x = cx + cosf( ang ) * rad;
			c.z = cz + sinf( ang ) * rad;
			c.y = g_sky.cloudLevel + ( rnd() - 0.5f ) * 260.0f - rad * 0.12f;
			c.size = 220.0f + rnd() * 420.0f;
			c.speed = 4.0f + rnd() * 10.0f;
			c.alpha = 0.35f + rnd() * 0.45f;
			c.tex = (int)( rnd() * texCount ) % texCount;
			g_clouds.push_back( c );
		}
	}

	// ---------------------------------------------------------------- texturas com mipmaps
	void Downsample( const std::vector<unsigned char>& src, unsigned w, unsigned h, std::vector<unsigned char>& dst, unsigned& nw, unsigned& nh ){
		nw = std::max( 1u, w / 2 );
		nh = std::max( 1u, h / 2 );
		dst.assign( (size_t)nw * nh * 4, 0 );
		for( unsigned y = 0; y < nh; y++ ){
			for( unsigned x = 0; x < nw; x++ ){
				unsigned sx0 = std::min( w - 1, x * 2 ), sx1 = std::min( w - 1, x * 2 + 1 );
				unsigned sy0 = std::min( h - 1, y * 2 ), sy1 = std::min( h - 1, y * 2 + 1 );
				const unsigned char* p[4] = {
					&src[( (size_t)sy0 * w + sx0 ) * 4], &src[( (size_t)sy0 * w + sx1 ) * 4],
					&src[( (size_t)sy1 * w + sx0 ) * 4], &src[( (size_t)sy1 * w + sx1 ) * 4] };
				// cor ponderada pelo alfa (sem franja escura nas bordas recortadas)
				unsigned a = p[0][3] + p[1][3] + p[2][3] + p[3][3];
				unsigned char* o = &dst[( (size_t)y * nw + x ) * 4];
				for( int c = 0; c < 3; c++ ){
					if( a > 0 ){
						o[c] = (unsigned char)( ( p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] + p[3][c] * p[3][3] ) / a );
					}else{
						o[c] = (unsigned char)( ( p[0][c] + p[1][c] + p[2][c] + p[3][c] ) / 4 );
					}
				}
				o[3] = (unsigned char)( a / 4 );
			}
		}
	}

	// Textura decodificada com a cadeia de mipmaps pronta (feita na CPU, pode ser em outra thread)
	struct Decoded {
		bool ok = false;
		unsigned w = 0, h = 0;
		std::vector<std::vector<unsigned char>> levels;
	};

	Decoded DecodeChain( const std::wstring& path, bool mips, void* wic ){
		Decoded d;
		std::vector<unsigned char> px;
		if( !gfx::DecodeImageFile( path, px, d.w, d.h, 4096, wic ) ){
			return d;
		}
		d.levels.push_back( std::move( px ) );
		unsigned cw = d.w, ch = d.h;
		while( mips && ( cw > 1 || ch > 1 ) ){
			std::vector<unsigned char> next;
			unsigned nw, nh;
			Downsample( d.levels.back(), cw, ch, next, nw, nh );
			d.levels.push_back( std::move( next ) );
			cw = nw;
			ch = nh;
		}
		d.ok = true;
		return d;
	}

	IDirect3DTexture9* Upload( IDirect3DDevice9* dev, const Decoded& d ){
		if( !d.ok ) return nullptr;
		UINT levels = (UINT)d.levels.size();
		IDirect3DTexture9* staging = nullptr;
		IDirect3DTexture9* tex = nullptr;
		if( FAILED( dev->CreateTexture( d.w, d.h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr ) ) ){
			return nullptr;
		}
		unsigned cw = d.w, ch = d.h;
		for( UINT level = 0; level < levels; level++ ){
			D3DLOCKED_RECT lr;
			if( SUCCEEDED( staging->LockRect( level, &lr, nullptr, 0 ) ) ){
				for( unsigned y = 0; y < ch; y++ ){
					memcpy( static_cast<BYTE*>( lr.pBits ) + y * lr.Pitch, &d.levels[level][(size_t)y * cw * 4], cw * 4 );
				}
				staging->UnlockRect( level );
			}
			cw = std::max( 1u, cw / 2 );
			ch = std::max( 1u, ch / 2 );
		}
		if( FAILED( dev->CreateTexture( d.w, d.h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) )
			|| FAILED( dev->UpdateTexture( staging, tex ) ) ){
			if( tex ) tex->Release();
			tex = nullptr;
		}
		staging->Release();
		return tex;
	}

	// Pré-carga: texturas já decodificadas, por caminho (preenchido pela thread; lido depois que ela termina)
	std::map<std::wstring, Decoded> g_pre;
	HANDLE g_preThread = nullptr;
	volatile bool g_preDone = false;

	void WaitPreload(){
		if( g_preThread ){
			WaitForSingleObject( g_preThread, INFINITE );
			CloseHandle( g_preThread );
			g_preThread = nullptr;
		}
	}

	IDirect3DTexture9* LoadTexture( IDirect3DDevice9* dev, const std::wstring& path, bool mips ){
		auto it = g_pre.find( path );
		if( it != g_pre.end() ){
			IDirect3DTexture9* t = Upload( dev, it->second );
			g_pre.erase( it ); // libera a memória da CPU
			return t;
		}
		Decoded d = DecodeChain( path, mips, nullptr );
		if( !d.ok ){
			logger::Write( "AVISO: textura 3D nao carregada: %ls", path.c_str() );
			return nullptr;
		}
		return Upload( dev, d );
	}

	IDirect3DTexture9* TextureFromPixels( IDirect3DDevice9* dev, unsigned w, unsigned h, const std::vector<unsigned char>& bgra ){
		IDirect3DTexture9* staging = nullptr;
		IDirect3DTexture9* tex = nullptr;
		UINT levels = 1;
		for( unsigned m = std::max( w, h ); m > 1; m /= 2 ) levels++;
		if( FAILED( dev->CreateTexture( w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr ) ) ) return nullptr;
		std::vector<unsigned char> cur = bgra, next;
		unsigned cw = w, ch = h;
		for( UINT level = 0; level < levels; level++ ){
			D3DLOCKED_RECT lr;
			if( SUCCEEDED( staging->LockRect( level, &lr, nullptr, 0 ) ) ){
				for( unsigned y = 0; y < ch; y++ ) memcpy( static_cast<BYTE*>( lr.pBits ) + y * lr.Pitch, &cur[(size_t)y * cw * 4], cw * 4 );
				staging->UnlockRect( level );
			}
			if( level + 1 < levels ){
				unsigned nw, nh;
				Downsample( cur, cw, ch, next, nw, nh );
				cur.swap( next );
				cw = nw;
				ch = nh;
			}
		}
		if( FAILED( dev->CreateTexture( w, h, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) )
			|| FAILED( dev->UpdateTexture( staging, tex ) ) ){
			if( tex ) tex->Release();
			tex = nullptr;
		}
		staging->Release();
		return tex;
	}

	// Halo: branco com alfa em queda suave (usado com mistura aditiva e cor no vértice)
	IDirect3DTexture9* MakeGlow( IDirect3DDevice9* dev ){
		const unsigned n = 128;
		std::vector<unsigned char> px( n * n * 4 );
		for( unsigned y = 0; y < n; y++ ){
			for( unsigned x = 0; x < n; x++ ){
				float dx = ( x + 0.5f ) / n * 2 - 1, dy = ( y + 0.5f ) / n * 2 - 1;
				float r = sqrtf( dx * dx + dy * dy );
				float a = r >= 1 ? 0 : powf( 1 - r, 2.2f );
				unsigned char* p = &px[( y * n + x ) * 4];
				p[0] = p[1] = p[2] = 255;
				p[3] = (unsigned char)( a * 255 );
			}
		}
		return TextureFromPixels( dev, n, n, px );
	}

	// Brilho: núcleo pequeno + cruz fina
	IDirect3DTexture9* MakeSpark( IDirect3DDevice9* dev ){
		const unsigned n = 64;
		std::vector<unsigned char> px( n * n * 4 );
		for( unsigned y = 0; y < n; y++ ){
			for( unsigned x = 0; x < n; x++ ){
				float dx = ( x + 0.5f ) / n * 2 - 1, dy = ( y + 0.5f ) / n * 2 - 1;
				float r = sqrtf( dx * dx + dy * dy );
				float core = r >= 1 ? 0 : powf( 1 - r, 3.0f );
				float cross = std::max( 0.0f, 1 - fabsf( dx ) * 14 ) * std::max( 0.0f, 1 - fabsf( dy ) ) +
					std::max( 0.0f, 1 - fabsf( dy ) * 14 ) * std::max( 0.0f, 1 - fabsf( dx ) );
				float a = std::min( 1.0f, core + cross * 0.55f );
				unsigned char* p = &px[( y * n + x ) * 4];
				p[0] = p[1] = p[2] = 255;
				p[3] = (unsigned char)( a * 255 );
			}
		}
		return TextureFromPixels( dev, n, n, px );
	}

	void ReleaseGpu(){
		if( g_vb ){ g_vb->Release(); g_vb = nullptr; }
		for( auto*& t : g_textures ) if( t ){ t->Release(); t = nullptr; }
		for( auto*& t : g_water ) if( t ){ t->Release(); t = nullptr; }
		for( auto*& t : g_cloudTex ) if( t ){ t->Release(); t = nullptr; }
		g_textures.clear();
		g_water.clear();
		g_cloudTex.clear();
		if( g_lightmap ){ g_lightmap->Release(); g_lightmap = nullptr; }
		if( g_glowTex ){ g_glowTex->Release(); g_glowTex = nullptr; }
		if( g_sparkTex ){ g_sparkTex->Release(); g_sparkTex = nullptr; }
		if( g_depth ){ g_depth->Release(); g_depth = nullptr; }
		if( g_state ){ g_state->Release(); g_state = nullptr; }
		if( g_ssTex ){ g_ssTex->Release(); g_ssTex = nullptr; }
		g_ssW = g_ssH = 0;
		g_gpuTried = false;
		g_gpuOk = false;
	}

	bool CreateGpu( IDirect3DDevice9* dev ){
		g_gpuTried = true;
		DWORD t0 = GetTickCount();
		UINT bytes = (UINT)( g_vertices.size() * sizeof( MapVertex ) );
		if( FAILED( dev->CreateVertexBuffer( bytes, D3DUSAGE_WRITEONLY, MAP_FVF, D3DPOOL_DEFAULT, &g_vb, nullptr ) ) ){
			logger::Write( "ERRO: CreateVertexBuffer (%u bytes)", bytes );
			return false;
		}
		void* p = nullptr;
		if( FAILED( g_vb->Lock( 0, bytes, &p, 0 ) ) ){
			return false;
		}
		memcpy( p, g_vertices.data(), bytes );
		g_vb->Unlock();

		g_textures.resize( g_texCount, nullptr );
		for( int i = 0; i < g_texCount; i++ ){
			wchar_t name[32];
			swprintf( name, 32, L"t%03d.png", i );
			g_textures[i] = LoadTexture( dev, g_dir + name, true );
		}
		g_lightmap = LoadTexture( dev, g_dir + L"luz.png", false );
		if( g_sky.water && g_hasWater ){
			for( int i = 0; i < g_waterFrames; i++ ){
				wchar_t name[32];
				swprintf( name, 32, L"agua\\%02d.png", i );
				g_water.push_back( LoadTexture( dev, g_dir + name, true ) );
			}
		}
		for( int i = 0; i < 16; i++ ){
			wchar_t name[32];
			swprintf( name, 32, L"nuvens\\%02d.png", i );
			if( !vfs::Exists( g_dir + name ) ) break;
			IDirect3DTexture9* t = LoadTexture( dev, g_dir + name, true );
			if( t ) g_cloudTex.push_back( t );
		}
		g_glowTex = MakeGlow( dev );
		g_sparkTex = MakeSpark( dev );
		MakeClouds();
		logger::Write( "Mapa 3D na GPU em %u ms (%d texturas, %u nuvens)", GetTickCount() - t0, g_texCount, (unsigned)g_cloudTex.size() );
		return true;
	}

	// Profundidade própria (mesmo tamanho e multiamostragem do alvo atual)
	bool EnsureDepth( IDirect3DDevice9* dev ){
		IDirect3DSurface9* rt = nullptr;
		if( FAILED( dev->GetRenderTarget( 0, &rt ) ) || rt == nullptr ){
			return false;
		}
		D3DSURFACE_DESC desc;
		rt->GetDesc( &desc );
		rt->Release();
		if( g_depth && g_depthW == desc.Width && g_depthH == desc.Height && g_depthMs == desc.MultiSampleType && g_depthMsQ == desc.MultiSampleQuality ){
			return true;
		}
		if( g_depth ){ g_depth->Release(); g_depth = nullptr; }
		if( FAILED( dev->CreateDepthStencilSurface( desc.Width, desc.Height, D3DFMT_D24S8, desc.MultiSampleType,
			desc.MultiSampleQuality, FALSE, &g_depth, nullptr ) ) ){
			logger::Write( "ERRO: CreateDepthStencilSurface %ux%u", desc.Width, desc.Height );
			return false;
		}
		g_depthW = desc.Width;
		g_depthH = desc.Height;
		g_depthMs = desc.MultiSampleType;
		g_depthMsQ = desc.MultiSampleQuality;
		return true;
	}

	void DrawSky( IDirect3DDevice9* dev, const D3DVIEWPORT9& vp ){
		float x0 = (float)vp.X - 0.5f, y0 = (float)vp.Y - 0.5f;
		float x1 = x0 + vp.Width, y1 = y0 + vp.Height;
		SkyVertex q[4] = {
			{ x0, y0, 0.999f, 1, g_sky.top },
			{ x1, y0, 0.999f, 1, g_sky.top },
			{ x0, y1, 0.999f, 1, g_sky.horizon },
			{ x1, y1, 0.999f, 1, g_sky.horizon },
		};
		dev->SetFVF( SKY_FVF );
		dev->SetTexture( 0, nullptr );
		dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2 );
		dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2 );
		dev->SetRenderState( D3DRS_ZENABLE, FALSE );
		dev->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
		dev->SetRenderState( D3DRS_FOGENABLE, FALSE );
		dev->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
		dev->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, q, sizeof( SkyVertex ) );
	}

	void SetSampler( IDirect3DDevice9* dev, DWORD stage, bool wrap, bool mip ){
		D3DCAPS9 caps;
		dev->GetDeviceCaps( &caps );
		bool aniso = mip && ( caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC ) && caps.MaxAnisotropy >= 2;
		dev->SetSamplerState( stage, D3DSAMP_MINFILTER, aniso ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR );
		dev->SetSamplerState( stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
		dev->SetSamplerState( stage, D3DSAMP_MIPFILTER, mip ? D3DTEXF_LINEAR : D3DTEXF_NONE );
		dev->SetSamplerState( stage, D3DSAMP_MAXANISOTROPY, aniso ? std::min<DWORD>( 8, caps.MaxAnisotropy ) : 1 );
		dev->SetSamplerState( stage, D3DSAMP_ADDRESSU, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP );
		dev->SetSamplerState( stage, D3DSAMP_ADDRESSV, wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP );
		dev->SetSamplerState( stage, D3DSAMP_MIPMAPLODBIAS, 0 );
	}

	void StageModulate( IDirect3DDevice9* dev ){
		dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
		dev->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		dev->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
		dev->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
		dev->SetTextureStageState( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE );
		dev->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, 0 );
		dev->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE );
	}

	// chão: textura * cor * sombra(lightmap.a) + cor do lightmap
	void StagesGround( IDirect3DDevice9* dev, bool on ){
		if( on && g_lightmap ){
			dev->SetTexture( 1, g_lightmap );
			dev->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_MODULATE );
			dev->SetTextureStageState( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE );
			dev->SetTextureStageState( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
			dev->SetTextureStageState( 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT );
			dev->SetTextureStageState( 1, D3DTSS_TEXCOORDINDEX, 1 );
			dev->SetTexture( 2, g_lightmap );
			dev->SetTextureStageState( 2, D3DTSS_COLOROP, D3DTOP_ADD );
			dev->SetTextureStageState( 2, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			dev->SetTextureStageState( 2, D3DTSS_COLORARG2, D3DTA_CURRENT );
			dev->SetTextureStageState( 2, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
			dev->SetTextureStageState( 2, D3DTSS_ALPHAARG1, D3DTA_CURRENT );
			dev->SetTextureStageState( 2, D3DTSS_TEXCOORDINDEX, 1 );
			dev->SetTextureStageState( 3, D3DTSS_COLOROP, D3DTOP_DISABLE );
			dev->SetTextureStageState( 3, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
			SetSampler( dev, 1, false, false );
			SetSampler( dev, 2, false, false );
		}else{
			dev->SetTexture( 1, nullptr );
			dev->SetTexture( 2, nullptr );
			dev->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
			dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
		}
	}

	void DrawBatches( IDirect3DDevice9* dev, int kind ){
		for( const Batch& b : g_batches ){
			if( b.kind != kind || b.count <= 0 ) continue;
			dev->SetTexture( 0, b.tex >= 0 && b.tex < (int)g_textures.size() ? g_textures[b.tex] : nullptr );
			dev->SetRenderState( D3DRS_CULLMODE, b.cull ? D3DCULL_CW : D3DCULL_NONE );
			dev->DrawPrimitive( D3DPT_TRIANGLELIST, b.start, b.count / 3 );
		}
	}

	void DrawWater( IDirect3DDevice9* dev, double timeMs ){
		if( !g_sky.water || !g_hasWater || g_water.empty() ) return;
		int frame = (int)( timeMs / 1000.0 * 60.0 / std::max( 1.0f, g_waterAnim ) ) % (int)g_water.size();
		if( g_water[frame] == nullptr ) return;
		const int N = 40;
		float w = 10.0f * g_gndW, h = 10.0f * g_gndH;
		float t = (float)( timeMs / 1000.0 );
		static std::vector<BillboardVertex> v;
		v.clear();
		D3DCOLOR c = D3DCOLOR_ARGB( 144, 255, 255, 255 );
		auto P = [&]( int i, int j ){
			float x = w * i / N, z = 10.0f + h * j / N;
			float y = g_waterLevel + g_waterAmp * sinf( ( x + z ) * 0.02f * g_waterPitch / 50.0f + t * g_waterSpeed );
			return BillboardVertex{ x, y, z, c, x / 20.0f, z / 20.0f };
		};
		for( int j = 0; j < N; j++ ){
			for( int i = 0; i < N; i++ ){
				BillboardVertex a = P( i, j ), b = P( i + 1, j ), cc = P( i, j + 1 ), d = P( i + 1, j + 1 );
				v.push_back( a ); v.push_back( b ); v.push_back( d );
				v.push_back( a ); v.push_back( d ); v.push_back( cc );
			}
		}
		dev->SetFVF( BILLBOARD_FVF );
		dev->SetTexture( 0, g_water[frame] );
		dev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
		dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, (UINT)v.size() / 3, v.data(), sizeof( BillboardVertex ) );
	}

	void DrawClouds( IDirect3DDevice9* dev, double timeMs ){
		if( g_cloudTex.empty() || g_clouds.empty() ) return;
		float t = (float)( timeMs / 1000.0 );
		float span = 10.0f * std::max( g_gndW, g_gndH ) + 3000.0f;
		struct Item { float d; int i; float x; };
		static std::vector<Item> order;
		order.clear();
		for( int i = 0; i < (int)g_clouds.size(); i++ ){
			const Cloud& c = g_clouds[i];
			// deriva lenta para o leste, volta pelo outro lado
			float x = c.x + fmodf( c.speed * t, span );
			float base = 5.0f * g_gndW + 10.0f;
			while( x > base + span * 0.5f ) x -= span;
			Vec3 d = Sub( { x, c.y, c.z }, g_camEye );
			order.push_back( { Dot( d, g_camForward ), i, x } );
		}
		std::sort( order.begin(), order.end(), []( const Item& a, const Item& b ){ return a.d > b.d; } );
		dev->SetFVF( BILLBOARD_FVF );
		dev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
		for( const Item& it : order ){
			if( it.d < 20.0f ) continue;
			const Cloud& c = g_clouds[it.i];
			// some suavemente perto da câmera
			float fade = std::min( 1.0f, ( it.d - 20.0f ) / 250.0f );
			float maxA = ( ( g_sky.cloudColor >> 24 ) & 0xFF ) / 255.0f;
			D3DCOLOR col = ( (DWORD)( 255 * c.alpha * fade * maxA ) << 24 ) | ( g_sky.cloudColor & 0x00FFFFFF );
			float s = c.size * 0.5f;
			Vec3 r = { g_camRight.x * s, g_camRight.y * s, g_camRight.z * s };
			Vec3 u = { g_camUp.x * s * 0.6f, g_camUp.y * s * 0.6f, g_camUp.z * s * 0.6f };
			BillboardVertex q[4] = {
				{ it.x - r.x + u.x, c.y - r.y + u.y, c.z - r.z + u.z, col, 0, 0 },
				{ it.x + r.x + u.x, c.y + r.y + u.y, c.z + r.z + u.z, col, 1, 0 },
				{ it.x - r.x - u.x, c.y - r.y - u.y, c.z - r.z - u.z, col, 0, 1 },
				{ it.x + r.x - u.x, c.y + r.y - u.y, c.z + r.z - u.z, col, 1, 1 },
			};
			dev->SetTexture( 0, g_cloudTex[c.tex % g_cloudTex.size()] );
			dev->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, q, sizeof( BillboardVertex ) );
		}
	}

	float Hash01( unsigned n ){
		n = ( n << 13 ) ^ n;
		n = n * ( n * n * 15731u + 789221u ) + 1376312589u;
		return ( n & 0x7FFFFFFF ) / 2147483647.0f;
	}

	void Billboard( IDirect3DDevice9* dev, float x, float y, float z, float w, float h, D3DCOLOR col ){
		Vec3 r = { g_camRight.x * w * 0.5f, g_camRight.y * w * 0.5f, g_camRight.z * w * 0.5f };
		Vec3 u = { g_camUp.x * h * 0.5f, g_camUp.y * h * 0.5f, g_camUp.z * h * 0.5f };
		BillboardVertex q[4] = {
			{ x - r.x + u.x, y - r.y + u.y, z - r.z + u.z, col, 0, 0 },
			{ x + r.x + u.x, y + r.y + u.y, z + r.z + u.z, col, 1, 0 },
			{ x - r.x - u.x, y - r.y - u.y, z - r.z - u.z, col, 0, 1 },
			{ x + r.x - u.x, y + r.y - u.y, z + r.z - u.z, col, 1, 1 },
		};
		dev->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, q, sizeof( BillboardVertex ) );
	}

	D3DCOLOR ColorF( float r, float g, float b, float a ){
		auto c = []( float v ){ return (int)( std::min( 1.0f, std::max( 0.0f, v ) ) * 255.0f + 0.5f ); };
		return D3DCOLOR_ARGB( c( a ), c( r ), c( g ), c( b ) );
	}

	void Additive( IDirect3DDevice9* dev ){
		dev->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
		dev->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
		dev->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_ONE );
		dev->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
		dev->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
		dev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
		dev->SetFVF( BILLBOARD_FVF );
		SetSampler( dev, 0, false, true );
	}

	// Estrelas numa esfera distante em volta da câmera (cintilando)
	void DrawStars( IDirect3DDevice9* dev, double timeMs ){
		if( g_sky.stars <= 0 || g_sparkTex == nullptr ) return;
		float t = (float)( timeMs / 1000.0 );
		dev->SetTexture( 0, g_sparkTex );
		for( int i = 0; i < g_sky.stars; i++ ){
			float u = Hash01( i * 3 + 1 ) * 2 - 1, a = Hash01( i * 3 + 2 ) * 2 * PI;
			float rr = sqrtf( 1 - u * u );
			Vec3 dir = { rr * cosf( a ), u * 0.8f + 0.15f, rr * sinf( a ) };
			float big = Hash01( i * 7 + 5 );
			float size = 14.0f + big * big * 46.0f;
			float tw = 0.55f + 0.45f * sinf( t * ( 0.8f + Hash01( i * 5 + 3 ) * 2.2f ) + i );
			float warm = Hash01( i * 11 + 7 );
			D3DCOLOR c = ColorF( 0.8f + 0.2f * warm, 0.85f, 1.0f - 0.25f * warm, ( 0.35f + 0.65f * big ) * tw );
			Billboard( dev, g_camEye.x + dir.x * 5000, g_camEye.y + dir.y * 5000, g_camEye.z + dir.z * 5000, size, size, c );
		}
	}

	// Halo pulsante nos modelos que brilham (sol/lua)
	void DrawGlows( IDirect3DDevice9* dev, double timeMs ){
		if( g_sky.glow <= 0 || g_glowTex == nullptr || g_glows.empty() ) return;
		float t = (float)( timeMs / 1000.0 );
		dev->SetTexture( 0, g_glowTex );
		for( size_t i = 0; i < g_glows.size(); i++ ){
			const Glow& g = g_glows[i];
			float pulse = 0.85f + 0.15f * sinf( t * 1.3f + i * 2.0f );
			// puxa o halo para a frente do modelo (não some dentro dele)
			Vec3 toCam = Norm( Sub( g_camEye, { g.x, g.y, g.z } ) );
			float px = g.x + toCam.x * g.size * 0.55f, py = g.y + toCam.y * g.size * 0.55f, pz = g.z + toCam.z * g.size * 0.55f;
			float k = g_sky.glow;
			Billboard( dev, px, py, pz, g.size * 2.6f * pulse, g.size * 2.6f * pulse, ColorF( g.r, g.g, g.b, 0.30f * k ) );
			Billboard( dev, px, py, pz, g.size * 1.3f, g.size * 1.3f, ColorF( g.r, g.g, g.b, 0.45f * k * pulse ) );
		}
	}

	// Brilhos subindo nos emissores de efeito do mapa
	void DrawParticles( IDirect3DDevice9* dev, double timeMs ){
		if( g_sky.particles <= 0 || g_sparkTex == nullptr || g_emitters.empty() ) return;
		float t = (float)( timeMs / 1000.0 );
		dev->SetTexture( 0, g_sparkTex );
		int per = std::max( 1, (int)( 7 * g_sky.particles ) );
		for( size_t e = 0; e < g_emitters.size(); e++ ){
			const Emitter& em = g_emitters[e];
			for( int p = 0; p < per; p++ ){
				unsigned seed = (unsigned)( e * 131 + p * 17 );
				float life = 3.0f + Hash01( seed ) * 3.0f;
				float age = fmodf( t + Hash01( seed + 1 ) * life, life ) / life;
				float ang = Hash01( seed + 2 ) * 2 * PI + t * 0.15f;
				float rad = 4.0f + Hash01( seed + 3 ) * 16.0f;
				float x = em.x + cosf( ang ) * rad + sinf( t * 0.7f + seed ) * 2.0f;
				float z = em.z + sinf( ang ) * rad + cosf( t * 0.6f + seed ) * 2.0f;
				float y = em.y + age * ( 18.0f + Hash01( seed + 4 ) * 16.0f );
				float a = sinf( age * PI ) * ( 0.55f + 0.45f * sinf( t * 5.0f + seed ) );
				float size = 2.2f + Hash01( seed + 5 ) * 2.8f;
				float warm = Hash01( seed + 6 );
				Billboard( dev, x, y, z, size, size, ColorF( 0.75f + 0.25f * warm, 0.9f, 1.0f, a ) );
			}
		}
	}
}

namespace {
	// Lista das texturas que o CreateGpu vai pedir (mesma ordem/caminhos)
	void PreloadList( std::vector<std::pair<std::wstring, bool>>& out ){
		for( int i = 0; i < g_texCount; i++ ){
			wchar_t name[32];
			swprintf( name, 32, L"t%03d.png", i );
			out.push_back( { g_dir + name, true } );
		}
		out.push_back( { g_dir + L"luz.png", false } );
		if( g_sky.water && g_hasWater ){
			for( int i = 0; i < g_waterFrames; i++ ){
				wchar_t name[32];
				swprintf( name, 32, L"agua\\%02d.png", i );
				out.push_back( { g_dir + name, true } );
			}
		}
		for( int i = 0; i < 16; i++ ){
			wchar_t name[32];
			swprintf( name, 32, L"nuvens\\%02d.png", i );
			if( !vfs::Exists( g_dir + name ) ) break;
			out.push_back( { g_dir + name, true } );
		}
	}

	DWORD WINAPI PreloadThread( LPVOID ){
		DWORD t0 = GetTickCount();
		g_parsed = ParseMap( g_dir );
		if( g_parsed ){
			CoInitializeEx( nullptr, COINIT_MULTITHREADED );
			IWICImagingFactory* wic = nullptr;
			CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &wic ) );
			std::vector<std::pair<std::wstring, bool>> list;
			PreloadList( list );
			for( auto& item : list ){
				Decoded d = DecodeChain( item.first, item.second, wic );
				if( d.ok ) g_pre[item.first] = std::move( d );
			}
			if( wic ) wic->Release();
			CoUninitialize();
		}
		logger::Write( "Mapa 3D pre-carregado em %u ms (%u texturas)", GetTickCount() - t0, (unsigned)g_pre.size() );
		g_preDone = true;
		return 0;
	}
}

void world3d::Preload( const std::wstring& dir ){
	if( g_preThread || g_preDone || g_parsed ) return;
	g_dir = dir;
	if( !g_dir.empty() && g_dir.back() != L'\\' ) g_dir += L'\\';
	vfs::GrfLoaded(); // abre a GRF nesta thread antes de a outra usar
	g_preThread = CreateThread( nullptr, 0, &PreloadThread, nullptr, 0, nullptr );
}

bool world3d::Warmup( IDirect3DDevice9* dev ){
	if( !g_preDone ) return false;
	WaitPreload();
	if( !g_parsed ) return false;
	if( dev != g_dev ){
		ReleaseGpu();
		g_dev = dev;
	}
	if( !g_gpuTried ){
		DWORD t0 = GetTickCount();
		g_gpuOk = CreateGpu( dev );
		logger::Write( "Mapa 3D enviado a placa antes da selecao (%u ms)", GetTickCount() - t0 );
	}
	return g_gpuOk;
}

bool world3d::Load( const std::wstring& dir ){
	WaitPreload();
	if( g_parsed && g_preDone ){
		return true;
	}
	if( g_parsed && dir == g_dir ){
		return true;
	}
	g_dir = dir;
	if( !g_dir.empty() && g_dir.back() != L'\\' ) g_dir += L'\\';
	ReleaseGpu();
	g_parsed = ParseMap( g_dir );
	return g_parsed;
}

bool world3d::Loaded(){
	return g_parsed;
}

void world3d::SetSky( const Sky& sky ){
	g_sky = sky;
}

bool world3d::Draw( IDirect3DDevice9* dev, const Camera& cam, double timeMs ){
	if( !g_parsed ){
		return false;
	}
	if( dev != g_dev ){
		ReleaseGpu();
		g_dev = dev;
	}
	if( !g_gpuTried ){
		g_gpuOk = CreateGpu( dev );
	}
	bool useSs = g_sky.supersample > 1.01f;
	if( !g_gpuOk || ( !useSs && !EnsureDepth( dev ) ) ){
		return false;
	}
	if( g_state == nullptr && FAILED( dev->CreateStateBlock( D3DSBT_ALL, &g_state ) ) ){
		return false;
	}
	g_state->Capture();
	bool wasDrawing = gfx::IsDrawing();
	gfx::SetDrawing( true );
	IDirect3DSurface9* oldDepth = nullptr;
	dev->GetDepthStencilSurface( &oldDepth );
	if( !useSs ) dev->SetDepthStencilSurface( g_depth );

	D3DVIEWPORT9 vp;
	dev->GetViewport( &vp );
	g_vp = vp; // projeção dos personagens: sempre na viewport real

	// Superamostragem: desenha num alvo N vezes maior e reduz no fim (bordas e folhagens suaves)
	IDirect3DSurface9* oldRt = nullptr;
	IDirect3DSurface9* ssSurf = nullptr;
	float ss = std::min( 3.0f, std::max( 1.0f, g_sky.supersample ) );
	if( ss > 1.01f ){
		UINT w = (UINT)( vp.Width * ss ), h = (UINT)( vp.Height * ss );
		if( g_ssTex == nullptr || g_ssW != w || g_ssH != h ){
			if( g_ssTex ){ g_ssTex->Release(); g_ssTex = nullptr; }
			if( SUCCEEDED( dev->CreateTexture( w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_ssTex, nullptr ) ) ){
				g_ssW = w;
				g_ssH = h;
			}else{
				logger::Write( "AVISO: superamostragem %ux%u indisponivel", w, h );
				g_sky.supersample = 1.0f;
			}
		}
		if( g_ssTex == nullptr ){
			if( EnsureDepth( dev ) ) dev->SetDepthStencilSurface( g_depth );
		}
		if( g_ssTex && SUCCEEDED( g_ssTex->GetSurfaceLevel( 0, &ssSurf ) ) ){
			dev->GetRenderTarget( 0, &oldRt );
			dev->SetRenderTarget( 0, ssSurf );
			if( !EnsureDepth( dev ) ){
				dev->SetRenderTarget( 0, oldRt );
				if( oldRt ){ oldRt->Release(); oldRt = nullptr; }
				ssSurf->Release();
				ssSurf = nullptr;
				if( EnsureDepth( dev ) ) dev->SetDepthStencilSurface( g_depth );
			}else{
				dev->SetDepthStencilSurface( g_depth );
				D3DVIEWPORT9 big = { 0, 0, g_ssW, g_ssH, 0.0f, 1.0f };
				dev->SetViewport( &big );
				vp = big;
			}
		}
	}

	dev->SetVertexShader( nullptr );
	dev->SetPixelShader( nullptr );
	dev->SetRenderState( D3DRS_LIGHTING, FALSE );
	dev->SetRenderState( D3DRS_SPECULARENABLE, FALSE );
	dev->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	dev->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	dev->SetRenderState( D3DRS_COLORWRITEENABLE, 0xF );
	dev->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	dev->SetRenderState( D3DRS_SHADEMODE, D3DSHADE_GOURAUD );
	dev->SetRenderState( D3DRS_FILLMODE, D3DFILL_SOLID );
	dev->Clear( 0, nullptr, D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0, 1.0f, 0 );

	DrawSky( dev, vp );

	// Câmera
	float yaw = cam.yaw * PI / 180.0f, pitch = cam.pitch * PI / 180.0f;
	Vec3 target = { cam.tx, cam.ty, cam.tz };
	Vec3 eye = { target.x + cam.dist * sinf( yaw ) * cosf( pitch ), target.y + cam.dist * sinf( pitch ),
		target.z + cam.dist * cosf( yaw ) * cosf( pitch ) };
	Mat view = LookAt( eye, target );
	Mat proj = Perspective( cam.fov, (float)vp.Width / std::max<DWORD>( 1, vp.Height ), 8.0f, 9000.0f, cam.shiftX, cam.shiftY );
	g_viewProj = Mul( view, proj );
	g_projScaleY = proj.m[1][1];
	g_viewValid = true;
	D3DMATRIX world = ToD3D( Identity() ), dview = ToD3D( view ), dproj = ToD3D( proj );
	dev->SetTransform( D3DTS_WORLD, &world );
	dev->SetTransform( D3DTS_VIEW, &dview );
	dev->SetTransform( D3DTS_PROJECTION, &dproj );

	StageModulate( dev );
	Additive( dev );
	dev->SetRenderState( D3DRS_ZENABLE, FALSE );
	DrawStars( dev, timeMs );

	dev->SetRenderState( D3DRS_ZENABLE, D3DZB_TRUE );
	dev->SetRenderState( D3DRS_ZWRITEENABLE, TRUE );
	dev->SetRenderState( D3DRS_ZFUNC, D3DCMP_LESSEQUAL );

	// névoa de distância na cor do horizonte: dá profundidade
	float fs = g_sky.fogStart, fe = g_sky.fogEnd;
	dev->SetRenderState( D3DRS_FOGENABLE, fe > fs ? TRUE : FALSE );
	dev->SetRenderState( D3DRS_FOGCOLOR, g_sky.horizon );
	dev->SetRenderState( D3DRS_FOGVERTEXMODE, D3DFOG_NONE );
	dev->SetRenderState( D3DRS_FOGTABLEMODE, D3DFOG_LINEAR );
	dev->SetRenderState( D3DRS_RANGEFOGENABLE, FALSE );
	dev->SetRenderState( D3DRS_FOGSTART, *reinterpret_cast<DWORD*>( &fs ) );
	dev->SetRenderState( D3DRS_FOGEND, *reinterpret_cast<DWORD*>( &fe ) );

	StageModulate( dev );
	SetSampler( dev, 0, true, true );
	dev->SetFVF( MAP_FVF );
	dev->SetStreamSource( 0, g_vb, 0, sizeof( MapVertex ) );

	// opacos (recorte por alfa)
	dev->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
	dev->SetRenderState( D3DRS_ALPHATESTENABLE, TRUE );
	dev->SetRenderState( D3DRS_ALPHAREF, 0x60 );
	dev->SetRenderState( D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL );
	StagesGround( dev, true );
	DrawBatches( dev, KIND_GROUND );
	StagesGround( dev, false );
	DrawBatches( dev, KIND_MODEL );

	// semitransparentes
	dev->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
	dev->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
	dev->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
	dev->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	dev->SetRenderState( D3DRS_ALPHAREF, 0x04 );
	dev->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	DrawBatches( dev, KIND_BLEND );

	DrawWater( dev, timeMs );
	dev->SetRenderState( D3DRS_FOGENABLE, FALSE );
	DrawClouds( dev, timeMs );

	Additive( dev );
	DrawGlows( dev, timeMs );
	DrawParticles( dev, timeMs );

	if( ssSurf ){
		// volta ao alvo real e reduz (bilinear no centro de cada bloco 2x2 = média)
		dev->SetRenderTarget( 0, oldRt );
		dev->SetViewport( &g_vp );
		dev->SetDepthStencilSurface( oldDepth );
		struct V { float x, y, z, rhw, u, v; };
		float x0 = (float)g_vp.X - 0.5f, y0 = (float)g_vp.Y - 0.5f, x1 = x0 + g_vp.Width, y1 = y0 + g_vp.Height;
		V q[4] = { { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 }, { x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
		dev->SetFVF( D3DFVF_XYZRHW | D3DFVF_TEX1 );
		dev->SetTexture( 0, g_ssTex );
		for( int st = 1; st < 4; st++ ) dev->SetTexture( st, nullptr );
		dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1 );
		dev->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1 );
		dev->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
		dev->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, 0 );
		dev->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
		dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
		dev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
		dev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
		dev->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
		dev->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
		dev->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
		dev->SetRenderState( D3DRS_ZENABLE, FALSE );
		dev->SetRenderState( D3DRS_ALPHABLENDENABLE, FALSE );
		dev->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
		dev->SetRenderState( D3DRS_FOGENABLE, FALSE );
		dev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
		dev->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, q, sizeof( V ) );
		ssSurf->Release();
		if( oldRt ) oldRt->Release();
	}else{
		dev->SetDepthStencilSurface( oldDepth );
	}
	if( oldDepth ) oldDepth->Release();
	g_state->Apply();
	gfx::SetDrawing( wasDrawing );
	return true;
}

void world3d::OnDeviceLost(){
	ReleaseGpu();
	g_viewValid = false;
}

bool world3d::Project( float x, float y, float z, float& sx, float& sy, float& pixelsPerUnit ){
	if( !g_viewValid ){
		return false;
	}
	const Mat& m = g_viewProj;
	float cx = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
	float cy = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
	float cw = x * m.m[0][3] + y * m.m[1][3] + z * m.m[2][3] + m.m[3][3];
	if( cw < 1.0f ){
		return false;
	}
	sx = g_vp.X + ( cx / cw + 1.0f ) * 0.5f * g_vp.Width;
	sy = g_vp.Y + ( 1.0f - cy / cw ) * 0.5f * g_vp.Height;
	pixelsPerUnit = g_projScaleY * 0.5f * g_vp.Height / cw;
	return true;
}

bool world3d::CellToWorld( float cellX, float cellY, float& x, float& y, float& z ){
	if( !g_parsed ){
		return false;
	}
	x = 5.0f * cellX + 2.5f;
	z = 10.0f * g_gndH - 5.0f * cellY + 7.5f;
	y = 0;
	int ix = (int)cellX, iy = (int)cellY;
	if( ix >= 0 && iy >= 0 && ix < g_gatW && iy < g_gatH ){
		y = g_gatHeight[(size_t)iy * g_gatW + ix];
	}
	return true;
}

bool world3d::CellWalkable( int cellX, int cellY ){
	if( cellX < 0 || cellY < 0 || cellX >= g_gatW || cellY >= g_gatH ) return false;
	unsigned char t = g_gatType[(size_t)cellY * g_gatW + cellX];
	return t == 0 || t == 3;
}

float world3d::ClearFraction( const Camera& cam ){
	if( !g_parsed || g_occW == 0 ){
		return 1.0f;
	}
	float yaw = cam.yaw * PI / 180.0f, pitch = cam.pitch * PI / 180.0f;
	float dx = sinf( yaw ) * cosf( pitch ), dy = sinf( pitch ), dz = cosf( yaw ) * cosf( pitch );
	// começa um pouco longe do alvo (o próprio chão/grama do personagem não conta)
	const float step = 2.0f, start = 16.0f, margin = 10.0f;
	for( float d = start; d <= cam.dist; d += step ){
		float x = cam.tx + dx * d, y = cam.ty + dy * d, z = cam.tz + dz * d;
		int cx = (int)( x / OCC_CELL ), cz = (int)( z / OCC_CELL );
		if( cx < 0 || cz < 0 || cx >= g_occW || cz >= g_occH ) continue;
		size_t k = (size_t)cz * g_occW + cx;
		if( y >= g_occLo[k] - 2.0f && y <= g_occHi[k] + 3.0f ){
			return std::max( 0.15f, ( d - margin ) / cam.dist );
		}
	}
	return 1.0f;
}

void world3d::MapCenter( float& x, float& z ){
	x = 5.0f * g_gndW + 10.0f;
	z = 5.0f * g_gndH + 10.0f;
}
