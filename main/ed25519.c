/*
 * ed25519.c -- Ed25519 signing (sign only, no verify)
 *
 * Based on public domain NaCl/SUPERCOP ref10 by:
 *   Daniel J. Bernstein, Niels Duif, Tanja Lange, Peter Schwabe, Bo-Yin Yang
 * Source: https://hyperelliptic.org/EFD/g1p/auto-twisted-extended-1.html
 *         https://nacl.cr.yp.to/  (public domain)
 *
 * Adaptation for ESP32-P4 (RISC-V 32-bit) and mbedTLS SHA-512:
 *   - Pure C, no inline assembly
 *   - All 64-bit arithmetic uses int64_t (compiler-generated, no __int128)
 *   - Uses mbedtls_sha512() for all hashing
 *
 * MIT licence for the adaptation layer.
 * Original ref10 code is public domain.
 */

#include "ed25519.h"
#include "mbedtls/sha512.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"
#include "esp_random.h"
#include <string.h>
#include <stdint.h>

/* RNG adapter required by mbedtls_ecp_mul (Montgomery ladder) */
static int ecp_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

/* =========================================================================
 * Utility: little-endian byte loaders
 * =========================================================================*/

static uint32_t load_3(const uint8_t *in)
{
    return (uint32_t)in[0]
         | ((uint32_t)in[1] << 8)
         | ((uint32_t)in[2] << 16);
}

static uint32_t load_4(const uint8_t *in)
{
    return (uint32_t)in[0]
         | ((uint32_t)in[1] <<  8)
         | ((uint32_t)in[2] << 16)
         | ((uint32_t)in[3] << 24);
}

/* =========================================================================
 * SECTION 1 -- Field arithmetic  GF(2^255 - 19)
 *
 * A field element h is stored as 10 signed 32-bit limbs in the alternating
 * radix 2^26 / 2^25 representation (ref10):
 *
 *   value = h[0]         (26-bit limb, radix 2^0)
 *         + h[1] * 2^26  (25-bit limb)
 *         + h[2] * 2^51  (26-bit limb)
 *         ...
 *         + h[9] * 2^230 (25-bit limb)
 * =========================================================================*/

typedef int32_t fe[10];

static void fe_0(fe h)        { memset(h, 0, sizeof(fe)); }
static void fe_1(fe h)        { memset(h, 0, sizeof(fe)); h[0] = 1; }
static void fe_copy(fe h, const fe f) { memcpy(h, f, sizeof(fe)); }

static void fe_add(fe h, const fe f, const fe g)
{
    int i;
    for (i = 0; i < 10; i++) h[i] = f[i] + g[i];
}

static void fe_sub(fe h, const fe f, const fe g)
{
    int i;
    for (i = 0; i < 10; i++) h[i] = f[i] - g[i];
}

static void __attribute__((unused)) fe_neg(fe h, const fe f)
{
    int i;
    for (i = 0; i < 10; i++) h[i] = -f[i];
}

/* Load 32 bytes (little-endian) into a field element */
static void __attribute__((unused)) fe_frombytes(fe h, const uint8_t *s)
{
    int64_t h0 = load_3(s);
    int64_t h1 = (int64_t)(load_4(s+ 3) >> 6);  /* bits 24..48 */
    int64_t h2 = (int64_t)(load_3(s+ 6) >> 5);  /* bits 46..68 */
    int64_t h3 = (int64_t)(load_3(s+ 9) >> 3);  /* bits 67..89 */
    int64_t h4 = (int64_t)(load_3(s+12) >> 1);  /* bits 88..109 */
    int64_t h5 = (int64_t)(load_4(s+15) >> 7);  /* bits 113..137 */
    int64_t h6 = (int64_t)(load_3(s+19) >> 5);  /* bits 135..157 */
    int64_t h7 = (int64_t)(load_3(s+22) >> 4);  /* bits 156..178 */
    int64_t h8 = (int64_t)(load_3(s+25) >> 2);  /* bits 177..199 */
    int64_t h9 = (int64_t)(load_3(s+28) & 0x7fffff); /* bits 200..222 */

    /* Mask to the actual limb sizes */
    h1 &= 0x1ffffff;   /* 25 bits */
    h2 &= 0x3ffffff;   /* 26 bits */
    h3 &= 0x1ffffff;   /* 25 bits */
    h4 &= 0x3ffffff;   /* 26 bits */
    h5 &= 0x1ffffff;   /* 25 bits */
    h6 &= 0x3ffffff;   /* 26 bits */
    h7 &= 0x1ffffff;   /* 25 bits */
    h8 &= 0x3ffffff;   /* 26 bits */
    h9 &= 0x1ffffff;   /* 25 bits -- top byte cleared below */

    /* Clear the high bit that was the sign of y */
    h9 &= (int64_t)((1L << 23) - 1);

    /* Carry-reduce */
    {
        int64_t c;
        c = (h9 + (1L<<24)) >> 25; h0 += c*19; h9 -= c*(1L<<25);
        c = (h1 + (1L<<24)) >> 25; h2 += c;    h1 -= c*(1L<<25);
        c = (h3 + (1L<<24)) >> 25; h4 += c;    h3 -= c*(1L<<25);
        c = (h5 + (1L<<24)) >> 25; h6 += c;    h5 -= c*(1L<<25);
        c = (h7 + (1L<<24)) >> 25; h8 += c;    h7 -= c*(1L<<25);
        c = (h0 + (1L<<25)) >> 26; h1 += c;    h0 -= c*(1L<<26);
        c = (h2 + (1L<<25)) >> 26; h3 += c;    h2 -= c*(1L<<26);
        c = (h4 + (1L<<25)) >> 26; h5 += c;    h4 -= c*(1L<<26);
        c = (h6 + (1L<<25)) >> 26; h7 += c;    h6 -= c*(1L<<26);
        c = (h8 + (1L<<25)) >> 26; h9 += c;    h8 -= c*(1L<<26);
    }

    h[0]=(int32_t)h0; h[1]=(int32_t)h1; h[2]=(int32_t)h2;
    h[3]=(int32_t)h3; h[4]=(int32_t)h4; h[5]=(int32_t)h5;
    h[6]=(int32_t)h6; h[7]=(int32_t)h7; h[8]=(int32_t)h8;
    h[9]=(int32_t)h9;
}

/* Store a field element into 32 bytes, canonical little-endian */
static void fe_tobytes(uint8_t *s, const fe h)
{
    int32_t h0=h[0], h1=h[1], h2=h[2], h3=h[3], h4=h[4];
    int32_t h5=h[5], h6=h[6], h7=h[7], h8=h[8], h9=h[9];
    int32_t q;
    int64_t carry0,carry1,carry2,carry3,carry4,
            carry5,carry6,carry7,carry8,carry9;

    /* Determine quotient: number of times p fits */
    q = (19*h9 + (1<<24)) >> 25;
    q = (h0 + q) >> 26;
    q = (h1 + q) >> 25;
    q = (h2 + q) >> 26;
    q = (h3 + q) >> 25;
    q = (h4 + q) >> 26;
    q = (h5 + q) >> 25;
    q = (h6 + q) >> 26;
    q = (h7 + q) >> 25;
    q = (h8 + q) >> 26;
    q = (h9 + q) >> 25;

    /* Subtract q*p */
    h0 += 19*q;

    carry0 = h0>>26; h1+=(int32_t)carry0; h0-=(int32_t)(carry0*(1L<<26));
    carry1 = h1>>25; h2+=(int32_t)carry1; h1-=(int32_t)(carry1*(1L<<25));
    carry2 = h2>>26; h3+=(int32_t)carry2; h2-=(int32_t)(carry2*(1L<<26));
    carry3 = h3>>25; h4+=(int32_t)carry3; h3-=(int32_t)(carry3*(1L<<25));
    carry4 = h4>>26; h5+=(int32_t)carry4; h4-=(int32_t)(carry4*(1L<<26));
    carry5 = h5>>25; h6+=(int32_t)carry5; h5-=(int32_t)(carry5*(1L<<25));
    carry6 = h6>>26; h7+=(int32_t)carry6; h6-=(int32_t)(carry6*(1L<<26));
    carry7 = h7>>25; h8+=(int32_t)carry7; h7-=(int32_t)(carry7*(1L<<25));
    carry8 = h8>>26; h9+=(int32_t)carry8; h8-=(int32_t)(carry8*(1L<<26));
    carry9 = h9>>25;                       h9-=(int32_t)(carry9*(1L<<25));

    s[ 0] = (uint8_t)( h0        );
    s[ 1] = (uint8_t)( h0 >> 8   );
    s[ 2] = (uint8_t)( h0 >> 16  );
    s[ 3] = (uint8_t)((h0 >> 24) | (h1 << 2));
    s[ 4] = (uint8_t)( h1 >> 6   );
    s[ 5] = (uint8_t)( h1 >> 14  );
    s[ 6] = (uint8_t)((h1 >> 22) | (h2 << 3));
    s[ 7] = (uint8_t)( h2 >> 5   );
    s[ 8] = (uint8_t)( h2 >> 13  );
    s[ 9] = (uint8_t)((h2 >> 21) | (h3 << 5));
    s[10] = (uint8_t)( h3 >> 3   );
    s[11] = (uint8_t)( h3 >> 11  );
    s[12] = (uint8_t)((h3 >> 19) | (h4 << 6));
    s[13] = (uint8_t)( h4 >> 2   );
    s[14] = (uint8_t)( h4 >> 10  );
    s[15] = (uint8_t)( h4 >> 18  );
    s[16] = (uint8_t)( h5        );
    s[17] = (uint8_t)( h5 >> 8   );
    s[18] = (uint8_t)( h5 >> 16  );
    s[19] = (uint8_t)((h5 >> 24) | (h6 << 1));
    s[20] = (uint8_t)( h6 >> 7   );
    s[21] = (uint8_t)( h6 >> 15  );
    s[22] = (uint8_t)((h6 >> 23) | (h7 << 3));
    s[23] = (uint8_t)( h7 >> 5   );
    s[24] = (uint8_t)( h7 >> 13  );
    s[25] = (uint8_t)((h7 >> 21) | (h8 << 4));
    s[26] = (uint8_t)( h8 >> 4   );
    s[27] = (uint8_t)( h8 >> 12  );
    s[28] = (uint8_t)((h8 >> 20) | (h9 << 6));
    s[29] = (uint8_t)( h9 >> 2   );
    s[30] = (uint8_t)( h9 >> 10  );
    s[31] = (uint8_t)( h9 >> 18  );
}

