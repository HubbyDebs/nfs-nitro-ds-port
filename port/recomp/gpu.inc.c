/* Dibujado del 3D con la tarjeta grafica (OpenGL 3.3 o mas). Se incluye al final de gx.c: usa sus
 * tipos y su lista de poligonos ya preparados (transformados, recortados, subdivididos, iluminados por
 * vertice) y sustituye solo el dibujado por software (draw_band). El resultado se lee de vuelta a fb,
 * asi el resto (mezcla con el 2D en video.c, resplandor, ventana) no cambia.
 *
 * Reproduce lo mismo que el dibujado por software: texturas (filtradas con sus reducciones y filtrado
 * anisotropico), modos modulate/decal/toon/brillo, luz por pixel con la formula de la consola,
 * reflejo por pixel, barniz de los coches, niebla, sombras por volumen (con el bufer de plantilla),
 * transparencias y prueba de alfa. Ademas: suavizado de bordes de verdad (MSAA).
 *
 * Diferencias conocidas con el dibujado por software (pequenas): la niebla se aplica a cada
 * fragmento con su propia profundidad (la consola la aplica al pixel final); la regla de la consola de
 * no pintar dos veces un pixel con el mismo translucido solo se aplica a las sombras.
 *
 * NFS_GPU=0 (o la casilla del menu, o F9 en marcha) vuelve al dibujado por software. NFS_MSAA=n
 * muestras de suavizado (por defecto 4). Si algo falla al arrancar OpenGL, se usa el software. */
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

/* Portatiles con dos graficas: pedir la potente (NVIDIA / AMD) */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

int video_gpu(void);
int video_msaa(void);

#define GL_LIST(X)                                                                                                    \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)                               \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)                               \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC, glCreateProgram)                     \
    X(PFNGLATTACHSHADERPROC, glAttachShader) X(PFNGLLINKPROGRAMPROC, glLinkProgram)                                 \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog)                     \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                       \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM2FPROC, glUniform2f)       \
    X(PFNGLUNIFORM3FPROC, glUniform3f) X(PFNGLUNIFORM3FVPROC, glUniform3fv) X(PFNGLUNIFORM4FPROC, glUniform4f)     \
    X(PFNGLUNIFORM1FVPROC, glUniform1fv) X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer)  \
    X(PFNGLBUFFERDATAPROC, glBufferData) X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays)                             \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray)   \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers)           \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)         \
    X(PFNGLFRAMEBUFFERRENDERBUFFERPROC, glFramebufferRenderbuffer) X(PFNGLGENRENDERBUFFERSPROC, glGenRenderbuffers) \
    X(PFNGLBINDRENDERBUFFERPROC, glBindRenderbuffer)                                                                \
    X(PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC, glRenderbufferStorageMultisample)                                   \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC, glCheckFramebufferStatus) X(PFNGLBLITFRAMEBUFFERPROC, glBlitFramebuffer)     \
    X(PFNGLACTIVETEXTUREPROC, glActiveTexture) X(PFNGLGENSAMPLERSPROC, glGenSamplers)                               \
    X(PFNGLSAMPLERPARAMETERIPROC, glSamplerParameteri) X(PFNGLSAMPLERPARAMETERFPROC, glSamplerParameterf)           \
    X(PFNGLBINDSAMPLERPROC, glBindSampler) X(PFNGLBLENDFUNCSEPARATEPROC, glBlendFuncSeparate)                       \
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate) X(PFNGLGENERATEMIPMAPPROC, glGenerateMipmap)

#define GL_DECLARE(type, name) static type p_##name;
GL_LIST(GL_DECLARE)
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform1f p_glUniform1f
#define glUniform2f p_glUniform2f
#define glUniform3f p_glUniform3f
#define glUniform3fv p_glUniform3fv
#define glUniform4f p_glUniform4f
#define glUniform1fv p_glUniform1fv
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glBlitFramebuffer p_glBlitFramebuffer
#define glActiveTexture p_glActiveTexture
#define glGenSamplers p_glGenSamplers
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterf p_glSamplerParameterf
#define glBindSampler p_glBindSampler
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendEquationSeparate p_glBlendEquationSeparate
#define glGenerateMipmap p_glGenerateMipmap

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

static int gpu_state = 0; /* 0 sin probar, 1 funcionando, -1 no disponible */
int gpu_bloom_done;        /* este fotograma el resplandor ya lo hizo la grafica */
static GLuint gpu_bl_prog[3], gpu_bl_fbo[2], gpu_bl_tex[2]; /* resplandor: brillo, difuminado, suma */
static GLuint gpu_comp_prog, gpu_scr_tex[2][4]; /* mezcla final con el 2D y texturas de sus capas */
static HDC gpu_win_dc;
extern int win_gpu_present;
static int gpu_bl_w, gpu_bl_h;
static HDC gpu_dc;
static HGLRC gpu_ctx;
static GLuint gpu_prog, gpu_vao, gpu_vbo, gpu_msaa_fbo, gpu_msaa_color, gpu_msaa_depth;
static GLuint gpu_res_fbo[3], gpu_res_tex[3], gpu_samplers[2][3][3];
static int gpu_res_cur, gpu_samples, gpu_w, gpu_h, gpu_have_prev;
static int gpu_dst = -1;           /* >= 0: dibujar en esa imagen resuelta (la intermedia) */
static float *gpu_verts;
static size_t gpu_verts_cap;
static uint32_t *gpu_read;
double perf_gpu_read_ms;

static const char *GPU_VS =
    "#version 330 core\n"
    "layout(location=0) in vec4 a_pos; layout(location=1) in vec3 a_col; layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in vec3 a_n; layout(location=4) in vec3 a_v; layout(location=5) in vec3 a_m;\n"
    "out vec3 v_col; out vec2 v_uv; out vec3 v_n; out vec3 v_v; out vec3 v_m; out float v_w; noperspective out float v_z;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x / 128.0 - 1.0, 1.0 - a_pos.y / 96.0);\n"
    "  gl_Position = vec4(ndc * a_pos.w, 0.0, a_pos.w);\n"
    "  v_col = a_col; v_uv = a_uv; v_n = a_n; v_v = a_v; v_m = a_m; v_w = a_pos.w; v_z = a_pos.z;\n"
    "}\n";

