#include "moar.h"
#include <platform/threads.h>

#if defined(_MSC_VER)
#define snprintf _snprintf
#endif

#ifndef _WIN32
#  include <unistd.h>
#else
#  include <process.h>
#endif

#define init_mutex(loc, name) do { \
    if ((init_stat = uv_mutex_init(&loc)) < 0) { \
        fprintf(stderr, "MoarVM: Initialization of " name " mutex failed\n    %s\n", \
            uv_strerror(init_stat)); \
        exit(1); \
    } \
} while (0)

static void setup_std_handles(MVMThreadContext *tc);

static FILE *fopen_perhaps_with_pid(char *path, const char *mode) {
    if (strstr(path, "%d")) {
        MVMuint64 path_length = strlen(path);
        MVMuint64 found_percents = 0;
        MVMuint64 i;

        /* Let's sanitize the format string a bit. Must only have
         * a single printf-recognized directive. */
        for (i = 0; i < path_length; i++) {
            if (path[i] == '%') {
                /* %% is all right. */
                if (i + 1 < path_length && path[i + 1] == '%') {
                    i++; continue;
                }
                found_percents++;
            }
        }
        /* We expect to pass only a single argument to snprintf here;
         * just bail out if there's more than one directive. */
        if (found_percents > 1) {
            return fopen(path, mode);
        } else {
            char *fixed_path = malloc(path_length + 16);
            FILE *result;
            MVMint64 pid;
#ifdef _WIN32
            pid = _getpid();
#else
            pid = getpid();
#endif
            /* We make the brave assumption that
             * pids only go up to 16 characters. */
            snprintf(fixed_path, path_length + 16, path, pid);
            result = fopen(fixed_path, mode);
            free(fixed_path);
            return result;
        }
    } else {
        return fopen(path, mode);
    }
}

