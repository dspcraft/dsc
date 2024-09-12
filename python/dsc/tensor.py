# Copyright (c) 2024, Christian Gilli <christian.gilli@dspcraft.com>
# All rights reserved.
#
# This code is licensed under the terms of the 3-clause BSD license
# (https://opensource.org/license/bsd-3-clause).

from ._bindings import (
    _DscTensor_p, _DSC_MAX_DIMS, _dsc_add, _dsc_sub, _dsc_mul, _dsc_div, _dsc_pow, _dsc_cast, _dsc_sum,
    _dsc_addc_f32, _dsc_addc_f64, _dsc_addc_c32, _dsc_addc_c64,
    _dsc_subc_f32, _dsc_subc_f64, _dsc_subc_c32, _dsc_subc_c64,
    _dsc_mulc_f32, _dsc_mulc_f64, _dsc_mulc_c32, _dsc_mulc_c64,
    _dsc_divc_f32, _dsc_divc_f64, _dsc_divc_c32, _dsc_divc_c64,
    _dsc_powc_f32, _dsc_powc_f64, _dsc_powc_c32, _dsc_powc_c64,
    _dsc_plan_fft, _dsc_fft, _dsc_ifft, _dsc_rfft, _dsc_irfft, _dsc_fftfreq, _dsc_rfftfreq, _dsc_arange, _dsc_randn,
    _dsc_cos, _dsc_sin, _dsc_sinc, _dsc_logn, _dsc_log2, _dsc_log10, _dsc_exp, _dsc_sqrt, _dsc_abs, _dsc_angle,
    _dsc_conj, _dsc_real, _dsc_imag, _dsc_tensor_1d, _dsc_tensor_2d, _dsc_tensor_3d, _dsc_tensor_4d,
)
from .dtype import *
from .context import _get_ctx
import ctypes
from ctypes import (
    c_uint8,
    c_int
)
import sys
from typing import Union


def _c_ptr(x: 'Tensor') -> _DscTensor_p:
    return x._c_ptr if x else None


class Tensor:
    def __init__(self, c_ptr: _DscTensor_p):
        self._dtype = Dtype(c_ptr.contents.dtype)
        self._shape = c_ptr.contents.shape
        self._n_dim = c_ptr.contents.n_dim
        self._c_ptr = c_ptr

    @property
    def dtype(self) -> Dtype:
        return self._dtype

    @property
    def shape(self) -> tuple[int]:
        return tuple(self._shape[_DSC_MAX_DIMS - self.n_dim:])

    @property
    def n_dim(self) -> int:
        return self._n_dim

    def __add__(self, other: Union[float, complex, 'Tensor', np.ndarray]) -> 'Tensor':
        return add(self, other)

    def __sub__(self, other: Union[float, complex, 'Tensor', np.ndarray]) -> 'Tensor':
        return sub(self, other)

    def __mul__(self, other: Union[float, complex, 'Tensor', np.ndarray]) -> 'Tensor':
        return mul(self, other)

    def __truediv__(self, other: Union[float, complex, 'Tensor', np.ndarray]) -> 'Tensor':
        return true_div(self, other)

    def __pow__(self, other: Union[float, complex, 'Tensor', np.ndarray]) -> 'Tensor':
        return power(self, other)

    def __len__(self):
        return self.shape[0]

    def numpy(self) -> np.ndarray:
        raw_tensor = self._c_ptr.contents

        typed_data = ctypes.cast(raw_tensor.data, DTYPE_TO_CTYPE[self.dtype])

        # Create a view of the underlying data buffer
        np_array = np.ctypeslib.as_array(typed_data, shape=self.shape)

        # If it's a complex number, change the np dtype
        if self.dtype == Dtype.C32:
            np_array = np_array.view(np.complex64)
        elif self.dtype == Dtype.C64:
            np_array = np_array.view(np.complex128)

        return np_array.reshape(self.shape)

    def cast(self, dtype: Dtype) -> 'Tensor':
        return Tensor(_dsc_cast(_get_ctx(), self._c_ptr, c_uint8(dtype.value)))