static const char *GPU_FS =
    "#version 330 core\n"
    "in vec3 v_col; in vec2 v_uv; in vec3 v_n; in vec3 v_v; in vec3 v_m; in float v_w; noperspective in float v_z;\n"
    "layout(location=0) out vec4 o_col;\n"
    "uniform sampler2D u_tex; uniform int u_has_tex; uniform vec2 u_tsize; uniform int u_unpremul;\n"
    "uniform int u_mode; uniform float u_alpha; uniform int u_lit;\n"
    "uniform int u_lmask; uniform vec3 u_ldir[4]; uniform vec3 u_lhalf[4]; uniform vec3 u_lcol[4];\n"
    "uniform vec3 u_dif; uniform vec3 u_amb; uniform vec3 u_spe; uniform vec3 u_emi; uniform int u_shin_on; uniform float u_shin[128];\n"
    "uniform int u_highlight; uniform vec3 u_toon[32];\n"
    "uniform int u_env; uniform vec4 u_envp0; uniform vec4 u_envp1;\n"
    "uniform int u_shiny; uniform vec3 u_sky_h; uniform vec3 u_sky_z; uniform sampler2D u_prev; uniform int u_has_prev;\n"
    "uniform vec2 u_res; uniform float u_scale;\n"
    "uniform int u_wbuffer; uniform float u_depth_bias; uniform float u_alpha_ref; uniform int u_cutout; uniform int u_pass;\n"
    "uniform int u_maskonly; uniform vec3 u_glow;\n"
    "uniform int u_fog; uniform int u_fog_alpha_only; uniform vec3 u_fog_rgb; uniform float u_fog_a; uniform float u_fog_offset;\n"
    "uniform float u_fog_step; uniform float u_fog_table[32];\n"
    "float t_lum(vec3 c) { return max(c.r, max(c.g, c.b)); }\n"
    "void main() {\n"
    "  float depth = (u_wbuffer != 0) ? v_w / 65536.0 : v_z * 0.5 + 0.5;\n"
    "  gl_FragDepth = clamp(depth + u_depth_bias, 0.0, 1.0);\n"
    "  if (u_maskonly != 0) { o_col = vec4(0.0); return; }\n"
    "  vec3 n = vec3(0.0, 0.0, 1.0); vec3 c; float a = u_alpha;\n"
    "  if (u_lit != 0) {\n"
    "    n = normalize(v_n); c = u_emi;\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      if (((u_lmask >> i) & 1) == 0) continue;\n"
    "      float d = max(-dot(u_ldir[i], n), 0.0);\n"
    "      float h = max(-dot(u_lhalf[i], n), 0.0); h = max(2.0 * h * h - 1.0, 0.0);\n"
    "      if (u_shin_on != 0) h = u_shin[int(h * 127.0)];\n"
    "      c += (u_spe * h + u_dif * d + u_amb) * u_lcol[i];\n"
    "    }\n"
    "    c = min(c, vec3(1.0));\n"
    "  } else c = v_col;\n"
    "  vec3 tone = vec3(0.0);\n"
    "  if (u_mode == 2) { int k = min(int(c.r * 31.0 + 0.5), 31); if (u_highlight != 0) tone = u_toon[k]; else c = u_toon[k]; }\n"
    "  if (u_has_tex != 0) {\n"
    "    vec2 st = v_uv;\n"
    "    if (u_env != 0) {\n"
    "      vec3 m = v_m; float ml = length(m); if (ml > 1e-6) m /= ml;\n"
    "      st = vec2(u_envp0.x + dot(m, vec3(u_envp0.z, u_envp0.w, u_envp1.x)), u_envp0.y + dot(m, u_envp1.yzw)) / 16.0;\n"
    "    }\n"
    "    vec4 t = texture(u_tex, st / u_tsize);\n"
    "    if (u_unpremul != 0) { if (t.a <= 0.0) discard; t.rgb /= t.a; }\n"
    "    if (u_mode == 1) c = mix(c, t.rgb, t.a); else { c *= t.rgb; a *= t.a; }\n"
    "  }\n"
    "  c += tone;\n"
    "  if (u_shiny != 0) {\n"
    "    vec3 V = normalize(-v_v); float ndv = max(dot(n, V), 0.0); float f = 1.0 - ndv;\n"
    "    float fres = 0.15 + 0.70 * f * f * f * f * f; if (u_env != 0) fres *= 0.6;\n"
    "    vec3 R = 2.0 * ndv * n - V; vec3 sky;\n"
    "    if (R.y >= 0.0) sky = mix(u_sky_h, u_sky_z, min(R.y, 1.0)); else sky = u_sky_h * (0.55 - 0.4 * min(-R.y * 3.0, 1.0));\n"
    "    float ws = 0.25 - R.z * 1.5;\n"
    "    if (u_has_prev != 0 && ws > 0.0) {\n"
    "      ws = min(ws, 1.0); vec2 cp = gl_FragCoord.xy + vec2(R.x * u_res.x * 0.30, R.y * u_res.y * 0.45);\n"
    "      float dd = u_scale * 2.0; vec3 acc = vec3(0.0); float got = 0.0;\n"
    "      for (int k = 0; k < 4; k++) {\n"
    "        vec2 q = clamp(cp + vec2((k & 1) != 0 ? dd : -dd, (k & 2) != 0 ? dd : -dd), vec2(0.5), u_res - 0.5);\n"
    "        vec4 e = texture(u_prev, q / u_res); if (e.a > 0.0) { acc += e.rgb; got += 1.0; }\n"
    "      }\n"
    "      if (got > 0.0) sky = mix(sky, acc / got, ws * got / 4.0);\n"
    "    }\n"
    "    c = mix(c, sky, fres);\n"
    "    for (int i = 0; i < 4; i++) {\n"
    "      if (((u_lmask >> i) & 1) == 0) continue;\n"
    "      float h = max(dot(n, normalize(V - u_ldir[i])), 0.0);\n"
    "      c += (pow(h, 96.0) * 1.3 + pow(h, 16.0) * 0.18) * u_lcol[i];\n"
    "    }\n"
    "  }\n"
    "  if (a <= 0.0 || a <= u_alpha_ref) discard;\n"
    "  if (u_cutout != 0 && a < 0.5) discard;\n"
    "  if (u_glow.r + u_glow.g + u_glow.b > 0.0) { o_col = vec4(t_lum(c) * u_glow * a * a, 0.0); return; }\n"
    "  if (u_pass == 1 && a < 0.999) discard;\n"
    "  if (u_pass == 2 && a >= 0.999) discard;\n"
    "  if (u_pass != 2) a = 1.0;\n"
    "  c = min(c, vec3(1.0));\n"
    "  if (u_fog != 0) {\n"
    "    float d15 = (u_wbuffer != 0) ? v_w * 8.0 : v_z * 16384.0 + 16383.0;\n"
    "    float pos = (d15 - u_fog_offset) / u_fog_step - 1.0, den;\n"
    "    if (pos <= 0.0) den = u_fog_table[0]; else if (pos >= 31.0) den = u_fog_table[31];\n"
    "    else { int k = int(pos); den = mix(u_fog_table[k], u_fog_table[k + 1], pos - float(k)); }\n"
    "    float k = den >= 127.0 ? 1.0 : den / 128.0;\n"
    "    if (u_fog_alpha_only == 0) c = mix(c, u_fog_rgb, k);\n"
    "    a = mix(a, u_fog_a, k);\n"
    "  }\n"
    "  o_col = vec4(c * a, a); /* premultiplicado: asi se mezcla y se lee de vuelta */\n"
    "}\n";

/* Resplandor en la grafica (mismo calculo que bloom_pass en gx.c): reducir a 1/4 quedandose con lo
 * que pasa del umbral, difuminar (gaussiano separable, radio 6) y sumar encima de los pixeles con 3D */
static const char *GPU_FULL_VS =
    "#version 330 core\n"
    "out vec2 v_p;\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); v_p = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
static const char *GPU_BRIGHT_FS =
    "#version 330 core\n"
    "in vec2 v_p; out vec4 o; uniform sampler2D u_src; uniform vec2 u_texel;\n"
    "void main() {\n"
    "  vec3 acc = vec3(0.0); vec3 rgb[16]; float lum[16]; float mean = 0.0;\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    vec4 c = texture(u_src, v_p + (vec2(i, j) - 1.5) * u_texel); int k = j * 4 + i;\n"
    "    rgb[k] = c.a > 0.0 ? c.rgb / c.a : vec3(0.0); lum[k] = c.a > 0.0 ? dot(rgb[k], vec3(0.3, 0.59, 0.11)) : -1.0;\n"
    "    mean += max(lum[k], 0.0);\n"
    "  }\n"
    "  mean /= 16.0;\n"
    "  float thr = max(0.84, mean + 0.10); /* solo lo que destaca sobre su entorno: un cielo claro y uniforme no brilla */\n"
    "  for (int k = 0; k < 16; k++) { float g = (lum[k] - thr) / 0.16; if (g > 0.0) acc += rgb[k] * min(g, 1.0); }\n"
    "  o = vec4(acc / 16.0, 1.0);\n"
    "}\n";
static const char *GPU_BLUR_FS =
    "#version 330 core\n"
    "in vec2 v_p; out vec4 o; uniform sampler2D u_src; uniform vec2 u_step;\n"
    "const float w[7] = float[7](0.1974, 0.1747, 0.1210, 0.0656, 0.0278, 0.0092, 0.0024);\n"
    "void main() {\n"
    "  vec3 v = texture(u_src, v_p).rgb * w[0];\n"
    "  for (int t = 1; t < 7; t++) v += (texture(u_src, v_p - u_step * float(t)).rgb + texture(u_src, v_p + u_step * float(t)).rgb) * w[t];\n"
    "  o = vec4(v, 1.0);\n"
    "}\n";
static const char *GPU_ADD_FS =
    "#version 330 core\n"
    "in vec2 v_p; out vec4 o; uniform sampler2D u_src;\n"
    "void main() { o = vec4(texture(u_src, v_p).rgb * 0.7, 0.0); }\n";

/* Mezcla final de una pantalla: las capas 2D (juntadas por video.c a 256x192) con el 3D a alta
 * resolucion, con las mismas reglas que upscale_row/over_3d/over_front de video.c, y el brillo maestro */
