# Phase 0 — the guided walkthrough

This document teaches a new contributor everything Phase 0 set up: why it
exists, what every file does, how the build works line by line, the C
idioms in play, and the mistakes already made so you do not repeat them.

## 1. Phase purpose

You cannot build a video editor on a foundation you cannot trust. Phase 0
deliberately ships **no editing features**. It ships proof that the
hardest-to-debug parts of the project work and stay working:

- the build compiles clean with warnings as errors against pinned
  dependency versions,
- the two external libraries (FFmpeg, SDL3) initialize and shut down
  symmetrically under automated tests, sanitizers, and Valgrind,
- a GTK window really maps on screen (proven headlessly under Xvfb),
- all of it is repeatable with one command each.

Every later phase builds features on this frame. If Phase 0 is boring,
it worked.

## 2. Per-file explanations

| File | Why it exists |
|---|---|
| `meson.build` | Declares the project, C standard, warning policy, all 11 dependency floors, both targets, and the test wiring. |
| `meson_options.txt` | Present but empty: makes adding the first real option a one-line change. |
| `.clang-format` | One committed C style. `clang-format --dry-run --Werror` in CI keeps formatting out of code review. |
| `src/main.c` | Entry point. Initializes logging, runs the application, converts a failed startup into exit code 1. |
| `src/app/oe_application.[ch]` | The GtkApplication. Owns startup/shutdown ordering and the --self-check option. |
| `src/app/oe_log.[ch]` | The one logging door. Structured records in the `oe` domain, `OE_LOG_LEVEL` override. |
| `src/media/oe_ffmpeg.[ch]` | FFmpeg lifecycle adapter: GError pattern, idempotent init/shutdown, logs linked library versions. |
| `src/playback/oe_audio_output.[ch]` | SDL3 audio adapter: same contract as oe_ffmpeg, different library. |
| `src/ui/oe_main_window.[ch]` | The main window: titled "Obvious Edit", 1280x720, no children yet. |
| `tests/test_lifecycle.c` | Three GLib tests: ffmpeg lifecycle, SDL lifecycle, log level override. |
| `tests/valgrind.supp` | Valgrind suppressions, GLib/GObject internals only, with a scope policy comment. |
| `scripts/run-headless.sh` | The repeatable headless self-check: dbus-run-session + xvfb-run + pinned env. |
| `docs/*` | Architecture, reserved project format, code map, glossary, and this walkthrough. |
| `README.md` | The single apt-get install line plus the exact pinned dpkg and pkg-config versions of the reference environment. |

## 3. Block-by-block build walkthrough

`meson.build`, top to bottom:

```meson
project('obvious-edit', 'c',
  version: '0.1.0',
  license: 'MIT',
  meson_version: '>= 1.3.0',
  default_options: [
    'c_std=c17',
    'warning_level=2',
    'werror=true',
  ])
```
The project declaration pins the language (C), the standard (C17), and
the warning policy. `werror=true` means a warning fails the build — the
gate that caught two real bugs during Phase 0 (see the bug log).

```meson
gtk4_dep = dependency('gtk4', version: '>= 4.10')
...
sdl3_dep = dependency('sdl3', version: '>= 3.2')
```
Eleven dependencies, each with an explicit floor. Meson queries
pkg-config and fails at configure time if anything is older. The floors
are policy, not decoration: gtk4 >= 4.10 (so we can rely on modern GTK
APIs), glib/gobject >= 2.74 (structured logging maturity), the FFmpeg
major-version floors (60/60/58/9/7/4 — FFmpeg bumps sonames aggressively
and code written for one major breaks on the next), sdl3 >= 3.2 (first
stable SDL3).

```meson
oe_log_cargs = ['-DG_LOG_DOMAIN="oe"']
```
A compile-time definition shared by every target, giving all log records
one domain. GLib skips structured logging silently when the domain is
NULL — this define is what makes the whole logging design work.

```meson
executable('obvious-edit', [...sources...], c_args: oe_log_cargs, dependencies: oe_deps)
```
The application binary: six C files, linked against all eleven deps.

```meson
test_lifecycle = executable('test_lifecycle', [tests + adapter sources], dependencies: test_deps)
test('lifecycle', test_lifecycle, env: ['SDL_AUDIODRIVER=dummy'])
```
The test binary reuses the logging and adapter sources directly (no
library yet — there is nothing to link against that the app does not
also need). The test runs with SDL's dummy audio driver so it never
depends on host audio hardware: hermetic tests, and no third-party
audio library leak noise under sanitizer/valgrind gates.

