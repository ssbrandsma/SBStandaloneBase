# Safe in-place UBIFS expansion research

## Verdict

**NO-GO — a simple, recoverable in-place expansion is not feasible within the
stated requirements.**

There is a narrow range where the mainline Linux 2.6.27 small-LPT geometry has
the same serialized pnode/nnode sizes and tree height as the 82-LEB filesystem.
That makes a superblock-only experiment theoretically interesting, but it does
not establish that the vendor backport can safely load, extend and commit the
existing LPT after changing a field it never expected to change in place.

The operational blocker is independent of that uncertainty: UBI resizing opens
the volume exclusively, while production UBIFS is mounted as the active UnionFS
writable root. SBStandaloneBase cannot obtain that exclusive access online.
After reboot, stock `/linuxrc` mounts `ubi0:ubifs` before any applet-controlled
hook can patch or resize it. Reboot alone therefore cannot complete the work.

No NAND write, volume resize, superblock modification, reboot or second-radio
access occurred during this research.

## Source basis and limitation

The exact Logitech vendor UBIFS backport source for kernel 2.6.26.8-rt16 remains
unavailable. The closest auditable source is upstream Linux v2.6.27, the first
mainline kernel with UBIFS, particularly:

- `fs/ubifs/sb.c`: superblock validation, reading and automatic `leb_cnt` growth;
- `fs/ubifs/lpt.c`: LPT geometry, serialization and CRC16 validation;
- `fs/ubifs/lpt_commit.c`: wandering-tree commit and LPT garbage collection;
- `fs/ubifs/master.c`: master-node resize accounting;
- `fs/ubifs/super.c`: mount initialization and capacity reporting;
- `fs/ubifs/recovery.c`: journal recovery;
- `fs/ubifs/ubifs.h` and `ubifs-media.h`: in-memory/on-media definitions.

UBI resize behavior was checked against upstream Linux v2.6.26
`drivers/mtd/ubi/cdev.c` and `include/mtd/ubi-user.h`. Physical behavior already
observed on the Radio—`w4/r0`, automatic-growth fields and the vendor BDI bug—is
consistent with a backport from this era, but upstream source is not proof of
vendor-source identity. That unresolved difference alone prevents a safety
claim for undocumented metadata surgery.

Primary source references:

- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/lpt.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/sb.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/master.c>
- <https://github.com/torvalds/linux/blob/v2.6.27/fs/ubifs/super.c>
- <https://github.com/torvalds/linux/blob/v2.6.26/drivers/mtd/ubi/cdev.c>
- <https://github.com/torvalds/linux/blob/v2.6.26/include/mtd/ubi-user.h>

## Relevant mount path

`ubifs_read_superblock()` reads `leb_cnt`, `max_leb_cnt`, journal/LPT/orphan
sizes and the big-LPT flag. If both the UBI volume and configured maximum are
larger than `leb_cnt`, it increases `leb_cnt`, writes the superblock immediately
on a read-write mount, and later recalculates `main_lebs`.

`ubifs_calc_lpt_geom()` then derives the LPT representation from the enlarged
values and verifies that twice the calculated LPT size fits in the reserved LPT
LEBs. `master.c` compares the master node's old `leb_cnt` with the new value and
adds the new LEBs to `empty_lebs`, `total_free` and `total_dark`. The updated
master is written during the normal read-write mount commit path.

This is a coordinated superblock, LPT and master-node process. It was designed
for a filesystem originally formatted with the final `max_leb_cnt`, not for
changing that maximum after the LPT was serialized.

Superblock validation also requires:

```text
max_leb_cnt >= leb_cnt
leb_cnt <= current UBI volume size
SB + master + log + LPT + orphan + main == leb_cnt
valid journal and reserved-pool constraints
```

Every UBIFS node has a CRC32. LPT nodes use CRC16 and sizes derived from geometry.
Master nodes carry their own `leb_cnt` and aggregate free/dirty/used/dead/dark
accounting. A syntactically repaired superblock CRC does not repair or prove any
of those structures.

## LPT formulas and calculator

`tools/ubi/ubifs-lpt-geometry.py` reproduces the upstream v2.6.27 formulas for
the Radio geometry. Constants are fanout 4, CRC field 16 bits, type field 4 bits,
LEB size 129024, minimum I/O 2048, two reserved LPT LEBs and small-LPT mode.

For a fully grown target using the production fixed layout (one superblock LEB,
two master, three log, two LPT and one orphan), results are:

