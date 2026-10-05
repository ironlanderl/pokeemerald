// platform/native/main.c
//
// Native entry point. Sets up the emulated memory map, then hands control to
// the game's own AgbMain, driving a frame loop from outside.

// libc headers come first: global.h defines min/max/abs as macros, which
// collide with the declarations in <stdlib.h> and <stdio.h>.
#include <stdio.h>
#include <stdlib.h>

#include "global.h"
#include "native.h"


// Provided by video.c
bool native_video_init(int scale);
void native_video_render(void);
void native_video_shutdown(void);
bool native_input_poll(void);

// Provided by ppu.c
void native_ppu_render_frame(void);

// The game's entry point (src/main.c).
void AgbMain(void);

static const char *kDefaultRom = "pokeemerald.gba";
static const char *kDefaultSave = "pokeemerald.sav";

// ENTRY points here, so no CRT has run and argc/argv are not set up. The
// kernel passes them on the initial stack (argc at [sp], argv just above), so
// recover them there. Reading them off the stack keeps this portable across
// architectures, unlike an inline-asm trampoline.
int native_main(int argc, char **argv);

// The C runtime's _start calls main(); forward so argc/argv are set up by the
// CRT rather than guessed from the stack.
int main(int argc, char **argv)
{
    return native_main(argc, argv);
}

int native_main(int argc, char **argv)
{
    const char *rom_path = kDefaultRom;
    const char *save_path = kDefaultSave;
    int scale = 3;
    int frame_limit = 0;          // 0 = run until the window closes
    const char *shot_path = NULL;  // write one PPM here and exit
    int press_at = -1;            // frame at which to simulate a button press
    uint16_t press_bits = 0;
    bool menu = false;            // open the debug overlay at startup

    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)
            rom_path = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc)
            save_path = argv[++i];
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc)
            scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frame_limit = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--shot") && i + 1 < argc)
            shot_path = argv[++i];
        else if (!strcmp(argv[i], "--press-at") && i + 1 < argc)
            press_at = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc)
            press_bits = (uint16_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--menu"))
            menu = true;
        else if (!strcmp(argv[i], "--help"))
        {
            printf("usage: %s [--rom FILE] [--save FILE] [--scale N]\n"
                   "          [--frames N] [--shot FILE.ppm]\n"
                   "          [--press-at FRAME --press BITS]\n"
                   "          [--menu]   open the debug overlay at startup\n"
                   "\n"
                   "The overlay also toggles on F1 or Ctrl+Shift+D at any time.\n",
                   argv[0]);
            return 0;
        }
    }

    if (!native_memory_init(rom_path, save_path))
    {
        native_log("fatal: %s", native_memory_error());
        return 1;
    }
    native_log("memory mapped, ROM patches applied");

    native_timing_init();
    native_timing_start_clock();

    if (!native_video_init(scale))
        return 1;

    if (frame_limit || shot_path)
        native_set_headless(frame_limit, shot_path);
    if (press_at >= 0)
        native_input_schedule(press_at, press_bits);
    if (menu)
        native_debug_menu_set_visible(true);

    native_log("starting game");

    // AgbMain never returns: it runs an infinite main loop that spin-waits on
    // VBlank. Frame rendering is driven from the VBlank interrupt instead, so
    // the loop below only services the window and input.
    //
    // AgbMain is entered on a separate stack region is unnecessary; it runs on
    // this one.
    AgbMain();

    native_video_shutdown();
    return 0;
}
