// DCT calculation with fftw (real)
// Copyright(c)2006 A.G.Balakhnin aka Fizick
// See legal notice in Copying.txt for more information

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .



#include	"conc/CritSec.h"
#include "DCTFFTW.h"

//#define __INTEL_COMPILER_USE_INTRINSIC_PROTOTYPES 1
#include <emmintrin.h>
#include	<algorithm>
#include <cstdio>
#include <tuple>
#include <map>
#include "types.h"
#include <avisynth.h>
#include "def.h"


conc::Mutex	DCTFFTW::_fftw_mutex;



DCTFFTW::DCTFFTW(int _sizex, int _sizey, HINSTANCE hinstFFTW3, int _dctmode, int _pixelsize, int _bits_per_pixel, int cpu)
{
  fftwf_free_addr = (fftwf_free_proc)GetProcAddress(hinstFFTW3, "fftwf_free");
  if (!fftwf_free_addr) fftwf_free_addr = (fftwf_free_proc)GetProcAddress(hinstFFTW3, "fftw_free"); // ffw3 v3.5!!!

  fftwf_malloc_addr = (fftwf_malloc_proc)GetProcAddress(hinstFFTW3, "fftwf_malloc");
  if (!fftwf_malloc_addr) fftwf_malloc_addr = (fftwf_malloc_proc)GetProcAddress(hinstFFTW3, "fftw_malloc");

  fftwf_destroy_plan_addr = (fftwf_destroy_plan_proc)GetProcAddress(hinstFFTW3, "fftwf_destroy_plan");
  if (!fftwf_destroy_plan_addr) fftwf_destroy_plan_addr = (fftwf_destroy_plan_proc)GetProcAddress(hinstFFTW3, "fftw_destroy_plan");

  fftwf_plan_r2r_2d_addr = (fftwf_plan_r2r_2d_proc)GetProcAddress(hinstFFTW3, "fftwf_plan_r2r_2d");
  if (!fftwf_plan_r2r_2d_addr) fftwf_plan_r2r_2d_addr = (fftwf_plan_r2r_2d_proc)GetProcAddress(hinstFFTW3, "fftw_plan_r2r_2d");

  fftwf_execute_r2r_addr = (fftwf_execute_r2r_proc)GetProcAddress(hinstFFTW3, "fftwf_execute_r2r");
  if (!fftwf_execute_r2r_addr) fftwf_execute_r2r_addr = (fftwf_execute_r2r_proc)GetProcAddress(hinstFFTW3, "fftw_execute_r2r");

  // members of the DCTClass
  sizex = _sizex;
  sizey = _sizey;
  dctmode = _dctmode;
  pixelsize = _pixelsize;
  bits_per_pixel = _bits_per_pixel;

  bool isse_flag = true;
  arch_t arch;
  if ((((cpu & CPUF_AVX2) != 0) & isse_flag))
    arch = USE_AVX2;
  else if ((((cpu & CPUF_AVX) != 0) & isse_flag))
    arch = USE_AVX;
  else if ((((cpu & CPUF_SSE4_1) != 0) & isse_flag))
    arch = USE_SSE41;
  else if ((((cpu & CPUF_SSE2) != 0) & isse_flag))
    arch = USE_SSE2;
  /*  else if ((pixelsize == 1) && _isse_flag) // PF no MMX support
  arch = USE_MMX;*/
  else
    arch = NO_SIMD;

  // function selector
  bytesToFloatPROC = get_bytesToFloatPROC_function(sizex, sizey, pixelsize, arch);
  floatToBytesPROC = get_floatToBytesPROC_function(sizex, sizey, pixelsize, arch);

  int size2d = sizey*sizex;

  int cursize = 1;
  dctshift = 0;
  while (cursize < size2d)
  {
    dctshift++;
    cursize = (cursize << 1);
  }

  dctshift0 = dctshift + 2;

  // FFTW plan construction and destruction are not thread-safe.
  // http://www.fftw.org/fftw3_doc/Thread-safety.html#Thread-safety
  conc::CritSec	lock(_fftw_mutex);

  fSrc = (float *)fftwf_malloc_addr(sizeof(float) * size2d);
  fSrcDCT = (float *)fftwf_malloc_addr(sizeof(float) * size2d);

  dctplan = fftwf_plan_r2r_2d_addr(sizey, sizex, fSrc, fSrcDCT, FFTW_REDFT10, FFTW_REDFT10, FFTW_ESTIMATE); // direct fft 
}



