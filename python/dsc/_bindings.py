import os
import ctypes
from ctypes import (
    c_bool,
    c_char,
    c_char_p,
    c_int,
    c_int8,
    c_int32,
    c_int64,
    c_uint8,
    c_uint32,
    c_size_t,
    c_float,
    c_double,
    c_void_p,
    Structure,
    Array,
    POINTER
)

_DSC_MAX_DIMS = 4
_DSC_MAX_LABEL = 64

_DscCtx = c_void_p

# Todo: make this more flexible
_lib_file = f'{os.path.dirname(__file__)}/libdsc.so'
if not os.path.exists(_lib_file):
    raise RuntimeError(f'Error loading DSC shared object "{_lib_file}"')

_lib = ctypes.CDLL(_lib_file)


class _DscTensor(Structure):
    _fields_ = [
        ('shape', c_int * _DSC_MAX_DIMS),
        ('stride', c_int * _DSC_MAX_DIMS),
        ('label', c_char * _DSC_MAX_LABEL),
        ('data', c_void_p),
        ('ne', c_int),
        ('n_dim', c_int),
        ('dtype', c_uint8),
    ]


_DscTensor_p = POINTER(_DscTensor)


class _C32(Structure):
    _fields_ = [('real', c_float), ('imag', c_float)]


class _C64(Structure):
    _fields_ = [('real', c_double), ('imag', c_double)]


# extern dsc_ctx *dsc_ctx_init(usize nb) noexcept;
def _dsc_ctx_init(nb: int) -> _DscCtx:
    return _lib.dsc_ctx_init(c_size_t(nb))


_lib.dsc_ctx_init.argtypes = [c_size_t]
_lib.dsc_ctx_init.restype = _DscCtx


# extern void dsc_init_fft(dsc_ctx *ctx, int n, int n_workers, dsc_dtype twiddles_dtype) noexcept;
def _dsc_init_fft(ctx: _DscCtx, n: int, n_workers: int, dtype: c_uint8):
    return _lib.dsc_init_fft(ctx, n, n_workers, dtype)


_lib.dsc_init_fft.argtypes = [_DscCtx, c_int, c_int, c_uint8]
_lib.dsc_init_fft.restype = None


# extern void dsc_ctx_free(dsc_ctx *ctx) noexcept;
def _dsc_ctx_free(ctx: _DscCtx):
    return _lib.dsc_ctx_free(ctx)


_lib.dsc_ctx_free.argtypes = [_DscCtx]
_lib.dsc_ctx_free.restype = None


# extern void dsc_ctx_clear(dsc_ctx *ctx) noexcept;
def _dsc_ctx_clear(ctx: _DscCtx):
    return _lib.dsc_ctx_clear(ctx)


_lib.dsc_ctx_clear.argtypes = [_DscCtx]
_lib.dsc_ctx_clear.restype = None


# extern dsc_tensor *dsc_tensor_1d(dsc_ctx *ctx,
#                                        const char *label,
#                                        dsc_dtype dtype,
#                                        int dim1) noexcept;
def _dsc_tensor_1d(ctx: _DscCtx, label: c_char_p, dtype: c_uint8, dim1: c_int):
    return _lib.dsc_tensor_1d(ctx, label, dtype, dim1)


_lib.dsc_tensor_1d.argtypes = [_DscCtx, c_char_p, c_uint8, c_int]
_lib.dsc_tensor_1d.restype = _DscTensor_p


# extern dsc_tensor *dsc_tensor_2d(dsc_ctx *ctx,
#                                        const char *label,
#                                        dsc_dtype dtype,
#                                        int dim1, int dim2) noexcept;
def _dsc_tensor_2d(ctx: _DscCtx, label: c_char_p, dtype: c_uint8, dim1: c_int, dim2: c_int):
    return _lib.dsc_tensor_2d(ctx, label, dtype, dim1, dim2)


_lib.dsc_tensor_2d.argtypes = [_DscCtx, c_char_p, c_uint8, c_int, c_int]
_lib.dsc_tensor_2d.restype = _DscTensor_p