def from_numpy(x: np.ndarray) -> Tensor:
    if x.dtype not in NP_TO_DTYPE:
        raise RuntimeError(f'NumPy dtype {x.dtype} is not supported')

    dtype = c_uint8(NP_TO_DTYPE[x.dtype].value)

    dims = list(x.shape)
    n_dims = len(dims)
    if n_dims > _DSC_MAX_DIMS or n_dims < 1:
        raise RuntimeError(f'can\'t create a Tensor with {n_dims} dimensions')

    if n_dims == 1:
        res = Tensor(_dsc_tensor_1d(_get_ctx(), dtype, c_int(dims[0])))
    elif n_dims == 2:
        res = Tensor(_dsc_tensor_2d(_get_ctx(), dtype,
                                    c_int(dims[0]), c_int(dims[1])))
    elif n_dims == 3:
        res = Tensor(_dsc_tensor_3d(_get_ctx(), dtype,
                                    c_int(dims[0]), c_int(dims[1]),
                                    c_int(dims[2])))
    else:
        res = Tensor(_dsc_tensor_4d(_get_ctx(), dtype,
                                    c_int(dims[0]), c_int(dims[1]),
                                    c_int(dims[2]), c_int(dims[3])))

    ctypes.memmove(_c_ptr(res).contents.data, x.ctypes.data, x.nbytes)
    return res


def _scalar_op(xa: Tensor, xb: Union[float, complex], out: Tensor, op_base_name: str) -> Tensor:
    op_dtype = xa.dtype
    if isinstance(xb, float):
        if xa.dtype == Dtype.C32 or xa.dtype == Dtype.C64:
            xb = complex(xb, 0)
    else:
        # The cast op is handled by the C library
        if xa.dtype == Dtype.F32:
            op_dtype = Dtype.C32
        elif xa.dtype == Dtype.F64:
            op_dtype = Dtype.C64

    op_name = f'{op_base_name}_{op_dtype}'
    if hasattr(sys.modules[__name__], op_name):
        op = getattr(sys.modules[__name__], op_name)
        return Tensor(op(_get_ctx(), _c_ptr(xa), xb, _c_ptr(out)))
    else:
        raise RuntimeError(f'scalar operation "{op_name}" doesn\'t exist in module')


def _tensor_op(xa: Tensor, xb: Union[Tensor, np.ndarray], out: Tensor, op_name: str) -> Tensor:
    if isinstance(xb, np.ndarray):
        # The conversion from (and to) NumPy is not free, so it's better to do that once and then work with DSC tensors.
        # This is here just for convenience not because it's a best practice.
        xb = from_numpy(xb)

    if hasattr(sys.modules[__name__], op_name):
        op = getattr(sys.modules[__name__], op_name)
        return Tensor(op(_get_ctx(), _c_ptr(xa), _c_ptr(xb), _c_ptr(out)))
    else:
        raise RuntimeError(f'tensor operation "{op_name}" doesn\'t exist in module')


def add(xa: Tensor, xb: Union[float, complex, Tensor, np.ndarray], out: Tensor = None) -> Tensor:
    if isinstance(xb, (float, complex)):
        return _scalar_op(xa, xb, out, op_base_name='_dsc_addc')
    elif isinstance(xb, (np.ndarray, Tensor)):
        return _tensor_op(xa, xb, out, op_name='_dsc_add')
    else:
        raise RuntimeError(f'can\'t add Tensor with object of type {type(xb)}')


def sub(xa: Tensor, xb: Union[float, complex, Tensor, np.ndarray], out: Tensor = None) -> Tensor:
    if isinstance(xb, (float, complex)):
        return _scalar_op(xa, xb, out, op_base_name='_dsc_subc')
    elif isinstance(xb, (np.ndarray, Tensor)):
        return _tensor_op(xa, xb, out, op_name='_dsc_sub')
    else:
        raise RuntimeError(f'can\'t subtract Tensor with object of type {type(xb)}')


def mul(xa: Tensor, xb: Union[float, complex, Tensor, np.ndarray], out: Tensor = None) -> Tensor:
    if isinstance(xb, (float, complex)):
        return _scalar_op(xa, xb, out, op_base_name='_dsc_mulc')
    elif isinstance(xb, (np.ndarray, Tensor)):
        return _tensor_op(xa, xb, out, op_name='_dsc_mul')
    else:
        raise RuntimeError(f'can\'t multiply Tensor with object of type {type(xb)}')


