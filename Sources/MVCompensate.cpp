// Make a motion compensate temporal denoiser
// Author: Manao
// Copyright(c)2006 A.G.Balakhnin aka Fizick (YUY2, overlap, edges processing)
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

#include	"ClipFnc.h"
#include "commonfunctions.h"
#include "maskfun.h"
#include "MVCompensate.h"
#include "MVFrame.h"
#include	"MVGroupOfFrames.h"
#include "MVPlane.h"
#include "profile.h"
#include "SuperParams64Bits.h"
#include "Time256ProviderCst.h"

#include	<mmintrin.h>



MVCompensate::MVCompensate(
	PClip _child, PClip _super, PClip vectors, bool sc, double _recursionPercent,
	sad_t thsad, bool _fields, double _time100, sad_t nSCD1, int nSCD2, bool _isse2, bool _planar,
	bool mt_flag, int trad, bool center_flag, PClip cclip_sptr, sad_t thsad2,
	IScriptEnvironment* env_ptr
)
:	GenericVideoFilter(_child)
,	_mv_clip_arr (1)
,	MVFilter(vectors, "MCompensate", env_ptr, 1, 0)
,	super(_super)
,	_trad (trad)
,	_cclip_sptr ((cclip_sptr != 0) ? cclip_sptr : _child)
,	_multi_flag (trad > 0)
,	_center_flag (center_flag)
,	_mt_flag (mt_flag)
//,	nLogxRatioUV (( xRatioUV == 2) ? 1 : 0) MVFilter has nLogxRatioUV
//,	nLogyRatioUV ((yRatioUV == 2) ? 1 : 0)
,	_boundary_cnt_arr ()
{
	if (trad < 0)
	{
		env_ptr->ThrowError (
			"MCompensate: temporal radius must greater or equal to 0."
		);
	}

	if (_trad <= 0)
	{
		_mv_clip_arr [0]._clip_sptr =
			MVClipSPtr (new MVClip (vectors, nSCD1, nSCD2, env_ptr, 1, 0));
	}
	else
	{
		_mv_clip_arr.resize (_trad * 2);
		for (int k = 0; k < _trad * 2; ++k)
		{
			_mv_clip_arr [k]._clip_sptr = MVClipSPtr (
				new MVClip (vectors, nSCD1, nSCD2, env_ptr, _trad * 2, k)
			);
			CheckSimilarity (*(_mv_clip_arr [k]._clip_sptr), "vectors", env_ptr);
		}

		int				tbsize = _trad * 2;
		if (_center_flag)
		{
			++ tbsize;
		}

		vi.num_frames    *= tbsize;
		vi.fps_numerator *= tbsize;
	}

	isse_flag = _isse2;
	scBehavior = sc;
	recursion = std::max (0, std::min (256, int (_recursionPercent/100*256))); // convert to int scaled 0 to 256
	fields = _fields;
	planar = _planar;

  if (_time100 <0 || _time100 > 100)
    env_ptr->ThrowError("MCompensate: time must be 0.0 to 100.0");
  time256 = int(256 * _time100 / 100);

	if (recursion>0 && nPel>1)
	{
		env_ptr->ThrowError("MCompensate: recursion is not supported for Pel>1");
	}

	if (fields && nPel<2 && !vi.IsFieldBased())
	{
		env_ptr->ThrowError("MCompensate: fields option is for fieldbased video and pel > 1");
	}

	// Normalize to block SAD
	for (int k = 0; k < int (_mv_clip_arr.size ()); ++k)
	{
		MvClipInfo &	c_info  = _mv_clip_arr [k];
		const sad_t		thscd1  = c_info._clip_sptr->GetThSCD1 ();
		const sad_t		thsadn  = thsad  * thscd1 / nSCD1; // PF check todo bits_per_pixel
		const sad_t		thsadn2 = thsad2 * thscd1 / nSCD1;
		const int		d       = k / 2 + 1;
		c_info._thsad = ClipFnc::interpolate_thsad (thsadn, thsadn2, d, _trad); // P.F. when testing, d=0, _trad=1, will assert inside.
	}

    // in overlaps.h
    // OverlapsLsbFunction
    // OverlapsFunction
    // in M(V)DegrainX: DenoiseXFunction
  /*
    arch_t arch;
    if ((pixelsize == 1) && (((env_ptr->GetCPUFlags() & CPUF_SSE2) != 0) & isse2))
        arch = USE_SSE2;
    else if ((pixelsize == 1) && isse2)
        arch = USE_MMX;
    else
        arch = NO_SIMD;
        */
  arch_t arch;
  if ((((env_ptr->GetCPUFlags() & CPUF_AVX2) != 0) & isse_flag))
    arch = USE_AVX2;
  else if ((((env_ptr->GetCPUFlags() & CPUF_AVX) != 0) & isse_flag))
    arch = USE_AVX;
  else if ((((env_ptr->GetCPUFlags() & CPUF_SSE4_1) != 0) & isse_flag))
    arch = USE_SSE41;
  else if ((((env_ptr->GetCPUFlags() & CPUF_SSE2) != 0) & isse_flag))
    arch = USE_SSE2;
  /*  else if ((pixelsize == 1) && _isse_flag) // PF no MMX support
  arch = USE_MMX;*/
  else
    arch = NO_SIMD;


    OVERSLUMA   = get_overlaps_function(nBlkSizeX, nBlkSizeY, pixelsize, arch);
    OVERSCHROMA = get_overlaps_function(nBlkSizeX >> nLogxRatioUV, nBlkSizeY >> nLogyRatioUV, pixelsize, arch);
    BLITLUMA = get_copy_function(nBlkSizeX, nBlkSizeY, pixelsize, arch);
    BLITCHROMA = get_copy_function(nBlkSizeX >> nLogxRatioUV, nBlkSizeY >> nLogyRatioUV, pixelsize, arch);
    // 161115
    OVERSLUMA16 = get_overlaps_function(nBlkSizeX, nBlkSizeY, sizeof(uint16_t), arch);
    OVERSCHROMA16 = get_overlaps_function(nBlkSizeX >> nLogxRatioUV, nBlkSizeY >> nLogyRatioUV, sizeof(uint16_t), arch);


	// get parameters of prepared super clip - v2.0
	SuperParams64Bits params;
	memcpy(&params, &super->GetVideoInfo().num_audio_samples, 8);
	int nHeightS = params.nHeight;
	nSuperHPad = params.nHPad;
	nSuperVPad = params.nVPad;
	int nSuperPel = params.nPel;
	int nSuperModeYUV = params.nModeYUV;
	int nSuperLevels = params.nLevels;
  pixelsize_super = super->GetVideoInfo().ComponentSize();
  bits_per_pixel_super = super->GetVideoInfo().BitsPerComponent();
  // todo check: what happens if pixelsize_super != pixelsize (+bits_per_pixel)

	pRefGOF = new MVGroupOfFrames(nSuperLevels, nWidth, nHeight, nSuperPel, nSuperHPad, nSuperVPad, nSuperModeYUV, isse_flag, xRatioUV, yRatioUV, pixelsize, bits_per_pixel, mt_flag);
	pSrcGOF = new MVGroupOfFrames(nSuperLevels, nWidth, nHeight, nSuperPel, nSuperHPad, nSuperVPad, nSuperModeYUV, isse_flag, xRatioUV, yRatioUV, pixelsize, bits_per_pixel, mt_flag);
	nSuperWidth = super->GetVideoInfo().width;
	nSuperHeight = super->GetVideoInfo().height;

	if (   nHeight != nHeightS
	    || nHeight != vi.height
	    || nWidth  != nSuperWidth - nSuperHPad * 2
	    || nWidth  != vi.width
	    || nPel    != nSuperPel)
	{
		env_ptr->ThrowError("MCompensate : wrong source or super frame size");
	}


	if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 && !planar)
	{
		DstPlanes =  new YUY2Planes(nWidth, nHeight);
	}
  // pitch: not byte* but short* or int* granurality, no need pixelsize correction
  dstShortPitch = AlignNumber(nWidth, 16); // ((nWidth + 15) / 16) * 16;
  dstShortPitchUV = AlignNumber(nWidth >> nLogxRatioUV, 16); //  (((nWidth >> nLogxRatioUV) + 15) / 16) * 16;
	if (nOverlapX > 0 || nOverlapY > 0)
	{
		OverWins = new OverlapWindows(nBlkSizeX, nBlkSizeY, nOverlapX, nOverlapY);
		OverWinsUV = new OverlapWindows(nBlkSizeX >> nLogxRatioUV, nBlkSizeY >> nLogyRatioUV, nOverlapX >> nLogxRatioUV, nOverlapY >> nLogyRatioUV);
		DstShort = new unsigned short[dstShortPitch*nHeight*pixelsize];   // pixelsize==2 -> int instead of short
		DstShortU = new unsigned short[dstShortPitchUV*nHeight*pixelsize];
		DstShortV = new unsigned short[dstShortPitchUV*nHeight*pixelsize];
	}
	if (nOverlapY > 0)
	{
		_boundary_cnt_arr.resize (nBlkY);
	}

	if (recursion>0)
	{
		if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 )
		{
      nLoopPitches[0] = AlignNumber(nSuperWidth * 2, 16); // ((nSuperWidth * 2 + 15) / 16) * 16;
			nLoopPitches[1]  = nLoopPitches[0];
			nLoopPitches[2]  = nLoopPitches[1];
			pLoop[0] = new unsigned char [nLoopPitches[0]*nSuperHeight];
			pLoop[1] = pLoop[0] + nSuperWidth;
			pLoop[2] = pLoop[1] + nSuperWidth/2;
		}
		else
		{
      // todo check pixelsize_super?
      nLoopPitches[0] = AlignNumber(nSuperWidth*pixelsize_super, 16); // ((nSuperWidth + 15) / 16) * 16; // todo pixelsize?
      nLoopPitches[1] = nLoopPitches[2] = AlignNumber((nSuperWidth >> nLogxRatioUV)*pixelsize_super, 16); // (((nSuperWidth >> nLogxRatioUV) + 15)/16)*16;
			pLoop[0] = new unsigned char [nLoopPitches[0]*nSuperHeight];
			pLoop[1] = new unsigned char [nLoopPitches[1]*nSuperHeight];
			pLoop[2] = new unsigned char [nLoopPitches[2]*nSuperHeight];
		}
	}

}

