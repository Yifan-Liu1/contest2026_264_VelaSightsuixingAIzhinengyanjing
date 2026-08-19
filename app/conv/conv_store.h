/****************************************************************************
 * app/conv/conv_store.h
 *
 * Conversation records on the SD-NAND, and the queries the web console runs
 * against them.
 *
 * Three properties of the filesystem shape everything here, and all three
 * were measured on the board rather than assumed:
 *
 *   - CONFIG_FAT_LFN is off, so names are 8.3.  A timestamp does not fit in
 *     eight characters, which rules out the obvious design of naming each
 *     record after its date and letting readdir() answer date queries.
 *
 *   - CONFIG_FS_FATTIME is off, so the filesystem does not record
 *     modification times either.  The second obvious design -- stat() each
 *     file and filter on mtime -- is out for the same reason.
 *
 *   - time(NULL) returns seconds since boot, not a wall clock: the first
 *     session this code's sibling uploaded was numbered 249.  The board
 *     cannot timestamp anything correctly on its own.
 *
 * So the date lives in an index file, as data, and it has to be supplied
 * from outside.  The index is the only file a query reads: one sequential
 * pass answers date-range, cue and confidence filters without opening a
 * single record.  Keyword search then opens only the transcripts of the
 * records that survived that pass, which is the difference between reading
 * a few hundred bytes and reading every conversation on the card.
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

#ifndef __APP_CONV_CONV_STORE_H
#define __APP_CONV_CONV_STORE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Where records live.
 *
 * Alongside social_cue's SC_DATA_DIR rather than at the root of the card:
 * that app already owns /mnt/sdnand/ai_agent for its skills, and the
 * analysis stored with each conversation here is the same social-cue/v1
 * payload it produces.
 *
 * Both path components are within 8.3, which is not a coincidence -- a
 * longer name would be silently mangled to CONVER~1 and every path built
 * from the constant would then miss.
 */

#define CONV_DIR          "/mnt/sdnand/ai_agent"
#define CONV_SUBDIR       CONV_DIR "/conv"
#define CONV_INDEX        CONV_SUBDIR "/INDEX.TXT"

/* Per-record files, formatted with the sequence number.
 *
 * There is no audio file.  A conversation record is the transcript and the
 * expression analysis; the recording itself is not kept.  That is a decision
 * about what this feature is for rather than a limitation: the text is what a
 * person searches and reads back, it is two orders of magnitude smaller, and
 * keeping audio of other people's conversations on a device that is carried
 * around is a liability nobody asked for.  Live audio still goes to the cloud
 * for recognition -- see audio_test's chunked upload -- it is only the
 * on-board history that is text.
 */

#define CONV_FMT_TEXT     CONV_SUBDIR "/C%04u.TXT"
#define CONV_FMT_CUE      CONV_SUBDIR "/C%04u.JSN"

/* Field widths.  Kept small deliberately: a query holds one of these on the
 * stack while it scans, and this runs on a task whose stack is measured in
 * kilobytes.
 */

#define CONV_CUE_MAX      24
#define CONV_SUMMARY_MAX  96
#define CONV_LINE_MAX     192

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* One line of the index.
 *
 * The transcript and the analysis are not in here -- they are in the
 * per-record files.  This struct is what a query can decide on without
 * opening anything else.
 */

struct conv_entry_s
{
  unsigned int seq;
  uint32_t epoch;                       /* seconds since 1970, from outside */
  unsigned int duration_ms;
  char cue[CONV_CUE_MAX];               /* social-cue/v1 `cue`, e.g. brow_furrow */
  float confidence;                     /* social-cue/v1 `overall_confidence` */
  bool unable_to_judge;                 /* social-cue/v1 flag, carried through */
  size_t text_bytes;                    /* transcript size, so a listing can
                                         * show how much there is to read
                                         * without opening it */
  char summary[CONV_SUMMARY_MAX];
};

/* What to match.  A zeroed filter matches everything. */

struct conv_filter_s
{
  uint32_t from_epoch;                  /* 0: no lower bound */
  uint32_t to_epoch;                    /* 0: no upper bound */
  const char *cue;                      /* NULL: any cue */
  const char *keyword;                  /* NULL: no transcript search */
  float min_confidence;                 /* 0: any */
  bool include_unjudged;                /* false: skip unable_to_judge records */
};

/* Called once per matching record, in index order. */

