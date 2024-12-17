// Copyright (c) 2024, Christian Gilli <christian.gilli@dspcraft.com>
// All rights reserved.
//
// This code is licensed under the terms of the 3-clause BSD license
// (https://opensource.org/license/bsd-3-clause).

#include "dsc.h"
#include "cpu/dsc_cpu.h"
#include "cuda/dsc_cuda.h"
#include "dsc_device.h"
#include "dsc_tracing.h"
#include <cstdarg> // va_xxx


#define DSC_SIMD_ALIGN ((int) 32)

// This needs to be a macro otherwise the pointer assignments to out, xa and xb
// would not work unless I pass them as pointers to pointers which is very ugly.
#define validate_binary_params()                                                               \
    DSC_ASSERT(xa != nullptr);                                                                 \
    DSC_ASSERT(xb != nullptr);                                                                 \
    DSC_ASSERT(can_broadcast(xa, xb));                                                         \
    DSC_ASSERT(xa->device == xb->device);                                                      \
                                                                                               \
    const int n_dim = DSC_MAX(xa->n_dim, xb->n_dim);                                           \
                                                                                               \
    int shape[DSC_MAX_DIMS];                                                                   \
    for (int i = 0; i < DSC_MAX_DIMS; ++i) shape[i] = DSC_MAX(xa->shape[i], xb->shape[i]);     \
                                                                                               \
    const dsc_dtype out_dtype = DSC_DTYPE_CONVERSION_TABLE[xa->dtype][xb->dtype];              \
                                                                                               \
    if (out == nullptr) {                                                                      \
        out = dsc_new_tensor(ctx, n_dim, &shape[DSC_MAX_DIMS - n_dim], out_dtype, xa->device); \
    } else {                                                                                   \
        DSC_ASSERT(out->dtype == out_dtype);                                                   \
        DSC_ASSERT(out->n_dim == n_dim);                                                       \
        DSC_ASSERT(memcmp(out->shape, shape, DSC_MAX_DIMS * sizeof(shape[0])) == 0);           \
        DSC_ASSERT(out->device == xa->device);                                                 \
    }                                                                                          \
    const dsc_tensor *xa__ = xa, *xb__ = xb;                                                   \
    xa = dsc_cast(ctx, xa, out_dtype);                                                         \
    xb = dsc_cast(ctx, xb, out_dtype)

#define cleanup_binary()                          \
    do {                                          \
        if (xa__ != xa) dsc_tensor_free(ctx, xa); \
        if (xb__ != xb) dsc_tensor_free(ctx, xb); \
    } while (0)

#define validate_unary_params()                                                                  \
    do {                                                                                         \
        DSC_ASSERT(x != nullptr);                                                                \
        if (out == nullptr) {                                                                    \
            out = dsc_new_like(ctx, x);                                                          \
        } else {                                                                                 \
            DSC_ASSERT(out->dtype == x->dtype);                                                  \
            DSC_ASSERT(out->n_dim == x->n_dim);                                                  \
            DSC_ASSERT(out->device == x->device);                                                \
            DSC_ASSERT(memcmp(out->shape, x->shape, DSC_MAX_DIMS * sizeof(out->shape[0])) == 0); \
        }                                                                                        \
    } while (0)

#define validate_reduce_params()                                                                    \
    do {                                                                                            \
        DSC_ASSERT(x != nullptr);                                                                   \
                                                                                                    \
        const int axis_idx = dsc_tensor_dim(x, axis);                                               \
        DSC_ASSERT(axis_idx < DSC_MAX_DIMS);                                                        \
                                                                                                    \
        int out_shape[DSC_MAX_DIMS];                                                                \
        int out_ndim = x->n_dim;                                                                    \
        if (keep_dims) {                                                                            \
            memcpy(out_shape, x->shape, DSC_MAX_DIMS * sizeof(*out_shape));                         \
            out_shape[axis_idx] = 1;                                                                \
        } else {                                                                                    \
            out_ndim--;                                                                             \
            const int out_offset = DSC_MAX_DIMS - out_ndim;                                         \
            memset(out_shape, 1, out_offset * sizeof(*out_shape));                                  \
            for (int x_idx = DSC_MAX_DIMS - x->n_dim, out_idx = 0; x_idx < DSC_MAX_DIMS; ++x_idx) { \
                if (x_idx == axis_idx)                                                              \
                    continue;                                                                       \
                                                                                                    \
                out_shape[out_offset + out_idx] = x->shape[x_idx];                                  \
                out_idx++;                                                                          \
            }                                                                                       \
        }                                                                                           \
                                                                                                    \
        if (out == nullptr) {                                                                       \
            out = dsc_new_tensor(ctx, out_ndim, &out_shape[DSC_MAX_DIMS - out_ndim], x->dtype);     \
        } else {                                                                                    \
            DSC_ASSERT(out->dtype == x->dtype);                                                     \
            DSC_ASSERT(out->n_dim == out_ndim);                                                     \
            DSC_ASSERT(memcmp(out->shape, out_shape, DSC_MAX_DIMS * sizeof(*out_shape)) == 0);      \
        }                                                                                           \
    } while (0)


#define DSC_GET_DEFAULT_DEVICE(CTX)                                    \
    dsc_device *dev = (CTX)->devices[(CTX)->default_device];           \
    do {                                                               \
        if (!dev)                                                      \
            DSC_LOG_FATAL("device %d is null", (CTX)->default_device); \
    } while (0)

// If DEV is DEFAULT use the system default setting otherwise use the specified device
#define DSC_GET_DEV_ID(CTX, DEV) (DEV) == DEFAULT ? (CTX)->default_device : (DEV)
#define DSC_GET_DEVICE(CTX, DEV)                                                      \
    dsc_device *dev = (CTX)->devices[(CTX)->device_lookup[DSC_GET_DEV_ID(CTX, DEV)]]; \
    do {                                                                              \
        if (!dev)                                                                     \
            DSC_LOG_FATAL("device %d is null", (CTX)->default_device);                \
    } while (0)

#define DSC_DISPATCH(device, func, ...)                                      \
    do {                                                                     \
        const dsc_device_type dev_id = DSC_GET_DEV_ID(ctx, device);          \
        DSC_GET_DEVICE(ctx, device);                                         \
        if (dev_id == CPU)                                                   \
            dsc_cpu_##func(dev, ##__VA_ARGS__);                              \
        else if (dev_id == CUDA)                                             \
            dsc_cuda_##func(dev, ##__VA_ARGS__);                             \
        else                                                                 \
            DSC_LOG_FATAL("cannot dispatch to unknown device %d", (device)); \
    } while (0)

#define dsc_tensor_invalid(PTR)     (PTR)->ne <= 0
#define dsc_tensor_set_invalid(PTR) (PTR)->ne = -1

struct dsc_ctx {
    dsc_device *devices[DSC_MAX_DEVICES];
    int device_lookup[DSC_MAX_DEVICES];
    dsc_tensor *tensors;
    dsc_device_type default_device;
};

// ============================================================
// Initialization

dsc_ctx *dsc_ctx_init(const usize mem_size) {
    DSC_ASSERT(mem_size > 0);

    dsc_ctx *ctx = (dsc_ctx *) calloc(1, sizeof(dsc_ctx));
    DSC_ASSERT(ctx != nullptr);

    ctx->default_device = DSC_DEFAULT_DEVICE;

    ctx->devices[0] = dsc_cpu_device(mem_size);
    ctx->device_lookup[CPU] = 0;

    // DSC supports a single CUDA device so for now the device with ID=1 will be the CUDA
    // device with the highest compute capabilities (if any).
    if (const int cuda_devices = dsc_cuda_devices(); cuda_devices > 0) {
        int max_compute = dsc_cuda_dev_capabilities(0);
        int max_dev = 0;
        for (int dev = 1; dev < cuda_devices; ++dev) {
            if (const int dev_compute = dsc_cuda_dev_capabilities(dev); dev_compute > max_compute) {
                max_compute = dev_compute;
                max_dev = dev;
            }
        }

        ctx->devices[1] = dsc_cuda_device(mem_size, max_dev);
        ctx->device_lookup[CUDA] = 1;
    }
    // Pre-allocate the tensor headers on the heap, this way we don't commit all the
    // memory upfront.
    ctx->tensors = (dsc_tensor *) calloc(DSC_MAX_OBJS, sizeof(dsc_tensor));

    dsc_internal_init_traces(DSC_MAX_TRACES);

    return ctx;
}

