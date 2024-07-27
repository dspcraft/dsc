#include "dsc.h"
#include "dsc_ops.h"
#include "dsc_fft.h"
#include <cstring>
#include <pthread.h>
#include <atomic>
#include <sys/sysinfo.h> // get_nprocs()

// How many independent FFT plans we support. This is completely arbitrary
#if !defined(DSC_FFT_PLANS)
#   define DSC_FFT_PLANS ((int) 16)
#endif

#define CONST_OP_IMPL(func, type) \
    dsc_tensor *dsc_##func##_##type(dsc_ctx *ctx,                               \
                                    dsc_tensor *DSC_RESTRICT x,                 \
                                    const type val,                             \
                                    dsc_tensor *DSC_RESTRICT out) noexcept {    \
        return dsc_##func(ctx, x, out, val);                                    \
}

#define validate_binary_params() \
    do {                                    \
        DSC_ASSERT(xa != nullptr);          \
        DSC_ASSERT(xb != nullptr);          \
        DSC_ASSERT(can_broadcast(xa, xb));  \
\
        const int n_dim = DSC_MAX(xa->n_dim, xb->n_dim); \
\
        int shape[DSC_MAX_DIMS];                                                                \
        for (int i = 0; i < DSC_MAX_DIMS; ++i) shape[i] = DSC_MAX(xa->shape[i], xb->shape[i]);  \
\
       const dsc_dtype out_dtype = DSC_DTYPE_CONVERSION_TABLE[xa->dtype][xb->dtype]; \
\
        if (out == nullptr) {                                                               \
            out = dsc_new_tensor(ctx, n_dim, &shape[DSC_MAX_DIMS - n_dim], out_dtype);      \
        } else {                                                                            \
            DSC_ASSERT(out->dtype == out_dtype);                                            \
            DSC_ASSERT(out->n_dim == n_dim);                                                \
            DSC_ASSERT(memcmp(out->shape, shape, DSC_MAX_DIMS * sizeof(shape[0])) == 0);    \
        }                                                                                   \
\
        xa = dsc_cast(ctx, out_dtype, xa);  \
        xb = dsc_cast(ctx, out_dtype, xb);  \
    } while (0)                             \

#define validate_unary_params() \
    do {                                \
        DSC_ASSERT(x != nullptr);       \
        if (out == nullptr) {           \
            out = dsc_new_like(ctx, x); \
            copy(x, out);               \
        } else {                        \
            DSC_ASSERT(out->dtype == x->dtype);                                                     \
            DSC_ASSERT(out->n_dim == x->n_dim);                                                     \
            DSC_ASSERT(memcmp(out->shape, x->shape, DSC_MAX_DIMS * sizeof(out->shape[0])) == 0);    \
        }                                                                                           \
    } while(0)


//enum dsc_op : u8 {
//    FFT,
//    IFFT,
//    DONE,
//};

//struct dsc_fft_plan {
//    void *twiddles;
//    int n;
//    dsc_dtype twiddles_dtype;
//};

//struct dsc_task {
//    c64 *x;
//    // Counter shared between all the workers, used to determine whether they have finished or not.
//    std::atomic_int *progress_counter;
//    int n_times;
//    dsc_op op;
//};

//struct dsc_worker {
//    pthread_cond_t cond;
//    pthread_mutex_t mtx;
//    dsc_fft_plan *plan;
//    dsc_tensor *work;
//    dsc_task task;
//    pthread_t id;
//    bool has_work;
//};

struct dsc_obj {
    usize offset;
    usize nb;
};

struct dsc_buffer {
    usize nb;
    dsc_obj *last;
    // Todo: not a good idea if we want to give each worker it's own buffer
    dsc_fft_plan *plans[DSC_FFT_PLANS];
    int n_objs;
    int n_plans;
};

struct dsc_ctx {
    dsc_buffer *buffer;
//    dsc_fft_plan *fft_plan;
//    dsc_tensor *fft_work;
    // Todo: not very good but good enough for now
//    dsc_worker *fft_workers;
//    int n_workers;
};

// ================================================== Private Functions ================================================== //
template<typename T, typename Op>
static void binary_op(const dsc_tensor *DSC_RESTRICT xa,
                      const dsc_tensor *DSC_RESTRICT xb,
                      dsc_tensor *DSC_RESTRICT out,
                      Op op) noexcept {
    DSC_TENSOR_DATA(T, xa);
    DSC_TENSOR_DATA(T, xb);
    DSC_TENSOR_DATA(T, out);

    DSC_TENSOR_FIELDS(xa, 3);
    DSC_TENSOR_FIELDS(xb, 3);
    DSC_TENSOR_FIELDS(out, 3);

    dsc_for(out, 3) {
        out_data[dsc_offset(out, 3)] = op(
                xa_data[dsc_broadcast_offset(xa, 3)],
                xb_data[dsc_broadcast_offset(xb, 3)]
        );
    }
}

template<typename T, typename Op>
static void unary_op(const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     Op op) noexcept {
    DSC_TENSOR_DATA(T, x);
    DSC_TENSOR_DATA(T, out);

    for (int i = 0; i < x->ne; ++i) {
        out_data[i] = op(x_data[i]);
    }
}