/* Create a new instance of the VM. */
MVMInstance * MVM_vm_create_instance(void) {
    MVMInstance *instance;
    char *spesh_log, *spesh_nodelay, *spesh_disable, *spesh_inline_disable,
         *spesh_osr_disable, *spesh_limit;
    char *jit_log, *jit_disable, *jit_bytecode_dir;
    char *dynvar_log;
    int init_stat;

    /* Set up instance data structure. */
    instance = MVM_calloc(1, sizeof(MVMInstance));

    /* Create the main thread's ThreadContext and stash it. */
    instance->main_thread = MVM_tc_create(NULL, instance);

    instance->main_thread->thread_id = 1;

    /* Next thread to be created gets ID 2 (the main thread got ID 1). */
    MVM_store(&instance->next_user_thread_id, 2);

    /* Set up the permanent roots storage. */
    instance->num_permroots         = 0;
    instance->alloc_permroots       = 16;
    instance->permroots             = MVM_malloc(sizeof(MVMCollectable **) * instance->alloc_permroots);
    instance->permroot_descriptions = MVM_malloc(sizeof(char *) * instance->alloc_permroots);
    init_mutex(instance->mutex_permroots, "permanent roots");

    /* Create fixed size allocator. */
    instance->fsa = MVM_fixed_size_create(instance->main_thread);

    /* Set up REPR registry mutex. */
    init_mutex(instance->mutex_repr_registry, "REPR registry");

    /* Set up HLL config mutex. */
    init_mutex(instance->mutex_hllconfigs, "hll configs");

    /* Set up DLL registry mutex. */
    init_mutex(instance->mutex_dll_registry, "REPR registry");

    /* Set up extension registry mutex. */
    init_mutex(instance->mutex_ext_registry, "extension registry");

    /* Set up extension op registry mutex. */
    init_mutex(instance->mutex_extop_registry, "extension op registry");

    /* Set up weak reference hash mutex. */
    init_mutex(instance->mutex_sc_weakhash, "sc weakhash");

    /* Set up loaded compunits hash mutex. */
    init_mutex(instance->mutex_loaded_compunits, "loaded compunits");

    /* Set up container registry mutex. */
    init_mutex(instance->mutex_container_registry, "container registry");

    /* Set up persistent object ID hash mutex. */
    init_mutex(instance->mutex_object_ids, "object ID hash");

    /* Allocate all things during following setup steps directly in gen2, as
     * they will have program lifetime. */
    MVM_gc_allocate_gen2_default_set(instance->main_thread);

    /* Set up integer constant and string cache. */
    init_mutex(instance->mutex_int_const_cache, "int constant cache");
    instance->int_const_cache = MVM_calloc(1, sizeof(MVMIntConstCache));
    instance->int_to_str_cache = MVM_calloc(MVM_INT_TO_STR_CACHE_SIZE, sizeof(MVMString *));

    /* Initialize Unicode database and NFG. */
    MVM_unicode_init(instance->main_thread);
    MVM_string_cclass_init(instance->main_thread);
    MVM_nfg_init(instance->main_thread);

    /* Bootstrap 6model. It is assumed the GC will not be called during this. */
    MVM_6model_bootstrap(instance->main_thread);

    /* Fix up main thread's usecapture and last_payload. */
    instance->main_thread->cur_usecapture = MVM_repr_alloc_init(instance->main_thread, instance->CallCapture);
    instance->main_thread->last_payload = instance->VMNull;

    /* Initialize event loop thread starting mutex. */
    init_mutex(instance->mutex_event_loop_start, "event loop thread start");

    /* Create main thread object, and also make it the start of the all threads
     * linked list. */
    MVM_store(&instance->threads,
        (instance->main_thread->thread_obj = (MVMThread *)
            REPR(instance->boot_types.BOOTThread)->allocate(
                instance->main_thread, STABLE(instance->boot_types.BOOTThread))));
    instance->threads->body.stage = MVM_thread_stage_started;
    instance->threads->body.tc = instance->main_thread;
    instance->threads->body.native_thread_id = MVM_platform_thread_id();
    instance->threads->body.thread_id = instance->main_thread->thread_id;

    /* Create compiler registry */
    instance->compiler_registry = MVM_repr_alloc_init(instance->main_thread, instance->boot_types.BOOTHash);

    /* Set up compiler registr mutex. */
    init_mutex(instance->mutex_compiler_registry, "compiler registry");

    /* Create hll symbol tables */
    instance->hll_syms = MVM_repr_alloc_init(instance->main_thread, instance->boot_types.BOOTHash);

    /* Set up hll symbol tables mutex. */
    init_mutex(instance->mutex_hll_syms, "hll syms");

    /* Create callsite intern pool. */
    instance->callsite_interns = MVM_calloc(1, sizeof(MVMCallsiteInterns));
    init_mutex(instance->mutex_callsite_interns, "callsite interns");

    /* There's some callsites we statically use all over the place. Intern
     * them, so that spesh may end up optimizing more "internal" stuff. */
    MVM_callsite_initialize_common(instance->main_thread);

    /* Multi-cache additions mutex. */
    init_mutex(instance->mutex_multi_cache_add, "multi-cache addition");

    /* Current instrumentation level starts at 1; used to trigger all frames
     * to be verified before their first run. */
    instance->instrumentation_level = 1;

    /* Mutex for spesh installations, and check if we've a file we
     * should log specializations to. */
    init_mutex(instance->mutex_spesh_install, "spesh installations");
    spesh_log = getenv("MVM_SPESH_LOG");
    if (spesh_log && strlen(spesh_log))
        instance->spesh_log_fh = fopen_perhaps_with_pid(spesh_log, "w");
    spesh_disable = getenv("MVM_SPESH_DISABLE");
    if (!spesh_disable || strlen(spesh_disable) == 0) {
        instance->spesh_enabled = 1;
        spesh_inline_disable = getenv("MVM_SPESH_INLINE_DISABLE");
        if (!spesh_inline_disable || strlen(spesh_inline_disable) == 0)
            instance->spesh_inline_enabled = 1;
        spesh_osr_disable = getenv("MVM_SPESH_OSR_DISABLE");
        if (!spesh_osr_disable || strlen(spesh_osr_disable) == 0)
            instance->spesh_osr_enabled = 1;
    }

    /* Should we specialize without warm up delays? Used to find bugs in the
     * specializer and JIT. */
    spesh_nodelay = getenv("MVM_SPESH_NODELAY");
    if (spesh_nodelay && strlen(spesh_nodelay)) {
        instance->spesh_nodelay = 1;
    }

    /* Should we limit the number of specialized frames produced? (This is
     * mostly useful for building spesh bug bisect tools.) */
    spesh_limit = getenv("MVM_SPESH_LIMIT");
    if (spesh_limit && strlen(spesh_limit))
        instance->spesh_limit = atoi(spesh_limit);

    /* JIT environment/logging setup. */
    jit_disable = getenv("MVM_JIT_DISABLE");
    if (!jit_disable || strlen(jit_disable) == 0)
        instance->jit_enabled = 1;
    jit_log = getenv("MVM_JIT_LOG");
    if (jit_log && strlen(jit_log))
        instance->jit_log_fh = fopen_perhaps_with_pid(jit_log, "w");
    jit_bytecode_dir = getenv("MVM_JIT_BYTECODE_DIR");
    if (jit_bytecode_dir && strlen(jit_bytecode_dir)) {
        char *bytecode_map_name = MVM_malloc(strlen(jit_bytecode_dir) + strlen("/jit-map.txt") + 1);
        sprintf(bytecode_map_name, "%s/jit-map.txt", jit_bytecode_dir);
        instance->jit_bytecode_map = fopen(bytecode_map_name, "w");
        instance->jit_bytecode_dir = jit_bytecode_dir;
        MVM_free(bytecode_map_name);
    }
    instance->jit_seq_nr = 0;

    /* Various kinds of debugging that can be enabled. */
    dynvar_log = getenv("MVM_DYNVAR_LOG");
    if (dynvar_log && strlen(dynvar_log)) {
        instance->dynvar_log_fh = fopen_perhaps_with_pid(dynvar_log, "w");
        fprintf(instance->dynvar_log_fh, "+ x 0 0 0 0 0 %"PRIu64"\n", uv_hrtime());
        fflush(instance->dynvar_log_fh);
        instance->dynvar_log_lasttime = uv_hrtime();
    }
    else
        instance->dynvar_log_fh = NULL;
    instance->nfa_debug_enabled = getenv("MVM_NFA_DEB") ? 1 : 0;
    if (getenv("MVM_CROSS_THREAD_WRITE_LOG")) {
        instance->cross_thread_write_logging = 1;
        instance->cross_thread_write_logging_include_locked =
            getenv("MVM_CROSS_THREAD_WRITE_LOG_INCLUDE_LOCKED") ? 1 : 0;
        instance->instrumentation_level++;
        init_mutex(instance->mutex_cross_thread_write_logging,
            "cross thread write logging output");
    }
    else {
        instance->cross_thread_write_logging = 0;
    }

    if (getenv("MVM_COVERAGE_LOG")) {
        char *coverage_log = getenv("MVM_COVERAGE_LOG");
        instance->coverage_logging = 1;
        instance->instrumentation_level++;
        if (strlen(coverage_log))
            instance->coverage_log_fh = fopen_perhaps_with_pid(coverage_log, "a");
        else
            instance->coverage_log_fh = stderr;
    }
    else {
        instance->coverage_logging = 0;
    }

    /* Create std[in/out/err]. */
    setup_std_handles(instance->main_thread);

    /* Back to nursery allocation, now we're set up. */
    MVM_gc_allocate_gen2_default_clear(instance->main_thread);

    return instance;
}