# extern dsc_tensor *dsc_tensor_3d(dsc_ctx *ctx,
#                                        const char *label,
#                                        dsc_dtype dtype,
#                                        int dim1, int dim2,
#                                        int dim3) noexcept;
def _dsc_tensor_3d(ctx: _DscCtx, label: c_char_p, dtype: c_uint8, dim1: c_int, dim2: c_int, dim3: c_int):
    return _lib.dsc_tensor_3d(ctx, label, dtype, dim1, dim2, dim3)


_lib.dsc_tensor_3d.argtypes = [_DscCtx, c_char_p, c_uint8, c_int, c_int, c_int]
_lib.dsc_tensor_3d.restype = _DscTensor_p


# extern dsc_tensor *dsc_tensor_4d(dsc_ctx *ctx,
#                                        const char *label,
#                                        dsc_dtype dtype,
#                                        int dim1, int dim2,
#                                        int dim3, int dim4) noexcept;
def _dsc_tensor_4d(ctx: _DscCtx, label: c_char_p, dtype: c_uint8, dim1: c_int, dim2: c_int,
                   dim3: c_int, dim4: c_int):
    return _lib.dsc_tensor_4d(ctx, label, dtype, dim1, dim2, dim3, dim4)


_lib.dsc_tensor_4d.argtypes = [_DscCtx, c_char_p, c_uint8, c_int, c_int, c_int, c_int]
_lib.dsc_tensor_4d.restype = _DscTensor_p


# extern dsc_tensor *dsc_arange(dsc_ctx *ctx,
#                                     int n,
#                                     dsc_dtype dtype = DSC_DEFAULT_TYPE) noexcept;
def _dsc_arange(ctx: _DscCtx, n: int, dtype: c_uint8) -> _DscTensor_p:
    return _lib.dsc_arange(ctx, n, dtype)


_lib.dsc_arange.argtypes = [_DscCtx, c_int, c_uint8]
_lib.dsc_arange.restype = _DscTensor_p


# extern dsc_tensor *dsc_mul(dsc_ctx *ctx,
#                                  dsc_tensor *__restrict xa,
#                                  dsc_tensor *__restrict xb) noexcept;
def _dsc_mul(ctx: _DscCtx, xa: _DscTensor_p, xb: _DscTensor_p) -> _DscTensor_p:
    return _lib.dsc_mul(ctx, xa, xb)


_lib.dsc_mul.argtypes = [_DscCtx, _DscTensor_p, _DscTensor_p]
_lib.dsc_mul.restype = _DscTensor_p


# TODO: codegen for this kind of function? --> something like this!
# # Define a helper function to generate bindings
# def bind_function(name, argtype):
#     func = getattr(lib, f"{name}_func")
#     func.argtypes = [argtype]
#     func.restype = None
#     return func
#
# # Generate bindings
# int_func = bind_function('int', ctypes.c_int)
# float_func = bind_function('float', ctypes.c_float)
# double_func = bind_function('double', ctypes.c_double)
def _dsc_mulc_f32(ctx: _DscCtx, x: _DscTensor_p, val: float) -> _DscTensor_p:
    return _lib.dsc_mulc_f32(ctx, x, c_float(val))


_lib.dsc_mulc_f32.argtypes = [_DscCtx, _DscTensor_p, c_float]
_lib.dsc_mulc_f32.restype = _DscTensor_p


def _dsc_mulc_f64(ctx: _DscCtx, x: _DscTensor_p, val: float) -> _DscTensor_p:
    return _lib.dsc_mulc_f64(ctx, x, c_double(val))


_lib.dsc_mulc_f64.argtypes = [_DscCtx, _DscTensor_p, c_double]
_lib.dsc_mulc_f64.restype = _DscTensor_p


def _dsc_mulc_c32(ctx: _DscCtx, x: _DscTensor_p, val: complex) -> _DscTensor_p:
    return _lib.dsc_mulc_c32(ctx, x, _C32(c_float(val.real), c_float(val.imag)))


