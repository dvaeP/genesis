/*
// ColdMUD was created and is copyright 1993, 1994 by Greg Hudson
//
// Genesis is a derivitive work, and is copyright 1995 by Brandon Gillespie.
// Full details and copyright information can be found in the file doc/CREDITS
//
// File: execute.c
// ---
// Routines for executing ColdC tasks.
*/

#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include "config.h"
#include "defs.h"
#include "y.tab.h"
#include "execute.h"
#include "memory.h"
#include "cdc_types.h"
#include "io.h"
#include "cache.h"
#include "util.h"
#include "opcodes.h"
#include "log.h"
#include "decode.h"

#define STACK_STARTING_SIZE                (256 - STACK_MALLOC_DELTA)
#define ARG_STACK_STARTING_SIZE                (32 - ARG_STACK_MALLOC_DELTA)

extern int running;

internal void execute(void);
internal void out_of_ticks_error(void);
internal void start_error(Ident error, string_t *explanation, data_t *arg,
                        list_t * location);
internal list_t * traceback_add(list_t * traceback, Ident error);
internal void fill_in_method_info(data_t *d);

internal Frame *frame_store = NULL;
internal int frame_depth;
string_t *numargs_str;

Frame *cur_frame, *suspend_frame;
connection_t *cur_conn;
data_t *stack;
int stack_pos, stack_size;
int *arg_starts, arg_pos, arg_size;
long task_id;
long tick;

#define DEBUG_VM 0
#define DEBUG_EXECUTE 0

VMState *tasks = NULL, *paused = NULL, *vmstore = NULL;
VMStack *stack_store = NULL, *holder_store = NULL; 

/*
// ---------------------------------------------------------------
//
// These two defines add and remove tasks from task lists.
//
*/
#define ADD_TASK(the_list, the_value) { \
        if (!the_list) { \
            the_list = the_value; \
            the_value->next = NULL; \
        } else { \
            the_value->next = the_list; \
            the_list = the_value; \
        } \
    }

#define REMOVE_TASK(the_list, the_value) { \
        if (the_list == the_value) { \
            the_list = the_list->next; \
        } else { \
            task_delete(the_list, the_value); \
        } \
    }

/*
// ---------------------------------------------------------------
*/
void store_stack(void) {
    VMStack * holder;

    if (holder_store) {
        holder = holder_store;
        holder_store = holder_store->next;
#if DEBUG_VM
        write_err("store_stack:  reusing holder %d", holder);
#endif
    } else {
        holder = EMALLOC(VMStack, 1);
#if DEBUG_VM
        write_err("store_stack:  allocating holder %d", holder);
#endif
    }
  
    holder->stack = stack;
    holder->stack_size = stack_size;

    holder->arg_starts = arg_starts;
    holder->arg_size = arg_size;

    holder->next = stack_store;
    stack_store = holder;
}

/*
// ---------------------------------------------------------------
*/
VMState *suspend_vm(void) {
    VMState * vm;

    if (vmstore) {
        vm = vmstore;
        vmstore = vmstore->next;
#if DEBUG_VM
        write_err("suspend_vm:  reusing vm %d", vm);
#endif
  } else {
        vm = EMALLOC(VMState, 1);
#if DEBUG_VM
        write_err("suspend_vm:  allocating vm %d", vm);
#endif
  }

    vm->paused = 0;
    vm->cur_frame = cur_frame;
    vm->cur_conn = cur_conn;
    vm->stack = stack;
    vm->stack_pos = stack_pos;
    vm->stack_size = stack_size;
    vm->arg_starts = arg_starts;
    vm->arg_pos = arg_pos;
    vm->arg_size = arg_size;
    vm->task_id = task_id;
    vm->next = NULL;

    return vm;
}

/*
// ---------------------------------------------------------------
*/
void restore_vm(VMState *vm) {
    task_id = vm->task_id;
    cur_frame = vm->cur_frame;
    cur_conn = vm->cur_conn;
    stack = vm->stack;
    stack_pos = vm->stack_pos;
    stack_size = vm->stack_size;
    arg_starts = vm->arg_starts;
    arg_pos = vm->arg_pos;
    arg_size = vm->arg_size;
#if DEBUG_VM
    write_err("restore_vm: tid %d opcode %s",
              vm->task_id, op_table[cur_frame->opcodes[cur_frame->pc]].name);
#endif
}


