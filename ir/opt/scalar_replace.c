/*
 * Copyright (C) 1995-2011 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief   Scalar replacement of compounds.
 * @author  Beyhan Veliev, Michael Beck
 * @version $Id$
 */
#include "config.h"

#include <string.h>

#include "iroptimize.h"
#include "scalar_replace.h"
#include "opt_init.h"
#include "irflag_t.h"
#include "irouts.h"
#include "set.h"
#include "pset.h"
#include "array.h"
#include "tv.h"
#include "ircons_t.h"
#include "hashptr.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irnode_t.h"
#include "irpass.h"
#include "irtools.h"
#include "xmalloc.h"
#include "debug.h"
#include "error.h"

#define SET_VNUM(node, vnum) set_irn_link(node, INT_TO_PTR(vnum))
#define GET_VNUM(node)       (unsigned)PTR_TO_INT(get_irn_link(node))

/**
 * A path element entry: it is either an entity
 * or a tarval, because we evaluate only constant array
 * accesses like a.b.c[8].d
 */
typedef union {
	ir_entity *ent;
	ir_tarval *tv;
} path_elem_t;

/**
 * An access path, used to assign value numbers
 * to variables that will be scalar replaced.
 */
typedef struct path_t {
	unsigned    vnum;      /**< The value number. */
	size_t      path_len;  /**< The length of the access path. */
	path_elem_t path[1];   /**< The path. */
} path_t;

/** The size of a path in bytes. */
#define PATH_SIZE(p)  (sizeof(*(p)) + sizeof((p)->path[0]) * ((p)->path_len - 1))

typedef struct scalars_t {
	ir_entity *ent;              /**< A entity for scalar replacement. */
} scalars_t;

DEBUG_ONLY(static firm_dbg_module_t *dbg;)

/**
 * Compare two pathes.
 *
 * @return 0 if they are identically
 */
static int path_cmp(const void *elt, const void *key, size_t size)
{
	const path_t *p1 = (const path_t*)elt;
	const path_t *p2 = (const path_t*)key;
	(void) size;

	/* we can use memcmp here, because identical tarvals should have identical addresses */
	return memcmp(p1->path, p2->path, p1->path_len * sizeof(p1->path[0]));
}

/**
 * Compare two elements of the scalars_t set.
 *
 * @return 0 if they are identically
 */
static int ent_cmp(const void *elt, const void *key, size_t size)
{
	const scalars_t *c1 = (const scalars_t*)elt;
	const scalars_t *c2 = (const scalars_t*)key;
	(void) size;

	return c1->ent != c2->ent;
}

/**
 * Calculate a hash value for a path.
 */
static unsigned path_hash(const path_t *path)
{
	unsigned hash = 0;
	unsigned i;

	for (i = 0; i < path->path_len; ++i)
		hash ^= (unsigned)PTR_TO_INT(path->path[i].ent);

	return hash >> 4;
}

/**
 * Returns non-zero, if all indeces of a Sel node are constants.
 *
 * @param sel  the Sel node that will be checked
 */
static int is_const_sel(ir_node *sel)
{
	int i, n = get_Sel_n_indexs(sel);

	for (i = 0; i < n; ++i) {
		ir_node *idx = get_Sel_index(sel, i);

		if (!is_Const(idx))
			return 0;
	}
	return 1;
}

/**
 * Check the mode of a Load/Store with the mode of the entity
 * that is accessed.
 * If the mode of the entity and the Load/Store mode do not match, we
 * have the bad reinterpret case:
 *
 * int i;
 * char b = *(char *)&i;
 *
 * We do NOT count this as one value and return address_taken
 * in that case.
 * However, we support an often used case. If the mode is two-complement
 * we allow casts between signed/unsigned.
 *
 * @param mode     the mode of the Load/Store
 * @param ent_mode the mode of the accessed entity
 */
