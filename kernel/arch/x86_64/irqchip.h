#pragma once

// Inregistreaza in HAL controlerul 8259 (dupa pic_init)
void irqchip_pic_register();
// Inregistreaza LAPIC+IOAPIC (dupa lapic_init si ioapic_init); devine controlerul activ
void irqchip_apic_register();
