"""Prévia do enquadramento da seleção 3D (mesma câmera da DLL) com marcadores nos personagens."""
import math, struct, sys, os
import numpy as np
import moderngl
from PIL import Image, ImageDraw
import preview3d as pv

def gat(d):
    b = open(os.path.join(d, 'gat.bin'), 'rb').read(); w, h = struct.unpack_from('<ii', b)
    return w, h, np.frombuffer(b, np.dtype([('h', '<f4'), ('t', 'u1')]), w * h, 8).reshape(h, w)

def cell_world(g, H, cx, cy):
    return (5 * cx + 2.5, float(g['h'][int(cy), int(cx)]), 10 * H - 5 * cy + 7.5)

def wrap(a):
    while a > 180: a -= 360
    while a < -180: a += 360
    return a

def goal(cfg, g, H, W, slot):
    cx, cz = 5 * W + 10, 5 * H + 10
    x, y, z = cell_world(g, H, *cfg['pos'][slot][:2])
    if len(cfg['pos'][slot]) > 2:
        return (x, y + cfg['height'], z), cfg['pos'][slot][2]
    yaw = cfg['yaw']
    dx, dz = cx - x, cz - z
    if cfg['orbit'] > 0 and dx * dx + dz * dz > 100:
        yaw += wrap(math.degrees(math.atan2(dx, dz)) - cfg['yaw']) * cfg['orbit']
    return (x, y + cfg['height'], z), yaw