template<typename Tx, typename To>
static void copy_op(const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    DSC_TENSOR_DATA(Tx, x);
    DSC_TENSOR_DATA(To, out);

    DSC_ASSERT(out->ne == x->ne);

    // Todo: I can probably do better but (if it works) it's fine for now
    for (int i = 0; i < out->ne; ++i) {
        if constexpr (dsc_is_complex<To>()) {
            if constexpr (dsc_is_real<Tx>()) {
                if constexpr (dsc_is_type<To, c32>()) {
                    out_data[i] = dsc_complex(To, (f32) x_data[i], 0);
                }
                if constexpr (dsc_is_type<To, c64>()) {
                    out_data[i] = dsc_complex(To, (f64) x_data[i], 0);
                }
            } else {
                if constexpr (dsc_is_type<To, c32>()) {
                    out_data[i] = dsc_complex(To, (f32) x_data[i].real, (f32) x_data[i].imag);
                }
                if constexpr (dsc_is_type<To, c64>()) {
                    out_data[i] = dsc_complex(To, (f64) x_data[i].real, (f64) x_data[i].imag);
                }
            }
        } else {
            if constexpr (dsc_is_real<Tx>()) {
                out_data[i] = (To) x_data[i];
            } else {
                if constexpr (dsc_is_type<To, f32>()) {
                    out_data[i] = (f32) x_data[i].real;
                }
                if constexpr (dsc_is_type<To, f64>()) {
                    out_data[i] = (f64) x_data[i].real;
                }
            }
        }
    }
}

template<typename T>
static void assign_op(dsc_tensor *DSC_RESTRICT x,
                      const T start, const T step) noexcept {
    DSC_TENSOR_DATA(T, x);

    T val = start;
    for (int i = 0; i < x->ne; ++i) {
        x_data[i] = val;
        val = add_op()(val, step);
    }
}

template<typename Op>
static void binary_op(const dsc_tensor *DSC_RESTRICT xa,
                      const dsc_tensor *DSC_RESTRICT xb,
                      dsc_tensor *DSC_RESTRICT out,
                      Op op) noexcept {
    switch (out->dtype) {
        case dsc_dtype::F32:
            binary_op<f32>(xa, xb, out, op);
            break;
        case dsc_dtype::F64:
            binary_op<f64>(xa, xb, out, op);
            break;
        case dsc_dtype::C32:
            binary_op<c32>(xa, xb, out, op);
            break;
        case dsc_dtype::C64:
            binary_op<c64>(xa, xb, out, op);
            break;
        default:
            DSC_LOG_ERR("unknown dtype %d", out->dtype);
    }
}

template<typename Op>
static void unary_op(const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     Op op) noexcept {
    switch (x->dtype) {
        case dsc_dtype::F32:
            unary_op<f32>(x, out, op);
            break;
        case dsc_dtype::F64:
            unary_op<f64>(x, out, op);
            break;
        case dsc_dtype::C32:
            unary_op<c32>(x, out, op);
            break;
        case dsc_dtype::C64:
            unary_op<c64>(x, out, op);
            break;
        default:
            DSC_LOG_ERR("unknown dtype %d", x->dtype);
    }
}


template<typename Tx>
static void copy_op(const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    switch (x->dtype) {
        case dsc_dtype::F32:
            copy_op<Tx, f32>(x, out);
            break;
        case dsc_dtype::F64:
            copy_op<Tx, f64>(x, out);
            break;
        case dsc_dtype::C32:
            copy_op<Tx, c32>(x, out);
            break;
        case dsc_dtype::C64:
            copy_op<Tx, c64>(x, out);
            break;
        default:
            DSC_LOG_ERR("unknown dtype %d", x->dtype);
    }
}

static void copy(const dsc_tensor *DSC_RESTRICT x,
                 dsc_tensor *DSC_RESTRICT out) noexcept {
    switch (x->dtype) {
        case dsc_dtype::F32:
            copy_op<f32>(x, out);
            break;
        case dsc_dtype::F64:
            copy_op<f64>(x, out);
            break;
        case dsc_dtype::C32:
            copy_op<c32>(x, out);
            break;
        case dsc_dtype::C64:
            copy_op<c64>(x, out);
            break;
        default:
            DSC_LOG_ERR("unknown dtype %d", x->dtype);
    }
}

template <typename T>
static DSC_INLINE dsc_tensor *dsc_wrap_value(dsc_ctx *ctx, const T val) noexcept {
    dsc_tensor *out = dsc_tensor_1d(ctx, dsc_type_mapping<T>::value, 1);
    DSC_TENSOR_DATA(T, out);
    out_data[0] = val;

    return out;
}

template <typename T>
static DSC_INLINE dsc_tensor *dsc_addc(dsc_ctx *ctx,
                                       dsc_tensor *DSC_RESTRICT x,
                                       dsc_tensor *DSC_RESTRICT out,
                                       const T val) noexcept {
    dsc_tensor *xb = dsc_wrap_value(ctx, val);

    return dsc_add(ctx, x, xb, out);
}