/*
// ---------------------------------------------------------------
*/
void task_delete(VMState *list, VMState *elem) {
    if (list != tasks)
        list = list->next;
    while (list && (list->next != elem))
        list = list->next;
    if (list)
        list->next = elem->next;
}

/*
// ---------------------------------------------------------------
*/
VMState *task_lookup(long tid) {
    VMState * vm;

    for (vm = tasks;  vm;  vm = vm->next)
        if (vm->task_id == tid)
            return vm;

    for (vm = paused;  vm;  vm = vm->next)
        if (vm->task_id == tid)
            return vm;

    return NULL;
}

/*
// ---------------------------------------------------------------
// we assume tid is a non-paused task
//
*/
void task_resume(long tid, data_t *ret) {
    VMState * vm = task_lookup(tid),
            * old_vm;

    old_vm = suspend_vm();
    restore_vm(vm);
    REMOVE_TASK(tasks, vm);
    ADD_TASK(vmstore, vm);
    if (ret) {
        check_stack(1);
        data_dup(&stack[stack_pos], ret);
        stack_pos++;
    } else {
        push_int(0);
    }
    execute();
    store_stack();
    restore_vm(old_vm);
    ADD_TASK(vmstore, old_vm);
}

/*
// ---------------------------------------------------------------
*/
void task_suspend(void) {
    VMState * vm = suspend_vm();

    ADD_TASK(tasks, vm);
    init_execute();
    cur_frame = NULL;
}

/*
// ---------------------------------------------------------------
*/
void task_cancel(long tid) {
    VMState * vm = task_lookup(tid),
            * old_vm;

    old_vm = suspend_vm();
    restore_vm(vm);
    while (cur_frame)
        frame_return();
    if (vm->paused) {
        REMOVE_TASK(paused, vm);
    } else {
        REMOVE_TASK(tasks, vm);
    }
    store_stack();
    ADD_TASK(vmstore, vm);
    restore_vm(old_vm);
    ADD_TASK(vmstore, old_vm);
}

/*
// ---------------------------------------------------------------
*/
void task_pause(void) {
    VMState * vm = suspend_vm();

    vm->paused = 1;
    ADD_TASK(paused, vm);
    init_execute();
    cur_frame = NULL;  
}

/*
// ---------------------------------------------------------------
*/
void run_paused_tasks(void) {
    VMState * vm = suspend_vm(),
            * task = paused,
            * last_task;

    /* don't want any task adding itself while we're running the paused tasks */
    paused = NULL;
    while (task) {
        restore_vm(task);
        cur_frame->ticks = PAUSED_METHOD_TICKS;
        last_task = task;
        task = task->next;
        ADD_TASK(vmstore, last_task);
        execute();
        store_stack();
    }
    restore_vm(vm);
    ADD_TASK(vmstore, vm);

#if DEBUG_VM
    for (vm = paused; vm; vm = vm->next)
        write_err("paused task tid %d", vm->task_id);
#endif
}

/*
// ---------------------------------------------------------------
*/
list_t * task_list(void) {
    list_t  * r;
    data_t      elem;
    VMState * vm = tasks;
  
    r = list_new(0);
  
    elem.type = INTEGER;
    for (; vm; vm = vm->next) {
        elem.u.val = vm->task_id;
        list_add(r, &elem); 
    }
    
    vm = paused;
    for (; vm; vm = vm->next) {
        elem.u.val = vm->task_id;
        list_add(r, &elem); 
    }
  
    return r;
}

/*
// ---------------------------------------------------------------
*/
list_t *task_callers(void) {
    list_t * r;
    data_t   elem,
           * d;
    Frame  * f;
  
    r = list_new(0);
    elem.type = LIST;
    for (f = cur_frame; f; f = f->caller_frame) {
        elem.u.list = list_new(4);
        d = list_empty_spaces(elem.u.list, 4);
        d->type = DBREF;
        d->u.dbref = f->object->dbref;
        d++;
        d->type = DBREF;
        d->u.dbref = f->method->object->dbref;
        d++;
        d->type = SYMBOL;
        d->u.symbol = f->method->name;
        d++;
        d->type = INTEGER;
        d->u.val = line_number(f->method, f->pc - 1);
        list_add(r, &elem);
    }
    return r;
}

