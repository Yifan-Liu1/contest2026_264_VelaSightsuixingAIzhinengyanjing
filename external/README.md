# External source overlay

`external/` is a single source overlay for repositories that are not committed through the team repository. Its four target directories are deliberately flat and mirror destination repository paths exactly:

```text
external/
├── apps/                 -> <openvela>/apps/
├── nuttx/                -> <openvela>/nuttx/
├── packages/ai_agent/    -> <openvela>/packages/ai_agent/
└── bk_avdk_smp/          -> <workspace>/bk_avdk_smp/
```

There are no patch directories inside this tree. Every managed path is the complete desired target file. Root-level `manifest.tsv` pins the public source baseline and build image; `prepare.sh` is the only installation and verification entry point.

## Prepare a clean workspace

Initialize and sync OpenVela as described in `docs/github开发指南.md`, then obtain the Beken SDK at the pinned public baseline:

```bash
git clone https://github.com/bekencorp/bk_avdk_smp.git ../bk_avdk_smp
git -C ../bk_avdk_smp checkout d2ded037798530175e5dc5cde6fa1878f5d5ef35
```

From the team repository, run a full preflight and install all four overlays with per-file atomic replacement:

```bash
./external/prepare.sh install
./external/prepare.sh check
```

For a nonstandard layout, provide explicit roots:

```bash
./external/prepare.sh install \
  --openvela-root /path/to/openvela \
  --bk-avdk-root /path/to/bk_avdk_smp
```

The installer is intentionally conservative:

- It strictly validates the four repository mappings, manifest schema and pinned image/archive identifiers; `build_and_flash.sh` checks the local image ID, while an offline archive is verified with the documented `sha256sum` command.
- Each target HEAD must be the pinned commit or its descendant; an unrelated lineage is rejected even when all managed files happen to match.
- The expected 1 + 1 + 14 + 39 files are locked by per-repository count and a digest of every relative path and file hash.
- It rejects overlay symlinks, special files, target symlink components and paths that escape a target repository.
- It validates every repository and managed file, stages changed sources, then revalidates every destination before writing anything.
- A target may already equal the overlay, equal its pinned baseline blob, or be a new file absent from that baseline.
- A descendant commit is accepted only when each managed target still equals the pinned blob or the final overlay.
- Unknown local modifications abort the whole install before the first write.
- Unrelated untracked or modified files are preserved.
- Only `projects/app_ab/partitions/bk7258/auto_partitions.csv` permits CRLF/LF-only equivalence; all other files are byte-exact.
- The one known generated source mutation, `projects/app_ab/cp/config/bk7258/config`, may be restored only on a baseline-compatible checkout when it has Armino's generated-file marker and shape.

`check` is always read-only. Each changed file is installed through a temporary file in its destination directory followed by atomic rename. The 55 replacements cannot form one filesystem transaction across four repositories: if the process is interrupted during replacement, rerun `install` to safely complete the already-preflighted prefix. `install` never runs `git reset`, `checkout`, `restore`, `clean` or `stash`.

## Pinned inputs

`manifest.tsv` is authoritative. The current tested baselines are:

| Repository | Commit |
| --- | --- |
| `apps` | `dcc6a95c3b323e533c98fde8fb209f99e24f0fdd` |
| `nuttx` | `4a67a7bad6bba672db2ac37a64acbdc3c024d6e5` |
| `packages/ai_agent` | `31faed70f683a6f5e690437c5507891360f0814a` |
| `bk_avdk_smp` | `d2ded037798530175e5dc5cde6fa1878f5d5ef35` |

The complete Armino overlay includes both partition inputs:

```text
external/bk_avdk_smp/projects/app_ab/partitions/bk7258/auto_partitions.csv
external/bk_avdk_smp/projects/app_ab/partitions/bk7258/ram_regions.csv
```

`auto_partitions.csv` fixes `primary_ap_app` at `4148k`. The overlay also includes `tools/build_tools/build_process/bk_sdk/bk_sdk_project.py`, which implements the `EXTERNAL_AP_BIN` packaging path.

Do not add expanded/generated inputs such as:

```text
bk_avdk_smp/build/**
bk_avdk_smp/projects/app_ab/build/**
bk_avdk_smp/**/__pycache__/**
bk_avdk_smp/projects/app_ab/ap/config/bk7258_ap/config
```

The checked-in CP config is the minimal seed, not the expanded Kconfig output.

## Armino 1.5 image

The final CP build uses `localhost/bekencorp/armino-idk:1.5`. Official sources documented by Beken are:

- [Docker Hub tags](https://hub.docker.com/r/bekencorp/armino-idk/tags)
- [Beken image downloads](https://dl.bekencorp.com/tools/arminosdk/docker_img/armino-idk)

Pull and assign the local tag with Podman:

```bash
podman pull docker.io/bekencorp/armino-idk:1.5
podman tag docker.io/bekencorp/armino-idk:1.5 \
  localhost/bekencorp/armino-idk:1.5
```

Or load Beken's archive:

```bash
sha256sum bekencorp-armino-idk-v1.5.tar.gz
podman load -i bekencorp-armino-idk-v1.5.tar.gz
```

The tested archive SHA-256 is `d79a8ec193dafdb27af881a847fd6d3083e5804808c49b890956cbd9dae10a79`; the tested image ID is `5617808dab7ae97336fbd58beb37387bb563e0aeef50234b7beca7e8fce01d2d`. The build refuses a different image unless the pinned metadata is intentionally updated and revalidated.

## Build

Verify or explicitly install the overlay before building:

```bash
./build_and_flash.sh --prepare-overlay
```

Without `--prepare-overlay`, the build performs a read-only overlay check and stops if preparation is incomplete. It then builds OpenVela AP, injects it through `EXTERNAL_AP_BIN`, builds Armino CP and packages `all-app.bin`. The build restores the minimal CP config even when the Armino command fails.

## Updating an external file

1. Start from the pinned target repository commit.
2. Apply and test the source change in its real repository.
3. Copy the complete final file to the identical relative path under the corresponding `external/<repository>/` directory.
4. Recompute that repository row's `file_count` and `overlay_tree_sha256`; the digest is SHA-256 over sorted `relative-path<TAB>file-sha256` records, one LF-terminated record per file.
5. Run `./external/prepare.sh check` against the prepared workspace.
6. If rebasing a target repository or image intentionally, update `manifest.tsv` and this document in the same change.

Public-stack fixes should still be submitted upstream. This overlay exists to make the exact product build reproducible until those fixes are merged.