/* Set up some standard file handles. */
static void setup_std_handles(MVMThreadContext *tc) {
    tc->instance->stdin_handle  = MVM_file_get_stdstream(tc, 0);
    MVM_gc_root_add_permanent_desc(tc, (MVMCollectable **)&tc->instance->stdin_handle,
        "stdin handle");

    tc->instance->stdout_handle = MVM_file_get_stdstream(tc, 1);
    MVM_gc_root_add_permanent_desc(tc, (MVMCollectable **)&tc->instance->stdout_handle,
        "stdout handle");

    tc->instance->stderr_handle = MVM_file_get_stdstream(tc, 2);
    MVM_gc_root_add_permanent_desc(tc, (MVMCollectable **)&tc->instance->stderr_handle,
        "stderr handle");
}

/* This callback is passed to the interpreter code. It takes care of making
 * the initial invocation. */
static void toplevel_initial_invoke(MVMThreadContext *tc, void *data) {
    /* Create initial frame, which sets up all of the interpreter state also. */
    MVM_frame_invoke(tc, (MVMStaticFrame *)data, MVM_callsite_get_common(tc, MVM_CALLSITE_ID_NULL_ARGS), NULL, NULL, NULL, -1);
}

/* Loads bytecode from the specified file name and runs it. */
void MVM_vm_run_file(MVMInstance *instance, const char *filename) {
    /* Map the compilation unit into memory and dissect it. */
    MVMThreadContext *tc = instance->main_thread;
    MVMCompUnit      *cu = MVM_cu_map_from_file(tc, filename);

    MVMROOT(tc, cu, {
        /* The call to MVM_string_utf8_decode() may allocate, invalidating the
           location cu->body.filename */
        MVMString *const str = MVM_string_utf8_c8_decode(tc, instance->VMString, filename, strlen(filename));
        cu->body.filename = str;

        /* Run deserialization frame, if there is one. */
        if (cu->body.deserialize_frame) {
            MVM_interp_run(tc, toplevel_initial_invoke, cu->body.deserialize_frame);
        }
    });

    /* Run the entry-point frame. */
    MVM_interp_run(tc, toplevel_initial_invoke, cu->body.main_frame);
}

