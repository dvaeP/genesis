/*
// ColdMUD was created and is copyright 1993, 1994 by Greg Hudson
//
// Genesis is a derivitive work, and is copyright 1995 by Brandon Gillespie.
// Full details and copyright information can be found in the file doc/CREDITS
//
// File: operators.c
// ---
// Operators.
*/

#include <string.h>
#include "config.h"
#include "defs.h"
#include "y.tab.h"
#include "operators.h"
#include "execute.h"
#include "memory.h"
#include "cdc_types.h"
#include "lookup.h"
#include "log.h"
#include "util.h"

/*
// -----------------------------------------------------------------
//
// The following are basic syntax operations
//
*/

void op_comment(void) {
    /* Do nothing, just increment the program counter past the comment. */
    cur_frame->pc++;
    /* actually, increment the number of ticks left too, since comments
       really don't do anything */
    cur_frame->ticks++;
    /* decrement system tick */
    tick--;
}

void op_pop(void) {
    pop(1);
}

void op_set_local(void) {
    data_t *var;

    /* Copy data in top of stack to variable. */
    var = &stack[cur_frame->var_start + cur_frame->opcodes[cur_frame->pc++]];
    data_discard(var);
    data_dup(var, &stack[stack_pos - 1]);
}

void op_set_obj_var(void) {
    long ind, id, result;
    data_t *val;

    ind = cur_frame->opcodes[cur_frame->pc++];
    id = object_get_ident(cur_frame->method->object, ind);
    val = &stack[stack_pos - 1];
    result = object_assign_var(cur_frame->object, cur_frame->method->object,
			       id, val);
    if (result == paramnf_id)
	cthrow(paramnf_id, "No such parameter %I.", id);
}

void op_if(void) {
    /* Jump if the condition is false. */
    if (!data_true(&stack[stack_pos - 1]))
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    else
	cur_frame->pc++;
    pop(1);
}

void op_else(void) {
    cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
}

void op_for_range(void) {
    int var;
    data_t *range;

    var = cur_frame->var_start + cur_frame->opcodes[cur_frame->pc + 1];
    range = &stack[stack_pos - 2];

    /* Make sure we have an integer range. */
    if (range[0].type != INTEGER || range[1].type != INTEGER) {
	cthrow(type_id, "Range bounds (%D, %D) are not both integers.",
	      &range[0], &range[1]);
	return;
    }

    if (range[0].u.val > range[1].u.val) {
	/* We're finished; pop the range and jump to the end. */
	pop(2);
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    } else {
	/* Replace the index variable with the lower range bound, increment the
	 * range, and continue. */
	data_discard(&stack[var]);
	stack[var] = range[0];
	range[0].u.val++;
	cur_frame->pc += 2;
    }
}

void op_for_list(void) {
    data_t *counter;
    data_t *domain;
    int var, len;
    list_t *pair;

    counter = &stack[stack_pos - 1];
    domain = &stack[stack_pos - 2];
    var = cur_frame->var_start + cur_frame->opcodes[cur_frame->pc + 1];

    /* Make sure we're iterating over a list.  We know the counter is okay. */
    if (domain->type != LIST && domain->type != DICT) {
	cthrow(type_id, "Domain (%D) is not a list or dictionary.", domain);
	return;
    }

    len = (domain->type == LIST) ? list_length(domain->u.list)
				 : dict_size(domain->u.dict);

    if (counter->u.val >= len) {
	/* We're finished; pop the list and counter and jump to the end. */
	pop(2);
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
	return;
    }

    /* Replace the index variable with the next list element and increment
     * the counter. */
    data_discard(&stack[var]);
    if (domain->type == LIST) {
	data_dup(&stack[var], list_elem(domain->u.list, counter->u.val));
    } else {
	pair = dict_key_value_pair(domain->u.dict, counter->u.val);
	stack[var].type = LIST;
	stack[var].u.list = pair;
    }
    counter->u.val++;
    cur_frame->pc += 2;
}

void op_while(void) {
    if (!data_true(&stack[stack_pos - 1])) {
	/* The condition expression is false.  Jump to the end of the loop. */
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    } else {
	/* The condition expression is true; continue. */
	cur_frame->pc += 2;
    }
    pop(1);
}

void op_switch(void) {
    /* This opcode doesn't actually do anything; it just provides a place-
     * holder for a break statement. */
    cur_frame->pc++;
}

