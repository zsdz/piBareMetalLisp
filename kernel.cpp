//
// kernel.cpp
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "kernel.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define null(x) ((x) == NULL || (x) == NIL)
#define EOL(x) (null((x)) || (x) == EMPTY_LIST)
#define error(x)                                                               \
    do {                                                                       \
        fprintf(stderr, "%s\n", x);                                            \
        exit(1);                                                               \
    } while (0)
#define caar(x) (car(car((x))))
#define cdar(x) (cdr(car((x))))
#define cadr(x) (car(cdr((x))))
#define caddr(x) (car(cdr(cdr((x)))))
#define cadddr(x) (car(cdr(cdr(cdr((x))))))
#define cadar(x) (car(cdr(car((x)))))
#define cddr(x) (cdr(cdr((x))))
#define cdadr(x) (cdr(car(cdr((x)))))
#define atom(x) (!null(x) && (x)->type != LIST)
#define ASSERT_TYPE(x, t) (__type_check(__func__, x, t))

typedef enum { INTEGER, SYMBOL, STRING, LIST, PRIMITIVE, VECTOR } type_t;
typedef struct object *(*primitive_t)(struct object *);

/* Lisp object. We want to mimic the homoiconicity of LISP, so we will not be
providing separate "types" for procedures, etc. Everything is represented as
atoms (integers, strings, booleans) or a list of atoms, except for the
primitive functions */

struct object {
    char gc;
    type_t type;
    union {
        int64_t integer;
        char *string;
        struct {
            struct object **vector;
            int vsize;
        };
        struct {
            struct object *car;
            struct object *cdr;
        };
        primitive_t primitive;
    };
} __attribute__((packed));

/* We declare a couple of global variables for keywords */
static struct object *ENV;
static struct object *NIL;
static struct object *EMPTY_LIST;
static struct object *TRUEE;
static struct object *FALSEE;
static struct object *QUOTE;
static struct object *DEFINE;
static struct object *SET;
static struct object *LET;
static struct object *IF;
static struct object *LAMBDA;
static struct object *BEGIN;
static struct object *PROCEDURE;

void print_exp(char *, struct object *);
bool is_tagged(struct object *cell, struct object *tag);
struct object *read_exp(FILE *in);
struct object *eval(struct object *exp, struct object *env);
struct object *cons(struct object *x, struct object *y);
struct object *load_file(struct object *args);
struct object *cdr(struct object *);
struct object *car(struct object *);
struct object *lookup_variable(struct object *var, struct object *env);

/*==============================================================================
Hash table for saving Lisp symbol objects. Conserves memory and faster compares
==============================================================================*/
struct htable {
    struct object *key;
};
/* One dimensional hash table */
static struct htable *HTABLE = NULL;
static int HTABLE_SIZE;

static uint64_t hash(const char *s) {
    uint64_t h = 0;
    uint8_t *u = (uint8_t *)s;
    while (*u) {
        h = (h * 256 + *u) % HTABLE_SIZE;
        u++;
    }
    return h;
}

int ht_init(int size) {
    if (HTABLE || !(size % 2))
        error("Hash table already initialized or even # of entries");
    HTABLE = (struct htable *) malloc(sizeof(struct htable) * size);
    memset(HTABLE, 0, sizeof(struct htable) * size);
    HTABLE_SIZE = size;
    return size;
}

void ht_insert(struct object *key) {
    uint64_t h = hash(key->string);
    HTABLE[h].key = key;
}

struct object *ht_lookup(char *s) {
    uint64_t h = hash(s);
    return HTABLE[h].key;
}

/*==============================================================================
  Memory management - Currently no GC
==============================================================================*/
int alloc_count = 0;

struct object *alloc() {
    struct object *ret = (struct object *) malloc(sizeof(struct object));
    alloc_count++;
    return ret;
}

/*============================================================================
Constructors and etc
==============================================================================*/
int __type_check(const char *func, struct object *obj, type_t type) {
    if (null(obj)) {
        fprintf(stderr, "Invalid argument to function %s: NIL\n", func);
        exit(1);
    } else if (obj->type != type) {
        char *types[6] = {"INTEGER", "SYMBOL",    "STRING",
                          "LIST",    "PRIMITIVE", "VECTOR"};
        fprintf(stderr, "Invalid argument to function %s. Expected %s got %s\n",
                func, types[type], types[obj->type]);
        exit(1);
    }
    return 1;
}