template <typename T>
static DSC_INLINE dsc_tensor *dsc_subc(dsc_ctx *ctx,
                                       dsc_tensor *DSC_RESTRICT x,
                                       dsc_tensor *DSC_RESTRICT out,
                                       const T val) noexcept {
    dsc_tensor *xb = dsc_wrap_value(ctx, val);

    return dsc_sub(ctx, x, xb, out);
}

template <typename T>
static DSC_INLINE dsc_tensor *dsc_mulc(dsc_ctx *ctx,
                                       dsc_tensor *DSC_RESTRICT x,
                                       dsc_tensor *DSC_RESTRICT out,
                                       const T val) noexcept {
    dsc_tensor *xb = dsc_wrap_value(ctx, val);

    return dsc_mul(ctx, x, xb, out);
}

template <typename T>
static DSC_INLINE dsc_tensor *dsc_divc(dsc_ctx *ctx,
                                       dsc_tensor *DSC_RESTRICT x,
                                       dsc_tensor *DSC_RESTRICT out,
                                       const T val) noexcept {
    dsc_tensor *xb = dsc_wrap_value(ctx, val);

    return dsc_div(ctx, x, xb, out);
}

static bool DSC_INLINE DSC_PURE can_broadcast(const dsc_tensor *DSC_RESTRICT xa,
                                              const dsc_tensor *DSC_RESTRICT xb) noexcept {
    bool can_broadcast = true;
    for (int i = 0; i < DSC_MAX_DIMS && can_broadcast; ++i) {
        can_broadcast = xa->shape[i] == xb->shape[i] || xa->shape[i] == 1 || xb->shape[i] == 1;
    }

    return can_broadcast;
}

template<typename T>
static dsc_tensor *dsc_log_space(dsc_ctx *ctx, const T start, const T stop,
                                 const int n, const T base) noexcept {
    static_assert(dsc_is_real<T>());
    DSC_ASSERT((stop - start) > std::numeric_limits<T>::min());

    dsc_tensor *x = dsc_tensor_1d(ctx, dsc_type_mapping<T>::value, n);

    DSC_TENSOR_DATA(T, x);

    const T step = (stop - start) / (n - 1);

    T e = step;
    for (int i = 0; i < x->ne; ++i) {
        x_data[i] = std::pow(base, e);
        e += step;
    }

    return x;
}

template<typename T>
static DSC_INLINE T dsc_interp_point(const T *DSC_RESTRICT x, const T *DSC_RESTRICT y,
                                     const int n, const T xp,
                                     const T left, const T right) noexcept {
    if (xp <= x[0])
        return left;
    if (xp >= x[n - 1])
        return right;

    int i = 0;
    if (xp >= x[n - 2]) {
        i = n - 2;
    } else {
        while(xp > x[i + 1]) ++i;
    }

    const T x_left = x[i], y_left = y[i];
    const T x_right = x[i + 1], y_right = y[i + 1];

    return y_left + (y_right - y_left) / (x_right - x_left) * (xp - x_left);
}

template<typename T>
static dsc_tensor *dsc_interp1d(dsc_ctx *ctx, const dsc_tensor *x,
                                const dsc_tensor *y, const dsc_tensor *xp,
                                T left, T right) noexcept {
    static_assert(dsc_is_real<T>());
    DSC_ASSERT(x->dtype == y->dtype);
    DSC_ASSERT(x->dtype == xp->dtype);
    DSC_ASSERT(dsc_type_mapping<T>::value == x->dtype);
    DSC_ASSERT(x->n_dim == 1);
    DSC_ASSERT(y->n_dim == 1);
    DSC_ASSERT(xp->n_dim == 1);
    DSC_ASSERT(x->ne == y->ne);
    DSC_ASSERT(y->ne == xp->ne);

    dsc_tensor *out = dsc_new_like(ctx, x);

    DSC_TENSOR_DATA(T, x);
    DSC_TENSOR_DATA(T, y);
    DSC_TENSOR_DATA(T, xp);
    DSC_TENSOR_DATA(T, out);

    if (left == std::numeric_limits<T>::max()) {
        left = y_data[0];
    }
    if (right == std::numeric_limits<T>::max()) {
        right = y_data[y->ne - 1];
    }

    for (int i = 0; i < out->ne; ++i) {
        out_data[i] = dsc_interp_point(x_data, y_data, out->ne,
                                       xp_data[i], left, right);
    }

    return out;
}

static dsc_buffer *dsc_buffer_alloc(const usize nb) noexcept {
    const usize buff_size = DSC_ALIGN(nb + sizeof(dsc_buffer), DSC_PAGE_SIZE);

    dsc_buffer *buff = (dsc_buffer *) aligned_alloc(DSC_PAGE_SIZE, buff_size);
    DSC_ASSERT(buff != nullptr);

    buff->nb = buff_size - sizeof(dsc_buffer);
    buff->n_objs = 0;
    buff->n_plans = 0;
    buff->last = nullptr;

    return buff;
}