static const char *GPU_COMP_FS =
    "#version 330 core\n"
    "in vec2 v_p; out vec4 o;\n"
    "uniform sampler2D t_back; uniform sampler2D t_front; uniform sampler2D t_raw; uniform sampler2D t_ids; uniform sampler2D t_3d;\n"
    "uniform int u_use3d; uniform int u_fx3d; uniform int u_cnt; uniform float u_eva; uniform float u_evb;\n"
    "uniform int u_bmode; uniform float u_bfac; uniform int u_smooth;\n"
    "vec3 fade(vec3 c, int fx) { if (fx > 0) return c + (1.0 - c) * float(fx) / 16.0; if (fx < 0) return c + c * float(fx) / 16.0; return c; }\n"
    "bool second(int layer) { return ((u_cnt >> (8 + layer)) & 1) != 0; }\n"
    /* Ampliacion suave de las capas 2D (NFS_SMOOTH2D): la misma idea que las calcomanias de los coches (gx.c,
     * smooth_texture). Color con Catmull-Rom; donde hay un borde entre los 4 pixeles mas cercanos, el color va
     * del mas oscuro al mas claro segun la luminancia interpolada con una B-spline: bordes nitidos y redondeados.
     * La capa de delante (sprites, letras) solo existe en algunos pixeles: los que no la tienen se rellenan con
     * uno vecino que si, y su contorno sobre el 3D se suaviza igual (cobertura con B-spline). */
    "float lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }\n"
    "float cr(float x) { x = abs(x); return x < 1.0 ? 1.5*x*x*x - 2.5*x*x + 1.0 : (x < 2.0 ? -0.5*x*x*x + 2.5*x*x - 4.0*x + 2.0 : 0.0); }\n"
    "float bs(float x) { x = abs(x); return x < 1.0 ? (4.0 - 6.0*x*x + 3.0*x*x*x) / 6.0 : (x < 2.0 ? (2.0-x)*(2.0-x)*(2.0-x) / 6.0 : 0.0); }\n"
    "bool has_front(ivec2 q) { return int(texelFetch(t_ids, q, 0).g * 255.0 + 0.5) != 255; }\n"
    "float edge(float u) { u = clamp((u - 0.3) / 0.4, 0.0, 1.0); return u * u * (3.0 - 2.0 * u); }\n"
    "vec3 smooth_layer(int which, vec2 q, out float cover) {\n"
    "  ivec2 i0 = ivec2(floor(q)); vec2 f = q - vec2(i0);\n"
    "  bool sparse = which != 0; vec3 fill = vec3(0.0); bool any = false;\n"
    "  for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {\n"
    "    ivec2 pp = clamp(i0 + ivec2(i, j), ivec2(0), ivec2(255, 191));\n"
    "    if (!any && (!sparse || has_front(pp))) { fill = which == 2 ? texelFetch(t_raw, pp, 0).rgb : (which == 1 ? texelFetch(t_front, pp, 0).rgb : texelFetch(t_back, pp, 0).rgb); any = true; }\n"
    "  }\n"
    "  vec3 acc = vec3(0.0), lo = fill, hi = fill; float lb = 0.0, cov = 0.0, llo = 2.0, lhi = -1.0;\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    ivec2 pp = clamp(i0 + ivec2(i - 1, j - 1), ivec2(0), ivec2(255, 191));\n"
    "    bool ok = !sparse || has_front(pp);\n"
    "    vec3 c = !ok ? fill : (which == 2 ? texelFetch(t_raw, pp, 0).rgb : (which == 1 ? texelFetch(t_front, pp, 0).rgb : texelFetch(t_back, pp, 0).rgb));\n"
    "    float wc = cr(f.x - float(i - 1)) * cr(f.y - float(j - 1)), wb = bs(f.x - float(i - 1)) * bs(f.y - float(j - 1));\n"
    "    acc += c * wc; lb += lum(c) * wb; cov += (ok ? 1.0 : 0.0) * wb;\n"
    "    if (i >= 1 && i <= 2 && j >= 1 && j <= 2 && ok) { float l = lum(c); if (l < llo) { llo = l; lo = c; } if (l > lhi) { lhi = l; hi = c; } }\n"
    "  }\n"
    "  cover = sparse ? edge(cov) : 1.0;\n"
    "  if (lhi - llo > 0.15) return mix(lo, hi, edge(clamp((lb - llo) / (lhi - llo), 0.0, 1.0)));\n"
    "  return clamp(acc, 0.0, 1.0);\n"
    "}\n"
    "void main() {\n"
    "  vec2 uv = vec2(v_p.x, 1.0 - v_p.y);\n"
    "  ivec2 p = clamp(ivec2(uv * vec2(256.0, 192.0)), ivec2(0), ivec2(255, 191));\n"
    "  vec4 id = texelFetch(t_ids, p, 0) * 255.0;\n"
    "  int bid = int(id.r + 0.5), fid = int(id.g + 0.5); bool pend = id.b > 0.5;\n"
    "  vec3 b, f, raw; float cover = fid != 255 ? 1.0 : 0.0;\n"
    "  if (u_smooth != 0) {\n"
    "    vec2 q = uv * vec2(256.0, 192.0) - 0.5; float dummy;\n"
    "    b = smooth_layer(0, q, dummy); f = smooth_layer(1, q, cover); raw = smooth_layer(2, q, dummy);\n"
    "  } else { b = texelFetch(t_back, p, 0).rgb; f = texelFetch(t_front, p, 0).rgb; raw = texelFetch(t_raw, p, 0).rgb; }\n"
    /* lo que se ve debajo de la capa de delante (3D o fondo) y con ella; luego se mezclan por la cobertura */
    "  vec3 under = b, over = f;\n"
    "  vec4 g = u_use3d != 0 ? texture(t_3d, v_p) : vec4(0.0);\n"
    "  if (g.a > 0.002) {\n"
    "    vec3 c3 = g.rgb / g.a;\n"
    "    if (u_fx3d != 0) c3 = fade(c3, u_fx3d);\n"
    "    if (g.a < 0.998 && second(bid)) c3 = c3 * g.a + b * (1.0 - g.a);\n"
    "    under = c3;\n"
    "    over = (pend && second(0)) ? min((raw * u_eva + c3 * u_evb) / 16.0, vec3(1.0)) : f;\n"
    "  } else {\n"
    "    over = (pend && second(bid)) ? min((raw * u_eva + b * u_evb) / 16.0, vec3(1.0)) : f;\n"
    "  }\n"
    "  vec3 c = mix(under, over, cover);\n"
    "  if (u_bmode == 1) c = c + (1.0 - c) * u_bfac; else if (u_bmode == 2) c = c - c * u_bfac;\n"
    "  o = vec4(c, 1.0);\n"
    "}\n";

static GLuint gpu_compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, 0);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, 0, log);
        fprintf(stderr, "[gpu] error al compilar un sombreador:%c%s%c", 10, log, 10);
        return 0;
    }
    return s;
}

static LRESULT CALLBACK gpu_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static void gpu_targets(void) { /* imagenes donde se dibuja, al tamano de la resolucion interna */
    gpu_w = fb_w;
    gpu_h = fb_h;
    GLint max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    gpu_samples = video_msaa();
    if (gpu_samples > max_samples) gpu_samples = max_samples;
    if (gpu_samples < 1) gpu_samples = 1;
    glGenFramebuffers(1, &gpu_msaa_fbo);
    glGenRenderbuffers(1, &gpu_msaa_color);
    glGenRenderbuffers(1, &gpu_msaa_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, gpu_msaa_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, gpu_samples, GL_RGBA8, gpu_w, gpu_h);
    glBindRenderbuffer(GL_RENDERBUFFER, gpu_msaa_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, gpu_samples, GL_DEPTH32F_STENCIL8, gpu_w, gpu_h);
    glBindFramebuffer(GL_FRAMEBUFFER, gpu_msaa_fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, gpu_msaa_color);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, gpu_msaa_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) fprintf(stderr, "[gpu] imagen MSAA incompleta%c", 10);
    for (int i = 0; i < 3; i++) { /* dos imagenes resueltas que se alternan (la anterior se refleja en la pintura) y la intermedia (120 fps) */
        glGenTextures(1, &gpu_res_tex[i]);
        glBindTexture(GL_TEXTURE_2D, gpu_res_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, gpu_w, gpu_h, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &gpu_res_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, gpu_res_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu_res_tex[i], 0);
    }
    gpu_read = malloc((size_t)gpu_w * gpu_h * 4);
    gpu_bl_w = gpu_w / 4;
    gpu_bl_h = gpu_h / 4;
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &gpu_bl_tex[i]);
        glBindTexture(GL_TEXTURE_2D, gpu_bl_tex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, gpu_bl_w, gpu_bl_h, 0, GL_RGBA, GL_FLOAT, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &gpu_bl_fbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, gpu_bl_fbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, gpu_bl_tex[i], 0);
    }
    fprintf(stderr, "[gpu] dibujando a %dx%d con %d muestras de suavizado%c", gpu_w, gpu_h, gpu_samples, 10);
}