/*
// ---------------------------------------------------------------
*/
void init_execute(void) {
    if (stack_store) {
        VMStack *holder;
        
        stack = stack_store->stack;
        stack_size = stack_store->stack_size;
    
        arg_starts = stack_store->arg_starts;
        arg_size = stack_store->arg_size;
    
        holder = stack_store;
        stack_store = holder->next;
        holder->next = holder_store;
        holder_store = holder;

#if DEBUG_VM
        write_err("resuing execution state");
#endif

    } else {
        stack = EMALLOC(data_t, STACK_STARTING_SIZE);
        stack_size = STACK_STARTING_SIZE;
    
        arg_starts = EMALLOC(int, ARG_STACK_STARTING_SIZE);
        arg_size = ARG_STACK_STARTING_SIZE;

#if DEBUG_VM
        write_err("allocating execution state");
#endif

    }
    stack_pos = 0;
    arg_pos = 0;
}

/*
// ---------------------------------------------------------------
//
// Execute a task by sending a message to an object.
//
*/
void task(connection_t *conn, Dbref dbref, long message, int num_args, ...) {
    va_list arg;

    /* Don't execute if a shutdown() has occured. */
    if (!running) {
        va_end(arg);
        return;
    }

    /* Set global variables. */
    cur_conn = conn;
    frame_depth = 0;

    va_start(arg, num_args);
    check_stack(num_args);
    while (num_args--)
        data_dup(&stack[stack_pos++], va_arg(arg, data_t *));
    va_end(arg);

    /* Send the message.  If this is succesful, start the task by calling
     * execute(). */
    ident_dup(message);
    if (send_message(dbref, message, 0, 0) == NOT_AN_IDENT) {
        execute();
        if (stack_pos != 0)
            panic("Stack not empty after interpretation.");
        task_id++;
    } else {
        pop(stack_pos);
    }
    ident_discard(message);
}

/*
// ---------------------------------------------------------------
//
// Execute a task by evaluating a method on an object.
//
*/
void task_method(connection_t *conn, object_t *obj, method_t *method) {
    cur_conn = conn;
    frame_start(obj, method, NOT_AN_IDENT, NOT_AN_IDENT, 0, 0);
    execute();

    if (stack_pos != 0)
        panic("Stack not empty after interpretation.");
}

/*
// ---------------------------------------------------------------
*/
long frame_start(object_t * obj,
                 method_t * method,
                 Dbref    sender,
                 Dbref    caller,
                 int      stack_start,
                 int      arg_start)
{
    Frame      * frame;
    int          i,
                 num_args,
                 num_rest_args;
    list_t     * rest;
    data_t       * d;
    Number_buf   nbuf1,
                 nbuf2;

    num_args = stack_pos - arg_start;
    if (num_args < method->num_args || (num_args > method->num_args &&
                                        method->rest == -1)) {
        if (numargs_str)
            string_discard(numargs_str);
        numargs_str = format("#%l.%s called with %s argument%s, requires %s%s",
                             obj->dbref, ident_name(method->name),
                             english_integer(num_args, nbuf1),
                             (num_args == 1) ? "" : "s",
                             (method->num_args == 0) ? "none" :
                             english_integer(method->num_args, nbuf2),
                             (method->rest == -1) ? "." : " or more.");
        return numargs_id;
    }

    if (frame_depth > MAX_CALL_DEPTH)
        return maxdepth_id;
    frame_depth++;

    if (method->rest != -1) {
        /* Make a list for the remaining arguments. */
        num_rest_args = stack_pos - (arg_start + method->num_args);
        rest = list_new(num_rest_args);

        /* Move aforementioned remaining arguments into the list. */
        d = list_empty_spaces(rest, num_rest_args);
        MEMCPY(d, &stack[stack_pos - num_rest_args], num_rest_args);
        stack_pos -= num_rest_args;

        /* Push the list onto the stack. */
        push_list(rest);
        list_discard(rest);
    }

    if (frame_store) {
        frame = frame_store;
        frame_store = frame_store->caller_frame;
    } else {
        frame = EMALLOC(Frame, 1);
    }

    frame->object = cache_grab(obj);
    frame->sender = sender;
    frame->caller = caller;
    frame->method = method_dup(method);
    cache_grab(method->object);
    frame->opcodes = method->opcodes;
    frame->pc = 0;
    frame->ticks = METHOD_TICKS;

    frame->specifiers = NULL;
    frame->handler_info = NULL;

    /* Set up stack indices. */
    frame->stack_start = stack_start;
    frame->var_start = arg_start;

    /* Initialize local variables to 0. */
    check_stack(method->num_vars);
    for (i = 0; i < method->num_vars; i++) {
        stack[stack_pos + i].type = INTEGER;
        stack[stack_pos + i].u.val = 0;
    }
    stack_pos += method->num_vars;

    frame->caller_frame = cur_frame;
    cur_frame = frame;

    return NOT_AN_IDENT;
}