/* Loads bytecode from the specified file name and dumps it. */
void MVM_vm_dump_file(MVMInstance *instance, const char *filename) {
    /* Map the compilation unit into memory and dissect it. */
    MVMThreadContext *tc = instance->main_thread;
    MVMCompUnit      *cu = MVM_cu_map_from_file(tc, filename);
    char *dump = MVM_bytecode_dump(tc, cu);
    size_t dumplen = strlen(dump);
    int position = 0;

    /* libuv already set up stdout to be nonblocking, but it can very well be
     * we encounter EAGAIN (Resource temporarily unavailable), so we need to
     * loop over our buffer, which can be quite big.
     *
     * The CORE.setting.moarvm has - as of writing this - about 32 megs of
     * output from dumping.
     */
    while (position < dumplen) {
        size_t written = write(1, dump + position, dumplen - position);
        if (written > 0)
            position += written;
    }

    MVM_free(dump);
}

/* Exits the process as quickly as is gracefully possible, respecting that
 * foreground threads should join first. Leaves all cleanup to the OS, as it
 * will be able to do it much more swiftly than we could. This is typically
 * not the right thing for embedding; see MVM_vm_destroy_instance for that. */
void MVM_vm_exit(MVMInstance *instance) {
    /* Join any foreground threads. */
    MVM_thread_join_foreground(instance->main_thread);

    /* Close any spesh or jit log. */
    if (instance->spesh_log_fh)
        fclose(instance->spesh_log_fh);
    if (instance->jit_log_fh)
        fclose(instance->jit_log_fh);
    if (instance->jit_bytecode_map)
        fclose(instance->jit_bytecode_map);
    if (instance->dynvar_log_fh) {
        fprintf(instance->dynvar_log_fh, "- x 0 0 0 0 %"PRId64" %"PRIu64" %"PRIu64"\n", instance->dynvar_log_lasttime, uv_hrtime(), uv_hrtime());
        fclose(instance->dynvar_log_fh);
    }

    /* And, we're done. */
    exit(0);
}

static void cleanup_callsite_interns(MVMInstance *instance) {
    int i;

    for (i = 0; i < MVM_INTERN_ARITY_LIMIT; i++) {
        int callsite_count = instance->callsite_interns->num_by_arity[i];
        int j;

        if (callsite_count) {
            MVMCallsite **callsites = instance->callsite_interns->by_arity[i];

            for (j = 0; j < callsite_count; j++) {
                MVMCallsite *callsite = callsites[j];

                if (MVM_callsite_is_common(callsite)) {
                    continue;
                }

                MVM_callsite_destroy(callsite);
            }

            MVM_free(callsites);
        }
    }
    MVM_free(instance->callsite_interns);
}

/* Destroys a VM instance. This must be called only from the main thread. It
 * should clear up all resources and free all memory; in practice, it falls
 * short of this goal at the moment. */
