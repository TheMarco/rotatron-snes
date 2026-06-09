/* Shared scalar types: snes.h on target, stdint on host (golden tests). */
#ifndef CORE_TYPES_H
#define CORE_TYPES_H

#ifdef HOST_BUILD
#include <stdint.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
#else
#include <snes.h>
#endif

#endif
