// Create an overlay mask with the motion vectors
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

#include "CopyCode.h"
#include "MVClip.h"

#include	<algorithm>

#include <cmath>
#include <emmintrin.h>

MV_FORCEINLINE void ByteOccMask(uint8_t *occMask, int occlusion, double occnorm, double fGamma)
{
  if (fGamma == 1.0)
  {
    *occMask = std::max(
      int(*occMask),
      std::min(int(255 * occlusion*occnorm), 255)
    );
  }
  else
  {
    *occMask = std::max(
      int(*occMask),
      std::min(int(255 * pow(occlusion*occnorm, fGamma)), 255)
    );
  }
}

template <class OP>
void MakeVectorOcclusionMaskTimePlane(MVClip &mvClip, int nBlkX, int nBlkY, double dMaskNormFactor, int nPel, uint8_t * occMask, int occMaskPitch, const uint8_t *pt256, int t256_pitch, int blkSizeX, int blkSizeY)
{
  pt256 += t256_pitch * (blkSizeY / 2) + (blkSizeX / 2);

  MemZoneSet(occMask, 0, nBlkX, nBlkY, 0, 0, nBlkX);
#ifndef _M_X64 
  _mm_empty();
#endif
  double occnorm = 10 / dMaskNormFactor / nPel;
  int occlusion;

  for (int by = 0; by < nBlkY; by++)
  {
    for (int bx = 0; bx < nBlkX; bx++)
    {
      const int		time256 = OP::compute(pt256[bx * blkSizeX]);

      int i = bx + by * nBlkX; // current block
      const FakeBlockData &block = mvClip.GetBlock(0, i);
      int vx = block.GetMV().x;
      int vy = block.GetMV().y;
      if (bx < nBlkX - 1) // right neighbor
      {
        int i1 = i + 1;
        const FakeBlockData &block1 = mvClip.GetBlock(0, i1);
        int vx1 = block1.GetMV().x;
        //int vy1 = block1.GetMV().y;
        if (vx1 < vx) {
          occlusion = vx - vx1;
          const int		time4096X = time256 * 16 / blkSizeX;
          for (int bxi = bx + vx1 * time4096X / 4096; bxi <= bx + vx * time4096X / 4096 + 1 && bxi >= 0 && bxi < nBlkX; bxi++)
            ByteOccMask(&occMask[bxi + by * occMaskPitch], occlusion, occnorm, 1.0);
        }
      }
      if (by < nBlkY - 1) // bottom neighbor
      {
        int i1 = i + nBlkX;
        const FakeBlockData &block1 = mvClip.GetBlock(0, i1);
        //int vx1 = block1.GetMV().x;
        int vy1 = block1.GetMV().y;
        if (vy1 < vy) {
          occlusion = vy - vy1;
          const int		time4096Y = time256 * 16 / blkSizeY;
          for (int byi = by + vy1 * time4096Y / 4096; byi <= by + vy * time4096Y / 4096 + 1 && byi >= 0 && byi < nBlkY; byi++)
            ByteOccMask(&occMask[bx + byi * occMaskPitch], occlusion, occnorm, 1.0);
        }
      }
    }

    pt256 += t256_pitch * blkSizeY;
  }
}

// FIND MEDIAN OF 3 ELEMENTS
//
MV_FORCEINLINE int Median3(int a, int b, int c)
{
  // b a c || c a b
  if (((b <= a) && (a <= c)) || ((c <= a) && (a <= b))) return a;

  // a b c || c b a
  else if (((a <= b) && (b <= c)) || ((c <= b) && (b <= a))) return b;

  // b c a || a c b
  else return c;

}

template<typename T>
MV_FORCEINLINE T Median3r(T a, T b, T c)
{
  // reduced median - if it is known that a <= c (more fast)

// b a c
  if (b <= a) return a;
  // a c b
  else  if (c <= b) return c;
  // a b c
  else return b;
}