/* h = f * g  (schoolbook with cross-term folding, all via int64_t) */
static void fe_mul(fe h, const fe f, const fe g)
{
    int32_t f0=f[0],f1=f[1],f2=f[2],f3=f[3],f4=f[4];
    int32_t f5=f[5],f6=f[6],f7=f[7],f8=f[8],f9=f[9];
    int32_t g0=g[0],g1=g[1],g2=g[2],g3=g[3],g4=g[4];
    int32_t g5=g[5],g6=g[6],g7=g[7],g8=g[8],g9=g[9];

    int32_t f1_2=2*f1, f3_2=2*f3, f5_2=2*f5, f7_2=2*f7, f9_2=2*f9;
    /* Use int64_t to avoid overflow when inputs are unreduced (e.g. from fe_add).
     * 19 * 2^27 = 2.55e9 > INT32_MAX; must widen before multiplying. */
    int64_t g1_19=(int64_t)19*g1, g2_19=(int64_t)19*g2, g3_19=(int64_t)19*g3, g4_19=(int64_t)19*g4;
    int64_t g5_19=(int64_t)19*g5, g6_19=(int64_t)19*g6, g7_19=(int64_t)19*g7, g8_19=(int64_t)19*g8, g9_19=(int64_t)19*g9;

    int64_t h0,h1,h2,h3,h4,h5,h6,h7,h8,h9;
    int64_t c0,c1,c2,c3,c4,c5,c6,c7,c8,c9;

    h0 = (int64_t)f0*g0   + (int64_t)f1_2*g9_19 + (int64_t)f2*g8_19
       + (int64_t)f3_2*g7_19 + (int64_t)f4*g6_19 + (int64_t)f5_2*g5_19
       + (int64_t)f6*g4_19 + (int64_t)f7_2*g3_19 + (int64_t)f8*g2_19
       + (int64_t)f9_2*g1_19;
    h1 = (int64_t)f0*g1   + (int64_t)f1*g0      + (int64_t)f2*g9_19
       + (int64_t)f3*g8_19  + (int64_t)f4*g7_19 + (int64_t)f5*g6_19
       + (int64_t)f6*g5_19  + (int64_t)f7*g4_19 + (int64_t)f8*g3_19
       + (int64_t)f9*g2_19;
    h2 = (int64_t)f0*g2   + (int64_t)f1_2*g1    + (int64_t)f2*g0
       + (int64_t)f3_2*g9_19 + (int64_t)f4*g8_19 + (int64_t)f5_2*g7_19
       + (int64_t)f6*g6_19 + (int64_t)f7_2*g5_19 + (int64_t)f8*g4_19
       + (int64_t)f9_2*g3_19;
    h3 = (int64_t)f0*g3   + (int64_t)f1*g2      + (int64_t)f2*g1
       + (int64_t)f3*g0   + (int64_t)f4*g9_19   + (int64_t)f5*g8_19
       + (int64_t)f6*g7_19 + (int64_t)f7*g6_19  + (int64_t)f8*g5_19
       + (int64_t)f9*g4_19;
    h4 = (int64_t)f0*g4   + (int64_t)f1_2*g3    + (int64_t)f2*g2
       + (int64_t)f3_2*g1  + (int64_t)f4*g0      + (int64_t)f5_2*g9_19
       + (int64_t)f6*g8_19 + (int64_t)f7_2*g7_19 + (int64_t)f8*g6_19
       + (int64_t)f9_2*g5_19;
    h5 = (int64_t)f0*g5   + (int64_t)f1*g4      + (int64_t)f2*g3
       + (int64_t)f3*g2   + (int64_t)f4*g1      + (int64_t)f5*g0
       + (int64_t)f6*g9_19 + (int64_t)f7*g8_19  + (int64_t)f8*g7_19
       + (int64_t)f9*g6_19;
    h6 = (int64_t)f0*g6   + (int64_t)f1_2*g5    + (int64_t)f2*g4
       + (int64_t)f3_2*g3  + (int64_t)f4*g2      + (int64_t)f5_2*g1
       + (int64_t)f6*g0   + (int64_t)f7_2*g9_19  + (int64_t)f8*g8_19
       + (int64_t)f9_2*g7_19;
    h7 = (int64_t)f0*g7   + (int64_t)f1*g6      + (int64_t)f2*g5
       + (int64_t)f3*g4   + (int64_t)f4*g3      + (int64_t)f5*g2
       + (int64_t)f6*g1   + (int64_t)f7*g0      + (int64_t)f8*g9_19
       + (int64_t)f9*g8_19;
    h8 = (int64_t)f0*g8   + (int64_t)f1_2*g7    + (int64_t)f2*g6
       + (int64_t)f3_2*g5  + (int64_t)f4*g4      + (int64_t)f5_2*g3
       + (int64_t)f6*g2   + (int64_t)f7_2*g1     + (int64_t)f8*g0
       + (int64_t)f9_2*g9_19;
    h9 = (int64_t)f0*g9   + (int64_t)f1*g8      + (int64_t)f2*g7
       + (int64_t)f3*g6   + (int64_t)f4*g5      + (int64_t)f5*g4
       + (int64_t)f6*g3   + (int64_t)f7*g2      + (int64_t)f8*g1
       + (int64_t)f9*g0;

    c0=(h0+(1L<<25))>>26; h1+=c0; h0-=c0*(1L<<26);
    c4=(h4+(1L<<25))>>26; h5+=c4; h4-=c4*(1L<<26);
    c1=(h1+(1L<<24))>>25; h2+=c1; h1-=c1*(1L<<25);
    c5=(h5+(1L<<24))>>25; h6+=c5; h5-=c5*(1L<<25);
    c2=(h2+(1L<<25))>>26; h3+=c2; h2-=c2*(1L<<26);
    c6=(h6+(1L<<25))>>26; h7+=c6; h6-=c6*(1L<<26);
    c3=(h3+(1L<<24))>>25; h4+=c3; h3-=c3*(1L<<25);
    c7=(h7+(1L<<24))>>25; h8+=c7; h7-=c7*(1L<<25);
    c4=(h4+(1L<<25))>>26; h5+=c4; h4-=c4*(1L<<26);
    c8=(h8+(1L<<25))>>26; h9+=c8; h8-=c8*(1L<<26);
    c9=(h9+(1L<<24))>>25; h0+=c9*19; h9-=c9*(1L<<25);
    c0=(h0+(1L<<25))>>26; h1+=c0; h0-=c0*(1L<<26);

    h[0]=(int32_t)h0; h[1]=(int32_t)h1; h[2]=(int32_t)h2; h[3]=(int32_t)h3;
    h[4]=(int32_t)h4; h[5]=(int32_t)h5; h[6]=(int32_t)h6; h[7]=(int32_t)h7;
    h[8]=(int32_t)h8; h[9]=(int32_t)h9;
}

/* h = f^2 */
static void fe_sq(fe h, const fe f)
{
    int32_t f0=f[0],f1=f[1],f2=f[2],f3=f[3],f4=f[4];
    int32_t f5=f[5],f6=f[6],f7=f[7],f8=f[8],f9=f[9];

    int32_t f0_2=2*f0, f1_2=2*f1, f2_2=2*f2, f3_2=2*f3, f4_2=2*f4;
    int32_t f5_2=2*f5, f6_2=2*f6, f7_2=2*f7;
    /* Use int64_t: 38 * 2^26 = 2.56e9 > INT32_MAX; widen before multiplying. */
    int64_t f5_38=(int64_t)38*f5, f6_19=(int64_t)19*f6, f7_38=(int64_t)38*f7;
    int64_t f8_19=(int64_t)19*f8, f9_38=(int64_t)38*f9;

    int64_t h0,h1,h2,h3,h4,h5,h6,h7,h8,h9;
    int64_t c0,c1,c2,c3,c4,c5,c6,c7,c8,c9;

    h0 = (int64_t)f0*f0    + (int64_t)f1_2*f9_38 + (int64_t)f2_2*f8_19
       + (int64_t)f3_2*f7_38 + (int64_t)f4_2*f6_19 + (int64_t)f5*f5_38;
    h1 = (int64_t)f0_2*f1  + (int64_t)f2*f9_38   + (int64_t)f3_2*f8_19
       + (int64_t)f4*f7_38 + (int64_t)f5_2*f6_19;
    h2 = (int64_t)f0_2*f2  + (int64_t)f1_2*f1    + (int64_t)f3_2*f9_38
       + (int64_t)f4_2*f8_19 + (int64_t)f5_2*f7_38 + (int64_t)f6*f6_19;
    h3 = (int64_t)f0_2*f3  + (int64_t)f1_2*f2    + (int64_t)f4*f9_38
       + (int64_t)f5_2*f8_19 + (int64_t)f6*f7_38;
    h4 = (int64_t)f0_2*f4  + (int64_t)f1_2*f3_2  + (int64_t)f2*f2
       + (int64_t)f5_2*f9_38 + (int64_t)f6_2*f8_19 + (int64_t)f7*f7_38;
    h5 = (int64_t)f0_2*f5  + (int64_t)f1_2*f4    + (int64_t)f2_2*f3
       + (int64_t)f6*f9_38 + (int64_t)f7_2*f8_19;
    h6 = (int64_t)f0_2*f6  + (int64_t)f1_2*f5_2  + (int64_t)f2_2*f4
       + (int64_t)f3_2*f3  + (int64_t)f7_2*f9_38 + (int64_t)f8*f8_19;
    h7 = (int64_t)f0_2*f7  + (int64_t)f1_2*f6    + (int64_t)f2_2*f5
       + (int64_t)f3_2*f4  + (int64_t)f8*f9_38;
    h8 = (int64_t)f0_2*f8  + (int64_t)f1_2*f7_2  + (int64_t)f2_2*f6
       + (int64_t)f3_2*f5_2 + (int64_t)f4*f4     + (int64_t)f9*f9_38;
    h9 = (int64_t)f0_2*f9  + (int64_t)f1_2*f8    + (int64_t)f2_2*f7
       + (int64_t)f3_2*f6  + (int64_t)f4_2*f5;

    c0=(h0+(1L<<25))>>26; h1+=c0; h0-=c0*(1L<<26);
    c4=(h4+(1L<<25))>>26; h5+=c4; h4-=c4*(1L<<26);
    c1=(h1+(1L<<24))>>25; h2+=c1; h1-=c1*(1L<<25);
    c5=(h5+(1L<<24))>>25; h6+=c5; h5-=c5*(1L<<25);
    c2=(h2+(1L<<25))>>26; h3+=c2; h2-=c2*(1L<<26);
    c6=(h6+(1L<<25))>>26; h7+=c6; h6-=c6*(1L<<26);
    c3=(h3+(1L<<24))>>25; h4+=c3; h3-=c3*(1L<<25);
    c7=(h7+(1L<<24))>>25; h8+=c7; h7-=c7*(1L<<25);
    c4=(h4+(1L<<25))>>26; h5+=c4; h4-=c4*(1L<<26);
    c8=(h8+(1L<<25))>>26; h9+=c8; h8-=c8*(1L<<26);
    c9=(h9+(1L<<24))>>25; h0+=c9*19; h9-=c9*(1L<<25);
    c0=(h0+(1L<<25))>>26; h1+=c0; h0-=c0*(1L<<26);

    h[0]=(int32_t)h0; h[1]=(int32_t)h1; h[2]=(int32_t)h2; h[3]=(int32_t)h3;
    h[4]=(int32_t)h4; h[5]=(int32_t)h5; h[6]=(int32_t)h6; h[7]=(int32_t)h7;
    h[8]=(int32_t)h8; h[9]=(int32_t)h9;
}