static GLuint gpu_link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, 0, log);
        fprintf(stderr, "[gpu] error al enlazar:%c%s%c", 10, log, 10);
        return 0;
    }
    return p;
}

static int gpu_init(void) {
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = gpu_wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "NfsNitroGL";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("NfsNitroGL", "", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, 0, 0, wc.hInstance, 0);
    if (!w) return 0;
    gpu_dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    if (!SetPixelFormat(gpu_dc, ChoosePixelFormat(gpu_dc, &pfd), &pfd)) return 0;
    HGLRC legacy = wglCreateContext(gpu_dc);
    if (!legacy || !wglMakeCurrent(gpu_dc, legacy)) return 0;
    PFNWGLCREATECONTEXTATTRIBSARBPROC cca = (PFNWGLCREATECONTEXTATTRIBSARBPROC)wglGetProcAddress("wglCreateContextAttribsARB");
    if (!cca) return 0;
    int attr[] = {0x2091, 3, 0x2092, 3, 0x9126, 1, 0}; /* 3.3, perfil core */
    gpu_ctx = cca(gpu_dc, 0, attr);
    if (!gpu_ctx) return 0;
    wglMakeCurrent(gpu_dc, gpu_ctx);
    wglDeleteContext(legacy);
#define GL_LOAD(type, name) if (!(p_##name = (type)wglGetProcAddress(#name))) { fprintf(stderr, "[gpu] falta %s%c", #name, 10); return 0; }
    GL_LIST(GL_LOAD)
    fprintf(stderr, "[gpu] %s | %s | OpenGL %s%c", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION), 10);
    GLuint vs = gpu_compile(GL_VERTEX_SHADER, GPU_VS), fs = gpu_compile(GL_FRAGMENT_SHADER, GPU_FS);
    if (!vs || !fs) return 0;
    gpu_prog = glCreateProgram();
    glAttachShader(gpu_prog, vs);
    glAttachShader(gpu_prog, fs);
    glLinkProgram(gpu_prog);
    GLint ok = 0;
    glGetProgramiv(gpu_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(gpu_prog, sizeof log, 0, log);
        fprintf(stderr, "[gpu] error al enlazar:%c%s%c", 10, log, 10);
        return 0;
    }
    GLuint fvs = gpu_compile(GL_VERTEX_SHADER, GPU_FULL_VS);
    {
        GLuint f = gpu_compile(GL_FRAGMENT_SHADER, GPU_COMP_FS);
        if (!fvs || !f || !(gpu_comp_prog = gpu_link(fvs, f))) return 0;
    }
    const char *bl_src[3] = {GPU_BRIGHT_FS, GPU_BLUR_FS, GPU_ADD_FS};
    for (int i = 0; i < 3; i++) {
        GLuint f = gpu_compile(GL_FRAGMENT_SHADER, bl_src[i]);
        if (!fvs || !f || !(gpu_bl_prog[i] = gpu_link(fvs, f))) return 0;
    }
    glGenVertexArrays(1, &gpu_vao);
    glBindVertexArray(gpu_vao);
    glGenBuffers(1, &gpu_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gpu_vbo);
    static const int sizes[6] = {4, 3, 2, 3, 3, 3};
    for (int i = 0, ofs = 0; i < 6; ofs += sizes[i], i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, 18 * 4, (void *)(intptr_t)(ofs * 4));
    }
    /* muestreadores: [filtro][modo s][modo t]; modo 0 = recortar, 1 = repetir, 2 = repetir en espejo */
    static const GLint wrap[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
    GLfloat max_aniso = 1;
    glGetFloatv(GL_TEXTURE_MAX_ANISOTROPY, &max_aniso);
    if (glGetError() != GL_NO_ERROR || max_aniso < 1) max_aniso = 1;
    for (int f = 0; f < 2; f++)
        for (int s = 0; s < 3; s++)
            for (int t = 0; t < 3; t++) {
                GLuint sm;
                glGenSamplers(1, &sm);
                glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, wrap[s]);
                glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, wrap[t]);
                /* NFS_SHARP=1 (por defecto): nitidas tambien de lejos. Sin mipmaps el suelo lejano parpadeaba; ahora
                 * con mipmaps, filtro anisotropico x16 (lo que mas emborronaba era el suelo visto de canto, y eso lo
                 * arregla el anisotropico) y el nivel adelantado 0,75 hacia el mas nitido. NFS_SHARP=2: sin mipmaps
                 * (lo de antes); NFS_SHARP=0: mipmaps normales (-0,4). */
                int sharp = video_sharp();
                glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER,
                                    f ? (sharp == 2 ? GL_LINEAR : GL_LINEAR_MIPMAP_LINEAR) : (sharp == 2 ? GL_NEAREST : GL_NEAREST_MIPMAP_NEAREST));
                glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, f ? GL_LINEAR : GL_NEAREST);
                if (f) glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY, max_aniso > 16 ? 16 : max_aniso);
                glSamplerParameterf(sm, GL_TEXTURE_LOD_BIAS, getenv("NFS_LODBIAS") ? (float)atof(getenv("NFS_LODBIAS")) : f ? (sharp ? -0.75f : -0.4f) : 0.0f);
                gpu_samplers[f][s][t] = sm;
            }
    glGetError();
    return 1;
}

static GLint U(const char *name) { return glGetUniformLocation(gpu_prog, name); }

static void gpu_upload_texture(TexImage *t) {
    static uint32_t *tmp;
    static size_t tmp_cap;
    if (!t->gl) glGenTextures(1, &t->gl);
    glBindTexture(GL_TEXTURE_2D, t->gl);
    for (int l = 0; l < t->levels; l++) {
        size_t n = (size_t)t->mw[l] * t->mh[l];
        if (n > tmp_cap) { tmp = realloc(tmp, n * 4); tmp_cap = n; }
        for (size_t i = 0; i < n; i++) { /* colores premultiplicados: el filtrado no ensucia los bordes recortados */
            uint32_t c = t->mip[l][i], a = c >> 24;
            if (a == 255) { tmp[i] = c; continue; }
            tmp[i] = (a << 24) | ((((c >> 16) & 255) * a / 255) << 16) | ((((c >> 8) & 255) * a / 255) << 8) | ((c & 255) * a / 255);
        }
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, t->mw[l], t->mh[l], 0, GL_BGRA, GL_UNSIGNED_BYTE, tmp);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, t->levels - 1);
    t->gl_version = t->version;
}

/* Clave de estado de un poligono: los seguidos con la misma clave van en una sola llamada de dibujo */
static int gpu_same_batch(const Poly *a, const Poly *b, const TexImage *ta, const TexImage *tb) {
    if (ta != tb || a->attr != b->attr || a->translucent != b->translucent || a->lit != b->lit || a->env != b->env) return 0;
    if (((a->tex >> 16) & 15) != ((b->tex >> 16) & 15)) return 0;
    if (a->lit && a->ls != b->ls) return 0;
    if (a->glow != b->glow) return 0;
    if (a->env && memcmp(a->envp, b->envp, sizeof a->envp)) return 0;
    return 1;
}

typedef struct GpuBatch {
    int first, count, poly;
} GpuBatch;

