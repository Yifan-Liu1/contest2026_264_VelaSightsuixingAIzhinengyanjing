/****************************************************************************
 * app/velasight/include/vs_faces.h
 *
 * The social session's expression graphics, and the one mapping from an
 * emotion to the face that reports it.
 *
 * The images themselves are generated -- see velasight_faces_44.c and the
 * generator that writes it -- and this header is the only place that knows
 * which of them belongs to which bucket.  Kept out of vs_display.c because the
 * mapping is a product decision about what the four emotions look like, while
 * that file is about where things sit on a 160x160 circle.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/
#ifndef __APP_VELASIGHT_INCLUDE_VS_FACES_H
#define __APP_VELASIGHT_INCLUDE_VS_FACES_H

#include <lvgl/lvgl.h>

#include "vs_types.h"

/* 44x44 RGB565, with the panel background already composited into every
 * antialiased edge.  That is what keeps them cheap: an image with no alpha
 * takes lv_draw_sw_img.c's plain-copy branch straight into the RGB565
 * framebuffer, with no per-pixel blend on a panel that costs 26 ms a frame.
 *
 * The consequence is that they may only be drawn on that background, which is
 * true by construction -- vs_panel_set_face() is the sole user and it puts them
 * on the status screen, whose background is the colour they were composited
 * against.
 */

LV_IMAGE_DECLARE(velasight_face_calm_44);
LV_IMAGE_DECLARE(velasight_face_happy_44);
LV_IMAGE_DECLARE(velasight_face_confused_44);
LV_IMAGE_DECLARE(velasight_face_tense_44);

/* The face for a reading, colour included.
 *
 * Each image carries its bucket's colour baked in -- the same four values
 * vs_app.c assigns and cloud_classify_emotion() sends -- so the emotion alone
 * picks both the shape and the ink.  That is deliberate rather than incidental:
 * it means snapshot->emotion_color never has to reach the status panel, and so
 * it can stay out of vs_status_changed(), where its absence is documented and
 * intended.  A runtime recolor would have had to put it back.
 *
 * VS_EMOTION_NONE shares the neutral face.  emotion_color already gives it the
 * same VS_COLOR_NEUTRAL a calm frame gets, on the reasoning recorded there: a
 * page reporting calm and a page reporting nothing are meant to look alike, so
 * a session shows a watching face from the moment it starts rather than waiting
 * for the first cloud reading to have something to draw.
 */

static inline const lv_image_dsc_t *vs_face_for_emotion(enum vs_emotion_e
                                                        emotion)
{
  switch (emotion)
    {
      case VS_EMOTION_HAPPY:
        return &velasight_face_happy_44;

      case VS_EMOTION_CONFUSED:
        return &velasight_face_confused_44;

      case VS_EMOTION_TENSE:
        return &velasight_face_tense_44;

      case VS_EMOTION_NONE:
      case VS_EMOTION_CALM:
      default:
        return &velasight_face_calm_44;
    }
}

#endif
