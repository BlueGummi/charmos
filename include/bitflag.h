/* @title: Bit flag macros */
#pragma once
#include <compiler/core.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

/* Various code around the kernel is a big fan of having bit flags
 * that are somewhat like the following:
 *
 * [ some available bits ] [ some reserved bits ] [ some bits in use ].
 *
 * This is all good and well, but the issue is that these bit flags
 * do not have a unified substrate for actually declaring variables
 * for the available bits and reserved bits, and end up handrolling
 * oddities to accomplish this. This is not quite good, so bitflag.h
 * serves to fix this.
 *
 * The idea:
 *
 * BITFLAG_RESERVED_DECLARE(type, bit)
 *
 */
