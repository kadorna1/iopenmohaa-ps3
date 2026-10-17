/* ps3_ctor_trace.c -- tells "died before the C++ constructors ran" apart from
 * "a constructor died" apart from "died after main() started".
 *
 * Reading earlytrace.log after a run:
 *   no "CTOR: first" line     -> failure before any user constructor: the
 *                                loader, SELF/ELF layout, TOC, CRT startup.
 *   "CTOR: first" but no
 *   "BOOT: main entry" line   -> one of the engine's static constructors
 *                                crashed (or hung).
 *   "BOOT: main entry" seen   -> constructors are fine, read on from there.
 *
 * Priority 101 is the earliest a user constructor may ask for, so this runs
 * ahead of the engine's default-priority (65535) ones.
 */
#include "ps3_log.h"

__attribute__((constructor(101)))
static void ps3_ctor_first(void)
{
    PS3_BootTracef("CTOR: first constructor (priority 101) reached");
}
