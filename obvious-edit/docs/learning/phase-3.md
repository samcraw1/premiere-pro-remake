# Phase 3 — the project & timeline data model

A guided walkthrough of Phase 3: what was built, why it is shaped this
way, and what to look at when you read the code.

## 1. Phase purpose

Phases 0–2 built a shell that can import and inspect media but holds no
document: close the window and every decision is gone. Phase 3 gives
Obvious Edit a document — the project model — and a file format for it.

Concretely, three modules appear in a new `src/core/` layer:

- `oe_time.[ch]` — the time-model floor as types: reduced rational
  frame rates, integer-microsecond positions, and frame↔µs conversions
  with one documented rounding rule. No `double` anywhere.
- `oe_project.[ch]` — the in-memory document: name, frame rate, media
  references, ordered tracks, sorted non-overlapping clips. A GObject
  final type in the `OeMediaLibrary` idiom: one plain-function
  observer, main-thread-only mutation, deep-copy getters.
- `oe_project_format.[ch]` — the `.oe` file: strict JSON v1, closed
  schema, integer-only serialization, atomic saves.

And the shell learns File > New/Open/Save: Open strictly parses a file,
resets the session, and re-imports every referenced path through the
existing cancellable worker, so probe statuses (OK / MISSING /
UNSUPPORTED) and the relink flow keep working unchanged.

What Phase 3 deliberately does **not** add: no timeline widget, no
playback, no undo/redo. The model exists and is fully tested; the
views come next.

## 2. Per-file explanations

| File | Role |
|---|---|
| `src/core/oe_time.[ch]` | `OeRational {num, den}` always reduced with `den > 0`; `oe_time_rate` (validating constructor), `oe_time_rate_reduce` (total reducer), `oe_time_round_ratio` (the rounding primitive), `oe_time_frame_to_us` / `oe_time_us_to_frame`. |
| `src/core/oe_project.[ch]` | `OeProject` GObject final type owning `OeSequence → OeTrack → OeClip`. Mutations: `add_track`, `insert_clip`, `move_clip`, `remove_clip`, `set_name`, `add_media`, `add_media_ref`. Deep-copy `get_sequence`; observer fires once per successful mutation. |
| `src/core/oe_project_format.[ch]` | `oe_project_format_load` / `oe_project_format_save` — the only code that knows the JSON shape; errors carry the `OE_PROJECT_FORMAT_ERROR` domain. |
| `src/app/oe_main_window.c` (changed) | Project commands registered in `constructed`, cleared in dispose in dependency order; session epoch guards stale worker results; file choosers and reporter feedback only — no parsing here. |
| `src/app/oe_import_worker.[ch]` (changed) | Jobs and results carry an opaque session tag so results from a dead session are dropped. |
| `src/app/oe_media_bin.c` (changed) | `oe_media_bin_set_library()` — the persistent bin widget follows a newly created session library without owning it. |
| `tests/test_time.c` | Reduction, typed rejection, NTSC/film/PAL/audio-rate conversions, identities, halves-away rounding. |
| `tests/test_project.c` | Ordering, overlap/adjacency, the uniform duration rule, observer-once, deep copies, media refs, destruction order. |
| `tests/test_project_format.c` | Round trip (byte-identical), integer-only serialization, strict-parse failures, newer-version rejection, atomic-failure byte identity. |
| `docs/project-format.md` | The v1 schema and its strictness rules — the format contract. |

## 3. Block-by-block build walkthrough

**Time first, because everything else stands on it.** `oe_time_rate()`
validates (`den <= 0` and non-positive numerators are typed errors) and
reduces. `oe_time_rate_reduce()` is the total primitive: sign normalized
into the numerator, `{0, 0}` sentinel for 0/0. Rounding lives in one
function, `oe_time_round_ratio`, "nearest, halves away from zero", in
pure integer arithmetic — every conversion rounds exactly once, at the
final step, so `frame → us → frame` is the identity on every frame
boundary.

