/* okbox: see okbox.h. Moved out of the drop-in's builtin.c (2026-11-05) so okplay wsock shows the same boxes. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "okbox.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE( a ) (sizeof(a) / sizeof((a)[0]))
#endif
extern void okp_log( int level, const char *fmt, ... );
extern int okp_win9x_paths( void );
extern void okp_sha256_parts( const void *const *parts, const size_t *lens, int n, unsigned char out[ 32 ] );
extern int okp_utf16_to_utf8( const WCHAR *in, char *out, int outsz );

static HINSTANCE okbox_self;
static BOOL (*okbox_aoe2)( void );
static char okbox_ini[ MAX_PATH ];
static BOOL okbox_top;

void okbox_set_topmost( BOOL on ) { okbox_top = on; }

void okbox_init( HINSTANCE hinst, BOOL (*is_aoe2)( void ), const char *ini )
{
    okbox_self = hinst;
    okbox_aoe2 = is_aoe2;
    lstrcpynA( okbox_ini, ini, sizeof(okbox_ini) );
}

static BOOL okbox_is_aoe2( void ) { return okbox_aoe2 && okbox_aoe2(); }

static void okbox_ini_str( const char *key, const char *def, char *out, DWORD size )
{
    GetPrivateProfileStringA( "okplay", key, def, out, size, okbox_ini );
}

/* ------------------------------------------------------------------ game passwords */

/* AoE2 has no password box, but DirectPlay has passwords (and the host checks them on every join).
 * On Online/LAN Play we ask: the host when it creates a game (okplay.ini use_password=ask|off; v0.5.0 called it host_password),
 * a joiner when the game it picked needs one. What DirectPlay gets - and what goes on the wire - is
 * never the typed text: both sides turn it into "okp1-" + SHA-256(text + the session's instance
 * GUID), so a sniffed value is useless for any other game and does not reveal the password.
 * Tests answer without a box: OKP_TEST_HOST_PASSWORD (<none> = open game) / OKP_TEST_JOIN_PASSWORD
 * (<cancel> = Cancel). */

void okbox_hash_password( const WCHAR *typed, const GUID *instance, WCHAR *out, DWORD size )
{
    static const char salt[] = "okplay game password v1|";
    UCHAR digest[ 32 ];
    char utf8[ 512 ];
    int n = okp_utf16_to_utf8( typed, utf8, sizeof(utf8) );   /* no CP_UTF8 on Windows 95 */
    const void *parts[ 3 ];
    size_t lens[ 3 ];
    DWORD i;

    if ( n < 0 ) n = 0;   /* an unconvertible password still hashes (as "") rather than failing open */
    parts[ 0 ] = salt;     lens[ 0 ] = sizeof(salt) - 1;
    parts[ 1 ] = utf8;     lens[ 1 ] = n;
    parts[ 2 ] = instance; lens[ 2 ] = sizeof(GUID);
    okp_sha256_parts( parts, lens, 3, digest );   /* the same digest bcrypt gave, on every Windows */
    { const WCHAR *pre = L"okp1-"; DWORD j; for ( j = 0; j + 1 < size && pre[ j ]; j++ ) out[ j ] = pre[ j ]; if ( size ) out[ j ] = 0; }   /* no lstrcpynW: Windows 9x only stubs it */
    for ( i = 0; i < 20 && 5 + i * 2 + 2 < size; i++ )   /* 160 bits is plenty for a lobby lock */
    {
        out[ 5 + i * 2 ] = L"0123456789abcdef"[ digest[ i ] >> 4 ];
        out[ 5 + i * 2 + 1 ] = L"0123456789abcdef"[ digest[ i ] & 15 ];
        out[ 5 + i * 2 + 2 ] = 0;
    }
}

/* The boxes look like AoE2's own dialogs: a blue speckled panel with a gold frame, a gothic title
 * (AoE2's own Lucida Blackletter, else Old English Text MT, else Georgia), dark-ink serif
 * text, a black field and tan-edged dark buttons. All drawn here: no caption bar, owner-drawn
 * buttons and checkbox, and the panel can be dragged anywhere. */

/* Colours: only the 16 basic VGA colours. On Windows 9x AoE2 runs the screen in 256 colours and sets
 * the palette itself through DirectDraw, which GDI can't see: any other colour is dithered or lands on
 * the wrong palette entry (the panel came out teal, then red). Games leave these 16 alone, so the boxes
 * look exactly the same in 16 colours, in-game on 98 and in full colour on Windows 10/11.
 * Scheme "Midnight Gold" (2026-09-30), one colour per part. */
#define VGA_BLACK   RGB(   0,   0,   0 )
#define VGA_MAROON  RGB( 128,   0,   0 )
#define VGA_GREEN   RGB(   0, 128,   0 )
#define VGA_OLIVE   RGB( 128, 128,   0 )
#define VGA_NAVY    RGB(   0,   0, 128 )
#define VGA_PURPLE  RGB( 128,   0, 128 )
#define VGA_TEAL    RGB(   0, 128, 128 )
#define VGA_SILVER  RGB( 192, 192, 192 )
#define VGA_GREY    RGB( 128, 128, 128 )
#define VGA_RED     RGB( 255,   0,   0 )
#define VGA_LIME    RGB(   0, 255,   0 )
#define VGA_YELLOW  RGB( 255, 255,   0 )
#define VGA_BLUE    RGB(   0,   0, 255 )
#define VGA_FUCHSIA RGB( 255,   0, 255 )
#define VGA_AQUA    RGB(   0, 255, 255 )
#define VGA_WHITE   RGB( 255, 255, 255 )

#define C_PANEL      VGA_BLACK     /* the panel ... */
#define C_SPECK      VGA_NAVY    /* ... and its specks (1 pixel in 16) */
#define C_TITLE      VGA_YELLOW
#define C_TEXT       VGA_SILVER    /* body text and the checkbox label */
#define C_FRAME      VGA_OLIVE    /* the 2 px frames */
#define C_EDGE       VGA_BLACK    /* the 1 px edges around them */
#define C_FIELD      VGA_NAVY    /* password field and checkbox */
#define C_FIELDTEXT  VGA_WHITE    /* typed text */
#define C_BUTTON     VGA_NAVY
#define C_BUTTONDOWN VGA_BLUE     /* pressed */
#define C_BUTTONTEXT VGA_YELLOW
#define C_CHECK      VGA_YELLOW    /* the tick */



#define PW_TEXT   100   /* the text, in the plain Windows box (simple_textboxes=1) */

