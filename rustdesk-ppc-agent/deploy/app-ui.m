/*
 * "Agent for RustDesk PPC.app" -- a real window, built programmatically.
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
 */
#import <Cocoa/Cocoa.h>

/* Rows are laid out from the top down, which reads in the order the window is
 * used; Cocoa's origin is bottom-left, so this converts once rather than making
 * every frame do the arithmetic. */
#define WIN_W 580.0
#define WIN_H 496.0
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
    NSButton       *installButton;
    NSButton       *startButton;
    NSButton       *stopButton;
    NSButton       *restartButton;
    NSTextField    *noteField;
}
- (id)initWithHelper:(NSString *)path;
- (void)show;
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
    [a setMessageText:@"Agent for RustDesk PPC"];
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

    [idServerField setStringValue:[self conf:@"rendezvous_server"]];
    [relayField setStringValue:[self conf:@"relay_server"]];
    [keyField setStringValue:[self conf:@"server_key"]];
    [passwordField setStringValue:@""];

    NSString *menu = [self run:[NSArray arrayWithObject:@"menu"]];
    BOOL installed = ([menu rangeOfString:@"Uninstall"].location != NSNotFound);
    BOOL running   = ([menu rangeOfString:@"Stop the agent"].location != NSNotFound);

    [installButton setTitle:(installed ? @"Uninstall" : @"Install")];
    [startButton setEnabled:(installed && !running)];
    [stopButton setEnabled:(installed && running)];
    [restartButton setEnabled:installed];
}

/* ---------------------------------------------------------------- actions */

/* Only changed fields are written, so pressing Save with nothing edited does
 * nothing at all -- and an empty password means "leave it alone" rather than
 * "clear it", which would lock everyone out. Every other field treats empty as
 * a real value, because empty is meaningful for all three: no ID server, no
 * relay override, no key. */
- (void)save:(id)sender
{
    NSMutableString *done = [NSMutableString string];
    NSString *v;

    v = [idServerField stringValue];
    if (![v isEqualToString:[self conf:@"rendezvous_server"]])
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"server", v, nil]]];

    v = [relayField stringValue];
    if (![v isEqualToString:[self conf:@"relay_server"]])
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"relay", v, nil]]];

    v = [keyField stringValue];
    if (![v isEqualToString:[self conf:@"server_key"]])
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"key", v, nil]]];

    v = [passwordField stringValue];
    if ([v length] > 0)
        [done appendString:[self run:[NSArray arrayWithObjects:@"set", @"password", v, nil]]];

    [self refresh];
    if ([[done stringByTrimmingCharactersInSet:
          [NSCharacterSet whitespaceAndNewlineCharacterSet]] length] == 0)
        [self alert:@"Nothing was changed."];
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

- (void)showKey:(id)sender
{
    [self alert:[NSString stringWithFormat:
        @"Someone connecting to this Mac by IP can pin this key:\n\n%@",
        [self run:[NSArray arrayWithObject:@"showkey"]]]];
}

/* Asked for, and answered honestly rather than with controls that do nothing.
 * See BACKLOG.md item 12 for the measurements behind both halves. */
- (void)whyNot:(id)sender
{
    [self alert:
     @"Websockets and an API server are not offered because neither would do "
     @"anything here.\n\n"
     @"Websockets: this agent has no websocket transport, and one would not "
     @"help. The open-source RustDesk server answers websocket registration "
     @"with NOT_SUPPORT and closes the connection — real registration "
     @"exists only in its UDP handler.\n\n"
     @"API server: that field only matters to a client signing in to an "
     @"account. This agent has no account and never contacts one. Worth "
     @"knowing that a client which does hold a login token cannot connect to "
     @"this agent at all, which looks like the agent's fault and is not."];
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
    [window setTitle:@"Agent for RustDesk PPC"];
    [window center];
    NSView *v = [window contentView];

    /* No heading inside the window: the title bar already says what this is,
     * and a second copy of it just pushes everything down. */

    /* Status, in a scroll view because the block grows a line whenever
     * something new is worth reporting. */
    NSScrollView *sv = [[[NSScrollView alloc]
                         initWithFrame:NSMakeRect(20, TOP(20, 130), WIN_W - 40, 130)] autorelease];
    [sv setHasVerticalScroller:YES];
    [sv setBorderType:NSBezelBorder];
    statusView = [[NSTextView alloc] initWithFrame:[[sv contentView] bounds]];
    [statusView setEditable:NO];
    [statusView setDrawsBackground:YES];
    [sv setDocumentView:statusView];
    [v addSubview:sv];

    /* The form. Every setting the agent has, all visible at once. */
    float y = 166;
    struct { NSString *label; NSString *hint; } rows[4] = {
        { @"Password:",    @"what someone types to connect; leave blank to keep the current one" },
        { @"ID server:",   @"the same ID Server your clients use; empty means direct IP only" },
        { @"Relay server:",@"optional; empty means whichever relay the ID server names" },
        { @"Server key:",  @"the same Key your clients use; only needed if hbbr runs with -k" },
    };
    NSTextField **targets[4];
    passwordField = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:YES];
    idServerField = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    relayField    = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    keyField      = [self fieldAt:NSMakeRect(130, 0, WIN_W - 150, 22) secure:NO];
    targets[0] = &passwordField;
    targets[1] = &idServerField;
    targets[2] = &relayField;
    targets[3] = &keyField;

    int i;
    for (i = 0; i < 4; i++) {
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
                           title:@"Why no websocket or API setting?" action:@selector(whyNot:)]];

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
 * a settings window. */
static void installMenuBar(void)
{
    NSMenu *bar = [[[NSMenu alloc] init] autorelease];
    NSMenuItem *appItem = [[[NSMenuItem alloc] init] autorelease];
    [bar addItem:appItem];
    [NSApp setMainMenu:bar];

    NSMenu *appMenu = [[[NSMenu alloc] init] autorelease];
    [appMenu addItemWithTitle:@"Quit Agent for RustDesk PPC"
                       action:@selector(terminate:)
                keyEquivalent:@"q"];
    [appItem setSubmenu:appMenu];
}

int main(int argc, const char *argv[])
{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    [NSApplication sharedApplication];
    installMenuBar();

    NSString *helper = [[[NSBundle mainBundle] resourcePath]
                        stringByAppendingPathComponent:@"agent-helper.sh"];
    AgentController *c = [[AgentController alloc] initWithHelper:helper];
    [NSApp setDelegate:c];
    [c show];

    /* Launched from Finder the app is already frontmost; launched any other way
     * it is not, and a settings window nobody can see is the same as no window. */
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp run];
    [pool release];
    return 0;
}
