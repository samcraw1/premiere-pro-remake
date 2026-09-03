# Phase 1 — the guided walkthrough

Phase 1 hangs the editor's body on the Phase 0 frame: a real editor shell
with panels, an original dark theme, keyboard command routing, and
workspace layout that survives a restart. Everything still "does nothing"
— deliberately. The seams this phase builds are where later phases plug
in.

## 1. Phase purpose

Phase 1 delivers four user-visible things and one invisible thing:

1. **The shell** — menu bar, toolbar, media bin, source monitor, program
   monitor, inspector, a labeled timeline area with transport controls,
   and a status bar. Every panel shows an empty state that names the
   phase which will fill it, never a blank canvas.
2. **An original dark theme** — a stylesheet compiled into the binary as
   a GResource and applied through `GtkCssProvider`. No libadwaita; no
   colors or assets derived from any commercial NLE.
3. **Command routing** — a GTK-free registry of named commands with
   stable IDs, accelerators, enablement, and a reporter seam. GTK talks
   to it through plain `GAction`s; the registry never includes gtk.h.
4. **Layout persistence** — window size and splitter positions saved to
   `$XDG_CONFIG_HOME/obvious-edit/layout.conf` on close, restored on
   launch, with defensive fallbacks for missing, corrupt, or
   newer-version files.

The invisible thing is the **seam discipline**: the registry and the
layout module are both GTK-free and unit-tested GTK-free, so the command
table and the persistence logic can be reasoned about — and tested —
without a display.

## 2. Per-file explanations

| File | What it is |
|---|---|
| `src/app/oe_command.[ch]` | The GTK-free command registry: an `OeCommandId` enum (the stable ABI for later phases), one table of 16 entries with dotted names and accelerator strings, per-command handler/enablement registration, and a reporter callback pointer. Dispatch is total: implemented-with-handler, not-yet-implemented, unknown ID, and disabled all terminate safely with a log line and a reporter call. |
| `src/ui/oe_theme.[ch]` | Loads the compiled-in stylesheet once (idempotent) and applies it to the default display via `GtkCssProvider`. Reads the CSS from a GResource so the binary can never come up unstyled from a mis-installed data file. |
| `src/ui/obvious-edit.css` | The theme itself: original neutral dark palette, light-on-dark text, visible focus outlines, panel title / empty-state / toolbar / status styling. |
| `src/ui/obvious-edit.gresource.xml` | Declares the CSS as a resource compiled into the binary by `gnome.compile_resources`. |
| `src/ui/oe_shell_layout.[ch]` | GTK-free persistence: an `OeShellLayout` struct (window size, maximized flag, three splitter positions), `oe_shell_layout_defaults()`, `save()` (GKeyFile, version group starting at 1, atomic temp-file + rename), and `load()` (missing file → defaults; newer version → defaults + warning; corrupt fields → clamp to sane minimums). Reads `XDG_CONFIG_HOME` directly because GLib caches `g_get_user_config_dir()` per process. |
| `src/ui/oe_main_window.[ch]` | The shell itself: menu bar and toolbar built from `app.<command>` actions, seven labeled panels nested in three `GtkPaned`s (zero absolute geometry), a status label, and the reporter that turns dispatch feedback into status-bar text. Applies loaded splitter positions on first map; saves layout on close-request. |
| `src/app/oe_application.c` | Grew two responsibilities: installs one `GSimpleAction` per command (`app.<dotted-name>`) plus its accelerator mapping, and initializes the theme before the first window is presented. |
| `tests/test_commands.c` | GTK-free registry tests: table integrity (unique IDs, dotted names parse, accelerator grammar valid), not-implemented dispatch, unknown command, disabled command, registered-handler dispatch, reporter observation. |
| `tests/test_shell_layout.c` | GTK-free persistence tests: documented defaults, save/load round trip, missing file, corrupt bytes, newer version, out-of-range clamping, partial fields. |

## 3. Block-by-block build walkthrough

The shell is composed outside-in, one nesting level at a time:

```
OeMainWindow (GtkApplicationWindow)
 ├─ root GtkBox (vertical)
 │   ├─ menu bar   — GtkPopoverMenuBar from a GMenu of app.<command> actions
 │   ├─ toolbar    — GtkBox of GtkButtons bound to the same actions
 │   ├─ bin_paned  (horizontal GtkPaned)
 │   │   ├─ start:  Media Bin panel
 │   │   └─ end:    timeline_paned (vertical GtkPaned)
 │   │       ├─ start: inspector_paned (horizontal GtkPaned)
 │   │       │   ├─ start: monitors GtkBox (homogeneous)
 │   │       │   │   ├─ Source Monitor panel
 │   │       │   │   └─ Program Monitor panel
 │   │       │   └─ end: Inspector panel
 │   │       └─ end: Timeline panel + transport row
 │   └─ status bar — GtkBox with a GtkLabel (command feedback lands here)
```

