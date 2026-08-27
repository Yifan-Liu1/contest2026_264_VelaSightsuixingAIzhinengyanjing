#!/bin/sh
############################################################################
# Install or verify the complete external source overlay.
#
# The external/ directory mirrors target repository paths.  This script is
# intentionally conservative: it validates the manifest and preflights every
# managed file before writing any of them.  It refuses to overwrite content
# that is neither the pinned baseline nor the already-installed overlay.
############################################################################

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
PROJECT_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd -P)
MANIFEST="$SCRIPT_DIR/manifest.tsv"
EXPECTED_MANAGED=55
MODE=check
OPENVELA_ROOT=${OPENVELA_ROOT:-}
BK_AVDK_ROOT=${BK_AVDK_ROOT:-}

usage()
{
  cat <<'EOF'
usage: external/prepare.sh [check|install] [options]

options:
  --openvela-root PATH  OpenVela repo workspace (contains apps/nuttx/packages)
  --bk-avdk-root PATH   Beken bk_avdk_smp checkout
  -h, --help            Show this help

check is read-only. install validates the complete 55-file overlay first, then
uses a same-directory temporary file and atomic rename for each changed path.
Unknown modifications abort before the first managed file is written.
EOF
}

fatal()
{
  echo "prepare.sh: $*" >&2
  exit 1
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    check|install)
      MODE=$1
      ;;
    --openvela-root)
      shift
      [ "$#" -gt 0 ] || { echo "prepare.sh: missing --openvela-root value" >&2; exit 2; }
      OPENVELA_ROOT=$1
      ;;
    --bk-avdk-root)
      shift
      [ "$#" -gt 0 ] || { echo "prepare.sh: missing --bk-avdk-root value" >&2; exit 2; }
      BK_AVDK_ROOT=$1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "prepare.sh: unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
  shift
done

[ -f "$MANIFEST" ] || fatal "missing $MANIFEST"

if [ -z "$OPENVELA_ROOT" ]; then
  OPENVELA_ROOT=$(CDPATH= cd -- "$PROJECT_ROOT/.." && pwd -P)
else
  [ -d "$OPENVELA_ROOT" ] || fatal "OpenVela root is not a directory: $OPENVELA_ROOT"
  OPENVELA_ROOT=$(CDPATH= cd -- "$OPENVELA_ROOT" && pwd -P)
fi

if [ -z "$BK_AVDK_ROOT" ]; then
  BK_AVDK_PARENT=$(CDPATH= cd -- "$OPENVELA_ROOT/.." && pwd -P)
  BK_AVDK_ROOT="$BK_AVDK_PARENT/bk_avdk_smp"
fi
[ -d "$BK_AVDK_ROOT" ] || fatal "Beken SDK root is not a directory: $BK_AVDK_ROOT"
BK_AVDK_ROOT=$(CDPATH= cd -- "$BK_AVDK_ROOT" && pwd -P)

for tool in git find sort cmp mktemp sha256sum realpath stat cp mv chmod \
  mkdir dirname sed grep awk wc rm; do
  command -v "$tool" >/dev/null 2>&1 || fatal "required tool not found: $tool"
done

