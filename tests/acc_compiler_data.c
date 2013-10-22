/*!
 *  \file tests/acc_compiler_data.c
 *
 * This file provides default version of the code that should be generated by the compiler for the runtime.
 *
 */

#include "OpenACC/libopenacc-internal.h"

#ifndef ACC_RUNTIME_DIR
# error "Need OpenACC runtime directory."
#endif
#ifndef ACC_RUNTIME_OPENCL
# define ACC_RUNTIME_OPENCL "lib/libopenacc.cl"
#endif

const char * ocl_files[1] = {
  "empty.cl"
};

acc_compiler_data_t compiler_data = {0, 0, 1, ocl_files , ACC_RUNTIME_DIR , ACC_RUNTIME_OPENCL };

