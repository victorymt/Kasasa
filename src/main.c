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

int
main (int   argc,
      char *argv[])
{
  g_autoptr(KasasaApplication) app = NULL;
  int ret;

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