static HBRUSH make_panel_brush( void )
{
    /* a 64x64 tile of panel colour with specks, the same every time */
    BITMAPINFO bi;
    DWORD *px = NULL;
    HBITMAP bmp;
    HBRUSH brush;
    UINT seed = 20260928;
    HDC dc = GetDC( NULL );
    int i;

    memset( &bi, 0, sizeof(bi) );
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = 64;
    bi.bmiHeader.biHeight = -64;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection( dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0 );
    ReleaseDC( NULL, dc );
    if ( !bmp || !px ) return CreateSolidBrush( C_PANEL );
    for ( i = 0; i < 64 * 64; i++ )
    {
        COLORREF c;
        seed = seed * 1103515245 + 12345;
        c = ((seed >> 9) & 15) == 0 ? C_SPECK : C_PANEL;   /* 1 in 16 pixels a speck */
        px[ i ] = ((DWORD)GetRValue( c ) << 16) | ((DWORD)GetGValue( c ) << 8) | GetBValue( c );
    }
    brush = CreatePatternBrush( bmp );
    DeleteObject( bmp );
    return brush ? brush : CreateSolidBrush( C_PANEL );
}

static int CALLBACK font_found( const LOGFONTA *lf, const TEXTMETRICA *tm, DWORD type, LPARAM lp )
{
    *(BOOL *)lp = TRUE;
    return 0;
}

static BOOL font_installed( const WCHAR *face )
{
    LOGFONTA lf;
    BOOL have = FALSE;
    HDC dc = GetDC( NULL );
    memset( &lf, 0, sizeof(lf) );
    lf.lfCharSet = DEFAULT_CHARSET;
    WideCharToMultiByte( CP_ACP, 0, face, -1, lf.lfFaceName, LF_FACESIZE, NULL, NULL );
    EnumFontFamiliesExA( dc, &lf, (FONTENUMPROCA)font_found, (LPARAM)&have, 0 );
    ReleaseDC( NULL, dc );
    return have;
}

/* the first of these faces this PC has (the last one is used regardless) */
static HFONT make_font( const WCHAR *const *faces, int px, int weight )
{
    HDC dc = GetDC( NULL );
    int dpi = GetDeviceCaps( dc, LOGPIXELSY );
    ReleaseDC( NULL, dc );
    char face[ LF_FACESIZE ];
    while ( faces[ 1 ] && !font_installed( faces[ 0 ] ) ) faces++;
    WideCharToMultiByte( CP_ACP, 0, faces[ 0 ], -1, face, sizeof(face), NULL, NULL );
    /* ClearType where Windows has it (XP+); 98 ignores the unknown quality and uses its default */
    return CreateFontA( -MulDiv( px, dpi, 96 ), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_ROMAN,
                        face );
}

/* AoE2's own fonts (its language.dll names them): Lucida Blackletter for titles and buttons,
 * Georgia for text. Lucida Blackletter comes with Office / older AoE installs, not with Windows. */
static const WCHAR *const gothic_faces[] = { L"Lucida Blackletter", L"Old English Text MT", L"Georgia", NULL };
static const WCHAR *const text_faces[]   = { L"Georgia", L"Times New Roman", NULL };

/* the dialogs keep their text as WCHAR; Windows 9x only draws ANSI (its W functions are stubs,
 * and not every 9x exports them, so the NT ones are looked up rather than imported) */
static FARPROC user32_w( const char *name )
{
    return okp_win9x_paths() ? NULL : GetProcAddress( GetModuleHandleA( "user32.dll" ), name );
}

static int draw_text( HDC dc, const WCHAR *text, RECT *r, UINT flags )
{
    static int (WINAPI *w)( HDC, LPCWSTR, int, RECT *, UINT );
    char a[ 1024 ];   /* the longest text (server unreachable / update notice) is 600 characters */
    if ( !w ) w = (void *)user32_w( "DrawTextW" );
    if ( w ) return w( dc, text, -1, r, flags );
    a[ sizeof(a) - 1 ] = 0;
    WideCharToMultiByte( CP_ACP, 0, text, -1, a, sizeof(a) - 1, NULL, NULL );
    return DrawTextA( dc, a, -1, r, flags );
}

static void set_text( HWND wnd, const WCHAR *text )
{
    BOOL (WINAPI *set_w)( HWND, LPCWSTR ) = (void *)user32_w( "SetWindowTextW" );
    char a[ 1024 ];
    if ( set_w ) { set_w( wnd, text ); return; }
    a[ sizeof(a) - 1 ] = 0;
    WideCharToMultiByte( CP_ACP, 0, text, -1, a, sizeof(a) - 1, NULL, NULL );
    SetWindowTextA( wnd, a );
}

static RECT dlu_rect( HWND dlg, int x, int y, int cx, int cy )
{
    RECT r = { x, y, x + cx, y + cy };
    MapDialogRect( dlg, &r );
    return r;
}

static void frame( HDC dc, RECT r, COLORREF c, int width )
{
    HBRUSH b = CreateSolidBrush( c );
    int i;
    for ( i = 0; i < width; i++ ) { FrameRect( dc, &r, b ); InflateRect( &r, -1, -1 ); }
    DeleteObject( b );
}

/* The text block is planned in dialog units, but how tall the text really is depends on the font this
 * PC has, its DPI and the text itself (the update note comes from the server): measure it, and if it
 * needs more room move everything below it down and grow the box. Nothing is ever cut off.
 * text_ctl: the control that shows the text (plain box), NULL when we paint it ourselves. */
static BOOL CALLBACK move_down( HWND child, LPARAM lp )
{
    LPARAM *a = (LPARAM *)lp;   /* dialog, the text control, pixels */
    RECT r;
    if ( GetParent( child ) != (HWND)a[ 0 ] ) return TRUE;
    GetWindowRect( child, &r );
    MapWindowPoints( NULL, (HWND)a[ 0 ], (POINT *)&r, 2 );
    if ( child == (HWND)a[ 1 ] )
        SetWindowPos( child, NULL, 0, 0, r.right - r.left, r.bottom - r.top + (int)a[ 2 ], SWP_NOMOVE | SWP_NOZORDER );
    else
        SetWindowPos( child, NULL, r.left, r.top + (int)a[ 2 ], 0, 0, SWP_NOSIZE | SWP_NOZORDER );
    return TRUE;
}