template<typename T>
static DSC_INLINE T *dsc_obj_alloc(dsc_buffer *buff, const usize nb) noexcept {
    const usize last_offset = buff->last == nullptr ? 0 : buff->last->offset;
    const usize last_size = buff->last == nullptr ? 0 : buff->last->nb;
    const usize last_end = last_offset + last_size;

    if (nb + sizeof(dsc_obj) + last_end > buff->nb) {
        DSC_LOG_FATAL("can't allocate %.2fKB", DSC_B_TO_KB(nb));
    }

    // The actual buffer starts after the 'header' of the arena struct.
    dsc_obj *new_obj = (dsc_obj *) ((byte *) buff + last_end + sizeof(dsc_buffer));
    // The offset refers to the actual offset of the "contained" object which comes after
    // the dsc_object "header".
    new_obj->offset = last_end + sizeof(dsc_obj);
    new_obj->nb = nb;

    buff->n_objs++;
    buff->last = new_obj;

    return (T *) ((byte *) buff + sizeof(dsc_buffer) + buff->last->offset);
}


static DSC_NOINLINE void dsc_fft_r2(c64 *DSC_RESTRICT x,
                                    c64 *DSC_RESTRICT work,
                                    const f64 *DSC_RESTRICT twiddles,
                                    const int n,
                                    const f64 sign) noexcept {
    // Base case
    if (n <= 1) return;

    const int n2 = n >> 1;

    // Divide
    for (int i = 0; i < n2; i++) {
        work[i] = x[2 * i];
        work[i + n2] = x[2 * i + 1];
    }

    // FFT of even indexes
    dsc_fft_r2(work, x, twiddles, n2, sign);
    // FFT of odd indexes
    dsc_fft_r2(work + n2, x, twiddles, n2, sign);

    const int t_base = (n2 - 1) << 1;

    // Conquer
    for (int k = 0; k < n2; ++k) {
        c64 tmp{};
        // Twiddle[k] = [Re, Imag] = [cos(theta), sin(theta)]
        // but cos(theta) = cos(-theta) and sin(theta) = -sin(theta)
        const f64 twiddle_r = twiddles[t_base + (2 * k)];
        const f64 twiddle_i = sign * twiddles[t_base + (2 * k) + 1];
        // t = w * x_odd[k]
        // x[k] = x_even[k] + t
        // x[k + n/2] = x_even[k] - t

        const c64 x_odd_k = work[k + n2];
        const c64 x_even_k = work[k];

        tmp.real = twiddle_r * x_odd_k.real - twiddle_i * x_odd_k.imag;
        tmp.imag = twiddle_r * x_odd_k.imag + twiddle_i * x_odd_k.real;

        x[k].real = x_even_k.real + tmp.real;
        x[k].imag = x_even_k.imag + tmp.imag;
        x[k + n2].real = x_even_k.real - tmp.real;
        x[k + n2].imag = x_even_k.imag - tmp.imag;
    }
}

static DSC_INLINE void dsc_internal_fft(c64 *DSC_RESTRICT x,
                                        c64 *DSC_RESTRICT work,
                                        const f64 *DSC_RESTRICT twiddles,
                                        const int n) noexcept {
    dsc_fft_r2(x, work, twiddles, n, 1);
}

static DSC_INLINE void dsc_internal_ifft(c64 *DSC_RESTRICT x,
                                         c64 *DSC_RESTRICT work,
                                         const f64 *DSC_RESTRICT twiddles,
                                         const int n) noexcept {
    dsc_fft_r2(x, work, twiddles, n, -1);

    // Scale by 1/N
    const f64 scale = 1. / n;
    for (int i = 0; i < n; ++i) {
        x[i].real *= scale;
        x[i].imag *= scale;
    }
}

//static DSC_NOINLINE void *fft_worker_thread(void *arg) noexcept {
//    dsc_worker *self = (dsc_worker *) arg;
//
//    // Todo: assuming the plan is already initialized when I do this
//    const int n = self->plan->n;
//    const f64 *twiddles = (f64 *) self->plan->twiddles;
//    c64 *work_buff = (c64 *) self->work->data;
//
//    dsc_task *work;
//    bool exit = false;
//    while (true) {
//        pthread_mutex_lock(&self->mtx);
//        while (!self->has_work)
//            pthread_cond_wait(&self->cond, &self->mtx);
//
//        self->has_work = false;
//        pthread_mutex_unlock(&self->mtx);
//
//        // We don't need to hold the lock as the master thread will wait for completion before setting another task
//        work = &self->task;
//
//        switch (work->op) {
//            case FFT: {
//                for (int i = 0; i < work->n_times; ++i) {
//                    dsc_internal_fft(
//                            &work->x[n * i],
//                            work_buff,
//                            twiddles,
//                            n
//                    );
//                }
//                work->progress_counter->fetch_add(1);
//                break;
//            }
//            case IFFT: {
//                for (int i = 0; i < work->n_times; ++i) {
//                    dsc_internal_ifft(
//                            &work->x[n * i],
//                            work_buff,
//                            twiddles,
//                            n
//                    );
//                }
//                work->progress_counter->fetch_add(1);
//                break;
//            }
//            case DONE: {
//                exit = true;
//                break;
//            }
//            default: {
//                DSC_LOG_ERR("unknown op=%d", work->op);
//                break;
//            }
//        }
//        if (exit)
//            break;
//    }
//
//    pthread_exit(nullptr);
//}

