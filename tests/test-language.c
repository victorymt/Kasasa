/* test-language.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gio/gio.h>

#include "kasasa-language.h"

static void
test_language_id_mapping (void)
{
  g_assert_cmpint (kasasa_language_from_id ("system"),
                   ==,
                   KASASA_LANGUAGE_SYSTEM);
  g_assert_cmpint (kasasa_language_from_id ("en"),
                   ==,
                   KASASA_LANGUAGE_ENGLISH);
  g_assert_cmpint (kasasa_language_from_id ("zh_CN"),
                   ==,
                   KASASA_LANGUAGE_CHINESE_SIMPLIFIED);
  g_assert_cmpint (kasasa_language_from_id ("unsupported"),
                   ==,
                   KASASA_LANGUAGE_SYSTEM);
  g_assert_cmpint (kasasa_language_from_id (NULL),
                   ==,
                   KASASA_LANGUAGE_SYSTEM);

  g_assert_cmpstr (kasasa_language_to_id (KASASA_LANGUAGE_SYSTEM),
                   ==,
                   "system");
  g_assert_cmpstr (kasasa_language_to_id (KASASA_LANGUAGE_ENGLISH),
                   ==,
                   "en");
  g_assert_cmpstr (
    kasasa_language_to_id (KASASA_LANGUAGE_CHINESE_SIMPLIFIED),
    ==,
    "zh_CN");
  g_assert_cmpstr (kasasa_language_to_id (KASASA_LANGUAGE_N_OPTIONS),
                   ==,
                   "system");

  g_assert_null (
    kasasa_language_to_gettext_locale (KASASA_LANGUAGE_SYSTEM));
  g_assert_cmpstr (
    kasasa_language_to_gettext_locale (KASASA_LANGUAGE_ENGLISH),
    ==,
    "en");
  g_assert_cmpstr (
    kasasa_language_to_gettext_locale (
      KASASA_LANGUAGE_CHINESE_SIMPLIFIED),
    ==,
    "zh_CN");
}

static void
test_language_setting_applies_before_startup (void)
{
  g_autoptr (GSettings) settings = NULL;
  g_autofree gchar *previous_language = g_strdup (g_getenv ("LANGUAGE"));

  settings = g_settings_new ("io.github.kelvinnovais.Kasasa");

  g_setenv ("LANGUAGE", "pt_BR", TRUE);
  g_assert_true (g_settings_set_string (settings, "language", "system"));
  kasasa_language_apply_from_settings ();
  g_assert_cmpstr (g_getenv ("LANGUAGE"), ==, "pt_BR");

  g_assert_true (g_settings_set_string (settings, "language", "en"));
  kasasa_language_apply_from_settings ();
  g_assert_cmpstr (g_getenv ("LANGUAGE"), ==, "en");

  g_assert_true (g_settings_set_string (settings, "language", "zh_CN"));
  kasasa_language_apply_from_settings ();
  g_assert_cmpstr (g_getenv ("LANGUAGE"), ==, "zh_CN");

  g_settings_reset (settings, "language");
  if (previous_language != NULL)
    g_setenv ("LANGUAGE", previous_language, TRUE);
  else
    g_unsetenv ("LANGUAGE");
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  g_test_add_func ("/language/id-mapping", test_language_id_mapping);
  g_test_add_func ("/language/setting-applies-before-startup",
                   test_language_setting_applies_before_startup);

  return g_test_run ();
}