/*
// ---------------------------------------------------------------
*/
void frame_return(void) {
    int i;
    Frame *caller_frame = cur_frame->caller_frame;

    /* Free old data on stack. */
    for (i = cur_frame->stack_start; i < stack_pos; i++)
        data_discard(&stack[i]);
    stack_pos = cur_frame->stack_start;

    /* Let go of method and objects. */
    method_discard(cur_frame->method);
    cache_discard(cur_frame->object);
    cache_discard(cur_frame->method->object);

    /* Discard any error action specifiers. */
    while (cur_frame->specifiers)
        pop_error_action_specifier();

    /* Discard any handler information. */
    while (cur_frame->handler_info)
        pop_handler_info();

    /* Append frame to frame store for later reuse. */
    cur_frame->caller_frame = frame_store;
    frame_store = cur_frame;

    /* Return to the caller frame. */
    cur_frame = caller_frame;

    frame_depth--;
}

/*
// ---------------------------------------------------------------
*/
internal void execute(void) {
    int opcode;

    while (cur_frame) {
        tick++;
        if ((--(cur_frame->ticks)) == 0) {
            out_of_ticks_error();
        } else {
            opcode = cur_frame->opcodes[cur_frame->pc];

#if DEBUG_EXECUTE
            write_err("#%d #%d.%I %d %s",
                cur_frame->object->dbref, 
                cur_frame->method->object->dbref,
                ((cur_frame->method->name != NOT_AN_IDENT) ?
                    cur_frame->method->name :
                    opcode_id),
                line_number(cur_frame->method, cur_frame->pc),
                op_table[cur_frame->opcodes[cur_frame->pc]].name);
#endif

            cur_frame->last_opcode = opcode;
            cur_frame->pc++;
            (*op_table[opcode].func)();
        }
    }
}

/*
// ---------------------------------------------------------------
//
// Requires cur_frame->pc to be the current instruction.  Do NOT call this
// function if there is any possibility of the assignment failing before the
// current instruction finishes.
//
*/
void anticipate_assignment(void) {
    int opcode, ind;
    long id;
    data_t *dp, d;

    opcode = cur_frame->opcodes[cur_frame->pc];
    if (opcode == SET_LOCAL) {
        /* Zero out local variable value. */
        dp = &stack[cur_frame->var_start +
                    cur_frame->opcodes[cur_frame->pc + 1]];
        data_discard(dp);
        dp->type = INTEGER;
        dp->u.val = 0;
    } else if (opcode == SET_OBJ_VAR) {
        /* Zero out the object variable, if it exists. */
        ind = cur_frame->opcodes[cur_frame->pc + 1];
        id = object_get_ident(cur_frame->method->object, ind);
        d.type = INTEGER;
        d.u.val = 0;
        object_assign_var(cur_frame->object, cur_frame->method->object,
                          id, &d);
    }
}

/*
// ---------------------------------------------------------------
*/
Ident pass_message(int stack_start, int arg_start) {
    method_t *method;
    Ident result;

    if (cur_frame->method->name == -1)
        return methodnf_id;

    /* Find the next method to handle the message. */
    method = object_find_next_method(cur_frame->object->dbref,
                                     cur_frame->method->name,
                                     cur_frame->method->object->dbref);
    if (!method)
        return methodnf_id;

    /* Start the new frame. */
    result = frame_start(cur_frame->object, method, cur_frame->sender,
                         cur_frame->caller, stack_start, arg_start);
    cache_discard(method->object);
    return result;
}

