/*
 * r-deskvint-irix-gui -- the IRIX settings panel for the RustDesk agent.
 *
 * IRIS IM (OSF/Motif) front end, built as a SECOND binary beside the agent
 * itself. The agent is a headless daemon and stays one: everything here can be
 * done with `r-deskvint-irix --server ...` and friends, and this exists because
 * a settings *window* is a different thing from a settings *command* to the
 * person at the machine.
 *
 * NO BEHAVIOUR IN THIS FILE. Every button runs one verb of
 * gui/agent-helper.sh, which is ordinary Bourne shell and is testable from a
 * terminal. The split is taken from irixscsitb's gui_motif.c and from the Mac
 * bundle's app-ui.m, and the reason is the same in all three: the interface is
 * the part that can only be exercised by a person sitting in front of it, so it
 * should hold as little as possible. Here it also keeps the panel honest about
 * the CLI -- each `set` verb is one `r-deskvint-irix --flag` invocation, so the
 * window cannot drift into having its own idea of what a setting means.
 *
 * WRITTEN AGAINST THE MOTIF 1.2 API DELIBERATELY. That is what IRIX 6.5 ships
 * (/usr/Motif-1.2, reported by XmVERSION_STRING as "OSF/Motif Version 1.2.4")
 * and what 5.3 carries, so one source covers the family. Do not reach for
 * Motif 2.x-only calls.
 *
 * CROSS-COMPILED, unlike irixscsitb which builds natively with MIPSpro. Two
 * things follow, both handled in gui/build-gui.sh:
 *   - The Motif headers are not in the stock cross sysroot and have to be
 *     copied off a machine; the script says how.
 *   - XmStrDefs.h declares `externalref _XmConst char ...` inside a branch that
 *     never defines _XmConst, which MIPSpro tolerates and clang does not, so
 *     the build defines it empty on the command line.
 *
 * Copyright (C) 2026 Dani Sarfati. Same licence as the rest of the tree.
 */

#include <X11/Intrinsic.h>
#include <X11/StringDefs.h>
#include <X11/Shell.h>
#include <X11/cursorfont.h>
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/Form.h>
#include <Xm/Frame.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/Separator.h>
#include <Xm/MessageB.h>
#include <Xm/TextF.h>
#include <Xm/Text.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * XtVaAppInitialize() takes Cardinal* for argc up to X11R5 and int* from R6.
 * IRIX 5.3 reports XtSpecificationRelease 4, 6.5 is R6, and one source covers
 * both -- so ask the headers in front of us rather than assuming either.
 * Straight from irixscsitb's gui_motif.c.
 */
#if defined(XtSpecificationRelease) && XtSpecificationRelease >= 6
typedef int XtArgcType;
#else
typedef Cardinal XtArgcType;
#endif

#define APP_CLASS "Rustdeskagentgui"

/*
 * The SGI look. These are the resources every stock IRIX Motif app sets (grep
 * useSchemes in /usr/lib/X11/app-defaults); without them the widgets render as
 * plain OSF/Motif battleship grey next to a desktop that is not. They are
 * fallbacks, so a system with no schemes installed simply ignores them.
 *
 * Every label lives here rather than in the code, so a site can re-word or
 * localise the panel from app-defaults without a rebuild -- the same reason
 * irixscsitb does it.
 */
static String fallback_resources[] = {
	"*useSchemes: all",
	"*schemeFileList: SgiSpec",
	"*scheme: Base",
	"*sgiMode: true",
	/* The first component must be the application NAME (argv[0]) or its
	 * CLASS, or the WM silently falls back to the bare binary name. */
	"Rustdeskagentgui.title: RustDesk Agent",
	"*logText.fontList: fixed",
	"*logText.columns: 74",
	"*logText.rows: 8",
	/*
	 * The text fields carry the panel's width, so it is a resource.
	 *
	 * NOT a shell geometry. The first attempt set one, and it made things
	 * worse rather than better: every row had its label attached to the left
	 * edge and its Apply button to the right, which asks the Form how wide it
	 * is while the Form is asking the rows -- and Motif answers a circular
	 * size negotiation by collapsing to nothing. Forcing the shell wider
	 * afterwards left the rows laid out for the collapsed size, so the window
	 * came up correctly sized and completely empty.
	 *
	 * The fix is to give every widget a real natural width and attach each to
	 * the one before it, so the size is computed outward from the content and
	 * there is nothing circular to resolve. A site whose scheme uses a larger
	 * font widens the panel by changing this one number.
	 */
	"*XmTextField.columns: 30",

	"*file.labelString: File",
	"*refreshItem.labelString: Refresh",
	"*quit.labelString: Quit",
	"*agentMenu.labelString: Agent",
	"*startItem.labelString: Start",
	"*stopItem.labelString: Stop",
	"*restartItem.labelString: Restart",
	"*showIdItem.labelString: Show ID...",
	"*showKeyItem.labelString: Show Public Key...",
	"*showLogItem.labelString: Show the Log",
	"*help.labelString: Help",
	"*about.labelString: About...",

	"*thisMachineFrameLabel.labelString: This machine",
	"*idLabel.labelString: Agent ID:",
	"*stateLabel.labelString: Status:",
	"*captureLabel.labelString: Capture:",
	"*passwordLabel.labelString: Password:",
	"*passwordApply.labelString: Set",

	"*infraFrameLabel.labelString: Infrastructure",
	"*serverLabel.labelString: ID server:",
	"*keyLabel.labelString: Server key:",
	"*relayLabel.labelString: Relay (optional):",
	"*apiLabel.labelString: Console (optional):",
	"*caLabel.labelString: CA bundle (optional):",
	"*serverApply.labelString: Apply",
	"*keyApply.labelString: Apply",
	"*relayApply.labelString: Apply",
	"*apiApply.labelString: Apply",
	"*caApply.labelString: Apply",

	"*startBtn.labelString: Start",
	"*stopBtn.labelString: Stop",
	"*restartBtn.labelString: Restart",
	"*refreshBtn.labelString: Refresh",
	"*status.labelString: Ready.",
	NULL
};