// static dsc_fft_plan *dsc_get_plan(dsc_ctx *ctx, const int n,
//                                   const dsc_fft_type fft_type,
//                                   const dsc_dtype dtype) {
//     dsc_dtype twd_dtype;
//     switch (dtype) {
//         case C32:
//         case F32:
//             twd_dtype = F32;
//             break;
//         case C64:
//         case F64:
//             twd_dtype = F64;
//             break;
//         DSC_INVALID_CASE("unknown dtype=%d", dtype);
//     }
//
//     DSC_GET_DEFAULT_DEVICE(ctx);
//
//     dsc_fft_plan *plan = nullptr;
//     for (int i = 0; i < DSC_MAX_FFT_PLANS; ++i) {
//         if (dsc_fft_plan *cached_plan = &dev->fft_plans[i]; cached_plan->fft_type != INVALID) {
//             if (cached_plan->n == n &&
//                 // Note: technically we could support complex FFTs of order N from real FFTs plans
//                 // but not the other way around because we need an extra set of twiddles in the RFFT.
//                 cached_plan->fft_type == fft_type &&
//                 cached_plan->dtype == twd_dtype) {
//                 plan = cached_plan;
//                 plan->last_used = 0;
//             } else {
//                 cached_plan->last_used++;
//             }
//         }
//     }
//
//     return plan;
// }
//
dsc_fft_plan *dsc_plan_fft(dsc_ctx *ctx, const int n,
                           const dsc_fft_type fft_type,
                           const dsc_dtype dtype) {
    return nullptr;
    // const int fft_n = dsc_pow2_n(n);
    //
    // DSC_TRACE_PLAN_FFT(n, fft_n, fft_type, dtype);
    //
    // dsc_fft_plan *plan = dsc_get_plan(ctx, fft_n, fft_type, dtype);
    //
    // DSC_GET_DEFAULT_DEVICE(ctx);
    //
    // if (plan == nullptr) {
    //     int free_slot = -1;
    //     for (int i = 0; i < DSC_MAX_FFT_PLANS; ++i) {
    //         if (dev->fft_plans[i].fft_type == INVALID) {
    //             free_slot = i;
    //             break;
    //         }
    //     }
    //
    //     if (free_slot < 0) {
    //         // Find the 'oldest' FFT plan and evict it
    //         int max = -1;
    //         for (int i = 0; i < DSC_MAX_FFT_PLANS; ++i) {
    //             if (const dsc_fft_plan *tmp = &dev->fft_plans[i]; tmp->last_used > max) {
    //                 max = tmp->last_used;
    //                 free_slot = i;
    //             }
    //         }
    //         dsc_data_free(dev, dev->fft_plans[free_slot].buf);
    //     }
    //     DSC_LOG_DEBUG("allocating new %s plan with N=%d dtype=%s",
    //                   fft_type == REAL ? "RFFT" : "FFT",
    //                   fft_n, DSC_DTYPE_NAMES[dtype]);
    //
    //     plan = &dev->fft_plans[free_slot];
    //
    //     plan->buf = dsc_data_alloc(dev, dsc_fft_storage(fft_n, dtype, fft_type));
    //     dsc_init_plan(plan, fft_n, dtype, fft_type);
    // } else {
    //     DSC_LOG_DEBUG("found cached %s plan with N=%d dtype=%s",
    //                   fft_type == REAL ? "RFFT" : "FFT",
    //                   fft_n, DSC_DTYPE_NAMES[dtype]);
    // }
    //
    // return plan;
}

// ============================================================
// Cleanup/Teardown

void dsc_ctx_free(dsc_ctx *ctx) {
    for (int i = 0; i < DSC_MAX_DEVICES; ++i) {
        if (dsc_device *dev = ctx->devices[i]; dev) {
            dev->dispose(dev);
        }
    }

    dsc_internal_free_traces();

    free(ctx->tensors);
    free(ctx);
}

void dsc_tensor_free(dsc_ctx *ctx, dsc_tensor *x) {
    if (x == nullptr) return;
    // DSC_TRACE_TENSOR_FREE(x);

    DSC_GET_DEVICE(ctx, x->device);

    dsc_data_free(dev, x->buf);

    dsc_tensor_set_invalid(x);
}

// ============================================================
// Utilities

usize dsc_used_mem(dsc_ctx *ctx) {
    return ctx->devices[ctx->default_device]->used_mem;
}

void dsc_print_mem_usage(dsc_ctx *ctx) {
    printf("DSC mem usage:");
    for (int i = 0; i < DSC_MAX_DEVICES; ++i) {
        if (const dsc_device *dev = ctx->devices[i]; dev) {
            printf("\n %s: %ld/%ld (%.1f%%)",
                   DSC_DEVICE_NAMES[dev->type],
                   (usize) DSC_B_TO_MB(dev->used_mem),
                   (usize) DSC_B_TO_MB(dev->mem_size),
                   (f64) dev->used_mem / (f64) dev->mem_size * 1e2);
        }
    }
    printf("\n");
}

void dsc_set_default_device(dsc_ctx *ctx,
                            const dsc_device_type device) {
    // Passing DEFAULT here restores the system settings
    ctx->default_device = device == DEFAULT ? DSC_DEFAULT_DEVICE : device;
}

// ============================================================
// Tracing

void dsc_traces_record(dsc_ctx *, const bool record) {
    dsc_internal_record_traces(record);
}

void dsc_dump_traces(dsc_ctx *, const char *filename) {
    dsc_internal_dump_traces(filename);
}

void dsc_clear_traces(dsc_ctx *) {
    dsc_internal_clear_traces();
}

// ============================================================
// Tensor Creation

static DSC_INLINE dsc_tensor *find_empty_tensor(dsc_ctx *ctx) {
    for (int i = 0; i < DSC_MAX_OBJS; ++i) {
        if (dsc_tensor *x = &ctx->tensors[i]; dsc_tensor_invalid(x)) {
            return x;
        }
    }
    return nullptr;
}

dsc_tensor *dsc_new_tensor(dsc_ctx *ctx,
                           const int n_dim,
                           const int *shape,
                           const dsc_dtype dtype,
                           const dsc_device_type device,
                           dsc_data_buffer *buf) {
    DSC_ASSERT((unsigned) n_dim <= DSC_MAX_DIMS);

    DSC_GET_DEVICE(ctx, device);

    // DSC_TRACE_TENSOR_NEW(shape, n_dim, dtype, backend);

    int ne = 1;
    for (int i = 0; i < n_dim; ++i) ne *= shape[i];

    dsc_tensor *new_tensor = find_empty_tensor(ctx);
    DSC_ASSERT(new_tensor != nullptr);

    if (buf == nullptr) {
        // Fixme: SIMD_ALIGN is useless on cuda...
        new_tensor->buf = dsc_data_alloc(dev, ne * DSC_DTYPE_SIZE[dtype] + DSC_SIMD_ALIGN);
    } else {
        new_tensor->buf = buf;
        new_tensor->buf->refs++;
    }

    new_tensor->dtype = dtype;
    new_tensor->ne = ne;
    new_tensor->n_dim = n_dim;
    new_tensor->device = DSC_GET_DEV_ID(ctx, device);

    // If n_dim is lower than DSC_MAX_DIM then we need to pre-fill the beginning of the array with 1
    for (int i = 0; i < DSC_MAX_DIMS; ++i) {
        new_tensor->shape[i] = i < (DSC_MAX_DIMS - n_dim) ? 1 : shape[i - (DSC_MAX_DIMS - n_dim)];
    }

    // Compute the stride
    memset(new_tensor->stride, 0, DSC_MAX_DIMS * sizeof(int));
    // Todo: stride as number of bytes?
    new_tensor->stride[DSC_MAX_DIMS - 1] = 1;
    for (int i = DSC_MAX_DIMS - 2; i >= 0; --i) {
        new_tensor->stride[i] = new_tensor->stride[i + 1] * new_tensor->shape[i + 1];
    }

    DSC_LOG_DEBUG("new tensor ptr=%p backend=%s n_dim=%d shape=[%d, %d, %d, %d] stride=[%d, %d, %d, %d] dtype=%s buffer=%p refs=%d",
                  new_tensor, DSC_DEVICE_NAMES[new_tensor->device], n_dim,
                  new_tensor->shape[0], new_tensor->shape[1], new_tensor->shape[2], new_tensor->shape[3],
                  new_tensor->stride[0], new_tensor->stride[1], new_tensor->stride[2], new_tensor->stride[3],
                  DSC_DTYPE_NAMES[dtype], new_tensor->buf, new_tensor->buf->refs
    );

    return new_tensor;
}

dsc_tensor *dsc_view(dsc_ctx *ctx, const dsc_tensor *x) {
    return dsc_new_view(ctx, x);
}

dsc_tensor *dsc_tensor_1d(dsc_ctx *ctx, const dsc_dtype dtype,
                          const int dim1, const dsc_device_type device) {
    const int shape[DSC_MAX_DIMS] = {dim1};
    return dsc_new_tensor(ctx, 1, shape, dtype, device);
}

dsc_tensor *dsc_tensor_2d(dsc_ctx *ctx, const dsc_dtype dtype,
                          const int dim1, const int dim2,
                          const dsc_device_type device) {
    const int shape[DSC_MAX_DIMS] = {dim1, dim2};
    return dsc_new_tensor(ctx, 2, shape, dtype, device);
}

