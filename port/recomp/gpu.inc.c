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
    X(PFNGLBLENDEQUATIONSEPARATEPROC, glBlendEquationSeparate)

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

#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#endif

static int gpu_state = 0; /* 0 sin probar, 1 funcionando, -1 no disponible */
int gpu_bloom_done;        /* este fotograma el resplandor ya lo hizo la grafica */
static GLuint gpu_bl_prog[3], gpu_bl_fbo[2], gpu_bl_tex[2]; /* resplandor: brillo, difuminado, suma */
static int gpu_bl_w, gpu_bl_h;
static HDC gpu_dc;
static HGLRC gpu_ctx;
static GLuint gpu_prog, gpu_vao, gpu_vbo, gpu_msaa_fbo, gpu_msaa_color, gpu_msaa_depth;
static GLuint gpu_res_fbo[2], gpu_res_tex[2], gpu_samplers[2][3][3];
static int gpu_res_cur, gpu_samples, gpu_w, gpu_h, gpu_have_prev;
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
    "uniform int u_maskonly;\n"
    "uniform int u_fog; uniform int u_fog_alpha_only; uniform vec3 u_fog_rgb; uniform float u_fog_a; uniform float u_fog_offset;\n"
    "uniform float u_fog_step; uniform float u_fog_table[32];\n"
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
    "  vec3 acc = vec3(0.0);\n"
    "  for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {\n"
    "    vec4 c = texture(u_src, v_p + (vec2(i, j) - 1.5) * u_texel);\n"
    "    if (c.a <= 0.0) continue;\n"
    "    vec3 rgb = c.rgb / c.a; float l = dot(rgb, vec3(0.3, 0.59, 0.11)); float k = (l - 0.84) / 0.16;\n"
    "    if (k > 0.0) acc += rgb * k;\n"
    "  }\n"
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
    for (int i = 0; i < 2; i++) { /* dos imagenes resueltas que se alternan: la anterior se refleja en la pintura */
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
                glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, f ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
                glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, f ? GL_LINEAR : GL_NEAREST);
                if (f) glSamplerParameterf(sm, GL_TEXTURE_MAX_ANISOTROPY, max_aniso > 8 ? 8 : max_aniso);
                glSamplerParameterf(sm, GL_TEXTURE_LOD_BIAS, f ? -0.4f : 0.0f);
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

    int last_ls = -1, shadow_drawn = 0;
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
        glStencilMask(0);
        glStencilFunc(GL_ALWAYS, 0, 0);
        glDrawArrays(GL_TRIANGLES, bt->first, bt->count);
    }
    glDisable(GL_BLEND);
    glDepthMask(1);
    glStencilMask(0xFF);

    /* resolver el suavizado y leer el resultado */
    double t2 = gpu_now();
    gpu_t[1] += t2 - t1;
    LARGE_INTEGER r0, r1, rf;
    QueryPerformanceCounter(&r0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu_msaa_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, gpu_res_fbo[gpu_res_cur]);
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
        glBindTexture(GL_TEXTURE_2D, gpu_res_tex[gpu_res_cur]);
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
        glBindFramebuffer(GL_FRAMEBUFFER, gpu_res_fbo[gpu_res_cur]);
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
    glBindFramebuffer(GL_READ_FRAMEBUFFER, gpu_res_fbo[gpu_res_cur]);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, gpu_w, gpu_h, GL_BGRA, GL_UNSIGNED_BYTE, gpu_read);
    double t3 = gpu_now();
    gpu_t[2] += t3 - t2;
    pool_run(gpu_h, gpu_convert_row, 0); /* OpenGL empieza por abajo; ademas se deshace el premultiplicado */
    QueryPerformanceCounter(&r1);
    QueryPerformanceFrequency(&rf);
    perf_gpu_read_ms += (double)(r1.QuadPart - r0.QuadPart) * 1000 / (double)rf.QuadPart;
    gpu_t[3] += gpu_now() - t3;
    gpu_res_cur ^= 1;
    gpu_have_prev = 1;
    static int frames;
    if (getenv("NFS_GPU_LOG") && ++frames % 300 == 0) {
        fprintf(stderr, "[gpu] media de 300: vertices %.2f ms, ordenes %.2f, espera+lectura %.2f, conversion %.2f | texturas %.2f, resplandor %.2f | %d poligonos%c",
                gpu_t[0] / 300, gpu_t[1] / 300, gpu_t[2] / 300, gpu_t[3] / 300, gpu_t[4] / 300, gpu_t[5] / 300, poly_count, 10);
        memset(gpu_t, 0, sizeof gpu_t);
    }
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        static int reported;
        if (!reported++) fprintf(stderr, "[gpu] error de OpenGL %04x%c", err, 10);
    }
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
    return 1;
}