struct object *make_vector(int size) {
    struct object *ret = alloc();
    ret->type = VECTOR;
    ret->vector = (struct object **) malloc(sizeof(struct object *) * size);
    ret->vsize = size;

    memset(ret->vector, 0, size);

    return ret;
}

char* my_strdup(const char* str)
{
    if (str == NULL)
        return NULL;

    char* strat = (char*)str;
    int len = 0;
    while (*str++ != '\0')
        len++;
    char* ret = (char*)malloc(len + 1);

    while ((*ret++ = *strat++) != '\0')
    {}

    return ret - (len + 1);
}

struct object *make_symbol(char *s) {
    struct object *ret = ht_lookup(s);
    if (null(ret)) {
        ret = alloc();
        ret->type = SYMBOL;
        ret->string = my_strdup(s);
        ht_insert(ret);
    }
    return ret;
}

struct object *make_integer(int x) {
    struct object *ret = alloc();
    ret->type = INTEGER;
    ret->integer = x;
    return ret;
}

struct object *make_primitive(primitive_t x) {
    struct object *ret = alloc();
    ret->type = PRIMITIVE;
    ret->primitive = x;
    return ret;
}

struct object *make_lambda(struct object *params, struct object *body) {
    return cons(LAMBDA, cons(params, body));
}

struct object *make_procedure(struct object *params, struct object *body,
                              struct object *env) {
    return cons(PROCEDURE, cons(params, cons(body, cons(env, EMPTY_LIST))));
}

struct object *cons(struct object *x, struct object *y) {
    struct object *ret = alloc();
    ret->type = LIST;
    ret->car = x;
    ret->cdr = y;
    return ret;
}

struct object *car(struct object *cell) {
    if (null(cell) || cell->type != LIST)
        return NIL;
    return cell->car;
}

struct object *cdr(struct object *cell) {
    if (null(cell) || cell->type != LIST)
        return NIL;
    return cell->cdr;
}

struct object *append(struct object *l1, struct object *l2) {
    if (null(l1))
        return l2;
    return cons(car(l1), append(cdr(l1), l2));
}

struct object *reverse(struct object *list, struct object *first) {
    if (null(list))
        return first;
    return reverse(cdr(list), cons(car(list), first));
}

// Pointer equality
bool is_equal(struct object *x, struct object *y) {

    if (x == y)
        return true;
    if (null(x) || null(y))
        return false;
    if (x->type != y->type)
        return false;
    switch (x->type) {
    case LIST:
        return false;
    case INTEGER:
        return x->integer == y->integer;
    case SYMBOL:
    case STRING:
        return !strcmp(x->string, y->string);
    case PRIMITIVE:
        return false;
    case VECTOR:
        return false;
    }
    return false;
}

bool not_false(struct object *x) {
    if (null(x) || is_equal(x, FALSEE))
        return false;
    if (x->type == INTEGER && x->integer == 0)
        return false;
    return true;
}

bool is_tagged(struct object *cell, struct object *tag) {
    if (null(cell) || cell->type != LIST)
        return false;
    return is_equal(car(cell), tag);
}

int length(struct object *exp) {
    if (null(exp))
        return 0;
    return 1 + length(cdr(exp));
}
/*==============================================================================
Primitive operations
==============================================================================*/

struct object *prim_type(struct object *args) {
    char *types[6] = {"integer", "symbol",    "string",
                      "list",    "primitive", "vector"};
    return make_symbol(types[car(args)->type]);
}

struct object *prim_get_env(struct object *args) {
    assert(null(args));
    return ENV;
}
struct object *prim_set_env(struct object *args) {
    ENV = car(args);
    return NIL;
}

struct object *prim_list(struct object *args) {
    return (args);
}
struct object *prim_cons(struct object *args) {
    return cons(car(args), cadr(args));
}

struct object *prim_car(struct object *args) {
#ifdef STRICT
    ASSERT_TYPE(car(args), LIST);
#endif
    return caar(args);
}

