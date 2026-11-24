/*
 * wpanel.c - a variable size strip remote panel for a Behringer WING, using wapi.
 * wpanel v 0.1.1 - (c) 2026 - Patrick-Gilles Maillot
 * Windows (Win32) GUI.
 *
 *   gcc -O3 -Wall -I<wapi include dir> -o wpanel.exe wpanel.c \
 *       -L<wapi lib dir> -lwapi -lws2_32 -lgdi32 -mwindows
 *
 * Layout: 12 strips in three groups of four, modelled on the console's own
 * surface. A row of bank buttons in the top bar chooses which sources the strips
 * show: CH 1-12, CH 13-24, CH 25-36, CH 37-40, AUX 1-8, BUS 1-8, BUS 9-16, MAIN 1-4 or MTX 1-8. Each strip has:
 *   - scribble strip: number, name and colour
 *   - SELECT button (click to select that source on the console)
 *   - SOLO button (click to toggle solo)
 *   - MUTE button (click to mute / unmute; red = muted)
 *   - fader with the WING dB scale and a numeric readout: drag the handle (or
 *     click the track) to set the level, double-click it for 0 dB
 *   - name: double-click the scribble strip to rename (Enter or clicking away
 *     to apply, Esc to cancel)
 * Everything is two-way: changes made on the WING show up here and vice versa.
 * The source currently selected on the console is highlighted and reported in
 * the top bar, whichever bank is showing.
 *
 * Finding the console: with an empty address box, Connect (or Find) calls
 * wOpen(""), which searches the network, connects to the first WING found and
 * returns its IP address (shown in the box).
 *
 * Changes made on the WING are pushed to the app by the console and picked up
 * by the wapi event loop (wGetParsedEventsTimed). A full resync of the visible
 * strips runs every few seconds as a safety net.
 *
 * History:
 * v 0.1.0: Initial commit
 * v 0.1.1: Small syntax changes to make code more readable
 *          Set test conditions according to wapi standards
 *          Meters connection port & ID based on time rather than process ID as this would conduct at
 *            non working meters when disconnecting/reconnecting within the same process
 *          Better definitions of size & position of buttons and GUI elements
 */
#include <math.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "../wapi/wapi.h"
#include "../wapi/wext.h"

/* ---- configuration ------------------------------------------------------ */
#define TICK_MS        25        /* event loop period                        */
#define EVENT_WAIT_US  1000      /* max time one tick waits for events (µs)  */
#define MAX_EVENTS     100       /* wapi events buffer size                  */
#define RESYNC_MS      5000      /* full re-read of every visible value      */
#define RESYNC_WAIT_US 50000     /* per-token wait during a resync (µs)      */
#define S_STRINGS      24        /* size of strings arrays                   */

/* meters (wapi metering API) */
#define METER_RENEW_MS 4000      /* the WING stops sending after 5 s          */
#define METER_REPAINT_MS 40      /* at most one meter repaint per this many ms */
#define METER_WAIT_US  100       /* wait per read when polling for meter data */
#define METER_FALL_DB  3.0f      /* how far a level falls per frame (~50 ms)  */
#define CLIP_HOLD_MS  500        /* the CLIP LED stays lit this long after a peak */
#define CLIP_DB       (-1.0f)    /* level that counts as clipping              */

/* ---- every source the panel can show: CH 1..40, AUX 1..8, BUS 1..16, MAIN 1..4, MTX 1..8 ------------ */
/* Source number s equals the console's own strip index (0 = CH 1 ... 39 = CH 40,
 * 40 = AUX 1 ... 47 = AUX 8, 48..63 = BUS 1..16, 64..67 = MAIN 1..4, 68..75 = MTX 1..8), which is also what the selected-strip token uses. */
typedef struct {
	wtoken mute, solo, name, col, fdr, dname, dyn_on, gate_on, clink;
} Source;
#define NSRC 76
static Source SRC[NSRC];
#define HAS_GATE(s) ((s) < 40)       /* only the input channels have a gate */
#define HAS_LINK(s) ((s) < 48)       /* "link to source" exists on channels and aux only */
#define CLINK_LINKED 1               /* value of the clink token when linked to the source
                                      * (clink = "custom link"; with the value below the padlock is drawn closed) */
/* The tokens of each source family (CH, AUX, BUS, MAIN, MTX) are laid out at a
 * constant distance in wapi.h's enum: CH_2_MUTE - CH_1_MUTE is the same for every
 * channel and for every field. So each family is described by its first two
 * strips, and the table is filled in with a loop at start-up. The static asserts
 * check the last strip of each family, so a different wapi.h cannot silently
 * produce wrong tokens (the build would stop instead). */

#define FIELDS(P, N) {P##_##N##_MUTE, P##_##N##_$SOLO, P##_##N##_NAME, P##_##N##_COL, \
                      P##_##N##_FDR,  P##_##N##_$NAME, P##_##N##_DYN_ON, GATE_##P(N), LINK_##P(N)}
#define GATE_CH(N)     CH_##N##_GATE_ON         /* only input channels have a gate; */
#define GATE_AUX(N)   AUX_##N##_DYN_ON          /* the others reuse the DYN token   */
#define GATE_BUS(N)   BUS_##N##_DYN_ON          /* (it is never used, see HAS_GATE) */
#define GATE_MAIN(N) MAIN_##N##_DYN_ON
#define GATE_MTX(N)   MTX_##N##_DYN_ON
#define LINK_CH(N)     CH_##N##_CLINK           /* same idea for the source link:   */
#define LINK_AUX(N)   AUX_##N##_CLINK           /* only CH and AUX have it          */
#define LINK_BUS(N)   BUS_##N##_DYN_ON
#define LINK_MAIN(N) MAIN_##N##_DYN_ON
#define LINK_MTX(N)   MTX_##N##_DYN_ON