# All rows have exactly eight tab-separated fields.  In addition to pinning the
# source revisions, each repository row locks the complete overlay path/content
# set by count and by a digest of sorted "relative-path<TAB>sha256" records.
if ! awk -F '\t' '
  function fail(message) {
    printf "prepare.sh: manifest:%d: %s\n", NR, message > "/dev/stderr"
    bad = 1
  }
  function is_hex(value, width) {
    return length(value) == width && value ~ /^[0-9a-f]+$/
  }
  /^[[:space:]]*$/ || /^#/ { next }
  {
    if (NF != 8) {
      fail("expected exactly 8 tab-separated fields")
      next
    }
    key = $1 SUBSEP $2
    if (seen[key]++)
      fail("duplicate " $1 " entry: " $2)

    if ($1 == "repo") {
      repo_count++
      if (!is_hex($5, 40))
        fail("repo baseline must be a 40-character lowercase hex commit")
      if ($6 == "")
        fail("repo source URL is empty")
      if ($7 !~ /^[1-9][0-9]*$/)
        fail("repo file count must be a positive integer")
      if (!is_hex($8, 64))
        fail("repo overlay digest must be 64-character lowercase hex")

      if ($2 == "apps") {
        if ($3 != "apps" || $4 != "apps" || $7 != "1")
          fail("apps mapping/count must be apps -> apps with 1 file")
      } else if ($2 == "nuttx") {
        if ($3 != "nuttx" || $4 != "nuttx" || $7 != "1")
          fail("nuttx mapping/count must be nuttx -> nuttx with 1 file")
      } else if ($2 == "packages_ai_agent") {
        if ($3 != "packages/ai_agent" || $4 != "packages/ai_agent" || $7 != "14")
          fail("packages_ai_agent mapping/count must be packages/ai_agent with 14 files")
      } else if ($2 == "bk_avdk_smp") {
        if ($3 != "bk_avdk_smp" || $4 != "bk_avdk_smp" || $7 != "39")
          fail("bk_avdk_smp mapping/count must contain 39 files")
      } else {
        fail("unknown repository: " $2)
      }
    } else if ($1 == "image") {
      image_count++
      if ($2 != "armino_idk" || $3 != "-" || $4 == "" || !is_hex($5, 64) || $6 == "" || $7 != "-" || $8 != "-")
        fail("invalid armino_idk image row")
    } else if ($1 == "archive") {
      archive_count++
      if ($2 != "armino_idk_v1.5" || $3 != "-" || $4 == "" || !is_hex($5, 64) || $6 == "" || $7 != "-" || $8 != "-")
        fail("invalid armino_idk_v1.5 archive row")
    } else {
      fail("unknown entry kind: " $1)
    }
  }
  END {
    if (repo_count != 4) {
      printf "prepare.sh: manifest: expected 4 repo rows, found %d\n", repo_count > "/dev/stderr"
      bad = 1
    }
    if (image_count != 1) {
      printf "prepare.sh: manifest: expected 1 image row, found %d\n", image_count > "/dev/stderr"
      bad = 1
    }
    if (archive_count != 1) {
      printf "prepare.sh: manifest: expected 1 archive row, found %d\n", archive_count > "/dev/stderr"
      bad = 1
    }
    exit bad
  }
' "$MANIFEST"; then
  exit 1
fi

WORK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/velasight-overlay.XXXXXX")
PLAN="$WORK_DIR/plan"
STAGED_PLAN="$WORK_DIR/staged-plan"
TARGET_SNAPSHOT="$WORK_DIR/target-snapshot"
FILES="$WORK_DIR/files"
DISCOVERED="$WORK_DIR/discovered"
UNSAFE="$WORK_DIR/unsafe"
INDEX="$WORK_DIR/index"
BASELINE_BLOB="$WORK_DIR/baseline-blob"
NORM_LEFT="$WORK_DIR/normalized-left"
NORM_RIGHT="$WORK_DIR/normalized-right"
GENERATED_HEAD="$WORK_DIR/generated-head"
STAGE_DIR="$WORK_DIR/stage"
ACTIVE_TMP=
mkdir -p -- "$STAGE_DIR"
: > "$PLAN"
: > "$STAGED_PLAN"
: > "$TARGET_SNAPSHOT"

cleanup()
{
  if [ -n "$ACTIVE_TMP" ]; then
    rm -f -- "$ACTIVE_TMP"
  fi
  if [ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ]; then
    rm -rf -- "$WORK_DIR"
  fi
}

on_signal()
{
  signal_status=$1
  trap - 0 HUP INT TERM
  cleanup
  exit "$signal_status"
}

trap cleanup 0
trap 'on_signal 129' HUP
trap 'on_signal 130' INT
trap 'on_signal 143' TERM

errors=0
managed=0
manifest_managed=0
repos_seen=0
ready=0
newline_equivalent=0
planned=0
TAB=$(printf '\t')
CR=$(printf '\r')

error()
{
  echo "prepare.sh: error: $*" >&2
  errors=$((errors + 1))
}

warn()
{
  echo "prepare.sh: warning: $*" >&2
}

