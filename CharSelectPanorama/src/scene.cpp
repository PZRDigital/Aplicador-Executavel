#include "scene.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

#include "config.hpp"
#include "gfx.hpp"
#include "log.hpp"
#include "world3d.hpp"

namespace {
	struct SceneImage {
		gfx::Image img;
		bool failed = false;
	};

	struct Camera {
		int image = -1;
		float cx = 0.5f, cy = 0.5f, zoom = 1.0f;
		bool follow = false;
	};

	// Última câmera desenhada (para posicionar personagens e placas no mapa)
	struct LastView {
		bool valid = false;
		float x0 = 0, y0 = 0;     // canto da região visível, em pixels da imagem
		float scale = 1;          // pixels de tela por pixel da imagem
		float imgW = 1, imgH = 1;
		float vpX = 0, vpY = 0;
		float zoom = 1;
	};
	LastView g_view;

	std::vector<SceneImage> g_images;

	int g_slot = -1;
	bool g_hasCamera = false;
	Camera g_to;
	double g_moveStart = 0;
	bool g_fading = false;
	Camera g_fadeFrom;
	double g_fadeStart = 0;

	double NowMs(){
		static LARGE_INTEGER freq = []{ LARGE_INTEGER f; QueryPerformanceFrequency( &f ); return f; }();
		LARGE_INTEGER c;
		QueryPerformanceCounter( &c );
		return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
	}

	float Clamp01( float v ){
		return v < 0.0f ? 0.0f : ( v > 1.0f ? 1.0f : v );
	}

	float EaseInOutCubic( float t ){
		return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf( -2.0f * t + 2.0f, 3.0f ) / 2.0f;
	}

	Camera Lerp( const Camera& a, const Camera& b, float t ){
		Camera c = b;
		c.cx = a.cx + ( b.cx - a.cx ) * t;
		c.cy = a.cy + ( b.cy - a.cy ) * t;
		c.zoom = a.zoom + ( b.zoom - a.zoom ) * t;
		return c;
	}

	bool g_pullBack = false; // transição entre personagens: afasta e aproxima

	// Câmera com mola criticamente amortecida: arranca rápido e freia suave até o alvo
	Camera g_cam;
	float g_velX = 0, g_velY = 0, g_velZ = 0;
	double g_lastStep = 0;

	void SpringAxis( float& x, float& v, float target, float omega, float dt ){
		// integração exata da mola crítica (estável com qualquer dt)
		float d = x - target;
		float e = expf( -omega * dt );
		float tmp = ( v + omega * d ) * dt;
		v = ( v - omega * tmp ) * e;
		x = target + ( d + tmp ) * e;
	}

	void StepCamera( double now ){
		float dt = g_lastStep > 0 ? (float)( ( now - g_lastStep ) / 1000.0 ) : 0.0f;
		g_lastStep = now;
		if( dt <= 0.0f ) return;
		if( dt > 0.1f ) dt = 0.1f;

		float smooth = std::max( 0.02f, config::Get().smoothness );
		float omega = 1.0f / smooth;
		g_cam.image = g_to.image;
		g_cam.follow = g_to.follow;
		SpringAxis( g_cam.cx, g_velX, g_to.cx, omega, dt );
		SpringAxis( g_cam.cy, g_velY, g_to.cy, omega, dt );
		SpringAxis( g_cam.zoom, g_velZ, g_to.zoom, omega, dt );
	}

	Camera CurrentCamera( double now ){
		Camera c = g_cam;
		if( g_pullBack ){
			// opcional: recua no meio do caminho e volta a aproximar no destino
			float t = Clamp01( (float)( ( now - g_moveStart ) / std::max( 1u, config::Get().transitionMs ) ) );
			c.zoom *= 1.0f - config::Get().pullBack * sinf( t * 3.14159265f );
		}
		return c;
	}

	const gfx::Image* GetImage( IDirect3DDevice9* dev, int index ){
		const auto& paths = config::Get().images;
		if( index < 0 || index >= (int)paths.size() ){
			return nullptr;
		}
		if( g_images.size() != paths.size() ){
			g_images.resize( paths.size() );
		}
		SceneImage& s = g_images[index];
		if( s.img.tex == nullptr && !s.failed ){
			s.failed = !gfx::LoadImageFile( dev, paths[index], s.img );
		}
		return s.img.tex != nullptr ? &s.img : nullptr;
	}