dsc_tensor *dsc_tensor_3d(dsc_ctx *ctx, const dsc_dtype dtype,
                          const int dim1, const int dim2,
                          const int dim3,
                          const dsc_device_type device) {
    const int shape[DSC_MAX_DIMS] = {dim1, dim2, dim3};
    return dsc_new_tensor(ctx, 3, shape, dtype, device);
}

dsc_tensor *dsc_tensor_4d(dsc_ctx *ctx, const dsc_dtype dtype,
                          const int dim1, const int dim2,
                          const int dim3, const int dim4,
                          const dsc_device_type device) {
    const int shape[DSC_MAX_DIMS] = {dim1, dim2, dim3, dim4};
    return dsc_new_tensor(ctx, 4, shape, dtype, device);
}

dsc_tensor *dsc_wrap_f32(dsc_ctx *ctx, const f32 val,
                         const dsc_device_type device) {
    dsc_tensor *out = dsc_tensor_1d(ctx, F32, 1, device);

    DSC_GET_DEVICE(ctx, device);
    dev->memcpy(out->buf->data, &val, sizeof(val), TO_DEVICE);
    return out;
}

dsc_tensor *dsc_wrap_f64(dsc_ctx *ctx, const f64 val,
                         const dsc_device_type device) {
    dsc_tensor *out = dsc_tensor_1d(ctx, F64, 1, device);

    DSC_GET_DEVICE(ctx, device);
    dev->memcpy(out->buf->data, &val, sizeof(val), TO_DEVICE);
    return out;
}

dsc_tensor *dsc_wrap_c32(dsc_ctx *ctx, const c32 val,
                         const dsc_device_type device) {
    dsc_tensor *out = dsc_tensor_1d(ctx, C32, 1, device);

    DSC_GET_DEVICE(ctx, device);
    dev->memcpy(out->buf->data, &val, sizeof(val), TO_DEVICE);
    return out;
}

dsc_tensor *dsc_wrap_c64(dsc_ctx *ctx, const c64 val,
                         const dsc_device_type device) {
    dsc_tensor *out = dsc_tensor_1d(ctx, C64, 1, device);

    DSC_GET_DEVICE(ctx, device);
    dev->memcpy(out->buf->data, &val, sizeof(val), TO_DEVICE);
    return out;
}

dsc_tensor *dsc_arange(dsc_ctx *ctx,
                       const int n,
                       const dsc_dtype dtype,
                       const dsc_device_type device) {
    // DSC_TRACE_ARANGE_OP(n, dtype);

    dsc_tensor *out = dsc_tensor_1d(ctx, dtype, n, device);
    DSC_DISPATCH(device, arange, out);
    return out;
}

dsc_tensor *dsc_randn(dsc_ctx *ctx,
                      const int n_dim,
                      const int *shape,
                      const dsc_dtype dtype,
                      const dsc_device_type device) {
    // DSC_TRACE_RANDN_OP(shape, n_dim, dtype);

    dsc_tensor *out = dsc_new_tensor(ctx, n_dim, shape, dtype, device);
    DSC_DISPATCH(device, randn, out);
    return out;
}

dsc_tensor *dsc_cast(dsc_ctx *ctx, dsc_tensor *DSC_RESTRICT x,
                     const dsc_dtype new_dtype) {
    // DSC_TRACE_CAST_OP(x, new_dtype);

    if (x->dtype == new_dtype) return x;

    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim,
                                     &x->shape[dsc_tensor_dim(x, 0)],
                                     new_dtype, x->device);

    DSC_DISPATCH(x->device, cast, x, out);

    return out;
}

dsc_tensor *dsc_to(dsc_ctx *ctx, dsc_tensor *DSC_RESTRICT x,
                   const dsc_device_type new_device) {
    if (x->device == new_device) return x;

    if (x->device == CUDA) dsc_cuda_sync();
    // Note: if we need to guard against CUDA this will not be possible, this functionality
    // must either become a void macro or this must be a function of the device
    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim,
                                     &x->shape[dsc_tensor_dim(x, 0)],
                                     x->dtype, new_device);

    if (x->device == CUDA) {
        DSC_GET_DEVICE(ctx, CUDA);
        dev->memcpy(out->buf->data, x->buf->data,
                    x->ne * DSC_DTYPE_SIZE[x->dtype], FROM_DEVICE);
    } else if (new_device == CUDA) {
        DSC_GET_DEVICE(ctx, CUDA);
        dev->memcpy(out->buf->data, x->buf->data,
                    x->ne * DSC_DTYPE_SIZE[x->dtype], TO_DEVICE);
    } else {
        DSC_GET_DEVICE(ctx, new_device);
        dev->memcpy(out->buf->data, x->buf->data,
                    x->ne * DSC_DTYPE_SIZE[x->dtype], ON_DEVICE);
    }
    return out;
}


dsc_tensor *dsc_reshape(dsc_ctx *ctx,
                        const dsc_tensor *DSC_RESTRICT x,
                        const int dimensions...) {
    DSC_ASSERT((unsigned) dimensions <= DSC_MAX_DIMS);

    int new_shape[DSC_MAX_DIMS];
    int new_ne = 1;
    int unknown_dim = -1;

    std::va_list args;
    va_start(args, dimensions);
    for (int i = 0; i < dimensions; ++i) {
        const int el = va_arg(args, int);

        if (el < 0) {
            if (unknown_dim == -1) unknown_dim = i;
            else DSC_LOG_FATAL("can only specify one unknown dim");
        } else {
            new_ne *= el;
            new_shape[i] = el;
        }

    }
    va_end(args);

    if (unknown_dim != -1) {
        if (x->ne % new_ne != 0) DSC_LOG_FATAL("cannot reshape %d into %d with an unknown dimension", x->ne, new_ne);

        new_shape[unknown_dim] = x->ne / new_ne;
        new_ne = x->ne;
    }

    // DSC_TRACE_RESHAPE_OP(x, dimensions, new_shape);

    DSC_ASSERT(x->ne == new_ne);

    return dsc_new_tensor(ctx, dimensions, new_shape, x->dtype, x->device, x->buf);
}

template<typename T>
static DSC_INLINE void concat(dsc_tensor **to_concat,
                              const int tensors,
                              dsc_tensor *DSC_RESTRICT out,
                              const int axis_idx) {
    // DSC_TENSOR_DATA(T, out);
    // Todo: validate the perf of this implementation
    // dsc_axis_iterator *iterators = (dsc_axis_iterator *) alloca(tensors * sizeof(dsc_axis_iterator));
    // for (int i = 0; i < tensors; ++i) iterators[i] = dsc_axis_iterator(to_concat[i], axis_idx);
    //
    // dsc_axis_iterator out_iterator(out, axis_idx);
    //
    // while (out_iterator.has_next()) {
    //     for (int i = 0; i < tensors; ++i) {
    //         const int axis_n = to_concat[i]->shape[axis_idx];
    //
    //         T *DSC_RESTRICT src_data = (T *) to_concat[i]->data;
    //         for (int el_idx = 0; el_idx < axis_n; ++el_idx) {
    //             int index = iterators[i].index(); // Index is the same on GPU and CPU
    //             out_data[out_iterator.index()] = src_data[index]; // This doesn't work on GPU!
    //
    //             out_iterator.next();
    //             iterators[i].next();
    //         }
    //     }
    // }
}

dsc_tensor *dsc_concat(dsc_ctx *ctx, const int axis,
                       const int tensors...) {
    DSC_ASSERT(tensors > 1);

    // DSC_TRACE_CONCAT_OP(tensors, axis);

    dsc_tensor **to_concat = (dsc_tensor **) alloca(tensors * sizeof(dsc_tensor *));
    std::va_list args;
    va_start(args, tensors);
    for (int i = 0; i < tensors; ++i) {
        dsc_tensor *el = va_arg(args, dsc_tensor *);
        DSC_ASSERT(el != nullptr);

        to_concat[i] = el;
    }
    va_end(args);

    // All the tensors must have the same dtype and the same number of dimensions and be on the same device
    const dsc_dtype dtype = to_concat[0]->dtype;
    const int n_dim = to_concat[0]->n_dim;
    const dsc_device_type device = to_concat[0]->device;
    for (int i = 1; i < tensors; ++i) {
        DSC_ASSERT(to_concat[i]->dtype == dtype);
        DSC_ASSERT(to_concat[i]->n_dim == n_dim);
        DSC_ASSERT(to_concat[i]->device == device);
    }

    DSC_GET_DEVICE(ctx, device);
    if (axis == DSC_VALUE_NONE) {
        // Flatten
        int ne = 0;
        for (int i = 0; i < tensors; ++i) ne += to_concat[i]->ne;

        dsc_tensor *out = dsc_tensor_1d(ctx, dtype, ne);
        usize offset = 0;
        for (int i = 0; i < tensors; ++i) {
            const dsc_tensor *src = to_concat[i];
            const usize nb = src->ne * DSC_DTYPE_SIZE[dtype];
            dev->memcpy((byte *) out->buf->data + offset, src->buf->data, nb, ON_DEVICE);
            offset += nb;
        }

        return out;
    }

    const int axis_idx = dsc_tensor_dim(to_concat[0], axis);
    DSC_ASSERT(axis_idx < DSC_MAX_DIMS);

    int resulting_shape[DSC_MAX_DIMS];
    memcpy(resulting_shape, to_concat[0]->shape, DSC_MAX_DIMS * sizeof(*resulting_shape));

    // All the tensors must have the same shape expect for the axis dimension
    for (int i = 1; i < tensors; ++i) {
        for (int idx = 0; idx < DSC_MAX_DIMS; ++idx) {
            if (idx == axis_idx) {
                resulting_shape[axis_idx] += to_concat[i]->shape[idx];
                continue;
            }

            DSC_ASSERT(to_concat[i]->shape[idx] == to_concat[0]->shape[idx]);
        }
    }

    dsc_tensor *out = dsc_new_tensor(ctx, n_dim,
                                     &resulting_shape[dsc_tensor_dim(to_concat[0], 0)],
                                     dtype, device);

    switch (dtype) {
        case F32:
            concat<f32>(to_concat, tensors, out, axis_idx);
            break;
        case F64:
            concat<f64>(to_concat, tensors, out, axis_idx);
            break;
        case C32:
            concat<c32>(to_concat, tensors, out, axis_idx);
            break;
        case C64:
            concat<c64>(to_concat, tensors, out, axis_idx);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", dtype);
    }

    return out;
}