MVCompensate::~MVCompensate()
{
	if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2  && !planar)
	{
		delete DstPlanes;
	}
	if (nOverlapX >0 || nOverlapY >0)
	{
		delete OverWins;
		delete OverWinsUV;
		delete [] DstShort;
		delete [] DstShortU;
		delete [] DstShortV;
	}
	delete pRefGOF; // v2.0
	delete pSrcGOF;

	if (recursion>0)
	{
		delete [] pLoop[0];
		if ( (pixelType & VideoInfo::CS_YUY2) != VideoInfo::CS_YUY2 )
		{
			delete [] pLoop[1];
			delete [] pLoop[2];
		}
	}

}

// todo hbd
PVideoFrame __stdcall MVCompensate::GetFrame(int n, IScriptEnvironment* env_ptr)
{
	int				nsrc;
	int				nvec;
	int				vindex;
	const bool		comp_flag = compute_src_frame (nsrc, nvec, vindex, n);
	if (! comp_flag)
	{
		return (_cclip_sptr->GetFrame (nsrc, env_ptr));
	}
	MvClipInfo &	info = _mv_clip_arr [vindex];
	_mv_clip_ptr = info._clip_sptr.get ();
	_thsad       = info._thsad;

	int nWidth_B = nBlkX*(nBlkSizeX - nOverlapX) + nOverlapX;
	int nHeight_B = nBlkY*(nBlkSizeY - nOverlapY) + nOverlapY;

	const BYTE *pRef[3];
	int nRefPitches[3];
	unsigned char *pDstYUY2;
	int nDstPitchYUY2;
	int nOffset[3];

	nOffset[0] = nHPadding*pixelsize + nVPadding * nLoopPitches[0];
	nOffset[1] = nOffset[2] = (nHPadding >> nLogxRatioUV)*pixelsize + (nVPadding >> nLogyRatioUV) * nLoopPitches[1];

	int nLogPel = ilog2(nPel); //shift

	PVideoFrame mvn = _mv_clip_ptr->GetFrame (nvec, env_ptr);
	_mv_clip_ptr->Update(mvn, env_ptr);
	mvn = 0; // free

	PVideoFrame	src = super->GetFrame(nsrc, env_ptr);
	PVideoFrame dst = env_ptr->NewVideoFrame(vi);
	bool				usable_flag = _mv_clip_ptr->IsUsable();
	int				nref;
	_mv_clip_ptr->use_ref_frame (nref, usable_flag, super, nsrc, env_ptr);

   if (usable_flag)
   {
		if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 )
		{
			if (!planar)
			{
				pDstYUY2       = dst->GetWritePtr();
				nDstPitchYUY2  = dst->GetPitch();
				pDst[0]        = DstPlanes->GetPtr();
				pDst[1]        = DstPlanes->GetPtrU();
				pDst[2]        = DstPlanes->GetPtrV();
				nDstPitches[0] = DstPlanes->GetPitch();
				nDstPitches[1] = DstPlanes->GetPitchUV();
				nDstPitches[2] = DstPlanes->GetPitchUV();
			}
			else
			{
				pDst[0]        = dst->GetWritePtr();
				pDst[1]        = pDst[0] + nWidth;
				pDst[2]        = pDst[1] + nWidth/2;
				nDstPitches[0] = dst->GetPitch();
				nDstPitches[1] = nDstPitches[0];
				nDstPitches[2] = nDstPitches[0];
			}
			pSrc[0]        = src->GetReadPtr();
			pSrc[1]        = pSrc[0] + nSuperWidth;
			pSrc[2]        = pSrc[1] + nSuperWidth/2;
			nSrcPitches[0] = src->GetPitch();
			nSrcPitches[1] = nSrcPitches[0];
			nSrcPitches[2] = nSrcPitches[0];
		}

		else
		{
			pDst[0]        = YWPLAN(dst);
			pDst[1]        = UWPLAN(dst);
			pDst[2]        = VWPLAN(dst);
			nDstPitches[0] = YPITCH(dst);
			nDstPitches[1] = UPITCH(dst);
			nDstPitches[2] = VPITCH(dst);
			pSrc[0]        = YRPLAN(src);
			pSrc[1]        = URPLAN(src);
			pSrc[2]        = VRPLAN(src);
			nSrcPitches[0] = YPITCH(src);
			nSrcPitches[1] = UPITCH(src);
			nSrcPitches[2] = VPITCH(src);
		}

		PVideoFrame ref = super->GetFrame(nref, env_ptr);

		if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 )
		{
			pRef[0]         = ref->GetReadPtr();
			pRef[1]         = pRef[0] + nSuperWidth;
			pRef[2]         = pRef[1] + nSuperWidth/2; // xRatioUV YUY2
			nRefPitches[0]  = ref->GetPitch();
			nRefPitches[1]  = nRefPitches[0];
			nRefPitches[2]  = nRefPitches[0];
		}
		else
		{
			pRef[0]        = YRPLAN(ref);
			pRef[1]        = URPLAN(ref);
			pRef[2]        = VRPLAN(ref);
			nRefPitches[0] = YPITCH(ref);
			nRefPitches[1] = UPITCH(ref);
			nRefPitches[2] = VPITCH(ref);
		}

		if (recursion>0)
		{
			// const Time256ProviderCst	t256_prov_cst (256-recursion, 0, 0);
      if (pixelsize == 1) {
        Blend<uint8_t>(pLoop[0], pLoop[0], pRef[0], nSuperHeight, nSuperWidth, nLoopPitches[0], nLoopPitches[0], nRefPitches[0], time256 /*t256_prov_cst*/, isse_flag);
        Blend<uint8_t>(pLoop[1], pLoop[1], pRef[1], nSuperHeight >> nLogyRatioUV, nSuperWidth >> nLogxRatioUV, nLoopPitches[1], nLoopPitches[1], nRefPitches[1], time256 /*t256_prov_cst*/, isse_flag);
        Blend<uint8_t>(pLoop[2], pLoop[2], pRef[2], nSuperHeight >> nLogyRatioUV, nSuperWidth >> nLogxRatioUV, nLoopPitches[2], nLoopPitches[2], nRefPitches[2], time256 /*t256_prov_cst*/, isse_flag);
      }
      else {
        Blend<uint16_t>(pLoop[0], pLoop[0], pRef[0], nSuperHeight, nSuperWidth, nLoopPitches[0], nLoopPitches[0], nRefPitches[0], time256 /*t256_prov_cst*/, isse_flag);
        Blend<uint16_t>(pLoop[1], pLoop[1], pRef[1], nSuperHeight >> nLogyRatioUV, nSuperWidth >> nLogxRatioUV, nLoopPitches[1], nLoopPitches[1], nRefPitches[1], time256 /*t256_prov_cst*/, isse_flag);
        Blend<uint16_t>(pLoop[2], pLoop[2], pRef[2], nSuperHeight >> nLogyRatioUV, nSuperWidth >> nLogxRatioUV, nLoopPitches[2], nLoopPitches[2], nRefPitches[2], time256 /*t256_prov_cst*/, isse_flag);
      }
			pRefGOF->Update(YUVPLANES, (BYTE*)pLoop[0], nLoopPitches[0], (BYTE*)pLoop[1], nLoopPitches[1], (BYTE*)pLoop[2], nLoopPitches[2]);
		}
		else
		{
			pRefGOF->Update(YUVPLANES, (BYTE*)pRef[0], nRefPitches[0], (BYTE*)pRef[1], nRefPitches[1], (BYTE*)pRef[2], nRefPitches[2]);// v2.0
		}
		pSrcGOF->Update(YUVPLANES, (BYTE*)pSrc[0], nSrcPitches[0], (BYTE*)pSrc[1], nSrcPitches[1], (BYTE*)pSrc[2], nSrcPitches[2]);


		pPlanes[0] = pRefGOF->GetFrame(0)->GetPlane(YPLANE);
		pPlanes[1] = pRefGOF->GetFrame(0)->GetPlane(UPLANE);
		pPlanes[2] = pRefGOF->GetFrame(0)->GetPlane(VPLANE);

		pSrcPlanes[0] = pSrcGOF->GetFrame(0)->GetPlane(YPLANE);
		pSrcPlanes[1] = pSrcGOF->GetFrame(0)->GetPlane(UPLANE);
		pSrcPlanes[2] = pSrcGOF->GetFrame(0)->GetPlane(VPLANE);

    /* 
    int fieldShift = 0;
    if (fields && nPel > 1 && ((nref - n) % 2 != 0))
    {
      bool paritySrc = child->GetParity(n);
      bool parityRef = child->GetParity(nref);
      fieldShift = (paritySrc && !parityRef) ? nPel / 2 : ((parityRef && !paritySrc) ? -(nPel / 2) : 0);
      // vertical shift of fields for fieldbased video at finest level pel2
    } more elegantly:
    */
    fieldShift = ClipFnc::compute_fieldshift (child, fields, nPel, nsrc, nref);

		PROFILE_START(MOTION_PROFILE_COMPENSATION);

		Slicer         slicer (_mt_flag);

		// No overlap
		if (nOverlapX==0 && nOverlapY==0)
		{
			slicer.start (nBlkY, *this, &MVCompensate::compensate_slice_normal);
			slicer.wait ();
		}

		// Overlap
		else
		{
			if (nOverlapY > 0)
			{
				memset (
					&_boundary_cnt_arr [0],
					0,
					_boundary_cnt_arr.size () * sizeof (_boundary_cnt_arr [0])
				);
			}

			MemZoneSet(reinterpret_cast<unsigned char*>(DstShort), 0, nWidth_B*sizeof(short)*pixelsize, nHeight_B, 0, 0, dstShortPitch * sizeof(short)*pixelsize);
			if (pPlanes[1])
				MemZoneSet(reinterpret_cast<unsigned char*>(DstShortU), 0, (nWidth_B*sizeof(short)*pixelsize) >> nLogxRatioUV, nHeight_B>>nLogyRatioUV, 0, 0, dstShortPitchUV * sizeof(short)*pixelsize);
			if (pPlanes[2])
				MemZoneSet(reinterpret_cast<unsigned char*>(DstShortV), 0, (nWidth_B*sizeof(short)*pixelsize) >> nLogxRatioUV, nHeight_B>>nLogyRatioUV, 0, 0, dstShortPitchUV * sizeof(short)*pixelsize);

			slicer.start (nBlkY, *this, &MVCompensate::compensate_slice_overlap, 2);
			slicer.wait ();

      if (pixelsize == 1) {
        // nWidth_B and nHeight_B, right and bottom was blended
        Short2Bytes(pDst[0], nDstPitches[0], DstShort, dstShortPitch, nWidth_B, nHeight_B);
        if(pPlanes[1])
          Short2Bytes(pDst[1], nDstPitches[1], DstShortU, dstShortPitchUV, nWidth_B>>nLogxRatioUV, nHeight_B>>nLogyRatioUV);
        if(pPlanes[2])
          Short2Bytes(pDst[2], nDstPitches[2], DstShortV, dstShortPitchUV, nWidth_B>>nLogxRatioUV, nHeight_B>>nLogyRatioUV);
      }
      else if (pixelsize == 2)
      {
        Short2Bytes_Int32toWord16((uint16_t *)(pDst[0]), nDstPitches[0], (int *)DstShort, dstShortPitch, nWidth_B, nHeight_B, bits_per_pixel);
        if (pPlanes[1])
          Short2Bytes_Int32toWord16((uint16_t *)(pDst[1]), nDstPitches[1], (int *)DstShortU, dstShortPitchUV, nWidth_B >> nLogxRatioUV, nHeight_B >> nLogyRatioUV, bits_per_pixel);
        if (pPlanes[2])
          Short2Bytes_Int32toWord16((uint16_t *)(pDst[2]), nDstPitches[2], (int *)DstShortV, dstShortPitchUV, nWidth_B >> nLogxRatioUV, nHeight_B >> nLogyRatioUV, bits_per_pixel);
      }
		}

		// padding of the non-covered regions
		const BYTE **  pSrcMapped     = (scBehavior) ? pSrc        : pRef;
		int *          pPitchesMapped = (scBehavior) ? nSrcPitches : nRefPitches;

		if (nWidth_B < nWidth) // Right padding
		{
			BitBlt(pDst[0] + nWidth_B*pixelsize, nDstPitches[0], pSrcMapped[0] + (nWidth_B + nHPadding)*pixelsize + nVPadding * pPitchesMapped[0], pPitchesMapped[0], pixelsize * (nWidth-nWidth_B), nHeight_B, isse_flag);
			if(pPlanes[1]) // chroma u
				BitBlt(pDst[1] + (nWidth_B>>nLogxRatioUV)*pixelsize, nDstPitches[1], pSrcMapped[1] + ((nWidth_B>>nLogxRatioUV) + (nHPadding>>nLogxRatioUV))*pixelsize + (nVPadding>>nLogyRatioUV) * pPitchesMapped[1], pPitchesMapped[1], pixelsize*(nWidth-nWidth_B)>>nLogxRatioUV, nHeight_B>>nLogyRatioUV, isse_flag);
			if(pPlanes[2])	// chroma v
				BitBlt(pDst[2] + (nWidth_B>>nLogxRatioUV)*pixelsize, nDstPitches[2], pSrcMapped[2] + ((nWidth_B>>nLogxRatioUV) + (nHPadding>>nLogxRatioUV))*pixelsize + (nVPadding>>nLogyRatioUV) * pPitchesMapped[2], pPitchesMapped[2], pixelsize*(nWidth-nWidth_B)>>nLogxRatioUV, nHeight_B>>nLogyRatioUV, isse_flag);
		}

		if (nHeight_B < nHeight) // Bottom padding
		{
			BitBlt(pDst[0] + nHeight_B*nDstPitches[0], nDstPitches[0], pSrcMapped[0] + nHPadding*pixelsize + (nHeight_B + nVPadding) * pPitchesMapped[0], pPitchesMapped[0], nWidth*pixelsize, nHeight-nHeight_B, isse_flag);
			if(pPlanes[1])	// chroma u
        // PF fix 161117: nHPadding was not shifted for bottom padding, exists in 2.5.11.22??
				// BitBlt(pDst[1] + (nHeight_B>>nLogyRatioUV)*nDstPitches[1], nDstPitches[1], pSrcMapped[1] + nHPadding + ((nHeight_B + nVPadding)>>nLogyRatioUV) * pPitchesMapped[1], pPitchesMapped[1], nWidth>>nLogxRatioUV, (nHeight-nHeight_B)>>nLogyRatioUV, isse_flag);
        BitBlt(pDst[1] + (nHeight_B>>nLogyRatioUV)*nDstPitches[1], nDstPitches[1], pSrcMapped[1] + (nHPadding>>nLogxRatioUV)*pixelsize + ((nHeight_B + nVPadding)>>nLogyRatioUV) * pPitchesMapped[1], pPitchesMapped[1], pixelsize * (nWidth>>nLogxRatioUV), (nHeight-nHeight_B)>>nLogyRatioUV, isse_flag);
			if(pPlanes[2])	// chroma v
				BitBlt(pDst[2] + (nHeight_B>>nLogyRatioUV)*nDstPitches[2], nDstPitches[2], pSrcMapped[2] + (nHPadding>>nLogxRatioUV)*pixelsize + ((nHeight_B + nVPadding)>>nLogyRatioUV) * pPitchesMapped[2], pPitchesMapped[2], pixelsize * (nWidth>>nLogxRatioUV), (nHeight-nHeight_B)>>nLogyRatioUV, isse_flag);
		}

		PROFILE_STOP(MOTION_PROFILE_COMPENSATION);

		// if we're in in-loop recursive mode, we copy the frame
		if ( recursion>0 )
		{
			env_ptr->BitBlt(pLoop[0] + nOffset[0], nLoopPitches[0], pDst[0], nDstPitches[0], nWidth*pixelsize, nHeight);
			env_ptr->BitBlt(pLoop[1] + nOffset[1], nLoopPitches[1], pDst[1], nDstPitches[1], (nWidth >> nLogxRatioUV)*pixelsize, nHeight >> nLogyRatioUV);
			env_ptr->BitBlt(pLoop[2] + nOffset[2], nLoopPitches[2], pDst[2], nDstPitches[2], (nWidth >> nLogxRatioUV)*pixelsize, nHeight >> nLogyRatioUV);
		}

		if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 && !planar)
		{
			YUY2FromPlanes(
				pDstYUY2, nDstPitchYUY2, nWidth, nHeight,
				pDst[0], nDstPitches[0], pDst[1], pDst[2], nDstPitches[1], isse_flag
			);
		}
	}

	// ! usable_flag
	else
	{
		if ( !scBehavior && ( nref < vi.num_frames ) && ( nref >= 0 ))
		{
			src = super->GetFrame (nref, env_ptr);
		}
		else
		{
			src = super->GetFrame (nsrc, env_ptr);
		}

		if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 )
		{
			if (!planar)
			{
				pDstYUY2       = dst->GetWritePtr();
				nDstPitchYUY2  = dst->GetPitch();
				pDst[0]        = DstPlanes->GetPtr();
				pDst[1]        = DstPlanes->GetPtrU();
				pDst[2]        = DstPlanes->GetPtrV();
				nDstPitches[0] = DstPlanes->GetPitch();
				nDstPitches[1] = DstPlanes->GetPitchUV();
				nDstPitches[2] = DstPlanes->GetPitchUV();
			}
			else
			{
				pDst[0]        = dst->GetWritePtr();
				pDst[1]        = pDst[0] + nWidth;
				pDst[2]        = pDst[1] + nWidth/2; // YUY2
				nDstPitches[0] = dst->GetPitch();
				nDstPitches[1] = nDstPitches[0];
				nDstPitches[2] = nDstPitches[0];
			}
			pSrc[0]        = src->GetReadPtr();
			pSrc[1]        = pSrc[0] + nSuperWidth;
			pSrc[2]        = pSrc[1] + nSuperWidth/2; // YUY2
			nSrcPitches[0] = src->GetPitch();
			nSrcPitches[1] = nSrcPitches[0];
			nSrcPitches[2] = nSrcPitches[0];
		}
		else
		{
			pDst[0]        = YWPLAN(dst);
			pDst[1]        = UWPLAN(dst);
			pDst[2]        = VWPLAN(dst);
			nDstPitches[0] = YPITCH(dst);
			nDstPitches[1] = UPITCH(dst);
			nDstPitches[2] = VPITCH(dst);
			pSrc[0]        = YRPLAN(src);
			pSrc[1]        = URPLAN(src);
			pSrc[2]        = VRPLAN(src);
			nSrcPitches[0] = YPITCH(src);
			nSrcPitches[1] = UPITCH(src);
			nSrcPitches[2] = VPITCH(src);
		}

		nOffset[0] = nHPadding*pixelsize + nVPadding * nSrcPitches[0];
		nOffset[1] = nOffset[2] = (nHPadding >> nLogxRatioUV)*pixelsize + (nVPadding >> nLogyRatioUV) * nSrcPitches[1];

		env_ptr->BitBlt(pDst[0], nDstPitches[0], pSrc[0] + nOffset[0], nSrcPitches[0], nWidth*pixelsize, nHeight);
		env_ptr->BitBlt(pDst[1], nDstPitches[1], pSrc[1] + nOffset[1], nSrcPitches[1], (nWidth >> nLogxRatioUV)*pixelsize, nHeight >> nLogyRatioUV);
		env_ptr->BitBlt(pDst[2], nDstPitches[2], pSrc[2] + nOffset[2], nSrcPitches[2], (nWidth >> nLogxRatioUV)*pixelsize, nHeight >> nLogyRatioUV);

		if ( (pixelType & VideoInfo::CS_YUY2) == VideoInfo::CS_YUY2 && !planar)
		{
			YUY2FromPlanes(pDstYUY2, nDstPitchYUY2, nWidth, nHeight,
			pDst[0], nDstPitches[0], pDst[1], pDst[2], nDstPitches[1], isse_flag);
		}

		if ( recursion>0 )
		{ 
            env_ptr->BitBlt(pLoop[0], nLoopPitches[0], pSrc[0], nSrcPitches[0], nWidth*pixelsize, nHeight);
			env_ptr->BitBlt(pLoop[1], nLoopPitches[1], pSrc[1], nSrcPitches[1], (nWidth >> nLogxRatioUV)*pixelsize, nHeight >> nLogyRatioUV);
			env_ptr->BitBlt(pLoop[2], nLoopPitches[2], pSrc[2], nSrcPitches[2], (nWidth >> nLogxRatioUV)*pixelsize, nHeight >> nLogyRatioUV);
		}
	}

