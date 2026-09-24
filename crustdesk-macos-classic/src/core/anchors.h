/* The certificate authorities the console's https is checked against:
 * ISRG (Let's Encrypt) roots, generated from certs/ by tools/gen-anchors.py.
 * A console behind another CA needs its root added there. */
#ifndef CDV_ANCHORS_H
#define CDV_ANCHORS_H

#include <bearssl.h>

extern const br_x509_trust_anchor cdv_anchors[];
extern const size_t cdv_anchors_count;

#endif