/* h = 2 * f^2 */
static void fe_sq2(fe h, const fe f)
{
    fe_sq(h, f);
    fe_add(h, h, h);
}

/* h = f^((p-2) mod p)  =  modular inverse of f */
static void fe_invert(fe out, const fe z)
{
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    int i;

    fe_sq(z2, z);
    fe_sq(t, z2); fe_sq(t, t);
    fe_mul(z9, t, z);
    fe_mul(z11, z9, z2);
    fe_sq(t, z11);
    fe_mul(z2_5_0, t, z9);

    fe_sq(t, z2_5_0);
    for (i = 1; i < 5; i++) fe_sq(t, t);
    fe_mul(z2_10_0, t, z2_5_0);

    fe_sq(t, z2_10_0);
    for (i = 1; i < 10; i++) fe_sq(t, t);
    fe_mul(z2_20_0, t, z2_10_0);

    fe_sq(t, z2_20_0);
    for (i = 1; i < 20; i++) fe_sq(t, t);
    fe_mul(t, t, z2_20_0);

    fe_sq(t, t);
    for (i = 1; i < 10; i++) fe_sq(t, t);
    fe_mul(z2_50_0, t, z2_10_0);

    fe_sq(t, z2_50_0);
    for (i = 1; i < 50; i++) fe_sq(t, t);
    fe_mul(z2_100_0, t, z2_50_0);

    fe_sq(t, z2_100_0);
    for (i = 1; i < 100; i++) fe_sq(t, t);
    fe_mul(t, t, z2_100_0);

    fe_sq(t, t);
    for (i = 1; i < 50; i++) fe_sq(t, t);
    fe_mul(t, t, z2_50_0);

    fe_sq(t, t);
    fe_sq(t, t);
    fe_sq(t, t);
    fe_sq(t, t);
    fe_sq(t, t);
    fe_mul(out, t, z11);   /* = z^(2^255-21) = z^(p-2) */
}

/* =========================================================================
 * SECTION 2 -- Extended twisted Edwards group law
 *
 * Points in extended coordinates (X:Y:Z:T) with x=X/Z, y=Y/Z, xy=T/Z.
 * =========================================================================*/

typedef struct { fe X, Y, Z, T; } ge_p3;
typedef struct { fe X, Y, Z;    } ge_p2;
typedef struct { fe X, Y, Z, T; } ge_p1p1;

/* Precomputed affine entry: (y+x, y-x, 2*d*t) with Z=1 */
typedef struct { fe yplusx, yminusx, xy2d; } ge_precomp;

static void ge_p3_to_p2(ge_p2 *r, const ge_p3 *p)
{
    fe_copy(r->X, p->X);
    fe_copy(r->Y, p->Y);
    fe_copy(r->Z, p->Z);
}

static void ge_p1p1_to_p3(ge_p3 *r, const ge_p1p1 *p)
{
    fe_mul(r->X, p->X, p->T);
    fe_mul(r->Y, p->Y, p->Z);
    fe_mul(r->Z, p->Z, p->T);
    fe_mul(r->T, p->X, p->Y);
}

static void ge_p2_dbl(ge_p1p1 *r, const ge_p2 *p)
{
    fe t0;
    fe_sq(r->X, p->X);
    fe_sq(r->Z, p->Y);
    fe_sq2(r->T, p->Z);
    fe_add(r->Y, p->X, p->Y);
    fe_sq(t0, r->Y);
    fe_add(r->Y, r->Z, r->X);
    fe_sub(r->Z, r->Z, r->X);
    fe_sub(r->X, t0, r->Y);
    fe_sub(r->T, r->T, r->Z);
}

static void ge_p3_dbl(ge_p1p1 *r, const ge_p3 *p)
{
    ge_p2 q;
    ge_p3_to_p2(&q, p);
    ge_p2_dbl(r, &q);
}

/* Mixed addition p3 + precomp (Z of precomp = 1) */
static void ge_madd(ge_p1p1 *r, const ge_p3 *p, const ge_precomp *q)
{
    fe t0;
    fe_add(r->X, p->Y, p->X);
    fe_sub(r->Y, p->Y, p->X);
    fe_mul(r->Z, r->X, q->yplusx);
    fe_mul(r->Y, r->Y, q->yminusx);
    fe_mul(r->T, q->xy2d, p->T);
    fe_add(t0, p->Z, p->Z);
    fe_sub(r->X, r->Z, r->Y);
    fe_add(r->Y, r->Z, r->Y);
    fe_add(r->Z, t0,   r->T);
    fe_sub(r->T, t0,   r->T);
}

/* =========================================================================
 * SECTION 3 -- Base-point precomputed table
 *
 * Bi[i] = (i+1) * B  for i = 0..7, stored as (y+x, y-x, 2*d*t).
 * Values from SUPERCOP ref10 (public domain).
 * =========================================================================*/

static const ge_precomp Bi[8] = {
    /* 1*B */
    {
        { 25967493,-14356035,29566456,3660896,-12694345,
           4014787,27544626,-11754271,-6079156,2047605 },
        { -12545711,934262,-2722910,3049990,-727428,
            9406986,12720692,5043384,19500929,-15469378 },
        { -8738181,4489570,9688441,-14785194,10184609,
           12363380,29287919,11864899,-24514362,-4438546 }
    },
    /* 2*B */
    {
        { -33784483,-1299171,28527375,-1086150,1525650,
           -2947090,-10972080,6080832,-13924527,-9368456 },
        { 11460580,11951374,10156265,-8808416,13060474,
            8655607,-7083095,-3836017,-15966661,-7344064 },
        { 20335526,3050654,28166674,7290027,3930771,
           -9252606,9014706,-9188284,14843278,-8438851 }
    },
    /* 3*B */
    {
        { 11631022,9153465,-6484981,-15085664,9635397,
           -5768205,-19403539,9278555,-7375584,5099614 },
        { 12179844,-10408512,13647615,-7576424,-19467578,
            1303829,875425,4649510,8735895,-6448814 },
        { -32315539,-2067026,18396022,5099581,7726967,
           -5908649,-12048498,16200464,3688500,-7480994 }
    },
    /* 4*B */
    {
        { -24653863,-7779260,29224952,16763302,-11642675,
            6099493,-8330659,5506860,8996832,-4555699 },
        { -8159273,-3283945,18512174,-4568289,-9145190,
           16108791,1323180,1959193,4743361,-5248901 },
        { -8296591,-9248128,29313767,13616900,-26977758,
           -7218408,-13695854,1547900,12948676,-8083950 }
    },
    /* 5*B */
    {
        { 17447330,7837782,16929285,8241347,-17757010,
           -8167765,1038474,-7440510,-19423897,-9606038 },
        { -17796004,-12251043,29643607,7401900,-7373301,
           -10524993,-7920797,5729052,31024506,-1723555 },
        { 13958065,2713421,2511649,9665513,-5927851,
           13985960,-22011679,-7316534,-19416793,-5282619 }
    },
    /* 6*B */
    {
        { -11630437,-7529994,4488748,-8669482,5387502,
            6697400,-28308301,14074234,5219453,-7434533 },
        { 6869778,7027892,2223951,-10921499,5739440,
            7064261,-11131882,9918701,6429905,-4776076 },
        { -11226840,-12712700,8447488,-7787350,2823861,
           -3547441,16438836,9839988,33017,7717289 }
    },
    /* 7*B */
    {
        { 4874107,5479487,1609467,2918502,20009067,
           -7213618,16662340,7193018,-14394610,-5818498 },
        { -18427000,13375929,9015528,-1981680,-2719474,
             7907213,7866196,12362533,-8449505,8831832 },
        { 8526644,9155497,3521399,-9499060,-14484510,
           -5697940,-12048498,16200464,3688500,-7480994 }
    },
    /* 8*B */
    {
        { -15490735,4714340,4536020,2508040,-7427155,
           -4530083,-15407298,12749702,11934978,-8656700 },
        { -21580729,2784283,7488686,8553507,3571555,
             4560448,-5604490,-10325436,-17622079,-8386792 },
        { -10904595,-8291039,14014516,-4636124,3520484,
           -14127440,18849289,4015440,-6834404,-15831392 }
    }
};

