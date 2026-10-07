"""Pré-visualiza mapa.bin (mesma combinação de texturas que a DLL)."""
import math, os, struct, sys
import numpy as np
import moderngl
from PIL import Image

def load(d):
    b = open(os.path.join(d, 'mapa.bin'), 'rb').read(); p = 8
    W, H, _, _ = struct.unpack_from('<iiff', b, p); p += 16
    water = struct.unpack_from('<ifffffi', b, p); p += 28
    diff = struct.unpack_from('<fff', b, p); p += 12; amb = struct.unpack_from('<fff', b, p); p += 12
    nt, nb = struct.unpack_from('<ii', b, p); p += 8
    tinfo = [struct.unpack_from('<ii', b, p + 8 * i) for i in range(nt)]; p += 8 * nt
    batches = [struct.unpack_from('<iiiii', b, p + 20 * i) for i in range(nb)]; p += 20 * nb
    nv = struct.unpack_from('<i', b, p)[0]; p += 4
    V = np.frombuffer(b, np.dtype([('p', '<f4', 3), ('c', '<u4'), ('t', '<f4', 2), ('t2', '<f4', 2)]), nv, p)
    return dict(W=W, H=H, water=water, tinfo=tinfo, batches=batches, V=V)

def look_at(eye, target, up=(0, 1, 0)):
    eye, target, up = map(lambda a: np.array(a, np.float64), (eye, target, up))
    f = target - eye; f /= np.linalg.norm(f)
    s = np.cross(f, up); s /= np.linalg.norm(s); u = np.cross(s, f)
    m = np.eye(4); m[0, :3] = s; m[1, :3] = u; m[2, :3] = -f
    m[:3, 3] = -m[:3, :3] @ eye
    return m

def persp(fovy, aspect, near, far):
    f = 1 / math.tan(math.radians(fovy) / 2)
    m = np.zeros((4, 4)); m[0, 0] = f / aspect; m[1, 1] = f
    m[2, 2] = (far + near) / (near - far); m[2, 3] = 2 * far * near / (near - far); m[3, 2] = -1
    return m

def camera(target, yaw, pitch, dist):
    """yaw 0 = olhando para -z (norte do mapa fica no alto); pitch = graus acima do horizonte."""
    t = np.array(target, np.float64)
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    eye = t + dist * np.array([math.sin(math.radians(yaw)) * cp, sp, math.cos(math.radians(yaw)) * cp])
    return eye

def render(d, out, target, yaw, pitch, dist, fov=20, size=(1600, 900), water_frame=0):
    m = load(d)
    ctx = moderngl.create_standalone_context(backend='egl')
    fbo = ctx.simple_framebuffer(size, components=4); fbo.use()
    dep = ctx.depth_renderbuffer(size); fbo = ctx.framebuffer(ctx.renderbuffer(size, 4), dep); fbo.use()
    ctx.enable(moderngl.DEPTH_TEST)
    ctx.clear(0.55, 0.62, 0.72, 1)
    prog = ctx.program(vertex_shader='''#version 330
        in vec3 p; in vec4 c; in vec2 t; in vec2 t2; uniform mat4 mvp;
        out vec4 vc; out vec2 vt; out vec2 vt2;
        void main(){ gl_Position = mvp * vec4(p,1); vc=c; vt=t; vt2=t2; }''',
        fragment_shader='''#version 330
        in vec4 vc; in vec2 vt; in vec2 vt2; uniform sampler2D tex; uniform sampler2D lm; uniform int kind;
        out vec4 o;
        void main(){ vec4 x = texture(tex, vt); if(kind!=2 && x.a<0.5) discard;
          vec3 rgb = x.rgb*vc.rgb; if(kind==0){ vec4 l=texture(lm,vt2); rgb = rgb*l.a + l.rgb; }
          o = vec4(rgb, x.a*vc.a); }''')
    V = m['V']
    col = np.stack([(V['c'] >> 16) & 255, (V['c'] >> 8) & 255, V['c'] & 255, V['c'] >> 24], 1).astype('f4') / 255
    data = np.hstack([V['p'], col, V['t'], V['t2']]).astype('f4')
    vbo = ctx.buffer(data.tobytes())
    vao = ctx.vertex_array(prog, [(vbo, '3f 4f 2f 2f', 'p', 'c', 't', 't2')])
    texs = []
    for i in range(len(m['tinfo'])):
        img = Image.open(os.path.join(d, 't%03d.png' % i)).convert('RGBA')
        tx = ctx.texture(img.size, 4, img.tobytes()); tx.build_mipmaps(); tx.repeat_x = tx.repeat_y = True
        texs.append(tx)
    li = Image.open(os.path.join(d, 'luz.png')).convert('RGBA')
    lmt = ctx.texture(li.size, 4, li.tobytes())
    eye = camera(target, yaw, pitch, dist)
    mvp = persp(fov, size[0] / size[1], 10, 5000) @ look_at(eye, target)
    prog['mvp'].write(mvp.T.astype('f4').tobytes())
    prog['tex'] = 0; prog['lm'] = 1; lmt.use(1)
    for tex, kind, cull, start, count in m['batches']:
        if kind == 2:
            ctx.enable(moderngl.BLEND); ctx.depth_mask = False
        else:
            ctx.disable(moderngl.BLEND); ctx.depth_mask = True
        if cull: ctx.enable(moderngl.CULL_FACE)
        else: ctx.disable(moderngl.CULL_FACE)
        texs[tex].use(0); prog['kind'] = kind
        vao.render(moderngl.TRIANGLES, vertices=count, first=start)
    # água
    w = m['water']
    if w[0]:
        wl = w[1]; W, H = m['W'], m['H']
        img = Image.open(os.path.join(d, 'agua', '%02d.png' % water_frame)).convert('RGBA')
        tx = ctx.texture(img.size, 4, img.tobytes()); tx.repeat_x = tx.repeat_y = True
        q = np.array([[0, wl, 0, 1, 1, 1, .56, 0, 0, 0, 0], [W * 10, wl, 0, 1, 1, 1, .56, W / 2, 0, 0, 0], [W * 10, wl, H * 10, 1, 1, 1, .56, W / 2, H / 2, 0, 0],
                      [0, wl, 0, 1, 1, 1, .56, 0, 0, 0, 0], [W * 10, wl, H * 10, 1, 1, 1, .56, W / 2, H / 2, 0, 0], [0, wl, H * 10, 1, 1, 1, .56, 0, H / 2, 0, 0]], 'f4')
        wb = ctx.buffer(q.tobytes()); wv = ctx.vertex_array(prog, [(wb, '3f 4f 2f 2f', 'p', 'c', 't', 't2')])
        ctx.enable(moderngl.BLEND); ctx.depth_mask = False; ctx.disable(moderngl.CULL_FACE)
        tx.use(0); prog['kind'] = 2; wv.render(moderngl.TRIANGLES)
    Image.frombytes('RGBA', size, fbo.read(components=4)).transpose(Image.FLIP_TOP_BOTTOM).convert('RGB').save(out)
    return eye

if __name__ == '__main__':
    d, out = sys.argv[1], sys.argv[2]
    tx, ty, tz, yaw, pitch, dist = map(float, sys.argv[3:9])
    fov = float(sys.argv[9]) if len(sys.argv) > 9 else 20
    print(render(d, out, (tx, ty, tz), yaw, pitch, dist, fov))