typedef int (*conv_visit_t)(const struct conv_entry_s *entry, void *arg);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: conv_store_ready
 *
 * Description:
 *   Create the record directory if it is missing and report whether the
 *   store is usable.
 *
 *   Checked rather than assumed because the mount is not synchronous with
 *   boot: bring-up defers SD-NAND initialisation, so a command run early
 *   can find /mnt/sdnand absent and the failure would otherwise look like a
 *   missing record rather than a filesystem that is not up yet.
 *
 * Returned Value:
 *   Zero on success, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_store_ready(void);

/****************************************************************************
 * Name: conv_store_append
 *
 * Description:
 *   Write one record: transcript, analysis, audio, and the index line that
 *   makes it findable.
 *
 *   The index line is written last on purpose.  A record whose files exist
 *   but whose index line does not is invisible and harmless; an index line
 *   pointing at files that were never finished is a query that fails.  If
 *   power is lost mid-write, the cheaper failure is the one that happens.
 *
 * Input Parameters:
 *   entry      - index fields; entry->seq and entry->text_bytes are filled
 *                in by this call
 *   transcript - conversation text, NUL-terminated
 *   cue_json   - social-cue/v1 analysis, NUL-terminated
 *
 * Returned Value:
 *   Zero on success, a negated errno otherwise.
 *
 ****************************************************************************/

int conv_store_append(struct conv_entry_s *entry, const char *transcript,
                      const char *cue_json);

/****************************************************************************
 * Name: conv_store_query
 *
 * Description:
 *   Visit every record matching filter, in index order.
 *
 *   Two stages, cheap first: the index alone decides date, cue and
 *   confidence, and only survivors have their transcript opened for the
 *   keyword.  With no keyword no record file is opened at all.
 *
 * Returned Value:
 *   Number of matches, or a negated errno.  Stops early and returns the
 *   visitor's error if it returns non-zero.
 *
 ****************************************************************************/

int conv_store_query(const struct conv_filter_s *filter, conv_visit_t visit,
                     void *arg);

/****************************************************************************
 * Name: conv_store_next_seq
 *
 * Description:
 *   Sequence number a new record would get.
 *
 ****************************************************************************/

unsigned int conv_store_next_seq(void);

/****************************************************************************
 * Name: conv_store_clear
 *
 * Description:
 *   Delete every record and the index.
 *
 *   Walks the index rather than the directory: readdir would also find
 *   whatever else is in there, and a "clear the history" command has no
 *   business deleting files it did not write.  A record whose index line is
 *   missing therefore survives, which is the safe direction to fail.
 *
 * Returned Value:
 *   Number of records removed, or a negated errno.
 *
 ****************************************************************************/

int conv_store_clear(void);

/****************************************************************************
 * Name: conv_store_read_file
 *
 * Description:
 *   Read one of a record's files into a caller buffer, NUL-terminating it.
 *
 * Input Parameters:
 *   fmt  - one of CONV_FMT_TEXT / CONV_FMT_CUE
 *   seq  - record sequence number
 *   buf  - destination
 *   len  - bytes available at buf, including the terminator
 *
 * Returned Value:
 *   Bytes read, or a negated errno.
 *
 ****************************************************************************/

int conv_store_read_file(const char *fmt, unsigned int seq, char *buf,
                         size_t len);

/****************************************************************************
 * Name: conv_store_read_raw
 *
 * Description:
 *   Read a whole file at a literal path into a caller buffer, NUL-terminating
 *   it.  Truncates silently at len - 1, which is why callers size the buffer
 *   from what the file is allowed to contain rather than from what they hope
 *   it does.
 *
 * Returned Value:
 *   Bytes read, or a negated errno.
 *
 ****************************************************************************/

int conv_store_read_raw(const char *path, char *buf, size_t len);

/****************************************************************************
 * Name: conv_epoch_to_date / conv_date_to_epoch
 *
 * Description:
 *   Convert between an epoch and a YYYYMMDD integer, using gmtime/timegm
 *   arithmetic that does not depend on the board knowing its own time.
 *
 *   Dates are handled as plain integers rather than strings because that is
 *   what a range comparison needs, and because the command line and the
 *   index both carry them that way.
 *
 ****************************************************************************/

unsigned int conv_epoch_to_date(uint32_t epoch);
uint32_t conv_date_to_epoch(unsigned int yyyymmdd, bool end_of_day);

#endif /* __APP_CONV_CONV_STORE_H */
