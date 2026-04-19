/*
 * cpc_main.c  –  Amstrad CPC 6128 emulator entry point
 *
 * gcc cpc_main.c cpc.c cpc_fdc.c ../jgz80/z80.c -o cpc -lSDL2 -lm
 *
 * Controles:
 *   Teclado PC → teclado CPC (mapeo directo)
 *   F1  : cargar game.sna del directorio base
 *   F2  : screenshot
 *   F3  : insertar game.dsk en unidad A:
 *   F4  : expulsar disco de unidad A:
 *   F5  : insertar game.dsk en unidad B:
 *   F6  : expulsar disco de unidad B:
 *   F11 : pausa
 *   F12 : reset
 *   ESC : salir
 *   También se puede arrastrar un .DSK o .SNA sobre la ventana
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

SDL_Window*       cpc_window    = NULL;
SDL_Renderer*     cpc_renderer  = NULL;
SDL_Texture*      cpc_texture   = NULL;
SDL_AudioDeviceID cpc_audio_dev = 0;

static bool should_quit = false;
static bool has_focus   = true;
static bool is_paused   = false;
static SDL_GameController* controller = NULL;
static char disk_status[128] = "No disk";  // status line para la barra de título

// ── Throttle (reloj maestro de audio) ─────────────────────────────────────────
#define QUEUE_TARGET (CPC_AUDIO_SPF * 2)
static void audio_throttle(void) {
    if (!cpc_audio_dev) return;
    uint32_t dl = SDL_GetTicks() + 80;
    while (SDL_GetTicks() < dl) {
        if (SDL_GetQueuedAudioSize(cpc_audio_dev)/sizeof(int16_t)
            <= (uint32_t)QUEUE_TARGET) break;
        SDL_Delay(1);
    }
}

// ── Mapeo de teclado CPC (matriz 10×8) ────────────────────────────────────────
// Cada entrada: {fila, bit}
// Solo mapeamos las teclas más comunes
typedef struct { int row; int bit; } KeyPos;

static void cpc_key(SDL_Keycode sym, bool press) {
    // Matriz CPC estándar (simplificada - filas 0-9, bits 0-7)
    int row=-1, bit=-1;
    switch(sym) {
        // Fila 0: CURSOR-UP, CURSOR-RIGHT, CURSOR-DOWN, F9, F6, F3, ENTER(num), .
        case SDLK_UP:     row=0;bit=0;break;
        case SDLK_RIGHT:  row=0;bit=1;break;
        case SDLK_DOWN:   row=0;bit=2;break;
        // Fila 1: CURSOR-LEFT, COPY, F7, F8, F5, F1, F2, F0
        case SDLK_LEFT:   row=1;bit=0;break;
        // Fila 2: CAPS-LK, A, TAB, Q, Z, W, S, X (letras fila izq)
        case SDLK_a:      row=8;bit=5;break;
        case SDLK_q:      row=2;bit=3;break;
        case SDLK_z:      row=2;bit=4;break;
        case SDLK_w:      row=2;bit=5;break;
        case SDLK_s:      row=2;bit=6;break;
        case SDLK_x:      row=2;bit=7;break;
        // Fila 3: DEL, J3, LOCK, F4, SHIFT, F7(joy?), E, D, C
        case SDLK_e:      row=3;bit=6;break;
        case SDLK_d:      row=3;bit=7;break;
        // Fila 4: 3, 4, R, F, V, (espacio izq)
        case SDLK_3:      row=4;bit=0;break;
        case SDLK_4:      row=4;bit=1;break;
        case SDLK_r:      row=4;bit=2;break;
        case SDLK_f:      row=4;bit=3;break;
        case SDLK_v:      row=4;bit=4;break;
        case SDLK_c:      row=7;bit=6;break;
        // Fila 5: 5, 6, T, G, B
        case SDLK_5:      row=5;bit=0;break;
        case SDLK_6:      row=5;bit=1;break;
        case SDLK_t:      row=6;bit=3;break;
        case SDLK_g:      row=5;bit=3;break;
        case SDLK_b:      row=5;bit=4;break;
        // Fila 6: SPACE, 7, Y, H, N
        case SDLK_SPACE:  row=6;bit=0;break;
        case SDLK_7:      row=6;bit=1;break;
        case SDLK_y:      row=6;bit=2;break;
        case SDLK_h:      row=6;bit=3;break;
        case SDLK_n:      row=6;bit=4;break;
        // Fila 7: 8, 9, U, I, O, M, K, J
        case SDLK_8:      row=7;bit=0;break;
        case SDLK_9:      row=7;bit=1;break;
        case SDLK_u:      row=7;bit=2;break;
        case SDLK_i:      row=7;bit=3;break;
        case SDLK_o:      row=7;bit=4;break;
        case SDLK_m:      row=7;bit=5;break;
        case SDLK_k:      row=7;bit=6;break;
        case SDLK_j:      row=7;bit=7;break;
        // Fila 8: 0, -, P, @, [, :, ;, /
        case SDLK_0:      row=8;bit=0;break;
        case SDLK_p:      row=8;bit=3;break;
        // Fila 9: DEL, ^, -, ], RETURN, \, L, '
        case SDLK_RETURN: row=2;bit=2;break;
        case SDLK_l:      row=9;bit=5;break;
        // Shifts
        case SDLK_LSHIFT:
        case SDLK_RSHIFT: row=2;bit=5;break;   // CPC SHIFT = fila 2 bit 5
        case SDLK_LCTRL:
        case SDLK_RCTRL:  row=2;bit=2;break;   // CTRL
        case SDLK_1:      row=9;bit=0;break;
        case SDLK_2:      row=9;bit=1;break;
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
             "Amstrad CPC 6128  %s %s  F3=InsertA F4=EjectA F12=Reset ESC=Quit",
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
                        // Modifier: Shift = unidad B, sin modifier = unidad A
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
                case SDL_SCANCODE_ESCAPE: should_quit=true; break;
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
                    // Insertar game.dsk en unidad A:
                    char* b = SDL_GetBasePath();
                    char p[512]; snprintf(p, sizeof(p), "%sgame.dsk", b);
                    load_dsk_to_drive(p, 0);
                    SDL_free(b);
                } break;

                case SDL_SCANCODE_F4:
                    // Expulsar disco de unidad A:
                    cpc_eject_disk(0);
                    update_window_title();
                    break;

                case SDL_SCANCODE_F5: {
                    // Insertar game.dsk en unidad B:
                    char* b = SDL_GetBasePath();
                    char p[512]; snprintf(p, sizeof(p), "%sgame.dsk", b);
                    load_dsk_to_drive(p, 1);
                    SDL_free(b);
                } break;

                case SDL_SCANCODE_F6:
                    // Expulsar disco de unidad B:
                    cpc_eject_disk(1);
                    update_window_title();
                    break;
                case SDL_SCANCODE_F11: is_paused=!is_paused; break;
                case SDL_SCANCODE_F12: cpc_reset(); break;
                default: break;
            }
            cpc_key(e.key.keysym.sym, press);
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

    cpc_window = SDL_CreateWindow("Amstrad CPC 6128 – F12=Reset F11=Pause ESC=Quit",
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
    SDL_AudioSpec want={0},have={0};
    want.freq=CPC_AUDIO_RATE; want.format=AUDIO_S16SYS;
    want.channels=1; want.samples=512; want.callback=NULL;
    cpc_audio_dev=SDL_OpenAudioDevice(NULL,0,&want,&have,0);
    if(cpc_audio_dev) SDL_PauseAudioDevice(cpc_audio_dev,0);

    if (cpc_init(rom_dir)<0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"CPC 6128 – ROM missing",
            "Place os.rom + basic.rom (and optionally amsdos.rom)\n"
            "or cpc6128.rom (48KB) in the rom directory.",cpc_window);
        return 1;
    }

    update_window_title();

    // Cargar archivo si se pasa como argumento
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
