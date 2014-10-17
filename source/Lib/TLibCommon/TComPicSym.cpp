/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2013, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     TComPicSym.cpp
    \brief    picture symbol class
*/

#include "TComPicSym.h"
#include "picyuv.h"

using namespace x265;

TComPicSym::TComPicSym()
{
    memset(this, 0, sizeof(*this));
}

bool TComPicSym::create(x265_param *param)
{
    uint32_t i;

    m_numPartitions   = 1 << (g_maxFullDepth * 2);
    m_numPartInCUSize = 1 << g_maxFullDepth;

    uint32_t widthInCU  = (param->sourceWidth  + g_maxCUSize - 1) >> g_maxLog2CUSize;
    uint32_t heightInCU = (param->sourceHeight + g_maxCUSize - 1) >> g_maxLog2CUSize;
    m_numCUsInFrame = widthInCU * heightInCU;

    m_slice = new Slice;
    m_picCTU = new TComDataCU[m_numCUsInFrame];

    uint32_t sizeL = 1 << (g_maxLog2CUSize * 2);
    uint32_t sizeC = sizeL >> (CHROMA_H_SHIFT(param->internalCsp) + CHROMA_V_SHIFT(param->internalCsp));

    m_cuMemPool.create(m_numPartitions, sizeL, sizeC, m_numCUsInFrame);
    m_mvFieldMemPool.create(m_numPartitions, m_numCUsInFrame);
    for (i = 0; i < m_numCUsInFrame; i++)
        m_picCTU[i].initialize(&m_cuMemPool, &m_mvFieldMemPool, m_numPartitions, g_maxCUSize, param->internalCsp, i);

    return true;
}

void TComPicSym::destroy()
{
    m_cuMemPool.destroy();
    m_mvFieldMemPool.destroy();
    delete m_slice;
    delete [] m_picCTU;
    delete m_saoParam;
}
