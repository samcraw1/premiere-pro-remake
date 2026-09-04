# Phase 6 — undo and redo: the model learns to step back

A guided walkthrough of Phase 6: what was built, why it is shaped this
way, and what to look at when you read the code.

## 1. Phase purpose

Phases 3–5 made the model legal, visible, and playable — but every
edit was a one-way door: a clip moved, trimmed, inserted, or deleted
was gone from the keyboard's reach. Phase 6 adds the safety net every
NLE user assumes: full undo and redo over the timeline.

Concretely, four things appear:

- `src/app/oe_undo_stack.[ch]` — the GTK-free command-object history:
  records for insert, delete, move, and trim; a depth-100 strict-LIFO
  stack; recorder helpers that perform the model mutation and push a
  record only on success; undo/redo that apply inverses ONLY through
  the same typed `oe_project_*` mutators; a changed-state seam for
  command enablement.
- `oe_project.[ch]` (extended) — two targeted getters
  (`oe_project_get_clip_count`, `oe_project_get_clip`) so recording
  can capture the exact positional state of one clip without copying
  the whole sequence.
- Wiring — `edit.undo` / `edit.redo` actions in the window,
  the four edit sites (timeline drag commit, selection delete,
  toolbar insert, bin DnD insert) routed through the recorder,
  enablement driven by the changed seam, status-bar reports,
  selection clearing, playhead preservation, and auto-pause before
  applying while the session is playing. `reset_session` clears the
  stack on open/new: history never crosses a project boundary.
- `tests/test_undo_stack.c` — a 14-case GTK-free suite: per-op
  inverse correctness, record-time and apply-time typed rejection,
  interleaved round trips through the JSON v1 serializer, depth
  eviction, redo-branch clearing, observer behavior, the changed
  sequence, and virtual-clock auto-pause.

What Phase 6 deliberately does **not** add: multi-step grouping
(one drag = one record is already the natural granularity), undo of
non-model concerns (zoom, selection, playhead — all widget-session
state by design), snapshot records for hard-to-invert operations (the
API accepts them; nothing produces them yet), and snapping/ripple
editing. History is per-project session state, never serialized:
`.oe` files stay format-v1 untouched.

## 2. Per-file explanations

| File | Role |
|---|---|
| `src/app/oe_undo_stack.[ch]` | The command-object history: `OeUndoOpKind` (INSERT, DELETE, MOVE, TRIM), `OeUndoRecord` (label, track index, owned deep-copied `OeClip` for insert/delete, old/new µs bounds for move/trim), strict LIFO with depth 100 (oldest dropped), redo branch cleared on any recorded edit, `OeUndoChangedFunc(can_undo, can_redo, ud)` seam, and `oe_edit_*` recorder helpers that mutate-then-record. |
| `src/core/oe_project.[ch]` (extended) | `oe_project_get_clip_count(track)` and `oe_project_get_clip(track, index, &clip)` — positional reads for record capture and index recovery after position-ordered insertion. |
| `src/ui/oe_timeline.c` (changed) | `commit_drag` routes move and trim commits through `oe_edit_move_clip` / `oe_edit_trim_clip` (one record per committed drag); the widget carries a weak stack pointer set by the window. |
| `src/ui/oe_main_window.c` (changed) | `edit.undo` / `edit.redo` handlers (status reports, selection clearing, auto-pause through the session, playhead untouched); every history transition drives `oe_command_set_enabled`; bin-insert paths (toolbar + DnD) and selection delete use the recorder; `reset_session` clears the stack at open/new; dispose clears the stack mirror. |
| `src/app/oe_command.h` (comment fix) | Undo/redo entries move from "reserved" to "wired in Phase 6"; shuttle/marks stay deferred to later phases (they never owned this one). |
| `tests/test_undo_stack.c` | Fourteen GTK-free cases linking only the modules under test; audio adapter init/shutdown around the run for the auto-pause case; `SDL_AUDIODRIVER=dummy` via the meson env. |
| `meson.build` | New sources in the executable list; one new test target (14 suites total) with a 300 s timeout and the dummy-driver env. |
| `docs/*` | Architecture "The command-object history (Phase 6)" section, code-map rows (plus Phase 5 tree/table drift fixes), glossary history terms, this file. |

## 3. Block-by-block build walkthrough

**Records are commands, not diffs.** Each `OeUndoRecord` carries
everything needed to apply its inverse in one place: insert/delete
hold a deep-copied `OeClip` (media ref + position + source range),
move/trim hold the old and new bounds as plain µs integers. No record
points into the model — the model is free to change underneath
(eviction, later edits), and redo replays from owned data.

**The mutator stays the only gatekeeper.** The recorder helpers are
deliberately thin: perform the `oe_project_*` call, and only if it
returns TRUE push a record. Every validation the model enforces
(OVERLAP, BAD_RANGE, BAD_CLIP, bounds, media-ref checks) applies
identically to an undoable edit and to its inverse later. There is no
second validation layer to drift out of sync, and a rejected edit
leaves no trace in history — the stack only ever holds edits that
actually happened.