/*
// ---------------------------------------------------------------
*/
Ident send_message(Dbref dbref, Ident message, int stack_start, int arg_start) {
    object_t *obj;
    method_t *method;
    Ident result;
    Dbref sender, caller;

    /* Get the target object from the cache. */
    obj = cache_retrieve(dbref);
    if (!obj)
        return objnf_id;

    /* Find the method to run. */
    method = object_find_method(obj->dbref, message);
    if (!method) {
        cache_discard(obj);
        return methodnf_id;
    }

    /*
    // check perms
    //     private:   caller has to be definer
    //     protected: sender has to be this
    //     root:      caller has to be $root
    //     driver:    only I can send to this method
    */
    if (cur_frame) {
        switch (method->m_state) {
            case MS_PRIVATE:
                if (cur_frame->method->object->dbref != method->object->dbref)
                    return private_id;
                break;
            case MS_PROTECTED:
                if (cur_frame->object->dbref != obj->dbref)
                    return protected_id;
                break;
            case MS_ROOT:
                if (cur_frame->method->object->dbref != ROOT_DBREF)
                    return root_id;
                break;
            case MS_DRIVER:
                /* if we are here, there is a current frame,
                   and the driver didn't send this */
                return driver_id;
        }
    }

    /* Start the new frame. */
    sender = (cur_frame) ? cur_frame->object->dbref : NOT_AN_IDENT;
    caller = (cur_frame) ? cur_frame->method->object->dbref : NOT_AN_IDENT;
    result = frame_start(obj, method, sender, caller, stack_start, arg_start);

    cache_discard(obj);
    cache_discard(method->object);

    return result;
}

/*
// ---------------------------------------------------------------
*/
void pop(int n) {

#ifdef DEBUG
    write_err("pop(%d)", n);
#endif

    while (n--)
        data_discard(&stack[--stack_pos]);
}

/*
// ---------------------------------------------------------------
*/
void check_stack(int n) {
    while (stack_pos + n > stack_size) {
        stack_size = stack_size * 2 + STACK_MALLOC_DELTA;
        stack = EREALLOC(stack, data_t, stack_size);
    }
}

