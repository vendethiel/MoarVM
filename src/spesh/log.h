/* Information about an inserted guard instruction due to logging. */
struct MVMSpeshLogGuard {
    /* Instruction and containing basic block. */
    MVMSpeshIns *ins;
    MVMSpeshBB  *bb;

    /* Have we made use of the gurad? */
    MVMuint32 used;
};

/* The default number of entries collected into a thread's spesh log buffer
 * before it is sent to a specialization worker. */
#define MVM_SPESH_LOG_DEFAULT_ENTRIES 4096

void MVM_spesh_log_create_for_thread(MVMThreadContext *tc);

/* These are part of the legacy spesh logging mechanism, and will be removed
 * (or very significantly changed) in the future. */
#define MVM_SPESH_LOG_RUNS  8
void MVM_spesh_log_add_logging(MVMThreadContext *tc, MVMSpeshGraph *g, MVMint32 osr);