/* =========================================================================
 * SECTION 4 -- Scalar multiplication  r = a * B
 *
 * Simple left-to-right double-and-add over 255 bits.
 * For each bit: double the accumulator, then add B if the bit is set.
 * =========================================================================*/

static void ge_p3_0(ge_p3 *h)
{
    fe_0(h->X);
    fe_1(h->Y);
    fe_1(h->Z);
    fe_0(h->T);
}

static void ge_scalarmult_base(ge_p3 *h, const uint8_t *a)
{
    ge_p1p1 r;
    int i;

    ge_p3_0(h);

    /* Process bits 254 down to 0 */
    for (i = 254; i >= 0; i--) {
        /* Double */
        ge_p3_dbl(&r, h);
        ge_p1p1_to_p3(h, &r);

        /* Conditionally add B (= Bi[0]) if bit i is set */
        if ((a[i >> 3] >> (i & 7)) & 1) {
            ge_madd(&r, h, &Bi[0]);
            ge_p1p1_to_p3(h, &r);
        }
    }
}

/* =========================================================================
 * SECTION 5 -- Point encoding
 * =========================================================================*/

static void ge_p3_tobytes(uint8_t *s, const ge_p3 *h)
{
    fe recip, x, y;
    uint8_t x_bytes[32];

    fe_invert(recip, h->Z);
    fe_mul(x, h->X, recip);
    fe_mul(y, h->Y, recip);

    fe_tobytes(s, y);

    /* Encode sign of x in the high bit of the last byte */
    fe_tobytes(x_bytes, x);
    s[31] |= (x_bytes[0] & 1) << 7;
}

/* =========================================================================
 * SECTION 6 -- Scalar arithmetic mod l
 *
 * l = 2^252 + 27742317777372353535851937790883648493
 *
 * The magic constants 666643, 470296, 654183, 997805, 136657, 683901
 * come from the decomposition of l in 21-bit limbs (SUPERCOP ref10,
 * sc_reduce.c, public domain).
 * =========================================================================*/

/*
 * sc_reduce: reduce a 64-byte (512-bit) little-endian integer mod l.
 * The upper 32 bytes are consumed; the result occupies the lower 32 bytes.
 */
static void sc_reduce(uint8_t *s)
{
    int64_t s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,
            s12,s13,s14,s15,s16,s17,s18,s19,s20,s21,s22,s23;
    int64_t carry0,carry1,carry2,carry3,carry4,carry5,
            carry6,carry7,carry8,carry9,carry10,carry11,
            carry12,carry13,carry14,carry15,carry16;

    s0  = 2097151 & load_3(s);
    s1  = 2097151 & (load_4(s+ 2) >> 5);
    s2  = 2097151 & (load_3(s+ 5) >> 2);
    s3  = 2097151 & (load_4(s+ 7) >> 7);
    s4  = 2097151 & (load_4(s+10) >> 4);
    s5  = 2097151 & (load_3(s+13) >> 1);
    s6  = 2097151 & (load_4(s+15) >> 6);
    s7  = 2097151 & (load_3(s+18) >> 3);
    s8  = 2097151 &  load_3(s+21);
    s9  = 2097151 & (load_4(s+23) >> 5);
    s10 = 2097151 & (load_3(s+26) >> 2);
    s11 = 2097151 & (load_4(s+28) >> 7);
    s12 = 2097151 & (load_4(s+31) >> 4);
    s13 = 2097151 & (load_3(s+34) >> 1);
    s14 = 2097151 & (load_4(s+36) >> 6);
    s15 = 2097151 & (load_3(s+39) >> 3);
    s16 = 2097151 &  load_3(s+42);
    s17 = 2097151 & (load_4(s+44) >> 5);
    s18 = 2097151 & (load_3(s+47) >> 2);
    s19 = 2097151 & (load_4(s+49) >> 7);
    s20 = 2097151 & (load_4(s+52) >> 4);
    s21 = 2097151 & (load_3(s+55) >> 1);
    s22 = 2097151 & (load_4(s+57) >> 6);
    s23 =           (load_4(s+60) >> 3);

    s11 += s23 * 666643;
    s12 += s23 * 470296;
    s13 += s23 * 654183;
    s14 -= s23 * 997805;
    s15 += s23 * 136657;
    s16 -= s23 * 683901;
    s23  = 0;

    s10 += s22 * 666643;
    s11 += s22 * 470296;
    s12 += s22 * 654183;
    s13 -= s22 * 997805;
    s14 += s22 * 136657;
    s15 -= s22 * 683901;
    s22  = 0;

    s9  += s21 * 666643;
    s10 += s21 * 470296;
    s11 += s21 * 654183;
    s12 -= s21 * 997805;
    s13 += s21 * 136657;
    s14 -= s21 * 683901;
    s21  = 0;

    s8  += s20 * 666643;
    s9  += s20 * 470296;
    s10 += s20 * 654183;
    s11 -= s20 * 997805;
    s12 += s20 * 136657;
    s13 -= s20 * 683901;
    s20  = 0;

    s7  += s19 * 666643;
    s8  += s19 * 470296;
    s9  += s19 * 654183;
    s10 -= s19 * 997805;
    s11 += s19 * 136657;
    s12 -= s19 * 683901;
    s19  = 0;

    s6  += s18 * 666643;
    s7  += s18 * 470296;
    s8  += s18 * 654183;
    s9  -= s18 * 997805;
    s10 += s18 * 136657;
    s11 -= s18 * 683901;
    s18  = 0;

    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry12=(s12+(1L<<20))>>21; s13+=carry12; s12-=carry12*(1L<<21);
    carry14=(s14+(1L<<20))>>21; s15+=carry14; s14-=carry14*(1L<<21);
    carry16=(s16+(1L<<20))>>21; s17+=carry16; s16-=carry16*(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);
    carry13=(s13+(1L<<20))>>21; s14+=carry13; s13-=carry13*(1L<<21);
    carry15=(s15+(1L<<20))>>21; s16+=carry15; s15-=carry15*(1L<<21);

    s5  += s17 * 666643;
    s6  += s17 * 470296;
    s7  += s17 * 654183;
    s8  -= s17 * 997805;
    s9  += s17 * 136657;
    s10 -= s17 * 683901;
    s17  = 0;

    s4  += s16 * 666643;
    s5  += s16 * 470296;
    s6  += s16 * 654183;
    s7  -= s16 * 997805;
    s8  += s16 * 136657;
    s9  -= s16 * 683901;
    s16  = 0;

    s3  += s15 * 666643;
    s4  += s15 * 470296;
    s5  += s15 * 654183;
    s6  -= s15 * 997805;
    s7  += s15 * 136657;
    s8  -= s15 * 683901;
    s15  = 0;

    s2  += s14 * 666643;
    s3  += s14 * 470296;
    s4  += s14 * 654183;
    s5  -= s14 * 997805;
    s6  += s14 * 136657;
    s7  -= s14 * 683901;
    s14  = 0;

    s1  += s13 * 666643;
    s2  += s13 * 470296;
    s3  += s13 * 654183;
    s4  -= s13 * 997805;
    s5  += s13 * 136657;
    s6  -= s13 * 683901;
    s13  = 0;

    s0  += s12 * 666643;
    s1  += s12 * 470296;
    s2  += s12 * 654183;
    s3  -= s12 * 997805;
    s4  += s12 * 136657;
    s5  -= s12 * 683901;
    s12  = 0;

    carry0 =(s0 +(1L<<20))>>21; s1 +=carry0;  s0 -=carry0 *(1L<<21);
    carry2 =(s2 +(1L<<20))>>21; s3 +=carry2;  s2 -=carry2 *(1L<<21);
    carry4 =(s4 +(1L<<20))>>21; s5 +=carry4;  s4 -=carry4 *(1L<<21);
    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry1 =(s1 +(1L<<20))>>21; s2 +=carry1;  s1 -=carry1 *(1L<<21);
    carry3 =(s3 +(1L<<20))>>21; s4 +=carry3;  s3 -=carry3 *(1L<<21);
    carry5 =(s5 +(1L<<20))>>21; s6 +=carry5;  s5 -=carry5 *(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);

    s0  += s12 * 666643;
    s1  += s12 * 470296;
    s2  += s12 * 654183;
    s3  -= s12 * 997805;
    s4  += s12 * 136657;
    s5  -= s12 * 683901;
    s12  = 0;

    carry0 =(s0 +(1L<<20))>>21; s1 +=carry0;  s0 -=carry0 *(1L<<21);
    carry1 =(s1 +(1L<<20))>>21; s2 +=carry1;  s1 -=carry1 *(1L<<21);
    carry2 =(s2 +(1L<<20))>>21; s3 +=carry2;  s2 -=carry2 *(1L<<21);
    carry3 =(s3 +(1L<<20))>>21; s4 +=carry3;  s3 -=carry3 *(1L<<21);
    carry4 =(s4 +(1L<<20))>>21; s5 +=carry4;  s4 -=carry4 *(1L<<21);
    carry5 =(s5 +(1L<<20))>>21; s6 +=carry5;  s5 -=carry5 *(1L<<21);
    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);

    /* Pack 12 * 21-bit limbs into 32 bytes */
    s[ 0] = (uint8_t)(s0 >> 0);
    s[ 1] = (uint8_t)(s0 >> 8);
    s[ 2] = (uint8_t)((s0>>16) | (s1 <<5));
    s[ 3] = (uint8_t)(s1 >> 3);
    s[ 4] = (uint8_t)(s1 >> 11);
    s[ 5] = (uint8_t)((s1>>19) | (s2 <<2));
    s[ 6] = (uint8_t)(s2 >> 6);
    s[ 7] = (uint8_t)((s2>>14) | (s3 <<7));
    s[ 8] = (uint8_t)(s3 >> 1);
    s[ 9] = (uint8_t)(s3 >> 9);
    s[10] = (uint8_t)((s3>>17) | (s4 <<4));
    s[11] = (uint8_t)(s4 >> 4);
    s[12] = (uint8_t)(s4 >> 12);
    s[13] = (uint8_t)((s4>>20) | (s5 <<1));
    s[14] = (uint8_t)(s5 >> 7);
    s[15] = (uint8_t)((s5>>15) | (s6 <<6));
    s[16] = (uint8_t)(s6 >> 2);
    s[17] = (uint8_t)(s6 >> 10);
    s[18] = (uint8_t)((s6>>18) | (s7 <<3));
    s[19] = (uint8_t)(s7 >> 5);
    s[20] = (uint8_t)(s7 >> 13);
    s[21] = (uint8_t)(s8 >> 0);
    s[22] = (uint8_t)(s8 >> 8);
    s[23] = (uint8_t)((s8>>16) | (s9 <<5));
    s[24] = (uint8_t)(s9 >> 3);
    s[25] = (uint8_t)(s9 >> 11);
    s[26] = (uint8_t)((s9>>19) | (s10<<2));
    s[27] = (uint8_t)(s10>> 6);
    s[28] = (uint8_t)((s10>>14) | (s11<<7));
    s[29] = (uint8_t)(s11>> 1);
    s[30] = (uint8_t)(s11>> 9);
    s[31] = (uint8_t)(s11>>17);
}

