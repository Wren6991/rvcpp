#ifndef _RV_TYPES_H
#define _RV_TYPES_H

#include <cstdint>

enum {XLEN = 32};
typedef uint32_t ux_t;
typedef int32_t sx_t;
typedef unsigned int uint;

typedef int64_t sdx_t;

// Inclusive msb:lsb style, like Verilog (and like the ISA manual)
#define BITS_UPTO(msb) (~((-1u << (msb)) << 1))
#define BITRANGE(msb, lsb) (BITS_UPTO((msb) - (lsb)) << (lsb))
#define GETBITS(x, msb, lsb) (((x) & BITRANGE(msb, lsb)) >> (lsb))
#define GETBIT(x, bit) (((x) >> (bit)) & 1u)

#endif
