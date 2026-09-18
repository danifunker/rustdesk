/*
 * "R-DeskVint.app" -- a real window, built programmatically.
 *
 * Compiled on the Mac by deploy/bundle.sh with Apple's gcc 4.0.1 (Xcode 3 on
 * 10.5). Everything that touches the system is a call into
 * Contents/Resources/agent-helper.sh, so the shell half stays testable from a
 * terminal and this file stays about the interface.
 *
 * WHY NOT APPLESCRIPT, which is what this replaced. An applet can only show one
 * dialog at a time, so "configure the agent" became a chain of prompts with no
 * way to see the current settings together, change two of them, and save. That
 * is a menu, not a settings window. Cocoa gives a form.
 *
 * WHY NOT A NIB. gcc 4.0.1 is here but Interface Builder's output would be one
 * more binary artifact to keep in the tree and regenerate; the window is a
 * dozen controls, so it is laid out in code with explicit frames.
 *
 * THIS IS DELIBERATELY OLD OBJECTIVE-C. gcc 4.0.1 predates ObjC 2.0 as shipped
 * in later Xcode 3: no properties, no dot syntax, no fast enumeration, no
 * blocks, no @autoreleasepool, and NS_DURING rather than @try. Nothing here is
 * newer than 10.4 API except where noted, because the agent supports Tiger.
 * Cocoa names are the pre-10.12 ones (NSTitledWindowMask, not
 * NSWindowStyleMaskTitled) for the same reason.
 *
 * ASCII ONLY IN STRING LITERALS. gcc 4.0.1 compiles as C89, where \uXXXX is not
 * a universal character name -- it warns and does not encode what was meant. Use
 * "--" rather than an em dash and so on.
 */
#import <Cocoa/Cocoa.h>

#define APP_NAME @"R-DeskVint"

/* Rows are laid out from the top down, which reads in the order the window is
 * used; Cocoa's origin is bottom-left, so this converts once rather than making
 * every frame do the arithmetic. */
#define WIN_W 580.0
#define WIN_H 646.0
#define TOP(y, h) (WIN_H - (y) - (h))

@interface AgentController : NSObject
{
    NSString       *helperPath;
    NSWindow       *window;
    NSTextView     *statusView;
    NSTextField    *passwordField;   /* an NSSecureTextField at runtime */
    NSTextField    *idServerField;
    NSTextField    *relayField;
    NSTextField    *keyField;
    NSTextField    *consoleField;
    NSTextField    *caField;
    NSButton       *installButton;
    NSButton       *startButton;
    NSButton       *stopButton;
    NSButton       *restartButton;
    NSTextField    *noteField;
    NSTextField    *closeNote;
}
- (id)initWithHelper:(NSString *)path;
- (void)show;
- (void)about:(id)sender;
- (void)openProject:(id)sender;
@end

@implementation AgentController

/* ---------------------------------------------------------------- helpers */

/* One place that runs the shell half. Output is returned whole; a failure to
 * launch comes back as text rather than an exception, so every caller can just
 * show what it got. */
- (NSString *)run:(NSArray *)args
{
    NSTask *task = [[NSTask alloc] init];
    NSMutableArray *argv = [NSMutableArray arrayWithObject:helperPath];
    [argv addObjectsFromArray:args];
    [task setLaunchPath:@"/bin/sh"];
    [task setArguments:argv];

    NSPipe *pipe = [NSPipe pipe];
    [task setStandardOutput:pipe];
    [task setStandardError:pipe];

    NSString *out = @"";
    NS_DURING
        [task launch];
        NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
        [task waitUntilExit];
        out = [[[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] autorelease];
    NS_HANDLER
        out = [NSString stringWithFormat:@"Could not run the helper:\n%@",
               [localException reason]];
    NS_ENDHANDLER
    [task release];
    return out ? out : @"";
}

- (NSString *)conf:(NSString *)key
{
    NSString *v = [self run:[NSArray arrayWithObjects:@"conf", key, nil]];
    return [v stringByTrimmingCharactersInSet:
            [NSCharacterSet whitespaceAndNewlineCharacterSet]];
}

- (void)alert:(NSString *)text
{
    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:APP_NAME];
    [a setInformativeText:[text stringByTrimmingCharactersInSet:
                           [NSCharacterSet whitespaceAndNewlineCharacterSet]]];
    [a addButtonWithTitle:@"OK"];
    [a runModal];
}

