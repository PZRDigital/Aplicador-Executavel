#include "d3d_hooks.hpp"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <vector>
#include <mutex>

#include "chardata.hpp"
#include "charwnd.hpp"
#include "client.hpp"
#include "config.hpp"
#include "gfx.hpp"
#include "game.hpp"
#include "itemdb.hpp"
#include "lgp.hpp"
#include "input.hpp"
#include "log.hpp"
#include "scene.hpp"
#include "ui.hpp"

namespace filelog { void Install(); }

namespace {
	// Índices de vtable (ordem da interface em d3d9.h)
	constexpr int IDX_D3D_CREATEDEVICE = 16;
	constexpr int IDX_D3DEX_CREATEDEVICEEX = 20;
	constexpr int IDX_DEV_RESET = 16;
	constexpr int IDX_DEV_PRESENT = 17;
	constexpr int IDX_DEV_DRAWPRIMITIVE = 81;
	constexpr int IDX_DEV_DRAWINDEXEDPRIMITIVE = 82;
	constexpr int IDX_DEV_DRAWPRIMITIVEUP = 83;
	constexpr int IDX_DEV_DRAWINDEXEDPRIMITIVEUP = 84;
	constexpr int IDX_DEVEX_PRESENTEX = 121;
	constexpr int IDX_DEVEX_RESETEX = 132;

	constexpr int DIAG_MAX_DRAWS = 3000;
	constexpr float SMALL_DRAW = 64.0f;      // cursor, marcadores de foco
	constexpr int CURSOR_LEARN_FRAMES = 30;  // frames com o mesmo "último desenho"

	// IID_IDirect3DDevice9Ex (evita depender de dxguid.lib)
	const GUID IID_Device9Ex = { 0xb18b10ce, 0x2649, 0x405a, { 0x87, 0x0f, 0x95, 0xf7, 0x77, 0xd4, 0x31, 0x3a } };