## 4. C concepts in play

- **GError pattern.** Fallible functions end in `GError **error`; the
  caller owns the error and frees it with `g_clear_error()`. A callee
  must never touch `*error` on success — the tests assert exactly that.
- **Idempotent lifecycle pairs.** `oe_ffmpeg_init` is safe to call
  twice, `oe_ffmpeg_shutdown` is safe to call twice, shutdown before
  init is safe. The state flag is set only after the underlying call
  succeeds, and cleared only when teardown actually ran.
- **Vfunc overriding.** OeApplication overrides four `GApplicationClass`
  vfuncs. The chain-up pattern
  (`G_APPLICATION_CLASS (oe_application_parent_class)->startup (application)`)
  calls the parent implementation first/last — required for GtkApplication
  to work.
- **Opaque final types.** `G_DECLARE_FINAL_TYPE` + a private-by-convention
  `struct _Oe…` in the .c file. Users get a typed pointer; the layout
  stays an implementation detail.
- **Structured logging.** `g_log_structured_array()` with explicit
  fields (PRIORITY, GLIB_DOMAIN, MESSAGE). No printf anywhere: a stray
  printf is invisible in production logs; a structured record is
  filterable.
- **One-shot signal connection.** The self-check connects `map` before
  presenting the window so the very first map cannot be missed, and
  `g_application_quit()` unwinds the main loop from inside the handler.

## 5. Ownership table

| Resource | Created by | Owned by | Released by |
|---|---|---|---|
| OeApplication instance | `oe_application_new()` | `main()` | `g_object_unref` before exit |
| GTK default main loop | `g_application_run()` | GApplication | returned when run() returns |
| Main window | `oe_main_window_new()` | GtkApplication (window property) | GTK destroy cycle on quit |
| "map" signal handler | `g_signal_connect` in activate | OeMainWindow | window destruction |
| FFmpeg global state | `oe_ffmpeg_init` | oe_ffmpeg module | `oe_ffmpeg_shutdown` |
| SDL audio subsystem | `oe_audio_output_init` | oe_audio_output module | `oe_audio_output_shutdown` |
| GError from adapters | callee (on failure) | caller | `g_clear_error` |
| Log writer state | `oe_log_init` | oe_log module | process exit |
| GString test capture | `main` in test_lifecycle | the test binary | process exit |

## 6. Call flow

Normal run:

```
main
 ├─ oe_log_init                 reads OE_LOG_LEVEL, installs writer
 ├─ oe_application_new          GType registration, option registration
 ├─ g_application_run
 │   ├─ startup vfunc
 │   │   ├─ chain up to GtkApplication.startup
 │   │   ├─ oe_ffmpeg_init        logs linked FFmpeg versions
 │   │   └─ oe_audio_output_init  logs SDL version
 │   ├─ activate vfunc
 │   │   ├─ oe_main_window_new
 │   │   └─ gtk_window_present
 │   └─ ... main loop runs ...
 │   └─ shutdown vfunc
 │       ├─ oe_audio_output_shutdown   (reverse order)
 │       ├─ oe_ffmpeg_shutdown
 │       └─ chain up to GtkApplication.shutdown
 └─ exit 0 (or 1 if startup failed)
```

Self-check run: identical, except activate also connects the one-shot
map handler; the first map calls `g_application_quit()`, which runs
shutdown and returns from `g_application_run()` with exit code 0.

## 7. Alternatives considered

- **Manual init in main.c instead of application vfuncs.** Rejected:
  vfuncs keep startup/shutdown paired with the framework's own
  lifecycle, so --self-check and interactive runs share one code path.
- **A static library target for adapters.** Rejected for Phase 0: two
  consumers (app, tests) compiling the same three adapter sources is
  simpler than a library until there is a third consumer or link-time
  secrecy needs.
- **printf/g_print logging.** Rejected: unstructured, unfilterable, and
  invisible to log aggregation. The structured writer cost one extra
  module and buys queryable logs forever.
- **Suppressing the third-party audio leak in valgrind.supp.** Rejected:
  the file's scope policy is GLib/GObject internals only. Instead, tests
  run with `SDL_AUDIODRIVER=dummy`, which removes the leak path AND
  makes tests hermetic. Fixing the environment beats widening the
  suppression allowlist.
