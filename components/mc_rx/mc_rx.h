// SPDX-FileCopyrightText: 2026 CJ van Soest
// SPDX-License-Identifier: MIT
// SPDX-FileContributor: Ilias el Matani <hello@ilias.codes>
//
// Register the MeshCore RX sink with the radio layer. Call once at boot,
// before radio_start_tasks, so received packets are decrypted and delivered.
#pragma once

void mc_rx_init(void);
