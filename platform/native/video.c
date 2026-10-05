// platform/native/video.c
//
// SDL2 window, OpenGL presentation, keyboard input, and the present thread.
//
// The PPU writes a 240x160 RGBA framebuffer (ppu.c); this uploads it as a GL
// texture and draws it with a single textured quad scaled to the window with
// integer-friendly filtering. Keyboard state is published to the emulated
// REG_KEYINPUT as an active-low mask, which is all the game ever reads.
//
// WHY THERE IS A PRESENT THREAD
//
// This used to all run on the game's thread, inside the VBlank interrupt
// handler, which itself runs inside the game's WaitForVBlank spin:
//
//     WaitForVBlank -> native_timing_run_interrupt -> VBlankIntr
//                                    -> native_ppu_render_frame
//                                    -> native_video_render -> SDL_GL_SwapWindow
//
// SDL_GL_SwapWindow blocks on vsync, so the game's VBlank servicing -- and
// with it the whole frame -- was gated on the display's refresh. That is also
// why there was nowhere off the interrupt path to run an ImGui overlay:
// every point on the game's thread is either inside the handler or inside the
// main loop that spins waiting for it.
//
// The split now is:
//
//   game's thread   rasterises the frame (ppu.c) and calls
//                   native_video_publish_frame(), which memcpy's the result
//                   into a triple buffer, publishes the index with a release
//                   store, and returns. It never blocks on the display.
//   present thread  owns the GL context. It waits for a published frame,
//                   uploads it, draws the quad, pumps the debug overlay, and
//                   swaps. The vsync wait happens here, where it costs the
//                   game nothing.
//
// The handoff is a triple buffer rather than a lock or a single buffer: the
// game must never wait for the presenter (it would reintroduce the coupling
// this removes) and the presenter must never read a frame the game is still
// writing. With three slots the game can be two frames ahead before it would
// have to skip, and a skip just means the presenter repeats the previous
// frame.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "global.h"
#include "native.h"
#include "debug_menu.h"

// GLEW, not <GL/gl.h>: the system header only declares OpenGL 1.1, so every
// shader entry point (glCreateShader and friends) comes out as an implicit
// declaration -- a warning on older GCC and a hard error from GCC 14 onward.
// GLEW exposes the modern entry points and resolves them at run time, which also
// copes with drivers that only offer a compatibility profile.
#define GL_GLEXT_PROTOTYPES 1
#include <GL/glew.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

static SDL_Window *s_window;
static SDL_GLContext s_gl;
static GLuint s_texture;
static GLuint s_program;
static GLint s_uniform_tex;
static GLuint s_vao;
static bool s_glew_ok;

static uint16_t s_keyinput = 0x03FF; // active low: a set bit means released
static uint16_t s_injected;          // bits forced by native_input_set_bits

// Defined below: the present thread takes the GL context over from this one.
static void start_present_thread(void);

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

    SDL_GL_SetSwapInterval(1); // vsync, paid on the present thread only

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
        -1.0f, 1.0f,  0.0f, 0.0f,
        1.0f,  -1.0f, 1.0f, 1.0f,
        1.0f,  1.0f,  1.0f, 0.0f,
        -1.0f, 1.0f,  0.0f, 0.0f,
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

    start_present_thread();

    return true;
}

SDL_Window *native_video_window(void)
{
    return s_window;
}

void *native_video_gl_context(void)
{
    return (void *)s_gl;
}

// ---------------------------------------------------------------------------
// Frame handoff
// ---------------------------------------------------------------------------
//
// Triple-buffered and lock-free. The game writes the slot at s_write_index %
// 3 and publishes s_write_index; the presenter reads whatever the newest
// published index is and marks that slot busy by advancing s_read_index.
//
// The presenter's index only ever moves forward to the newest published
// frame, so it never reads a slot the game is writing. If the game gets more
// than two frames ahead -- which only happens if the presenter is blocked for
// longer than two frames, e.g. a resize -- publish_frame skips the publish
// rather than overwriting a slot in flight. Dropping a frame is invisible;
// corrupting one is not.

#define FRAME_SLOTS 3

static uint32_t s_frames[FRAME_SLOTS][DISPLAY_HEIGHT][DISPLAY_WIDTH];
static volatile uint32_t s_write_index; // game writes, presenter reads
static volatile uint32_t s_read_index;  // presenter writes, game reads

// Set once the presenter has taken the context current, so init does not race.
static volatile bool s_present_ready;
static volatile bool s_present_stop;

