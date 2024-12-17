// Copyright (c) 2024, Christian Gilli <christian.gilli@dspcraft.com>
// All rights reserved.
//
// This code is licensed under the terms of the 3-clause BSD license
// (https://opensource.org/license/bsd-3-clause).

#pragma once

#include "dsc.h"

#define DSC_MEMCPY_DIRECTIONS ((int) 3)

struct dsc_data_buffer {
    void *data;
    usize size;
    int refs;
};

struct dsc_free_node {
    void *data;
    dsc_free_node *next;
    usize size;
};

enum dsc_memcpy_dir : u8 {
    FROM_DEVICE,
    TO_DEVICE,
    ON_DEVICE
};

struct dsc_device {
    dsc_data_buffer used_nodes[DSC_MAX_OBJS];
    dsc_free_node free_nodes[DSC_MAX_OBJS];
    dsc_free_node *head;
    dsc_fft_plan fft_plans[DSC_MAX_FFT_PLANS];
    void *device_mem;

    // Extra device-specific infos
    void *extra_info;

    usize mem_size, used_mem;
    dsc_device_type type;

    void (*memcpy)  (void *dst, const void *src, usize nb, dsc_memcpy_dir dir);
    void (*dispose) (dsc_device *dev);
};

namespace {
DSC_INLINE dsc_free_node *find_best(dsc_device *dev,
                                    const usize required_size,
                                    dsc_free_node **prev) {
    dsc_free_node *node = dev->head;
    dsc_free_node *best = node->size >= required_size ? node : nullptr;
    dsc_free_node *prev_node = nullptr;

    while (node->next != nullptr) {
        if (node->next->size >= required_size &&
            (best == nullptr || best->size >= node->next->size)) {
            prev_node = node;
            best = node->next;
        }
        node = node->next;
    }

    *prev = prev_node;

    return best;
}

DSC_INLINE void node_insert(dsc_free_node **head,
                            dsc_free_node *prev,
                            dsc_free_node *to_insert) {
    if (prev == nullptr) {
        if (*head != nullptr) {
            to_insert->next = *head;
        }
        *head = to_insert;
    } else {
        if (prev->next == nullptr) {
            prev->next = to_insert;
            to_insert->next = nullptr;
        } else {
            to_insert->next = prev->next;
            prev->next = to_insert;
        }
    }
}

DSC_INLINE void node_remove(dsc_free_node **head,
                            dsc_free_node *prev,
                            dsc_free_node *to_remove) {
    if (prev == nullptr) {
        *head = to_remove->next;
    } else {
        prev->next = to_remove->next;
    }
}

DSC_INLINE dsc_free_node *next_free_node(dsc_device *dev) {
    for (int i = 0; i < DSC_MAX_OBJS; ++i) {
        if (dsc_free_node *bin = &dev->free_nodes[i];
            bin->data == nullptr && bin->size == 0) {
            return bin;
        }
    }

    return nullptr;
}
}

static DSC_MALLOC DSC_INLINE dsc_data_buffer *dsc_data_alloc(dsc_device *dev, const usize nb) {
    DSC_ASSERT(dev != nullptr);
    DSC_ASSERT(nb > 0);

    dsc_free_node *prev = nullptr;
    dsc_free_node *node = find_best(dev, nb, &prev);
    if (node == nullptr) {
        DSC_LOG_FATAL("error allocating %.2fKB on %s", DSC_B_TO_KB(nb), DSC_DEVICE_NAMES[dev->type]);
    }

    // What's the smallest size we need to accommodate on this device buffer? We can wrap single values
    // in tensors so for now the answer in the size of a single float32.
    if (const usize left = node->size - nb; left >= sizeof(f32)) {
        dsc_free_node *new_node = next_free_node(dev);
        node->size = nb;
        new_node->size = left;
        // The data for the new bin starts after the previous one
        new_node->data = (byte *) node->data + node->size;
        node_insert(&dev->head, node, new_node);
    }

    node_remove(&dev->head, prev, node);

    dsc_data_buffer *data_buf = nullptr;
    for (int i = 0; i < DSC_MAX_OBJS; ++i) {
        if (dsc_data_buffer *free_buf = &dev->used_nodes[i]; free_buf->data == nullptr) {
            data_buf = free_buf;
            break;
        }
    }
    if (!data_buf) {
        DSC_LOG_FATAL("can't allocate any more objects!");
    }

    data_buf->data = node->data;
    data_buf->refs = 1;
    data_buf->size = node->size;
    dev->used_mem += nb;

    return data_buf;
}

static DSC_INLINE void dsc_data_free(dsc_device *dev, dsc_data_buffer *ptr) {
    DSC_ASSERT(dev != nullptr);
    DSC_ASSERT(ptr != nullptr);
    DSC_ASSERT(ptr->refs > 0);

    ptr->refs--;

    if (ptr->refs > 0) return;

    DSC_LOG_DEBUG("%p will be freed", ptr);

    const uintptr_t ptr_addr = (uintptr_t) ptr->data;
    dsc_free_node *new_node = next_free_node(dev);

    dsc_free_node *node = dev->head, *prev = nullptr;
    while (node != nullptr) {
        if (const uintptr_t node_addr = (uintptr_t) node->data; ptr_addr < node_addr) {
            new_node->size = ptr->size;
            new_node->next = nullptr;
            new_node->data = ptr->data;
            node_insert(&dev->head, prev, new_node);
            break;
        }

        prev = node;
        node = node->next;
    }

    dev->used_mem -= new_node->size;

    // Coalescence
    if (new_node->next != nullptr &&
        (uintptr_t) ((byte *) new_node->data + new_node->size) == (uintptr_t) new_node->next->data) {
        new_node->size += new_node->next->size;
        node_remove(&dev->head, new_node, new_node->next);
    }

    if (prev != nullptr && prev->next != nullptr &&
        (uintptr_t) ((byte *) prev + prev->size) == (uintptr_t) new_node) {
        prev->size += new_node->size;
        node_remove(&dev->head, prev, new_node);
    }
}

extern dsc_device *dsc_cpu_device(usize mem_size);

extern dsc_device *dsc_cuda_device(usize mem_size, int cuda_dev);