	using CreateDeviceFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9** );
	using CreateDeviceExFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex** );
	using ResetFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, D3DPRESENT_PARAMETERS* );
	using ResetExFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX* );
	using PresentFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA* );
	using PresentExFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD );
	using DrawPrimitiveFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT );
	using DrawIndexedPrimitiveFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT );
	using DrawPrimitiveUPFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT );
	using DrawIndexedPrimitiveUPFn = HRESULT( STDMETHODCALLTYPE* )( IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT );

	CreateDeviceFn o_CreateDevice = nullptr;
	CreateDeviceExFn o_CreateDeviceEx = nullptr;
	ResetFn o_Reset = nullptr;
	ResetExFn o_ResetEx = nullptr;
	PresentFn o_Present = nullptr;
	PresentExFn o_PresentEx = nullptr;
	DrawPrimitiveFn o_DrawPrimitive = nullptr;
	DrawIndexedPrimitiveFn o_DrawIndexedPrimitive = nullptr;
	DrawPrimitiveUPFn o_DrawPrimitiveUP = nullptr;
	DrawIndexedPrimitiveUPFn o_DrawIndexedPrimitiveUP = nullptr;

	// Etapas do frame da seleção, na ordem em que o cliente desenha
	enum class Phase {
		Background, // fundo em tela cheia -> panorama
		Window,     // fundo da janela nativa -> interface nova
		Tiles,      // blocos 256x256 da janela nativa (cartões, textos, botões) -> escondidos
		Sprites,    // personagens nos cartões -> só o selecionado, ampliado
		After,      // outras janelas do cliente e cursor -> normais
	};

	bool g_wasActive = false;
	bool g_firstDrawDone = false;
	int g_lastSlot = -1;
	unsigned g_frame = 0;
	IDirect3DDevice9* g_device = nullptr; // device do cliente (LGP desenha nele)

	Phase g_phase = Phase::Background;
	float g_originX = 0, g_originY = 0;
	bool g_popupSeen = false;
	bool g_popupActive = false;

	// Aprendizado do desenho do cursor (o último desenho de cada frame)
	ULONG g_drawHash = 0, g_lastHash = 0, g_prevLastHash = 0, g_cursorHash = 0;
	int g_sameCount = 0;
	uintptr_t g_selfBase = 0, g_selfEnd = 0;

	std::vector<BYTE> g_vertexCopy;

	// Registro de um frame do jogo ao ligar o modo foto
	int g_photoLogFrames = 0;
	bool g_photoLogArmed = false;
	bool g_lastPhoto = false;

	// Diagnóstico
	int g_diagDelay = 0;
	int g_diagFramesLeft = 0;
	int g_diagDrawIndex = 0;

	struct DrawInfo {
		const char* api;
		D3DPRIMITIVETYPE type;
		UINT primCount;
		bool hasBounds = false;
		float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
		UINT texWidth = 0, texHeight = 0;
		D3DFORMAT texFormat = D3DFMT_UNKNOWN;
		uintptr_t tex = 0;     // textura do estágio 0 (só o endereço; não segura referência)
		uintptr_t caller = 0;

		float Width() const { return maxX - minX; }
		float Height() const { return maxY - minY; }
	};

	enum class Verdict { Pass, Skip };

	template <typename T> void HookVtable( void* object, int index, T hook, T& original ){
		void** vt = *reinterpret_cast<void***>( object );
		if( vt[index] == reinterpret_cast<void*>( hook ) ){
			return; // vtable compartilhada já alterada
		}
		DWORD old;
		VirtualProtect( &vt[index], sizeof( void* ), PAGE_EXECUTE_READWRITE, &old );
		original = reinterpret_cast<T>( vt[index] );
		vt[index] = reinterpret_cast<void*>( hook );
		VirtualProtect( &vt[index], sizeof( void* ), old, &old );
	}

	UINT VertexCount( D3DPRIMITIVETYPE type, UINT prims ){
		switch( type ){
			case D3DPT_TRIANGLELIST: return prims * 3;
			case D3DPT_TRIANGLESTRIP:
			case D3DPT_TRIANGLEFAN: return prims + 2;
			case D3DPT_LINELIST: return prims * 2;
			case D3DPT_LINESTRIP: return prims + 1;
			default: return prims;
		}
	}

	bool IsPretransformed( IDirect3DDevice9* dev ){
		DWORD fvf = 0;
		dev->GetFVF( &fvf );
		return ( fvf & D3DFVF_POSITION_MASK ) == D3DFVF_XYZRHW;
	}

	void AccumulateBounds( DrawInfo& info, const void* data, UINT stride, UINT count ){
		const BYTE* p = static_cast<const BYTE*>( data );
		for( UINT i = 0; i < count; i++, p += stride ){
			const float* v = reinterpret_cast<const float*>( p );
			info.minX = std::min( info.minX, v[0] );
			info.minY = std::min( info.minY, v[1] );
			info.maxX = std::max( info.maxX, v[0] );
			info.maxY = std::max( info.maxY, v[1] );
		}
		info.hasBounds = count > 0;
	}

	// Lê o vertex buffer do stream 0 (apenas se não for WRITEONLY)
	void BoundsFromStream( IDirect3DDevice9* dev, DrawInfo& info, UINT startVertex ){
		IDirect3DVertexBuffer9* vb = nullptr;
		UINT offset = 0, stride = 0;
		if( FAILED( dev->GetStreamSource( 0, &vb, &offset, &stride ) ) || vb == nullptr ){
			return;
		}
		D3DVERTEXBUFFER_DESC desc;
		vb->GetDesc( &desc );
		UINT count = VertexCount( info.type, info.primCount );
		UINT begin = offset + startVertex * stride;
		void* data = nullptr;
		if( !( desc.Usage & D3DUSAGE_WRITEONLY ) && stride >= 16 && begin + count * stride <= desc.Size
			&& SUCCEEDED( vb->Lock( begin, count * stride, &data, D3DLOCK_READONLY ) ) ){
			AccumulateBounds( info, data, stride, count );
			vb->Unlock();
		}
		vb->Release();
	}

	void ReadTexture( IDirect3DDevice9* dev, DrawInfo& info ){
		IDirect3DBaseTexture9* base = nullptr;
		if( FAILED( dev->GetTexture( 0, &base ) ) || base == nullptr ){
			return;
		}
		info.tex = reinterpret_cast<uintptr_t>( base );
		if( base->GetType() == D3DRTYPE_TEXTURE ){
			D3DSURFACE_DESC desc;
			if( SUCCEEDED( static_cast<IDirect3DTexture9*>( base )->GetLevelDesc( 0, &desc ) ) ){
				info.texWidth = desc.Width;
				info.texHeight = desc.Height;
				info.texFormat = desc.Format;
			}
		}
		base->Release();
	}

	// Identifica quem chamou o desenho: hash dos primeiros endereços de retorno no Codex.exe
	void ReadCaller( DrawInfo& info ){
		void* frames[12];
		USHORT n = RtlCaptureStackBackTrace( 0, 12, frames, nullptr );
		ULONG hash = 0;
		int used = 0;
		for( USHORT i = 0; i < n && used < 4; i++ ){
			uintptr_t a = reinterpret_cast<uintptr_t>( frames[i] );
			if( a >= g_selfBase && a < g_selfEnd ){
				continue; // quadros da própria DLL
			}
			if( used == 0 ){
				info.caller = a;
			}
			hash = hash * 31 + (ULONG)a;
			used++;
		}
		g_drawHash = hash;
	}

	bool CoversViewport( IDirect3DDevice9* dev, const DrawInfo& info ){
		if( !info.hasBounds ){
			return false;
		}
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		float threshold = config::Get().fullscreenThreshold;
		return info.Width() >= vp.Width * threshold && info.Height() >= vp.Height * threshold;
	}

	bool MatchesBackgroundTexture( const DrawInfo& info ){
		const config::Settings& cfg = config::Get();
		return cfg.bgTextureWidth != 0 && info.texWidth == cfg.bgTextureWidth && info.texHeight == cfg.bgTextureHeight;
	}

	bool IsNativeWindow( const DrawInfo& info ){
		const config::Settings& cfg = config::Get();
		if( info.texWidth == cfg.nativeWidth && info.texHeight == cfg.nativeHeight ){
			return true;
		}
		return info.hasBounds && fabsf( info.Width() - cfg.nativeWidth ) <= 2.0f && fabsf( info.Height() - cfg.nativeHeight ) <= 2.0f;
	}

	// Blocos da janela nativa: alinhados à grade de 256px a partir do canto da janela
	bool IsNativeTile( const DrawInfo& info ){
		if( !info.hasBounds || info.Width() < SMALL_DRAW || info.Height() < SMALL_DRAW ){
			return false;
		}
		const config::Settings& cfg = config::Get();
		float dx = info.minX - g_originX, dy = info.minY - g_originY;
		if( dx < -1.0f || dy < -1.0f || info.maxX > g_originX + cfg.nativeWidth + 1.0f || info.maxY > g_originY + cfg.nativeHeight + 1.0f ){
			return false;
		}
		float rx = fmodf( dx + 0.5f, 256.0f ), ry = fmodf( dy + 0.5f, 256.0f );
		return rx < 1.5f && ry < 1.5f;
	}

	// seleção hardcoded: blocos da janela em tela cheia (grade de 256 px a partir de 0,0)
	bool IsSelectTile( IDirect3DDevice9* dev, const DrawInfo& info ){
		if( !info.hasBounds || info.texWidth <= 1 ) return false;
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		if( info.maxX > vp.Width + 1.0f || info.maxY > vp.Height + 1.0f || info.minX < -1.0f || info.minY < -1.0f ) return false;
		float rx = fmodf( info.minX + 0.5f, 256.0f ), ry = fmodf( info.minY + 0.5f, 256.0f );
		if( rx > 1.5f || ry > 1.5f ) return false;
		// do tamanho de um bloco (ou o resto na borda direita/de baixo)
		if( info.Width() > 257.5f || info.Height() > 257.5f ) return false;
		bool fullW = fabsf( info.Width() - 256.0f ) <= 1.5f || fabsf( info.maxX - vp.Width ) <= 1.5f;
		bool fullH = fabsf( info.Height() - 256.0f ) <= 1.5f || fabsf( info.maxY - vp.Height ) <= 1.5f;
		return fullW && fullH;
	}

	bool g_sceneShown = false; // cenário desenhado na tela neste quadro

	bool DrawScene( IDirect3DDevice9* dev ){
		bool ok = scene::Draw( dev );
		g_sceneShown |= ok;
		return ok;
	}

	// Quadro da seleção sem nenhum desenho na tela (o cliente desenhou numa textura e copiou):
	// cobre com o cenário para a tela original não aparecer.
	void CoverWithScene( IDirect3DDevice9* dev ){
		IDirect3DSurface9* bb = nullptr;
		IDirect3DSurface9* oldRt = nullptr;
		if( FAILED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb ) ) ) return;
		dev->GetRenderTarget( 0, &oldRt );
		D3DVIEWPORT9 oldVp;
		dev->GetViewport( &oldVp );
		dev->SetRenderTarget( 0, bb ); // também ajusta a viewport para a tela inteira
		if( SUCCEEDED( dev->BeginScene() ) ){
			if( DrawScene( dev ) && config::Get().debug ){
				logger::Write( "Quadro sem desenho na tela coberto pelo cenario" );
			}
			dev->EndScene();
		}
		if( oldRt ){
			dev->SetRenderTarget( 0, oldRt );
			oldRt->Release();
		}
		dev->SetViewport( &oldVp );
		bb->Release();
	}

	// Copia os vértices de um desenho UP transformados (x' = ox + (x - ax) * s),
	// opcionalmente escurecidos/esmaecidos (cor difusa).
	void TransformCopy( IDirect3DDevice9* dev, const DrawInfo& info, const void* data, UINT stride,
		float ax, float ay, float ox, float oy, float scale, float brightness, float alpha, std::vector<BYTE>& out ){
		UINT count = VertexCount( info.type, info.primCount );
		out.assign( static_cast<const BYTE*>( data ), static_cast<const BYTE*>( data ) + count * stride );

		DWORD fvf = 0;
		dev->GetFVF( &fvf );
		bool tint = ( brightness < 0.999f || alpha < 0.999f ) && ( fvf & D3DFVF_DIFFUSE ) && stride >= 20;

		for( UINT i = 0; i < count; i++ ){
			BYTE* vtx = out.data() + i * stride;
			float* v = reinterpret_cast<float*>( vtx );
			v[0] = ox + ( v[0] - ax ) * scale;
			v[1] = oy + ( v[1] - ay ) * scale;
			if( tint ){
				D3DCOLOR* c = reinterpret_cast<D3DCOLOR*>( vtx + 16 ); // XYZRHW + DIFFUSE
				int a = (int)( ( ( *c >> 24 ) & 0xFF ) * alpha );
				int r = (int)( ( ( *c >> 16 ) & 0xFF ) * brightness );
				int g = (int)( ( ( *c >> 8 ) & 0xFF ) * brightness );
				int b = (int)( ( *c & 0xFF ) * brightness );
				*c = ( a << 24 ) | ( r << 16 ) | ( g << 8 ) | b;
			}
		}
	}

	// Ampliação dos sprites: shader "sharp bilinear" (ou vizinho mais próximo se não houver shader)
	void DrawCopy( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const std::vector<BYTE>& verts, UINT stride,
		bool point, float scale, const RECT* clip ){
		DWORD minF = 0, magF = 0, mipF = 0, addrU = 0, addrV = 0, scissorOn = 0;
		RECT oldClip = {};
		IDirect3DPixelShader9* oldPs = nullptr;
		bool shader = false;
		if( point ){
			dev->GetSamplerState( 0, D3DSAMP_MINFILTER, &minF );
			dev->GetSamplerState( 0, D3DSAMP_MAGFILTER, &magF );
			dev->GetSamplerState( 0, D3DSAMP_MIPFILTER, &mipF );
			dev->GetSamplerState( 0, D3DSAMP_ADDRESSU, &addrU );
			dev->GetSamplerState( 0, D3DSAMP_ADDRESSV, &addrV );
			IDirect3DPixelShader9* ps = gfx::SharpShader( dev );
			IDirect3DBaseTexture9* base = nullptr;
			dev->GetTexture( 0, &base );
			IDirect3DTexture9* tex = nullptr;
			D3DSURFACE_DESC desc = {};
			if( ps && base && SUCCEEDED( base->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast<void**>( &tex ) ) )
				&& SUCCEEDED( tex->GetLevelDesc( 0, &desc ) ) && desc.Width > 0 && desc.Height > 0 ){
				float smooth = std::min( 1.0f, std::max( 0.0f, config::Get().spriteSmooth ) );
				float k = 1.0f + ( std::max( 1.0f, scale ) - 1.0f ) * ( 1.0f - smooth );
				float c[8] = { (float)desc.Width, (float)desc.Height, 1.0f / desc.Width, 1.0f / desc.Height, k, k, 0, 0 };
				dev->GetPixelShader( &oldPs );
				dev->SetPixelShader( ps );
				dev->SetPixelShaderConstantF( 0, c, 2 );
				shader = true;
			}
			if( tex ) tex->Release();
			if( base ) base->Release();
			dev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_POINT );
			dev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT );
			dev->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
			if( shader ){
				dev->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
				dev->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );
			}
		}
		if( clip != nullptr ){
			dev->GetRenderState( D3DRS_SCISSORTESTENABLE, &scissorOn );
			dev->GetScissorRect( &oldClip );
			dev->SetScissorRect( clip );
			dev->SetRenderState( D3DRS_SCISSORTESTENABLE, TRUE );
		}

		o_DrawPrimitiveUP( dev, type, prims, verts.data(), stride );

		if( clip != nullptr ){
			dev->SetScissorRect( &oldClip );
			dev->SetRenderState( D3DRS_SCISSORTESTENABLE, scissorOn );
		}
		if( point ){
			if( shader ){
				dev->SetPixelShader( oldPs );
				dev->SetSamplerState( 0, D3DSAMP_ADDRESSU, addrU );
				dev->SetSamplerState( 0, D3DSAMP_ADDRESSV, addrV );
			}
			if( oldPs ) oldPs->Release();
			dev->SetSamplerState( 0, D3DSAMP_MINFILTER, minF );
			dev->SetSamplerState( 0, D3DSAMP_MAGFILTER, magF );
			dev->SetSamplerState( 0, D3DSAMP_MIPFILTER, mipF );
		}
	}

	void DrawTransformed( IDirect3DDevice9* dev, const DrawInfo& info, const void* data, UINT stride,
		float ax, float ay, float ox, float oy, float scale, bool point, float brightness = 1.0f, const RECT* clip = nullptr ){
		TransformCopy( dev, info, data, stride, ax, ay, ox, oy, scale, brightness, 1.0f, g_vertexCopy );
		DrawCopy( dev, info.type, info.primCount, g_vertexCopy, stride, point, scale, clip );
	}

	// Personagens no mapa: guardados e redesenhados do mais distante para o mais próximo
	const D3DRENDERSTATETYPE DEFER_RS[] = {
		D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE,
		D3DRS_ALPHAREF, D3DRS_ALPHAFUNC, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_FOGENABLE, D3DRS_LIGHTING,
	};
	const D3DTEXTURESTAGESTATETYPE DEFER_TSS[] = {
		D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
	};
	constexpr int N_RS = sizeof( DEFER_RS ) / sizeof( DEFER_RS[0] );
	constexpr int N_TSS = sizeof( DEFER_TSS ) / sizeof( DEFER_TSS[0] );

	struct DeferredState {
		DWORD rs[N_RS];
		DWORD tss[N_TSS];
		DWORD stage1Color, stage1Alpha, addrU, addrV, fvf;
		IDirect3DBaseTexture9* tex;

		void Capture( IDirect3DDevice9* dev ){
			for( int i = 0; i < N_RS; i++ ) dev->GetRenderState( DEFER_RS[i], &rs[i] );
			for( int i = 0; i < N_TSS; i++ ) dev->GetTextureStageState( 0, DEFER_TSS[i], &tss[i] );
			dev->GetTextureStageState( 1, D3DTSS_COLOROP, &stage1Color );
			dev->GetTextureStageState( 1, D3DTSS_ALPHAOP, &stage1Alpha );
			dev->GetSamplerState( 0, D3DSAMP_ADDRESSU, &addrU );
			dev->GetSamplerState( 0, D3DSAMP_ADDRESSV, &addrV );
			dev->GetFVF( &fvf );
			tex = nullptr;
			dev->GetTexture( 0, &tex ); // AddRef
		}
		void Apply( IDirect3DDevice9* dev ) const {
			for( int i = 0; i < N_RS; i++ ) dev->SetRenderState( DEFER_RS[i], rs[i] );
			for( int i = 0; i < N_TSS; i++ ) dev->SetTextureStageState( 0, DEFER_TSS[i], tss[i] );
			dev->SetTextureStageState( 1, D3DTSS_COLOROP, stage1Color );
			dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, stage1Alpha );
			dev->SetSamplerState( 0, D3DSAMP_ADDRESSU, addrU );
			dev->SetSamplerState( 0, D3DSAMP_ADDRESSV, addrV );
			dev->SetFVF( fvf );
			dev->SetTexture( 0, tex );
		}
		void Release(){
			if( tex ){ tex->Release(); tex = nullptr; }
		}
	};

	struct DeferredSprite {
		float order;   // escala do personagem (maior = mais perto da câmera)
		int seq;       // ordem original (partes do mesmo personagem)
		D3DPRIMITIVETYPE type;
		UINT prims, stride;
		std::vector<BYTE> verts;
		bool point;
		float scale;
		RECT clip;
		DeferredState state;
	};
	std::vector<DeferredSprite> g_deferred;
	std::vector<DeferredSprite> g_onTop; // por cima de tudo no fim do quadro (item arrastado sobre a janela Personagem)

	void DeferSprite( IDirect3DDevice9* dev, const DrawInfo& info, const void* data, UINT stride, const ui::SpriteXform& xf, bool point ){
		DeferredSprite d;
		d.order = xf.order;
		d.seq = (int)g_deferred.size();
		d.type = info.type;
		d.prims = info.primCount;
		d.stride = stride;
		d.point = point;
		d.scale = xf.scale;
		d.clip = xf.clip;
		TransformCopy( dev, info, data, stride, xf.anchorX, xf.anchorY, xf.targetX, xf.targetY, xf.scale, xf.brightness, xf.alpha, d.verts );
		d.state.Capture( dev );
		g_deferred.push_back( std::move( d ) );
	}

	bool g_platesPending = false; // placas de nome ainda não desenhadas neste frame

	void FlushDeferred( IDirect3DDevice9* dev ){
		if( g_deferred.empty() ){
			if( g_platesPending ){
				g_platesPending = false;
				ui::DrawPlates( dev );
			}
			return;
		}
		std::stable_sort( g_deferred.begin(), g_deferred.end(), []( const DeferredSprite& a, const DeferredSprite& b ){
			return a.order < b.order;
		} );
		DeferredState saved;
		saved.Capture( dev );
		for( DeferredSprite& d : g_deferred ){
			d.state.Apply( dev );
			DrawCopy( dev, d.type, d.prims, d.verts, d.stride, d.point, d.scale, &d.clip );
			d.state.Release();
		}
		saved.Apply( dev );
		saved.Release();
		g_deferred.clear();
		if( g_platesPending ){
			g_platesPending = false;
			ui::DrawPlates( dev );
		}
	}

	void LogDraw( IDirect3DDevice9* dev, const DrawInfo& info, const char* note ){
		if( g_diagDrawIndex >= DIAG_MAX_DRAWS ){
			return;
		}
		DWORD fvf = 0;
		dev->GetFVF( &fvf );
		if( info.hasBounds ){
			logger::Write( "  #%03d %s tipo=%d prims=%u fvf=0x%X tex=%ux%u fmt=%d area=(%.1f,%.1f)-(%.1f,%.1f) de=%08X %s",
				g_diagDrawIndex, info.api, info.type, info.primCount, fvf, info.texWidth, info.texHeight, info.texFormat,
				info.minX, info.minY, info.maxX, info.maxY, (unsigned)info.caller, note );
		}else{
			logger::Write( "  #%03d %s tipo=%d prims=%u fvf=0x%X tex=%ux%u fmt=%d area=? de=%08X %s",
				g_diagDrawIndex, info.api, info.type, info.primCount, fvf, info.texWidth, info.texHeight, info.texFormat,
				(unsigned)info.caller, note );
		}
		g_diagDrawIndex++;
	}

	// Criação: blocos da janela nativa (escondidos; a interface nova recorta os penteados deles)
	std::vector<ui::MakeTile> g_makeTiles;
	bool g_makeUiPending = false;
	client::WndRect g_makeRect;

	void ClearMakeTiles(){
		for( ui::MakeTile& t : g_makeTiles ) if( t.tex ) t.tex->Release();
		g_makeTiles.clear();
	}

	bool IsMakeTile( const DrawInfo& info ){
		if( !info.hasBounds || g_makeRect.w <= 0 ) return false;
		float dx = info.minX - g_makeRect.x, dy = info.minY - g_makeRect.y;
		if( dx < -1.0f || dy < -1.0f || info.maxX > g_makeRect.x + g_makeRect.w + 1.0f || info.maxY > g_makeRect.y + g_makeRect.h + 1.0f ) return false;
		float rx = fmodf( dx + 0.5f, 256.0f ), ry = fmodf( dy + 0.5f, 256.0f );
		return rx < 1.5f && ry < 1.5f && info.Width() >= 16.0f && info.Height() >= 16.0f && info.texHeight == 256;
	}

	void RecordMakeTile( IDirect3DDevice9* dev, const DrawInfo& info, const void* data, UINT stride ){
		ui::MakeTile t = {};
		IDirect3DBaseTexture9* base = nullptr;
		dev->GetTexture( 0, &base );
		if( base == nullptr ) return;
		if( FAILED( base->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast<void**>( &t.tex ) ) ) ){
			base->Release();
			return;
		}
		base->Release();
		t.x = info.minX; t.y = info.minY; t.w = info.Width(); t.h = info.Height();
		t.u0 = t.v0 = 1e9f; t.u1 = t.v1 = -1e9f;
		UINT count = VertexCount( info.type, info.primCount );
		for( UINT i = 0; i < count && data != nullptr && stride >= 28; i++ ){
			const float* v = reinterpret_cast<const float*>( static_cast<const BYTE*>( data ) + i * stride );
			t.u0 = std::min( t.u0, v[5] ); t.u1 = std::max( t.u1, v[5] );
			t.v0 = std::min( t.v0, v[6] ); t.v1 = std::max( t.v1, v[6] );
		}
		if( t.u1 <= t.u0 ){ t.u0 = 0; t.v0 = 0; t.u1 = t.w / 256.0f; t.v1 = t.h / 256.0f; }
		g_makeTiles.push_back( t );
	}

	void DrawMakeUi( IDirect3DDevice9* dev ){
		g_makeUiPending = false;
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		input::SetViewport( (float)vp.Width, (float)vp.Height );
		ui::MakeFrame f;
		f.vpWidth = (float)vp.Width;
		f.vpHeight = (float)vp.Height;
		f.originX = (float)g_makeRect.x;
		f.originY = (float)g_makeRect.y;
		f.popupActive = g_popupActive;
		f.tiles = g_makeTiles.data();
		f.tileCount = (int)g_makeTiles.size();
		ui::DrawMake( dev, f );
	}

	// Prévia da criação: sprites da rotina de personagem vão para o lugar do slot no mapa
	struct PreviewXform {
		bool on = false;       // quadro com prévia enfileirada (os sprites chegam depois, na fila do cliente)
		float ax = 0, ay = 0, tx = 0, ty = 0, scale = 1;
	};
	PreviewXform g_preview;
	constexpr int PREVIEW_X = 400, PREVIEW_Y = 300; // pés da prévia, relativos à janela de criação

	void DrawMakePreview( IDirect3DDevice9* dev ){
		const config::Settings& cfg = config::Get();
		client::MakeState st;
		client::WndRect r;
		float fx, fy, scale;
		if( !client::ReadMakeState( st ) || !client::MakeWindowRect( r ) || !scene::SlotToScreen( client::SelectedSlot(), fx, fy, scale ) ){
			return;
		}
		g_preview.on = true;
		g_preview.ax = (float)( r.x + PREVIEW_X );
		g_preview.ay = (float)( r.y + PREVIEW_Y );
		g_preview.tx = fx;
		g_preview.ty = fy;
		g_preview.scale = scale;
		int action = ( cfg.selectedAction >= 0 ? cfg.selectedAction : 0 ) + st.dir;
		int motion = (int)( GetTickCount() / (DWORD)std::max( 16, cfg.actionFrameMs ) );
		if( !client::DrawCharacter( client::MakeWindow(), PREVIEW_X, PREVIEW_Y, st.sex, 0, st.hair, st.hairColor, action, motion ) ){
			g_preview.on = false;
		}
	}

	// Sprite da prévia: pequeno e em volta da posição virtual dos pés
	bool IsPreviewSprite( const DrawInfo& info ){
		if( !g_preview.on || !info.hasBounds || info.Width() > 220.0f || info.Height() > 260.0f ) return false;
		if( info.texHeight < 64 ) return false; // dicas de botão (128x32) e retângulos sem textura
		float cx = ( info.minX + info.maxX ) * 0.5f, cy = ( info.minY + info.maxY ) * 0.5f;
		return cx > g_preview.ax - 120.0f && cx < g_preview.ax + 120.0f && cy > g_preview.ay - 200.0f && cy < g_preview.ay + 40.0f;
	}

	// Decide o destino de cada desenho do cliente durante a seleção.
	bool g_toggleDrawn = false;

	// ---------------------------------------------------------------- telas de entrada (login)
	bool g_loginActive = false;

	bool LoginPhase(){
		return config::Feature( config::FEAT_LOGIN ) && !client::InGame() && config::CustomActive() && config::Get().uiEnabled && !client::IsSelectActive()
			&& !client::IsMakeCharActive() && !client::IsCreatingSelect();
	}

	bool InLoginWindow( const DrawInfo& info ){
		if( !info.hasBounds ) return false;
		for( int k = 0; k < (int)client::LoginWnd::COUNT; k++ ){
			client::WndRect r;
			// as janelas de aviso são desenhadas pelo próprio cliente no tema: aparecem
			if( charwnd::MessageBoxActive() && ( k == (int)client::LoginWnd::MessageBox || k == (int)client::LoginWnd::MessageBoxAuto ) ) continue;
			if( charwnd::LoginActive() && k == (int)client::LoginWnd::Login ) continue; // painel do próprio cliente
			if( charwnd::WaitActive() && k == (int)client::LoginWnd::Wait ) continue;
			if( charwnd::ServerActive() && k == (int)client::LoginWnd::SelectServer ) continue;
			if( client::LoginWindowRect( (client::LoginWnd)k, r ) && r.x > 0 && r.y > 0 ){
				float m = k == (int)client::LoginWnd::Wait ? 24.0f : 8.0f; // a de espera desenha a moldura um pouco fora
				if( info.minX >= r.x - m && info.maxX <= r.x + r.w + m && info.minY >= r.y - m && info.maxY <= r.y + r.h + m ){
					return true;
				}
			}
		}
		return false;
	}

	void* MensagemAberta( client::WndRect& r );

	void DrawLoginUi( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		if( charwnd::LoginActive() ) game::SubstituteLoginWindow( (int)vp.Width, (int)vp.Height );
		if( charwnd::WaitActive() ) game::SubstituteWaitWindow( (int)vp.Width, (int)vp.Height );
		if( charwnd::ServerActive() ) game::SubstituteServerWindow( (int)vp.Width, (int)vp.Height );
		input::SetViewport( (float)vp.Width, (float)vp.Height );
		ui::LoginFrame f;
		f.vpWidth = (float)vp.Width;
		f.vpHeight = (float)vp.Height;
		f.popupActive = g_popupActive;
		client::WndRect r;
		if( client::LoginWindowRect( client::LoginWnd::Login, r ) && ( r.x > 0 || charwnd::LoginActive() ) ){
			f.hasLogin = true; f.loginX = (float)r.x; f.loginY = (float)r.y; // (o painel do cliente entra deslizando de x < 0)
		}
		if( client::LoginWindowRect( client::LoginWnd::SelectServer, r ) && r.x > 0 ){
			f.hasServers = true; f.serversX = (float)r.x; f.serversY = (float)r.y;
		}
		f.hasWait = client::LoginWindow( client::LoginWnd::Wait ) != nullptr;
		// aviso do cliente aberto (desenhado por ele no tema): sem o "Conectando" por baixo
		client::WndRect mr;
		if( charwnd::MessageBoxActive() && MensagemAberta( mr ) ) f.hasWait = false;
		ui::DrawLogin( dev, f );
	}

	// Mapa ainda não pronto: fundo escuro no lugar da arte original
	void FillDark( IDirect3DDevice9* dev ){
		if( !gfx::Begin( dev ) ) return;
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		gfx::RectV( 0, 0, (float)vp.Width, (float)vp.Height, gfx::Rgba( 10, 18, 34, 1 ), gfx::Rgba( 3, 6, 12, 1 ) );
		gfx::End();
	}

	// Modo original logo após a troca: o fundo da janela nativa é uma foto da tela tirada quando ela
	// abriu (com a interface personalizada). Refazemos com o fundo nativo, como o cliente faria.
	bool g_snapshotDirty = false;
	IDirect3DBaseTexture9* g_bgTex = nullptr;
	float g_bgU0 = 0, g_bgV0 = 0, g_bgU1 = 1, g_bgV1 = 1;

	void ReleaseBgTex(){
		if( g_bgTex ){ g_bgTex->Release(); g_bgTex = nullptr; }
	}

	void UvRange( const void* data, UINT stride, UINT count, float& u0, float& v0, float& u1, float& v1 ){
		u0 = v0 = 1e9f; u1 = v1 = -1e9f;
		for( UINT i = 0; i < count; i++ ){
			const float* uv = reinterpret_cast<const float*>( static_cast<const BYTE*>( data ) + i * stride + stride - 8 );
			u0 = std::min( u0, uv[0] ); u1 = std::max( u1, uv[0] );
			v0 = std::min( v0, uv[1] ); v1 = std::max( v1, uv[1] );
		}
	}

	bool RedrawWindowBackground( IDirect3DDevice9* dev, const DrawInfo& info, const void* data, UINT stride ){
		if( g_bgTex == nullptr || data == nullptr || stride < 28 || !info.hasBounds ) return false;
		UINT count = VertexCount( info.type, info.primCount );
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		g_vertexCopy.assign( static_cast<const BYTE*>( data ), static_cast<const BYTE*>( data ) + count * stride );
		for( UINT i = 0; i < count; i++ ){
			BYTE* vtx = g_vertexCopy.data() + i * stride;
			const float* p = reinterpret_cast<const float*>( vtx );
			float* uv = reinterpret_cast<float*>( vtx + stride - 8 );
			// o trecho do fundo que fica atrás deste pedaço da janela (posição na tela), como a foto do cliente
			float fx = ( p[0] + 0.5f ) / std::max( 1.0f, (float)vp.Width );
			float fy = ( p[1] + 0.5f ) / std::max( 1.0f, (float)vp.Height );
			uv[0] = g_bgU0 + fx * ( g_bgU1 - g_bgU0 );
			uv[1] = g_bgV0 + fy * ( g_bgV1 - g_bgV0 );
			*reinterpret_cast<DWORD*>( vtx + 16 ) = 0xFFDDDDDD; // a foto do cliente é o fundo a 87% (0xDD)
		}
		IDirect3DBaseTexture9* old = nullptr;
		dev->GetTexture( 0, &old );
		dev->SetTexture( 0, g_bgTex );
		o_DrawPrimitiveUP( dev, info.type, info.primCount, g_vertexCopy.data(), stride );
		dev->SetTexture( 0, old );
		if( old ) old->Release();
		return true;
	}

	// ---------------------------------------------------------------- mensagens do cliente (UIMessageBox)
	bool g_msgUi = false;           // mensagem aberta e redesenhada pela interface neste quadro
	bool g_msgDrawn = false;
	client::WndRect g_msgRect;
	void* g_msgWnd = nullptr;

	constexpr uintptr_t VT_EMAIL_PROMPT = 0x101268C; // pedido de e-mail da exclusão de personagem (campo + OK)
	constexpr uintptr_t VT_EDIT = 0x10089F8;         // campo de texto do cliente: texto em [+0xD8], tamanho +0xE8
	bool g_msgPrompt = false;
	bool g_msgNative = false; // aviso aberto, desenhado pelo próprio cliente

	bool Visivel( void* w ){
		__try{ return *reinterpret_cast<int*>( reinterpret_cast<uintptr_t>( w ) + 0x28 ) != 0; }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return false; }
	}

	void* MensagemAberta( client::WndRect& r ){
		g_msgPrompt = false;
		if( uintptr_t p = game::ShownWindow( VT_EMAIL_PROMPT ) ){
			__try{
				r.x = *reinterpret_cast<int*>( p + 0x1C ); r.y = *reinterpret_cast<int*>( p + 0x20 );
				r.w = *reinterpret_cast<int*>( p + 0x14 ); r.h = *reinterpret_cast<int*>( p + 0x18 );
			}__except( EXCEPTION_EXECUTE_HANDLER ){ return nullptr; }
			g_msgPrompt = true;
			return reinterpret_cast<void*>( p );
		}
		for( client::LoginWnd k : { client::LoginWnd::MessageBox, client::LoginWnd::MessageBoxAuto } ){
			void* w = client::LoginWindow( k );
			// a da exclusão continua criada (escondida) enquanto o pedido de e-mail está aberto
			if( w && Visivel( w ) && client::LoginWindowRect( k, r ) && r.w > 0 ) return w;
		}
		return nullptr;
	}

	// texto do campo do pedido de e-mail (cp1252)
	int PromptTextRaw( void* w, char* buf, int cap ){
		int n = 0;
		__try{
			uintptr_t head = *reinterpret_cast<uintptr_t*>( reinterpret_cast<uintptr_t>( w ) + 0x50 ), node = *reinterpret_cast<uintptr_t*>( head );
			for( int i = 0; i < 16 && node && node != head; i++ ){
				uintptr_t c = *reinterpret_cast<uintptr_t*>( node + 8 );
				if( c && *reinterpret_cast<uintptr_t*>( c ) == VT_EDIT ){
					const char* t = *reinterpret_cast<const char**>( c + 0xD8 );
					int len = *reinterpret_cast<int*>( c + 0xE8 );
					if( t && len > 0 && len < cap ){ memcpy( buf, t, len ); n = len; }
					break;
				}
				node = *reinterpret_cast<uintptr_t*>( node );
			}
		}__except( EXCEPTION_EXECUTE_HANDLER ){ n = 0; }
		return n;
	}

	std::string PromptText( void* w ){
		char buf[256];
		int n = PromptTextRaw( w, buf, sizeof( buf ) );
		return std::string( buf, n );
	}

	unsigned FilhosDaJanela( void* w ){
		__try{ return *reinterpret_cast<unsigned*>( reinterpret_cast<uintptr_t>( w ) + 0x54 ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return 0; }
	}

	// botões (UIButton 0x1009910) da mensagem nativa: os pontos certos para o clique em qualquer tamanho de janela
	int MessageButtons( void* w, float* bx, float* by, uintptr_t* btn ){
		int n = 0;
		__try{
			uintptr_t head = *reinterpret_cast<uintptr_t*>( reinterpret_cast<uintptr_t>( w ) + 0x50 ), node = *reinterpret_cast<uintptr_t*>( head );
			for( int i = 0; i < 16 && node && node != head && n < 3; i++ ){
				uintptr_t c = *reinterpret_cast<uintptr_t*>( node + 8 );
				if( c && *reinterpret_cast<uintptr_t*>( c ) == 0x1009910 ){
					int x = *reinterpret_cast<int*>( c + 0x1C ), y = *reinterpret_cast<int*>( c + 0x20 );
					int cw = *reinterpret_cast<int*>( c + 0x14 ), ch = *reinterpret_cast<int*>( c + 0x18 );
					if( x > -1000 && cw > 0 ){ bx[n] = x + cw * 0.5f; by[n] = y + ch * 0.5f; btn[n] = c; n++; }
				}
				node = *reinterpret_cast<uintptr_t*>( node );
			}
		}__except( EXCEPTION_EXECUTE_HANDLER ){}
		for( int i = 0; i < n; i++ ) for( int j = i + 1; j < n; j++ ) if( bx[j] < bx[i] ){ std::swap( bx[i], bx[j] ); std::swap( by[i], by[j] ); std::swap( btn[i], btn[j] ); }
		return n;
	}

	void PressMessageButton( uintptr_t w, uintptr_t btn ){
		int id = 0;
		__try{
			uintptr_t vt = *reinterpret_cast<uintptr_t*>( w );
			id = *reinterpret_cast<int*>( btn + 0x2C );
			int state = *reinterpret_cast<int*>( btn + 0x30 );
			reinterpret_cast<int( __thiscall* )( uintptr_t, uintptr_t, int, int, int, int, int )>( *reinterpret_cast<uintptr_t*>( vt + 37 * 4 ) )( w, btn, 6, id, state, 0, 0 );
		}__except( EXCEPTION_EXECUTE_HANDLER ){ id = -1; }
		logger::Write( "Mensagem: botao %d acionado", id );
	}

	void DrawMessageUi( IDirect3DDevice9* dev ){
		if( !g_msgUi || g_msgDrawn ) return;
		g_msgDrawn = true;
		std::string t;
		if( g_msgPrompt ) t = PromptText( g_msgWnd );
		else client::MessageText( g_msgWnd, t );
		int n = MultiByteToWideChar( 1252, 0, t.data(), (int)t.size(), nullptr, 0 );
		std::wstring wt( n, L'\0' );
		MultiByteToWideChar( 1252, 0, t.data(), (int)t.size(), &wt[0], n );
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::MessageFrame f;
		f.vpWidth = (float)vp.Width;
		f.vpHeight = (float)vp.Height;
		f.x = (float)g_msgRect.x; f.y = (float)g_msgRect.y; f.w = (float)g_msgRect.w; f.h = (float)g_msgRect.h;
		f.prompt = g_msgPrompt;
		if( f.prompt ){ f.input = wt; f.text = L"Para apagar o personagem, digite o e-mail da conta."; }
		else f.text = wt;
		f.twoButtons = FilhosDaJanela( g_msgWnd ) >= 3; // 2 = só OK, 3 = OK e cancelar
		f.buttons = MessageButtons( g_msgWnd, f.bx, f.by, f.btn );

		if( f.buttons >= 2 ) f.twoButtons = true;
		if( f.prompt ) f.twoButtons = true; // APAGAR (o OK do cliente) e CANCELAR (Esc)
		ui::DrawMessage( dev, f );
	}

	void DrawToggle( IDirect3DDevice9* dev ){
		if( g_toggleDrawn || !client::IsSelectActive() || !config::Get().uiEnabled || !config::Feature( config::FEAT_TOGGLE ) ) return;
		g_toggleDrawn = true;
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::DrawModeToggle( dev, (float)vp.Width, (float)vp.Height );
	}

	Verdict Classify( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride, const char*& note ){
		const config::Settings& cfg = config::Get();
		note = "";

		// Janela da seleção sendo criada: só o cenário (e o cursor); nada nativo aparece
		if( client::IsCreatingSelect() && config::CustomActive() && config::Feature( config::FEAT_SELECT ) && cfg.mode == config::BackgroundMode::Replace ){
			if( g_cursorHash != 0 && g_drawHash == g_cursorHash ){
				return Verdict::Pass;
			}
			if( g_phase == Phase::Background && CoversViewport( dev, info ) ){
				g_phase = Phase::After;
				if( DrawScene( dev ) ){
					note = "<== FUNDO SUBSTITUIDO (abrindo a selecao)";
					return Verdict::Skip;
				}
			}
			note = "<== abrindo a selecao (escondido)";
			return Verdict::Skip;
		}

		// Seleção original escolhida pelo jogador: o cliente desenha tudo; só a chave de modo é nossa
		// (também quando a seleção/criação não foi escolhida no Aplicador)
		bool selectOff = ( client::IsSelectActive() || client::IsCreatingSelect() ) && !config::Feature( config::FEAT_SELECT );
		bool makeOff = !client::IsSelectActive() && client::IsMakeCharActive() && !config::Feature( config::FEAT_MAKE );
		bool outsideOff = !client::IsSelectActive() && !client::IsMakeCharActive() && !g_loginActive;
		if( !config::CustomActive() || selectOff || makeOff || outsideOff ){
			if( g_cursorHash != 0 && g_drawHash == g_cursorHash ){
				DrawToggle( dev );
				note = "<== cursor (modo original)";
			}else if( client::IsSelectActive() && g_snapshotDirty ){
				if( upData != nullptr && CoversViewport( dev, info ) ){
					ReleaseBgTex();
					dev->GetTexture( 0, &g_bgTex );
					UvRange( upData, upStride, VertexCount( info.type, info.primCount ), g_bgU0, g_bgV0, g_bgU1, g_bgV1 );
				}else if( IsNativeWindow( info ) && RedrawWindowBackground( dev, info, upData, upStride ) ){
					note = "<== fundo da janela refeito (modo original)";
					return Verdict::Skip;
				}
			}
			return Verdict::Pass;
		}


		// com a mensagem aberta o cliente desenha o cursor por outro caminho (laço da janela modal): o desenho
		// pequeno junto do mouse é o cursor; a mensagem nova entra antes dele e ele passa por cima
		if( g_msgUi && info.hasBounds && info.Width() <= 48 && info.Height() <= 48 ){
			input::Point m = input::RealCursor(), off = input::CursorOffset();
			float cx = m.x - off.x, cy = m.y - off.y;
			if( info.minX >= cx - 40 && info.maxX <= cx + 48 && info.minY >= cy - 40 && info.maxY <= cy + 48 ){
				DrawMessageUi( dev );
				if( upData != nullptr && ( off.x != 0.0f || off.y != 0.0f ) ){
					DrawTransformed( dev, info, upData, upStride, 0, 0, off.x, off.y, 1.0f, false );
					note = "<== cursor sobre a mensagem (reposicionado)";
					return Verdict::Skip;
				}
				note = "<== cursor sobre a mensagem";
				return Verdict::Pass;
			}
		}
		// mensagem do cliente redesenhada: a nativa some
		if( g_msgUi && info.hasBounds && g_drawHash != g_cursorHash
			&& info.minX >= g_msgRect.x - 4 && info.maxX <= g_msgRect.x + g_msgRect.w + 4
			&& info.minY >= g_msgRect.y - 4 && info.maxY <= g_msgRect.y + g_msgRect.h + 4 ){
			note = "<== mensagem do cliente (redesenhada)";
			return Verdict::Skip;
		}

		// janela de aviso desenhada pelo próprio cliente (charwnd::InstallMessageBox): passa inteira por cima da
		// seleção/criação e, como janela da frente, pausa a interface (o mouse vai direto a ela)
		if( g_msgNative && info.hasBounds && g_drawHash != g_cursorHash
			&& info.minX >= g_msgRect.x - 4 && info.maxX <= g_msgRect.x + g_msgRect.w + 4
			&& info.minY >= g_msgRect.y - 4 && info.maxY <= g_msgRect.y + g_msgRect.h + 4 ){
			g_popupSeen = true;
			note = "<== aviso do cliente (desenhado por ele)";
			return Verdict::Pass;
		}

		// terminados os personagens: redesenha-os em ordem de profundidade e põe as placas por cima
		if( ( !g_deferred.empty() || g_platesPending ) && ( g_phase == Phase::Sprites || g_phase == Phase::After ) && !charwnd::SelectActive() ){
			bool cardSprite = g_phase == Phase::Sprites && info.hasBounds && g_drawHash != g_cursorHash
				&& ui::CardAt( ( info.minX + info.maxX ) * 0.5f, ( info.minY + info.maxY ) * 0.5f ) >= 0;
			if( !cardSprite ){
				FlushDeferred( dev );
			}
		}

		// Cursor do cliente: desenhado onde o cliente "acha" que o mouse está; volta para a posição real
		if( g_cursorHash != 0 && g_drawHash == g_cursorHash ){
			if( g_makeUiPending ){
				DrawMakeUi( dev ); // a interface da criação fica embaixo do cursor
			}
			FlushDeferred( dev );
			DrawToggle( dev );
			DrawMessageUi( dev );
			input::Point off = input::CursorOffset();
			if( upData != nullptr && ( off.x != 0.0f || off.y != 0.0f ) ){
				DrawTransformed( dev, info, upData, upStride, 0, 0, off.x, off.y, 1.0f, false );
				note = "<== cursor reposicionado";
				return Verdict::Skip;
			}
			note = "<== cursor";
			return Verdict::Pass;
		}

		// Telas de entrada: mapa ao fundo, interface O Codex e janelas nativas escondidas
		if( g_loginActive && LoginPhase() ){
			if( g_phase == Phase::Background && CoversViewport( dev, info ) ){
				g_phase = Phase::After;
				if( !DrawScene( dev ) ){
					FillDark( dev );
				}
				DrawLoginUi( dev );
				note = "<== FUNDO + LOGIN O CODEX";
				return Verdict::Skip;
			}
			if( InLoginWindow( info ) ){
				note = "<== janela de entrada (escondida)";
				return Verdict::Skip;
			}
			if( info.hasBounds && info.Width() > SMALL_DRAW && info.Height() > SMALL_DRAW && info.texWidth > 1 ){
				g_popupSeen = true; // mensagem do cliente por cima
				note = "<== mensagem do cliente";
			}
			return Verdict::Pass;
		}

		// Tela de criação: só o fundo vira o mapa; o resto é do cliente
		if( !client::IsSelectActive() ){
			if( IsPreviewSprite( info ) ){
				if( upData != nullptr ){
					DrawTransformed( dev, info, upData, upStride, g_preview.ax, g_preview.ay, g_preview.tx, g_preview.ty,
						g_preview.scale, cfg.pixelated );
				}
				note = "<== previa da criacao";
				return Verdict::Skip;
			}

			if( g_phase == Phase::Background && cfg.mode == config::BackgroundMode::Replace
				&& ( CoversViewport( dev, info ) || MatchesBackgroundTexture( info ) ) ){
				g_phase = Phase::After;
				if( DrawScene( dev ) ){
					DrawMakePreview( dev );
					note = "<== FUNDO SUBSTITUIDO (criacao)";
					return Verdict::Skip;
				}
			}
			// Restos da seleção e a moldura com a cópia congelada da tela anterior
			if( ( info.texWidth == cfg.nativeWidth && info.texHeight == cfg.nativeHeight )
				|| ( info.texWidth == cfg.makeBgWidth && info.texHeight == cfg.makeBgHeight ) ){
				note = "<== moldura da criacao (escondida)";
				return Verdict::Skip;
			}
			if( charwnd::MakeActive() ) return Verdict::Pass; // criação hardcoded: a janela é do cliente

			if( cfg.uiEnabled && client::IsMakeCharActive() ){
				// janela nativa: guarda os blocos e esconde; a interface nova vem logo depois
				if( IsMakeTile( info ) ){
					if( !g_makeUiPending ){
						ClearMakeTiles();
						g_makeUiPending = true;
					}
					RecordMakeTile( dev, info, upData, upStride );
					note = "<== bloco da criacao (escondido)";
					return Verdict::Skip;
				}
				if( g_makeUiPending ){
					DrawMakeUi( dev );
				}
				// dicas dos botões nativos ("Pra Direita"...) e outros pedaços pequenos da janela escondida
				if( info.hasBounds && g_makeRect.w > 0 && info.Height() < 48.0f && info.Width() < 320.0f
					&& info.minX >= g_makeRect.x - 200 && info.maxX <= g_makeRect.x + g_makeRect.w + 200
					&& info.minY >= g_makeRect.y - 60 && info.maxY <= g_makeRect.y + g_makeRect.h + 60 ){
					note = "<== dica/detalhe da criacao (escondido)";
					return Verdict::Skip;
				}
				// outra janela grande por cima (aviso do cliente): deixa passar e pausa a interface
				if( info.hasBounds && info.Width() > SMALL_DRAW && info.Height() > SMALL_DRAW && info.texWidth > 1 ){
					g_popupSeen = true;
				}
			}
			return Verdict::Pass;
		}

		// Seleção hardcoded: a janela (tela cheia) e os filhos são do cliente; o fundo vira o cenário e os bonecos
		// (nos lugares originais dos cartões) vão para o mapa e para os retratos das linhas
		if( charwnd::SelectActive() ){
			client::WndRect sr;
			{ // primeiro quadro da seleção: a janela ainda no tamanho original (antes da troca) -> só o cenário
				uintptr_t sw = reinterpret_cast<uintptr_t>( client::SelectWindow() );
				D3DVIEWPORT9 vp0;
				dev->GetViewport( &vp0 );
				if( sw && *reinterpret_cast<int*>( sw + client::OFS_WND_WIDTH ) != (int)vp0.Width ){
					if( g_phase == Phase::Background && CoversViewport( dev, info ) ){
						g_phase = Phase::After;
						DrawScene( dev );
					}
					note = "<== selecao ainda nao trocada (escondida)";
					return Verdict::Skip;
				}
			}
			(void)sr;
			if( g_phase == Phase::Background || g_phase == Phase::Window ){
				bool bg = cfg.mode == config::BackgroundMode::Replace && g_phase == Phase::Background
					&& ( CoversViewport( dev, info ) || MatchesBackgroundTexture( info ) ) && !IsSelectTile( dev, info );
				g_phase = Phase::Sprites;
				g_originX = 0.0f; g_originY = 0.0f;
				DrawScene( dev ); // o primeiro desenho do quadro: o cenário fica atrás de tudo
				D3DVIEWPORT9 vp;
				dev->GetViewport( &vp );
				input::SetViewport( (float)vp.Width, (float)vp.Height );
				ui::NativeFrame frame;
				frame.vpWidth = (float)vp.Width;
				frame.vpHeight = (float)vp.Height;
				frame.popupActive = g_popupActive;
				if( cfg.uiEnabled ) ui::Draw( dev, frame );
				g_platesPending = true;
				if( bg ){
					note = "<== FUNDO SUBSTITUIDO (selecao hardcoded)";
					return Verdict::Skip;
				}
			}
			if( IsSelectTile( dev, info ) ){
				note = "<== janela da selecao";
				return Verdict::Pass;
			}
			if( IsNativeWindow( info ) ){ // moldura antiga (997x626) no lugar original
				note = "<== moldura antiga da selecao (escondida)";
				return Verdict::Skip;
			}
			if( info.hasBounds ){
				float cx = ( info.minX + info.maxX ) * 0.5f, cy = ( info.minY + info.maxY ) * 0.5f;
				int card = ui::CardAt( cx, cy );
				if( card >= 0 && info.Width() < 400.0f && info.Height() < 400.0f ){
					ui::NoteSprite( card, info.minY );
					ui::SpriteXform xf;
					if( upData != nullptr && ui::SceneXform( card, xf ) ) DeferSprite( dev, info, upData, upStride, xf, cfg.pixelated );
					ui::SpriteXform tx = {};
					if( upData != nullptr && ui::ThumbXform( card, tx ) ){
						tx.alpha = 1.0f;
						tx.order = 1e9f; // retrato: depois das linhas da lista (fim do quadro)
						DeferSprite( dev, info, upData, upStride, tx, cfg.pixelated );
					}
					note = "<== personagem (mapa + retrato)";
					return Verdict::Skip;
				}
			}
			return Verdict::Pass;
		}

		switch( g_phase ){
			case Phase::Background:
				if( cfg.mode == config::BackgroundMode::Replace && ( CoversViewport( dev, info ) || MatchesBackgroundTexture( info ) ) ){
					g_phase = Phase::Window;
					if( DrawScene( dev ) ){
						note = "<== FUNDO SUBSTITUIDO";
						return Verdict::Skip;
					}
					return Verdict::Pass;
				}
				// sem fundo em tela cheia: segue para a janela
				[[fallthrough]];

			case Phase::Window:
				if( IsNativeWindow( info ) ){
					g_originX = floorf( info.minX + 0.5f );
					g_originY = floorf( info.minY + 0.5f );
					g_phase = Phase::Tiles;
					if( !cfg.uiEnabled ){
						return Verdict::Pass;
					}
					D3DVIEWPORT9 vp;
					dev->GetViewport( &vp );
					input::SetViewport( (float)vp.Width, (float)vp.Height );
					ui::NativeFrame frame;
					frame.vpWidth = (float)vp.Width;
					frame.vpHeight = (float)vp.Height;
					frame.originX = g_originX;
					frame.originY = g_originY;
					frame.popupActive = g_popupActive;
					ui::Draw( dev, frame );
					g_platesPending = true;
					note = "<== janela nativa -> interface nova";
					return Verdict::Skip;
				}
				return Verdict::Pass;

			case Phase::Tiles:
				if( IsNativeTile( info ) ){
					note = "<== bloco da janela (escondido)";
					return cfg.uiEnabled ? Verdict::Skip : Verdict::Pass;
				}
				g_phase = Phase::Sprites;
				[[fallthrough]];

			case Phase::Sprites:
				if( cfg.uiEnabled && info.hasBounds ){
					float cx = ( info.minX + info.maxX ) * 0.5f, cy = ( info.minY + info.maxY ) * 0.5f;
					int card = ui::CardAt( cx, cy );
					if( card >= 0 ){
						// Cada personagem é redesenhado no mapa e na miniatura da lista
						ui::NoteSprite( card, info.minY );
						ui::SpriteXform xf;
						bool drawn = false;
						if( upData != nullptr && ui::SceneXform( card, xf ) ){
							DeferSprite( dev, info, upData, upStride, xf, cfg.pixelated );
							drawn = true;
						}
						if( upData != nullptr && ui::ThumbXform( card, xf ) ){
							DrawTransformed( dev, info, upData, upStride, xf.anchorX, xf.anchorY,
								xf.targetX, xf.targetY, xf.scale, cfg.pixelated, xf.brightness, &xf.clip );
							drawn = true;
						}
						note = drawn ? ( card == client::IndexInPage() ? "<== personagem selecionado (mapa + lista)" : "<== personagem (mapa + lista)" )
							: "<== detalhe do cartao (escondido)";
						return Verdict::Skip;
					}
					if( info.Width() <= SMALL_DRAW && info.Height() <= SMALL_DRAW
						&& cx >= g_originX && cx < g_originX + cfg.nativeWidth && cy >= g_originY && cy < g_originY + cfg.nativeHeight ){
						note = "<== detalhe da janela (escondido)";
						return Verdict::Skip;
					}
				}
				g_phase = Phase::After;
				[[fallthrough]];

			case Phase::After:
				if( info.hasBounds && ( info.Width() > SMALL_DRAW || info.Height() > SMALL_DRAW ) ){
					g_popupSeen = true;
					note = "<== janela do cliente";
				}
				return Verdict::Pass;
		}
		return Verdict::Pass;
	}

	// Desenho indo para a tela (back buffer)? O cliente às vezes desenha janelas numa textura
	// (foto da janela ao abrir); esses desenhos não são a tela e não podem mudar as etapas.
	bool DrawingToScreen( IDirect3DDevice9* dev ){
		IDirect3DSurface9* rt = nullptr;
		IDirect3DSurface9* bb = nullptr;
		bool screen = true;
		if( SUCCEEDED( dev->GetRenderTarget( 0, &rt ) ) && SUCCEEDED( dev->GetBackBuffer( 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb ) ) ){
			screen = rt == bb;
		}
		if( rt ) rt->Release();
		if( bb ) bb->Release();
		return screen;
	}

	// ---------------------------------------------------------------- skin: preenchimentos lisos
	// Fundos claros pintados pelo código (sem textura), como a lista de habilidades: viram o
	// azul-marinho da skin (os textos claros da skin precisam de fundo escuro).
	std::vector<BYTE> g_fillCopy;

	DWORD SkinFillColor( DWORD c ){
		float a = ( c >> 24 ) / 255.0f;
		float r = ( ( c >> 16 ) & 0xFF ) / 255.0f, g = ( ( c >> 8 ) & 0xFF ) / 255.0f, b = ( c & 0xFF ) / 255.0f;
		float mx = std::max( r, std::max( g, b ) ), mn = std::min( r, std::min( g, b ) );
		float l = ( mx + mn ) * 0.5f, d = mx - mn;
		float s = d < 1e-4f ? 0 : d / ( 1 - fabsf( 2 * l - 1 ) );
		if( l < 0.62f || s > 0.55f ) return c;
		float nl = 0.055f + ( 1 - l ) * 0.34f;
		// azul-marinho (matiz 216, saturação 0.42)
		float cc = ( 1 - fabsf( 2 * nl - 1 ) ) * 0.42f, m = nl - cc * 0.5f;
		float hp = 216.0f / 60.0f, x = cc * ( 1 - fabsf( fmodf( hp, 2.0f ) - 1 ) );
		float rr = m, gg = x + m, bb = cc + m; // 180..240 graus: (0, x, c)
		(void)a;
		return ( c & 0xFF000000 ) | ( (DWORD)( rr * 255 + 0.5f ) << 16 ) | ( (DWORD)( gg * 255 + 0.5f ) << 8 ) | (DWORD)( bb * 255 + 0.5f );
	}

	bool SkinFill( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT prims, const void* data, UINT stride ){
		if( data == nullptr || stride < 20 ) return false;
		DWORD fvf = 0;
		dev->GetFVF( &fvf );
		if( ( fvf & D3DFVF_POSITION_MASK ) != D3DFVF_XYZRHW || !( fvf & D3DFVF_DIFFUSE ) ) return false;
		IDirect3DBaseTexture9* tex = nullptr;
		dev->GetTexture( 0, &tex );
		if( tex ){ tex->Release(); return false; }
		UINT count = VertexCount( type, prims );
		g_fillCopy.assign( static_cast<const BYTE*>( data ), static_cast<const BYTE*>( data ) + count * stride );
		bool changed = false;
		for( UINT i = 0; i < count; i++ ){
			DWORD* c = reinterpret_cast<DWORD*>( g_fillCopy.data() + i * stride + 16 );
			DWORD n = SkinFillColor( *c );
			if( n != *c ){ *c = n; changed = true; }
		}
		if( !changed ) return false;
		o_DrawPrimitiveUP( dev, type, prims, g_fillCopy.data(), stride );
		return true;
	}

	// ---------------------------------------------------------------- dentro do jogo
	// Janelas nativas substituídas: a barra de status (UIBasicInfoWnd), a descrição do item
	// (UIItemCollectionWnd) e a comparação com o equipado (UIItemCollection_ComparisonWnd).
	// Os blocos de textura delas somem e, no lugar do primeiro, entra o desenho O Codex
	// (mantém a ordem das janelas: o que estiver por cima continua por cima).
	struct WindowTile {
		IDirect3DBaseTexture9* tex = nullptr;
		float x0, y0, x1, y1, u0, v0, u1, v1;
	};
	struct ItemWnd {
		bool open = false;
		client::WndRect rect;
		std::vector<WindowTile> tiles, prev; // deste frame e do anterior (para copiar a ilustração)
		uint32_t shownId = 0;
	};
	ItemWnd g_itemWnd, g_compareWnd, g_equipWnd, g_invWnd, g_skillWnd, g_statWnd;
	bool g_statDrawn = false;
	DWORD g_itemShownTick = 0; // quando a descrição passou a mostrar o item atual
	bool g_equipDrawn = false, g_invDrawn = false, g_skillDrawn = false;
	int g_equipCharDraws = 0, g_equipCharLog = 0;
	ULONG g_windowHash = 0;        // pilha de chamadas que desenha as janelas (aprendida na barra de status)
	bool g_hasBasic = false, g_basicDrawn = false, g_itemDrawn = false;
	game::MenuState g_menu;            // menu de ícones nativo (escondido embaixo do menu do Zero)
	bool g_hasMenu = false, g_menuDrawn = false;
	client::WndRect g_questRect;         // janela de quests nativa (escondida embaixo da do Zero)
	bool g_hasQuest = false, g_questDrawn = false;
	client::WndRect g_petRect;           // janela de pet nativa (escondida embaixo da Pokédex)
	bool g_hasPet = false, g_petDrawn = false;
	// janelas nativas na frente da de quests/pet (ex.: conquistas): o desenho delas continua
	std::vector<client::WndRect> g_questCovers, g_petCovers;
	std::vector<client::WndRect> g_basicCovers, g_menuCovers, g_itemCovers, g_compareCovers, g_equipCovers, g_invCovers, g_skillCovers, g_statCovers;
	// Fase do quadro de cada janela substituída. O renderizador desenha as janelas de trás para a frente,
	// todas pela mesma pilha: 0 = antes do primeiro bloco dela (janelas de trás, somem embaixo da nossa),
	// 1 = a própria nativa (some sempre, até embaixo de uma janela arrastada, que fica transparente),
	// 2 = janelas da frente (passam onde estão)
	struct WndPhase { int st = 0; };
	WndPhase g_questPh, g_petPh, g_menuPh, g_basicPh, g_invPh, g_itemPh, g_cmpPh, g_eqPh, g_skPh, g_stPh;
	bool g_cmpDrawn = false;
	// posição relida no primeiro desenho do quadro: ao arrastar, a janela anda depois do fim do quadro
	// anterior e o retângulo antigo deixaria a nativa aparecer
	bool g_wndRectFresh = false;
	bool g_skinFills = false; // dentro do jogo com a skin escura
	client::WndRect g_basicRect;
	client::WndRect g_gageRect;     // barra de HP/SP embaixo do personagem (UIPlayerGage)
	bool g_hasGage = false, g_gageDrawn = false;
	client::PartyGage g_party[32];     // barras do grupo (UIPcGage) deste quadro
	bool g_partyDrawn[32] = {};
	int g_partyCount = 0;

	void DrawZeroMenuUi( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::DrawZeroMenu( dev, g_menu, (float)vp.Width, (float)vp.Height );
	}

	// Partes do HUD desenhadas em outro lugar/tamanho (buffs, minimapa, atalhos): onde aparecem e como voltar
	// ao ponto nativo (nativo = a + (tela - o) / k), para o mouse do cliente cair no controle certo
	struct Remap { float x0, y0, x1, y1, ax, ay, ox, oy, k; };
	std::mutex g_buffLock; // protege os Remap (o redirecionamento do mouse roda em outra thread)

	// Ícones de status (buffs): a coluna nativa da direita (32x32 em x = largura - 48, desenhados pelo mesmo
	// caminho das janelas) vira uma linha ao lado do cartão de HP, no tamanho original (32 px) e sem fundo. Cada ícone vai
	// para a posição da ordem dele na coluna (topos do quadro anterior).
	constexpr float BUFF_ICON = 32.0f, BUFF_FRAME = 32.0f, BUFF_STEP = 34.0f, BUFF_GAP = 10.0f, BUFF_RIGHT = 300.0f; // BUFF_RIGHT: minimapa
	std::vector<Remap> g_buffRects, g_buffRectsPrev;
	std::vector<float> g_buffTops, g_buffTopsPrev;      // topo de cada ícone na coluna nativa
	std::vector<int> g_buffFramed;                      // molduras já desenhadas neste quadro
	struct TipBounds { bool on = false; float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX; };
	TipBounds g_tip, g_tipPrev;                         // tooltip do ícone (coordenadas nativas)
	int g_tipSlot = -1, g_tipSlotPrev = -1;

	const Remap* BuffHit( float x, float y ){
		for( const Remap& r : g_buffRectsPrev ) if( x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1 ) return &r;
		return nullptr;
	}

	int TopIndex( const std::vector<float>& tops, float y, float* top = nullptr ){
		for( size_t i = 0; i < tops.size(); i++ ){
			if( y >= tops[i] - 1.0f && y < tops[i] + 30.0f ){ if( top ) *top = tops[i]; return (int)i; }
		}
		return -1;
	}

	// posição do ícone cujo bloco começa em minY: índice na coluna do quadro anterior (já ordenada)
	Remap BuffSlot( float minY, float screenW, int& slot ){
		float top = minY;
		if( TopIndex( g_buffTops, minY ) < 0 ) g_buffTops.push_back( minY );
		slot = TopIndex( g_buffTopsPrev, minY, &top );
		if( slot < 0 ){
			slot = 0;
			for( float t : g_buffTopsPrev ) if( t < minY ) slot++;
		}
		float rowX = g_basicRect.x + g_basicRect.w + BUFF_GAP, rowY = g_basicRect.y + 2.0f;
		int perRow = std::max( 1, (int)( ( screenW - BUFF_RIGHT - rowX ) / BUFF_STEP ) );
		float fx = rowX + ( slot % perRow ) * BUFF_STEP, fy = rowY + ( slot / perRow ) * BUFF_STEP;
		float pad = ( BUFF_FRAME - BUFF_ICON ) * 0.5f;
		return { fx, fy, fx + BUFF_FRAME, fy + BUFF_FRAME, screenW - 48.0f, top, fx + pad, fy + pad, BUFF_ICON / 32.0f };
	}

	// Minimapa: o mapa (textura 512x512 da janela nativa, 128x128) ampliado 2x num quadrado no canto superior
	// direito; seta do personagem e marcadores só mudam de lugar; o resto da janela (zoom, coordenadas) some
	constexpr float MM_K = 2.0f, MM_SIZE = 256.0f, MM_MARGIN = 12.0f;
	client::WndRect g_mmRect;
	bool g_hasMm = false, g_mmFramed = false;
	float g_mmX = 0, g_mmY = 0;   // canto do mapa na tela (= canto do mapa nativo)
	Remap g_mmRemap = {};

	// Barra de atalhos: janela nativa (escala SC_K) com o fundo trocado pelos quadrados cinza da referência
	// (a imagem shortcut_frame vira transparente pela char.grf); números e botões da direita ficam de fora
	constexpr float SC_K = 1.0f, SC_SLOT_X = 15, SC_PITCH = 29, SC_SLOT = 27, SC_ROW = 34, SC_SLOT_Y = 4;
	constexpr int SC_COLS = 9;
	client::WndRect g_scRect;
	bool g_hasSc = false, g_scDrawn = false;
	int g_scRows = 1;
	Remap g_scRemap = {};

	bool RemapHit( const Remap& r, float x, float y ){ return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1; }

	// Chat no centro, sem fundo
	client::WndRect g_chatRect;
	bool g_hasChat = false;

	// Só digitando (Enter) a janela do chat aparece; fora isso ela fica fora da tela (não pega os cliques no
	// chão) e só as mensagens novas aparecem, centradas e com fade, no lugar dela (g_chatSpot)
	bool g_chatTyping = false;
	client::WndRect g_chatSpot;

	constexpr DWORD FEED_SHOW_MS = 7000, FEED_FADE_MS = 1200;
	constexpr size_t FEED_MAX = 6;
	struct FeedLine { std::wstring text; uint32_t color; DWORD tick; };
	std::vector<FeedLine> g_feed;
	size_t g_feedTotal = 0;
	std::wstring g_feedLast;
	bool g_feedInit = false;

	// mensagens que chegaram desde o último quadro (a aba cresce; cheia, a mais antiga sai e a última muda)
	void UpdateChatFeed(){
		std::vector<game::ChatLine> lines;
		size_t total = 0;
		if( !game::ReadChat( lines, total ) ) return;
		std::wstring last = lines.empty() ? L"" : lines.back().text;
		size_t fresh = 0;
		if( !g_feedInit ){ g_feedInit = true; }
		else if( total > g_feedTotal ) fresh = total - g_feedTotal;
		else if( total == g_feedTotal && last != g_feedLast ){
			fresh = 1;
			for( size_t k = lines.size(); k-- > 0; ){
				if( lines[k].text == g_feedLast ){ fresh = lines.size() - 1 - k; break; }
			}
		}
		g_feedTotal = total;
		g_feedLast = last;
		fresh = std::min( fresh, lines.size() );
		DWORD now = GetTickCount();
		for( size_t k = lines.size() - fresh; k < lines.size(); k++ ) g_feed.push_back( { lines[k].text, lines[k].color, now } );
		while( !g_feed.empty() && ( g_feed.size() > FEED_MAX || now - g_feed.front().tick > FEED_SHOW_MS + FEED_FADE_MS ) ) g_feed.erase( g_feed.begin() );
	}

	void DrawChatFeed( IDirect3DDevice9* dev ){
		DWORD now = GetTickCount();
		std::vector<ui::ChatFeedLine> out;
		for( const FeedLine& f : g_feed ){
			DWORD age = now - f.tick;
			float a = age <= FEED_SHOW_MS ? 1.0f : 1.0f - (float)( age - FEED_SHOW_MS ) / FEED_FADE_MS;
			if( a > 0 ) out.push_back( { f.text, f.color, a } );
		}
		ui::DrawChatFeed( dev, g_chatSpot.x + g_chatSpot.w * 0.5f, (float)( g_chatSpot.y + g_chatSpot.h - 4 ), g_chatSpot.w - 20.0f, out );
	}

	constexpr float QUEST_W = 592, QUEST_H = 435, POKEDEX_W = 575, POKEDEX_H = 490;
	constexpr int INV_W = 312, INV_H = 283; // inventário: 8 colunas x 7 linhas (máximo de largura do cliente; a altura mínima é a das abas)

	// a barra do personagem é desenhada depois das janelas: some onde a janela de quests/Pokédex está
	bool GageCovered( const client::WndRect& g ){
		float cx = g.x + g.w * 0.5f, cy = g.y + g.h * 0.5f;
		auto in = [&]( const client::WndRect& r, float w, float h ){ return cx >= r.x && cx < r.x + std::max( (float)r.w, w ) && cy >= r.y && cy < r.y + std::max( (float)r.h, h ); };
		return ( g_hasQuest && in( g_questRect, QUEST_W, QUEST_H ) ) || ( g_hasPet && in( g_petRect, POKEDEX_W, POKEDEX_H ) );
	}

	void DrawPlayerGage( IDirect3DDevice9* dev ){
		game::Status st;
		if( game::ReadStatus( st ) ){
			ui::DrawGageBars( dev, (float)g_gageRect.x, (float)g_gageRect.y, (float)g_gageRect.w, (float)g_gageRect.h,
				st.hp, st.maxHp, st.sp, st.maxSp, true, config::Get().playerBarDown );
		}
	}
	std::map<uint32_t, IDirect3DTexture9*> g_itemImages; // ilustrações já vistas (por item)
	constexpr float ILLU_X = 12, ILLU_Y = 11, ILLU_W = 74, ILLU_H = 101; // ilustração na janela nativa
	constexpr UINT ILLU_TEX_W = 128, ILLU_TEX_H = 172;

	bool GameHudOn( unsigned feature ){
		return client::InGame() && config::Get().uiEnabled && config::Feature( feature );
	}

	void ReleaseTiles( std::vector<WindowTile>& v ){
		for( WindowTile& t : v ) if( t.tex ) t.tex->Release();
		v.clear();
	}

	void ReleaseItemImages(){
		for( auto& kv : g_itemImages ) if( kv.second ) kv.second->Release();
		g_itemImages.clear();
	}

	bool Within( const DrawInfo& info, const client::WndRect& r, float m ){
		return info.hasBounds && info.minX >= r.x - m && info.maxX <= r.x + r.w + m && info.minY >= r.y - m && info.maxY <= r.y + r.h + m;
	}

	bool FirstBlockOf( const DrawInfo& info, const client::WndRect& r ){
		return fabsf( info.minX - r.x ) <= 1 && fabsf( info.minY - r.y ) <= 1;
	}

	bool WithinAny( const DrawInfo& info, const std::vector<client::WndRect>& rects ){
		for( const client::WndRect& r : rects ) if( Within( info, r, 1.0f ) ) return true;
		return false;
	}

	// a nativa acabou quando começa uma janela da frente: o primeiro bloco dela (canto) ou um desenho
	// dela fora da área da substituída
	void Advance( WndPhase& ph, const client::WndRect& r, const std::vector<client::WndRect>& covers, const DrawInfo& info ){
		if( ph.st != 1 || covers.empty() ) return;
		bool corner = false;
		for( const client::WndRect& c : covers ) corner = corner || FirstBlockOf( info, c );
		if( corner || ( WithinAny( info, covers ) && !Within( info, r, 1.0f ) ) ) ph.st = 2;
	}

	// desenho de uma janela da frente em cima da substituída: passa
	bool InFront( const WndPhase& ph, const DrawInfo& info, const std::vector<client::WndRect>& covers ){
		return ph.st == 2 && WithinAny( info, covers );
	}

	// As ilustrações das coleções têm fundo branco no próprio arquivo: o branco ligado à borda
	// (preenchimento a partir das bordas) fica transparente; o branco dentro do item continua.
	IDirect3DTexture9* RemoveWhiteBackground( IDirect3DDevice9* dev, IDirect3DSurface9* rt, UINT tw, UINT th, bool wide = false ){
		IDirect3DSurface9* sys = nullptr;
		if( FAILED( dev->CreateOffscreenPlainSurface( tw, th, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr ) ) ) return nullptr;
		if( FAILED( dev->GetRenderTargetData( rt, sys ) ) ){ sys->Release(); return nullptr; }
		D3DLOCKED_RECT lr;
		if( FAILED( sys->LockRect( &lr, nullptr, 0 ) ) ){ sys->Release(); return nullptr; }
		const int W = (int)tw, H = (int)th;
		std::vector<DWORD> px( W * H );
		for( int y = 0; y < H; y++ ) memcpy( &px[y * W], (BYTE*)lr.pBits + y * lr.Pitch, W * 4 );
		sys->UnlockRect();
		sys->Release();
		auto light = [&]( DWORD c ){
			int r = ( c >> 16 ) & 0xFF, g = ( c >> 8 ) & 0xFF, b = c & 0xFF;
			int sat = std::max( r, std::max( g, b ) ) - std::min( r, std::min( g, b ) );
			// wide: fundo do personagem na janela de equipamentos (listras cinza, degradê azulado e as asas da aba Visual)
			if( wide ) return ( r * 3 + g * 6 + b ) / 10 > 165 && sat < 64;
			return r > 228 && g > 228 && b > 228 && sat < 24;
		};
		std::vector<uint8_t> bg( W * H, 0 );
		std::vector<int> stack;
		for( int x = 0; x < W; x++ ){ stack.push_back( x ); stack.push_back( ( H - 1 ) * W + x ); }
		for( int y = 0; y < H; y++ ){ stack.push_back( y * W ); stack.push_back( y * W + W - 1 ); }
		while( !stack.empty() ){
			int i = stack.back(); stack.pop_back();
			if( bg[i] || !light( px[i] ) ) continue;
			bg[i] = 1;
			int x = i % W, y = i / W;
			if( x > 0 ) stack.push_back( i - 1 );
			if( x < W - 1 ) stack.push_back( i + 1 );
			if( y > 0 ) stack.push_back( i - W );
			if( y < H - 1 ) stack.push_back( i + W );
		}
		for( int i = 0; i < W * H; i++ ){
			if( bg[i] ){ px[i] = 0; continue; }
			// borda suave: vizinho do fundo e claro -> meio transparente
			int x = i % W, y = i / W;
			bool edge = ( x > 0 && bg[i - 1] ) || ( x < W - 1 && bg[i + 1] ) || ( y > 0 && bg[i - W] ) || ( y < H - 1 && bg[i + W] );
			if( edge && light( px[i] ) ) px[i] = ( px[i] & 0x00FFFFFF ) | 0x60000000;
		}
		IDirect3DTexture9 *stage = nullptr, *out = nullptr;
		if( FAILED( dev->CreateTexture( W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &stage, nullptr ) ) ) return nullptr;
		if( SUCCEEDED( stage->LockRect( 0, &lr, nullptr, 0 ) ) ){
			for( int y = 0; y < H; y++ ) memcpy( (BYTE*)lr.pBits + y * lr.Pitch, &px[y * W], W * 4 );
			stage->UnlockRect( 0 );
			if( SUCCEEDED( dev->CreateTexture( W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr ) ) ){
				if( FAILED( dev->UpdateTexture( stage, out ) ) ){ out->Release(); out = nullptr; }
			}
		}
		stage->Release();
		return out;
	}

	IDirect3DTexture9* CaptureRegion( IDirect3DDevice9* dev, const ItemWnd& w, float sx, float sy, float sw, float sh, UINT tw, UINT th, bool spritesOnly = false );

	// Copia a ilustração da janela nativa (blocos do frame anterior) para uma textura própria
	IDirect3DTexture9* CaptureIllustration( IDirect3DDevice9* dev, const ItemWnd& w ){
		return CaptureRegion( dev, w, ILLU_X, ILLU_Y, ILLU_W, ILLU_H, ILLU_TEX_W, ILLU_TEX_H );
	}

	IDirect3DTexture9* CaptureRegion( IDirect3DDevice9* dev, const ItemWnd& w, float sx, float sy, float sw, float sh, UINT tw, UINT th, bool spritesOnly ){
		if( w.prev.empty() ) return nullptr;
		IDirect3DTexture9* tex = nullptr;
		if( FAILED( dev->CreateTexture( tw, th, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) ) ) return nullptr;
		IDirect3DSurface9 *surf = nullptr, *old = nullptr;
		tex->GetSurfaceLevel( 0, &surf );
		dev->GetRenderTarget( 0, &old );
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		dev->SetRenderTarget( 0, surf );
		dev->Clear( 0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0 );
		float rx = w.rect.x + sx, ry = w.rect.y + sy;
		float kx = tw / sw, ky = th / sh;
		for( const WindowTile& t : w.prev ){
			float ix0 = std::max( t.x0, rx ), iy0 = std::max( t.y0, ry ), ix1 = std::min( t.x1, rx + sw ), iy1 = std::min( t.y1, ry + sh );
			if( ix1 <= ix0 || iy1 <= iy0 || t.x1 <= t.x0 || t.y1 <= t.y0 ) continue;
			auto U = [&]( float px ){ return t.u0 + ( px - t.x0 ) / ( t.x1 - t.x0 ) * ( t.u1 - t.u0 ); };
			auto V = [&]( float py ){ return t.v0 + ( py - t.y0 ) / ( t.y1 - t.y0 ) * ( t.v1 - t.v0 ); };
			gfx::Image img;
			img.tex = static_cast<IDirect3DTexture9*>( t.tex );
			gfx::DrawImage( img, ( ix0 - rx ) * kx, ( iy0 - ry ) * ky, ( ix1 - ix0 ) * kx, ( iy1 - iy0 ) * ky, 0xFFFFFFFF, U( ix0 ), V( iy0 ), U( ix1 ), V( iy1 ) );
		}
		dev->SetRenderTarget( 0, old );
		dev->SetViewport( &vp );
		if( old ) old->Release();
		IDirect3DTexture9* clean = RemoveWhiteBackground( dev, surf, tw, th, spritesOnly );
		if( surf ) surf->Release();
		if( clean ){ tex->Release(); return clean; }
		return tex;
	}

	bool DrawIllustration( IDirect3DDevice9* dev, uint32_t id, float x, float y, float w, float h ){
		// ilustração lida dos arquivos do cliente pelo id (a cópia da janela misturava itens ao trocar de item)
		gfx::Image file;
		if( ui::ItemIllustration( dev, id, file ) && file.width > 0 && file.height > 0 ){
			float k = std::min( w / file.width, h / file.height );
			float dw = file.width * k, dh = file.height * k;
			gfx::DrawImage( file, x + ( w - dw ) * 0.5f, y + ( h - dh ) * 0.5f, dw, dh );
			return true;
		}
		if( g_itemWnd.open && g_itemWnd.shownId == id && GetTickCount() - g_itemShownTick < 400 ) return false; // janela ainda trocando de item
		auto it = g_itemImages.find( id );
		if( it == g_itemImages.end() ){
			const ItemWnd* src = g_itemWnd.open && g_itemWnd.shownId == id ? &g_itemWnd
				: g_compareWnd.open && g_compareWnd.shownId == id ? &g_compareWnd : nullptr;
			IDirect3DTexture9* tex = src ? CaptureIllustration( dev, *src ) : nullptr;
			if( tex == nullptr ) return false;
			it = g_itemImages.emplace( id, tex ).first;
		}
		gfx::Image img;
		img.tex = it->second;
		img.width = ILLU_TEX_W; img.height = ILLU_TEX_H;
		gfx::DrawImage( img, x, y, w, h );
		return true;
	}

	// Faixa nativa das cartas (embaixo da janela): continua sendo a do cliente, com a skin, para
	// os slots serem clicáveis como no padrão. Copiada dos blocos da janela no lugar original.
	constexpr float STRIP_Y = 432;
	void DrawNativeStrip( const ItemWnd& w ){
		float rx = (float)w.rect.x, ry = w.rect.y + STRIP_Y, rw = (float)w.rect.w, rh = w.rect.h - STRIP_Y;
		for( const WindowTile& t : w.prev ){
			float ix0 = std::max( t.x0, rx ), iy0 = std::max( t.y0, ry ), ix1 = std::min( t.x1, rx + rw ), iy1 = std::min( t.y1, ry + rh );
			if( ix1 <= ix0 || iy1 <= iy0 || t.x1 <= t.x0 || t.y1 <= t.y0 ) continue;
			auto U = [&]( float px ){ return t.u0 + ( px - t.x0 ) / ( t.x1 - t.x0 ) * ( t.u1 - t.u0 ); };
			auto V = [&]( float py ){ return t.v0 + ( py - t.y0 ) / ( t.y1 - t.y0 ) * ( t.v1 - t.v0 ); };
			gfx::Image img;
			img.tex = static_cast<IDirect3DTexture9*>( t.tex );
			gfx::DrawImage( img, ix0, iy0, ix1 - ix0, iy1 - iy0, 0xFFFFFFFF, U( ix0 ), V( iy0 ), U( ix1 ), V( iy1 ) );
		}
	}

	// Copia um pedaço da janela nativa (blocos do frame anterior): origem relativa -> destino na tela
	void BlitNative( const ItemWnd& w, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh ){
		float rx = w.rect.x + sx, ry = w.rect.y + sy;
		float kx = dw / sw, ky = dh / sh;
		for( const WindowTile& t : w.prev ){
			float ix0 = std::max( t.x0, rx ), iy0 = std::max( t.y0, ry ), ix1 = std::min( t.x1, rx + sw ), iy1 = std::min( t.y1, ry + sh );
			if( ix1 <= ix0 || iy1 <= iy0 || t.x1 <= t.x0 || t.y1 <= t.y0 ) continue;
			auto U = [&]( float px ){ return t.u0 + ( px - t.x0 ) / ( t.x1 - t.x0 ) * ( t.u1 - t.u0 ); };
			auto V = [&]( float py ){ return t.v0 + ( py - t.y0 ) / ( t.y1 - t.y0 ) * ( t.v1 - t.v0 ); };
			gfx::Image img;
			img.tex = static_cast<IDirect3DTexture9*>( t.tex );
			gfx::DrawImage( img, dx + ( ix0 - rx ) * kx, dy + ( iy0 - ry ) * ky, ( ix1 - ix0 ) * kx, ( iy1 - iy0 ) * ky, 0xFFFFFFFF, U( ix0 ), V( iy0 ), U( ix1 ), V( iy1 ) );
		}
	}

	// Personagem da janela nativa (faz parte da imagem dela): copiado sem o fundo claro, a cada 400 ms
	IDirect3DTexture9* g_equipChar = nullptr;
	DWORD g_equipCharTick = 0;
	// janela de equipamentos de outro jogador (segunda UIEquipWnd, refeita do mesmo jeito)
	ItemWnd g_otherWnd;
	WndPhase g_otPh;
	std::vector<client::WndRect> g_otherCovers;
	bool g_otherDrawn = false;
	IDirect3DTexture9* g_otherChar = nullptr;
	DWORD g_otherCharTick = 0;

	// arraste com o botão esquerdo (andou mais de 6 px desde que apertou): o ícone arrastado é do cliente
	bool g_leftDrag = false;
	void TrackDrag(){
		static bool s_down = false;
		static input::Point s_press;
		bool down = input::IsLeftDown();
		input::Point m = input::RealCursor();
		if( down && !s_down ){ s_press = m; g_leftDrag = false; }
		if( down && fabsf( m.x - s_press.x ) + fabsf( m.y - s_press.y ) > 6 ) g_leftDrag = true;
		if( !down ) g_leftDrag = false;
		s_down = down;
	}

	// ícone que o cliente desenha embaixo do cursor durante um arraste (habilidade levada para os atalhos)
	bool DraggedIcon( const DrawInfo& info ){
		if( !g_leftDrag || info.Width() > 40 || info.Height() > 40 ) return false;
		input::Point m = input::RealCursor();
		float cx = ( info.minX + info.maxX ) * 0.5f, cy = ( info.minY + info.maxY ) * 0.5f;
		return fabsf( cx - m.x ) <= 24 && fabsf( cy - m.y ) <= 24;
	}

	// Dentro de uma janela substituída, depois do painel: cursor, dica e item arrastado seguem o mouse
	// real (o cliente os põe no ponto redirecionado); o resto (ícones nativos etc.) não aparece.
	bool FollowCursor( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride ){
		input::Point off = input::CursorOffset();
		input::Point real = input::RealCursor();
		float rx = real.x - off.x, ry = real.y - off.y;
		// cursor (seta ou mãozinha): junto do ponto que o cliente vê
		bool cursor = info.minX >= rx - 40 && info.maxX <= rx + 48 && info.minY >= ry - 40 && info.maxY <= ry + 48
			&& info.Width() <= 48 && info.Height() <= 48;
		// sem desvio (ponto coberto por uma janela da frente): o cursor do cliente já está no lugar certo
		if( off.x == 0.0f && off.y == 0.0f ) return !cursor;
		if( upData == nullptr ) return true;
		bool tooltip = info.minX >= rx + 2 && info.minX <= rx + 60 && info.minY >= ry - 60 && info.minY <= ry + 80;
		float cx = ( info.minX + info.maxX ) * 0.5f, cy = ( info.minY + info.maxY ) * 0.5f;
		bool dragged = input::IsLeftDown() && fabsf( cx - rx ) <= 18 && fabsf( cy - ry ) <= 18;
		if( cursor || tooltip || dragged ){
			DrawTransformed( dev, info, upData, upStride, 0, 0, off.x, off.y, 1.0f, false );
		}
		return true;
	}

	void DrawSkillWindowUi( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::DrawNativeSkills( dev, (float)g_skillWnd.rect.x, (float)g_skillWnd.rect.y, (float)g_skillWnd.rect.w, (float)g_skillWnd.rect.h,
			[]( float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh ){ BlitNative( g_skillWnd, sx, sy, sw, sh, dx, dy, dw, dh ); } );
	}

	void DrawInventoryWindowUi( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::DrawNativeInventory( dev, (float)g_invWnd.rect.x, (float)g_invWnd.rect.y, (float)g_invWnd.rect.w, (float)g_invWnd.rect.h );
	}

	// pés do personagem que o cliente desenha na janela de equipamentos refeita (original 149): embaixo dos nomes
	// antigos (até y 170), a mesma nas duas janelas (o OnDraw das duas lê o mesmo valor)
	constexpr int CHAR_FEET_Y = 280; // dentro da janela de outro jogador (284 de altura)

	void DrawEquipWindowUi( IDirect3DDevice9* dev, int which = 0 ){
		ItemWnd& w = which ? g_otherWnd : g_equipWnd;
		IDirect3DTexture9*& chr = which ? g_otherChar : g_equipChar;
		DWORD& chrTick = which ? g_otherCharTick : g_equipCharTick;
		if( !w.prev.empty() && ( chr == nullptr || GetTickCount() - chrTick > 400 ) ){
			IDirect3DTexture9* t = nullptr;
			if( gfx::Begin( dev ) ){
				// só o personagem (sem o fundo da janela): o cliente o desenha centrado na largura da janela
				t = CaptureRegion( dev, w, floorf( w.rect.w * 0.5f ) - 25, (float)CHAR_FEET_Y - 113, 50, 136, 100, 272, true );
				gfx::End();
			}
			if( t ){
				if( chr ) chr->Release();
				chr = t;
				chrTick = GetTickCount();
			}
		}
		gfx::Image character;
		character.tex = chr;
		character.width = 100; character.height = 272;
		if( GameHudOn( config::FEAT_HUD ) ){
			// janela Personagem (equipamentos + atributos; de outro jogador, só os equipamentos)
			game::SelectEquipWnd( which );
			ui::DrawCharacterWindow( dev, (float)w.rect.x, (float)w.rect.y, (float)w.rect.w, (float)w.rect.h, character, which );
			game::SelectEquipWnd( 0 );
			return;
		}
		ui::DrawNativeEquip( dev, (float)g_equipWnd.rect.x, (float)g_equipWnd.rect.y, (float)g_equipWnd.rect.w, (float)g_equipWnd.rect.h, character,
			[]( float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh ){ BlitNative( g_equipWnd, sx, sy, sw, sh, dx, dy, dw, dh ); } );
	}

	void DrawItemWindowUi( IDirect3DDevice9* dev, bool compare ){
		game::Item item;
		ItemWnd& w = compare ? g_compareWnd : g_itemWnd;
		if( !w.open || !( compare ? game::ReadCompareWindow( item ) : game::ReadItemWindow( item ) ) ) return;
		w.shownId = item.id;
		if( compare ){ // opções do equipado (pacotes do inventário)
			game::Item inv;
			if( game::EquippedAt( item.wear ? item.wear : item.location, inv ) && inv.id == item.id ){
				for( int k = 0; k < 5; k++ ) item.options[k] = inv.options[k];
			}
		}
		game::ItemNative nat;
		if( !ui::ItemRunesNative() && game::ReadItemNative( compare, nat ) ) ui::DrawItemRunes( dev, (float)w.rect.x, (float)w.rect.y, item, nat );
	}

	void KeepTile( ItemWnd& w, IDirect3DDevice9* dev, const DrawInfo& info, const void* upData, UINT upStride ){
		if( upData == nullptr || upStride < 28 ) return;
		WindowTile t;
		dev->GetTexture( 0, &t.tex );
		t.x0 = info.minX; t.y0 = info.minY; t.x1 = info.maxX; t.y1 = info.maxY;
		UvRange( upData, upStride, VertexCount( info.type, info.primCount ), t.u0, t.v0, t.u1, t.v1 );
		w.tiles.push_back( t );
	}

	// progresso da barra de cast nativa: linha do meio da textura, pixels verdes (preenchido) sobre cinza;
	// false = não é a barra de cast (ex.: a do pet, mesma textura)
	bool CastProgress( IDirect3DDevice9* dev, float& progress ){
		IDirect3DBaseTexture9* base = nullptr;
		IDirect3DTexture9* tex = nullptr;
		bool ok = false;
		if( SUCCEEDED( dev->GetTexture( 0, &base ) ) && base && SUCCEEDED( base->QueryInterface( __uuidof( IDirect3DTexture9 ), reinterpret_cast<void**>( &tex ) ) ) ){
			D3DLOCKED_RECT lr;
			if( SUCCEEDED( tex->LockRect( 0, &lr, nullptr, D3DLOCK_READONLY ) ) ){
				const uint16_t* row = reinterpret_cast<const uint16_t*>( static_cast<const BYTE*>( lr.pBits ) + 4 * lr.Pitch );
				const uint16_t* edge = reinterpret_cast<const uint16_t*>( lr.pBits );
				// a barra do pet usa a mesma textura, mas a linha do meio é toda da cor da borda: não é cast
				bool cast = row[30] != edge[30] || row[10] != edge[10];
				int filled = 0, total = 0;
				for( int x = 1; x < 60; x++ ){
					int r = ( row[x] >> 10 ) & 31, g = ( row[x] >> 5 ) & 31;
					total++;
					if( g > r + 8 ) filled++;
				}
				tex->UnlockRect( 0 );
				progress = total ? (float)filled / total : 0.0f;
				ok = cast;
			}
		}
		if( tex ) tex->Release();
		if( base ) base->Release();
		return ok;
	}

	// true = o desenho do cliente é parte de uma janela substituída (não desenha)
	bool GameWindowDraw( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride, UINT startVertex, bool indexed ){
		// Alt+V recria/redimensiona a barra de status no meio do quadro: janela e retângulo de agora
		// (senão a nativa aparece por um quadro)
		g_hasBasic = GameHudOn( config::FEAT_HUD ) && game::BasicInfoRect( g_basicRect );
		if( ( !g_hasBasic && !g_hasMenu && !g_hasQuest && !g_hasPet && !g_hasGage && g_partyCount == 0 && !g_itemWnd.open && !g_equipWnd.open && !g_otherWnd.open && !g_invWnd.open && !g_skillWnd.open && !g_statWnd.open ) || indexed || !IsPretransformed( dev ) || !DrawingToScreen( dev ) ) return false;
		if( upData != nullptr ) AccumulateBounds( info, upData, upStride, VertexCount( info.type, info.primCount ) );
		else BoundsFromStream( dev, info, startVertex );
		if( !info.hasBounds ) return false;
		// arrastando sobre a janela Personagem: o item arrastado e o cursor (desenhados pela mesma pilha das
		// janelas, por cima de tudo) passam
		if( g_leftDrag && ( ( g_equipWnd.open && Within( info, g_equipWnd.rect, 1.0f ) ) || ( g_otherWnd.open && Within( info, g_otherWnd.rect, 1.0f ) ) )
			&& info.Width() <= 48 && info.Height() <= 48 ){
			input::Point m = input::RealCursor();
			if( info.minX >= m.x - 40 && info.maxX <= m.x + 48 && info.minY >= m.y - 40 && info.maxY <= m.y + 48 ){
				// o cliente os desenha antes das janelas: guardados e desenhados no fim do quadro, por cima dela
				if( upData == nullptr ) return false;
				DeferredSprite d;
				d.order = 0; d.seq = (int)g_onTop.size(); d.type = info.type; d.prims = info.primCount; d.stride = upStride;
				d.point = false; d.scale = 1.0f; d.clip = {};
				TransformCopy( dev, info, upData, upStride, 0, 0, 0, 0, 1.0f, 1.0f, 1.0f, d.verts );
				d.state.Capture( dev );
				g_onTop.push_back( std::move( d ) );
				return true;
			}
		}
		if( g_hasMenu ) game::MenuRect( g_menu.rect ); // o menu muda de tamanho ao abrir/fechar
		// cursor do cliente sobre uma janela nossa (barra de status, menu, quests, Pokédex): o clique vai para um
		// ponto da nativa, o cursor desenhado volta para onde o mouse realmente está
		if( ( g_cursorHash != 0 || g_questDrawn || g_petDrawn ) && upData != nullptr ){
			input::Point off = input::CursorOffset();
			if( off.x != 0.0f || off.y != 0.0f ){
				input::Point real = input::RealCursor();
				float rx = real.x - off.x, ry = real.y - off.y;
				ReadCaller( info );
				// todas as camadas do cursor (aprendido no login); fora das janelas, também o que estiver junto do ponto
				// informado (a mãozinha sobre um botão nativo não vem pela mesma pilha da seta)
				bool nearPoint = info.minX >= rx - 40 && info.maxX <= rx + 48 && info.minY >= ry - 40 && info.maxY <= ry + 48
					&& info.Width() <= 48 && info.Height() <= 48;
				bool cursor = ( g_cursorHash != 0 && g_drawHash == g_cursorHash ) || ( nearPoint && g_drawHash != g_windowHash );
				// clique no nosso X levado ao X nativo: o tooltip que o cliente mostra lá ("Alt + J") some
				if( !cursor && ui::CloseClickActive() && info.minX >= rx - 120 && info.maxX <= rx + 120 && info.minY >= ry - 60
					&& info.maxY <= ry + 60 && info.Height() <= 32 && info.Width() <= 200 ) return true;
				if( cursor ){
					DrawTransformed( dev, info, upData, upStride, 0, 0, off.x, off.y, 1.0f, false );
					return true;
				}
			}
		}
		// barra de cast (textura dinâmica 64x8 A1R5G5B5; o progresso são os pixels verdes do meio) e a caixa do
		// nome da habilidade (retângulo sem textura preto 40%): no estilo da referência
		if( upData != nullptr && info.Height() <= 8 && info.Width() >= 30 && info.Width() <= 90 ){
			ReadTexture( dev, info );
			if( info.texWidth == 64 && info.texHeight == 8 && info.texFormat == D3DFMT_A1R5G5B5 ){
				ReadCaller( info );
				if( g_windowHash != 0 && g_drawHash == g_windowHash ){
					float progress = 0;
					if( CastProgress( dev, progress ) ){
						ui::DrawCastBar( dev, ( info.minX + info.maxX ) * 0.5f, info.minY, info.Width(), progress );
						return true;
					}
				}
			}
		}
		if( upData != nullptr && info.Height() >= 14 && info.Height() <= 28 && info.Width() >= 24 && upStride >= 20 ){
			DWORD fvf = 0;
			dev->GetFVF( &fvf );
			if( ( fvf & D3DFVF_DIFFUSE ) && *reinterpret_cast<const DWORD*>( static_cast<const BYTE*>( upData ) + 16 ) == 0x66000000 ){
				ReadTexture( dev, info );
				ReadCaller( info );
				if( info.texWidth == 0 && g_windowHash != 0 && g_drawHash == g_windowHash ){
					ui::DrawSkillNameBox( dev, info.minX, info.minY, info.Width(), info.Height() );
					return true;
				}
			}
		}
		// pedaços das janelas de equipamentos que caem dentro do minimapa ou da barra de atalhos são delas
		// (tratados mais abaixo), não do minimapa/barra
		bool overEquip = ( g_equipWnd.open && Within( info, g_equipWnd.rect, 1.0f ) ) || ( g_otherWnd.open && Within( info, g_otherWnd.rect, 1.0f ) );
		// minimapa ampliado
		if( g_hasMm && !overEquip && info.minX >= g_mmRect.x - 2 && info.maxX <= g_mmRect.x + g_mmRect.w + 2 && info.minY >= g_mmRect.y - 3
			&& info.maxY <= g_mmRect.y + g_mmRect.h + 2 ){
			ReadCaller( info );
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				if( upData == nullptr ) return true;
				ReadTexture( dev, info );
				Remap m;
				{ std::lock_guard<std::mutex> g( g_buffLock ); m = g_mmRemap; }
				RECT clip = { (LONG)m.x0, (LONG)m.y0, (LONG)m.x1, (LONG)m.y1 };
				if( info.texWidth >= 256 && info.Width() >= 64 ){
					DrawTransformed( dev, info, upData, upStride, m.ax, m.ay, m.ox, m.oy, m.k, false, 1.0f, &clip );
					if( !g_mmFramed ){
						g_mmFramed = true;
						int cx = -1, cy = -1;
						game::PlayerCell( cx, cy );
						std::string map = game::CurrentMap();
						ui::DrawMinimapFrame( dev, m.x0, m.y0, MM_SIZE, map.empty() ? L"" : chardata::MapName( map ), cx, cy );
					}
				}else if( info.Width() >= 64 ){
					if( !config::MinimapIcons() ) return true; // botão "NPC" desligado
					// imagem da janela: ícones de NPC/saídas sobre o mapa (zoom, coordenadas e borda ficam fora do recorte)
					DrawTransformed( dev, info, upData, upStride, m.ax, m.ay, m.ox, m.oy, m.k, true, 1.0f, &clip );
				}else if( info.Width() <= 24 && info.Height() <= 24 ){
					// seta do personagem e marcadores: mesmo tamanho, no lugar ampliado
					float cx = ( info.minX + info.maxX ) * 0.5f, cy = ( info.minY + info.maxY ) * 0.5f;
					DrawTransformed( dev, info, upData, upStride, cx, cy, m.ox + ( cx - m.ax ) * m.k, m.oy + ( cy - m.ay ) * m.k, 1.0f, false, 1.0f, &clip );
				}
				return true;
			}
		}
		// barra de atalhos sobre os quadrados cinza
		if( g_hasSc && !overEquip && Within( info, g_scRect, 1.0f ) ){
			ReadCaller( info );
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				Remap m;
				{ std::lock_guard<std::mutex> g( g_buffLock ); m = g_scRemap; }
				if( !g_scDrawn ){
					g_scDrawn = true;
					ui::DrawShortcutSlots( dev, m.x0, m.y0, SC_SLOT * SC_K, SC_PITCH * SC_K, SC_COLS, g_scRows, SC_ROW * SC_K );
				}
				if( upData == nullptr ) return true;
				RECT clip = { (LONG)m.x0, (LONG)m.y0, (LONG)m.x1, (LONG)m.y1 };
				DrawTransformed( dev, info, upData, upStride, m.ax, m.ay, m.ox, m.oy, m.k, m.k != 1.0f, 1.0f, &clip );
				return true;
			}
		}
		// ícones de buff: da coluna da direita para a linha ao lado do cartão de HP
		// (só a camada da interface, z = 0: o cenário também passa pela coluna em pedaços pequenos, com z > 0)
		if( g_hasBasic && upData != nullptr && info.Width() <= 34 && info.Height() <= 34 && reinterpret_cast<const float*>( upData )[2] < 0.01f ){
			D3DVIEWPORT9 vp;
			dev->GetViewport( &vp );
			if( info.minX >= vp.Width - 52.0f && info.maxX <= vp.Width + 1.0f && info.minY >= 150.0f && info.minY < vp.Height - 120.0f ){
				int slot;
				Remap r;
				{
					std::lock_guard<std::mutex> g( g_buffLock );
					r = BuffSlot( info.minY, (float)vp.Width, slot );
					if( std::find( g_buffFramed.begin(), g_buffFramed.end(), slot ) == g_buffFramed.end() ){
						g_buffFramed.push_back( slot );
						g_buffRects.push_back( r );
					}
				}
				DrawTransformed( dev, info, upData, upStride, r.ax, r.ay, r.ox, r.oy, r.k, false );
				return true;
			}
		}
		// tooltip do ícone sob o mouse (o cliente o põe à esquerda do ícone nativo): embaixo do ícone novo
		if( g_hasBasic && upData != nullptr ){
			input::Point real = input::RealCursor();
			Remap hit = {};
			bool hover = false;
			{
				std::lock_guard<std::mutex> g( g_buffLock );
				if( const Remap* h = BuffHit( real.x, real.y ) ){ hit = *h; hover = true; }
			}
			D3DVIEWPORT9 vp;
			dev->GetViewport( &vp );
			float tx = hit.ax + ( real.x - hit.ox ) / ( hit.k > 0 ? hit.k : 1.0f ), ty = hit.ay + ( real.y - hit.oy ) / ( hit.k > 0 ? hit.k : 1.0f ); // ponto que o cliente vê
			// (só peças com transparência: o cenário também é desenhado em blocos opacos nessa faixa)
			DWORD blend = 0;
			if( hover ) dev->GetRenderState( D3DRS_ALPHABLENDENABLE, &blend );
			if( hover && blend && info.Width() <= 320.0f && info.Height() <= 240.0f && info.minX >= tx - 340.0f
				&& info.minX < vp.Width - 52.0f && info.maxX <= vp.Width - 40.0f
				&& info.minY >= ty - 40.0f && info.maxY <= ty + 260.0f ){
				std::lock_guard<std::mutex> g( g_buffLock );
				int slot = (int)( hit.x0 * 4096 + hit.y0 );
				g_tip.on = true;
				g_tip.x0 = std::min( g_tip.x0, info.minX ); g_tip.y0 = std::min( g_tip.y0, info.minY );
				g_tip.x1 = std::max( g_tip.x1, info.maxX ); g_tip.y1 = std::max( g_tip.y1, info.maxY );
				g_tipSlot = slot;
				if( !g_tipPrev.on || g_tipSlotPrev != slot ) return true; // tamanho ainda desconhecido: aparece no próximo quadro
				float w = g_tipPrev.x1 - g_tipPrev.x0;
				float nx = std::min( hit.x0, vp.Width - 4.0f - w ), ny = hit.y1 + 6.0f;
				DrawTransformed( dev, info, upData, upStride, 0, 0, nx - g_tipPrev.x0, ny - g_tipPrev.y0, 1.0f, false );
				return true;
			}
		}
		// barra embaixo do personagem: as imagens no lugar do bloco da janela nativa
		if( g_hasGage && Within( info, g_gageRect, 1.0f ) ){
			ReadCaller( info );
			if( GageCovered( g_gageRect ) ) return true; // embaixo da janela de quests/Pokédex
			if( g_windowHash == 0 || g_drawHash == g_windowHash ){
				if( !g_gageDrawn ){
					g_gageDrawn = true;
					DrawPlayerGage( dev );
				}
				return true;
			}
		}
		for( int i = 0; i < g_partyCount; i++ ){
			const client::PartyGage& g = g_party[i];
			if( g.inWindow || !Within( info, g.rect, 1.0f ) ) continue;
			ReadCaller( info );
			if( GageCovered( g.rect ) ) return true;
			if( g_windowHash != 0 && g_drawHash != g_windowHash ) break;
			if( !g_partyDrawn[i] ){
				g_partyDrawn[i] = true;
				ui::DrawGageBars( dev, (float)g.rect.x, (float)g.rect.y, (float)g.rect.w, (float)g.rect.h, g.hp, g.maxHp, g.sp, g.maxSp, g.withSp, 0.0f );
			}
			return true;
		}
		// barras dentro da janela de grupo: são pintadas na imagem da própria janela (sem bloco próprio).
		// O bloco DA JANELA DE GRUPO que contém a barra é desenhado e a barra vai logo por cima (um bloco de
		// outra janela atrás dela, ex. o menu, também cobre o ponto: a barra saía antes e a janela a cobria)
		auto partyTile = [&]( const client::PartyGage& g ){
			float cx = g.rect.x + g.rect.w * 0.5f, cy = g.rect.y + g.rect.h * 0.5f;
			if( !g.inWindow || cx < info.minX || cx > info.maxX || cy < info.minY || cy > info.maxY ) return false;
			return ( info.minX >= g.window.x - 1.0f && info.minY >= g.window.y - 1.0f
				&& info.maxX <= g.window.x + g.window.w + 1.0f && info.maxY <= g.window.y + g.window.h + 1.0f );
		};
		bool covers = false;
		for( int i = 0; i < g_partyCount && !covers; i++ ) covers = !g_partyDrawn[i] && partyTile( g_party[i] );
		if( covers ){
			ReadCaller( info );
			if( g_windowHash == 0 || g_drawHash == g_windowHash ){
				if( upData != nullptr ) o_DrawPrimitiveUP( dev, info.type, info.primCount, upData, upStride );
				else o_DrawPrimitive( dev, info.type, startVertex, info.primCount );
				for( int i = 0; i < g_partyCount; i++ ){
					const client::PartyGage& g = g_party[i];
					if( g_partyDrawn[i] || !partyTile( g ) ) continue;
					g_partyDrawn[i] = true;
					ui::DrawGageBars( dev, (float)g.rect.x, (float)g.rect.y, (float)g.rect.w, (float)g.rect.h, g.hp, g.maxHp, g.sp, g.maxSp, g.withSp, 0.0f );
				}
				return true;
			}
		}
		if( !g_wndRectFresh ){
			g_wndRectFresh = true;
			client::WndRect r;
			if( g_hasPet && game::PetWindowRect( r ) ) g_petRect = r;
			if( g_hasQuest && game::QuestWindowRect( r ) ) g_questRect = r;
			// janelas substituídas: posição de agora (ao arrastar, a janela anda depois do fim do quadro anterior
			// e o primeiro bloco dela não bateria com o retângulo velho: o quadro ficava sem o nosso desenho)
			if( g_invWnd.open && game::InventoryWindowRect( r ) ) g_invWnd.rect = r;
			if( g_statWnd.open && game::StatusWindowRect( r ) ) g_statWnd.rect = r;
			if( g_itemWnd.open && game::ItemWindowRect( r ) ) g_itemWnd.rect = r;
			if( g_compareWnd.open && game::CompareWindowRect( r ) ) g_compareWnd.rect = r;
			if( g_equipWnd.open && game::EquipWindowRect( r ) ) g_equipWnd.rect = r;
			game::SelectEquipWnd( 1 );
			if( g_otherWnd.open && game::EquipWindowRect( r ) ) g_otherWnd.rect = r;
			if( g_otherWnd.open ) game::CoversOfEquip( g_otherCovers );
			game::SelectEquipWnd( 0 );
			if( g_skillWnd.open && game::SkillWindowRect( r ) ) g_skillWnd.rect = r;
			if( g_hasBasic && game::BasicInfoRect( r ) ) g_basicRect = r;
			if( g_hasMenu ) game::MenuRect( g_menu.rect );
			if( g_hasPet ) game::PetCovers( g_petCovers );
			if( g_hasQuest ) game::QuestCovers( g_questCovers );
			if( g_hasBasic ) game::CoversOf( game::Replaced::Basic, g_basicCovers );
			if( g_hasMenu ) game::CoversOf( game::Replaced::Menu, g_menuCovers );
			if( g_itemWnd.open ) game::CoversOf( game::Replaced::Item, g_itemCovers );
			if( g_compareWnd.open ) game::CoversOf( game::Replaced::Compare, g_compareCovers );
			if( g_equipWnd.open ) game::CoversOfEquip( g_equipCovers );
			if( g_invWnd.open ) game::CoversOf( game::Replaced::Inventory, g_invCovers );
			if( g_statWnd.open ) game::CoversOf( game::Replaced::Status, g_statCovers );
			if( g_skillWnd.open ) game::CoversOf( game::Replaced::Skill, g_skillCovers );
		}
		if( g_questPh.st == 1 || g_petPh.st == 1 || g_menuPh.st == 1 || g_basicPh.st == 1 || g_invPh.st == 1 || g_itemPh.st == 1 || g_cmpPh.st == 1 || g_eqPh.st == 1 || g_otPh.st == 1 || g_skPh.st == 1 || g_stPh.st == 1 ){
			ReadCaller( info );
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				Advance( g_questPh, g_questRect, g_questCovers, info );
				Advance( g_petPh, g_petRect, g_petCovers, info );
				Advance( g_menuPh, g_menu.rect, g_menuCovers, info );
				client::WndRect bx = g_basicRect;
				bx.h += 12; // botão nativo de expandir, colado embaixo
				Advance( g_basicPh, bx, g_basicCovers, info );
				Advance( g_invPh, g_invWnd.rect, g_invCovers, info );
				Advance( g_itemPh, g_itemWnd.rect, g_itemCovers, info );
				Advance( g_cmpPh, g_compareWnd.rect, g_compareCovers, info );
				Advance( g_eqPh, g_equipWnd.rect, g_equipCovers, info );
				Advance( g_otPh, g_otherWnd.rect, g_otherCovers, info );
				Advance( g_skPh, g_skillWnd.rect, g_skillCovers, info );
				Advance( g_stPh, g_statWnd.rect, g_statCovers, info );
			}
		}
		// pet: a Pokédex no lugar do primeiro bloco da nativa
		if( g_hasPet && Within( info, g_petRect, 1.0f ) && !InFront( g_petPh, info, g_petCovers ) ){
			ReadCaller( info );
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				if( !g_petDrawn ){
					// só no primeiro bloco da própria nativa (canto superior esquerdo): antes dele vêm as janelas
					// de trás, que somem embaixo da Pokédex (maior que a nativa)
					if( !FirstBlockOf( info, g_petRect ) ) return true;
					g_petDrawn = true;
					g_petPh.st = 1;
					ui::DrawPokedex( dev, (float)g_petRect.x, (float)g_petRect.y, (float)g_petRect.w, (float)g_petRect.h );
				}
				return true;
			}
			if( g_petDrawn ) return FollowCursor( dev, info, upData, upStride );
			return false;
		}
		// quests: a janela do Zero no lugar do primeiro bloco da nativa
		if( g_hasQuest && Within( info, g_questRect, 1.0f ) && !InFront( g_questPh, info, g_questCovers ) ){
			ReadCaller( info );
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				if( !g_questDrawn ){
					// só no primeiro bloco da própria nativa (canto superior esquerdo): antes dele vêm as janelas
					// de trás, que somem embaixo da janela do Zero (maior que a nativa)
					if( !FirstBlockOf( info, g_questRect ) ) return true;
					g_questDrawn = true;
					g_questPh.st = 1;
					ui::DrawZeroQuestWindow( dev, (float)g_questRect.x, (float)g_questRect.y, (float)g_questRect.w, (float)g_questRect.h );
				}
				return true;
			}
			if( g_questDrawn ) return FollowCursor( dev, info, upData, upStride );
			return false;
		}
		// menu de ícones: o do Zero no lugar do primeiro bloco da janela nativa
		if( g_hasMenu && Within( info, g_menu.rect, 1.0f ) && !InFront( g_menuPh, info, g_menuCovers ) ){
			ReadCaller( info );
			if( g_windowHash == 0 && fabsf( info.minX - g_menu.rect.x ) <= 1 && fabsf( info.minY - g_menu.rect.y ) <= 1 ){
				g_windowHash = g_drawHash;
			}
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				if( !g_menuDrawn ){
					if( !FirstBlockOf( info, g_menu.rect ) ) return true; // janelas de trás: embaixo do menu
					g_menuDrawn = true;
					g_menuPh.st = 1;
					DrawZeroMenuUi( dev );
				}
				return true;
			}
			if( g_menuDrawn ) return FollowCursor( dev, info, upData, upStride );
			return false;
		}
		client::WndRect basic = g_basicRect;
		basic.h += 10; // botão nativo de expandir, logo abaixo
		bool inBasic = g_hasBasic && Within( info, basic, 1.0f ) && !InFront( g_basicPh, info, g_basicCovers );
		// parte expansível da janela nativa: mesma largura, colada embaixo (quase toda transparente)
		bool attached = g_hasBasic && fabsf( info.minX - g_basicRect.x ) <= 1 && fabsf( info.maxX - ( g_basicRect.x + g_basicRect.w ) ) <= 1
			&& info.minY >= g_basicRect.y + g_basicRect.h - 6 && info.minY <= g_basicRect.y + g_basicRect.h + 2;
		inBasic = inBasic || ( attached && !InFront( g_basicPh, info, g_basicCovers ) );
		bool inItem = g_itemWnd.open && Within( info, g_itemWnd.rect, 1.0f ) && !InFront( g_itemPh, info, g_itemCovers );
		const client::WndRect& ir = g_itemWnd.rect;
		constexpr bool tipInItem = false; // substituída: o cursor e as dicas são do próprio cliente
		bool inCompare = g_itemWnd.open && g_compareWnd.open && Within( info, g_compareWnd.rect, 1.0f ) && !InFront( g_cmpPh, info, g_compareCovers );
		input::Point off = input::CursorOffset();
		bool moved = off.x != 0.0f || off.y != 0.0f;
		const client::WndRect& er = g_equipWnd.rect;
		bool inEquip = g_equipWnd.open && Within( info, er, 1.0f ) && !InFront( g_eqPh, info, g_equipCovers );
		bool inOther = g_otherWnd.open && Within( info, g_otherWnd.rect, 1.0f ) && !InFront( g_otPh, info, g_otherCovers );
		// janela Personagem desenhada pelo próprio cliente: nada nosso nela
		if( charwnd::Active() ) inEquip = inOther = false;
		// as duas janelas de equipamentos se sobrepondo: a que está sendo desenhada agora (a de cima vem depois)
		if( inEquip && inOther ){
			if( g_otPh.st == 1 || ( g_eqPh.st != 1 && g_equipDrawn ) ) inEquip = false;
			else inOther = false;
		}
		constexpr bool tipInEquip = false; // substituída: o cursor e as dicas são do próprio cliente
		const client::WndRect& vr = g_invWnd.rect;
		bool inInv = g_invWnd.open && Within( info, vr, 1.0f ) && !InFront( g_invPh, info, g_invCovers );
		constexpr bool tipInInv = false; // substituída: o cursor e as dicas são do próprio cliente
		const client::WndRect& sr = g_skillWnd.rect;
		bool inSkill = g_skillWnd.open && Within( info, sr, 1.0f ) && !InFront( g_skPh, info, g_skillCovers );
		constexpr bool tipInSkill = false; // substituída: o cursor e as dicas são do próprio cliente
		bool inStat = g_statWnd.open && Within( info, g_statWnd.rect, 1.0f ) && !InFront( g_stPh, info, g_statCovers );
		if( !inBasic && !inItem && !inCompare && !inEquip && !inOther && !inInv && !inSkill && !inStat && !( moved && ( tipInItem || tipInEquip || tipInInv || tipInSkill ) ) ) return false;
		ReadCaller( info );
		if( ( inSkill || tipInSkill ) && !inItem && !tipInItem && !inEquip && !inInv ){
			// substituída: blocos da janela do cliente somem, o nosso desenho entra no lugar do primeiro
			if( g_windowHash != 0 && g_drawHash == g_windowHash && inSkill ){
				if( g_skillDrawn && DraggedIcon( info ) ) return false; // habilidade sendo arrastada: por cima da nossa
				KeepTile( g_skillWnd, dev, info, upData, upStride ); // nível escolhido e SP (texto do cliente)
				if( !g_skillDrawn ){
					if( !FirstBlockOf( info, g_skillWnd.rect ) ) return true;
					g_skillDrawn = true;
					g_skPh.st = 1;
					DrawSkillWindowUi( dev );
				}
				return true;
			}
			return false;
		}
		if( ( inInv || tipInInv ) && !inItem && !tipInItem && !inEquip ){
			// substituído: a janela é a do cliente; no lugar do primeiro bloco dela entra o nosso desenho, os
			// blocos dela somem (as janelas de trás também, embaixo do nosso) e o resto (cursor, item arrastado,
			// dicas) passa
			if( g_windowHash != 0 && g_drawHash == g_windowHash && inInv ){
				if( !g_invDrawn ){
					if( !FirstBlockOf( info, g_invWnd.rect ) ) return true;
					g_invDrawn = true;
					g_invPh.st = 1;
					DrawInventoryWindowUi( dev );
				}
				return true;
			}
			return false;
		}
		if( ( inEquip || tipInEquip ) && !inItem && !tipInItem ){
			// substituída: blocos da janela do cliente somem (guardados para o personagem e as prévias),
			// o nosso desenho entra no lugar do primeiro; o resto (cursor, item arrastado, dicas) passa
			// item arrastado (desenhado pelo cliente junto do mouse, pela mesma pilha das janelas): passa
			if( DraggedIcon( info ) ) return false;
			if( g_windowHash != 0 && g_drawHash == g_windowHash && inEquip ){
				KeepTile( g_equipWnd, dev, info, upData, upStride );
				if( !g_equipDrawn ){
					if( !FirstBlockOf( info, g_equipWnd.rect ) ) return true;
					g_equipDrawn = true;
					g_eqPh.st = 1;
					DrawEquipWindowUi( dev );
				}
				return true;
			}
			return false;
		}
		if( inOther && !inItem && !inCompare ){
			// equipamento de outro jogador: o mesmo da própria
			if( DraggedIcon( info ) ) return false;
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				KeepTile( g_otherWnd, dev, info, upData, upStride );
				if( !g_otherDrawn ){
					if( !FirstBlockOf( info, g_otherWnd.rect ) ) return true;
					g_otherDrawn = true;
					g_otPh.st = 1;
					DrawEquipWindowUi( dev, 1 );
				}
				return true;
			}
			return false;
		}
		if( inStat && !inItem && !inCompare && !inEquip && !inInv && !inSkill ){
			// atributos substituídos: blocos do cliente somem, o nosso desenho no lugar do primeiro
			if( g_windowHash != 0 && g_drawHash == g_windowHash ){
				if( !g_statDrawn ){
					if( !FirstBlockOf( info, g_statWnd.rect ) ) return true;
					g_statDrawn = true;
					g_stPh.st = 1;
					ui::DrawNativeStatus( dev, (float)g_statWnd.rect.x, (float)g_statWnd.rect.y, (float)g_statWnd.rect.w, (float)g_statWnd.rect.h );
				}
				return true;
			}
			return false;
		}
		if( inBasic ){
			if( g_windowHash == 0 && fabsf( info.minX - g_basicRect.x ) <= 1 && fabsf( info.minY - g_basicRect.y ) <= 1 ){
				g_windowHash = g_drawHash; // primeiro bloco da barra: é assim que o cliente desenha janelas
			}
			bool below = attached || info.minY >= g_basicRect.y + g_basicRect.h - 2; // botão de expandir (desenhado à parte)
			if( g_drawHash != g_windowHash && !below ) return false;
			if( !g_basicDrawn && !below && !FirstBlockOf( info, g_basicRect ) ) return true; // janelas de trás
			if( !g_basicDrawn ){
				g_basicDrawn = true;
				g_basicPh.st = 1;
				game::Status st;
				if( game::ReadStatus( st ) ){
					ui::DrawStatusHud( dev, (float)g_basicRect.x, (float)g_basicRect.y, st );
				}
				// mensagens novas do chat (a janela do chat fica fora da tela enquanto não digita)
				if( !g_chatTyping ) DrawChatFeed( dev );
			}
			return true;
		}
		// descrição do item / comparação: a do cliente; depois de cada bloco dela, as runas por cima
		if( g_windowHash != 0 && g_drawHash != g_windowHash ) return false;
		if( ( !inItem && !inCompare ) || upData == nullptr ) return false;
		{ // blocos da própria janela: a partir daqui, uma janela da frente encerra a fase (e as runas param)
			WndPhase& ph = inItem ? g_itemPh : g_cmpPh;
			if( ph.st == 0 ) ph.st = 1;
		}
		o_DrawPrimitiveUP( dev, info.type, info.primCount, upData, upStride );
		DrawItemWindowUi( dev, !inItem );
		return true;
	}

	void FrameItemWnd( ItemWnd& w, bool open, const client::WndRect& r ){
		ReleaseTiles( w.prev );
		w.prev.swap( w.tiles );
		w.open = open;
		if( open ) w.rect = r;
		else{ ReleaseTiles( w.prev ); w.shownId = 0; }
	}

	// Com a comparação aberta, o cliente fecha as duas janelas no clique direito em outro item do inventário
	// sem abrir a dele: o clique é repetido (o segundo abre a descrição pedida, como sem comparação)
	unsigned g_reopenTick = 0; // aperto do botão direito que fechou as duas janelas (repete ao soltar)
	void ReopenAfterCompare( uint32_t shownId, const client::WndRect& item, const client::WndRect& cmp ){
		unsigned tick = 0;
		input::Point at;
		static unsigned s_repeated = 0;
		if( !input::LastRightClick( tick, at ) || tick == s_repeated || GetTickCount() - tick > 500 ) return;
		client::WndRect ir = g_invWnd.rect; // inventário refeito no cliente: sem o desenho por cima, a posição vem dele
		bool iopen = g_invWnd.open;
		if( charwnd::InventoryActive() ) iopen = game::InventoryWindowRect( ir );
		auto in = [&]( const client::WndRect& w ){ return at.x >= w.x && at.y >= w.y && at.x < w.x + w.w && at.y < w.y + w.h; };
		if( config::Get().debug ) logger::Write( "reabrir? clique (%.0f,%.0f) item %d cmp %d inv %d/%d", at.x, at.y, in( item ), in( cmp ), iopen, in( ir ) );
		if( in( item ) || in( cmp ) || !iopen || !in( ir ) ) return;
		game::NativeInventory inv;
		if( !game::ReadNativeInventory( inv ) || inv.cols <= 0 ) return;
		int c = (int)floorf( ( at.x - ir.x - 40 ) / 32 ), rr = (int)floorf( ( at.y - ir.y - 17 ) / 32 );
		if( charwnd::InventoryActive() ){
			c = (int)floorf( ( at.x - ir.x - ui::INV_M ) / ui::INV_PITCH );
			rr = (int)floorf( ( at.y - ir.y - ui::INV_GRID_Y ) / ui::INV_PITCH );
		}
		if( c < 0 || c >= inv.cols || rr < 0 || rr >= inv.rows ) return;
		size_t k = (size_t)( rr * inv.cols + c );
		game::Item it;
		bool got = k < inv.cells.size() && game::InventoryItem( inv.cells[k], it );
		if( config::Get().debug ) logger::Write( "reabrir: célula %d,%d k %d de %d item %u (mostrado %u)", c, rr, (int)k, (int)inv.cells.size(), got ? it.id : 0, shownId );
		if( !got || it.id == shownId ) return; // o mesmo item: fechar era o pedido
		s_repeated = tick;
		g_reopenTick = tick;
	}

	unsigned REOPEN_MS = 700;
	void ReopenPending(){
		static int s_stage = 0, s_wait = 0; // 1 = moveu, esperando para apertar; 2 = apertado, esperando para soltar
		if( s_stage ){
			if( --s_wait > 0 ) return;
			if( s_stage == 1 ){ input::RepeatRightClick( true ); s_stage = 2; s_wait = 3; }
			else{ input::RepeatRightClick( false ); s_stage = 0; }
			return;
		}
		if( g_reopenTick == 0 ) return;
		unsigned tick = 0;
		input::Point at;
		if( !input::LastRightClick( tick, at ) || tick != g_reopenTick || GetTickCount() - tick > 3000 ){ g_reopenTick = 0; return; }
		if( !input::RightReleased() || GetTickCount() - tick < REOPEN_MS ) return;
		g_reopenTick = 0;
		if( config::Get().debug ) logger::Write( "reabrir: clique direito repetido" );
		input::NudgeMouse();
		s_stage = 1; s_wait = 3;
	}

	// Cartão de HP fixo no canto superior esquerdo, minimapa no superior direito; barra de atalhos e chat
	// embaixo, no centro. Botões de loja/roleta e os 2 slots rápidos do topo saem da tela.
	void PlaceHud( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		int W = (int)vp.Width, H = (int)vp.Height;
		client::WndRect r;
		if( game::BasicInfoRect( r ) && ( r.x != (int)ui::HUD_X || r.y != (int)ui::HUD_Y ) ) game::PlaceHudWindow( game::HudWnd::Basic, (int)ui::HUD_X, (int)ui::HUD_Y );
		for( game::HudWnd h : { game::HudWnd::QuickSlot, game::HudWnd::Cash, game::HudWnd::Roulette } ){
			if( game::HudWindowRect( h, r ) && r.x > -3000 ) game::PlaceHudWindow( h, -4000, -4000 );
		}
		Remap mm = {}, sc = {};
		bool hasMm = false, hasSc = false;
		int rows = 1;
		// minimapa: a janela se posiciona sozinha no canto; o mapa nativo fica em (+1, -1) dela
		if( game::HudWindowRect( game::HudWnd::Minimap, g_mmRect ) ){
			float x = W - MM_MARGIN - MM_SIZE, y = MM_MARGIN;
			hasMm = true;
			mm = { x, y, x + MM_SIZE, y + MM_SIZE, g_mmRect.x + 1.0f, g_mmRect.y - 1.0f, x, y, MM_K };
		}
		int bottom = H - 14; // a EXP fica na janela Personagem (Alt+Q)
		if( game::HudWindowRect( game::HudWnd::Shortcut, r ) ){
			rows = std::max( 1, ( r.h + 10 ) / (int)SC_ROW );
			// quadrados centrados; a janela nativa fica no canto da ampliada (embaixo dela)
			int x = (int)( W * 0.5f - SC_K * ( SC_SLOT_X + ( SC_PITCH * ( SC_COLS - 1 ) + SC_SLOT ) * 0.5f ) );
			int y = (int)( bottom - SC_K * ( SC_ROW * ( rows - 1 ) + SC_SLOT_Y + SC_SLOT ) );
			if( r.x != x || r.y != y ) game::PlaceHudWindow( game::HudWnd::Shortcut, x, y );
			hasSc = game::HudWindowRect( game::HudWnd::Shortcut, g_scRect );
			float sx = x + SC_SLOT_X * SC_K, sy = y + SC_SLOT_Y * SC_K;
			sc = { sx, sy, sx + ( SC_PITCH * ( SC_COLS - 1 ) + SC_SLOT ) * SC_K, sy + ( SC_ROW * ( rows - 1 ) + SC_SLOT ) * SC_K,
				(float)x, (float)y, (float)x, (float)y, SC_K };
			bottom = (int)sy - 6;
		}
		if( game::HudWindowRect( game::HudWnd::Chat, r ) ){
			g_chatTyping = game::ChatTyping();
			int x = ( W - r.w ) / 2, y = bottom - r.h;
			g_chatSpot = { x, y, r.w, r.h };
			if( !g_chatTyping ){ x = -4000; y = -4000; }
			if( r.x != x || r.y != y ) game::PlaceHudWindow( game::HudWnd::Chat, x, y );
			g_hasChat = game::HudWindowRect( game::HudWnd::Chat, g_chatRect );
			UpdateChatFeed();
		}
		// clique no botão "NPC" do minimapa (o clique não chega ao cliente: BuffRedirect)
		static bool s_down = false, s_pressIn = false;
		bool down = input::IsLeftDown();
		if( hasMm ){
			float bx, by, bw, bh;
			ui::MinimapIconButton( mm.x0, mm.y0, MM_SIZE, bx, by, bw, bh );
			input::Point p = input::RealCursor();
			bool in = p.x >= bx && p.x < bx + bw && p.y >= by && p.y < by + bh;
			if( down && !s_down ) s_pressIn = in;
			if( !down && s_down && s_pressIn && in ) config::SetMinimapIcons( !config::MinimapIcons() );
		}
		s_down = down;
		std::lock_guard<std::mutex> g( g_buffLock );
		g_hasMm = hasMm; g_mmRemap = mm;
		g_hasSc = hasSc; g_scRemap = sc; g_scRows = rows;
	}

	void GameFrameEnd( IDirect3DDevice9* dev ){
		g_menuDrawn = g_questDrawn = g_petDrawn = g_wndRectFresh = false;
		g_questPh.st = g_petPh.st = g_menuPh.st = g_basicPh.st = g_invPh.st = g_itemPh.st = g_cmpPh.st = g_eqPh.st = g_otPh.st = g_skPh.st = g_stPh.st = 0;
		g_cmpDrawn = false;
		// ícones do chat do Zero e botões removidos (ITEM da barra de atalhos, os dois do Alt+Q): a linha tinha
		// saído por engano junto com o nome do mapa (ff157e9)
		if( charwnd::Active() && GameHudOn( config::FEAT_HUD ) ) ui::CharacterInput();
		if( charwnd::SkillsActive() && GameHudOn( config::FEAT_HUD ) ){ game::SubstituteSkillWindow(); ui::SkillInput(); }
		if( charwnd::InventoryActive() && GameHudOn( config::FEAT_HUD ) ){ game::SubstituteInventoryWindow(); ui::InventoryInput(); }
		if( charwnd::StorageActive() && GameHudOn( config::FEAT_HUD ) ){ game::SubstituteStorageWindow(); ui::StorageInput(); }
		if( charwnd::CartActive() && GameHudOn( config::FEAT_HUD ) ) game::SubstituteCartWindow();
		if( charwnd::SearchActive() && GameHudOn( config::FEAT_HUD ) ) game::SubstituteSearchWindow();
		if( GameHudOn( config::FEAT_HUD ) ) game::SubstituteItemWindows();
		if( GameHudOn( config::FEAT_HUD ) ){ game::FixChatIcons(); game::RemoveButtons(); game::HideNativeAmmo(); game::CloseShortcutItemSlot(); }
		if( GameHudOn( config::FEAT_PETWND ) ) game::SubstitutePetWindow( (int)POKEDEX_W, (int)POKEDEX_H, 23, 16, 48, 48 );
		g_hasPet = GameHudOn( config::FEAT_PETWND ) && game::PetWindowRect( g_petRect );
		if( GameHudOn( config::FEAT_QUESTWND ) ) game::SubstituteQuestWindow( (int)QUEST_W, (int)QUEST_H, (int)QUEST_W - 24, 4 );
		g_hasQuest = GameHudOn( config::FEAT_QUESTWND ) && game::QuestWindowRect( g_questRect );
		g_hasMenu = GameHudOn( config::FEAT_MENU ) && game::ReadMenu( g_menu );
		if( g_hasMenu ){
			// a janela nativa fica no canto superior esquerdo do menu do Zero (escondida embaixo dele)
			D3DVIEWPORT9 vp;
			dev->GetViewport( &vp );
			float x, y;
			ui::ZeroMenuOrigin( g_menu.open, (float)vp.Width, (float)vp.Height, x, y );
			if( g_menu.rect.x != (int)x || g_menu.rect.y != (int)y ){
				game::PlaceMenu( (int)x, (int)y );
				g_hasMenu = game::ReadMenu( g_menu );
			}
			// substituição: a nativa do tamanho do menu do Zero, com cada ícone embaixo do equivalente
			int mw = 0, mh = 0;
			std::vector<game::Placement> place;
			ui::ZeroMenuLayout( g_menu, mw, mh, place );
			game::SubstituteMenu( mw, mh, place );
			if( g_hasMenu ) g_hasMenu = game::ReadMenu( g_menu );
		}
		g_otherDrawn = false;
		g_basicDrawn = g_itemDrawn = g_equipDrawn = g_invDrawn = g_skillDrawn = g_gageDrawn = g_statDrawn = false;
		{
			std::lock_guard<std::mutex> g( g_buffLock );
			g_buffRectsPrev.swap( g_buffRects );
			g_buffRects.clear();
			std::sort( g_buffTops.begin(), g_buffTops.end() );
			g_buffTopsPrev.swap( g_buffTops );
			g_buffTops.clear();
			g_tipPrev = g_tip;
			g_tipSlotPrev = g_tipSlot;
			g_tip = TipBounds();
			g_tipSlot = -1;
			g_buffFramed.clear();
		}
		g_hasChat = g_hasMm = g_hasSc = g_mmFramed = g_scDrawn = false;
		if( GameHudOn( config::FEAT_HUD ) ){
			game::SubstituteBasicInfo( (int)ui::HUD_W, (int)ui::HUD_H );
			PlaceHud( dev );
		}
		g_hasBasic = GameHudOn( config::FEAT_HUD ) && game::BasicInfoRect( g_basicRect );
		// barras de HP/SP: pintadas na superfície das próprias barras nativas (client.cpp); aqui só o aviso de
		// que o status mudou (a do jogador). O desenho por cima do bloco ficou desligado
		if( config::Get().playerBars && client::InGame() ) client::RefreshPlayerGage();
		g_hasGage = false;
		g_partyCount = 0;
		for( bool& d : g_partyDrawn ) d = false;
		client::WndRect r;
		TrackDrag();
		bool wasBoth = g_itemWnd.open && g_compareWnd.open;
		uint32_t wasId = g_itemWnd.shownId;
		client::WndRect wasItem = g_itemWnd.rect, wasCmp = g_compareWnd.rect;
		bool itemOn = GameHudOn( config::FEAT_ITEMWND ) && game::ItemWindowRect( r );
		FrameItemWnd( g_itemWnd, itemOn, r );
		bool cmpOn = itemOn && game::CompareWindowRect( r );
		FrameItemWnd( g_compareWnd, cmpOn, r );
		if( config::Get().debug && wasBoth != ( itemOn && cmpOn ) ) logger::Write( "comparação: antes %d agora item %d cmp %d", wasBoth, itemOn, cmpOn );
		if( wasBoth && !itemOn ) ReopenAfterCompare( wasId, wasItem, wasCmp );
		ReopenPending();
		// janela Personagem (só pelo Alt+Q): a de atributos (Alt+A) não abre; fica inteira na tela
		bool charWnd = GameHudOn( config::FEAT_HUD );
		if( charWnd && game::StatusWindowShown() ) game::HideStatusWindow();
		// as duas janelas de equipamentos (a própria e a de outro jogador) viram a janela Personagem: tamanho, filhos e
		// encaixes nas nossas posições; ficam inteiras na tela
		for( int which = 1; which >= 0 && charWnd; which-- ){
			game::SelectEquipWnd( which );
			if( game::EquipWindowRect( r ) && game::InstallEquipLayout( &ui::CharacterSlotAt, CHAR_FEET_Y, &ui::CharacterTipPos ) ){
				client::WndRect cl, ck, ca;
				game::NativeEquip neq;
				int tab = which == 0 && game::ReadNativeEquip( neq ) ? neq.tab : 0;
				ui::CharacterChildRects( cl, ck, ca, tab );
				int ch = tab >= 2 ? ui::CHAR_TAB_H : (int)( which ? ui::CHAR_OTHER_H : ui::CHAR_H );
				int cw = tab >= 2 ? ui::CHAR_TAB_W : (int)ui::CHAR_W;
				game::SubstituteEquipWindow( cw, ch, cl, ck, ca );
				game::EquipWindowRect( r );
				D3DVIEWPORT9 vp;
				dev->GetViewport( &vp );
				int cx = std::max( 0, std::min( r.x, (int)vp.Width - cw ) ), cy = std::max( 0, std::min( r.y, (int)vp.Height - ch ) );
				if( cx != r.x || cy != r.y ) game::MoveEquipWindow( cx, cy );
			}
			if( which == 1 ){
				bool otherOn = game::EquipWindowRect( r );
				if( !otherOn ) ui::CharacterWindowClosed( 1 );
				FrameItemWnd( g_otherWnd, otherOn, r );
			}
		}
		game::SelectEquipWnd( 0 );
		bool equipOn = ( charWnd || GameHudOn( config::FEAT_EQUIPWND ) ) && game::EquipWindowRect( r );
		if( !equipOn ) ui::CharacterWindowClosed( 0 );
		FrameItemWnd( g_equipWnd, equipOn, r );
		bool oldInv = GameHudOn( config::FEAT_INVWND ) && !charwnd::InventoryActive(); // o refeito no cliente dispensa o desenho por cima
		if( oldInv && game::InventoryWindowRect( r ) ) game::ResizeNativeInventory( INV_W, INV_H );
		bool invOn = oldInv && game::InventoryWindowRect( r );
		FrameItemWnd( g_invWnd, invOn, r );
		bool skillOn = GameHudOn( config::FEAT_SKILLWND ) && game::SkillWindowRect( r );
		FrameItemWnd( g_skillWnd, skillOn, r );
		if( !charWnd && GameHudOn( config::FEAT_EQUIPWND ) && game::StatusWindowRect( r ) ){
			client::WndRect up[6];
			ui::StatusUpButtons( up );
			bool vis[6];
			game::SubstituteStatusWindow( (int)ui::STATUS_W, (int)ui::STATUS_H, up, vis );
			ui::SetStatusUpVisible( vis );
		}
		bool statOn = !charWnd && GameHudOn( config::FEAT_EQUIPWND ) && game::StatusWindowRect( r );
		FrameItemWnd( g_statWnd, statOn, r );
		if( statOn ){
			input::Point m = input::RealCursor();
			if( m.x >= r.x && m.y >= r.y && m.x < r.x + r.w && m.y < r.y + r.h && !game::Covered( game::Replaced::Status, m.x, m.y ) ) game::HideTooltip();
		}
		if( itemOn || g_hasBasic || equipOn || invOn ) itemdb::StartLoading();
	}

	// Retorna true se a chamada original deve ser pulada.
	bool BeforeDraw( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride, UINT startVertex, bool indexed ){
		if( gfx::IsDrawing() ){
			return false;
		}
		lgp::CountDraw();
		// Modo foto (tecla Pause) fora da seleção: por enquanto só registra um frame para análise
		if( !g_wasActive && !g_loginActive ){
			if( GameWindowDraw( dev, info, upData, upStride, startVertex, indexed ) ){
				return true;
			}
			if( g_photoLogFrames > 0 && g_photoLogArmed ){
				if( !indexed && IsPretransformed( dev ) ){
					if( upData != nullptr ) AccumulateBounds( info, upData, upStride, VertexCount( info.type, info.primCount ) );
					else BoundsFromStream( dev, info, startVertex );
				}
				ReadTexture( dev, info );
				ReadCaller( info );
				char note[64];
				sprintf( note, "pilha=%08X", (unsigned)g_drawHash );
				LogDraw( dev, info, note );
			}
			if( input::PhotoMode() ){
				ReadCaller( info );
				const auto& hide = config::Get().photoHide;
				return std::find( hide.begin(), hide.end(), (unsigned)g_drawHash ) != hide.end();
			}
			return false;
		}

		const config::Settings& cfg = config::Get();
		if( !DrawingToScreen( dev ) ){
			return false;
		}
		bool diag = g_diagFramesLeft > 0 && g_diagDelay == 0;

		if( cfg.mode == config::BackgroundMode::Under && !g_firstDrawDone ){
			g_firstDrawDone = true;
			DrawScene( dev );
		}

		if( !indexed && IsPretransformed( dev ) ){
			if( upData != nullptr ){
				AccumulateBounds( info, upData, upStride, VertexCount( info.type, info.primCount ) );
			}else{
				BoundsFromStream( dev, info, startVertex );
			}
		}
		ReadTexture( dev, info );
		ReadCaller( info );
		g_lastHash = g_drawHash;

		const char* note = "";
		Verdict v = Classify( dev, info, upData, upStride, note );

		if( diag ){
			LogDraw( dev, info, note );
		}
		return v == Verdict::Skip;
	}

	void OnFrameEnd( IDirect3DDevice9* dev ){
		const config::Settings& cfg = config::Get();
		if( g_hasGage && !g_gageDrawn && !GageCovered( g_gageRect ) ){
			// janela vazia que o cliente não chegou a desenhar: barra por cima no fim do quadro
			DrawPlayerGage( dev );
		}
		if( g_hasMenu && !g_menuDrawn ){
			DrawZeroMenuUi( dev ); // cliente não desenhou a janela neste quadro: por cima de tudo
		}
		// primeiro bloco da nativa não reconhecido: por cima de tudo (se nada estiver na frente dela)
		if( g_hasQuest && !g_questDrawn && g_questCovers.empty() ){
			if( cfg.debug ) logger::Write( "quests: primeiro bloco nao visto, desenhada no fim do quadro" );
			ui::DrawZeroQuestWindow( dev, (float)g_questRect.x, (float)g_questRect.y, (float)g_questRect.w, (float)g_questRect.h );
		}
		if( g_hasPet && !g_petDrawn && g_petCovers.empty() ){
			if( cfg.debug ) logger::Write( "pokedex: primeiro bloco nao visto, desenhada no fim do quadro" );
			ui::DrawPokedex( dev, (float)g_petRect.x, (float)g_petRect.y, (float)g_petRect.w, (float)g_petRect.h );
		}
		if( !g_onTop.empty() ){
			HRESULT scene = dev->BeginScene();
			DeferredState saved;
			saved.Capture( dev );
			for( DeferredSprite& d : g_onTop ){
				d.state.Apply( dev );
				DrawCopy( dev, d.type, d.prims, d.verts, d.stride, false, 1.0f, nullptr );
				d.state.Release();
			}
			saved.Apply( dev );
			saved.Release();
			g_onTop.clear();
			if( SUCCEEDED( scene ) ) dev->EndScene();
		}
		GameFrameEnd( dev );
		if( !g_sceneShown && ( client::IsSelectActive() || client::IsCreatingSelect() ) && config::CustomActive() && config::Feature( config::FEAT_SELECT )
			&& cfg.mode == config::BackgroundMode::Replace ){
			CoverWithScene( dev );
		}
		g_sceneShown = false;
		if( input::TakeToggleClick() ){
			bool on = !config::CustomActive();
			if( !on ) g_snapshotDirty = true;
			config::SetCustomActive( on );
			bool combat = on && config::Feature( config::FEAT_COMBAT );
			client::SetSelectedAction( combat ? cfg.selectedAction : -1, cfg.actionFrameMs, combat ? cfg.attackAction : -1, cfg.attackEveryMs );
			scene::Reset();
		}
		if( !g_toggleDrawn && client::IsSelectActive() && cfg.uiEnabled && SUCCEEDED( dev->BeginScene() ) ){
			DrawToggle( dev );
			dev->EndScene();
		}
		if( !client::IsSelectActive() ) input::SetToggleRect( 0, 0, 0, 0 );
		g_toggleDrawn = false;
		// botão escolhido na mensagem redesenhada: o botão do cliente avisa a mensagem (SendMsg 6, id), como num clique
		if( uintptr_t btn = ui::TakeMessageCommand() ){
			if( g_msgWnd ) PressMessageButton( reinterpret_cast<uintptr_t>( g_msgWnd ), btn );
		}
		if( g_msgUi && !g_msgDrawn && SUCCEEDED( dev->BeginScene() ) ){
			DrawMessageUi( dev );
			dev->EndScene();
		}
		// Lista de servidores de personagem (depois do login): a escolha é sempre manual, mas o
		// login-server só aguarda 30 s (AUTH_TIMEOUT); a lista mostra o tempo que resta.
		{
			static void* janela = nullptr;
			static DWORD desde = 0;
			void* sw = client::LoginWindow( client::LoginWnd::SelectServer );
			if( sw != janela ){
				janela = sw;
				desde = GetTickCount();
			}
			bool aposLogin = sw && !chardata::CharServers().empty();
			ui::SetServerCountdown( aposLogin ? cfg.serverChoiceMs - (int)( GetTickCount() - desde ) : -1 );
		}
		{
			g_msgWnd = MensagemAberta( g_msgRect );
			bool fase = g_loginActive || ( client::IsSelectActive() && config::Feature( config::FEAT_SELECT ) )
				|| ( client::IsMakeCharActive() && !client::IsSelectActive() && config::Feature( config::FEAT_MAKE ) );
			// a janela de aviso é desenhada no próprio cliente (charwnd::InstallMessageBox): sem desenho por cima;
			// o pedido de e-mail da exclusão ainda é o nosso
			bool nativa = charwnd::MessageBoxActive() && !g_msgPrompt;
			g_msgUi = g_msgWnd != nullptr && fase && config::CustomActive() && cfg.uiEnabled && !nativa;
			g_msgNative = g_msgWnd != nullptr && nativa;
			g_msgDrawn = false;
			if( !g_msgUi ) ui::ClearMessage();
		}
		if( g_makeUiPending && SUCCEEDED( dev->BeginScene() ) ){
			DrawMakeUi( dev );
			dev->EndScene();
		}
		if( ( !g_deferred.empty() || g_platesPending ) && SUCCEEDED( dev->BeginScene() ) ){
			FlushDeferred( dev );
			dev->EndScene();
		}
		bool active = client::IsSelectActive() || client::IsMakeCharActive();
		if( client::InGame() && charwnd::NpcActive() ){ game::RodexFields(); game::OptionButtons(); }
		if( client::InGame() && charwnd::NpcActive() ){
			D3DVIEWPORT9 vp;
			dev->GetViewport( &vp );
			game::SubstituteNpcWindows( (int)vp.Width, (int)vp.Height );
		}
		if( client::IsSelectActive() && !client::IsCreatingSelect() ){
			D3DVIEWPORT9 vp;
			dev->GetViewport( &vp );
			game::SubstituteSelectWindow( (int)vp.Width, (int)vp.Height, !charwnd::SelectActive() );
		}
		if( client::IsMakeCharActive() && !client::IsSelectActive() ){
			D3DVIEWPORT9 vp;
			dev->GetViewport( &vp );
			game::SubstituteMakeWindow( (int)vp.Width, (int)vp.Height, !charwnd::MakeActive() );
		}

		if( cfg.uiEnabled ){
			chardata::Poll();
		}

		if( active && cfg.mode == config::BackgroundMode::Over && SUCCEEDED( dev->BeginScene() ) ){
			DrawScene( dev );
			dev->EndScene();
		}

		if( input::PhotoMode() != g_lastPhoto ){
			g_lastPhoto = input::PhotoMode();
			if( g_lastPhoto ){
				g_photoLogFrames = 1;
				g_photoLogArmed = false;
			}
		}else if( g_photoLogFrames > 0 ){
			if( g_photoLogArmed ){
				g_photoLogFrames--;
				g_photoLogArmed = false;
			}else{
				g_photoLogArmed = true;
				g_diagDrawIndex = 0;
				logger::Write( "=== Frame do jogo (modo foto) ===" );
			}
		}

		// O cursor é o último desenho do frame; quando isso se repete, aprende quem o desenha
		if( ( active || g_loginActive ) && g_cursorHash == 0 && g_lastHash != 0 ){
			g_sameCount = g_lastHash == g_prevLastHash ? g_sameCount + 1 : 0;
			g_prevLastHash = g_lastHash;
			if( g_sameCount >= CURSOR_LEARN_FRAMES ){
				g_cursorHash = g_lastHash;
				logger::Write( "Cursor do cliente identificado (hash %08X)", (unsigned)g_cursorHash );
			}
		}

		if( input::TakeDiagRequest() ){
			g_diagDelay = 1;
			g_diagFramesLeft = 1;
		}
		if( g_diagFramesLeft > 0 ){
			if( g_diagDelay > 0 ){
				g_diagDelay--;
			}else{
				g_diagFramesLeft--;
			}
		}

		// Criação aberta: diagnóstico de um frame (debug) e posição da janela
		if( !client::MakeWindowRect( g_makeRect ) ){
			g_makeRect = client::WndRect();
		}
		{
			static bool lastMake = false;
			bool make = client::IsMakeCharActive();
			if( make && !lastMake ){
				client::WndRect r;
				if( client::MakeWindowRect( r ) ){
					logger::Write( "Criacao: janela (%d,%d) %dx%d", r.x, r.y, r.w, r.h );
				}
				if( cfg.debug ){
					g_diagDelay = 40;
					g_diagFramesLeft = 1;
				}
			}
			lastMake = make;

		}

		// Antes da seleção (login, servidores): mapa 3D já vai para a placa, sem travar a entrada
		if( !active && SUCCEEDED( dev->TestCooperativeLevel() ) ){
			scene::Warmup( dev );
			ui::Warmup( dev );
		}

		// Prepara o próximo frame
		if( active ){
			int slot = client::SelectedSlot();
			if( !g_wasActive ){
				g_snapshotDirty = config::CustomActive();
				logger::Write( "Seleção ativa, slot %d", slot );
				if( cfg.debug ){
					client::LogGeometry();
					g_diagDelay = 60; // espera a tela estabilizar
					g_diagFramesLeft = 2;
				}
			}else if( slot != g_lastSlot && cfg.debug && g_diagFramesLeft == 0 ){
				g_diagDelay = 5;
				g_diagFramesLeft = 1;
			}
			g_lastSlot = slot;
			scene::SetSlot( slot );
		}else if( g_wasActive ){
			scene::Reset();
			g_lastSlot = -1;
			g_diagFramesLeft = 0;
		}
		g_wasActive = active;
		g_loginActive = LoginPhase();
		game::ClearBitmapCache(); // ponteiros de bitmap do cliente valem só dentro do quadro

		g_preview.on = false;
		input::SetEscapeCapture( client::IsSelectActive() && config::CustomActive() && config::Feature( config::FEAT_SELECT ) && !g_popupSeen && !g_msgUi ); // com mensagem nativa o Esc é dela (cancelar)
		g_popupActive = g_popupSeen || g_msgUi;
		g_popupSeen = false;
		g_phase = Phase::Background;
		g_lastHash = 0;
		g_frame++;
		g_firstDrawDone = false;
		g_diagDrawIndex = 0;
		if( g_diagFramesLeft > 0 && g_diagDelay == 0 ){
			logger::Write( "=== Diagnostico frame %u (slot %d, popup=%d) ===", g_frame, g_lastSlot, g_popupActive );
		}
	}

	HRESULT STDMETHODCALLTYPE Hook_Present( IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty ){
		OnFrameEnd( dev );
		return o_Present( dev, src, dst, wnd, dirty );
	}

	HRESULT STDMETHODCALLTYPE Hook_PresentEx( IDirect3DDevice9Ex* dev, const RECT* src, const RECT* dst, HWND wnd, const RGNDATA* dirty, DWORD flags ){
		OnFrameEnd( dev );
		return o_PresentEx( dev, src, dst, wnd, dirty, flags );
	}

	void OnDeviceLost(){
		lgp::OnDeviceLost();
		for( DeferredSprite& d : g_deferred ) d.state.Release();
		g_deferred.clear();
		g_platesPending = false;
		ReleaseBgTex();
		ClearMakeTiles();
		g_makeUiPending = false;
		ReleaseTiles( g_itemWnd.tiles ); ReleaseTiles( g_itemWnd.prev );
		ReleaseTiles( g_compareWnd.tiles ); ReleaseTiles( g_compareWnd.prev );
		ReleaseTiles( g_equipWnd.tiles ); ReleaseTiles( g_equipWnd.prev );
		ReleaseTiles( g_otherWnd.tiles ); ReleaseTiles( g_otherWnd.prev );
		if( g_otherChar ){ g_otherChar->Release(); g_otherChar = nullptr; }
		ReleaseTiles( g_invWnd.tiles ); ReleaseTiles( g_invWnd.prev );
		ReleaseTiles( g_statWnd.tiles ); ReleaseTiles( g_statWnd.prev );
		if( g_equipChar ){ g_equipChar->Release(); g_equipChar = nullptr; }
		ReleaseItemImages();
		scene::OnDeviceLost();
		ui::OnDeviceLost();
	}

	HRESULT STDMETHODCALLTYPE Hook_Reset( IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp ){
		OnDeviceLost();
		return o_Reset( dev, pp );
	}

	HRESULT STDMETHODCALLTYPE Hook_ResetEx( IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode ){
		OnDeviceLost();
		return o_ResetEx( dev, pp, mode );
	}

	HRESULT STDMETHODCALLTYPE Hook_DrawPrimitive( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT start, UINT count ){
		DrawInfo info{ "DP", type, count };
		if( BeforeDraw( dev, info, nullptr, 0, start, false ) ){
			return D3D_OK;
		}
		return o_DrawPrimitive( dev, type, start, count );
	}

	HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitive( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT baseVertex, UINT minIndex, UINT numVertices, UINT startIndex, UINT count ){
		DrawInfo info{ "DIP", type, count };
		if( BeforeDraw( dev, info, nullptr, 0, 0, true ) ){
			return D3D_OK;
		}
		return o_DrawIndexedPrimitive( dev, type, baseVertex, minIndex, numVertices, startIndex, count );
	}

	HRESULT STDMETHODCALLTYPE Hook_DrawPrimitiveUP( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT count, const void* data, UINT stride ){
		DrawInfo info{ "DPUP", type, count };
		if( BeforeDraw( dev, info, data, stride, 0, false ) ){
			return D3D_OK;
		}
		if( g_skinFills && !gfx::IsDrawing() && SkinFill( dev, type, count, data, stride ) ){
			return D3D_OK;
		}
		return o_DrawPrimitiveUP( dev, type, count, data, stride );
	}

	HRESULT STDMETHODCALLTYPE Hook_DrawIndexedPrimitiveUP( IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices, UINT count, const void* indices, D3DFORMAT indexFormat, const void* data, UINT stride ){
		DrawInfo info{ "DIPUP", type, count };
		if( BeforeDraw( dev, info, nullptr, 0, 0, true ) ){
			return D3D_OK;
		}
		return o_DrawIndexedPrimitiveUP( dev, type, minIndex, numVertices, count, indices, indexFormat, data, stride );
	}

	void HookDevice( IDirect3DDevice9* dev, HWND focusWindow ){
		g_device = dev;
		HookVtable( dev, IDX_DEV_RESET, &Hook_Reset, o_Reset );
		HookVtable( dev, IDX_DEV_PRESENT, &Hook_Present, o_Present );
		HookVtable( dev, IDX_DEV_DRAWPRIMITIVE, &Hook_DrawPrimitive, o_DrawPrimitive );
		HookVtable( dev, IDX_DEV_DRAWINDEXEDPRIMITIVE, &Hook_DrawIndexedPrimitive, o_DrawIndexedPrimitive );
		HookVtable( dev, IDX_DEV_DRAWPRIMITIVEUP, &Hook_DrawPrimitiveUP, o_DrawPrimitiveUP );
		HookVtable( dev, IDX_DEV_DRAWINDEXEDPRIMITIVEUP, &Hook_DrawIndexedPrimitiveUP, o_DrawIndexedPrimitiveUP );

		IDirect3DDevice9Ex* ex = nullptr;
		bool isEx = SUCCEEDED( dev->QueryInterface( IID_Device9Ex, reinterpret_cast<void**>( &ex ) ) );
		if( isEx ){
			HookVtable( ex, IDX_DEVEX_PRESENTEX, &Hook_PresentEx, o_PresentEx );
			HookVtable( ex, IDX_DEVEX_RESETEX, &Hook_ResetEx, o_ResetEx );
			ex->Release();
		}
		logger::Write( "Device D3D9 criado e interceptado (Ex=%d)", isEx );
		if( config::Feature( config::FEAT_LGP ) ) lgp::OnDevice( dev );
		scene::Preload();

		if( config::Get().uiEnabled ){
			D3DDEVICE_CREATION_PARAMETERS cp;
			dev->GetCreationParameters( &cp );
			input::Install( cp.hFocusWindow != nullptr ? cp.hFocusWindow : focusWindow );
			ui::Init();
		}
	}

	HRESULT STDMETHODCALLTYPE Hook_CreateDevice( IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out ){
		HRESULT hr = o_CreateDevice( d3d, adapter, type, wnd, flags, pp, out );
		if( SUCCEEDED( hr ) && out != nullptr && *out != nullptr ){
			HookDevice( *out, wnd );
		}
		return hr;
	}

	HRESULT STDMETHODCALLTYPE Hook_CreateDeviceEx( IDirect3D9Ex* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* mode, IDirect3DDevice9Ex** out ){
		HRESULT hr = o_CreateDeviceEx( d3d, adapter, type, wnd, flags, pp, mode, out );
		if( SUCCEEDED( hr ) && out != nullptr && *out != nullptr ){
			HookDevice( *out, wnd );
		}
		return hr;
	}
}