/*
 * Window-manager icon: a display with a curve leaving its top-right corner --
 * a screen, and something going out of it. 32x32, one bit deep.
 *
 * XBM rather than XPM because IRIX 5.3 ships libXpm.so WITHOUT its header, so
 * it cannot be built against; XCreateBitmapFromData is core Xlib and always
 * there. Depth 1 on purpose: ICCCM allows the screen depth too, but 4Dwm
 * carries a "color icon pixmap not supported" error string, so one bit is the
 * safe form. Both notes are irixscsitb's, confirmed rather than re-derived.
 *
 * This is the iconified-window icon. A proper Indigo Magic DESKTOP icon is a
 * different and much larger job (FTR rules plus a vector .icon file) and is
 * deliberately not attempted -- see the resume prompt.
 */
#define ICON_WIDTH  32
#define ICON_HEIGHT 32
static unsigned char icon_bits[] = {
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xff, 0xff, 0x3f,
   0x04, 0x00, 0x00, 0x20, 0x04, 0x00, 0x00, 0x20, 0x04, 0x00, 0x80, 0x27,
   0x04, 0x00, 0x00, 0x26, 0x04, 0x00, 0x00, 0x25, 0x04, 0x00, 0x00, 0x24,
   0x04, 0x38, 0x00, 0x20, 0x04, 0x44, 0x00, 0x20, 0x04, 0x82, 0x00, 0x20,
   0x04, 0x82, 0x00, 0x20, 0x04, 0x82, 0x00, 0x20, 0x04, 0x44, 0x00, 0x20,
   0x04, 0x38, 0x00, 0x20, 0x04, 0x00, 0x00, 0x20, 0x04, 0x00, 0x00, 0x20,
   0x04, 0x00, 0x00, 0x20, 0x04, 0x00, 0x00, 0x20, 0xfc, 0xff, 0xff, 0x3f,
   0x00, 0x60, 0x06, 0x00, 0x00, 0x60, 0x06, 0x00, 0x00, 0x60, 0x06, 0x00,
   0x00, 0xf8, 0x1f, 0x00, 0x00, 0xf8, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static Widget toplevel;
static Widget status_w;
static Widget id_value, state_value, capture_value;
static Widget password_f, server_f, key_f, relay_f, api_f, ca_f;
static Widget start_btn, stop_btn, restart_btn;
static Widget info_dialog = NULL, error_dialog = NULL;
static Widget log_shell = NULL, log_text = NULL;

/* Where the helper is. Settled once in main(), because it is found relative to
 * argv[0] and every callback needs it. */
static char helper_path[1024];

/* ------------------------------------------------------------------ *
 * talking to the helper
 * ------------------------------------------------------------------ */

/*
 * The status line, flattened to one line and bounded.
 *
 * XmStringCreateLtoR turns every newline into a SEPARATE LINE of the label, and
 * a Label recomputes its size from its string -- so one multi-line reply from
 * the helper (a shell error, a usage message, anything unexpected) makes the
 * label taller than the space the MainWindow gave it, and with no window
 * manager to renegotiate the shell it simply draws past the bottom of the
 * window onto the root. That is what the second button-press run photographed,
 * and it looks like the panel has corrupted the screen.
 *
 * So: newlines become spaces and the whole thing is bounded. The label also has
 * XmNrecomputeSize False (see main), which is the belt to this braces -- either
 * alone would do, and the failure is ugly enough to be worth both.
 */
static void set_status(const char *text)
{
	char one[400];
	size_t i = 0;
	XmString xs;

	for (; text != NULL && *text != '\0' && i < sizeof(one) - 4; text++) {
		one[i++] = (*text == '\n' || *text == '\r' || *text == '\t') ? ' ' : *text;
	}
	if (text != NULL && *text != '\0') {
		one[i++] = '.'; one[i++] = '.'; one[i++] = '.';
	}
	one[i] = '\0';

	xs = XmStringCreateLtoR(one, XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(status_w, XmNlabelString, xs, NULL);
	XmStringFree(xs);
}

/*
 * Say we are busy, and mean it.
 *
 * Every helper call blocks the event loop for as long as it takes, and setting
 * the password or the server runs the agent binary -- which on a 66 MHz R5000
 * is a visible pause. Without this the window freezes mid-click with no
 * explanation. The status text is pushed out with XmUpdateDisplay() because the
 * expose that would paint it cannot be processed until we come back.
 */
static void set_busy(const char *text, int busy)
{
	static Cursor watch_cursor = None;
	Display *dpy = XtDisplay(toplevel);

	if (text != NULL)
		set_status(text);

	if (dpy != NULL && XtIsRealized(toplevel)) {
		if (busy) {
			if (watch_cursor == None)
				watch_cursor = XCreateFontCursor(dpy, XC_watch);
			if (watch_cursor != None)
				XDefineCursor(dpy, XtWindow(toplevel), watch_cursor);
		} else {
			XUndefineCursor(dpy, XtWindow(toplevel));
		}
		XFlush(dpy);
	}
	XmUpdateDisplay(toplevel);
}

/*
 * Run one helper verb and return its output.
 *
 * The buffer is static and reused, which is fine because nothing here is
 * re-entrant: Motif callbacks run one at a time and each finishes with the
 * answer before returning. Arguments are quoted with single quotes and any
 * embedded quote is escaped, because a password or a server key is arbitrary
 * text arriving from a text field.
 */
static const char *helper(const char *verb, const char *field, const char *value)
{
	static char out[8192];
	char cmd[4096];
	size_t n = 0;
	FILE *p;

	out[0] = '\0';

	if (field == NULL) {
		sprintf(cmd, "%s %s 2>&1", helper_path, verb);
	} else {
		char q[2048];
		const char *s;
		size_t j = 0;

		/* '...' with '\'' for each embedded quote. */
		q[j++] = '\'';
		for (s = (value != NULL ? value : ""); *s != '\0' && j < sizeof(q) - 8; s++) {
			if (*s == '\'') {
				q[j++] = '\''; q[j++] = '\\'; q[j++] = '\''; q[j++] = '\'';
			} else {
				q[j++] = *s;
			}
		}
		q[j++] = '\'';
		q[j] = '\0';
		sprintf(cmd, "%s %s %s %s 2>&1", helper_path, verb, field, q);
	}

	p = popen(cmd, "r");
	if (p == NULL) {
		strcpy(out, "could not run the helper script");
		return out;
	}
	n = fread(out, 1, sizeof(out) - 1, p);
	out[n] = '\0';
	pclose(p);

	/* One trailing newline is the shell's, not the message's. */
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return out;
}

/* ------------------------------------------------------------------ *
 * dialogs
 * ------------------------------------------------------------------ */

static void show_msg(const char *title, const char *text, int is_error)
{
	Widget dlg;
	XmString xs, xt;

	if (is_error) {
		if (error_dialog == NULL) {
			error_dialog = XmCreateErrorDialog(toplevel, "errorDialog", NULL, 0);
			/* Message boxes arrive with Cancel and Help buttons we
			 * have no use for. Only worth doing once. */
			XtUnmanageChild(XmMessageBoxGetChild(error_dialog, XmDIALOG_CANCEL_BUTTON));
			XtUnmanageChild(XmMessageBoxGetChild(error_dialog, XmDIALOG_HELP_BUTTON));
		}
		dlg = error_dialog;
	} else {
		if (info_dialog == NULL) {
			info_dialog = XmCreateInformationDialog(toplevel, "infoDialog", NULL, 0);
			XtUnmanageChild(XmMessageBoxGetChild(info_dialog, XmDIALOG_CANCEL_BUTTON));
			XtUnmanageChild(XmMessageBoxGetChild(info_dialog, XmDIALOG_HELP_BUTTON));
		}
		dlg = info_dialog;
	}

	xs = XmStringCreateLtoR((char *)text, XmSTRING_DEFAULT_CHARSET);
	xt = XmStringCreateLtoR((char *)title, XmSTRING_DEFAULT_CHARSET);
	XtVaSetValues(dlg, XmNmessageString, xs, XmNdialogTitle, xt, NULL);
	XmStringFree(xs);
	XmStringFree(xt);
	XtManageChild(dlg);
}

/*
 * The log window: a scrolling read-only text area in its own shell.
 *
 * Its own shell rather than a dialog because a person reads the log *while*
 * changing something and watching what happens -- a modal box would make that
 * two operations instead of one.
 */
static void show_log(void)
{
	const char *text = helper("showlog", NULL, NULL);

	if (log_shell == NULL) {
		Arg args[8];
		int n = 0;

		log_shell = XtVaCreatePopupShell("logShell", topLevelShellWidgetClass,
						 toplevel, XmNtitle, "Agent log", NULL);
		XtSetArg(args[n], XmNeditable, False); n++;
		XtSetArg(args[n], XmNcursorPositionVisible, False); n++;
		XtSetArg(args[n], XmNeditMode, XmMULTI_LINE_EDIT); n++;
		XtSetArg(args[n], XmNscrollHorizontal, False); n++;
		XtSetArg(args[n], XmNwordWrap, True); n++;
		log_text = XmCreateScrolledText(log_shell, "logText", args, n);
		XtManageChild(log_text);
	}
	XmTextSetString(log_text, (char *)text);
	XtPopup(log_shell, XtGrabNone);
}

/* ------------------------------------------------------------------ *
 * reading the current state
 * ------------------------------------------------------------------ */

static void set_label(Widget w, const char *text)
{
	XmString xs = XmStringCreateLtoR((char *)text, XmSTRING_DEFAULT_CHARSET);

	XtVaSetValues(w, XmNlabelString, xs, NULL);
	XmStringFree(xs);
}

/* One `key=value` line out of the helper's `status` block. */
static const char *field_of(const char *blob, const char *key, char *buf, size_t cap)
{
	const char *p = blob;
	size_t klen = strlen(key);

	buf[0] = '\0';
	while (p != NULL && *p != '\0') {
		if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
			const char *v = p + klen + 1;
			const char *e = strchr(v, '\n');
			size_t len = (e != NULL) ? (size_t)(e - v) : strlen(v);

			if (len >= cap)
				len = cap - 1;
			memcpy(buf, v, len);
			buf[len] = '\0';
			return buf;
		}
		p = strchr(p, '\n');
		if (p != NULL)
			p++;
	}
	return buf;
}

/*
 * Pull everything the panel shows in one helper call.
 *
 * One round trip rather than one per field: each is a fork, an exec and a read
 * of the config, and on this hardware six of those in a row is a visible stall
 * every time the window refreshes.
 */
static void refresh_state(void)
{
	char v[1024];
	const char *blob;
	int running, present;

	set_busy("Reading the agent's settings...", 1);
	blob = helper("status", NULL, NULL);

	present = (strcmp(field_of(blob, "present", v, sizeof(v)), "yes") == 0);
	running = (strcmp(field_of(blob, "running", v, sizeof(v)), "yes") == 0);

	set_label(id_value, field_of(blob, "id", v, sizeof(v)));
	if (!present)
		set_label(state_value, "the agent binary was not found");
	else
		set_label(state_value, running ? "running" : "stopped");

	/* Blank until the agent has logged one, which it does per session. An
	 * agent on the ReadDisplay fallback works and is slow enough to look
	 * broken; this is the only place a person would find that out. */
	field_of(blob, "capture", v, sizeof(v));
	set_label(capture_value, v[0] != '\0' ? v : "(not yet -- start a session)");

	XmTextFieldSetString(password_f, (char *)field_of(blob, "password", v, sizeof(v)));
	XmTextFieldSetString(server_f,   (char *)field_of(blob, "server",   v, sizeof(v)));
	XmTextFieldSetString(key_f,      (char *)field_of(blob, "key",      v, sizeof(v)));
	XmTextFieldSetString(relay_f,    (char *)field_of(blob, "relay",    v, sizeof(v)));
	XmTextFieldSetString(api_f,      (char *)field_of(blob, "api",      v, sizeof(v)));
	XmTextFieldSetString(ca_f,       (char *)field_of(blob, "ca",       v, sizeof(v)));

	XtSetSensitive(start_btn,   present && !running);
	XtSetSensitive(stop_btn,    present && running);
	XtSetSensitive(restart_btn, present);

	set_busy(running ? "The agent is running." : "The agent is stopped.", 0);
}

/* ------------------------------------------------------------------ *
 * callbacks
 * ------------------------------------------------------------------ */

/* Which text field an Apply button belongs to, passed as its client data. */
struct applyto {
	const char *field;    /* the helper's name for it */
	Widget     *text;
};

static struct applyto ap_password, ap_server, ap_key, ap_relay, ap_api, ap_ca;

static void apply_cb(Widget w, XtPointer client, XtPointer call)
{
	struct applyto *a = (struct applyto *)client;
	char *value = XmTextFieldGetString(*a->text);
	const char *out;
	char said[512];

	(void)w; (void)call;
	set_busy("Applying...", 1);
	out = helper("set", a->field, value != NULL ? value : "");
	XtFree(value);
	/*
	 * COPY IT. helper() returns a static buffer and refresh_state() calls
	 * helper() again, so `out` afterwards points at the STATUS BLOB rather
	 * than at what this Apply did -- which is precisely what the status line
	 * showed on the machine: `agent=/usr/sbin/r-deskvint-irix present=yes
	 * running=yes id=...`, one long line of key=value where a sentence
	 * belonged. Before set_status was taught to flatten newlines it was
	 * worse: the label grew to fit and drew off the bottom of the window.
	 */
	strncpy(said, out, sizeof(said) - 1);
	said[sizeof(said) - 1] = '\0';
	set_busy(said, 0);
	refresh_state();
	/* The helper's own words, and it is the only feedback there is that the
	 * setting reached the config rather than the text field. */
	set_status(said);
}

static void lifecycle_cb(Widget w, XtPointer client, XtPointer call)
{
	const char *verb = (const char *)client;
	const char *out;
	char said[512];

	(void)w; (void)call;
	set_busy(strcmp(verb, "stop") == 0 ? "Stopping..." : "Starting...", 1);
	out = helper(verb, NULL, NULL);
	/* Copied for the same reason as in apply_cb: refresh_state() reuses the
	 * buffer `out` points into. */
	strncpy(said, out, sizeof(said) - 1);
	said[sizeof(said) - 1] = '\0';
	set_busy(said, 0);
	refresh_state();
	set_status(said);
}

static void refresh_cb(Widget w, XtPointer client, XtPointer call)
{
	(void)w; (void)client; (void)call;
	refresh_state();
}

static void showid_cb(Widget w, XtPointer client, XtPointer call)
{
	(void)w; (void)client; (void)call;
	show_msg("Agent ID", helper("showid", NULL, NULL), 0);
}

static void showkey_cb(Widget w, XtPointer client, XtPointer call)
{
	(void)w; (void)client; (void)call;
	show_msg("Public key", helper("showkey", NULL, NULL), 0);
}

static void showlog_cb(Widget w, XtPointer client, XtPointer call)
{
	(void)w; (void)client; (void)call;
	show_log();
}

static void about_cb(Widget w, XtPointer client, XtPointer call)
{
	(void)w; (void)client; (void)call;
	show_msg("About",
		 "RustDesk agent for IRIX\n\n"
		 "Screen sharing and remote control for SGI workstations,\n"
		 "speaking the RustDesk protocol.\n\n"
		 "This panel only sets what the agent's own command line sets;\n"
		 "anything here can be done with r-deskvint-irix --help.",
		 0);
}

static void quit_cb(Widget w, XtPointer client, XtPointer call)
{
	(void)w; (void)client; (void)call;
	exit(0);
}

/* ------------------------------------------------------------------ *
 * building the window
 * ------------------------------------------------------------------ */

static void set_window_icon(Widget shell)
{
	Display *dpy = XtDisplay(shell);
	Pixmap pm;

	if (dpy == NULL)
		return;
	pm = XCreateBitmapFromData(dpy, RootWindow(dpy, DefaultScreen(dpy)),
				   (char *)icon_bits, ICON_WIDTH, ICON_HEIGHT);
	if (pm == None)
		return;
	XtVaSetValues(shell, XtNiconPixmap, pm, XtNiconName, "RustDesk", NULL);
}

static Widget menu_item(Widget menu, const char *name, XtCallbackProc cb, XtPointer data)
{
	Widget w = XtVaCreateManagedWidget(name, xmPushButtonWidgetClass, menu, NULL);

	XtAddCallback(w, XmNactivateCallback, cb, data);
	return w;
}

static void build_menu(Widget menubar)
{
	Widget pane, cascade;

	pane = XmCreatePulldownMenu(menubar, "filePane", NULL, 0);
	menu_item(pane, "refreshItem", refresh_cb, NULL);
	XtVaCreateManagedWidget("sep1", xmSeparatorWidgetClass, pane, NULL);
	menu_item(pane, "quit", quit_cb, NULL);
	cascade = XtVaCreateManagedWidget("file", xmCascadeButtonWidgetClass, menubar,
					  XmNsubMenuId, pane, NULL);

	pane = XmCreatePulldownMenu(menubar, "agentPane", NULL, 0);
	menu_item(pane, "startItem",   lifecycle_cb, (XtPointer)"start");
	menu_item(pane, "stopItem",    lifecycle_cb, (XtPointer)"stop");
	menu_item(pane, "restartItem", lifecycle_cb, (XtPointer)"restart");
	XtVaCreateManagedWidget("sep2", xmSeparatorWidgetClass, pane, NULL);
	menu_item(pane, "showIdItem",  showid_cb,  NULL);
	menu_item(pane, "showKeyItem", showkey_cb, NULL);
	menu_item(pane, "showLogItem", showlog_cb, NULL);
	XtVaCreateManagedWidget("agentMenu", xmCascadeButtonWidgetClass, menubar,
				XmNsubMenuId, pane, NULL);

	pane = XmCreatePulldownMenu(menubar, "helpPane", NULL, 0);
	menu_item(pane, "about", about_cb, NULL);
	cascade = XtVaCreateManagedWidget("help", xmCascadeButtonWidgetClass, menubar,
					  XmNsubMenuId, pane, NULL);
	/* Motif convention: Help sits at the right-hand end of the bar. */
	XtVaSetValues(menubar, XmNmenuHelpWidget, cascade, NULL);
}

/*
 * LAYOUT IS ROWCOLUMN, NOT FORM, AND THAT IS THE SECOND ATTEMPT.
 *
 * The first used XmForm with attachments, which is the obvious choice and does
 * not work here. A Form whose children attach to its left and right edges asks
 * the Form how wide it is while the Form is asking the children, and Motif
 * answers a circular size negotiation by collapsing to nothing: the panel came
 * up as a 180-pixel stub. Forcing a shell geometry afterwards made it worse --
 * correctly sized, completely empty, because the rows had already been laid out
 * for the collapsed size. Attaching each widget to the one before it instead
 * fixed the rows and left a Form-inside-a-Frame still reporting a preferred
 * size of nothing.
 *
 * XmRowColumn computes its size from its children and has no negotiation to
 * get wrong. Columns line up across rows because every label is a fixed width
 * with XmNrecomputeSize False, which is the same trick that makes the reference
 * implementation's list columns line up. Motif 1.2 throughout.
 */

/*
 * A vertical stack.
 *
 * XmNadjustLast False is not cosmetic. It defaults to True, which tells
 * RowColumn to stretch its final child to the container's edge -- and with a
 * Frame sizing itself to the stack at the same time, the last row of each group
 * came out with no height at all. The panel rendered perfectly except that the
 * password field and the CA bundle field, the last row of each frame, were
 * simply not there. Three rows of four, then four of five: a missing row is
 * much harder to notice than a broken one.
 */
static Widget vstack(Widget parent, const char *name)
{
	return XtVaCreateManagedWidget(name, xmRowColumnWidgetClass, parent,
		XmNorientation, XmVERTICAL,
		XmNpacking,     XmPACK_TIGHT,
		XmNadjustLast,  False,
		XmNspacing,     2,
		NULL);
}

/* One row: widgets side by side, left to right. */
static Widget hrow(Widget parent, const char *name)
{
	return XtVaCreateManagedWidget(name, xmRowColumnWidgetClass, parent,
		XmNorientation, XmHORIZONTAL,
		XmNpacking,     XmPACK_TIGHT,
		XmNadjustLast,  False,
		XmNspacing,     4,
		XmNmarginHeight, 1,
		NULL);
}

/* The fixed-width label that makes the columns line up. */
static Widget row_label(Widget row, const char *name)
{
	return XtVaCreateManagedWidget(name, xmLabelWidgetClass, row,
		XmNalignment,     XmALIGNMENT_BEGINNING,
		XmNwidth,         175,
		XmNrecomputeSize, False,
		NULL);
}

/*
 * A settings row: label, text field, Apply.
 *
 * Return in the field does the same as pressing Apply, which is what anyone
 * typing a hostname expects and costs one extra callback registration.
 */
static void field_row(Widget parent, const char *row_name, const char *label_name,
		      const char *field_name, const char *apply_name,
		      struct applyto *ap, const char *helper_field,
		      Widget *field_out)
{
	Widget row = hrow(parent, row_name);
	Widget text, apply;

	row_label(row, label_name);
	text = XtVaCreateManagedWidget(field_name, xmTextFieldWidgetClass, row, NULL);
	apply = XtVaCreateManagedWidget(apply_name, xmPushButtonWidgetClass, row, NULL);

	ap->field = helper_field;
	ap->text = field_out;
	*field_out = text;
	XtAddCallback(apply, XmNactivateCallback, apply_cb, (XtPointer)ap);
	XtAddCallback(text,  XmNactivateCallback, apply_cb, (XtPointer)ap);
}

/* A read-only "Label:  value" row, for what the panel reports rather than sets. */
static void value_row(Widget parent, const char *row_name, const char *label_name,
		      const char *value_name, Widget *value_out)
{
	Widget row = hrow(parent, row_name);

	row_label(row, label_name);
	*value_out = XtVaCreateManagedWidget(value_name, xmLabelWidgetClass, row,
		XmNalignment,     XmALIGNMENT_BEGINNING,
		XmNwidth,         330,
		XmNrecomputeSize, False,
		NULL);
}

/*
 * A titled group: a heading, a separator, then the rows.
 *
 * NO XmFrame, and that is the third attempt at this. A frame around the rows --
 * with XmFrame's own XmNchildType title child, and again with a plain heading
 * above an untitled frame -- gave its work area one row less height than the
 * work area asked for, every time. The LAST ROW OF EVERY GROUP was silently
 * clipped: four rows where there should be five, a panel that looked entirely
 * right, and nothing in any log. A spacer at the end absorbed it in a four-row
 * group and not in a five-row one, so the shortfall is not a fixed number of
 * pixels either.
 *
 * Grouping does not need a box. A bold-ish heading and a rule above the rows
 * reads the same on this desktop and takes one manager out of the size
 * negotiation, which is where the whole problem was.
 */
static Widget titled_frame(Widget parent, const char *frame_name,
			   const char *label_name)
{
	(void)frame_name;
	XtVaCreateManagedWidget(label_name, xmLabelWidgetClass, parent,
		XmNalignment, XmALIGNMENT_BEGINNING, NULL);
	XtVaCreateManagedWidget("groupRule", xmSeparatorWidgetClass, parent,
		XmNorientation, XmHORIZONTAL, NULL);
	return vstack(parent, "frameStack");
}

/*
 * Where the helper is.
 *
 * Beside the binary, which is how it is installed and how it is run out of a
 * build directory, with the installed location as the fallback. Resolved once
 * rather than searched per call, and the result is what every popen() uses --
 * so if this is wrong the panel says so on its first refresh rather than
 * failing one button at a time.
 */
static void find_helper(const char *argv0)
{
	const char *env = getenv("RD_HELPER");
	char dir[900];
	char *slash;

	if (env != NULL && *env != '\0') {
		strncpy(helper_path, env, sizeof(helper_path) - 1);
		helper_path[sizeof(helper_path) - 1] = '\0';
		return;
	}

	strncpy(dir, argv0 != NULL ? argv0 : "", sizeof(dir) - 1);
	dir[sizeof(dir) - 1] = '\0';
	slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		sprintf(helper_path, "%s/agent-helper.sh", dir);
		if (access(helper_path, X_OK) == 0)
			return;
	}
	/* Where the package puts it. This has to be here: installed, the panel
	 * is $PREFIX/sbin/r-deskvint-irix-gui, so "beside argv[0]" looks in sbin
	 * and finds nothing -- and the first install test duly fell through to
	 * the /tmp copy, which existed only because that machine had been used
	 * for development. On anyone else's machine the panel would have found
	 * no helper at all and said so on its first refresh.
	 *
	 * /usr/lib is still searched after /usr/local: the package installed
	 * there until 2026-09-17, and a panel from a new build should still find
	 * an older install rather than silently falling through to /tmp. */
	strcpy(helper_path, "/usr/local/lib/r-deskvint-irix/agent-helper.sh");
	if (access(helper_path, X_OK) == 0)
		return;
	strcpy(helper_path, "/usr/lib/r-deskvint-irix/agent-helper.sh");
	if (access(helper_path, X_OK) == 0)
		return;
	strcpy(helper_path, "/usr/sgug/lib/r-deskvint-irix/agent-helper.sh");
	if (access(helper_path, X_OK) == 0)
		return;
	strcpy(helper_path, "/tmp/agent-helper.sh");
}

