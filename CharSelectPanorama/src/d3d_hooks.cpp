#include "d3d_hooks.hpp"

#include <windows.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <map>
#include <vector>

#include "chardata.hpp"
#include "client.hpp"
#include "config.hpp"
#include "gfx.hpp"
#include "game.hpp"
#include "itemdb.hpp"
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
			if( client::LoginWindowRect( (client::LoginWnd)k, r ) && r.x > 0 && r.y > 0 ){
				float m = 8.0f;
				if( info.minX >= r.x - m && info.maxX <= r.x + r.w + m && info.minY >= r.y - m && info.maxY <= r.y + r.h + m ){
					return true;
				}
			}
		}
		return false;
	}

	void DrawLoginUi( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		input::SetViewport( (float)vp.Width, (float)vp.Height );
		ui::LoginFrame f;
		f.vpWidth = (float)vp.Width;
		f.vpHeight = (float)vp.Height;
		f.popupActive = g_popupActive;
		client::WndRect r;
		if( client::LoginWindowRect( client::LoginWnd::Login, r ) && r.x > 0 ){
			f.hasLogin = true; f.loginX = (float)r.x; f.loginY = (float)r.y;
		}
		if( client::LoginWindowRect( client::LoginWnd::SelectServer, r ) && r.x > 0 ){
			f.hasServers = true; f.serversX = (float)r.x; f.serversY = (float)r.y;
		}
		f.hasWait = client::LoginWindow( client::LoginWnd::Wait ) != nullptr;
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
		g_vertexCopy.assign( static_cast<const BYTE*>( data ), static_cast<const BYTE*>( data ) + count * stride );
		for( UINT i = 0; i < count; i++ ){
			BYTE* vtx = g_vertexCopy.data() + i * stride;
			const float* p = reinterpret_cast<const float*>( vtx );
			float* uv = reinterpret_cast<float*>( vtx + stride - 8 );
			float fx = ( p[0] - info.minX ) / std::max( 1.0f, info.Width() );
			float fy = ( p[1] - info.minY ) / std::max( 1.0f, info.Height() );
			uv[0] = g_bgU0 + fx * ( g_bgU1 - g_bgU0 );
			uv[1] = g_bgV0 + fy * ( g_bgV1 - g_bgV0 );
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

	void* MensagemAberta( client::WndRect& r ){
		for( client::LoginWnd k : { client::LoginWnd::MessageBox, client::LoginWnd::MessageBoxAuto } ){
			void* w = client::LoginWindow( k );
			if( w && client::LoginWindowRect( k, r ) && r.w > 0 ) return w;
		}
		return nullptr;
	}

	unsigned FilhosDaJanela( void* w ){
		__try{ return *reinterpret_cast<unsigned*>( reinterpret_cast<uintptr_t>( w ) + 0x54 ); }
		__except( EXCEPTION_EXECUTE_HANDLER ){ return 0; }
	}

	void DrawMessageUi( IDirect3DDevice9* dev ){
		if( !g_msgUi || g_msgDrawn ) return;
		g_msgDrawn = true;
		std::string t;
		client::MessageText( g_msgWnd, t );
		int n = MultiByteToWideChar( 1252, 0, t.data(), (int)t.size(), nullptr, 0 );
		std::wstring wt( n, L'\0' );
		MultiByteToWideChar( 1252, 0, t.data(), (int)t.size(), &wt[0], n );
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::MessageFrame f;
		f.vpWidth = (float)vp.Width;
		f.vpHeight = (float)vp.Height;
		f.x = (float)g_msgRect.x; f.y = (float)g_msgRect.y; f.w = (float)g_msgRect.w; f.h = (float)g_msgRect.h;
		f.text = wt;
		f.twoButtons = FilhosDaJanela( g_msgWnd ) >= 3; // 2 = só OK, 3 = OK e cancelar
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


		// mensagem do cliente redesenhada: a nativa some
		if( g_msgUi && info.hasBounds && g_drawHash != g_cursorHash
			&& info.minX >= g_msgRect.x - 4 && info.maxX <= g_msgRect.x + g_msgRect.w + 4
			&& info.minY >= g_msgRect.y - 4 && info.maxY <= g_msgRect.y + g_msgRect.h + 4 ){
			note = "<== mensagem do cliente (redesenhada)";
			return Verdict::Skip;
		}

		// terminados os personagens: redesenha-os em ordem de profundidade e põe as placas por cima
		if( ( !g_deferred.empty() || g_platesPending ) && ( g_phase == Phase::Sprites || g_phase == Phase::After ) ){
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
	ItemWnd g_itemWnd, g_compareWnd, g_equipWnd, g_invWnd, g_skillWnd;
	bool g_equipDrawn = false, g_invDrawn = false, g_skillDrawn = false;
	int g_equipCharDraws = 0, g_equipCharLog = 0;
	ULONG g_windowHash = 0;        // pilha de chamadas que desenha as janelas (aprendida na barra de status)
	bool g_hasBasic = false, g_basicDrawn = false, g_itemDrawn = false;
	bool g_skinFills = false; // dentro do jogo com a skin escura
	client::WndRect g_basicRect;
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

	// As ilustrações das coleções têm fundo branco no próprio arquivo: o branco ligado à borda
	// (preenchimento a partir das bordas) fica transparente; o branco dentro do item continua.
	IDirect3DTexture9* RemoveWhiteBackground( IDirect3DDevice9* dev, IDirect3DSurface9* rt, UINT tw, UINT th ){
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
			return r > 228 && g > 228 && b > 228 && ( std::max( r, std::max( g, b ) ) - std::min( r, std::min( g, b ) ) ) < 24;
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

	IDirect3DTexture9* CaptureRegion( IDirect3DDevice9* dev, const ItemWnd& w, float sx, float sy, float sw, float sh, UINT tw, UINT th );

	// Copia a ilustração da janela nativa (blocos do frame anterior) para uma textura própria
	IDirect3DTexture9* CaptureIllustration( IDirect3DDevice9* dev, const ItemWnd& w ){
		return CaptureRegion( dev, w, ILLU_X, ILLU_Y, ILLU_W, ILLU_H, ILLU_TEX_W, ILLU_TEX_H );
	}

	IDirect3DTexture9* CaptureRegion( IDirect3DDevice9* dev, const ItemWnd& w, float sx, float sy, float sw, float sh, UINT tw, UINT th ){
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
		IDirect3DTexture9* clean = RemoveWhiteBackground( dev, surf, tw, th );
		if( surf ) surf->Release();
		if( clean ){ tex->Release(); return clean; }
		return tex;
	}

	bool DrawIllustration( IDirect3DDevice9* dev, uint32_t id, float x, float y, float w, float h ){
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

	// Dentro de uma janela substituída, depois do painel: cursor, dica e item arrastado seguem o mouse
	// real (o cliente os põe no ponto redirecionado); o resto (ícones nativos etc.) não aparece.
	bool FollowCursor( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride ){
		input::Point off = input::CursorOffset();
		if( upData == nullptr || ( off.x == 0.0f && off.y == 0.0f ) ) return true;
		input::Point real = input::RealCursor();
		float rx = real.x - off.x, ry = real.y - off.y;
		bool cursor = fabsf( info.minX - rx ) <= 6 && fabsf( info.minY - ry ) <= 6 && info.Width() <= 48 && info.Height() <= 48;
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
		ui::SkillFrame f;
		f.nx = (float)g_skillWnd.rect.x; f.ny = (float)g_skillWnd.rect.y; f.nw = (float)g_skillWnd.rect.w; f.nh = (float)g_skillWnd.rect.h;
		f.vpWidth = (float)vp.Width; f.vpHeight = (float)vp.Height;
		ui::DrawSkillWindow( dev, f );
	}

	void DrawInventoryWindowUi( IDirect3DDevice9* dev ){
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::InventoryFrame f;
		f.nx = (float)g_invWnd.rect.x; f.ny = (float)g_invWnd.rect.y; f.nw = (float)g_invWnd.rect.w; f.nh = (float)g_invWnd.rect.h;
		f.vpWidth = (float)vp.Width; f.vpHeight = (float)vp.Height;
		ui::DrawInventoryWindow( dev, f );
	}

	void DrawEquipWindowUi( IDirect3DDevice9* dev ){
		if( !g_equipWnd.prev.empty() && ( g_equipChar == nullptr || GetTickCount() - g_equipCharTick > 400 ) ){
			IDirect3DTexture9* t = nullptr;
			if( gfx::Begin( dev ) ){
				t = CaptureRegion( dev, g_equipWnd, 117, 36, 50, 136, 100, 272 ); // entre as divisórias
				gfx::End();
			}
			if( t ){
				if( g_equipChar ) g_equipChar->Release();
				g_equipChar = t;
				g_equipCharTick = GetTickCount();
			}
		}
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::EquipFrame f;
		f.nx = (float)g_equipWnd.rect.x; f.ny = (float)g_equipWnd.rect.y; f.nw = (float)g_equipWnd.rect.w; f.nh = (float)g_equipWnd.rect.h;
		f.vpWidth = (float)vp.Width; f.vpHeight = (float)vp.Height;
		f.hasStats = game::ReadEquipStats( f.stats );
		f.showEquip = game::ShowEquipFlag();
		f.character.tex = g_equipChar;
		f.character.width = 100; f.character.height = 272;
		f.blitNative = []( float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh ){
			BlitNative( g_equipWnd, sx, sy, sw, sh, dx, dy, dw, dh );
		};
		ui::DrawEquipWindow( dev, f );
	}

	void DrawItemWindowUi( IDirect3DDevice9* dev ){
		game::Item item;
		if( !g_itemWnd.open || !game::ReadItemWindow( item ) ) return;
		g_itemWnd.shownId = item.id;
		D3DVIEWPORT9 vp;
		dev->GetViewport( &vp );
		ui::ItemPanels p;
		p.x = (float)g_itemWnd.rect.x; p.y = (float)g_itemWnd.rect.y; p.w = (float)g_itemWnd.rect.w; p.h = (float)g_itemWnd.rect.h;
		p.vpWidth = (float)vp.Width; p.vpHeight = (float)vp.Height;
		p.item = item;
		game::Item eq;
		if( g_compareWnd.open && game::ReadCompareWindow( eq ) ){
			// comparação aberta pelo próprio cliente: o equipado vai onde ela está
			g_compareWnd.shownId = eq.id;
			p.hasEquipped = true;
			p.equipped = eq;
			p.hasEquippedPos = true;
			p.ex = (float)g_compareWnd.rect.x; p.ey = (float)g_compareWnd.rect.y;
			game::Item inv; // opções e grau do inventário (pacotes)
			if( game::EquippedAt( eq.wear ? eq.wear : eq.location, inv ) && inv.id == eq.id ){
				for( int k = 0; k < 5; k++ ) p.equipped.options[k] = inv.options[k];
			}
		}else if( item.wear == 0 && item.location != 0 && game::EquippedAt( item.location, eq ) && eq.index != item.index ){
			p.hasEquipped = true;
			p.equipped = eq;
		}
		p.image = [dev]( uint32_t id, float x, float y, float w, float h ){ return DrawIllustration( dev, id, x, y, w, h ); };
		ui::DrawItemPanels( dev, p );
	}

	void KeepTile( ItemWnd& w, IDirect3DDevice9* dev, const DrawInfo& info, const void* upData, UINT upStride ){
		if( upData == nullptr || upStride < 28 ) return;
		WindowTile t;
		dev->GetTexture( 0, &t.tex );
		t.x0 = info.minX; t.y0 = info.minY; t.x1 = info.maxX; t.y1 = info.maxY;
		UvRange( upData, upStride, VertexCount( info.type, info.primCount ), t.u0, t.v0, t.u1, t.v1 );
		w.tiles.push_back( t );
	}

	// true = o desenho do cliente é parte de uma janela substituída (não desenha)
	bool GameWindowDraw( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride, UINT startVertex, bool indexed ){
		if( ( !g_hasBasic && !g_itemWnd.open && !g_equipWnd.open && !g_invWnd.open && !g_skillWnd.open ) || indexed || !IsPretransformed( dev ) || !DrawingToScreen( dev ) ) return false;
		if( upData != nullptr ) AccumulateBounds( info, upData, upStride, VertexCount( info.type, info.primCount ) );
		else BoundsFromStream( dev, info, startVertex );
		if( !info.hasBounds ) return false;
		client::WndRect basic = g_basicRect;
		basic.h += 10; // botão nativo de expandir, logo abaixo
		bool inBasic = g_hasBasic && Within( info, basic, 1.0f );
		// parte expansível da janela nativa: mesma largura, colada embaixo (quase toda transparente)
		bool attached = g_hasBasic && fabsf( info.minX - g_basicRect.x ) <= 1 && fabsf( info.maxX - ( g_basicRect.x + g_basicRect.w ) ) <= 1
			&& info.minY >= g_basicRect.y + g_basicRect.h - 6 && info.minY <= g_basicRect.y + g_basicRect.h + 2;
		inBasic = inBasic || attached;
		bool inItem = g_itemWnd.open && Within( info, g_itemWnd.rect, 1.0f );
		const client::WndRect& ir = g_itemWnd.rect;
		bool tipInItem = g_itemWnd.open && info.minX >= ir.x - 2 && info.minX <= ir.x + ir.w && info.minY >= ir.y - 2 && info.minY <= ir.y + ir.h;
		bool inCompare = g_itemWnd.open && g_compareWnd.open && Within( info, g_compareWnd.rect, 1.0f );
		input::Point off = input::CursorOffset();
		bool moved = off.x != 0.0f || off.y != 0.0f;
		const client::WndRect& er = g_equipWnd.rect;
		bool inEquip = g_equipWnd.open && Within( info, er, 1.0f );
		bool tipInEquip = g_equipWnd.open && info.minX >= er.x - 2 && info.minX <= er.x + er.w && info.minY >= er.y - 2 && info.minY <= er.y + er.h;
		const client::WndRect& vr = g_invWnd.rect;
		bool inInv = g_invWnd.open && Within( info, vr, 1.0f );
		bool tipInInv = g_invWnd.open && info.minX >= vr.x - 2 && info.minX <= vr.x + vr.w && info.minY >= vr.y - 2 && info.minY <= vr.y + vr.h;
		const client::WndRect& sr = g_skillWnd.rect;
		bool inSkill = g_skillWnd.open && Within( info, sr, 1.0f );
		bool tipInSkill = g_skillWnd.open && info.minX >= sr.x - 2 && info.minX <= sr.x + sr.w && info.minY >= sr.y - 2 && info.minY <= sr.y + sr.h;
		if( !inBasic && !inItem && !inCompare && !inEquip && !inInv && !inSkill && !( moved && ( tipInItem || tipInEquip || tipInInv || tipInSkill ) ) ) return false;
		ReadCaller( info );
		if( ( inSkill || tipInSkill ) && !inItem && !tipInItem && !inEquip && !inInv ){
			bool native = ui::SkillNativeVisible();
			if( g_windowHash != 0 && g_drawHash == g_windowHash && inSkill ){
				if( !g_skillDrawn ){
					g_skillDrawn = true;
					DrawSkillWindowUi( dev );
				}
				return !native; // "Configurar Atalhos": a lista nativa aparece por cima da nossa
			}
			if( !g_skillDrawn ) return false;
			if( native ) return false;
			return FollowCursor( dev, info, upData, upStride );
		}
		if( ( inInv || tipInInv ) && !inItem && !tipInItem && !inEquip ){
			if( g_windowHash != 0 && g_drawHash == g_windowHash && inInv ){
				KeepTile( g_invWnd, dev, info, upData, upStride );
				if( !g_invDrawn ){
					g_invDrawn = true;
					DrawInventoryWindowUi( dev );
				}
				return true;
			}
			if( !g_invDrawn ) return false; // antes da janela: mapa
			return FollowCursor( dev, info, upData, upStride );
		}
		if( ( inEquip || tipInEquip ) && !inItem && !tipInItem ){
			if( g_windowHash != 0 && g_drawHash == g_windowHash && inEquip ){
				KeepTile( g_equipWnd, dev, info, upData, upStride );
				if( !g_equipDrawn ){
					g_equipDrawn = true;
					DrawEquipWindowUi( dev );
				}
				return true;
			}
			// antes dos blocos da janela é o mapa (personagens, NPCs): passa normalmente
			if( !g_equipDrawn ) return false;
			return FollowCursor( dev, info, upData, upStride );
		}
		if( inBasic ){
			if( g_windowHash == 0 && fabsf( info.minX - g_basicRect.x ) <= 1 && fabsf( info.minY - g_basicRect.y ) <= 1 ){
				g_windowHash = g_drawHash; // primeiro bloco da barra: é assim que o cliente desenha janelas
			}
			bool below = attached || info.minY >= g_basicRect.y + g_basicRect.h - 2; // botão de expandir (desenhado à parte)
			if( g_drawHash != g_windowHash && !below ) return false;
			if( !g_basicDrawn ){
				g_basicDrawn = true;
				game::Status st;
				if( game::ReadStatus( st ) ) ui::DrawStatusHud( dev, (float)g_basicRect.x, (float)g_basicRect.y, st );
			}
			return true;
		}
		if( g_windowHash != 0 && g_drawHash != g_windowHash ){
			// cursor do cliente: com o clique redirecionado ele "está" dentro da janela escondida;
			// volta para onde o mouse realmente está
			if( tipInItem && upData != nullptr && moved && info.Width() <= 48 && info.Height() <= 48 ){
				DrawTransformed( dev, info, upData, upStride, 0, 0, off.x, off.y, 1.0f, false );
				return true;
			}
			return false;
		}
		KeepTile( inItem ? g_itemWnd : g_compareWnd, dev, info, upData, upStride );
		if( !g_itemDrawn ){
			g_itemDrawn = true;
			DrawItemWindowUi( dev );
		}
		return true;
	}

	void FrameItemWnd( ItemWnd& w, bool open, const client::WndRect& r ){
		ReleaseTiles( w.prev );
		w.prev.swap( w.tiles );
		w.open = open;
		if( open ) w.rect = r;
		else{ ReleaseTiles( w.prev ); w.shownId = 0; }
	}

	void GameFrameEnd(){
		g_basicDrawn = g_itemDrawn = g_equipDrawn = g_invDrawn = g_skillDrawn = false;
		g_hasBasic = GameHudOn( config::FEAT_HUD ) && game::BasicInfoRect( g_basicRect );
		client::WndRect r;
		bool itemOn = GameHudOn( config::FEAT_ITEMWND ) && game::ItemWindowRect( r );
		FrameItemWnd( g_itemWnd, itemOn, r );
		bool cmpOn = itemOn && game::CompareWindowRect( r );
		FrameItemWnd( g_compareWnd, cmpOn, r );
		bool equipOn = GameHudOn( config::FEAT_EQUIPWND ) && game::EquipWindowRect( r );
		FrameItemWnd( g_equipWnd, equipOn, r );
		bool invOn = GameHudOn( config::FEAT_INVWND ) && game::InventoryWindowRect( r );
		FrameItemWnd( g_invWnd, invOn, r );
		bool skillOn = GameHudOn( config::FEAT_SKILLWND ) && game::SkillWindowRect( r );
		FrameItemWnd( g_skillWnd, skillOn, r );
		if( itemOn || g_hasBasic || equipOn || invOn ) itemdb::StartLoading();
	}

	// Retorna true se a chamada original deve ser pulada.
	bool BeforeDraw( IDirect3DDevice9* dev, DrawInfo& info, const void* upData, UINT upStride, UINT startVertex, bool indexed ){
		if( gfx::IsDrawing() ){
			return false;
		}
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
		GameFrameEnd();
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
			g_msgUi = g_msgWnd != nullptr && fase && config::CustomActive() && cfg.uiEnabled;
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

		g_preview.on = false;
		input::SetEscapeCapture( client::IsSelectActive() && config::CustomActive() && config::Feature( config::FEAT_SELECT ) && !g_popupSeen );
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
		for( DeferredSprite& d : g_deferred ) d.state.Release();
		g_deferred.clear();
		g_platesPending = false;
		ReleaseBgTex();
		ClearMakeTiles();
		g_makeUiPending = false;
		ReleaseTiles( g_itemWnd.tiles ); ReleaseTiles( g_itemWnd.prev );
		ReleaseTiles( g_compareWnd.tiles ); ReleaseTiles( g_compareWnd.prev );
		ReleaseTiles( g_equipWnd.tiles ); ReleaseTiles( g_equipWnd.prev );
		ReleaseTiles( g_invWnd.tiles ); ReleaseTiles( g_invWnd.prev );
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