DCTFFTW::~DCTFFTW()
{
  conc::CritSec	lock(_fftw_mutex);

  fftwf_destroy_plan_addr(dctplan);
  fftwf_free_addr(fSrc);
  fftwf_free_addr(fSrcDCT);
}

// put source data to real array for FFT
// see also DePanEstimate_fftw::frame_data2d
template<typename pixel_t>
void DCTFFTW::Bytes2Float_C(const unsigned char * srcp, int src_pitch, float * realdata)
{
  int floatpitch = sizex;
  int i, j;
  for (j = 0; j < sizey; j++)
  {
    for (i = 0; i < sizex; i += 1) // typical sizex is 16
    {
      realdata[i] = reinterpret_cast<const pixel_t *>(srcp)[i];
    }
    srcp += src_pitch;
    realdata += floatpitch;
  }
}

template<typename pixel_t, int nBlkSizeX>
void DCTFFTW::Bytes2Float_SSE2(const unsigned char * srcp8, int src_pitch, float * realdata)
{
  const pixel_t *srcp = reinterpret_cast<const pixel_t *>(srcp8);
  src_pitch /= sizeof(pixel_t);

  __m128i zero = _mm_setzero_si128();

  int floatpitch = nBlkSizeX;
  for (int y = 0; y < sizey; y++)
  {
    if (nBlkSizeX == 4) {
      // 4 pixels
      if (sizeof(pixel_t) == 1)
      {
        __m128i src = _mm_unpacklo_epi8(_mm_castps_si128(_mm_load_ss(reinterpret_cast<const float *>(srcp))), zero); // 4 bytes->4 words
        __m128i src_lo = _mm_unpacklo_epi16(src, zero);
        _mm_storeu_ps(realdata, _mm_cvtepi32_ps(src_lo));
      }
      else if (sizeof(pixel_t) == 2) {
        // uint16_t pixels
        __m128i src = _mm_loadl_epi64(reinterpret_cast<const __m128i *>(srcp));
        __m128i src_lo = _mm_unpacklo_epi16(src, zero);
        _mm_storeu_ps(realdata, _mm_cvtepi32_ps(src_lo));
      }
      else if (sizeof(pixel_t) == 4) {
        // float pixels
        __m128 src = _mm_loadu_ps(reinterpret_cast<const float *>(srcp));
        _mm_storeu_ps(realdata, src);
        // realdata[i] = reinterpret_cast<const pixel_t *>(srcp)[i];
      }
    }
    else {
      // 8 pixels at a time
      for (int x = 0; x < nBlkSizeX; x += 8)
      {
        if (sizeof(pixel_t) == 1)
        {
          __m128i src = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i *>(srcp + x)), zero); // 8 words
          __m128i src_lo = _mm_unpacklo_epi16(src, zero);
          _mm_storeu_ps(realdata + x, _mm_cvtepi32_ps(src_lo));
          __m128i src_hi = _mm_unpackhi_epi16(src, zero);
          _mm_storeu_ps(realdata + x + 4, _mm_cvtepi32_ps(src_hi));
        }
        else if (sizeof(pixel_t) == 2) {
          // uint16_t pixels
          __m128i src = _mm_loadu_si128(reinterpret_cast<const __m128i *>(srcp + x));
          __m128i src_lo = _mm_unpacklo_epi16(src, zero);
          _mm_storeu_ps(realdata + x, _mm_cvtepi32_ps(src_lo));
          __m128i src_hi = _mm_unpackhi_epi16(src, zero);
          _mm_storeu_ps(realdata + x + 4, _mm_cvtepi32_ps(src_hi));
        }
        else if (sizeof(pixel_t) == 4) {
          // float pixels
          __m128 src = _mm_loadu_ps(reinterpret_cast<const float *>(srcp + x));
          _mm_storeu_ps(realdata + x, src);
          src = _mm_loadu_ps(reinterpret_cast<const float *>(srcp + x + 4));
          _mm_storeu_ps(realdata + x + 4, src);
          // realdata[i] = reinterpret_cast<const pixel_t *>(srcp)[i];
        }
      }
    }
    srcp += src_pitch;
    realdata += floatpitch;
  }
}