void op_case_value(void) {
    /* There are two expression values on the stack: the controlling expression
     * for the switch statement, and the value for this case.  If they are
     * equal, pop them off the stack and jump to the body of this case.
     * Otherwise, just pop the value for this case, and go on. */
    if (data_cmp(&stack[stack_pos - 2], &stack[stack_pos - 1]) == 0) {
	pop(2);
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    } else {
	pop(1);
	cur_frame->pc++;
    }
}

void op_case_range(void) {
    data_t *switch_expr, *range;
    int is_match;

    switch_expr = &stack[stack_pos - 3];
    range = &stack[stack_pos - 2];

    /* Verify that range[0] and range[1] make a value type. */
    if (range[0].type != range[1].type) {
	cthrow(type_id, "%D and %D are not of the same type.",
	      &range[0], &range[1]);
	return;
    } else if (range[0].type != INTEGER && range[0].type != STRING) {
	cthrow(type_id, "%D and %D are not integers or strings.", &range[0],
	      &range[1]);
	return;
    }

    /* Decide if this is a match.  In order for it to be a match, switch_expr
     * must be of the same type as the range expressions, must be greater than
     * or equal to the lower bound of the range, and must be less than or equal
     * to the upper bound of the range. */
    is_match = (switch_expr->type == range[0].type);
    is_match = (is_match) && (data_cmp(switch_expr, &range[0]) >= 0);
    is_match = (is_match) && (data_cmp(switch_expr, &range[1]) <= 0);

    /* If it's a match, pop all three expressions and jump to the case body.
     * Otherwise, just pop the range and go on. */
    if (is_match) {
	pop(3);
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    } else {
	pop(2);
	cur_frame->pc++;
    }
}

void op_last_case_value(void) {
    /* There are two expression values on the stack: the controlling expression
     * for the switch statement, and the value for this case.  If they are
     * equal, pop them off the stack and go on.  Otherwise, just pop the value
     * for this case, and jump to the next case. */
    if (data_cmp(&stack[stack_pos - 2], &stack[stack_pos - 1]) == 0) {
	pop(2);
	cur_frame->pc++;
    } else {
	pop(1);
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    }
}

void op_last_case_range(void) {
    data_t *switch_expr, *range;
    int is_match;

    switch_expr = &stack[stack_pos - 3];
    range = &stack[stack_pos - 2];

    /* Verify that range[0] and range[1] make a value type. */
    if (range[0].type != range[1].type) {
	cthrow(type_id, "%D and %D are not of the same type.",
	      &range[0], &range[1]);
	return;
    } else if (range[0].type != INTEGER && range[0].type != STRING) {
	cthrow(type_id, "%D and %D are not integers or strings.", &range[0],
	      &range[1]);
	return;
    }

    /* Decide if this is a match.  In order for it to be a match, switch_expr
     * must be of the same type as the range expressions, must be greater than
     * or equal to the lower bound of the range, and must be less than or equal
     * to the upper bound of the range. */
    is_match = (switch_expr->type == range[0].type);
    is_match = (is_match) && (data_cmp(switch_expr, &range[0]) >= 0);
    is_match = (is_match) && (data_cmp(switch_expr, &range[1]) <= 0);

    /* If it's a match, pop all three expressions and go on.  Otherwise, just
     * pop the range and jump to the next case. */
    if (is_match) {
	pop(3);
	cur_frame->pc++;
    } else {
	pop(2);
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    }
}

void op_end_case(void) {
    /* Jump to end of switch statement. */
    cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
}

void op_default(void) {
    /* Pop the controlling switch expression. */
    pop(1);
}

void op_end(void) {
    /* Jump to the beginning of the loop or condition expression. */
    cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
}

void op_break(void) {
    int n, op;

    /* Get loop instruction from argument. */
    n = cur_frame->opcodes[cur_frame->pc];

    /* If it's a for loop, pop the loop information on the stack (either a list
     * and an index, or two range bounds. */
    op = cur_frame->opcodes[n];
    if (op == FOR_LIST || op == FOR_RANGE)
	pop(2);

    /* Jump to the end of the loop. */
    cur_frame->pc = cur_frame->opcodes[n + 1];
}

void op_continue(void) {
    /* Jump back to the beginning of the loop.  If it's a WHILE loop, jump back
     * to the beginning of the condition expression. */
    cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    if (cur_frame->opcodes[cur_frame->pc] == WHILE)
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc + 2];
}

void op_return(void) {
    long dbref;

    dbref = cur_frame->object->dbref;
    frame_return();
    if (cur_frame)
	push_dbref(dbref);
}