static void gpu_set_poly_state(const Poly *p, const TexImage *ti, const RenderState *ds, int pass, int *last_ls) {
    int mode = (p->attr >> 4) & 3;
    glUniform1i(U("u_mode"), mode);
    glUniform1f(U("u_alpha"), ((p->attr >> 16) & 31) / 31.0f);
    int lit = p->lit && ds->pixel_light;
    glUniform1i(U("u_lit"), lit);
    if (lit && p->ls != *last_ls) {
        const LightState *ls = &lstates[p->ls];
        *last_ls = p->ls;
        glUniform1i(U("u_lmask"), (int)ls->mask);
        glUniform3fv(U("u_ldir"), 4, &ls->dir[0][0]);
        glUniform3fv(U("u_lhalf"), 4, &ls->half[0][0]);
        glUniform3fv(U("u_lcol"), 4, &ls->col[0][0]);
        glUniform3fv(U("u_dif"), 1, ls->dif);
        glUniform3fv(U("u_amb"), 1, ls->amb);
        glUniform3fv(U("u_spe"), 1, ls->spe);
        glUniform3fv(U("u_emi"), 1, ls->emi);
        glUniform1i(U("u_shin_on"), ls->shin_on);
        float shin[128];
        for (int i = 0; i < 128; i++) shin[i] = ls->shin[i] / 255.0f;
        glUniform1fv(U("u_shin"), 128, shin);
    }
    glUniform1i(U("u_has_tex"), ti != 0);
    if (ti) {
        if (ti->gl_version != ti->version || !ti->gl) gpu_upload_texture((TexImage *)ti);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ti->gl);
        int rs = (p->tex >> 16) & 1, rt = (p->tex >> 17) & 1, fs = (p->tex >> 18) & 1, ft = (p->tex >> 19) & 1;
        glBindSampler(0, gpu_samplers[ds->filter][rs ? (fs ? 2 : 1) : 0][rt ? (ft ? 2 : 1) : 0]);
        glUniform2f(U("u_tsize"), (float)ti->w, (float)ti->h);
        glUniform1i(U("u_unpremul"), ti->has_alpha);
    }
    int env = p->env && ds->env_pixel;
    glUniform1i(U("u_env"), env);
    if (env) {
        glUniform4f(U("u_envp0"), p->envp[0], p->envp[1], p->envp[2], p->envp[3]);
        glUniform4f(U("u_envp1"), p->envp[4], p->envp[5], p->envp[6], p->envp[7]);
    }
    glUniform1i(U("u_shiny"), lit && ds->shiny && (p->env || mode == 2));
    glUniform1f(U("u_depth_bias"), (p->attr & 0x4000) ? -0.00002f : 0.0f);
    glDepthFunc((p->attr & 0x4000) ? GL_LEQUAL : GL_LESS);
    glUniform1i(U("u_cutout"), ds->filter && !p->translucent);
    glUniform1i(U("u_pass"), pass);
    glUniform1i(U("u_fog"), ds->fog && (p->attr & 0x8000));
}

/* Una fila de la imagen leida de la grafica a fb: se le da la vuelta (OpenGL empieza por abajo) y se
 * deshace el premultiplicado del alfa */
static void gpu_convert_row(void *ctx, int y) {
    (void)ctx;
    const uint32_t *src = &gpu_read[(size_t)(gpu_h - 1 - y) * gpu_w];
    uint32_t *dst = &fb[(size_t)y * fb_w];
    for (int x = 0; x < gpu_w; x++) {
        uint32_t c = src[x], a = c >> 24;
        if (a == 255 || a == 0) { dst[x] = c; continue; }
        uint32_t r = ((c >> 16) & 255) * 255 / a, g = ((c >> 8) & 255) * 255 / a, b = (c & 255) * 255 / a;
        dst[x] = (a << 24) | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
    }
}