template <typename T>
static DSC_INLINE void copy_with_stride(const dsc_tensor *DSC_RESTRICT x,
                                        dsc_tensor *DSC_RESTRICT out,
                                        const int *shape,
                                        const int *stride) {
    // Todo: it's probably better to have a copy that takes care of this?
    // DSC_TENSOR_DATA(T, x);
    // DSC_TENSOR_DATA(T, out);
    //
    // dsc_stride_iterator x_it(shape, stride);
    // dsc_for(i, out) {
    //     out_data[i] = x_data[x_it.index()];
    //     x_it.next();
    // }
}

dsc_tensor *dsc_transpose(dsc_ctx *ctx,
                          const dsc_tensor *DSC_RESTRICT x,
                          const int axes...) {
    // Transpose the given axes of tensor x.
    // If axes are not given (0) reverse the order of the axes of x.
    DSC_ASSERT(x != nullptr);

    if (x->n_dim == 1) {
        // Return a view of the same vector since a transpose is a NOP in this case
        return dsc_new_view(ctx, x);
    }

    int swap_axes[DSC_MAX_DIMS];
    if (axes == 0) {
        // [0, 1, .., N-1] --> [N-1, .., 1, 0]
        for (int i = 0; i < x->n_dim; ++i) swap_axes[i] = x->n_dim - (i + 1);
    } else {
        DSC_ASSERT(axes == x->n_dim);
        std::va_list args;
        va_start(args, axes);
        for (int i = 0; i < axes; ++i) {
            const int el = va_arg(args, int);
            DSC_ASSERT((unsigned) el < DSC_MAX_DIMS);
            swap_axes[i] = el;
        }
        va_end(args);
    }
    DSC_TRACE_TRANSPOSE_OP(x, swap_axes);

    int swapped_shape[DSC_MAX_DIMS], swapped_stride[DSC_MAX_DIMS];
    for (int i = 0; i < DSC_MAX_DIMS - x->n_dim; ++i) {
        // Fixme: useless??
        swapped_shape[i] = x->shape[i];
        swapped_stride[i] = x->stride[i];
    }

    for (int i = 0; i < x->n_dim; ++i) {
        const int idx = dsc_tensor_dim(x, swap_axes[i]);
        swapped_shape[dsc_tensor_dim(x, i)] = x->shape[idx];
        swapped_stride[dsc_tensor_dim(x, i)] = x->stride[idx];
    }

    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim,
                                     &swapped_shape[dsc_tensor_dim(x, 0)],
                                     x->dtype);

    switch (x->dtype) {
        case F32:
            copy_with_stride<f32>(x, out, swapped_shape, swapped_stride);
            break;
        case F64:
            copy_with_stride<f64>(x, out, swapped_shape, swapped_stride);
            break;
        case C32:
            copy_with_stride<c32>(x, out, swapped_shape, swapped_stride);
            break;
        case C64:
            copy_with_stride<c64>(x, out, swapped_shape, swapped_stride);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", x->dtype);
    }

    return out;
}

// ============================================================
// Indexing and Slicing
//

dsc_tensor *dsc_tensor_get_idx(dsc_ctx *ctx,
                               const dsc_tensor *DSC_RESTRICT x,
                               const int indexes...) {
    // DSC_ASSERT(x != nullptr);
    // DSC_ASSERT((unsigned) indexes <= DSC_MAX_DIMS);
    //
    // if (indexes > x->n_dim) {
    //     DSC_LOG_FATAL("too many indexes");
    // }
    //
    // int el_idx[DSC_MAX_DIMS];
    //
    // std::va_list args;
    // va_start(args, indexes);
    // for (int i = 0; i < indexes; ++i) {
    //     int idx = va_arg(args, int);
    //     const int x_dim_i = x->shape[dsc_tensor_dim(x, i)];
    //     // Negative indexes mean accessing from the end
    //     if (idx < 0) idx += x_dim_i;
    //
    //     DSC_ASSERT((unsigned) idx < (unsigned) x_dim_i);
    //
    //     el_idx[i] = idx;
    // }
    // va_end(args);
    //
    // DSC_TRACE_GET_IDX(x, el_idx, indexes);
    //
    // // Since we are wrapping scalars the resulting tensor will be always at least 1D
    // const int out_n_dim = x->n_dim == indexes ? 1 : x->n_dim - indexes;
    // // If we are indexing a single element then of course the output shape will be just 1
    // int out_shape[DSC_MAX_DIMS] = {1};
    // if (x->n_dim > indexes) {
    //     memcpy(out_shape, &x->shape[DSC_MAX_DIMS - out_n_dim], out_n_dim * sizeof(*x->shape));
    // }
    //
    // dsc_tensor *out = dsc_new_tensor(ctx, out_n_dim, out_shape, x->dtype);
    //
    // int offset = 0;
    // for (int i = 0; i < indexes; ++i) {
    //     offset += (x->stride[dsc_tensor_dim(x, i)] * el_idx[i]);
    // }
    // const int stride = x->stride[dsc_tensor_dim(x, (indexes - 1))];
    //
    // memcpy(out->data, ((byte *) x->data) + (offset * DSC_DTYPE_SIZE[x->dtype]), stride * DSC_DTYPE_SIZE[x->dtype]);
    //
    // return out;
    return nullptr;
}

template <typename T>
static DSC_INLINE void copy_slice(const dsc_tensor *DSC_RESTRICT x,
                                  dsc_tensor *DSC_RESTRICT out,
                                  const int n_slices,
                                  dsc_slice *slices) {
    // DSC_TENSOR_DATA(T, out);
    // DSC_TENSOR_DATA(T, x);
    // dsc_slice_iterator x_it(x, n_slices, slices);
    //
    // dsc_for(i, out) {
    //     out_data[i] = x_data[x_it.index()];
    //
    //     x_it.next();
    // }
}

static DSC_INLINE void parse_slices(const dsc_tensor *DSC_RESTRICT x,
                                    dsc_slice *parsed_slices,
                                    bool *collapse_dim,
                                    const int slices,
                                    std::va_list args) {
    for (int i = 0; i < slices; ++i) {
        dsc_slice slice = va_arg(args, dsc_slice);
        const int x_dim_i = x->shape[dsc_tensor_dim(x, i)];

        // The convention is to set all fields in the slice to the same value != NONE to signal
        // access to a single index rather than a slice (happens in mixed scenarios like x[:, 1])
        if (slice.start == slice.stop &&
            slice.start == slice.step &&
            slice.start != DSC_VALUE_NONE) {
            // If we need to return a tensor then we need to keep track of the dimensions that must
            // be collapsed to match NumPy behaviour
            if (collapse_dim != nullptr) collapse_dim[i] = true;
            slice.step = 1;
            if (slice.start < 0) {
                slice.start += x_dim_i;
                slice.stop += x_dim_i + 1;
            } else {
                slice.stop += 1;
            }
        }

        DSC_ASSERT(slice.step != 0);

        // If a field is marked using DSC_VALUE_NONE then replace it with the 'default' behaviour.
        // The default behaviour is controlled by step (see: https://numpy.org/doc/stable/user/basics.indexing.html)
        if (slice.step == DSC_VALUE_NONE) slice.step = 1;
        if (slice.start == DSC_VALUE_NONE) {
            if (slice.step > 0) slice.start = 0;
            else slice.start = x_dim_i - 1;
        }
        if (slice.stop == DSC_VALUE_NONE) {
            if (slice.step > 0) slice.stop = x_dim_i;
            else slice.stop = -x_dim_i - 1;
        }

        if (slice.start < 0) slice.start += x_dim_i;
        if (slice.stop < 0) slice.stop += x_dim_i;

        DSC_ASSERT(abs(slice.stop - slice.start) <= x_dim_i);
        DSC_ASSERT((slice.step > 0 && slice.start < slice.stop) || (slice.step < 0 && slice.start > slice.stop));

        DSC_ASSERT(abs(slice.step) <= x_dim_i);

        parsed_slices[i] = slice;
    }
}