/* ------------------------------------------------------------------ state */

/* Read everything back from the agent rather than remembering it here. The
 * config can also be changed from the command line, and a settings window that
 * shows a stale copy of the truth is worse than no window. */
- (void)refresh
{
    [[statusView textStorage] setAttributedString:
        [[[NSAttributedString alloc]
          initWithString:[self run:[NSArray arrayWithObject:@"status"]]] autorelease]];
    [statusView setFont:[NSFont userFixedPitchFontOfSize:11.0]];

    [idServerField setStringValue:[self conf:@"id_server"]];
    [relayField setStringValue:[self conf:@"relay_server"]];
    [keyField setStringValue:[self conf:@"key"]];
    [consoleField setStringValue:[self conf:@"api_server"]];
    [caField setStringValue:[self conf:@"ca_bundle"]];
    [passwordField setStringValue:@""];

    NSString *menu = [self run:[NSArray arrayWithObject:@"menu"]];
    BOOL installed = ([menu rangeOfString:@"Uninstall"].location != NSNotFound);
    BOOL running   = ([menu rangeOfString:@"Stop the agent"].location != NSNotFound);

    [installButton setTitle:(installed ? @"Uninstall" : @"Install")];
    [startButton setEnabled:(installed && !running)];
    [stopButton setEnabled:(installed && running)];
    [restartButton setEnabled:installed];

    /* Said in terms of what will actually happen, and recomputed here rather
     * than written once: closing a settings window is the moment someone
     * wonders whether they have just switched the machine off. */
    if (!installed)
        [closeNote setStringValue:@"The agent is not installed; closing changes nothing."];
    else if (running)
        [closeNote setStringValue:@"Closing this window leaves the agent running."];
    else
        [closeNote setStringValue:@"Closing this window leaves the agent stopped."];
}

/* ---------------------------------------------------------------- actions */

/* Only changed fields are written, so pressing Save with nothing edited does
 * nothing at all -- and an empty password means "leave it alone" rather than
 * "clear it", which would lock everyone out. Every other field treats empty as
 * a real value, because empty is meaningful for every one of them: no ID
 * server, no relay override, no key, no console, and no CA bundle override. */
- (void)save:(id)sender
{
    NSMutableString *done = [NSMutableString string];
    NSString *v;
    /* Counted separately from the text the helper returns. Reporting "nothing
     * changed" whenever that text came back empty conflated a genuine no-op
     * with a command that failed silently, which is exactly what happened when
     * an older installed agent did not know --relay-server. */
    int changed = 0;

    v = [idServerField stringValue];
    if (![v isEqualToString:[self conf:@"id_server"]]) {
        changed++;
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"server", v, nil]]];
    }

    v = [relayField stringValue];
    if (![v isEqualToString:[self conf:@"relay_server"]]) {
        changed++;
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"relay", v, nil]]];
    }

    v = [keyField stringValue];
    if (![v isEqualToString:[self conf:@"key"]]) {
        changed++;
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"key", v, nil]]];
    }

    v = [consoleField stringValue];
    if (![v isEqualToString:[self conf:@"api_server"]]) {
        changed++;
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"console", v, nil]]];
    }

    v = [caField stringValue];
    if (![v isEqualToString:[self conf:@"ca_bundle"]]) {
        changed++;
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"cabundle", v, nil]]];
    }

    v = [passwordField stringValue];
    if ([v length] > 0) {
        changed++;
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"password", v, nil]]];
    }

    [self refresh];
    if (changed == 0)
        [self alert:@"Nothing was changed - the fields already match what the agent has."];
    else if ([[done stringByTrimmingCharactersInSet:
               [NSCharacterSet whitespaceAndNewlineCharacterSet]] length] == 0)
        [self alert:@"The agent did not report anything back, so the change may "
                    @"not have been applied. Try Show Log."];
    else
        [self alert:done];
}

