/* spu_fn_registry_stub.c -- minimal spu_lifted_lookup for spu_interp.c
 * (from the 2026-08-14 upstream fold, see runtime/spu/spu_interp.h).
 *
 * Upstream's own spu_fn_registry.c (the "single owner" spu_interp.c's header
 * comment refers to) wasn't pulled in by this project's merge. Always
 * returning NULL forces pure interpretation with no lifted-code fallback --
 * exactly what the GT6_PDI_USE_INTERPRETER oracle in spu_lifted_job.h wants
 * (a clean A/B: 100% lifted vs. 100% interpreted, not a mix). A real hybrid
 * registry bridging to spu_register_function()/spu_begin_image() in
 * spu_channels.c would be a reasonable follow-up if computed branches ever
 * need to reach compiled code mid-interpretation.
 */
#include "spu_interp.h"

spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    (void)ctx;
    (void)lsa;
    return NULL;
}