dsc_tensor *dsc_tensor_get_slice(dsc_ctx *ctx,
                                 const dsc_tensor *DSC_RESTRICT x,
                                 const int slices...) {
    DSC_ASSERT(x != nullptr);
    DSC_ASSERT((unsigned) slices <= DSC_MAX_DIMS);

    if (slices > x->n_dim) {
        DSC_LOG_FATAL("too many slices");
    }

    dsc_slice el_slices[DSC_MAX_DIMS];
    bool collapse_dim[DSC_MAX_DIMS] = {false};

    std::va_list args;
    va_start(args, slices);
    parse_slices(x, el_slices, collapse_dim, slices, args);
    va_end(args);

    DSC_TRACE_GET_SLICE(x, el_slices, slices);

    int out_shape[DSC_MAX_DIMS];
    int out_n_dim = x->n_dim;
    for (int i = 0, out_idx = 0; i < x->n_dim; ++i) {
        if (i < slices) {
            if (collapse_dim[i]) {
                out_n_dim -= 1;
                continue;
            }
            const dsc_slice slice_i = el_slices[i];
            const int ne_i = abs(slice_i.stop - slice_i.start);
            const int abs_step = abs(slice_i.step);
            out_shape[out_idx] = (ne_i + abs_step - 1) / abs_step;
        } else {
            out_shape[out_idx] = x->shape[dsc_tensor_dim(x, i)];
        }
        out_idx += 1;
    }

    dsc_tensor *out = dsc_new_tensor(ctx, out_n_dim, out_shape, x->dtype);

    switch (out->dtype) {
        case F32:
            copy_slice<f32>(x, out, slices, el_slices);
            break;
        case F64:
            copy_slice<f64>(x, out, slices, el_slices);
            break;
        case C32:
            copy_slice<c32>(x, out, slices, el_slices);
            break;
        case C64:
            copy_slice<c64>(x, out, slices, el_slices);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", out->dtype);
    }

    return out;
}

template <typename T>
static DSC_INLINE void tensor_set(dsc_tensor *DSC_RESTRICT xa,
                                  const bool xa_scalar,
                                  const dsc_tensor *DSC_RESTRICT xb,
                                  const int n_slices,
                                  const dsc_slice *slices) {
    // DSC_TENSOR_DATA(T, xa);
    // DSC_TENSOR_DATA(T, xb);
    // // Todo: macro is_scalar
    // if (xa_scalar) {
    //     int offset = 0;
    //     for (int i = 0; i < n_slices; ++i)
    //         offset += (slices[i].start * xa->stride[dsc_tensor_dim(xa, i)]);
    //
    //     xa_data[offset] = xb_data[0];
    // } else if (xb->n_dim == 1 && xb->shape[dsc_tensor_dim(xb, -1)] == 1) {
    //     const T el = xb_data[0];
    //
    //     for (dsc_slice_iterator xa_it(xa, n_slices, slices);
    //          xa_it.has_next();
    //          xa_it.next()) {
    //         xa_data[xa_it.index()] = el;
    //     }
    // } else {
    //     int xb_idx = 0;
    //     for (dsc_slice_iterator xa_it(xa, n_slices, slices);
    //          xa_it.has_next();
    //          xa_it.next()) {
    //         xa_data[xa_it.index()] = xb_data[xb_idx];
    //
    //         xb_idx = (xb_idx + 1) % xb->ne;
    //     }
    // }
}

void dsc_tensor_set_idx(dsc_ctx *,
                        dsc_tensor *DSC_RESTRICT xa,
                        const dsc_tensor *DSC_RESTRICT xb,
                        const int indexes...) {
    DSC_ASSERT(xa != nullptr);
    DSC_ASSERT(xb != nullptr);
    DSC_ASSERT((unsigned) indexes <= (unsigned) xa->n_dim);
    DSC_ASSERT(xa->dtype == xb->dtype);

    // Use slices so it's easier to iterate
    dsc_slice el_slices[DSC_MAX_DIMS];

    std::va_list args;
    va_start(args, indexes);
    for (int i = 0; i < indexes; ++i) {
        const int idx = va_arg(args, int);
        const int x_dim_i = xa->shape[dsc_tensor_dim(xa, i)];

        el_slices[i].start = idx;
        el_slices[i].stop = idx + 1;
        el_slices[i].step = 1;
        if (idx < 0) {
            el_slices[i].start += x_dim_i;
            el_slices[i].stop += x_dim_i;
        }
    }
    va_end(args);

    DSC_TRACE_SET_IDX(xa, xb, el_slices, indexes);

    // If we do something like xa[2] and xa has more than one dimension then, the remaining
    // dimensions of xa and xb must be broadcastable together
    int xa_sub_shape[DSC_MAX_DIMS];
    for (int i = indexes; i < xa->n_dim; ++i)
        xa_sub_shape[i - indexes] = xa->shape[dsc_tensor_dim(xa, i - indexes)];

    const bool xb_scalar = xb->n_dim == 1 && xb->shape[dsc_tensor_dim(xb, -1)] == 1;
    const int xa_sub_ndim = xa->n_dim - indexes;

    if (xa_sub_ndim == 0) DSC_ASSERT(xb_scalar);

    if (!xb_scalar) {
        // If xb is not a scalar then its shape must be compatible with xa_sub_shape
        DSC_ASSERT(xb->n_dim == xa_sub_ndim);
        for (int i = 0; i < xa_sub_ndim; ++i) DSC_ASSERT(xa_sub_shape[i] == xb->shape[dsc_tensor_dim(xb, i)]);
    }

    switch (xa->dtype) {
        case F32:
            tensor_set<f32>(xa, xa_sub_ndim == 0, xb, indexes, el_slices);
            break;
        case F64:
            tensor_set<f64>(xa, xa_sub_ndim == 0, xb, indexes, el_slices);
            break;
        case C32:
            tensor_set<c32>(xa, xa_sub_ndim == 0, xb, indexes, el_slices);
            break;
        case C64:
            tensor_set<c64>(xa, xa_sub_ndim == 0, xb, indexes, el_slices);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", xa->dtype);
    }
}

void dsc_tensor_set_slice(dsc_ctx *,
                          dsc_tensor *DSC_RESTRICT xa,
                          const dsc_tensor *DSC_RESTRICT xb,
                          const int slices...) {
    DSC_ASSERT(xa != nullptr);
    DSC_ASSERT(xb != nullptr);
    DSC_ASSERT((unsigned) slices <= (unsigned) xa->n_dim);
    DSC_ASSERT(xa->dtype == xb->dtype);

    dsc_slice el_slices[DSC_MAX_DIMS];

    std::va_list args;
    va_start(args, slices);
    parse_slices(xa, el_slices, nullptr, slices, args);
    va_end(args);

    DSC_TRACE_SET_SLICE(xa, xb, el_slices, slices);

    int xa_slice_shape[DSC_MAX_DIMS];
    for (int i = 0; i < xa->n_dim; ++i) {
        if (i < slices) {
            const dsc_slice slice_i = el_slices[i];
            const int ne_i = abs(slice_i.stop - slice_i.start);
            const int abs_step = abs(slice_i.step);
            xa_slice_shape[i] = (ne_i + abs_step - 1) / abs_step;
        } else {
            xa_slice_shape[i] = xa->shape[dsc_tensor_dim(xa, i)];
        }
    }

    const bool xb_scalar = xb->n_dim == 1 && xb->shape[dsc_tensor_dim(xb, -1)] == 1;

    if (!xb_scalar) {
        // Check whether xb is broadcastable with xa
        const int dims_to_compare = DSC_MIN(xa->n_dim, xb->n_dim);
        for (int i = 0; i < dims_to_compare; ++i) {
            const int xb_dim_i = xb->shape[dsc_tensor_dim(xb, i)];
            const int xa_slice_i = xa_slice_shape[i];
            DSC_ASSERT(xa_slice_i == 1 || xb_dim_i == 1 || xa_slice_i == xb_dim_i);
        }
    }

    bool xa_scalar = true;
    for (int i = 0; i < xa->n_dim && xa_scalar; ++i)
        xa_scalar &= xa_slice_shape[i] == 1;

    switch (xa->dtype) {
        case F32:
            tensor_set<f32>(xa, xa_scalar, xb, slices, el_slices);
            break;
        case F64:
            tensor_set<f64>(xa, xa_scalar, xb, slices, el_slices);
            break;
        case C32:
            tensor_set<c32>(xa, xa_scalar, xb, slices, el_slices);
            break;
        case C64:
            tensor_set<c64>(xa, xa_scalar, xb, slices, el_slices);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", xa->dtype);
    }
}

