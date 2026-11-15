#ifndef OVERLAY_TIMER_I18N_H
#define OVERLAY_TIMER_I18N_H

/*
 * Internationalization (GNU gettext).
 *
 * Usage: wrap user-visible strings with _( ... ).
 * Initialization (setlocale/bindtextdomain/textdomain) is done in main().
 */

#include <libintl.h>
#include <locale.h>

#define GETTEXT_PACKAGE "overlay-timer"

#define _(String) gettext(String)
#define N_(String) (String)

#endif /* OVERLAY_TIMER_I18N_H */