struct object *prim_cdr(struct object *args) {
#ifdef STRICT
    ASSERT_TYPE(car(args), LIST);
#endif
    return cdar(args);
}

struct object *prim_setcar(struct object *args) {
    ASSERT_TYPE(car(args), LIST);
    (args->car->car = (cadr(args)));
    return NIL;
}
struct object *prim_setcdr(struct object *args) {
    ASSERT_TYPE(car(args), LIST);
    (args->car->cdr = (cadr(args)));
    return NIL;
}

struct object *prim_nullq(struct object *args) {
    return EOL(car(args)) ? TRUEE : FALSEE;
}

struct object *prim_pairq(struct object *args) {
    if (car(args)->type != LIST)
        return FALSEE;
    return (atom(caar(args)) && atom(cdar(args))) ? TRUEE : FALSEE;
}

struct object *prim_listq(struct object *args) {
    struct object *list;
    if (car(args)->type != LIST)
        return FALSEE;
    for (list = car(args); !null(list); list = list->cdr)
        if (!null(list->cdr) && (list->cdr->type != LIST))
            return FALSEE;
    return (car(args)->type == LIST && prim_pairq(args) != TRUEE) ? TRUEE : FALSEE;
}

struct object *prim_atomq(struct object *sexp) {
    return atom(car(sexp)) ? TRUEE : FALSEE;
}

/* = primitive, only valid for numbers */
struct object *prim_neq(struct object *args) {
    if ((car(args)->type != INTEGER) || (cadr(args)->type != INTEGER))
        return FALSEE;
    return (car(args)->integer == cadr(args)->integer) ? TRUEE : FALSEE;
}

/* eq? primitive, checks memory location, or if equal values for primitives */
struct object *prim_eq(struct object *args) {
    return is_equal(car(args), cadr(args)) ? TRUEE : FALSEE;
}

struct object *prim_equal(struct object *args) {
    if (is_equal(car(args), cadr(args)))
        return TRUEE;
    if ((car(args)->type == LIST) && (cadr(args)->type == LIST)) {
        struct object *a, *b;
        a = car(args);
        b = cadr(args);
        while (!null(a) && !null(b)) {
            if (!is_equal(car(a), car(b)))
                return FALSEE;
            a = cdr(a);
            b = cdr(b);
        }
        return TRUEE;
    }
    if ((car(args)->type == VECTOR) && (cadr(args)->type == VECTOR)) {
        if (car(args)->vsize != cadr(args)->vsize) {
            return FALSEE;
        }
        struct object **va = car(args)->vector;
        struct object **vb = cadr(args)->vector;
        int i = 0;
        for (i = 0; i < car(args)->vsize; i++) {
            if (!is_equal(*(va + i), *(vb + i))) {
                return FALSEE;
            }
        }
        return TRUEE;
    }
    return FALSEE;
}

struct object *prim_add(struct object *list) {
    ASSERT_TYPE(car(list), INTEGER);
    int64_t total = car(list)->integer;
    list = cdr(list);
    while (!EOL(car(list))) {
        ASSERT_TYPE(car(list), INTEGER);
        total += car(list)->integer;
        list = cdr(list);
    }
    return make_integer(total);
}

struct object *prim_sub(struct object *list) {
    ASSERT_TYPE(car(list), INTEGER);
    int64_t total = car(list)->integer;
    list = cdr(list);
    while (!null(list)) {
        ASSERT_TYPE(car(list), INTEGER);
        total -= car(list)->integer;
        list = cdr(list);
    }
    return make_integer(total);
}

struct object *prim_div(struct object *list) {
    ASSERT_TYPE(car(list), INTEGER);
    int64_t total = car(list)->integer;
    list = cdr(list);
    while (!null(list)) {
        ASSERT_TYPE(car(list), INTEGER);
        total /= car(list)->integer;
        list = cdr(list);
    }
    return make_integer(total);
}

struct object *prim_mul(struct object *list) {
    ASSERT_TYPE(car(list), INTEGER);
    int64_t total = car(list)->integer;
    list = cdr(list);
    while (!null(list)) {
        ASSERT_TYPE(car(list), INTEGER);
        total *= car(list)->integer;
        list = cdr(list);
    }
    return make_integer(total);
}
struct object *prim_gt(struct object *sexp) {
    ASSERT_TYPE(car(sexp), INTEGER);
    ASSERT_TYPE(cadr(sexp), INTEGER);
    return (car(sexp)->integer > cadr(sexp)->integer) ? TRUEE : NIL;
}