// ============================================================
// Binary Operations

static bool DSC_INLINE DSC_PURE can_broadcast(const dsc_tensor *DSC_RESTRICT xa,
                                              const dsc_tensor *DSC_RESTRICT xb) {
    bool can_broadcast = true;
    for (int i = 0; i < DSC_MAX_DIMS && can_broadcast; ++i) {
        can_broadcast = xa->shape[i] == xb->shape[i] ||
                        xa->shape[i] == 1 ||
                        xb->shape[i] == 1;
    }

    return can_broadcast;
}

dsc_tensor *dsc_add(dsc_ctx *ctx,
                    dsc_tensor *xa,
                    dsc_tensor *xb,
                    dsc_tensor *out) {
    // DSC_TRACE_BINARY_OP(xa, xb, out);

    validate_binary_params();

    DSC_DISPATCH(xa->device, add, xa, xb, out);

    cleanup_binary();

    return out;
}

dsc_tensor *dsc_sub(dsc_ctx *ctx,
                    dsc_tensor *xa,
                    dsc_tensor *xb,
                    dsc_tensor *out) {
    // DSC_TRACE_BINARY_OP(xa, xb, out);

    validate_binary_params();

    DSC_DISPATCH(xa->device, sub, xa, xb, out);

    cleanup_binary();

    return out;
}

dsc_tensor *dsc_mul(dsc_ctx *ctx,
                    dsc_tensor *xa,
                    dsc_tensor *xb,
                    dsc_tensor *out) {
    // DSC_TRACE_BINARY_OP(xa, xb, out);

    validate_binary_params();

    DSC_DISPATCH(xa->device, mul, xa, xb, out);

    cleanup_binary();

    return out;
}

dsc_tensor *dsc_div(dsc_ctx *ctx,
                    dsc_tensor *xa,
                    dsc_tensor *xb,
                    dsc_tensor *out) {
    // DSC_TRACE_BINARY_OP(xa, xb, out);

    validate_binary_params();

    DSC_DISPATCH(xa->device, div, xa, xb, out);

    cleanup_binary();

    return out;
}

dsc_tensor *dsc_pow(dsc_ctx *ctx,
                    dsc_tensor *xa,
                    dsc_tensor *xb,
                    dsc_tensor *out) {
    // DSC_TRACE_BINARY_OP(xa, xb, out);

    validate_binary_params();

    DSC_DISPATCH(xa->device, pow, xa, xb, out);

    cleanup_binary();

    return out;
}

// ============================================================
// Unary Operations

dsc_tensor *dsc_cos(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, cos, x, out);

    return out;
}

dsc_tensor *dsc_sin(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, sin, x, out);

    return out;
}

dsc_tensor *dsc_sinc(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, sinc, x, out);

    return out;
}

dsc_tensor *dsc_logn(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, logn, x, out);

    return out;
}

dsc_tensor *dsc_log2(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, log2, x, out);

    return out;
}

dsc_tensor *dsc_log10(dsc_ctx *ctx,
                      const dsc_tensor *DSC_RESTRICT x,
                      dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, log10, x, out);

    return out;
}

dsc_tensor *dsc_exp(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, exp, x, out);

    return out;
}

dsc_tensor *dsc_sqrt(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out) {
    // DSC_TRACE_UNARY_OP(x, out);

    validate_unary_params();

    DSC_DISPATCH(x->device, sqrt, x, out);

    return out;
}

dsc_tensor *dsc_abs(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) {
    DSC_ASSERT(x != nullptr);

    // DSC_TRACE_UNARY_OP(x, out);

    const dsc_dtype out_dtype = DSC_DTYPE_TO_REAL[x->dtype];
    if (out == nullptr) {
        out = dsc_new_tensor(ctx, x->n_dim, &x->shape[DSC_MAX_DIMS - x->n_dim], out_dtype, x->device);
    } else {
        DSC_ASSERT(out->dtype == out_dtype);
        DSC_ASSERT(out->n_dim == x->n_dim);
        DSC_ASSERT(out->device == x->device);
        DSC_ASSERT(memcmp(out->shape, x->shape, DSC_MAX_DIMS * sizeof(out->shape[0])) == 0);
    }

    DSC_DISPATCH(x->device, abs, x, out);

    return out;
}

dsc_tensor *dsc_angle(dsc_ctx *ctx,
                      const dsc_tensor *DSC_RESTRICT x) {
    DSC_ASSERT(x != nullptr);

    // DSC_TRACE_UNARY_NO_OUT_OP(x);

    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim, &x->shape[DSC_MAX_DIMS - x->n_dim], DSC_DTYPE_TO_REAL[x->dtype], x->device);

    DSC_DISPATCH(x->device, angle, x, out);

    return out;
}

dsc_tensor *dsc_conj(dsc_ctx *ctx,
                     dsc_tensor *DSC_RESTRICT x) {
    DSC_ASSERT(x != nullptr);

    // DSC_TRACE_UNARY_NO_OUT_OP(x);

    if (x->dtype == F32 || x->dtype == F64) {
        DSC_LOG_DEBUG("the input is real so it will be returned as is");
        return x;
    }

    dsc_tensor *out = dsc_new_like(ctx, x);

    DSC_DISPATCH(x->device, conj, x, out);

    return out;
}

dsc_tensor *dsc_real(dsc_ctx *ctx,
                     dsc_tensor *DSC_RESTRICT x) {
    DSC_ASSERT(x != nullptr);

    // DSC_TRACE_UNARY_NO_OUT_OP(x);

    if (x->dtype == F32 || x->dtype == F64) {
        DSC_LOG_DEBUG("the input is real so it will be returned as is");
        return x;
    }

    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim,
                                     &x->shape[DSC_MAX_DIMS - x->n_dim],
                                     DSC_DTYPE_TO_REAL[x->dtype], x->device);

    DSC_DISPATCH(x->device, real, x, out);

    return out;
}

dsc_tensor *dsc_imag(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x) {
    DSC_ASSERT(x != nullptr);

    // DSC_TRACE_UNARY_NO_OUT_OP(x);

    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim,
                                     &x->shape[DSC_MAX_DIMS - x->n_dim],
                                     DSC_DTYPE_TO_REAL[x->dtype], x->device);

    DSC_DISPATCH(x->device, imag, x, out);

    return out;
}

dsc_tensor *dsc_i0(dsc_ctx *ctx,
                   const dsc_tensor *DSC_RESTRICT x) {
    DSC_ASSERT(x != nullptr);
    DSC_ASSERT(x->dtype == F32 || x->dtype == F64);

    // DSC_TRACE_UNARY_NO_OUT_OP(x);

    dsc_tensor *out = dsc_new_like(ctx, x);

    DSC_DISPATCH(x->device, i0, x, out);

    return out;
}

dsc_tensor *dsc_clip(dsc_ctx *ctx, const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const f64 x_min, const f64 x_max) {
    // DSC_TRACE_UNARY_OP(x, out);

    DSC_ASSERT(x != nullptr);

    validate_unary_params();

    DSC_DISPATCH(x->device, clip, x, out, x_min, x_max);

    return out;
}

// ============================================================
// Unary Operations Along Axis

template <typename T>
static DSC_INLINE void sum(const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out,
                           int axis_idx) {
    // DSC_TENSOR_DATA(T, x);
    // DSC_TENSOR_DATA(T, out);
    //
    // const int axis_n = x->shape[axis_idx];
    // dsc_axis_iterator x_it(x, axis_idx, axis_n);
    // dsc_for(i, out) {
    //     T acc = dsc_zero<T>();
    //     for (int j = 0; j < axis_n; ++j) {
    //         acc = add_op()(acc, x_data[x_it.index()]);
    //         x_it.next();
    //     }
    //     out_data[i] = acc;
    // }
}

dsc_tensor *dsc_sum(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out,
                    const int axis,
                    const bool keep_dims) {
    // Fixme: keepdims=false won't work if x->n_dim = 1 because a scalar cannot be returned
    //  from this function, for now probably it makes sense to emulate this in Python
    DSC_TRACE_UNARY_AXIS_OP(x, out, axis, keep_dims);

    validate_reduce_params();

    const int axis_idx = dsc_tensor_dim(x, axis);

    switch (out->dtype) {
        case F32:
            sum<f32>(x, out, axis_idx);
            break;
        case F64:
            sum<f64>(x, out, axis_idx);
            break;
        case C32:
            sum<c32>(x, out, axis_idx);
            break;
        case C64:
            sum<c64>(x, out, axis_idx);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", out->dtype);
    }

    return out;
}

