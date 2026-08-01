# Changelog

## v0.1.2

- Hide selected accounts from the boot user selector without changing account
  data.
- Represent visible accounts as compact virtual slots and translate account
  metadata queries and account loads back to their physical ACT slots.
- Keep the active and default account slot consistent with the selector after a
  translated selection.
- Prevent cross-thread mapping leaks with per-thread re-entrancy tracking and
  atomic slot-map snapshots.
- Apply visibility changes by restarting the Wii U Menu once after the Aroma
  configuration menu closes.
- Prevent an error fallback from selecting a different, potentially hidden
  physical account.

## v0.1.1

- Initial public release.