struct object *prim_lt(struct object *sexp) {
    ASSERT_TYPE(car(sexp), INTEGER);
    ASSERT_TYPE(cadr(sexp), INTEGER);
    return (car(sexp)->integer < cadr(sexp)->integer) ? TRUEE : NIL;
}

struct object *prim_print(struct object *args) {
    print_exp(NULL, car(args));
    printf("\n");
    return NIL;
}

struct object *prim_exit(struct object *args) {
    assert(null(args));
    exit(0);
}

struct object *prim_read(struct object *args) {
    assert(null(args));
    return read_exp(stdin);
}

struct object *prim_vget(struct object *args) {
    ASSERT_TYPE(car(args), VECTOR);
    ASSERT_TYPE(cadr(args), INTEGER);
    if (cadr(args)->integer >= car(args)->vsize)
        return NIL;
    return car(args)->vector[cadr(args)->integer];
}

struct object *prim_vset(struct object *args) {
    ASSERT_TYPE(car(args), VECTOR);
    ASSERT_TYPE(cadr(args), INTEGER);
    if (null(caddr(args)))
        return NIL;
    if (cadr(args)->integer >= car(args)->vsize)
        return NIL;
    car(args)->vector[cadr(args)->integer] = caddr(args);
    return make_symbol("ok");
}

struct object *prim_vec(struct object *args) {
    ASSERT_TYPE(car(args), INTEGER);
    return make_vector(car(args)->integer);
}

/*==============================================================================
Environment handling
==============================================================================*/

struct object *extend_env(struct object *var, struct object *val,
                          struct object *env) {
    return cons(cons(var, val), env);
}

struct object *lookup_variable(struct object *var, struct object *env) {
    while (!null(env)) {
        struct object *frame = car(env);
        struct object *vars = car(frame);
        struct object *vals = cdr(frame);
        while (!null(vars)) {
            if (is_equal(car(vars), var))
                return car(vals);
            vars = cdr(vars);
            vals = cdr(vals);
        }
        env = cdr(env);
    }
    return NIL;
}

/* set_variable binds var to val in the first frame in which var occurs */
void set_variable(struct object *var, struct object *val, struct object *env) {
    while (!null(env)) {
        struct object *frame = car(env);
        struct object *vars = car(frame);
        struct object *vals = cdr(frame);
        while (!null(vars)) {
            if (is_equal(car(vars), var)) {
                vals->car = val;
                return;
            }
            vars = cdr(vars);
            vals = cdr(vals);
        }
        env = cdr(env);
    }
}

/* define_variable binds var to val in the *current* frame */
struct object *define_variable(struct object *var, struct object *val,
                               struct object *env) {
    struct object *frame = car(env);
    struct object *vars = car(frame);
    struct object *vals = cdr(frame);

    while (!null(vars)) {
        if (is_equal(var, car(vars))) {
            vals->car = val;
            return val;
        }
        vars = cdr(vars);
        vals = cdr(vals);
    }
    frame->car = cons(var, car(frame));
    frame->cdr = cons(val, cdr(frame));
    return val;
}

/*==============================================================================
Recursive descent parser
==============================================================================*/

char SYMBOLS[] = "~!@#$%^&*_-+\\:,.<>|{}[]?=/";

int peek(FILE *in) {
    int c = getc(in);
    ungetc(c, in);
    return c;
}

/* skip characters until end of line */
void skip(FILE *in) {
    int c;
    for (;;) {
        c = getc(in);
        if (c == '\n' || c == EOF)
            return;
    }
}

struct object *read_string(FILE *in) {
    char buf[256];
    int i = 0;
    int c;
    while ((c = getc(in)) != '\"') {
        if (c == EOF)
            return NIL;
        if (i >= 256)
            error("String too long - maximum length 256 characters");
        buf[i++] = (char)c;
    }
    buf[i] = '\0';
    struct object *s = make_symbol(buf);
    s->type = STRING;
    return s;
}

