#include "oe_application.h"

#include "oe_command.h"
#include "oe_log.h"

#include "../media/oe_ffmpeg.h"
#include "../playback/oe_audio_output.h"
#include "../ui/oe_main_window.h"
#include "../ui/oe_theme.h"

struct _OeApplication
{
  GtkApplication parent_instance;

  gboolean self_check;
  gboolean startup_attempted;
  gboolean startup_succeeded;
};

G_DEFINE_TYPE (OeApplication, oe_application, GTK_TYPE_APPLICATION)

/* Every registry command is exposed as an app.<dotted-name> action; the
 * action layer is pure plumbing — all policy lives in the registry. */
static void
on_command_action (GSimpleAction *action G_GNUC_UNUSED, GVariant *parameter G_GNUC_UNUSED,
                   gpointer user_data)
{
  oe_command_dispatch ((OeCommandId) GPOINTER_TO_INT (user_data), NULL);
}

static void
oe_application_install_commands (OeApplication *self)
{
  const OeCommandEntry *table = oe_command_table ();

  for (int i = 0; i < OE_CMD_COUNT; i++)
    {
      GSimpleAction *action = g_simple_action_new (table[i].name, NULL);

      g_signal_connect (action, "activate", G_CALLBACK (on_command_action),
                        GINT_TO_POINTER (table[i].id));
      g_action_map_add_action (G_ACTION_MAP (self), G_ACTION (action));
      g_object_unref (action);

      if (table[i].accelerator != NULL)
        {
          g_autofree gchar *detailed = g_strdup_printf ("app.%s", table[i].name);
          const gchar *accels[] = { table[i].accelerator, NULL };

          gtk_application_set_accels_for_action (GTK_APPLICATION (self), detailed, accels);
        }
    }

  oe_log (OE_LOG_LEVEL_INFO, "installed %d commands", (int) OE_CMD_COUNT);
}

/* Runs once, on the window's first map; the self-check unwinds from here. */
static void
on_window_map (GtkWidget *widget G_GNUC_UNUSED, gpointer user_data)
{
  GtkApplication *application = GTK_APPLICATION (user_data);

  oe_log (OE_LOG_LEVEL_DEBUG, "self-check: window mapped, unwinding");
  g_application_quit (G_APPLICATION (application));
}

static void
oe_application_activate (GApplication *application)
{
  OeApplication *self = OE_APPLICATION (application);
  GtkApplication *gtk_application = GTK_APPLICATION (application);
  GtkWidget *window;

  if (!self->startup_succeeded)
    {
      oe_log (OE_LOG_LEVEL_ERROR, "startup failed; refusing to open a window");
      g_application_quit (application);
      return;
    }

  window = oe_main_window_new (gtk_application);

  /*
   * The handler is connected before present so the one-shot self-check
   * cannot miss the map transition.
   */
  if (self->self_check)
    g_signal_connect (window, "map", G_CALLBACK (on_window_map), gtk_application);

  gtk_window_present (GTK_WINDOW (window));
}

static void
oe_application_startup (GApplication *application)
{
  OeApplication *self = OE_APPLICATION (application);
  GError *error = NULL;

  G_APPLICATION_CLASS (oe_application_parent_class)->startup (application);

  self->startup_attempted = TRUE;

  /* Paired with oe_application_shutdown, which runs in reverse order. */
  self->startup_succeeded = oe_ffmpeg_init (&error);
  if (!self->startup_succeeded)
    {
      oe_log (OE_LOG_LEVEL_ERROR, "ffmpeg init failed: %s", error->message);
      g_clear_error (&error);
      return;
    }

  self->startup_succeeded = oe_audio_output_init (&error);
  if (!self->startup_succeeded)
    {
      oe_log (OE_LOG_LEVEL_ERROR, "audio output init failed: %s", error->message);
      g_clear_error (&error);
      return;
    }

  /* Shell prerequisites, in order: theme before the first window, then the
   * command actions so menu, toolbar, and accelerators all have targets. */
  oe_theme_init ();
  oe_application_install_commands (self);
}

static void
oe_application_shutdown (GApplication *application)
{
  /* Reverse order of startup; both calls are safe when nothing started. */
  oe_audio_output_shutdown ();
  oe_ffmpeg_shutdown ();

  G_APPLICATION_CLASS (oe_application_parent_class)->shutdown (application);
}

static gint
oe_application_handle_local_options (GApplication *application, GVariantDict *options)
{
  OeApplication *self = OE_APPLICATION (application);

  if (g_variant_dict_contains (options, "self-check"))
    self->self_check = TRUE;

  /* -1 continues into the local application instance. */
  return -1;
}

static void
oe_application_class_init (OeApplicationClass *klass)
{
  GApplicationClass *gapplication_class = G_APPLICATION_CLASS (klass);

  gapplication_class->startup = oe_application_startup;
  gapplication_class->shutdown = oe_application_shutdown;
  gapplication_class->activate = oe_application_activate;
  gapplication_class->handle_local_options = oe_application_handle_local_options;
}

static void
oe_application_init (OeApplication *self)
{
  self->self_check = FALSE;
  self->startup_attempted = FALSE;
  self->startup_succeeded = FALSE;
}

OeApplication *
oe_application_new (void)
{
  OeApplication *self;

  g_set_application_name ("Obvious Edit");

  self = g_object_new (OE_TYPE_APPLICATION, "application-id", "com.mempickup.obvious-edit", NULL);

  g_application_add_main_option (G_APPLICATION (self), "self-check", '\0', G_OPTION_FLAG_NONE,
                                 G_OPTION_ARG_NONE,
                                 "Open the window, quit after the first map, exit 0", NULL);

  return self;
}

void
oe_application_set_self_check (OeApplication *self, gboolean self_check)
{
  g_return_if_fail (OE_IS_APPLICATION (self));

  self->self_check = self_check;
}

gboolean
oe_application_startup_attempted (OeApplication *self)
{
  g_return_val_if_fail (OE_IS_APPLICATION (self), FALSE);

  return self->startup_attempted;
}

gboolean
oe_application_startup_succeeded (OeApplication *self)
{
  g_return_val_if_fail (OE_IS_APPLICATION (self), FALSE);

  return self->startup_succeeded;
}
