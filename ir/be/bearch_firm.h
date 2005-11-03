
/**
 * @file   bearch_firm.h
 * @date   11.05.2005
 * @author Sebastian Hack
 *
 * An instruction set architecture made up of firm nodes.
 *
 * Copyright (C) 2005 Universitaet Karlsruhe
 * Released under the GPL
 */

#ifndef _BEARCH_FIRM_H
#define _BEARCH_FIRM_H

#include "bearch.h"

extern const arch_isa_if_t firm_isa;
extern const arch_irn_handler_t firm_irn_handler;

/* TODO UGLY*/
int is_Imm(const ir_node *irn);

tarval  *get_Imm_tv(ir_node *irn);
ir_node *get_Imm_sc(ir_node *irn);

typedef struct {
  enum  { imm_Const, imm_SymConst } tp;
  union {
    tarval  *tv;
    ir_node *symconst;
  } data;
} imm_attr_t;

#endif /* _BEARCH_FIRM_H */
