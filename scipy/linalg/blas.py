#
# Author: Pearu Peterson, March 2002
#

__all__ = ['get_blas_funcs']


# The following ensures that possibly missing flavor (C or Fortran) is
# replaced with the available one. If none is available, exception
# is raised at the first attempt to use the resources.

from scipy.linalg import fblas
try:
    from scipy.linalg import cblas
except ImportError:
    cblas = fblas
else:
    if hasattr(cblas,'empty_module'):
        cblas = fblas
    elif hasattr(fblas,'empty_module'):
        fblas = cblas
from funcinfo import register_func_info

_type_conv = {'f':'s', 'd':'d', 'F':'c', 'D':'z'} # 'd' will be default for 'i',..
_inv_type_conv = {'s':'f','d':'d','c':'F','z':'D'}

def has_column_major_storage(arr):
    """Is array stored in column-major format"""
    return arr.flags['FORTRAN']

def get_blas_funcs(names,arrays=(),debug=0):
    """Return available BLAS function objects with names.
    arrays are used to determine the optimal prefix of
    BLAS routines.

    """
    ordering = []
    for i in range(len(arrays)):
        t = arrays[i].dtype.char
        if t not in _type_conv:
            t = 'd'
        ordering.append((t,i))
    if ordering:
        ordering.sort()
        required_prefix = _type_conv[ordering[0][0]]
    else:
        required_prefix = 'd'
    typecode = _inv_type_conv[required_prefix]
    # Default lookup:
    if ordering and has_column_major_storage(arrays[ordering[0][1]]):
        # prefer Fortran code for leading array with column major order
        m1,m2 = fblas,cblas
    else:
        # in all other cases, C code is preferred
        m1,m2 = cblas,fblas
    funcs = []
    for name in names:
        if name=='ger' and typecode in 'FD':
            name = 'gerc'
        func_name = required_prefix + name
        func = getattr(m1,func_name,None)
        if func is None:
            func = getattr(m2,func_name)
            module_name = m2.__name__.split('.')[-1]
        else:
            module_name = m1.__name__.split('.')[-1]
        register_func_info(func, module_name, required_prefix, typecode, None)
        funcs.append(func)
    return tuple(funcs)
