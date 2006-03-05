#include "Python.h"
#include "structmember.h"
#include "numpy/arrayobject.h"
#include "math.h"

/*
    I'm using suffixes like _XC and _CX to indicate which argument is a constant
    when there is more than one argument to afunctions

    For these functions, all the variables are passed in before all the constants,
    then reordered based on the suffix.
    
    Currently, only where is treated specially like this: it seems like a common
    function and it can be implemented inline, so we avoid any extra overhead.
*/

enum OpCodes {
    OP_NOOP = 0,
    OP_COPY,
    OP_COPY_C,
    OP_NEG,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_POW,
    OP_MOD,
    OP_ADD_C,
    OP_SUB_C,
    OP_MUL_C,
    OP_DIV_C,
    /* pow_c and mod_c are backwards from  div_c and sub_c in that constant
       is second in the former versus first in the latter since this is the
       more common case for those ops. */
    OP_POW_C,
    OP_MOD_C,
    OP_GT,
    OP_GE,
    OP_EQ,
    OP_NE,
    OP_GT_C,
    OP_GE_C,
    OP_EQ_C,
    OP_NE_C,
    OP_LT_C,
    OP_LE_C,
    OP_SIN,
    OP_COS,
    OP_TAN,
    OP_ARCTAN2,
    OP_WHERE,
    OP_WHERE_XCX,
    OP_WHERE_XXC,
    OP_FUNC_1,
    OP_FUNC_2,
};

/* 
   Lots of functions still to be added: exp, ln, log10, etc, etc. Still not
   sure which get there own opcodes and which get relegated to loopup table. 
   Some functions at least (sin and arctan2 for instance) seem to have a large
   slowdown when run through lookup table. Not entirely sure why.

   To add a function to the lookup table, add to FUNC_CODES (first group is 
   1-arg functions, second is 2-arg functions), also to functions_1 or functions_2
   as appropriate. Finally, use add_func down below to add to funccodes. Functions
   with more arguments aren't implemented at present, but should be easy; just copy
   the 1- or 2-arg case.

   To add a function opcode, just copy OP_SIN or OP_ARCTAN2.

*/

enum FuncCodes {
    FUNC_SINH = 0,
    FUNC_COSH,
    FUNC_TANH,
    
    FUNC_FMOD = 0,
};

typedef double (*Func1Ptr)(double);

Func1Ptr functions_1[] = {
    sinh,
    cosh,
    tanh,
};

typedef double (*Func2Ptr)(double, double);

Func2Ptr functions_2[] = {
    fmod,
};


#define BLOCK_SIZE1 128
#define BLOCK_SIZE2 8

typedef struct
{
    PyObject_HEAD
    unsigned int n_inputs;
    unsigned int n_temps;
    PyObject *program;      /* a python string */
    PyObject *constants;    /* a PyArrayObject */
    PyObject *input_names;  /* tuple of strings */
    double **mem;
    double *temps;
} NumExprObject;

static void
NumExpr_dealloc(NumExprObject *self)
{
    Py_XDECREF(self->program);
    Py_XDECREF(self->constants);
    Py_XDECREF(self->input_names);
    PyMem_Del(self->mem);
    PyMem_Del(self->temps);
}

static PyObject *
NumExpr_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    NumExprObject *self;
    intp dims[] = {0};
    self = (NumExprObject *)type->tp_alloc(type, 0);
    if (self != NULL) {
        self->n_inputs = 0;
        self->n_temps = 0;
        self->mem = NULL;
        self->temps = NULL;
        self->program = PyString_FromString("");
        if (!self->program) {
            Py_DECREF(self);
            return NULL;
        }
        self->constants = PyArray_SimpleNew(1, dims, PyArray_DOUBLE);
        if (!self->constants) {
            Py_DECREF(self);
            return NULL;
        }
        Py_INCREF(Py_None);
        self->input_names = Py_None;
    }
    return (PyObject *)self;
}

