// platform/native/video.c
//
// SDL2 window, OpenGL presentation, and keyboard input.
//
// The PPU writes a 240x160 RGBA framebuffer (ppu.c); this uploads it as a GL
// texture and draws it with a single textured quad scaled to the window with
// integer-friendly filtering. Keyboard state is published to the emulated
// REG_KEYINPUT as an active-low mask, which is all the game ever reads.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "global.h"
#include "native.h"

// GLEW, not <GL/gl.h>: the system header only declares OpenGL 1.1, so every
// shader entry point (glCreateShader and friends) comes out as an implicit
// declaration -- a warning on older GCC and a hard error from GCC 14 onward.
// GLEW exposes the modern entry points and resolves them at run time, which also
// copes with drivers that only offer a compatibility profile.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>
#include <string.h>

static SDL_Window *s_window;
static SDL_GLContext s_gl;
static GLuint s_texture;
static GLuint s_program;
static GLint s_uniform_tex;
static GLuint s_vao;
static bool s_glew_ok;

static uint16_t s_keyinput = 0x03FF; // active low: a set bit means released
static uint16_t s_injected;          // bits forced by native_input_inject

// Used by the headless capture path to simulate a held button.
uint16_t native_input_set_bits(uint16_t bits)
{
    s_injected = bits;
    return s_keyinput;
}

static const char *kVertexShader =
    "#version 130\n"
    "in vec2 aPos;\n"
    "in vec2 aUV;\n"
    "out vec2 vUV;\n"
    "void main() {\n"
    "    vUV = aUV;\n"
    "    gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "}\n";

static const char *kFragmentShader =
    "#version 130\n"
    "in vec2 vUV;\n"
    "out vec4 fragColor;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "    fragColor = texture(uTex, vUV);\n"
    "}\n";

static GLuint CompileShader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        native_log("shader compile failed: %s", log);
    }
    return s;
}

bool native_video_init(int scale)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        native_log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // Try a core profile first, then fall back. Remote displays (x2go, VNC) and
    // some drivers expose only compatibility contexts, and asking for core
    // there fails outright with "Couldn't find matching GLX visual".
    //
    // The shaders are written for GLSL 130, which is what both a 3.3 core
    // context and a 3.0+ compatibility context accept.
    static const struct
    {
        int major, minor, profile;
    } kProfiles[] = {
        {3, 3, SDL_GL_CONTEXT_PROFILE_CORE},
        {3, 0, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY},
        {2, 1, 0}, // last resort: GLSL 120, so rewrite the shaders
    };

    s_window = NULL;
    for (size_t i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kProfiles[i].major);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kProfiles[i].minor);
        if (kProfiles[i].profile)
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, kProfiles[i].profile);
        else
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);

        s_window = SDL_CreateWindow("pokeemerald (native)", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, DISPLAY_WIDTH * scale,
                                    DISPLAY_HEIGHT * scale,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (s_window)
            break;
    }
    if (!s_window)
    {
        native_log("SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    s_gl = SDL_GL_CreateContext(s_window);
    if (!s_gl)
    {
        native_log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    // GLEW has to load the entry points itself, and that requires a current
    // context, so it happens after SDL_GL_CreateContext.
    // Not fatal: the offscreen/dummy drivers used for headless frame capture
    // expose no usable GL, and those paths never draw through the shaders.
    glewExperimental = GL_TRUE;
    GLenum glewStatus = glewInit();
    if (glewStatus != GLEW_OK)
        native_log("glewInit failed (%s); continuing",
                   (const char *)glewGetErrorString(glewStatus));
    else
        s_glew_ok = true;
    // GLEW_ERROR_CHECKING is mutually exclusive with core profiles; clear the
    // spurious error it can leave behind on a compatibility context.
    glGetError();

    SDL_GL_SetSwapInterval(1); // vsync

    // Publish the initial key state before anything else can return. The game
    // polls REG_KEYINPUT during boot (the A+B+Start+Select soft-reset check runs
    // in the main loop) and would otherwise read 0, which is "every key held".
    *(volatile uint16_t *)(NATIVE_IO + REG_OFFSET_KEYINPUT) = s_keyinput;

    // Without GLEW there are no resolved shader entry points, so there is
    // nothing to draw with. Leave the context alone and let the caller run
    // frame-capture only; this is the headless/offscreen case.
    if (!s_glew_ok)
    {
        native_log("GL unavailable; running without presentation");
        return true;
    }

    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    s_program = glCreateProgram();
    glAttachShader(s_program, vs);
    glAttachShader(s_program, fs);
    glLinkProgram(s_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glUseProgram(s_program);

    s_uniform_tex = glGetUniformLocation(s_program, "uTex");

    // Two triangles covering clip space, with texture coordinates.
    static const float kVerts[] = {
        // x     y     u    v
        -1.0f, -1.0f, 0.0f, 1.0f,
        1.0f,  -1.0f, 1.0f, 1.0f,
        -1.0f, 1.0f, 0.0f, 0.0f,
        1.0f,  -1.0f, 1.0f, 1.0f,
        1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f, 1.0f, 0.0f, 0.0f,
    };

    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);

    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVerts), kVerts, GL_STATIC_DRAW);

    GLint aPos = glGetAttribLocation(s_program, "aPos");
    GLint aUV = glGetAttribLocation(s_program, "aUV");
    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(aUV);
    glVertexAttribPointer(aUV, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void *)(2 * sizeof(float)));

    // The framebuffer texture.
    glGenTextures(1, &s_texture);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // Seed with an opaque black image so the first frame is well defined.
    uint32_t *blank = calloc(DISPLAY_WIDTH * DISPLAY_HEIGHT, sizeof(uint32_t));
    for (int i = 0; i < DISPLAY_WIDTH * DISPLAY_HEIGHT; i++)
        blank[i] = 0xFF000000u;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, blank);
    free(blank);

    native_log("video: %dx%d window, GL %s", DISPLAY_WIDTH * scale, DISPLAY_HEIGHT * scale,
               (const char *)glGetString(GL_VERSION));

}