// fake _mm_packus_epi32 (orig is SSE4.1 only)
MV_FORCEINLINE __m128i _MM_PACKUS_EPI32(__m128i a, __m128i b)
{
  a = _mm_slli_epi32(a, 16);
  a = _mm_srai_epi32(a, 16);
  b = _mm_slli_epi32(b, 16);
  b = _mm_srai_epi32(b, 16);
  a = _mm_packs_epi32(a, b);
  return a;
}

//  put source data to real array for FFT
// only for sizex>=8!
template <typename pixel_t, int nBlkSizeX, bool hasSSE4>
void DCTFFTW::Float2Bytes_SSE2(unsigned char * dstp0, int dst_pitch, float * realdata)
{
  pixel_t *dstp = reinterpret_cast<pixel_t *>(dstp0);
  dst_pitch /= sizeof(pixel_t);

  //sizex = nBlkSizeX; // X from template

  int floatpitch = nBlkSizeX;

  int maxPixelValue = (1 << bits_per_pixel) - 1; // 255/65535
  int middlePixelValue = 1 << (bits_per_pixel - 1);   // 128/32768 
  __m128i max_pixel_value = _mm_set1_epi16((short)(maxPixelValue));
  __m128i half = _mm_set1_epi32(middlePixelValue);
  __m128 root2div2 = _mm_set_ps(0.707f, 0.707f, 0.707f, 0.707f);

  float f = realdata[0] * 0.5f; // to be compatible with integer DCTINT8
  int first_integ = std::min(maxPixelValue, std::max(0, (int(f) >> dctshift0) + middlePixelValue)); // DC;
  // we update it at the end, till then, save pointer
  // dstp[0] = std::min(maxPixelValue, std::max(0, (integ>>dctshift0) + middlePixelValue)); // DC
  pixel_t *dstp_save = dstp;

  // uint16_t target: 2x4 float = 32 bytes -> 8 uint16_t = 16 bytes
  // uint8_t target: 2x4 float = 32 bytes -> 8 uint8_t = 8 bytes
  __m128i zero = _mm_setzero_si128();

  for (int y = 0; y < sizey; y++)
  {
    __m128 src, mulres;
    __m128i intres, intres_lo, intres_hi;
    __m128i res07;
    for (int x = 0; x < nBlkSizeX; x += 8) // 8 pixels at a time
    {
      // 0-3
      src = _mm_loadu_ps(reinterpret_cast<float *>(realdata + x));
      mulres = _mm_mul_ps(src, root2div2); // f = realdata[i]*0.707f; // to be compatible with integer DCTINT8
      intres = _mm_cvtps_epi32(mulres); // 4 float -> 4xint  // integ = (int)f;
      intres = _mm_srai_epi32(intres, dctshift); // (integ>>dctshift)
      intres_lo = _mm_add_epi32(intres, half); // (integ>>dctshift) + middlePixelValue)


      // 4-7
      src = _mm_loadu_ps(reinterpret_cast<float *>(realdata + x + 4));
      mulres = _mm_mul_ps(src, root2div2); // f = realdata[i]*0.707f; // to be compatible with integer DCTINT8
      intres = _mm_cvtps_epi32(mulres); // 4 float -> 4xint  // integ = (int)f;
      intres = _mm_srai_epi32(intres, dctshift); // (integ>>dctshift)
      intres_hi = _mm_add_epi32(intres, half); // (integ>>dctshift) + middlePixelValue)

      __m128i u16res = hasSSE4 ? _mm_packus_epi32(intres_lo, intres_hi) : _MM_PACKUS_EPI32(intres_lo, intres_hi); // SSE4

      if (sizeof(pixel_t) == 2) {
        res07 = _mm_min_epu16(u16res, max_pixel_value); // clamp to maxPixelValue
        _mm_storeu_si128(reinterpret_cast<__m128i *>(dstp + x), res07);
      }
      else {
        res07 = _mm_packus_epi16(u16res, zero); // clamp to 255
        _mm_storel_epi64(reinterpret_cast<__m128i *>(dstp + x), res07);
      }
      // dstp[x] = std::min(maxPixelValue, std::max(0, (integ>>dctshift) + middlePixelValue));
    }
    dstp += dst_pitch;
    realdata += floatpitch;
  }

  // overwrite very first
  dstp_save[0] = (pixel_t)first_integ;

}