void op_return_expr(void) {
    data_t *val;

    /* Return, and push frame onto caller stack.  Transfers reference count to
     * caller stack.  Assumes (correctly) that there is space on the caller
     * stack. */
    val = &stack[--stack_pos];
    frame_return();
    if (cur_frame) {
	stack[stack_pos] = *val;
	stack_pos++;
    } else {
	data_discard(val);
    }
}

void op_catch(void) {
    Error_action_specifier *spec;

    /* Make a new error action specifier and push it onto the stack. */
    spec = EMALLOC(Error_action_specifier, 1);
    spec->type = CATCH;
    spec->stack_pos = stack_pos;
    spec->u.ccatch.handler = cur_frame->opcodes[cur_frame->pc++];
    spec->u.ccatch.error_list = cur_frame->opcodes[cur_frame->pc++];
    spec->next = cur_frame->specifiers;
    cur_frame->specifiers = spec;
}

void op_catch_end(void) {
    /* Pop the error action specifier for the catch statement, and jump past
     * the handler. */
    pop_error_action_specifier();
    cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
}

void op_handler_end(void) {
    pop_handler_info();
}

void op_zero(void) {
    /* Push a zero. */
    push_int(0);
}

void op_one(void) {
    /* Push a one. */
    push_int(1);
}

void op_integer(void) {
    push_int(cur_frame->opcodes[cur_frame->pc++]);
}

void op_float(void) {
    push_float(*((float*)(&cur_frame->opcodes[cur_frame->pc++])));
}

void op_string(void) {
    string_t *str;
    int ind = cur_frame->opcodes[cur_frame->pc++];

    str = object_get_string(cur_frame->method->object, ind);
    push_string(str);
}

void op_dbref(void) {
    int id;

    id = cur_frame->opcodes[cur_frame->pc++];
    push_dbref(id);
}

void op_symbol(void) {
    int ind, id;

    ind = cur_frame->opcodes[cur_frame->pc++];
    id = object_get_ident(cur_frame->method->object, ind);
    push_symbol(id);
}

void op_error(void) {
    int ind, id;

    ind = cur_frame->opcodes[cur_frame->pc++];
    id = object_get_ident(cur_frame->method->object, ind);
    push_error(id);
}

void op_name(void) {
    int ind, id;
    long dbref;

    ind = cur_frame->opcodes[cur_frame->pc++];
    id = object_get_ident(cur_frame->method->object, ind);
    if (lookup_retrieve_name(id, &dbref))
	push_dbref(dbref);
    else
	cthrow(namenf_id, "Can't find object name %I.", id);
}

void op_get_local(void) {
    int var;

    /* Push value of local variable on stack. */
    var = cur_frame->var_start + cur_frame->opcodes[cur_frame->pc++];
    check_stack(1);
    data_dup(&stack[stack_pos], &stack[var]);
    stack_pos++;
}

void op_get_obj_var(void) {
    long ind, id, result;
    data_t val;

    /* Look for variable, and push it onto the stack if we find it. */
    ind = cur_frame->opcodes[cur_frame->pc++];
    id = object_get_ident(cur_frame->method->object, ind);
    result = object_retrieve_var(cur_frame->object, cur_frame->method->object,
				 id, &val);
    if (result == paramnf_id) {
	cthrow(paramnf_id, "No such parameter %I.", id);
    } else {
	check_stack(1);
	stack[stack_pos] = val;
	stack_pos++;
    }
}

void op_start_args(void) {
    /* Resize argument stack if necessary. */
    if (arg_pos == arg_size) {
	arg_size = arg_size * 2 + ARG_STACK_MALLOC_DELTA;
	arg_starts = EREALLOC(arg_starts, int, arg_size);
    }

    /* Push stack position onto argument start stack. */
    arg_starts[arg_pos] = stack_pos;
    arg_pos++;
}

void op_pass(void) {
    int arg_start, result;

    arg_start = arg_starts[--arg_pos];

    /* Attempt to pass the message we're processing. */
    result = pass_message(arg_start, arg_start);

    if (result == numargs_id)
	interp_error(result, numargs_str);
    else if (result == methodnf_id)
	cthrow(result, "No next method found.");
    else if (result == maxdepth_id)
	cthrow(result, "Maximum call depth exceeded.");
}