// ================================================ End Private Functions ================================================ //


dsc_ctx *dsc_ctx_init(const usize nb) noexcept {
    DSC_ASSERT(nb > 0);

    dsc_ctx *ctx = (dsc_ctx *) malloc(sizeof(dsc_ctx));
    DSC_ASSERT(ctx != nullptr);

    // Todo: inline if we don't need more buffers
    ctx->buffer = dsc_buffer_alloc(nb);

    DSC_LOG_INFO("created new context %p of %ldMB",
                 (void *) ctx,
                 (usize) DSC_B_TO_MB(ctx->buffer->nb)
    );

    return ctx;
}

static dsc_fft_plan *dsc_get_plan(dsc_ctx *ctx, const int n,
                                  const dsc_dtype dtype) noexcept {
    const dsc_buffer *buffer = ctx->buffer;
    dsc_fft_plan *plan = nullptr;
    for (int i = 0; i < buffer->n_plans; ++i) {
        dsc_fft_plan *cached_plan = buffer->plans[i];
        if ((cached_plan != nullptr) &&
            (cached_plan->n == n) &&
            (cached_plan->dtype == dtype)) {
            plan = cached_plan;
            break;
        }
    }

    return plan;
}

dsc_fft_plan *dsc_plan_fft(dsc_ctx *ctx, const int n,
                  const dsc_dtype dtype) noexcept {
    const int fft_n = dsc_fft_best_n(n);

    DSC_LOG_DEBUG("N=%d (%d) dtype=%s", fft_n, n, DSC_DTYPE_NAMES[dtype]);

    dsc_fft_plan *plan = dsc_get_plan(ctx, fft_n, dtype);

    if (plan == nullptr) {
        dsc_buffer *buffer = ctx->buffer;
        if (buffer->n_plans <= DSC_FFT_PLANS) {
            const usize storage = dsc_fft_storage(fft_n);

            DSC_LOG_DEBUG("allocating new FFT plan with N=%d dtype=%s",
                          fft_n, DSC_DTYPE_NAMES[dtype]);

            plan = dsc_obj_alloc<dsc_fft_plan>(buffer, storage);
            dsc_init_plan(plan, fft_n, dtype);

            buffer->plans[buffer->n_plans++] = plan;
        } else {
            DSC_LOG_FATAL("too many plans in context!");
        }
    } else {
        DSC_LOG_DEBUG("found cached FFT plan with N=%d dtype=%s",
                      fft_n, DSC_DTYPE_NAMES[dtype]);
    }

    return plan;

//    DSC_ASSERT(twiddles_dtype == F64);
//
//    // Precompute twiddles
//    usize twiddle_storage = 0;
//    for (int twiddle_n = 2; twiddle_n <= n; twiddle_n <<= 1) {
//        // Given N we need N/2 twiddle factors but each twiddle factor has a real and an imaginary part so that's N
//        twiddle_storage += twiddle_n;
//    }
//
//    dsc_fft_plan *plan = dsc_obj_alloc<dsc_fft_plan>(ctx->buffer, twiddle_storage * DSC_DTYPE_SIZE[twiddles_dtype]);
//
//    plan->twiddles = (plan + 1);
//    plan->n = n;
//    plan->twiddles_dtype = twiddles_dtype;
//
//    for (int twiddle_n = 2; twiddle_n <= n; twiddle_n <<= 1) {
//        const int twiddle_n2 = twiddle_n >> 1;
//        // Todo: a lot of storage can be saved by exploiting the periodic properties of sin/cos
//        for (int k = 0; k < twiddle_n2; ++k) {
//            const f64 theta = (-2. * M_PI * k) / (f64) (twiddle_n);
//            ((f64 *) plan->twiddles)[(2 * (twiddle_n2 - 1)) + (2 * k)] = cos(theta);
//            ((f64 *) plan->twiddles)[(2 * (twiddle_n2 - 1)) + (2 * k) + 1] = sin(theta);
//        }
//    }

    // Allocate a work array
//    ctx->fft_work = dsc_tensor_1d(ctx, "", C64, n);
//    ctx->fft_plan = plan;
//
//    // Todo: refactor the whole workers handling stuff
//    const int n_cpus = get_nprocs();
//    if (n_workers > n_cpus) {
//        DSC_LOG_INFO("n_workers=%d > n_cpus=%d, the actual number of workers will be limited to n_cpus", n_workers, n_cpus);
//        n_workers = n_cpus;
//    } else if (n_workers < 0) {
//        n_workers = (int) (n_cpus * 0.5);
//    }
//
//    ctx->n_workers = n_workers;
//
//    if (n_workers > 1) {
//        ctx->fft_workers = (dsc_worker *) malloc((n_workers - 1) * sizeof(dsc_worker));
//        // If we can, bind threads to even cores
//        const int restrict_even = n_workers <= (int) (n_cpus * 0.5) ? 2 : 1;
//        for (int i = 1; i < n_workers; ++i) {
//            dsc_worker *worker = &ctx->fft_workers[i - 1];
//
//            worker->work = dsc_tensor_1d(ctx, "", C64, n);
//            worker->plan = plan;
//
//            pthread_mutex_init(&worker->mtx, nullptr);
//            pthread_cond_init(&worker->cond, nullptr);
//            worker->has_work = false;
//            pthread_create(&worker->id, nullptr, fft_worker_thread, worker);
//
//            // Set affinity
//            cpu_set_t cpu_set{};
//            CPU_ZERO(&cpu_set);
//            CPU_SET(i * restrict_even, &cpu_set);
//            pthread_setaffinity_np(worker->id, sizeof(cpu_set_t), &cpu_set);
//        }
//    }
//
//    // Set affinity for the main worker
//    cpu_set_t cpu_set{};
//    CPU_ZERO(&cpu_set);
//    CPU_SET(0, &cpu_set);
//    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpu_set);
}