	bool DrawCamera( IDirect3DDevice9* dev, const D3DVIEWPORT9& vp, const Camera& cam, float alpha, double now ){
		const gfx::Image* img = GetImage( dev, cam.image );
		if( img == nullptr ){
			return false;
		}

		const config::Settings& cfg = config::Get();
		float imgW = (float)img->width, imgH = (float)img->height;

		// "cover": a imagem sempre preenche a tela; zoom aproxima a partir disso
		float scale = std::max( vp.Width / imgW, vp.Height / imgH ) * std::max( 1.0f, cam.zoom );
		float regionW = vp.Width / scale, regionH = vp.Height / scale;

		float cx = ( cam.cx + cfg.sway * sinf( (float)( now * 0.00021 ) ) ) * imgW;
		float cy = ( cam.cy + cfg.sway * cosf( (float)( now * 0.00017 ) ) ) * imgH;
		if( cam.follow ){
			// o personagem seguido fica deslocado do centro (a lista ocupa a direita da tela)
			cx -= cfg.frameX * regionW;
			cy -= cfg.frameY * regionH;
		}
		cx = std::min( std::max( cx, regionW * 0.5f ), imgW - regionW * 0.5f );
		cy = std::min( std::max( cy, regionH * 0.5f ), imgH - regionH * 0.5f );

		if( alpha >= 0.5f ){
			g_view.valid = true;
			g_view.x0 = cx - regionW * 0.5f;
			g_view.y0 = cy - regionH * 0.5f;
			g_view.scale = scale;
			g_view.imgW = imgW;
			g_view.imgH = imgH;
			g_view.vpX = (float)vp.X;
			g_view.vpY = (float)vp.Y;
			g_view.zoom = std::max( 1.0f, cam.zoom );
		}

		gfx::DrawImage( *img, (float)vp.X, (float)vp.Y, (float)vp.Width, (float)vp.Height, gfx::Rgba( 255, 255, 255, Clamp01( alpha ) ),
			( cx - regionW * 0.5f ) / imgW, ( cy - regionH * 0.5f ) / imgH,
			( cx + regionW * 0.5f ) / imgW, ( cy + regionH * 0.5f ) / imgH );
		return true;
	}
}

namespace {
	// ---------------------------------------------------------------- câmera 3D
	struct Cam3 {
		float x = 0, y = 0, z = 0, yaw = 0, pitch = 30, dist = 200;
	};
	bool g_3dReady = false;     // mapa 3D carregado
	bool g_3dHasCam = false;
	Cam3 g_c3, g_v3, g_goal3;   // posição, velocidade e alvo das molas
	float g_travelFrom[3] = {}; // início da viagem entre personagens
	float g_travelDist = 0;
	float g_travelSide = 0;     // -1/1: lado para onde a câmera se move (giro)
	double g_last3d = 0;
	double g_open3d = 0;
	float g_clear = 1.0f, g_clearVel = 0.0f; // colisão: fração da distância livre (suavizada)

	void SetupSky(){
		static bool done = false;
		if( done ) return;
		done = true;
		const config::Settings& cfg = config::Get();
		world3d::Sky sky;
		sky.top = cfg.skyTop;
		sky.horizon = cfg.skyHorizon;
		sky.fogStart = cfg.fogStart;
		sky.fogEnd = cfg.fogEnd;
		sky.water = cfg.water;
		sky.clouds = cfg.clouds;
		sky.cloudLevel = cfg.cloudLevel;
		sky.cloudColor = cfg.cloudColor;
		sky.stars = cfg.stars;
		sky.particles = cfg.particles;
		sky.glow = cfg.glow;
		sky.supersample = cfg.supersample;
		world3d::SetSky( sky );
	}

	bool Use3d(){
		const config::Settings& cfg = config::Get();
		if( !cfg.map3d ) return false;
		if( !g_3dReady ){
			static bool tried = false;
			if( !tried ){
				tried = true;
				SetupSky();
				g_3dReady = world3d::Load( cfg.map3dDir );
			}
		}
		return g_3dReady;
	}