- (void)installOrRemove:(id)sender
{
    if ([[installButton title] isEqualToString:@"Uninstall"]) {
        NSAlert *a = [[[NSAlert alloc] init] autorelease];
        [a setMessageText:@"Remove the agent from this Mac?"];
        [a setInformativeText:@"The service stops and is removed. Your ID, password "
                              @"and server settings are kept, so reinstalling keeps the same ID."];
        [a addButtonWithTitle:@"Cancel"];
        [a addButtonWithTitle:@"Uninstall"];
        if ([a runModal] != NSAlertSecondButtonReturn) return;
        [self alert:[self run:[NSArray arrayWithObject:@"uninstall"]]];
    } else {
        [self alert:[self run:[NSArray arrayWithObject:@"install"]]];
    }
    [self refresh];
}

- (void)start:(id)sender   { [self alert:[self run:[NSArray arrayWithObject:@"start"]]];   [self refresh]; }
- (void)stop:(id)sender    { [self alert:[self run:[NSArray arrayWithObject:@"stop"]]];    [self refresh]; }
- (void)restart:(id)sender { [self alert:[self run:[NSArray arrayWithObject:@"restart"]]]; [self refresh]; }
- (void)showLog:(id)sender { [self alert:[self run:[NSArray arrayWithObject:@"showlog"]]]; }
- (void)refreshAction:(id)sender { [self refresh]; }

/* Quits the settings window only. The agent is a launchd job and is untouched
 * either way -- which is exactly what the line above the button says. */
- (void)closeWindow:(id)sender { [NSApp terminate:nil]; }

- (void)showKey:(id)sender
{
    [self alert:[NSString stringWithFormat:
        @"Someone connecting to this Mac by IP can pin this key:\n\n%@",
        [self run:[NSArray arrayWithObject:@"showkey"]]]];
}

/* The fork this came from, so someone holding only the .app can find the
 * source. Shown in About and opened by the button there. */
#define PROJECT_URL @"https://github.com/danifunker/rustdesk"

- (void)openProject:(id)sender
{
    [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:PROJECT_URL]];
}

/* One key out of Contents/Resources/BUILD-INFO, which bundle.sh writes. The app
 * cannot work its own commit out at runtime, and should not have to re-derive
 * its CPU from the Mach-O header. */
- (NSString *)buildInfo:(NSString *)key
{
    NSString *path = [[[NSBundle mainBundle] resourcePath]
                      stringByAppendingPathComponent:@"BUILD-INFO"];
    NSString *all = [NSString stringWithContentsOfFile:path];
    if (!all) return @"?";
    NSString *prefix = [key stringByAppendingString:@"="];
    NSEnumerator *e = [[all componentsSeparatedByString:@"\n"] objectEnumerator];
    NSString *line;
    while ((line = [e nextObject]) != nil) {
        if ([line hasPrefix:prefix]) return [line substringFromIndex:[prefix length]];
    }
    return @"?";
}

- (void)about:(id)sender
{
    NSString *commit = [self buildInfo:@"commit"];
    /* A build from a tree with uncommitted changes cannot be reproduced from
     * its commit alone, so say so where anyone reporting a problem will see
     * it, rather than only in the file. */
    NSString *dirty = ([commit rangeOfString:@"dirty"].location != NSNotFound)
        ? @"\n\nThis build was made from a tree with uncommitted changes, so"
          @"\nthe commit above does not fully describe it."
        : @"";

    NSAlert *a = [[[NSAlert alloc] init] autorelease];
    [a setMessageText:APP_NAME];
    [a setInformativeText:[NSString stringWithFormat:
        @"Version %@   (%@)\n"
        @"Built %@\n"
        @"For PowerPC %@, Mac OS X 10.4 and 10.5\n\n"
        @"R-DeskVint, an unofficial RustDesk fork. The controlled side, for PowerPC Macs, "
        @"built with mrustc because RustDesk itself needs an async runtime this "
        @"hardware has no compiler for.\n\n"
        @"Source and issues:\n%@\n(branch ppc-agent)%@",
        [self buildInfo:@"version"], commit, [self buildInfo:@"built"],
        [self buildInfo:@"arch"], PROJECT_URL, dirty]];
    [a addButtonWithTitle:@"OK"];
    [a addButtonWithTitle:@"Open GitHub Page"];
    if ([a runModal] == NSAlertSecondButtonReturn) [self openProject:nil];
}