**The model second.** `OeProject` owns a plain-struct `OeSequence`
(exactly what the format serializes) plus the observer. Tracks are
`GPtrArray`s of value structs; clips are kept sorted by position at
insert. Validation is typed: `BAD_RANGE` (negative placement, empty
source range), `OVERLAP` (with the clip it hit named in the message),
`BAD_TRACK`, `BAD_CLIP`, `UNKNOWN_MEDIA`, `DUPLICATE_REF`. Deep-copy
getters mean a reader can never observe a half-mutated document.

**The format third.** Load: parse (SYNTAX) → check the root and
version (MISSING/VERSION) → walk every member strictly, rejecting
unknown members at every depth (UNKNOWN_MEMBER) and wrong types (TYPE,
including float tokens) → build via the model's own validating
constructors so VALUE failures are impossible to bypass. Save: one
deterministic writer; temp file in the target directory, fsync, rename
only on success.

**The shell last.** New/Open/Save are registered at
`oe_main_window_constructed` next to media.import, cleared in dispose
in dependency order. Open: strict parse → version check → reset the
session (model + library, bumping the epoch) → re-import each
referenced path through the existing worker → probe results mark assets
OK/MISSING/UNSUPPORTED → model populated, bin refills as a projection.

## 4. C concepts in play

- **Rational invariants as a discipline, not a hope.** Every
  constructor maintains "reduced, den > 0" — call sites never reduce
  by hand.
- **Value structs with clear ownership rules.** `OeClip` is plain
  integers; `OeSequence`/`OeTrack` own `GPtrArray`s with paired
  `_init`/`_clear`/`_copy` helpers so deep copies are mechanical.
- **Observer without signals.** A single plain function pointer +
  user data (the `OeMediaLibrary` idiom): no marshalers, no
  reference cycles, trivial to test, trivial to call from dispose
  safely.
- **Closed-schema JSON reading.** json-glib's object iteration lets
  the loader enumerate members and reject ones it does not know —
  the property that keeps a future save from silently dropping data.
- **Atomic writes as a sequence.** temp-in-same-directory (so rename
  is atomic), write, fsync, rename. A failed save must leave the
  pre-existing file byte-identical — the tests prove it, including
  that no temp residue remains.
- **Session epochs.** An opaque, monotonically increasing tag on every
  worker job; results whose tag does not match the current session are
  dropped. This closes the stale-result aliasing hole opened when Open
  replaces the media library while jobs are in flight.

## 5. Ownership table

| Object | Owner | Lifetime |
|---|---|---|
| `OeProject` | the main window (clears in dispose, before the library) | session |
| `OeSequence` deep copies | the caller of `oe_project_get_sequence` | until `oe_sequence_clear` |
| Media path strings | the project (getters copy; `dup_media_path` returns a transfer) | project lifetime |
| Worker jobs/results | the worker until delivery; results die with their session tag | session |
| Project files | the user on disk; the model never caches file state | user-controlled |
| The bin widget | the window (persists across session resets via `set_library`) | window |

Destruction order in the window's dispose: project commands/actions →
project → library → worker. The project's observer is disconnected
before teardown so dispose never notifies (tested).

## 6. Call flow

**Save.** menu → `app.file-save` action → window asks the chooser only
if no path is current → `oe_project_format_save` → temp + fsync +
rename → reporter "Project saved to <path>". A failed save leaves the
old file byte-identical and does not update the current path.

**Open.** menu → chooser → `oe_project_format_load` (any typed failure
→ reporter message, session untouched) → reset session (new library,
bump epoch, bin follows) → for each media entry, submit through the
existing cancellable worker → probe results arrive on the main context,
filtered by epoch → assets marked OK/MISSING/UNSUPPORTED exactly as an
interactive import → model populated from the loaded project → reporter
"Loaded <name> (N tracks, M media)".