void op_message(void) {
    int arg_start, result, ind;
    data_t *target;
    long message, dbref;
    Frob *frob;

    ind = cur_frame->opcodes[cur_frame->pc++];
    message = object_get_ident(cur_frame->method->object, ind);

    arg_start = arg_starts[--arg_pos];
    target = &stack[arg_start - 1];

#if 0
    write_err("##message: %s[%d] #%d #%d.%I %d",
	      ident_name(message),
              ind,
	      cur_frame->object->dbref, 
	      cur_frame->method->object->dbref,
	      (cur_frame->method->name != NOT_AN_IDENT)
	       ? cur_frame->method->name
	       : opcode_id,
	      cur_frame->method->name);
#endif

    if (target->type == DBREF) {
	dbref = target->u.dbref;
    } else if (target->type == FROB) {
	/* Convert the frob to its rep and pass as first argument. */
	frob = target->u.frob;
	dbref = frob->cclass;
	*target = frob->rep;
	arg_start--;
	TFREE(frob, 1);
    } else {
        /* JBB - changed to support messages to all object types */
        if (!lookup_retrieve_name(data_type_id(target->type), &dbref)) {
            cthrow(objnf_id,
                   "No object for data type %I.",
                   data_type_id(target->type));
            return;
	}
        arg_start--;
    }

    /* Attempt to send the message. */
    ident_dup(message);
    result = send_message(dbref, message, target - stack, arg_start);

    if (result == numargs_id)
	interp_error(result, numargs_str);
    else if (result == objnf_id)
	cthrow(result, "Target (#%l) not found.", dbref);
    else if (result == methodnf_id)
	cthrow(result, "Method %I not found.", message);
    else if (result == maxdepth_id)
	cthrow(result, "Maximum call depth exceeded.");
    else if (result == private_id)
        cthrow(result, "Method %I is private.", message);
    else if (result == protected_id)
        cthrow(result, "Method %I is protected.", message);
    else if (result == root_id)
        cthrow(result, "Method %I can only be called by $root.", message);
    else if (result == driver_id)
        cthrow(result, "Method %I can only be by the driver.", message);

    ident_discard(message);
}

void op_expr_message(void) {
    int arg_start, result;
    data_t *target, *message_data;
    long dbref, message;

    arg_start = arg_starts[--arg_pos];
    target = &stack[arg_start - 2];
    message_data = &stack[arg_start - 1];

    if (message_data->type != SYMBOL) {
	cthrow(type_id, "Message (%D) is not a symbol.", message_data);
	return;
    }
    message = ident_dup(message_data->u.symbol);

    if (target->type == DBREF) {
	dbref = target->u.dbref;
    } else if (target->type == FROB) {
	dbref = target->u.frob->cclass;

	/* Pass frob rep as first argument (where the message data is now). */
	data_discard(message_data);
	*message_data = target->u.frob->rep;
	arg_start--;

	/* Discard the frob and replace it with a dummy value. */
	TFREE(target->u.frob, 1);
	target->type = INTEGER;
	target->u.val = 0;
    } else {
        /* JBB - changed to support messages to all object types */
        if (!lookup_retrieve_name(data_type_id(target->type), &dbref)) {
            cthrow(objnf_id,
                   "No object for data type %I",
                   data_type_id(target->type));
	    ident_discard(message);
            return;
	}
        arg_start--;
        data_discard(message_data);
        message_data = &stack[arg_start -1];
    }

    /* Attempt to send the message. */
    ident_dup(message);
    result = send_message(dbref, message, target - stack, arg_start);

    if (result == numargs_id)
	interp_error(result, numargs_str);
    else if (result == objnf_id)
	cthrow(result, "Target (#%l) not found.", dbref);
    else if (result == methodnf_id)
	cthrow(result, "Method %I not found.", message);
    else if (result == maxdepth_id)
	cthrow(result, "Maximum call depth exceeded.");
    else if (result == private_id)
        cthrow(result, "Method %I is private.", message);
    else if (result == protected_id)
        cthrow(result, "Method %I is protected.", message);
    else if (result == root_id)
        cthrow(result, "Method %I can only be called by $root.", message);
    else if (result == driver_id)
        cthrow(result, "Method %I can only be by the driver.", message);

    ident_discard(message);
}

void op_list(void) {
    int start, len;
    list_t *list;
    data_t *d;

    start = arg_starts[--arg_pos];
    len = stack_pos - start;

    /* Move the elements into a list. */
    list = list_new(len);
    d = list_empty_spaces(list, len);
    MEMCPY(d, &stack[start], len);
    stack_pos = start;

    /* Push the list onto the stack where elements began. */
    push_list(list);
    list_discard(list);
}

