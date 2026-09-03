# Phase 2 — the guided walkthrough

Phase 2 gives the shell a body: media goes in, the bin shows it, and the
first real facts about the files (duration, codecs, frame rate, a
thumbnail, a waveform) become inspectable — all decoded by FFmpeg in a
worker thread, never on the UI thread, never inside a GTK file.

## 1. Phase purpose

Phase 2 delivers four user-visible things and three structural ones:

1. **Import** — File ▸ Import Media… (Ctrl+I) opens the GTK file
   chooser in multi-select mode; dropping a file list on the bin takes
   the same path. Unsupported files are rejected with a status-bar
   message and never enter the bin; files that vanish between pick and
   probe stay in the bin as MISSING with a Relink button.
2. **The media bin** — one row per session asset: thumbnail, name,
   kind+duration line, status badge, and a Relink button on broken rows.
   Selecting a row fills the inspector with the full probed record.
3. **The inspector** — name, status, kind, duration, dimensions, frame
   rate, sample rate, channels, container, codecs, and path, laid out as
   key/value rows. Empty state preserved when nothing is selected.
4. **Status feedback** — "Imported N file(s)", per-file failures, and a
   canceled chooser that stays silent, all through the Phase 1 reporter
   seam.

The structural things: a **GTK-free media layer** (probe + decode jobs),
a **GTK-free application layer** (cache, asset library, import worker),
and a **threading model** with exactly one decode thread whose results
land on the main context.

## 2. Per-file explanations

| File | What it is |
|---|---|
| `src/media/oe_probe.[ch]` | FFmpeg metadata probe: opens the container, classifies the file (video / audio / still image), and fills an `OeProbeInfo` — container name, integer-microsecond duration, width/height, rational frame rate (num/den), sample rate, channels, codec names. Errors use a private `OE_PROBE_ERROR` domain with two codes: `OPEN_FAILED` (unopenable) and `UNSUPPORTED` (opens, but no decodable A/V stream). |
| `src/media/oe_media_jobs.[ch]` | The two decode jobs. The thumbnail job seeks to 10% of the duration capped at 3 s (first frame as fallback), converts through swscale into a 96×96 box that preserves aspect, and returns raw RGBA bytes. The waveform job resamples the first audio channel to mono through swresample and compresses it into fixed min/max peak pairs. Both take a cancellation callback and return plain owned buffers — no GTK type crosses this layer. |
| `src/app/oe_media_cache.[ch]` | Raw-binary cache under `$XDG_CACHE_HOME/obvious-edit/media/`, keyed by the SHA-256 of canonical path + size + mtime. Entries are framed with a magic header and length so truncated or hand-corrupted files are treated as misses. Writes are temp-file-then-rename. No eviction policy; the directory is safe to delete wholesale. An env override (`OE_MEDIA_CACHE_DIR`) keeps tests out of the real cache. |
| `src/app/oe_media_library.[ch]` | The session asset store: opaque `guint` ids, one record per import (path, display name, status, deep-copied probe metadata, owned thumbnail bytes), GTK-free observer callback, and statuses IMPORTING / OK / MISSING / UNSUPPORTED. Every OK asset gets a `GFileMonitor`; an external deletion flips it to MISSING. `relink()` re-points the record and returns it to IMPORTING for re-probe. |
| `src/app/oe_import_worker.[ch]` | One `GThread` plus a `GAsyncQueue` of immutable, refcounted jobs. The worker checks the cache before running FFmpeg, checks an atomic cancel flag between decode steps, and marshals results back with `g_main_context_invoke` so the callback always runs on the main thread. Shutdown drains and joins the thread BEFORE `oe_ffmpeg_shutdown` in the app's reverse-order teardown. |
| `src/ui/oe_media_bin.[ch]` | The bin panel: `GtkListBox` rows rebuilt from the library on every refresh (the bin is a projection, never a second copy of the state), `GdkMemoryTexture` thumbnails built from the library's raw RGBA, status badges, per-row Relink buttons on MISSING/UNSUPPORTED rows, a labeled empty state, and a `GtkDropTarget` accepting `GDK_TYPE_FILE_LIST` with `GDK_ACTION_COPY`. |
| `src/ui/oe_main_window.c` (Phase 2 parts) | The composition root: owns the library and worker, installs the `media.import` handler through the same static-seam pattern as the reporter (dispatch passes no user_data), routes chooser + drop paths through one import entry point, turns worker verdicts into library transitions and status-bar messages, and populates the inspector from the selected record. |
| `tests/fixture_media.[ch]` | Runtime fixture generator: no media files are committed. Tests generate a WAV (pcm_s16le, known rate/channels/duration), an MJPEG-in-AVI (known dimensions/frame rate), and a PNG still into a `g_dir_make_tmp` directory using the same FFmpeg libraries the app links, plus a garbage text file and an empty file for the error paths. Compiled into test executables only. |
| `tests/test_probe.c` | Exact metadata assertions (tolerance only on duration), and the error contract: missing file → `OPEN_FAILED`, text/empty file → `UNSUPPORTED`. |
| `tests/test_media_jobs.c` | Box-fit thumbnail math, non-empty waveform peaks, and the cache miss → run → hit sequence with no second decode. |
| `tests/test_media_library.c` | Record + observer contract, and a real `GFileMonitor` round trip: create in tmp, import, delete the file behind the monitor, pump the main context, assert MISSING, relink, assert OK. |
| `tests/test_import_worker.c` | Submits from a `GMainLoop` and asserts completion arrives on the main context, plus cache and missing-file behavior. |
| `tests/test_commands.c` (pin change) | The `media.import` pin moves from NULL to `<Control>i` — a deliberate change, see §8. |