file_sha256()
{
  sha_line=$(sha256sum -- "$1") || return 1
  printf '%s\n' "${sha_line%% *}"
}

# Only auto_partitions.csv has a known upstream mixed-CRLF/LF representation.
# Remove CR immediately before LF for this one logical comparison; every other
# managed file, including ram_regions.csv, remains byte-for-byte strict.
files_equal()
{
  fe_repo=$1
  fe_rel=$2
  fe_left=$3
  fe_right=$4

  [ -f "$fe_left" ] && [ -f "$fe_right" ] || return 1
  [ ! -L "$fe_left" ] && [ ! -L "$fe_right" ] || return 1
  cmp -s -- "$fe_left" "$fe_right" && return 0

  if [ "$fe_repo" = bk_avdk_smp ] \
    && [ "$fe_rel" = projects/app_ab/partitions/bk7258/auto_partitions.csv ]; then
    sed "s/${CR}\$//" < "$fe_left" > "$NORM_LEFT" || return 1
    sed "s/${CR}\$//" < "$fe_right" > "$NORM_RIGHT" || return 1
    cmp -s -- "$NORM_LEFT" "$NORM_RIGHT"
    return
  fi

  return 1
}

# Reject every symlink path component, non-directory intermediate component and
# canonical escape before examining or replacing a destination.
destination_path_safe()
{
  ds_target=$1
  ds_rel=$2
  ds_cursor=$ds_target
  ds_remaining=$ds_rel

  while :; do
    ds_component=${ds_remaining%%/*}
    ds_cursor="$ds_cursor/$ds_component"
    [ ! -L "$ds_cursor" ] || return 1

    case "$ds_remaining" in
      */*)
        if [ -e "$ds_cursor" ] && [ ! -d "$ds_cursor" ]; then
          return 1
        fi
        ds_remaining=${ds_remaining#*/}
        ;;
      *)
        break
        ;;
    esac
  done

  ds_resolved=$(realpath -m -- "$ds_cursor") || return 1
  case "$ds_resolved" in
    "$ds_target"/*) return 0 ;;
    *) return 1 ;;
  esac
}

is_generated_cp_config()
{
  ig_file=$1
  [ -f "$ig_file" ] && [ ! -L "$ig_file" ] || return 1
  ig_lines=$(wc -l < "$ig_file") || return 1
  [ "$ig_lines" -ge 100 ] || return 1
  sed -n '1,10p' "$ig_file" > "$GENERATED_HEAD" || return 1
  grep -qx '# Automatically generated file\. DO NOT EDIT\.' "$GENERATED_HEAD"
}

target_state_matches()
{
  ts_destination=$1
  ts_kind=$2
  ts_expected_sha=$3

  case "$ts_kind" in
    absent)
      [ ! -e "$ts_destination" ] && [ ! -L "$ts_destination" ]
      ;;
    file)
      [ -f "$ts_destination" ] && [ ! -L "$ts_destination" ] || return 1
      ts_actual_sha=$(file_sha256 "$ts_destination") || return 1
      [ "$ts_actual_sha" = "$ts_expected_sha" ]
      ;;
    *)
      return 1
      ;;
  esac
}

# Phase 1: inspect every repository and every overlay file. Nothing is written
# to any target repository until this entire phase and staging both succeed.
while IFS="$TAB" read -r kind name overlay_path default_target baseline source expected_count expected_tree \
  || [ -n "${kind:-}" ]; do
  case "$kind" in
    ''|'#'*) continue ;;
    repo) ;;
    *) continue ;;
  esac

  repos_seen=$((repos_seen + 1))
  manifest_managed=$((manifest_managed + expected_count))

  case "$name" in
    apps)
      expected_overlay=apps
      expected_target=apps
      target="$OPENVELA_ROOT/apps"
      ;;
    nuttx)
      expected_overlay=nuttx
      expected_target=nuttx
      target="$OPENVELA_ROOT/nuttx"
      ;;
    packages_ai_agent)
      expected_overlay=packages/ai_agent
      expected_target=packages/ai_agent
      target="$OPENVELA_ROOT/packages/ai_agent"
      ;;
    bk_avdk_smp)
      expected_overlay=bk_avdk_smp
      expected_target=bk_avdk_smp
      target="$BK_AVDK_ROOT"
      ;;
    *)
      error "unknown repository mapping: $name"
      continue
      ;;
  esac

  if [ "$overlay_path" != "$expected_overlay" ] || [ "$default_target" != "$expected_target" ]; then
    error "$name manifest mapping is not allowed: $overlay_path -> $default_target"
    continue
  fi

  overlay_root="$SCRIPT_DIR/$overlay_path"
  if [ ! -d "$overlay_root" ] || [ -L "$overlay_root" ]; then
    error "$name overlay directory is missing or is a symlink: $overlay_root"
    continue
  fi

  if ! target_top=$(git -C "$target" rev-parse --show-toplevel 2>/dev/null); then
    error "$name target is not a Git checkout: $target"
    continue
  fi
  target_top=$(CDPATH= cd -- "$target_top" && pwd -P)
  target=$(CDPATH= cd -- "$target" && pwd -P)
  if [ "$target_top" != "$target" ]; then
    error "$name target must be its repository root: $target (root is $target_top)"
    continue
  fi

  if ! git -C "$target" cat-file -e "$baseline^{commit}" 2>/dev/null; then
    error "$name baseline commit is unavailable: $baseline; fetch $source"
    continue
  fi

  if ! head=$(git -C "$target" rev-parse HEAD 2>/dev/null); then
    error "$name cannot resolve target HEAD: $target"
    continue
  fi
  baseline_compatible=no
  if [ "$head" = "$baseline" ]; then
    baseline_compatible=yes
  elif git -C "$target" merge-base --is-ancestor "$baseline" "$head" 2>/dev/null; then
    baseline_compatible=yes
    warn "$name HEAD $head is a descendant of pinned baseline $baseline; managed paths are still checked byte-for-byte"
  else
    error "$name HEAD $head is not the pinned baseline $baseline or its descendant; refusing a non-reproducible repository lineage"
  fi

  : > "$UNSAFE"
  if ! find "$overlay_root" ! -type d ! -type f -print > "$UNSAFE"; then
    error "$name overlay traversal failed: $overlay_root"
    continue
  fi
  if [ -s "$UNSAFE" ]; then
    while IFS= read -r unsafe_path; do
      error "$name overlay contains a symlink or special file: ${unsafe_path#"$overlay_root"/}"
    done < "$UNSAFE"
    continue
  fi

  if ! find "$overlay_root" -type f -print > "$DISCOVERED"; then
    error "$name overlay traversal failed: $overlay_root"
    continue
  fi
  if ! LC_ALL=C sort "$DISCOVERED" > "$FILES"; then
    error "$name overlay path sorting failed: $overlay_root"
    continue
  fi
  if [ ! -s "$FILES" ]; then
    error "$name overlay contains no files: $overlay_root"
    continue
  fi

  : > "$INDEX"
  repo_files=0
  while IFS= read -r overlay_file; do
    managed=$((managed + 1))
    repo_files=$((repo_files + 1))
    rel=${overlay_file#"$overlay_root"/}

    case "$rel" in
      ''|/*|../*|*/../*|*/..)
        error "$name has unsafe overlay path: $rel"
        continue
        ;;
    esac

    if [ ! -f "$overlay_file" ] || [ -L "$overlay_file" ]; then
      error "$name overlay entry is not a regular file: $rel"
      continue
    fi

    if ! overlay_sha=$(file_sha256 "$overlay_file"); then
      error "$name cannot hash overlay file: $rel"
      continue
    fi
    printf '%s\t%s\n' "$rel" "$overlay_sha" >> "$INDEX"

    destination="$target/$rel"
    if ! destination_path_safe "$target" "$rel"; then
      error "$name destination is unsafe or escapes target root: $rel"
      continue
    fi

    if files_equal "$name" "$rel" "$overlay_file" "$destination"; then
      ready=$((ready + 1))
      if ! cmp -s -- "$overlay_file" "$destination"; then
        newline_equivalent=$((newline_equivalent + 1))
      fi
      if [ "$MODE" = install ]; then
        if ! ready_sha=$(file_sha256 "$destination"); then
          error "$name cannot snapshot ready target: $rel"
          continue
        fi
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
          "$name" "$overlay_file" "$overlay_sha" "$target" "$rel" \
          "$destination" file "$ready_sha" >> "$TARGET_SNAPSHOT"
      fi
      continue
    fi

    if [ "$MODE" = check ]; then
      if [ -e "$destination" ] || [ -L "$destination" ]; then
        error "$name overlay mismatch: $rel"
      else
        error "$name overlay missing from target: $rel"
      fi
      continue
    fi

    can_install=no
    reason=
    if [ "$baseline_compatible" = yes ]; then
      if git -C "$target" cat-file -e "$baseline:$rel" 2>/dev/null; then
        if git -C "$target" cat-file blob "$baseline:$rel" > "$BASELINE_BLOB" 2>/dev/null; then
          if [ -f "$destination" ] && [ ! -L "$destination" ] \
            && cmp -s -- "$BASELINE_BLOB" "$destination"; then
            can_install=yes
            reason=baseline
          fi
        else
          error "$name baseline object is not a readable blob: $rel"
          continue
        fi
      elif [ ! -e "$destination" ] && [ ! -L "$destination" ]; then
        can_install=yes
        reason=new
      fi
    fi

    # Armino expands this checked-in minimal fragment in place. Accept that
    # generated state only on a baseline-compatible checkout, with the marker
    # near the beginning and the substantial generated-file shape.
    if [ "$baseline_compatible" = yes ] \
      && [ "$name" = bk_avdk_smp ] \
      && [ "$rel" = projects/app_ab/cp/config/bk7258/config ] \
      && is_generated_cp_config "$destination"; then
      can_install=yes
      reason=generated-config
    fi

    if [ "$can_install" = yes ]; then
      if ! file_mode=$(stat -c '%a' -- "$overlay_file"); then
        error "$name cannot read overlay mode: $rel"
        continue
      fi

      case "$reason" in
        new)
          expected_kind=absent
          expected_sha=-
          ;;
        baseline|generated-config)
          expected_kind=file
          if ! expected_sha=$(file_sha256 "$destination"); then
            error "$name cannot hash target before install: $rel"
            continue
          fi
          ;;
        *)
          error "$name internal install reason error: $rel"
          continue
          ;;
      esac

      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$overlay_file" "$target" "$rel" "$destination" "$file_mode" \
        "$reason" "$expected_kind" "$expected_sha" "$overlay_sha" >> "$PLAN"
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$name" "$overlay_file" "$overlay_sha" "$target" "$rel" \
        "$destination" "$expected_kind" "$expected_sha" >> "$TARGET_SNAPSHOT"
      planned=$((planned + 1))
    else
      error "$name refuses to overwrite unknown modification: $rel"
    fi
  done < "$FILES"

  if ! actual_tree=$(file_sha256 "$INDEX"); then
    error "$name cannot hash overlay index"
  elif [ "$repo_files" -ne "$expected_count" ]; then
    error "$name overlay file count changed: $repo_files != $expected_count"
  elif [ "$actual_tree" != "$expected_tree" ]; then
    error "$name overlay tree digest changed: $actual_tree != $expected_tree"
  fi