/* Asked for, and answered honestly rather than with a control that does
 * nothing. See BACKLOG.md item 12 for the measurements. This used to say the
 * same about an API server; item 14 is why it no longer does, and the Console
 * field above is the result. */
- (void)whyNot:(id)sender
{
    [self alert:
     @"There is no websocket setting because this agent has no websocket "
     @"transport, and one would not help.\n\n"
     @"The open-source RustDesk server answers websocket registration with "
     @"NOT_SUPPORT and closes the connection -- real registration exists only "
     @"in its UDP handler. So the native protocol this agent already speaks is "
     @"the one that works.\n\n"
     @"The Console field above is a different thing, and it does work: it is "
     @"where this Mac reports in so that it appears in a device list. An ID "
     @"server makes this Mac reachable; a console makes it visible. Neither "
     @"one implies the other, and having one without the other is normal."];
}

/* ------------------------------------------------------------------ chrome */

- (NSTextField *)labelAt:(NSRect)r text:(NSString *)t bold:(BOOL)bold
{
    NSTextField *f = [[[NSTextField alloc] initWithFrame:r] autorelease];
    [f setStringValue:t];
    [f setBezeled:NO];
    [f setDrawsBackground:NO];
    [f setEditable:NO];
    [f setSelectable:NO];
    if (bold) [f setFont:[NSFont boldSystemFontOfSize:13.0]];
    else      [f setFont:[NSFont systemFontOfSize:11.0]];
    return f;
}

- (NSTextField *)fieldAt:(NSRect)r secure:(BOOL)secure
{
    NSTextField *f = secure ? [[[NSSecureTextField alloc] initWithFrame:r] autorelease]
                            : [[[NSTextField alloc] initWithFrame:r] autorelease];
    [f setFont:[NSFont systemFontOfSize:12.0]];
    return f;
}

- (NSButton *)buttonAt:(NSRect)r title:(NSString *)t action:(SEL)sel
{
    NSButton *b = [[[NSButton alloc] initWithFrame:r] autorelease];
    [b setTitle:t];
    [b setBezelStyle:NSRoundedBezelStyle];
    [b setFont:[NSFont systemFontOfSize:12.0]];
    [b setTarget:self];
    [b setAction:sel];
    return b;
}