/* Dibuja la lista de poligonos del fotograma y deja el resultado en fb */
/* gpu_t (declarado en gx.c), NFS_GPU_LOG: vertices, ordenes, espera+lectura, conversion, (render: texturas, resplandor) */
static double gpu_now(void) { /* milisegundos */
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void gpu_render_frame(void) {
    double t0 = gpu_now();
    if (fb_w != gpu_w || fb_h != gpu_h) gpu_targets();
    RenderState *ds = &draw_state;
    /* vertices: cada poligono en abanico de triangulos; orden: opacos y luego translucidos */
    size_t need = 0;
    for (int i = 0; i < poly_count; i++) need += (size_t)(polys[i].n - 2) * 3;
    if (need * 18 > gpu_verts_cap) {
        gpu_verts_cap = need * 18 + 18 * 3 * 4096;
        gpu_verts = realloc(gpu_verts, gpu_verts_cap * sizeof(float));
    }
    static GpuBatch *batches;
    static int batch_cap;
    if (batch_cap < poly_count + 1) { batch_cap = poly_count + 1024; batches = realloc(batches, sizeof(GpuBatch) * batch_cap); }
    int nb = 0, nv = 0, n_opaque_batches;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < poly_count; i++) {
            Poly *p = &polys[i];
            if (p->translucent != pass || ((p->attr >> 16) & 31) == 0) continue;
            if (((p->attr >> 4) & 3) == 3 && !ds->shadows) continue;
            if (nb && batches[nb - 1].poly >= 0 && gpu_same_batch(&polys[batches[nb - 1].poly], p, poly_tex[batches[nb - 1].poly], poly_tex[i]) &&
                ((p->attr >> 4) & 3) != 3) {
            } else {
                batches[nb].first = nv;
                batches[nb].count = 0;
                batches[nb].poly = i;
                nb++;
            }
            for (int k = 1; k + 1 < p->n; k++) {
                const Vtx *tri[3] = {&p->v[0], &p->v[k], &p->v[k + 1]};
                for (int j = 0; j < 3; j++) {
                    const Vtx *v = tri[j];
                    float *o = &gpu_verts[(size_t)nv * 18];
                    o[0] = v->x; o[1] = v->y; o[2] = v->z; o[3] = v->w;
                    o[4] = v->r; o[5] = v->g; o[6] = v->b;
                    o[7] = v->s; o[8] = v->t;
                    o[9] = v->nx; o[10] = v->ny; o[11] = v->nz;
                    o[12] = v->vx; o[13] = v->vy; o[14] = v->vz;
                    o[15] = v->mx; o[16] = v->my; o[17] = v->mz;
                    nv++;
                }
            }
            batches[nb - 1].count = nv - batches[nb - 1].first;
        }
        if (pass == 0) n_opaque_batches = nb;
    }
    double t1 = gpu_now();
    gpu_t[0] += t1 - t0;
    glBindVertexArray(gpu_vao);
    glBindBuffer(GL_ARRAY_BUFFER, gpu_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nv * 18 * 4, gpu_verts, GL_STREAM_DRAW);

    glBindFramebuffer(GL_FRAMEBUFFER, gpu_msaa_fbo);
    glViewport(0, 0, gpu_w, gpu_h);
    glUseProgram(gpu_prog);
    /* fondo: el color de fondo, ya con su niebla si la lleva */
    float cr = ((clear_color >> 16) & 255) / 255.0f, cg = ((clear_color >> 8) & 255) / 255.0f, cb = (clear_color & 255) / 255.0f,
          ca = (clear_color >> 24) / 255.0f;
    uint32_t cc = io_reg(0x350);
    if (ds->fog && (cc & 0x8000)) {
        float k = fog_density(ds, ds->clear_depth15) / 128.0f;
        if (!ds->fog_alpha_only) {
            cr += (ds->fog_rgb[0] / 255 - cr) * k; cg += (ds->fog_rgb[1] / 255 - cg) * k; cb += (ds->fog_rgb[2] / 255 - cb) * k;
        }
        ca += (ds->fog_a / 255 - ca) * k;
    }
    glClearColor(cr * ca, cg * ca, cb * ca, ca);
    glClearDepth(1.0);
    glClearStencil((int)((cc >> 24) & 63));
    glStencilMask(0xFF);
    glColorMask(1, 1, 1, 1);
    glDepthMask(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_MULTISAMPLE);

    /* estado global */
    glUniform1i(U("u_tex"), 0);
    glUniform1i(U("u_prev"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gpu_res_tex[gpu_res_cur ^ 1]);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(U("u_has_prev"), gpu_have_prev && ds->shiny);
    glUniform2f(U("u_res"), (float)gpu_w, (float)gpu_h);
    glUniform1f(U("u_scale"), (float)scale);
    glUniform1i(U("u_wbuffer"), ds->wbuffer);
    glUniform1f(U("u_alpha_ref"), ds->alpha_test >= 0 ? ds->alpha_test / 31.0f : -1.0f);
    glUniform1i(U("u_highlight"), ds->highlight);
    glUniform3fv(U("u_toon"), 32, &ds->toon[0][0]);
    glUniform3fv(U("u_sky_h"), 1, ds->sky_h);
    glUniform3fv(U("u_sky_z"), 1, ds->sky_z);
    glUniform1i(U("u_fog_alpha_only"), ds->fog_alpha_only);
    glUniform3f(U("u_fog_rgb"), ds->fog_rgb[0] / 255, ds->fog_rgb[1] / 255, ds->fog_rgb[2] / 255);
    glUniform1f(U("u_fog_a"), ds->fog_a / 255);
    glUniform1f(U("u_fog_offset"), (float)ds->fog_offset);
    glUniform1f(U("u_fog_step"), (float)ds->fog_step);
    float ft[32];
    for (int i = 0; i < 32; i++) ft[i] = ds->fog_table[i];
    glUniform1fv(U("u_fog_table"), 32, ft);
    glUniform1i(U("u_maskonly"), 0);
    glUniform3f(U("u_glow"), 0, 0, 0);

    int last_ls = -1, shadow_drawn = 0, last_trans_id = -1;
    for (int b = 0; b < nb; b++) {
        const GpuBatch *bt = &batches[b];
        const Poly *p = &polys[bt->poly];
        const TexImage *ti = poly_tex[bt->poly];
        int mode = (p->attr >> 4) & 3, id = (p->attr >> 24) & 63;
        if (b < n_opaque_batches) { /* opacos: escriben profundidad e identificador (plantilla, bits 0-5) */
            gpu_set_poly_state(p, ti, ds, 0, &last_ls);
            glDisable(GL_BLEND);
            glDepthMask(1);
            glColorMask(1, 1, 1, 1);
            glStencilFunc(GL_ALWAYS, id, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask(0x3F);
            glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
            continue;
        }
        if (mode == 3) { /* sombras por volumen, como en la consola (ver draw_triangle) */
            gpu_set_poly_state(p, ti, ds, 2, &last_ls);
            glDepthMask(0);
            if (!id) { /* mascara: marca (bit 7) donde falla la prueba de profundidad */
                if (shadow_drawn) { glStencilMask(0x80); glClearStencil(0); glClear(GL_STENCIL_BUFFER_BIT); shadow_drawn = 0; }
                glColorMask(0, 0, 0, 0);
                glUniform1i(U("u_maskonly"), 1);
                glStencilFunc(GL_ALWAYS, 0x80, 0x80);
                glStencilOp(GL_KEEP, GL_REPLACE, GL_KEEP);
                glStencilMask(0x80);
                glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
                glUniform1i(U("u_maskonly"), 0);
                glColorMask(1, 1, 1, 1);
                continue;
            }
            /* 1) quitar la marca de los pixeles del propio objeto (mismo identificador) */
            glColorMask(0, 0, 0, 0);
            glUniform1i(U("u_maskonly"), 1);
            glDisable(GL_DEPTH_TEST);
            glStencilFunc(GL_EQUAL, id, 0x3F);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask(0x80);
            glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
            glEnable(GL_DEPTH_TEST);
            glUniform1i(U("u_maskonly"), 0);
            glColorMask(1, 1, 1, 1);
            /* 2) pintar la sombra donde quede marca, una sola vez por pixel (se borra la marca al pintar) */
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
            glStencilFunc(GL_EQUAL, 0x80, 0x80);
            glStencilOp(GL_KEEP, GL_KEEP, GL_ZERO);
            glStencilMask(0x80);
            glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
            shadow_drawn = 1;
            continue;
        }
        if (p->glow) { /* luces del coche: luz sumada (roja de freno, blanca de marcha atras), sin tapar el piloto */
            gpu_set_poly_state(p, ti, ds, 2, &last_ls);
            if (p->glow == 1) glUniform3f(U("u_glow"), 1.15f, 0.14f, 0.08f);
            else glUniform3f(U("u_glow"), 0.35f, 0.35f, 0.32f); /* el halo cubre toda la trasera: tenue */
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
            glDepthMask(0);
            glStencilMask(0);
            glStencilFunc(GL_ALWAYS, 0, 0);
            glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
            glUniform3f(U("u_glow"), 0, 0, 0);
            continue;
        }
        /* translucidos: primero sus pixeles opacos (como un opaco), luego los translucidos mezclados */
        if (((p->attr >> 16) & 31) == 31) {
            gpu_set_poly_state(p, ti, ds, 1, &last_ls);
            glDisable(GL_BLEND);
            glDepthMask(1);
            glStencilFunc(GL_ALWAYS, id, 0xFF);
            glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
            glStencilMask(0x3F);
            glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
        }
        gpu_set_poly_state(p, ti, ds, 2, &last_ls);
        if (ds->blend) {
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
            glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX);
        } else {
            glDisable(GL_BLEND);
        }
        glDepthMask((p->attr & 0x800) ? 1 : 0);
        /* Regla de la consola: un translucido no pinta un pixel que ya pinto otro translucido con su mismo
         * identificador (asi el humo o las particulas que se solapan no se acumulan). Se lleva con el bit 6
         * de la plantilla, que se limpia cuando cambia el identificador. */
        if (id != last_trans_id) {
            glStencilMask(0x40);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            last_trans_id = id;
        }
        glStencilFunc(GL_NOTEQUAL, 0x40, 0x40); /* pasa si el bit 6 esta a 0 */
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE); /* y al pintar lo pone a 1 */
        glStencilMask(0x40);
        glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
    }
    glDisable(GL_BLEND);
    glDepthMask(1);
    glStencilMask(0xFF);

    /* resolver el suavizado y leer el resultado */
    double t2 = gpu_now();
    gpu_t[1] += t2 - t1;
    int dst = gpu_dst >= 0 ? gpu_dst : gpu_res_cur; /* la intermedia (120 fps) va a la tercera imagen */
    LARGE_INTEGER r0, r1, rf;
    QueryPerformanceCounter(&r0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu_msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gpu_res_fbo[dst]);
    glBlitFramebuffer(0, 0, gpu_w, gpu_h, 0, 0, gpu_w, gpu_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gpu_bloom_done = 0;
    if (ds->bloom) { /* resplandor, en la imagen ya resuelta */
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_BLEND);
        glActiveTexture(GL_TEXTURE0);
        glBindSampler(0, 0);
        glViewport(0, 0, gpu_bl_w, gpu_bl_h);
        glBindFramebuffer(GL_FRAMEBUFFER, gpu_bl_fbo[0]);
        glUseProgram(gpu_bl_prog[0]);
        glBindTexture(GL_TEXTURE_2D, gpu_res_tex[dst]);
        glUniform1i(glGetUniformLocation(gpu_bl_prog[0], "u_src"), 0);
        glUniform2f(glGetUniformLocation(gpu_bl_prog[0], "u_texel"), 1.0f / gpu_w, 1.0f / gpu_h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glUseProgram(gpu_bl_prog[1]);
        glUniform1i(glGetUniformLocation(gpu_bl_prog[1], "u_src"), 0);
        for (int pass = 0; pass < 2; pass++) { /* horizontal 0->1, vertical 1->0 */
            glBindFramebuffer(GL_FRAMEBUFFER, gpu_bl_fbo[pass ^ 1]);
            glBindTexture(GL_TEXTURE_2D, gpu_bl_tex[pass]);
            glUniform2f(glGetUniformLocation(gpu_bl_prog[1], "u_step"), pass ? 0.0f : 1.0f / gpu_bl_w, pass ? 1.0f / gpu_bl_h : 0.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        /* suma solo donde hay 3D: el factor es el alfa de destino */
        glViewport(0, 0, gpu_w, gpu_h);
        glBindFramebuffer(GL_FRAMEBUFFER, gpu_res_fbo[dst]);
        glUseProgram(gpu_bl_prog[2]);
        glBindTexture(GL_TEXTURE_2D, gpu_bl_tex[0]);
        glUniform1i(glGetUniformLocation(gpu_bl_prog[2], "u_src"), 0);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
        glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisable(GL_BLEND);
        gpu_bloom_done = 1;
    }
    double t3 = t2;
    if (!win_gpu_present && gpu_dst < 0) { /* la mezcla con el 2D la hace el procesador: hay que traer la imagen */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu_res_fbo[dst]);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, gpu_w, gpu_h, GL_BGRA, GL_UNSIGNED_BYTE, gpu_read);
        t3 = gpu_now();
        gpu_t[2] += t3 - t2;
    }
    if (!win_gpu_present && gpu_dst < 0) pool_run(gpu_h, gpu_convert_row, 0); /* OpenGL empieza por abajo; se deshace el premultiplicado */
    QueryPerformanceCounter(&r1);
    QueryPerformanceFrequency(&rf);
    perf_gpu_read_ms += (double)(r1.QuadPart - r0.QuadPart) * 1000 / (double)rf.QuadPart;
    gpu_t[3] += gpu_now() - t3;
    if (gpu_dst >= 0) return; /* imagen intermedia: no cambia cual es la anterior */
    gpu_res_cur ^= 1;
    gpu_have_prev = 1;
    static int frames;
    if (getenv("NFS_GPU_LOG") && ++frames % 300 == 0) {
        extern int perf_meshes;
        fprintf(stderr, "[gpu] media de 300: vertices %.2f ms, ordenes %.2f, espera+lectura %.2f, conversion %.2f | texturas %.2f, resplandor %.2f | %d poligonos, %d piezas sustituidas%c",
                gpu_t[0] / 300, gpu_t[1] / 300, gpu_t[2] / 300, gpu_t[3] / 300, gpu_t[4] / 300, gpu_t[5] / 300, poly_count, perf_meshes, 10);
        perf_meshes = 0;
        memset(gpu_t, 0, sizeof gpu_t);
    }
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        static int reported;
        if (!reported++) fprintf(stderr, "[gpu] error de OpenGL %04x%c", err, 10);
    }
}

