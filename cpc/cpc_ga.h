/*
 * cpc_ga.h  –  Interfaz pública del Gate Array reescrito según CPCEC
 *              (https://github.com/cpcitor/cpcec)
 *
 * Este módulo reemplaza la implementación inline de cpc.c con un enfoque
 * modular que sigue fielmente las técnicas de CPCEC:
 *
 *   - Tablas de lookup precalculadas (gate_mode0, gate_mode1) idénticas a CPCEC
 *   - CLUT video_clut[32] con actualización diferida (dirty flag)
 *   - Corrección gamma 1.6 por canal (igual que video_table[] en CPCEC)
 *   - Renderizado con video_target (puntero lineal al framebuffer)
 *   - Lógica IRQ/VSYNC documentada con los mismos invariantes que CPCEC
 */

#ifndef CPC_GA_H
#define CPC_GA_H

#include <stdint.h>
#include <stdbool.h>

/* ── Inicialización ───────────────────────────────────────────────────────── */

/**
 * ga_init() – Inicializa tablas de lookup y paleta.
 *
 * Debe llamarse UNA VEZ al arrancar el emulador, antes de cualquier
 * otra función de este módulo.
 *
 * Construye:
 *   · ga_video_table[32]   – paleta hardware con gamma 1.6
 *   · gate_mode0[2][256]   – decodificación Modo 0 (4 bits/píxel)
 *   · gate_mode1[4][256]   – decodificación Modo 1 (2 bits/píxel)
 */
void ga_init(void);

/**
 * ga_reset() – Resetea el estado del Gate Array al estado inicial del CPC.
 *
 * Establece tintas por defecto del firmware, modo 1, CLUT sucia para
 * recálculo, contadores a cero.
 */
void ga_reset(void);

/* ── I/O del Gate Array ───────────────────────────────────────────────────── */

/**
 * ga_write_port() – Decodifica y procesa una escritura en el GA.
 *
 * @param port  Puerto de 16 bits (debe tener A15=0, A14=1, i.e. 0x7Fxx)
 * @param val   Byte de dato escrito
 *
 * Función bits 7:6 del dato:
 *   00 → seleccionar lápiz (0-16)
 *   01 → asignar color al lápiz seleccionado
 *   10 → registro de control (modo, ROM, reset IRQ)
 *   11 → configuración de RAM (CPC 6128)
 */
void ga_write_port(uint16_t port, uint8_t val);

/* ── Frame timing ─────────────────────────────────────────────────────────── */

/**
 * ga_frame_start() – Prepara el GA al inicio de un nuevo frame.
 *
 * Resetea el contador de líneas y pre-rellena gate_mode_per_line[].
 * Llamar al inicio de cpc_update() antes del bucle de ejecución Z80.
 */
void ga_frame_start(void);

/**
 * ga_hsync() – Notifica al GA de un pulso de HSYNC del CRTC.
 *
 * @param vsync_line  Línea del CRTC en que comienza el VSYNC (R7)
 * @param vsync_dur   Duración del VSYNC en líneas (R3[7:4])
 * @return            true si se generó una IRQ en este HSYNC
 *
 * Implementa:
 *   · Registro del modo activo para la línea actual (mid-line mode changes)
 *   · Activación/desactivación de VSYNC
 *   · Resincronización del contador IRQ en el flanco de VSYNC
 *     (<32 → 0, >=32 → 32, idéntico a CPCEC)
 *   · Incremento y desbordamiento del contador de 52 HSYNCs
 */
bool ga_hsync(uint32_t vsync_line, uint32_t vsync_dur);

/**
 * ga_irq_pending() – Consulta si hay una IRQ pendiente del GA.
 */
bool ga_irq_pending(void);

/**
 * ga_irq_acknowledge() – Reconoce (consume) la IRQ pendiente.
 *
 * Llamar inmediatamente después de entregar la IRQ al Z80.
 */
void ga_irq_acknowledge(void);

/**
 * ga_vsync_active() – Devuelve true mientras el VSYNC está activo.
 *
 * Usado por la PPI para el bit 7 del puerto B (activo bajo en hardware,
 * la lógica de inversión está en la PPI).
 */
bool ga_vsync_active(void);

/* ── Renderizado ──────────────────────────────────────────────────────────── */

/**
 * ga_render_frame() – Renderiza un frame completo al framebuffer.
 *
 * @param crtc_reg  Array de 18 registros CRTC
 * @param vram      Puntero al banco de VRAM (banco físico 3, 16K)
 * @param pixels    Framebuffer de destino (CPC_H * CPC_W entradas uint32_t ARGB)
 *
 * Algoritmo (según CPCEC):
 *   1. Actualizar CLUT si hay cambios pendientes (dirty flag)
 *   2. Rellenar borde superior
 *   3. Para cada línea activa:
 *      a. Obtener modo de gate_mode_per_line[] (soporta mid-line changes)
 *      b. Calcular dirección de línea CRTC con interleave
 *      c. Dibujar borde izquierdo
 *      d. Decodificar bytes VRAM con tablas gate_mode0/gate_mode1
 *      e. Dibujar borde derecho
 *   4. Rellenar borde inferior
 */
void ga_render_frame(const uint8_t *crtc_reg,
                     const uint8_t *vram,
                     uint32_t      *pixels);

/* ── Snapshots ────────────────────────────────────────────────────────────── */

/**
 * ga_snapshot_load() – Restaura el estado del GA desde una cabecera SNA.
 *
 * @param hdr          Cabecera SNA de 256 bytes
 * @param version      Versión del snapshot (1, 2 ó 3)
 * @param crtc_reg_out Buffer de salida para los registros CRTC (puede ser NULL)
 *
 * Offsets leídos (según spec oficial, igual que CPCEC snap_load()):
 *   0x2E       lápiz seleccionado
 *   0x2F-0x3F  tabla de tintas (17 bytes)
 *   0x40       registro de control (modo, ROM)
 *   0x41       configuración de RAM
 *   0xB2       (v3) delay VSYNC
 *   0xB3       (v3) contador IRQ
 *   0xB4       (v3) IRQ pendiente
 */
void ga_snapshot_load(const uint8_t *hdr, uint8_t version,
                      uint8_t *crtc_reg_out);

/* ── Consultas de color ───────────────────────────────────────────────────── */

/**
 * ga_get_border_color() – Color ARGB del borde actual (CLUT[16]).
 */
uint32_t ga_get_border_color(void);

/**
 * ga_get_ink_color() – Color ARGB de una tinta (0-15).
 */
uint32_t ga_get_ink_color(int ink);

#endif /* CPC_GA_H */
