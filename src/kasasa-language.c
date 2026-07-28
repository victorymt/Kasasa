/* kasasa-language.c
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "kasasa-language.h"

#include <gio/gio.h>
#include <locale.h>

#define KASASA_APPLICATION_ID "io.github.kelvinnovais.Kasasa"

static const gchar *language_ids[KASASA_LANGUAGE_N_OPTIONS] = {
  [KASASA_LANGUAGE_SYSTEM] = "system",
  [KASASA_LANGUAGE_ENGLISH] = "en",
  [KASASA_LANGUAGE_CHINESE_SIMPLIFIED] = "zh_CN",
};

static void
set_message_locale (KasasaLanguage language)
{
  static const gchar *english_locales[] = {
    "en_US.UTF-8",
    "en_US.utf8",
    "C.UTF-8",
    "C",
  };
  static const gchar *chinese_locales[] = {
    "zh_CN.UTF-8",
    "zh_CN.utf8",
    "zh_CN",
    /* LANGUAGE can select the Chinese catalog while an English locale acts as
     * the non-C message locale on systems without generated zh_CN locales. */
    "en_US.UTF-8",
    "en_US.utf8",
  };
  const gchar *const *locales = NULL;
  gsize n_locales = 0;

  switch (language)
    {
    case KASASA_LANGUAGE_ENGLISH:
      locales = english_locales;
      n_locales = G_N_ELEMENTS (english_locales);
      break;

    case KASASA_LANGUAGE_CHINESE_SIMPLIFIED:
      locales = chinese_locales;
      n_locales = G_N_ELEMENTS (chinese_locales);
      break;

    case KASASA_LANGUAGE_SYSTEM:
    case KASASA_LANGUAGE_N_OPTIONS:
    default:
      return;
    }

  /* LANGUAGE is ignored when LC_MESSAGES resolves to the C locale.  Select a
   * real message locale as well, trying common spellings because distributions
   * do not use one consistent UTF-8 suffix. */
  for (gsize i = 0; i < n_locales; i++)
    {
      if (setlocale (LC_MESSAGES, locales[i]) != NULL)
        return;
    }
}

KasasaLanguage
kasasa_language_from_id (const gchar *id)
{
  for (guint i = 0; i < G_N_ELEMENTS (language_ids); i++)
    {
      if (g_strcmp0 (id, language_ids[i]) == 0)
        return (KasasaLanguage) i;
    }

  return KASASA_LANGUAGE_SYSTEM;
}

const gchar *
kasasa_language_to_id (KasasaLanguage language)
{
  if (language < KASASA_LANGUAGE_SYSTEM
      || language >= KASASA_LANGUAGE_N_OPTIONS)
    return language_ids[KASASA_LANGUAGE_SYSTEM];

  return language_ids[language];
}

const gchar *
kasasa_language_to_gettext_locale (KasasaLanguage language)
{
  switch (language)
    {
    case KASASA_LANGUAGE_ENGLISH:
      return "en";

    case KASASA_LANGUAGE_CHINESE_SIMPLIFIED:
      return "zh_CN";

    case KASASA_LANGUAGE_SYSTEM:
    case KASASA_LANGUAGE_N_OPTIONS:
    default:
      return NULL;
    }
}

void
kasasa_language_apply_from_settings (void)
{
  g_autoptr (GSettings) settings = NULL;
  g_autofree gchar *language_id = NULL;
  KasasaLanguage language;
  const gchar *locale;

  setlocale (LC_ALL, "");

  settings = g_settings_new (KASASA_APPLICATION_ID);
  language_id = g_settings_get_string (settings, "language");
  language = kasasa_language_from_id (language_id);
  locale = kasasa_language_to_gettext_locale (language);

  /* GNU gettext reads LANGUAGE before the first translated string is looked
   * up.  For the system option, leave the caller's locale environment intact. */
  if (locale != NULL)
    {
      g_setenv ("LANGUAGE", locale, TRUE);
      set_message_locale (language);
    }
}
