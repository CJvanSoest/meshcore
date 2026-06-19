# Host-side tests for code we ship on the device.
#
# These tests compile + run on the CI runner (Ubuntu) before any IDF build.
# They link directly against the same .c files that go into the firmware, so
# a crypto regression makes CI red and blocks the merge.
#
# Run locally:
#     cd tests && make && ./test_ed25519
#
# Requirements: gcc + libmbedtls-dev v3.x (provides libmbedcrypto + headers
# for mbedtls_mpi_mod_mpi / mbedtls_mpi_write_binary_le, both used here).
#
# Verified on Ubuntu 22.04 (mbedtls 2.28) and 24.04 (mbedtls 3.6) -- which is
# what Gitea Actions uses. macOS Homebrew currently ships mbedtls 4.x where
# these MPI helpers were dropped from the public API, so local Mac testing is
# best-effort -- CI is the source of truth.

CC        ?= gcc
CFLAGS    += -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -I../components/mc_proto -I../components/vendor
LDLIBS    += -lmbedcrypto
# -Wno-deprecated-declarations: mbedtls 2.28 (Ubuntu 22.04 / Gitea runner)
# marks mbedtls_sha512_{starts,update,finish} as deprecated in favour of the
# _ret variants. mbedtls 3.x renamed back to the unsuffixed names and IDF
# v5.5.1 uses 3.6 where these are the primary names. Keep the unsuffixed
# names in source (forward-compatible) and silence the warning here.

# Source files from main/ that the tests link against.
SRC_ED25519     = ../components/vendor/ed25519_mpi.c
SRC_GPS_NMEA    = ../components/mc_proto/gps_parser.c
SRC_COMP_PROTO  = ../components/mc_proto/companion-radio-protocol/mc_companion_command_parser.c \
                  ../components/mc_proto/companion-radio-protocol/mc_companion_serial_interface.c
SRC_MC_CRYPTO   = ../components/mc_crypto/mc_crypto.c
SRC_PAYLOADS    = ../components/mc_proto/meshcore/payload/grp_txt.c \
                  ../components/mc_proto/meshcore/payload/advert.c
SRC_INTEGRATION = ../components/mc_crypto/mc_crypto.c \
                  ../components/mc_proto/meshcore/packet.c \
                  ../components/mc_proto/meshcore/payload/grp_txt.c
SRC_DM          = ../components/mc_crypto/mc_crypto_dm.c \
                  ../components/vendor/ed25519.c \
                  ../components/vendor/ed25519_mpi.c
SRC_ADVERT_SIGN = ../components/mc_proto/advert_sign.c \
                  ../components/mc_proto/meshcore/payload/advert.c \
                  ../components/vendor/ed25519_mpi.c

.PHONY: all test clean

all: test_ed25519 test_gps_nmea test_companion_protocol test_region_limits test_meshcore_packet test_mc_crypto test_meshcore_payloads test_region_lookup test_gps_parser_edge test_integration_message test_mc_crypto_dm test_advert_sign

test_ed25519: test_ed25519.c $(SRC_ED25519)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Pure-C parser test -- no libmbedcrypto link needed. Catches NMEA regressions
# (checksum gate, DDMM→decimal conversion, talker-agnostic match) before merge.
test_gps_nmea: test_gps_nmea.c $(SRC_GPS_NMEA)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# Companion-radio-protocol parser test. Links against the upstream-vendored
# parser + framer translation units that ship in the firmware. Verifies that
# opcode 14 (SET_ADVERT_LATLON) survives full-frame, chunked, and byte-by-byte
# feeds, plus a direct parse_command call.
# -Wno-unused-parameter: upstream mc_companion_write_serial_response has a
# size_t output_buffer_size parameter it never reads. Our IDF build ignores
# this; the host test has -Werror so suppress just for this TU.
test_companion_protocol: test_companion_protocol.c $(SRC_COMP_PROTO)
	$(CC) $(CFLAGS) -Wno-unused-parameter -o $@ $^

# Pure-C regulatory-table test -- no libmbedcrypto link needed. Guards the
# sub-band boundaries, ERP/EIRP power unit and duty-cycle budget that gate
# every transmit on the device.
test_region_limits: test_region_limits.c ../components/mc_proto/region_limits.c
	$(CC) $(CFLAGS) -o $@ $^