struct object *read_symbol(FILE *in, char start) {
    char buf[128];
    buf[0] = start;
    int i = 1;
    while (isalnum(peek(in)) || strchr(SYMBOLS, peek(in))) {
        if (i >= 128)
            error("Symbol name too long - maximum length 128 characters");
        buf[i++] = getc(in);
    }
    buf[i] = '\0';
    return make_symbol(buf);
}

int read_int(FILE *in, int start) {
    while (isdigit(peek(in)))
        start = start * 10 + (getc(in) - '0');
    return start;
}

struct object *read_list(FILE *in) {
    struct object *obj;
    struct object *cell = EMPTY_LIST;
    for (;;) {
        obj = read_exp(in);

        if (obj == EMPTY_LIST)
            return reverse(cell, EMPTY_LIST);
        cell = cons(obj, cell);
    }
    return EMPTY_LIST;
}

struct object *read_quote(FILE *in) {
    return cons(QUOTE, cons(read_exp(in), NIL));
}

int depth = 0;

struct object *read_exp(FILE *in) {
    int c;

    for (;;) {
        c = getc(in);
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            if ((c == '\n' || c == '\r') && in == stdin) {
                int i;
                for (i = 0; i < depth; i++)
                    printf("..");
            }
            continue;
        }
        if (c == ';') {
            skip(in);
            continue;
        }
        if (c == EOF)
            return NULL;
        if (c == '\"')
            return read_string(in);
        if (c == '\'')
            return read_quote(in);
        if (c == '(') {
            depth++;
            return read_list(in);
        }
        if (c == ')') {
            depth--;
            return EMPTY_LIST;
        }
        if (isdigit(c))
            return make_integer(read_int(in, c - '0'));
        if (c == '-' && isdigit(peek(in)))
            return make_integer(-1 * read_int(in, getc(in) - '0'));
        if (isalpha(c) || strchr(SYMBOLS, c))
            return read_symbol(in, c);
    }
    return NIL;
}

void print_exp(char *str, struct object *e) {
    if (str)
        printf("%s ", str);
    if (null(e)) {
        printf("'()");
        return;
    }
    switch (e->type) {
    case STRING:
        printf("\"%s\"", e->string);
        break;
    case SYMBOL:
        printf("%s", e->string);
        break;
    case INTEGER:
        printf("%ld", e->integer);
        break;
    case PRIMITIVE:
        printf("<function>");
        break;
    case VECTOR:
        printf("<vector %d>", e->vsize);
        break;
    case LIST:
        if (is_tagged(e, PROCEDURE)) {
            printf("<closure>");
            return;
        }
        printf("(");
        struct object **t = &e;
        while (!null(*t)) {
            print_exp(NULL, (*t)->car);
            if (!null((*t)->cdr)) {
                printf(" ");
                if ((*t)->cdr->type == LIST) {
                    t = &(*t)->cdr;
                } else {
                    print_exp(".", (*t)->cdr);
                    break;
                }
            } else
                break;
        }
        printf(")");
    }
}

/*==============================================================================
LISP evaluator
==============================================================================*/

struct object *evlis(struct object *exp, struct object *env) {
    if (null(exp))
        return NIL;
    return cons(eval(car(exp), env), evlis(cdr(exp), env));
}

struct object *eval_sequence(struct object *exps, struct object *env) {
    if (null(cdr(exps)))
        return eval(car(exps), env);
    eval(car(exps), env);
    return eval_sequence(cdr(exps), env);
}