dsc_tensor *dsc_mean(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const int axis,
                     const bool keep_dims) {
    // DSC_TRACE_UNARY_AXIS_OP(x, out, axis, keep_dims);
    //
    // out = dsc_sum(ctx, x, out, axis, keep_dims);
    //
    // const int axis_idx = dsc_tensor_dim(x, axis);
    // const int axis_n = x->shape[axis_idx];
    //
    // dsc_tensor *scale;
    // switch (out->dtype) {
    //     case F32:
    //         DSC_WRAP_VALUE(scale, f32, 1.f / (f32) axis_n);
    //         break;
    //     case F64:
    //         DSC_WRAP_VALUE(scale, f64, 1. / (f64) axis_n);
    //         break;
    //     case C32:
    //         DSC_WRAP_VALUE(scale, c32, dsc_complex(c32, 1.f / (f32) axis_n, 0.f));
    //         break;
    //     case C64:
    //         DSC_WRAP_VALUE(scale, c64, dsc_complex(c64, 1. / (f64) axis_n, 0.));
    //         break;
    //     DSC_INVALID_CASE("unknown dtype=%d", out->dtype);
    // }
    // out = dsc_mul(ctx, out, scale, out);
    // return out;
    return nullptr;
}

template <typename T>
static DSC_INLINE void max(const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out,
                           int axis_idx) {
    // DSC_TENSOR_DATA(T, x);
    // DSC_TENSOR_DATA(T, out);
    //
    // const int axis_n = x->shape[axis_idx];
    // dsc_axis_iterator x_it(x, axis_idx, axis_n);
    // dsc_for(i, out) {
    //     T max = dsc_inf<T, false>();
    //     for (int j = 0; j < axis_n; ++j) {
    //         max = max_op()(max, x_data[x_it.index()]);
    //         x_it.next();
    //     }
    //     out_data[i] = max;
    // }
}

dsc_tensor *dsc_max(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const int axis,
                     const bool keep_dims) {
    DSC_TRACE_UNARY_AXIS_OP(x, out, axis, keep_dims);

    validate_reduce_params();

    const int axis_idx = dsc_tensor_dim(x, axis);

    switch (out->dtype) {
        case F32:
            max<f32>(x, out, axis_idx);
            break;
        case F64:
            max<f64>(x, out, axis_idx);
            break;
        case C32:
            max<c32>(x, out, axis_idx);
            break;
        case C64:
            max<c64>(x, out, axis_idx);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", out->dtype);
    }

    return out;
}

template <typename T>
static DSC_INLINE void min(const dsc_tensor *DSC_RESTRICT x,
                           dsc_tensor *DSC_RESTRICT out,
                           int axis_idx) {
    // DSC_TENSOR_DATA(T, x);
    // DSC_TENSOR_DATA(T, out);
    //
    // const int axis_n = x->shape[axis_idx];
    // dsc_axis_iterator x_it(x, axis_idx, axis_n);
    // dsc_for(i, out) {
    //     T min = dsc_inf<T, true>();
    //     for (int j = 0; j < axis_n; ++j) {
    //         min = min_op()(min, x_data[x_it.index()]);
    //         x_it.next();
    //     }
    //     out_data[i] = min;
    // }
}

dsc_tensor *dsc_min(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const int axis,
                     const bool keep_dims) {
    // DSC_TRACE_UNARY_AXIS_OP(x, out, axis, keep_dims);

    validate_reduce_params();

    const int axis_idx = dsc_tensor_dim(x, axis);

    switch (out->dtype) {
        case F32:
            min<f32>(x, out, axis_idx);
            break;
        case F64:
            min<f64>(x, out, axis_idx);
            break;
        case C32:
            min<c32>(x, out, axis_idx);
            break;
        case C64:
            min<c64>(x, out, axis_idx);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", out->dtype);
    }

    return out;
}

// ============================================================
// Fourier Transforms

template<typename Tin, typename Tout, bool forward>
static DSC_INLINE void exec_fft(dsc_ctx *ctx,
                                const dsc_tensor *DSC_RESTRICT x,
                                dsc_tensor *DSC_RESTRICT out,
                                const int axis, const int x_n,
                                const int fft_n) {
    // const dsc_dtype out_dtype = out->dtype;
    // dsc_fft_plan *plan = dsc_plan_fft(ctx, fft_n, COMPLEX, out_dtype);
    //
    // // Push the arena to make these two buffers temporary
    // dsc_tensor *buff = dsc_tensor_1d(ctx, out_dtype, fft_n);
    // dsc_tensor *fft_work = dsc_tensor_1d(ctx, out_dtype, fft_n);
    //
    // DSC_TENSOR_DATA(Tin, x);
    // DSC_TENSOR_DATA(Tout, buff);
    // DSC_TENSOR_DATA(Tout, out);
    // DSC_TENSOR_DATA(Tout, fft_work);
    //
    // dsc_axis_iterator x_it(x, axis, fft_n);
    // dsc_axis_iterator out_it(out, axis, fft_n);
    //
    // while (x_it.has_next()) {
    //     for (int i = 0; i < fft_n; ++i) {
    //         if (i < x_n) {
    //             int idx = x_it.index();
    //             if constexpr (dsc_is_type<Tin, Tout>()) {
    //                 buff_data[i] = x_data[idx];
    //             } else {
    //                 buff_data[i] = cast_op().template operator()<Tin, Tout>(x_data[idx]);
    //             }
    //
    //             x_it.next();
    //         } else {
    //             buff_data[i] = dsc_zero<Tout>();
    //         }
    //     }
    //
    //     dsc_complex_fft<Tout, forward>(plan, buff_data, fft_work_data);
    //
    //     for (int i = 0; i < fft_n; ++i) {
    //         const int idx = out_it.index();
    //         out_data[idx] = buff_data[i];
    //
    //         out_it.next();
    //     }
    // }
}

template<bool forward>
static DSC_INLINE dsc_tensor *dsc_internal_fft(dsc_ctx *ctx,
                                               const dsc_tensor *DSC_RESTRICT x,
                                               dsc_tensor *DSC_RESTRICT out,
                                               int n,
                                               const int axis) {
    DSC_ASSERT(x != nullptr);

    DSC_TRACE_FFT_OP(x, out, n, axis, dsc_fft_type::COMPLEX, forward);

    const int axis_idx = dsc_tensor_dim(x, axis);
    DSC_ASSERT(axis_idx < DSC_MAX_DIMS);

    const int x_n = x->shape[axis_idx];
    const int axis_n = dsc_pow2_n(x_n);
    if (n > 0) {
        n = dsc_pow2_n(n);
    } else {
        n = axis_n;
    }

    int out_shape[DSC_MAX_DIMS];
    for (int i = 0; i < DSC_MAX_DIMS; ++i)
        out_shape[i] = i != axis_idx ? x->shape[i] : n;

    dsc_dtype out_dtype = x->dtype;
    if (x->dtype == F32) {
        out_dtype = C32;
    } else if (x->dtype == F64) {
        out_dtype = C64;
    }

    if (out == nullptr) {
        out = dsc_new_tensor(ctx, x->n_dim, &out_shape[DSC_MAX_DIMS - x->n_dim], out_dtype);
    } else {
        DSC_ASSERT(out->dtype == out_dtype);
        DSC_ASSERT(out->n_dim == x->n_dim);
        DSC_ASSERT(memcmp(out_shape, out->shape, DSC_MAX_DIMS * sizeof(out->shape[0])) == 0);
    }

    DSC_LOG_DEBUG("performing %s FFT of length %d on x=[%d %d %d %d] over axis %d with size %d",
                  forward ? "FWD" : "BWD", n,
                  x->shape[0], x->shape[1], x->shape[2], x->shape[3],
                  axis_idx, x->shape[axis_idx]);

    switch (x->dtype) {
        case F32:
            exec_fft<f32, c32, forward>(ctx, x, out, axis_idx, x_n, n);
            break;
        case F64:
            exec_fft<f64, c64, forward>(ctx, x, out, axis_idx, x_n, n);
            break;
        case C32:
            exec_fft<c32, c32, forward>(ctx, x, out, axis_idx, x_n, n);
            break;
        case C64:
            exec_fft<c64, c64, forward>(ctx, x, out, axis_idx, x_n, n);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", x->dtype);
    }

    return out;
}

dsc_tensor *dsc_fft(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out,
                    const int n,
                    const int axis) {
    // Find N

    // Get the plan

    // --> Parallel START <--
    // Copy the desired axis of X over to a temp buffer (or out if we can go in-place?)

    // Perform the FFT

    // Write the result back (or not if we are working in-place?)
    // --> Parallel STOP <--

    // Done!
    return dsc_internal_fft<true>(ctx, x, out, n, axis);
}

dsc_tensor *dsc_ifft(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const int n,
                     const int axis) {
    return dsc_internal_fft<false>(ctx, x, out, n, axis);
}

