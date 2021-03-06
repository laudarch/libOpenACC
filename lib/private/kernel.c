
#include "OpenACC/internal/region.h"
#include "OpenACC/internal/kernel.h"
#include "OpenACC/internal/loop.h"
#include "OpenACC/device/host-context.h"
#include "OpenACC/private/debug.h"

#include "OpenACC/private/runtime.h"
#include "OpenACC/private/memory.h"

#include "OpenACC/utils/profiling.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <inttypes.h>

#include <assert.h>

#ifndef DBG_KERNEL
# define DBG_KERNEL 0
#endif

typedef struct acc_kernel_t_ * acc_kernel_t;
typedef struct acc_region_t_ * acc_region_t;
typedef struct acc_context_t_ * acc_context_t;

acc_kernel_t acc_build_kernel(struct acc_kernel_desc_t_ * kernel) {
  acc_init_once();

  acc_kernel_t result = (acc_kernel_t)malloc(sizeof(struct acc_kernel_t_));

  result->desc = kernel;

  result->param_ptrs   = (  void **)malloc(kernel->num_params  * sizeof(  void *));
  result->scalar_ptrs  = (  void **)malloc(kernel->num_scalars * sizeof(  void *));
  result->data_ptrs    = (d_void **)malloc(kernel->num_datas   * sizeof(d_void *));
  result->data_size    = ( size_t *)malloc(kernel->num_datas   * sizeof(  size_t));
  result->private_ptrs = (d_void **)malloc(kernel->num_datas   * sizeof(d_void *));
  result->private_size = ( size_t *)malloc(kernel->num_datas   * sizeof(  size_t));

  result->loops = malloc(kernel->num_loops * sizeof(struct acc_loop_t_));
  unsigned i;
  for (i = 0; i < kernel->num_loops; i++) {
    result->loops[i].lower = 0;
    result->loops[i].upper = 0;
    result->loops[i].stride = 0;
  }

  return result;
}

