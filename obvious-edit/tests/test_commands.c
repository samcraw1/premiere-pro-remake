/* test_commands.c — GTK-free tests for the command registry (Phase 1).
 *
 * Five GLib test cases, all display-free:
 *   /commands/table-integrity   IDs sequential and unique, dotted names
 *                               valid and unique, accelerators valid or NULL.
 *   /commands/accelerators      the validator accepts GTK's accelerator
 *                               grammar subset and rejects malformed input.
 *   /commands/dispatch          dispatch of an implemented-table command
 *                               with no handler logs and reports
 *                               "<command> not implemented yet".
 *   /commands/unknown-disabled  out-of-range IDs and disabled commands
 *                               degrade to a status report and a log line,
 *                               never a crash.
 *   /commands/handler           a registered handler receives the dispatch
 *                               and suppresses the not-implemented report.
 */

#include <string.h>

#include <glib.h>

#include "../src/app/oe_command.h"
#include "../src/app/oe_log.h"

/*
 * Capturing log writer: records the MESSAGE field of every record in the OE
 * domain so tests can assert what oe_log emitted. Non-OE records are returned
 * UNHANDLED so GLib falls back to the default writer and framework
 * diagnostics stay visible. Same pattern as test_lifecycle.c.
 */
static GString *captured = NULL;

static GLogWriterOutput
capture_writer (GLogLevelFlags log_level, const GLogField *fields, gsize n_fields,
                gpointer user_data G_GNUC_UNUSED)
{
  gboolean in_oe_domain = FALSE;
  gsize i;

  for (i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, "GLIB_DOMAIN") == 0
          && g_strcmp0 ((const gchar *) fields[i].value, G_LOG_DOMAIN) == 0)
        {
          in_oe_domain = TRUE;
          break;
        }
    }

  if (!in_oe_domain)
    return G_LOG_WRITER_UNHANDLED;

  for (i = 0; i < n_fields; i++)
    {
      if (g_strcmp0 (fields[i].key, "MESSAGE") == 0)
        g_string_append_printf (captured, "[%d] %s\n", (int) log_level,
                                (const gchar *) fields[i].value);
    }

  return G_LOG_WRITER_HANDLED;
}

/* Capturing reporter: records the status message each dispatch produces. */
static GString *reported = NULL;

static void
report_capture (OeCommandId id G_GNUC_UNUSED, const gchar *message,
                gpointer user_data G_GNUC_UNUSED)
{
  g_string_append_printf (reported, "%s\n", message);
}

/* Capturing handler: records the dispatch the handler receives. */
static OeCommandId handler_seen_id = OE_CMD_COUNT;
static gpointer handler_seen_data = NULL;

static void
handler_capture (OeCommandId id, gpointer user_data)
{
  handler_seen_id = id;
  handler_seen_data = user_data;
}

static void
state_reset (void)
{
  g_string_truncate (captured, 0);
  g_string_truncate (reported, 0);

  for (int i = 0; i < OE_CMD_COUNT; i++)
    {
      oe_command_set_handler ((OeCommandId) i, NULL);
      oe_command_set_enabled ((OeCommandId) i, TRUE);
    }

  oe_command_set_reporter (report_capture, NULL);
}

static gboolean
captured_contains (const gchar *needle)
{
  return strstr (captured->str, needle) != NULL;
}

/* Dotted-name rule for command names: lowercase letters, digits, and '-',
 * separated by exactly one '.', with no empty segments. Mirrors what GTK
 * accepts as an action name so the table can double as action names. */
static gboolean
dotted_name_is_valid (const gchar *name)
{
  if (name == NULL || *name == '\0')
    return FALSE;

  gboolean has_dot = FALSE;
  gboolean segment_empty = TRUE;

  for (const gchar *p = name; *p != '\0'; p++)
    {
      if (*p == '.')
        {
          if (segment_empty)
            return FALSE;
          has_dot = TRUE;
          segment_empty = TRUE;
          continue;
        }

      if (!(g_ascii_islower (*p) || g_ascii_isdigit (*p) || *p == '-'))
        return FALSE;

      segment_empty = FALSE;
    }

  return has_dot && !segment_empty;
}

static void
assert_entry (OeCommandId id, const gchar *name, const gchar *accelerator)
{
  const OeCommandEntry *entry = oe_command_entry (id);

  g_assert_nonnull (entry);
  g_assert_cmpuint (entry->id, ==, id);
  g_assert_cmpstr (entry->name, ==, name);
  g_assert_cmpstr (entry->accelerator, ==, accelerator);
}