def main(d, out, cfg, slot, size=(1600, 900)):
    W = H = 100
    gw, gh, g = gat(d)
    tgt, yaw = goal(cfg, g, H, W, slot)
    # mesma câmera/projeção da DLL
    m = pv.load(d)
    ctx = moderngl.create_standalone_context(backend='egl')
    fbo = ctx.framebuffer(ctx.renderbuffer(size, 4), ctx.depth_renderbuffer(size)); fbo.use()
    ctx.enable(moderngl.DEPTH_TEST)
    top = np.array(cfg['sky_top']) / 255; hor = np.array(cfg['sky_hor']) / 255
    ctx.clear(*hor, 1)
    eye = pv.camera(tgt, yaw, cfg['pitch'], cfg['dist'])
    view = pv.look_at(eye, tgt)
    proj = pv.persp(cfg['fov'], size[0] / size[1], cfg.get('near', 8), 9000)
    proj[0, 2] -= cfg['shiftx'] * 2  # GL: coluna z (w=-z)
    proj[1, 2] -= -cfg['shifty'] * 2
    mvp = proj @ view
    prog = ctx.program(vertex_shader='''#version 330
        in vec3 p; in vec4 c; in vec2 t; in vec2 t2; uniform mat4 mvp; out vec4 vc; out vec2 vt; out vec2 vt2; out float dz;
        void main(){ gl_Position = mvp * vec4(p,1); vc=c; vt=t; vt2=t2; dz=gl_Position.w; }''',
        fragment_shader='''#version 330
        in vec4 vc; in vec2 vt; in vec2 vt2; in float dz; uniform sampler2D tex; uniform sampler2D lm; uniform int kind; uniform vec3 fogc; uniform vec2 fog;
        out vec4 o;
        void main(){ vec4 x = texture(tex, vt); if(kind!=2 && x.a<0.376) discard;
          vec3 rgb = x.rgb*vc.rgb; if(kind==0){ vec4 l=texture(lm,vt2); rgb = rgb*l.a + l.rgb; }
          float f = clamp((dz-fog.x)/(fog.y-fog.x),0.,1.); rgb = mix(rgb, fogc, f);
          o = vec4(rgb, x.a*vc.a); }''')
    V = m['V']
    col = np.stack([(V['c'] >> 16) & 255, (V['c'] >> 8) & 255, V['c'] & 255, V['c'] >> 24], 1).astype('f4') / 255
    data = np.hstack([V['p'], col, V['t'], V['t2']]).astype('f4')
    vao = ctx.vertex_array(prog, [(ctx.buffer(data.tobytes()), '3f 4f 2f 2f', 'p', 'c', 't', 't2')])
    texs = []
    for i in range(len(m['tinfo'])):
        img = Image.open(os.path.join(d, 't%03d.png' % i)).convert('RGBA')
        tx = ctx.texture(img.size, 4, img.tobytes()); tx.build_mipmaps(); tx.repeat_x = tx.repeat_y = True; tx.anisotropy = 8
        texs.append(tx)
    li = Image.open(os.path.join(d, 'luz.png')).convert('RGBA'); lmt = ctx.texture(li.size, 4, li.tobytes())
    prog['mvp'].write(mvp.T.astype('f4').tobytes()); prog['tex'] = 0; prog['lm'] = 1; lmt.use(1)
    prog['fogc'].value = tuple(hor); prog['fog'].value = tuple(cfg['fog'])
    for tex, kind, cull, start, count in sorted(m['batches'], key=lambda b: b[1]):
        ctx.enable(moderngl.BLEND) if kind == 2 else ctx.disable(moderngl.BLEND)
        ctx.depth_mask = kind != 2
        ctx.enable(moderngl.CULL_FACE) if cull else ctx.disable(moderngl.CULL_FACE)
        texs[tex].use(0); prog['kind'] = kind
        vao.render(moderngl.TRIANGLES, vertices=count, first=start)
    img = Image.frombytes('RGBA', size, fbo.read(components=4)).transpose(Image.FLIP_TOP_BOTTOM).convert('RGB')
    # céu em degradê onde não há geometria (aprox.: pixels iguais ao fundo)
    a = np.array(img).astype(np.float32)
    bg = (np.abs(a - hor * 255).sum(2) < 2)
    grad = top[None, :] * (1 - np.linspace(0, 1, size[1]))[:, None] + hor[None, :] * np.linspace(0, 1, size[1])[:, None]
    a[bg] = (np.repeat(grad[:, None, :], size[0], 1) * 255)[bg]
    img = Image.fromarray(a.astype(np.uint8))
    dr = ImageDraw.Draw(img)
    # marcadores: sprite de 90 px (altura) x 44 px
    for s, pp in enumerate(cfg['pos']):
        px, py = pp[:2]
        x, y, z = cell_world(g, H, px, py)
        clip = mvp @ np.array([x, y, z, 1])
        if clip[3] < 1: continue
        sx = (clip[0] / clip[3] + 1) / 2 * size[0]; sy = (1 - clip[1] / clip[3]) / 2 * size[1]
        ppu = proj[1, 1] * size[1] / 2 / clip[3]
        sc = ppu * cfg['upp']
        hh = 90 * sc; ww = 30 * sc
        c = (255, 200, 60) if s == slot else (80, 200, 255)
        dr.rectangle([sx - ww / 2, sy - hh, sx + ww / 2, sy], outline=c, width=2)
        dr.text((sx - ww / 2, sy - hh - 12), str(s), fill=c)
    img.save(out)

CFG = dict(yaw=0, pitch=24, dist=150, fov=30, height=14, orbit=0.6, upp=0.19, shiftx=-0.12, shifty=0.08,
           sky_top=(0x3F, 0x6F, 0xB0), sky_hor=(0xC4, 0xDC, 0xF0), fog=(500, 2200))
if __name__ == '__main__':
    d = sys.argv[1]; out = sys.argv[2]; slot = int(sys.argv[3])
    pos = [tuple(map(float, l.split(';')[0].split('=')[1].split(','))) for l in open(sys.argv[4]).read().split('\n') if '=' in l]
    cfg = dict(CFG); cfg['pos'] = pos
    for kv in sys.argv[5:]:
        k, v = kv.split('='); cfg[k] = float(v)
    main(d, out, cfg, slot)
