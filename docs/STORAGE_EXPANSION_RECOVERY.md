# Storage expansion failure and recovery analysis

## Durable state machine

The prototype models these ordered states:

```text
idle -> backup_written -> backup_verified -> updater_armed ->
destructive_started -> production_removed -> production_created ->
image_written -> restore_started -> restore_verified -> complete
```

The production design requires two redundant journal records with schema,
generation, previous/current state, target geometry, all volume identities,
artifact and backup hashes, CRC and commit marker. At least one journal copy
must survive deletion of production `ubifs`; putting the only journal there is
forbidden. Atomic file rename in the prototype demonstrates sequencing but is
not proof of raw-NAND power-loss safety.

## Failure matrix

| Interruption | Persistent truth | Safe response |
|---|---|---|
| Before/during backup | Original UBIFS intact; backup absent/incomplete | Normal boot; discard only uncommitted backup generation |
| After backup write | Original intact; digest not committed | Re-read and verify; never enter destructive state |
| After backup verification | Original and committed backup intact | Preparation may resume or normal boot may continue |
| After updater armed | Original and backup intact | Early updater revalidates everything; user may still cancel before destructive marker |
| During backup resize/copy | Original intact; one backup generation may be incomplete | Select last committed redundant generation; otherwise stop for external recovery |
| After destructive marker/before removal | Original and backup intact | Early updater may resume after full validation; normal `/linuxrc` must not continue |
| During/after production removal | Original may be absent; backup committed | Recreate only exact ID/name from journal; if backup unreadable, stop for external recovery |
| During production creation | Volume may be absent or partially described | Accept only exact healthy volume; otherwise stop, never guess or auto-delete |
| During image write | Update marker/corruption may be set | Rewrite only after explicit state/identity validation; no unattended retry policy until physically tested |
| During restore | New filesystem partial; backup intact | Reformat/restart restore only under reviewed recovery policy, or resume from verified per-file manifest |
| After restore verification | New filesystem and backup intact | Clean unmount, commit complete, then normal boot |
| Before normal boot resumes | Complete journal required | Refuse normal boot unless exact final topology and manifest verify |

## Limits on the power-loss claim

True unattended recovery is impossible with the current stock boot flow. It
always attempts to mount `ubifs` before any writable hook can inspect migration
state. If that volume is absent or incomplete, `/linuxrc` can enter destructive
factory-reset handling. A pre-mount environment must intercept this condition.

The project therefore does **not** claim power-loss safety. It supplies a
reviewable state model and simulation only. Power-cut testing is still required
at every transition on disposable hardware, along with an offline recovery path.