- (id)initWithHelper:(NSString *)path
{
    self = [super init];
    if (!self) return nil;
    helperPath = [path retain];

    window = [[NSWindow alloc]
              initWithContentRect:NSMakeRect(0, 0, WIN_W, WIN_H)
                        styleMask:(NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask)
                          backing:NSBackingStoreBuffered
                            defer:NO];
    [window setTitle:APP_NAME];
    [window center];
    NSView *v = [window contentView];

    /* No heading inside the window: the title bar already says what this is,
     * and a second copy of it just pushes everything down. */

    /* Status, in a scroll view because the block grows a line whenever
     * something new is worth reporting. */
    NSScrollView *sv = [[[NSScrollView alloc]
                         initWithFrame:NSMakeRect(20, TOP(20, 152), WIN_W - 40, 152)] autorelease];
    [sv setHasVerticalScroller:YES];
    [sv setBorderType:NSBezelBorder];
    statusView = [[NSTextView alloc] initWithFrame:[[sv contentView] bounds]];
    [statusView setEditable:NO];
    [statusView setDrawsBackground:YES];
    [sv setDocumentView:statusView];
    [v addSubview:sv];

    /* The form. Every setting the agent has, all visible at once. */
    float y = 188;
    struct { NSString *label; NSString *hint; } rows[6] = {
        { @"Password:",    @"What someone types to connect. Leave blank to keep the current one." },
        { @"ID server:",   @"The same ID Server your clients use. Empty means direct IP only." },
        { @"Relay server:",@"Optional. Empty means whichever relay the ID server names." },
        { @"Server key:",  @"The same Key your clients use. Only needed if hbbr runs with -k." },
        /* The console is a different thing from the ID server and the window
         * has to say so, because having one without the other is a normal
         * state that looks like a fault: registered but in no list, or listed
         * but unreachable. */
        { @"Console:",     @"Optional. Reports in so this Mac appears in a device list." },
        { @"CA bundle:",   @"Only for an https console with a private CA. Empty is usually right." },
    };
    NSTextField **targets[6];
    passwordField = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:YES];
    idServerField = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    relayField    = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    keyField      = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    consoleField  = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    caField       = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    targets[0] = &passwordField;
    targets[1] = &idServerField;
    targets[2] = &relayField;
    targets[3] = &keyField;
    targets[4] = &consoleField;
    targets[5] = &caField;

    int i;
    for (i = 0; i < 6; i++) {
        [v addSubview:[self labelAt:NSMakeRect(20, TOP(y + 3, 18), 105, 18)
                               text:rows[i].label bold:NO]];
        NSTextField *f = *(targets[i]);
        [f setFrame:NSMakeRect(130, TOP(y, 22), WIN_W - 150, 22)];
        [v addSubview:f];
        /* One line, and it does not wrap or ellipsise -- it simply gets cut
         * off, which is how the first version of these hints lost the end of
         * two of them. Keep each under about 70 characters at this width. */
        [v addSubview:[self labelAt:NSMakeRect(132, TOP(y + 23, 14), WIN_W - 150, 14)
                               text:rows[i].hint bold:NO]];
        y += 44;
    }

    [v addSubview:[self buttonAt:NSMakeRect(WIN_W - 160, TOP(y + 2, 30), 140, 30)
                           title:@"Save Changes" action:@selector(save:)]];
    noteField = [self labelAt:NSMakeRect(20, TOP(y + 10, 16), 380, 16)
                         text:@"Saving restarts the agent if it is running." bold:NO];
    [v addSubview:noteField];
    y += 44;

    /* The service row. Install/Uninstall is one button whose title follows the
     * state, because the two are never both available. */
    installButton = [self buttonAt:NSMakeRect(20, TOP(y, 30), 120, 30)
                             title:@"Install" action:@selector(installOrRemove:)];
    startButton   = [self buttonAt:NSMakeRect(148, TOP(y, 30), 90, 30)
                             title:@"Start" action:@selector(start:)];
    stopButton    = [self buttonAt:NSMakeRect(246, TOP(y, 30), 90, 30)
                             title:@"Stop" action:@selector(stop:)];
    restartButton = [self buttonAt:NSMakeRect(344, TOP(y, 30), 90, 30)
                             title:@"Restart" action:@selector(restart:)];
    [v addSubview:installButton];
    [v addSubview:startButton];
    [v addSubview:stopButton];
    [v addSubview:restartButton];
    [v addSubview:[self buttonAt:NSMakeRect(452, TOP(y, 30), 108, 30)
                           title:@"Refresh" action:@selector(refreshAction:)]];
    y += 40;

    [v addSubview:[self buttonAt:NSMakeRect(20, TOP(y, 28), 120, 28)
                           title:@"Show Log" action:@selector(showLog:)]];
    [v addSubview:[self buttonAt:NSMakeRect(148, TOP(y, 28), 130, 28)
                           title:@"Public Key" action:@selector(showKey:)]];
    [v addSubview:[self buttonAt:NSMakeRect(286, TOP(y, 28), 274, 28)
                           title:@"Why no websocket?" action:@selector(whyNot:)]];
    y += 42;

    /* A rule, then the close row: everything above acts on the agent, the
     * button below acts only on this window, and the line says which. */
    NSBox *rule = [[[NSBox alloc] initWithFrame:NSMakeRect(20, TOP(y, 2), WIN_W - 40, 2)] autorelease];
    [rule setBoxType:NSBoxSeparator];
    [v addSubview:rule];
    y += 12;

    /* One line, truncated rather than wrapped -- as with the field hints. */
    closeNote = [self labelAt:NSMakeRect(20, TOP(y + 7, 16), WIN_W - 210, 16) text:@"" bold:NO];
    [v addSubview:closeNote];
    [v addSubview:[self buttonAt:NSMakeRect(WIN_W - 190, TOP(y, 30), 170, 30)
                           title:@"Close" action:@selector(closeWindow:)]];

    [self refresh];
    return self;
}