	bool SlotWorld( int slot, float& x, float& y, float& z ){
		const config::Settings& cfg = config::Get();
		if( slot < 0 || slot >= (int)cfg.charCells.size() || cfg.charCells[slot].x < 0 ) return false;
		return world3d::CellToWorld( cfg.charCells[slot].x, cfg.charCells[slot].y, x, y, z );
	}

	float WrapDeg( float a ){
		while( a > 180.0f ) a -= 360.0f;
		while( a < -180.0f ) a += 360.0f;
		return a;
	}

	Cam3 GoalFor( int slot ){
		const config::Settings& cfg = config::Get();
		Cam3 g;
		float cx, cz;
		world3d::MapCenter( cx, cz );
		float x = cx, y = 0, z = cz;
		SlotWorld( slot, x, y, z );
		g.x = x;
		g.y = y + cfg.camHeight;
		g.z = z;
		// Gira para a câmera ficar do lado do centro olhando para fora: atrás do personagem
		// aparecem a borda da ilha e o céu, e as pedras da borda não tapam a vista
		float yaw = cfg.camYaw;
		float dx = cx - x, dz = cz - z;
		if( slot >= 0 && slot < (int)cfg.charYaw.size() && !std::isnan( cfg.charYaw[slot] ) ){
			yaw = cfg.charYaw[slot]; // calculada por tools\posicoes3d.py (linha de visão livre)
		}else if( cfg.camOrbit > 0.0f && dx * dx + dz * dz > 100.0f ){
			float toCenter = atan2f( dx, dz ) * 180.0f / 3.14159265f; // personagem -> centro
			yaw += WrapDeg( toCenter - cfg.camYaw ) * cfg.camOrbit;
		}
		g.yaw = yaw;
		g.pitch = cfg.camPitch;
		g.dist = cfg.camDist;
		return g;
	}

	void Spring( float& x, float& v, float target, float omega, float dt ){
		float d = x - target;
		float e = expf( -omega * dt );
		float tmp = ( v + omega * d ) * dt;
		v = ( v - omega * tmp ) * e;
		x = target + ( d + tmp ) * e;
	}

	void SetSlot3d( int slot, double now ){
		Cam3 goal = GoalFor( slot );
		if( !g_3dHasCam ){
			// entrada: tomada aberta que desce até o personagem
			g_c3 = goal;
			g_c3.dist = goal.dist * 2.6f;
			g_c3.pitch = goal.pitch + 18.0f;
			g_c3.yaw = goal.yaw - 35.0f;
			g_c3.y = goal.y + 30.0f;
			g_v3 = Cam3();
			g_v3.pitch = 0;
			g_v3.dist = 0;
			g_3dHasCam = true;
			g_open3d = now;
			g_travelDist = 0;
		}else{
			float dx = goal.x - g_c3.x, dz = goal.z - g_c3.z;
			g_travelFrom[0] = g_c3.x;
			g_travelFrom[1] = g_c3.y;
			g_travelFrom[2] = g_c3.z;
			g_travelDist = sqrtf( dx * dx + dz * dz );
			// lado do movimento em relação à câmera atual
			float yaw = g_c3.yaw * 3.14159265f / 180.0f;
			float rightX = cosf( yaw ), rightZ = -sinf( yaw );
			g_travelSide = ( dx * rightX + dz * rightZ ) >= 0 ? 1.0f : -1.0f;
		}
		goal.yaw = g_c3.yaw + WrapDeg( goal.yaw - g_c3.yaw ); // caminho mais curto
		g_goal3 = goal;
	}