void acc_enqueue_kernel(acc_region_t region, acc_kernel_t kernel) {
#if DBG_KERNEL
  printf("[debug] acc_enqueue_kernel #%zd\n", kernel->desc->id);
#endif

  size_t i, j, k, l, dev_idx;
  for (dev_idx = 0; dev_idx < region->desc->num_devices; dev_idx++) {
    assert(region->devices[dev_idx].num_gang[0] > 0);
    assert(region->devices[dev_idx].num_gang[1] > 0);
    assert(region->devices[dev_idx].num_gang[2] > 0);
    assert(region->devices[dev_idx].num_worker[0] > 0);
    assert(region->devices[dev_idx].num_worker[1] > 0);
    assert(region->devices[dev_idx].num_worker[2] > 0);
    assert(region->devices[dev_idx].vector_length > 0);

    for (i = 0; i < kernel->desc->num_loops; i++)
      assert(kernel->loops[i].stride != 0);

    size_t device_idx = region->devices[dev_idx].device_idx;
    assert(acc_runtime.opencl_data->devices_data[device_idx] != NULL);

    // Create a default context
    acc_context_t context;

    // Look for a matching ‭version of the kernel, fill the context according to the selected version
    cl_kernel ocl_kernel = acc_build_ocl_kernel(region, kernel, &context, device_idx);

    cl_int status;
    cl_uint idx = 0;

    // Set params kernel arguments 
    for (i = 0; i < kernel->desc->num_params; i++) {
      size_t size_param = region->desc->size_params[kernel->desc->param_ids[i]];
      status = clSetKernelArg(ocl_kernel, idx, size_param, kernel->param_ptrs[i]);
      if (status != CL_SUCCESS) {
        const char * status_str = acc_ocl_status_to_char(status);
        printf("[fatal]   clSetKernelArg return %s for region[%zd].kernel[%zd] argument %u (scalar #%zd).\n",
                  status_str, region->desc->id, kernel->desc->id, idx, i
              );
        exit(-1); /// \todo error code
      }
      idx++;
    }

    // Set scalar kernel arguments 
    for (i = 0; i < kernel->desc->num_scalars; i++) {
      size_t size_scalar = region->desc->size_scalars[kernel->desc->scalar_ids[i]];
      status = clSetKernelArg(ocl_kernel, idx, size_scalar, kernel->scalar_ptrs[i]);
      if (status != CL_SUCCESS) {
        const char * status_str = acc_ocl_status_to_char(status);
        printf("[fatal]   clSetKernelArg return %s for region[%zd].kernel[%zd] argument %u (scalar #%zd).\n",
                  status_str, region->desc->id, kernel->desc->id, idx, i
              );
        exit(-1); /// \todo error code
      }
      idx++;
    }

    // Set data kernel argument
    for (i = 0; i < kernel->desc->num_datas; i++) {
      assert(kernel->data_ptrs[i] != NULL);

      h_void * h_data_ptr = kernel->data_ptrs[i];
      size_t n = kernel->data_size[i];

      acc_distributed_data(region, device_idx, &h_data_ptr, &n);

      d_void * d_data_ptr = acc_deviceptr_(device_idx, h_data_ptr);
      if (d_data_ptr == NULL) {
        printf("[fatal]   Cannot find device pointer for %016" PRIxPTR " (%016" PRIxPTR ") on device #%zd for region[%zd].kernel[%zd] argument %u (data #%zd).\n",
                  (uintptr_t)h_data_ptr, (uintptr_t)kernel->data_ptrs[i], device_idx, region->desc->id, kernel->desc->id, idx, i);
        exit(-1); /// \todo error code
      }
      status = clSetKernelArg(ocl_kernel, idx, sizeof(cl_mem), &(d_data_ptr));
      if (status != CL_SUCCESS) {
        const char * status_str = acc_ocl_status_to_char(status);
        printf("[fatal]   clSetKernelArg return %s for region[%zd].kernel[%zd] argument %u (data #%zd).\n",
                  status_str, region->desc->id, kernel->desc->id, idx, i
              );
        exit(-1); /// \todo error code
      }
      idx++;

      // if data is distributed need to provide the offset
      for (j = 0; j < region->desc->num_distributed_data; j++)
        if (kernel->desc->data_ids[i] == region->desc->distributed_data[j].id)
          break;
      if (j < region->desc->num_distributed_data) {
#if DBG_KERNEL
        printf("[debug]   region[%zd].kernel[%zd] on device #%zd  data #%zd is distributed.\n",
                    region->desc->id, kernel->desc->id, device_idx, i
                );
#endif
        assert( region->desc->distributed_data[j].mode == e_contiguous &&
                region->desc->distributed_data[j].nbr_dev == region->desc->num_devices &&
                region->desc->distributed_data[j].portions != NULL
              );

        for (k = 0; k < region->desc->num_devices; k++)
          if (region->devices[k].device_idx == device_idx)
            break;
        assert(k < region->desc->num_devices);

        unsigned sum_portions = 0;
        unsigned prev_portion = 0;
        for (l = 0; l < region->desc->num_devices; l++) {
          sum_portions += region->desc->distributed_data[j].portions[l];
          if (l < k)
            prev_portion += region->desc->distributed_data[j].portions[l];
        };

        int offset = (region->data[kernel->desc->data_ids[i]].nbr_elements_dominant_dimension * prev_portion) / sum_portions;

#if DBG_KERNEL
        printf("[debug]       sum_portions = %d\n", sum_portions);
        printf("[debug]       prev_portion = %d\n", prev_portion);
        printf("[debug]       offset       = %d\n", offset);
#endif

        status = clSetKernelArg(ocl_kernel, idx, sizeof(int), &offset);
        if (status != CL_SUCCESS) {
          const char * status_str = acc_ocl_status_to_char(status);
          printf("[fatal]   clSetKernelArg return %s for region[%zd].kernel[%zd] argument %u: offset for distributed data %zd.\n",
                    status_str, region->desc->id, kernel->desc->id, idx, i
                );
          exit(-1); /// \todo error code
        }
        idx++;
      }
    }

    // Set private data kernel argument
    for (i = 0; i < kernel->desc->num_privates; i++) {
      status = clSetKernelArg(ocl_kernel, idx, kernel->private_size[i], NULL);
      if (status != CL_SUCCESS) {
        const char * status_str = acc_ocl_status_to_char(status);
        printf("[fatal]   clSetKernelArg return %s for region[%zd].kernel[%zd] argument %u (privatedata #%zd).\n",
                  status_str, region->desc->id, kernel->desc->id, idx, i
              );
        exit(-1); /// \todo error code
      }
      idx++;
    }

    // Allocate/copy context in constant memory \todo alloc only copy before launch with event wait
    cl_mem ocl_context = clCreateBuffer( acc_runtime.opencl_data->devices_data[device_idx]->context,
                                         CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                         sizeof(struct acc_context_t_) + 2 * (context->num_loops + context->num_tiles) * sizeof(long),
                                         context, &status );
    if (status != CL_SUCCESS) {
      const char * status_str = acc_ocl_status_to_char(status);
      printf("[fatal]   clCreateBuffer return %s for region[%zd].kernel[%zd] when call to build the kernel copy of context.\n",
                status_str, region->desc->id, kernel->desc->id
            );
      exit(-1); /// \todo error code
    }

    free(context); // Not needed anymore

    // Set context of the kernel
    status = clSetKernelArg(ocl_kernel, idx, sizeof(cl_mem), &ocl_context);
    if (status != CL_SUCCESS) {
      const char * status_str = acc_ocl_status_to_char(status);
      printf("[fatal]   clSetKernelArg return %s for region[%zd].kernel[%zd] argument %u (context).\n",
                status_str, region->desc->id, kernel->desc->id, idx, i
            );
      exit(-1); /// \todo error code
    }
    idx++;

    assert(acc_runtime.opencl_data->devices_data[device_idx]->command_queue != NULL);

    // Launch the kernel
    size_t work_dim = 0;
    if      (region->devices[dev_idx].num_gang[2] > 1 || region->devices[dev_idx].num_worker[2] > 1) work_dim = 3;
    else if (region->devices[dev_idx].num_gang[1] > 1 || region->devices[dev_idx].num_worker[1] > 1) work_dim = 2;
    else if (region->devices[dev_idx].num_gang[0] > 1 || region->devices[dev_idx].num_worker[0] > 1) work_dim = 1;
    assert(work_dim > 0);

    size_t global_work_size[3] = {
                                   region->devices[dev_idx].num_gang[0] * region->devices[dev_idx].num_worker[0],
                                   region->devices[dev_idx].num_gang[1] * region->devices[dev_idx].num_worker[1],
                                   region->devices[dev_idx].num_gang[2] * region->devices[dev_idx].num_worker[2]
                                 };
    size_t local_work_size[3] =  {
                                   region->devices[dev_idx].num_worker[0],
                                   region->devices[dev_idx].num_worker[1],
                                   region->devices[dev_idx].num_worker[2]
                                 };

#if DBG_KERNEL
    printf("[debug]              work_dim = %zd\n", work_dim);
    printf("[debug]   global_work_size[3] = {%zd,%zd,%zd} (= %zd)\n", global_work_size[0], global_work_size[1], global_work_size[2], global_work_size[0] * global_work_size[1] * global_work_size[2]);
    printf("[debug]    local_work_size[3] = {%zd,%zd,%zd} (= %zd)\n", local_work_size[0], local_work_size[1], local_work_size[2], local_work_size[0] * local_work_size[1] * local_work_size[2]);

    cl_ulong kernel_local_mem_size = 0;
    status = clGetKernelWorkGroupInfo(ocl_kernel, acc_runtime.opencl_data->devices[0][device_idx], CL_KERNEL_LOCAL_MEM_SIZE, sizeof(cl_ulong), &kernel_local_mem_size, NULL);
    printf("[debug]   kernel_local_mem_size = %lu\n", kernel_local_mem_size);

    size_t kernel_work_group_size = 0;
    status = clGetKernelWorkGroupInfo(ocl_kernel, acc_runtime.opencl_data->devices[0][device_idx], CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t), &kernel_work_group_size, NULL);
    printf("[debug]   kernel_work_group_size = %lu\n", kernel_work_group_size);
#endif

    cl_event event;

    status = clEnqueueNDRangeKernel(
      acc_runtime.opencl_data->devices_data[device_idx]->command_queue,
      ocl_kernel,
      /* cl_uint work_dim                  = */ work_dim,
      /* const size_t * global_work_offset = */ NULL,
      /* const size_t * global_work_size   = */ global_work_size,
      /* const size_t * local_work_size    = */ local_work_size,
      /* cl_uint num_events_in_wait_list   = */ 0,
      /* const cl_event * event_wait_list  = */ NULL,
      /* cl_event * event                  = */ &event
    );
    if (status != CL_SUCCESS) {
      const char * status_str = acc_ocl_status_to_char(status);
      printf("[fatal]   clEnqueueNDRangeKernel return %s for region[%zd].kernel[%zd].\n",
                status_str, region->desc->id, kernel->desc->id
            );
      assert(0); /// \todo error code
    }

    acc_profiling_register_kernel_launch(event, device_idx, region->desc->id, kernel->desc->id);

    clReleaseMemObject(ocl_context);

    clReleaseKernel(ocl_kernel);
  }
}