void op_dict(void) {
    int start, len;
    list_t *list;
    data_t *d;
    Dict *dict;

    start = arg_starts[--arg_pos];
    len = stack_pos - start;

    /* Move the elements into a list. */
    list = list_new(len);
    d = list_empty_spaces(list, len);
    MEMCPY(d, &stack[start], len);
    stack_pos = start;

    /* Construct a dictionary from the list. */
    dict = dict_from_slices(list);
    list_discard(list);
    if (!dict) {
	cthrow(type_id, "Arguments were not all two-element lists.");
    } else {
	push_dict(dict);
	dict_discard(dict);
    }
}

void op_buffer(void) {
    int start, len, i;
    Buffer *buf;

    start = arg_starts[--arg_pos];
    len = stack_pos - start;
    for (i = 0; i < len; i++) {
	if (stack[start + i].type != INTEGER) {
	    cthrow(type_id, "Element %d (%D) is not an integer.", i + 1,
		  &stack[start + i]);
	    return;
	}
    }
    buf = buffer_new(len);
    for (i = 0; i < len; i++)
	buf->s[i] = ((unsigned long) stack[start + i].u.val) % (1 << 8);
    stack_pos = start;
    push_buffer(buf);
    buffer_discard(buf);
}

void op_frob(void) {
    data_t *cclass, *rep;

    cclass = &stack[stack_pos - 2];
    rep = &stack[stack_pos - 1];
    if (cclass->type != DBREF) {
	cthrow(type_id, "Class (%D) is not a dbref.", cclass);
    } else if (rep->type != LIST && rep->type != DICT) {
	cthrow(type_id, "Rep (%D) is not a list or dictionary.", rep);
    } else {
      Dbref dbref = cclass->u.dbref;
      cclass->type = FROB;
      cclass->u.frob = TMALLOC(Frob, 1);
      cclass->u.frob->cclass = dbref;
      data_dup(&cclass->u.frob->rep, rep);
      pop(1);
    }
}

void op_index(void) {
    data_t *d, *ind, element;
    int i, len;
    string_t *str;

    d = &stack[stack_pos - 2];
    ind = &stack[stack_pos - 1];
    if (d->type != LIST && d->type != STRING && d->type != DICT) {
	cthrow(type_id, "Array (%D) is not a list, string, or dictionary.", d);
	return;
    } else if (d->type != DICT && ind->type != INTEGER) {
	cthrow(type_id, "Offset (%D) is not an integer.", ind);
	return;
    } 

    if (d->type == DICT) {
	/* Get the value corresponding to a key. */
	if (dict_find(d->u.dict, ind, &element) == keynf_id) {
	    cthrow(keynf_id, "Key (%D) is not in the dictionary.", ind);
	} else {
	    pop(1);
	    data_discard(d);
	    *d = element;
	}
	return;
    }

    /* It's not a dictionary.  Make sure ind is within bounds. */
    len = (d->type == LIST) ? list_length(d->u.list) : string_length(d->u.str);
    i = ind->u.val - 1;
    if (i < 0) {
	cthrow(range_id, "Index (%d) is less than one.", i + 1);
    } else if (i > len - 1) {
	cthrow(range_id, "Index (%d) is greater than length (%d)",
	      i + 1, len);
    } else {
	/* Replace d with the element of d numbered by ind. */
	if (d->type == LIST) {
	    data_dup(&element, list_elem(d->u.list, i));
	    pop(2);
	    stack[stack_pos] = element;
	    stack_pos++;
	} else {
	    str = string_from_chars(string_chars(d->u.str) + i, 1);
	    pop(2);
	    push_string(str);
	    string_discard(str);
	}
    }
}

void op_and(void) {
    /* Short-circuit if left side is false; otherwise discard. */
    if (!data_true(&stack[stack_pos - 1])) {
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    } else {
	cur_frame->pc++;
	pop(1);
    }
}

void op_or(void) {
    /* Short-circuit if left side is true; otherwise discard. */
    if (data_true(&stack[stack_pos - 1])) {
	cur_frame->pc = cur_frame->opcodes[cur_frame->pc];
    } else {
	cur_frame->pc++;
	pop(1);
    }
}

