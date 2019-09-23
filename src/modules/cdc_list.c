/*
// Full copyright information is available in the file ../doc/CREDITS
*/

#define NATIVE_MODULE "$list"

#include "cdc.h"

NATIVE_METHOD(listlen) {
    Int len;

    INIT_1_ARG(LIST);

    len = list_length(LIST1);

    CLEAN_RETURN_INTEGER(len);
}

NATIVE_METHOD(sublist) {
    Int      start,
             span,
             len;
    cList * list;

    INIT_2_OR_3_ARGS(LIST, INTEGER, INTEGER);

    len = list_length(LIST1);
    start = INT2 - 1;
    span = (argc == 3) ? INT3 : len - start;

    /* Make sure range is in bounds. */
    if (start < 0)
        THROW((range_id, "Start (%d) less than one", start + 1));
    else if (span < 0)
        THROW((range_id, "Sublist length (%d) less than zero", span));
    else if (start + span > len)
        THROW((range_id, "Sublist extends to %d, past end of list (length %d)",
              start + span, len));

    list = list_dup(LIST1);

    CLEAN_STACK();
    anticipate_assignment();

    list = list_sublist(list, start, span);

    RETURN_LIST(list);
}

NATIVE_METHOD(insert) {
    Int      pos,
             len;
    cList * list;
    cData   data;
    DEF_args;

    INIT_ARGC(ARG_COUNT, 3, "three");
    INIT_ARG1(LIST);
    INIT_ARG2(INTEGER);

    pos = INT2 - 1;
    len = list_length(LIST1);

    if (pos < 0)
        THROW((range_id, "Position (%d) less than one", pos + 1));
    else if (pos > len)
        THROW((range_id, "Position (%d) beyond end of list (length %d)",
              pos + 1, len));

    data_dup(&data, &args[2]);
    list = list_dup(LIST1);

    CLEAN_STACK();
    anticipate_assignment();

    list = list_insert(list, pos, &data);
    data_discard(&data);

    RETURN_LIST(list);
}

NATIVE_METHOD(replace) {
    Int      pos,
             len;
    cList * list;
    cData   data;
    DEF_args;

    INIT_ARGC(ARG_COUNT, 3, "three");
    INIT_ARG1(LIST);
    INIT_ARG2(INTEGER);

    len = list_length(LIST1);
    pos = INT2 - 1;

    if (pos < 0)
        THROW((range_id, "Position (%d) less than one", pos + 1));
    else if (pos > len - 1)
        THROW((range_id, "Position (%d) greater than length of list (%d)",
              pos + 1, len));

    data_dup(&data, &args[2]);
    list = list_dup(LIST1);
    CLEAN_STACK();
    anticipate_assignment();

    list = list_replace(list, pos, &data);
    data_discard(&data);

    RETURN_LIST(list);
}

NATIVE_METHOD(delete) {
    Int      pos,
             len;
    cList * list;

    INIT_2_ARGS(LIST, INTEGER);

    len = list_length(LIST1);
    pos = INT2 - 1;

    if (pos < 0)
        THROW((range_id, "Position (%d) less than one", pos + 1));
    else if (pos > len - 1)
        THROW((range_id, "Position (%d) greater than length of list (%d)",
              pos + 1, len));

    list = list_dup(LIST1);

    CLEAN_STACK();
    anticipate_assignment();

    RETURN_LIST(list_delete(list, pos));
}

NATIVE_METHOD(setadd) {
    cList * list;
    cData   data;
    DEF_args;

    INIT_ARGC(ARG_COUNT, 2, "two");
    INIT_ARG1(LIST);

    data_dup(&data, &args[1]);
    list = list_dup(LIST1);

    CLEAN_STACK();
    anticipate_assignment();

    list = list_setadd(list, &data);
    data_discard(&data);

    RETURN_LIST(list);
}