template <typename pixel_t>
void DCTFFTW::Float2Bytes_C(unsigned char * dstp0, int dst_pitch, float * realdata)
{
  pixel_t *dstp = reinterpret_cast<pixel_t *>(dstp0);
  dst_pitch /= sizeof(pixel_t);

  int floatpitch = sizex;
  int i, j;
  int integ;

  int maxPixelValue = (1 << bits_per_pixel) - 1; // 255/65535
  int middlePixelValue = 1 << (bits_per_pixel - 1);   // 128/32768 

  float f = realdata[0] * 0.5f; // to be compatible with integer DCTINT8
  integ = int(f);
  dstp[0] = std::min(maxPixelValue, std::max(0, (integ >> dctshift0) + middlePixelValue)); // DC

  for (i = 1; i < sizex; i += 1)
  {
    f = realdata[i] * 0.707f; // to be compatible with integer DCTINT8
    integ = int(f);
    dstp[i] = std::min(maxPixelValue, std::max(0, (integ >> dctshift) + middlePixelValue));
  }

  dstp += dst_pitch;
  realdata += floatpitch;

  for (j = 1; j < sizey; j++)
  {
    for (i = 0; i < sizex; i += 1)
    {
      f = realdata[i] * 0.707f; // to be compatible with integer DCTINT8
      integ = (int)f;
      dstp[i] = std::min(maxPixelValue, std::max(0, (integ >> dctshift) + middlePixelValue));
    }
    dstp += dst_pitch;
    realdata += floatpitch;
  }
}

DCTFFTW::Float2BytesFunction DCTFFTW::get_floatToBytesPROC_function(int BlockX, int BlockY, int pixelsize, arch_t arch)
{
  // BlkSizeX, NO:BlkSizeY, pixelsize, arch_t
  std::map<std::tuple<int, int, arch_t>, DCTFFTW::Float2BytesFunction> func;
  using std::make_tuple;

  // SSE4

  // uint8_t
  func[make_tuple(32, 1, USE_SSE41)] = &DCTFFTW::Float2Bytes_SSE2<uint8_t, 32, true>;
  func[make_tuple(16, 1, USE_SSE41)] = &DCTFFTW::Float2Bytes_SSE2<uint8_t, 16, true>;
  func[make_tuple(8, 1, USE_SSE41)] = &DCTFFTW::Float2Bytes_SSE2<uint8_t, 8, true>;
  // uint16_t
  func[make_tuple(32, 2, USE_SSE41)] = &DCTFFTW::Float2Bytes_SSE2<uint16_t, 32, true>;
  func[make_tuple(16, 2, USE_SSE41)] = &DCTFFTW::Float2Bytes_SSE2<uint16_t, 16, true>;
  func[make_tuple(8, 2, USE_SSE41)] = &DCTFFTW::Float2Bytes_SSE2<uint16_t, 8, true>;

  // SSE2

  // uint8_t
  func[make_tuple(32, 1, USE_SSE2)] = &DCTFFTW::Float2Bytes_SSE2<uint8_t, 32, false>;
  func[make_tuple(16, 1, USE_SSE2)] = &DCTFFTW::Float2Bytes_SSE2<uint8_t, 16, false>;
  func[make_tuple(8, 1, USE_SSE2)] = &DCTFFTW::Float2Bytes_SSE2<uint8_t, 8, false>;
  // uint16_t
  func[make_tuple(32, 2, USE_SSE2)] = &DCTFFTW::Float2Bytes_SSE2<uint16_t, 32, false>;
  func[make_tuple(16, 2, USE_SSE2)] = &DCTFFTW::Float2Bytes_SSE2<uint16_t, 16, false>;
  func[make_tuple(8, 2, USE_SSE2)] = &DCTFFTW::Float2Bytes_SSE2<uint16_t, 8, false>;

  DCTFFTW::Float2BytesFunction result = nullptr;
  arch_t archlist[] = { USE_AVX2, USE_AVX, USE_SSE41, USE_SSE2, NO_SIMD };
  int index = 0;
  while (result == nullptr) {
    arch_t current_arch_try = archlist[index++];
    if (current_arch_try > arch) continue;
    result = func[make_tuple(BlockX, pixelsize, current_arch_try)];
    if (result == nullptr && current_arch_try == NO_SIMD)
      break;
  }
  if (result == nullptr)
    result = (pixelsize == 1) ? &DCTFFTW::Float2Bytes_C<uint8_t> : &DCTFFTW::Float2Bytes_C<uint16_t>;
  return result;
}


