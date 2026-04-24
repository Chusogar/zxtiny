/*
 * cpc_main.c  –  Amstrad CPC 6128 emulator entry point
 *
 * gcc cpc_main.c cpc.c cpc_fdc.c ../jgz80/z80.c -o cpc -lSDL2 -lm
 *
 * Controles:
 * Teclado PC → teclado CPC (mapeo completo por scancodes)
 * F1  : cargar game.sna del directorio base
 * F2  : screenshot
 * F3  : insertar game.dsk en unidad A:
 * F4  : expulsar disco de unidad A:
 * F5  : insertar game.dsk en unidad B:
 * F6  : expulsar disco de unidad B:
 * F10 : salir
 * F11 : pausa
 * F12 : reset
 * También se puede arrastrar un .DSK o .SNA sobre la ventana
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <signal.h>
#include <SDL2/SDL.h>
#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#endif
#include "cpc.h"

SDL_Window* cpc_window    = NULL;
SDL_Renderer* cpc_renderer  = NULL;
SDL_Texture* cpc_texture   = NULL;
SDL_AudioDeviceID cpc_audio_dev = 0;

static bool should_quit = false;
static bool has_focus   = true;
static bool is_paused   = false;
static SDL_GameController* controller = NULL;
static char disk_status[128] = "No disk";  // status line para la barra de título

// ── Throttle (reloj maestro de audio) ─────────────────────────────────────────
#define AUDIO_CHANNELS 2
#define QUEUE_TARGET_FRAMES (CPC_AUDIO_SPF * 2)  // mantener ~2 frames de vídeo en cola

static void audio_throttle(void) {
    if (!cpc_audio_dev) return;

    uint32_t dl = SDL_GetTicks() + 80;
    while (SDL_GetTicks() < dl) {
        uint32_t queued_bytes = SDL_GetQueuedAudioSize(cpc_audio_dev);
        uint32_t queued_frames = queued_bytes / (sizeof(int16_t) * AUDIO_CHANNELS);

        if (queued_frames <= (uint32_t)QUEUE_TARGET_FRAMES) break;
    }
}

// ── Mapeo de teclado CPC (matriz 10×8) por Scancodes ──────────────────────────
static void cpc_key(SDL_Scancode sc, bool press) {
    int row=-1, bit=-1;
    switch(sc) {
        // Fila 0
        case SDL_SCANCODE_UP:        row=0; bit=0; break;
        case SDL_SCANCODE_RIGHT:     row=0; bit=1; break;
        case SDL_SCANCODE_DOWN:      row=0; bit=2; break;
        case SDL_SCANCODE_KP_9:      row=0; bit=3; break;
        case SDL_SCANCODE_KP_6:      row=0; bit=4; break;
        case SDL_SCANCODE_KP_3:      row=0; bit=5; break;
        case SDL_SCANCODE_KP_ENTER:  row=0; bit=6; break;
        case SDL_SCANCODE_KP_PERIOD: row=0; bit=7; break;

        // Fila 1
        case SDL_SCANCODE_LEFT:      row=1; bit=0; break;
        case SDL_SCANCODE_LALT:
        case SDL_SCANCODE_RALT:      row=1; bit=1; break; // Tecla COPY
        case SDL_SCANCODE_KP_7:      row=1; bit=2; break;
        case SDL_SCANCODE_KP_8:      row=1; bit=3; break;
        case SDL_SCANCODE_KP_5:      row=1; bit=4; break;
        case SDL_SCANCODE_KP_1:      row=1; bit=5; break;
        case SDL_SCANCODE_KP_2:      row=1; bit=6; break;
        case SDL_SCANCODE_KP_0:      row=1; bit=7; break;

        // Fila 2
        case SDL_SCANCODE_HOME:
        case SDL_SCANCODE_INSERT:    row=2; bit=0; break; // Tecla CLR
        case SDL_SCANCODE_RIGHTBRACKET: row=2; bit=1; break; // [ (Físico: Tecla a la derecha de P)
        case SDL_SCANCODE_RETURN:    row=2; bit=2; break;
        case SDL_SCANCODE_BACKSLASH: row=2; bit=3; break; // ] (Físico: Tecla encima de Intro)
        case SDL_SCANCODE_KP_4:      row=2; bit=4; break;
        case SDL_SCANCODE_LSHIFT:
        case SDL_SCANCODE_RSHIFT:    row=2; bit=5; break;
        case SDL_SCANCODE_GRAVE:     row=2; bit=6; break; // \ (Físico: Tecla a la izquierda del 1)
        case SDL_SCANCODE_LCTRL:
        case SDL_SCANCODE_RCTRL:     row=2; bit=7; break;

        // Fila 3
        case SDL_SCANCODE_EQUALS:    row=3; bit=0; break; // ^
        case SDL_SCANCODE_MINUS:     row=3; bit=1; break; // -
        case SDL_SCANCODE_LEFTBRACKET: row=3; bit=2; break; // @
        case SDL_SCANCODE_P:         row=3; bit=3; break;
        case SDL_SCANCODE_SEMICOLON: row=3; bit=4; break; // ;
        case SDL_SCANCODE_APOSTROPHE: row=3; bit=5; break; // :
        case SDL_SCANCODE_SLASH:     row=3; bit=6; break; // /
        case SDL_SCANCODE_PERIOD:    row=3; bit=7; break; // .

        // Fila 4
        case SDL_SCANCODE_0:         row=4; bit=0; break;
        case SDL_SCANCODE_9:         row=4; bit=1; break;
        case SDL_SCANCODE_O:         row=4; bit=2; break;
        case SDL_SCANCODE_I:         row=4; bit=3; break;
        case SDL_SCANCODE_L:         row=4; bit=4; break;
        case SDL_SCANCODE_K:         row=4; bit=5; break;
        case SDL_SCANCODE_M:         row=4; bit=6; break;
        case SDL_SCANCODE_COMMA:     row=4; bit=7; break;

        // Fila 5
        case SDL_SCANCODE_8:         row=5; bit=0; break;
        case SDL_SCANCODE_7:         row=5; bit=1; break;
        case SDL_SCANCODE_U:         row=5; bit=2; break;
        case SDL_SCANCODE_Y:         row=5; bit=3; break;
        case SDL_SCANCODE_H:         row=5; bit=4; break;
        case SDL_SCANCODE_J:         row=5; bit=5; break;
        case SDL_SCANCODE_N:         row=5; bit=6; break;
        case SDL_SCANCODE_SPACE:     row=5; bit=7; break;

        // Fila 6
        case SDL_SCANCODE_6:         row=6; bit=0; break;
        case SDL_SCANCODE_5:         row=6; bit=1; break;
        case SDL_SCANCODE_R:         row=6; bit=2; break;
        case SDL_SCANCODE_T:         row=6; bit=3; break;
        case SDL_SCANCODE_G:         row=6; bit=4; break;
        case SDL_SCANCODE_F:         row=6; bit=5; break;
        case SDL_SCANCODE_B:         row=6; bit=6; break;
        case SDL_SCANCODE_V:         row=6; bit=7; break;

        // Fila 7
        case SDL_SCANCODE_4:         row=7; bit=0; break;
        case SDL_SCANCODE_3:         row=7; bit=1; break;
        case SDL_SCANCODE_E:         row=7; bit=2; break;
        case SDL_SCANCODE_W:         row=7; bit=3; break;
        case SDL_SCANCODE_S:         row=7; bit=4; break;
        case SDL_SCANCODE_D:         row=7; bit=5; break;
        case SDL_SCANCODE_C:         row=7; bit=6; break;
        case SDL_SCANCODE_X:         row=7; bit=7; break;

        // Fila 8
        case SDL_SCANCODE_1:         row=8; bit=0; break;
        case SDL_SCANCODE_2:         row=8; bit=1; break;
        case SDL_SCANCODE_ESCAPE:    row=8; bit=2; break; // ESCAPE de CPC
        case SDL_SCANCODE_Q:         row=8; bit=3; break;
        case SDL_SCANCODE_TAB:       row=8; bit=4; break;
        case SDL_SCANCODE_A:         row=8; bit=5; break;
        case SDL_SCANCODE_CAPSLOCK:  row=8; bit=6; break;
        case SDL_SCANCODE_Z:         row=8; bit=7; break;

        // Fila 9
        case SDL_SCANCODE_BACKSPACE:
        case SDL_SCANCODE_DELETE:    row=9; bit=7; break; // DEL

        default: break;
    }
    
    if (row>=0 && bit>=0) {
        if (press) cpc_keymap[row] &= ~(1<<bit);
        else       cpc_keymap[row] |=  (1<<bit);
    }
}

#ifndef __EMSCRIPTEN__
static void on_sigint(int s){(void)s;should_quit=true;}
static void screenshot(void){
    time_t t=time(NULL); struct tm tm=*localtime(&t);
    char fn[64]; snprintf(fn,sizeof(fn),"%d%02d%02d_%02d%02d%02d-cpc.bmp",
        tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,tm.tm_hour,tm.tm_min,tm.tm_sec);
    SDL_Surface* s=SDL_CreateRGBSurfaceWithFormatFrom(cpc_pixels,CPC_W,CPC_H,32,
        CPC_W*4,SDL_PIXELFORMAT_ARGB8888);
    SDL_SaveBMP(s,fn); SDL_FreeSurface(s); SDL_Log("Screenshot: %s",fn);
}
#endif

static void update_window_title(void) {
    char title[256];
    const char* dsk_a = cpc_disk_inserted(0) ? "A:[disk]" : "A:[---]";
    const char* dsk_b = cpc_disk_inserted(1) ? "B:[disk]" : "B:[---]";
    snprintf(title, sizeof(title),
             "Amstrad CPC 6128  %s %s  F3=InsertA F4=EjectA F12=Reset F10=Quit",
             dsk_a, dsk_b);
    SDL_SetWindowTitle(cpc_window, title);
}

static void load_dsk_to_drive(const char* path, int drive) {
    if (cpc_load_dsk_drive(path, drive)) {
        printf("CPC: DSK cargado en unidad %c: %s\n", 'A'+drive, path);
        update_window_title();
    } else {
        fprintf(stderr, "CPC: Error cargando DSK %s\n", path);
    }
}

static void process_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type==SDL_QUIT){should_quit=true;continue;}
        if (e.type==SDL_WINDOWEVENT){
            if(e.window.event==SDL_WINDOWEVENT_FOCUS_GAINED) has_focus=true;
            else if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST) has_focus=false;
            continue;
        }
        // Drag & Drop de archivos DSK/SNA
        if (e.type == SDL_DROPFILE) {
            char* dropped = e.drop.file;
            if (dropped) {
                const char* ext = strrchr(dropped, '.');
                if (ext) {
                    if (!strcasecmp(ext, ".dsk")) {
                        const uint8_t* ks = SDL_GetKeyboardState(NULL);
                        int drv = (ks[SDL_SCANCODE_LSHIFT] || ks[SDL_SCANCODE_RSHIFT]) ? 1 : 0;
                        load_dsk_to_drive(dropped, drv);
                    } else if (!strcasecmp(ext, ".sna")) {
                        cpc_load_sna(dropped);
                    }
                }
                SDL_free(dropped);
            }
            continue;
        }

        if (e.type==SDL_KEYDOWN||e.type==SDL_KEYUP) {
            bool press=(e.type==SDL_KEYDOWN);
            if (press) switch(e.key.keysym.scancode){
                case SDL_SCANCODE_F10: should_quit=true; break; // Cambiado ESC por F10 para salir
                case SDL_SCANCODE_F1: {
                    char* b=SDL_GetBasePath();
                    char p[512]; snprintf(p,sizeof(p),"%sgame.sna",b);
                    cpc_load_sna(p); SDL_free(b);
                } break;
                case SDL_SCANCODE_F2:
#ifndef __EMSCRIPTEN__
                    screenshot();
#endif
                    break;

                case SDL_SCANCODE_F3: {
                    char* b = SDL_GetBasePath();
                    char p[512]; snprintf(p, sizeof(p), "%sgame.dsk", b);
                    load_dsk_to_drive(p, 0);
                    SDL_free(b);
                } break;

                case SDL_SCANCODE_F4:
                    cpc_eject_disk(0);
                    update_window_title();
                    break;

                case SDL_SCANCODE_F5: {
                    char* b = SDL_GetBasePath();
                    char p[512]; snprintf(p, sizeof(p), "%sgame.dsk", b);
                    load_dsk_to_drive(p, 1);
                    SDL_free(b);
                } break;

                case SDL_SCANCODE_F6:
                    cpc_eject_disk(1);
                    update_window_title();
                    break;
                case SDL_SCANCODE_F11: is_paused=!is_paused; break;
                case SDL_SCANCODE_F12: cpc_reset(); break;
                default: break;
            }
            // Pasamos el Scancode para independizar el layout (QWERTY/AZERTY...)
            cpc_key(e.key.keysym.scancode, press);
        }
    }
}

static void mainloop(void) {
    process_events();
    if (!is_paused && has_focus) {
        audio_throttle();
        cpc_update();
    }
    cpc_render();
}

int main(int argc, char** argv) {
#ifndef __EMSCRIPTEN__
    signal(SIGINT, on_sigint);
#endif
    const char* rom_dir = (argc>1 && argv[1][0]!='-') ? argv[1] : ".";
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)!=0){
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }

    cpc_window = SDL_CreateWindow("Amstrad CPC 6128 – F12=Reset F11=Pause F10=Quit",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CPC_W*CPC_SCALE, CPC_H*CPC_SCALE,
        SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_SetWindowMinimumSize(cpc_window, CPC_W, CPC_H);

    cpc_renderer = SDL_CreateRenderer(cpc_window,-1,
        SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(cpc_renderer, CPC_W, CPC_H);

    cpc_texture = SDL_CreateTexture(cpc_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, CPC_W, CPC_H);

    for (int i=0;i<SDL_NumJoysticks();i++)
        if(SDL_IsGameController(i)){controller=SDL_GameControllerOpen(i);break;}

    // Audio
    SDL_AudioSpec want = {0}, have = {0};
    want.freq     = CPC_AUDIO_RATE;
    want.format   = AUDIO_S16SYS;
    want.channels = 2;
    want.samples  = 512;
    want.callback = NULL;

    cpc_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!cpc_audio_dev) {
        fprintf(stderr, "SDL_OpenAudioDevice: %s\n", SDL_GetError());
    } else {
        if (have.channels != 2 || have.freq != CPC_AUDIO_RATE) {
            fprintf(stderr, "Audio have: freq=%d channels=%d format=0x%x samples=%d\n",
                    have.freq, have.channels, have.format, have.samples);
        }
        SDL_PauseAudioDevice(cpc_audio_dev, 0);
    }

    if (cpc_init(rom_dir)<0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"CPC 6128 – ROM missing",
            "Place os.rom + basic.rom (and optionally amsdos.rom)\n"
            "or cpc6128.rom (48KB) in the rom directory.",cpc_window);
        return 1;
    }

    update_window_title();

    for (int i=1;i<argc;i++) {
        const char* ext=strrchr(argv[i],'.');
        if (!ext) continue;
        if (!strcasecmp(ext,".sna")) cpc_load_sna(argv[i]);
        if (!strcasecmp(ext,".dsk")) { load_dsk_to_drive(argv[i], 0); update_window_title(); }
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainloop,0,1);
#else
    while(!should_quit) mainloop();
#endif

    cpc_quit();
    if(cpc_audio_dev) SDL_CloseAudioDevice(cpc_audio_dev);
    if(controller) SDL_GameControllerClose(controller);
    SDL_DestroyTexture(cpc_texture);
    SDL_DestroyRenderer(cpc_renderer);
    SDL_DestroyWindow(cpc_window);
    SDL_Quit();
    return 0;
}