**Edit (API level).** every mutation validates, then mutates, then
sorts (if needed), then notifies exactly once. Failed mutations never
notify.

## 7. Alternatives considered

- **GObject signals for change notification.** Rejected: one observer
  is all the app needs (the window), and a plain pointer keeps the core
  GTK-free without `g_cclosure` machinery and keeps dispose ordering
  explicit.
- **Float seconds / floats in JSON.** Rejected outright — the no-double
  floor. NTSC 30000/1001 cannot survive a float round trip; num/den
  integer pairs can, exactly.
- **Tolerant loading (skip unknown members).** Rejected: a reader that
  drops what it does not understand will silently drop the same data on
  the next save. Strict + closed schema makes data loss loud.
- **Storing probe metadata/thumbnails in the project file.** Rejected
  for v1: they are derivable session state. Re-import on Open reuses
  the existing worker, statuses, and relink flow instead of adding a
  second source of truth.
- **Undo/redo in the model now.** Deferred (roadmap order: after the
  playback clock). The model's typed mutations are the seams a future
  command history will wrap.

## 8. Bug log

- **Compound-literal comma in a macro.** The first
  `oe_time_rate_reduce` draft passed a `{0, 0}` compound literal to
  `g_return_val_if_fail`; the preprocessor split it at the comma and
  read a phantom third argument. Fixed by naming the sentinel instead
  of inlining it.
- **Asserting on a value return as if it were a pointer.** The first
  test draft wrote `g_assert_null (oe_time_rate (...))` — but the rate
  functions return `OeRational` by value. Fixed by asserting the `{0,
  0}` sentinel fields and the typed error instead.
- **"Own footprint" misread.** A test assumed moving clip B onto
  clip A's exact span should succeed because "a clip does not block
  itself". The implementation is right: B's own *current* footprint is
  exempt; A's span is not. The test now pins both behaviors (no-op move
  succeeds; landing on the neighbor fails with OVERLAP).
- **Failed-mutation test that did not fail.** An observer test re-
  inserted a clip into a track that a prior step had emptied — the
  mutation legitimately succeeded. Replaced with genuinely invalid
  mutations (empty source range, unknown media ref).
- **Ref vs index.** `oe_project_dup_media_path` takes a media
  *reference number*; `oe_project_get_media` takes a positional index.
  The test conflated them and was corrected — and now documents the
  distinction.
- **Root can defeat permission tests.** The atomic-failure test makes
  the directory read-only to force a save failure; under `root` that
  cannot fail. The test skips honestly rather than asserting a lie.

## 9. What is next

Phase 4 puts the model on screen: the timeline widget — track
containers, clip rectangles projected from the sorted arrays, trims and
moves driving the same typed mutations the tests exercise — plus the
program monitor filling its empty state. The playback clock and audio
output wiring follow (Phase 5), then undo/redo around the command
seams, snapping, ripple edits, and export.

## 10. Five review questions (with answers)

1. **Why is a clip's duration `source_out − source_in` even for still
   images?** One rule, no special cases: a still's "source range"
   encodes its screen duration (5 s default at insert). Every consumer
   — layout, format, future playback — reads duration the same way.
2. **Why reject unknown JSON members instead of skipping them?**
   Skipping means the next save silently drops them: real data loss
   with no error. Closed-schema loading turns that into a typed error
   naming the member.
3. **How does Open avoid acting on worker results from a previous
   session?** Every job carries the session epoch it was created in;
   delivered results with a stale epoch are dropped before they can
   touch the current library or model.
4. **Why does the observer fire exactly once per mutation — and never
   during dispose?** One notification per successful change keeps
   projections (the bin) cheap and idempotent; disconnecting before
   teardown means destruction can never re-enter a dying view.
5. **What proves the round trip is lossless?**
   `tests/test_project_format.c` saves, loads, and saves again,
   asserting the two files are byte-identical — including NTSC rates,
   document-order media refs, and the still-duration rule.
