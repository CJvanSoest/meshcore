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
SRC_ED25519 = ../main/ed25519_mpi.c

.PHONY: all test clean

all: test_ed25519

test_ed25519: test_ed25519.c $(SRC_ED25519)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

test: test_ed25519
	./test_ed25519

clean:
	rm -f test_ed25519