NATIVE_METHOD(setremove) {
    cList * list;
    cData   data;
    DEF_args;

    INIT_ARGC(ARG_COUNT, 2, "two");
    INIT_ARG1(LIST);

    data_dup(&data, &args[1]);
    list = list_dup(LIST1);

    CLEAN_STACK();
    anticipate_assignment();

    list = list_setremove(list, &data);
    data_discard(&data);

    RETURN_LIST(list);
}

NATIVE_METHOD(union) {
    cList * list, * list2;

    INIT_2_ARGS(LIST, LIST);

    list = list_dup(LIST1);
    list2 = list_dup(LIST2);

    CLEAN_STACK();
    anticipate_assignment();

    list = list_union(list, list2);

    list_discard(list2);

    RETURN_LIST(list);
}

NATIVE_METHOD(join) {
    bool      discard_sep=false;
    cStr    * str, * sep;

    INIT_1_OR_2_ARGS(LIST, STRING);

    if (!LIST1->len) {
        str = string_new(0);
    } else {
        if (argc == 1) {
            sep = string_from_chars(" ", 1);
            discard_sep=true;
        } else {
            sep = STR2;
        }
        str = list_join(LIST1, sep);
        if (discard_sep)
            string_discard(sep);
    }

    CLEAN_RETURN_STRING(str);
}

static void merge_lists (cData *l, cData *key,
                         Int start1, Int end1,
                         Int start2, Int end2,
                         cData *l_out, cData *key_out)
{
    Int i,j,k;

    i=start1;
    j=start2;
    k=start1;

    while (i<=end1 && j<=end2)
        if (data_cmp(key+j,key+i)>=0)
            key_out[k]=key[i], l_out[k++]=l[i++];
        else
            key_out[k]=key[j], l_out[k++]=l[j++];
    while (i<=end1)
        key_out[k]=key[i], l_out[k++]=l[i++];
    while (j<=end2)
        key_out[k]=key[j], l_out[k++]=l[j++];
    memcpy (l+start1, l_out+start1, sizeof(cData)*(k-start1));
    memcpy (key+start1, key_out+start1, sizeof(cData)*(k-start1));
}

static void merge_sort (cData *l, cData *key,
                        cData *l1, cData *key1,
                        Int start, Int end)
{

    Int mid;

    if (start==end)
        return;

    mid=(start+end)/2;
    merge_sort (l, key, l1, key1, start, mid);
    merge_sort (l, key, l1, key1, mid+1, end);
    merge_lists (l, key, start, mid, mid+1, end, l1, key1);
}

NATIVE_METHOD(sort) {
    cData *d1, *d2, *key1, *key2;
    Int n, i;
    cList *data, *keys;
    cList *out;

    INIT_1_OR_2_ARGS(LIST, LIST);
    data=LIST1;
    if (argc==1)
        keys=data;
    else
        keys=LIST2;

    n=list_length(data);
    if (!(list_length(keys)==n)) {
        THROW((range_id, "Key and data lists are not of the same length"));
    }

    if (!n) {
        out=list_dup(data);
        CLEAN_RETURN_LIST(out);
    }

    d1=emalloc(sizeof(cData)*n);
    d2=emalloc(sizeof(cData)*n);
    key1=emalloc(sizeof(cData)*n);
    key2=emalloc(sizeof(cData)*n);

    for (i=0; i<n; i++) {
        data_dup(d1+i, list_elem(data, i));
        data_dup(key1+i, list_elem(keys, i));
    }
    merge_sort (d1, key1, d2, key2, 0, n-1);

    out=list_new(n);
    out->len=n;
    for (i=0; i<n; i++) {
        *list_elem(out, i)=d1[i]; /* We already did data_dup */
        data_discard(key1+i);
    }

    efree(d1);
    efree(d2);
    efree(key1);
    efree(key2);

    CLEAN_RETURN_LIST(out);
}

static Int validate_dict_args(cList * list, cData * key) {
    cData * elem;

    for (elem = list_first(list); elem; elem = list_next(list, elem)) {
        if ((elem->type != DICT) || !dict_contains(elem->u.dict, key)) {
            THROW((type_id,
                  "Values in list must be dicts and contain the right key."));
        }
    }

    return 1;
}