Every panel is produced by one helper, `panel_new(title, empty_text)` —
a vertical box with a title label and a centered, wrapping empty-state
label. `shrink` is disabled on every `GtkPaned`, so a splitter can never
drag a child to zero width.

Keyboard routing has three hops: a keypress activates `app.<name>`
(GTK's accelerator map resolves it), `on_command_action` converts the
action back to its `OeCommandId`, and `oe_command_dispatch()` runs the
registry path (enablement check → handler or not-implemented → report →
log). The registry's reporter writes to the status bar; the registry
never knows the status bar exists.

Layout persistence has two directions. On construction the window loads
the struct, sets the default window size, and stashes splitter values;
a `map` handler applies them once the window has a real allocation. On
`close-request` the window reads its live allocation and splitter
positions back into the struct and saves. The widget layer only moves
integers in and out of the struct — it never touches GKeyFile.

## 4. C concepts in play

**Function-pointer callback tables with context.** The reporter and
handler slots are plain function pointers plus a `gpointer user_data`.
GTK-style C uses this to invert dependencies: the registry calls out
without including anything about the caller.

**Enums as ABI.** `OeCommandId` values are the permanent identity of
commands (the dotted names are permanent too); the enum keeps dispatch
O(1) and switch-able while the name strings keep logs and menus
readable.

**Out-parameters and two-step init.** `oe_command_dispatch()` mirrors
the framework style: enum in, boolean-ish result out, details via the
reporter. `oe_shell_layout_load()` fills a caller-owned struct so tests
can inspect it directly — no hidden allocation.

**GResource embedding.** Compile-time resources turn "missing data
file" from a runtime failure into an impossibility. `gnome.compile_resources`
generates a C file that registers the stylesheet in a global
filesystem namespace read with `g_resources_lookup_data`.

**Idempotent global init.** `oe_theme_init()` guards its work so calling
it twice is harmless — the same discipline the Phase 0 adapters use,
applied to display-wide state.

**Deprecation survival.** GTK 4.18 deprecates `GtkStatusBar` and removed
`gtk_window_get_size()` years ago. The shell reads live allocation via
`gtk_widget_get_width/height()` and renders status text in a plain
styled label. When a toolkit moves, adapt instead of pinning the past.

## 5. Ownership table

| Symbol | Owner | Created | Read | Lifetime |
|---|---|---|---|---|
| `OeCommandEntry` table | oe_command.c | at build | dispatch, tests | static |
| Handler / enablement slots | registry | `oe_command_register_*` | dispatch | process |
| Reporter pointer | registry | `oe_command_set_reporter` | dispatch | process; cleared in window dispose |
| `OeShellLayout` struct | caller | defaults / load | save, window | stack — no globals |
| layout.conf | GKeyFile | save (atomic) | load | process → disk |
| Status label | OeMainWindow | constructed | reporter | window lifetime |
| GResource CSS | binary | compile time | theme init | static |

The one cross-module pointer is the reporter; the window clears it in
`dispose()` so a dispatch can never reach a freed widget.

## 6. Call flow

**Keypress → status bar:**

```
 GDK key event
   └─ GtkWindow accelerator map ("Space" → app.transport.play-pause)
        └─ GSimpleAction activate → on_command_action (id)
             └─ oe_command_dispatch(OE_CMD_TRANSPORT_PLAY_PAUSE)
                  ├─ enabled? no  → report "<name> is disabled" + log, return
                  ├─ handler set? yes → handler(id) → report via reporter
                  └─ handler NULL → report "'<name>' not implemented yet"
                                     + OE_LOG_LEVEL_INFO line
```

**Save → restore round trip:**

```
 close-request → on_close_request
   ├─ read window width/height (widget allocation), maximized flag,
   │    three GtkPaned positions into OeShellLayout
   └─ oe_shell_layout_save → GKeyFile → mkdir -p config dir →
        write layout.conf.tmp → rename over layout.conf

 process start → oe_main_window_constructed
   └─ oe_shell_layout_load → struct (defaults | file | fallback)
        ├─ gtk_window_set_default_size / maximize
        └─ pending positions applied on first map
```

**Startup order (unchanged from Phase 0, extended):** FFmpeg → audio →
(on success) theme → command actions → activate → window.

## 7. Alternatives considered

- **GtkStatusBar for command feedback.** Rejected after discovery:
  deprecated in GTK 4.18 with `-Werror` in place. A plain labeled box
  gets identical behavior with zero deprecation debt.
- **gtk_window_get_size() for save-on-close.** Rejected by the
  compiler: the API was removed in GTK4. The live widget allocation
  (`gtk_widget_get_width/height`) is the supported replacement and is
  what close-time actually needs.
- **A per-command GSimpleAction subclass carrying behavior.** Rejected:
  the action layer must stay dumb plumbing. All policy (enablement,
  not-implemented, reporting) lives in the registry, so the registry
  stays GTK-free and testable headlessly.
- **libadwaita widgets for the "modern" look.** Rejected by the spec's
  locked decision: the theme must be original; libadwaita would both
  add a dependency (forbidden this phase) and blur authorship of the
  look.
- **GSettings for layout persistence.** Deferred: GSettings brings a
  schema toolchain and dconf coupling the spec did not ask for. A
  versioned GKeyFile keeps the failure modes (missing, corrupt, newer)
  explicit and unit-testable.
- **g_get_user_config_dir() for the layout path.** Rejected after
  finding GLib caches it per process — test isolation via
  XDG_CONFIG_HOME silently failed. Reading the env var directly makes
  every test hermetic.

## 8. Bug log

Mistakes made and fixed during Phase 1, recorded so they stay fixed:

1. **Missing `#include <glib.h>` in oe_theme.h.** `G_BEGIN_DECLS` is
   not self-contained; without glib.h the header failed to compile
   wherever it was included first. Symptom: `expected ';' before
   'void'` at the first prototype. Fix: include glib.h (and keep the
   header self-sufficient rule from Phase 0).
2. **GTK 4.18 deprecations under -Werror.** The first draft used
   `gtk_statusbar_*`; the toolchain flagged `GtkStatusBar` as
   deprecated and the cast macro did not exist under that spelling.
   Fix: replaced with a plain styled GtkLabel — see Alternatives.
3. **`gtk_window_get_size` does not exist in GTK4.** Caught as an
   implicit-declaration error. Fix: widget-allocation reads at close
   time, with a >0 guard so an unallocated window cannot write zeros
   into the layout.
4. **Invalid CSS property.** `.toolbar { spacing: 4px; }` is GTK3-era
   CSS; GTK4's parser rejected it (widget spacing is a GtkBox property,
   set in code). Symptom: `Theme parser error` in the headless log.
   Fix: removed the property; the toolbar keeps its 4px spacing from
   `gtk_box_new`.