static int
NumExpr_init(NumExprObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *program = NULL, *o_constants = NULL, *input_names = NULL, *tmp;
    static char *kwlist[] = {"n_inputs", "n_temps",
                             "program", "constants",
                             "input_names", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "IIS|OO", kwlist,
                                     &self->n_inputs,
                                     &self->n_temps,
                                     &program, &o_constants, &input_names)) {
        return -1;
    }
    if (self->n_inputs + self->n_temps + 1 > 256) {
        PyErr_SetString(PyExc_ValueError,
                        "number inputs + outputs + temporaries must be <= 256");
        return -1;
    }
    if (o_constants) {
        PyObject *constants;
        constants = PyArray_FromAny(o_constants,
                                    PyArray_DescrFromType(PyArray_DOUBLE),
                                    1, 1,
                                    ENSURECOPY | CARRAY_FLAGS_RO,
                                    NULL);
        if (!constants) {
            return -1;
        }
        if (PyArray_DIM(constants, 0) > 256) {
            PyErr_SetString(PyExc_ValueError,
                            "number of constants must be <= 256");
            Py_DECREF(constants);
            return -1;
        }
        tmp = self->constants;
        self->constants = constants;
        Py_XDECREF(tmp);
    }

    tmp = self->program;
    Py_INCREF(program);
    self->program = program;
    Py_XDECREF(tmp);

    tmp = self->input_names;
    if (!input_names) {
        input_names = Py_None;
    }
    Py_INCREF(input_names);
    self->input_names = input_names;
    Py_XDECREF(tmp);

    PyMem_Del(self->mem);
    self->mem = PyMem_New(double *, 1 + self->n_inputs + self->n_temps);
    PyMem_Del(self->temps);
    self->temps = PyMem_New(double, self->n_temps * BLOCK_SIZE1);

    return 0;
}

static PyMemberDef NumExpr_members[] = {
    {"n_inputs", T_UINT, offsetof(NumExprObject, n_inputs), READONLY, NULL},
    {"n_temps", T_UINT, offsetof(NumExprObject, n_temps), READONLY, NULL},
    {"program", T_OBJECT_EX, offsetof(NumExprObject, program), READONLY, NULL},
    {"constants", T_OBJECT_EX, offsetof(NumExprObject, constants),
     READONLY, NULL},
    {"input_names", T_OBJECT, offsetof(NumExprObject, input_names), 0, NULL},
    {NULL},
};

static int
run_interpreter(NumExprObject *self, int len, double *output, double **inputs)
{
    double **mem, *constants;
    char *program;
    unsigned int n_inputs, prog_len, t, blen1, index;

    n_inputs = self->n_inputs;
    mem = self->mem;
    for (t = 0; t < self->n_temps; t++) {
        mem[1+n_inputs+t] = self->temps + BLOCK_SIZE1 * t;
    }

    if (PyString_AsStringAndSize(self->program, &program, &prog_len) < 0) {
        return -1;
    }
    constants = PyArray_DATA(self->constants);

    blen1 = len - len % BLOCK_SIZE1;
    for (index = 0; index < blen1; index += BLOCK_SIZE1) {
#define VECTOR_SIZE BLOCK_SIZE1
#include "interp_body.c"
#undef VECTOR_SIZE
    }
    if (len != blen1) {
        int blen2 = len - len % BLOCK_SIZE2;
        for (index = blen1; index < blen2; index += BLOCK_SIZE2) {
#define VECTOR_SIZE BLOCK_SIZE2
#include "interp_body.c"
#undef VECTOR_SIZE
        }
        if (len != blen2) {
            int rest = len - blen2;
            index = blen2;
#define VECTOR_SIZE rest
#include "interp_body.c"
#undef VECTOR_SIZE
        }
    }
    return 0;
}

/* keyword arguments are ignored! */
static PyObject *
NumExpr_run(NumExprObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *output = NULL, *a_inputs;
    unsigned int n_inputs;
    int i, len = -1;
    double **inputs;

    n_inputs = PyTuple_Size(args);
    if (self->n_inputs != n_inputs) {
        return PyErr_Format(PyExc_ValueError,
                            "number of inputs doesn't match program");
    }
    a_inputs = PyTuple_New(n_inputs);
    if (!a_inputs) {
        return NULL;
    }
    inputs = PyMem_New(double *, n_inputs);
    for (i = 0; i < n_inputs; i++) {
        PyObject *o = PyTuple_GetItem(args, i); /* borrowed ref */
        PyObject *a = PyArray_ContiguousFromAny(o, PyArray_DOUBLE, 0, 0);
        PyTuple_SET_ITEM(a_inputs, i, a);  /* steals reference */
        if (len == -1) {
            len = PyArray_SIZE(a);
            output = PyArray_SimpleNew(PyArray_NDIM(a),
                                       PyArray_DIMS(a),
                                       PyArray_DOUBLE);
        } else {
            if (len != PyArray_SIZE(a)) {
                Py_XDECREF(a_inputs);
                Py_XDECREF(output);
                PyMem_Del(inputs);
                return PyErr_Format(PyExc_ValueError,
                                    "all inputs must be the same size");
            }
        }
        inputs[i] = PyArray_DATA(a);
    }
    if (run_interpreter(self, len, PyArray_DATA(output), inputs) < 0) {
        Py_XDECREF(output);
        output = NULL;
        PyErr_SetString(PyExc_RuntimeError,
                        "an error occurred while running the program");
    }
    Py_XDECREF(a_inputs);
    PyMem_Del(inputs);
    return output;
}

static PyMethodDef NumExpr_methods[] = {
    {"run", (PyCFunction) NumExpr_run, METH_VARARGS, NULL},
    {NULL, NULL}
};