/* ------------------------------------------------------------------ *
 * where the buttons are
 * ------------------------------------------------------------------ */

/*
 * With RD_GUI_GEOM set in the environment, print every managed widget's
 * position in ROOT coordinates once the window manager has placed the window.
 *
 * This exists because the panel is the first thing on this machine that a
 * pointer can usefully be aimed at, and there is no xdotool here, no wmctrl,
 * and no way to ask 4Dwm where it put anything. The alternative is reading
 * coordinates off a screen capture, which is at 1/2 scale -- so every button
 * is +-2 native pixels before any arithmetic, and a click that lands one pixel
 * outside a PushButton does nothing at all and looks exactly like injection
 * being broken. Asking the toolkit is exact and costs a few lines.
 *
 * A timeout rather than an expose handler, because the window manager places
 * the shell AFTER it is realized: XtTranslateCoords before that reports where
 * the shell asked to be, not where 4Dwm put it.
 */
static void dump_tree(Widget w)
{
	WidgetList kids = NULL;
	Cardinal   nkids = 0, i;

	if (w == NULL || !XtIsWidget(w))
		return;

	if (XtIsRealized(w) && XtIsManaged(w)) {
		Position  rx = 0, ry = 0;
		Dimension ww = 0, hh = 0;

		XtVaGetValues(w, XmNwidth, &ww, XmNheight, &hh, NULL);
		XtTranslateCoords(w, 0, 0, &rx, &ry);
		/* name, then the centre -- the centre is what gets clicked. */
		printf("geom %-16s %4d %4d  %4dx%-4d  centre %4d %4d\n",
		       XtName(w), (int)rx, (int)ry, (int)ww, (int)hh,
		       (int)rx + (int)ww / 2, (int)ry + (int)hh / 2);
	}

	if (XtIsComposite(w)) {
		XtVaGetValues(w, XtNchildren, &kids, XtNnumChildren, &nkids, NULL);
		for (i = 0; i < nkids; i++)
			dump_tree(kids[i]);
	}
}