## 3. Block-by-block build walkthrough

The import of one video file, end to end:

```
Ctrl+I or drop onto bin
  └─ oe_main_window: media_import_command_handler / bin_import_sink
       └─ import_paths(): for each path
            ├─ oe_media_library_add(path)  → record IMPORTING, observer fires,
            │                                bin shows the row with a badge
            └─ oe_import_worker_submit(path, id, relink=FALSE)
                 (worker thread) oe_import_worker_run_job:
                   ├─ cache hit?  → done, no FFmpeg
                   ├─ oe_probe_file()   → OeProbeInfo (or verdict)
                   ├─ cancel check
                   ├─ oe_media_job_thumbnail() → RGBA bytes
                   ├─ cancel check
                   ├─ oe_media_job_waveform()  → peaks
                   └─ store in cache; result queued
                       └─ g_main_context_invoke → main thread
  main thread: on_import_done(result)
       ├─ OK          → library mark_ok(probe) + set_thumbnail → observer
       ├─ MISSING     → mark_missing; status "Missing: 'name'"
       ├─ UNSUPPORTED → fresh import: removed, "Unsupported: 'name'"
       │                relink attempt: row stays UNSUPPORTED
       └─ batch empty → "Imported N file(s)"
```

Two details worth pausing on. The **cancel flag is atomic and checked
between decode steps**, so a shutdown during a long waveform decode
stops at the next boundary instead of mid-buffer. And **the worker never
allocates the result the main thread will free twice**: jobs are
refcounted, results own their bytes, and ownership moves once at the
callback.

## 4. C concepts in play

- **`g_once` for global init.** A second thread may now call FFmpeg, so
  `oe_ffmpeg_init` became a `GOnce`-guarded init (the case the Phase 0
  adapter's comment predicted). Still idempotent; still paired with
  `oe_ffmpeg_shutdown`.
- **`GAsyncQueue` + refcounted jobs.** The queue transfers ownership of
  one reference; the worker drops its reference when done. No lock is
  visible above the queue.
- **`g_main_context_invoke`.** The safe way to hop threads: if already
  on the main context it runs inline, otherwise it schedules — either
  way the callback runs exactly once.
- **Rational time.** Duration is `gint64` microseconds; frame rate is
  `num`/`den` integers. No `double` seconds anywhere — the project-format
  time-model floor, applied from the first line that touches metadata.
- **The GError pattern for classification.** `OE_PROBE_ERROR` has exactly
  two codes because the callers branch on exactly two cases. Adding a
  third without a third branch to make is how error domains rot.
- **Borrowed vs. owned, again.** `gdk_file_list_get_files` returns a
  borrowed `GSList` of borrowed `GFile`s — copy the paths out, free
  nothing. `GdkMemoryTexture` keeps its own reference to the `GBytes`
  you hand it — release yours immediately.

## 5. Ownership table

| Object | Created | Owned / freed by | Notes |
|---|---|---|---|
| `OeProbeInfo` | probe / library | record or caller via `oe_probe_info_clear` | deep-copy helper `oe_probe_info_copy` for hand-offs |
| Thumbnail RGBA | thumbnail job / cache | library record (setter copies) → freed in `asset_free` | plain `g_free` buffer, stride = width×4 |
| Waveform peaks | waveform job / cache | worker result → freed after cache store | fixed bucket count |
| Cache entry | worker | cache file (no in-memory table) | delete the dir = cold cache |
| Job | `submit` | `GAsyncQueue` → worker → freed after dispatch | refcounted, immutable after submit |
| `OeMediaLibrary` | window `constructed` | window `dispose` | observer wired to bin refresh |
| `OeImportWorker` | window `constructed` | window `dispose`, BEFORE `oe_ffmpeg_shutdown` | free drains + joins |
| `OeMediaBin` | window | GTK child of the window | import func pointer cleared first |
| `GtkFileDialog` | chooser open | freed after `open_multiple` starts | async op holds its own ref |

## 6. Call flow

**Startup:** unchanged Phase 1 order (`oe_ffmpeg_init` now
GOnce-guarded). The window's `constructed` creates library → bin →
worker, wires the observer (library change → `oe_media_bin_refresh`),
installs the `media.import` handler and the bin's import sink.

**Import:** see §3.

**External deletion (OK asset):** `GFileMonitor` "changed" → library
marks MISSING → observer → bin refresh → row badge flips, Relink button
appears. The inspector updates on next selection change.

**Relink:** bin button → `relink-requested(asset_id)` → window opens a
single-file chooser → `oe_media_library_relink(path)` (record back to
IMPORTING, monitor stopped) → `worker_submit(..., relink=TRUE)` →
verdict updates the record. A relink that lands on another unsupported
file leaves the row in place as UNSUPPORTED — you can always try again.

**Shutdown:** application reverse order. The window's `dispose` clears
both seams first (no late dispatch can touch a dying widget), then frees
the worker — which drains the queue, joins the thread, and flushes
pending results onto the still-alive main context — then the library.

## 7. Alternatives considered

- **GLib `GTask` per import** instead of one worker thread. Rejected:
  the spec pins one decode thread; a task pool would re-serialize the
  threading review every time someone adds concurrency, and the queue
  gives us natural backpressure and drain semantics.
- **GDK-Pixbuf for thumbnails.** Rejected: it is a second decoder with
  its own security surface, and it cannot seek into an AVI. The media
  layer already has FFmpeg; one decode path stays one path.
- **Storing PNG/WebP in the cache.** Rejected: re-encoding adds a
  failure mode and loses exactness. The cache stores the same raw RGBA
  the job produced — WYSIWYG between cache hit and fresh decode.
- **Observable model with per-row widget updates** (diff the library
  into the bin). Rejected for session scale: rebuild-from-records is
  O(n) at sizes where n is small, and it cannot drift. The projection
  comment in `oe_media_bin_refresh` is the design decision.
- **DnD via `GtkDropTargetAsync`.** The typed `GtkDropTarget` with
  `GDK_TYPE_FILE_LIST` is sufficient and keeps the drop handler
  synchronous and simple.

## 8. Bug log

- **Nested comment broke the build.** An `edit-file` splice duplicated
  the `oe_main_window.c` header comment; GCC's `-Wcomment` (under
  `-Werror`) caught `/*` within comment at line 2. Lesson: after
  line-splice edits, compile before reading further — the compiler is
  the cheapest reviewer.
