/* okbox: the boxes okplay shows over a game - game passwords, choices, notices.
 * Shared by the DirectPlay drop-in (winedplay dplayx builtin.c) and okplay wsock: the same look (styled like
 * Age of Empires II's own dialogs there, plain Windows dialogs elsewhere: okplay.ini simple_textboxes=) and the
 * same password scrambling ("okp1-" + SHA-256 of the text and the game's instance id). */
#ifndef OKBOX_H
#define OKBOX_H
#include <windows.h>

#define PW_NOASK  102
#define PW_THIRD  103   /* an optional third button, between the first and the last */

struct pw_dialog
{
    const WCHAR *title, *text;
    const WCHAR *ok, *cancel;       /* cancel NULL: one button */
    const WCHAR *third;             /* a third choice between them (PW_THIRD), NULL: none */
    const WCHAR *check;             /* NULL: no checkbox */
    BOOL edit;                      /* a password field */
    WCHAR typed[ 128 ];
    BOOL checked;
    short text_cy;                  /* height of the text block (dialog units), 0 = 26 */
    /* while shown */
    HBRUSH panel, field;
    HFONT title_font, text_font, button_font, small_font;
    short cx, cy, y_text, y_edit, y_check, y_buttons;   /* layout, dialog units */
    short x_check, x_buttons;                           /* left edges: checkbox control, first button */
    short x_text, text_h;                               /* the text block: side margin and planned height */
    int grow;                                           /* pixels the text block grew to fit its text */
};

/* hinst: the module the dialogs belong to; is_aoe2: NULL = never styled automatically; ini: okplay.ini path */
void okbox_init( HINSTANCE hinst, BOOL (*is_aoe2)( void ), const char *ini );
INT_PTR okbox_show( struct pw_dialog *d );   /* IDOK, IDCANCEL or PW_THIRD */
void okbox_hash_password( const WCHAR *typed, const GUID *instance, WCHAR *out, DWORD size );
void okbox_brand( WCHAR *out, DWORD size );
BOOL okbox_test_answers( void );   /* FALSE when OKP_PREVIEW_BOXES=1: show boxes tests would answer */
/* the boxes themselves (the drop-in's DP_BUILTIN_* call these); tests answer through environment variables:
 * OKP_TEST_HOST_PASSWORD (<none> = open game), OKP_TEST_JOIN_PASSWORD (a|b|c per attempt, <cancel>),
 * OKP_TEST_CLASSIC_ASK (windows|online|lan) */
BOOL okbox_ask_host_password( const GUID *instance, WCHAR *hashed, DWORD size );   /* TRUE = a password */
int okbox_ask_join_password( const GUID *instance, WCHAR *hashed, DWORD size );    /* 1 = typed, 0 = cancelled */
void okbox_wrong_password( void );
int okbox_ask_mode( const char *what );   /* 1 = the game's original connection, 2 = Online Play, 3 = LAN Play */
#endif
