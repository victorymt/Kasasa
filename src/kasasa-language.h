/* kasasa-language.h
 *
 * Copyright 2026 victorymt
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  KASASA_LANGUAGE_SYSTEM,
  KASASA_LANGUAGE_ENGLISH,
  KASASA_LANGUAGE_CHINESE_SIMPLIFIED,
  KASASA_LANGUAGE_N_OPTIONS,
} KasasaLanguage;

KasasaLanguage kasasa_language_from_id            (const gchar    *id);
const gchar   *kasasa_language_to_id              (KasasaLanguage language);
const gchar   *kasasa_language_to_gettext_locale  (KasasaLanguage language);
void           kasasa_language_apply_from_settings (void);

G_END_DECLS
