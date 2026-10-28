#pragma once
#include "types.h"

const uint8_t LAPIC_SPURIOUS_VECTOR = 0xFF;

// Mapeaza LAPIC-ul de la adresa din MADT si il porneste prin SVR
void lapic_init(uint64_t phys);
// Porneste LAPIC-ul CPU-ului curent (AP), mapat deja de lapic_init
void lapic_init_ap();
bool lapic_enabled();
void lapic_eoi();
uint32_t lapic_id();

// Numara tick-urile timerului LAPIC (divizor 16) cat dureaza wait_ms(ms)
uint32_t lapic_timer_measure(void (*wait_ms)(uint32_t), uint32_t ms);
// Mod periodic: intrerupere pe vector la fiecare `ticks` tick-uri (divizor 16)
void lapic_timer_periodic(uint32_t ticks, uint8_t vector);
// Mod one-shot (divizor 16): lapic_timer_oneshot o data, apoi lapic_timer_arm pentru fiecare eveniment.
// O noua armare anuleaza evenimentul anterior
void lapic_timer_oneshot(uint8_t vector);
void lapic_timer_arm(uint32_t ticks);
// Opreste evenimentul armat (one-shot)
void lapic_timer_stop();

// IPI fix catre LAPIC-ul dest; asteapta sa fie livrat (delivery status)
void lapic_send_ipi(uint32_t dest, uint8_t vector);
// Pornirea unui AP: INIT, apoi STARTUP cu vectorul = pagina fizica a trampolinei (0x8000 -> 0x08)
void lapic_send_init(uint32_t dest);
void lapic_send_startup(uint32_t dest, uint8_t page);
// NMI catre toate celelalte CPU-uri (panic)
void lapic_send_nmi_others();