void op_splice(void) {
    int i;
    list_t *list;
    data_t *d;

    if (stack[stack_pos - 1].type != LIST) {
	cthrow(type_id, "%D is not a list.", &stack[stack_pos - 1]);
	return;
    }
    list = stack[stack_pos - 1].u.list;

    /* Splice the list onto the stack, overwriting the list. */
    check_stack(list_length(list) - 1);
    for (d = list_first(list), i=0; d; d = list_next(list, d), i++)
	data_dup(&stack[stack_pos - 1 + i], d);
    stack_pos += list_length(list) - 1;

    list_discard(list);
}

void op_critical(void) {
    Error_action_specifier *spec;

    /* Make an error action specifier for the critical expression, and push it
     * onto the stack. */
    spec = EMALLOC(Error_action_specifier, 1);
    spec->type = CRITICAL;
    spec->stack_pos = stack_pos;
    spec->u.critical.end = cur_frame->opcodes[cur_frame->pc++];
    spec->next = cur_frame->specifiers;
    cur_frame->specifiers = spec;
}

void op_critical_end(void) {
    pop_error_action_specifier();
}

void op_propagate(void) {
    Error_action_specifier *spec;

    /* Make an error action specifier for the critical expression, and push it
     * onto the stack. */
    spec = EMALLOC(Error_action_specifier, 1);
    spec->type = PROPAGATE;
    spec->stack_pos = stack_pos;
    spec->u.propagate.end = cur_frame->opcodes[cur_frame->pc++];
    spec->next = cur_frame->specifiers;
    cur_frame->specifiers = spec;
}

void op_propagate_end(void) {
    pop_error_action_specifier();
}

/*
// -----------------------------------------------------------------
//
// The following are extended operations, math and the like
//
*/

/* All of the following functions are interpreter opcodes, so they require
   that the interpreter data (the globals in execute.c) be in a state
   consistent with interpretation.  They may modify the interpreter data
   by pushing and popping the data stack or by throwing exceptions. */

/* Effects: Pops the top value on the stack and pushes its logical inverse. */
void op_not(void) {
    data_t *d = &stack[stack_pos - 1];
    int val = !data_true(d);

    /* Replace d with the inverse of its truth value. */
    data_discard(d);
    d->type = INTEGER;
    d->u.val = val;
}

/* Effects: If the top value on the stack is an integer, pops it and pushes its
 *	    its arithmetic inverse. */
void op_negate(void) {
    data_t *d = &stack[stack_pos - 1];

    /* Replace d with -d. */
    if (d->type == INTEGER) {
	d->u.val *= -1;
    } else if (d->type == FLOAT) {
        d->u.fval *= -1;
    } else {
	cthrow(type_id, "Argument (%D) is not an integer or float.", d);
    }
}

/* Effects: If the top two values on the stack are integers, pops them and
 *	    pushes their product. */
void op_multiply(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* do type conversions */
    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
        cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (d1->type != INTEGER && d1->type != FLOAT) {
        cthrow(type_id, "%D and %D are not integers or floats.", d1, d2);
    } else {
        /* Replace d1 with d1 - d2 and pop d2 */
        if (d1->type == INTEGER)
            d1->u.val *= d2->u.val;
        else
            d1->u.fval *= d2->u.fval;
        pop(1);
    }
}

/* Effects: If the top two values on the stack are integers and the second is
 *	    not zero, pops them, divides the first by the second, and pushes
 *	    the quotient. */
void op_divide(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
        cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (d1->type != INTEGER && d1->type != FLOAT) {
        cthrow(type_id, "%D and %D are not integers or floats.", d1, d2);
    } else if (d2->type == INTEGER ? (d2->u.val == 0) : (d2->u.fval == 0.0)) {
        cthrow(div_id, "Attempt to divide %D by zero.", d1);
    } else {
        /* Replace d1 with d1 - d2, and pop d2. */
        if (d1->type == INTEGER)
            d1->u.val /= d2->u.val;
        else
            d1->u.fval /= d2->u.fval;
        pop(1);
    }
}

/* Effects: If the top two values on the stack are integers and the second is
 *	    not zero, pops them, divides the first by the second, and pushes
 *	    the remainder. */
void op_modulo(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* Make sure we're multiplying two integers. */
    if (d1->type != INTEGER) {
	cthrow(type_id, "Left side (%D) is not an integer.", d1);
    } else if (d2->type != INTEGER) {
	cthrow(type_id, "Right side (%D) is not an integer.", d2);
    } else if (d2->u.val == 0) {
	cthrow(div_id, "Attempt to divide %D by zero.", d1);
    } else {
	/* Replace d1 with d1 % d2, and pop d2. */
	d1->u.val %= d2->u.val;
	pop(1);
    }
}

