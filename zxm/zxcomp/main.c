/*
 * CPC6128 Emulator - Main entry point
 * Uses Z80 core by Carmikel (https://github.com/carmikel/z80)
 * SDL2 for video/audio/input
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#include "cpc.h"
#include "z80.h"
#include "memory.h"
#include "gate_array.h"
#include "psg.h"
#include "fdc.h"
#include "keyboard.h"
#include "sna.h"

#define CPC_SCREEN_WIDTH  768
#define CPC_SCREEN_HEIGHT 544
#define WINDOW_SCALE      1

/* SDL globals */
SDL_Window   *g_window   = NULL;
SDL_Renderer *g_renderer = NULL;
SDL_Texture  *g_texture  = NULL;
SDL_AudioDeviceID g_audio_dev = 0;

static int init_sdl(void);
static void cleanup_sdl(void);
static void handle_events(cpc_t *cpc, int *running);
static void render_frame(cpc_t *cpc);
static void audio_callback(void *userdata, Uint8 *stream, int len);

int main(int argc, char *argv[])
{
    cpc_t cpc;
    int   running = 1;
    const char *rom_os    = "roms/cpc6128.rom";
    const char *rom_basic = "roms/basic.rom";
    const char *rom_amsdos= "roms/amsdos.rom";
    const char *sna_file  = NULL;
    const char *dsk_file  = NULL;

    printf("CPC6128 Emulator v1.0\n");
    printf("Z80 core by Carmikel\n\n");

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-sna") == 0 && i+1 < argc) {
            sna_file = argv[++i];
        } else if (strcmp(argv[i], "-dsk") == 0 && i+1 < argc) {
            dsk_file = argv[++i];
        } else if (strcmp(argv[i], "-rom") == 0 && i+1 < argc) {
            rom_os = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [options]\n", argv[0]);
            printf("  -sna <file>   Load SNA snapshot\n");
            printf("  -dsk <file>   Insert DSK disk image\n");
            printf("  -rom <file>   Override OS ROM path\n");
            printf("\nKeys:\n");
            printf("  F1           Reset CPC\n");
            printf("  F2           Insert/eject disk\n");
            printf("  F10          Quit\n");
            printf("  F12          Toggle warp speed\n");
            return 0;
        }
    }

    /* Init SDL */
    if (init_sdl() != 0) {
        fprintf(stderr, "Failed to init SDL: %s\n", SDL_GetError());
        return 1;
    }

    /* Init CPC */
    if (cpc_init(&cpc, rom_os, rom_basic, rom_amsdos) != 0) {
        fprintf(stderr, "Failed to init CPC\n");
        cleanup_sdl();
        return 1;
    }

    /* Load SNA if provided */
    if (sna_file) {
        printf("Loading SNA: %s\n", sna_file);
        if (sna_load(&cpc, sna_file) != 0) {
            fprintf(stderr, "Warning: Failed to load SNA\n");
        }
    }

    /* Insert disk if provided */
    if (dsk_file) {
        printf("Inserting DSK: %s\n", dsk_file);
        fdc_insert_disk(&cpc.fdc, dsk_file, 0);
    }

    /* Audio */
    SDL_PauseAudioDevice(g_audio_dev, 0);

    printf("CPC6128 running. F10=quit, F1=reset\n");

    Uint32 frame_start, frame_time;
    const Uint32 FRAME_MS = 1000 / 50; /* 50Hz */

    while (running) {
        frame_start = SDL_GetTicks();

        handle_events(&cpc, &running);
        cpc_run_frame(&cpc);
        render_frame(&cpc);

        frame_time = SDL_GetTicks() - frame_start;
        if (!cpc.warp && frame_time < FRAME_MS) {
            SDL_Delay(FRAME_MS - frame_time);
        }
    }

    cpc_destroy(&cpc);
    cleanup_sdl();
    return 0;
}

static int init_sdl(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0)
        return -1;

    g_window = SDL_CreateWindow(
        "CPC6128 Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CPC_SCREEN_WIDTH * WINDOW_SCALE,
        CPC_SCREEN_HEIGHT * WINDOW_SCALE,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );
    if (!g_window) return -1;

    g_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!g_renderer) return -1;

    SDL_RenderSetLogicalSize(g_renderer,
        CPC_SCREEN_WIDTH, CPC_SCREEN_HEIGHT);

    g_texture = SDL_CreateTexture(g_renderer,
        SDL_PIXELFORMAT_RGB888,
        SDL_TEXTUREACCESS_STREAMING,
        CPC_SCREEN_WIDTH, CPC_SCREEN_HEIGHT);
    if (!g_texture) return -1;

    /* Audio setup */
    SDL_AudioSpec want, have;
    SDL_memset(&want, 0, sizeof(want));
    want.freq     = PSG_SAMPLE_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 512;
    want.callback = audio_callback;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (g_audio_dev == 0) {
        fprintf(stderr, "Warning: No audio: %s\n", SDL_GetError());
    }

    return 0;
}

static void cleanup_sdl(void)
{
    if (g_audio_dev) SDL_CloseAudioDevice(g_audio_dev);
    if (g_texture)  SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window)   SDL_DestroyWindow(g_window);
    SDL_Quit();
}

static void handle_events(cpc_t *cpc, int *running)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            *running = 0;
            break;
        case SDL_KEYDOWN:
            if (e.key.keysym.sym == SDLK_F10) {
                *running = 0;
            } else if (e.key.keysym.sym == SDLK_F1) {
                cpc_reset(cpc);
            } else if (e.key.keysym.sym == SDLK_F12) {
                cpc->warp = !cpc->warp;
                printf("Warp: %s\n", cpc->warp ? "ON" : "OFF");
            } else {
                keyboard_key_down(&cpc->keyboard, e.key.keysym.sym,
                                  e.key.keysym.mod);
            }
            break;
        case SDL_KEYUP:
            keyboard_key_up(&cpc->keyboard, e.key.keysym.sym,
                            e.key.keysym.mod);
            break;
        case SDL_DROPFILE: {
            char *file = e.drop.file;
            size_t len = strlen(file);
            if (len > 4 && strcasecmp(file + len - 4, ".sna") == 0) {
                printf("Loading SNA: %s\n", file);
                sna_load(cpc, file);
            } else if (len > 4 && strcasecmp(file + len - 4, ".dsk") == 0) {
                printf("Inserting DSK: %s\n", file);
                fdc_insert_disk(&cpc->fdc, file, 0);
            }
            SDL_free(file);
            break;
        }
        default:
            break;
        }
    }
}

static void render_frame(cpc_t *cpc)
{
    void  *pixels;
    int    pitch;

    SDL_LockTexture(g_texture, NULL, &pixels, &pitch);
    /* Copy framebuffer - gate array produces 768x544 RGB888 */
    uint8_t *dst = (uint8_t *)pixels;
    uint8_t *src = (uint8_t *)cpc->gate_array.framebuffer;
    for (int y = 0; y < CPC_SCREEN_HEIGHT; y++) {
        memcpy(dst + y * pitch,
               src + y * CPC_SCREEN_WIDTH * 4,
               CPC_SCREEN_WIDTH * 4);
    }
    SDL_UnlockTexture(g_texture);

    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, NULL, NULL);
    SDL_RenderPresent(g_renderer);
}

/* Audio callback: pull samples from PSG ring buffer */
static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    /* userdata not used; access global cpc via psg */
    /* In a real impl, cpc would be passed via userdata */
    memset(stream, 0, len);
    /* PSG fills audio via psg_fill_buffer() called from CPC main loop */
}