- (void)show
{
    [window makeKeyAndOrderFront:nil];
}

/* A settings window with nothing else to it: closing it means done. */
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app { return YES; }

@end

/* A menu bar has to be built by hand in a nib-less app, and without one there
 * is no Quit item and no Cmd-Q -- which on 10.5 leaves the user force-quitting
 * a settings window. Built after the controller exists so About has something
 * to target: a menu item whose target is nil goes to the responder chain, and
 * a plain NSObject controller is not in it. */
/* `setAppleMenu:` is how a nib-less app tells AppKit which submenu is the
 * application menu. It has never been in a public header -- every programmatic
 * Cocoa app of this vintage declares it the same way -- so it is declared here
 * rather than left for the compiler to guess a signature for. */
@interface NSApplication (RustDeskAppleMenu)
- (void)setAppleMenu:(NSMenu *)menu;
@end

static void installMenuBar(id controller)
{
    NSMenu *bar = [[[NSMenu alloc] initWithTitle:APP_NAME] autorelease];
    /* Both titles are set on purpose. Built from a nib, AppKit knows which menu
     * is the application menu and puts the app's name on it; built by hand it
     * does not, and an untitled first menu shows up as a nameless one with
     * About stranded inside it. */
    NSMenuItem *appItem = [[[NSMenuItem alloc] initWithTitle:APP_NAME
                                                     action:NULL
                                              keyEquivalent:@""] autorelease];
    [bar addItem:appItem];

    NSMenu *appMenu = [[[NSMenu alloc] initWithTitle:APP_NAME] autorelease];
    NSMenuItem *aboutItem = [appMenu addItemWithTitle:@"About " APP_NAME
                                               action:@selector(about:)
                                        keyEquivalent:@""];
    [aboutItem setTarget:controller];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *siteItem = [appMenu addItemWithTitle:@"Project Page on GitHub"
                                              action:@selector(openProject:)
                                       keyEquivalent:@""];
    [siteItem setTarget:controller];
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"Quit " APP_NAME
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];

    /* WHY THERE WERE TWO MENUS NAMED AFTER THE APP, one populated and one not.
     *
     * Adding a first item to the main menu does not make it the *application*
     * menu. Told nothing, AppKit synthesises its own -- which is the empty one
     * -- and leaves the hand-built menu sitting beside it, so About and Quit
     * end up in the second of two identically named menus. `setAppleMenu:` is
     * what adopts ours, and there is then one menu with everything in it.
     *
     * Guarded rather than called outright: the selector is private, so a system
     * that does not answer it gets exactly the behaviour it had before instead
     * of an unrecognised-selector crash on launch. */
    if ([NSApp respondsToSelector:@selector(setAppleMenu:)])
        [NSApp setAppleMenu:appMenu];

    /* Last, and not where it used to be. The bar is adopted here, so every menu
     * it carries has to be finished first -- this ran before the submenu above
     * existed, which is the other half of why the result looked half-built. */
    [NSApp setMainMenu:bar];
}

int main(int argc, const char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];

    NSString *helper = [[[NSBundle mainBundle] resourcePath]
                        stringByAppendingPathComponent:@"agent-helper.sh"];
    AgentController *c = [[AgentController alloc] initWithHelper:helper];
    installMenuBar(c);
    [NSApp setDelegate:c];
    [c show];

    /* Launched from Finder the app is already frontmost; launched any other way
     * it is not, and a settings window nobody can see is the same as no window. */
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    [pool release];
    return 0;
}
