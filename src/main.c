/* main.c
 *
 * Copyright 2024 Kelvin
 * Copyright 2026 victorymt
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <glib/gi18n.h>
#include <gst/gst.h>

#include "kasasa-application.h"
#include "kasasa-language.h"

int
main (int   argc,
      char *argv[])
{
  g_autoptr(KasasaApplication) app = NULL;
  gboolean diagnostics = FALSE;
  int i;
  int ret;

  for (i = 1; i < argc; i++)
    {
      if (g_str_equal (argv[i], "--diagnostics"))
        {
          diagnostics = TRUE;
          break;
        }
    }

  /* Apply the saved language before gettext or any GTK template performs its
   * first translation lookup.  Existing widgets cannot be safely retranslated
   * in place, so preference changes take effect on the next launch. */
  /* Diagnostics must remain usable before a display or installed GSettings
   * schema is available, so skip preference initialization in that mode. */
  if (!diagnostics)
    kasasa_language_apply_from_settings ();

  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);

  /* Initialize GStreamer outside GObject class initialization.  Calling it
   * from KasasaScreencastClass::class_init reverses the GType/GStreamer lock
   * order and can deadlock when the first screencast is constructed. */
  gst_init (&argc, &argv);

  app = kasasa_application_new ("io.github.kelvinnovais.Kasasa");
  ret = g_application_run (G_APPLICATION (app), argc, argv);

  return ret;
}
