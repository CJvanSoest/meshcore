#pragma once

#include <stddef.h>
#include <stdint.h>
#include "mc_companion.h"
#include "mc_companion_command_parser.h"

#ifndef FIELD_SIZE
#define FIELD_SIZE(type, field) (sizeof(((type*)0)->field))
#endif

void mc_companion_server_serialize_response(companion_response_packet_t* packet, uint16_t args_length,
                                            size_t output_buffer_size, uint8_t* out_framed_data,
                                            size_t* out_framed_data_length);
mc_companion_command_parser_error_t mc_companion_server_parse_request(const uint8_t* data, uint16_t length,
                                                                      companion_command_packet_t* out_packet);
