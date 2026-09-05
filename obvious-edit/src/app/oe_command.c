/* oe_command.c — command registry implementation (Phase 1).
 *
 * The static table is the single source of truth for names, accelerators,
 * and initial enablement; it must stay in OeCommandId order (the tests pin
 * it). Dispatch state is deliberately minimal: one handler slot per
 * command, one reporter. All of it lives on the UI thread.
 */

#include "oe_command.h"

#include <string.h>

#include "oe_log.h"

/* Mutable despite the const exposure: oe_command_set_enabled() flips
 * entries at runtime (later phases gate undo on an empty history). */
static OeCommandEntry command_table[OE_CMD_COUNT] = {
  { OE_CMD_PLAY_PAUSE, "transport.play-pause", "space", TRUE },
  { OE_CMD_STOP, "transport.stop", "k", TRUE },
  { OE_CMD_SHUTTLE_FORWARD, "transport.shuttle-forward", "l", TRUE },
  { OE_CMD_SHUTTLE_BACK, "transport.shuttle-back", "j", TRUE },
  { OE_CMD_MARK_IN, "transport.mark-in", "i", TRUE },
  { OE_CMD_MARK_OUT, "transport.mark-out", "o", TRUE },
  { OE_CMD_TOOL_SELECT, "tool.select", "v", TRUE },
  { OE_CMD_TOOL_RAZOR, "tool.razor", "c", TRUE },
  { OE_CMD_DELETE_SELECTION, "selection.delete", "Delete", TRUE },
  { OE_CMD_UNDO, "edit.undo", "<Control>z", TRUE },
  { OE_CMD_REDO, "edit.redo", "<Control><Shift>z", TRUE },
  { OE_CMD_IMPORT_MEDIA, "media.import", "<Control>i", TRUE },
  { OE_CMD_NEW_PROJECT, "project.new", NULL, TRUE },
  { OE_CMD_OPEN_PROJECT, "project.open", NULL, TRUE },
  { OE_CMD_SAVE_PROJECT, "project.save", NULL, TRUE },
  { OE_CMD_IMPORT_FROM_BIN, "media.insert-from-bin", "<Control>e", TRUE },
  { OE_CMD_ZOOM_IN, "view.zoom-in", "<Control>equal", TRUE },
  { OE_CMD_ZOOM_OUT, "view.zoom-out", "<Control>minus", TRUE },
  { OE_CMD_SNAP_TOGGLE, "edit.snap-toggle", "s", TRUE },
  { OE_CMD_SHOW_ABOUT, "help.about", NULL, TRUE },
  { OE_CMD_EXPORT, "project.export", NULL, TRUE },
  /* Phase 11 Wave B: generated-clip insertion — menu-only like the
   * project commands; the handler inserts through the same validated
   * generator mutator and records nothing (media.import precedent). */
  { OE_CMD_INSERT_TITLE, "media.insert-title", NULL, TRUE },
  { OE_CMD_INSERT_SOLID, "media.insert-solid", NULL, TRUE },
};

static OeCommandHandler handlers[OE_CMD_COUNT];
static OeCommandReport reporter;
static gpointer reporter_data;

const OeCommandEntry *
oe_command_table (void)
{
  return command_table;
}

const OeCommandEntry *
oe_command_entry (OeCommandId id)
{
  if ((int) id < 0 || id >= OE_CMD_COUNT)
    return NULL;

  return &command_table[id];
}

/* Modifier tokens GTK's accelerator parser accepts inside <...>. */
static const gchar *modifier_tokens[] = {
  "control", "shift", "alt", "meta", "hyper", "super",
};

/* Named key tokens GTK accepts as the key part. Names mirror gdk keyval
 * names; the validator accepts the ASCII spellings case-insensitively. */
static const gchar *named_key_tokens[] = {
  "space",
  "tab",
  "return",
  "enter",
  "escape",
  "backspace",
  "delete",
  "insert",
  "home",
  "end",
  "up",
  "down",
  "left",
  "right",
  "pageup",
  "pagedown",
  "f1",
  "f2",
  "f3",
  "f4",
  "f5",
  "f6",
  "f7",
  "f8",
  "f9",
  "f10",
  "f11",
  "f12",
  /* Named punctuation keysyms GTK parses (Ctrl+= / Ctrl+- zoom). */
  "equal",
  "minus",
  "plus",
};