5. **Scope bug in the command tests.** A handler callback referenced
   state declared inside one test function, so a different test's
   dispatch would read garbage. Fix: moved the capture state and
   callback to file scope, making the intent explicit.
6. **GLib caches the user-config path.** First layout-path code used
   `g_get_user_config_dir()`; the corrupt-file test could not be
   isolated via XDG_CONFIG_HOME because the value was cached at first
   use. Fix: read `XDG_CONFIG_HOME` directly with a `$HOME/.config`
   fallback; tests set the env var before any call.

## 9. What is next

Phase 2 makes the media bin real: import, probing, and thumbnails —
`oe_ffmpeg` stops being a lifecycle adapter and grows its first real
API. The command registry gets its first handlers (media.import),
the source monitor loads its first clip, and the empty states earned
this phase start disappearing one panel at a time. Phase 3 adds the
project/timeline data model; the timeline area stays a placeholder
until then by design.

## 10. Five review questions (with answers)

**Q1. Why is the command registry GTK-free, and what would break if it
included gtk.h?**
A: Two consumers already exist (the app and the unit tests), and the
tests must run headless. Including gtk.h would force every dispatch
test through a display and make the command table untestable in CI
sandboxes. The cost is one reporter indirection; the benefit is a
headless-guaranteed command layer.

**Q2. Why save layout in a GKeyFile instead of the project's JSON
format?**
A: The project format is versioned user data with its own schema and
migration story (Phase 3+). Layout is application state — window size
and splitter positions — with different validity rules (clamping, not
validation). Sharing a format would couple two change cadences.

**Q3. What happens on each of the four layout-file failure modes, and
why is each safe?**
A: Missing file → documented defaults (first run is a valid state).
Corrupt bytes → parse fails, defaults, warning log. Newer version →
defaults + warning (we cannot interpret future fields). Out-of-range
fields → clamped to sane minimums. The launch never fails; the worst
case is a default-sized window.

**Q4. Why does the window apply splitter positions on map instead of
at construction?**
A: `gtk_paned_set_position` before allocation is fought by the size
negotiation — the value can be overwritten or ignored. The map signal
fires when the window is actually visible, so one deferred assignment
sticks. The flag makes it once-only.

**Q5. What stops an unknown or disabled command from crashing?**
A: Dispatch is a total function over the enum plus a guard for IDs
outside the table: unknown → report + log; disabled → report + log;
implemented-but-handlerless → report + log. No path reaches a NULL
handler call. The not-implemented message is the phase's honest
contract: "recognized, logged, not built yet."