static void
test_table_integrity (void)
{
  const OeCommandEntry *table = oe_command_table ();

  g_assert_nonnull (table);

  for (int i = 0; i < OE_CMD_COUNT; i++)
    {
      const OeCommandEntry *entry = &table[i];

      /* IDs are the enum values: entry i must describe command i. */
      g_assert_cmpuint (entry->id, ==, i);
      g_assert_true (dotted_name_is_valid (entry->name));
      g_assert_true (entry->accelerator == NULL
                     || oe_command_accelerator_is_valid (entry->accelerator));

      /* Names are permanent API: no duplicates anywhere in the table. */
      for (int j = 0; j < i; j++)
        g_assert_cmpstr (table[j].name, !=, entry->name);
    }

  /* The keyboard spine of the editor, pinned by name and accelerator. */
  assert_entry (OE_CMD_PLAY_PAUSE, "transport.play-pause", "space");
  assert_entry (OE_CMD_STOP, "transport.stop", "k");
  assert_entry (OE_CMD_SHUTTLE_FORWARD, "transport.shuttle-forward", "l");
  assert_entry (OE_CMD_SHUTTLE_BACK, "transport.shuttle-back", "j");
  assert_entry (OE_CMD_MARK_IN, "transport.mark-in", "i");
  assert_entry (OE_CMD_MARK_OUT, "transport.mark-out", "o");
  assert_entry (OE_CMD_TOOL_SELECT, "tool.select", "v");
  assert_entry (OE_CMD_TOOL_RAZOR, "tool.razor", "c");
  assert_entry (OE_CMD_DELETE_SELECTION, "selection.delete", "Delete");

  /* Reserved undo/redo: accelerators wired now, behaviour arrives later. */
  assert_entry (OE_CMD_UNDO, "edit.undo", "<Control>z");
  assert_entry (OE_CMD_REDO, "edit.redo", "<Control><Shift>z");

  /* Phase 2 deliberately moves media.import out of the menu-only set:
   * import is the spine of the editing workflow, so it gets the
   * conventional Ctrl+I (deliberate pin change, noted in the PR body). */
  assert_entry (OE_CMD_IMPORT_MEDIA, "media.import", "<Control>i");

  /* Menu-only commands carry no accelerator by contract. */
  g_assert_null (oe_command_entry (OE_CMD_NEW_PROJECT)->accelerator);
  g_assert_null (oe_command_entry (OE_CMD_OPEN_PROJECT)->accelerator);
  g_assert_null (oe_command_entry (OE_CMD_SAVE_PROJECT)->accelerator);
  g_assert_null (oe_command_entry (OE_CMD_SHOW_ABOUT)->accelerator);

  /* Every command starts enabled. */
  for (int i = 0; i < OE_CMD_COUNT; i++)
    g_assert_true (table[i].enabled);
}

static void
test_accelerators (void)
{
  /* Valid: single keys, named keys, modifier chains. */
  static const gchar *valid[] = {
    "space",
    "j",
    "k",
    "l",
    "i",
    "o",
    "v",
    "c",
    "Delete",
    "z",
    "<Control>z",
    "<Control><Shift>z",
    "<Alt>f4",
    "F5",
    "escape",
    "pageup",
    "Up",
    "a",
    "0",
    "-",
    "<Control><Alt>Delete",
    "<Control>equal",
    "<Control>minus",
    "<Control>plus",
    NULL,
  };

  for (int i = 0; valid[i] != NULL; i++)
    g_assert_true (oe_command_accelerator_is_valid (valid[i]));

  /* Invalid: empty, missing key, missing '>', unknown modifier, two keys,
   * modifiers after the key, whitespace, junk. NULL is never an accelerator
   * the validator should accept. */
  static const gchar *invalid[] = {
    "",           " ",  "<Control>",        "Control>z", "<Bogus>z",    "j+k",
    "z<Control>", "ab", "<Control><Shift>", "  ",        "<Control> z", NULL,
  };

  for (int i = 0; invalid[i] != NULL; i++)
    g_assert_false (oe_command_accelerator_is_valid (invalid[i]));

  g_assert_false (oe_command_accelerator_is_valid (NULL));
}

