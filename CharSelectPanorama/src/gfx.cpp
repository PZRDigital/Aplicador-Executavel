#include "gfx.hpp"

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <list>
#include <unordered_map>
#include <vector>

#include "config.hpp"
#include "log.hpp"
#include "vfs.hpp"

namespace {
	struct Vertex {
		float x, y, z, rhw;
		D3DCOLOR color;
		float u, v;
	};
	constexpr DWORD VERTEX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
	constexpr size_t TEXT_CACHE_MAX = 384;

	struct FontSpec {
		const wchar_t* face;
		const wchar_t* fallback;
		int size;
		int weight;
	};

	const FontSpec FONT_SPECS[(int)gfx::Font::COUNT] = {
		{ L"Cinzel", L"Georgia", 24, FW_BOLD },     // Title
		{ L"Cinzel", L"Georgia", 15, FW_BOLD },     // TitleSmall
		{ L"Inter", L"Segoe UI", 14, FW_NORMAL },   // Body
		{ L"Inter", L"Segoe UI", 14, FW_SEMIBOLD }, // BodyBold
		{ L"Inter", L"Segoe UI", 11, FW_MEDIUM },   // Small
		{ L"Inter", L"Segoe UI", 11, FW_BOLD },     // SmallBold
		{ L"Inter", L"Segoe UI", 15, FW_EXTRABOLD },// Button
		{ L"Inter", L"Segoe UI", 17, FW_BOLD },     // Name
	};

	struct TextEntry {
		IDirect3DTexture9* tex = nullptr;
		unsigned width = 0, height = 0; // tamanho útil do texto
		unsigned texWidth = 0, texHeight = 0;
		std::list<std::wstring>::iterator lru;
	};

	IDirect3DDevice9* g_dev = nullptr;
	IDirect3DPixelShader9* g_sharpPs = nullptr;
	bool g_sharpTried = false;

