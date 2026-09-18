product r_deskvint_irix
    id "R-DeskVint for IRIX"
    image sw
        id "R-DeskVint Software"
        version @VERSION@
        subsys @SUBSYS@ default
            id "R-DeskVint agent, settings panel and helper (@ABI_DESC@)"
            replaces self
            exp r_deskvint_irix.sw.@SUBSYS@
        endsubsys
    endimage
endproduct