_lib.dsc_mulc_c32.argtypes = [_DscCtx, _DscTensor_p, _C32]
_lib.dsc_mulc_c32.restype = _DscTensor_p


def _dsc_mulc_c64(ctx: _DscCtx, x: _DscTensor_p, val: complex) -> _DscTensor_p:
    return _lib.dsc_mulc_c64(ctx, x, _C64(c_double(val.real), c_double(val.imag)))


_lib.dsc_mulc_c64.argtypes = [_DscCtx, _DscTensor_p, _C64]
_lib.dsc_mulc_c64.restype = _DscTensor_p


#def _dsc_full_f32(ctx: _DscCtx, n: int, val: float) -> _DscTensor_p:
#    return _lib.dsc_full_f32(ctx, n, c_float(val))
#
#
#_lib.dsc_full_f32.argtypes = [_DscCtx, c_int, c_float]
#_lib.dsc_full_f32.restype = _DscTensor_p
#
#
#def _dsc_full_f64(ctx: _DscCtx, n: int, val: float) -> _DscTensor_p:
#    return _lib.dsc_full_f64(ctx, n, c_double(val))
#
#
#_lib.dsc_full_f64.argtypes = [_DscCtx, c_int, c_double]
#_lib.dsc_full_f64.restype = _DscTensor_p
#
#
#def _dsc_full_c32(ctx: _DscCtx, n: int, val: complex) -> _DscTensor_p:
#    return _lib.dsc_full_c32(ctx, n, c_float(val.real), c_float(val.imag))
#
#
#_lib.dsc_full_c32.argtypes = [_DscCtx, c_int, c_float, c_float]
#_lib.dsc_full_c32.restype = _DscTensor_p
#
#
#def _dsc_full_c64(ctx: _DscCtx, n: int, val: complex) -> _DscTensor_p:
#    return _lib.dsc_full_c64(ctx, n, c_double(val.real), c_double(val.imag))
#
#
#_lib.dsc_full_c64.argtypes = [_DscCtx, c_int, c_double, c_double]
#_lib.dsc_full_c64.restype = _DscTensor_p


# extern dsc_tensor *dsc_cast(dsc_ctx *ctx,
#                                   dsc_dtype new_dtype,
#                                   dsc_tensor *__restrict x) noexcept;
def _dsc_cast(ctx: _DscCtx, dtype: c_uint8, x: _DscTensor_p) -> _DscTensor_p:
    return _lib.dsc_cast(ctx, dtype, x)


_lib.dsc_cast.argtypes = [_DscCtx, c_uint8, _DscTensor_p]
_lib.dsc_cast.restype = _DscTensor_p


# extern dsc_tensor *dsc_cos(dsc_ctx *,
#                                  dsc_tensor *__restrict x) noexcept;
def _dsc_cos(ctx: _DscCtx, x: _DscTensor_p) -> _DscTensor_p:
    return _lib.dsc_cos(ctx, x)


_lib.dsc_cos.argtypes = [_DscCtx, _DscTensor_p]
_lib.dsc_cos.restype = _DscTensor_p


# extern dsc_tensor *dsc_fft(dsc_ctx *ctx,
#                                  dsc_tensor *DSC_RESTRICT x,
#                                  int axis = -1) noexcept;
def _dsc_fft(ctx: _DscCtx, x: _DscTensor_p, axis: c_int) -> _DscTensor_p:
    return _lib.dsc_fft(ctx, x, axis)


_lib.dsc_fft.argtypes = [_DscCtx, _DscTensor_p, c_int]
_lib.dsc_fft.restype = _DscTensor_p


# extern dsc_tensor *dsc_ifft(dsc_ctx *ctx,
#                                   dsc_tensor *DSC_RESTRICT x,
#                                   int axis = -1) noexcept;
def _dsc_ifft(ctx: _DscCtx, x: _DscTensor_p, axis: c_int) -> _DscTensor_p:
    return _lib.dsc_ifft(ctx, x, axis)


_lib.dsc_ifft.argtypes = [_DscCtx, _DscTensor_p, c_int]
_lib.dsc_ifft.restype = _DscTensor_p