void dsc_ctx_free(dsc_ctx *ctx) noexcept {
    DSC_LOG_INFO("freeing context %p of %ldMB",
                 (void *) ctx,
                 (usize) DSC_B_TO_MB(ctx->buffer->nb)
    );

//    if (ctx->n_workers > 1) {
//        for (int i = 1; i < ctx->n_workers; ++i) {
//            dsc_worker *worker = &ctx->fft_workers[i - 1];
//            pthread_mutex_lock(&worker->mtx);
//
//            worker->task.op = DONE;
//            worker->has_work = true;
//
//            pthread_cond_signal(&worker->cond);
//            pthread_mutex_unlock(&worker->mtx);
//        }
//
//        for (int i = 1; i < ctx->n_workers; ++i) {
//            dsc_worker *worker = &ctx->fft_workers[i - 1];
//            pthread_join(worker->id, nullptr);
//            pthread_mutex_destroy(&worker->mtx);
//            pthread_cond_destroy(&worker->cond);
//        }
//
//        free(ctx->fft_workers);
//    }

    free(ctx->buffer);
    free(ctx);
}

void dsc_ctx_clear(dsc_ctx *ctx) noexcept {
    DSC_LOG_DEBUG("clearing context %p: mem_size=%ldMB n_objs=%d fft_plans=%d",
                  (void *)ctx,
                  (usize) DSC_B_TO_MB(ctx->buffer->nb),
                  ctx->buffer->n_objs,
                  ctx->buffer->n_plans
    );
    ctx->buffer->last = nullptr;
    ctx->buffer->n_objs = 0;
    // Set all the current plans to nullptr
    for (int i = 0; i < ctx->buffer->n_plans; ++i)
        ctx->buffer->plans[i] = nullptr;

    ctx->buffer->n_plans = 0;
}


dsc_tensor *dsc_new_tensor(dsc_ctx *ctx,
                           const int n_dim,
                           const int *shape,
                           const dsc_dtype dtype) noexcept {
    DSC_ASSERT((unsigned) n_dim <= DSC_MAX_DIMS);

    int ne = 1;
    for (int i = 0; i < n_dim; ++i) ne *= shape[i];

    const usize mem_needed = sizeof(dsc_tensor) + ne * DSC_DTYPE_SIZE[dtype];

    dsc_buffer *buff = ctx->buffer;

    // Allocate the actual tensor
    dsc_tensor *new_tensor = dsc_obj_alloc<dsc_tensor>(buff, mem_needed);

    new_tensor->dtype = dtype;

    new_tensor->ne = ne;
    new_tensor->n_dim = n_dim;

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


    new_tensor->data = (new_tensor + 1);

    DSC_LOG_DEBUG("n_dim=%d shape=[%d, %d, %d, %d] stride=[%d, %d, %d, %d] dtype=%s",
                  n_dim, new_tensor->shape[0], new_tensor->shape[1], new_tensor->shape[2],
                  new_tensor->shape[3], new_tensor->stride[0], new_tensor->stride[1], new_tensor->stride[2],
                  new_tensor->stride[3], DSC_DTYPE_NAMES[dtype]
    );

    return new_tensor;
}

extern dsc_tensor *dsc_tensor_1d(dsc_ctx *ctx, const dsc_dtype dtype,
                                 const int dim1) noexcept {
    const int shape[DSC_MAX_DIMS] = {dim1};
    return dsc_new_tensor(ctx, 1, shape, dtype);
}

extern dsc_tensor *dsc_tensor_2d(dsc_ctx *ctx, const dsc_dtype dtype,
                                 const int dim1, const int dim2) noexcept {
    const int shape[DSC_MAX_DIMS] = {dim1, dim2};
    return dsc_new_tensor(ctx, 2, shape, dtype);
}

extern dsc_tensor *dsc_tensor_3d(dsc_ctx *ctx, const dsc_dtype dtype,
                                 const int dim1, const int dim2,
                                 const int dim3) noexcept {
    const int shape[DSC_MAX_DIMS] = {dim1, dim2, dim3};
    return dsc_new_tensor(ctx, 3, shape, dtype);
}