/* The platform mapping rewrites <Control> to the host's primary modifier
 * and nothing else; every table chord stays valid after mapping. */
static void
test_platform_accelerator (void)
{
  const OeCommandEntry *table = oe_command_table ();

  for (int i = 0; i < OE_CMD_COUNT; i++)
    {
      g_autofree gchar *mapped = oe_command_platform_accelerator (table[i].accelerator);

      if (table[i].accelerator == NULL)
        g_assert_null (mapped);
      else
        g_assert_true (oe_command_accelerator_is_valid (mapped));
    }

  g_autofree gchar *undo = oe_command_platform_accelerator ("<Control>z");
  g_autofree gchar *redo = oe_command_platform_accelerator ("<Control><Shift>z");
  g_autofree gchar *plain = oe_command_platform_accelerator ("space");
  g_autofree gchar *alt = oe_command_platform_accelerator ("<Alt>f4");

  g_assert_cmpstr (undo, ==, OE_COMMAND_PRIMARY_MODIFIER "z");
  g_assert_cmpstr (redo, ==, OE_COMMAND_PRIMARY_MODIFIER "<Shift>z");
  g_assert_cmpstr (plain, ==, "space");
  g_assert_cmpstr (alt, ==, "<Alt>f4");

#ifdef __APPLE__
  g_assert_cmpstr (undo, ==, "<Meta>z");
#else
  g_assert_cmpstr (undo, ==, "<Control>z");
#endif
}

static void
test_dispatch_not_implemented (void)
{
  state_reset ();

  oe_command_dispatch (OE_CMD_PLAY_PAUSE, NULL);

  /* The exact status-bar wording the brief pins, and a matching log line. */
  g_assert_cmpstr (reported->str, ==, "transport.play-pause not implemented yet\n");
  g_assert_true (captured_contains ("transport.play-pause not implemented yet"));
}

static void
test_dispatch_unknown_and_disabled (void)
{
  state_reset ();

  /* Out-of-range IDs must degrade to a report and a log line, not crash. */
  oe_command_dispatch (OE_CMD_COUNT, NULL);
  oe_command_dispatch ((OeCommandId) -1, NULL);

  g_assert_cmpstr (reported->str, ==, "Unknown command\nUnknown command\n");
  g_assert_true (captured_contains ("unknown command id"));

  state_reset ();
  oe_command_set_enabled (OE_CMD_UNDO, FALSE);
  oe_command_dispatch (OE_CMD_UNDO, NULL);

  g_assert_cmpstr (reported->str, ==, "edit.undo is disabled\n");
  g_assert_true (captured_contains ("edit.undo is disabled"));

  /* Re-enabling restores the not-implemented path. */
  state_reset ();
  oe_command_dispatch (OE_CMD_UNDO, NULL);
  g_assert_cmpstr (reported->str, ==, "edit.undo not implemented yet\n");
}

static void
test_dispatch_handler (void)
{
  handler_seen_id = OE_CMD_COUNT;
  handler_seen_data = NULL;

  state_reset ();
  oe_command_set_handler (OE_CMD_STOP, handler_capture);
  oe_command_dispatch (OE_CMD_STOP, GINT_TO_POINTER (42));

  g_assert_cmpint (handler_seen_id, ==, OE_CMD_STOP);
  g_assert_true (handler_seen_data == GINT_TO_POINTER (42));

  /* The handler owns the command: no not-implemented report is made. */
  g_assert_cmpstr (reported->str, ==, "");

  oe_command_set_handler (OE_CMD_STOP, NULL);
  oe_command_dispatch (OE_CMD_STOP, NULL);
  g_assert_cmpstr (reported->str, ==, "transport.stop not implemented yet\n");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  captured = g_string_new ("");
  reported = g_string_new ("");
  g_log_set_writer_func (capture_writer, NULL, NULL);
  oe_log_init ();

  g_test_add_func ("/commands/table-integrity", test_table_integrity);
  g_test_add_func ("/commands/accelerators", test_accelerators);
  g_test_add_func ("/commands/platform-accelerator", test_platform_accelerator);
  g_test_add_func ("/commands/dispatch", test_dispatch_not_implemented);
  g_test_add_func ("/commands/unknown-disabled", test_dispatch_unknown_and_disabled);
  g_test_add_func ("/commands/handler", test_dispatch_handler);

  return g_test_run ();
}