	world3d::Camera Step3d( double now ){
		const config::Settings& cfg = config::Get();
		float dt = g_last3d > 0 ? (float)( ( now - g_last3d ) / 1000.0 ) : 0.0f;
		g_last3d = now;
		dt = std::min( std::max( dt, 0.0f ), 0.1f );

		float smooth = std::max( 0.05f, cfg.smoothness );
		float w = 1.0f / smooth;
		Spring( g_c3.x, g_v3.x, g_goal3.x, w, dt );
		Spring( g_c3.y, g_v3.y, g_goal3.y, w, dt );
		Spring( g_c3.z, g_v3.z, g_goal3.z, w, dt );
		// ângulos e distância um pouco mais lentos: a câmera "chega" depois do alvo
		Spring( g_c3.yaw, g_v3.yaw, g_goal3.yaw, w * 0.75f, dt );
		Spring( g_c3.pitch, g_v3.pitch, g_goal3.pitch, w * 0.75f, dt );
		Spring( g_c3.dist, g_v3.dist, g_goal3.dist, w * 0.75f, dt );

		world3d::Camera cam;
		cam.tx = g_c3.x;
		cam.ty = g_c3.y;
		cam.tz = g_c3.z;
		cam.yaw = g_c3.yaw;
		cam.pitch = g_c3.pitch;
		cam.dist = g_c3.dist;
		cam.fov = cfg.camFov;
		cam.shiftX = cfg.frameX;
		cam.shiftY = cfg.frameY;

		// arco da viagem: sobe e afasta no meio do caminho, girando para o lado do movimento
		if( g_travelDist > 1.0f ){
			float dx = g_goal3.x - g_c3.x, dz = g_goal3.z - g_c3.z;
			float remain = sqrtf( dx * dx + dz * dz );
			float p = std::min( 1.0f, std::max( 0.0f, 1.0f - remain / g_travelDist ) );
			float bump = sinf( p * 3.14159265f );
			float lift = std::min( g_travelDist * cfg.camArc, cfg.camDist * 0.9f );
			cam.dist += lift * bump;
			cam.pitch += 10.0f * bump * std::min( 1.0f, g_travelDist / 150.0f ) * ( cfg.camArc > 0 ? 1.0f : 0.0f );
			cam.yaw += cfg.camSwing * bump * g_travelSide * std::min( 1.0f, g_travelDist / 120.0f );
			if( p > 0.995f ) g_travelDist = 0;
		}

		// colisão: se algo entra entre o personagem e a câmera, ela fica na frente do obstáculo
		// na hora; quando o caminho libera, volta à distância normal devagar
		{
			float want = world3d::ClearFraction( cam );
			Spring( g_clear, g_clearVel, want, 3.0f, dt );
			if( want < g_clear ){
				g_clear = want;
				g_clearVel = 0.0f;
			}
			cam.dist *= g_clear;
		}

		// vida parada: órbita lenta e respiração
		float t = (float)( now / 1000.0 );
		float k = cfg.camIdle;
		cam.yaw += k * 3.0f * sinf( t * 0.29f ) + k * 1.0f * sinf( t * 0.71f );
		cam.pitch += k * 1.2f * sinf( t * 0.37f + 1.0f );
		cam.dist *= 1.0f + k * 0.02f * sinf( t * 0.23f + 2.0f );
		cam.ty += k * 1.5f * sinf( t * 0.53f );
		return cam;
	}

	// Tela de criação (sem slot): volta lenta em torno da ilha
	world3d::Camera Overview( double now ){
		const config::Settings& cfg = config::Get();
		world3d::Camera cam;
		world3d::MapCenter( cam.tx, cam.tz );
		cam.ty = 0;
		cam.yaw = cfg.camYaw + (float)( now / 1000.0 ) * 2.0f;
		cam.pitch = cfg.camPitch + 8.0f;
		cam.dist = cfg.camDist * 2.2f;
		cam.fov = cfg.camFov;
		return cam;
	}
}

void scene::SetSlot( int slot ){
	if( slot == g_slot ){
		return;
	}
	g_slot = slot;

	if( Use3d() ){
		SetSlot3d( slot, NowMs() );
		if( config::Get().debug ){
			logger::Write( "Slot %d -> camera 3D alvo (%.0f, %.0f, %.0f) direcao %.0f", slot, g_goal3.x, g_goal3.y, g_goal3.z, g_goal3.yaw );
		}
		return;
	}

	config::View view = config::ViewForSlot( slot );
	if( view.image < 0 ){
		return;
	}

	const config::Settings& cfg = config::Get();
	Camera target{ view.image, view.focusX, view.focusY, view.zoom, view.follow };
	Camera entry = target;
	entry.zoom *= 1.0f + cfg.pushIn;
	double now = NowMs();

	if( !g_hasCamera ){
		g_cam = entry;
		g_velX = g_velY = g_velZ = 0;
		g_to = target;
		g_moveStart = now;
		g_lastStep = now;
		g_fading = false;
		g_hasCamera = true;
		g_pullBack = false;
		return;
	}

	Camera current = CurrentCamera( now );
	if( current.image == target.image ){
		// mesma imagem: a mola leva a câmera até o novo ponto (mantém a velocidade atual)
		g_pullBack = cfg.pullBack > 0.0f;
	}else{
		g_pullBack = false;
		// imagem diferente: esmaece a antiga enquanto a nova entra com leve aproximação
		g_fadeFrom = current;
		g_fading = true;
		g_fadeStart = now;
		g_cam = entry;
		g_velX = g_velY = g_velZ = 0;
	}
	g_to = target;
	g_moveStart = now;

	if( cfg.debug ){
		logger::Write( "Slot %d -> imagem %d foco (%.2f, %.2f) zoom %.2f", slot, target.image, target.cx, target.cy, target.zoom );
	}
}

