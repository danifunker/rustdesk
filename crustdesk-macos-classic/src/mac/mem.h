/* Big buffers: the screen's shadow and planes, the encoder, the send queue.
 *
 * At 1280x1024 in millions of colours they come to about 11 MB, more than
 * the application's partition may hold. So a block the partition cannot give
 * comes from the Process Manager's temporary memory instead (the free RAM
 * outside every partition), locked for as long as it is used -- which is also
 * what makes it safe for the engine to touch at deferred-task time.
 * Main loop only.
 */
#ifndef CDV_MEM_H
#define CDV_MEM_H

#include <Multiverse.h>

void *big_alloc(long size);   /* zeroed; NULL if neither heap has it */
void big_free(void *p);
long big_temp_bytes(void);    /* how much is in temporary memory now */

#endif