/*
 * sc_muladd: s = (a*b + c) mod l
 * All 32-byte little-endian operands, 32-byte result.
 */
static void sc_muladd(uint8_t *s,
                      const uint8_t *a,
                      const uint8_t *b,
                      const uint8_t *c)
{
    int64_t a0,a1,a2,a3,a4,a5,a6,a7,a8,a9,a10,a11;
    int64_t b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,b10,b11;
    int64_t c0,c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11;
    int64_t s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,s12,
            s13,s14,s15,s16,s17,s18,s19,s20,s21,s22;
    int64_t carry0,carry1,carry2,carry3,carry4,carry5,carry6,
            carry7,carry8,carry9,carry10,carry11,carry12,
            carry13,carry14,carry15,carry16,carry17,carry18,
            carry19,carry20,carry21;

    a0  = 2097151 & load_3(a);
    a1  = 2097151 & (load_4(a+ 2) >>  5);
    a2  = 2097151 & (load_3(a+ 5) >>  2);
    a3  = 2097151 & (load_4(a+ 7) >>  7);
    a4  = 2097151 & (load_4(a+10) >>  4);
    a5  = 2097151 & (load_3(a+13) >>  1);
    a6  = 2097151 & (load_4(a+15) >>  6);
    a7  = 2097151 & (load_3(a+18) >>  3);
    a8  = 2097151 &  load_3(a+21);
    a9  = 2097151 & (load_4(a+23) >>  5);
    a10 = 2097151 & (load_3(a+26) >>  2);
    a11 =            load_4(a+28) >>  7;

    b0  = 2097151 & load_3(b);
    b1  = 2097151 & (load_4(b+ 2) >>  5);
    b2  = 2097151 & (load_3(b+ 5) >>  2);
    b3  = 2097151 & (load_4(b+ 7) >>  7);
    b4  = 2097151 & (load_4(b+10) >>  4);
    b5  = 2097151 & (load_3(b+13) >>  1);
    b6  = 2097151 & (load_4(b+15) >>  6);
    b7  = 2097151 & (load_3(b+18) >>  3);
    b8  = 2097151 &  load_3(b+21);
    b9  = 2097151 & (load_4(b+23) >>  5);
    b10 = 2097151 & (load_3(b+26) >>  2);
    b11 =            load_4(b+28) >>  7;

    c0  = 2097151 & load_3(c);
    c1  = 2097151 & (load_4(c+ 2) >>  5);
    c2  = 2097151 & (load_3(c+ 5) >>  2);
    c3  = 2097151 & (load_4(c+ 7) >>  7);
    c4  = 2097151 & (load_4(c+10) >>  4);
    c5  = 2097151 & (load_3(c+13) >>  1);
    c6  = 2097151 & (load_4(c+15) >>  6);
    c7  = 2097151 & (load_3(c+18) >>  3);
    c8  = 2097151 &  load_3(c+21);
    c9  = 2097151 & (load_4(c+23) >>  5);
    c10 = 2097151 & (load_3(c+26) >>  2);
    c11 =            load_4(c+28) >>  7;

    s0  = c0 + a0*b0;
    s1  = c1 + a0*b1  + a1*b0;
    s2  = c2 + a0*b2  + a1*b1  + a2*b0;
    s3  = c3 + a0*b3  + a1*b2  + a2*b1  + a3*b0;
    s4  = c4 + a0*b4  + a1*b3  + a2*b2  + a3*b1  + a4*b0;
    s5  = c5 + a0*b5  + a1*b4  + a2*b3  + a3*b2  + a4*b1  + a5*b0;
    s6  = c6 + a0*b6  + a1*b5  + a2*b4  + a3*b3  + a4*b2  + a5*b1  + a6*b0;
    s7  = c7 + a0*b7  + a1*b6  + a2*b5  + a3*b4  + a4*b3  + a5*b2  + a6*b1  + a7*b0;
    s8  = c8 + a0*b8  + a1*b7  + a2*b6  + a3*b5  + a4*b4  + a5*b3  + a6*b2  + a7*b1  + a8*b0;
    s9  = c9 + a0*b9  + a1*b8  + a2*b7  + a3*b6  + a4*b5  + a5*b4  + a6*b3  + a7*b2  + a8*b1  + a9*b0;
    s10 = c10+ a0*b10 + a1*b9  + a2*b8  + a3*b7  + a4*b6  + a5*b5  + a6*b4  + a7*b3  + a8*b2  + a9*b1  + a10*b0;
    s11 = c11+ a0*b11 + a1*b10 + a2*b9  + a3*b8  + a4*b7  + a5*b6  + a6*b5  + a7*b4  + a8*b3  + a9*b2  + a10*b1 + a11*b0;
    s12 =      a1*b11 + a2*b10 + a3*b9  + a4*b8  + a5*b7  + a6*b6  + a7*b5  + a8*b4  + a9*b3  + a10*b2 + a11*b1;
    s13 =      a2*b11 + a3*b10 + a4*b9  + a5*b8  + a6*b7  + a7*b6  + a8*b5  + a9*b4  + a10*b3 + a11*b2;
    s14 =      a3*b11 + a4*b10 + a5*b9  + a6*b8  + a7*b7  + a8*b6  + a9*b5  + a10*b4 + a11*b3;
    s15 =      a4*b11 + a5*b10 + a6*b9  + a7*b8  + a8*b7  + a9*b6  + a10*b5 + a11*b4;
    s16 =      a5*b11 + a6*b10 + a7*b9  + a8*b8  + a9*b7  + a10*b6 + a11*b5;
    s17 =      a6*b11 + a7*b10 + a8*b9  + a9*b8  + a10*b7 + a11*b6;
    s18 =      a7*b11 + a8*b10 + a9*b9  + a10*b8 + a11*b7;
    s19 =      a8*b11 + a9*b10 + a10*b9 + a11*b8;
    s20 =      a9*b11 + a10*b10+ a11*b9;
    s21 =      a10*b11+ a11*b10;
    s22 =      a11*b11;

    carry0 =(s0 +(1L<<20))>>21; s1 +=carry0;  s0 -=carry0 *(1L<<21);
    carry2 =(s2 +(1L<<20))>>21; s3 +=carry2;  s2 -=carry2 *(1L<<21);
    carry4 =(s4 +(1L<<20))>>21; s5 +=carry4;  s4 -=carry4 *(1L<<21);
    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry12=(s12+(1L<<20))>>21; s13+=carry12; s12-=carry12*(1L<<21);
    carry14=(s14+(1L<<20))>>21; s15+=carry14; s14-=carry14*(1L<<21);
    carry16=(s16+(1L<<20))>>21; s17+=carry16; s16-=carry16*(1L<<21);
    carry18=(s18+(1L<<20))>>21; s19+=carry18; s18-=carry18*(1L<<21);
    carry20=(s20+(1L<<20))>>21; s21+=carry20; s20-=carry20*(1L<<21);
    carry1 =(s1 +(1L<<20))>>21; s2 +=carry1;  s1 -=carry1 *(1L<<21);
    carry3 =(s3 +(1L<<20))>>21; s4 +=carry3;  s3 -=carry3 *(1L<<21);
    carry5 =(s5 +(1L<<20))>>21; s6 +=carry5;  s5 -=carry5 *(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);
    carry13=(s13+(1L<<20))>>21; s14+=carry13; s13-=carry13*(1L<<21);
    carry15=(s15+(1L<<20))>>21; s16+=carry15; s15-=carry15*(1L<<21);
    carry17=(s17+(1L<<20))>>21; s18+=carry17; s17-=carry17*(1L<<21);
    carry19=(s19+(1L<<20))>>21; s20+=carry19; s19-=carry19*(1L<<21);
    carry21=(s21+(1L<<20))>>21; s22+=carry21; s21-=carry21*(1L<<21);

    /* Reduce high limbs */
    s11 += s22 * 666643;
    s12 += s22 * 470296;
    s13 += s22 * 654183;
    s14 -= s22 * 997805;
    s15 += s22 * 136657;
    s16 -= s22 * 683901;
    s22  = 0;

    s10 += s21 * 666643;
    s11 += s21 * 470296;
    s12 += s21 * 654183;
    s13 -= s21 * 997805;
    s14 += s21 * 136657;
    s15 -= s21 * 683901;
    s21  = 0;

    s9  += s20 * 666643;
    s10 += s20 * 470296;
    s11 += s20 * 654183;
    s12 -= s20 * 997805;
    s13 += s20 * 136657;
    s14 -= s20 * 683901;
    s20  = 0;

    s8  += s19 * 666643;
    s9  += s19 * 470296;
    s10 += s19 * 654183;
    s11 -= s19 * 997805;
    s12 += s19 * 136657;
    s13 -= s19 * 683901;
    s19  = 0;

    s7  += s18 * 666643;
    s8  += s18 * 470296;
    s9  += s18 * 654183;
    s10 -= s18 * 997805;
    s11 += s18 * 136657;
    s12 -= s18 * 683901;
    s18  = 0;

    s6  += s17 * 666643;
    s7  += s17 * 470296;
    s8  += s17 * 654183;
    s9  -= s17 * 997805;
    s10 += s17 * 136657;
    s11 -= s17 * 683901;
    s17  = 0;

    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry12=(s12+(1L<<20))>>21; s13+=carry12; s12-=carry12*(1L<<21);
    carry14=(s14+(1L<<20))>>21; s15+=carry14; s14-=carry14*(1L<<21);
    carry16=(s16+(1L<<20))>>21; s17+=carry16; s16-=carry16*(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);
    carry13=(s13+(1L<<20))>>21; s14+=carry13; s13-=carry13*(1L<<21);
    carry15=(s15+(1L<<20))>>21; s16+=carry15; s15-=carry15*(1L<<21);

    s5  += s16 * 666643;
    s6  += s16 * 470296;
    s7  += s16 * 654183;
    s8  -= s16 * 997805;
    s9  += s16 * 136657;
    s10 -= s16 * 683901;
    s16  = 0;

    s4  += s15 * 666643;
    s5  += s15 * 470296;
    s6  += s15 * 654183;
    s7  -= s15 * 997805;
    s8  += s15 * 136657;
    s9  -= s15 * 683901;
    s15  = 0;

    s3  += s14 * 666643;
    s4  += s14 * 470296;
    s5  += s14 * 654183;
    s6  -= s14 * 997805;
    s7  += s14 * 136657;
    s8  -= s14 * 683901;
    s14  = 0;

    s2  += s13 * 666643;
    s3  += s13 * 470296;
    s4  += s13 * 654183;
    s5  -= s13 * 997805;
    s6  += s13 * 136657;
    s7  -= s13 * 683901;
    s13  = 0;

    s1  += s12 * 666643;
    s2  += s12 * 470296;
    s3  += s12 * 654183;
    s4  -= s12 * 997805;
    s5  += s12 * 136657;
    s6  -= s12 * 683901;
    s12  = 0;

    carry0 =(s0 +(1L<<20))>>21; s1 +=carry0;  s0 -=carry0 *(1L<<21);
    carry2 =(s2 +(1L<<20))>>21; s3 +=carry2;  s2 -=carry2 *(1L<<21);
    carry4 =(s4 +(1L<<20))>>21; s5 +=carry4;  s4 -=carry4 *(1L<<21);
    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry1 =(s1 +(1L<<20))>>21; s2 +=carry1;  s1 -=carry1 *(1L<<21);
    carry3 =(s3 +(1L<<20))>>21; s4 +=carry3;  s3 -=carry3 *(1L<<21);
    carry5 =(s5 +(1L<<20))>>21; s6 +=carry5;  s5 -=carry5 *(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);

    s0  += s12 * 666643;
    s1  += s12 * 470296;
    s2  += s12 * 654183;
    s3  -= s12 * 997805;
    s4  += s12 * 136657;
    s5  -= s12 * 683901;
    s12  = 0;

    carry0 =(s0 +(1L<<20))>>21; s1 +=carry0;  s0 -=carry0 *(1L<<21);
    carry1 =(s1 +(1L<<20))>>21; s2 +=carry1;  s1 -=carry1 *(1L<<21);
    carry2 =(s2 +(1L<<20))>>21; s3 +=carry2;  s2 -=carry2 *(1L<<21);
    carry3 =(s3 +(1L<<20))>>21; s4 +=carry3;  s3 -=carry3 *(1L<<21);
    carry4 =(s4 +(1L<<20))>>21; s5 +=carry4;  s4 -=carry4 *(1L<<21);
    carry5 =(s5 +(1L<<20))>>21; s6 +=carry5;  s5 -=carry5 *(1L<<21);
    carry6 =(s6 +(1L<<20))>>21; s7 +=carry6;  s6 -=carry6 *(1L<<21);
    carry7 =(s7 +(1L<<20))>>21; s8 +=carry7;  s7 -=carry7 *(1L<<21);
    carry8 =(s8 +(1L<<20))>>21; s9 +=carry8;  s8 -=carry8 *(1L<<21);
    carry9 =(s9 +(1L<<20))>>21; s10+=carry9;  s9 -=carry9 *(1L<<21);
    carry10=(s10+(1L<<20))>>21; s11+=carry10; s10-=carry10*(1L<<21);
    carry11=(s11+(1L<<20))>>21; s12+=carry11; s11-=carry11*(1L<<21);

    s[ 0] = (uint8_t)(s0 >> 0);
    s[ 1] = (uint8_t)(s0 >> 8);
    s[ 2] = (uint8_t)((s0>>16) | (s1 <<5));
    s[ 3] = (uint8_t)(s1 >> 3);
    s[ 4] = (uint8_t)(s1 >> 11);
    s[ 5] = (uint8_t)((s1>>19) | (s2 <<2));
    s[ 6] = (uint8_t)(s2 >> 6);
    s[ 7] = (uint8_t)((s2>>14) | (s3 <<7));
    s[ 8] = (uint8_t)(s3 >> 1);
    s[ 9] = (uint8_t)(s3 >> 9);
    s[10] = (uint8_t)((s3>>17) | (s4 <<4));
    s[11] = (uint8_t)(s4 >> 4);
    s[12] = (uint8_t)(s4 >> 12);
    s[13] = (uint8_t)((s4>>20) | (s5 <<1));
    s[14] = (uint8_t)(s5 >> 7);
    s[15] = (uint8_t)((s5>>15) | (s6 <<6));
    s[16] = (uint8_t)(s6 >> 2);
    s[17] = (uint8_t)(s6 >> 10);
    s[18] = (uint8_t)((s6>>18) | (s7 <<3));
    s[19] = (uint8_t)(s7 >> 5);
    s[20] = (uint8_t)(s7 >> 13);
    s[21] = (uint8_t)(s8 >> 0);
    s[22] = (uint8_t)(s8 >> 8);
    s[23] = (uint8_t)((s8>>16) | (s9 <<5));
    s[24] = (uint8_t)(s9 >> 3);
    s[25] = (uint8_t)(s9 >> 11);
    s[26] = (uint8_t)((s9>>19) | (s10<<2));
    s[27] = (uint8_t)(s10>> 6);
    s[28] = (uint8_t)((s10>>14) | (s11<<7));
    s[29] = (uint8_t)(s11>> 1);
    s[30] = (uint8_t)(s11>> 9);
    s[31] = (uint8_t)(s11>>17);
}

