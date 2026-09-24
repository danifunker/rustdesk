/* The Control Strip module: one 'sdev' code resource in a file of type
 * 'sdev', for the Control Strip Modules folder. */
#include "Retro68.r"
#include "MacTypes.r"

type 'sdev' {
    RETRO68_CODE_TYPE
};

resource 'sdev' (128, locked) {
    dontBreakAtEntry, $$read("cstrip.flt");
};

resource 'vers' (1) {
    0x00, 0x01, development, 0x00, verUS,
    "0.1d",
    "C-Desk-Vint in the Control Strip"
};