static PyTypeObject NumExprType = {
    PyObject_HEAD_INIT(NULL)
    0,                         /*ob_size*/
    "numexpr.NumExpr",         /*tp_name*/
    sizeof(NumExprObject),     /*tp_basicsize*/
    0,                         /*tp_itemsize*/
    (destructor)NumExpr_dealloc, /*tp_dealloc*/
    0,                         /*tp_print*/
    0,                         /*tp_getattr*/
    0,                         /*tp_setattr*/
    0,                         /*tp_compare*/
    0,                         /*tp_repr*/
    0,                         /*tp_as_number*/
    0,                         /*tp_as_sequence*/
    0,                         /*tp_as_mapping*/
    0,                         /*tp_hash */
    (ternaryfunc)NumExpr_run,  /*tp_call*/
    0,                         /*tp_str*/
    0,                         /*tp_getattro*/
    0,                         /*tp_setattro*/
    0,                         /*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "NumExpr objects",         /* tp_doc */
    0,		               /* tp_traverse */
    0,		               /* tp_clear */
    0,		               /* tp_richcompare */
    0,		               /* tp_weaklistoffset */
    0,		               /* tp_iter */
    0,		               /* tp_iternext */
    NumExpr_methods,           /* tp_methods */
    NumExpr_members,           /* tp_members */
    0,                         /* tp_getset */
    0,                         /* tp_base */
    0,                         /* tp_dict */
    0,                         /* tp_descr_get */
    0,                         /* tp_descr_set */
    0,                         /* tp_dictoffset */
    (initproc)NumExpr_init,    /* tp_init */
    0,                         /* tp_alloc */
    NumExpr_new,               /* tp_new */
};

static PyMethodDef module_methods[] = {
    {NULL}
};

void
initinterpreter(void)
{
    PyObject *m, *d, *o;
    int r;

    if (PyType_Ready(&NumExprType) < 0)
        return;

    m = Py_InitModule3("interpreter", module_methods, NULL);
    if (m == NULL)
        return;

    Py_INCREF(&NumExprType);
    PyModule_AddObject(m, "NumExpr", (PyObject *)&NumExprType);

    import_array();

    d = PyDict_New();
    if (!d) return;

#define add_op(sname, name) o = PyInt_FromLong(name);   \
    r = PyDict_SetItemString(d, sname, o);              \
    Py_XDECREF(o);                                      \
    if (r < 0) {PyErr_SetString(PyExc_RuntimeError, "add_op"); return;}

    add_op("noop", OP_NOOP);
    add_op("copy", OP_COPY);
    add_op("copy_c", OP_COPY_C);
    add_op("neg", OP_NEG);
    add_op("add", OP_ADD);
    add_op("sub", OP_SUB);
    add_op("mul", OP_MUL);
    add_op("div", OP_DIV);
    add_op("pow", OP_POW);
    add_op("mod", OP_MOD);
    add_op("add_c", OP_ADD_C);
    add_op("sub_c", OP_SUB_C);
    add_op("mul_c", OP_MUL_C);
    add_op("div_c", OP_DIV_C);
    add_op("pow_c", OP_POW_C);
    add_op("mod_c", OP_MOD_C);
    add_op("gt", OP_GT);
    add_op("ge", OP_GE);    
    add_op("eq", OP_EQ);
    add_op("ne", OP_NE);
    add_op("gt_c", OP_GT_C);
    add_op("ge_c", OP_GE_C);
    add_op("eq_c", OP_EQ_C);
    add_op("ne_c", OP_NE_C);
    add_op("lt_c", OP_LT_C);
    add_op("le_c", OP_LE_C);
    add_op("sin", OP_SIN);
    add_op("cos", OP_COS);
    add_op("tan", OP_TAN);
    add_op("arctan2", OP_ARCTAN2);
    add_op("where", OP_WHERE);
    add_op("where_xxc", OP_WHERE_XXC);
    add_op("where_xcx", OP_WHERE_XCX);
    add_op("func_1", OP_FUNC_1);
    add_op("func_2", OP_FUNC_2);
#undef add_op

    if (PyModule_AddObject(m, "opcodes", d) < 0) return;
    
    d = PyDict_New();
    if (!d) return;
    
#define add_func(sname, name) o = PyInt_FromLong(name);   \
    r = PyDict_SetItemString(d, sname, o);              \
    Py_XDECREF(o);                                      \
    if (r < 0) {PyErr_SetString(PyExc_RuntimeError, "add_func"); return;}

    add_func("sinh", FUNC_SINH);
    add_func("cosh", FUNC_COSH);
    add_func("tanh", FUNC_TANH);
    
    add_func("fmod", FUNC_FMOD);

#undef add_func

   if (PyModule_AddObject(m, "funccodes", d) < 0) return;

}
