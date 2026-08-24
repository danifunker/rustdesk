product rustdesk_agent
    id "RustDesk agent for IRIX"
    image sw
        id "RustDesk Agent Software"
        version @VERSION@
        subsys @SUBSYS@ default
            id "RustDesk agent, settings panel and helper (@ABI_DESC@)"
            replaces self
            exp rustdesk_agent.sw.@SUBSYS@
        endsubsys
    endimage
endproduct
