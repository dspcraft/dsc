#pragma once

#include "dsc.h"

struct dsc_axis_iterator {
    dsc_axis_iterator(const dsc_tensor *x, const int axis,
                      const int axis_n) noexcept :
            shape_(x->shape), stride_(x->stride),
            axis_(axis), axis_n_(axis_n < x->shape[axis] ? axis_n : x->shape[axis]) {

    }

    DSC_INLINE void next() noexcept {
        if (++idx_[axis_] < axis_n_) [[likely]] {
            index_ += stride_[axis_];
            return;
        }

        index_ -= (idx_[axis_] - 1) * stride_[axis_];
        idx_[axis_] = 0;

        bool still_left = false;
        for (int i = DSC_MAX_DIMS - 1; i >= 0; --i) {
            if (i == axis_)
                continue;

            if (++idx_[i] < shape_[i]) {
                index_ += stride_[i];
                still_left = true;
                break;
            }

            // Rollover this dimension
            index_ -= (idx_[i] - 1) * stride_[i];
            idx_[i] = 0;
            // If this is the first dimension, and it rolls over we are done
            end_ = i == 0;
        }
        if (axis_ == 0 && !still_left)
            end_ = true;
    }

    DSC_INLINE int index() const noexcept {
        return index_;
    }

    DSC_INLINE bool has_next() const noexcept {
        return !end_;
    }

private:
    int index_ = 0;
    int idx_[DSC_MAX_DIMS]{};
    const int *shape_;
    const int *stride_;
    const int axis_;
    const int axis_n_;
    bool end_ = false;
};

struct dsc_broadcast_iterator {
    dsc_broadcast_iterator(const dsc_tensor *x, const int *out_shape) noexcept :
            x_shape_(x->shape), x_stride_(x->stride), out_shape_(out_shape) {
        for (int i = 0; i < DSC_MAX_DIMS; ++i) {
            x_broadcast_stride_[i] = x_shape_[i] < out_shape_[i] ? 0 : x_stride_[i];
        }
    }

    DSC_INLINE void next() noexcept {
        for (int i = DSC_MAX_DIMS - 1; i >= 0; --i) {
            if (++x_idx_[i] < out_shape_[i]) [[likely]] {
                index_ += x_broadcast_stride_[i];
                return;
            }
            // Rollover this dimension
            index_ -= (x_idx_[i] - 1) * x_broadcast_stride_[i];
            x_idx_[i] = 0;
        }
    }

    DSC_INLINE int index() const noexcept {
        return index_;
    }

private:
    int index_ = 0;
    const int *x_shape_, *x_stride_, *out_shape_;
    int x_broadcast_stride_[DSC_MAX_DIMS]{}, x_idx_[DSC_MAX_DIMS]{};
};