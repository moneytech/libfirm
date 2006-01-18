/**
 * Backend driver.
 * @author Sebastian Hack
 * @date 25.11.2004
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>

#ifdef WITH_LIBCORE
#include <libcore/lc_opts.h>
#include <libcore/lc_opts_enum.h>
#endif /* WITH_LIBCORE */

#include "obst.h"
#include "bitset.h"

#include "irprog.h"
#include "irgopt.h"
#include "irgraph.h"
#include "irdump.h"
#include "phiclass.h"
#include "irdom_t.h"
#include "iredges_t.h"
#include "irloop_t.h"
#include "irtools.h"

#include "bearch.h"
#include "firm/bearch_firm.h"
#include "ia32/bearch_ia32.h"

#include "be_t.h"
#include "benumb_t.h"
#include "beutil.h"
#include "benode_t.h"
#include "beirgmod.h"
#include "besched_t.h"
#include "belistsched.h"
#include "belive_t.h"
#include "bespillilp.h"
#include "bespillbelady.h"
#include "bera.h"
#include "beraextern.h"
#include "bechordal_t.h"
#include "beifg.h"
#include "beifg_impl.h"
#include "becopyoptmain.h"
#include "becopystat.h"
#include "bessadestr.h"


#define DUMP_INITIAL		(1 << 0)
#define DUMP_SCHED			(1 << 1)
#define DUMP_PREPARED		(1 << 2)
#define DUMP_RA				(1 << 3)
#define DUMP_FINAL			(1 << 4)

/* options visible for anyone */
be_options_t be_options = {
	/* ilp server */
	"i44pc52.info.uni-karlsruhe.de",

	/* ilp solver */
	"cplex"
};

/* dump flags */
static unsigned dump_flags = DUMP_INITIAL | DUMP_SCHED | DUMP_PREPARED | DUMP_RA | DUMP_FINAL;

/* register allocator to use. */
static const be_ra_t *ra = &be_ra_chordal_allocator;

/* back end instruction set architecture to use */
static const arch_isa_if_t *isa_if = &ia32_isa_if;

#ifdef WITH_LIBCORE

static lc_opt_entry_t *be_grp_root = NULL;

/* possible dumping options */
static const lc_opt_enum_mask_items_t dump_items[] = {
	{ "none",       0 },
	{ "initial",    DUMP_INITIAL },
	{ "sched",      DUMP_SCHED  },
	{ "prepared",   DUMP_PREPARED },
	{ "regalloc",   DUMP_RA },
	{ "final",      DUMP_FINAL },
	{ "all",        2 * DUMP_FINAL - 1 },
	{ NULL,         0 }
};

/* register allocators */
static const lc_opt_enum_const_ptr_items_t ra_items[] = {
	{ "chordal", &be_ra_chordal_allocator },
	{ NULL,      NULL }
};

/* instruction set architectures. */
static const lc_opt_enum_const_ptr_items_t isa_items[] = {
	{ "firm",    &firm_isa },
	{ "ia32",    &ia32_isa_if },
	{ NULL,      NULL }
};

static lc_opt_enum_mask_var_t dump_var = {
	&dump_flags, dump_items
};

static lc_opt_enum_const_ptr_var_t ra_var = {
	(const void **) &ra, ra_items
};

static lc_opt_enum_const_ptr_var_t isa_var = {
	(const void **) &isa_if, isa_items
};

static const lc_opt_table_entry_t be_main_options[] = {
	LC_OPT_ENT_ENUM_MASK("dump", "dump irg on several occasions", &dump_var),
	LC_OPT_ENT_ENUM_PTR("ra", "register allocator", &ra_var),
	LC_OPT_ENT_ENUM_PTR("isa", "the instruction set architecture", &isa_var),

	LC_OPT_ENT_STR ("ilp.server", "the ilp server name", be_options.ilp_server, sizeof(be_options.ilp_server)),
	LC_OPT_ENT_STR ("ilp.solver", "the ilp solver name", be_options.ilp_solver, sizeof(be_options.ilp_solver)),
	{ NULL }
};

#endif /* WITH_LIBCORE */