void d3dhooks::DebugDiagNow( int frames ){
	g_diagDelay = 0;
	g_diagFramesLeft = frames;
	logger::Write( "=== Diagnostico imediato ===" );
}

IDirect3DDevice9* d3dhooks::Device(){
	return g_device;
}

void d3dhooks::SetSelfImage( void* base ){
	if( base == nullptr ) return;
	auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>( base );
	auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>( reinterpret_cast<const BYTE*>( base ) + dos->e_lfanew );
	g_selfBase = reinterpret_cast<uintptr_t>( base );
	g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
}

void d3dhooks::OnDirect3DCreated( IDirect3D9* d3d, bool isEx ){
	if( d3d == nullptr || !config::Get().enabled ){
		return;
	}

	static bool once = false;
	if( !once ){
		once = true;
		if( g_selfBase == 0 ){
			HMODULE self = nullptr;
			GetModuleHandleExW( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>( &d3dhooks::OnDirect3DCreated ), &self );
			d3dhooks::SetSelfImage( self );
		}

		if( config::Get().uiEnabled ){
			chardata::LoadTables();
			chardata::Install();
		}
		if( config::Get().debug ){
			filelog::Install();
		}
	}

	HookVtable( d3d, IDX_D3D_CREATEDEVICE, &Hook_CreateDevice, o_CreateDevice );
	if( isEx ){
		HookVtable( d3d, IDX_D3DEX_CREATEDEVICEEX, &Hook_CreateDeviceEx, o_CreateDeviceEx );
	}
}

