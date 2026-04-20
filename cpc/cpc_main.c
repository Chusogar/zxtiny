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
#if 0
  protected static final int[] KEY_MAP = {
    // Row 0
    KeyEvent.VK_UP, KeyEvent.VK_RIGHT, KeyEvent.VK_DOWN, KeyEvent.VK_NUMPAD9,
    KeyEvent.VK_NUMPAD6, KeyEvent.VK_NUMPAD3, KeyEvent.VK_END, KeyEvent.VK_DECIMAL,

    // Row 1
    KeyEvent.VK_LEFT, KeyEvent.VK_ALT, KeyEvent.VK_NUMPAD7, KeyEvent.VK_NUMPAD8,
    KeyEvent.VK_NUMPAD5, KeyEvent.VK_NUMPAD1, KeyEvent.VK_NUMPAD2, KeyEvent.VK_NUMPAD0,

    // Row 2
    KeyEvent.VK_BACK_SLASH, KeyEvent.VK_ALT_GRAPH, KeyEvent.VK_ENTER,
    KeyEvent.VK_CLOSE_BRACKET, KeyEvent.VK_NUMPAD4, KeyEvent.VK_SHIFT,
    KeyEvent.VK_BACK_QUOTE, KeyEvent.VK_CONTROL,

    // Row 3
    KeyEvent.VK_EQUALS, KeyEvent.VK_MINUS, KeyEvent.VK_OPEN_BRACKET, KeyEvent.VK_P,
    KeyEvent.VK_QUOTE, KeyEvent.VK_SEMICOLON, KeyEvent.VK_SLASH, KeyEvent.VK_PERIOD,

    // Row 4
    KeyEvent.VK_0, KeyEvent.VK_9, KeyEvent.VK_O, KeyEvent.VK_I,
    KeyEvent.VK_L, KeyEvent.VK_K, KeyEvent.VK_M, KeyEvent.VK_COMMA,

    // Row 5
    KeyEvent.VK_8, KeyEvent.VK_7, KeyEvent.VK_U, KeyEvent.VK_Y,
    KeyEvent.VK_H, KeyEvent.VK_J, KeyEvent.VK_N, KeyEvent.VK_SPACE,

    // Row 6
    KeyEvent.VK_6, KeyEvent.VK_5, KeyEvent.VK_R, KeyEvent.VK_T,
    KeyEvent.VK_G, KeyEvent.VK_F, KeyEvent.VK_B, KeyEvent.VK_V,

    // Row 7
    KeyEvent.VK_4, KeyEvent.VK_3, KeyEvent.VK_E, KeyEvent.VK_W,
    KeyEvent.VK_S, KeyEvent.VK_D, KeyEvent.VK_C, KeyEvent.VK_X,

    // Row 8
    KeyEvent.VK_1, KeyEvent.VK_2, KeyEvent.VK_ESCAPE, KeyEvent.VK_Q,
    KeyEvent.VK_TAB, KeyEvent.VK_A, KeyEvent.VK_CAPS_LOCK, KeyEvent.VK_Z,

    // Row 9
    -1, -1, -1, -1, -1, -1, -1, KeyEvent.VK_BACK_SPACE
  };
#endif