def true_div(xa: Tensor, xb: Union[float, complex, Tensor, np.ndarray], out: Tensor = None) -> Tensor:
    if isinstance(xb, (float, complex)):
        return _scalar_op(xa, xb, out, op_base_name='_dsc_divc')
    elif isinstance(xb, (np.ndarray, Tensor)):
        return _tensor_op(xa, xb, out, op_name='_dsc_div')
    else:
        raise RuntimeError(f'can\'t divide Tensor with object of type {type(xb)}')


def power(xa: Tensor, xb: Union[float, complex, Tensor, np.ndarray], out: Tensor = None) -> Tensor:
    if isinstance(xb, (float, complex)):
        return _scalar_op(xa, xb, out, op_base_name='_dsc_powc')
    elif isinstance(xb, (np.ndarray, Tensor)):
        return _tensor_op(xa, xb, out, op_name='_dsc_pow')
    else:
        raise RuntimeError(f'can\'t raise to the power Tensor with object of type {type(xb)}')


def cos(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_cos(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def sin(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_sin(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def sinc(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_sinc(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def logn(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_logn(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def log2(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_log2(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def log10(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_log10(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def exp(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_exp(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def sqrt(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_sqrt(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def absolute(x: Tensor, out: Tensor = None) -> Tensor:
    return Tensor(_dsc_abs(_get_ctx(), _c_ptr(x), _c_ptr(out)))


def angle(x: Tensor) -> Tensor:
    return Tensor(_dsc_angle(_get_ctx(), _c_ptr(x)))


def conj(x: Tensor) -> Tensor:
    return Tensor(_dsc_conj(_get_ctx(), _c_ptr(x)))


def real(x: Tensor) -> Tensor:
    return Tensor(_dsc_real(_get_ctx(), _c_ptr(x), ))


def imag(x: Tensor) -> Tensor:
    return Tensor(_dsc_imag(_get_ctx(), _c_ptr(x)))

def sum(x: Tensor, out: Tensor = None, axis: int = -1, keepdims: bool = True) -> Tensor:
    return Tensor(_dsc_sum(_get_ctx(), _c_ptr(x), _c_ptr(out), axis, keepdims))

def arange(n: int, dtype: Dtype = Dtype.F32) -> Tensor:
    return Tensor(_dsc_arange(_get_ctx(), n, c_uint8(dtype.value)))


def randn(*shape: int, dtype: Dtype = Dtype.F32) -> Tensor:
    return Tensor(_dsc_randn(_get_ctx(), shape, c_uint8(dtype.value)))


def plan_fft(n: int, dtype: Dtype = Dtype.F64):
    """
    Create the plan for a one-dimensional FFT/IFFT of size N using dtype for the twiddle factors.
    If this function is not executed before calling either `dsc.fft` or `dsc.ifft` then it will be
    called automatically before doing the first transform causing a slowdown.
    """
    return _dsc_plan_fft(_get_ctx(), n, c_uint8(dtype.value))


def fft(x: Tensor, out: Tensor = None, n: int = -1, axis: int = -1) -> Tensor:
    return Tensor(_dsc_fft(_get_ctx(), _c_ptr(x), _c_ptr(out), n=c_int(n), axis=c_int(axis)))


def ifft(x: Tensor, out: Tensor = None, n: int = -1, axis: int = -1) -> Tensor:
    return Tensor(_dsc_ifft(_get_ctx(), _c_ptr(x), _c_ptr(out), n=c_int(n), axis=c_int(axis)))


def rfft(x: Tensor, out: Tensor = None, n: int = -1, axis: int = -1) -> Tensor:
    return Tensor(_dsc_rfft(_get_ctx(), _c_ptr(x), _c_ptr(out), n=c_int(n), axis=c_int(axis)))


def irfft(x: Tensor, out: Tensor = None, n: int = -1, axis: int = -1) -> Tensor:
    return Tensor(_dsc_irfft(_get_ctx(), _c_ptr(x), _c_ptr(out), n=c_int(n), axis=c_int(axis)))


def fftfreq(n: int, d: float = 1., dtype: Dtype = Dtype.F32) -> Tensor:
    return Tensor(_dsc_fftfreq(_get_ctx(), c_int(n), c_double(d), c_uint8(dtype.value)))


def rfftfreq(n: int, d: float = 1., dtype: Dtype = Dtype.F32) -> Tensor:
    return Tensor(_dsc_rfftfreq(_get_ctx(), c_int(n), c_double(d), c_uint8(dtype.value)))