static Int validate_list_args(cList * list, Int offset) {
    cData * elem;

    for (elem = list_first(list); elem; elem = list_next(list, elem)) {
        if ((elem->type != LIST) || (offset > list_length(elem->u.list))) {
            THROW((type_id,
                   "Values in list must be lists of the right length."));
        }
    }

    return 1;
}

static Int validate_sorted_args(Int stack_start, Int arg_start) {
    Int offset;
    DEF_args;
    DEF_argc;
    CHECK_BINDING
    INIT_ARG1(LIST);
    if (argc == 2) {
        if (args[ARG2].type == LIST) {
            THROW((methoderr_id, "Inserting list data requires an index value"));
        } else if (args[ARG2].type == DICT) {
            THROW((methoderr_id, "Inserting dict data requires a key value"));
        }
    } else if (argc == 3) {
        if ((args[ARG2].type != LIST) && (args[ARG2].type != DICT)) {
            THROW((type_id, "Second arg must be a list or dict."));
        }
    } else if ((argc != 2) && (argc != 3)) {
        THROW_NUM_ERROR(argc, "two or three");
    }

    if (args[ARG2].type == LIST) {
        if (args[ARG3].type != INTEGER) {
            THROW((type_id, "List data requires an integral index value"));
        }

        offset = INT3 - 1;

        if ((offset < 0) || (offset > list_length(LIST1))) {
            THROW((type_id, "Third arg must be an offset into the data."));
        }

        if (!validate_list_args(LIST1, offset))
            return 0;
    } else if (args[ARG2].type == DICT) {
        if (!dict_contains(DICT2, &args[ARG3])) {
            THROW((type_id, "Third arg must be a key into the data."));
        }

        if (!validate_dict_args(LIST1, &args[ARG3]))
            return 0;
    }

    return 1;
}

NATIVE_METHOD(sorted_index) {
    Int out;
    cList * list;
    cData data, key;

    DEF_args;
    DEF_argc;

    if (!validate_sorted_args(stack_start, arg_start))
        return 0;

    list = LIST1;
    data_dup(&data, &args[ARG2]);

    if (argc == 3)
        data_dup(&key, &args[ARG3]);

    /* Do the work! */
    out = list_binary_search(list, &data, &key);

    /* Bring back to 1-based array index */
    if (out != -1)
        out++;

    data_discard(&data);
    if (argc == 3)
        data_discard(&key);

    CLEAN_RETURN_INTEGER(out);
}

NATIVE_METHOD(sorted_insert) {
    cList * list;
    cData   data,
            key;

    DEF_args;
    DEF_argc;

    if (!validate_sorted_args(stack_start, arg_start))
        return 0;

    list = list_dup(LIST1);
    data_dup(&data, &args[ARG2]);

    if (argc == 3)
        data_dup(&key, &args[ARG3]);

    /* Do the work! */
    CLEAN_STACK();
    anticipate_assignment();
    list = list_add_sorted(list, &data, &key);

    data_discard(&data);
    if (argc == 3)
        data_discard(&key);

    RETURN_LIST(list);
}

NATIVE_METHOD(sorted_delete) {
    cList * list, * result;
    cData   data,
            key;

    DEF_args;
    DEF_argc;

    if (!validate_sorted_args(stack_start, arg_start))
        return 0;

    list = list_dup(LIST1);
    data_dup(&data, &args[ARG2]);

    if (argc == 3)
        data_dup(&key, &args[ARG3]);

    result = list_delete_sorted_element(list, &data, &key);

    data_discard(&data);
    if (argc == 3)
        data_discard(&key);

    if (result == NULL) {
        list_discard(list);
        THROW((range_id, "Value must be within the list"));
    }

    CLEAN_RETURN_LIST(result);
}

NATIVE_METHOD(sorted_validate) {
    return 1;
}