struct object *eval(struct object *exp, struct object *env) {

tail:
    if (null(exp) || exp == EMPTY_LIST) {
        return NIL;
    } else if (exp->type == INTEGER || exp->type == STRING) {
        return exp;
    } else if (exp->type == SYMBOL) {
        struct object *s = lookup_variable(exp, env);
#ifdef STRICT
        if (null(s)) {
            print_exp("Unbound symbol:", exp);
            printf("\n");
        }
#endif
        return s;
    } else if (is_tagged(exp, QUOTE)) {
        return cadr(exp);
    } else if (is_tagged(exp, LAMBDA)) {
        return make_procedure(cadr(exp), cddr(exp), env);
    } else if (is_tagged(exp, DEFINE)) {
        if (atom(cadr(exp)))
            define_variable(cadr(exp), eval(caddr(exp), env), env);
        else {
            struct object *closure =
                eval(make_lambda(cdr(cadr(exp)), cddr(exp)), env);
            define_variable(car(cadr(exp)), closure, env);
        }
        return make_symbol("ok");
    } else if (is_tagged(exp, BEGIN)) {
        struct object *args = cdr(exp);
        for (; !null(cdr(args)); args = cdr(args))
            eval(car(args), env);
        exp = car(args);
        goto tail;
    } else if (is_tagged(exp, IF)) {
        struct object *predicate = eval(cadr(exp), env);
        exp = (not_false(predicate)) ? caddr(exp) : cadddr(exp);
        goto tail;
    } else if (is_tagged(exp, make_symbol("or"))) {
        struct object *predicate = eval(cadr(exp), env);
        exp = (not_false(predicate)) ? caddr(exp) : cadddr(exp);
        goto tail;
    } else if (is_tagged(exp, make_symbol("cond"))) {
        struct object *branch = cdr(exp);
        for (; !null(branch); branch = cdr(branch)) {
            if (is_tagged(car(branch), make_symbol("else")) ||
                not_false(eval(caar(branch), env))) {
                exp = cons(BEGIN, cdar(branch));
                goto tail;
            }
        }
        return NIL;
    } else if (is_tagged(exp, SET)) {
        if (atom(cadr(exp)))
            set_variable(cadr(exp), eval(caddr(exp), env), env);
        else {
            struct object *closure =
                eval(make_lambda(cdr(cadr(exp)), cddr(exp)), env);
            set_variable(car(cadr(exp)), closure, env);
        }
        return make_symbol("ok");
    } else if (is_tagged(exp, LET)) {
        /* We go with the strategy of transforming let into a lambda function*/
        struct object **tmp;
        struct object *vars = NIL;
        struct object *vals = NIL;
        if (null(cadr(exp)))
            return NIL;
        /* NAMED LET */
        if (atom(cadr(exp))) {
            for (tmp = &exp->cdr->cdr->car; !null(*tmp); tmp = &(*tmp)->cdr) {
                vars = cons(caar(*tmp), vars);
                vals = cons(cadar(*tmp), vals);
            }
            /* Define the named let as a lambda function */
            define_variable(cadr(exp),
                            eval(make_lambda(vars, cdr(cddr(exp))),
                                 extend_env(vars, vals, env)),
                            env);
            /* Then evaluate the lambda function with the starting values */
            exp = cons(cadr(exp), vals);
            goto tail;
        }
        for (tmp = &exp->cdr->car; !null(*tmp); tmp = &(*tmp)->cdr) {
            vars = cons(caar(*tmp), vars);
            vals = cons(cadar(*tmp), vals);
        }
        exp = cons(make_lambda(vars, cddr(exp)), vals);
        goto tail;
    } else {
        /* procedure structure is as follows:
        ('procedure, (parameters), (body), (env)) */
        struct object *proc = eval(car(exp), env);
        struct object *args = evlis(cdr(exp), env);
        if (null(proc)) {
#ifdef STRICT
            print_exp("Invalid arguments to eval:", exp);
            printf("\n");
#endif

            return NIL;
        }
        if (proc->type == PRIMITIVE)
            return proc->primitive(args);
        if (is_tagged(proc, PROCEDURE)) {
            env = extend_env(cadr(proc), args, cadddr(proc));
            exp = cons(BEGIN, caddr(proc)); /* procedure body */
            goto tail;
        }
    }
    print_exp("Invalid arguments to eval:", exp);
    printf("\n");
    return NIL;
}

extern char **environ;

struct object *prim_exec(struct object *args) {
    ASSERT_TYPE(car(args), STRING);
    int l = length(args);
    struct object *tmp = args;

    char **newarg = (char **) malloc(sizeof(char *) * (l + 1));
    char **n = newarg;
    for (; l; l--) {
        ASSERT_TYPE(car(tmp), STRING);
        *n++ = car(tmp)->string;
        tmp = cdr(tmp);
    }
    *n = NULL;
    int pid = fork();
    if (pid == 0) {
        /* if execve returns -1, there was an errorm so we need to kill*/
        if (execve(car(args)->string, newarg, environ)) {
            perror(car(args)->string);
            //kill(getpid(), SIGTERM);
        }
    }
    wait(&pid);
    return NIL;
}