static void cpc_key(SDL_Keycode sym, bool press) {
    // Matriz CPC estándar (simplificada - filas 0-9, bits 0-7)
    int row=-1, bit=-1;
    switch(sym) {
		// Fila 0
        case SDLK_UP:     row=0;bit=0;break;
        case SDLK_RIGHT:  row=0;bit=1;break;
        case SDLK_DOWN:   row=0;bit=2;break;
		//KeyEvent.VK_NUMPAD9
		//KeyEvent.VK_NUMPAD6
		//KeyEvent.VK_NUMPAD3
		//KeyEvent.VK_END
		//KeyEvent.VK_DECIMAL
        
		// Fila 1
		case SDLK_LEFT:   row=1;bit=0;break;
		//KeyEvent.VK_ALT
		//KeyEvent.VK_NUMPAD7
		//KeyEvent.VK_NUMPAD8
		//KeyEvent.VK_NUMPAD5
		//KeyEvent.VK_NUMPAD1
		//KeyEvent.VK_NUMPAD2
		//KeyEvent.VK_NUMPAD0,
        
        // Fila 2
		//KeyEvent.VK_BACK_SLASH
		//KeyEvent.VK_ALT_GRAPH
		case SDLK_RETURN: row=2;bit=2;break;
		//KeyEvent.VK_CLOSE_BRACKET
		//KeyEvent.VK_NUMPAD4
		case SDLK_LSHIFT: row=2;bit=5;break;//KeyEvent.VK_SHIFT,
		case SDLK_RSHIFT: row=2;bit=5;break;//KeyEvent.VK_SHIFT,
		//KeyEvent.VK_BACK_QUOTE
		//KeyEvent.VK_CONTROL,
        
		// Fila 3
		//KeyEvent.VK_EQUALS
		//KeyEvent.VK_MINUS
		//KeyEvent.VK_OPEN_BRACKET
		case SDLK_p:      row=3;bit=3;break;//KeyEvent.VK_P,
		//KeyEvent.VK_QUOTE
		//KeyEvent.VK_SEMICOLON
		//KeyEvent.VK_SLASH
		//KeyEvent.VK_PERIOD,
        
		// Fila 4
		case SDLK_0:      row=4;bit=0;break;//KeyEvent.VK_0
		case SDLK_9:      row=4;bit=1;break;//KeyEvent.VK_9
		case SDLK_o:      row=4;bit=2;break;//KeyEvent.VK_O
		case SDLK_i:      row=4;bit=3;break;//KeyEvent.VK_I
		case SDLK_l:      row=4;bit=4;break;//KeyEvent.VK_L
		case SDLK_k:      row=4;bit=5;break;//KeyEvent.VK_K
		case SDLK_m:      row=4;bit=6;break;//KeyEvent.VK_M
		case SDLK_COMMA:      row=4;bit=7;break;//KeyEvent.VK_COMMA,
        
		// Fila 5
		case SDLK_8:      row=5;bit=0;break;//KeyEvent.VK_8
		case SDLK_7:      row=5;bit=1;break;//KeyEvent.VK_7
		case SDLK_u:      row=5;bit=2;break;//KeyEvent.VK_U
		case SDLK_y:      row=5;bit=3;break;//KeyEvent.VK_Y,
		case SDLK_h:      row=5;bit=4;break;//KeyEvent.VK_H
		case SDLK_j:      row=5;bit=5;break;//KeyEvent.VK_J
		case SDLK_n:      row=5;bit=6;break;//KeyEvent.VK_N
		case SDLK_SPACE:      row=5;bit=7;break;//KeyEvent.VK_SPACE
        
		// Fila 6
		case SDLK_6:  row=6;bit=0;break;//KeyEvent.VK_6
		case SDLK_5:  row=6;bit=1;break;//KeyEvent.VK_5
		case SDLK_r:  row=6;bit=2;break;//KeyEvent.VK_R
		case SDLK_t:  row=6;bit=3;break;//KeyEvent.VK_T,
		case SDLK_g:  row=6;bit=4;break;//KeyEvent.VK_G
		case SDLK_f:  row=6;bit=5;break;//KeyEvent.VK_F
		case SDLK_b:  row=6;bit=6;break;//KeyEvent.VK_B
		case SDLK_v:  row=6;bit=7;break;//KeyEvent.VK_V,
        
		// Fila 7
		case SDLK_4:      row=7;bit=0;break;//KeyEvent.VK_4
		case SDLK_3:      row=7;bit=1;break;//KeyEvent.VK_3
		case SDLK_e:      row=7;bit=2;break;//KeyEvent.VK_E
		case SDLK_w:      row=7;bit=3;break;//KeyEvent.VK_W,
		case SDLK_s:      row=7;bit=4;break;//KeyEvent.VK_S
		case SDLK_d:      row=7;bit=5;break;//KeyEvent.VK_D
		case SDLK_c:      row=7;bit=6;break;//KeyEvent.VK_C
		case SDLK_x:      row=7;bit=7;break;//KeyEvent.VK_X
        
		// Fila 8
		case SDLK_1:      row=8;bit=0;break;//KeyEvent.VK_1
		case SDLK_2:      row=8;bit=1;break;//KeyEvent.VK_2
		case SDLK_ESCAPE:      row=8;bit=2;break;//KeyEvent.VK_ESCAPE
		case SDLK_q:      row=8;bit=3;break;//KeyEvent.VK_Q,
		case SDLK_TAB:      row=8;bit=4;break;//KeyEvent.VK_TAB
		case SDLK_a:      row=8;bit=5;break;//KeyEvent.VK_A
		case SDLK_CAPSLOCK:      row=8;bit=6;break;//KeyEvent.VK_CAPS_LOCK
		case SDLK_z:      row=8;bit=7;break;//KeyEvent.VK_Z
        
		// Fila 9: DEL, ^, -, ], RETURN, \, L, '
        case SDLK_BACKSPACE:      row=9;bit=7;break;
        
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
