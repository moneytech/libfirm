/**
 * Author:      Daniel Grund
 * Date:		11.04.2005
 * Copyright:   (c) Universitaet Karlsruhe
 * Licence:     This file protected by GPL -  GNU GENERAL PUBLIC LICENSE.

 * Main header for the optimization reducing the copies needed for:
 * - phi coalescing
 * - register-constrained nodes
 *
 * Checker included.
 * By request some statistics are collected too.
 */

#ifndef _BECOPYOPTMAIN_H
#define _BECOPYOPTMAIN_H

#include "irgraph.h"
#include "bearch.h"

void be_copy_opt_init(void);
void be_copy_opt(ir_graph* irg, const arch_env_t *env, const arch_register_class_t *cls);

#endif