static void fit_text( HWND dlg, struct pw_dialog *d, HFONT font, HWND text_ctl )
{
    RECT have = dlu_rect( dlg, d->x_text, d->y_text, d->cx - 2 * d->x_text, d->text_h ), need = have, wr;
    HDC dc = GetDC( dlg );
    HGDIOBJ old = SelectObject( dc, font );
    LPARAM args[ 3 ];
    int want;

    need.bottom = need.top;
    draw_text( dc, d->text, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX );
    SelectObject( dc, old );
    ReleaseDC( dlg, dc );
    want = need.bottom - need.top + 2;
    d->grow = want > have.bottom - have.top ? want - (have.bottom - have.top) : 0;
    okp_log( 2, "box: the text needs %d px, the layout planned %ld px - %s%d px", want, have.bottom - have.top,
             d->grow ? "box grown by " : "fits, spare ", d->grow ? d->grow : (int)(have.bottom - have.top) - want );
    if ( !d->grow ) return;
    args[ 0 ] = (LPARAM)dlg; args[ 1 ] = (LPARAM)text_ctl; args[ 2 ] = d->grow;
    EnumChildWindows( dlg, move_down, (LPARAM)args );
    GetWindowRect( dlg, &wr );
    SetWindowPos( dlg, NULL, wr.left, wr.top - d->grow / 2, wr.right - wr.left, wr.bottom - wr.top + d->grow,
                  SWP_NOZORDER );
}

/* both kinds of box, once laid out: in front, focus on the field (or the first button).
 * Tests: OKP_TEST_BOX_CLOSE_MS=<ms> closes a box by itself (as Cancel) so its layout can be checked. */
static void box_shown( HWND dlg, struct pw_dialog *d )
{
    const char *ms = getenv( "OKP_TEST_BOX_CLOSE_MS" );
    int def = d->cancel && d->cancel_default ? IDCANCEL : IDOK;
    SendMessageA( dlg, DM_SETDEFID, def, 0 );
    SetWindowPos( dlg, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE );
    SetForegroundWindow( dlg );
    SetFocus( GetDlgItem( dlg, d->edit ? 101 : def ) );
    if ( ms && atoi( ms ) > 0 ) SetTimer( dlg, 1, atoi( ms ), NULL );
}

static void read_typed( HWND dlg, struct pw_dialog *d )
{
    UINT (WINAPI *get_w)( HWND, int, LPWSTR, int ) = (void *)user32_w( "GetDlgItemTextW" );
    if ( !d->edit ) return;
    if ( get_w ) get_w( dlg, 101, d->typed, ARRAY_SIZE( d->typed ) );
    else
    {
        char a[ ARRAY_SIZE( d->typed ) ];
        GetDlgItemTextA( dlg, 101, a, sizeof(a) );
        MultiByteToWideChar( CP_ACP, 0, a, -1, d->typed, ARRAY_SIZE( d->typed ) );
        SecureZeroMemory( a, sizeof(a) );
    }
}

static void pw_paint( HWND dlg, struct pw_dialog *d )
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint( dlg, &ps );
    RECT rc, r;

    GetClientRect( dlg, &rc );
    frame( dc, rc, C_EDGE, 1 );
    InflateRect( &rc, -1, -1 );
    frame( dc, rc, C_FRAME, 2 );
    InflateRect( &rc, -3, -3 );
    frame( dc, rc, C_EDGE, 1 );

    SetBkMode( dc, TRANSPARENT );
    SetTextColor( dc, C_TITLE );
    r = dlu_rect( dlg, 8, 3, d->cx - 16, 22 );
    SelectObject( dc, d->title_font );
    draw_text( dc, d->title, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
    r = dlu_rect( dlg, d->x_text, d->y_text, d->cx - 2 * d->x_text, d->text_h );
    r.bottom += d->grow;
    SelectObject( dc, d->text_font );
    SetTextColor( dc, C_TEXT );
    draw_text( dc, d->text, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX );   /* centred over the field */
    if ( d->edit )   /* a tan edge around the black field, like AoE2's */
    {
        GetWindowRect( GetDlgItem( dlg, 101 ), &r );
        MapWindowPoints( NULL, dlg, (POINT *)&r, 2 );
        InflateRect( &r, 2, 2 );
        frame( dc, r, C_FRAME, 2 );
    }
    EndPaint( dlg, &ps );
}

static void pw_draw_item( HWND dlg, struct pw_dialog *d, const DRAWITEMSTRUCT *di )
{
    HDC dc = di->hDC;
    RECT r = di->rcItem;

    SetBkMode( dc, TRANSPARENT );
    if ( di->CtlID == PW_NOASK )
    {
        /* the panel behind it, lined up with the dialog's texture */
        POINT o = { 0, 0 };
        RECT box, label;
        MapWindowPoints( di->hwndItem, dlg, &o, 1 );
        SetBrushOrgEx( dc, -o.x, -o.y, NULL );
        FillRect( dc, &r, d->panel );
        /* the box and its label, centred together under the password field */
        SelectObject( dc, d->small_font );
        {
            TEXTMETRICA tm;
            RECT m = { 0, 0, 0, 0 };
            int bs, gap, x0, y0;
            GetTextMetricsA( dc, &tm );
            bs = tm.tmAscent + 4;                   /* box size follows the font (+ its 2 px edge) */
            gap = bs / 2;
            m.right = (r.right - r.left) - bs - gap - 4;
            draw_text( dc, d->check, &m, DT_CALCRECT | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX );
            {   /* the box lines up with the left edge of the first button (Host) */
                RECT a = { d->x_check, 0, d->x_buttons, 0 };
                MapDialogRect( dlg, &a );
                x0 = r.left + (a.right - a.left);
            }
            y0 = r.top + ((r.bottom - r.top) - (m.bottom - m.top)) / 2;
            box.left = x0;
            box.top = r.top + ((r.bottom - r.top) - bs) / 2;
            box.right = box.left + bs;
            box.bottom = box.top + bs;
            label.left = box.right + gap;
            label.top = y0;
            label.right = label.left + (m.right - m.left);
            label.bottom = y0 + (m.bottom - m.top);
        }
        { HBRUSH f = CreateSolidBrush( C_FIELD ); FillRect( dc, &box, f ); DeleteObject( f ); }
        frame( dc, box, C_FRAME, 2 );   /* the same 2 px frame as the field and the buttons */
        if ( d->checked )
        {
            HPEN pen = CreatePen( PS_SOLID, 2, C_CHECK ), old = SelectObject( dc, pen );
            int w = box.right - box.left;
            MoveToEx( dc, box.left + w / 5, box.top + w / 2, NULL );
            LineTo( dc, box.left + w * 2 / 5, box.bottom - w / 4 );
            LineTo( dc, box.right - w / 6, box.top + w / 5 );
            SelectObject( dc, old );
            DeleteObject( pen );
        }
        SetTextColor( dc, C_TEXT );
        draw_text( dc, d->check, &label, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX );
        if ( di->itemState & ODS_FOCUS ) { RECT f = box; InflateRect( &f, 2, 2 ); DrawFocusRect( dc, &f ); }
        return;
    }
    {   /* Host / No password / Join / Cancel / OK */
        HBRUSH fill = CreateSolidBrush( (di->itemState & ODS_SELECTED) ? C_BUTTONDOWN : C_BUTTON );
        const WCHAR *text = di->CtlID == IDOK ? d->ok : di->CtlID == PW_THIRD ? d->third : d->cancel;
        FillRect( dc, &r, fill );
        DeleteObject( fill );
        frame( dc, r, C_EDGE, 1 );
        InflateRect( &r, -1, -1 );
        frame( dc, r, C_FRAME, 2 );
        if ( di->itemState & ODS_SELECTED ) OffsetRect( &r, 1, 1 );
        SetTextColor( dc, C_BUTTONTEXT );
        SelectObject( dc, d->button_font );
        draw_text( dc, text, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX );
        if ( di->itemState & ODS_FOCUS ) { InflateRect( &r, -4, -4 ); DrawFocusRect( dc, &r ); }
    }
}

