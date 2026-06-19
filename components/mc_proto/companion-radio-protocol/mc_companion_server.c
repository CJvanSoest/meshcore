#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "mc_companion.h"
#include "mc_companion_serial_interface.h"

void mc_companion_server_serialize_response(companion_response_packet_t* packet, uint16_t args_length,
                                            size_t output_buffer_size, uint8_t* out_framed_data,
                                            size_t* out_framed_data_length) {
    uint16_t position         = 0;
    out_framed_data[position] = packet->response;
    position++;
    if (args_length > 0) {
        memcpy(&out_framed_data[position], packet->args, args_length);
    }
    position                += args_length;
    *out_framed_data_length  = position;
}

mc_companion_command_parser_error_t mc_companion_server_parse_request(const uint8_t* data, uint16_t length,
                                                                      companion_command_packet_t* out_packet) {
    return mc_companion_parse_command(data, length, out_packet);
}