void scene::Reset(){
	g_slot = -1;
	g_3dHasCam = false;
	g_last3d = 0;
	g_clear = 1.0f;
	g_clearVel = 0.0f;
	g_hasCamera = false;
	g_fading = false;
	g_view.valid = false;
}

bool scene::WorldToScreen( float u, float v, float& x, float& y ){
	if( !g_view.valid ){
		return false;
	}
	x = g_view.vpX + ( u * g_view.imgW - g_view.x0 ) * g_view.scale;
	y = g_view.vpY + ( v * g_view.imgH - g_view.y0 ) * g_view.scale;
	return true;
}

float scene::ZoomRatio(){
	float base = config::Get().defaultView.zoom;
	return g_view.valid && base > 0.0f ? g_view.zoom / base : 1.0f;
}

bool scene::Draw( IDirect3DDevice9* dev ){
	if( Use3d() ){
		double now = NowMs();
		world3d::Camera cam = g_3dHasCam ? Step3d( now ) : Overview( now );
		return world3d::Draw( dev, cam, now );
	}
	if( !g_hasCamera || !gfx::Begin( dev ) ){
		return false;
	}

	D3DVIEWPORT9 vp;
	dev->GetViewport( &vp );

	double now = NowMs();
	StepCamera( now );
	Camera cam = CurrentCamera( now );
	bool drawn = false;

	if( g_fading ){
		float t = Clamp01( (float)( ( now - g_fadeStart ) / std::max( 1u, config::Get().crossfadeMs ) ) );
		if( t >= 1.0f ){
			g_fading = false;
		}else{
			drawn = DrawCamera( dev, vp, g_fadeFrom, 1.0f, now );
			drawn |= DrawCamera( dev, vp, cam, EaseInOutCubic( t ), now );
		}
	}
	if( !g_fading ){
		drawn = DrawCamera( dev, vp, cam, 1.0f, now );
	}

	gfx::End();
	return drawn;
}

void scene::Preload(){
	const config::Settings& cfg = config::Get();
	if( !cfg.map3d || !config::CustomActive() ) return;
	if( !config::Feature( config::FEAT_LOGIN ) && !config::Feature( config::FEAT_SELECT ) ) return;
	SetupSky();
	world3d::Preload( cfg.map3dDir );
}

void scene::Warmup( IDirect3DDevice9* dev ){
	const config::Settings& cfg = config::Get();
	if( !cfg.map3d || !config::CustomActive() ) return;
	if( !config::Feature( config::FEAT_LOGIN ) && !config::Feature( config::FEAT_SELECT ) ) return;
	world3d::Warmup( dev );
}

bool scene::SlotToScreen( int slot, float& x, float& y, float& scale ){
	const config::Settings& cfg = config::Get();
	if( Use3d() ){
		float wx, wy, wz, ppu;
		if( !SlotWorld( slot, wx, wy, wz ) || !world3d::Project( wx, wy, wz, x, y, ppu ) ){
			return false;
		}
		scale = ppu * cfg.unitsPerPixel;
		return true;
	}
	if( slot < 0 || slot >= (int)cfg.charPositions.size() || !WorldToScreen( cfg.charPositions[slot].x, cfg.charPositions[slot].y, x, y ) ){
		return false;
	}
	scale = cfg.charScale * ZoomRatio();
	return true;
}

void scene::OnDeviceLost(){
	world3d::OnDeviceLost();
	for( SceneImage& s : g_images ){
		gfx::Release( s.img );
	}
}