/*
// ---------------------------------------------------------------
*/
void push_int(long n) {

#ifdef DEBUG
    write_err("push(%d)", n);
#endif

    check_stack(1);
    stack[stack_pos].type = INTEGER;
    stack[stack_pos].u.val = n;
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_float(float f) {

#ifdef DEBUG
    write_err("push(%f)", f);
#endif

    check_stack(1);
    stack[stack_pos].type = FLOAT;
    stack[stack_pos].u.fval = f;
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_string(string_t *str) {

#ifdef DEBUG
    write_err("push(\"%S\")", str);
#endif

    check_stack(1);
    stack[stack_pos].type = STRING;
    stack[stack_pos].u.str = string_dup(str);
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_dbref(Dbref dbref) {

#ifdef DEBUG
    write_err("push($%d)", dbref);
#endif

    check_stack(1);
    stack[stack_pos].type = DBREF;
    stack[stack_pos].u.dbref = dbref;
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_list(list_t * list) {
#ifdef DEBUG
    string_t *str = string_new(0);

    write_err("push(%S)", data_add_list_literal_to_str(str, list));

    string_discard(str);
#endif

    check_stack(1);
    stack[stack_pos].type = LIST;
    stack[stack_pos].u.list = list_dup(list);
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_dict(Dict *dict) {
#ifdef DEBUG
    string_t *str = string_new(0);

    write_err("push(%S)", dict_add_literal_to_str(str, dict));

    string_discard(str);
#endif

    check_stack(1);
    stack[stack_pos].type = DICT;
    stack[stack_pos].u.dict = dict_dup(dict);
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_symbol(Ident id) {

#ifdef DEBUG
    write_err("push(\'%s)", ident_name(id));
#endif

    check_stack(1);
    stack[stack_pos].type = SYMBOL;
    stack[stack_pos].u.symbol = ident_dup(id);
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_error(Ident id) {

#ifdef DEBUG
    write_err("push(\'%s)", ident_name(id));
#endif

    check_stack(1);
    stack[stack_pos].type = ERROR;
    stack[stack_pos].u.error = ident_dup(id);
    stack_pos++;
}

/*
// ---------------------------------------------------------------
*/
void push_buffer(Buffer *buf) {

#ifdef DEBUG
    write_err("push() buffer");
#endif

    check_stack(1);
    stack[stack_pos].type = BUFFER;
    stack[stack_pos].u.buffer = buffer_dup(buf);
    stack_pos++;
}

int func_init_0(void)
{
    int arg_start = arg_starts[--arg_pos];
    int num_args = stack_pos - arg_start;

    if (num_args)
        func_num_error(num_args, "none");
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref)
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
    else
        return 1;
    return 0;
}

int func_init_1(data_t **args, int type1)
{
    int arg_start = arg_starts[--arg_pos];
    int num_args = stack_pos - arg_start;

    *args = &stack[arg_start];
    if (num_args != 1)
        func_num_error(num_args, "one");
    else if (type1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
       /* pop(1); */
    } else
        return 1;
    return 0;
}

int func_init_2(data_t **args, int type1, int type2)
{
    int arg_start = arg_starts[--arg_pos];
    int num_args = stack_pos - arg_start;

    *args = &stack[arg_start];
    if (num_args != 2)
        func_num_error(num_args, "two");
    else if (type1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (type2 && stack[arg_start + 1].type != type2)
        func_type_error("second", &stack[arg_start + 1], english_type(type2));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
       /* pop(2); */
    } else
        return 1;
    return 0;
}

int func_init_3(data_t **args, int type1, int type2, int type3)
{
    int arg_start = arg_starts[--arg_pos];
    int num_args = stack_pos - arg_start;

    *args = &stack[arg_start];
    if (num_args != 3)
        func_num_error(num_args, "three");
    else if (type1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (type2 && stack[arg_start + 1].type != type2)
        func_type_error("second", &stack[arg_start + 1], english_type(type2));
    else if (type3 && stack[arg_start + 2].type != type3)
        func_type_error("third", &stack[arg_start + 2], english_type(type3));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
        /* pop(3); */
    } else
        return 1;
    return 0;
}

int func_init_0_or_1(data_t **args, int *num_args, int type1)
{
    int arg_start = arg_starts[--arg_pos];

    *args = &stack[arg_start];
    *num_args = stack_pos - arg_start;
    if (*num_args > 1)
        func_num_error(*num_args, "at most one");
    else if (type1 && *num_args == 1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
        /* pop(*num_args); */
    } else
        return 1;
    return 0;
}

int func_init_1_or_2(data_t **args, int *num_args, int type1, int type2)
{
    int arg_start = arg_starts[--arg_pos];

    *args = &stack[arg_start];
    *num_args = stack_pos - arg_start;
    if (*num_args < 1 || *num_args > 2)
        func_num_error(*num_args, "one or two");
    else if (type1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (type2 && *num_args == 2 && stack[arg_start + 1].type != type2)
        func_type_error("second", &stack[arg_start + 1], english_type(type2));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
        /* pop(*num_args); */
    } else
        return 1;
    return 0;
}

int func_init_2_or_3(data_t **args, int *num_args, int type1, int type2,
                     int type3)
{
    int arg_start = arg_starts[--arg_pos];

    *args = &stack[arg_start];
    *num_args = stack_pos - arg_start;
    if (*num_args < 2 || *num_args > 3)
        func_num_error(*num_args, "two or three");
    else if (type1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (type2 && stack[arg_start + 1].type != type2)
        func_type_error("second", &stack[arg_start + 1], english_type(type2));
    else if (type3 && *num_args == 3 && stack[arg_start + 2].type != type3)
        func_type_error("third", &stack[arg_start + 2], english_type(type3));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
        /* pop(*num_args); */
    } else
        return 1;
    return 0;
}

int func_init_1_to_3(data_t **args, int *num_args, int type1, int type2,
                     int type3)
{
    int arg_start = arg_starts[--arg_pos];

    *args = &stack[arg_start];
    *num_args = stack_pos - arg_start;
    if (*num_args < 1 || *num_args > 3)
        func_num_error(*num_args, "one to three");
    else if (type1 && stack[arg_start].type != type1)
        func_type_error("first", &stack[arg_start], english_type(type1));
    else if (type2 && *num_args >= 2 && stack[arg_start + 1].type != type2)
        func_type_error("second", &stack[arg_start + 1], english_type(type2));
    else if (type3 && *num_args == 3 && stack[arg_start + 2].type != type3)
        func_type_error("third", &stack[arg_start + 2], english_type(type3));
    else if (op_table[cur_frame->last_opcode].binding != INV_OBJNUM &&
             op_table[cur_frame->last_opcode].binding
             != cur_frame->method->object->dbref) {
        cthrow(perm_id,
               "Caller (#%l) is not the object bound to this function.",
               cur_frame->method->object->dbref);
        /* pop(*num_args); */
    } else
        return 1;
    return 0;
}

void func_num_error(int num_args, char *required)
{
    Number_buf nbuf;

    cthrow(numargs_id, "Called with %s argument%s, requires %s.",
          english_integer(num_args, nbuf),
          (num_args == 1) ? "" : "s", required);
}

void func_type_error(char *which, data_t *wrong, char *required)
{
    cthrow(type_id, "The %s argument (%D) is not %s.", which, wrong, required);
}

void cthrow(Ident error, char *fmt, ...)
{
    string_t *str;
    va_list arg;

    va_start(arg, fmt);
    str = vformat(fmt, arg);

    va_end(arg);
    interp_error(error, str);
    string_discard(str);
}

void interp_error(Ident error, string_t *explanation)
{
    list_t * location;
    Ident location_type;
    data_t *d;
    char *opname;

    /* Get the opcode name and decide whether it's a function or not. */
    opname = op_table[cur_frame->last_opcode].name;
    location_type = (islower(*opname)) ? function_id : opcode_id;

    /* Construct a two-element list giving the location. */
    location = list_new(2);
    d = list_empty_spaces(location, 2);

    /* The first element is 'function or 'opcode. */
    d->type = SYMBOL;
    d->u.symbol = ident_dup(location_type);
    d++;

    /* The second element is the symbol for the opcode. */
    d->type = SYMBOL;
    d->u.symbol = ident_dup(op_table[cur_frame->last_opcode].symbol);

    start_error(error, explanation, NULL, location);
    list_discard(location);
}

void user_error(Ident error, string_t *explanation, data_t *arg)
{
    list_t * location;
    data_t *d;

    /* Construct a list giving the location. */
    location = list_new(5);
    d = list_empty_spaces(location, 5);

    /* The first element is 'method. */
    d->type = SYMBOL;
    d->u.symbol = ident_dup(method_id);
    d++;

    /* The second through fifth elements are the current method info. */
    fill_in_method_info(d);

    /* Return from the current method, and propagate the error. */
    frame_return();
    start_error(error, explanation, arg, location);
    list_discard(location);
}

internal void out_of_ticks_error(void)
{
    internal string_t *explanation;
    list_t * location;
    data_t *d;

    /* Construct a list giving the location. */
    location = list_new(5);
    d = list_empty_spaces(location, 5);

    /* The first element is 'interpreter. */
    d->type = SYMBOL;
    d->u.symbol = ident_dup(interpreter_id);
    d++;

    /* The second through fifth elements are the current method info. */
    fill_in_method_info(d);

    /* Don't give the topmost frame a chance to return. */
    frame_return();

    if (!explanation)
        explanation = string_from_chars("Out of ticks", 12);
    start_error(methoderr_id, explanation, NULL, location);
    list_discard(location);
}

internal void start_error(Ident error, string_t *explanation, data_t *arg,
                        list_t * location)
{
    list_t * error_condition, *traceback;
    data_t *d;

    /* Construct a three-element list for the error condition. */
    error_condition = list_new(3);
    d = list_empty_spaces(error_condition, 3);

    /* The first element is the error code. */
    d->type = ERROR;
    d->u.error = ident_dup(error);
    d++;

    /* The second element is the explanation string. */
    d->type = STRING;
    d->u.str = string_dup(explanation);
    d++;

    /* The third element is the error arg, or 0 if there is none. */
    if (arg) {
        data_dup(d, arg);
    } else {
        d->type = INTEGER;
        d->u.val = 0;
    }

    /* Now construct a traceback, starting as a two-element list. */
    traceback = list_new(2);
    d = list_empty_spaces(traceback, 2);

    /* The first element is the error condition. */
    d->type = LIST;
    d->u.list = error_condition;
    d++;

    /* The second argument is the location. */
    d->type = LIST;
    d->u.list = list_dup(location);

    /* Start the error propagating.  This consumes traceback. */
    propagate_error(traceback, error);
}

/* Requires:        traceback is a list of strings containing the traceback
 *                        information to date.  THIS FUNCTION CONSUMES TRACEBACK.
 *                id is an error id.  This function accounts for an error id
 *                        which is "owned" by a data stack frame that we will
 *                        nuke in the course of unwinding the call stack.
 *                str is a string containing an explanation of the error. */
void propagate_error(list_t * traceback, Ident error)
{
    int i, ind, propagate = 0;
    Error_action_specifier *spec;
    Error_list *errors;
    Handler_info *hinfo;

    /* If there's no current frame, drop all this on the floor. */
    if (!cur_frame) {
        list_discard(traceback);
        return;
    }

    /* Add message to traceback. */
    traceback = traceback_add(traceback, error);

    /* Look for an appropriate specifier in this frame. */
    for (; cur_frame->specifiers; pop_error_action_specifier()) {

        spec = cur_frame->specifiers;
        switch (spec->type) {

          case CRITICAL:

            /* We're in a critical expression.  Make a copy of the error,
             * since it may currently be living in the region of the stack
             * we're about to nuke. */
            error = ident_dup(error);

            /* Nuke the stack back to where we were at the beginning of the
             * critical expression. */
            pop(stack_pos - spec->stack_pos);

            /* Jump to the end of the critical expression. */
            cur_frame->pc = spec->u.critical.end;

            /* Push the error on the stack, and discard our copy of it. */
            push_error(error);
            ident_discard(error);

            /* Pop this error spec, discard the traceback, and continue
             * processing. */
            pop_error_action_specifier();
            list_discard(traceback);
            return;

          case PROPAGATE:

            /* We're in a propagate expression.  Set the propagate flag and
             * keep going. */
            propagate = 1;
            break;

          case CATCH:

            /* We're in a catch statement.  Get the error list index. */
            ind = spec->u.ccatch.error_list;

            /* If the index is -1, this was a 'catch any' statement.
             * Otherwise, check if this error code is in the error list. */
            if (spec->u.ccatch.error_list != -1) {
                errors = &cur_frame->method->error_lists[ind];
                for (i = 0; i < errors->num_errors; i++) {
                    if (errors->error_ids[i] == error)
                        break;
                }

                /* Keep going if we didn't find the error. */
                if (i == errors->num_errors)
                    break;
            }

            /* We catch this error.  Make a handler info structure and push it
             * onto the stack. */
            hinfo = EMALLOC(Handler_info, 1);
            hinfo->traceback = traceback;
            hinfo->error = ident_dup(error);
            hinfo->next = cur_frame->handler_info;
            cur_frame->handler_info = hinfo;

            /* Pop the stack down to where we were at the beginning of the
             * catch statement.  This may nuke our copy of error, but we don't
             * need it any more. */
            pop(stack_pos - spec->stack_pos);

            /* Jump to the handler expression, pop this specifier, and continue
             * processing. */
            cur_frame->pc = spec->u.ccatch.handler;
            pop_error_action_specifier();
            return;

        }
    }

    /* There was no handler in the current frame. */
    frame_return();
    propagate_error(traceback, (propagate) ? error : methoderr_id);
}

internal list_t * traceback_add(list_t * traceback, Ident error)
{
    list_t * frame;
    data_t *d, frame_data;

    /* Construct a list giving information about this stack frame. */
    frame = list_new(5);
    d = list_empty_spaces(frame, 5);

    /* First element is the error code. */
    d->type = ERROR;
    d->u.error = ident_dup(error);
    d++;

    /* Second through fifth elements are the current method info. */
    fill_in_method_info(d);

    /* Add the frame to the list. */
    frame_data.type = LIST;
    frame_data.u.list = frame;
    traceback = list_add(traceback, &frame_data);
    list_discard(frame);

    return traceback;
}

void pop_error_action_specifier(void)
{ 
    Error_action_specifier *old;

    /* Pop the first error action specifier off that stack. */
    old = cur_frame->specifiers;
    cur_frame->specifiers = old->next;
    efree(old);
}

void pop_handler_info(void)
{
    Handler_info *old;

    /* Free the data in the first handler info specifier, and pop it off that
     * stack. */
    old = cur_frame->handler_info;
    list_discard(old->traceback);
    ident_discard(old->error);
    cur_frame->handler_info = old->next;
    efree(old);
}

internal void fill_in_method_info(data_t *d)
{
    Ident method_name;

    /* The method name, or 0 for eval. */
    method_name = cur_frame->method->name;
    if (method_name == NOT_AN_IDENT) {
        d->type = INTEGER;
        d->u.val = 0;
    } else {
        d->type = SYMBOL;
        d->u.val = ident_dup(method_name);
    }
    d++;

    /* The current object. */
    d->type = DBREF;
    d->u.dbref = cur_frame->object->dbref;
    d++;

    /* The defining object. */
    d->type = DBREF;
    d->u.dbref = cur_frame->method->object->dbref;
    d++;

    /* The line number. */
    d->type = INTEGER;
    d->u.val = line_number(cur_frame->method, cur_frame->pc);
}

void bind_opcode(int opcode, Dbref dbref) {
    op_table[opcode].binding = dbref;
}
