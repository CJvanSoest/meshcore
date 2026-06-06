// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
//
// Ed25519 sign/keypair via mbedtls_mpi. See ed25519_mpi.h.
//
// Depends only on mbedtls (sha512 + bignum). No esp-idf includes so the same
// translation unit can be linked into both the device firmware and the host
// test harness in tests/.

#include "ed25519_mpi.h"

#include <string.h>

#include "mbedtls/bignum.h"
#include "mbedtls/sha512.h"

/* ------------------------------------------------------------------------
 * Constants p, l, d, base point B -- all 32-byte little-endian.
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

/* Extended twisted-Edwards point addition (a = -1), add-2008-hwcd-3 */
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

/* Left-to-right binary double-and-add scalar mult. Constant time WRT loop
 * length (always 255 iterations) but NOT constant time WRT scalar bits --
 * acceptable for this app's threat model (single-user device, no remote
 * timing oracle on the signing side). */
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