done < "$MANIFEST"

if [ "$repos_seen" -ne 4 ]; then
  error "processed repository count changed: $repos_seen != 4"
fi
if [ "$manifest_managed" -ne "$EXPECTED_MANAGED" ]; then
  error "manifest managed-file count changed: $manifest_managed != $EXPECTED_MANAGED"
fi
if [ "$managed" -ne "$EXPECTED_MANAGED" ]; then
  error "overlay managed-file count changed: $managed != $EXPECTED_MANAGED"
fi
if [ "$MODE" = install ]; then
  snapshot_count=$(wc -l < "$TARGET_SNAPSHOT")
  if [ "$snapshot_count" -ne "$EXPECTED_MANAGED" ]; then
    error "install target snapshot is incomplete: $snapshot_count != $EXPECTED_MANAGED"
  fi
fi

if [ "$errors" -ne 0 ]; then
  echo "prepare.sh: $errors error(s); no managed files were written" >&2
  exit 1
fi

if [ "$MODE" = check ]; then
  echo "prepare.sh: overlay verified ($managed files, $newline_equivalent auto_partitions newline-equivalent)"
  exit 0
fi

# Stage every pending source and verify it still has the hash observed during
# the full preflight. A source mutation or copy failure therefore occurs before
# any target file is replaced.
staged_index=0
while IFS="$TAB" read -r name overlay_file target rel destination file_mode reason expected_kind expected_sha overlay_sha; do
  staged_index=$((staged_index + 1))
  stage_file="$STAGE_DIR/$staged_index"

  if ! current_overlay_sha=$(file_sha256 "$overlay_file"); then
    error "$name cannot re-hash overlay before staging: $rel"
    continue
  fi
  if [ "$current_overlay_sha" != "$overlay_sha" ]; then
    error "$name overlay changed during preflight: $rel"
    continue
  fi
  if ! cp -- "$overlay_file" "$stage_file" || ! chmod "$file_mode" "$stage_file"; then
    error "$name cannot stage overlay file: $rel"
    continue
  fi
  if ! staged_sha=$(file_sha256 "$stage_file") || [ "$staged_sha" != "$overlay_sha" ]; then
    error "$name staged copy verification failed: $rel"
    continue
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$name" "$stage_file" "$target" "$rel" "$destination" "$file_mode" \
    "$reason" "$expected_kind" "$expected_sha" >> "$STAGED_PLAN"