static INT_PTR CALLBACK pw_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct pw_dialog *d = (struct pw_dialog *)GetWindowLongPtrA( dlg, DWLP_USER );
    switch ( msg )
    {
    case WM_INITDIALOG:
        SetWindowLongPtrA( dlg, DWLP_USER, lp );
        d = (struct pw_dialog *)lp;
        d->panel = make_panel_brush();
        d->field = CreateSolidBrush( C_FIELD );
        d->title_font = make_font( gothic_faces, 28, FW_BOLD );
        d->text_font = make_font( text_faces, 15, FW_BOLD );
        d->small_font = make_font( text_faces, 13, FW_BOLD );
        d->button_font = make_font( gothic_faces, 19, FW_BOLD );
        set_text( dlg, d->title );   /* for the taskbar and Alt+Tab */
        if ( d->edit )
        {
            SendDlgItemMessageA( dlg, 101, WM_SETFONT, (WPARAM)d->text_font, FALSE );
            SendDlgItemMessageA( dlg, 101, EM_LIMITTEXT, ARRAY_SIZE( d->typed ) - 1, 0 );
        }
        fit_text( dlg, d, d->text_font, NULL );
        box_shown( dlg, d );
        return FALSE;
    case WM_TIMER:
        EndDialog( dlg, IDCANCEL );
        return TRUE;
    case WM_ERASEBKGND:
    {
        RECT rc;
        GetClientRect( dlg, &rc );
        SetBrushOrgEx( (HDC)wp, 0, 0, NULL );
        FillRect( (HDC)wp, &rc, d->panel );
        SetWindowLongPtrA( dlg, DWLP_MSGRESULT, TRUE );
        return TRUE;
    }
    case WM_PAINT:
        pw_paint( dlg, d );
        return TRUE;
    case WM_CTLCOLOREDIT:
        SetTextColor( (HDC)wp, C_FIELDTEXT );
        SetBkColor( (HDC)wp, C_FIELD );
        return (INT_PTR)d->field;
    case WM_DRAWITEM:
        pw_draw_item( dlg, d, (const DRAWITEMSTRUCT *)lp );
        return TRUE;
    case WM_NCHITTEST:   /* drag the panel anywhere */
        SetWindowLongPtrA( dlg, DWLP_MSGRESULT, HTCAPTION );
        return TRUE;
    case WM_COMMAND:
        if ( LOWORD( wp ) == PW_NOASK && HIWORD( wp ) == BN_CLICKED )
        {
            d->checked = !d->checked;
            InvalidateRect( GetDlgItem( dlg, PW_NOASK ), NULL, TRUE );
            return TRUE;
        }
        if ( LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL || LOWORD( wp ) == PW_THIRD )
        {
            read_typed( dlg, d );
            EndDialog( dlg, LOWORD( wp ) );
            return TRUE;
        }
        break;
    case WM_DESTROY:
        DeleteObject( d->panel );
        DeleteObject( d->field );
        DeleteObject( d->title_font );
        DeleteObject( d->text_font );
        DeleteObject( d->small_font );
        DeleteObject( d->button_font );
        break;
    }
    return FALSE;
}

/* the dialog is built in memory (no resource file) */
static WORD *dlg_align( WORD *p ) { return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }
static WORD *dlg_str( WORD *p, const WCHAR *s ) { while ( (*p++ = *s++) ); return p; }
static WORD *dlg_item( WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id,
                       WORD cls, const WCHAR *text )
{
    DLGITEMTEMPLATE *it = (DLGITEMTEMPLATE *)dlg_align( p );
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xffff; *p++ = cls;              /* 0x80 button, 0x81 edit */
    p = dlg_str( p, text );
    *p++ = 0;                               /* no creation data */
    return p;
}

static INT_PTR run_dialog( DLGTEMPLATE *t, DLGPROC proc, struct pw_dialog *d )
{
    /* DLGTEMPLATE strings are WCHAR for both calls; 9x only creates ANSI dialogs */
    INT_PTR (WINAPI *box_w)( HINSTANCE, LPCDLGTEMPLATEW, HWND, DLGPROC, LPARAM ) =
        (void *)user32_w( "DialogBoxIndirectParamW" );
    HWND owner = GetActiveWindow();
    /* on top and in front: when there is no window to own the box, and always for okplay wsock (games
     * that draw full screen over everything; okbox_set_topmost) */
    if ( !owner || okbox_top ) t->style |= DS_SYSMODAL | DS_SETFOREGROUND;
    if ( box_w ) return box_w( okbox_self, t, owner, proc, (LPARAM)d );
    return DialogBoxIndirectParamA( okbox_self, t, owner, proc, (LPARAM)d );
}

/* okplay.ini simple_textboxes=1: the same boxes as plain Windows dialogs - a title bar, the system's
 * dialog font and its ordinary field, checkbox and buttons; nothing drawn by us. Same questions,
 * same answers. */