#ifndef _M_X64
  _mm_empty (); // (we may use double-float somewhere) Fizick
#endif

	_mv_clip_ptr = 0;

   return dst;
}



void	MVCompensate::compensate_slice_normal (Slicer::TaskData &td)
{
	assert (&td != 0);

	const int		rowsize_l = nBlkSizeY;
	const int		rowsize_c = rowsize_l >> nLogyRatioUV;

	BYTE *         pDstCur[3];
	const BYTE *   pSrcCur[3];
  ptrdiff_t      DstCurPitches[3];
  ptrdiff_t      SrcCurPitches[3];

	pDstCur[0] = pDst[0] + td._y_beg * rowsize_l * (nDstPitches[0]);
	pDstCur[1] = pDst[1] + td._y_beg * rowsize_c * (nDstPitches[1]);
	pDstCur[2] = pDst[2] + td._y_beg * rowsize_c * (nDstPitches[2]);
	pSrcCur[0] = pSrc[0] + td._y_beg * rowsize_l * (nSrcPitches[0]);
	pSrcCur[1] = pSrc[1] + td._y_beg * rowsize_c * (nSrcPitches[1]);
	pSrcCur[2] = pSrc[2] + td._y_beg * rowsize_c * (nSrcPitches[2]);

  DstCurPitches[0] = rowsize_l * nDstPitches[0];
  DstCurPitches[1] = rowsize_c * nDstPitches[1];
  DstCurPitches[2] = rowsize_c * nDstPitches[2];
  SrcCurPitches[0] = rowsize_l * nSrcPitches[0];
  SrcCurPitches[1] = rowsize_c * nSrcPitches[1];
  SrcCurPitches[2] = rowsize_c * nSrcPitches[2];

	for (int by = td._y_beg; by < td._y_end; ++by)
	{
		int xx = 0; // pixelsize-aware, no need to multiply inside
		for (int bx = 0; bx < nBlkX; ++bx)
		{
			int i = by*nBlkX + bx;
      const FakeBlockData &block = _mv_clip_ptr->GetBlock(0, i);
      /*
      blx = block.GetX() * nPel + block.GetMV().x * time256 / 256;
      bly = block.GetY() * nPel + block.GetMV().y * time256 / 256 + fieldShift;
      */
      const int      blx = block.GetX() * nPel + block.GetMV().x * time256 / 256; // 2.5.11.22
			const int      bly = block.GetY() * nPel + block.GetMV().y * time256 / 256 + fieldShift; // 2.5.11.22
			if (block.GetSAD() < _thsad)
			{
				// luma
				BLITLUMA (
					pDstCur[0] + xx, nDstPitches[0],
					pPlanes[0]->GetPointer(blx, bly), pPlanes[0]->GetPitch()
				);
				// chroma u
				if (pPlanes[1])
				{
					BLITCHROMA (
						pDstCur[1] + (xx>>nLogxRatioUV), nDstPitches[1],
						pPlanes[1]->GetPointer(blx>>nLogxRatioUV, bly>>nLogyRatioUV), pPlanes[1]->GetPitch()
					);
				}
				// chroma v
				if (pPlanes[2])
				{
					BLITCHROMA (
						pDstCur[2] + (xx>>nLogxRatioUV), nDstPitches[2],
						pPlanes[2]->GetPointer(blx>>nLogxRatioUV, bly>>nLogyRatioUV), pPlanes[2]->GetPitch()
					);
				}
			}
			else
			{
				int blxsrc = bx * nBlkSizeX * nPel;
				int blysrc = by * nBlkSizeY * nPel + fieldShift;

				BLITLUMA (
					pDstCur[0] + xx, nDstPitches[0],
					pSrcPlanes[0]->GetPointer(blxsrc, blysrc), pSrcPlanes[0]->GetPitch()
				);
				// chroma u
				if (pSrcPlanes[1])
				{
					BLITCHROMA (
						pDstCur[1] + (xx>>nLogxRatioUV), nDstPitches[1],
						pSrcPlanes[1]->GetPointer(blxsrc>>nLogxRatioUV, blysrc>>nLogyRatioUV), pSrcPlanes[1]->GetPitch()
					);
				}
				// chroma v
				if (pSrcPlanes[2])
				{
					BLITCHROMA (
						pDstCur[2] + (xx>>nLogxRatioUV), nDstPitches[2],
						pSrcPlanes[2]->GetPointer(blxsrc>>nLogxRatioUV, blysrc>>nLogyRatioUV), pSrcPlanes[2]->GetPitch()
					);
				}
			}

			xx += nBlkSizeX << pixelsize_shift;
		}	// for bx

		pDstCur[0] += DstCurPitches[0];
		pDstCur[1] += DstCurPitches[1];
		pDstCur[2] += DstCurPitches[2];
		pSrcCur[0] += SrcCurPitches[0];
		pSrcCur[1] += SrcCurPitches[1];
		pSrcCur[2] += SrcCurPitches[2];
	}	// for by
}




