/*****************************************************************************
 * Copyright (C) 2013 x265 project
 *
 * Authors: Steve Borho <steve@borho.org>
 *          Deepthi Devaki <deepthidevaki@multicorewareinc.com>
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

#include "TLibCommon/CommonDef.h"
#include "TLibCommon/TComPicYuv.h"
#include "TLibCommon/TComPrediction.h"
#include "PPA/ppa.h"
#include "primitives.h"
#include "reference.h"

using namespace x265;

MotionReference::MotionReference(TComPicYuv* pic)
{
    /* Create buffers for Hpel/Qpel Planes */
    m_lumaStride = pic->getStride();
    m_startPad = pic->m_iLumaMarginY * m_lumaStride + pic->m_iLumaMarginX;
    m_reconPic = pic;
    m_next = NULL;

    size_t padwidth = pic->getWidth() + pic->m_iLumaMarginX * 2;
    size_t padheight = pic->getHeight() + pic->m_iLumaMarginY * 2;

    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            if (i == 0 && j == 0)
                continue;
            m_lumaPlane[i][j] = (Pel*)xMalloc(pixel,  padwidth * padheight) + m_startPad;
        }
    }

    m_lumaPlane[0][0] = m_reconPic->m_apiPicBufY + m_startPad;

    m_width = m_reconPic->getWidth();
    m_height = m_reconPic->getHeight();
    m_intStride = m_width + (s_tmpMarginX << 2);
    m_extendOffset = s_tmpMarginY * m_lumaStride + s_tmpMarginX;
    m_offsetToLuma = (s_tmpMarginY << 1) * m_intStride  + (s_tmpMarginX << 1);
    m_filterWidth = m_width + (s_tmpMarginX << 1);
    m_filterHeight = m_height + (s_tmpMarginY << 1);
    m_extendWidth = m_width + 2 * s_tmpMarginX;
    m_extendHeight = m_height + 2 * s_tmpMarginY;
}

MotionReference::~MotionReference()
{
    for (int i = 0; i < 4; i++)
    {
        for (int j = 0; j < 4; j++)
        {
            if (i == 0 && j == 0)
                continue;
            if (m_lumaPlane[i][j])
            { 
                xFree(m_lumaPlane[i][j] - m_startPad);
            }
        }
    }
}

void MotionReference::generateReferencePlanes()
{
    generateReferencePlane(0);
    generateReferencePlane(1);
    generateReferencePlane(2);
    generateReferencePlane(3);
}

void MotionReference::generateReferencePlane(int idx)
{
    PPAScopeEvent(GenerateReferencePlanes);

    short* filteredBlockTmp = (short*)xMalloc(short, m_intStride * (m_height + (s_tmpMarginY << 2)));

    Pel *srcPtr = m_reconPic->getLumaAddr() - (s_tmpMarginY + 4) * m_lumaStride - (s_tmpMarginX + 4);
    Short *intPtr = filteredBlockTmp + m_offsetToLuma - (s_tmpMarginY + 4) * m_intStride - (s_tmpMarginX + 4);
    Pel *dstPtr;

    if (idx == 0)
    {
        // no horizontal filter necessary
        primitives.ipfilterConvert_p_s(g_bitDepthY, (pixel*)srcPtr, m_lumaStride, intPtr, m_intStride, m_filterWidth + 8, m_filterHeight + 8);

        /* The 0,0 (full-pel) plane needs no interpolation, and was already extended by TComPicYuv */
    }
    else
    {
        // Horizontal filter into intermediate buffer
        primitives.ipFilter_p_s[FILTER_H_P_S_8](g_bitDepthY, (pixel*)srcPtr, m_lumaStride, intPtr, m_intStride, m_filterWidth + 8, m_filterHeight + 8, TComPrediction::m_lumaFilter[idx]);

        // Generate @ 0,idx
        intPtr = filteredBlockTmp  + m_offsetToLuma - s_tmpMarginY * m_intStride - s_tmpMarginX;
        dstPtr = m_lumaPlane[idx][0]  - s_tmpMarginY * m_lumaStride - s_tmpMarginX;
        primitives.ipFilter_s_p[FILTER_V_S_P_8](g_bitDepthY, intPtr, m_intStride, (pixel*)dstPtr, m_lumaStride,  m_filterWidth, m_filterHeight, TComPrediction::m_lumaFilter[0]);
        m_reconPic->xExtendPicCompBorder(dstPtr, m_lumaStride, m_extendWidth, m_extendHeight, m_reconPic->m_iLumaMarginX - s_tmpMarginX, m_reconPic->m_iLumaMarginY - s_tmpMarginY);
    }

    // Generate @ 1,idx
    intPtr = filteredBlockTmp + m_offsetToLuma - s_tmpMarginY * m_intStride - s_tmpMarginX;
    dstPtr = m_lumaPlane[idx][1] - m_extendOffset;
    primitives.ipFilter_s_p[FILTER_V_S_P_8](g_bitDepthY, intPtr, m_intStride, (pixel*)dstPtr, m_lumaStride,  m_filterWidth, m_filterHeight, TComPrediction::m_lumaFilter[1]);
    m_reconPic->xExtendPicCompBorder(dstPtr, m_lumaStride, m_extendWidth, m_extendHeight, m_reconPic->m_iLumaMarginX - s_tmpMarginX, m_reconPic->m_iLumaMarginY - s_tmpMarginY);

    // Generate @ 2,idx
    dstPtr = m_lumaPlane[idx][2] - m_extendOffset;
    primitives.ipFilter_s_p[FILTER_V_S_P_8](g_bitDepthY, intPtr, m_intStride, (pixel*)dstPtr, m_lumaStride, m_filterWidth, m_filterHeight, TComPrediction::m_lumaFilter[2]);
    m_reconPic->xExtendPicCompBorder(dstPtr, m_lumaStride, m_extendWidth, m_extendHeight, m_reconPic->m_iLumaMarginX - s_tmpMarginX, m_reconPic->m_iLumaMarginY - s_tmpMarginY);

    // Generate @ 3,idx
    dstPtr = m_lumaPlane[idx][3] - m_extendOffset;
    primitives.ipFilter_s_p[FILTER_V_S_P_8](g_bitDepthY, intPtr, m_intStride, (pixel*)dstPtr, m_lumaStride, m_filterWidth, m_filterHeight, TComPrediction::m_lumaFilter[3]);
    m_reconPic->xExtendPicCompBorder(dstPtr, m_lumaStride, m_extendWidth, m_extendHeight, m_reconPic->m_iLumaMarginX - s_tmpMarginX, m_reconPic->m_iLumaMarginY - s_tmpMarginY);

    xFree(filteredBlockTmp);
}
