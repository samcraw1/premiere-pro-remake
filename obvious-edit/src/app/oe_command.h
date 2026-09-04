/* oe_command.h — the editor's GTK-free command registry (Phase 1).
 *
 * Commands are the stable vocabulary of the application: the dotted names
 * and enum IDs chosen here are permanent API. Later phases build undo/redo
 * entries on this registry (after the playback clock, per the roadmap).
 * Configurable keymaps remain deferred to a later phase — the accelerator
 * column below stays the default, immutable map until then.
 *
 * The registry knows nothing about GTK. The application layer wires GTK
 * actions and default accelerators to oe_command_dispatch(); the UI layer
 * installs a reporter that writes status-bar feedback. Dispatch never
 * crashes on unknown input — it logs and reports instead.
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * OeCommandId:
 * @OE_CMD_PLAY_PAUSE: transport play/pause toggle (Space).
 * @OE_CMD_STOP: transport stop (K).
 * @OE_CMD_SHUTTLE_FORWARD: shuttle forward (L) — deferred: multi-speed
 *   and reverse playback arrive in a later phase; Phase 5 leaves the
 *   command registered but unwired.
 * @OE_CMD_SHUTTLE_BACK: shuttle back (J) — deferred: multi-speed and
 *   reverse playback arrive in a later phase; Phase 5 leaves the
 *   command registered but unwired.
 * @OE_CMD_MARK_IN: mark in point (I) — deferred: range playback between
 *   in/out points arrives in a later phase; Phase 5 leaves the command
 *   registered but unwired.
 * @OE_CMD_MARK_OUT: mark out point (O) — deferred: range playback
 *   between in/out points arrives in a later phase; Phase 5 leaves the
 *   command registered but unwired.
 * @OE_CMD_TOOL_SELECT: select tool (V).
 * @OE_CMD_TOOL_RAZOR: razor tool (C).
 * @OE_CMD_DELETE_SELECTION: delete the current selection (Delete).
 * @OE_CMD_UNDO: reserved — not implemented; undo/redo arrive after the
 *   playback clock (roadmap order).
 * @OE_CMD_REDO: reserved — not implemented; undo/redo arrive after the
 *   playback clock (roadmap order).
 * @OE_CMD_IMPORT_MEDIA: media import (wired in Phase 2).
 * @OE_CMD_NEW_PROJECT: project lifecycle (wired in Phase 3).
 * @OE_CMD_OPEN_PROJECT: project lifecycle (wired in Phase 3).
 * @OE_CMD_SAVE_PROJECT: project lifecycle (wired in Phase 3).
 * @OE_CMD_IMPORT_FROM_BIN: insert the selected bin asset at the
 *     playhead on the first kind-matching track (wired in Phase 4).
 * @OE_CMD_ZOOM_IN: timeline zoom in, around the widget center (wired
 *     in Phase 4; zoom is view session state).
 * @OE_CMD_ZOOM_OUT: timeline zoom out, around the widget center
 *     (wired in Phase 4; zoom is view session state).
 * @OE_CMD_SHOW_ABOUT: menu-only; about surface.
 * @OE_CMD_COUNT: sentinel — number of commands, not a valid ID.
 *
 * Enum order is permanent API: IDs must never be renumbered or removed.
 * Append new commands before @OE_CMD_COUNT.
 */
typedef enum
{
  OE_CMD_PLAY_PAUSE,
  OE_CMD_STOP,
  OE_CMD_SHUTTLE_FORWARD,
  OE_CMD_SHUTTLE_BACK,
  OE_CMD_MARK_IN,
  OE_CMD_MARK_OUT,
  OE_CMD_TOOL_SELECT,
  OE_CMD_TOOL_RAZOR,
  OE_CMD_DELETE_SELECTION,
  OE_CMD_UNDO,
  OE_CMD_REDO,
  OE_CMD_IMPORT_MEDIA,
  OE_CMD_NEW_PROJECT,
  OE_CMD_OPEN_PROJECT,
  OE_CMD_SAVE_PROJECT,
  OE_CMD_IMPORT_FROM_BIN,
  OE_CMD_ZOOM_IN,
  OE_CMD_ZOOM_OUT,
  OE_CMD_SHOW_ABOUT,
  OE_CMD_COUNT
} OeCommandId;

