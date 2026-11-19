/* adbng - a clean-room ADB device implementation
 * SPDX-License-Identifier: MIT
 *
 * Only the wire format is used here, as specified by the public ADB
 * protocol (24-byte little-endian header + payload).
 */
#ifndef ADBNG_PROTO_H
#define ADBNG_PROTO_H

#include <stdint.h>
#include <stddef.h>

#define A_VERSION   0x01000001u
#define A_MAXDATA   262144u          /* 256 KiB */

#define A_SYNC  0x434e5953u
#define A_CNXN  0x4e584e43u
#define A_OPEN  0x4e45504fu
#define A_OKAY  0x59414b4fu
#define A_CLSE  0x45534c43u
#define A_WRTE  0x45545257u
#define A_AUTH  0x48545541u
#define A_STLS  0x534c5453u

#define AUTH_TOKEN        1
#define AUTH_SIGNATURE    2
#define AUTH_RSAPUBLICKEY 3

#define TOKEN_SIZE   20
#define RSA_SIG_SIZE 256

struct amessage {
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;
} __attribute__((packed));

#define AMSG_SIZE 24

static inline uint32_t amsg_magic(uint32_t cmd) { return cmd ^ 0xffffffffu; }

static inline const char *amsg_name(uint32_t cmd)
{
    switch (cmd) {
    case A_SYNC: return "SYNC";
    case A_CNXN: return "CNXN";
    case A_OPEN: return "OPEN";
    case A_OKAY: return "OKAY";
    case A_CLSE: return "CLSE";
    case A_WRTE: return "WRTE";
    case A_AUTH: return "AUTH";
    case A_STLS: return "STLS";
    default:     return "????";
    }
}

#endif