- **xvfb-run without dbus-run-session.** Rejected: GtkApplication needs
  a session bus; without one the self-check hangs or fails depending on
  the machine.

## 8. Bug log

Mistakes made and fixed during Phase 0, recorded so they stay fixed:

1. **Wrong vfunc signature.** `oe_application_activate` was declared
   `void (*)(GtkApplication *)`, but `activate` on `GApplicationClass`
   is `void (*)(GApplication *)`. `-Werror` caught the incompatible
   pointer assignment at build time. Fix: take `GApplication *` and
   cast to `GtkApplication *` where the GTK-specific API needs it.
2. **Valgrind suppression matched nothing.** The first `obj:` patterns
   were `obj:libgobject-2.0.so*`; Valgrind matches object paths, so the
   pattern needed the leading wildcard: `obj:*/libgobject-2.0.so*`.
   Symptom: `suppressed: 0 bytes` despite the file parsing fine.
3. **Third-party leak polluted sanitizer runs.** A 159-byte leak inside
   libasound (`snd_device_name_hint`) reached through SDL3 audio probing
   failed the ASan gate. It is not our memory. Root-cause fix: the test
   uses `SDL_AUDIODRIVER=dummy` (hermetic + no ALSA probe), and the
   suppression file stays within its GLib/GObject scope policy.
4. **Edit-tool line-range damage.** A line-range edit to
   oe_application.c clipped a closing brace and duplicated two lines.
   Symptom: clang-format check failed at the mangled brace. Fix:
   re-read, restore the brace, drop the duplicate, reformat.
5. **Duplicate test declaration.** Adding the env-aware `test()` call
   next to the old declaration produced two identical test names;
   meson would reject it at configure. Fix: keep exactly one
   declaration, with `env: ['SDL_AUDIODRIVER=dummy']`.

## 9. What is next

After Phase 0, later phases add, in rough order: media import and probe
(oe_ffmpeg grows real API), the project data model (json-glib starts
earning its dependency), a timeline widget with clips and trims, the
playback clock and audio output wiring, undo/redo commands, snapping and
ripple edits, then export. The seams built here — adapters, logging
domain, window, and the headless recipe — are the extension points each
phase plugs into.

## 10. Five review questions (with answers)

**Q1. Why does shutdown run the adapters in reverse order of startup?**
A: Because teardown dependencies mirror setup dependencies. Audio
output (SDL) may still reference resources FFmpeg or process state
established after ffmpeg init; unwinding in reverse guarantees nothing
is torn down while something started later still depends on it. It is
the same discipline as stack unwinding in C++ and makes the shutdown
order predictable without bookkeeping.

**Q2. The second `oe_ffmpeg_init` call returns TRUE without doing work.
Why return TRUE instead of FALSE or an error?**
A: The adapter's contract is "the library is ready after this call."
Idempotency means calling init twice leaves the system in the ready
state — which is exactly success. Returning FALSE would force every
caller to distinguish "not initialized" from "already initialized,"
duplicating the adapter's own bookkeeping in callers.

**Q3. Why is the `map` handler connected before `gtk_window_present`,
and why does that matter?**
A: `gtk_window_present` can map the window synchronously (especially
under X11). If the handler were connected after present, the first map
could fire before the handler existed, the self-check would never quit,
and the run would hang until timeout. Connecting first means the
one-shot quit cannot miss the transition.

**Q4. What would break first if someone added a printf to a media
adapter, and why is that a real problem?**
A: Nothing would break at compile time — which is the problem. printf
bypasses the structured pipeline: no domain, no priority, no level
filtering, invisible to OE_LOG_LEVEL and to log aggregation. In a
decoder that can emit thousands of lines per second, that is both a
performance hazard and a data leak into stdout. The no-printf rule is
checked by review; grep is a good first pass in code review.

**Q5. The lifecycle test sets SDL_AUDIODRIVER=dummy. Doesn't that make
the SDL test meaningless?**
A: No — the test's contract is the ADAPTER, not the driver: init is
idempotent, shutdown is paired, state flags are truthful, and SDL
reports its version. Those are our code's guarantees. Which backend
SDL uses to open audio hardware is SDL's domain, and depending on a
real device would make the test fail on machines without one (CI
included). Hermetic beats thorough-looking.