/* Initialize the global environment, add primitive functions and symbols */
void init_env() {
#define add_prim(s, c) define_variable(make_symbol(s), make_primitive(c), ENV)
#define add_sym(s, c)                                                          \
    do {                                                                       \
        c = make_symbol(s);                                                    \
        define_variable(c, c, ENV);                                            \
    } while (0);
    ENV = extend_env(NIL, NIL, NIL);
    add_sym("#t", TRUEE);
    add_sym("#f", FALSEE);
    add_sym("quote", QUOTE);
    add_sym("lambda", LAMBDA);
    add_sym("procedure", PROCEDURE);
    add_sym("define", DEFINE);
    add_sym("let", LET);
    add_sym("set!", SET);
    add_sym("begin", BEGIN);
    add_sym("if", IF);
    define_variable(make_symbol("true"), TRUEE, ENV);
    define_variable(make_symbol("false"), FALSEE, ENV);

    add_prim("cons", prim_cons);
    add_prim("car", prim_car);
    add_prim("cdr", prim_cdr);
    add_prim("set-car!", prim_setcar);
    add_prim("set-cdr!", prim_setcdr);
    add_prim("list", prim_list);
    add_prim("list?", prim_listq);
    add_prim("null?", prim_nullq);
    add_prim("pair?", prim_pairq);
    add_prim("atom?", prim_atomq);
    add_prim("eq?", prim_eq);
    add_prim("equal?", prim_equal);

    add_prim("+", prim_add);
    add_prim("-", prim_sub);
    add_prim("*", prim_mul);
    add_prim("/", prim_div);
    add_prim("=", prim_neq);
    add_prim("<", prim_lt);
    add_prim(">", prim_gt);

    add_prim("type", prim_type);
    add_prim("load", load_file);
    add_prim("print", prim_print);
    add_prim("get-global-environment", prim_get_env);
    add_prim("set-global-environment", prim_set_env);
    //add_prim("exit", prim_exit);
    //add_prim("exec", prim_exec);
    add_prim("read", prim_read);
    add_prim("vector", prim_vec);
    add_prim("vector-get", prim_vget);
    add_prim("vector-set", prim_vset);
}

/* Loads and evaluates a file containing lisp s-expressions */
struct object *load_file(struct object *args) {
    struct object *exp;
    struct object *ret = NULL;
    char *filename = car(args)->string;
    printf("Evaluating file %s\n", filename);
    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        printf("Error opening file %s\n", filename);
        return NIL;
    }

for (;;) {
        exp = read_exp(fp);
        if (null(exp))
            break;
        ret = eval(exp, ENV);
    }
    fclose(fp);
    return ret;
}

/*
int mainX(int argc, char **argv) {
    int NELEM = 8191;
    ht_init(NELEM);
    init_env();
    struct object *exp;
    int i;

    printf(
        "Microlisp intrepreter - (c) Michael Lazear 2016-2019, MIT License\n");
    for (i = 1; i < argc; i++)
        load_file(cons(make_symbol(argv[i]), NIL));

    for (;;) {
        printf("user> ");
        exp = eval(read_exp(stdin), ENV);
        if (!null(exp)) {
            print_exp("====>", exp);
            printf("\n");
        }
    }
}
*/

CKernel::CKernel (void)
:	CStdlibAppStdio ("circleLisp")
{	
}

CStdlibApp::TShutdownMode CKernel::Run (void)
{
	int NELEM = 8191;
    ht_init(NELEM);
    init_env();
    struct object *exp;
    int i;
    
    
    printf(
        "Microlisp intrepreter - (c) Michael Lazear 2016-2019, MIT License\n");
    //for (i = 1; i < argc; i++)
    load_file(cons(make_symbol("lib.scm"), NIL));

    for (;;) {
        printf("user> ");
        exp = eval(read_exp(stdin), ENV);
        if (!null(exp)) {
            print_exp("====>", exp);
            printf("\n");
        }
    }

	return ShutdownHalt;
}