| max/leb count | main | lnum bits | pcnt bits | pnode bytes | nnode bytes | height | pnodes | nnodes | calculated LPT bytes | LPT LEBs needed/reserved | model |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 82 | 73 | 7 | 5 | 17 | 12 | 3 | 19 | 8 | 2048 | 1 / 2 | small |
| 100 | 91 | 7 | 5 | 17 | 12 | 3 | 23 | 9 | 2048 | 1 / 2 | small |
| 128 | 119 | 7 | 5 | 17 | 12 | 3 | 30 | 11 | 2048 | 1 / 2 | small |
| 139 | 130 | 8 | 6 | 17 | 12 | 3 | 33 | 13 | 2048 | 1 / 2 | small |
| 160 | 151 | 8 | 6 | 17 | 12 | 3 | 38 | 14 | 2048 | 1 / 2 | small |
| 256 | 247 | 8 | 6 | 17 | 12 | 3 | 62 | 21 | 2048 | 1 / 2 | small |
| 400 | 391 | 9 | 7 | 17 | 12 | 4 | 98 | 35 | 4096 | 1 / 2 | small |
| 500 | 491 | 9 | 7 | 17 | 12 | 4 | 123 | 42 | 4096 | 1 / 2 | small |
| 600 | 591 | 10 | 8 | 17 | 12 | 4 | 148 | 51 | 4096 | 1 / 2 | small |
| 638 | 629 | 10 | 8 | 17 | 12 | 4 | 158 | 54 | 4096 | 1 / 2 | small |

The aligned LPT size remains small and the two-LEB reservation is sufficient for
all requested values. That fact is necessary but not sufficient: it says nothing
about whether the existing serialized tree contains the right topology/nodes.

For an 82-LEB filesystem whose `max_leb_cnt` alone is hypothetically patched,
the existing main population remains 73 LEBs. Small-LPT pnodes omit `pcnt_bits`,
and small LPT has no serialized lsave table, so changes to `lnum_bits` and
`pcnt_bits` do not change pnode/nnode byte sizes. Tree height is the decisive
structural boundary:

```text
max_leb_cnt <= 265: height 3, pnode 17 bytes, nnode 12 bytes
max_leb_cnt >= 266: height 4; existing root topology is incompatible
```

Thus 265 is the largest value with the same basic small-LPT encoding dimensions.
It is **not** a proven safe maximum. Matching height and node sizes does not prove
that missing future pnodes, nnode branches, CRCs, free-space state, master totals
and commit behavior can be reconstructed from an LPT originally created with
maximum 82.

## Superblock-only transformation experiment

`tools/ubi/patch-ubifs-superblock.py` operates only on regular disposable files,
refuses in-place output and devices, verifies the original UBIFS node CRC, changes
offset 44 (`max_leb_cnt`) and recalculates the v2.6.27 superblock CRC32.

On a disposable copy of `C:\SqueezeboxBackup\ubifs-online.img`, changing maximum
82 to 221 produced a CRC-valid superblock that the read-only inspector decoded:

```text
leb_cnt=82
max_leb_cnt=221
log/lpt/orphan=3/2/1
format=w4/r0
```

This proves only field location and CRC calculation. No compatible kernel/UBI
mount was performed, so it does not prove LPT or data safety. The utility prints
`mount_compatibility_proven=false` deliberately and contains no device-writing
path.

Changing only `leb_cnt` would immediately conflict with current UBI size and the
sum of fixed/main areas. Changing `main_lebs` is not a superblock field; it is
derived. `lpt_lebs`, log/orphan geometry and `max_bud_bytes` need not necessarily
change for a modest growth, but LPT nodes and master accounting must remain
internally consistent. For targets needing a new tree height, the LPT must be
rebuilt, including nnodes, pnodes, locations, CRC16 values and master root
pointers. That is filesystem reconstruction, not a superblock edit.

## Historical formatter controls

`tools/ubi/test-inplace-expansion.sh` generated disposable images with the pinned
historical `mkfs.ubifs` 1.3 for every requested maximum. All decoded as `w4/r0`,
LEB 129024, minimum I/O 2048 and two LPT LEBs. Empty images through maximum 500
materialized 13 LEBs with four log LEBs; 600 and 638 materialized 14 LEBs with
five log LEBs. This demonstrates that correctly formatting for a larger maximum
works, but those images were born with matching LPT geometry and do not validate
retrofitting the production filesystem.

There is no available host kernel/UBI environment matching Logitech's vendor
backport. QEMU userspace can validate tools and bytes but cannot validate mount,
journal recovery or LPT commits. Experiments C/D therefore stop at structural
parsing; no claim of filesystem preservation is made.

## UBI resizing

Capacity layers are distinct:

1. UBI volume size controls how many LEBs the volume exposes.
2. UBIFS `max_leb_cnt` controls the designed growth ceiling and LPT geometry.
3. UBIFS `leb_cnt` controls the currently incorporated LEBs and grows on mount.

