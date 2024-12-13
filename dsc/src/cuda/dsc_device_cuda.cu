// Copyright (c) 2024, Christian Gilli <christian.gilli@dspcraft.com>
// All rights reserved.
//
// This code is licensed under the terms of the 3-clause BSD license
// (https://opensource.org/license/bsd-3-clause).

#include "dsc_device.h"
#include "cuda/dsc_cuda.h"

#define DSC_DEVICE_CUDA_ALIGN ((usize) 1024)

static DSC_CUDA_KERNEL void k_init_random(curandState *state) {
    DSC_CUDA_TID();
    curand_init(clock64(), tid, 0, &state[tid]);
}

static void cuda_dispose(dsc_device *dev) {
    DSC_CUDA_FAIL_ON_ERROR(cudaFree(dev->device_mem));

    const dsc_cuda_dev_info *info = (dsc_cuda_dev_info *) dev->extra_info;

    DSC_LOG_INFO("%s:%d device (%s) disposed",
                 DSC_DEVICE_NAMES[dev->type],
                 info->dev_idx,
                 info->name);
}

static void cuda_memcpy(void *dst, const void *src, const usize nb, const dsc_copy_direction direction) {
    DSC_CUDA_FAIL_ON_ERROR(cudaMemcpy(dst, src, nb, direction == FROM_DEVICE ? cudaMemcpyDeviceToHost : cudaMemcpyHostToDevice));
}

dsc_device *dsc_cuda_device(const usize mem_size, const int cuda_dev) {
    static dsc_cuda_dev_info extra = {
        .dev_idx = cuda_dev,
    };

    static dsc_device dev = {
        .extra_info = &extra,
        .mem_size = DSC_ALIGN(mem_size, DSC_DEVICE_CUDA_ALIGN),
        .used_mem = 0,
        .type = CUDA,
        .dispose = cuda_dispose,
        .memcpy = cuda_memcpy
    };

    DSC_CUDA_FAIL_ON_ERROR(cudaSetDevice(cuda_dev));

    dsc_cuda_dev_name(cuda_dev, extra.name);

    DSC_CUDA_FAIL_ON_ERROR(cudaMalloc(&extra.randState, DSC_CUDA_DEFAULT_THREADS_PER_BLOCK * sizeof(curandState)));

    k_init_random<<<1, DSC_CUDA_DEFAULT_THREADS_PER_BLOCK>>>(extra.randState);

    DSC_CUDA_FAIL_ON_ERROR(cudaDeviceSynchronize());

    DSC_CUDA_FAIL_ON_ERROR(cudaMalloc(&dev.device_mem, dev.mem_size));

    DSC_LOG_INFO("%s:%d device (%s) initialized with a buffer of %ldMB (total: %ldMB)",
                 DSC_DEVICE_NAMES[dev.type],
                 cuda_dev,
                 extra.name,
                 (usize) DSC_B_TO_MB(dev.mem_size),
                 (usize) DSC_B_TO_MB(dsc_cuda_dev_mem(cuda_dev)));

    return &dev;
}