/* ---- 120 imagenes por segundo (NFS_FPS120, experimental) ----------------------------------------------
 * El juego hace 60 imagenes por segundo. Entre cada dos se dibuja otra intermedia: los poligonos que estan en
 * las dos (mismo material, mismo numero de vertices, en el mismo orden y cerca) se ponen a medio camino entre
 * donde estaban y donde estan; los demas (los que aparecen o se recortan distinto en el borde) se quedan como
 * en la actual. win.c ensena la intermedia y medio periodo despues la actual: 8 ms mas de retraso. */
typedef struct PrevPoly {
    uint32_t attr, tex, pal;
    int n;
    float cx, cy;
    float pos[10][4];
} PrevPoly;
static PrevPoly *gpu_prev_polys;
static int gpu_prev_count, gpu_prev_cap, gpu_interp_ready, gpu_present_interp;
int gpu_interp_pending(void) { return gpu_interp_ready; }
void gpu_set_present_interp(int on) { gpu_present_interp = on; }
int video_fps120(void);

static void poly_center(const Poly *p, float *cx, float *cy) {
    float x = 0, y = 0;
    for (int k = 0; k < p->n; k++) { x += p->v[k].x; y += p->v[k].y; }
    *cx = x / p->n; *cy = y / p->n;
}

/* Desplazamiento de cada vertice (de la imagen actual a la anterior), por su posicion: los poligonos vecinos
 * comparten vertices, asi se mueven juntos y no se abren rendijas entre los emparejados y los que no */
#define VMAP_BITS 18
typedef struct VDelta { int32_t kx, ky, kw; uint32_t gen; int has; float d[4]; } VDelta;
static VDelta *gpu_vmap;
static uint32_t gpu_vmap_gen; /* entradas de otra imagen = libres (sin vaciar la tabla cada vez) */
static uint32_t vkey_hash(int32_t a, int32_t b, int32_t c) {
    uint32_t h = (uint32_t)a * 73856093u ^ (uint32_t)b * 19349663u ^ (uint32_t)c * 83492791u;
    return h & ((1u << VMAP_BITS) - 1);
}
static void vkey(const Vtx *v, int32_t *a, int32_t *b, int32_t *c) {
    *a = (int32_t)lrintf(v->x * 64); *b = (int32_t)lrintf(v->y * 64); *c = (int32_t)lrintf(v->w * 4096);
}
static VDelta *vmap_find(const Vtx *v, int insert) {
    int32_t a, b, c;
    vkey(v, &a, &b, &c);
    for (uint32_t h = vkey_hash(a, b, c), n = 0; n < 64; h = (h + 1) & ((1u << VMAP_BITS) - 1), n++) {
        VDelta *e = &gpu_vmap[h];
        if (e->gen != gpu_vmap_gen) {
            if (!insert) return 0;
            e->gen = gpu_vmap_gen; e->has = 0; e->kx = a; e->ky = b; e->kw = c;
            return e;
        }
        if (e->kx == a && e->ky == b && e->kw == c) return e;
    }
    return 0;
}

static double gpu_interp_t[3]; /* emparejar, dibujar, guardar (ms acumulados, para NFS_FPS120_LOG) */
static void gpu_interp_frame(void) {
    double ti0 = gpu_now();
    gpu_interp_ready = 0;
    if (!video_fps120() || !win_gpu_present) { gpu_prev_count = 0; return; }
    static float (*saved)[10][4];
    static int *match, saved_cap;
    if (saved_cap < poly_count) {
        saved_cap = poly_count + 4096;
        saved = realloc(saved, sizeof(*saved) * saved_cap);
        match = realloc(match, sizeof(int) * saved_cap);
    }
    if (!gpu_vmap) gpu_vmap = calloc((size_t)1 << VMAP_BITS, sizeof(VDelta));
    int matched = 0;
    if (gpu_prev_count) { /* emparejar con la imagen anterior: en orden, el mas parecido de los cercanos */
        int j = 0;
        for (int i = 0; i < poly_count; i++) {
            const Poly *p = &polys[i];
            match[i] = -1;
            float best = 12; /* como mucho 12 pixeles de la consola por vertice */
            int lo = j - 8 < 0 ? 0 : j - 8;
            for (int k = lo; k < gpu_prev_count && k < j + 64; k++) {
                const PrevPoly *q = &gpu_prev_polys[k];
                if (q->attr != p->attr || q->tex != p->tex || q->pal != p->pal || q->n != p->n) continue;
                float worst = 0;
                for (int v = 0; v < p->n && worst < best; v++) {
                    float d = fabsf(q->pos[v][0] - p->v[v].x) + fabsf(q->pos[v][1] - p->v[v].y);
                    if (d > worst) worst = d;
                }
                if (worst < best) { best = worst; match[i] = k; }
            }
            if (match[i] >= 0) { j = match[i] + 1; matched++; }
        }
        gpu_interp_t[0] += gpu_now() - ti0;
        if (matched > poly_count / 4) { /* hay bastante en comun (no es un cambio de escena) */
            gpu_vmap_gen++;
            for (int i = 0; i < poly_count; i++) { /* desplazamientos de los vertices de los emparejados */
                if (match[i] < 0) continue;
                const Poly *p = &polys[i];
                const PrevPoly *q = &gpu_prev_polys[match[i]];
                for (int k = 0; k < p->n; k++) {
                    VDelta *e = vmap_find(&p->v[k], 1);
                    if (e && !e->has) {
                        e->has = 1;
                        e->d[0] = q->pos[k][0] - p->v[k].x; e->d[1] = q->pos[k][1] - p->v[k].y;
                        e->d[2] = q->pos[k][2] - p->v[k].z; e->d[3] = q->pos[k][3] - p->v[k].w;
                    }
                }
            }
            for (int i = 0; i < poly_count; i++) {
                Poly *p = &polys[i];
                float d[10][4], avg[4] = {0, 0, 0, 0};
                int have[10], nh = 0;
                for (int k = 0; k < p->n; k++) {
                    saved[i][k][0] = p->v[k].x; saved[i][k][1] = p->v[k].y; saved[i][k][2] = p->v[k].z; saved[i][k][3] = p->v[k].w;
                    const VDelta *e = vmap_find(&p->v[k], 0);
                    have[k] = e && e->has;
                    if (have[k]) { memcpy(d[k], e->d, sizeof d[k]); for (int c = 0; c < 4; c++) avg[c] += e->d[c]; nh++; }
                    else if (match[i] >= 0) {
                        const PrevPoly *q = &gpu_prev_polys[match[i]];
                        d[k][0] = q->pos[k][0] - p->v[k].x; d[k][1] = q->pos[k][1] - p->v[k].y;
                        d[k][2] = q->pos[k][2] - p->v[k].z; d[k][3] = q->pos[k][3] - p->v[k].w;
                        have[k] = 1;
                    }
                }
                if (nh) for (int c = 0; c < 4; c++) avg[c] /= nh;
                for (int k = 0; k < p->n; k++) { /* sin dato propio: lo que se mueven sus vecinos del mismo poligono */
                    const float *dd = have[k] ? d[k] : avg;
                    p->v[k].x += dd[0] * 0.5f; p->v[k].y += dd[1] * 0.5f; p->v[k].z += dd[2] * 0.5f; p->v[k].w += dd[3] * 0.5f;
                }
            }
            double ti1 = gpu_now();
            gpu_dst = 2;
            gpu_render_frame();
            gpu_dst = -1;
            gpu_interp_t[1] += gpu_now() - ti1;
            for (int i = 0; i < poly_count; i++) {
                Poly *p = &polys[i];
                for (int k = 0; k < p->n; k++) {
                    p->v[k].x = saved[i][k][0]; p->v[k].y = saved[i][k][1]; p->v[k].z = saved[i][k][2]; p->v[k].w = saved[i][k][3];
                }
            }
            gpu_interp_ready = 1;
        }
    }
    if (getenv("NFS_FPS120_LOG") && g_frame % 120 == 0) {
        fprintf(stderr, "[120] fotograma %u: %d de %d poligonos emparejados%s | ms por imagen: emparejar+mover %.2f, dibujar %.2f%c", g_frame, matched, poly_count,
                gpu_interp_ready ? "" : " (sin imagen intermedia)", gpu_interp_t[0] / 120, gpu_interp_t[1] / 120, 10);
        gpu_interp_t[0] = gpu_interp_t[1] = 0;
    }
    /* la actual pasa a ser la anterior */
    if (gpu_prev_cap < poly_count) {
        gpu_prev_cap = poly_count + 4096;
        gpu_prev_polys = realloc(gpu_prev_polys, sizeof(PrevPoly) * gpu_prev_cap);
    }
    for (int i = 0; i < poly_count; i++) {
        const Poly *p = &polys[i];
        PrevPoly *q = &gpu_prev_polys[i];
        q->attr = p->attr; q->tex = p->tex; q->pal = p->pal; q->n = p->n;
        poly_center(p, &q->cx, &q->cy);
        for (int k = 0; k < p->n; k++) { q->pos[k][0] = p->v[k].x; q->pos[k][1] = p->v[k].y; q->pos[k][2] = p->v[k].z; q->pos[k][3] = p->v[k].w; }
    }
    gpu_prev_count = poly_count;
}