- **`gtk_drop_target_new` takes a `GType`, not formats.** First draft
  passed `gdk_content_formats_new_for_gtype(...)`. GTK 4 offers two APIs;
  the typed constructor wants `GDK_TYPE_FILE_LIST` directly.
- **Wrong include path.** `oe_media_bin.c` included `"oe_log.h"` — the
  log module lives in `src/app/`, so the header search failed under
  `-Werror`. Relative includes follow the directory, not the target.
- **A forward declaration landed inside a function.** Another splice
  placed `populate_inspector`'s declaration between
  `set_status_message`'s signature and body. Reverted to a clean block
  above the definition. Lesson: when a tool applies edits by line
  number, re-read the seam before building.
- **Deliberate pin change, not a bug:** `test_commands.c` pinned
  `media.import` to a NULL accelerator when the command was
  menu-only-reserved. Phase 2 promotes import to the keyboard spine
  (Ctrl+I), so the pin moves in the same PR — flagged here and in the
  PR body so no one mistakes it for drift.

## 9. What is next

Phase 3 inherits: `OeProbeInfo` as the metadata currency for the source
monitor; the waveform peaks for audio track rendering; the cache for
anything expensive (waveform redraws, proxy generation); the library's
ids for clips that reference assets; and the worker pattern (queue +
main-context dispatch) for any future background work. The bin's
selection signal is where a "load into source monitor" action will
attach.

## 10. Five review questions (with answers)

**Q1. Why does the probe reject a text file with UNSUPPORTED instead of
OPEN_FAILED?**
Because FFmpeg opens text files fine — `avformat_open_input` succeeds;
classification then finds no decodable audio or video stream. The error
codes describe *where* the pipeline stopped, not how ugly the file is.
Missing/unreadable path → `OPEN_FAILED`; opened but nothing to play →
`UNSUPPORTED`.

**Q2. Why is the cache key content-agnostic (path+size+mtime) rather
than a content hash?**
Hashing file content on every import would read and checksum every
byte — the exact cost the cache exists to avoid. Stat metadata is
free, and for session-scale editing it is precise enough; a genuinely
edited file changes size or mtime. The trade-off (an in-place edit that
preserves both) is accepted and documented.

**Q3. Why must the worker be freed before `oe_ffmpeg_shutdown`?**
The worker's thread calls FFmpeg. Freeing the adapter first would leave
the join racing a teardown of FFmpeg's global state. The window's
`dispose` frees the worker first; the application then shuts FFmpeg
down in its reverse-order pass.

**Q4. How does a dropped file reach the same code as the chooser?**
The bin's drop handler copies the borrowed `GSList` of `GFile`s into a
NULL-terminated array of path strings and calls the window's import
sink — the same `import_paths()` the chooser's finish callback uses.
There is exactly one import entry point; the two entry gestures are
thin adapters.

**Q5. Why does a relinked-unsupported asset stay in the bin while a
freshly imported unsupported file is removed?**
A fresh import of junk was never wanted — remove it and say so. A
relink attempt was a user decision about a record they chose to keep;
deleting their record on failure would throw away the context they were
fixing. The row stays UNSUPPORTED with its Relink button.
