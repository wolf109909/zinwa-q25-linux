# Daily-use Q25 recovery retention

Do not flash or erase userdata. Root snapshots do not include the separate `/home` subvolume, nor raw boot partitions. Recovery is not a guarantee against hardware/driver hangs.

## Policy

- Reuse a suitable verified checkpoint for diagnostics; no new snapshot for read-only inspection or each retry.
- Keep at most **four agent-managed manual root/home pairs**: telephony baseline, explicitly retained application fallback, known-good baseline, and newest pre-change pair. Keep additional points only while a documented active recovery dependency requires them.
- Before removing a superseded pair, verify its exact subvolume IDs, paths, read-only flags and root description. Verify retained replacements, current/default root and home, and that no dependent recovery transaction is running.
- Delete only known project-created superseded points. Never delete user/unknown snapshots, current/default subvolumes or active rollback dependencies to satisfy a count.
- Use Snapper to delete its root snapshots and Btrfs for their separately created home counterparts. Do not recursively delete arbitrary snapshot directories.
- Existing numbered/timeline Snapper history has automatic count limits; the boot helper independently retains three `q25-good` roots. Manual snapshots have no automatic cleanup algorithm, so account for them explicitly.
- Record counts and whole-filesystem usage after risky work/cleanup. Btrfs snapshots share extents: count is not multiplied filesystem size. With quotas disabled, whole-filesystem usage is not exclusive snapshot cost.
- Package-based temporary diagnostics must consider automatic pre/post snapshot hooks. If an existing verified pair covers the transaction, a reviewed transaction-local override may suppress only those hooks; do not disable permanent recovery policy or unrelated hooks.

## Verified cleanup record — 2026-11-09

Initial inventory: **29 root +12 home =41**. First retired three concluded Bluetooth-rehearsal pairs (roots42/51/53). Later retired five superseded terminal/account/keyboard/earlier-SMS/GPU-probe pairs (roots14/19/20/28/46), after retaining newer verified replacements.

Final observed count: **21 root +4 home =25**, including ordinary Snapper history. Whole-filesystem used about5.87GiB; free about221.55GiB. Automatic history can change these counts afterward.

Retained manual pairs on this specific phone:

| Purpose | Root snapshot / subvolume | Home subvolume |
|---|---|---|
| Before modem/core work | 29 /296 |297 |
| Prism instance fallback |47 /317 |318 |
| Verified known-good baseline |55 /328 |329 |
| Latest pre-change |57 /331 |332 |

Automatic boot fallback root1/subvolume264 was also retained. All retained roots/homes were verified read-only after cleanup. No new snapshot was created by these cleanups or the temporary IMS observer package transactions. Original package archives and exact-file backups remain separate recovery assets.

Host evidence and current ledger: `~/q25-work/recovery-inventory/status.md`, the two deletion scripts, their result logs and `initial.txt`. These IDs are historical evidence, not portable defaults or permission to delete anything on another installation.