static INT_PTR CALLBACK simple_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct pw_dialog *d = (struct pw_dialog *)GetWindowLongPtrA( dlg, DWLP_USER );
    switch ( msg )
    {
    case WM_INITDIALOG:
        SetWindowLongPtrA( dlg, DWLP_USER, lp );
        d = (struct pw_dialog *)lp;
        set_text( dlg, d->title );
        set_text( GetDlgItem( dlg, PW_TEXT ), d->text );
        set_text( GetDlgItem( dlg, IDOK ), d->ok );
        if ( d->cancel ) set_text( GetDlgItem( dlg, IDCANCEL ), d->cancel );
        if ( d->third ) set_text( GetDlgItem( dlg, PW_THIRD ), d->third );
        if ( d->check ) set_text( GetDlgItem( dlg, PW_NOASK ), d->check );
        if ( d->edit ) SendDlgItemMessageA( dlg, 101, EM_LIMITTEXT, ARRAY_SIZE( d->typed ) - 1, 0 );
        fit_text( dlg, d, (HFONT)SendMessageA( dlg, WM_GETFONT, 0, 0 ), GetDlgItem( dlg, PW_TEXT ) );
        box_shown( dlg, d );
        return FALSE;
    case WM_TIMER:
        EndDialog( dlg, IDCANCEL );
        return TRUE;
    case WM_COMMAND:
        if ( LOWORD( wp ) == IDOK || LOWORD( wp ) == IDCANCEL || LOWORD( wp ) == PW_THIRD )   /* the title bar's X is Cancel */
        {
            if ( d->check ) d->checked = IsDlgButtonChecked( dlg, PW_NOASK ) == BST_CHECKED;
            read_typed( dlg, d );
            EndDialog( dlg, LOWORD( wp ) );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* simple_textboxes=auto (the default): the styled boxes belong to Age of Empires II - any other
 * game gets the plain ones. 0 / 1 force one kind everywhere. */

static BOOL simple_boxes( void )
{
    static int simple = -1;
    char v[ 16 ];
    if ( simple >= 0 ) return simple;
    okbox_ini_str( "simple_textboxes", "auto", v, sizeof(v) );
    if ( v[ 0 ] == '0' || v[ 0 ] == '1' ) simple = v[ 0 ] == '1';
    else simple = !okbox_is_aoe2();
    okp_log( 1, "boxes: %s (okplay.ini simple_textboxes=%s%s)", simple ? "plain Windows dialogs" : "Age of Empires II style",
             v, v[ 0 ] == '0' || v[ 0 ] == '1' ? "" : okbox_is_aoe2() ? ": this is Age of Empires II" : ": not Age of Empires II" );
    return simple;
}

static INT_PTR show_simple_dialog( struct pw_dialog *d )
{
    WORD buf[ 512 ], *p;
    DLGTEMPLATE *t;
    short y, bw = 62, bh = 14, m = 8;   /* button size, margin */
    int n = 2;

    /* layout, top to bottom (dialog units); the text starts as one line and fit_text grows it */
    d->cx = 236;
    d->x_text = m;
    d->y_text = m;
    d->text_h = 9;
    y = d->y_text + d->text_h + 6;
    d->y_edit = d->edit ? y : 0;
    if ( d->edit ) y += 18;
    d->y_check = d->check ? y : 0;
    if ( d->check ) y += (wcschr( d->check, '\n' ) ? 20 : 10) + 6;
    d->y_buttons = y;
    d->cy = d->y_buttons + bh + m;

    memset( buf, 0, sizeof(buf) );
    t = (DLGTEMPLATE *)dlg_align( buf );
    t->style = DS_SETFONT | DS_CENTER | DS_MODALFRAME | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cx = d->cx; t->cy = d->cy;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    p = dlg_str( p, L"" );                  /* caption and texts are set in WM_INITDIALOG */
    *p++ = 8; p = dlg_str( p, L"MS Shell Dlg" );
    p = dlg_item( p, SS_LEFT | SS_NOPREFIX, m, d->y_text, d->cx - 2 * m, d->text_h, PW_TEXT, 0x82, L"" );
    if ( d->edit )
    {
        p = dlg_item( p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, m, d->y_edit, d->cx - 2 * m, 13, 101, 0x81, L"" );
        n++;
    }
    if ( d->check )
    {
        p = dlg_item( p, BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP | WS_TABSTOP, m, d->y_check, d->cx - 2 * m,
                      wcschr( d->check, '\n' ) ? 20 : 10, PW_NOASK, 0x80, L"" );
        n++;
    }
    if ( d->cancel && d->third )
    {
        short b3 = (d->cx - 2 * m - 8) / 3;
        p = dlg_item( p, (d->cancel_default ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON) | WS_TABSTOP, m, d->y_buttons, b3, bh,
                      IDOK, 0x80, L"" );
        p = dlg_item( p, BS_PUSHBUTTON | WS_TABSTOP, m + b3 + 4, d->y_buttons, b3, bh, PW_THIRD, 0x80, L"" );
        p = dlg_item( p, (d->cancel_default ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | WS_TABSTOP, m + 2 * (b3 + 4),
                      d->y_buttons, b3, bh, IDCANCEL, 0x80, L"" );
        n += 2;
    }
    else if ( d->cancel )
    {
        p = dlg_item( p, (d->cancel_default ? BS_PUSHBUTTON : BS_DEFPUSHBUTTON) | WS_TABSTOP, d->cx - m - 2 * bw - 4,
                      d->y_buttons, bw, bh, IDOK, 0x80, L"" );
        p = dlg_item( p, (d->cancel_default ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | WS_TABSTOP, d->cx - m - bw,
                      d->y_buttons, bw, bh, IDCANCEL, 0x80, L"" );
        n++;
    }
    else
        p = dlg_item( p, BS_DEFPUSHBUTTON | WS_TABSTOP, d->cx / 2 - bw / 2, d->y_buttons, bw, bh, IDOK, 0x80, L"" );
    t->cdit = n;
    return run_dialog( t, simple_proc, d );
}

INT_PTR okbox_show( struct pw_dialog *d )
{
    WORD buf[ 1024 ], *p;
    DLGTEMPLATE *t;
    short y, bw = 104;   /* room for "No Password" in the gothic font */
    int n = 1;

    if ( simple_boxes() ) return show_simple_dialog( d );

    /* layout, top to bottom (dialog units) */
    d->cx = d->third ? 3 * bw + 24 + 40 : 280;   /* three buttons need the wider panel */
    d->x_text = 12;
    d->y_text = 28;
    d->text_h = (d->text_cy ? d->text_cy : 26) - (d->edit ? 2 : 0);
    y = d->y_text + (d->text_cy ? d->text_cy : 26);
    d->y_edit = d->edit ? y : 0;
    if ( d->edit ) y += 20;
    d->y_check = d->check ? y : 0;
    if ( d->check ) y += 22;
    d->y_buttons = y + 4;
    d->cy = d->y_buttons + 26;
    d->x_check = 14;
    d->x_buttons = d->third ? (d->cx - (3 * bw + 24)) / 2 : d->cancel ? d->cx / 2 - bw - 6 : d->cx / 2 - bw / 2;

    memset( buf, 0, sizeof(buf) );
    t = (DLGTEMPLATE *)dlg_align( buf );
    t->style = DS_SETFONT | DS_CENTER | WS_POPUP;
    t->cx = d->cx; t->cy = d->cy;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    p = dlg_str( p, L"" );                  /* no caption: we draw the title */
    *p++ = 9; p = dlg_str( p, L"MS Shell Dlg" );
    if ( d->edit )
    {
        p = dlg_item( p, ES_AUTOHSCROLL | WS_TABSTOP, 14, d->y_edit, d->cx - 28, 14, 101, 0x81, L"" );
        n++;
    }
    if ( d->check )
    {
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, 14, d->y_check, d->cx - 28, 20, PW_NOASK, 0x80, L"" );
        n++;
    }
    if ( d->cancel && d->third )
    {
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->x_buttons, d->y_buttons, bw, 18, IDOK, 0x80, L"" );
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->x_buttons + bw + 12, d->y_buttons, bw, 18, PW_THIRD, 0x80, L"" );
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->x_buttons + 2 * (bw + 12), d->y_buttons, bw, 18, IDCANCEL, 0x80, L"" );
        n += 2;
    }
    else if ( d->cancel )
    {
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->cx / 2 - bw - 6, d->y_buttons, bw, 18, IDOK, 0x80, L"" );
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->cx / 2 + 6, d->y_buttons, bw, 18, IDCANCEL, 0x80, L"" );
        n++;
    }
    else
        p = dlg_item( p, BS_OWNERDRAW | WS_TABSTOP, d->cx / 2 - bw / 2, d->y_buttons, bw, 18, IDOK, 0x80, L"" );
    t->cdit = n;
    return run_dialog( t, pw_proc, d );
}

