/*
 * coleco_main.c – ColecoVision emulator entry point
 * * Compilación sugerida:
 * gcc coleco_main.c coleco.c ../jgz80/z80.c -o coleco -lSDL2 -lm
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

#include "coleco.h"

// Variables globales de SDL
SDL_Window* cv_window    = NULL;
SDL_Renderer* cv_renderer  = NULL;
SDL_Texture* cv_texture   = NULL;
SDL_AudioDeviceID  cv_audio_dev = 0;

static bool should_quit = false;
static bool has_focus   = true;
static bool is_paused   = false;
static SDL_GameController* controller = NULL;

// Sincronización de audio
#define QUEUE_TARGET (COLECO_AUDIO_SPF * 2)
static void audio_throttle(void){
    if(!cv_audio_dev) return;
    uint32_t dl = SDL_GetTicks() + 80;
    while(SDL_GetTicks() < dl){
        if(SDL_GetQueuedAudioSize(cv_audio_dev) / sizeof(int16_t) <= (uint32_t)QUEUE_TARGET) break;
        SDL_Delay(1);
    }
}

#ifndef __EMSCRIPTEN__
static void on_sigint(int s){ (void)s; should_quit = true; }

static void screenshot(void){
    time_t t = time(NULL); struct tm tm = *localtime(&t); char fn[64];
    snprintf(fn, sizeof(fn), "%d%02d%02d_%02d%02d%02d-coleco.bmp",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(cv_pixels, COLECO_W, COLECO_H,
        32, COLECO_W * 4, SDL_PIXELFORMAT_ARGB8888);
    SDL_SaveBMP(s, fn); SDL_FreeSurface(s); SDL_Log("Screenshot: %s", fn);
}
#endif

/*
 * ACTUALIZACIÓN DE CONTROLES
 * cv_joy[0]: Bit 0:Arriba, 1:Derecha, 2:Abajo, 3:Izquierda, 6:Fuego1
 * cv_keypad[0]: Bits 0-3: Código Hex, Bit 6: Fuego2 (Active-LOW)
 */
static void update_input(void) {
    const Uint8 *state = SDL_GetKeyboardState(NULL);

    // --- JOYSTICK 1 ---
    uint8_t j1 = 0;
    if (state[SDL_SCANCODE_UP]    || state[SDL_SCANCODE_W]) j1 |= 0x01;
    if (state[SDL_SCANCODE_RIGHT] || state[SDL_SCANCODE_D]) j1 |= 0x02;
    if (state[SDL_SCANCODE_DOWN]  || state[SDL_SCANCODE_S]) j1 |= 0x04;
    if (state[SDL_SCANCODE_LEFT]  || state[SDL_SCANCODE_A]) j1 |= 0x08;
    
    // Fuego 1: ESPACIO o Z (Mapeado al Bit 6)
    if (state[SDL_SCANCODE_SPACE] || state[SDL_SCANCODE_Z]) j1 |= 0x40;
    
    cv_joy[0] = j1;

    // --- KEYPAD Y BOTÓN 2 ---
    // Valor por defecto: 0x0F (ninguna tecla pulsada)
    uint8_t kp = 0x0F; 

    // Mapeo Hexadecimal real que espera la BIOS/Hardware de Coleco
    if      (state[SDL_SCANCODE_1] || state[SDL_SCANCODE_KP_1]) kp = 0x02;
    else if (state[SDL_SCANCODE_2] || state[SDL_SCANCODE_KP_2]) kp = 0x08;
    else if (state[SDL_SCANCODE_3] || state[SDL_SCANCODE_KP_3]) kp = 0x03;
    else if (state[SDL_SCANCODE_4] || state[SDL_SCANCODE_KP_4]) kp = 0x0D;
    else if (state[SDL_SCANCODE_5] || state[SDL_SCANCODE_KP_5]) kp = 0x0C;
    else if (state[SDL_SCANCODE_6] || state[SDL_SCANCODE_KP_6]) kp = 0x01;
    else if (state[SDL_SCANCODE_7] || state[SDL_SCANCODE_KP_7]) kp = 0x0A;
    else if (state[SDL_SCANCODE_8] || state[SDL_SCANCODE_KP_8]) kp = 0x0E;
    else if (state[SDL_SCANCODE_9] || state[SDL_SCANCODE_KP_9]) kp = 0x04;
    else if (state[SDL_SCANCODE_0] || state[SDL_SCANCODE_KP_0]) kp = 0x05;
    else if (state[SDL_SCANCODE_MINUS])  kp = 0x06; // '*'
    else if (state[SDL_SCANCODE_EQUALS]) kp = 0x09; // '#'

    // Botón 2: Tecla X (Active-LOW: 0 al pulsar, 0x40 al soltar)
    uint8_t fire2 = (state[SDL_SCANCODE_X]) ? 0x00 : 0x40;

    // Combinación final para el registro del Keypad
    cv_keypad[0] = 0x80 | fire2 | kp;
}

