#include <nuttx/config.h>

#include <nuttx/board.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arch/board/board.h>

#include "include/vs_input.h"

#define VS_INPUT_DEBOUNCE_SAMPLES 3

struct vs_key_state_s
{
  bool raw;
  bool stable;
  bool long_sent;
  uint8_t samples;
  uint32_t pressed_ms;
  uint8_t last_progress;
};

struct vs_input_s
{
  struct vs_key_state_s key[VS_KEY_COUNT];
  bool combo_sent;
  bool combo_started;
  uint32_t combo_start_ms;
};

static uint32_t vs_input_now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

static uint32_t vs_input_mask(enum vs_key_e key)
{
  static const uint32_t masks[VS_KEY_COUNT] =
  {
    BUTTON_POWER_BIT,
    BUTTON_VOLUME_UP_BIT,
    BUTTON_VOLUME_DOWN_BIT
  };

  return masks[key];
}

int vs_input_open(struct vs_input_s **input)
{
  struct vs_input_s *state;

  if (input == NULL)
    {
      return -EINVAL;
    }

  state = calloc(1, sizeof(*state));
  if (state == NULL)
    {
      return -ENOMEM;
    }

  *input = state;
  return 0;
}

int vs_input_poll(struct vs_input_s *input, struct vs_input_event_s *event)
{
  uint32_t now;
  uint32_t buttons;
  int key;

  if (input == NULL || event == NULL)
    {
      return -EINVAL;
    }

  memset(event, 0, sizeof(*event));
  event->type = VS_INPUT_NONE;
  now = vs_input_now_ms();
  buttons = board_buttons();

  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      struct vs_key_state_s *state = &input->key[key];
      bool raw = (buttons & vs_input_mask(key)) != 0;

      if (raw != state->raw)
        {
          state->raw = raw;
          state->samples = 1;
        }
      else if (state->samples < VS_INPUT_DEBOUNCE_SAMPLES)
        {
          state->samples++;
        }

      if (state->samples == VS_INPUT_DEBOUNCE_SAMPLES &&
          state->stable != state->raw)
        {
          state->stable = state->raw;
          if (state->stable)
            {
              state->pressed_ms = now;
              state->long_sent = false;
              state->last_progress = 0xff;
            }
          else if (!state->long_sent && !input->combo_sent &&
                   !input->combo_started &&
                   now - state->pressed_ms >= CONFIG_VS_SHORT_PRESS_MIN_MS &&
                   now - state->pressed_ms <= CONFIG_VS_SHORT_PRESS_MAX_MS)
            {
              event->type = VS_INPUT_SHORT;
              event->key = key;
              return 1;
            }
          else if (!state->long_sent && !input->combo_sent &&
                   !input->combo_started &&
                   now - state->pressed_ms > CONFIG_VS_SHORT_PRESS_MAX_MS)
            {
              state->long_sent = true;
              event->type = VS_INPUT_CANCEL;
              event->key = key;
              return 1;
            }
        }
    }

  if (input->key[VS_KEY_BACK].stable && input->key[VS_KEY_NEXT].stable)
    {
      uint32_t start = input->key[VS_KEY_BACK].pressed_ms;
      uint32_t held;
      uint8_t progress;

      if (input->key[VS_KEY_NEXT].pressed_ms > start)
        {
          start = input->key[VS_KEY_NEXT].pressed_ms;
        }

      if (!input->combo_started)
        {
          input->combo_started = true;
          input->combo_start_ms = start;
          input->key[VS_KEY_BACK].last_progress = 0xff;
          event->type = VS_INPUT_COMBO_PROGRESS;
          event->key = VS_KEY_BACK;
          event->progress = 0;
          return 1;
        }

      held = now - start;
      progress = (uint8_t)(held * 100u / CONFIG_VS_COMBO_PRESS_MS);
      if (progress > 100)
        {
          progress = 100;
        }

      if (!input->combo_sent && progress < 100 &&
          progress / 4u * 4u != input->key[VS_KEY_BACK].last_progress)
        {
          input->key[VS_KEY_BACK].last_progress = progress / 4u * 4u;
          event->type = VS_INPUT_COMBO_PROGRESS;
          event->key = VS_KEY_BACK;
          event->progress = input->key[VS_KEY_BACK].last_progress;
          return 1;
        }

      if (!input->combo_sent && held >= CONFIG_VS_COMBO_PRESS_MS)
        {
          input->combo_sent = true;
          input->key[VS_KEY_BACK].long_sent = true;
          input->key[VS_KEY_NEXT].long_sent = true;
          event->type = VS_INPUT_NET_TOGGLE;
          return 1;
        }
    }
  else if (input->combo_started && !input->combo_sent)
    {
      uint32_t held = now - input->combo_start_ms;

      input->combo_sent = true;
      input->key[VS_KEY_BACK].long_sent = true;
      input->key[VS_KEY_NEXT].long_sent = true;
      if (held >= CONFIG_VS_COMBO_PRESS_MS)
        {
          event->type = VS_INPUT_NET_TOGGLE;
          return 1;
        }

      event->type = VS_INPUT_COMBO_CANCEL;
      return 1;
    }
  else if (!input->key[VS_KEY_BACK].stable &&
           !input->key[VS_KEY_NEXT].stable)
    {
      input->combo_sent = false;
      input->combo_started = false;
    }

  for (key = 0; key < VS_KEY_COUNT; key++)
    {
      struct vs_key_state_s *state = &input->key[key];
      uint32_t held;

      if (!state->stable || state->long_sent || input->combo_sent ||
          input->combo_started)
        {
          continue;
        }

      held = now - state->pressed_ms;
      if (held >= CONFIG_VS_LONG_PRESS_MS)
        {
          state->long_sent = true;
          event->type = VS_INPUT_LONG;
          event->key = key;
          event->progress = 100;
          return 1;
        }

      if (key == VS_KEY_CONFIRM || key == VS_KEY_BACK)
        {
          uint32_t progress_ms;
          uint32_t progress_range;
          uint8_t progress;

          if (held <= CONFIG_VS_SHORT_PRESS_MAX_MS)
            {
              continue;
            }

          progress_ms = held - CONFIG_VS_SHORT_PRESS_MAX_MS;
          progress_range = CONFIG_VS_LONG_PRESS_MS >
                           CONFIG_VS_SHORT_PRESS_MAX_MS ?
                           CONFIG_VS_LONG_PRESS_MS -
                           CONFIG_VS_SHORT_PRESS_MAX_MS : 1;
          progress = (uint8_t)(progress_ms * 100u / progress_range);

          progress = (uint8_t)(progress / 4u * 4u);
          if (progress != state->last_progress)
            {
              state->last_progress = progress;
              event->type = VS_INPUT_PROGRESS;
              event->key = key;
              event->progress = progress;
              return 1;
            }
        }
    }

  return 0;
}

void vs_input_close(struct vs_input_s *input)
{
  free(input);
}