void okbox_brand( WCHAR *out, DWORD size )
{
    char b[ 96 ];
    okbox_ini_str( "brand", "Classic Netplay", b, sizeof(b) );
    MultiByteToWideChar( CP_ACP, 0, b, -1, out, size );
}

/* OKP_PREVIEW_BOXES=1: show the boxes even where tests would answer them (to look at the design) */
BOOL okbox_test_answers( void )
{
    const char *p = getenv( "OKP_PREVIEW_BOXES" );
    return !(p && *p == '1');
}


BOOL okbox_ask_host_password( const GUID *instance, WCHAR *hashed, DWORD size )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    const char *test = okbox_test_answers() ? getenv( "OKP_TEST_HOST_PASSWORD" ) : NULL;
    char mode[ 16 ];

    memset( &d, 0, sizeof(d) );
    if ( test )   /* tests: the password to host with; "<none>" = an open game, no box */
    {
        if ( strcmp( test, "<none>" ) )
            MultiByteToWideChar( CP_ACP, 0, test, -1, d.typed, ARRAY_SIZE( d.typed ) );
    }
    else
    {
        okbox_ini_str( "use_password", "", mode, sizeof(mode) );
        if ( !mode[ 0 ] ) okbox_ini_str( "host_password", "ask", mode, sizeof(mode) );   /* v0.5.0's name */
        if ( !_stricmp( mode, "off" ) )
        {
            okp_log( 2, "password: not asking (use_password=off) - an open game" );
            return FALSE;
        }
        okbox_brand( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = L"Enter a password for this game (optional).\nAn empty entry will allow anyone to join.";
        d.edit = TRUE;
        d.check = L"Disable Hosting Password\n(okplay.ini: use_password=ask to re-enable)";
        d.ok = L"Host";
        d.cancel = L"No Password";
        if ( okbox_show( &d ) != IDOK ) d.typed[ 0 ] = 0;   /* "No password" */
        if ( d.checked )
        {
            WritePrivateProfileStringA( "okplay", "use_password", "off", okbox_ini );
            okp_log( 1, "password: use_password=off written to okplay.ini (the player ticked \"Disable Hosting Password\")" );
        }
    }
    if ( !d.typed[ 0 ] )
    {
        okp_log( 1, "password: none - an open game" );
        return FALSE;
    }
    okbox_hash_password( d.typed, instance, hashed, size );
    SecureZeroMemory( d.typed, sizeof(d.typed) );
    okp_log( 1, "password: this game needs a password (only its salted hash is used)" );
    return TRUE;
}

int okbox_ask_join_password( const GUID *instance, WCHAR *hashed, DWORD size )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    const char *test = okbox_test_answers() ? getenv( "OKP_TEST_JOIN_PASSWORD" ) : NULL;
    INT_PTR r;

    memset( &d, 0, sizeof(d) );
    if ( test )   /* "a|b|c": one answer per join attempt, the last one repeats */
    {
        static int attempt;
        char one[ 128 ];
        const char *s = test, *bar;
        int i;
        for ( i = 0; i < attempt && (bar = strchr( s, '|' )); i++ ) s = bar + 1;
        attempt++;
        lstrcpynA( one, s, sizeof(one) );
        if ( (bar = strchr( one, '|' )) ) *(char *)bar = 0;
        if ( !strcmp( one, "<cancel>" ) ) r = IDCANCEL;
        else { MultiByteToWideChar( CP_ACP, 0, one, -1, d.typed, ARRAY_SIZE( d.typed ) ); r = IDOK; }
    }
    else
    {
        okbox_brand( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = L"This game has a password.\nType it to join:";
        d.edit = TRUE;
        d.ok = L"Join";
        d.cancel = L"Cancel";
        r = okbox_show( &d );
    }
    if ( r != IDOK )
    {
        okp_log( 1, "password: the player cancelled the join" );
        return 0;
    }
    okbox_hash_password( d.typed, instance, hashed, size );
    SecureZeroMemory( d.typed, sizeof(d.typed) );
    okp_log( 1, "password: joining a password game (sending only the salted hash)" );
    return 1;
}

void okbox_wrong_password( void )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
        okp_log( 1, "password: wrong password - the host did not let us in" );
    if ( okbox_test_answers() && getenv( "OKP_TEST_JOIN_PASSWORD" ) ) return;
    memset( &d, 0, sizeof(d) );
    okbox_brand( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = L"Wrong password - the host did not let you in.";
    d.ok = L"OK";
    okbox_show( &d );
}