void native_video_render(void)
{
    if (!s_glew_ok)
        return;

    glBindTexture(GL_TEXTURE_2D, s_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA,
                    GL_UNSIGNED_BYTE, native_ppu_framebuffer());

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glUniform1i(s_uniform_tex, 0);
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    SDL_GL_SwapWindow(s_window);
}

void native_video_shutdown(void)
{
    if (s_gl)
        SDL_GL_DeleteContext(s_gl);
    if (s_window)
        SDL_DestroyWindow(s_window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

static uint16_t TranslateScancode(SDL_Scancode sc)
{
    switch (sc)
    {
    case SDL_SCANCODE_Z: return A_BUTTON;
    case SDL_SCANCODE_X: return B_BUTTON;
    case SDL_SCANCODE_BACKSPACE: return SELECT_BUTTON;
    case SDL_SCANCODE_RETURN: return START_BUTTON;
    case SDL_SCANCODE_RIGHT: return DPAD_RIGHT;
    case SDL_SCANCODE_LEFT: return DPAD_LEFT;
    case SDL_SCANCODE_UP: return DPAD_UP;
    case SDL_SCANCODE_DOWN: return DPAD_DOWN;
    case SDL_SCANCODE_S: return R_BUTTON;
    case SDL_SCANCODE_A: return L_BUTTON;
    default: return 0;
    }
}

// Pump the SDL event queue and refresh REG_KEYINPUT.
//
// Returns false if the user asked to close the window.
bool native_input_poll(void)
{
    SDL_Event ev;
    bool running = true;

    while (SDL_PollEvent(&ev))
    {
        switch (ev.type)
        {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
        {
            if (ev.key.repeat)
                break;
            uint16_t bit = TranslateScancode(ev.key.keysym.scancode);
            if (bit)
            {
                if (ev.type == SDL_KEYDOWN)
                    s_keyinput &= (uint16_t)~bit;
                else
                    s_keyinput |= bit;
            }
            break;
        }
        default:
            break;
        }
    }

    // REG_KEYINPUT is active low: a set bit means the key is up. Injected bits
    // override the real keyboard so a headless run can drive the game.
    uint16_t state = s_keyinput & ~s_injected;
    *(volatile uint16_t *)(NATIVE_IO + REG_OFFSET_KEYINPUT) = state;

    // Toggle the debug menu.
    extern bool native_debug_menu_toggle_requested(void);
    native_debug_menu_toggle_requested();

    return running;
}