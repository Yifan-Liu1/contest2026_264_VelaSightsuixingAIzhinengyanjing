#ifndef __APP_VELASIGHT_INCLUDE_VS_INPUT_H
#define __APP_VELASIGHT_INCLUDE_VS_INPUT_H

#include "vs_types.h"

struct vs_input_s;

int vs_input_open(struct vs_input_s **input);
int vs_input_poll(struct vs_input_s *input, struct vs_input_event_s *event);
void vs_input_close(struct vs_input_s *input);

#endif