	const char* SHARP_PS = R"(
sampler2D s0 : register(s0);
float4 texSize : register(c0);
float4 prescale : register(c1);
float4 main( float2 uv : TEXCOORD0, float4 col : COLOR0 ) : COLOR0 {
	float2 texel = uv * texSize.xy;
	float2 fl = floor( texel );
	float2 cd = texel - fl - 0.5;
	float2 range = 0.5 - 0.5 / prescale.xy;
	float2 f = ( cd - clamp( cd, -range, range ) ) * prescale.xy + 0.5;
	float2 p = fl + f - 0.5;
	float2 b = floor( p );
	float2 t = p - b;
	float4 c00 = tex2D( s0, ( b + float2( 0.5, 0.5 ) ) * texSize.zw );
	float4 c10 = tex2D( s0, ( b + float2( 1.5, 0.5 ) ) * texSize.zw );
	float4 c01 = tex2D( s0, ( b + float2( 0.5, 1.5 ) ) * texSize.zw );
	float4 c11 = tex2D( s0, ( b + float2( 1.5, 1.5 ) ) * texSize.zw );
	float w00 = ( 1 - t.x ) * ( 1 - t.y ) * c00.a;
	float w10 = t.x * ( 1 - t.y ) * c10.a;
	float w01 = ( 1 - t.x ) * t.y * c01.a;
	float w11 = t.x * t.y * c11.a;
	float a = w00 + w10 + w01 + w11;
	float3 rgb = ( c00.rgb * w00 + c10.rgb * w10 + c01.rgb * w01 + c11.rgb * w11 ) / max( a, 0.0001 );
	return float4( rgb, a ) * col;
}
)";
	IDirect3DStateBlock9* g_stateBlock = nullptr;
	IWICImagingFactory* g_wic = nullptr;
	bool g_drawing = false;
	bool g_textured = false;
	float g_uiScale = 1.0f;

	HDC g_dc = nullptr;
	HFONT g_fonts[(int)gfx::Font::COUNT] = {};
	std::unordered_map<std::wstring, TextEntry> g_textCache;
	std::list<std::wstring> g_textLru;

	int CALLBACK OnFontFound( const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found ){
		*reinterpret_cast<bool*>( found ) = true;
		return 0;
	}

	bool FontAvailable( const wchar_t* face ){
		LOGFONTW lf = {};
		lf.lfCharSet = DEFAULT_CHARSET;
		wcsncpy_s( lf.lfFaceName, face, _TRUNCATE );
		bool found = false;
		EnumFontFamiliesExW( g_dc, &lf, &OnFontFound, reinterpret_cast<LPARAM>( &found ), 0 );
		return found;
	}

	void CreateFonts(){
		for( HFONT& f : g_fonts ){
			if( f != nullptr ){
				DeleteObject( f );
				f = nullptr;
			}
		}
		for( int i = 0; i < (int)gfx::Font::COUNT; i++ ){
			const FontSpec& spec = FONT_SPECS[i];
			const wchar_t* face = FontAvailable( spec.face ) ? spec.face : spec.fallback;
			int px = (int)( spec.size * g_uiScale + 0.5f );
			g_fonts[i] = CreateFontW( -px, 0, 0, 0, spec.weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
				OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, face );
		}
	}

	void ClearTextCache(){
		for( auto& kv : g_textCache ){
			if( kv.second.tex != nullptr ){
				kv.second.tex->Release();
			}
		}
		g_textCache.clear();
		g_textLru.clear();
	}

	// Sobe pixels BGRA para uma textura DEFAULT via staging SYSTEMMEM.
	IDirect3DTexture9* UploadBgra( IDirect3DDevice9* dev, unsigned w, unsigned h, const void* pixels, unsigned pitch ){
		IDirect3DTexture9* staging = nullptr;
		IDirect3DTexture9* tex = nullptr;
		if( FAILED( dev->CreateTexture( w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &staging, nullptr ) ) ){
			return nullptr;
		}
		D3DLOCKED_RECT lr;
		if( SUCCEEDED( staging->LockRect( 0, &lr, nullptr, 0 ) ) ){
			for( unsigned y = 0; y < h; y++ ){
				memcpy( static_cast<BYTE*>( lr.pBits ) + y * lr.Pitch, static_cast<const BYTE*>( pixels ) + y * pitch, w * 4 );
			}
			staging->UnlockRect( 0 );
			if( FAILED( dev->CreateTexture( w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr ) )
				|| FAILED( dev->UpdateTexture( staging, tex ) ) ){
				if( tex ) tex->Release();
				tex = nullptr;
			}
		}
		staging->Release();
		return tex;
	}

	TextEntry* GetText( gfx::Font font, const std::wstring& text ){
		std::wstring key = std::wstring( 1, (wchar_t)( L'A' + (int)font ) ) + text;
		auto it = g_textCache.find( key );
		if( it != g_textCache.end() ){
			g_textLru.splice( g_textLru.begin(), g_textLru, it->second.lru );
			return &it->second;
		}

		HFONT old = (HFONT)SelectObject( g_dc, g_fonts[(int)font] );
		SIZE size = {};
		GetTextExtentPoint32W( g_dc, text.c_str(), (int)text.size(), &size );
		TEXTMETRICW tm;
		GetTextMetricsW( g_dc, &tm );

		unsigned w = (unsigned)std::max<LONG>( 1, size.cx + 4 );
		unsigned h = (unsigned)std::max<LONG>( 1, tm.tmHeight + 2 );

		BITMAPINFO bi = {};
		bi.bmiHeader.biSize = sizeof( bi.bmiHeader );
		bi.bmiHeader.biWidth = (LONG)w;
		bi.bmiHeader.biHeight = -(LONG)h;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		void* bits = nullptr;
		HBITMAP bmp = CreateDIBSection( g_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0 );
		TextEntry entry;
		if( bmp != nullptr ){
			HBITMAP oldBmp = (HBITMAP)SelectObject( g_dc, bmp );
			memset( bits, 0, w * h * 4 );
			SetBkMode( g_dc, TRANSPARENT );
			SetTextColor( g_dc, RGB( 255, 255, 255 ) );
			TextOutW( g_dc, 2, 1, text.c_str(), (int)text.size() );
			GdiFlush();

			// Texto branco; a cobertura vira o alfa
			DWORD* px = static_cast<DWORD*>( bits );
			for( unsigned i = 0; i < w * h; i++ ){
				DWORD c = px[i];
				DWORD a = std::max( { c & 0xFF, ( c >> 8 ) & 0xFF, ( c >> 16 ) & 0xFF } );
				px[i] = ( a << 24 ) | 0x00FFFFFF;
			}
			entry.tex = UploadBgra( g_dev, w, h, bits, w * 4 );
			SelectObject( g_dc, oldBmp );
			DeleteObject( bmp );
		}
		SelectObject( g_dc, old );

		entry.width = (unsigned)size.cx;
		entry.height = (unsigned)tm.tmHeight;
		entry.texWidth = w;
		entry.texHeight = h;

		if( g_textCache.size() >= TEXT_CACHE_MAX ){
			auto victim = g_textCache.find( g_textLru.back() );
			if( victim != g_textCache.end() ){
				if( victim->second.tex ) victim->second.tex->Release();
				g_textCache.erase( victim );
			}
			g_textLru.pop_back();
		}
		g_textLru.push_front( key );
		entry.lru = g_textLru.begin();
		return &( g_textCache[key] = entry );
	}

	void SetTextured( bool textured ){
		if( textured == g_textured ){
			return;
		}
		g_textured = textured;
		if( textured ){
			g_dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
			g_dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE );
		}else{
			g_dev->SetTexture( 0, nullptr );
			g_dev->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2 );
			g_dev->SetTextureStageState( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2 );
		}
	}

	void Quad( float x, float y, float w, float h, D3DCOLOR c0, D3DCOLOR c1, D3DCOLOR c2, D3DCOLOR c3,
		float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1 ){
		x -= 0.5f;
		y -= 0.5f;
		Vertex q[4] = {
			{ x, y, 0, 1, c0, u0, v0 },
			{ x + w, y, 0, 1, c1, u1, v0 },
			{ x, y + h, 0, 1, c2, u0, v1 },
			{ x + w, y + h, 0, 1, c3, u1, v1 },
		};
		g_dev->DrawPrimitiveUP( D3DPT_TRIANGLESTRIP, 2, q, sizeof( Vertex ) );
	}
}

