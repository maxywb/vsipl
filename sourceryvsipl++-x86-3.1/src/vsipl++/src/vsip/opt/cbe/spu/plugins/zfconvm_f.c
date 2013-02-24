/* Copyright (c) 2009 by CodeSourcery.  All rights reserved.

   This file is available for license from CodeSourcery, Inc. under the terms
   of a commercial license and under the GPL.  It is not part of the VSIPL++
   reference implementation and is not available under the BSD license.
*/

#include <stdio.h>
#include <alf_accel.h>
#include <assert.h>
#include <spu_mfcio.h>
#include <cml.h>
#include <cml_core.h>
#include <vsip/opt/cbe/fconv_params.h>
#include <lwp_kernel.h>

#define MAX_DT_CHUNK 16*1024
#define MAX_SIZE VSIP_IMPL_MAX_FCONV_SPLIT_SIZE

// FFT object memory.
// (See `cml_fft1d_size_f`)
char fft_obj_mem[CML_INCREASE_TO_SIMD_SIZE(sizeof(fft1d_f)) +
		 MAX_SIZE*sizeof(float)];
// FFT temporary buffer memory.
// (See `cml_zzfft1d_buf_size_f()`)
char fft_buf[MAX_SIZE*2*sizeof(float)];

int
input(lwp_functions* pf,
      void *params, 
      void *entries, 
      unsigned int iter,
      unsigned int total)
{
  (void)total;

  Fastconv_split_params* fc = (Fastconv_split_params *)params;
  alf_data_addr64_t ea;

  (pf->f_dtl_begin)(entries, ALF_BUF_OVL_IN, 0);
  ea = fc->ea_input_re + iter * fc->input_stride * sizeof(float);
  (pf->f_dtl_entry_add)(entries, fc->elements, ALF_DATA_FLOAT, ea);
  ea = fc->ea_input_im + iter * fc->input_stride * sizeof(float);
  (pf->f_dtl_entry_add)(entries, fc->elements, ALF_DATA_FLOAT, ea);
  ea = fc->ea_kernel_re + iter * fc->kernel_stride * sizeof(float);
  (pf->f_dtl_entry_add)(entries, fc->elements, ALF_DATA_FLOAT, ea);
  ea = fc->ea_kernel_im + iter * fc->kernel_stride * sizeof(float);
  (pf->f_dtl_entry_add)(entries, fc->elements, ALF_DATA_FLOAT, ea);
  (pf->f_dtl_end)(entries);
  return 0;
}

int
output(lwp_functions* pf,
       void *params, 
       void *entries, 
       unsigned int iter, 
       unsigned int total)
{
  (void)total;

  Fastconv_split_params* fc = (Fastconv_split_params *)params;
  alf_data_addr64_t ea;

  (pf->f_dtl_begin)(entries, ALF_BUF_OVL_OUT, 0);
  ea = fc->ea_output_re + iter * fc->output_stride * sizeof(float);
  (pf->f_dtl_entry_add)(entries, fc->elements, ALF_DATA_FLOAT, ea);
  ea = fc->ea_output_im + iter * fc->output_stride * sizeof(float);
  (pf->f_dtl_entry_add)(entries, fc->elements, ALF_DATA_FLOAT, ea);
  (pf->f_dtl_end)(entries);
  return 0;
}

int
kernel(lwp_functions* pf,
       void *params,
       void *inout,
       unsigned int iter,
       unsigned int total)
{
  // Instance-id.  Used to determine when new coefficients must be loaded.
  static unsigned int instance_id = 0;

  // Persistent FFT object, only rebuilt if the new size doesn't match the old.
  static size_t current_size = 0;
  static fft1d_f *fft = 0;

  Fastconv_split_params* fc = (Fastconv_split_params *)params;
  unsigned int fft_size = fc->elements;
  assert(fft_size <= MAX_SIZE);

  (void)total;

  float *data_re = (float*)inout;
  float *data_im = (float*)inout + 1 * fft_size;

  float *coeff_re = (float*)inout + 2 * fft_size;
  float *coeff_im = (float*)inout + 3 * fft_size;

  // If we aren't set up for the new FFT size, regenerate the
  // FFT object (incl. twiddle factors).
  if (iter == 0 && fft_size != current_size)
  {
    int rt = cml_fft1d_setup_f(&fft, CML_FFT_CC, fft_size, fft_obj_mem);
    assert(rt && fft);
    current_size = fft_size;
  }

  if (fc->transform_kernel)
  {
    // Perform the forward FFT on the kernel, in place.  This only need 
    // be done once -- subsequent calls will utilize the same kernel.
    cml_zzfft1d_ip_f(fft, coeff_re, coeff_im, CML_FFT_FWD, fft_buf);
  }

  // Switch to frequency space
  cml_zzfft1d_ip_f(fft, data_re, data_im, CML_FFT_FWD, fft_buf);
  // Perform convolution -- now a straight multiplication
  cml_zvmul1_f(coeff_re, coeff_im, data_re, data_im, data_re, data_im, fft_size);
  // Revert back the time domain
  cml_zzfft1d_ip_f(fft, data_re, data_im, CML_FFT_INV, fft_buf);
  // Scale by 1/n.
  cml_core_rzsvmul1_f(1.f / fft_size, data_re, data_im, data_re, data_im, fft_size);
  return 0;
}