/**
 * OeCommandEntry:
 * @id: the command's stable enum ID.
 * @name: stable dotted name (e.g. "transport.play-pause"); doubles as the
 *   GTK action name in the "app." action group.
 * @accelerator: default accelerator in GTK syntax, or NULL for menu-only
 *   commands.
 * @enabled: disabled commands report "is disabled" instead of dispatching.
 *
 * The table has exactly #OE_CMD_COUNT entries, indexed by ID.
 */
typedef struct
{
  OeCommandId id;
  const gchar *name;
  const gchar *accelerator;
  gboolean enabled;
} OeCommandEntry;

/**
 * oe_command_table:
 *
 * Returns: (transfer none): the static command table, @OE_CMD_COUNT long.
 */
const OeCommandEntry *oe_command_table (void);

/**
 * oe_command_entry:
 * @id: command ID
 *
 * Returns: (transfer none): the entry for @id, or NULL when @id is out of
 * range. Unknown IDs are handled, never trusted.
 */
const OeCommandEntry *oe_command_entry (OeCommandId id);

/**
 * oe_command_accelerator_is_valid:
 * @accelerator: accelerator string in GTK syntax, or NULL
 *
 * GTK-free structural validator for accelerator strings, mirroring the
 * subset of gtk_accelerator_parse() the registry accepts: zero or more
 * <Control>/<Shift>/<Alt>/<Meta>/<Hyper>/<Super> modifiers followed by
 * exactly one key — a single printable character or a named key token
 * ("space", "Delete", "F5", ...). Lets the GTK-free tests pin table
 * integrity without linking GTK.
 *
 * Returns: TRUE when @accelerator is structurally valid.
 */
gboolean oe_command_accelerator_is_valid (const gchar *accelerator);

/**
 * OeCommandHandler:
 * @id: the dispatched command
 * @user_data: the user_data passed to oe_command_dispatch()
 *
 * Installed with oe_command_set_handler() once a phase implements a
 * command. In Phase 1 no handlers exist; every command reports
 * not-implemented.
 */
typedef void (*OeCommandHandler) (OeCommandId id, gpointer user_data);

/**
 * OeCommandReport:
 * @id: the dispatched command (or the unknown ID)
 * @message: ready-to-display status message
 * @user_data: the user_data given to oe_command_set_reporter()
 *
 * The dispatch-to-UI seam: the window installs a reporter that writes to
 * the status bar. GTK-free by construction — the registry only holds a
 * function pointer.
 */
typedef void (*OeCommandReport) (OeCommandId id, const gchar *message, gpointer user_data);

/**
 * oe_command_set_handler:
 * @id: command ID
 * @handler: handler function, or NULL to clear
 *
 * Registers (or clears) the implementation of one command. Safe for out-of
 * range IDs (ignored).
 */
void oe_command_set_handler (OeCommandId id, OeCommandHandler handler);

/**
 * oe_command_set_enabled:
 * @id: command ID
 * @enabled: whether the command may dispatch
 *
 * Later phases gate commands (e.g. undo with an empty history) through
 * this switch; disabled commands report "is disabled" on dispatch.
 */
void oe_command_set_enabled (OeCommandId id, gboolean enabled);

/**
 * oe_command_set_reporter:
 * @reporter: report sink, or NULL to clear
 * @user_data: passed back to every report call
 *
 * Installs the single report sink (the shell's status bar). The reporter
 * must be cleared before its owner is destroyed — the window does this in
 * its finalize.
 */
void oe_command_set_reporter (OeCommandReport reporter, gpointer user_data);

/**
 * oe_command_dispatch:
 * @id: command ID to dispatch
 * @user_data: passed through to a registered handler
 *
 * The single dispatch path. Rules, in order:
 *   1. Unknown ID: warning log + "Unknown command" report. Never crashes.
 *   2. Disabled command: info log + "<command> is disabled" report.
 *   3. Registered handler: the handler owns the command.
 *   4. Otherwise: info log + "<command> not implemented yet" report.
 */
void oe_command_dispatch (OeCommandId id, gpointer user_data);

G_END_DECLS