/* Effects: If the top two values on the stack are integers, pops them and
 *	    pushes their sum.  If the top two values are strings, pops them,
 *	    concatenates the second onto the first, and pushes the result. */
void op_add(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    /* If we're adding two integers or two strings, replace d1 with
       d1 + d2 and discard d2. */
    if (d1->type == INTEGER && d2->type == INTEGER) {
	/* Replace d1 with d1 + d2, and pop d2. */
	d1->u.val += d2->u.val;
    } else if (d1->type == FLOAT && d2->type == FLOAT) {
        d1->u.fval += d2->u.fval;
    } else if (d1->type == STRING && d2->type == STRING) {
	anticipate_assignment();
	d1->u.str = string_add(d1->u.str, d2->u.str);
    } else if (d1->type == LIST && d2->type == LIST) {
	anticipate_assignment();
	d1->u.list = list_append(d1->u.list, d2->u.list);
    } else {
	cthrow(type_id, "Cannot add %D and %D.", d1, d2);
	return;
    }
    pop(1);
}

/* Effects: Adds two lists.  (This is used for [@foo, ...];) */
void op_splice_add(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* No need to check if d2 is a list, due to code generation. */
    if (d1->type != LIST) {
	cthrow(type_id, "%D is not a list.", d1);
	return;
    }

    anticipate_assignment();
    d1->u.list = list_append(d1->u.list, d2->u.list);
    pop(1);
}

/* Effects: If the top two values on the stack are integers, pops them and
 *	    pushes their difference. */
void op_subtract(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
        cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (d1->type != INTEGER && d1->type != FLOAT) {
        cthrow(type_id, "%D and %D are not integers or floats.", d1, d2);
    } else {
        if (d1->type == INTEGER)
            d1->u.val -= d2->u.val;
        else
            d1->u.fval -= d2->u.fval;
	pop(1);
    }
}

/* Effects: Pops the top two values on the stack and pushes 1 if they are
 *	    equal, 0 if not. */
void op_equal(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int val = (data_cmp(d1, d2) == 0);

    pop(2);
    push_int(val);
}

/* Effects: Pops the top two values on the stack and returns 1 if they are
 *	    unequal, 0 if they are equal. */   
void op_not_equal(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int val = (data_cmp(d1, d2) != 0);

    pop(2);
    push_int(val);
}

/* Definition: Two values are comparable if they are of the same type and that
 * 	       type is integer or string. */

/* Effects: If the top two values on the stack are comparable, pops them and
 *	    pushes 1 if the first is greater than the second, 0 if not. */
void op_greater(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int val, t = d1->type;

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d2->type == FLOAT && d1->type == INTEGER) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
	cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (t != INTEGER && t != STRING && t != FLOAT) {
	cthrow(type_id,"%D and %D are not integers, floats or strings.",d1,d2);
    } else {
	/* Discard d1 and d2 and push the appropriate truth value. */
	val = (data_cmp(d1, d2) > 0);
	pop(2);
	push_int(val);
    }
}

/* Effects: If the top two values on the stack are comparable, pops them and
 *	    pushes 1 if the first is greater than or equal to the second, 0 if
 *	    not. */
void op_greater_or_equal(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int val, t = d1->type;

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
	cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (t != INTEGER && t != FLOAT && t != STRING) {
	cthrow(type_id,"%D and %D are not integers, floats or strings.",d1,d2);
    } else {
	/* Discard d1 and d2 and push the appropriate truth value. */
	val = (data_cmp(d1, d2) >= 0);
	pop(2);
	push_int(val);
    }
}

/* Effects: If the top two values on the stack are comparable, pops them and
 *	    pushes 1 if the first is less than the second, 0 if not. */
void op_less(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int val, t = d1->type;

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
	cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (t != INTEGER && t != FLOAT && t != STRING) {
	cthrow(type_id,"%D and %D are not integers, floats or strings.",d1,d2);
    } else {
	/* Discard d1 and d2 and push the appropriate truth value. */
	val = (data_cmp(d1, d2) < 0);
	pop(2);
	push_int(val);
    }
}

/* Effects: If the top two values on the stack are comparable, pops them and
 *	    pushes 1 if the first is greater than or equal to the second, 0 if
 *	    not. */