extern dsc_tensor *dsc_tensor_4d(dsc_ctx *ctx, const dsc_dtype dtype,
                                 const int dim1, const int dim2,
                                 const int dim3, const int dim4) noexcept {
    const int shape[DSC_MAX_DIMS] = {dim1, dim2, dim3, dim4};
    return dsc_new_tensor(ctx, 4, shape, dtype);
}

dsc_tensor *dsc_arange(dsc_ctx *ctx,
                       const int n,
                       const dsc_dtype dtype) noexcept {
    dsc_tensor *out = dsc_tensor_1d(ctx, dtype, n);
    switch (dtype) {
        case dsc_dtype::F32:
            assign_op<f32>(out, {}, 1);
            break;
        case dsc_dtype::F64:
            assign_op<f64>(out, {}, 1);
            break;
        case dsc_dtype::C32:
            assign_op<c32>(out, dsc_complex(c32, 0, 0), dsc_complex(c32, 1, 0));
            break;
        case dsc_dtype::C64:
            assign_op<c64>(out, dsc_complex(c64, 0, 0), dsc_complex(c64, 1, 0));
            break;
        default:
            DSC_LOG_ERR("unknown dtype %d", dtype);
            break;
    }
    return out;
}

dsc_tensor *dsc_log_space_f32(dsc_ctx *ctx, const f32 start, const f32 stop,
                              const int n, const f32 base) noexcept {
    return dsc_log_space(ctx, start, stop, n, base);
}

dsc_tensor *dsc_log_space_f64(dsc_ctx *ctx, const f64 start, const f64 stop,
                              const int n, const f64 base) noexcept {
    return dsc_log_space(ctx, start, stop, n, base);
}

dsc_tensor *dsc_interp1d_f32(dsc_ctx *ctx, const dsc_tensor *x,
                             const dsc_tensor *y, const dsc_tensor *xp,
                             const f32 left, const f32 right) noexcept {
    return dsc_interp1d(ctx, x, y, xp, left, right);
}

dsc_tensor *dsc_interp1d_f64(dsc_ctx *ctx, const dsc_tensor *x,
                             const dsc_tensor *y, const dsc_tensor *xp,
                             const f64 left, const f64 right) noexcept {
    return dsc_interp1d(ctx, x, y, xp, left, right);
}

dsc_tensor *dsc_cast(dsc_ctx *ctx, const dsc_dtype new_dtype,
                     dsc_tensor *DSC_RESTRICT x) noexcept {
    if (x->dtype == new_dtype)
        return x;

    dsc_tensor *out = dsc_new_tensor(ctx, x->n_dim, x->shape, new_dtype);
    copy(x, out);

    return out;
}

dsc_tensor *dsc_add(dsc_ctx *ctx,
                    dsc_tensor *DSC_RESTRICT xa,
                    dsc_tensor *DSC_RESTRICT xb,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    validate_binary_params();

    binary_op(xa, xb, out, add_op());

    return out;
}

dsc_tensor *dsc_sub(dsc_ctx *ctx,
                    dsc_tensor *DSC_RESTRICT xa,
                    dsc_tensor *DSC_RESTRICT xb,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    validate_binary_params();

    binary_op(xa, xb, out, sub_op());

    return out;
}

dsc_tensor *dsc_mul(dsc_ctx *ctx,
                    dsc_tensor *DSC_RESTRICT xa,
                    dsc_tensor *DSC_RESTRICT xb,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    validate_binary_params();

    binary_op(xa, xb, out, mul_op());

    return out;
}

dsc_tensor *dsc_div(dsc_ctx *ctx,
                    dsc_tensor *DSC_RESTRICT xa,
                    dsc_tensor *DSC_RESTRICT xb,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    validate_binary_params();

    binary_op(xa, xb, out, div_op());

    return out;
}

CONST_OP_IMPL(addc, f32)
CONST_OP_IMPL(addc, f64)
CONST_OP_IMPL(addc, c32)
CONST_OP_IMPL(addc, c64)

CONST_OP_IMPL(subc, f32)
CONST_OP_IMPL(subc, f64)
CONST_OP_IMPL(subc, c32)
CONST_OP_IMPL(subc, c64)

CONST_OP_IMPL(mulc, f32)
CONST_OP_IMPL(mulc, f64)
CONST_OP_IMPL(mulc, c32)
CONST_OP_IMPL(mulc, c64)

CONST_OP_IMPL(divc, f32)
CONST_OP_IMPL(divc, f64)
CONST_OP_IMPL(divc, c32)
CONST_OP_IMPL(divc, c64)


dsc_tensor *dsc_cos(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    validate_unary_params();

    unary_op(x, out, cos_op());

    return out;
}


dsc_tensor *dsc_sin(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out) noexcept {
    validate_unary_params();
    
    unary_op(x, out, sin_op());

    return out;
}

