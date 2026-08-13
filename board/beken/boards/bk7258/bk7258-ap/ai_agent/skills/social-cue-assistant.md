# Social Cue Assistant

Read one facial-expression snapshot of the person you are talking to and hand
the wearer a short, low-key hint about how the conversation is landing.

This is VelaSight's core skill. It is a *communication aid*, not a judgement:
every output is a possibility with a confidence attached, and "I can't tell" is
a first-class answer rather than a failure.

## When to use

- The user asks how the other person is reacting: "他听懂了吗", "对方什么反应",
  "am I losing them", "how did that land".
- The user presses the analyse button (short press). The device then calls this
  skill once; it does not run continuously.

Do **not** use this skill to answer "who is this person", to guess personality,
mood history, health or intent, or to compare two people. Those are out of
scope by design — see Boundaries.

## How to use

1. `get_current_time` — the result is stamped, and a stale frame is worthless.
2. `camera_capture` — exactly one frame. If it fails, stop and say so; do not
   retry in a loop and do not fall back to a stored image.
3. Send that frame to the vision model and ask for the JSON in Output, nothing
   else. Ask for at most three cues.
4. Apply the confidence rule below. Then speak **one or two sentences**, in the
   user's language, phrased as a possibility and paired with something the user
   can actually do.
5. Discard the frame. Do not write it to a file, do not put it in memory, do
   not include it in a daily note.

## Output

The vision model must return exactly this shape:

```json
{
  "cues": [
    { "cue": "brow_furrow", "meaning": "possible confusion", "confidence": 0.72 }
  ],
  "overall_confidence": 0.72,
  "unable_to_judge": false,
  "reason": "",
  "suggestion": "slow down and check whether the last point landed"
}
```

- `cue` — an observable, not a conclusion: `brow_furrow`, `gaze_away`,
  `smile`, `lip_press`, `nod`, `head_tilt`, `eyes_widen`, `neutral`.
- `meaning` — always hedged: "possible confusion", not "confused".
- `confidence` — 0.0 to 1.0, per cue.
- `unable_to_judge` — `true` whenever the rule below says so; then `cues` may
  be empty and `reason` must be filled in.
- `reason` — why not: `no_face`, `face_too_small`, `occluded`, `side_profile`,
  `motion_blur`, `overexposed`, `underexposed`, `conflicting_cues`.
- `suggestion` — one actionable clause, or `""` when unable to judge.

## The confidence rule

| `overall_confidence` | What you say |
|---|---|
| ≥ 0.60 | The cue, hedged, plus the suggestion |
| 0.40 – 0.59 | The cue, hedged, and say the read is weak — no suggestion |
| < 0.40 | "无法判断" plus the reason, and nothing else |

Two more cases force `unable_to_judge` regardless of the number:

- **No face, or a face smaller than roughly 1/8 of the frame.** There is
  nothing to read.
- **Conflicting cues** — e.g. `smile` and `brow_furrow` both above 0.6. Say the
  signals disagree instead of picking the louder one.

Never round a low confidence up in the wording. "可能有点困惑" and "他困惑了"
are different claims, and only the first one is available to you.

## Example

User: 他听懂了吗？

```
get_current_time            → 2026-08-13T19:42+08:00
camera_capture              → 41 KB JPEG, 480x480
vision model                → {"cues":[{"cue":"brow_furrow","meaning":"possible
                               confusion","confidence":0.74}],
                               "overall_confidence":0.74,
                               "unable_to_judge":false,"reason":"",
                               "suggestion":"slow down, check the last point"}
```

Reply: 「他眉头微皱，可能没完全跟上。可以放慢一点，问一句刚才那段是否清楚。」

Same call, side profile:

```
vision model → {"cues":[],"overall_confidence":0.11,
                "unable_to_judge":true,"reason":"side_profile","suggestion":""}
```

Reply: 「这个角度看不清表情，判断不了。」— and stop there. Do not guess, and do
not ask the user to reposition the other person.

## Boundaries

These are product requirements, not style preferences:

- **No identity.** Never identify, name, or re-recognise a person; never build
  a profile or remember "this person tends to…".
- **No diagnosis.** No medical, psychiatric, neurological or personality
  claims. Autism, depression, lying, hostility are not yours to infer.
- **No history.** One call, one frame, one answer. Nothing about this analysis
  is written to `MEMORY.md` or a daily note.
- **Say the uncertainty out loud.** If the wearer asks how sure you are, give
  them the number.
- **The wearer is in charge.** Long press cancels; when cancelled, say nothing
  about the frame that was already captured, and discard it.
