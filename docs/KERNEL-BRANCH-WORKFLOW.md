# Kernel branches, upstream tracking and Retrode3 hardware integration

Stand: 2026-11-22. Build and boot documentation:
[RETRODE3-KERNEL-SOURCE.md](RETRODE3-KERNEL-SOURCE.md).

## Branch roles

| Branch | Ownership / purpose | Automatic changes |
| --- | --- | --- |
| `project-retrode3-v2` | Letux Retrode3 project source branch in this repository; **keep intact** | Only the CI monitoring files installed on the current default branch |
| `upstream/letux-retrode3` | Read-only-by-policy mirror of the complete `project-retrode3-v2` history | Daily GitHub Actions update; **no merge** |
| `upstream/letux-current` | Mirror of `goldelico/letux-kernel:letux-current` | Existing daily GitHub Actions update; **no merge** |
| `develop` | Retrode3 integration, driver and DTS/DTSI work for current kernel generation | Changes reviewed or intentionally authored; no automatic upstream imports |
| `master` | Hardware-tested kernel source baseline used for releases | Updated only after hardware validation, not by mirror automation |
| `feature/*`, `fix/*`, `integration/*` | Short-lived experimental changes, PRs target `develop` | No |
| `archive/*` | Preserved former major-version branches or histories | No |

Old master (7.2) and old develop (7.1) must be preserved in
archive branches before source-history migrations. Never delete a branch
that still contains unmerged work. GitHub's default branch should be
`master` after the 7.3 migration, rather than Letux' project
branch. **Scheduled GitHub Actions execute from the default branch**;
ensure both mirror workflows are present there before changing it.

## Where the Retrode3 files come from

The general Letux kernel branch is *not* a complete Retrode3 hardware
source. Retrode3-specific source is maintained separately in the
`project-retrode3-v2` branch, including:

- `drivers/misc/retrode3/` (driver, headers, Kconfig and Makefile)
- `arch/mips/boot/dts/ingenic/lx16+retrode3.dtsi`
- `arch/mips/boot/dts/ingenic/lx16-retrode-*.dts`
- Board-level LX16 includes and relevant DTB Makefile entries
- Relevant general misc Kconfig/Makefile and Letux MIPS defconfigs

The original Retrode3 2.9.5 DTB was reproduced **byte-for-byte** from
the integration tree; see the boot/build evidence document above.

## GitHub Actions (no automatic code merges)

- `.github/workflows/sync-letux-upstream.yml`: mirrors
  `goldelico/letux-kernel:letux-current` into
  `upstream/letux-current`.
- `.github/workflows/sync-retrode3-upstream.yml`: mirrors
  `project-retrode3-v2` into `upstream/letux-retrode3`. It
  compares each new snapshot with the old mirror, checks only the
  Retrode3 source/integration paths listed above and creates a
  **review issue** when those paths change. It includes the changed
  files, a commit comparison, and best-effort `git cherry` patch-ID
  equivalence hints against `develop`. The first mirror sync creates
  a baseline without reporting historical changes.

A mirror branch can be force-updated to accurately follow its source,
but the workflows do **not** push to `develop`, `master`, or
`project-retrode3-v2`. If Letux rewrites history or if a shallow
history prevents meaningful `git cherry` results, perform a manual
review. A `-` from `git cherry` is only evidence that an
equivalent patch exists; it is **not** a proof that all behavior,
context and kernel ABI are equivalent. Squash or modified patches can
be misidentified, especially across kernel series.

The workflows require GitHub Actions with repository-scoped write
permissions for contents and review issues. A committed workflow is
not evidence that a scheduled run has completed: check Actions runs
for successful execution.

## How an upstream change reaches our kernel

1. Mirror the relevant source branch; do not overwrite current code.
2. Review the new source commits and file-scoped diff, then compare
   patch content against `develop`:
   ```sh
   git fetch origin develop upstream/letux-retrode3 upstream/letux-current
   git cherry origin/upstream/letux-retrode3 origin/develop
   git log --cherry-pick --right-only --oneline \
     origin/develop...origin/upstream/letux-retrode3
   ```
3. If the patch was already incorporated, document that fact and
   avoid importing it again. Where necessary, use `git range-diff`
   and inspect the full diff, especially if Letux used squash,
   rebase or adapted changes.
4. Select only wanted changes, preferably as small, reviewable PRs to
   `develop`. Do not merge the entire moving project or Letux
   history into `develop` merely because it changed.
5. Rebuild with the pinned GCC/toolchain, config and
   `LOCALVERSION=+`; validate on a removable SD card. Keep the
   tested `retrode3.ko` separate from the installed module until
   explicit hardware validation.
6. Promote the verified source from `develop` to `master` after
   checking regressions, source provenance, module ABI and artifact
   identity. Release image and package publication are **separate**
   gates.

When submitting our changes to Letux, send narrowly scoped
driver/DTS commits. If possible preserve individual commits rather
than squashing unrelated patches. After upstream acceptance,
`git cherry` can help detect equivalent changes regardless of
commit SHA, but neither Git nor this workflow automatically declares
an accepted upstream patch safe to import.

## Known 7.3 integration evidence and follow-ups

Our independently built 7.3.0-rc5 kernel (Bootlin GCC 8.2.0) was
physically booted on Retrode3 2.9.5. USB networking, cartridge
sense on MD/SNES, controller device registration and the Web
interface's automatic cartridge recognition worked. The tested
2.9.5 DTB matched the existing DTB by SHA256. Our separately
built full module set and final SD image have not yet been subjected
to complete release acceptance.

Open follow-ups: on boot with cartridges already inserted, the
associated LEDs remain on the heartbeat trigger until a physical
insert/remove event; ESP-Hosted/WLAN also reports an initialization
failure. The status LED policy is intended to be redesigned
separately. Do not claim that LED behavior or Wi-Fi has passed
release acceptance merely because the kernel boots.