int okbox_ask_mode( const char *what )
{
    struct pw_dialog d;
    WCHAR title[ 96 ], text[ 400 ], wwhat[ 16 ], online[ 64 ], lan[ 64 ];
    char a[ 64 ];
    const char *test = getenv( "OKP_TEST_CLASSIC_ASK" );
    INT_PTR r;

    if ( test && *test ) return !_stricmp( test, "online" ) ? 2 : !_stricmp( test, "lan" ) ? 3 : 1;
    memset( &d, 0, sizeof(d) );
    okbox_brand( title, ARRAY_SIZE( title ) );
    MultiByteToWideChar( CP_ACP, 0, what, -1, wwhat, ARRAY_SIZE( wwhat ) );
    okbox_ini_str( "online_name", "Classic Netplay Online", a, sizeof(a) ); MultiByteToWideChar( CP_ACP, 0, a, -1, online, ARRAY_SIZE( online ) );
    okbox_ini_str( "lan_name", "Classic Netplay LAN", a, sizeof(a) ); MultiByteToWideChar( CP_ACP, 0, a, -1, lan, ARRAY_SIZE( lan ) );
    _snwprintf( text, ARRAY_SIZE( text ), L"The game is about to start Multiplayer with its \"%ls\" connection.\n\n"
                L"How would you like to proceed?\n\n"
                L"Online Play = %ls\n"
                L"LAN Play = %ls\n"
                L"Original = The game's original %ls connection.", wwhat, online, lan, wwhat );
    text[ ARRAY_SIZE( text ) - 1 ] = 0;
    d.title = title;
    d.text = text;
    d.text_cy = 64;
    d.ok = L"Online Play";
    d.third = L"LAN Play";
    d.cancel = L"Original";
    r = okbox_show( &d );
    return r == IDOK ? 2 : r == PW_THIRD ? 3 : 1;
}

/* ------------------------------------------------------------------ notices and questions
 * The box bodies only: each shows its box right here, on the calling thread, and returns the answer.
 * When to show them (once per game run, on a thread of their own, not over a match) is the caller's:
 * the drop-in's okp_ui_* in builtin.c, okplay wsock's in wsock.c. Moved from builtin.c (2026-11-08). */

static int okbox_ini_int( const char *key, int def )
{
    return GetPrivateProfileIntA( "okplay", key, def, okbox_ini );
}

/* okplay couldn't create any internet socket: TCP/IP isn't installed (or not bound to a network card).
 * Without this box the game just reports a failed session and drops out. */
void okbox_network_unavailable( void )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    memset( &d, 0, sizeof(d) );
    okbox_brand( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = okp_win9x_paths()
        ? L"This PC's TCP/IP networking isn't working, so Online and LAN Play can't start.\n\n"
          L"In Control Panel > Network, TCP/IP must be listed and bound to your network card "
          L"(Add > Protocol > Microsoft > TCP/IP), then restart."
        : L"This PC's TCP/IP networking isn't working, so Online and LAN Play can't start.\n\n"
          L"Check that the network adapter is enabled and has IPv4 turned on.";
    d.text_cy = 64;
    d.ok = L"OK";
    okbox_show( &d );
}

/* Online Play's room server (okplay.php) failed 3 times in a row: say so, with the reason, instead of
 * an empty game list that looks like nobody is playing. server = okplay.ini server=, why = the reason. */