static int check_load_store_mode(ir_mode *mode, ir_mode *ent_mode)
{
	if (ent_mode != mode) {
		if (ent_mode == NULL ||
		    get_mode_size_bits(ent_mode) != get_mode_size_bits(mode) ||
		    get_mode_sort(ent_mode) != get_mode_sort(mode) ||
		    get_mode_arithmetic(ent_mode) != irma_twos_complement ||
		    get_mode_arithmetic(mode) != irma_twos_complement)
			return 0;
	}
	return 1;
}

/*
 * Returns non-zero, if the address of an entity
 * represented by a Sel node (or it's successor Sels) is taken.
 */
int is_address_taken(ir_node *sel)
{
	int       i, input_nr, k;
	ir_mode   *emode, *mode;
	ir_node   *value;
	ir_entity *ent;

	if (! is_const_sel(sel))
		return 1;

	for (i = get_irn_n_outs(sel) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(sel, i);

		switch (get_irn_opcode(succ)) {
		case iro_Load:
			/* do not remove volatile variables */
			if (get_Load_volatility(succ) == volatility_is_volatile)
				return 1;
			/* check if this load is not a hidden conversion */
			mode = get_Load_mode(succ);
			ent = get_Sel_entity(sel);
			emode = get_type_mode(get_entity_type(ent));
			if (! check_load_store_mode(mode, emode))
				return 1;
			break;

		case iro_Store:
			/* check that Sel is not the Store's value */
			value = get_Store_value(succ);
			if (value == sel)
				return 1;
			/* do not remove volatile variables */
			if (get_Store_volatility(succ) == volatility_is_volatile)
				return 1;
			/* check if this Store is not a hidden conversion */
			mode = get_irn_mode(value);
			ent = get_Sel_entity(sel);
			emode = get_type_mode(get_entity_type(ent));
			if (! check_load_store_mode(mode, emode))
				return 1;
			break;

		case iro_Sel: {
			int       res;
			ir_entity *entity = get_Sel_entity(succ);
			/* we can't handle unions correctly yet -> address taken */
			if (is_Union_type(get_entity_owner(entity)))
				return 1;

			/* Check the Sel successor of Sel */
			res = is_address_taken(succ);
			if (res)
				return 1;
			break;
		}

		case iro_Call:
			/* The address of an entity is given as a parameter.
			 * As long as we do not have analyses that can tell what
			 * is done with parameters, think is taken.
			 * One special case: If the Call type tells that it's a
			 * value parameter, the address is NOT taken.
			 */
			return 1;

		case iro_Id: {
			int res = is_address_taken(succ);
			if (res)
				return 1;
			break;
		}

		case iro_Tuple:
			/* Non-optimized Tuple, happens in inlining */
			for (input_nr = get_Tuple_n_preds(succ) - 1; input_nr >= 0; --input_nr) {
				ir_node *pred = get_Tuple_pred(succ, input_nr);

				if (pred == sel) {
					/* we found one input */
					for (k = get_irn_n_outs(succ) - 1; k >= 0; --k) {
						ir_node *proj = get_irn_out(succ, k);

						if (is_Proj(proj) && get_Proj_proj(proj) == input_nr) {
							int res = is_address_taken(proj);
							if (res)
								return 1;
						}
					}
				}
			}
			break;

		default:
			/* another op, the address is taken */
			return 1;
		}
	}
	return 0;
}

/**
 * Link all leave Sels with the entity.
 *
 * @param ent  the entity that will be scalar replaced
 * @param sel  a Sel node that selects some fields of this entity
 */
static int link_all_leave_sels(ir_entity *ent, ir_node *sel)
{
	int i, is_leave = 1;

	for (i = get_irn_n_outs(sel) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(sel, i);

		if (is_Sel(succ)) {
			/* the current leave has further Sel's, no leave */
			is_leave = 0;
			link_all_leave_sels(ent, succ);
		} else if (is_Id(succ)) {
			is_leave &= link_all_leave_sels(ent, succ);
		}
	}

	if (is_leave) {
		/* beware of Id's */
		sel = skip_Id(sel);

		/* we know we are at a leave, because this function is only
		 * called if the address is NOT taken, so sel's successor(s)
		 * must be Loads or Stores
		 */
		set_irn_link(sel, get_entity_link(ent));
		set_entity_link(ent, sel);
	}
	return is_leave;
}