template<typename T, bool forward>
static DSC_INLINE void exec_rfft(dsc_ctx *ctx,
                                 const dsc_tensor *DSC_RESTRICT x,
                                 dsc_tensor *DSC_RESTRICT out,
                                 const int axis, const int x_n,
                                 const int out_n, const int fft_order) {
    // static_assert(dsc_is_complex<T>(), "T must be the complex type of the transform");
    //
    // const dsc_dtype out_dtype = out->dtype;
    // dsc_fft_plan *plan = dsc_plan_fft(ctx, fft_order, REAL, out_dtype);
    //
    // // Push the arena to make these two buffers temporary
    // dsc_tensor *buff = dsc_tensor_1d(ctx, out_dtype, out_n);
    // dsc_tensor *fft_work = dsc_tensor_1d(ctx, out_dtype, out_n);
    //
    // DSC_TENSOR_DATA(T, fft_work);
    //
    // if constexpr (forward) {
    //     dsc_axis_iterator x_it(x, axis, DSC_MIN(x_n, fft_order * 2));
    //     dsc_axis_iterator out_it(out, axis, out_n);
    //
    //     while (x_it.has_next()) {
    //         for (int i = 0; i < (fft_order << 1); ++i) {
    //             if (i < x_n) {
    //                 int idx = x_it.index();
    //                 ((real<T> *) buff->data)[i] = ((real<T> *) x->data)[idx];
    //                 x_it.next();
    //             } else {
    //                 ((real<T> *) buff->data)[i] = dsc_zero<real<T>>();
    //             }
    //         }
    //
    //         dsc_real_fft<T, true>(plan, (T *)buff->data, fft_work_data);
    //
    //         for (int i = 0; i < out_n; ++i) {
    //             const int idx = out_it.index();
    //             ((T *) out->data)[idx] = ((T *)buff->data)[i];
    //
    //             out_it.next();
    //         }
    //     }
    // } else {
    //     dsc_axis_iterator x_it(x, axis, fft_order + 1);
    //     dsc_axis_iterator out_it(out, axis, out_n);
    //
    //     while (x_it.has_next()) {
    //         for (int i = 0; i < fft_order + 1; ++i) {
    //             if (i < x_n) {
    //                 int idx = x_it.index();
    //                 ((T *) buff->data)[i] = ((T *) x->data)[idx];
    //                 x_it.next();
    //             } else {
    //                 ((T *) buff->data)[i] = dsc_zero<T>();
    //             }
    //         }
    //
    //         dsc_real_fft<T, false>(plan, (T *) buff->data, fft_work_data);
    //
    //         for (int i = 0; i < out_n; ++i) {
    //             const int idx = out_it.index();
    //             ((real<T> *) out->data)[idx] = ((real<T> *) buff->data)[i];
    //
    //             out_it.next();
    //         }
    //     }
    // }
}

template<bool forward>
static DSC_INLINE dsc_tensor *dsc_internal_rfft(dsc_ctx *ctx,
                                                const dsc_tensor *DSC_RESTRICT x,
                                                dsc_tensor *DSC_RESTRICT out,
                                                const int n,
                                                const int axis) {
    // For an RFFT if N is not specified N = (dim / 2) + 1
    // For an IRFFT if N is not specified N = 2 * (dim - 1)
    // Note: for now, since we support only power of 2 FFTs, the input of IRFFT is assumed to have
    //       the same shape as the output of RFFT. If this is not the case be careful, there can be
    //       issues with the results of IRFFT.
    DSC_ASSERT(x != nullptr);

    DSC_TRACE_FFT_OP(x, out, n, axis, dsc_fft_type::REAL, forward);

    const int axis_idx = dsc_tensor_dim(x, axis);
    DSC_ASSERT(axis_idx < DSC_MAX_DIMS);

    const int x_n = x->shape[axis_idx];
    int out_n, fft_order;

    if constexpr (forward) {
        fft_order = ((n > 0) ? dsc_pow2_n(n) : dsc_pow2_n(x_n)) >> 1;
        out_n = fft_order + 1;
    } else {
        // Todo: verify that this makes sense
        fft_order = (n > 0) ? dsc_pow2_n(n - 1) : dsc_pow2_n(x_n - 1);
        out_n = fft_order << 1;
    }

    int out_shape[DSC_MAX_DIMS];
    for (int i = 0; i < DSC_MAX_DIMS; ++i)
        out_shape[i] = i != axis_idx ? x->shape[i] : out_n;

    dsc_dtype out_dtype;
    if constexpr (forward) {
        if (x->dtype == F32) out_dtype = C32;
        else if (x->dtype == F64) out_dtype = C64;
        else DSC_LOG_FATAL("RFFT input must be real");
    } else {
        if (x->dtype == C32) out_dtype = F32;
        else if (x->dtype == C64) out_dtype = F64;
        else DSC_LOG_FATAL("IRFFT input must be complex");
    }

    if (out == nullptr) {
        out = dsc_new_tensor(ctx, x->n_dim, &out_shape[DSC_MAX_DIMS - x->n_dim], out_dtype);
    } else {
        DSC_ASSERT(out->dtype == out_dtype);
        DSC_ASSERT(out->n_dim == x->n_dim);
        DSC_ASSERT(memcmp(out_shape, out->shape, DSC_MAX_DIMS * sizeof(out->shape[0])) == 0);
    }

    DSC_LOG_DEBUG("performing %s RFFT of length %d on x=[%d %d %d %d] over axis %d with size %d",
                  forward ? "FWD" : "BWD", n,
                  x->shape[0], x->shape[1], x->shape[2], x->shape[3],
                  axis_idx, x->shape[axis_idx]);

    switch (x->dtype) {
        case F32:
        case C32:
            exec_rfft<c32, forward>(ctx, x, out, axis_idx, x_n, out_n, fft_order);
            break;
        case F64:
        case C64:
            exec_rfft<c64, forward>(ctx, x, out, axis_idx, x_n, out_n, fft_order);
            break;
        DSC_INVALID_CASE("unknown dtype=%d", x->dtype);
    }

    return out;
}

dsc_tensor *dsc_rfft(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const int n,
                     const int axis) {
    return dsc_internal_rfft<true>(ctx, x, out, n, axis);
}

dsc_tensor *dsc_irfft(dsc_ctx *ctx,
                      const dsc_tensor *DSC_RESTRICT x,
                      dsc_tensor *DSC_RESTRICT out,
                      const int n,
                      const int axis) {
    return dsc_internal_rfft<false>(ctx, x, out, n, axis);
}

template<typename T>
static DSC_INLINE void dsc_internal_fftfreq(dsc_tensor *x,
                                           const int n,
                                           const T d) {
    static_assert(dsc_is_real<T>(), "T must be real");
    const T factor = 1 / (n * d);

    DSC_TENSOR_DATA(T, x);

    const int odd = n & 1;
    const int n2 = odd ? ((n - 1) >> 1) : (n >> 1);

    for (int i = 0; i < (n2 + odd); ++i)
        x_data[i] = i * factor;

    for (int i = 0; i < n2; ++i)
        x_data[(n2 + odd) + i] = (-n2 + i) * factor;
}

dsc_tensor *dsc_fftfreq(dsc_ctx *ctx,
                        const int n,
                        const f64 d,
                        const dsc_dtype dtype) {
    DSC_ASSERT(n > 0);

    // out = [0, 1, ...,   n/2-1,     -n/2, ..., -1] / (d*n)   if n is even
    // out = [0, 1, ..., (n-1)/2, -(n-1)/2, ..., -1] / (d*n)   if n is odd

    dsc_tensor *out = dsc_tensor_1d(ctx, dtype, n);
    switch (dtype) {
        case F32:
            dsc_internal_fftfreq(out, n, (f32) d);
            break;
        case F64:
            dsc_internal_fftfreq(out, n, (f64) d);
            break;
        DSC_INVALID_CASE("dtype must be real");
    }

    return out;
}

template<typename T>
static DSC_INLINE void dsc_internal_rfftfreq(dsc_tensor *x,
                                            const int n,
                                            const T d) {
    static_assert(dsc_is_real<T>(), "T must be real");
    const T factor = 1 / (n * d);

    DSC_TENSOR_DATA(T, x);

    for (int i = 0; i < x->ne; ++i) {
        x_data[i] = i * factor;
    }
}

dsc_tensor *dsc_rfftfreq(dsc_ctx *ctx,
                         const int n,
                         const f64 d,
                         const dsc_dtype dtype) {
    DSC_ASSERT(n > 0);
    // out = [0, 1, ...,     n/2-1,     n/2] / (d*n)   if n is even
    // out = [0, 1, ..., (n-1)/2-1, (n-1)/2] / (d*n)   if n is odd
    // Note that the value of n that multiplies d is the same in both cases.
    const int n2 = (n & 1) ? (((n - 1) >> 1) + 1) : ((n >> 1) + 1);

    dsc_tensor *out = dsc_tensor_1d(ctx, dtype, n2);
    switch (dtype) {
        case F32:
            dsc_internal_rfftfreq(out, n, (f32) d);
            break;
        case F64:
            dsc_internal_rfftfreq(out, n, (f64) d);
            break;
        DSC_INVALID_CASE("dtype must be real");
    }

    return out;
}