void	MVCompensate::compensate_slice_overlap (Slicer::TaskData &td)
{
	assert (&td != 0);

	if (   nOverlapY == 0
	    || (td._y_beg == 0 && td._y_end == nBlkY))
	{
		compensate_slice_overlap (td._y_beg, td._y_end);
	}

	else
	{
		assert (td._y_end - td._y_beg >= 2);

		compensate_slice_overlap (td._y_beg, td._y_end - 1);

		const conc::AioAdd <int>	inc_ftor (+1);

		const int		cnt_top = conc::AtomicIntOp::exec_new (
			_boundary_cnt_arr [td._y_beg],
			inc_ftor
		);
		if (td._y_beg > 0 && cnt_top == 2)
		{
			compensate_slice_overlap (td._y_beg - 1, td._y_beg);
		}

		int				cnt_bot = 2;
		if (td._y_end < nBlkY)
		{
			cnt_bot = conc::AtomicIntOp::exec_new (
				_boundary_cnt_arr [td._y_end],
				inc_ftor
			);
		}
		if (cnt_bot == 2)
		{
			compensate_slice_overlap (td._y_end - 1, td._y_end);
		}
	}
}



void	MVCompensate::compensate_slice_overlap (int y_beg, int y_end)
{
	const int		rowsize_l = nBlkSizeY - nOverlapY;
	const int		rowsize_c = rowsize_l >> nLogyRatioUV;

	unsigned short *pDstShort;
	unsigned short *pDstShortU;
	unsigned short *pDstShortV;
	BYTE *         pDstCur[3];
	const BYTE *   pSrcCur[3];
	pDstShort  = DstShort  + y_beg * rowsize_l * dstShortPitch * pixelsize;
	pDstShortU = DstShortU + y_beg * rowsize_c * dstShortPitchUV * pixelsize;
	pDstShortV = DstShortV + y_beg * rowsize_c * dstShortPitchUV * pixelsize;
	pDstCur[0] = pDst[0]   + y_beg * rowsize_l * nDstPitches[0];
	pDstCur[1] = pDst[1]   + y_beg * rowsize_c * nDstPitches[1];
	pDstCur[2] = pDst[2]   + y_beg * rowsize_c * nDstPitches[2];
	pSrcCur[0] = pSrc[0]   + y_beg * rowsize_l * nSrcPitches[0];
	pSrcCur[1] = pSrc[1]   + y_beg * rowsize_c * nSrcPitches[1];
	pSrcCur[2] = pSrc[2]   + y_beg * rowsize_c * nSrcPitches[2];

	for (int by = y_beg; by < y_end; ++by)
	{
		int wby = ((by + nBlkY - 3) / (nBlkY - 2)) * 3;
		int xx  = 0; // xx is pixelsize-aware
		for (int bx = 0; bx < nBlkX; ++bx)
		{
			// select window
			int            wbx = (bx + nBlkX - 3) / (nBlkX - 2);
			short *        winOver   = OverWins->GetWindow(wby + wbx);
			short *        winOverUV = OverWinsUV->GetWindow(wby + wbx);

			int            i = by*nBlkX + bx;
			const FakeBlockData & block = _mv_clip_ptr->GetBlock(0, i);

	     /*
      blx = block.GetX() * nPel + block.GetMV().x * time256 / 256;
      bly = block.GetY() * nPel + block.GetMV().y * time256 / 256 + fieldShift;
      */
      const int      blx = block.GetX() * nPel + block.GetMV().x * time256 / 256; // 2.5.11.22
      const int      bly = block.GetY() * nPel + block.GetMV().y * time256 / 256 + fieldShift; // 2.5.11.22

      // todo: oversluma16, overschroma16
			if (block.GetSAD() < _thsad)
			{
        if (pixelsize == 1) {
        // luma
          OVERSLUMA(
            pDstShort + xx, dstShortPitch,
            pPlanes[0]->GetPointer(blx, bly), pPlanes[0]->GetPitch(),
            winOver, nBlkSizeX
          );
          // chroma u
          if (pPlanes[1])
          {
            OVERSCHROMA(
              pDstShortU + (xx >> nLogxRatioUV), dstShortPitchUV,
              pPlanes[1]->GetPointer(blx >> nLogxRatioUV, bly >> nLogyRatioUV), pPlanes[1]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
          // chroma v
          if (pPlanes[2])
          {
            OVERSCHROMA(
              pDstShortV + (xx >> nLogxRatioUV), dstShortPitchUV,
              pPlanes[2]->GetPointer(blx >> nLogxRatioUV, bly >> nLogyRatioUV), pPlanes[2]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
        }
        else {
          // luma
          OVERSLUMA16( // (unsigned short *)((int *)(pDstShort)+current_xx) if current_xx is not pixelsize-aware
            pDstShort + xx, dstShortPitch,
            pPlanes[0]->GetPointer(blx, bly), pPlanes[0]->GetPitch(),
            winOver, nBlkSizeX
          );
          // chroma u
          if (pPlanes[1])
          {
            OVERSCHROMA16(
              pDstShortU + (xx >> nLogxRatioUV), dstShortPitchUV,
              pPlanes[1]->GetPointer(blx >> nLogxRatioUV, bly >> nLogyRatioUV), pPlanes[1]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
          // chroma v
          if (pPlanes[2])
          {
            OVERSCHROMA16(
              pDstShortV + (xx >> nLogxRatioUV), dstShortPitchUV,
              pPlanes[2]->GetPointer(blx >> nLogxRatioUV, bly >> nLogyRatioUV), pPlanes[2]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
        }
			}

			// bad compensation, use src
			else
			{
				int blxsrc = bx * (nBlkSizeX - nOverlapX) * nPel;
				int blysrc = by * (nBlkSizeY - nOverlapY) * nPel + fieldShift;

        if (pixelsize == 1) {
          OVERSLUMA(
            pDstShort + xx, dstShortPitch,
            pSrcPlanes[0]->GetPointer(blxsrc, blysrc), pSrcPlanes[0]->GetPitch(),
            winOver, nBlkSizeX
          );
          // chroma u
          if (pSrcPlanes[1])
          {
            OVERSCHROMA(
              pDstShortU + (xx >> nLogxRatioUV), dstShortPitchUV,
              pSrcPlanes[1]->GetPointer(blxsrc >> nLogxRatioUV, blysrc >> nLogyRatioUV), pSrcPlanes[1]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
          // chroma v
          if (pSrcPlanes[2])
          {
            OVERSCHROMA(
              pDstShortV + (xx >> nLogxRatioUV), dstShortPitchUV,
              pSrcPlanes[2]->GetPointer(blxsrc >> nLogxRatioUV, blysrc >> nLogyRatioUV), pSrcPlanes[2]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
        }
        else {
          // pixelsize == 2
          OVERSLUMA16(
            pDstShort + xx, dstShortPitch,
            pSrcPlanes[0]->GetPointer(blxsrc, blysrc), pSrcPlanes[0]->GetPitch(),
            winOver, nBlkSizeX
          );
          // chroma u
          if (pSrcPlanes[1])
          {
            OVERSCHROMA16(
              pDstShortU + (xx >> nLogxRatioUV), dstShortPitchUV,
              pSrcPlanes[1]->GetPointer(blxsrc >> nLogxRatioUV, blysrc >> nLogyRatioUV), pSrcPlanes[1]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
          // chroma v
          if (pSrcPlanes[2])
          {
            OVERSCHROMA16(
              pDstShortV + (xx >> nLogxRatioUV), dstShortPitchUV,
              pSrcPlanes[2]->GetPointer(blxsrc >> nLogxRatioUV, blysrc >> nLogyRatioUV), pSrcPlanes[2]->GetPitch(),
              winOverUV, nBlkSizeX >> nLogxRatioUV
            );
          }
        }
			}

			xx += (nBlkSizeX - nOverlapX) << pixelsize_shift; // really if xx is a simple counter or byte pointer, this should be *2 (pixelsize==1) or *4 (pixelsize==2)
		}	// for bx

		pDstShort  += rowsize_l * dstShortPitch * pixelsize;
		pDstShortU += rowsize_c * dstShortPitchUV * pixelsize;
		pDstShortV += rowsize_c * dstShortPitchUV * pixelsize;
		pDstCur[0] += rowsize_l * nDstPitches[0];
		pDstCur[1] += rowsize_c * nDstPitches[1];
		pDstCur[2] += rowsize_c * nDstPitches[2];
		pSrcCur[0] += rowsize_l * nSrcPitches[0];
		pSrcCur[1] += rowsize_c * nSrcPitches[1];
		pSrcCur[2] += rowsize_c * nSrcPitches[2];
	}	// for by
}



// Returns false if center frame should be used.
bool	MVCompensate::compute_src_frame (int &nsrc, int &nvec, int &vindex, int n) const
{
	assert (n >= 0);

	bool           comp_flag = true;

	nsrc   = n;
	nvec   = n;
	vindex = 0;

	if (_multi_flag)
	{
		int				tbsize = _trad * 2;
		if (_center_flag)
		{
			++ tbsize;

			const int		base = n / tbsize;
			const int		offset = n - base * tbsize;
			if (offset == _trad)
			{
				comp_flag = false;
				vindex = 0;
				nvec   = 0;
			}
			else
			{
				const int		bf = (offset > _trad) ? 1-2 : 0-2;
				const int		td = abs (offset - _trad);
				vindex = td * 2 + bf;
			}
			nsrc = base;
			nvec = base;
		}
		else
		{
			nsrc   = n / tbsize;
			vindex = n % tbsize;
		}
	}

	return (comp_flag);
}



