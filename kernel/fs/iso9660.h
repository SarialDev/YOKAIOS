#pragma once
#include "types.h"

// ISO 9660 read-only cu Rock Ridge de baza (NM, PX, SL, TF, CE, CL/RE) si SUSP. Tip de sistem de fisiere "iso9660";
// sursa montarii = numele unui dispozitiv bloc din HAL ("sr0" = CD/DVD pe AHCI, dar merge si o imagine pe disc).
// Optiuni (data): "ro" (implicit oricum), "norr" (ignora Rock Ridge, nume ISO in litere mici fara ";1").
// Fara Joliet, fara El Torito, fisierele multi-extent se vad doar cu primul extent.
void iso9660_init();

// Monteaza pe /media/cdrom primul dispozitiv "sr*" din HAL care contine un volum ISO 9660 valid.
// 0, -ENODEV daca nu exista nici un sr*, altfel -errno al ultimei montari esuate
int iso9660_automount();