/* we need a special address that serves as an address taken marker */
static char _x;
static void *ADDRESS_TAKEN = &_x;

/**
 * Find possible scalar replacements.
 *
 * @param irg  an IR graph
 *
 * This function finds variables on the (members of the) frame type
 * that can be scalar replaced, because their address is never taken.
 * If such a variable is found, it's entity link will hold a list of all
 * Sel nodes, that selects the atomic fields of this entity.
 * Otherwise, the link will be ADDRESS_TAKEN or NULL.
 *
 * @return  non-zero if at least one entity could be replaced
 *          potentially
 */
static int find_possible_replacements(ir_graph *irg)
{
	ir_node *irg_frame;
	ir_type *frame_tp;
	int     i, j, k, static_link_arg;
	int     res = 0;

	/*
	 * First, clear the link field of all interesting entities.
	 */
	frame_tp = get_irg_frame_type(irg);
	for (i = get_class_n_members(frame_tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_class_member(frame_tp, i);
		set_entity_link(ent, NULL);
	}

	/* check for inner functions:
	 * FIXME: need a way to get the argument position for the static link */
	static_link_arg = 0;
	for (i = get_class_n_members(frame_tp) - 1; i >= 0; --i) {
		ir_entity *ent = get_class_member(frame_tp, i);
		if (is_method_entity(ent)) {
			ir_graph *inner_irg = get_entity_irg(ent);
			ir_node  *args;

			assure_irg_outs(inner_irg);
			args = get_irg_args(inner_irg);
			for (j = get_irn_n_outs(args) - 1; j >= 0; --j) {
				ir_node *arg = get_irn_out(args, j);

				if (get_Proj_proj(arg) == static_link_arg) {
					for (k = get_irn_n_outs(arg) - 1; k >= 0; --k) {
						ir_node *succ = get_irn_out(arg, k);

						if (is_Sel(succ)) {
							ir_entity *ent = get_Sel_entity(succ);

							if (get_entity_owner(ent) == frame_tp) {
								/* found an access to the outer frame */
								set_entity_link(ent, ADDRESS_TAKEN);
							}
						}
					}
				}
			}
		}
	}

	/*
	 * Check the ir_graph for Sel nodes. If the entity of Sel
	 * isn't a scalar replacement set the link of this entity
	 * equal ADDRESS_TAKEN.
	 */
	irg_frame = get_irg_frame(irg);
	for (i = get_irn_n_outs(irg_frame) - 1; i >= 0; --i) {
		ir_node *succ = get_irn_out(irg_frame, i);

		if (is_Sel(succ)) {
			ir_entity *ent = get_Sel_entity(succ);
			ir_type *ent_type;

			/* we are only interested in entities on the frame, NOT
			   on the value type */
			if (get_entity_owner(ent) != frame_tp)
				continue;

			if (get_entity_link(ent) == ADDRESS_TAKEN)
				continue;

			/*
			 * Beware: in rare cases even entities on the frame might be
			 * volatile. This might happen if the entity serves as a store
			 * to a value that must survive a exception. Do not optimize
			 * such entities away.
			 */
			if (get_entity_volatility(ent) == volatility_is_volatile) {
				set_entity_link(ent, ADDRESS_TAKEN);
				continue;
			}

			ent_type = get_entity_type(ent);

			/* we can handle arrays, structs and atomic types yet */
			if (is_Array_type(ent_type) || is_Struct_type(ent_type) || is_atomic_type(ent_type)) {
				if (is_address_taken(succ)) {
					 /* killing one */
					if (get_entity_link(ent))
						--res;
					set_entity_link(ent, ADDRESS_TAKEN);
				} else {
					/* possible found one */
					if (get_entity_link(ent) == NULL)
						++res;
					link_all_leave_sels(ent, succ);
				}
			}
		}
	}

	return res;
}

/**
 * Return a path from the Sel node sel to it's root.
 *
 * @param sel  the Sel node
 * @param len  the length of the path so far
 */
static path_t *find_path(ir_node *sel, size_t len)
{
	size_t pos;
	int i, n;
	path_t *res;
	ir_node *pred = get_Sel_ptr(sel);

	/* the current Sel node will add some path elements */
	n    = get_Sel_n_indexs(sel);
	len += n + 1;

	if (! is_Sel(pred)) {
		/* we found the root */
		res = XMALLOCF(path_t, path, len);
		res->path_len = len;
	} else
		res = find_path(pred, len);

	pos = res->path_len - len;

	res->path[pos++].ent = get_Sel_entity(sel);
	for (i = 0; i < n; ++i) {
		ir_node *index = get_Sel_index(sel, i);

		res->path[pos++].tv = get_Const_tarval(index);
	}
	return res;
}


/**
 * Allocate value numbers for the leaves
 * in our found entities.
 *
 * @param sels  a set that will contain all Sels that have a value number
 * @param ent   the entity that will be scalar replaced
 * @param vnum  the first value number we can assign
 * @param modes a flexible array, containing all the modes of
 *              the value numbers.
 *
 * @return the next free value number
 */
static unsigned allocate_value_numbers(pset *sels, ir_entity *ent, unsigned vnum, ir_mode ***modes)
{
	ir_node *sel, *next;
	path_t *key, *path;
	set *pathes = new_set(path_cmp, 8);

	DB((dbg, SET_LEVEL_3, "  Visiting Sel nodes of entity %+F\n", ent));
	/* visit all Sel nodes in the chain of the entity */
	for (sel = (ir_node*)get_entity_link(ent); sel != NULL;
	     sel = next) {
		next = (ir_node*)get_irn_link(sel);

		/* we must mark this sel for later */
		pset_insert_ptr(sels, sel);

		key  = find_path(sel, 0);
		path = (path_t*)set_find(pathes, key, PATH_SIZE(key), path_hash(key));

		if (path) {
			SET_VNUM(sel, path->vnum);
			DB((dbg, SET_LEVEL_3, "  %+F represents value %u\n", sel, path->vnum));
		} else {
			key->vnum = vnum++;

			set_insert(pathes, key, PATH_SIZE(key), path_hash(key));

			SET_VNUM(sel, key->vnum);
			DB((dbg, SET_LEVEL_3, "  %+F represents value %u\n", sel, key->vnum));

			ARR_EXTO(ir_mode *, *modes, (int)((key->vnum + 15) & ~15));

			(*modes)[key->vnum] = get_type_mode(get_entity_type(get_Sel_entity(sel)));

			assert((*modes)[key->vnum] && "Value is not atomic");

#ifdef DEBUG_libfirm
			/* Debug output */
			{
				unsigned i;
				DB((dbg, SET_LEVEL_2, "  %s", get_entity_name(key->path[0].ent)));
				for (i = 1; i < key->path_len; ++i) {
					if (is_entity(key->path[i].ent))
						DB((dbg, SET_LEVEL_2, ".%s", get_entity_name(key->path[i].ent)));
					else
						DB((dbg, SET_LEVEL_2, "[%ld]", get_tarval_long(key->path[i].tv)));
				}
				DB((dbg, SET_LEVEL_2, " = %u (%s)\n", PTR_TO_INT(get_irn_link(sel)), get_mode_name((*modes)[key->vnum])));
			}
#endif /* DEBUG_libfirm */
		}
		free(key);
	}

	del_set(pathes);
	set_entity_link(ent, NULL);
	return vnum;
}

/**
 * A list entry for the fixing lists
 */
typedef struct list_entry_t {
	ir_node  *node;   /**< the node that must be fixed */
	unsigned vnum;    /**< the value number of this node */
} list_entry_t;

/**
 * environment for memory walker
 */
typedef struct env_t {
	int          nvals;       /**< number of values */
	ir_mode      **modes;     /**< the modes of the values */
	pset         *sels;       /**< A set of all Sel nodes that have a value number */
} env_t;

/**
 * topological post-walker.
 */
static void topologic_walker(ir_node *node, void *ctx)
{
	env_t    *env = (env_t*)ctx;
	ir_graph *irg = get_irn_irg(node);
	ir_node  *adr, *block, *mem, *val;
	ir_mode  *mode;
	unsigned vnum;

	if (is_Load(node)) {
		/* a load, check if we can resolve it */
		adr = get_Load_ptr(node);

		DB((dbg, SET_LEVEL_3, "  checking %+F for replacement ", node));
		if (! is_Sel(adr)) {
			DB((dbg, SET_LEVEL_3, "no Sel input (%+F)\n", adr));
			return;
		}

		if (! pset_find_ptr(env->sels, adr)) {
			DB((dbg, SET_LEVEL_3, "Sel %+F has no VNUM\n", adr));
			return;
		}

		/* ok, we have a Load that will be replaced */
		vnum = GET_VNUM(adr);
		assert(vnum < (unsigned)env->nvals);

		DB((dbg, SET_LEVEL_3, "replacing by value %u\n", vnum));

		block = get_nodes_block(node);
		set_cur_block(block);

		/* check, if we can replace this Load */
		val = get_value(vnum, env->modes[vnum]);

		/* Beware: A Load can contain a hidden conversion in Firm.
		This happens for instance in the following code:

		 int i;
		 unsigned j = *(unsigned *)&i;

		Handle this here. */
		mode = get_Load_mode(node);
		if (mode != get_irn_mode(val))
			val = new_rd_Conv(get_irn_dbg_info(node), block, val, mode);

		mem = get_Load_mem(node);
		turn_into_tuple(node, pn_Load_max);
		set_Tuple_pred(node, pn_Load_M,         mem);
		set_Tuple_pred(node, pn_Load_res,       val);
		set_Tuple_pred(node, pn_Load_X_regular, new_r_Jmp(block));
		set_Tuple_pred(node, pn_Load_X_except,  new_r_Bad(irg));
	} else if (is_Store(node)) {
		DB((dbg, SET_LEVEL_3, "  checking %+F for replacement ", node));

		/* a Store always can be replaced */
		adr = get_Store_ptr(node);

		if (! is_Sel(adr)) {
			DB((dbg, SET_LEVEL_3, "no Sel input (%+F)\n", adr));
			return;
		}

		if (! pset_find_ptr(env->sels, adr)) {
			DB((dbg, SET_LEVEL_3, "Sel %+F has no VNUM\n", adr));
			return;
		}

		vnum = GET_VNUM(adr);
		assert(vnum < (unsigned)env->nvals);

		DB((dbg, SET_LEVEL_3, "replacing by value %u\n", vnum));

		block = get_nodes_block(node);
		set_cur_block(block);

		/* Beware: A Store can contain a hidden conversion in Firm. */
		val = get_Store_value(node);
		if (get_irn_mode(val) != env->modes[vnum])
			val = new_rd_Conv(get_irn_dbg_info(node), block, val, env->modes[vnum]);

		set_value(vnum, val);

		mem = get_Store_mem(node);
		turn_into_tuple(node, pn_Store_max);
		set_Tuple_pred(node, pn_Store_M,         mem);
		set_Tuple_pred(node, pn_Store_X_regular, new_r_Jmp(block));
		set_Tuple_pred(node, pn_Store_X_except,  new_r_Bad(irg));
	}
}

/**
 * Make scalar replacement.
 *
 * @param sels    A set containing all Sel nodes that have a value number
 * @param nvals   The number of scalars.
 * @param modes   A flexible array, containing all the modes of
 *                the value numbers.
 */
static void do_scalar_replacements(ir_graph *irg, pset *sels, int nvals,
                                   ir_mode **modes)
{
	env_t env;

	ssa_cons_start(irg, nvals);

	env.nvals = nvals;
	env.modes = modes;
	env.sels  = sels;

	/*
	 * second step: walk over the graph blockwise in topological order
	 * and fill the array as much as possible.
	 */
	DB((dbg, SET_LEVEL_3, "Substituting Loads and Stores in %+F\n", irg));
	irg_walk_blkwise_graph(irg, NULL, topologic_walker, &env);

	ssa_cons_finish(irg);
}

/*
 * Find possible scalar replacements
 *
 * @param irg  The current ir graph.
 */
int scalar_replacement_opt(ir_graph *irg)
{
	unsigned  nvals;
	int       i;
	scalars_t key, *value;
	ir_node   *irg_frame;
	ir_mode   **modes;
	set       *set_ent;
	pset      *sels;
	ir_type   *ent_type, *frame_tp;
	int       res = 0;

	/* Call algorithm that computes the out edges */
	assure_irg_outs(irg);

	/* we use the link field to store the VNUM */
	ir_reserve_resources(irg, IR_RESOURCE_IRN_LINK);
	irp_reserve_resources(irp, IR_RESOURCE_ENTITY_LINK);

	/* Find possible scalar replacements */
	if (find_possible_replacements(irg)) {
		DB((dbg, SET_LEVEL_1, "Scalar Replacement: %+F\n", irg));

		/* Insert in set the scalar replacements. */
		irg_frame = get_irg_frame(irg);
		nvals     = 0;
		modes     = NEW_ARR_F(ir_mode *, 16);
		set_ent   = new_set(ent_cmp, 8);
		sels      = pset_new_ptr(8);
		frame_tp  = get_irg_frame_type(irg);

		for (i = get_irn_n_outs(irg_frame) - 1; i >= 0; --i) {
			ir_node *succ = get_irn_out(irg_frame, i);

			if (is_Sel(succ)) {
				ir_entity *ent = get_Sel_entity(succ);

				/* we are only interested in entities on the frame, NOT
				   on the value type */
				if (get_entity_owner(ent) != frame_tp)
					continue;

				if (get_entity_link(ent) == NULL || get_entity_link(ent) == ADDRESS_TAKEN)
					continue;

				ent_type = get_entity_type(ent);

				key.ent       = ent;
				set_insert(set_ent, &key, sizeof(key), HASH_PTR(key.ent));

#ifdef DEBUG_libfirm
				if (is_Array_type(ent_type)) {
					DB((dbg, SET_LEVEL_1, "  found array %s\n", get_entity_name(ent)));
				} else if (is_Struct_type(ent_type)) {
					DB((dbg, SET_LEVEL_1, "  found struct %s\n", get_entity_name(ent)));
				} else if (is_atomic_type(ent_type))
					DB((dbg, SET_LEVEL_1, "  found atomic value %s\n", get_entity_name(ent)));
				else {
					panic("Neither an array nor a struct or atomic value found in scalar replace");
				}
#endif /* DEBUG_libfirm */

				nvals = allocate_value_numbers(sels, ent, nvals, &modes);
			}
		}

		DB((dbg, SET_LEVEL_1, "  %u values will be needed\n", nvals));

		/* If scalars were found. */
		if (nvals > 0) {
			do_scalar_replacements(irg, sels, nvals, modes);

			foreach_set(set_ent, scalars_t*, value) {
				free_entity(value->ent);
			}

			/*
			 * We changed the graph, but did NOT introduce new blocks
			 * neither changed control flow, cf-backedges should be still
			 * consistent.
			 */
			set_irg_outs_inconsistent(irg);
			set_irg_loopinfo_inconsistent(irg);

			res = 1;
		}
		del_pset(sels);
		del_set(set_ent);
		DEL_ARR_F(modes);
	}

	ir_free_resources(irg, IR_RESOURCE_IRN_LINK);
	irp_free_resources(irp, IR_RESOURCE_ENTITY_LINK);

	return res;
}

ir_graph_pass_t *scalar_replacement_opt_pass(const char *name)
{
	return def_graph_pass_ret(name ? name : "scalar_rep",
	                          scalar_replacement_opt);
}

void firm_init_scalar_replace(void)
{
	FIRM_DBG_REGISTER(dbg, "firm.opt.scalar_replace");
}
