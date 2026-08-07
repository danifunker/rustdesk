-- "Agent for RustDesk PPC.app" -- the whole user interface.
--
-- Compiled by deploy/bundle.sh with `osacompile -o "Agent for RustDesk PPC.app"`, which
-- produces a real application: Apple's own applet Mach-O as the executable,
-- with this script inside. Everything that touches the system is a
-- `do shell script` call into Contents/Resources/agent-helper.sh, so the shell
-- half stays testable from a terminal and this file stays about dialogs.
--
-- WHY AN APPLET RATHER THAN A SHELL SCRIPT AS CFBundleExecutable. Both measured
-- on 10.5.8 rather than assumed:
--   * LaunchServices refuses a non-trivial shell-script app with -10810
--     (kLSUnknownErr). A three-line one launches; the real one never did,
--     whatever was done to Info.plist, Resources or the bundle name.
--   * Even when such an app launches, only its FIRST osascript is user
--     interactive; every later one fails -1713 "No user interaction allowed",
--     because a script app never becomes a foreground application.
-- An applet has neither problem: verified showing three dialogs in a row,
-- including one `with hidden answer`.
--
-- ASCII ONLY, and no continuation glyphs. AppleScript's continuation character
-- is U+00AC and 10.5's osacompile reads a script file as MacRoman, so a UTF-8
-- file breaks its own line continuations and reports the error on an unrelated
-- line. Long strings are built up statement by statement instead.
--
-- Handler names are not free: `ask` is a reserved term ("application constant
-- or consideration") and `on ask(...)` will not compile. Found by compiling
-- one-line variants until one failed. bundle.sh compiles this file at build
-- time, which is the only way to catch that class of thing without a Mac.

on run
    set helper to quoted form of (POSIX path of (path to me) & "Contents/Resources/agent-helper.sh")
    set h to "/bin/sh " & helper

    repeat
        set statusText to sh(h & " status")
        set menuText to sh(h & " menu")
        set choice to choose from list splitLines(menuText) with prompt statusText with title "Agent for RustDesk PPC" OK button name "Choose" cancel button name "Quit"
        if choice is false then exit repeat
        set act to item 1 of choice

        if act is "Install" then
            note_(sh(h & " install"))

        else if act is "Uninstall" then
            if confirm_("Remove the RustDesk agent from this Mac?") then
                note_(sh(h & " uninstall"))
            end if

        else if act is "Start the agent" then
            note_(sh(h & " start"))

        else if act is "Stop the agent" then
            note_(sh(h & " stop"))

        else if act is "Set the password" then
            set m to "Password someone types to connect to this Mac." & return
            set m to m & "At least 6 characters."
            set v to askHidden(m)
            if v is not false then note_(sh(h & " set password " & quoted form of v))

        else if act is "Set the ID server" then
            set m to "Host name of your RustDesk (hbbs) server." & return
            set m to m & "Leave it empty to turn registration off and reach this Mac by IP only."
            set v to askText(m, sh(h & " conf rendezvous_server"))
            if v is not false then note_(sh(h & " set server " & quoted form of v))

        else if act is "Set the server key" then
            set m to "The key from your server - the same string a RustDesk client puts in" & return
            set m to m & "its Key field. Only needed if your relay was started with -k." & return
            set m to m & "Leave it empty for none."
            set v to askText(m, sh(h & " conf server_key"))
            if v is not false then note_(sh(h & " set key " & quoted form of v))

        else if act is "Set the relay server" then
            set m to "Use a specific relay instead of the one your ID server names." & return
            set m to m & "Leave it empty to use whichever it names, which is normally right."
            set v to askText(m, sh(h & " conf relay_server"))
            if v is not false then note_(sh(h & " set relay " & quoted form of v))

        else if act is "Show my public key" then
            set m to "Someone connecting to this Mac by IP can pin this key:" & return & return
            set m to m & sh(h & " showkey")
            note_(m)

        else if act is "Show the log" then
            note_(sh(h & " showlog"))

        else if act starts with "Why is there no websocket" then
            note_(whyNotSupported())
        end if
    end repeat
end run

-- One place that runs shell commands, so a failure becomes a dialog rather than
-- the app silently quitting.
on sh(cmd)
    try
        return do shell script cmd
    on error errMsg
        return "Something went wrong:" & return & errMsg
    end try
end sh

on note_(msg)
    display dialog msg buttons {"OK"} default button 1 with title "Agent for RustDesk PPC"
end note_

on confirm_(msg)
    set r to display dialog msg buttons {"Cancel", "Yes"} default button 1 with title "Agent for RustDesk PPC"
    return (button returned of r) is "Yes"
end confirm_

on askText(prompt_, current)
    try
        set r to display dialog prompt_ default answer current buttons {"Cancel", "Save"} default button 2 with title "Agent for RustDesk PPC"
        return text returned of r
    on error number -128
        return false
    end try
end askText

on askHidden(prompt_)
    try
        set r to display dialog prompt_ default answer "" with hidden answer buttons {"Cancel", "Save"} default button 2 with title "Agent for RustDesk PPC"
        return text returned of r
    on error number -128
        return false
    end try
end askHidden

on whyNotSupported()
    set m to "Neither setting would do anything here, so neither is offered." & return & return
    set m to m & "Websockets: this agent has no websocket transport, and one would not help." & return
    set m to m & "The open-source RustDesk server answers websocket registration with" & return
    set m to m & "NOT_SUPPORT and closes the connection - real registration exists only in" & return
    set m to m & "its UDP handler. The native protocol this agent speaks needs neither a" & return
    set m to m & "websocket client nor TLS on Mac OS X 10.5." & return & return
    set m to m & "API server: that field only matters to a client signing in to an account." & return
    set m to m & "This agent has no account and never contacts an API server. Worth knowing:" & return
    set m to m & "a client that DOES hold a login token cannot connect to this agent at all," & return
    set m to m & "which looks like the agent's fault and is not."
    return m
end whyNotSupported

-- `do shell script` returns LF-separated text while AppleScript's `return`
-- constant is a carriage return, so splitting on `return` yields one long item
-- and the menu shows a single unusable row.
on splitLines(t)
    set {tid, AppleScript's text item delimiters} to {AppleScript's text item delimiters, linefeed}
    set parts to text items of t
    set AppleScript's text item delimiters to tid
    set out to {}
    repeat with p in parts
        if (p as text) is not "" then set end of out to (p as text)
    end repeat
    return out
end splitLines