template <typename pixel_t, int NPELL2>
static void FlowInter_NPel(
  uint8_t * pdst8, int dst_pitch, const uint8_t *prefB8, const uint8_t *prefF8, int ref_pitch,
  short *VXFullB, short *VXFullF, short *VYFullB, short *VYFullF, uint8_t *MaskB, uint8_t *MaskF,
  int VPitch, int width, int height, int time256)
{
  dst_pitch /= sizeof(pixel_t);
  ref_pitch /= sizeof(pixel_t);
  pixel_t *pdst = reinterpret_cast<pixel_t *>(pdst8);
  const pixel_t *prefB = reinterpret_cast<const pixel_t *>(prefB8);
  const pixel_t *prefF = reinterpret_cast<const pixel_t *>(prefF8);

  const float time256_f = time256 / 256.0f;

  for (int h = 0; h < height; h++)
  {
    for (int w = 0; w < width; w++)
    {
      int vxF = (VXFullF[w] * time256) >> 8; // 2.5.11.22
      int vyF = (VYFullF[w] * time256) >> 8; // 2.5.11.22
      pixel_t dstF = prefF[vyF*ref_pitch + vxF + (w << NPELL2)];
      pixel_t dstF0 = prefF[(w << NPELL2)]; // zero

      int vxB = (VXFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
      int vyB = (VYFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
      pixel_t dstB = prefB[vyB*ref_pitch + vxB + (w << NPELL2)];
      pixel_t dstB0 = prefB[(w << NPELL2)]; // zero

      if constexpr (sizeof(pixel_t) == 4) {
        constexpr float maskScale = 1.0f / 255.0f;
        const float MaskF_f = MaskF[w] * maskScale;
        const float MaskB_f = MaskB[w] * maskScale;
        pdst[w] =
          (dstF*(1.0f - MaskF_f) + MaskF_f * (dstB*(1.0f - MaskB_f) + MaskB_f * dstF0))*(1.0f - time256_f) +
          (dstB*(1.0f - MaskB_f) + MaskB_f * (dstF*(1.0f - MaskF_f) + MaskF_f * dstB0))*        time256_f;
      }
      else {
        // avoid possible 8*16*8bit int32 overflow
        typedef typename std::conditional < sizeof(pixel_t) == 1, int, int64_t>::type calc_t;

        pdst[w] = (pixel_t)((((dstF*(255 - MaskF[w]) + (((calc_t)MaskF[w] * (dstB*(255 - MaskB[w]) + MaskB[w] * dstF0) + 255) >> 8) + 255) >> 8)*(256 - time256) +
          ((dstB*(255 - MaskB[w]) + (((calc_t)MaskB[w] * (dstF*(255 - MaskF[w]) + MaskF[w] * dstB0) + 255) >> 8) + 255) >> 8)*     time256) >> 8);
      }
    }
    pdst += dst_pitch;
    prefB += ref_pitch << NPELL2;
    prefF += ref_pitch << NPELL2;
    VXFullB += VPitch;
    VYFullB += VPitch;
    VXFullF += VPitch;
    VYFullF += VPitch;
    MaskB += VPitch;
    MaskF += VPitch;
  }
}

// NPELL2 = Log2(NPEL) (shift needed)
template <typename pixel_t, int NPELL2>
static void FlowInterExtra_NPel(
  uint8_t * pdst8, int dst_pitch, const uint8_t *prefB8, const uint8_t *prefF8, int ref_pitch,
  short *VXFullB, short *VXFullF, short *VYFullB, short *VYFullF, uint8_t *MaskB, uint8_t *MaskF,
  int VPitch, int width, int height, int time256,
  short *VXFullBB, short *VXFullFF, short *VYFullBB, short *VYFullFF)
{
  dst_pitch /= sizeof(pixel_t);
  ref_pitch /= sizeof(pixel_t);
  pixel_t *pdst = reinterpret_cast<pixel_t *>(pdst8);
  const pixel_t *prefB = reinterpret_cast<const pixel_t *>(prefB8);
  const pixel_t *prefF = reinterpret_cast<const pixel_t *>(prefF8);

  const float time256_f = time256 / 256.0f;

  for (int h = 0; h < height; h++)
  {
    for (int w = 0; w < width; w++)
    {
      int vxF = (VXFullF[w] * time256) >> 8; // 2.5.11.22
      int vyF = (VYFullF[w] * time256) >> 8; // 2.5.11.22
      int adrF = vyF * ref_pitch + vxF + (w << NPELL2);
      pixel_t dstF = prefF[adrF];
      int vxFF = (VXFullFF[w] * time256) >> 8; // is it correct time?
      int vyFF = (VYFullFF[w] * time256) >> 8;  // 2.5.11.22
      int adrFF = vyFF * ref_pitch + vxFF + (w << NPELL2);
      pixel_t dstFF = prefF[adrFF];
      int vxB = (VXFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
      int vyB = (VYFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
      int adrB = vyB * ref_pitch + vxB + (w << NPELL2);
      pixel_t dstB = prefB[adrB];
      int vxBB = (VXFullBB[w] * (256 - time256)) >> 8;
      int vyBB = (VYFullBB[w] * (256 - time256)) >> 8;
      int adrBB = vyBB * ref_pitch + vxBB + (w << NPELL2);
      pixel_t dstBB = prefB[adrBB];
      pixel_t minfb;
      pixel_t maxfb;
      if (dstF > dstB) {
        minfb = dstB;
        maxfb = dstF;
      }
      else {
        maxfb = dstB;
        minfb = dstF;
      }

      if constexpr (sizeof(pixel_t) == 4) {
        constexpr float maskScale = 1.0f / 255.0f;
        const float MaskF_f = MaskF[w] * maskScale;
        const float MaskB_f = MaskB[w] * maskScale;
        pdst[w] =
          (Median3r(minfb, dstBB, maxfb)*MaskF_f + dstF * (1.0f - MaskF_f))*(1.0f - time256_f) +
          (Median3r(minfb, dstFF, maxfb)*MaskB_f + dstB * (1.0f - MaskB_f))*        time256_f
          ;
      }
      else {
        pdst[w] = (
          ((Median3r(minfb, dstBB, maxfb)*MaskF[w] + dstF * (255 - MaskF[w]) + 255) >> 8)*(256 - time256) +
          ((Median3r(minfb, dstFF, maxfb)*MaskB[w] + dstB * (255 - MaskB[w]) + 255) >> 8)*     time256
          ) >> 8;
      }
    }
    pdst += dst_pitch;
    prefB += ref_pitch << NPELL2;
    prefF += ref_pitch << NPELL2;
    VXFullB += VPitch;
    VYFullB += VPitch;
    VXFullF += VPitch;
    VYFullF += VPitch;
    MaskB += VPitch;
    MaskF += VPitch;
    VXFullBB += VPitch;
    VYFullBB += VPitch;
    VXFullFF += VPitch;
    VYFullFF += VPitch;
  }
}

// todo SSE2
template <typename pixel_t, int NPELL2>
static void FlowInterSimple_NPel(
  uint8_t * pdst8, int dst_pitch, const uint8_t *prefB8, const uint8_t *prefF8, int ref_pitch,
  short *VXFullB, short *VXFullF, short *VYFullB, short *VYFullF, uint8_t *MaskB, uint8_t *MaskF,
  int VPitch, int width, int height, int time256 /* T256P &t256_provider*/)
{
  dst_pitch /= sizeof(pixel_t);
  ref_pitch /= sizeof(pixel_t);
  pixel_t *pdst = reinterpret_cast<pixel_t *>(pdst8);
  const pixel_t *prefB = reinterpret_cast<const pixel_t *>(prefB8);
  const pixel_t *prefF = reinterpret_cast<const pixel_t *>(prefF8);

  const float time256_f = time256 / 256.0f;

  if (time256 == 128) // special case double fps - fastest
  {
    for (int h = 0; h < height; h++)
    {
      for (int w = 0; w < width; w += 1)
      {
        int vxF = (VXFullF[w]) >> 1; // 2.5.11.22
        int vyF = (VYFullF[w]) >> 1; // 2.5.11.22
        int adrF = vyF * ref_pitch + vxF + (w << NPELL2);
        pixel_t dstF = prefF[adrF];

        int vxB = (VXFullB[w]) >> 1; // 2.5.11.22
        int vyB = (VYFullB[w]) >> 1; // 2.5.11.22
        int adrB = vyB * ref_pitch + vxB + (w << NPELL2);
        pixel_t dstB = prefB[adrB];

        if constexpr (sizeof(pixel_t) == 4)
        {
          constexpr float maskScale = 1.0f / 255.0f;
          const float MaskF_f = MaskF[w] * maskScale;
          const float MaskB_f = MaskB[w] * maskScale;

          pdst[w] = (((dstF + dstB)) + (dstB - dstF)*(MaskF_f - MaskB_f)) * 0.5f;
        }
        else {
          pdst[w] = (((dstF + dstB) << 8) + (dstB - dstF)*(MaskF[w] - MaskB[w])) >> 9;
          /*
          256*F + 256*B + B*MF - F*MF - B*MB + F*MB
          // Inaccurate when mask 0..255 is used. 0..256 would be correct.
          256*F + 256*B + B*256 - F*256 - B*0 + F*0 =     B*(256+256) // MF=256 MB=0
          256*F + 256*B + B*255 - F*255 - B*0 + F*0 = F + B*(255+256) // MF=255 MB=0
          256*F + 256*B + B*256 - F*256 - B*256 + F*256 = 2*256*B = B*256 + F*256 = 256*(B+F) // MF=256 MB=256
          256*F + 256*B + B*256 - F*256 - B*256 + F*256 = 2*256*B = B*256 + F*256 = 256*(B+F) // MF=256 MB=256
          */
        }
      }
      pdst += dst_pitch;
      prefB += ref_pitch << NPELL2;
      prefF += ref_pitch << NPELL2;
      VXFullB += VPitch;
      VYFullB += VPitch;
      VXFullF += VPitch;
      VYFullF += VPitch;
      MaskB += VPitch;
      MaskF += VPitch;
    }
  }

  else // general case
  {
    for (int h = 0; h < height; h++)
    {
      for (int w = 0; w < width; w += 1)
      {
        int vxF = (VXFullF[w] * time256) >> 8; // 2.5.11.22
        int vyF = (VYFullF[w] * time256) >> 8; // 2.5.11.22
        int adrF = vyF * ref_pitch + vxF + (w << NPELL2);
        pixel_t dstF = prefF[adrF];
        int vxB = (VXFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
        int vyB = (VYFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
        int adrB = vyB * ref_pitch + vxB + (w << NPELL2);
        pixel_t dstB = prefB[adrB];

        if constexpr(sizeof(pixel_t) == 4)
        {
          constexpr float maskScale = 1.0f / 255.0f;
          const float MaskF_f = MaskF[w] * maskScale;
          const float MaskB_f = MaskB[w] * maskScale;

          pdst[w] =
            (dstF*(1.0f - MaskF_f) + dstB * MaskF_f)*(1.0f - time256_f) +
            (dstB*(1.0f - MaskB_f) + dstF * MaskB_f)*        time256_f;
        }
        else {
          pdst[w] =
            (
            ((dstF*(255 - MaskF[w]) + dstB * MaskF[w] + 255) >> 8)*(256 - time256) +
              ((dstB*(255 - MaskB[w]) + dstF * MaskB[w] + 255) >> 8)*     time256
              ) >> 8;
          /*
          Reason for +255: keep B or F as 255 when masks are extreme 0 and 255
          test for time256 = 0
          Inaccurate when mask 0..255 is used. 0..256 would be correct.
          F(255-MF) + B*MF + 255 = 255*F -F*255 + B*255 + 255 = B*255 + 255 = (B+1)*255 // MF=255 MB=0 : incorrect
          F(256-MF) + B*MF       = 256*F -F*256 + B*256                     =   B  *256 // MF=256 MB=0 (no +255 addition) : correct
          */
        }
      }
      pdst += dst_pitch;
      prefB += ref_pitch << NPELL2;
      prefF += ref_pitch << NPELL2;
      VXFullB += VPitch;
      VYFullB += VPitch;
      VXFullF += VPitch;
      VYFullF += VPitch;
      MaskB += VPitch;
      MaskF += VPitch;
    }
  }
}

template<typename pixel_t>
static void FlowInterSimple_Pel1(
  uint8_t * pdst8, int dst_pitch, const uint8_t *prefB8, const uint8_t *prefF8, int ref_pitch,
  short *VXFullB, short *VXFullF, short *VYFullB, short *VYFullF, uint8_t *MaskB, uint8_t *MaskF,
  int VPitch, int width, int height, int time256)
{
  dst_pitch /= sizeof(pixel_t);
  ref_pitch /= sizeof(pixel_t);
  pixel_t *pdst = reinterpret_cast<pixel_t *>(pdst8);
  const pixel_t *prefB = reinterpret_cast<const pixel_t *>(prefB8);
  const pixel_t *prefF = reinterpret_cast<const pixel_t *>(prefF8);

  const float time256_f = time256 / 256.0f;

  if (time256 == 128) // special case double fps - fastest
  {
    for (int h = 0; h < height; h++)
    {
      for (int w = 0; w < width; w += 2) // paired for speed
      {
        int vxF = (VXFullF[w]) >> 1; // 2.5.11.22
        int vyF = (VYFullF[w]) >> 1; // 2.5.11.22
        int addrF = vyF * ref_pitch + vxF + w;
        pixel_t dstF = prefF[addrF];
        pixel_t dstF1 = prefF[addrF + 1]; // approximation for speed
        int vxB = (VXFullB[w]) >> 1; // 2.5.11.22
        int vyB = (VYFullB[w]) >> 1; // 2.5.11.22
        int addrB = vyB * ref_pitch + vxB + w;
        pixel_t dstB = prefB[addrB];
        pixel_t dstB1 = prefB[addrB + 1];

        if constexpr (sizeof(pixel_t) == 4) {
          constexpr float maskScale = 1.0f / 255.0f;
          const float MaskF_f = MaskF[w] * maskScale;
          const float MaskB_f = MaskB[w] * maskScale;
          pdst[w] = ((dstF + dstB) + (dstB - dstF)*(MaskF_f - MaskB_f)) * 0.5f;
          const float MaskF1_f = MaskF[w + 1] * maskScale;
          const float MaskB1_f = MaskB[w + 1] * maskScale;
          pdst[w + 1] = ((dstF1 + dstB1) + (dstB1 - dstF1)*(MaskF1_f - MaskB1_f)) * 0.5f;
        }
        else {
          pdst[w] = (((dstF + dstB) << 8) + (dstB - dstF)*(MaskF[w] - MaskB[w])) >> 9; // /2 >>8
          pdst[w + 1] = (((dstF1 + dstB1) << 8) + (dstB1 - dstF1)*(MaskF[w + 1] - MaskB[w + 1])) >> 9;
        }
      }
      pdst += dst_pitch;
      prefB += ref_pitch;
      prefF += ref_pitch;
      VXFullB += VPitch;
      VYFullB += VPitch;
      VXFullF += VPitch;
      VYFullF += VPitch;
      MaskB += VPitch;
      MaskF += VPitch;
    }
  }

  else // general case
  {
    // avoid "uint16*uint16 can't get into int32" overflows
    typedef std::conditional < sizeof(pixel_t) == 1, int, typename std::conditional < sizeof(pixel_t) == 2, int64_t, float>::type >::type result_t;

    for (int h = 0; h < height; h++)
    {
      for (int w = 0; w < width; w += 2) // paired for speed
      {
        int vxF = (VXFullF[w]) >> 1; // 2.5.11.22
        int vyF = (VYFullF[w]) >> 1; // 2.5.11.22
        int addrF = vyF * ref_pitch + vxF + w;
        pixel_t dstF = prefF[addrF];
        pixel_t dstF1 = prefF[addrF + 1]; // approximation for speed
        int vxB = (VXFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
        int vyB = (VYFullB[w] * (256 - time256)) >> 8; // 2.5.11.22
        int addrB = vyB * ref_pitch + vxB + w;
        pixel_t dstB = prefB[addrB];
        pixel_t dstB1 = prefB[addrB + 1];

        if constexpr (sizeof(pixel_t) == 4)
        {
          constexpr float maskScale = 1.0f / 255.0f;
          const float MaskF_f = MaskF[w] * maskScale;
          const float MaskB_f = MaskB[w] * maskScale;
          pdst[w] =
            (dstF * 1.0f + (dstB - dstF)*MaskF_f)*(1.0f - time256_f) +
            (dstB * 1.0f - (dstB - dstF)*MaskB_f)*        time256_f;
          const float MaskF1_f = MaskF[w + 1] * maskScale;
          const float MaskB1_f = MaskB[w + 1] * maskScale;
          pdst[w + 1] =
            (dstF1 * 1.0f + (dstB1 - dstF1)*MaskF1_f)*(1.0f - time256_f) +
            (dstB1 * 1.0f - (dstB1 - dstF1)*MaskB1_f)*        time256_f;
        }
        else {
          // possible overflow for 16 bit
          pdst[w] = (
            ((result_t)(dstF * 255 + (dstB - dstF)*MaskF[w] + 255))*(256 - time256) +
            ((result_t)(dstB * 255 - (dstB - dstF)*MaskB[w] + 255))*       time256
            ) >> 16;
          pdst[w + 1] = (
            ((result_t)(dstF1 * 255 + (dstB1 - dstF1)*MaskF[w + 1] + 255))*(256 - time256) +
            ((result_t)(dstB1 * 255 - (dstB1 - dstF1)*MaskB[w + 1] + 255))*       time256
            ) >> 16;
        }
      }
      pdst += dst_pitch;
      prefB += ref_pitch;
      prefF += ref_pitch;
      VXFullB += VPitch;
      VYFullB += VPitch;
      VXFullF += VPitch;
      VYFullF += VPitch;
      MaskB += VPitch;
      MaskF += VPitch;
    }
  }
}