/* =========================================================================
 * SECTION 7 -- SHA-512 helpers (mbedTLS)
 * =========================================================================*/

static void sha512_2(const uint8_t *a, size_t alen,
                     const uint8_t *b, size_t blen,
                     uint8_t *out)
{
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0 /* is384=0 */);
    mbedtls_sha512_update(&ctx, a, alen);
    mbedtls_sha512_update(&ctx, b, blen);
    mbedtls_sha512_finish(&ctx, out);
    mbedtls_sha512_free(&ctx);
}

static void sha512_3(const uint8_t *a, size_t alen,
                     const uint8_t *b, size_t blen,
                     const uint8_t *c, size_t clen,
                     uint8_t *out)
{
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    mbedtls_sha512_update(&ctx, a, alen);
    mbedtls_sha512_update(&ctx, b, blen);
    mbedtls_sha512_update(&ctx, c, clen);
    mbedtls_sha512_finish(&ctx, out);
    mbedtls_sha512_free(&ctx);
}

/* forward declaration -- defined later in SECTION 9 */
static void x25519_scalarmult(uint8_t *out, const uint8_t *k, const uint8_t *u);

/* =========================================================================
 * SECTION 8 -- Public API
 * =========================================================================*/

/* ------------------------------------------------------------------------
 * MPI-based Ed25519 -- RFC 8032 compliant, correctness > speed.
 *
 * Replaces the broken fe_ / ge_ implementations (those produce wrong points
 * for the RFC TV1 seed -- verified via on-device self-test). All field and
 * group arithmetic is delegated to mbedtls_mpi, the same well-tested BigInt
 * layer that powers the working X25519 path.
 *
 * Constants p, l, d, base point B, encoded as 32-byte little-endian.
 * ------------------------------------------------------------------------ */
static const uint8_t MPI_P_LE[32] = {
    0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
    0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff, 0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
};
static const uint8_t MPI_L_LE[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58, 0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10
};
static const uint8_t MPI_D_LE[32] = {
    0xa3,0x78,0x59,0x13,0xca,0x4d,0xeb,0x75, 0xab,0xd8,0x41,0x41,0x4d,0x0a,0x70,0x00,
    0x98,0xe8,0x79,0x77,0x79,0x40,0xc7,0x8c, 0x73,0xfe,0x6f,0x2b,0xee,0x6c,0x03,0x52
};
static const uint8_t MPI_BX_LE[32] = {
    0x1a,0xd5,0x25,0x8f,0x60,0x2d,0x56,0xc9, 0xb2,0xa7,0x25,0x95,0x60,0xc7,0x2c,0x69,
    0x5c,0xdc,0xd6,0xfd,0x31,0xe2,0xa4,0xc0, 0xfe,0x53,0x6e,0xcd,0xd3,0x36,0x69,0x21
};
static const uint8_t MPI_BY_LE[32] = {
    0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66, 0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66, 0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
};

