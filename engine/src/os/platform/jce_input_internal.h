/*
 * jce_input_internal.h  SEAM C, second half — the state machine's backend slot.
 *
 * NOT a public header: it lives under engine/src/, never under <jce/...>.  It
 * contains no SDL token, and it is inside the set scanned by
 * tools/lint/check_input_seam.py.
 *
 * jce_input_set_backend() USED to be declared here, because JceInputBackend
 * itself was internal.  Both are public now
 * (<jce/os/platform/jce_input_device.h>): a dedicated server installs the null
 * backend and a test installs a recording fake, so neither the type nor its
 * installer can live behind engine/src/.  Declaring it in two places would put
 * two spellings of one symbol into the tree -- there is exactly one, and this
 * header includes it rather than repeating it.
 *
 * The reason it must exist at all is unchanged: without an installable
 * backend, the only way to reach the device half of jce_input_submit() would
 * be to have a controller physically attached to the machine running the
 * suite, which is exactly the "cannot be tested, therefore is not tested" this
 * batch exists to remove.
 */

#ifndef JCE_INPUT_INTERNAL_H
#define JCE_INPUT_INTERNAL_H

#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_input_device.h>   /* JceInputBackend,
                                                   jce_input_set_backend */

#endif /* JCE_INPUT_INTERNAL_H */
