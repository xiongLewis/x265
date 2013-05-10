/*****************************************************************************
 * Copyright (C) 2013 x265 project
 *
 * Authors: Steve Borho <steve@borho.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111, USA.
 *
 * This program is also available under a commercial proprietary license.
 * For more information, contact us at licensing@multicorewareinc.com.
 *****************************************************************************/

#ifndef __MOTIONESTIMATE__
#define __MOTIONESTIMATE__

#include "primitives.h"
#include "mv.h"
#include "bitcost.h"

namespace x265 {
// private x265 namespace

struct MotionReference
{
    /* indexed by [hpelx|qpelx][hpely|qpely][plane]
     * aka: full pel Y, Cb, Cr referenced by plane[0][0][0..2] */
    pixel* plane[4][4][3];

    intptr_t lumaStride;

    intptr_t chromaStride;

protected:

    MotionReference& operator=(const MotionReference&);
};

class MotionEstimate : public BitCost
{
protected:

    /* Aligned copy of original pixels, in case their native alignment
     * was insufficient (AMP) */
    ALIGN_VAR_32(pixel, fenc[64 * FENC_STRIDE]);

    pixel *fencplanes[3];
    intptr_t fencLumaStride;
    intptr_t fencChromaStride;

    short *residual[3];
    intptr_t resLumaStride;
    intptr_t resChromaStride;

    MV mvmin, mvmax;

    MotionReference *ref;   // current reference frame

    pixelcmp sad;
    pixelcmp satd;
    pixelcmp_x3 sad_x3;
    pixelcmp_x4 sad_x4;

    int blockWidth;
    int blockHeight;
    int blockOffset;

    MotionEstimate& operator=(const MotionEstimate&);

public:

    MotionEstimate() {}

    ~MotionEstimate() {}

    /* Methods called at slice setup */

    void setSourcePlanes(pixel *Y, pixel *Cb, pixel *Cr, intptr_t luma, intptr_t chroma)
    {
        fencplanes[0] = Y;
        fencplanes[1] = Cb;
        fencplanes[2] = Cr;
        fencLumaStride = luma;
        fencChromaStride = chroma;
    }

    /* Methods called at CU setup */

    void setSourcePU(int offset, int pwidth, int pheight);

    void setSearchLimits(MV& min, MV& max)    { mvmin = min; mvmax = max; }

    /* Methods called for searches */

    int bufSAD(pixel *fref, intptr_t stride)  { return sad(fenc, FENC_STRIDE, fref, stride); }

    int bufSATD(pixel *fref, intptr_t stride) { return satd(fenc, FENC_STRIDE, fref, stride); }

    void setReference(MotionReference* tref)  { ref = tref; }

    /* returns SATD QPEL cost of best outMV for this PU */
    int motionEstimate(const MV &qmvp, int numCandidates, const MV *mvc, int merange, MV &outQMv);

protected:

    /* Helper functions for motionEstimate.  fref is coincident block in reference frame */
    inline int fpelSad(pixel *fref, const MV& fmv)
    {
        return sad(fenc, FENC_STRIDE,
                   fref + fmv.y * ref->lumaStride + fmv.x,
                   ref->lumaStride);
    }

    inline int qpelSad(const MV& qmv)
    {
        MV fmv = qmv >> 2;
        pixel *qfref = ref->plane[qmv.x & 3][qmv.y & 3][0] + blockOffset;
        return sad(fenc, FENC_STRIDE,
                   qfref + fmv.y * ref->lumaStride + fmv.x,
                   ref->lumaStride);
    }

    inline int qpelSatd(const MV& qmv)
    {
        MV fmv = qmv >> 2;
        pixel *qfref = ref->plane[qmv.x & 3][qmv.y & 3][0] + blockOffset;
        return satd(fenc, FENC_STRIDE,
                    qfref + fmv.y * ref->lumaStride + fmv.x,
                    ref->lumaStride);
    }
};
}

#endif // ifndef __MOTIONESTIMATE__