/* Check that strip L of family P sits where the stride of strips 1 and 2 says. */
#define CHECK_FIELD(P, L, F) \
    _Static_assert((int)P##_##L##_##F == (int)P##_1_##F + ((L) - 1) * \
                  ((int)P##_2_##F - (int)P##_1_##F), #P " tokens are not evenly spaced in wapi.h")
#define CHECK_FAMILY(P, L) \
    CHECK_FIELD(P, L, MUTE); CHECK_FIELD(P, L, $SOLO); CHECK_FIELD(P, L, NAME); \
    CHECK_FIELD(P, L, COL);  CHECK_FIELD(P, L, FDR);   CHECK_FIELD(P, L, $NAME); \
    CHECK_FIELD(P, L, DYN_ON)

CHECK_FAMILY(CH, 40);
CHECK_FAMILY(AUX, 8);
CHECK_FAMILY(BUS, 16);
CHECK_FAMILY(MAIN, 4);
CHECK_FAMILY(MTX, 8);
CHECK_FIELD(CH, 40, GATE_ON);
CHECK_FIELD(CH, 40, CLINK);
CHECK_FIELD(AUX, 8, CLINK);


/* Fill SRC[first .. first+count-1] from the first two strips of a family. */
static void add_family(int first, int count, Source a, Source b) {
	wtoken *pa = (wtoken*) &a, *pb = (wtoken*) &b;
	int k, f;

	for (k = 0; k < count; k++) {
		wtoken *out = (wtoken*) &SRC[first + k];
		for (f = 0; f < (int) (sizeof(Source) / sizeof(wtoken)); f++)
			out[f] = (wtoken) ((int) pa[f] + k * ((int) pb[f] - (int) pa[f]));
	}
}

static void init_sources(void) {
#define FAMILY(P, FIRST, COUNT) \
    add_family(FIRST, COUNT, (Source)FIELDS(P, 1), (Source)FIELDS(P, 2))
	FAMILY(CH, 0, 40);   /* source 0..39  = CH 1..40  */
	FAMILY(AUX, 40, 8);  /*        40..47 = AUX 1..8  */
	FAMILY(BUS, 48, 16); /*        48..63 = BUS 1..16 */
	FAMILY(MAIN, 64, 4); /*        64..67 = MAIN 1..4 */
	FAMILY(MTX, 68, 8);  /*        68..75 = MTX 1..8  */
#undef FAMILY
}

/* ---- banks: which sources the strips are showing ---------------------- */
typedef struct {
	const char *label;
	int base, count;
} Bank;
static const Bank BANKS[] = {
		{"CH 1-12", 0, 12}, {"CH 13-24", 12, 12}, {"CH 25-36", 24, 12},
		{"CH 37-40", 36, 4},
		{"CH 1-16", 0, 16}, {"CH 17-32", 16, 16}, {"CH 33-40", 32, 8},
		{"AUX 1-8", 40, 8},
		{"BUS 1-8", 48, 8}, {"BUS 9-16", 56, 8}, {"BUS 1-16", 48, 16},
		{"MAIN 1-4", 64, 4}, {"MTX 1-8", 68, 8},};
#define NBANK sizeof(BANKS) / sizeof (Bank)

#define GROUP  4
#define NSTRIP 4 * GROUP /* max number of strips shown at once (4 groups of 4) */

/* ---- layout (pixels) ----------------------------------------------------- */
#define WPANEL_VERSION "0.1.1"
#define WPANEL_TITLE   "wpanel - (c) 2026 - Patrick-Gilles Maillot - v " WPANEL_VERSION

#define STRIP_W      90
#define GROUP_GAP    9 
#define H_BTN        28
#define X_MARGIN     14
#define Y_MARGIN     8
#define BTN_X        8
#define BTN_W        (STRIP_W - 2 * BTN_X)
#define LOCK_X       70           /* padlock on the scribble strip: offset from the strip x */
#define LOCK_W       10
#define LOCK_H       13
#define Y_SCRIB      48           /* scribble button Y position                */
#define H_SCRIB      60           /* scribble panel height                     */
#define Y_SEL        (Y_SCRIB + H_SCRIB + Y_MARGIN)
#define H_SEL        H_BTN        /* SEL button height                         */
#define Y_SOLO       (Y_SEL + H_SEL + Y_MARGIN)
#define H_SOLO       H_BTN        /* SOLO button height                        */
#define Y_METER      (Y_SOLO + H_SOLO + Y_MARGIN)
#define H_METER      96           /* meter panel height                        */
#define Y_MUTE       (Y_METER + H_METER + Y_MARGIN)
#define H_MUTE       38           /* MUTE button height                        */

#define H_FDR        210          /* fader height 10 to -oo                    */
#define H_FDR_EXT    18           /* extra area top and bottom of fader 10/-oo */
#define Y_FDR_TOP    (Y_MUTE + H_MUTE + H_FDR_EXT)  /* handle center at +10 dB */
#define Y_FDR_BOT    (Y_FDR_TOP + H_FDR)            /* handle center at -oo    */
#define Y_FDR_READ   (Y_FDR_BOT + H_FDR_EXT)
#define H_FDR_READ   20           /* fader value height                        */
#define Y_STATUS     (Y_FDR_READ + H_FDR_READ + Y_MARGIN / 2)
#define H_STATUS     18           /* status line height                        */
#define STRIP_TOP    (Y_SCRIB - 6)
#define STRIP_BOT    (Y_FDR_READ + H_FDR_READ + 4)

#define BANK_W       84           /* width of the vertical bank buttons at the left */
#define STRIP_X0     (X_MARGIN + BANK_W + 14)   /* x of the first strip */
#define RIGHT_PAD    X_MARGIN     /* space right of the last strip's buttons: same as the left X_MARGIN */
#define BOTTOM_PAD   6            /* space below the status line */

enum {
	ID_IP = 101,
	ID_FIND,
	ID_CONNECT,
	ID_METERMODE,
	ID_STATUS,
	ID_SELLBL,
	ID_TIMER,
	ID_BANK0 = 300
};
#define WM_STARTUP   (WM_APP + 1)

#define C_BG        RGB(204, 201, 194)
#define C_DARK      RGB( 64,  64,  68)
#define C_LIVE      C_DARK    /* same as an inactive SOLO */
#define C_MUTED     RGB(200,  40,  40)
#define C_UNKNOWN   RGB(130, 130, 130)
#define C_SOLO      RGB(245, 205,  50)
#define C_SELECT    RGB(255, 170,  40)

/* WING color indices 1..18, taken from the colour table of the WING Remote
 * Protocols document (page 249). Index 0 is used while a colour is unknown. */
#define NCOLORS 18
static const COLORREF WCOL[NCOLORS + 1] = {RGB(110, 110, 110), /*  0: unknown            */
RGB(75, 100, 207),  /*  1                     */
RGB(61, 130, 255),  /*  2                     */
RGB(94, 60, 255),   /*  3                     */
RGB(85, 204, 209),  /*  4                     */
RGB(69, 176, 58),   /*  5                     */
RGB(158, 202, 0),   /*  6                     */
RGB(238, 221, 0),   /*  7                     */
RGB(182, 107, 33),  /*  8                     */
RGB(207, 42, 67),   /*  9                     */
RGB(241, 125, 123), /* 10                     */
RGB(240, 68, 249),  /* 11                     */
RGB(158, 64, 255),  /* 12                     */
RGB(244, 185, 25),  /* 13                     */
RGB(92, 196, 255),  /* 14                     */
RGB(239, 94, 53),   /* 15                     */
RGB(104, 228, 163), /* 16                     */
RGB(112, 112, 112), /* 17                     */
RGB(224, 224, 224)  /* 18                     */
};

/* Fader law read off the console's scale: dB -> 0..1 handle position. */
static const double SC_DB[] =
		{-90, -60, -50, -40, -30, -20, -10, -5, 0, 5, 10};
static const double SC_T[] = {0, .061, .122, .183, .246, .373, .498, .622,
		.751, .878, 1.0};
#define NSC ((int)(sizeof SC_DB / sizeof SC_DB[0]))
static const int SCALE_LABELS[] = {10, 5, 0, -5, -10, -20, -30, -40, -50, -60};

/* ---- state -------------------------------------------------------------- */
static HWND g_metermode;
static HWND g_wnd, g_ip, g_find, g_connect, g_status, g_sellbl, g_bankbtn[NBANK];
static HFONT g_font, g_bold, g_small, g_smallbold, g_tiny;
static HBRUSH g_bg;
static int g_connected = 0;

static int g_bank = 0;  /* current bank                 */
static int g_base = 0;  /* source shown on strip 0      */
static int g_nvis = 12; /* strips in use in this bank   */

/* per-strip values (indexed by strip 0..11, not by source) */
static int g_mute[NSTRIP], g_solo[NSTRIP], g_col[NSTRIP]; /* -1 = unknown */
static float g_fdr[NSTRIP]; /* dB */
static int g_fdr_ok[NSTRIP];
static char g_name[NSTRIP][S_STRINGS];
static int g_name_dirty[NSTRIP]; /* re-read this name from the console */

/* meters (per strip) */
#define NO_LEVEL (-128.0f)
static int g_link[NSTRIP]; /* link to source: -1 unknown, else the clink value */
static int g_dyn_on[NSTRIP], g_gate_on[NSTRIP]; /* processor enabled: -1 unknown, 0, 1 */
static int g_gate_eng[NSTRIP], g_dyn_eng[NSTRIP]; /* engaged (gating / reducing) now */
static int g_meter_post; /* 0 = PRE fader (input), 1 = POST (output) */
static int g_clip_shown[NSTRIP][2]; /* CLIP LED state last drawn */
static int g_meter_dirty; /* meter values changed, repaint due */
static DWORD g_meter_drawn; /* tick of the last meter repaint   */
static DWORD g_clip_until[NSTRIP][2]; /* tick count until which CLIP stays lit */
static float g_lvl[NSTRIP][2]; /* level in dB (left, right)       */
static float g_gate_gr[NSTRIP], g_dyn_gr[NSTRIP]; /* gain reduction in dB (>= 0)     */
static int g_meter_ok; /* a meter request is active       */
static int g_meter_port_ok; /* UDP port for meter data is set  */
static int g_meter_id; /* id of the current request       */
static DWORD g_meter_renew;

static int g_sel = -1;     /* selected source, 0..75 (0 = CH 1) */
static char g_selname[S_STRINGS]; /* its name, when it is not on screen */
static DWORD g_last_sync, g_keepalive;

/* The wapi tokens of strip i in the current bank. */
#define TOK(i) SRC[g_base + (i)]

/* mouse interaction state */
static int g_drag = -1; /* strip whose fader is being dragged */
static int g_drag_dy; /* grab offset inside the handle (px) */
static float g_drag_sent; /* last dB value sent while dragging  */
static HWND g_edit; /* inline name editor, or NULL        */
static int g_edit_strip = -1;
static int g_edit_commit;
static WNDPROC g_edit_oldproc;

/* discovery results */

static double fader_t(double db); /* defined with the drawing code */
static int fader_y(double t);
static void finish_edit(void);
static void start_meters(void);

/* ---- small helpers ------------------------------------------------------ */
static int strip_x(int i) {
	return STRIP_X0 + i * STRIP_W + (i / GROUP) * GROUP_GAP;
}

/* Client width needed for n strips */
static int client_width(int n) {
	/* right edge of the last strip's buttons, plus a small right X_MARGIN */
	int w = strip_x(n > 0 ? n - 1 : 0) + BTN_X + BTN_W + RIGHT_PAD;
	return w;
}

/* y of bank button k: a small gap between CH / AUX / BUS / MAIN / MTX families */
static int bank_y(int k) {
	static const int fam[5] = {4, 7, 8, 11, 12}; /* first button of each group of banks */
	int j, y = Y_SCRIB + k * 32;
	for (j = 0; j < 5; j++)
		if (k >= fam[j])
			y += 10;
	return y;
}

/* Resize the window so that it fits the strips on screen. */
static void apply_layout(void) {
	DWORD style = (DWORD) GetWindowLong(g_wnd, GWL_STYLE);
	int cw = client_width(g_nvis);
	RECT rc = {0, 0, cw, Y_STATUS + H_STATUS + BOTTOM_PAD};
	AdjustWindowRect(&rc, style, FALSE);
	SetWindowPos(g_wnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
	SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	SetWindowPos(g_status, NULL, 0, 0, cw - 2 * X_MARGIN, H_STATUS, SWP_NOMOVE | SWP_NOZORDER);
	InvalidateRect(g_wnd, NULL, FALSE);
}

static void set_status(const char *msg) {
	SetWindowTextA(g_status, msg);
}

static void invalidate_strip(int i) {
	RECT r;
	r.left = strip_x(i) - 2;
	r.right = strip_x(i) + STRIP_W + 2;
	r.top = STRIP_TOP;
	r.bottom = STRIP_BOT + 2;
	InvalidateRect(g_wnd, &r, FALSE);
}

static void invalidate_all(void) {
	RECT r = {0, STRIP_TOP, 4000, STRIP_BOT + 2};
	InvalidateRect(g_wnd, &r, FALSE);
}

/* Repaint the strip that shows source s, if it is on screen. */
static void invalidate_src(int s) {
	int i = s - g_base;
	if (i >= 0 && i < g_nvis) invalidate_strip(i);
}

/* Short number shown on a strip: "1".."40" channels, "A1".. aux, "B1".. bus, "M1".. main, "MX1".. matrix. */
static void src_label(int s, char *out, size_t n) {
	if (s < 40)      snprintf(out, n, "%d", s + 1);
	else if (s < 48) snprintf(out, n, "A%d", s - 39);
	else if (s < 64) snprintf(out, n, "B%d", s - 47);
	else if (s < 68) snprintf(out, n, "M%d", s - 63);
	else             snprintf(out, n, "MX%d", s - 67);
}

/* Console strip index (0..75) -> text such as "CH 5", "AUX 2", "BUS 3". */
static void index_label(int idx, char *out, size_t n) {
	if (idx < 0 || idx > 75) snprintf(out, n, "--");
	else if (idx < 40)       snprintf(out, n, "CH %d", idx + 1);
	else if (idx < 48)       snprintf(out, n, "AUX %d", idx - 39);
	else if (idx < 64)       snprintf(out, n, "BUS %d", idx - 47);
	else if (idx < 68)       snprintf(out, n, "MAIN %d", idx - 63);
	else                     snprintf(out, n, "MTX %d", idx - 67);
}

static void update_sel_label(void) {
	char lbl[32], text[96];
	const char *name;
	int vis = g_sel - g_base;

	if (!g_connected || g_sel < 0) {
		SetWindowTextA(g_sellbl, "Selected:\r\n--");
		return;
	}
	index_label(g_sel, lbl, sizeof lbl);
	name = (vis >= 0 && vis < g_nvis) ? g_name[vis] : g_selname;
	if (name[0]) snprintf(text, sizeof text, "Selected:\r\n%s (%s)", lbl, name);
	else         snprintf(text, sizeof text, "Selected:\r\n%s", lbl);
	SetWindowTextA(g_sellbl, text);
}

/* ---- setters (repaint only what changed) --------------------------------- */
static void set_mute(int i, int v) {
	if (g_mute[i] != v) {
		g_mute[i] = v;
		invalidate_strip(i);
	}
}
static void set_solo(int i, int v) {
	if (g_solo[i] != v) {
		g_solo[i] = v;
		invalidate_strip(i);
	}
}
static void set_dyn_on(int i, int v) {
	if (g_dyn_on[i] != v) {
		g_dyn_on[i] = v;
		invalidate_strip(i);
	}
}
static void set_gate_on(int i, int v) {
	if (g_gate_on[i] != v) {
		g_gate_on[i] = v;
		invalidate_strip(i);
	}
}
static void set_link(int i, int v) {
	if (g_link[i] != v) {
		g_link[i] = v;
		invalidate_strip(i);
	}
}
static void set_col(int i, int v) {
	if (g_col[i] != v) {
		g_col[i] = v;
		invalidate_strip(i);
	}
}

static void set_fdr(int i, float v) {
	if (g_fdr_ok[i] && g_fdr[i] == v) return;
	g_fdr[i] = v;
	g_fdr_ok[i] = 1;
	invalidate_strip(i);
}

static void set_name(int i, const char *s) {
	if (strncmp(g_name[i], s, sizeof g_name[i] - 1) == 0) return;
	snprintf(g_name[i], sizeof g_name[i], "%s", s);
	invalidate_strip(i);
	if (g_sel == g_base + i) update_sel_label();
}

/* wapi reports the selected strip with the console's own numbering, 1..76
 * (CH 1 = 1), and wants the same numbering when setting it. Convert to the
 * 0-based index used everywhere in this program. */
static int sel_index(int v) {
	return (v >= 1 && v <= 76) ? v - 1 : -1;
}

/* The selected source's name, when it is not one of the strips on screen. */
static void refresh_selname(void) {
	char buf[256];
	int vis = g_sel - g_base;

	g_selname[0] = '\0';
	if (!g_connected || g_sel < 0 || g_sel >= NSRC) return;
	if (vis >= 0 && vis < g_nvis) return;
	buf[0] = '\0';
	if (wGetTokenStringTimed(SRC[g_sel].name, buf, RESYNC_WAIT_US) > 0) {
		buf[sizeof buf - 1] = '\0';
		snprintf(g_selname, sizeof g_selname, "%s", buf);
	}
}

static void set_sel(int idx) {
	int old = g_sel;
	if (idx == g_sel) return;
	g_sel = idx;
	invalidate_src(old);
	invalidate_src(idx);
	refresh_selname();
	update_sel_label();
}

static void reset_strips(void) {
	int i;
	for (i = 0; i < NSTRIP; i++) {
		g_mute[i] = g_solo[i] = g_col[i] = -1;
		g_fdr_ok[i] = 0;
		g_fdr[i] = 0;
		g_name[i][0] = '\0';
		g_name_dirty[i] = 0;
		g_lvl[i][0] = g_lvl[i][1] = NO_LEVEL;
		g_clip_until[i][0] = g_clip_until[i][1] = 0;
		g_gate_gr[i] = g_dyn_gr[i] = 0.0f;
		g_gate_eng[i] = g_dyn_eng[i] = 0;
		g_dyn_on[i] = g_gate_on[i] = -1;
		g_link[i] = -1;
	}
}

/* Forget meter values (after switching PRE/POST). */
static void reset_strips_meters(void) {
	int i;
	for (i = 0; i < NSTRIP; i++) {
		g_lvl[i][0] = g_lvl[i][1] = NO_LEVEL;
		g_clip_until[i][0] = g_clip_until[i][1] = 0;
		g_gate_gr[i] = g_dyn_gr[i] = 0.0f;
		g_gate_eng[i] = g_dyn_eng[i] = 0;
	}
	invalidate_all();
}

static void reset_data(void) {
	reset_strips();
	g_sel = -1;
	g_selname[0] = '\0';
}

/* ---- connection --------------------------------------------------------- */
static void disconnect(const char *why) {
	KillTimer(g_wnd, ID_TIMER);
	if (g_connected) wClose();
	g_connected = 0;
	g_meter_ok = 0;
	g_meter_port_ok = 0;
	g_drag = -1;
	g_edit_commit = 0; /* drop any half-typed name */
	finish_edit();
	reset_data();
	SetWindowTextA(g_connect, "Connect");
	update_sel_label();
	invalidate_all();
	set_status(why);
}

/* Read every value of the visible strips from the console. The wapi getters
 * return WSUCCESS (1) when the value was read, so a timeout or error (0 or
 * negative) leaves the displayed value alone. Stops at the first token that
 * doesn't answer, so an unresponsive console can't freeze the window for long. */
static void read_all(int with_names) {
	int i, v;
	float f;
	char buf[256];

	for (i = 0; i < g_nvis; i++) {
		if (wGetTokenIntTimed(TOK(i).mute, &v, RESYNC_WAIT_US) != WSUCCESS || (v != 0 && v != 1)) return;
		set_mute(i, v);
		if (wGetTokenIntTimed(TOK(i).solo, &v, RESYNC_WAIT_US) != WSUCCESS) return;
		set_solo(i, v ? 1 : 0);
		if (wGetTokenIntTimed(TOK(i).col, &v, RESYNC_WAIT_US) != WSUCCESS) return;
		set_col(i, v);
		if (wGetTokenFloatTimed(TOK(i).fdr, &f, RESYNC_WAIT_US) != WSUCCESS) return;
		if (i != g_drag) set_fdr(i, f); /* don't fight the mouse */
		if (wGetTokenIntTimed(TOK(i).dyn_on, &v, RESYNC_WAIT_US) == WSUCCESS) set_dyn_on(i, v ? 1 : 0);
		if (HAS_GATE(g_base + i) && wGetTokenIntTimed(TOK(i).gate_on, &v, RESYNC_WAIT_US) == WSUCCESS)
		set_gate_on(i, v ? 1 : 0);
		if (HAS_LINK(g_base + i) && wGetTokenIntTimed(TOK(i).clink, &v, RESYNC_WAIT_US) == WSUCCESS)
			set_link(i, v ? 1 : 0);
		if (with_names) {
			buf[0] = '\0';
			if (wGetTokenStringTimed(TOK(i).name, buf, RESYNC_WAIT_US) == WSUCCESS) {
				buf[sizeof buf - 1] = '\0';
				set_name(i, buf); /* a failed read must not blank the name */
			}
		}
	}
	if (wGetTokenIntTimed($CTL_$STAT_SELIDX, &v, RESYNC_WAIT_US) == WSUCCESS)
		set_sel(sel_index(v));
}

/* Re-read one strip's name (used when the console reports a name change). */
static void refresh_name(int i) {
	char buf[256];
	buf[0] = '\0';
	if (wGetTokenStringTimed(TOK(i).name, buf, RESYNC_WAIT_US) == WSUCCESS) {
		buf[sizeof buf - 1] = '\0';
		set_name(i, buf);
	}
}

/* ---- connecting ---------------------------------------------------------- */
/* With an empty address, wOpen() searches the local network itself, connects to
 * the first WING that answers and writes that console's IP address into the
 * buffer we give it (so the buffer must be big enough for an address). */
static void do_connect(void) {
	char ip[S_STRINGS]; /* receives the IP when searching (an IPv4 address needs 16 bytes at most) */
	int searching;
//	int searching, rc;

	if (g_connected) {
		disconnect("Disconnected");
		return;
	}

	memset(ip, 0, sizeof ip);
	GetWindowTextA(g_ip, ip, sizeof ip);
	searching = (ip[0] == '\0');
	set_status( searching ?
					"Searching for a WING on the network..." :
					"Connecting...");
	UpdateWindow(g_status);

/* wOpen() returns WSUCCESS (1) on success, a negative werror code otherwise. */
	if (wOpen(ip) != WSUCCESS) {;
//	if (!WOPEN_SUCCEEDED(rc)) {
		set_status( searching ?
						"No WING found. Type its IP address." :
						"Could not connect. Check IP and network.");
		return;
	}
	if (searching)
		SetWindowTextA(g_ip, ip); /* show the address that was found */
	g_connected = 1;
	SetWindowTextA(g_connect, "Disconnect");
	read_all(1);
	update_sel_label();
	set_status("Connected.");

	start_meters(); /* may replace the status if meters fail */
	g_last_sync = GetTickCount();
	SetTimer(g_wnd, ID_TIMER, TICK_MS, NULL);
}

/* "Find": forget the typed address and let wOpen() search. */
static void do_find(void) {
	if (g_connected) return;
	SetWindowTextA(g_ip, "");
	do_connect();
}

/* ---- meters (wapi metering API) ------------------------------------------ */
static void invalidate_meters(void) {
	RECT r;
	r.left = strip_x(0) - 2;
	r.right = strip_x(g_nvis > 0 ? g_nvis - 1 : 0) + STRIP_W + 2;
	r.top = Y_METER;
	r.bottom = Y_METER + H_METER;
	InvalidateRect(g_wnd, &r, FALSE);
}

/* Ask the console for the meters of the strips on screen. Called after
 * connecting and whenever the bank changes. */
/* Bit of the wMid request mask of the V2 meter family for source s (console
 * strip index); its records add the gate LED and the dyn state. */
static int meter_bit(int s) {
	/* The 29-byte table in the documentation puts the V2 families at bytes 19-28;
	 * the console's mask is 30 bytes long and the V2 families sit one byte later. */
	if (s < 40) return 160 + s; /* channel V2: bytes 20-24 */
	if (s < 48) return 200 + (s - 40); /* aux V2:     byte 25     */
	if (s < 64) return 208 + (s - 48); /* bus V2:     bytes 26-27 */
	if (s < 68) return 224 + (s - 64); /* main V2:    byte 28     */
	return 232 + (s - 68); /* matrix V2:  byte 29     */
}

static void start_meters(void) {
	unsigned char mb[30]; /* meter selection bit mask (30 bytes) */
	int i, s;

	g_meter_ok = 0;
	if (!g_connected) return;

	/* Set id and port based on time for meter requests */
	g_meter_id = (time(NULL) % 5000) + 10000;
	if (wMeterUDPPort(g_meter_id) == WSUCCESS) g_meter_port_ok = 1;
	if (!g_meter_port_ok) {
		set_status("Meters port access refused, try reconnecting.");
		return;
	}

	memset(mb, 0, sizeof mb); /* only the V2 meter families are used */
	for (i = 0; i < g_nvis; i++) {
		s = meter_bit(g_base + i);
		mb[s >> 3] |= (unsigned char) (0x80 >> (s & 7));
	}
	/* Set id to p (port #) for meter requests */
	if (wSetMetersRequest(g_meter_id, mb) == WSUCCESS) {
		g_meter_ok = 1;
		g_meter_renew = GetTickCount();
	} else {
		set_status("Meters ID request refused, try reconnecting.");
	}
}

static int be16(const unsigned char *p) {
	return (short) ((p[0] << 8) | p[1]);
}

/* One meter datagram: <request id, 4 bytes> then, for each strip on screen in
 * order, a V2 record of 11 big-endian 16-bit words:
 *   0 input L, 1 input R, 2 output L, 3 output R   (1/256 dB)
 *   4 gate key, 5 gate gain, 6 gate LED (0/1)
 *   7 dyn key,  8 dyn gain,  9 dyn state (0/1), 10 automix gain
 * Gains: 1.0 (256) = 20 dB of reduction. */
#define V2_WORDS 11
static void parse_meters(const unsigned char *buf, int len) {
	int id, rec, i, k;
	DWORD now = GetTickCount();

	if (len < 4 || g_nvis <= 0) return;
	id = (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
	if (id != g_meter_id) return;
	rec = (len - 4) / g_nvis; /* bytes of meter data per strip */
	if (rec < V2_WORDS * 2) return;

	g_meter_dirty = 0;
	for (i = 0; i < g_nvis; i++) {
		const unsigned char *d = buf + 4 + i * rec;
		float v[2], old[2];
		int og = (int) g_gate_gr[i], od = (int) g_dyn_gr[i],
				oge = g_gate_eng[i], ode = g_dyn_eng[i];
		float gg = fabsf((float) be16(d + 10)) / 256.0f * 20.0f;
		float dg = fabsf((float) be16(d + 16)) / 256.0f * 20.0f;

		old[0] = g_lvl[i][0];
		old[1] = g_lvl[i][1];
		v[0] = (float) be16(d + (g_meter_post ? 4 : 0)) / 256.0f;
		v[1] = (float) be16(d + (g_meter_post ? 6 : 2)) / 256.0f;
		for (k = 0; k < 2; k++) {/* instant rise, steady fall */
			if (v[k] >= CLIP_DB) g_clip_until[i][k] = GetTickCount() + CLIP_HOLD_MS;
			if (v[k] >= g_lvl[i][k]) g_lvl[i][k] = v[k];
			else if ((g_lvl[i][k] -= METER_FALL_DB) < v[k]) g_lvl[i][k] = v[k];
			if (g_lvl[i][k] != old[k]) g_meter_dirty = 1;
			{/* did the held CLIP LED change state? */
				int cl = (int) (g_clip_until[i][k] - now) > 0;
				if (cl != g_clip_shown[i][k]) {
					g_clip_shown[i][k] = cl;
					g_meter_dirty = 1;
				}
			}
		}
		g_gate_gr[i] = (HAS_GATE(g_base + i) && gg < 100.0f) ? gg : 0.0f;
		g_dyn_gr[i] = dg < 100.0f ? dg : 0.0f;
		g_gate_eng[i] = HAS_GATE(g_base + i) && be16(d + 12) == 1;
		g_dyn_eng[i] = be16(d + 18) == 1;
		if (og != (int) g_gate_gr[i] || od != (int) g_dyn_gr[i]
				|| oge != g_gate_eng[i] || ode != g_dyn_eng[i])
			g_meter_dirty = 1;
	}
}

static void pump_meters(void) {
	unsigned char buf[1024];
	int len, n = 0;

	if (!g_meter_ok) return;
	if (GetTickCount() - g_meter_renew >= METER_RENEW_MS) {
		wRenewMeters(g_meter_id); /* the console stops after 5 s otherwise */
		g_meter_renew = GetTickCount();
	}
	while (n++ < 8 && (len = wGetMeters(buf, sizeof buf, METER_WAIT_US)) > 0)
		parse_meters(buf, len);

	if (g_meter_dirty && GetTickCount() - g_meter_drawn >= METER_REPAINT_MS) {
		g_meter_dirty = 0; /* repaint only when something changed,   */
		g_meter_drawn = GetTickCount(); /* and at most ~25 times per second       */
		invalidate_meters();
	}
}

/* ---- event loop (runs from the timer) ----------------------------------- */
static int ev_int(const wTV *e) {
	return e->type == F32 ? (int) e->d.fdata : e->d.idata;
}

static float ev_float(const wTV *e) {
	return e->type == I32 ? (float) e->d.idata : e->d.fdata;
}

static void handle_event(const wTV *e) {
	int i;

	if (e->token == $CTL_$STAT_SELIDX) {
		set_sel(sel_index(ev_int(e)));
		return;
	}

	for (i = 0; i < g_nvis; i++) {/* only what is on screen */
		if (e->token == TOK(i).mute) {set_mute(i, ev_int(e) ? 1 : 0); return;}
		else if (e->token == TOK(i).solo) {set_solo(i, ev_int(e) ? 1 : 0); return;}
		else if (e->token == TOK(i).col) {set_col(i, ev_int(e)); return;}
		else if (e->token == TOK(i).dyn_on) {set_dyn_on(i, ev_int(e) ? 1 : 0); return;}
		else if (HAS_GATE(g_base + i) && e->token == TOK(i).gate_on) {set_gate_on(i, ev_int(e) ? 1 : 0); return;}
		else if (HAS_LINK(g_base + i) && e->token == TOK(i).clink) {set_link(i, ev_int(e) ? 1 : 0); g_name_dirty[i] = 1; return;}
		else if (e->token == TOK(i).fdr) {if (i != g_drag) set_fdr(i, ev_float(e)); return;}
		else if (e->token == TOK(i).name) {
			/* The event itself is the truth: a string event with no text (NULL or "")
			 * or an event carrying no string at all means the name was cleared. */
			set_name(i, (e->type == S32 && e->d.sdata) ? e->d.sdata : "");
			return;
		}
		else if (e->token == TOK(i).dname) {/* displayed name changed: re-read it */
			g_name_dirty[i] = 1;
			return;
		}
	}
}

static void pump_events(void) {
	wTV tv[MAX_EVENTS];
	int n, k;

	if (GetTickCount() - g_keepalive >= 1000) {
		g_keepalive = GetTickCount();
		if (wKeepAlive() < 0) {
			disconnect("Connection lost.");
			return;
		}
	}

	do {
		n = wGetParsedEventsTimed(tv, MAX_EVENTS, EVENT_WAIT_US);
		if (n < 0) {
			disconnect("Connection lost.");
			return;
		}
		for (k = 0; k < n; k++) {
			handle_event(&tv[k]);
			if (tv[k].type == S32 && tv[k].d.sdata) /* wapi allocates strings */
				free(tv[k].d.sdata);
		}
	} while (n == MAX_EVENTS);

	for (k = 0; k < g_nvis; k++) /* names flagged by events above */
		if (g_name_dirty[k]) {
			g_name_dirty[k] = 0;
			refresh_name(k);
		}

	pump_meters();

	if (GetTickCount() - g_last_sync >= RESYNC_MS) {
		read_all(1); /* includes names, as a safety net */
		g_last_sync = GetTickCount();
	}
}

/* ---- actions ------------------------------------------------------------ */
static void toggle_mute(int i) {
	int muted = -1;
	wGetTokenInt(TOK(i).mute, &muted); /* toggle from the real state */
	if (muted != 0 && muted != 1) {
		set_status("No answer from console.");
		return;
	}
	wSetTokenInt(TOK(i).mute, muted ? 0 : 1);
	set_mute(i, muted ? 0 : 1); /* instant feedback; the console
	 confirms via an event       */
}

static void select_strip(int i) {
	wSetTokenInt($CTL_$STAT_SELIDX, g_base + i + 1); /* console numbering: CH 1 = 1 */
	set_sel(g_base + i);
}

static void toggle_dyn(int i) {
	int on = (g_dyn_on[i] == 1) ? 0 : 1;
	wSetTokenInt(TOK(i).dyn_on, on);
	set_dyn_on(i, on);
}

static void toggle_gate(int i) {
	int on = (g_gate_on[i] == 1) ? 0 : 1;
	if (!HAS_GATE(g_base + i)) return;
	wSetTokenInt(TOK(i).gate_on, on);
	set_gate_on(i, on);
}

/* Toggle "link to source". The name shown may change with it, so re-read it. */
static void toggle_link(int i) {
	int clink = (g_link[i] == 1) ? 0 : 1;
	if (!HAS_LINK(g_base + i) || g_link[i] < 0) return;
	wSetTokenInt(TOK(i).clink, clink);
	set_link(i, clink);
	g_name_dirty[i] = 1;
}

static void toggle_solo(int i) {
	int on = (g_solo[i] == 1) ? 0 : 1;
	wSetTokenInt(TOK(i).solo, on);
	set_solo(i, on); /* the console confirms via an event */
}

/* ---- fader: mouse position <-> dB ---------------------------------------- */
static double db_from_t(double t) /* inverse of fader_t() */
{
	int k;
	if (t <= 0.012) return -144.0; /* bottom of the travel = -oo */
	if (t >= 1.0)   return 10.0;
	for (k = 1; k < NSC; k++)
		if (t <= SC_T[k])
			return SC_DB[k - 1]
					+ (SC_DB[k] - SC_DB[k - 1]) * (t - SC_T[k - 1])
							/ (SC_T[k] - SC_T[k - 1]);
	return 10.0;
}

static void send_fader(int i, double db) {
	wSetTokenFloat(TOK(i).fdr, (float)db);
	set_fdr(i, (float)db);
}

/* Follow the mouse while a fader is being dragged. */
static void drag_fader(int y) {
	double t = (double) (Y_FDR_BOT - (y + g_drag_dy)) / (Y_FDR_BOT - Y_FDR_TOP);
	double db;

	if (g_drag < 0) return;
	if (t < 0.0) t = 0.0;
	if (t > 1.0) t = 1.0;
	db = db_from_t(t);
	if (db > -144.0) {
		db = (int) (db * 10.0 + (db >= 0 ? 0.5 : -0.5)) / 10.0; /* 0.1 dB steps */
		if (db > -0.4 && db < 0.4)
			db = 0.0; /* snap to unity */
	}
	if ((float) db == g_drag_sent) return; /* nothing new to send */
	g_drag_sent = (float) db;
	send_fader(g_drag, db);
}

/* ---- inline name editor -------------------------------------------------- */
static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
	if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) {
		g_edit_commit = (w == VK_RETURN);
		SetFocus(g_wnd); /* ends editing via WM_KILLFOCUS */
		return 0;
	}
	if (m == WM_CHAR && (w == '\r' || w == 27))
		return 0; /* no beep */
	if (m == WM_KILLFOCUS && h == g_edit) {
		LRESULT r = CallWindowProcA(g_edit_oldproc, h, m, w, l);
		PostMessage(g_wnd, WM_APP, 0, 0); /* finish outside this callback */
		return r;
	}
	return CallWindowProcA(g_edit_oldproc, h, m, w, l);
}

static void finish_edit(void) {
	char buf[64];
	int i = g_edit_strip;
	HWND e = g_edit;

	if (!e) return;
	GetWindowTextA(e, buf, sizeof buf);
	g_edit = NULL;
	g_edit_strip = -1;
	DestroyWindow(e);

	if (g_edit_commit && g_connected && i >= 0 && i < g_nvis) {
		char back[256];
		int rc;

		buf[16] = '\0'; /* WING names: 16 characters max */
		rc = wSetTokenString(TOK(i).name, buf);
		set_name(i, buf); /* instant feedback */

		/* Read the name back, so the strip shows what the console really holds. */
		back[0] = '\0';
		if (rc > 0 && wGetTokenStringTimed(TOK(i).name, back, RESYNC_WAIT_US) == WSUCCESS) {
			back[sizeof back - 1] = '\0';
			set_name(i, back);
		}
	}
}

static void start_edit(int i) {
	int sx = strip_x(i);

	if (!g_connected) return;
	finish_edit();
	g_edit_commit = 1; /* leaving the box also commits */
	g_edit_strip = i;
	g_edit = CreateWindowExA(0, "EDIT", g_name[i],
	WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL, sx + 4,
			Y_SCRIB + 22, STRIP_W - 8, 28, g_wnd, NULL, GetModuleHandle(NULL), NULL);
	SendMessage(g_edit, WM_SETFONT, (WPARAM) g_font, TRUE);
	SendMessage(g_edit, EM_SETLIMITTEXT, 16, 0);
	g_edit_oldproc = (WNDPROC) SetWindowLongPtrA(g_edit, GWLP_WNDPROC, (LONG_PTR) edit_proc);
	SetFocus(g_edit);
	SendMessage(g_edit, EM_SETSEL, 0, -1);
}

/* ---- mouse hit-testing --------------------------------------------------- */
enum {
	Z_NONE, Z_SCRIB, Z_SEL, Z_SOLO, Z_MUTE, Z_FDR, Z_DYN, Z_GATE, Z_LINK
};

static int hit(int x, int y, int *zone) {
	int i;
	*zone = Z_NONE;
	for (i = 0; i < g_nvis; i++) {
		int sx = strip_x(i), bx = sx + BTN_X;
		if (x < sx || x >= sx + STRIP_W)
			continue;
		if (HAS_LINK(g_base + i) && x >= sx + LOCK_X - 3 && x < sx + LOCK_X + LOCK_W + 3
				&& y >= Y_SCRIB && y < Y_SCRIB + LOCK_H + 8)                      *zone = Z_LINK;
		else if (y >= Y_SCRIB && y < Y_SCRIB + H_SCRIB)                           *zone = Z_SCRIB;
		else if (x >= bx && x < bx + BTN_W && y >= Y_SEL && y < Y_SEL + H_SEL)    *zone = Z_SEL;
		else if (x >= bx && x < bx + BTN_W && y >= Y_SOLO && y < Y_SOLO + H_SOLO)  *zone = Z_SOLO;
		else if (x >= bx && x < bx + BTN_W && y >= Y_MUTE && y < Y_MUTE + H_MUTE) *zone = Z_MUTE;
		else if (x >= bx && x < bx + 30 && y >= Y_METER + 2 && y < Y_METER + 16)  *zone = Z_DYN;
		else if (x >= bx&& x < bx + 30 && y >= Y_METER + 80 && y < Y_METER + 94 && HAS_GATE(g_base + i))
			*zone = Z_GATE;
		else if (x >= sx + 11 && x < sx + 51 && y >= Y_FDR_TOP - H_FDR_EXT
				&& y <= Y_FDR_BOT + H_FDR_EXT)
			*zone = Z_FDR;
		return i;
	}
	return -1;
}

/* Left button pressed. */
static void click(int x, int y) {
	int zone, i;

	SetFocus(g_wnd); /* commits a name being edited */
	if (!g_connected)
		return;
	i = hit(x, y, &zone);
	if (i < 0)
		return;

	switch (zone) {
	case Z_SEL:
		select_strip(i);
		break;
	case Z_SOLO:
		toggle_solo(i);
		break;
	case Z_MUTE:
		toggle_mute(i);
		break;
	case Z_DYN:
		toggle_dyn(i);
		break;
	case Z_GATE:
		toggle_gate(i);
		break;
	case Z_FDR: {
		int cy = g_fdr_ok[i] ? fader_y(fader_t(g_fdr[i])) : 0;
		/* grabbing the handle keeps its offset; clicking the track jumps to it */
		g_drag_dy = (g_fdr_ok[i] && y >= cy - 14 && y <= cy + 14) ? cy - y : 0;
		g_drag = i;
		g_drag_sent = 1e30f;
		SetCapture(g_wnd);
		drag_fader(y);
		break;
	}
	default:
		break;
	}
}

/* Left button double-clicked: scribble strip = rename, fader = back to 0 dB. */
static void dblclick(int x, int y) {
	int zone, i;

	if (!g_connected)      return;
	i = hit(x, y, &zone);
	if (i < 0) return;
	if (zone == Z_LINK)     toggle_link(i);   /* a single click does nothing here */
	else if (zone == Z_SCRIB) start_edit(i);
	else if (zone == Z_FDR) send_fader(i, 0.0);
	else                    click(x, y); /* fast double-tap on a button */
}

/* ---- banks --------------------------------------------------------------- */
static void set_bank(int b) {
	int k;

	g_edit_commit = 0; /* drop a half-typed name */
	finish_edit();
	if (g_drag >= 0) {
		g_drag = -1;
		ReleaseCapture();
	}

	g_bank = b;
	g_base = BANKS[b].base;
	g_nvis = BANKS[b].count;
	reset_strips(); /* values belong to the old bank */
	apply_layout(); /* window width follows the strip count */
	for (k = 0; k < NBANK; k++)
		SendMessage(g_bankbtn[k], BM_SETCHECK, k == b ? BST_CHECKED : BST_UNCHECKED, 0);
	invalidate_all();

	if (g_connected) {
		read_all(1); /* also refreshes the selection */
		start_meters(); /* meters of the new bank */
		g_last_sync = GetTickCount();
	}
	refresh_selname();
	update_sel_label();
}

/* ---- drawing ------------------------------------------------------------ */
static void fill_rect(HDC dc, int x, int y, int w, int h, COLORREF c) {
	RECT r = {x, y, x + w, y + h};
	SetBkColor(dc, c); /* no brush objects: much cheaper */
	ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &r, NULL, 0, NULL);
}

static void text(HDC dc, HFONT f, COLORREF c, int x, int y, int w, int h, UINT flags, const char *s) {
	RECT r = {x, y, x + w, y + h};
	HGDIOBJ old = SelectObject(dc, f);
	SetBkMode(dc, TRANSPARENT);
	SetTextColor(dc, c);
	DrawTextA(dc, s, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
	SelectObject(dc, old);
}

static void button(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF txt, const char *s) {
	RECT r = {x, y, x + w, y + h};
	fill_rect(dc, x, y, w, h, fill);
	DrawEdge(dc, &r, EDGE_RAISED, BF_RECT);
	text(dc, g_smallbold, txt, x, y, w, h, DT_CENTER | DT_VCENTER, s);
}

static double fader_t(double db) {
	int k;
	if (db <= SC_DB[0])       return 0.0;
	if (db >= SC_DB[NSC - 1]) return 1.0;
	for (k = 1; k < NSC; k++)
		if (db <= SC_DB[k])
			return SC_T[k - 1]
					+ (SC_T[k] - SC_T[k - 1]) * (db - SC_DB[k - 1])
							/ (SC_DB[k] - SC_DB[k - 1]);
	return 1.0;
}

static int fader_y(double t) {
	return Y_FDR_BOT - (int) (t * (Y_FDR_BOT - Y_FDR_TOP) + 0.5);
}

/* Level meters: DYN gain-reduction row, six LED rows (L | label | R), GATE row. */
/* DYN / GATE row: the label is yellow when the processor is ON and grey when it
 * is off (click it to switch it); the red LED lights while the console reports it
 * engaged (gate closed / dynamics acting); the bar shows the gain reduction.
 * avail = 0 draws an empty row (no such processor on this strip). */
static void gr_row(HDC dc, int x, int y, const char *name, float gr, float full, int eng, int on, int avail) {
	int w = (int) (gr / full * 35.0f + 0.5f);
	int lit = on != 0 && (eng || gr > 0.3f); /* nothing lights while the processor is off */
	if (on == 0) w = 0;
	if (w > 35) w = 35;
	if (w < 0) w = 0;
	if (!avail) {
		fill_rect(dc, x, y, 26, 14, RGB(26, 27, 30));
		fill_rect(dc, x + 28, y + 3, 44, 8, RGB(26, 27, 30));
		return;
	}
	fill_rect(dc, x, y, 26, 14, RGB(34, 35, 39));
	text(dc, g_tiny, on == 1 ? RGB(240, 200, 0) : RGB(120, 120, 120), x, y, 26, 14,
			DT_CENTER | DT_VCENTER, name);
	/* [gain-reduction bar] [square LED], 8 pixels high like the level LEDs, with a
	 * 1-pixel gap between them; same geometry for DYN and GATE. */
	fill_rect(dc, x + 28, y + 3, 35, 8, RGB(30, 31, 34));
	if (w > 0) fill_rect(dc, x + 63 - w, y + 3, w, 8, RGB(235, 40, 40)); /* grows from the right */
	fill_rect(dc, x + 64, y + 3, 8, 8, lit ? RGB(235, 40, 40) : RGB(60, 30, 32));
}

static void draw_meter(HDC dc, int i, int x) {
	static const char *lab[6] = {"CLIP", "-6", "-12", "-18", "-30", "-60"};
	static const float thr[6] = {-1.0f, -6.0f, -12.0f, -18.0f, -30.0f, -60.0f};
	static const COLORREF lit[6] = {RGB(255, 40, 40), RGB(240, 220, 0), RGB(
			240, 220, 0), RGB(240, 220, 0), RGB(40, 210, 60), RGB(40, 210, 60)};
	int k;
	DWORD now = GetTickCount();

	fill_rect(dc, x - 2, Y_METER, BTN_W + 4, H_METER, RGB(0, 0, 0));
	gr_row(dc, x, Y_METER + 2, "DYN", g_dyn_gr[i], 18.0f, g_dyn_eng[i], g_dyn_on[i], 1);
	for (k = 0; k < 6; k++) {
		int y = Y_METER + 18 + k * 10;
		int on0 = g_lvl[i][0] >= thr[k], on1 = g_lvl[i][1] >= thr[k];
		if (k == 0) {/* CLIP: held for CLIP_HOLD_MS */
			on0 = on0 || (int) (g_clip_until[i][0] - now) > 0;
			on1 = on1 || (int) (g_clip_until[i][1] - now) > 0;
		}
		fill_rect(dc, x + 2, y, 20, 8, on0 ? lit[k] : RGB(46, 48, 52));
		fill_rect(dc, x + 52, y, 20, 8, on1 ? lit[k] : RGB(46, 48, 52));
		text(dc, g_tiny, RGB(200, 200, 200), x + 22, y - 2, 30, 12,
				DT_CENTER | DT_VCENTER, lab[k]);
	}
	gr_row(dc, x, Y_METER + 80, "GATE", g_gate_gr[i], 20.0f, g_gate_eng[i], g_gate_on[i], HAS_GATE(g_base + i));
}

/* Small padlock: closed = linked to the source, open = custom (not linked). */
static void draw_lock(HDC dc, int x, int y, int linked, COLORREF c) {
	fill_rect(dc, x, y + 6, LOCK_W, 7, c);               /* body */
	fill_rect(dc, x + 2, y, 6, 2, c);                    /* shackle: top bar */
	fill_rect(dc, x + 2, y, 2, 6, c);                    /*          left post */
	if (linked) fill_rect(dc, x + 6, y, 2, 6, c);        /* closed: right post reaches the body */
	else        fill_rect(dc, x + 6, y, 2, 2, c);        /* open: right post is lifted */
}

static void draw_strip(HDC dc, int i) {
	int sx = strip_x(i), bx = sx + BTN_X, tx = sx + 30, k, y;
	int sel = (g_sel == g_base + i);
	COLORREF sc = (g_col[i] >= 1 && g_col[i] <= NCOLORS) ? WCOL[g_col[i]] : WCOL[0];
	COLORREF mc = g_mute[i] == 1 ? C_MUTED : g_mute[i] == 0 ? C_LIVE : C_UNKNOWN;
	char buf[32];
	/* dark text on light colours, white text on dark ones */
	COLORREF tc = (299 * GetRValue(sc) + 587 * GetGValue(sc) + 114 * GetBValue(sc))
					/ 1000 > 150 ? RGB(20, 20, 20) : RGB(255, 255, 255);

	/* scribble strip (white outline when this is the selected source) */
	fill_rect(dc, sx + 1, Y_SCRIB - 3, STRIP_W - 2, H_SCRIB + 6, sel ? RGB(255, 255, 255) : C_BG);
	fill_rect(dc, sx + 4, Y_SCRIB, STRIP_W - 8, H_SCRIB, sc);
	src_label(g_base + i, buf, sizeof buf);
	text(dc, g_bold, tc, sx + 9, Y_SCRIB + 3, 40, 20, DT_LEFT, buf);
	text(dc, g_font, tc, sx + 6, Y_SCRIB + 24, STRIP_W - 12, 32,
	DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS, g_name[i]);

	if (HAS_LINK(g_base + i) && g_link[i] >= 0)
		draw_lock(dc, sx + LOCK_X, Y_SCRIB + 4, g_link[i] == CLINK_LINKED, tc);

	/* SELECT / SOLO / MUTE */
	button(dc, bx, Y_SEL, BTN_W, H_SEL, sel ? C_SELECT : C_DARK, sel ? RGB(0, 0, 0) : RGB(255, 255, 255), "SELECT");
	button(dc, bx, Y_SOLO, BTN_W, H_SOLO, g_solo[i] == 1 ? C_SOLO : C_DARK, g_solo[i] == 1 ? RGB(0, 0, 0) : RGB(255, 255, 255), "SOLO");
	draw_meter(dc, i, bx);
	button(dc, bx, Y_MUTE, BTN_W, H_MUTE, mc, RGB(255, 255, 255), "MUTE");

	/* fader track and dB scale */
	fill_rect(dc, tx - 2, Y_FDR_TOP - 6, 4, Y_FDR_BOT - Y_FDR_TOP + 12, RGB(0, 0, 0));
	for (k = 0; k < (int) (sizeof SCALE_LABELS / sizeof SCALE_LABELS[0]); k++) {
		y = fader_y(fader_t(SCALE_LABELS[k]));
		fill_rect(dc, sx + 49, y, 5, 1, RGB(40, 40, 40));
		snprintf(buf, sizeof buf, "%d", SCALE_LABELS[k]);
		text(dc, g_small, RGB(30, 30, 30), sx + 55, y - 8, 22, 16, DT_RIGHT | DT_VCENTER, buf);
	}
	fill_rect(dc, sx + 49, Y_FDR_BOT, 5, 1, RGB(40, 40, 40));
	{
		static const WCHAR inf[] = {'-', 0x221E, 0};
		RECT r = {sx + 55, Y_FDR_BOT - 8, sx + 77, Y_FDR_BOT + 8};
		HGDIOBJ old = SelectObject(dc, g_small);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(30, 30, 30));
		DrawTextW(dc, inf, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, old);
	}

	/* fader handle at the current position */
	if (g_fdr_ok[i]) {
		RECT hr;
		y = fader_y(fader_t(g_fdr[i]));
		hr.left = tx - 17;
		hr.right = tx + 17;
		hr.top = y - 14;
		hr.bottom = y + 14;
		fill_rect(dc, hr.left, hr.top, 34, 28, RGB(58, 58, 62));
		DrawEdge(dc, &hr, EDGE_RAISED, BF_RECT);
		fill_rect(dc, hr.left + 4, y - 1, 26, 2, RGB(255, 255, 255));
	}

	/* numeric readout */
	fill_rect(dc, sx + 10, Y_FDR_READ, STRIP_W - 20, H_FDR_READ, RGB(30, 30, 32));
	if (!g_fdr_ok[i])          snprintf(buf, sizeof buf, "--");
	else if (g_fdr[i] <= -90)  snprintf(buf, sizeof buf, "-oo");
	else                       snprintf(buf, sizeof buf, "%.1f dB", g_fdr[i]);
	text(dc, g_font, RGB(255, 255, 255), sx + 10, Y_FDR_READ, STRIP_W - 20, H_FDR_READ, DT_CENTER | DT_VCENTER, buf);
}

static void paint(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC dc, mem;
	HBITMAP bmp, oldbmp;
	RECT pr;
	int i, w, h, meters_only;

	dc = BeginPaint(hwnd, &ps);
	pr = ps.rcPaint;
	w = pr.right - pr.left;
	h = pr.bottom - pr.top;
	if (w <= 0 || h <= 0) {
		EndPaint(hwnd, &ps);
		return;
	}

	mem = CreateCompatibleDC(dc);
	bmp = CreateCompatibleBitmap(dc, w, h); /* only the area being repainted */
	oldbmp = (HBITMAP) SelectObject(mem, bmp);
	SetViewportOrgEx(mem, -pr.left, -pr.top, NULL);

	/* Meter updates (most repaints) only touch the meter band of each strip. */
	meters_only = (pr.top >= Y_METER && pr.bottom <= Y_METER + H_METER);

	fill_rect(mem, pr.left, pr.top, w, h, C_BG);
	/* vertical line between the bank buttons and the first group of strips */
	fill_rect(mem, (X_MARGIN + BANK_W + STRIP_X0 + 4) / 2 - 1, STRIP_TOP, 2,
	STRIP_BOT - STRIP_TOP, RGB(120, 118, 112));
	for (i = 1; i * GROUP < g_nvis; i++) /* group dividers */
		fill_rect(mem, strip_x(i * GROUP) - GROUP_GAP / 2 - 1, STRIP_TOP, 2,
		STRIP_BOT - STRIP_TOP, RGB(120, 118, 112));
	for (i = 0; i < g_nvis; i++) {
		int sx = strip_x(i);
		if (sx + STRIP_W < pr.left || sx > pr.right)
			continue; /* not in the dirty area */
		if (meters_only) draw_meter(mem, i, sx + BTN_X);
		else             draw_strip(mem, i);
	}

	BitBlt(dc, pr.left, pr.top, w, h, mem, pr.left, pr.top, SRCCOPY);

	SetViewportOrgEx(mem, 0, 0, NULL);
	SelectObject(mem, oldbmp);
	DeleteObject(bmp);
	DeleteDC(mem);
	EndPaint(hwnd, &ps);
}

/* ---- window ------------------------------------------------------------- */
static HWND make(const char *cls, const char *txt, DWORD style, int x, int y, int w, int h, HWND parent, int id) {
	HWND c = CreateWindowExA(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y,
			w, h, parent, (HMENU) (INT_PTR) id, GetModuleHandle(NULL), NULL);
	SendMessage(c, WM_SETFONT, (WPARAM) g_font, TRUE);
	return c;
}

static HFONT mkfont(int px, int weight) {
	return CreateFontA(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	int k;

	switch (msg) {
	case WM_CREATE:
		g_wnd = hwnd;
		g_bg = CreateSolidBrush(C_BG);
		g_font = mkfont(15, FW_NORMAL);
		g_bold = mkfont(17, FW_BOLD);
		g_small = mkfont(12, FW_NORMAL);
		g_smallbold = mkfont(13, FW_BOLD);
		g_tiny = mkfont(10, FW_BOLD);
		reset_data();

		make("STATIC", "WING IP:", 0, X_MARGIN, 16, 66, 24, hwnd, 0);
		g_ip = make("EDIT", "", WS_BORDER | ES_AUTOHSCROLL,
		X_MARGIN + 70, 12, 120, 28, hwnd, ID_IP);
		g_find = make("BUTTON", "Find", BS_PUSHBUTTON,
		X_MARGIN + 194, 12, 48, 28, hwnd, ID_FIND);
		g_connect = make("BUTTON", "Connect", BS_PUSHBUTTON,
		X_MARGIN + 246, 12, 80, 28, hwnd, ID_CONNECT);

		g_metermode = make("BUTTON", "Meters: PRE", BS_PUSHBUTTON,
		X_MARGIN + 330, 12, 98, 28, hwnd, ID_METERMODE);

		for (k = 0; k < NBANK; k++)
			g_bankbtn[k] = make("BUTTON", BANKS[k].label,
			BS_AUTORADIOBUTTON | BS_PUSHLIKE | (k == 0 ? WS_GROUP : 0),
			X_MARGIN, bank_y(k), BANK_W, 28, hwnd, ID_BANK0 + k);
		SendMessage(g_bankbtn[0], BM_SETCHECK, BST_CHECKED, 0);

		g_sellbl = make("STATIC", "Selected:\r\n--", 0,
		X_MARGIN, bank_y(NBANK - 1) + 44, BANK_W, 110, hwnd, ID_SELLBL);
		SendMessage(g_sellbl, WM_SETFONT, (WPARAM) g_font, TRUE);
		g_status = make("STATIC", "Not connected", 0,
		X_MARGIN, Y_STATUS, 680, H_STATUS, hwnd, ID_STATUS);
		return 0;

	case WM_ERASEBKGND:
		return 1; /* paint() draws everything */

	case WM_CTLCOLORSTATIC:
		SetBkColor((HDC) wp, C_BG);
		return (LRESULT) g_bg;

	case WM_PAINT:
		paint(hwnd);
		return 0;

	case WM_LBUTTONDOWN:
		click((short) LOWORD(lp), (short) HIWORD(lp));
		return 0;

	case WM_LBUTTONDBLCLK:
		dblclick((short) LOWORD(lp), (short) HIWORD(lp));
		return 0;

	case WM_MOUSEMOVE:
		if (g_drag >= 0 && (wp & MK_LBUTTON))
			drag_fader((short) HIWORD(lp));
		return 0;

	case WM_LBUTTONUP:
	case WM_CAPTURECHANGED:
		if (g_drag >= 0) {
			g_drag = -1;
			if (GetCapture() == hwnd)
				ReleaseCapture();
		}
		return 0;

	case WM_APP: /* name editor lost focus */
		finish_edit();
		return 0;

	case WM_STARTUP: /* look for a console at start-up */
		do_find();
		return 0;

	case WM_COMMAND:
		if (LOWORD(wp) == ID_CONNECT)
			do_connect();
		else if (LOWORD(wp) == ID_FIND)
			do_find();
		else if (LOWORD(wp) == ID_METERMODE) {/* PRE fader (input) <-> POST fader (output) */
			g_meter_post = !g_meter_post;
			SetWindowTextA(g_metermode,
					g_meter_post ? "Meters: POST" : "Meters: PRE");
			reset_strips_meters();
		} else if (LOWORD(wp) >= ID_BANK0 && LOWORD(wp) < ID_BANK0 + NBANK &&
		HIWORD(wp) == BN_CLICKED)
			set_bank(LOWORD(wp) - ID_BANK0);
		return 0;

	case WM_TIMER:
		if (wp == ID_TIMER && g_connected)
			pump_events();
		return 0;

	case WM_DESTROY:
		if (g_connected)
			wClose();
		DeleteObject(g_font);
		DeleteObject(g_bold);
		DeleteObject(g_small);
		DeleteObject(g_smallbold);
		DeleteObject(g_tiny);
		DeleteObject(g_bg);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
	init_sources();
	WNDCLASSA wc = {0};
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
	int client_w = client_width(12);
	int client_h = Y_STATUS + H_STATUS + BOTTOM_PAD;
	RECT rc = {0, 0, client_w, client_h};
	HWND hwnd;
	MSG m;
	(void) prev;
	(void) cmd;

	AdjustWindowRect(&rc, style, FALSE);

	wc.style = CS_DBLCLKS;
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName = "wpanelWnd";
	RegisterClassA(&wc);

	FreeConsole();

	hwnd = CreateWindowExA(0, wc.lpszClassName, WPANEL_TITLE, style, CW_USEDEFAULT, CW_USEDEFAULT,
			rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, inst, NULL);
	apply_layout();
	ShowWindow(hwnd, show);
	UpdateWindow(hwnd);
	PostMessage(hwnd, WM_STARTUP, 0, 0);

	while (GetMessage(&m, NULL, 0, 0) > 0) {
		TranslateMessage(&m);
		DispatchMessage(&m);
	}
	return (int) m.wParam;
}
