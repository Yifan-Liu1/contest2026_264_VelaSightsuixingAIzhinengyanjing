/****************************************************************************
 * app/social_cue/social_cue_skill.h
 *
 * GENERATED FILE -- do not edit.  Source of truth:
 *   board/beken/boards/bk7258/bk7258-ap/ai_agent/skills/social-cue-assistant.md
 * Regenerate with:
 *   python3 app/social_cue/tools/md2c.py \
 *       board/beken/boards/bk7258/bk7258-ap/ai_agent/skills/social-cue-assistant.md \
 *       app/social_cue/social_cue_skill.h SOCIAL_CUE_SKILL_MD
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __SOCIAL_CUE_SKILL_H
#define __SOCIAL_CUE_SKILL_H

#define SOCIAL_CUE_SKILL_MD \
    "# Social Cue Assistant\n" \
    "\n" \
    "Read one facial-expression snapshot of the person you are talking to and hand\n" \
    "the wearer a short, low-key hint about how the conversation is landing.\n" \
    "\n" \
    "This is VelaSight's core skill. It is a *communication aid*, not a judgement:\n" \
    "every output is a possibility with a confidence attached, and \"I can't tell\" is\n" \
    "a first-class answer rather than a failure.\n" \
    "\n" \
    "## When to use\n" \
    "\n" \
    "- The user asks how the other person is reacting: \"他听懂了吗\", \"对方什么反应\",\n" \
    "  \"am I losing them\", \"how did that land\".\n" \
    "- The user presses the analyse button (short press). The device then calls this\n" \
    "  skill once; it does not run continuously.\n" \
    "\n" \
    "Do **not** use this skill to answer \"who is this person\", to guess personality,\n" \
    "mood history, health or intent, or to compare two people. Those are out of\n" \
    "scope by design — see Boundaries.\n" \
    "\n" \
    "## How to use\n" \
    "\n" \
    "1. `get_current_time` — the result is stamped, and a stale frame is worthless.\n" \
    "2. `camera_capture` — exactly one frame. If it fails, stop and say so; do not\n" \
    "   retry in a loop and do not fall back to a stored image.\n" \
    "3. Send that frame to the vision model and ask for the JSON in Output, nothing\n" \
    "   else. Ask for at most three cues.\n" \
    "4. Apply the confidence rule below. Then speak **one or two sentences**, in the\n" \
    "   user's language, phrased as a possibility and paired with something the user\n" \
    "   can actually do.\n" \
    "5. Discard the frame. Do not write it to a file, do not put it in memory, do\n" \
    "   not include it in a daily note.\n" \
    "\n" \
    "## Output\n" \
    "\n" \
    "The vision model must return exactly this shape:\n" \
    "\n" \
    "```json\n" \
    "{\n" \
    "  \"cues\": [\n" \
    "    { \"cue\": \"brow_furrow\", \"meaning\": \"possible confusion\", \"confidence\": 0.72 }\n" \
    "  ],\n" \
    "  \"overall_confidence\": 0.72,\n" \
    "  \"unable_to_judge\": false,\n" \
    "  \"reason\": \"\",\n" \
    "  \"suggestion\": \"slow down and check whether the last point landed\"\n" \
    "}\n" \
    "```\n" \
    "\n" \
    "- `cue` — an observable, not a conclusion: `brow_furrow`, `gaze_away`,\n" \
    "  `smile`, `lip_press`, `nod`, `head_tilt`, `eyes_widen`, `neutral`.\n" \
    "- `meaning` — always hedged: \"possible confusion\", not \"confused\".\n" \
    "- `confidence` — 0.0 to 1.0, per cue.\n" \
    "- `unable_to_judge` — `true` whenever the rule below says so; then `cues` may\n" \
    "  be empty and `reason` must be filled in.\n" \
    "- `reason` — why not: `no_face`, `face_too_small`, `occluded`, `side_profile`,\n" \
    "  `motion_blur`, `overexposed`, `underexposed`, `conflicting_cues`.\n" \
    "- `suggestion` — one actionable clause, or `\"\"` when unable to judge.\n" \
    "\n" \
    "## The confidence rule\n" \
    "\n" \
    "| `overall_confidence` | What you say |\n" \
    "|---|---|\n" \
    "| ≥ 0.60 | The cue, hedged, plus the suggestion |\n" \
    "| 0.40 – 0.59 | The cue, hedged, and say the read is weak — no suggestion |\n" \
    "| < 0.40 | \"无法判断\" plus the reason, and nothing else |\n" \
    "\n" \
    "Two more cases force `unable_to_judge` regardless of the number:\n" \
    "\n" \
    "- **No face, or a face smaller than roughly 1/8 of the frame.** There is\n" \
    "  nothing to read.\n" \
    "- **Conflicting cues** — e.g. `smile` and `brow_furrow` both above 0.6. Say the\n" \
    "  signals disagree instead of picking the louder one.\n" \
    "\n" \
    "Never round a low confidence up in the wording. \"可能有点困惑\" and \"他困惑了\"\n" \
    "are different claims, and only the first one is available to you.\n" \
    "\n" \
    "## Example\n" \
    "\n" \
    "User: 他听懂了吗？\n" \
    "\n" \
    "```\n" \
    "get_current_time            → 2026-08-13T19:42+08:00\n" \
    "camera_capture              → 41 KB JPEG, 480x480\n" \
    "vision model                → {\"cues\":[{\"cue\":\"brow_furrow\",\"meaning\":\"possible\n" \
    "                               confusion\",\"confidence\":0.74}],\n" \
    "                               \"overall_confidence\":0.74,\n" \
    "                               \"unable_to_judge\":false,\"reason\":\"\",\n" \
    "                               \"suggestion\":\"slow down, check the last point\"}\n" \
    "```\n" \
    "\n" \
    "Reply: 「他眉头微皱，可能没完全跟上。可以放慢一点，问一句刚才那段是否清楚。」\n" \
    "\n" \
    "Same call, side profile:\n" \
    "\n" \
    "```\n" \
    "vision model → {\"cues\":[],\"overall_confidence\":0.11,\n" \
    "                \"unable_to_judge\":true,\"reason\":\"side_profile\",\"suggestion\":\"\"}\n" \
    "```\n" \
    "\n" \
    "Reply: 「这个角度看不清表情，判断不了。」— and stop there. Do not guess, and do\n" \
    "not ask the user to reposition the other person.\n" \
    "\n" \
    "## Boundaries\n" \
    "\n" \
    "These are product requirements, not style preferences:\n" \
    "\n" \
    "- **No identity.** Never identify, name, or re-recognise a person; never build\n" \
    "  a profile or remember \"this person tends to…\".\n" \
    "- **No diagnosis.** No medical, psychiatric, neurological or personality\n" \
    "  claims. Autism, depression, lying, hostility are not yours to infer.\n" \
    "- **No history.** One call, one frame, one answer. Nothing about this analysis\n" \
    "  is written to `MEMORY.md` or a daily note.\n" \
    "- **Say the uncertainty out loud.** If the wearer asks how sure you are, give\n" \
    "  them the number.\n" \
    "- **The wearer is in charge.** Long press cancels; when cancelled, say nothing\n" \
    "  about the frame that was already captured, and discard it.\n" \
    "\n"

#endif /* __SOCIAL_CUE_SKILL_H */