Upstream Linux 2.6.26 implements `UBI_IOCRSVOL`, but handles it by opening the
target volume with `UBI_EXCLUSIVE` before calling `ubi_resize_volume()`. A mounted
UBIFS holds the volume open, so the resize cannot obtain exclusive access. The
physical Radio also has no installed `ubirsvol` utility. Bundling an ioctl caller
would not remove the exclusive-access restriction.

Even if a vendor variation allowed online UBI enlargement, the mounted UBIFS
instance has already read `vi.size`, `max_leb_cnt` and LPT geometry. There is no
demonstrated notification path that safely rebuilds this in-memory state. The
documented growth path runs while reading the superblock on mount. A remount is
not equivalent to an unmount/new mount of the active UnionFS backing store.

With `sbdata` untouched, only 139 UBI LEBs are free. Absolute zero-reserve UBI
growth could take production from 82 to 221 LEBs. Retaining the project's
64-LEB safety reserve limits it to 157 LEBs:

```text
157 * 129024 = 20,256,768 nominal bytes
```

Usable UBIFS capacity would be lower. Targets 400–638 are impossible without
changing `sbdata`, which this task prohibits.

## Online and offline feasibility

### Online

There is no supported UBIFS ioctl to change `max_leb_cnt`. Rawly replacing LEB 0
while UBIFS is mounted races cached superblock/LPT/master state and normal
commits. It can leave the running kernel and flash disagreeing. UBI resize needs
exclusive access. Online in-place expansion is therefore rejected.

### Offline

A theoretically safe transformer would need exclusive access, vendor-exact
geometry rules, transactional rewriting of every affected LPT/master structure,
and recovery for every power-loss boundary. Stock boot provides none of that:
`/linuxrc` mounts production UBIFS before `rcS.local`, media hooks or
SBStandaloneBase run. The active UnionFS root cannot be safely unmounted by an
applet. An ordinary reboot simply repeats the same ordering.

Therefore an offline transformation cannot become an SBStandaloneBase feature
without changing the boot path or adding an early recovery environment—both
explicitly forbidden.

## Power-loss analysis

Patching the superblock and resizing UBI are separate persistent operations.
Power loss can leave:

- a new maximum with the old UBI size;
- a larger UBI volume with the old maximum;
- a rewritten superblock but old LPT/master state;
- partially transformed LPT nodes or mismatched master copies;
- the stock boot attempting journal recovery against unproven geometry.

Some individual UBI LEB changes and volume-table updates are designed to be
atomic, but the multi-structure filesystem transaction is not. No persistent
pre-mount state machine exists to distinguish and repair these states. A failed
mount can enter the Radio's destructive factory-reset branch. The procedure
cannot meet the requested interrupted-operation bootability or credible recovery
criteria.

## Live-device note

Read-only investigation was restricted to `192.168.1.222`; `192.168.1.141` was
left untouched and was reported offline. During a final COM5 read-only metadata
query, COM5 began returning garbled bytes and the Radio stopped responding to
SSH and ping. No write, resize, mount, reboot or recovery command was issued.
Physical interaction was stopped immediately. Previously captured and verified
geometry is used above; the unexpected connectivity state must be checked by the
owner separately before any future device work.

## Direct answers

1. **Can the existing UBIFS be enlarged without recreating it?** Not safely or
   operationally within these requirements.
2. **Can `max_leb_cnt` be increased safely?** A CRC-correct edit is easy; safe
   filesystem behavior is unproven and cannot be assumed.
3. **Maximum compatible size before LPT layout changes?** Basic small-LPT node
   dimensions and height match through 265, but this is not a safe limit.
4. **Can it be performed online?** No: mounted-state metadata surgery is unsafe
   and UBI resize requires exclusive access.
5. **Can reboot finish it without boot changes?** No; stock `/linuxrc` mounts the
   volume before SBStandaloneBase can act.
6. **Would it preserve existing data?** Not demonstrated; LPT/master mismatch can
   lose free-space accounting or prevent mounting.
7. **Power loss outcome?** Potential mixed geometry with no pre-mount recovery
   agent; bootability cannot be guaranteed.
8. **Can SBStandaloneBase manage it end-to-end?** No under the current boot flow.
9. **Maximum realistic capacity?** No safe expansion is currently achievable;
   capacity arithmetic gives 157 LEBs with reserve or 221 with none while
   `sbdata` remains untouched.
10. **Safer than rebuilding?** No. A correctly formatted replacement has known
    coherent metadata, while retrofit transformation is undocumented and less
    recoverable—though rebuilding is also excluded here.

Do not add an Expand Storage action for this approach. The research result is
NO-GO and no physical experiment is recommended under the current constraints.
