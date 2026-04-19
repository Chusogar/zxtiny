/*
 * msx_main.c  –  MSX1 & MSX2 emulator entry point
 *
 * gcc msx_main.c msx.c ../jgz80/z80.c -o msx -lSDL2 -lm
 *
 * Uso:
 *   ./msx [--msx2] [rom_dir] [cartucho.rom|.mx1|.mx2|.cas]
 *
 * Alternativa (sin ambigüedad):
 *   ./msx [--msx2] --romdir <rom_dir> --cart <cartucho.rom>
 *
 * Controles:
 *   Teclado PC → teclado MSX (mapeo directo QWERTY)
 *   WASD / Flechas : Joystick 1
 *   SPACE / Z      : Fuego
 *   F1  : Cargar game.rom del directorio base
 *   F2  : Screenshot
 *   F11 : Pausa
 *   F12 : Reset (y recarga último cartucho si lo había)
 *   ESC : Salir
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
#ifdef _WIN32
#  define strcasecmp _stricmp
#endif
#include "msx.h"

SDL_Window*       msx_window    = NULL;
SDL_Renderer*     msx_renderer  = NULL;
SDL_Texture*      msx_texture   = NULL;
SDL_AudioDeviceID msx_audio_dev = 0;

static bool       should_quit = false;
static bool       has_focus   = true;
static bool       is_paused   = false;
static MSXModel   model       = MSX_MODEL_1;
static SDL_GameController* controller = NULL;

// Guardar el último cartucho/cinta cargado para F12
static char last_media_path[512] = {0}; // .rom/.mx1/.mx2/.cas
static bool last_media_is_cart = false;

#define QUEUE_TARGET (MSX_AUDIO_SPF * 2)
static void audio_throttle(void){
    if(!msx_audio_dev)return;
    uint32_t dl=SDL_GetTicks()+80;
    while(SDL_GetTicks()<dl){
        if(SDL_GetQueuedAudioSize(msx_audio_dev)/sizeof(int16_t)<=(uint32_t)QUEUE_TARGET)break;
        SDL_Delay(1);
    }
}

#ifndef __EMSCRIPTEN__
static void on_sigint(int s){(void)s;should_quit=true;}
static void screenshot(void){
    time_t t=time(NULL);struct tm tm=*localtime(&t);char fn[64];
    snprintf(fn,sizeof(fn),"%d%02d%02d_%02d%02d%02d-msx%d.bmp",
        tm.tm_year+1900,tm.tm_mon+1,tm.tm_mday,
        tm.tm_hour,tm.tm_min,tm.tm_sec,(int)model);
    SDL_Surface* s=SDL_CreateRGBSurfaceWithFormatFrom(msx_pixels,MSX_W,MSX_H,
        32,MSX_W*4,SDL_PIXELFORMAT_ARGB8888);
    SDL_SaveBMP(s,fn);SDL_FreeSurface(s);SDL_Log("Screenshot: %s",fn);
}
#endif

static bool has_ext(const char* path, const char* ext){
    const char* p = strrchr(path,'.');
    if(!p) return false;
    return (strcasecmp(p, ext) == 0);
}

static bool is_media_file(const char* path){
    return has_ext(path,".rom") || has_ext(path,".mx1") || has_ext(path,".mx2") || has_ext(path,".cas");
}

static bool file_exists(const char* path){
    FILE* f = fopen(path,"rb");
    if(!f) return false;
    fclose(f);
    return true;
}

static void load_media(const char* path){
    if(!path || !path[0]) return;

    if(has_ext(path,".cas")){
        if(msx_load_cas(path)){
            SDL_Log("Cargada cinta: %s", path);
            strncpy(last_media_path, path, sizeof(last_media_path)-1);
            last_media_is_cart = false;
        } else {
            SDL_Log("No se pudo cargar cinta: %s", path);
        }
        return;
    }

    // ROM / MX1 / MX2
    if(msx_load_rom(path)){
        SDL_Log("Cargado cartucho: %s", path);
        strncpy(last_media_path, path, sizeof(last_media_path)-1);
        last_media_is_cart = true;
    } else {
        SDL_Log("No se pudo cargar cartucho: %s", path);
    }
}

// ── Teclado MSX (matriz 11 filas × 8 bits) ────────────────────────────────────
static void msx_key(SDL_Keycode sym, bool press){
    int row=-1,bit=-1;
    switch(sym){
        // Fila 0: 7 6 5 4 3 2 1 0
        case SDLK_0:    row=0;bit=7;break; case SDLK_1: row=0;bit=6;break;
        case SDLK_2:    row=0;bit=5;break; case SDLK_3: row=0;bit=4;break;
        case SDLK_4:    row=0;bit=3;break; case SDLK_5: row=0;bit=2;break;
        case SDLK_6:    row=0;bit=1;break; case SDLK_7: row=0;bit=0;break;
        // Fila 2: b a _ ^ \ z y x (parcial)
        case SDLK_a:    row=2;bit=6;break; case SDLK_b: row=2;bit=7;break;
        case SDLK_z:    row=6;bit=5;break; case SDLK_y: row=2;bit=4;break;
        case SDLK_x:    row=2;bit=3;break;
        // Fila 3: j i h g f e d c
        case SDLK_c:    row=3;bit=0;break; case SDLK_d: row=3;bit=1;break;
        case SDLK_e:    row=3;bit=2;break; case SDLK_f: row=3;bit=3;break;
        case SDLK_g:    row=3;bit=4;break; case SDLK_h: row=3;bit=5;break;
        case SDLK_i:    row=3;bit=6;break; case SDLK_j: row=3;bit=7;break;
        // Fila 4: r q p o n m l k
        case SDLK_k:    row=4;bit=0;break; case SDLK_l: row=4;bit=1;break;
        case SDLK_m:    row=4;bit=2;break; case SDLK_n: row=4;bit=3;break;
        case SDLK_o:    row=4;bit=4;break; case SDLK_p: row=4;bit=5;break;
        case SDLK_q:    row=4;bit=6;break; case SDLK_r: row=4;bit=7;break;
        // Fila 5: ... w v u t s (parcial)
        case SDLK_s:    row=5;bit=0;break; case SDLK_t: row=5;bit=1;break;
        case SDLK_u:    row=5;bit=2;break; case SDLK_v: row=5;bit=3;break;
        case SDLK_w:    row=5;bit=4;break;
        // Fila 6: SHIFT / CTRL
        case SDLK_LSHIFT:case SDLK_RSHIFT: row=6;bit=0;break;
        case SDLK_LCTRL: case SDLK_RCTRL:  row=6;bit=1;break;
        // Fila 7: RET / BS / TAB
        case SDLK_RETURN:   row=7;bit=7;break;
        case SDLK_BACKSPACE:row=7;bit=5;break;
        case SDLK_TAB:      row=7;bit=4;break;
        // Fila 8: cursores / SPACE
        case SDLK_SPACE:   row=8;bit=0;break;
        case SDLK_LEFT:    row=8;bit=4;break;
        case SDLK_UP:      row=8;bit=5;break;
        case SDLK_DOWN:    row=8;bit=6;break;
        case SDLK_RIGHT:   row=8;bit=7;break;
        // Fila 9: 9 8 (parcial)
        case SDLK_8:    row=9;bit=1;break; case SDLK_9: row=9;bit=0;break;
        default: break;
    }
    if(row>=0&&bit>=0){
        if(press) msx_keymap[row]&=~(1<<bit);
        else      msx_keymap[row]|= (1<<bit);
    }
    // Joystick via cursor keys también
    switch(sym){
        case SDLK_UP:    if(press)msx_joy[0]|=1; else msx_joy[0]&=~1;break;
        case SDLK_DOWN:  if(press)msx_joy[0]|=2; else msx_joy[0]&=~2;break;
        case SDLK_LEFT:  if(press)msx_joy[0]|=4; else msx_joy[0]&=~4;break;
        case SDLK_RIGHT: if(press)msx_joy[0]|=8; else msx_joy[0]&=~8;break;
        case SDLK_z:     if(press)msx_joy[0]|=16;else msx_joy[0]&=~16;break;
        default:break;
    }
}

static void process_events(void){
    SDL_Event e;
    while(SDL_PollEvent(&e)){
        if(e.type==SDL_QUIT){should_quit=true;continue;}
        if(e.type==SDL_WINDOWEVENT){
            if(e.window.event==SDL_WINDOWEVENT_FOCUS_GAINED)has_focus=true;
            else if(e.window.event==SDL_WINDOWEVENT_FOCUS_LOST)has_focus=false;
            continue;
        }
        if(e.type==SDL_KEYDOWN||e.type==SDL_KEYUP){
            bool press=(e.type==SDL_KEYDOWN);
            if(press)switch(e.key.keysym.scancode){
                case SDL_SCANCODE_ESCAPE: should_quit=true;break;
                case SDL_SCANCODE_F1:{
                    char* b=SDL_GetBasePath();char p[512];
                    snprintf(p,sizeof(p),"%shero.rom",b);
                    load_media(p);
                    SDL_free(b);
                }break;
                case SDL_SCANCODE_F2:
#ifndef __EMSCRIPTEN__
                    screenshot();
#endif
                    break;
                case SDL_SCANCODE_F11: is_paused=!is_paused;break;
                case SDL_SCANCODE_F12:
                    msx_reset();
                    // Recargar último cartucho/cinta si existía
                    if(last_media_path[0]) load_media(last_media_path);
                    break;
                default:break;
            }
            msx_key(e.key.keysym.sym,press);
        }
        if(e.type==SDL_CONTROLLERBUTTONDOWN||e.type==SDL_CONTROLLERBUTTONUP){
            bool press=(e.type==SDL_CONTROLLERBUTTONDOWN);
#define JB(b) do{if(press)msx_joy[0]|=(b);else msx_joy[0]&=~(b);}while(0)
            switch(e.cbutton.button){
                case SDL_CONTROLLER_BUTTON_DPAD_UP:    JB(1);break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  JB(2);break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  JB(4);break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: JB(8);break;
                case SDL_CONTROLLER_BUTTON_A:          JB(16);break;
                case SDL_CONTROLLER_BUTTON_B:          JB(32);break;
                case SDL_CONTROLLER_BUTTON_START: if(press){ msx_reset(); if(last_media_path[0]) load_media(last_media_path);} break;
                default:break;
            }
#undef JB
        }
    }
}

static void mainloop(void){
    process_events();
    if(!is_paused && has_focus){audio_throttle();msx_update();}
    msx_render();
}

static void print_usage(const char* exe){
    fprintf(stderr,
        "Uso:\n"
        "  %s [--msx2] [rom_dir] [cartucho.rom|.mx1|.mx2|.cas]\n"
        "  %s [--msx2] --romdir <rom_dir> --cart <cartucho.rom>\n",
        exe, exe);
}

int main(int argc, char** argv){
#ifndef __EMSCRIPTEN__
    signal(SIGINT,on_sigint);
#endif

    // En macOS a veces llega -psn_...
    if(argc>1 && strncmp(argv[1],"-psn",4)==0){ argc--; argv++; }

    const char* rom_dir=".";
    const char* media_path=NULL;

    // Parseo robusto:
    // - Soporta: --msx2, --romdir <dir>, --cart <file>
    // - Si hay args no opción:
    //    * si parece archivo media -> media_path
    //    * si no y es dir existente -> rom_dir
    //    * si no existe como dir pero existe como file media -> media_path
    for(int i=1;i<argc;i++){
        const char* a = argv[i];

        if(!strcmp(a,"--msx2")){
            model = MSX_MODEL_2;
            continue;
        }
        if(!strcmp(a,"--romdir") && i+1<argc){
            rom_dir = argv[++i];
            continue;
        }
        if(!strcmp(a,"--cart") && i+1<argc){
            media_path = argv[++i];
            continue;
        }
        if(a[0]=='-'){
            // opción desconocida
            if(strcmp(a,"--help")==0 || strcmp(a,"-h")==0){
                print_usage(argv[0]);
                return 0;
            }
            SDL_Log("Opción desconocida: %s", a);
            continue;
        }

        // Argumento libre (no opción)
        if(is_media_file(a) || file_exists(a)) {
            // Si tiene extensión de media -> media
            if(is_media_file(a)) {
                media_path = a;
                continue;
            }
            // Si existe como archivo y parece media (por extensión) ya se manejó.
            // Si existe como archivo pero sin extensión, no lo tratamos como media.
        }

        // Si es un directorio (rom_dir)
        // (No hay API portable simple aquí; asumimos que rom_dir no tiene extensión media)
        // Si ya tenemos rom_dir en ".", permitimos que el primer arg libre sea rom_dir.
        if(!is_media_file(a) && (strcmp(rom_dir,".")==0)) {
            rom_dir = a;
            continue;
        }

        // Si no encaja, lo ignoramos
    }

    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO|SDL_INIT_GAMECONTROLLER)!=0){
        fprintf(stderr,"SDL_Init: %s\n",SDL_GetError());
        return 1;
    }

    char title[64];
    snprintf(title,sizeof(title),"MSX%d – F12=Reset F11=Pause ESC=Quit",(int)model);

    msx_window=SDL_CreateWindow(title,
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        MSX_W*MSX_SCALE,MSX_H*MSX_SCALE,
        SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    if(!msx_window){
        fprintf(stderr,"SDL_CreateWindow: %s\n",SDL_GetError());
        return 1;
    }
    SDL_SetWindowMinimumSize(msx_window,MSX_W,MSX_H);

    msx_renderer=SDL_CreateRenderer(msx_window,-1,
        SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(msx_renderer,MSX_W,MSX_H);

    msx_texture=SDL_CreateTexture(msx_renderer,SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STATIC,MSX_W,MSX_H);

    for(int i=0;i<SDL_NumJoysticks();i++)
        if(SDL_IsGameController(i)){controller=SDL_GameControllerOpen(i);break;}

    SDL_AudioSpec want={0},have={0};
    want.freq=MSX_AUDIO_RATE;want.format=AUDIO_S16SYS;
    want.channels=1;want.samples=512;want.callback=NULL;
    msx_audio_dev=SDL_OpenAudioDevice(NULL,0,&want,&have,0);
    if(msx_audio_dev)SDL_PauseAudioDevice(msx_audio_dev,0);

    if(msx_init(rom_dir,model)<0){
        char msg[256];
        snprintf(msg,sizeof(msg),
            "Place %s ROMs in the rom directory:\n"
            "  MSX1: msx1_bios.rom (32KB) + msx1_basic.rom (16KB)\n"
            "  MSX2: msx2_bios.rom (32KB) + msx1_basic.rom (16KB)\n"
            "  Or combined: msx1.rom (48KB) / msx2.rom (64KB)",
            model==MSX_MODEL_2?"MSX2":"MSX1");
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"MSX – ROM missing",msg,msx_window);
        return 1;
    }

    // Cargar cartucho/cinta si se pasó
    if(media_path && media_path[0]){
        load_media(media_path);
    }

    SDL_Log("MSX%d listo. Controles: Flechas=Joystick Z=Fuego F12=Reset",
            (int)model);

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainloop,0,1);
#else
    while(!should_quit)mainloop();
#endif

    msx_quit();
    if(msx_audio_dev)SDL_CloseAudioDevice(msx_audio_dev);
    if(controller)SDL_GameControllerClose(controller);
    SDL_DestroyTexture(msx_texture);
    SDL_DestroyRenderer(msx_renderer);
    SDL_DestroyWindow(msx_window);
    SDL_Quit();
    return 0;
}