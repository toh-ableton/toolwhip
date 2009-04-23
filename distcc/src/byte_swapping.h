/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 * Copyright 2009 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
*/

/* Authors: Mark Mentovai */

/*
 * byte_swapping.h:
 * Performs byte swapping operations.
 */


#ifndef DISTCC_BYTE_SWAPPING_H__
#define DISTCC_BYTE_SWAPPING_H__

#include <stdint.h>

/* Use OS-provided swapping functions when they're known to be available. */

inline uint32_t maybe_swap_32(int swap, uint32_t x);
inline uint64_t maybe_swap_64(int swap, uint64_t x);
inline uint32_t swap_big_to_cpu_32(uint32_t x);
inline uint64_t swap_big_to_cpu_64(uint64_t x);

#if defined(__APPLE__)

#include <libkern/OSByteOrder.h>

inline uint32_t maybe_swap_32(int swap, uint32_t x) {
  if (swap)
    return OSSwapInt32(x);
  return x;
}

inline uint64_t maybe_swap_64(int swap, uint64_t x) {
  if (swap)
    return OSSwapInt64(x);
  return x;
}

inline uint32_t swap_big_to_cpu_32(uint32_t x) {
  return OSSwapBigToHostInt32(x);
}

inline uint64_t swap_big_to_cpu_64(uint64_t x) {
  return OSSwapBigToHostInt64(x);
}

#elif defined(linux)

#include <asm/byteorder.h>

inline uint32_t maybe_swap_32(int swap, uint32_t x) {
  if (swap)
    return __swab32(x);
  return x;
}

inline uint64_t maybe_swap_64(int swap, uint64_t x) {
  if (swap)
    return __swab64(x);
  return x;
}

inline uint32_t swap_big_to_cpu_32(uint32_t x) {
  return __be32_to_cpu(x);
}

inline uint64_t swap_big_to_cpu_64(uint64_t x) {
  return __be64_to_cpu(x);
}

#else  /* !apple & !linux */

/* If other systems provide swapping functions, they should be used in
 * preference to this fallback code. */

inline uint32_t maybe_swap_32(int swap, uint32_t x) {
  if (!swap)
    return x;

  return  (x >> 24) |
         ((x >> 8) & 0x0000ff00) |
         ((x << 8) & 0x00ff0000) |
          (x << 24);
}

inline uint64_t maybe_swap_64(int swap, uint64_t x) {
  if (!swap)
    return x;

  uint32_t* x32 = (uint32_t*)&x;
  uint64_t y = maybe_swap_32(swap, x32[0]);
  uint64_t z = maybe_swap_32(swap, x32[1]);
  return (z << 32) | y;
}

inline uint32_t swap_big_to_cpu_32(uint32_t x) {
#ifdef WORDS_BIGENDIAN
  return x;
#else
  return maybe_swap_32(1, x);
#endif
}

inline uint64_t swap_big_to_cpu_64(uint64_t x) {
#ifdef WORDS_BIGENDIAN
  return x;
#else
  return maybe_swap_64(1, x);
#endif
}

#endif /* !apple & !linux */

#endif  /* DISTCC_BYTE_SWAPPING_H__ */