static void geom_cb(XtPointer client, XtIntervalId *id)
{
	(void)client; (void)id;
	printf("--- geometry, root coordinates ---\n");
	dump_tree(toplevel);
	printf("--- end geometry ---\n");
	fflush(stdout);
}

int main(int argc, char *argv[])
{
	XtAppContext app;
	Widget mainw, menubar, form, inner, buttons;
	XtArgcType xargc;

	xargc = (XtArgcType)argc;
	toplevel = XtVaAppInitialize(&app, APP_CLASS, NULL, 0,
				     &xargc, argv, fallback_resources, NULL);
	argc = (int)xargc;

	find_helper(argv[0]);
	set_window_icon(toplevel);

	mainw = XmCreateMainWindow(toplevel, "mainw", NULL, 0);
	XtManageChild(mainw);

	menubar = XmCreateMenuBar(mainw, "menubar", NULL, 0);
	XtManageChild(menubar);
	build_menu(menubar);

	form = vstack(mainw, "work");

	/* What this machine is, and how to reach it with no server at all. */
	inner = titled_frame(form, "thisMachineFrame", "thisMachineFrameLabel");
	value_row(inner, "idRow",      "idLabel",      "idValue",      &id_value);
	value_row(inner, "stateRow",   "stateLabel",   "stateValue",   &state_value);
	value_row(inner, "captureRow", "captureLabel", "captureValue", &capture_value);
	field_row(inner, "passwordRow", "passwordLabel", "passwordField",
		  "passwordApply", &ap_password, "password", &password_f);

	/*
	 * The five that point it at a deployment, in the order they get filled
	 * in: the ID server and its key are what make the machine reachable, and
	 * the rest are optional and say so in their labels.
	 */
	inner = titled_frame(form, "infraFrame", "infraFrameLabel");
	field_row(inner, "serverRow", "serverLabel", "serverField", "serverApply",
		  &ap_server, "server", &server_f);
	field_row(inner, "keyRow",    "keyLabel",    "keyField",    "keyApply",
		  &ap_key,    "key",    &key_f);
	field_row(inner, "relayRow",  "relayLabel",  "relayField",  "relayApply",
		  &ap_relay,  "relay",  &relay_f);
	field_row(inner, "apiRow",    "apiLabel",    "apiField",    "apiApply",
		  &ap_api,    "api",    &api_f);
	field_row(inner, "caRow",     "caLabel",     "caField",     "caApply",
		  &ap_ca,     "ca",     &ca_f);

	buttons = hrow(form, "buttons");
	start_btn = XtVaCreateManagedWidget("startBtn", xmPushButtonWidgetClass, buttons, NULL);
	XtAddCallback(start_btn, XmNactivateCallback, lifecycle_cb, (XtPointer)"start");
	stop_btn = XtVaCreateManagedWidget("stopBtn", xmPushButtonWidgetClass, buttons, NULL);
	XtAddCallback(stop_btn, XmNactivateCallback, lifecycle_cb, (XtPointer)"stop");
	restart_btn = XtVaCreateManagedWidget("restartBtn", xmPushButtonWidgetClass, buttons, NULL);
	XtAddCallback(restart_btn, XmNactivateCallback, lifecycle_cb, (XtPointer)"restart");
	XtAddCallback(XtVaCreateManagedWidget("refreshBtn", xmPushButtonWidgetClass, buttons, NULL),
		      XmNactivateCallback, refresh_cb, NULL);

	/* recomputeSize False: whatever the helper says, this label keeps the
	 * height and width the layout gave it. See set_status. */
	status_w = XtVaCreateManagedWidget("status", xmLabelWidgetClass, mainw,
					   XmNalignment,     XmALIGNMENT_BEGINNING,
					   XmNrecomputeSize, False,
					   NULL);

	XmMainWindowSetAreas(mainw, menubar, NULL, NULL, NULL, form);
	XtVaSetValues(mainw, XmNmessageWindow, status_w, NULL);

	XtRealizeWidget(toplevel);
	/* Only when asked: an ordinary run should say nothing on stdout. */
	if (getenv("RD_GUI_GEOM") != NULL)
		XtAppAddTimeOut(app, 2500, geom_cb, NULL);
	/*
	 * After realizing, not before -- unlike irixscsitb, which scans first so
	 * it can size the window to the rows it got. Here every row is a fixed
	 * width, so there is nothing to preflight, and reading the state costs a
	 * fork: doing it first would leave a blank screen for the length of it.
	 */
	refresh_state();
	XtAppMainLoop(app);
	return 0;
}
