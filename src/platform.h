#ifndef PLATFORM_SETUP_H
#define PLATFORM_SETUP_H

#ifndef _CRT_SECURE_NO_WARNINGS
    #define _CRT_SECURE_NO_WARNINGS
#endif

#include <stdio.h>

//-----------------------------------------------------------------------

#if defined(__EMSCRIPTEN__)
#   include <emscripten/emscripten.h>
#   include <emscripten/html5.h>
#endif

/* PSYQ's r3000.h defines bare lowercase macros for MIPS assembler mnemonics
 * (lcache/scache/flush/inval). They are unused by the PC port and collide
 * with SDL3 identifiers — notably SDL_IOStreamInterface's `flush` member,
 * which a stray `#define flush lwr $0,` turns into a syntax error. Undef
 * them before pulling in SDL3 so its headers parse regardless of whether a
 * PSYQ header was included first in this translation unit. */
#undef lcache
#undef scache
#undef flush
#undef inval
#include <SDL3/SDL.h>

//-----------------------------------------------------------------------

#ifdef _MSC_VER
#define FUNCNAME __FUNCTION__
#else
#define FUNCNAME __func__
#endif

#include "PsyX/PsyX_globals.h"

#if defined(__ANDROID__)
#   include <android/log.h>
#   define LOG_TAG_EMU "[PsyX] "
#   define eprintf(fmt, ...)       __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG_EMU, fmt, ##__VA_ARGS__)
#   define eprintinfof(fmt, ...)   __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG_EMU " [INFO]", fmt, ##__VA_ARGS__)
#   define eprintwarnf(fmt, ...)   __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG_EMU " [WARN]", fmt, ##__VA_ARGS__)
#   define eprinterr(fmt, ...)     eprintf("[%s] - " fmt, FUNCNAME, ##__VA_ARGS__);
#else
#   define eprintf(fmt, ...)       PsyX_Log("[Psy-X] " fmt, ##__VA_ARGS__)
#   define eprintinfo(fmt, ...)    PsyX_Log_Info("[Psy-X] " fmt, ##__VA_ARGS__)
#   define eprintwarn(fmt, ...)    PsyX_Log_Warning("[Psy-X] " fmt, ##__VA_ARGS__)
#   define eprinterr(fmt, ...)     PsyX_Log_Error("[Psy-X] [%s] - " fmt, FUNCNAME, ##__VA_ARGS__);
#endif

//-----------------------------------------------------------------------

#if defined(__EMSCRIPTEN__) || !defined(_DEBUG)
#   define PSYX_UNIMPLEMENTED()
#else
#   define PSYX_UNIMPLEMENTED() eprinterr("Unimplemented!\n");
#endif

#endif