/****************************************************************************
 * app/social_cue/social_cue_main.c
 *
 * VelaSight's表情线索 state machine, end to end, without the cloud.
 *
 * What this is for: the plan's week-4 deliverable is the Agent state flow
 * idle -> capturing -> analyzing -> notifying -> cancelled plus one Mock
 * closed loop, and the risk table's fallback for "Wi-Fi not through yet" is
 * exactly this -- demonstrate the flow with a local mock response.  So the
 * only mocked step here is the network request.  Everything else is real:
 * the frame comes from /dev/video0 through the same V4L2 sequence
 * packages/ai_agent's tool_camera.c uses, the confidence policy is the one
 * from the skill document, and the haptic output drives /dev/pwm0.
 *
 * The policy is the point.  A demo that always prints a cheerful verdict
 * proves nothing; what has to be visible is the *refusal* path -- low
 * confidence, no face, conflicting cues -- because that is what the product
 * promises and what a reviewer should be able to trigger on demand.
 *
 * Usage:
 *   social_cue                 one cycle, real capture, mock analysis
 *   social_cue case <0..4>     force a specific mock verdict (see below)
 *   social_cue mock            skip the camera too (no hardware needed)
 *   social_cue schema          print the JSON contract the vision model owes us
 *   social_cue install         write the skill document to the agent's skills dir
 *   social_cue n=<count>       repeat the cycle
 *
 * Ctrl-C during capture or analysis takes the cancelled path: the frame is
 * released and nothing is reported, which is the privacy requirement rather
 * than a nicety.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/fs/ioctl.h>
#include <nuttx/timers/pwm.h>

#include <sys/videoio.h>

#include "social_cue_skill.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SC_VIDEO_DEV      "/dev/video0"
#define SC_PWM_DEV        "/dev/pwm0"

/* Where the agent looks for skills.  Kept as a literal rather than including
 * the agent's headers: this app must build whether or not ai_agent is in the
 * configuration.  It matches CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR.
 */

#ifdef CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#  define SC_DATA_DIR     CONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR
#else
#  define SC_DATA_DIR     "/mnt/sdnand/ai_agent"
#endif
#define SC_SKILLS_DIR     SC_DATA_DIR "/skills"
#define SC_SKILL_PATH     SC_SKILLS_DIR "/social-cue-assistant.md"

/* One frame, JPEG.  480x480 is the only geometry this sensor driver offers
 * that is square, and sizeimage is what decides the buffer size for a
 * compressed format (docs/reference/camera.md 14.2).
 */

#define SC_WIDTH          480
#define SC_HEIGHT         480
#define SC_SIZEIMAGE      (160 * 1024)
#define SC_NBUFFERS       2
#define SC_CAPTURE_TMO_MS 5000

/* The thresholds from the skill document.  They live here as well because
 * this is the code that has to enforce them.
 */

#define SC_CONF_REPORT    0.60f
#define SC_CONF_WEAK      0.40f
#define SC_CONFLICT_CONF  0.60f

/* Haptics.  Three patterns, deliberately distinguishable without looking:
 * one short pulse for "capturing", two for "here is a hint", one long for
 * "I cannot tell".  1 kHz at 70% duty is what bk7258_motor.c uses.
 */

#define SC_MOTOR_FREQ     1000
#define SC_MOTOR_DUTY     ((ub16_t)((7u * 65536u) / 10u))

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum sc_state_e
{
  SC_IDLE = 0,
  SC_CAPTURING,
  SC_ANALYZING,
  SC_NOTIFYING,
  SC_CANCELLED
};

/* One cue as the vision model reports it. */

struct sc_cue_s
{
  const char *cue;        /* observable, e.g. "brow_furrow" */
  const char *meaning;    /* hedged reading, e.g. "possible confusion" */
  float       confidence;
};

/* The whole response.  This mirrors the JSON contract in the skill document;
 * when the real request replaces the mock, only the filling of this struct
 * changes.
 */