void be_opt_register(void)
{
#ifdef WITH_LIBCORE
	int i;

	be_grp_root = lc_opt_get_grp(firm_opt_get_root(), "be");

	lc_opt_add_table(be_grp_root, be_main_options);

	/* register register allocator options */
	for(i = 0; ra_items[i].name != NULL; ++i) {
		const be_ra_t *ra = ra_items[i].value;
		ra->register_options(be_grp_root);
	}

	/* register isa options */
	for(i = 0; isa_items[i].name != NULL; ++i) {
		const arch_isa_if_t *isa = isa_items[i].value;
		isa->register_options(be_grp_root);
	}
#endif /* WITH_LIBCORE */
}


void be_init(void)
{
	be_opt_register();

	be_sched_init();
	be_liveness_init();
	be_numbering_init();
	be_copy_opt_init();
	copystat_init();
	phi_class_init();
}

static be_main_env_t *be_init_env(be_main_env_t *env)
{
  obstack_init(&env->obst);
  env->dbg = firm_dbg_register("be.main");

  env->arch_env = obstack_alloc(&env->obst, sizeof(env->arch_env[0]));
  arch_env_init(env->arch_env, isa_if);

  /* Register the irn handler of the architecture */
  if (arch_isa_get_irn_handler(env->arch_env->isa))
		arch_env_add_irn_handler(env->arch_env, arch_isa_get_irn_handler(env->arch_env->isa));

  /*
   * Register the node handler of the back end infrastructure.
   * This irn handler takes care of the platform independent
   * spill, reload and perm nodes.
   */
  arch_env_add_irn_handler(env->arch_env, &be_node_irn_handler);

  return env;
}

static void be_done_env(be_main_env_t *env)
{
	env->arch_env->isa->impl->done(env->arch_env->isa);
	obstack_free(&env->obst, NULL);
}

static void dump(int mask, ir_graph *irg, const char *suffix,
				 void (*dumper)(ir_graph *, const char *))
{
	if(dump_flags & mask)
		dumper(irg, suffix);
}

static void prepare_graph(be_main_env_t *s, ir_graph *irg)
{
	/* Normalize proj nodes. */
	normalize_proj_nodes(irg);

	/* Remove critical edges */
	remove_critical_cf_edges(irg);

	/* Compute the dominance information. */
	free_dom(irg);
	compute_doms(irg);

	/* Ensure, that the ir_edges are computed. */
	edges_activate(irg);

	/* Compute loop nesting information (for weighting copies) */
	if (get_irg_loopinfo_state(irg) != (loopinfo_valid & loopinfo_cf_consistent))
		construct_cf_backedges(irg);

	be_check_dominance(irg);
}

static void be_main_loop(FILE *file_handle)
{
	int i, n;
	arch_isa_t *isa;
	be_main_env_t env;

	be_init_env(&env);

	isa = arch_env_get_isa(env.arch_env);

	/* For all graphs */
	for(i = 0, n = get_irp_n_irgs(); i < n; ++i) {
		ir_graph *irg = get_irp_irg(i);

		arch_code_generator_t *cg;
		const arch_code_generator_if_t *cg_if;

		DBG((env.dbg, LEVEL_2, "====> IRG: %F\n", irg));
		dump(DUMP_INITIAL, irg, "-begin", dump_ir_block_graph);

		/* set the current graph (this is important for several firm functions) */
		current_ir_graph = irg;

		/* Get the code generator interface. */
		cg_if = isa->impl->get_code_generator(isa);

		/* get a code generator for this graph. */
		cg = cg_if->init(file_handle, irg, env.arch_env);

		/* create the code generator and generate code. */
		prepare_graph(&env, irg);
		arch_code_generator_prepare_graph(cg);

		edges_deactivate(irg);
		dead_node_elimination(irg);
		edges_activate(irg);

		dump(DUMP_PREPARED, irg, "-prepared", dump_ir_block_graph);

		/* Schedule the graphs. */
		arch_code_generator_before_sched(cg);
		list_sched(isa, irg);

		dump(DUMP_SCHED, irg, "-sched", dump_ir_block_graph_sched);

		/* Verify the schedule */
		sched_verify_irg(irg);

		/* Do register allocation */
		arch_code_generator_before_ra(cg);
		ra->allocate(&env, irg);

		dump(DUMP_RA, irg, "-ra", dump_ir_block_graph_sched);

		arch_code_generator_done(cg);
		dump(DUMP_FINAL, irg, "-end", dump_ir_block_graph_sched);
	}

	be_done_env(&env);
}

void be_main(FILE *file_handle)
{
	be_node_init();
	be_main_loop(file_handle);
}