# MeshCore packet codec round-trip + error-path test. Guards the wire boundary
# (header bit-packing, transport-code gating, path length-byte encoding).
# packet.c is the upstream protocol mirror (see docs/ARCHITECTURE.md "Wire-boundary
# discipline") and must not diverge locally, so suppress two upstream warnings
# here instead of editing the source: -Wtype-limits fires on the uint8_t size
# bound check and -Wsign-compare on its int/size_t length math.
test_meshcore_packet: test_meshcore_packet.c ../components/mc_proto/meshcore/packet.c
	$(CC) $(CFLAGS) -I../components/mc_proto -Wno-type-limits -Wno-sign-compare -o $@ $^

# Channel crypto + ACK-binding test. Links the same mc_crypto.c shipped in the
# firmware: GRP_TXT encrypt/decrypt round-trip, the channel-match property (a
# wrong key fails the MAC), ciphertext-tamper rejection, and the ACK CRC.
test_mc_crypto: test_mc_crypto.c $(SRC_MC_CRYPTO)
	$(CC) $(CFLAGS) -I../components/mc_crypto -o $@ $^ $(LDLIBS)

# GRP_TXT + ADVERT payload-codec round-trips. packet.c is covered by
# test_meshcore_packet; these guard the two payload bodies that ride inside it.
# -D_GNU_SOURCE: advert.c uses strnlen (POSIX), which strict -std=c11 hides;
# the IDF build defines _GNU_SOURCE already, so match it for the host link.
test_meshcore_payloads: test_meshcore_payloads.c $(SRC_PAYLOADS)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I../components/mc_proto -Wno-type-limits -Wno-sign-compare -o $@ $^

# Deeper region_limits coverage: the country/sub-band lookup + power/duty helpers
# that test_region_limits leaves thin (region_get_country, region_match_subband,
# region_effective_power_dbm with the 2.15 dBi dipole offset, dc-budget).
test_region_lookup: test_region_lookup.c ../components/mc_proto/region_limits.c
	$(CC) $(CFLAGS) -o $@ $^

# Extra gps_parser edge cases beyond test_gps_nmea (checksum rejects, void RMC,
# hemisphere signs, boundary coordinates, unhandled sentences).
test_gps_parser_edge: test_gps_parser_edge.c $(SRC_GPS_NMEA)
	$(CC) $(CFLAGS) -o $@ $^ -lm

# Cross-component integration: a full GRP_TXT message lifecycle proving mc_crypto
# and mc_proto compose (frame -> encrypt -> serialize -> deserialize -> decrypt).
test_integration_message: test_integration_message.c $(SRC_INTEGRATION)
	$(CC) $(CFLAGS) -I../components/mc_proto -I../components/mc_crypto -Wno-type-limits -Wno-sign-compare -o $@ $^ $(LDLIBS)

# DM (TXT_MSG) ed25519-ECDH crypto round-trip. Links the vendored ed25519.c
# (ECDH) + ed25519_mpi.c (keypair) shipped on the badge, with tests/stubs for
# esp_random. -Wno-unused-function: ed25519.c keeps sign/verify statics this
# call graph does not reach.
test_mc_crypto_dm: test_mc_crypto_dm.c $(SRC_DM)
	$(CC) $(CFLAGS) -Wno-unused-function -Istubs -I../components/mc_crypto -o $@ $^ $(LDLIBS)

# ADVERT signature layout. Guards the to_sign byte range (pub_key + timestamp +
# post-signature tail) that meshcore_advert_signable_bytes feeds ed25519_sign --
# the layout half of the direct-advert signing bug; the math half is test_ed25519.
# -D_GNU_SOURCE: advert.c uses strnlen, matching the IDF build.
test_advert_sign: test_advert_sign.c $(SRC_ADVERT_SIGN)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I../components/mc_proto -Wno-type-limits -Wno-sign-compare -o $@ $^ $(LDLIBS)

test: test_ed25519 test_gps_nmea test_companion_protocol test_region_limits test_meshcore_packet test_mc_crypto test_meshcore_payloads test_region_lookup test_gps_parser_edge test_integration_message test_mc_crypto_dm test_advert_sign
	./test_ed25519
	./test_gps_nmea
	./test_companion_protocol
	./test_region_limits
	./test_meshcore_packet
	./test_mc_crypto
	./test_meshcore_payloads
	./test_region_lookup
	./test_gps_parser_edge
	./test_integration_message
	./test_mc_crypto_dm
	./test_advert_sign

clean:
	rm -f test_ed25519 test_gps_nmea test_companion_protocol test_region_limits test_meshcore_packet test_mc_crypto test_meshcore_payloads test_region_lookup test_gps_parser_edge test_integration_message test_mc_crypto_dm test_advert_sign