dsc_tensor *dsc_fft(dsc_ctx *ctx,
                    const dsc_tensor *DSC_RESTRICT x,
                    dsc_tensor *DSC_RESTRICT out,
                    int n,
                    const int axis) noexcept {
    // Find N

    // Get the plan

    // --> Parallel START <--
    // Copy the desired axis of X over to a temp buffer (or out if we can go in-place?)

    // Perform the FFT

    // Write the result back (or not if we are working in-place?)
    // --> Parallel STOP <--

    // Done!

    // Todo: if x is not complex we have to cast it to complex before performing the FFT
    DSC_ASSERT(x != nullptr);

    const int axis_idx = dsc_tensor_dim(x, axis);
    DSC_ASSERT(axis_idx < DSC_MAX_DIMS);

    const int x_n = x->shape[axis_idx];
    const int axis_n = dsc_fft_best_n(x_n);
    if (n > 0) {
        n = dsc_fft_best_n(n);
        n = DSC_MAX(n, axis_n);
    } else {
        n = axis_n;
    }

    DSC_LOG_DEBUG("performing FFT of length %d over axis %d with size %d",
                  n, axis_idx, x->shape[axis_idx]);

    dsc_fft_plan *plan = dsc_plan_fft(ctx, n, x->dtype);

    dsc_tensor *buf = dsc_tensor_1d(ctx, x->dtype, n);

    void *DSC_RESTRICT x_data = ((byte *) x->data) + (x->stride[axis_idx] * DSC_DTYPE_SIZE[x->dtype]);
    if (x_n > n) {
        // Crop
        memcpy(buf->data, x_data, n * DSC_DTYPE_SIZE[x->dtype]);
    } else {
        // Pad
        const usize nb = x_n * DSC_DTYPE_SIZE[x->dtype];
        memcpy(buf->data, x_data, nb);
        memset(((byte *)buf->data) + nb, 0, (n - x_n) * DSC_DTYPE_SIZE[x->dtype]);
    }



//    // Todo: handle axis!
//    DSC_UNUSED(axis);
//
//    DSC_ASSERT(ctx->fft_plan != nullptr);
//    DSC_ASSERT(x->dtype == C64);
//    DSC_ASSERT(x->n_dim == 2);
//
//    std::atomic_int progress{1};
//
//    // Todo: implement getter
//    const int n = x->shape[3];
//    const int m = x->shape[2];
//    const int m_work = m / ctx->n_workers;
//    const int leftover_m = m - m_work * (ctx->n_workers - 1);
//
//    c64 *x_data = (c64 *) x->data;
//
//    for (int i = 1; i < ctx->n_workers; ++i) {
//        dsc_worker *worker = &ctx->fft_workers[i - 1];
//        pthread_mutex_lock(&worker->mtx);
//
//        dsc_task *t = &worker->task;
//        t->x = &x_data[(i - 1) * m_work * n];
//        t->n_times = m_work;
//        t->progress_counter = &progress;
//        t->op = FFT;
//        worker->has_work = true;
//
//        pthread_cond_signal(&worker->cond);
//        pthread_mutex_unlock(&worker->mtx);
//    }
//
//    // Do the leftover work on the main thread
//    for (int i = 0; i < leftover_m; ++i) {
//        dsc_internal_fft(
//                &x_data[((ctx->n_workers - 1) * m_work + i) * n],
//                (c64 *) ctx->fft_work->data,
//                (f64 *) ctx->fft_plan->twiddles,
//                n
//        );
//    }
//
//    // Wait for the other threads
//    while (progress.load() != ctx->n_workers)
//        ;
//
//    return x;
}

dsc_tensor *dsc_ifft(dsc_ctx *ctx,
                     const dsc_tensor *DSC_RESTRICT x,
                     dsc_tensor *DSC_RESTRICT out,
                     const int n,
                     const int axis) noexcept {
//    // Todo: handle axis!
//    DSC_UNUSED(axis);
//
//    DSC_ASSERT(ctx->fft_plan != nullptr);
//    DSC_ASSERT(x->dtype == C64);
//    DSC_ASSERT(x->n_dim == 2);
//
//    std::atomic_int progress{1};
//
//    // Todo: implement getter
//    const int n = x->shape[3];
//    const int m = x->shape[2];
//    const int m_work = m / ctx->n_workers;
//    const int leftover_m = m - m_work * (ctx->n_workers - 1);
//
//    c64 *x_data = (c64 *) x->data;
//
//    for (int i = 1; i < ctx->n_workers; ++i) {
//        dsc_worker *worker = &ctx->fft_workers[i - 1];
//        pthread_mutex_lock(&worker->mtx);
//
//        dsc_task *t = &worker->task;
//        t->x = &x_data[(i - 1) * m_work * n];
//        t->n_times = m_work;
//        t->progress_counter = &progress;
//        t->op = IFFT;
//        worker->has_work = true;
//
//        pthread_cond_signal(&worker->cond);
//        pthread_mutex_unlock(&worker->mtx);
//    }
//
//    // Do the leftover work on the main thread
//    for (int i = 0; i < leftover_m; ++i) {
//        dsc_internal_ifft(
//                &x_data[((ctx->n_workers - 1) * m_work + i) * n],
//                (c64 *) ctx->fft_work->data,
//                (f64 *) ctx->fft_plan->twiddles,
//                n
//        );
//    }
//
//    // Wait for the other threads
//    while (progress.load() != ctx->n_workers)
//        ;
//
//    return x;
}