typedef struct { mbedtls_mpi X, Y, Z, T; } mpi_point_t;

static void mp_init(mpi_point_t *p) {
    mbedtls_mpi_init(&p->X); mbedtls_mpi_init(&p->Y);
    mbedtls_mpi_init(&p->Z); mbedtls_mpi_init(&p->T);
}
static void mp_free(mpi_point_t *p) {
    mbedtls_mpi_free(&p->X); mbedtls_mpi_free(&p->Y);
    mbedtls_mpi_free(&p->Z); mbedtls_mpi_free(&p->T);
}
static void mp_copy(mpi_point_t *d, const mpi_point_t *s) {
    mbedtls_mpi_copy(&d->X, &s->X); mbedtls_mpi_copy(&d->Y, &s->Y);
    mbedtls_mpi_copy(&d->Z, &s->Z); mbedtls_mpi_copy(&d->T, &s->T);
}
static void mp_set_identity(mpi_point_t *p) {
    mbedtls_mpi_lset(&p->X, 0); mbedtls_mpi_lset(&p->Y, 1);
    mbedtls_mpi_lset(&p->Z, 1); mbedtls_mpi_lset(&p->T, 0);
}
static void mp_set_B(mpi_point_t *p, const mbedtls_mpi *P_) {
    mbedtls_mpi_read_binary_le(&p->X, MPI_BX_LE, 32);
    mbedtls_mpi_read_binary_le(&p->Y, MPI_BY_LE, 32);
    mbedtls_mpi_lset(&p->Z, 1);
    mbedtls_mpi_mul_mpi(&p->T, &p->X, &p->Y);
    mbedtls_mpi_mod_mpi(&p->T, &p->T, P_);
}

/* Extended twisted-Edwards point addition (a = -1), formula add-2008-hwcd-3. */
static void mp_add_points(mpi_point_t *r, const mpi_point_t *p1, const mpi_point_t *p2,
                          const mbedtls_mpi *P_, const mbedtls_mpi *D_)
{
    mbedtls_mpi A, BB, C, D, E, F, G, H, tmp;
    mbedtls_mpi_init(&A);  mbedtls_mpi_init(&BB); mbedtls_mpi_init(&C);
    mbedtls_mpi_init(&D);  mbedtls_mpi_init(&E);  mbedtls_mpi_init(&F);
    mbedtls_mpi_init(&G);  mbedtls_mpi_init(&H);  mbedtls_mpi_init(&tmp);
    /* A = (Y1-X1)*(Y2-X2) */
    mbedtls_mpi_sub_mpi(&A, &p1->Y, &p1->X);  mbedtls_mpi_mod_mpi(&A, &A, P_);
    mbedtls_mpi_sub_mpi(&tmp, &p2->Y, &p2->X); mbedtls_mpi_mod_mpi(&tmp, &tmp, P_);
    mbedtls_mpi_mul_mpi(&A, &A, &tmp); mbedtls_mpi_mod_mpi(&A, &A, P_);
    /* B = (Y1+X1)*(Y2+X2) */
    mbedtls_mpi_add_mpi(&BB, &p1->Y, &p1->X);  mbedtls_mpi_mod_mpi(&BB, &BB, P_);
    mbedtls_mpi_add_mpi(&tmp, &p2->Y, &p2->X); mbedtls_mpi_mod_mpi(&tmp, &tmp, P_);
    mbedtls_mpi_mul_mpi(&BB, &BB, &tmp); mbedtls_mpi_mod_mpi(&BB, &BB, P_);
    /* C = 2*d*T1*T2 */
    mbedtls_mpi_mul_mpi(&C, &p1->T, &p2->T); mbedtls_mpi_mod_mpi(&C, &C, P_);
    mbedtls_mpi_mul_mpi(&C, &C, D_);          mbedtls_mpi_mod_mpi(&C, &C, P_);
    mbedtls_mpi_add_mpi(&C, &C, &C);          mbedtls_mpi_mod_mpi(&C, &C, P_);
    /* D = 2*Z1*Z2 */
    mbedtls_mpi_mul_mpi(&D, &p1->Z, &p2->Z); mbedtls_mpi_add_mpi(&D, &D, &D);
    mbedtls_mpi_mod_mpi(&D, &D, P_);
    /* E=B-A, F=D-C, G=D+C, H=B+A */
    mbedtls_mpi_sub_mpi(&E, &BB, &A); mbedtls_mpi_mod_mpi(&E, &E, P_);
    mbedtls_mpi_sub_mpi(&F, &D, &C);  mbedtls_mpi_mod_mpi(&F, &F, P_);
    mbedtls_mpi_add_mpi(&G, &D, &C);  mbedtls_mpi_mod_mpi(&G, &G, P_);
    mbedtls_mpi_add_mpi(&H, &BB, &A); mbedtls_mpi_mod_mpi(&H, &H, P_);
    /* X3=E*F, Y3=G*H, Z3=F*G, T3=E*H */
    mbedtls_mpi_mul_mpi(&r->X, &E, &F); mbedtls_mpi_mod_mpi(&r->X, &r->X, P_);
    mbedtls_mpi_mul_mpi(&r->Y, &G, &H); mbedtls_mpi_mod_mpi(&r->Y, &r->Y, P_);
    mbedtls_mpi_mul_mpi(&r->Z, &F, &G); mbedtls_mpi_mod_mpi(&r->Z, &r->Z, P_);
    mbedtls_mpi_mul_mpi(&r->T, &E, &H); mbedtls_mpi_mod_mpi(&r->T, &r->T, P_);

    mbedtls_mpi_free(&A);  mbedtls_mpi_free(&BB); mbedtls_mpi_free(&C);
    mbedtls_mpi_free(&D);  mbedtls_mpi_free(&E);  mbedtls_mpi_free(&F);
    mbedtls_mpi_free(&G);  mbedtls_mpi_free(&H);  mbedtls_mpi_free(&tmp);
}

/* Left-to-right binary double-and-add scalar mult. ~250ms on P4 (slow but correct). */
static void mp_scalar_mult(mpi_point_t *out, const mpi_point_t *base, const uint8_t *scalar,
                           const mbedtls_mpi *P_, const mbedtls_mpi *D_)
{
    mpi_point_t acc, tmp;
    mp_init(&acc); mp_init(&tmp);
    mp_set_identity(&acc);
    for (int i = 254; i >= 0; i--) {
        mp_add_points(&tmp, &acc, &acc, P_, D_);
        mp_copy(&acc, &tmp);
        if ((scalar[i >> 3] >> (i & 7)) & 1) {
            mp_add_points(&tmp, &acc, base, P_, D_);
            mp_copy(&acc, &tmp);
        }
    }
    mp_copy(out, &acc);
    mp_free(&acc); mp_free(&tmp);
}

/* Encode point as 32-byte compressed form: y_LE_bytes with sign(x) in bit 255. */
static void mp_point_encode(uint8_t out[32], const mpi_point_t *p, const mbedtls_mpi *P_)
{
    mbedtls_mpi z_inv, x_aff, y_aff;
    mbedtls_mpi_init(&z_inv); mbedtls_mpi_init(&x_aff); mbedtls_mpi_init(&y_aff);
    mbedtls_mpi_inv_mod(&z_inv, &p->Z, P_);
    mbedtls_mpi_mul_mpi(&x_aff, &p->X, &z_inv); mbedtls_mpi_mod_mpi(&x_aff, &x_aff, P_);
    mbedtls_mpi_mul_mpi(&y_aff, &p->Y, &z_inv); mbedtls_mpi_mod_mpi(&y_aff, &y_aff, P_);
    mbedtls_mpi_write_binary_le(&y_aff, out, 32);
    out[31] &= 0x7F;
    if (mbedtls_mpi_get_bit(&x_aff, 0)) out[31] |= 0x80;
    mbedtls_mpi_free(&z_inv); mbedtls_mpi_free(&x_aff); mbedtls_mpi_free(&y_aff);
}

void ed25519_create_keypair(uint8_t *public_key, uint8_t *private_key, const uint8_t *seed)
{
    uint8_t hash[64];
    mbedtls_sha512(seed, 32, hash, 0);
    hash[0]  &= 248;
    hash[31] &= 63;
    hash[31] |= 64;
    memcpy(private_key, hash, 64);

    mbedtls_mpi P_, D_;
    mbedtls_mpi_init(&P_); mbedtls_mpi_init(&D_);
    mbedtls_mpi_read_binary_le(&P_, MPI_P_LE, 32);
    mbedtls_mpi_read_binary_le(&D_, MPI_D_LE, 32);

    mpi_point_t B, A;
    mp_init(&B); mp_init(&A);
    mp_set_B(&B, &P_);
    mp_scalar_mult(&A, &B, hash, &P_, &D_);
    mp_point_encode(public_key, &A, &P_);

    mp_free(&A); mp_free(&B);
    mbedtls_mpi_free(&P_); mbedtls_mpi_free(&D_);
}