void gfx::Init(){
	g_dc = CreateCompatibleDC( nullptr );

	// fontes da interface (pasta charselect\fonts ou char.grf), só para este processo
	for( const std::wstring& name : vfs::List( config::BaseDir() + L"fonts\\" ) ){
		if( name.size() < 4 || _wcsicmp( name.c_str() + name.size() - 4, L".ttf" ) != 0 ) continue;
		std::vector<unsigned char> data;
		DWORD n = 0;
		if( vfs::Read( config::BaseDir() + L"fonts\\" + name, data ) ){
			AddFontMemResourceEx( data.data(), (DWORD)data.size(), nullptr, &n );
		}
		logger::Write( "Fonte %ls: %s", name.c_str(), n > 0 ? "ok" : "falhou" );
	}
	CreateFonts();
}

bool gfx::IsDrawing(){
	return g_drawing;
}

void gfx::SetDrawing( bool on ){
	g_drawing = on;
}

void gfx::SetUiScale( float scale ){
	if( fabsf( scale - g_uiScale ) < 0.01f ){
		return;
	}
	g_uiScale = scale;
	ClearTextCache();
	CreateFonts();
}

bool gfx::Begin( IDirect3DDevice9* dev ){
	g_dev = dev;
	if( g_stateBlock == nullptr && FAILED( dev->CreateStateBlock( D3DSBT_ALL, &g_stateBlock ) ) ){
		return false;
	}
	g_stateBlock->Capture();
	g_drawing = true;

	dev->SetVertexShader( nullptr );
	dev->SetPixelShader( nullptr );
	dev->SetFVF( VERTEX_FVF );
	dev->SetRenderState( D3DRS_ZENABLE, FALSE );
	dev->SetRenderState( D3DRS_ZWRITEENABLE, FALSE );
	dev->SetRenderState( D3DRS_CULLMODE, D3DCULL_NONE );
	dev->SetRenderState( D3DRS_LIGHTING, FALSE );
	dev->SetRenderState( D3DRS_FOGENABLE, FALSE );
	dev->SetRenderState( D3DRS_ALPHATESTENABLE, FALSE );
	dev->SetRenderState( D3DRS_STENCILENABLE, FALSE );
	dev->SetRenderState( D3DRS_SCISSORTESTENABLE, FALSE );
	dev->SetRenderState( D3DRS_COLORWRITEENABLE, 0xF );
	dev->SetRenderState( D3DRS_SPECULARENABLE, FALSE );
	dev->SetRenderState( D3DRS_ALPHABLENDENABLE, TRUE );
	dev->SetRenderState( D3DRS_BLENDOP, D3DBLENDOP_ADD );
	dev->SetRenderState( D3DRS_SRCBLEND, D3DBLEND_SRCALPHA );
	dev->SetRenderState( D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA );
	dev->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	dev->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	dev->SetTextureStageState( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	dev->SetTextureStageState( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE );
	dev->SetTextureStageState( 0, D3DTSS_TEXCOORDINDEX, 0 );
	dev->SetTextureStageState( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE );
	dev->SetTextureStageState( 1, D3DTSS_COLOROP, D3DTOP_DISABLE );
	dev->SetTextureStageState( 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE );
	dev->SetSamplerState( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR );
	dev->SetSamplerState( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR );
	dev->SetSamplerState( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE );
	dev->SetSamplerState( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP );
	dev->SetSamplerState( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP );

	g_textured = true;
	SetTextured( false );
	return true;
}

void gfx::End(){
	if( g_stateBlock != nullptr ){
		g_stateBlock->Apply();
	}
	g_drawing = false;
}

IDirect3DPixelShader9* gfx::SharpShader( IDirect3DDevice9* dev ){
	if( g_sharpTried ){
		return g_sharpPs;
	}
	g_sharpTried = true;
	HMODULE dll = LoadLibraryW( L"d3dcompiler_47.dll" );
	if( dll == nullptr ){
		logger::Write( "AVISO: d3dcompiler_47.dll ausente; sprites sem suavizacao" );
		return nullptr;
	}
	auto compile = reinterpret_cast<pD3DCompile>( GetProcAddress( dll, "D3DCompile" ) );
	ID3DBlob* code = nullptr;
	ID3DBlob* errors = nullptr;
	if( compile == nullptr || FAILED( compile( SHARP_PS, strlen( SHARP_PS ), "sharp", nullptr, nullptr, "main", "ps_2_0",
		D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors ) ) ){
		logger::Write( "ERRO: shader dos sprites: %s", errors ? (const char*)errors->GetBufferPointer() : "D3DCompile indisponivel" );
		if( errors ) errors->Release();
		return nullptr;
	}
	if( FAILED( dev->CreatePixelShader( static_cast<const DWORD*>( code->GetBufferPointer() ), &g_sharpPs ) ) ){
		g_sharpPs = nullptr;
	}
	code->Release();
	if( errors ) errors->Release();
	logger::Write( g_sharpPs ? "Shader dos sprites pronto (sharp bilinear)" : "ERRO: CreatePixelShader" );
	return g_sharpPs;
}

void gfx::OnDeviceLost(){
	if( g_sharpPs ){
		g_sharpPs->Release();
		g_sharpPs = nullptr;
	}
	g_sharpTried = false;
	ClearTextCache();
	if( g_stateBlock != nullptr ){
		g_stateBlock->Release();
		g_stateBlock = nullptr;
	}
}

bool gfx::DecodeImageFile( const std::wstring& path, std::vector<unsigned char>& bgra, unsigned& width, unsigned& height, unsigned maxSize,
	void* wicFactory ){
	IWICImagingFactory* wic = static_cast<IWICImagingFactory*>( wicFactory );
	if( wic == nullptr ){
		if( g_wic == nullptr ){
			HRESULT hr = CoInitializeEx( nullptr, COINIT_APARTMENTTHREADED );
			if( FAILED( hr ) && hr != RPC_E_CHANGED_MODE ){
				return false;
			}
			if( FAILED( CoCreateInstance( CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS( &g_wic ) ) ) ){
				logger::Write( "ERRO: WIC indisponivel" );
				return false;
			}
		}
		wic = g_wic;
	}

	IWICBitmapDecoder* decoder = nullptr;
	IWICBitmapFrameDecode* frame = nullptr;
	IWICBitmapScaler* scaler = nullptr;
	IWICFormatConverter* converter = nullptr;
	IWICStream* stream = nullptr;
	std::vector<unsigned char> fileData;
	bool ok = false;

	do{
		if( !vfs::Read( path, fileData ) ) break;
		if( FAILED( wic->CreateStream( &stream ) ) ) break;
		if( FAILED( stream->InitializeFromMemory( fileData.data(), (DWORD)fileData.size() ) ) ) break;
		if( FAILED( wic->CreateDecoderFromStream( stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder ) ) ) break;
		if( FAILED( decoder->GetFrame( 0, &frame ) ) ) break;

		UINT w, h;
		frame->GetSize( &w, &h );
		float fit = maxSize > 0 ? std::min( 1.0f, std::min( (float)maxSize / w, (float)maxSize / h ) ) : 1.0f;
		IWICBitmapSource* source = frame;
		if( fit < 1.0f ){
			UINT sw = (UINT)( w * fit ), sh = (UINT)( h * fit );
			logger::Write( "Imagem %ux%u excede o limite da GPU; reduzindo para %ux%u", w, h, sw, sh );
			if( FAILED( wic->CreateBitmapScaler( &scaler ) ) ) break;
			if( FAILED( scaler->Initialize( frame, sw, sh, WICBitmapInterpolationModeFant ) ) ) break;
			source = scaler;
			w = sw;
			h = sh;
		}

		if( FAILED( wic->CreateFormatConverter( &converter ) ) ) break;
		if( FAILED( converter->Initialize( source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom ) ) ) break;

		bgra.resize( (size_t)w * h * 4 );
		if( FAILED( converter->CopyPixels( nullptr, w * 4, (UINT)bgra.size(), bgra.data() ) ) ) break;
		width = w;
		height = h;
		ok = true;
	}while( false );

	if( converter ) converter->Release();
	if( scaler ) scaler->Release();
	if( frame ) frame->Release();
	if( decoder ) decoder->Release();
	if( stream ) stream->Release();
	return ok;
}

bool gfx::LoadImageFile( IDirect3DDevice9* dev, const std::wstring& path, Image& out ){
	D3DCAPS9 caps;
	dev->GetDeviceCaps( &caps );
	std::vector<BYTE> pixels;
	unsigned w = 0, h = 0;
	bool ok = DecodeImageFile( path, pixels, w, h, std::min( caps.MaxTextureWidth, caps.MaxTextureHeight ) );
	if( ok ){
		out.tex = UploadBgra( dev, w, h, pixels.data(), w * 4 );
		out.width = w;
		out.height = h;
		ok = out.tex != nullptr;
	}
	if( !ok ) logger::Write( "ERRO: falha ao carregar %ls", path.c_str() );
	return ok;
}

bool gfx::CreateGlow( IDirect3DDevice9* dev, Image& out ){
	const unsigned w = 256, h = 96;
	std::vector<DWORD> px( w * h );
	for( unsigned y = 0; y < h; y++ ){
		for( unsigned x = 0; x < w; x++ ){
			float nx = ( x + 0.5f ) / w * 2.0f - 1.0f, ny = ( y + 0.5f ) / h * 2.0f - 1.0f;
			float d = sqrtf( nx * nx + ny * ny );
			float ring = expf( -( d - 0.78f ) * ( d - 0.78f ) / 0.006f );
			float fill = d < 1.0f ? 0.35f * ( 1.0f - d ) : 0.0f;
			float a = std::min( 1.0f, ring + fill );
			px[y * w + x] = ( (DWORD)( a * 255.0f ) << 24 ) | 0x00FFFFFF;
		}
	}
	out.tex = UploadBgra( dev, w, h, px.data(), w * 4 );
	out.width = w;
	out.height = h;
	return out.tex != nullptr;
}

bool gfx::CreateGear( IDirect3DDevice9* dev, Image& out ){
	const unsigned n = 128;
	const int teeth = 9, ss = 4;
	std::vector<DWORD> px( n * n );
	for( unsigned y = 0; y < n; y++ ){
		for( unsigned x = 0; x < n; x++ ){
			int inside = 0;
			for( int sy = 0; sy < ss; sy++ ){
				for( int sx = 0; sx < ss; sx++ ){
					float nx = ( x + ( sx + 0.5f ) / ss ) / n * 2.0f - 1.0f;
					float ny = ( y + ( sy + 0.5f ) / ss ) / n * 2.0f - 1.0f;
					float d = sqrtf( nx * nx + ny * ny );
					float a = atan2f( ny, nx );
					// dente: trapézio suave em volta do disco
					float k = 0.5f + 0.5f * cosf( a * teeth );
					float tooth = std::min( 1.0f, std::max( 0.0f, ( k - 0.35f ) / 0.3f ) );
					float r = 0.80f + 0.17f * tooth;
					if( d <= r ) inside++;
				}
			}
			float alpha = (float)inside / ( ss * ss );
			px[y * n + x] = ( (DWORD)( alpha * 255.0f ) << 24 ) | 0x00FFFFFF;
		}
	}
	out.tex = UploadBgra( dev, n, n, px.data(), n * 4 );
	out.width = n;
	out.height = n;
	return out.tex != nullptr;
}

bool gfx::CreateDisc( IDirect3DDevice9* dev, Image& out ){
	const unsigned n = 64;
	std::vector<DWORD> px( n * n );
	for( unsigned y = 0; y < n; y++ ){
		for( unsigned x = 0; x < n; x++ ){
			float nx = ( x + 0.5f ) / n * 2.0f - 1.0f, ny = ( y + 0.5f ) / n * 2.0f - 1.0f;
			float d = sqrtf( nx * nx + ny * ny );
			float a = std::min( 1.0f, std::max( 0.0f, ( 1.0f - d ) * n * 0.5f ) );
			px[y * n + x] = ( (DWORD)( a * 255.0f ) << 24 ) | 0x00FFFFFF;
		}
	}
	out.tex = UploadBgra( dev, n, n, px.data(), n * 4 );
	out.width = n;
	out.height = n;
	return out.tex != nullptr;
}

void gfx::RoundRect( float x, float y, float w, float h, float r, D3DCOLOR color, const Image& disc ){
	r = std::min( r, std::min( w, h ) * 0.5f );
	if( disc.tex == nullptr || r < 1.0f ){
		Rect( x, y, w, h, color );
		return;
	}
	// miolo e faixas sem sobreposição; cantos com quartos do disco
	Rect( x + r, y, w - 2 * r, h, color );
	Rect( x, y + r, r, h - 2 * r, color );
	Rect( x + w - r, y + r, r, h - 2 * r, color );
	DrawImage( disc, x, y, r, r, color, 0.0f, 0.0f, 0.5f, 0.5f );
	DrawImage( disc, x + w - r, y, r, r, color, 0.5f, 0.0f, 1.0f, 0.5f );
	DrawImage( disc, x, y + h - r, r, r, color, 0.0f, 0.5f, 0.5f, 1.0f );
	DrawImage( disc, x + w - r, y + h - r, r, r, color, 0.5f, 0.5f, 1.0f, 1.0f );
}

void gfx::Release( Image& img ){
	if( img.tex != nullptr ){
		img.tex->Release();
	}
	img = Image();
}

void gfx::Rect( float x, float y, float w, float h, D3DCOLOR color ){
	SetTextured( false );
	Quad( x, y, w, h, color, color, color, color );
}

void gfx::RectV( float x, float y, float w, float h, D3DCOLOR top, D3DCOLOR bottom ){
	SetTextured( false );
	Quad( x, y, w, h, top, top, bottom, bottom );
}

void gfx::RectH( float x, float y, float w, float h, D3DCOLOR left, D3DCOLOR right ){
	SetTextured( false );
	Quad( x, y, w, h, left, right, left, right );
}

void gfx::Frame( float x, float y, float w, float h, D3DCOLOR color, float t ){
	Rect( x, y, w, t, color );
	Rect( x, y + h - t, w, t, color );
	Rect( x, y + t, t, h - 2 * t, color );
	Rect( x + w - t, y + t, t, h - 2 * t, color );
}

void gfx::Triangle( float x1, float y1, float x2, float y2, float x3, float y3, D3DCOLOR color ){
	SetTextured( false );
	Vertex t[3] = {
		{ x1 - 0.5f, y1 - 0.5f, 0, 1, color, 0, 0 },
		{ x2 - 0.5f, y2 - 0.5f, 0, 1, color, 0, 0 },
		{ x3 - 0.5f, y3 - 0.5f, 0, 1, color, 0, 0 },
	};
	g_dev->DrawPrimitiveUP( D3DPT_TRIANGLELIST, 1, t, sizeof( Vertex ) );
}

void gfx::Line( float x1, float y1, float x2, float y2, D3DCOLOR color, float thickness ){
	float dx = x2 - x1, dy = y2 - y1;
	float len = sqrtf( dx * dx + dy * dy );
	if( len < 0.001f ){
		return;
	}
	float nx = -dy / len * thickness * 0.5f, ny = dx / len * thickness * 0.5f;
	Triangle( x1 + nx, y1 + ny, x2 + nx, y2 + ny, x2 - nx, y2 - ny, color );
	Triangle( x1 + nx, y1 + ny, x2 - nx, y2 - ny, x1 - nx, y1 - ny, color );
}

void gfx::Circle( float cx, float cy, float radius, D3DCOLOR color ){
	const int segments = 16;
	for( int i = 0; i < segments; i++ ){
		float a0 = i * 6.2831853f / segments, a1 = ( i + 1 ) * 6.2831853f / segments;
		Triangle( cx, cy, cx + cosf( a0 ) * radius, cy + sinf( a0 ) * radius, cx + cosf( a1 ) * radius, cy + sinf( a1 ) * radius, color );
	}
}

void gfx::DrawImage( const Image& img, float x, float y, float w, float h, D3DCOLOR color, float u0, float v0, float u1, float v1 ){
	if( img.tex == nullptr ){
		return;
	}
	SetTextured( true );
	g_dev->SetTexture( 0, img.tex );
	Quad( x, y, w, h, color, color, color, color, u0, v0, u1, v1 );
}

float gfx::TextWidth( Font font, const std::wstring& text ){
	HFONT old = (HFONT)SelectObject( g_dc, g_fonts[(int)font] );
	SIZE size = {};
	GetTextExtentPoint32W( g_dc, text.c_str(), (int)text.size(), &size );
	SelectObject( g_dc, old );
	return (float)size.cx;
}

float gfx::LineHeight( Font font ){
	HFONT old = (HFONT)SelectObject( g_dc, g_fonts[(int)font] );
	TEXTMETRICW tm;
	GetTextMetricsW( g_dc, &tm );
	SelectObject( g_dc, old );
	return (float)tm.tmHeight;
}

float gfx::Text( Font font, const std::wstring& text, float x, float y, D3DCOLOR color, int align, float maxWidth, bool shadow ){
	if( text.empty() || g_dev == nullptr ){
		return 0.0f;
	}

	std::wstring shown = text;
	if( maxWidth > 0.0f && TextWidth( font, shown ) > maxWidth ){
		while( shown.size() > 1 && TextWidth( font, shown + L"…" ) > maxWidth ){
			shown.pop_back();
		}
		shown += L"…";
	}

	TextEntry* e = GetText( font, shown );
	if( e == nullptr || e->tex == nullptr ){
		return 0.0f;
	}

	float w = (float)e->width;
	if( align == Center ) x -= w * 0.5f;
	else if( align == Right ) x -= w;
	x = floorf( x ) - 2.0f;
	y = floorf( y ) - 1.0f;

	SetTextured( true );
	g_dev->SetTexture( 0, e->tex );
	if( shadow ){
		D3DCOLOR sc = D3DCOLOR_ARGB( ( ( color >> 24 ) & 0xFF ) * 3 / 4, 0, 0, 0 );
		Quad( x + 1, y + 1, (float)e->texWidth, (float)e->texHeight, sc, sc, sc, sc );
	}
	Quad( x, y, (float)e->texWidth, (float)e->texHeight, color, color, color, color );
	return w;
}

bool gfx::CreateImage( IDirect3DDevice9* dev, unsigned w, unsigned h, const void* bgra, Image& out ){
	out.tex = UploadBgra( dev, w, h, bgra, w * 4 );
	out.width = w;
	out.height = h;
	return out.tex != nullptr;
}