done < "$PLAN"

if [ "$errors" -ne 0 ]; then
  echo "prepare.sh: $errors staging error(s); no managed files were written" >&2
  exit 1
fi

# Revalidate all 55 overlay sources and destinations after staging and before
# the first replacement, including files that were already current.
while IFS="$TAB" read -r name overlay_file overlay_sha target rel destination expected_kind expected_sha; do
  if ! current_overlay_sha=$(file_sha256 "$overlay_file"); then
    error "$name cannot re-hash overlay during revalidation: $rel"
  elif [ "$current_overlay_sha" != "$overlay_sha" ]; then
    error "$name overlay changed during preflight: $rel"
  elif ! destination_path_safe "$target" "$rel"; then
    error "$name destination became unsafe during preflight: $rel"
  elif ! target_state_matches "$destination" "$expected_kind" "$expected_sha"; then
    error "$name target changed during preflight: $rel"
  fi
done < "$TARGET_SNAPSHOT"

if [ "$errors" -ne 0 ]; then
  echo "prepare.sh: $errors revalidation error(s); no managed files were written" >&2
  exit 1
fi

# Phase 2: each replacement uses a same-directory temporary file and rename.
# The operation is deliberately per-file atomic; interruption can leave a
# validated prefix installed, and rerunning install safely completes it.
while IFS="$TAB" read -r name stage_file target rel destination file_mode reason expected_kind expected_sha; do
  destination_path_safe "$target" "$rel" \
    || fatal "$name destination became unsafe before replacement: $rel"
  target_state_matches "$destination" "$expected_kind" "$expected_sha" \
    || fatal "$name target changed before replacement: $rel"

  parent=$(dirname -- "$destination")
  mkdir -p -- "$parent"
  destination_path_safe "$target" "$rel" \
    || fatal "$name destination became unsafe after creating its parent: $rel"

  ACTIVE_TMP=$(mktemp "$parent/.velasight-overlay.XXXXXX")
  cp -- "$stage_file" "$ACTIVE_TMP"
  chmod "$file_mode" "$ACTIVE_TMP"
  cmp -s -- "$stage_file" "$ACTIVE_TMP" \
    || fatal "$name temporary copy verification failed: $rel"
  mv -f -- "$ACTIVE_TMP" "$destination"
  ACTIVE_TMP=
  echo "prepare.sh: installed $name:$rel ($reason)"
done < "$STAGED_PLAN"

# Re-run the read-only verification after installation.
"$0" check --openvela-root "$OPENVELA_ROOT" --bk-avdk-root "$BK_AVDK_ROOT"
echo "prepare.sh: install complete ($planned changed, $ready already current)"