void okbox_server_unreachable( const char *server, const char *why )
{
    struct pw_dialog d;
    char host[ 128 ] = "";
    const char *h = strstr( server, "://" );
    WCHAR title[ 96 ], text[ 600 ], whost[ 128 ], wwhy[ 200 ];
    int i;

    h = h ? h + 3 : server;
    for ( i = 0; h[ i ] && h[ i ] != '/' && h[ i ] != ':' && i < (int)sizeof(host) - 1; i++ ) host[ i ] = h[ i ];
    host[ i ] = 0;
    MultiByteToWideChar( CP_ACP, 0, host, -1, whost, ARRAY_SIZE( whost ) );
    MultiByteToWideChar( CP_ACP, 0, why, -1, wwhy, ARRAY_SIZE( wwhy ) );
    memset( text, 0, sizeof(text) );
    _snwprintf( text, ARRAY_SIZE( text ) - 1, L"Can't reach the Online Play server (%ls).\n\n%ls.\n\n"
                L"Online games can't be listed or hosted until it's reachable. LAN Play still works.", whost, wwhy );
    memset( &d, 0, sizeof(d) );
    okbox_brand( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = text;
    d.text_cy = 78;
    d.ok = L"OK";
    okbox_show( &d );
}

/* The room server's replies carry "latest <version> <note>" for the drop-in and "latest_wsock <version>
 * <note>" for okplay wsock (okplay.php LATEST_VERSION/LATEST_NOTE, LATEST_WSOCK_*). When it is newer than
 * this edition, the update box offers the download - never blocking: games, hosting and joining go on as
 * before. okplay.ini: update_check=0 turns it off; update_seen=<version> is written by "Don't remind me
 * about this version". The Download button opens the server's own folder (from server=), never an address
 * the server sends. Tests: OKP_TEST_UPDATE=download|later|never answers without a box (and opens nothing). */

int okbox_version_cmp( const char *a, const char *b )   /* "0.7.10" > "0.7.9" */
{
    while ( *a || *b )
    {
        long x = strtol( a, (char **)&a, 10 ), y = strtol( b, (char **)&b, 10 );
        if ( x != y ) return x < y ? -1 : 1;
        if ( *a == '.' ) a++;
        if ( *b == '.' ) b++;
        if ( (*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9')) ) break;
    }
    return 0;
}

int okbox_update_wanted( const char *latest, const char *current )
{
    char seen[ 24 ];

    if ( !latest || !*latest || okbox_version_cmp( latest, current ) <= 0 ) return 0;
    if ( !okbox_ini_int( "update_check", 1 ) )
    {
        okp_log( 1, "update: v%s is available (you have v%s); not asking (update_check=0)", latest, current );
        return 0;
    }
    okbox_ini_str( "update_seen", "", seen, sizeof(seen) );
    if ( seen[ 0 ] && okbox_version_cmp( latest, seen ) <= 0 )
    {
        okp_log( 1, "update: v%s is available (you have v%s); not asking (update_seen=%s)", latest, current, seen );
        return 0;
    }
    okp_log( 1, "update: v%s is available (you have v%s) - offering it", latest, current );
    return 1;
}

/* UTF-8 -> WCHAR without the system (Windows 95 has no CP_UTF8); control characters become spaces */
static void utf8_to_wide( const char *in, WCHAR *out, int outsz )
{
    const unsigned char *s = (const unsigned char *)in;
    int o = 0;
    while ( *s && o < outsz - 1 )
    {
        unsigned int c = *s++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if ( more ) c &= 0x3f >> more;
        while ( more-- > 0 && (*s & 0xc0) == 0x80 ) c = (c << 6) | (*s++ & 0x3f);
        if ( c < 0x20 ) c = ' ';
        if ( c > 0xffff )
        {
            if ( o + 2 >= outsz ) break;
            c -= 0x10000;
            out[ o++ ] = (WCHAR)(0xd800 + (c >> 10));
            c = 0xdc00 + (c & 0x3ff);
        }
        out[ o++ ] = (WCHAR)c;
    }
    out[ o ] = 0;
}

void okbox_offer_update( const char *latest, const char *note_utf8, const char *current )
{
    struct pw_dialog d;
    WCHAR title[ 96 ], text[ 700 ], note[ 200 ], wlatest[ 24 ], wcurrent[ 24 ], wurl[ 300 ];
    const char *test = getenv( "OKP_TEST_UPDATE" );
    char url[ 300 ], *slash;
    /* Windows 9x: no Download button. A browser opened over a full-screen game sends the game to the desktop,
     * and Road Rash can't come back from that (black screen); the address is shown instead */
    int download, no_browser = okp_win9x_paths();

    memset( &d, 0, sizeof(d) );
    /* the download page = the room server's own folder (https://oldkid.net/aoe2net/ for okplay.php);
     * Windows 9x browsers can't do today's HTTPS, so they get http:// */
    okbox_ini_str( "server", "https://oldkid.net/aoe2net/okplay.php", url, sizeof(url) - 8 );
    if ( (slash = strrchr( url, '/' )) && slash > strstr( url, "://" ) + 2 ) slash[ 1 ] = 0;
    if ( no_browser && !_strnicmp( url, "https://", 8 ) ) memmove( url + 4, url + 5, strlen( url + 5 ) + 1 );
    MultiByteToWideChar( CP_ACP, 0, url, -1, wurl, ARRAY_SIZE( wurl ) );
    utf8_to_wide( note_utf8 ? note_utf8 : "", note, ARRAY_SIZE( note ) );
    MultiByteToWideChar( CP_ACP, 0, latest, -1, wlatest, ARRAY_SIZE( wlatest ) );
    MultiByteToWideChar( CP_ACP, 0, current, -1, wcurrent, ARRAY_SIZE( wcurrent ) );
    _snwprintf( text, ARRAY_SIZE( text ), L"A newer version is available: v%ls\n(you have v%ls)\n\n%ls%ls"
                L"You can keep playing. If you run into connection problems, please update%ls%ls",
                wlatest, wcurrent, note, note[ 0 ] ? L"\n\n" : L"",
                no_browser ? L" - download it from\n" : L".", no_browser ? wurl : L"" );
    text[ ARRAY_SIZE( text ) - 1 ] = 0;

    if ( test && *test )
    {
        download = !strcmp( test, "download" ) && !no_browser;
        d.checked = !strcmp( test, "never" );
    }
    else
    {
        okbox_brand( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = text;
        d.text_cy = (note[ 0 ] ? 78 : 56) + (no_browser ? 18 : 0);
        d.check = L"Don't remind me about this version";
        if ( no_browser )
            d.ok = L"OK";
        else
        {
            d.ok = L"Download";
            d.cancel = L"Not Now";
            d.cancel_default = TRUE;   /* a key meant for the game must not open a browser over it */
        }
        download = okbox_show( &d ) == IDOK && !no_browser;
    }
    if ( d.checked )
    {
        WritePrivateProfileStringA( "okplay", "update_seen", latest, okbox_ini );
        okp_log( 1, "update: v%s - the player chose not to be reminded (okplay.ini update_seen)", latest );
    }
    if ( download )
    {
        okp_log( 1, "update: v%s - the player chose Download (%s)", latest, url );
        if ( !(test && *test) )
        {
            HMODULE shell = LoadLibraryA( "shell32.dll" );
            HINSTANCE (WINAPI *open)( HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT ) =
                shell ? (void *)GetProcAddress( shell, "ShellExecuteA" ) : NULL;
            if ( !open || (INT_PTR)open( NULL, "open", url, NULL, NULL, SW_SHOWNORMAL ) <= 32 )
                okp_log( 0, "update: could not open %s in the browser", url );
        }
    }
    else if ( no_browser && !d.checked )
        okp_log( 1, "update: v%s - shown with its address (%s; Windows 9x: no browser over the game)", latest, url );
    else if ( !d.checked )
        okp_log( 1, "update: v%s - the player chose Not Now", latest );
}

/* A crash report from the last run is waiting: ask before sending it. "View report" opens it in
 * Notepad and asks again. 0 = don't send (the report is deleted), 1 = send, 2 = send, and from now
 * on without asking. */
int okbox_ask_crash_report( const char *path )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    INT_PTR r;

    for (;;)
    {
        memset( &d, 0, sizeof(d) );
        okbox_brand( title, ARRAY_SIZE( title ) );
        d.title = title;
        d.text = L"The game closed unexpectedly last time.\n\n"
                 L"Send a crash report to oldkid.net to help fix it? It says where the crash happened and "
                 L"which versions of the game, Windows and this add-on you have. "
                 L"It never contains player names, chat or addresses.";
        d.text_cy = 78;
        d.check = L"Always send crash reports without asking";
        d.ok = L"Send";
        d.third = L"View Report";
        d.cancel = L"Don't Send";
        d.cancel_default = TRUE;   /* sending needs a real click, not a key meant for the game */
        r = okbox_show( &d );
        if ( r != PW_THIRD ) break;
        {
            char cmd[ MAX_PATH + 32 ];
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            memset( &si, 0, sizeof(si) );
            si.cb = sizeof(si);
            snprintf( cmd, sizeof(cmd), "notepad.exe \"%s\"", path );
            if ( CreateProcessA( NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ) )
            {
                WaitForSingleObject( pi.hProcess, INFINITE );   /* ask again once it's closed */
                CloseHandle( pi.hThread );
                CloseHandle( pi.hProcess );
            }
        }
    }
    okp_log( 1, "report: crash report box - the player chose %s%s", r == IDOK ? "Send" : "Don't Send",
             r == IDOK && d.checked ? " (always)" : "" );
    return r == IDOK ? (d.checked ? 2 : 1) : 0;
}

/* Connection reports were never asked about (connection_reports=ask): 1 = yes (report.c writes
 * connection_reports=yes), 0 = no. */
int okbox_ask_conn_reports( void )
{
    struct pw_dialog d;
    WCHAR title[ 96 ];
    INT_PTR r;

    memset( &d, 0, sizeof(d) );
    okbox_brand( title, ARRAY_SIZE( title ) );
    d.title = title;
    d.text = L"Help improve Online Play?\n\n"
             L"After each game, send oldkid.net a short summary of how well it connected: whether joining "
             L"worked, the ping, and how many messages had to be resent. "
             L"No player names, chat or addresses. You can change this later in okplay.ini (connection_reports=).";
    d.text_cy = 86;
    d.ok = L"Yes, Send";
    d.cancel = L"No Thanks";
    d.cancel_default = TRUE;
    r = okbox_show( &d );
    okp_log( 1, "report: connection reports box - the player chose %s", r == IDOK ? "Yes" : "No" );
    return r == IDOK;
}
