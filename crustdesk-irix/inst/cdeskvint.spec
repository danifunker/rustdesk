product cdeskvint
    id "C-Desk-Vint, a RustDesk agent for IRIX 5.3 and later"
    image sw
        id "C-Desk-Vint Software"
        version @VERSION@
        subsys base default
            id "C-Desk-Vint agent, started at boot (MIPS I, o32)"
            replaces self
            exp cdeskvint.sw.base
        endsubsys
    endimage
endproduct