void native_video_publish_frame(void)
{
    if (!s_glew_ok)
        return;

    uint32_t write = __atomic_load_n(&s_write_index, __ATOMIC_ACQUIRE);
    uint32_t read = __atomic_load_n(&s_read_index, __ATOMIC_ACQUIRE);

    // Never write into a slot the presenter has not finished with.
    if ((write - read) >= FRAME_SLOTS)
        return;

    memcpy(s_frames[write % FRAME_SLOTS], native_ppu_framebuffer(),
           sizeof(s_frames[0]));

    __atomic_store_n(&s_write_index, write + 1, __ATOMIC_RELEASE);
}

uint16_t native_video_last_keyinput(void)
{
    return s_keyinput;
}

// ---------------------------------------------------------------------------
// Present thread
// ---------------------------------------------------------------------------

static void PresentFrame(const uint32_t *pixels)
{
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, GL_RGBA,
                    GL_UNSIGNED_BYTE, pixels);

    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(s_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_texture);
    glUniform1i(s_uniform_tex, 0);
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void *present_main(void *arg)
{
    (void)arg;

    // The context was created on the game's thread; make it current here and
    // nowhere else. From here on this thread owns all GL state.
    if (SDL_GL_MakeCurrent(s_window, s_gl) != 0)
        native_log("present: SDL_GL_MakeCurrent failed: %s", SDL_GetError());
    native_log("present: thread up, GL_VERSION=%s",
               (const char *)glGetString(GL_VERSION));

    native_debug_menu_init();

    __atomic_store_n(&s_present_ready, true, __ATOMIC_RELEASE);

    uint32_t last_presented = 0;
    uint64_t fps_window_start = native_debug_now_us();
    uint32_t fps_frames = 0;
    float fps = 0.0f;

    // The overlay has to run even when the game has published nothing new --
    // otherwise a paused game would freeze the menu too, and the toggle would
    // appear not to work. Presenting the previous frame again is free.
    while (!__atomic_load_n(&s_present_stop, __ATOMIC_ACQUIRE))
    {
        // Feed the overlay the events the game's thread dequeued. This has to
        // happen before ImGui_ImplSDL2_NewFrame, which is where ImGui turns
        // its event queue into input state.
        native_video_drain_events();

        uint32_t write = __atomic_load_n(&s_write_index, __ATOMIC_ACQUIRE);
        const uint32_t *pixels = NULL;

        if (write != last_presented)
        {
            // Claim every frame up to the newest: the presenter is allowed to
            // skip intermediate ones (that is the point of the triple buffer),
            // but it must publish the slot it read so the game knows it is free.
            uint32_t newest = write - 1;
            __atomic_store_n(&s_read_index, newest + 1, __ATOMIC_RELEASE);
            pixels = &s_frames[newest % FRAME_SLOTS][0][0];
            last_presented = write;
        }

        if (pixels)
            PresentFrame(pixels);

        // The overlay runs on this thread, after the game's GL work for the
        // frame and before the swap, so what it draws lands on this frame's
        // back buffer rather than a frame later. It is handed `pixels` rather
        // than re-reading the PPU's framebuffer, which the game's thread is
        // already rewriting.
        native_debug_menu_render(pixels);

        SDL_GL_SwapWindow(s_window);

        fps_frames++;
        uint64_t now = native_debug_now_us();
        if (now - fps_window_start >= 1000000)
        {
            fps = (float)fps_frames * 1000000.0f / (float)(now - fps_window_start);
            fps_frames = 0;
            fps_window_start = now;
        }
        native_debug_menu_note_frame(fps);

        if (!pixels)
        {
            // Nothing new to show. Sleep rather than spinning, but stay
            // responsive to a toggle: 4 ms bounds the input latency and costs
            // nothing at 60 Hz.
            struct timespec ts = {0, 4000000L};
            nanosleep(&ts, NULL);
        }
    }

    native_debug_menu_shutdown();
    SDL_GL_MakeCurrent(s_window, NULL);

    return NULL;
}

static pthread_t s_present_thread;
static bool s_present_running;

static void start_present_thread(void)
{
    if (s_present_running || !s_glew_ok)
        return;

    __atomic_store_n(&s_present_stop, false, __ATOMIC_RELEASE);

    // Hand the context over rather than sharing it. SDL refuses to make a
    // context current on a second thread while it is still current on the
    // first -- "BadAccess: attempt to access private resource denied" -- so
    // release it here first. This thread must not touch GL again afterwards,
    // which is exactly the split the present thread exists to create.
    SDL_GL_MakeCurrent(s_window, NULL);

    if (pthread_create(&s_present_thread, NULL, present_main, NULL) != 0)
    {
        native_log("warning: could not start present thread; frames will not be drawn");
        // Put the context back so the caller can still shut it down cleanly.
        SDL_GL_MakeCurrent(s_window, s_gl);
        return;
    }

    s_present_running = true;

    // Wait for it to take the context current, so ImGui's init (which needs a
    // current context) cannot race with the game's own first frame.
    while (!__atomic_load_n(&s_present_ready, __ATOMIC_ACQUIRE))
    {
        struct timespec ts = {0, 1000000L};
        nanosleep(&ts, NULL);
    }
}