void ed25519_sign(uint8_t *signature, const uint8_t *message, size_t message_len,
                  const uint8_t *public_key, const uint8_t *private_key)
{
    uint8_t nonce[64], hram[64];
    /* r_full = SHA-512(priv[32..63] || msg) ; r = r_full mod l */
    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    mbedtls_sha512_update(&ctx, private_key + 32, 32);
    mbedtls_sha512_update(&ctx, message, message_len);
    mbedtls_sha512_finish(&ctx, nonce);
    mbedtls_sha512_free(&ctx);

    mbedtls_mpi P_, D_, L_, r_mpi, k_mpi, a_mpi, S_, tmp_mpi;
    mbedtls_mpi_init(&P_); mbedtls_mpi_init(&D_); mbedtls_mpi_init(&L_);
    mbedtls_mpi_init(&r_mpi); mbedtls_mpi_init(&k_mpi);
    mbedtls_mpi_init(&a_mpi); mbedtls_mpi_init(&S_); mbedtls_mpi_init(&tmp_mpi);
    mbedtls_mpi_read_binary_le(&P_, MPI_P_LE, 32);
    mbedtls_mpi_read_binary_le(&D_, MPI_D_LE, 32);
    mbedtls_mpi_read_binary_le(&L_, MPI_L_LE, 32);
    mbedtls_mpi_read_binary_le(&r_mpi, nonce, 64);
    mbedtls_mpi_mod_mpi(&r_mpi, &r_mpi, &L_);

    uint8_t r_bytes[32];
    mbedtls_mpi_write_binary_le(&r_mpi, r_bytes, 32);

    /* R = r * B, encoded into signature[0..31] */
    mpi_point_t B, R;
    mp_init(&B); mp_init(&R);
    mp_set_B(&B, &P_);
    mp_scalar_mult(&R, &B, r_bytes, &P_, &D_);
    mp_point_encode(signature, &R, &P_);

    /* k = SHA-512(R || pub || msg) mod l */
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, 0);
    mbedtls_sha512_update(&ctx, signature, 32);
    mbedtls_sha512_update(&ctx, public_key, 32);
    mbedtls_sha512_update(&ctx, message, message_len);
    mbedtls_sha512_finish(&ctx, hram);
    mbedtls_sha512_free(&ctx);
    mbedtls_mpi_read_binary_le(&k_mpi, hram, 64);
    mbedtls_mpi_mod_mpi(&k_mpi, &k_mpi, &L_);

    /* a = clamped priv scalar (first 32 bytes of priv) */
    mbedtls_mpi_read_binary_le(&a_mpi, private_key, 32);

    /* S = (r + k*a) mod l, encoded LE into signature[32..63] */
    mbedtls_mpi_mul_mpi(&tmp_mpi, &k_mpi, &a_mpi);
    mbedtls_mpi_mod_mpi(&tmp_mpi, &tmp_mpi, &L_);
    mbedtls_mpi_add_mpi(&S_, &r_mpi, &tmp_mpi);
    mbedtls_mpi_mod_mpi(&S_, &S_, &L_);
    mbedtls_mpi_write_binary_le(&S_, signature + 32, 32);

    mp_free(&B); mp_free(&R);
    mbedtls_mpi_free(&P_); mbedtls_mpi_free(&D_); mbedtls_mpi_free(&L_);
    mbedtls_mpi_free(&r_mpi); mbedtls_mpi_free(&k_mpi);
    mbedtls_mpi_free(&a_mpi); mbedtls_mpi_free(&S_); mbedtls_mpi_free(&tmp_mpi);
}

// ── X25519 key exchange ────────────────────────────────────────────────────────

/* Load 32 bytes (little-endian) as a GF(2^255-19) field element.
 * Consistent with fe_tobytes: alternating 26/25-bit limbs at the same bit offsets.
 * Bit 255 (sign bit in Ed25519 encoding) is cleared by the 25-bit mask on h9. */
static void fe_frombytes_x25519(fe h, const uint8_t *s)
{
    int64_t h0 = (int64_t)( load_4(s     )         & 0x3FFFFFF);   /* bits   0..25 */
    int64_t h1 = (int64_t)((load_4(s+ 3) >>  2)    & 0x1FFFFFF);   /* bits  26..50 */
    int64_t h2 = (int64_t)((load_4(s+ 6) >>  3)    & 0x3FFFFFF);   /* bits  51..76 */
    int64_t h3 = (int64_t)((load_4(s+ 9) >>  5)    & 0x1FFFFFF);   /* bits  77..101*/
    int64_t h4 = (int64_t)((load_4(s+12) >>  6)    & 0x3FFFFFF);   /* bits 102..127*/
    int64_t h5 = (int64_t)( load_4(s+16)            & 0x1FFFFFF);   /* bits 128..152*/
    int64_t h6 = (int64_t)((load_4(s+19) >>  1)    & 0x3FFFFFF);   /* bits 153..178*/
    int64_t h7 = (int64_t)((load_4(s+22) >>  3)    & 0x1FFFFFF);   /* bits 179..203*/
    int64_t h8 = (int64_t)((load_4(s+25) >>  4)    & 0x3FFFFFF);   /* bits 204..229*/
    int64_t h9 = (int64_t)((load_4(s+28) >>  6)    & 0x1FFFFFF);   /* bits 230..254*/
    h[0]=(int32_t)h0; h[1]=(int32_t)h1; h[2]=(int32_t)h2; h[3]=(int32_t)h3;
    h[4]=(int32_t)h4; h[5]=(int32_t)h5; h[6]=(int32_t)h6; h[7]=(int32_t)h7;
    h[8]=(int32_t)h8; h[9]=(int32_t)h9;
}


/* X25519 scalar multiplication via mbedTLS Curve25519 ECP.
 * k:   32-byte clamped scalar (little-endian)
 * u:   32-byte u-coordinate input (little-endian)
 * out: 32-byte u-coordinate output (little-endian)
 *
 * mbedtls_ecp_point_read/write_binary for Montgomery curves uses
 * mpi_read/write_binary_le, so no byte-reversal is needed. */
static void x25519_scalarmult(uint8_t *out, const uint8_t *k, const uint8_t *u)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q, R;
    mbedtls_mpi d;
    size_t olen;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);
    mbedtls_ecp_point_init(&R);
    mbedtls_mpi_init(&d);

    mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);

    /* Scalar: little-endian bytes → MPI (mbedTLS bignum is big-endian internally,
     * but mpi_read_binary_le reads LE so we use it directly) */
    mbedtls_mpi_read_binary_le(&d, k, 32);

    /* u-coordinate: read as little-endian point using the public API */
    mbedtls_ecp_point_read_binary(&grp, &Q, u, 32);

    mbedtls_ecp_mul(&grp, &R, &d, &Q, ecp_rng, NULL);

    /* Write result as little-endian 32 bytes */
    mbedtls_ecp_point_write_binary(&grp, &R, MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, out, 32);

    mbedtls_ecp_group_free(&grp);
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_point_free(&R);
    mbedtls_mpi_free(&d);
}

/* Ed25519 public key → Curve25519 Montgomery u-coordinate.
 * Uses mbedTLS bignum so we don't depend on our custom fe_* arithmetic.
 * u = (1 + y) / (1 - y) mod (2^255 - 19)
 * The Ed25519 pub key encodes y in the lower 255 bits; bit 255 is the x-sign. */
static void ed25519_pub_to_curve25519(uint8_t *u_bytes, const uint8_t *ed25519_pub)
{
    /* p = 2^255 - 19, little-endian */
    static const uint8_t p25519[32] = {
        0xed,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x7f
    };
    mbedtls_mpi Y, num, den, inv_den, u, P;
    mbedtls_mpi_init(&Y); mbedtls_mpi_init(&num); mbedtls_mpi_init(&den);
    mbedtls_mpi_init(&inv_den); mbedtls_mpi_init(&u); mbedtls_mpi_init(&P);

    mbedtls_mpi_read_binary_le(&P, p25519, 32);
    mbedtls_mpi_read_binary_le(&Y, ed25519_pub, 32);
    mbedtls_mpi_set_bit(&Y, 255, 0);           /* clear x-sign bit */

    /* num = (1 + Y) mod P */
    mbedtls_mpi_add_int(&num, &Y, 1);
    mbedtls_mpi_mod_mpi(&num, &num, &P);

    /* den = (1 - Y) mod P  (add P first to avoid negative) */
    mbedtls_mpi_sub_mpi(&den, &P, &Y);
    mbedtls_mpi_add_int(&den, &den, 1);
    mbedtls_mpi_mod_mpi(&den, &den, &P);

    /* u = num * inv(den) mod P */
    mbedtls_mpi_inv_mod(&inv_den, &den, &P);
    mbedtls_mpi_mul_mpi(&u, &num, &inv_den);
    mbedtls_mpi_mod_mpi(&u, &u, &P);

    mbedtls_mpi_write_binary_le(&u, u_bytes, 32);

    mbedtls_mpi_free(&Y); mbedtls_mpi_free(&num); mbedtls_mpi_free(&den);
    mbedtls_mpi_free(&inv_den); mbedtls_mpi_free(&u); mbedtls_mpi_free(&P);
}

/* X25519 Diffie-Hellman using Ed25519 keys.
 * Converts the Ed25519 public key (Edwards y) to Montgomery u via mbedTLS bignum,
 * then runs X25519 via mbedTLS ECP.
 * private_key: 64-byte expanded Ed25519 key (first 32 bytes = clamped scalar) */
void ed25519_key_exchange(uint8_t *shared_secret,
                          const uint8_t *public_key,
                          const uint8_t *private_key)
{
    uint8_t scalar[32], u_bytes[32];

    for (int i = 0; i < 32; i++) scalar[i] = private_key[i];
    scalar[0]  &= 248;
    scalar[31] &= 63;
    scalar[31] |= 64;

    ed25519_pub_to_curve25519(u_bytes, public_key);
    x25519_scalarmult(shared_secret, scalar, u_bytes);
}

/* Raw X25519: treats public_key bytes directly as Curve25519 u-coordinate.
 * Used for the RFC 7748 test vector and direct Curve25519 key exchange. */
void ed25519_key_exchange_raw(uint8_t *shared_secret,
                               const uint8_t *public_key,
                               const uint8_t *private_key)
{
    uint8_t scalar[32];
    for (int i = 0; i < 32; i++) scalar[i] = private_key[i];
    scalar[0]  &= 248;
    scalar[31] &= 63;
    scalar[31] |= 64;
    x25519_scalarmult(shared_secret, scalar, public_key);
}

void ed25519_pub_to_x25519(uint8_t *curve25519_pub, const uint8_t *ed25519_pub)
{
    ed25519_pub_to_curve25519(curve25519_pub, ed25519_pub);
}