DCTFFTW::Bytes2FloatFunction DCTFFTW::get_bytesToFloatPROC_function(int BlockX, int BlockY, int pixelsize, arch_t arch)
{
  // BlkSizeX, NO:BlkSizeY, pixelsize, arch_t
  std::map<std::tuple<int, int, arch_t>, DCTFFTW::Bytes2FloatFunction> func;
  using std::make_tuple;

  // SSE2

  // uint8_t
  func[make_tuple(32, 1, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint8_t, 32>;
  func[make_tuple(16, 1, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint8_t, 16>;
  func[make_tuple(8, 1, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint8_t, 8>;
  func[make_tuple(4, 1, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint8_t, 4>;
  // uint16_t
  func[make_tuple(32, 2, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint16_t, 32>;
  func[make_tuple(16, 2, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint16_t, 16>;
  func[make_tuple(8, 2, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint16_t, 8>;
  func[make_tuple(4, 2, USE_SSE2)] = &DCTFFTW::Bytes2Float_SSE2<uint16_t, 4>;

  DCTFFTW::Bytes2FloatFunction result = nullptr;
  arch_t archlist[] = { USE_AVX2, USE_AVX, USE_SSE41, USE_SSE2, NO_SIMD };
  int index = 0;
  while (result == nullptr) {
    arch_t current_arch_try = archlist[index++];
    if (current_arch_try > arch) continue;
    result = func[make_tuple(BlockX, pixelsize, current_arch_try)];
    if (result == nullptr && current_arch_try == NO_SIMD)
      break;
  }
  if (result == nullptr)
    result = (pixelsize == 1) ? &DCTFFTW::Bytes2Float_C<uint8_t> : &DCTFFTW::Bytes2Float_C<uint16_t>;
  return result;
}


void DCTFFTW::DCTBytes2D(const unsigned char *srcp, int src_pitch, unsigned char *dctp, int dct_pitch)
{
#if 0
  PF 161201 Moved to SIMD Intrinsics(SSE2 / SSE4.1) and dispatcher
#ifndef _M_X64 
    _mm_empty();
#endif
  if (pixelsize == 1) {
    Bytes2Float_C<uint8_t>(srcp, src_pitch, fSrc);
    fftwf_execute_r2r_addr(dctplan, fSrc, fSrcDCT);
    Float2Bytes_C<uint8_t>(dctp, dct_pitch, fSrcDCT);
  }
  else {
    Bytes2Float_C<uint16_t>(srcp, src_pitch, fSrc);
    fftwf_execute_r2r_addr(dctplan, fSrc, fSrcDCT);
    Float2Bytes_C<uint16_t>(dctp, dct_pitch, fSrcDCT);
  }
#else
  // calling member function pointer
  (this->*bytesToFloatPROC)(srcp, src_pitch, fSrc); // selected variable function
  fftwf_execute_r2r_addr(dctplan, fSrc, fSrcDCT);
  (this->*floatToBytesPROC)(dctp, dct_pitch, fSrcDCT); // selected variable function
#endif
}