void native_video_shutdown(void)
{
    if (s_present_running)
    {
        __atomic_store_n(&s_present_stop, true, __ATOMIC_RELEASE);
        pthread_join(s_present_thread, NULL);
        s_present_running = false;
    }

    if (s_gl)
        SDL_GL_DeleteContext(s_gl);
    if (s_window)
        SDL_DestroyWindow(s_window);
    SDL_Quit();
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
//
// Events are still pumped on the game's thread, inside the VBlank handler,
// exactly as before. That is deliberate and must not move: the game reads
// REG_KEYINPUT at a specific point in the frame, and the port's memory notes
// record that polling it any earlier or later drops presses (a level that is
// never sampled never becomes newKeys, and one frame can fall between two
// ReadKeys calls).
//
// What moved is ImGui. The overlay runs on the present thread, and ImGui
// contexts are not thread-safe, so the events this thread dequeues are copied
// into a lock-free ring that the present thread drains and hands to
// ImGui_ImplSDL2_ProcessEvent. That is the pattern the SDL2 backend is
// designed for (it also means the overlay sees every event, not just the ones
// the game binds a key to).
//
// The one SDL call the game's thread makes that the present thread does not is
// SDL_GetModState, for the Ctrl+Shift+D binding. That reads state that only
// this thread's own SDL_PollEvent mutates, so it is not a cross-thread race.
//
// The toggle itself is forwarded the same way an event is: video.c raises a
// flag here and debug_menu_ui.cpp consumes it, because the window that would
// act on it lives on the present thread.

#define EVENT_QUEUE_SIZE 64
#define EVENT_QUEUE_MASK (EVENT_QUEUE_SIZE - 1)

static SDL_Event s_events[EVENT_QUEUE_SIZE];
static volatile uint32_t s_event_head; // game writes, present reads
static volatile uint32_t s_event_tail; // present writes, game reads

// Drops the event when the queue is full. Same reasoning as the command queue:
// a lost mouse-move is not worth stalling a frame, and ImGui recovers on the
// next one.
static void queue_event(const SDL_Event *ev)
{
    uint32_t head = __atomic_load_n(&s_event_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&s_event_tail, __ATOMIC_ACQUIRE);

    if ((head - tail) >= EVENT_QUEUE_SIZE)
        return;

    s_events[head & EVENT_QUEUE_MASK] = *ev;
    __atomic_store_n(&s_event_head, head + 1, __ATOMIC_RELEASE);
}

// Called from the present thread, before ImGui_ImplSDL2_NewFrame().
void native_video_drain_events(void)
{
    uint32_t tail = __atomic_load_n(&s_event_tail, __ATOMIC_ACQUIRE);

    for (;;)
    {
        uint32_t head = __atomic_load_n(&s_event_head, __ATOMIC_ACQUIRE);
        if (tail == head)
            return;

        SDL_Event ev = s_events[tail & EVENT_QUEUE_MASK];
        __atomic_store_n(&s_event_tail, tail + 1, __ATOMIC_RELEASE);
        tail++;

        native_debug_menu_process_event(&ev);
    }
}

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

// F1, or Ctrl+Shift+D, opens and closes the overlay.
//
// Both are handled here rather than in ImGui so the key is seen at the same
// point in the frame as every other input, and so the binding works whether or
// not the overlay's window has focus. Neither key is in the GBA mapping, so
// there is nothing for the game to lose.
static bool IsMenuToggle(const SDL_KeyboardEvent *key)
{
    if (key->keysym.scancode == SDL_SCANCODE_F1)
        return true;

    SDL_Keymod mod = SDL_GetModState();
    if (key->keysym.scancode == SDL_SCANCODE_D
        && (mod & KMOD_CTRL) && (mod & KMOD_SHIFT))
        return true;

    return false;
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

            if (ev.type == SDL_KEYDOWN && IsMenuToggle(&ev.key))
            {
                native_debug_menu_request_toggle();
                break;
            }

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

        // Hand it on to the overlay, which is on another thread and cannot
        // touch this context.
        queue_event(&ev);
    }

    // REG_KEYINPUT is active low: a set bit means the key is up. Injected bits
    // override the real keyboard so a headless run can drive the game.
    uint16_t state = s_keyinput & ~s_injected;
    *(volatile uint16_t *)(NATIVE_IO + REG_OFFSET_KEYINPUT) = state;

    return running;
}