void op_less_or_equal(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int val, t = d1->type;

    if (d1->type == FLOAT && d2->type == INTEGER) {
        d2->type = FLOAT;
        d2->u.fval = (float) d2->u.val;
    } else if (d1->type == INTEGER && d2->type == FLOAT) {
        d1->type = FLOAT;
        d1->u.fval = (float) d1->u.val;
    }

    if (d1->type != d2->type) {
	cthrow(type_id, "%D and %D are not of the same type.", d1, d2);
    } else if (t != INTEGER && t != FLOAT && t != STRING) {
	cthrow(type_id,"%D and %D are not integers, floats or strings.",d1,d2);
    } else {
	/* Discard d1 and d2 and push the appropriate truth value. */
	val = (data_cmp(d1, d2) <= 0);
	pop(2);
	push_int(val);
    }
}

/* Effects: If the top value on the stack is a string or a list, pops the top
 *	    two values on the stack and pushes the location of the first value
 *	    in the second (where the first element is 1), or 0 if the first
 *	    value does not exist in the second. */
void op_in(void)
{
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];
    int pos;
    char *s;

    if (d2->type == LIST) {
	pos = list_search(d2->u.list, d1);
	pop(2);
	push_int(pos + 1);
	return;
    }

    if (d1->type != STRING || d2->type != STRING) {
	cthrow(type_id, "Cannot search for %D in %D.", d1, d2);
	return;
    }

    s = strcstr(string_chars(d2->u.str), string_chars(d1->u.str));
    if (s) {
	pos = s - string_chars(d2->u.str);
    } else {
	pos = -1;
    }

    pop(2);
    push_int(pos + 1);
}

/*
// ----------------------------------------------------------------
// Bitwise integer operators.
//
// Added by Jeff Kesselman, March 1995
// ----------------------------------------------------------------
*/

/*
// Effects: If the top two values on the stack are integers 
//	    pops them, bitwise ands them, and pushes
//	    the result.
*/
void op_bwand(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* Make sure we're multiplying two integers. */
    if (d1->type != INTEGER) {
        cthrow(type_id, "Left side (%D) is not an integer.", d1);
    } else if (d2->type != INTEGER) {
        cthrow(type_id, "Right side (%D) is not an integer.", d2);
    } else if (d2->u.val == 0) {
        cthrow(div_id, "Attempt to divide %D by zero.", d1);
    } else {
        /* Replace d1 with d1 / d2, and pop d2. */
        d1->u.val &= d2->u.val;
        pop(1);
    }
}


/*
// Effects: If the top two values on the stack are integers 
//          pops them, bitwise ors them, and pushes
//          the result.
*/
void op_bwor(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* Make sure we're multiplying two integers. */
    if (d1->type != INTEGER) {
        cthrow(type_id, "Left side (%D) is not an integer.", d1);
    } else if (d2->type != INTEGER) {
        cthrow(type_id, "Right side (%D) is not an integer.", d2);
    } else if (d2->u.val == 0) {
        cthrow(div_id, "Attempt to divide %D by zero.", d1);
    } else {
        /* Replace d1 with d1 / d2, and pop d2. */
        d1->u.val |= d2->u.val;
        pop(1);
    }
}

/*
// Effects: If the top two values on the stack are integers 
//          pops them, shifts the left operand to the right
//          right-operand times, and pushes the result.
*/
void op_bwshr(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* Make sure we're multiplying two integers. */
    if (d1->type != INTEGER) {
        cthrow(type_id, "Left side (%D) is not an integer.", d1);
    } else if (d2->type != INTEGER) {
        cthrow(type_id, "Right side (%D) is not an integer.", d2);
    } else if (d2->u.val == 0) {
        cthrow(div_id, "Attempt to divide %D by zero.", d1);
    } else {
        /* Replace d1 with d1 / d2, and pop d2. */
        d1->u.val >>= d2->u.val;
        pop(1);
    }
}

/*
// Effects: If the top two values on the stack are integers 
//          pops them, shifts the left operand to the left
//          right-operand times, and pushes  the result.
*/
void op_bwshl(void) {
    data_t *d1 = &stack[stack_pos - 2];
    data_t *d2 = &stack[stack_pos - 1];

    /* Make sure we're multiplying two integers. */
    if (d1->type != INTEGER) {
        cthrow(type_id, "Left side (%D) is not an integer.", d1);
    } else if (d2->type != INTEGER) {
        cthrow(type_id, "Right side (%D) is not an integer.", d2);
    } else if (d2->u.val == 0) {
        cthrow(div_id, "Attempt to divide %D by zero.", d1);
    } else {
        /* Replace d1 with d1 / d2, and pop d2. */
        d1->u.val <<= d2->u.val;
        pop(1);
    }
}