struct sc_verdict_s
{
  struct sc_cue_s cues[3];
  int             ncues;
  float           overall;
  bool            unable;
  const char     *reason;      /* no_face / occluded / ... */
  const char     *suggestion;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_state_names[] =
{
  "idle", "capturing", "analyzing", "notifying", "cancelled"
};

static volatile sig_atomic_t g_cancelled;

/* The mock responses.  Case 0 is the happy path; the rest exist so that every
 * branch of the policy can be demonstrated on demand rather than waited for.
 */

static const struct sc_verdict_s g_mock_cases[] =
{
  { /* 0: clear, actionable */
    .cues = { { "brow_furrow", "possible confusion", 0.74f } },
    .ncues = 1, .overall = 0.74f, .unable = false, .reason = "",
    .suggestion = "slow down, then check whether the last point landed"
  },
  { /* 1: weak read -- report the cue, withhold the advice */
    .cues = { { "gaze_away", "possible distraction", 0.48f } },
    .ncues = 1, .overall = 0.48f, .unable = false, .reason = "",
    .suggestion = ""
  },
  { /* 2: below the floor */
    .cues = { { "neutral", "no clear signal", 0.22f } },
    .ncues = 1, .overall = 0.22f, .unable = false, .reason = "",
    .suggestion = ""
  },
  { /* 3: nothing to read */
    .ncues = 0, .overall = 0.11f, .unable = true, .reason = "side_profile",
    .suggestion = ""
  },
  { /* 4: the signals disagree */
    .cues = { { "smile", "possible warmth", 0.71f },
              { "brow_furrow", "possible confusion", 0.68f } },
    .ncues = 2, .overall = 0.70f, .unable = false, .reason = "",
    .suggestion = "ask directly rather than reading the face"
  },
};

#define SC_NMOCKS (sizeof(g_mock_cases) / sizeof(g_mock_cases[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void sc_sigint(int signo)
{
  (void)signo;
  g_cancelled = 1;
}

static void sc_enter(enum sc_state_e *state, enum sc_state_e next)
{
  printf("social_cue: %s -> %s\n", g_state_names[*state], g_state_names[next]);
  *state = next;
}

/****************************************************************************
 * Name: sc_buzz
 *
 * Description:
 *   Pulse the vibration motor count times.  Best-effort: a board without
 *   /dev/pwm0 still runs the state machine, it just says so once.
 *
 ****************************************************************************/

static void sc_buzz(int count, int on_ms, int off_ms)
{
  struct pwm_info_s info;
  int fd;
  int i;

  fd = open(SC_PWM_DEV, O_RDONLY);
  if (fd < 0)
    {
      printf("social_cue: no %s (errno=%d), haptics skipped\n",
             SC_PWM_DEV, errno);
      return;
    }

  memset(&info, 0, sizeof(info));
  info.frequency = SC_MOTOR_FREQ;
  info.duty      = SC_MOTOR_DUTY;

  if (ioctl(fd, PWMIOC_SETCHARACTERISTICS, (unsigned long)&info) < 0)
    {
      printf("social_cue: PWMIOC_SETCHARACTERISTICS failed, errno=%d\n",
             errno);
      close(fd);
      return;
    }

  for (i = 0; i < count; i++)
    {
      ioctl(fd, PWMIOC_START, 0);
      usleep((useconds_t)on_ms * 1000);
      ioctl(fd, PWMIOC_STOP, 0);
      if (i + 1 < count)
        {
          usleep((useconds_t)off_ms * 1000);
        }
    }

  close(fd);
}

/****************************************************************************
 * Name: sc_capture
 *
 * Description:
 *   Grab one JPEG frame.  Same ioctl order as packages/ai_agent's
 *   tool_camera.c: S_FMT -> REQBUFS -> QUERYBUF+mmap -> QBUF -> STREAMON ->
 *   poll -> DQBUF.  The bytes are counted and then dropped; nothing here
 *   needs the pixels, and keeping them would violate the privacy rule for no
 *   benefit.
 *
 * Returned Value:
 *   Frame length in bytes, or a negated errno.
 *
 ****************************************************************************/

static ssize_t sc_capture(void)
{
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2_format fmt;
  struct pollfd pfd;
  void *addr[SC_NBUFFERS];
  size_t len[SC_NBUFFERS];
  ssize_t used = -EIO;
  int fd;
  int i;

  for (i = 0; i < SC_NBUFFERS; i++)
    {
      addr[i] = NULL;
      len[i]  = 0;
    }

  fd = open(SC_VIDEO_DEV, O_RDWR);
  if (fd < 0)
    {
      printf("social_cue: open %s failed, errno=%d\n", SC_VIDEO_DEV, errno);
      return -errno;
    }

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = SC_WIDTH;
  fmt.fmt.pix.height      = SC_HEIGHT;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_JPEG;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;
  fmt.fmt.pix.sizeimage   = SC_SIZEIMAGE;

  if (ioctl(fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      printf("social_cue: S_FMT JPEG %dx%d failed, errno=%d\n",
             SC_WIDTH, SC_HEIGHT, errno);
      used = -errno;
      goto out;
    }

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count  = SC_NBUFFERS;

  if (ioctl(fd, VIDIOC_REQBUFS, (unsigned long)&req) < 0)
    {
      printf("social_cue: REQBUFS failed, errno=%d\n", errno);
      used = -errno;
      goto out;
    }

  for (i = 0; i < SC_NBUFFERS; i++)
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.index  = (uint32_t)i;

      if (ioctl(fd, VIDIOC_QUERYBUF, (unsigned long)&buf) < 0)
        {
          printf("social_cue: QUERYBUF[%d] failed, errno=%d\n", i, errno);
          used = -errno;
          goto out;
        }

      addr[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fd, (off_t)buf.m.offset);
      if (addr[i] == MAP_FAILED)
        {
          addr[i] = NULL;
          printf("social_cue: mmap[%d] failed, errno=%d\n", i, errno);
          used = -errno;
          goto out;
        }

      len[i] = buf.length;

      if (ioctl(fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
        {
          printf("social_cue: QBUF[%d] failed, errno=%d\n", i, errno);
          used = -errno;
          goto out;
        }
    }

  if (ioctl(fd, VIDIOC_STREAMON, (unsigned long)&req.type) < 0)
    {
      printf("social_cue: STREAMON failed, errno=%d\n", errno);
      used = -errno;
      goto out;
    }

  pfd.fd     = fd;
  pfd.events = POLLIN;

  if (poll(&pfd, 1, SC_CAPTURE_TMO_MS) <= 0)
    {
      printf("social_cue: no frame within %d ms\n", SC_CAPTURE_TMO_MS);
      used = -ETIMEDOUT;
    }
  else
    {
      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_MMAP;

      if (ioctl(fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
        {
          printf("social_cue: DQBUF failed, errno=%d\n", errno);
          used = -errno;
        }
      else
        {
          used = (ssize_t)buf.bytesused;
        }
    }

  ioctl(fd, VIDIOC_STREAMOFF, (unsigned long)&req.type);

out:
  for (i = 0; i < SC_NBUFFERS; i++)
    {
      if (addr[i] != NULL)
        {
          munmap(addr[i], len[i]);
        }
    }

  close(fd);
  return used;
}

/****************************************************************************
 * Name: sc_print_request / sc_print_response
 *
 * Description:
 *   Show what would go up and what came back.  Printed rather than sent,
 *   because the mock is the point -- but printed in the exact shape the real
 *   request uses, so the swap is a small diff and a reviewer can see the
 *   contract.
 *
 ****************************************************************************/

static void sc_print_request(ssize_t frame_len)
{
  printf("social_cue: request  {\"model\":\"vision\",\"image_bytes\":%ld,"
         "\"max_cues\":3,\"schema\":\"social-cue/v1\"}\n", (long)frame_len);
}

static void sc_print_response(const struct sc_verdict_s *v)
{
  int i;

  printf("social_cue: response {\"cues\":[");
  for (i = 0; i < v->ncues; i++)
    {
      printf("%s{\"cue\":\"%s\",\"meaning\":\"%s\",\"confidence\":%.2f}",
             i ? "," : "", v->cues[i].cue, v->cues[i].meaning,
             (double)v->cues[i].confidence);
    }

  printf("],\"overall_confidence\":%.2f,\"unable_to_judge\":%s,"
         "\"reason\":\"%s\",\"suggestion\":\"%s\"}\n",
         (double)v->overall, v->unable ? "true" : "false",
         v->reason, v->suggestion);
}

/****************************************************************************
 * Name: sc_conflicting
 *
 * Description:
 *   Two cues that both read strongly and point in different directions.  The
 *   skill document forces "unable to judge" here; without this check the
 *   policy would happily report whichever cue the model listed first, which
 *   is the failure mode the rule exists to prevent.
 *
 ****************************************************************************/

static bool sc_conflicting(const struct sc_verdict_s *v)
{
  int strong = 0;
  int i;

  for (i = 0; i < v->ncues; i++)
    {
      if (v->cues[i].confidence >= SC_CONFLICT_CONF)
        {
          strong++;
        }
    }

  return strong >= 2;
}

/****************************************************************************
 * Name: sc_notify
 *
 * Description:
 *   Apply the confidence policy and produce the wearer-facing line.  This is
 *   the whole product logic in one place, on purpose.
 *
 ****************************************************************************/

static void sc_notify(const struct sc_verdict_s *v)
{
  if (v->unable)
    {
      printf("social_cue: SAY  \"看不清表情，判断不了（%s）。\"\n", v->reason);
      printf("social_cue: policy unable_to_judge=true reason=%s\n", v->reason);
      sc_buzz(1, 400, 0);
      return;
    }

  if (sc_conflicting(v))
    {
      printf("social_cue: SAY  \"信号不一致，%s 和 %s 都很明显，直接问一句更稳。\"\n",
             v->cues[0].meaning, v->cues[1].meaning);
      printf("social_cue: policy conflicting_cues -> withheld a verdict\n");
      sc_buzz(1, 400, 0);
      return;
    }

  if (v->overall < SC_CONF_WEAK)
    {
      printf("social_cue: SAY  \"没有明显线索，判断不了。\"\n");
      printf("social_cue: policy overall=%.2f < %.2f -> unable\n",
             (double)v->overall, (double)SC_CONF_WEAK);
      sc_buzz(1, 400, 0);
      return;
    }

  if (v->overall < SC_CONF_REPORT)
    {
      printf("social_cue: SAY  \"可能有点%s，不过这个判断不太确定。\"\n",
             v->cues[0].meaning);
      printf("social_cue: policy overall=%.2f in [%.2f,%.2f) -> cue only, "
             "no suggestion\n", (double)v->overall,
             (double)SC_CONF_WEAK, (double)SC_CONF_REPORT);
      sc_buzz(2, 80, 120);
      return;
    }

  printf("social_cue: SAY  \"他%s（%s，%.0f%%）。建议：%s。\"\n",
         v->cues[0].cue, v->cues[0].meaning,
         (double)(v->overall * 100.0f), v->suggestion);
  printf("social_cue: policy overall=%.2f >= %.2f -> cue + suggestion\n",
         (double)v->overall, (double)SC_CONF_REPORT);
  sc_buzz(2, 80, 120);
}

/****************************************************************************
 * Name: sc_install_skill
 *
 * Description:
 *   Write the skill document where the agent will read it.  The agent installs
 *   its own built-ins but has no idea about this one, and the board has no way
 *   to receive a file from the host, so the firmware carries the text.
 *
 ****************************************************************************/

static int sc_install_skill(void)
{
  static const char skill[] = SOCIAL_CUE_SKILL_MD;
  FILE *f;
  size_t n;

  /* The agent creates this tree itself when it starts, but this command has
   * to work whether or not it has run yet.
   */

  mkdir(SC_DATA_DIR, 0755);
  mkdir(SC_SKILLS_DIR, 0755);

  f = fopen(SC_SKILL_PATH, "w");
  if (f == NULL)
    {
      printf("social_cue: cannot write %s (errno=%d).\n",
             SC_SKILL_PATH, errno);
      printf("social_cue: is /mnt mounted?  bring-up mounts it on littlefs; "
             "check the boot log for \"ramdisk: /mnt mounted\".\n");
      return -errno;
    }

  n = fwrite(skill, 1, sizeof(skill) - 1, f);
  fclose(f);

  if (n != sizeof(skill) - 1)
    {
      printf("social_cue: short write to %s (%zu of %zu)\n",
             SC_SKILL_PATH, n, sizeof(skill) - 1);
      return -EIO;
    }

  printf("social_cue: installed %s (%zu bytes)\n", SC_SKILL_PATH, n);
  printf("social_cue: the agent picks it up on its next prompt build; "
         "no restart needed.\n");
  return OK;
}

/****************************************************************************
 * Name: sc_cycle
 ****************************************************************************/

static int sc_cycle(bool real_capture, const struct sc_verdict_s *verdict)
{
  enum sc_state_e state = SC_IDLE;
  ssize_t frame_len = 0;

  g_cancelled = 0;

  sc_enter(&state, SC_CAPTURING);
  sc_buzz(1, 60, 0);

  if (real_capture)
    {
      frame_len = sc_capture();
      if (frame_len < 0)
        {
          printf("social_cue: capture failed (%d); no verdict, nothing "
                 "retained\n", (int)frame_len);
          sc_enter(&state, SC_IDLE);
          return (int)frame_len;
        }

      printf("social_cue: captured %ld bytes, JPEG %dx%d\n",
             (long)frame_len, SC_WIDTH, SC_HEIGHT);
    }
  else
    {
      frame_len = 41984;
      printf("social_cue: mock frame, %ld bytes (camera not touched)\n",
             (long)frame_len);
    }

  if (g_cancelled)
    {
      sc_enter(&state, SC_CANCELLED);
      printf("social_cue: cancelled during capture; frame released, "
             "nothing reported\n");
      return OK;
    }

  sc_enter(&state, SC_ANALYZING);
  sc_print_request(frame_len);

  /* Where the HTTPS request goes.  Until then, a fixed response and a pause
   * long enough that the cancelled path is reachable by hand.
   */

  usleep(300 * 1000);

  if (g_cancelled)
    {
      sc_enter(&state, SC_CANCELLED);
      printf("social_cue: cancelled during analysis; result discarded\n");
      return OK;
    }

  sc_print_response(verdict);

  sc_enter(&state, SC_NOTIFYING);
  sc_notify(verdict);

  sc_enter(&state, SC_IDLE);
  printf("social_cue: frame released, no media or verdict retained\n");
  return OK;
}

/****************************************************************************
 * Name: sc_usage
 ****************************************************************************/

static void sc_usage(void)
{
  printf("Usage: social_cue [mock] [case <0..%u>] [n=<count>] "
         "[schema|install]\n", (unsigned int)SC_NMOCKS - 1);
  printf("  (no args)   one cycle: real capture, mock analysis\n");
  printf("  mock        skip the camera as well\n");
  printf("  case <n>    0 clear  1 weak  2 below floor  3 no face  "
         "4 conflicting\n");
  printf("  n=<count>   repeat the cycle\n");
  printf("  schema      print the JSON contract\n");
  printf("  install     write the skill document to %s\n", SC_SKILL_PATH);
  printf("Ctrl-C during capture or analysis takes the cancelled path.\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, char *argv[])
{
  struct sigaction act;
  const struct sc_verdict_s *verdict = &g_mock_cases[0];
  bool real_capture = true;
  int count = 1;
  int i;
  int ret = OK;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "mock") == 0)
        {
          real_capture = false;
        }
      else if (strcmp(argv[i], "case") == 0 && i + 1 < argc)
        {
          int n = atoi(argv[++i]);
          if (n < 0 || n >= (int)SC_NMOCKS)
            {
              printf("social_cue: case must be 0..%u\n",
                     (unsigned int)SC_NMOCKS - 1);
              return 1;
            }

          verdict = &g_mock_cases[n];
        }
      else if (strncmp(argv[i], "n=", 2) == 0)
        {
          count = atoi(argv[i] + 2);
          if (count < 1)
            {
              count = 1;
            }
        }
      else if (strcmp(argv[i], "schema") == 0)
        {
          printf("social-cue/v1:\n"
                 "{\n"
                 "  \"cues\": [ { \"cue\": \"brow_furrow\","
                 " \"meaning\": \"possible confusion\","
                 " \"confidence\": 0.72 } ],\n"
                 "  \"overall_confidence\": 0.72,\n"
                 "  \"unable_to_judge\": false,\n"
                 "  \"reason\": \"\",\n"
                 "  \"suggestion\": \"slow down and check\"\n"
                 "}\n"
                 "thresholds: >=%.2f cue+suggestion, >=%.2f cue only, "
                 "else unable\n"
                 "forced unable: no_face, face_too_small, conflicting_cues\n",
                 (double)SC_CONF_REPORT, (double)SC_CONF_WEAK);
          return 0;
        }
      else if (strcmp(argv[i], "install") == 0)
        {
          return sc_install_skill() == OK ? 0 : 1;
        }
      else
        {
          sc_usage();
          return 1;
        }
    }

  /* Ctrl-C has to reach this task for the cancelled path to be demonstrable.
   * NSH does not always claim the console for a builtin on this board (see
   * app/camera_preview's note), but installing a handler is what makes the
   * signal actionable rather than fatal.
   */

  memset(&act, 0, sizeof(act));
  act.sa_handler = sc_sigint;
  sigaction(SIGINT, &act, NULL);

  for (i = 0; i < count && ret == OK; i++)
    {
      if (count > 1)
        {
          printf("social_cue: --- cycle %d/%d ---\n", i + 1, count);
        }

      ret = sc_cycle(real_capture, verdict);
    }

  return ret == OK ? 0 : 1;
}
