/*
 * /dev/titra/advice: what the nurse station reads and asks of the advisor. read() gives one JSON line; devctl asks for
 * a shift handover or stores a nurse's note on a cot. Nothing here reaches a controller.
 */
#ifndef TITRA_ADVISOR_PROTO_H
#define TITRA_ADVISOR_PROTO_H

#include <devctl.h>

typedef struct {
    int cot;
    char text[120];
} advice_note_t;

#define DCMD_ADVICE_HANDOVER __DION(_DCMD_MISC, 0x70)
#define DCMD_ADVICE_NOTE __DIOT(_DCMD_MISC, 0x71, advice_note_t)

#endif