static gboolean
token_matches (const gchar *token, gsize len, const gchar *names[], gsize n_names)
{
  for (gsize i = 0; i < n_names; i++)
    {
      if (strlen (names[i]) == len && g_ascii_strncasecmp (names[i], token, len) == 0)
        return TRUE;
    }

  return FALSE;
}

gboolean
oe_command_accelerator_is_valid (const gchar *accelerator)
{
  if (accelerator == NULL || *accelerator == '\0')
    return FALSE;

  const gchar *p = accelerator;

  /* Zero or more <Modifier> groups, each closed and known. */
  while (*p == '<')
    {
      const gchar *close = strchr (p + 1, '>');

      if (close == NULL)
        return FALSE;

      if (!token_matches (p + 1, (gsize) (close - (p + 1)), modifier_tokens,
                          G_N_ELEMENTS (modifier_tokens)))
        return FALSE;

      p = close + 1;
    }

  /* Exactly one key must follow: nothing empty, no stray '<'. */
  if (*p == '\0' || strchr (p, '<') != NULL)
    return FALSE;

  /* One printable UTF-8 character... */
  if (*g_utf8_next_char (p) == '\0')
    {
      gunichar c = g_utf8_get_char (p);

      return c != ' ' && g_unichar_isprint (c);
    }

  /* ...or one named key token. */
  return token_matches (p, strlen (p), named_key_tokens, G_N_ELEMENTS (named_key_tokens));
}

gchar *
oe_command_platform_accelerator (const gchar *accelerator)
{
  if (accelerator == NULL)
    return NULL;

  GString *out = g_string_new (NULL);
  const gchar *p = accelerator;

  while (*p == '<')
    {
      const gchar *close = strchr (p + 1, '>');

      if (close == NULL)
        break;

      gsize len = (gsize) (close - (p + 1));

      if (len == strlen ("control") && g_ascii_strncasecmp (p + 1, "control", len) == 0)
        g_string_append (out, OE_COMMAND_PRIMARY_MODIFIER);
      else
        g_string_append_len (out, p, (gssize) (close + 1 - p));

      p = close + 1;
    }

  g_string_append (out, p);

  return g_string_free (out, FALSE);
}

void
oe_command_set_handler (OeCommandId id, OeCommandHandler handler)
{
  if ((int) id < 0 || id >= OE_CMD_COUNT)
    {
      oe_log (OE_LOG_LEVEL_WARNING, "set_handler: unknown command id %d ignored", (int) id);
      return;
    }

  handlers[id] = handler;
}

void
oe_command_set_enabled (OeCommandId id, gboolean enabled)
{
  if ((int) id < 0 || id >= OE_CMD_COUNT)
    {
      oe_log (OE_LOG_LEVEL_WARNING, "set_enabled: unknown command id %d ignored", (int) id);
      return;
    }

  command_table[id].enabled = enabled;
}

void
oe_command_set_reporter (OeCommandReport new_reporter, gpointer user_data)
{
  reporter = new_reporter;
  reporter_data = user_data;
}

/* Report if a sink is installed; without one (window not yet up) dispatch
 * still logs — never reports into a dangling pointer. */
static void
report_to_sink (OeCommandId id, const gchar *message)
{
  if (reporter != NULL)
    reporter (id, message, reporter_data);
}

void
oe_command_dispatch (OeCommandId id, gpointer user_data)
{
  const OeCommandEntry *entry = oe_command_entry (id);

  if (entry == NULL)
    {
      oe_log (OE_LOG_LEVEL_WARNING, "unknown command id %d; ignoring", (int) id);
      report_to_sink (id, "Unknown command");
      return;
    }

  oe_log (OE_LOG_LEVEL_DEBUG, "command '%s' dispatched", entry->name);

  if (!entry->enabled)
    {
      g_autofree gchar *message = g_strdup_printf ("%s is disabled", entry->name);

      oe_log (OE_LOG_LEVEL_INFO, "command rejected: %s", message);
      report_to_sink (id, message);
      return;
    }

  if (handlers[id] != NULL)
    {
      handlers[id](id, user_data);
      return;
    }

  g_autofree gchar *message = g_strdup_printf ("%s not implemented yet", entry->name);

  oe_log (OE_LOG_LEVEL_INFO, "%s", message);
  report_to_sink (id, message);
}