static void process_events(void){
    SDL_Event e;
    while(SDL_PollEvent(&e)){
        if(e.type == SDL_QUIT){ should_quit = true; continue; }
        
        if(e.type == SDL_WINDOWEVENT){
            if(e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) has_focus = true;
            else if(e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) has_focus = false;
            continue;
        }

        if(e.type == SDL_KEYDOWN){
            switch(e.key.keysym.scancode){
                case SDL_SCANCODE_ESCAPE: should_quit = true; break;
                case SDL_SCANCODE_F12:    cv_reset(); break;
                case SDL_SCANCODE_F11:    is_paused = !is_paused; break;
                case SDL_SCANCODE_F1: {
                    char* b = SDL_GetBasePath(); char p[512];
                    snprintf(p, sizeof(p), "%sgame.col", b);
                    cv_load_rom(p); SDL_free(b);
                } break;
                case SDL_SCANCODE_F2:
#ifndef __EMSCRIPTEN__
                    screenshot();
#endif
                    break;
                default: break;
            }
        }
    }
    // Leemos el teclado globalmente en cada iteración
    update_input();
}

static void mainloop(void){
    process_events();
    if(!is_paused && has_focus){
        audio_throttle();
        cv_update();
    }
    cv_render();
}

int main(int argc, char** argv){
#ifndef __EMSCRIPTEN__
    signal(SIGINT, on_sigint);
#endif

    const char* rom_dir = (argc > 1 && argv[1][0] != '-') ? argv[1] : ".";

    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0){
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }

    cv_window = SDL_CreateWindow("ColecoVision Emulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        COLECO_W * COLECO_SCALE, COLECO_H * COLECO_SCALE,
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    cv_renderer = SDL_CreateRenderer(cv_window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(cv_renderer, COLECO_W, COLECO_H);

    cv_texture = SDL_CreateTexture(cv_renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STATIC, COLECO_W, COLECO_H);

    // Audio setup
    SDL_AudioSpec want = {0}, have = {0};
    want.freq = COLECO_AUDIO_RATE; want.format = AUDIO_S16SYS;
    want.channels = 1; want.samples = 512;
    cv_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if(cv_audio_dev) SDL_PauseAudioDevice(cv_audio_dev, 0);

    // Inicializar core
    if(cv_init(rom_dir) < 0){
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Error", 
            "Place coleco.rom in the directory.", cv_window);
        return 1;
    }

    // Cargar ROM por argumento si existe
    for(int i = 1; i < argc; i++){
        const char* ext = strrchr(argv[i], '.');
        if(ext && (!strcasecmp(ext, ".col") || !strcasecmp(ext, ".rom"))) cv_load_rom(argv[i]);
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(mainloop, 0, 1);
#else
    while(!should_quit) mainloop();
#endif

    // Cleanup
    cv_quit();
    if(cv_audio_dev) SDL_CloseAudioDevice(cv_audio_dev);
    SDL_DestroyTexture(cv_texture);
    SDL_DestroyRenderer(cv_renderer);
    SDL_DestroyWindow(cv_window);
    SDL_Quit();
    return 0;
}