bool d3dhooks::BuffRedirect( float x, float y, float& tx, float& ty ){
	std::lock_guard<std::mutex> g( g_buffLock );
	auto back = [&]( const Remap& r ){ tx = r.ax + ( x - r.ox ) / r.k; ty = r.ay + ( y - r.oy ) / r.k; return true; };
	if( const Remap* r = BuffHit( x, y ) ) return back( *r );
	// minimapa embaixo de outra janela (ex.: o armazém arrastado para o canto): o clique é dela
	if( g_hasMm && RemapHit( g_mmRemap, x, y ) && game::Covered( game::Replaced::Minimap, x, y ) ) return false;
	if( g_hasMm ){
		float bx, by, bw, bh;
		ui::MinimapIconButton( g_mmRemap.x0, g_mmRemap.y0, MM_SIZE, bx, by, bw, bh );
		if( x >= bx && x < bx + bw && y >= by && y < by + bh ){ tx = x; ty = y; input::SwallowClick(); return true; }
	}
	if( g_hasMm && RemapHit( g_mmRemap, x, y ) ){
		if( config::MinimapIcons() ) return back( g_mmRemap );
		// ícones desligados: ponto neutro da janela (linha das coordenadas), sem dica nem mãozinha
		tx = g_mmRect.x + 40.0f; ty = g_mmRect.y + g_mmRect.h - 5.0f;
		input::SwallowClick();
		return true;
	}
	if( g_hasSc && RemapHit( g_scRemap, x, y ) ) return back( g_scRemap );
	// lugar antigo dos buffs (coluna da direita, agora vazio): o alvo sai da coluna para o cliente não achar o ícone
	for( const Remap& r : g_buffRectsPrev ){
		float nx0 = r.ax, ny0 = r.ay;
		if( x >= nx0 - 2 && x < nx0 + 34 && y >= ny0 && y < ny0 + 32 ){ tx = nx0 - 12.0f; ty = y; return true; }
	}
	return false;
}