**Positional identity is recovered, not assumed.** The model inserts
clips position-ordered, so the index a clip had before a delete is
not the index it gets back after a restore. Insert records recover
their resulting index by matching the inserted tuple; delete records
rely on the position-ordered restore putting the clip back where
position order says it belongs. The JSON round-trip test proves the
property end to end: undoing everything lands byte-identical to a
pristine empty project, redoing lands byte-identical to the built one.

**Rejection is a first-class outcome.** Undo/redo apply inverses
through the same mutators, so an inverse can fail — a direct
non-recording writer above the model can occupy the gap an insert
wants back. The typed error propagates out of
`oe_undo_stack_undo/redo`, and the cursor does not move: the failed
step stays current and can be retried once the interference is
cleared. No partial application exists — mutators are single-phase
(validated fully before the first write), so a rejected inverse
cannot leave a half-applied model.

**The seam drives the buttons.** `OeUndoChangedFunc(can_undo,
can_redo, ud)` fires on every transition — record, undo, redo, clear
— and the window maps it to `oe_command_set_enabled`. Nothing polls;
the enablement state is exactly the history state by construction.
The same callback carries the status report: "Undo: <label>" /
"Redo: <label>", or "Nothing to undo" / "Nothing to redo" — one line,
no dialogs.

**Time is not history.** Undo/redo never move the playhead: the clock
is session state, not model state, and rewinding the user's clock as
a side effect of rewinding their edit would be hostile. The one
required interaction is ordering: if the session is PLAYING, undo
pauses first, then applies — playback's next play re-copies the
mutated project, so a stale snapshot never survives a paint cycle.
The auto-pause test proves the order on a virtual clock: session
not PLAYING, op applied.

**Project boundaries clear the stack.** `reset_session` (open, new)
calls `oe_undo_stack_clear` — records reference media refs and model
shape from a replaced project, and the changed seam reports the empty
history so the buttons disable immediately.

## 4. C concepts in play

- **Command objects.** The GoF command pattern in plain C: each edit
  is a value (union of kind + payload) whose inverse is applied by a
  switch in one place. No closures, no vtables — a tagged struct is
  the whole machinery.
- **Deep copies as ownership.** The record owns a full `OeClip` value
  for insert/delete. Values, not pointers into model storage, are
  what make records immune to model churn (and to their own eviction
  order).
- **Sole-path discipline.** Instead of intercepting every write, the
  codebase gains one blessed write path (`oe_edit_*`) and the
  invariant lives in the module header: edits that should be undoable
  go through it. A future hard-to-invert mutator's escape hatch — a
  snapshot record — is documented on the same seam.
- **Ring-of-records depth capping.** The stack is a `GPtrArray` with
  a cursor; pushing past depth 100 drops index 0. The cursor moves
  only on successful apply, so `get_size` counts parked records too —
  history length and cursor position are related but distinct.
- **Callback + user_data seams.** The same observer pattern as the
  model's notify and the library's session events: a function
  pointer, a `gpointer`, no boxed signal machinery, GTK stays blind.
- **Adapter lifecycle in tests.** The auto-pause case needs the audio
  adapter initialized (the session opens an SDL stream at play);
  `oe_audio_output_init/shutdown` bracket the suite exactly as the
  application brackets its own lifetime.

## 5. Ownership table

| Object | Owner | Lifetime |
|---|---|---|
| `OeUndoRecord` payloads | the stack until eviction/clear | depth-100 window |
| The stack | the window | created with the session, cleared on project replace, freed in dispose |
| `OeClip` copies inside records | each record | record lifetime (freed with the stack) |
| Undo/redo enablement state | the command registry, driven by the seam | per transition |
| Selection after undo/redo | the timeline (cleared) | per application |
| Playhead position | the playback session | untouched by history |