void MVM_vm_destroy_instance(MVMInstance *instance) {
    /* Join any foreground threads. */
    MVM_thread_join_foreground(instance->main_thread);

    /* Run the GC global destruction phase. After this,
     * no 6model object pointers should be accessed. */
    MVM_gc_global_destruction(instance->main_thread);

    /* Cleanup REPR registry */
    uv_mutex_destroy(&instance->mutex_repr_registry);
    MVM_HASH_DESTROY(hash_handle, MVMReprRegistry, instance->repr_hash);
    MVM_free(instance->repr_list);

    /* Clean up GC permanent roots related resources. */
    uv_mutex_destroy(&instance->mutex_permroots);
    MVM_free(instance->permroots);
    MVM_free(instance->permroot_descriptions);

    /* Clean up Hash of HLLConfig. */
    uv_mutex_destroy(&instance->mutex_hllconfigs);
    MVM_HASH_DESTROY(hash_handle, MVMHLLConfig, instance->compiler_hll_configs);
    MVM_HASH_DESTROY(hash_handle, MVMHLLConfig, instance->compilee_hll_configs);

    /* Clean up Hash of DLLs. */
    uv_mutex_destroy(&instance->mutex_dll_registry);
    MVM_HASH_DESTROY(hash_handle, MVMDLLRegistry, instance->dll_registry);

    /* Clean up Hash of extensions. */
    uv_mutex_destroy(&instance->mutex_ext_registry);
    MVM_HASH_DESTROY(hash_handle, MVMExtRegistry, instance->ext_registry);

    /* Clean up Hash of extension ops. */
    uv_mutex_destroy(&instance->mutex_extop_registry);
    MVM_HASH_DESTROY(hash_handle, MVMExtOpRegistry, instance->extop_registry);

    /* Clean up Hash of all known serialization contexts, along with list. */
    uv_mutex_destroy(&instance->mutex_sc_weakhash);
    MVM_HASH_DESTROY(hash_handle, MVMSerializationContextBody, instance->sc_weakhash);
    MVM_free(instance->all_scs);

    /* Clean up Hash of filenames of compunits loaded from disk. */
    uv_mutex_destroy(&instance->mutex_loaded_compunits);
    MVM_HASH_DESTROY(hash_handle, MVMLoadedCompUnitName, instance->loaded_compunits);

    /* Clean up Container registry. */
    uv_mutex_destroy(&instance->mutex_container_registry);
    MVM_HASH_DESTROY(hash_handle, MVMContainerRegistry, instance->container_registry);

    /* Clean up Hash of compiler objects keyed by name. */
    uv_mutex_destroy(&instance->mutex_compiler_registry);

    /* Clean up Hash of hashes of symbol tables per hll. */
    uv_mutex_destroy(&instance->mutex_hll_syms);

    /* Clean up multi cache addition mutex. */
    uv_mutex_destroy(&instance->mutex_multi_cache_add);

    /* Clean up interned callsites */
    uv_mutex_destroy(&instance->mutex_callsite_interns);
    cleanup_callsite_interns(instance);

    /* Release this interpreter's hold on Unicode database */
    MVM_unicode_release(instance->main_thread);

    /* Clean up spesh install mutex and close any log. */
    uv_mutex_destroy(&instance->mutex_spesh_install);
    if (instance->spesh_log_fh)
        fclose(instance->spesh_log_fh);
    if (instance->jit_log_fh)
        fclose(instance->jit_log_fh);
    if (instance->dynvar_log_fh)
        fclose(instance->dynvar_log_fh);

    /* Clean up cross-thread-write-logging mutex */
    uv_mutex_destroy(&instance->mutex_cross_thread_write_logging);

    /* Clean up NFG. */
    uv_mutex_destroy(&instance->nfg->update_mutex);
    MVM_nfg_destroy(instance->main_thread);

    /* Clean up fixed size allocator */
    MVM_fixed_size_destroy(instance->fsa);

    /* Clean up integer constant and string cache. */
    uv_mutex_destroy(&instance->mutex_int_const_cache);
    MVM_free(instance->int_const_cache);
    MVM_free(instance->int_to_str_cache);

    /* Clean up event loop starting mutex. */
    uv_mutex_destroy(&instance->mutex_event_loop_start);

    /* Destroy main thread contexts. */
    MVM_tc_destroy(instance->main_thread);

    /* Clear up VM instance memory. */
    MVM_free(instance);
}

void MVM_vm_set_clargs(MVMInstance *instance, int argc, char **argv) {
    instance->num_clargs = argc;
    instance->raw_clargs = argv;
}

void MVM_vm_set_exec_name(MVMInstance *instance, const char *exec_name) {
    instance->exec_name = exec_name;
}

void MVM_vm_set_prog_name(MVMInstance *instance, const char *prog_name) {
    instance->prog_name = prog_name;
}

void MVM_vm_set_lib_path(MVMInstance *instance, int count, const char **lib_path) {
    enum { MAX_COUNT = sizeof instance->lib_path / sizeof *instance->lib_path };

    int i = 0;

    if (count > MAX_COUNT)
        MVM_panic(1, "Cannot set more than %i library paths", MAX_COUNT);

    for (; i < count; ++i)
        instance->lib_path[i] = lib_path[i];

    /* Clear remainder to allow repeated calls */
    for (; i < MAX_COUNT; ++i)
        instance->lib_path[i] = NULL;
}
