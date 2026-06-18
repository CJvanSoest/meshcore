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
CFLAGS    += -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -O1 -I../main
LDLIBS    += -lmbedcrypto
# -Wno-deprecated-declarations: mbedtls 2.28 (Ubuntu 22.04 / Gitea runner)
# marks mbedtls_sha512_{starts,update,finish} as deprecated in favour of the
# _ret variants. mbedtls 3.x renamed back to the unsuffixed names and IDF
# v5.5.1 uses 3.6 where these are the primary names. Keep the unsuffixed
# names in source (forward-compatible) and silence the warning here.

# Source files from main/ that the tests link against.
SRC_ED25519     = ../main/ed25519_mpi.c
SRC_GPS_NMEA    = ../main/gps_parser.c
SRC_COMP_PROTO  = ../main/companion-radio-protocol/mc_companion_command_parser.c \
                  ../main/companion-radio-protocol/mc_companion_serial_interface.c

.PHONY: all test clean

all: test_ed25519 test_gps_nmea test_companion_protocol test_region_limits test_meshcore_packet

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
test_region_limits: test_region_limits.c ../main/region_limits.c
	$(CC) $(CFLAGS) -o $@ $^

# MeshCore packet codec round-trip + error-path test. Guards the wire boundary
# (header bit-packing, transport-code gating, path length-byte encoding).
# packet.c is the upstream protocol mirror (see ARCHITECTURE.md "Wire-boundary
# discipline") and must not diverge locally, so suppress two upstream warnings
# here instead of editing the source: -Wtype-limits fires on the uint8_t size
# bound check and -Wsign-compare on its int/size_t length math.
test_meshcore_packet: test_meshcore_packet.c ../main/meshcore/packet.c
	$(CC) $(CFLAGS) -Wno-type-limits -Wno-sign-compare -o $@ $^

test: test_ed25519 test_gps_nmea test_companion_protocol test_region_limits test_meshcore_packet
	./test_ed25519
	./test_gps_nmea
	./test_companion_protocol
	./test_region_limits
	./test_meshcore_packet

clean:
	rm -f test_ed25519 test_gps_nmea test_companion_protocol test_region_limits test_meshcore_packet
