// Copyright (c) 2024, Christian Gilli <christian.gilli@dspcraft.com>
// All rights reserved.
//
// This code is licensed under the terms of the 3-clause BSD license
// (https://opensource.org/license/bsd-3-clause).

#pragma once

#include <cstdio>
#include <cstdlib>
#include "dsc_dtype.h"

// How many independent FFT plans we support. This is completely arbitrary
#if !defined(DSC_MAX_FFT_PLANS)
#   define DSC_MAX_FFT_PLANS ((int) 16)
#endif

// Max number of traces that can be recorded. Changing this will result in more memory
// allocated during context initialization.
#if !defined(DSC_MAX_TRACES)
#   define DSC_MAX_TRACES ((u64) 1'000)
#endif

#define DSC_MAX_OBJS            ((int) 1'000)
#define DSC_MAX_DEVICES         ((int) 2)
#define DSC_DEFAULT_DEVICE      CPU
#define DSC_DEVICE_USE_DEFAULT  ((int) -1)

#define DSC_LOG_FATAL(format, ...)                                            \
    do {                                                                      \
        fprintf(stderr, "[FATAL] %s: " format "\n", __func__, ##__VA_ARGS__); \
        exit(EXIT_FAILURE);                                                   \
    } while (0)
#define DSC_LOG_ERR(format, ...)   fprintf(stderr, "[ERROR] %s: " format"\n",__func__, ##__VA_ARGS__)
#define DSC_LOG_INFO(format, ...)  fprintf(stdout, "[INFO ] %s: " format"\n",__func__, ##__VA_ARGS__)

#define DSC_ASSERT(x)                                                           \
    do {                                                                        \
        if (!(x)) {                                                             \
            fprintf(stderr, "DSC_ASSERT: %s:%d %s\n", __FILE__, __LINE__, #x);  \
            exit(EXIT_FAILURE);                                                 \
        }                                                                       \
    } while(0)

#if defined(DSC_DEBUG)
#   define DSC_LOG_DEBUG(format, ...)  fprintf(stdout, "[DEBUG] %s: " format"\n",__func__, ##__VA_ARGS__)
#else
#   define DSC_LOG_DEBUG(format, ...)  ((void) 0)
#endif

#define DSC_INVALID_CASE(format, ...)   \
    default:                            \
        DSC_LOG_FATAL(format, ##__VA_ARGS__)

#define DSC_UNUSED(x)        ((void) (x))
// Compute the next value of X aligned to Y
#define DSC_ALIGN(x, y)      (((x) + (y) - 1) & ~((y) - 1))
#define DSC_MAX(x, y)        ((x) > (y) ? (x) : (y))
#define DSC_MIN(x, y)        ((x) < (y) ? (x) : (y))
#define DSC_B_TO_KB(b)       ((f64)(b) / 1024.)
#define DSC_B_TO_MB(b)       ((f64)(b) / (1024. * 1024.))
#define DSC_MB(mb)           ((usize) ((mb) * 1024l * 1024l))
#define DSC_KB(kb)           ((usize) ((kb) * 1024l))

#if defined(__GNUC__) && !defined(EMSCRIPTEN)
// A 'strictly pure' function is a function whose return value doesn't depend on the global state of the program,
// this means that it must not access global variables subject to change or access parameters passed by pointer
// unless the actual value of the pointer does not change after the first invocation.
// A 'pure' function is basically the same thing without the restriction on global state change, this means
// that a 'pure' function can take in and read the value of parameters passed by pointer even if that value
// changes between subsequent invocations.
#   define DSC_STRICTLY_PURE    __attribute_const__
#   define DSC_PURE             __attribute_pure__
#   define DSC_INLINE           inline __attribute__((always_inline))
#   define DSC_NOINLINE         __attribute__((noinline))
#   define DSC_MALLOC           __attribute__((malloc))
#else
#   define DSC_STRICTLY_PURE
#   define DSC_PURE
#   define DSC_INLINE           inline
#   define DSC_NOINLINE
#   define DSC_MALLOC
#endif

#define DSC_RESTRICT    __restrict

#if !defined(DSC_MAX_DIMS)
#   define DSC_MAX_DIMS ((int) 4)
#endif

static_assert(DSC_MAX_DIMS == 4, "DSC_MAX_DIMS != 4 - update the code");

#define DSC_VALUE_NONE          INT32_MAX
#define DSC_TWIDDLES(T, PTR)    T *DSC_RESTRICT twiddles = (T *) (PTR)->buf->data
#define DSC_TENSOR_DATA(T, PTR) T *DSC_RESTRICT PTR##_data = (T *) (PTR)->buf->data

#define dsc_tensor_dim(PTR, dim)    (((dim) < 0) ? (DSC_MAX_DIMS + (dim)) : (DSC_MAX_DIMS - (PTR)->n_dim + (dim)))
#define dsc_new_like(CTX, PTR)      (dsc_new_tensor((CTX), (PTR)->n_dim, &(PTR)->shape[dsc_tensor_dim(PTR, 0)], (PTR)->dtype, (PTR)->device))
#define dsc_new_view(CTX, PTR)      (dsc_new_tensor((CTX), (PTR)->n_dim, &(PTR)->shape[dsc_tensor_dim(PTR, 0)], (PTR)->dtype, (PTR)->device, (PTR)->buf))

#if defined(__cplusplus)
extern "C" {
#endif

struct dsc_ctx;
struct dsc_data_buffer;

enum dsc_device_type : i8 {
    DEFAULT = -1,
    CPU,
    CUDA
};

static constexpr const char *DSC_DEVICE_NAMES[2] = {
        "CPU",
        "CUDA",
};

enum dsc_fft_type : u8 {
    INVALID,
    REAL,
    COMPLEX,
};

struct dsc_fft_plan {
    dsc_data_buffer *buf;
    int n;
    // Set to 0 when the plan is used, increment by one each time we go through the plans
    int last_used;
    int device;
    dsc_dtype dtype;
    // An RFFT plan is equal to an FFT plan with N = N/2 but with an extra set
    // of twiddles (hence the storage requirement is the same of an order N FFT).
    dsc_fft_type fft_type;
};

struct dsc_tensor {
    // The shape of this tensor, right-aligned. For example a 1D tensor T of 4 elements
    // will have dim = [1, 1, 1, 4].
    int shape[DSC_MAX_DIMS];
    // Stride for a given dimension expressed in number of bytes.
    int stride[DSC_MAX_DIMS];
    dsc_data_buffer *buf;
    int ne;
    int n_dim;
    dsc_dtype dtype;
    dsc_device_type device;
};

struct dsc_slice {
    union {
        int d[3];
        struct {
            int start, stop, step;
        };
    };
};

// ============================================================
// Helper Functions

static DSC_INLINE DSC_STRICTLY_PURE int dsc_pow2_n(const int n) {
    // Compute the power-of-2 closest to n (n must be a 32bit integer)
    DSC_ASSERT(n > 0);
    int next_pow2_n = n - 1;
    next_pow2_n |= next_pow2_n >> 1;
    next_pow2_n |= next_pow2_n >> 2;
    next_pow2_n |= next_pow2_n >> 4;
    next_pow2_n |= next_pow2_n >> 8;
    next_pow2_n |= next_pow2_n >> 16;
    return next_pow2_n + 1;
}

// ============================================================
// Initialization

extern dsc_ctx *dsc_ctx_init(usize mem_size);

extern dsc_fft_plan *dsc_plan_fft(dsc_ctx *ctx, int n,
                                  dsc_fft_type fft_type,
                                  dsc_dtype dtype = F64);

// ============================================================
// Cleanup/Teardown

extern void dsc_ctx_free(dsc_ctx *ctx);

extern void dsc_tensor_free(dsc_ctx *ctx, dsc_tensor *x);

// ============================================================
// Utilities

extern usize dsc_used_mem(dsc_ctx *ctx);

extern void dsc_print_mem_usage(dsc_ctx *ctx);

extern void dsc_set_default_device(dsc_ctx *ctx, dsc_device_type device);

// ============================================================
// Tracing

extern void dsc_traces_record(dsc_ctx *,
                              bool record = true);

extern void dsc_dump_traces(dsc_ctx *,
                            const char *filename);

extern void dsc_clear_traces(dsc_ctx *);

// ============================================================
// Tensor Creation

extern dsc_tensor *dsc_new_tensor(dsc_ctx *ctx,
                                  int n_dim,
                                  const int *shape,
                                  dsc_dtype dtype,
                                  dsc_device_type device = DEFAULT,
                                  dsc_data_buffer *buf = nullptr);

extern dsc_tensor *dsc_view(dsc_ctx *ctx,
                            const dsc_tensor *x);

extern dsc_tensor *dsc_tensor_1d(dsc_ctx *ctx,
                                 dsc_dtype dtype,
                                 int dim1,
                                 dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_tensor_2d(dsc_ctx *ctx,
                                 dsc_dtype dtype,
                                 int dim1, int dim2,
                                 dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_tensor_3d(dsc_ctx *ctx,
                                 dsc_dtype dtype,
                                 int dim1, int dim2,
                                 int dim3,
                                 dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_tensor_4d(dsc_ctx *ctx,
                                 dsc_dtype dtype,
                                 int dim1, int dim2,
                                 int dim3, int dim4,
                                 dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_wrap_f32(dsc_ctx *ctx,
                                f32 val,
                                dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_wrap_f64(dsc_ctx *ctx,
                                f64 val,
                                dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_wrap_c32(dsc_ctx *ctx,
                                c32 val,
                                dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_wrap_c64(dsc_ctx *ctx,
                                c64 val,
                                dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_arange(dsc_ctx *ctx,
                              int n,
                              dsc_dtype dtype = DSC_DEFAULT_TYPE,
                              dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_randn(dsc_ctx *ctx,
                             int n_dim,
                             const int *shape,
                             dsc_dtype dtype = DSC_DEFAULT_TYPE,
                             dsc_device_type device = DEFAULT);

extern dsc_tensor *dsc_cast(dsc_ctx *ctx,
                            dsc_tensor *DSC_RESTRICT x,
                            dsc_dtype new_dtype);

extern dsc_tensor *dsc_to(dsc_ctx *ctx,
                          dsc_tensor *DSC_RESTRICT x,
                          dsc_device_type new_device = DEFAULT);

extern dsc_tensor *dsc_reshape(dsc_ctx *ctx,
                               const dsc_tensor *DSC_RESTRICT x,
                               int dimensions...);

extern dsc_tensor *dsc_concat(dsc_ctx *ctx,
                              int axis,
                              int tensors...);

extern dsc_tensor *dsc_transpose(dsc_ctx *ctx,
                                 const dsc_tensor *DSC_RESTRICT x,
                                 int axes...);

// ============================================================
// Indexing and Slicing
//
// All indexing and slicing operations will return a new tensor.
// If the number of indexes passed to dsc_tensor_get_idx is equal to the number of
// dimensions of x then a new tensor will be allocated with a single element,
// the caller must take care of unwrapping it if needed.
extern dsc_tensor *dsc_tensor_get_idx(dsc_ctx *ctx,
                                      const dsc_tensor *DSC_RESTRICT x,
                                      int indexes...);

extern dsc_tensor *dsc_tensor_get_slice(dsc_ctx *ctx,
                                        const dsc_tensor *DSC_RESTRICT x,
                                        int slices...);

extern void dsc_tensor_set_idx(dsc_ctx *,
                               dsc_tensor *DSC_RESTRICT xa,
                               const dsc_tensor *DSC_RESTRICT xb,
                               int indexes...);

extern void dsc_tensor_set_slice(dsc_ctx *,
                                 dsc_tensor *DSC_RESTRICT xa,
                                 const dsc_tensor *DSC_RESTRICT xb,
                                 int slices...);

// ============================================================
// Binary Operations

extern dsc_tensor *dsc_add(dsc_ctx *ctx,
                           dsc_tensor *xa,
                           dsc_tensor *xb,
                           dsc_tensor *out = nullptr);

extern dsc_tensor *dsc_sub(dsc_ctx *ctx,
                           dsc_tensor *xa,
                           dsc_tensor *xb,
                           dsc_tensor *out = nullptr);

extern dsc_tensor *dsc_mul(dsc_ctx *ctx,
                           dsc_tensor *xa,
                           dsc_tensor *xb,
                           dsc_tensor *out = nullptr);

extern dsc_tensor *dsc_div(dsc_ctx *ctx,
                           dsc_tensor *xa,
                           dsc_tensor *xb,
                           dsc_tensor *out = nullptr);

extern dsc_tensor *dsc_pow(dsc_ctx *ctx,
                           dsc_tensor *xa,
                           dsc_tensor *xb,
                           dsc_tensor *out = nullptr);

// ============================================================
// Unary Operations

extern dsc_tensor *dsc_cos(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_sin(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_sinc(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_logn(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_log2(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_log10(dsc_ctx *ctx,
                             const dsc_tensor *DSC_RESTRICT x,
                             dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_exp(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_sqrt(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_abs(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr);

extern dsc_tensor *dsc_angle(dsc_ctx *ctx,
                             const dsc_tensor *DSC_RESTRICT x);

// conj and real are NOP if the input is real meaning x will be returned as is.
extern dsc_tensor *dsc_conj(dsc_ctx *ctx,
                            dsc_tensor *DSC_RESTRICT x);

extern dsc_tensor *dsc_real(dsc_ctx *ctx,
                            dsc_tensor *DSC_RESTRICT x);

extern dsc_tensor *dsc_imag(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x);

// Modified Bessel function of the first kind order 0
extern dsc_tensor *dsc_i0(dsc_ctx *ctx,
                          const dsc_tensor *DSC_RESTRICT x);

// Use a single function for all the dtypes since complex comparison is done by just checking
// the real part. By setting min to -inf and max to +inf we can simply clip x with two comparisons:
// out = min(max(x, x_min), x_max).
extern dsc_tensor *dsc_clip(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr,
                            f64 x_min = dsc_inf<f64, false>(),
                            f64 x_max = dsc_inf<f64, true>());

// ============================================================
// Unary Operations Along Axis

extern dsc_tensor *dsc_sum(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr,
                           int axis = -1,
                           bool keep_dims = true);

extern dsc_tensor *dsc_mean(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr,
                            int axis = -1,
                            bool keep_dims = true);

extern dsc_tensor *dsc_max(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr,
                           int axis = -1,
                           bool keep_dims = true);

extern dsc_tensor *dsc_min(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr,
                           int axis = -1,
                           bool keep_dims = true);

// ============================================================
// Fourier Transforms
//
// FFTs are always performed out-of-place. If the out param is provided then
// it will be used to store the result otherwise a new tensor will be allocated.
// The axis parameter specifies over which dimension the FFT must be performed,
// if x is 1-dimensional it will be ignored.
// If n is not specified then the FFT will be assumed to have the same size as the
// selected dimension of x otherwise that dimension will be padded/cropped to the value of n
// before performing the FFT.
extern dsc_tensor *dsc_fft(dsc_ctx *ctx,
                           const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out = nullptr,
                           int n = -1,
                           int axis = -1);

extern dsc_tensor *dsc_ifft(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr,
                            int n = -1,
                            int axis = -1);

extern dsc_tensor *dsc_rfft(dsc_ctx *ctx,
                            const dsc_tensor *DSC_RESTRICT x,
                            dsc_tensor *DSC_RESTRICT out = nullptr,
                            int n = -1,
                            int axis = -1);

extern dsc_tensor *dsc_irfft(dsc_ctx *ctx,
                             const dsc_tensor *DSC_RESTRICT x,
                             dsc_tensor *DSC_RESTRICT out = nullptr,
                             int n = -1,
                             int axis = -1);

extern dsc_tensor *dsc_fftfreq(dsc_ctx *ctx,
                               int n,
                               f64 d = 1.,
                               dsc_dtype dtype = DSC_DEFAULT_TYPE);

extern dsc_tensor *dsc_rfftfreq(dsc_ctx *ctx,
                                int n,
                                f64 d = 1.,
                                dsc_dtype dtype = DSC_DEFAULT_TYPE);

#if defined(__cplusplus)
}
#endif
