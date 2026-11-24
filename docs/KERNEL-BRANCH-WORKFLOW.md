# Retrode3 kernel: Letux base, our hardware integration

Ownership policy updated 2026-11-24. Historical build and hardware
provenance remains in [RETRODE3-KERNEL-SOURCE.md](RETRODE3-KERNEL-SOURCE.md).

## One repository, one reviewed patch layer

`DragonBox-Shop/retrode3-kernel` is the full Linux source tree for the
Retrode3. It is **not** a second copy of the native OSCR source, and
the kernel module, board DTS/DTSI and kernel configuration should stay
in the same repository as the matching kernel version.

- Generic Linux base: `goldelico/letux-kernel`, tracked by its
  `letux-current` moving branch for notifications.
- Pinned starting base of the currently boot-tested 7.3 series:
  upstream `letux-7.3-rc5`, commit
  `f71a8243f157cac91094bc0f7106b7f51cfe5fd1`.
- Retrode3 changes: `drivers/misc/retrode3/`, the Kconfig/Makefile
  integration, `lx16+retrode3.dtsi`, `lx16-retrode-*.dts`, other
  necessary board-level DTB Makefile entries, and the approved
  Retrode3 kernel config. Keep changes to generic Letux files minimal.
- `CONFIG_MODVERSIONS`, full `.config`, `Module.symvers`, toolchain,
  module vermagic and kernel release must match for live module tests.

Do **not** split the cartridge driver or DTSI into a new Git tree.
Such a split would create more version coupling and make reproducible
kernel/DTB/.ko builds needlessly complicated.

## Branch roles

| Branch | Purpose | Automatic updates |
| --- | --- | --- |
| `upstream/letux-current` | Mirror of generic Nikolaus Linux | Nightly only |
| `develop` | Reviewed Retrode3 driver, DTS and config development | Never |
| `master` | Hardware-tested release source, immutable until approval | Never |
| `integration/*` | Temporary kernel-version upgrades/rebases for testing | Never |
| `feature/*` | Small isolated changes and their tests | Never |
| `archive/*` | Read-only preserved historical tips if needed | Never |

`upstream/letux-current` is a moving **reference**, not the input for
an uncontrolled build or automatic merge into `develop` or `master`.
Each image manifest must pin the exact selected Git commit SHA.
Do not rewrite published release history.

## Nightly synchronization

Only `.github/workflows/sync-letux-upstream.yml` should remain active.
It fetches the public `goldelico/letux-kernel:letux-current`, advances
only `upstream/letux-current`, and creates a review issue when the
upstream SHA changes. It never compiles upstream code or merges patches.
Schedule: 03:23 UTC daily, with a manual `workflow_dispatch` option.
Scheduled workflows run from GitHub's **default branch**, currently
`master`. Until the new workflow is merged into `master`, the workflow
policy in this feature PR is *not* the active nightly configuration.

The former `.github/workflows/sync-retrode3-upstream.yml` mirrored
the in-repository `project-retrode3-v2` branch. This is no longer
needed: ownership of the cartridge driver and board files has moved
to us. Retire that workflow, but preserve its history and do not
automatically delete its source or mirror branch.

## Kernel upgrade procedure

1. Review the mirror's reported upstream commit and select an exact
   Letux kernel release/tag or explicitly approved commit as base.
   A newer moving `letux-current` snapshot is **not** by itself
   a release decision.
2. Branch from `develop` as `integration/letux-VERSION`. Check the
   common ancestry and merge the **selected pinned upstream commit**
   (with a regular merge commit) into the integration branch.
   Avoid rebasing shared `master` and `develop` history.
3. Resolve conflicts in our driver, board DTS/DTSI and configuration.
   Keep each Retrode3-specific change reviewable and document any
   unavoidable modifications to generic Letux code.
4. Compile the complete kernel, DTB and modules using the pinned
   MIPS32EL toolchain and config. Verify `CONFIG_MODVERSIONS`,
   `modules.dep`, kernel release, module symbol CRCs and `modinfo`.
   For isolated driver-only development, build just the
   `M=drivers/misc/retrode3` module against prepared symbols.
5. Test a **separate SD** on the real Retrode3: boot, slots, LED
   triggers, USB, Wi-Fi (where firmware allows), and cartridge reads.
   Keep the installed kernel/module untouched until backed up.
6. Merge the reviewed integration branch into `develop`. Promote
   `develop` to `master` only after explicit hardware acceptance.
   The image/package release is a separate decision.

## Historical branches, safe cleanup

Do not delete old branches just because a similarly named mirror
exists. The current legacy candidates are `project-retrode3-v2`,
`project-retrode3-v5`, `project-retrode3`,
`project-retrode3-2.9.4`, `upstream/letux-retrode3` and
`letux-current`. Before removal inspect open PRs, unique commits,
and hardware provenance; preserve useful tips under `archive/*`
or through annotated tags. Repository history is not removed by
deleting a branch ref, but unreferenced commits may eventually
become unreachable. Avoid deleting any legacy refs in this cleanup PR.

Retire redundant jobs and document ownership first; delete old
branch refs in a **separately approved** maintenance change.