Teardown order (Phase 2's invariant, one more object): free the stack
after the timeline/session references drop and before the project —
records own plain values only, so the stack never frees model memory.

## 6. Call flow

**A recorded move.** Drag ends → `commit_drag` →
`oe_edit_move_clip(project, stack, track, index, new_pos, &error)` →
model validates and mutates → record (MOVE, old/new bounds) pushed,
redo branch discarded → changed seam → "Move clip N on track M" via
the recorder seam at commit time, enablement refreshed.

**Undo.** Ctrl+Z → `edit.undo` action → window pauses the session if
PLAYING → `oe_undo_stack_undo(project, &record, &error)` → the
record's inverse runs through the `oe_project_*` mutator → success:
cursor steps back, selection clears, status reports "Undo: <label>",
seam re-drives enablement → the timeline's observer repaints from the
mutated model.

**Redo.** Ctrl+Shift+Z → mirror path, forward through the records.
Recording any new edit clears the redo branch first — the user has
left that future by editing.

**A rejected step.** Inverse fails (say, OVERLAP) → typed error
returns, cursor frozen, status reports the failure line → after the
interference is cleared (by an undo or a delete), the same step
applies. No history is lost to a failed application.

**Open / new.** `reset_session` → `oe_undo_stack_clear` → seam fires
(both buttons disable) → fresh project starts with empty history.

## 7. Alternatives considered

- **Snapshot-per-edit (save the whole sequence each step).**
  Trivially correct inverses, but O(project) memory per keystroke,
  and it hides which property changed — labels like "Move clip 2"
  become "restore state 47". Rejected as the default; kept as the
  documented escape hatch for future hard-to-invert mutators.
- **Intercept at the model (wrap every mutator with recording).**
  Total capture with zero recorder call sites — but the model grows
  a history dependency (it is a GTK-free, UI-agnostic document), and
  undo/redo's own writes would need a bypass channel, the classic
  recursion trap. The recorder above the model keeps both directions
  explicit.
- **Timestamp/label filtering for coalescing.** Real editors coalesce
  slider drags. Phase 6 has exactly one drag → one commit event, so
  coalescing machinery would be dead code; the record stream is
  already the right granularity. Revisit if a future interaction
  produces rapid-fire commits.
- **Serializing history into `.oe` files.** No: versioned schema
  churn, cross-project record ambiguity, and little user value —
  session state stays session state, exactly like zoom and selection.

## 8. Bug log

- **`g_ptr_array_remove` is not `g_ptr_array_remove_index`.** The
  depth-cap eviction called `g_ptr_array_remove(array, 0)` — which
  matches a pointer *value* (NULL), not index 0 — so the oldest
  record was never dropped and the stack grew unbounded. Caught by
  the new depth-eviction test the first time it ran: 120 pushes
  reported size 120. Fix: `g_ptr_array_remove_index`, which is the
  index-based call and runs the element free func. The test that
  exists to pin the cap is the one that caught the bug — write the
  depth test before you trust a cap.
- **`g_assert_cmpint64` does not exist.** The test file used it
  throughout for µs comparisons; the build failed with implicit
  declarations. The codebase convention (test_project.c) is
  `g_assert_cmpint`, which compares through GLib's standard macro
  machinery. Lesson: check the suite next door before writing
  assertions.
- **The depth fixture outgrew its media.** The eviction test built
  120 one-second clips with `source_out = (i+1)·1s` — the source
  range grew past the default media duration and the model (rightly)
  rejected insert #6. A fixed one-second source range keeps the
  fixture legal while the positions still vary.
- **The auto-pause test trapped with SIGTRAP — the audio adapter was
  never initialized.** `play()` warns "audio subsystem not
  initialised" and the state never reaches PLAYING, so the assert
  fired. test_playback_clock.c brackets its run with
  `oe_audio_output_init/shutdown`; the undo suite needed the same
  bracket (plus the include). Reuse the fixture pattern from the
  suite that already solved the problem.
- **A line-anchored insert landed outside its function.** The
  eviction fix was first inserted after the preceding function's
  closing brace (the anchor was chosen by stale line numbers). The
  compiler caught it immediately; the repair replaced the correct
  call site inside `stack_push` in one atomic edit. Rule reaffirmed:
  anchor edits on content, never on remembered line numbers.

## 9. What is next

Snapping and ripple edits build on the same mutators — and because
all edits now flow through the recorder, ripple deletion can become
one composite record (delete several, shift several) without touching
the stack machinery. The deferred transport commands (shuttle/
multi-speed, marks, looping) remain independent of history. The
snapshot-record escape hatch is the prepared seam for effects and
transitions, whose state will not invert as four integers.

## 10. Five review questions (with answers)

1. **Why does a typed rejection at record time matter more than a
   nice error message?** Because the stack must only contain edits
   that happened. If recorders recorded "attempted" edits, undo
   would replay fictions — the rejection test pins that the stack is
   byte-stable across OVERLAP, BAD_RANGE, and bad-index attempts.
2. **Why apply inverses through the mutators instead of writing the
   reverse operation directly into the model?** One validation path,
   one observer path, one code path the tests already cover. Writing
   inverses directly would fork the model's rules: an inverse that
   skips OVERLAP checking could corrupt the track on apply-time
   rejection robustness scenarios — exactly what the direct-poke
   test exercises.
3. **Why is undo of a *move* stored as bounds rather than index
   arithmetic?** Because position and index can diverge (inserts
   reorder). Old/new µs values are the user-meaningful facts and are
   index-free; the model's position-ordered insert makes index
   recovery deterministic from them.
4. **Why does undo pause playback instead of applying live?** The
   session plays a snapshot; a live mutation during play means the
   end-of-sequence line and audio queue reference a stale project
   for up to a frame. Pause-then-apply costs one transport pause and
   guarantees the next play re-copies the mutated model.
5. **Why does the changed seam exist when the window could just
   update the buttons after each call it makes?** Because the stack
   is also mutated by clear (project replace) and by internal
   rejection paths — every transition must move enablement, not just
   the ones the window initiated. The seam makes "history state
   changed" a single observable fact with exactly one driver.