/* 1 si se ha dibujado con la grafica; 0 = usar el dibujado por software */
static int gpu_render(void) {
    gpu_bloom_done = 0;
    if (!video_gpu()) return 0;
    if (gpu_state == 0) {
        gpu_state = gpu_init() ? 1 : -1;
        if (gpu_state < 0) fprintf(stderr, "[gpu] no se puede usar la grafica: dibujado por software%c", 10);
    }
    if (gpu_state < 0) return 0;
    wglMakeCurrent(gpu_dc, gpu_ctx);
    gpu_render_frame();
    gpu_interp_frame();
    return 1;
}

/* Pinta las dos pantallas en la ventana del juego: mezcla del 2D con el 3D en la grafica. 0 = no se pudo
 * (no hay grafica): entonces la ventana la pinta el procesador como antes. */
int gpu_present(const GpuScreen *top, const GpuScreen *bottom, void *hwnd, int win_w, int win_h) {
    if (gpu_state != 1 || !hwnd) return 0;
    if (!gpu_win_dc) { /* la ventana del juego, con el mismo formato de pixel que el contexto */
        HDC dc = GetDC((HWND)hwnd);
        PIXELFORMATDESCRIPTOR pfd = {sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
        if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd) || !wglMakeCurrent(dc, gpu_ctx)) {
            fprintf(stderr, "[gpu] no se puede pintar en la ventana: la pinta el procesador%c", 10);
            gpu_state = -1;
            return 0;
        }
        gpu_win_dc = dc;
        gpu_dc = dc;
        PFNWGLSWAPINTERVALEXTPROC swap = (PFNWGLSWAPINTERVALEXTPROC)wglGetProcAddress("wglSwapIntervalEXT");
        if (swap) swap(0); /* el ritmo de 60 lo lleva win.c */
        for (int k = 0; k < 2; k++)
            for (int t = 0; t < 4; t++) {
                glGenTextures(1, &gpu_scr_tex[k][t]);
                glBindTexture(GL_TEXTURE_2D, gpu_scr_tex[k][t]);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 192, 0, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            }
        fprintf(stderr, "[gpu] la ventana la pinta la grafica%c", 10);
    }
    wglMakeCurrent(gpu_win_dc, gpu_ctx);
    /* el 3D recien dibujado; si se reduce (resolucion interna mayor que la de la ventana), con reducciones */
    GLuint t3d = gpu_present_interp && gpu_interp_ready ? gpu_res_tex[2] : gpu_res_tex[gpu_res_cur ^ 1];
    glBindTexture(GL_TEXTURE_2D, t3d);
    int reduce = gpu_w > top->w || gpu_h / 2 > top->h;
    if (reduce) glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, reduce ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_MULTISAMPLE);
    glColorMask(1, 1, 1, 1);
    glViewport(0, 0, win_w, win_h);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(gpu_comp_prog);
    static const char *names[5] = {"t_back", "t_front", "t_raw", "t_ids", "t_3d"};
    for (int t = 0; t < 5; t++) glUniform1i(glGetUniformLocation(gpu_comp_prog, names[t]), t);
    glBindVertexArray(gpu_vao);
    const GpuScreen *scr[2] = {top, bottom};
    for (int k = 0; k < 2; k++) {
        const GpuScreen *g = scr[k];
        if (g->w <= 0 || g->h <= 0) continue; /* pantalla oculta (disposicion "solo la del juego") */
        const uint32_t *src[4] = {g->back, g->front, g->raw, g->ids};
        for (int t = 0; t < 4; t++) {
            glActiveTexture(GL_TEXTURE0 + t);
            glBindSampler(t, 0);
            glBindTexture(GL_TEXTURE_2D, gpu_scr_tex[k][t]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_BGRA, GL_UNSIGNED_BYTE, src[t]);
        }
        glActiveTexture(GL_TEXTURE4);
        glBindSampler(4, 0);
        glBindTexture(GL_TEXTURE_2D, t3d);
        glUniform1i(glGetUniformLocation(gpu_comp_prog, "u_use3d"), g->use3d && gpu_have_prev);
        glUniform1i(glGetUniformLocation(gpu_comp_prog, "u_fx3d"), g->fx3d);
        glUniform1i(glGetUniformLocation(gpu_comp_prog, "u_cnt"), g->cnt);
        glUniform1f(glGetUniformLocation(gpu_comp_prog, "u_eva"), (float)g->eva);
        glUniform1f(glGetUniformLocation(gpu_comp_prog, "u_evb"), (float)g->evb);
        glUniform1i(glGetUniformLocation(gpu_comp_prog, "u_bmode"), g->bright_factor ? g->bright_mode : 0);
        glUniform1f(glGetUniformLocation(gpu_comp_prog, "u_bfac"), g->bright_factor / 16.0f);
        glUniform1i(glGetUniformLocation(gpu_comp_prog, "u_smooth"), video_smooth2d() && g->w > 256); /* a x1 no hace falta */
        glViewport(g->x, win_h - g->y - g->h, g->w, g->h);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glActiveTexture(GL_TEXTURE0);
    { /* NFS_WINSHOT_EVERY=n (NFS_WINSHOT_FROM / _TO): guarda lo que se pinta en la ventana, para comprobar el
       * camino de la grafica (las capturas NFS_SHOT_* salen del dibujado por procesador) */
        static int every = -1, from, to;
        if (every < 0) {
            every = getenv("NFS_WINSHOT_EVERY") ? atoi(getenv("NFS_WINSHOT_EVERY")) : 0;
            from = getenv("NFS_WINSHOT_FROM") ? atoi(getenv("NFS_WINSHOT_FROM")) : 0;
            to = getenv("NFS_WINSHOT_TO") ? atoi(getenv("NFS_WINSHOT_TO")) : 0x7FFFFFFF;
        }
        if (every > 0 && (int)g_frame >= from && (int)g_frame <= to && g_frame % (uint32_t)every == 0) {
            uint32_t *px = malloc((size_t)win_w * win_h * 4), *fl = malloc((size_t)win_w * win_h * 4);
            glReadBuffer(GL_BACK);
            glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_BYTE, px);
            for (int y = 0; y < win_h; y++) memcpy(fl + (size_t)y * win_w, px + (size_t)(win_h - 1 - y) * win_w, (size_t)win_w * 4);
            char name[64];
            snprintf(name, sizeof name, "win_%05u%s.png", g_frame, gpu_present_interp && gpu_interp_ready ? "_i" : "");
            void video_save_png(const char *path, const uint32_t *pixels, int w, int h);
            video_save_png(name, fl, win_w, win_h);
            free(px);
            free(fl);
        }
    }
    SwapBuffers(gpu_win_dc);
    return 1;
}
