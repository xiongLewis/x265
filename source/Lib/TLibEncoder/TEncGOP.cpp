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

/** \file     TEncGOP.cpp
    \brief    GOP encoder class
*/

#include "TEncTop.h"
#include "TEncGOP.h"
#include "TEncPic.h"
#include "TEncAnalyze.h"
#include "TLibCommon/SEI.h"
#include "TLibCommon/NAL.h"
#include "PPA/ppa.h"
#include "NALwrite.h"
#include "common.h"

#include <list>
#include <algorithm>
#include <functional>
#include <time.h>
#include <math.h>

using namespace std;
using namespace x265;

/**
 * Produce an ascii(hex) representation of picture digest.
 *
 * Returns: a statically allocated null-terminated string.  DO NOT FREE.
 */
inline const char*digestToString(const unsigned char digest[3][16], int numChar)
{
    const char* hex = "0123456789abcdef";
    static char string[99];
    int cnt = 0;

    for (int yuvIdx = 0; yuvIdx < 3; yuvIdx++)
    {
        for (int i = 0; i < numChar; i++)
        {
            string[cnt++] = hex[digest[yuvIdx][i] >> 4];
            string[cnt++] = hex[digest[yuvIdx][i] & 0xf];
        }

        string[cnt++] = ',';
    }

    string[cnt - 1] = '\0';
    return string;
}

static inline Int getLSB(Int poc, Int maxLSB)
{
    if (poc >= 0)
    {
        return poc % maxLSB;
    }
    else
    {
        return (maxLSB - ((-poc) % maxLSB)) % maxLSB;
    }
}

//! \ingroup TLibEncoder
//! \{

// ====================================================================================================================
// Constructor / destructor / initialization / destroy
// ====================================================================================================================
TEncGOP::TEncGOP()
{
    m_pcCfg               = NULL;
    m_cFrameEncoders      = NULL;
    m_pcEncTop            = NULL;
    m_pcRateCtrl          = NULL;
    m_iLastIDR            = 0;
    m_totalCoded          = 0;
    m_bRefreshPending     = 0;
    m_pocCRA              = 0;
    m_numLongTermRefPicSPS = 0;
    m_lastBPSEI           = 0;
    ::memset(m_ltRefPicPocLsbSps, 0, sizeof(m_ltRefPicPocLsbSps));
    ::memset(m_ltRefPicUsedByCurrPicFlag, 0, sizeof(m_ltRefPicUsedByCurrPicFlag));
}

TEncGOP::~TEncGOP()
{}

/** Create list to contain pointers to LCU start addresses of slice.
 */
Void  TEncGOP::create()
{}

Void TEncGOP::destroy()
{
    // release and join the worker thread
    m_threadActive = false;
    m_batchSize = 0;
    m_startPOC = 0;
    m_inputLock.release();
    Thread::stop();

    TComList<TComPic*>::iterator iterPic = m_cListPic.begin();
    Int iSize = Int(m_cListPic.size());
    for (Int i = 0; i < iSize; i++)
    {
        TComPic* pcPic = *(iterPic++);

        pcPic->destroy();
        delete pcPic;
        pcPic = NULL;
    }

    if (m_cFrameEncoders)
    {
        m_cFrameEncoders->destroy();
        delete m_cFrameEncoders;
    }

    if (m_recon)
        delete [] m_recon;
}

Void TEncGOP::init(TEncTop* pcTEncTop)
{
    m_pcEncTop   = pcTEncTop;
    m_pcCfg      = pcTEncTop;
    m_pcRateCtrl = pcTEncTop->getRateCtrl();

    m_threadActive = true;
    m_inputLock.acquire();
    m_batchSize = 0;
    m_startPOC = 0;
    Thread::start();

    // initialize SPS
    pcTEncTop->xInitSPS(&m_cSPS);

    // initialize PPS
    m_cPPS.setSPS(&m_cSPS);

    pcTEncTop->xInitPPS(&m_cPPS);
    pcTEncTop->xInitRPS(&m_cSPS);

    int numRows = (m_pcCfg->getSourceHeight() + m_cSPS.getMaxCUHeight() - 1) / m_cSPS.getMaxCUHeight();
    m_cFrameEncoders = new x265::FrameEncoder(pcTEncTop->getThreadPool());
    m_cFrameEncoders->init(pcTEncTop, numRows);

    int maxGOP = m_pcCfg->getNumGOPThreads() > 1 ? m_pcCfg->getIntraPeriod() : 1;
    maxGOP = X265_MAX(maxGOP, m_pcCfg->getGOPSize()) + m_pcCfg->getGOPSize();
    m_recon = new x265_picture_t[maxGOP];

    // pre-allocate a full keyframe interval of TComPic
    for (int i = 0; i < maxGOP; i++)
    {
        TComPic *pcPic;
        if (m_pcCfg->getUseAdaptiveQP())
        {
            TEncPic* pcEPic = new TEncPic;
            pcEPic->create(m_pcCfg->getSourceWidth(), m_pcCfg->getSourceHeight(), g_maxCUWidth, g_maxCUHeight, g_maxCUDepth,
                           m_cPPS.getMaxCuDQPDepth() + 1, m_pcCfg->getConformanceWindow(), m_pcCfg->getDefaultDisplayWindow());
            pcPic = pcEPic;
        }
        else
        {
            pcPic = new TComPic;
            pcPic->create(m_pcCfg->getSourceWidth(), m_pcCfg->getSourceHeight(), g_maxCUWidth, g_maxCUHeight, g_maxCUDepth,
                          m_pcCfg->getConformanceWindow(), m_pcCfg->getDefaultDisplayWindow());
        }
        if (m_pcCfg->getUseSAO())
        {
            pcPic->getPicSym()->allocSaoParam(m_cFrameEncoders->getSAO());
        }
        pcPic->getSlice()->setPOC(MAX_INT);
        m_cListPic.pushBack(pcPic);
    }
}

int TEncGOP::getOutputs(x265_picture_t** pic_out, std::list<AccessUnit>& accessUnitsOut)
{
    while (1)
    {
        m_outputLock.acquire();
        if (!m_accessUnits.empty())
            break;
        m_outputLock.release();
    }

    m_inputLock.acquire();

    // move access units from member variable list to end of user's container
    accessUnitsOut.splice(accessUnitsOut.end(), m_accessUnits);

    if (pic_out)
    {
        for (Int i = 0; i < m_batchSize; i++)
        {
            TComList<TComPic*>::iterator iterPic = m_cListPic.begin();
            while (iterPic != m_cListPic.end() && (*iterPic)->getPOC() != (m_startPOC + i))
            {
                iterPic++;
            }

            TComPicYuv *recpic = (*iterPic)->getPicYuvRec();
            x265_picture_t& recon = m_recon[i];
            recon.planes[0] = recpic->getLumaAddr();
            recon.stride[0] = recpic->getStride();
            recon.planes[1] = recpic->getCbAddr();
            recon.stride[1] = recpic->getCStride();
            recon.planes[2] = recpic->getCrAddr();
            recon.stride[2] = recpic->getCStride();
            recon.bitDepth = sizeof(Pel) * 8;
        }

        *pic_out = m_recon;
    }
    m_outputLock.release();
    return m_batchSize;
}

void TEncGOP::addPicture(Int poc, const x265_picture_t *pic)
{
    TComSlice::sortPicList(m_cListPic);

    TComList<TComPic*>::iterator iterPic = m_cListPic.begin();
    while (iterPic != m_cListPic.end())
    {
        TComPic *pcPic = *(iterPic++);
        if (pcPic->getSlice()->isReferenced() == false)
        {
            pcPic->getSlice()->setPOC(poc);
            pcPic->getPicYuvOrg()->copyFromPicture(*pic);
            pcPic->getPicYuvRec()->setBorderExtension(false);
            pcPic->getSlice()->setReferenced(true);
            if (m_pcCfg->getUseAdaptiveQP())
            {
                // compute image characteristics
                dynamic_cast<TEncPic*>(pcPic)->preanalyze();
            }
            return;
        }
    }

    assert(!"No room for added picture");
}

void TEncGOP::threadMain()
{
    int lastPOC = -1;

    while (m_threadActive)
    {
        m_inputLock.acquire();

        if (m_batchSize > 0 && m_startPOC != lastPOC)
        {
            m_outputLock.acquire();

            lastPOC = m_startPOC;
            int poc = m_totalCoded;
            int numEncoded = 0;
            while (numEncoded < m_batchSize)
            {
                int gopSize = (poc == 0) ? 1 : m_pcCfg->getGOPSize();
                gopSize = X265_MIN(gopSize, m_batchSize - numEncoded);
                compressGOP(poc + gopSize - 1, gopSize);
                numEncoded += gopSize;
                poc += gopSize;
            }

            m_inputLock.release();
            m_outputLock.release();
        }
        else
            m_inputLock.release();
    }
}

void TEncGOP::processKeyframeInterval(Int POCLast, Int numFrames)
{
    // TEncTOP has queued an entire keyframe interval of frames in our picture list
    // Encode them in batches of m_pcCfg->getGOPSize()
    m_totalCoded = POCLast - numFrames + 1;
    m_startPOC = m_totalCoded;
    m_batchSize = numFrames;
    m_inputLock.release();
}

// ====================================================================================================================
// Public member functions
// ====================================================================================================================
Void TEncGOP::compressGOP(Int pocLast, Int numPicRecvd)
{
    PPAScopeEvent(TEncGOP_compressGOP);

    AccessUnit::iterator  itLocationToPushSliceHeaderNALU; // used to store location where NALU containing slice header is to be inserted
    Int                   picSptDpbOutputDuDelay = 0;
    UInt*                 accumBitsDU = NULL;
    UInt*                 accumNalsDU = NULL;
    UInt                  uiOneBitstreamPerSliceLength = 0; // TODO: Remove
    TComOutputBitstream*  pcBitstreamRedirect = new TComOutputBitstream;
    TComOutputBitstream*  outStreams = NULL;
    x265::FrameEncoder*   frameEncoder = &m_cFrameEncoders[0];
    TEncEntropy*          entropyCoder = frameEncoder->getEntropyCoder(0);
    TEncSlice*            sliceEncoder = frameEncoder->getSliceEncoder();
    TEncCavlc*            cavlcCoder   = frameEncoder->getCavlcCoder();
    TEncSbac*             sbacCoder    = frameEncoder->getSingletonSbac();
    TEncBinCABAC*         binCABAC     = frameEncoder->getBinCABAC();
    TComLoopFilter*       loopFilter   = frameEncoder->getLoopFilter();
    TComBitCounter*       bitCounter   = frameEncoder->getBitCounter();
    TEncSampleAdaptiveOffset* sao      = frameEncoder->getSAO();
    Bool bActiveParameterSetSEIPresentInAU = false;
    Bool bBufferingPeriodSEIPresentInAU = false;
    Bool bPictureTimingSEIPresentInAU = false;
    Bool bNestedBufferingPeriodSEIPresentInAU = false;

    if (m_pcCfg->getUseRateCtrl())
    {
        m_pcRateCtrl->initRCGOP(numPicRecvd);
    }

    Int gopSize = pocLast == 0 ? 1 : m_pcCfg->getGOPSize();
    Int numPicCoded = 0;
    Bool writeSOP = m_pcCfg->getSOPDescriptionSEIEnabled();

    SEIPictureTiming pictureTimingSEI;

    // Initialize Scalable Nesting SEI with single layer values
    SEIScalableNesting scalableNestingSEI;
    scalableNestingSEI.m_bitStreamSubsetFlag           = 1;     // If the nested SEI messages are picture buffering SEI messages, picture timing SEI messages or sub-picture timing SEI messages, bitstream_subset_flag shall be equal to 1
    scalableNestingSEI.m_nestingOpFlag                 = 0;
    scalableNestingSEI.m_nestingNumOpsMinus1           = 0;     // nesting_num_ops_minus1
    scalableNestingSEI.m_allLayersFlag                 = 0;
    scalableNestingSEI.m_nestingNoOpMaxTemporalIdPlus1 = 6 + 1; // nesting_no_op_max_temporal_id_plus1
    scalableNestingSEI.m_nestingNumLayersMinus1        = 1 - 1; // nesting_num_layers_minus1
    scalableNestingSEI.m_nestingLayerId[0]             = 0;
    scalableNestingSEI.m_callerOwnsSEIs                = true;

    SEIDecodingUnitInfo decodingUnitInfoSEI;
    for (Int gopIdx = 0; gopIdx < gopSize; gopIdx++)
    {
        UInt colDir = 1;

        //select uiColDir
        Int closeLeft = 1, closeRight = -1;
        for (Int i = 0; i < m_pcCfg->getGOPEntry(gopIdx).m_numRefPics; i++)
        {
            Int iRef = m_pcCfg->getGOPEntry(gopIdx).m_referencePics[i];
            if (iRef > 0 && (iRef < closeRight || closeRight == -1))
            {
                closeRight = iRef;
            }
            else if (iRef < 0 && (iRef > closeLeft || closeLeft == 1))
            {
                closeLeft = iRef;
            }
        }

        if (closeRight > -1)
        {
            closeRight = closeRight + m_pcCfg->getGOPEntry(gopIdx).m_POC - 1;
        }
        if (closeLeft < 1)
        {
            closeLeft = closeLeft + m_pcCfg->getGOPEntry(gopIdx).m_POC - 1;
            while (closeLeft < 0)
            {
                closeLeft += gopSize;
            }
        }
        Int leftQP = 0, rightQP = 0;
        for (Int i = 0; i < gopSize; i++)
        {
            if (m_pcCfg->getGOPEntry(i).m_POC == (closeLeft % gopSize) + 1)
            {
                leftQP = m_pcCfg->getGOPEntry(i).m_QPOffset;
            }
            if (m_pcCfg->getGOPEntry(i).m_POC == (closeRight % gopSize) + 1)
            {
                rightQP = m_pcCfg->getGOPEntry(i).m_QPOffset;
            }
        }

        if (closeRight > -1 && rightQP < leftQP)
        {
            colDir = 0;
        }

        /////////////////////////////////////////////////////////////////////////////////////////////////// Initial to start encoding
        Int pocCurr = pocLast - numPicRecvd + m_pcCfg->getGOPEntry(gopIdx).m_POC;
        if (pocLast == 0)
        {
            pocCurr = 0;
        }
        if (pocCurr >= m_pcCfg->getFramesToBeEncoded())
        {
            continue;
        }

        if (getNalUnitType(pocCurr, m_iLastIDR) == NAL_UNIT_CODED_SLICE_IDR_W_RADL || getNalUnitType(pocCurr, m_iLastIDR) == NAL_UNIT_CODED_SLICE_IDR_N_LP)
        {
            m_iLastIDR = pocCurr;
        }
        // start a new access unit: create an entry in the list of output access units
        m_accessUnits.push_back(AccessUnit());
        AccessUnit& accessUnit = m_accessUnits.back();

        TComPic*   pic = NULL;
        TComSlice* slice;
        {
            // Locate input picture with the correct POC (makes no assumption on
            // input picture ordering because list is often re-ordered)
            TComList<TComPic*>::iterator iterPic = m_cListPic.begin();
            while (iterPic != m_cListPic.end())
            {
                pic = *(iterPic++);
                if (pic->getPOC() == pocCurr)
                {
                    break;
                }
            }
        }
        if (!pic || pic->getPOC() != pocCurr)
        {
            printf("error: Encode frame POC not found in input list!\n");
            assert(0);
            return;
        }

        //  Slice data initialization
        sliceEncoder->setSliceIdx(0);
        slice = sliceEncoder->initEncSlice(pic, frameEncoder, gopSize <= 1, pocLast, pocCurr, gopIdx, &m_cSPS, &m_cPPS);
        slice->setLastIDR(m_iLastIDR);

        //set default slice level flag to the same as SPS level flag
        slice->setScalingList(m_pcEncTop->getScalingList());
        slice->getScalingList()->setUseTransformSkip(m_cPPS.getUseTransformSkip());
        if (m_pcEncTop->getUseScalingListId() == SCALING_LIST_OFF)
        {
            frameEncoder->setFlatScalingList();
            frameEncoder->setUseScalingList(false);
            m_cSPS.setScalingListPresentFlag(false);
            m_cPPS.setScalingListPresentFlag(false);
        }
        else if (m_pcEncTop->getUseScalingListId() == SCALING_LIST_DEFAULT)
        {
            slice->setDefaultScalingList();
            m_cSPS.setScalingListPresentFlag(false);
            m_cPPS.setScalingListPresentFlag(false);
            frameEncoder->setScalingList(slice->getScalingList());
            frameEncoder->setUseScalingList(true);
        }
        else if (m_pcEncTop->getUseScalingListId() == SCALING_LIST_FILE_READ)
        {
            if (slice->getScalingList()->xParseScalingList(m_pcCfg->getScalingListFile()))
            {
                slice->setDefaultScalingList();
            }
            slice->getScalingList()->checkDcOfMatrix();
            m_cSPS.setScalingListPresentFlag(slice->checkDefaultScalingList());
            m_cPPS.setScalingListPresentFlag(false);
            frameEncoder->setScalingList(slice->getScalingList());
            frameEncoder->setUseScalingList(true);
        }
        else
        {
            printf("error : ScalingList == %d no support\n", m_pcEncTop->getUseScalingListId());
            assert(0);
        }

        if (slice->getSliceType() == B_SLICE && m_pcCfg->getGOPEntry(gopIdx).m_sliceType == 'P')
        {
            slice->setSliceType(P_SLICE);
        }
        // Set the nal unit type
        slice->setNalUnitType(getNalUnitType(pocCurr, m_iLastIDR));
        if (slice->getTemporalLayerNonReferenceFlag())
        {
            if (slice->getNalUnitType() == NAL_UNIT_CODED_SLICE_TRAIL_R)
            {
                slice->setNalUnitType(NAL_UNIT_CODED_SLICE_TRAIL_N);
            }
            if (slice->getNalUnitType() == NAL_UNIT_CODED_SLICE_RADL_R)
            {
                slice->setNalUnitType(NAL_UNIT_CODED_SLICE_RADL_N);
            }
            if (slice->getNalUnitType() == NAL_UNIT_CODED_SLICE_RASL_R)
            {
                slice->setNalUnitType(NAL_UNIT_CODED_SLICE_RASL_N);
            }
        }

        // Do decoding refresh marking if any
        slice->decodingRefreshMarking(m_pocCRA, m_bRefreshPending, m_cListPic);
        selectReferencePictureSet(slice, pocCurr, gopIdx);
        slice->getRPS()->setNumberOfLongtermPictures(0);

        if ((slice->checkThatAllRefPicsAreAvailable(m_cListPic, slice->getRPS(), false) != 0) || (slice->isIRAP()))
        {
            slice->createExplicitReferencePictureSetFromReference(m_cListPic, slice->getRPS(), slice->isIRAP());
        }
        slice->applyReferencePictureSet(m_cListPic, slice->getRPS());

        if (slice->getTLayer() > 0)
        {
            if (slice->isTemporalLayerSwitchingPoint(m_cListPic) || m_cSPS.getTemporalIdNestingFlag())
            {
                if (slice->getTemporalLayerNonReferenceFlag())
                {
                    slice->setNalUnitType(NAL_UNIT_CODED_SLICE_TSA_N);
                }
                else
                {
                    slice->setNalUnitType(NAL_UNIT_CODED_SLICE_TLA_R);
                }
            }
            else if (slice->isStepwiseTemporalLayerSwitchingPointCandidate(m_cListPic))
            {
                Bool isSTSA = true;
                for (Int ii = gopIdx + 1; (ii < m_pcCfg->getGOPSize() && isSTSA == true); ii++)
                {
                    Int lTid = m_pcCfg->getGOPEntry(ii).m_temporalId;
                    if (lTid == slice->getTLayer())
                    {
                        TComReferencePictureSet* nRPS = m_cSPS.getRPSList()->getReferencePictureSet(ii);
                        for (Int jj = 0; jj < nRPS->getNumberOfPictures(); jj++)
                        {
                            if (nRPS->getUsed(jj))
                            {
                                Int tPoc = m_pcCfg->getGOPEntry(ii).m_POC + nRPS->getDeltaPOC(jj);
                                Int kk = 0;
                                for (kk = 0; kk < m_pcCfg->getGOPSize(); kk++)
                                {
                                    if (m_pcCfg->getGOPEntry(kk).m_POC == tPoc)
                                        break;
                                }

                                Int tTid = m_pcCfg->getGOPEntry(kk).m_temporalId;
                                if (tTid >= slice->getTLayer())
                                {
                                    isSTSA = false;
                                    break;
                                }
                            }
                        }
                    }
                }

                if (isSTSA == true)
                {
                    if (slice->getTemporalLayerNonReferenceFlag())
                    {
                        slice->setNalUnitType(NAL_UNIT_CODED_SLICE_STSA_N);
                    }
                    else
                    {
                        slice->setNalUnitType(NAL_UNIT_CODED_SLICE_STSA_R);
                    }
                }
            }
        }
        arrangeLongtermPicturesInRPS(slice);
        TComRefPicListModification* refPicListModification = slice->getRefPicListModification();
        refPicListModification->setRefPicListModificationFlagL0(false);
        refPicListModification->setRefPicListModificationFlagL1(false);
        slice->setNumRefIdx(REF_PIC_LIST_0, min(m_pcCfg->getGOPEntry(gopIdx).m_numRefPicsActive, slice->getRPS()->getNumberOfPictures()));
        slice->setNumRefIdx(REF_PIC_LIST_1, min(m_pcCfg->getGOPEntry(gopIdx).m_numRefPicsActive, slice->getRPS()->getNumberOfPictures()));

        //  Set reference list
        slice->setRefPicList(m_cListPic);

        //  Slice info. refinement
        if ((slice->getSliceType() == B_SLICE) && (slice->getNumRefIdx(REF_PIC_LIST_1) == 0))
        {
            slice->setSliceType(P_SLICE);
        }

        if (slice->getSliceType() == B_SLICE)
        {
            slice->setColFromL0Flag(1 - colDir);
            Bool bLowDelay = true;
            Int curPOC = slice->getPOC();
            Int refIdx = 0;

            for (refIdx = 0; refIdx < slice->getNumRefIdx(REF_PIC_LIST_0) && bLowDelay; refIdx++)
            {
                if (slice->getRefPic(REF_PIC_LIST_0, refIdx)->getPOC() > curPOC)
                {
                    bLowDelay = false;
                }
            }

            for (refIdx = 0; refIdx < slice->getNumRefIdx(REF_PIC_LIST_1) && bLowDelay; refIdx++)
            {
                if (slice->getRefPic(REF_PIC_LIST_1, refIdx)->getPOC() > curPOC)
                {
                    bLowDelay = false;
                }
            }

            slice->setCheckLDC(bLowDelay);
        }
        else
        {
            slice->setCheckLDC(true);
        }

        colDir = 1 - colDir;

        //-------------------------------------------------------------
        slice->setRefPOCList();

        slice->setList1IdxToList0Idx();

        if (m_pcEncTop->getTMVPModeId() == 2)
        {
            if (gopIdx == 0) // first picture in SOP (i.e. forward B)
            {
                slice->setEnableTMVPFlag(0);
            }
            else
            {
                // Note: pcSlice->getColFromL0Flag() is assumed to be always 0 and getcolRefIdx() is always 0.
                slice->setEnableTMVPFlag(1);
            }
            m_cSPS.setTMVPFlagsPresent(1);
        }
        else if (m_pcEncTop->getTMVPModeId() == 1)
        {
            m_cSPS.setTMVPFlagsPresent(1);
            slice->setEnableTMVPFlag(1);
        }
        else
        {
            m_cSPS.setTMVPFlagsPresent(0);
            slice->setEnableTMVPFlag(0);
        }
        /////////////////////////////////////////////////////////////////////////////////////////////////// Compress a slice
        //  Slice compression
        if (m_pcCfg->getUseASR())
        {
            sliceEncoder->setSearchRange(slice, frameEncoder);
        }

        Bool bGPBcheck = false;
        if (slice->getSliceType() == B_SLICE)
        {
            if (slice->getNumRefIdx(RefPicList(0)) == slice->getNumRefIdx(RefPicList(1)))
            {
                bGPBcheck = true;
                for (Int i = 0; i < slice->getNumRefIdx(RefPicList(1)); i++)
                {
                    if (slice->getRefPOC(RefPicList(1), i) != slice->getRefPOC(RefPicList(0), i))
                    {
                        bGPBcheck = false;
                        break;
                    }
                }
            }
        }
        if (bGPBcheck)
        {
            slice->setMvdL1ZeroFlag(true);
        }
        else
        {
            slice->setMvdL1ZeroFlag(false);
        }
        pic->getSlice()->setMvdL1ZeroFlag(slice->getMvdL1ZeroFlag());

        Double lambda            = 0.0;
        Int actualHeadBits       = 0;
        Int actualTotalBits      = 0;
        Int estimatedBits        = 0;
        Int tmpBitsBeforeWriting = 0;

        if (m_pcCfg->getUseRateCtrl())
        {
            Int frameLevel = m_pcRateCtrl->getRCSeq()->getGOPID2Level(gopIdx);
            if (pic->getSlice()->getSliceType() == I_SLICE)
            {
                frameLevel = 0;
            }
            m_pcRateCtrl->initRCPic(frameLevel);
            estimatedBits = m_pcRateCtrl->getRCPic()->getTargetBits();

            Int sliceQP = m_pcCfg->getInitialQP();
            if ((slice->getPOC() == 0 && m_pcCfg->getInitialQP() > 0) || (frameLevel == 0 && m_pcCfg->getForceIntraQP())) // QP is specified
            {
                Int    NumberBFrames = (m_pcCfg->getGOPSize() - 1);
                Double scale         = 1.0 - Clip3(0.0, 0.5, 0.05 * (Double)NumberBFrames);
                Double qpFactor      = 0.57 * scale;
                Int    SHIFT_QP      = 12;
                Int    bitdepth_luma_qp_scale = 0;
                Double qp_temp = (Double)sliceQP + bitdepth_luma_qp_scale - SHIFT_QP;
                lambda = qpFactor * pow(2.0, qp_temp / 3.0);
            }
            else if (frameLevel == 0) // intra case, but use the model
            {
                if (m_pcCfg->getIntraPeriod() != 1) // do not refine allocated bits for all intra case
                {
                    Int bits = m_pcRateCtrl->getRCSeq()->getLeftAverageBits();
                    bits = m_pcRateCtrl->getRCSeq()->getRefineBitsForIntra(bits);
                    if (bits < 200)
                    {
                        bits = 200;
                    }
                    m_pcRateCtrl->getRCPic()->setTargetBits(bits);
                }

                list<TEncRCPic*> listPreviousPicture = m_pcRateCtrl->getPicList();
                lambda  = m_pcRateCtrl->getRCPic()->estimatePicLambda(listPreviousPicture);
                sliceQP = m_pcRateCtrl->getRCPic()->estimatePicQP(lambda, listPreviousPicture);
            }
            else // normal case
            {
                list<TEncRCPic*> listPreviousPicture = m_pcRateCtrl->getPicList();
                lambda  = m_pcRateCtrl->getRCPic()->estimatePicLambda(listPreviousPicture);
                sliceQP = m_pcRateCtrl->getRCPic()->estimatePicQP(lambda, listPreviousPicture);
            }

            sliceQP = Clip3(-m_cSPS.getQpBDOffsetY(), MAX_QP, sliceQP);
            m_pcRateCtrl->getRCPic()->setPicEstQP(sliceQP);

            sliceEncoder->resetQP(pic, frameEncoder, sliceQP, lambda);
        }

        UInt uiInternalAddress = pic->getNumPartInCU() - 4;
        UInt uiExternalAddress = pic->getPicSym()->getNumberOfCUsInFrame() - 1;
        UInt uiPosX = (uiExternalAddress % pic->getFrameWidthInCU()) * g_maxCUWidth + g_rasterToPelX[g_zscanToRaster[uiInternalAddress]];
        UInt uiPosY = (uiExternalAddress / pic->getFrameWidthInCU()) * g_maxCUHeight + g_rasterToPelY[g_zscanToRaster[uiInternalAddress]];
        UInt width = m_cSPS.getPicWidthInLumaSamples();
        UInt height = m_cSPS.getPicHeightInLumaSamples();
        while (uiPosX >= width || uiPosY >= height)
        {
            uiInternalAddress--;
            uiPosX = (uiExternalAddress % pic->getFrameWidthInCU()) * g_maxCUWidth + g_rasterToPelX[g_zscanToRaster[uiInternalAddress]];
            uiPosY = (uiExternalAddress / pic->getFrameWidthInCU()) * g_maxCUHeight + g_rasterToPelY[g_zscanToRaster[uiInternalAddress]];
        }

        uiInternalAddress++;
        if (uiInternalAddress == pic->getNumPartInCU())
        {
            uiInternalAddress = 0;
            uiExternalAddress++;
        }
        UInt uiRealEndAddress = uiExternalAddress * pic->getNumPartInCU() + uiInternalAddress;

        // Allocate some coders, now we know how many tiles there are.
        const Bool bWaveFrontsynchro = m_pcCfg->getWaveFrontsynchro();
        const UInt uiHeightInLCUs = pic->getPicSym()->getFrameHeightInCU();
        const Int  iNumSubstreams = (bWaveFrontsynchro ? uiHeightInLCUs : 1);

        // Allocate some coders, now we know how many tiles there are.
        outStreams = new TComOutputBitstream[iNumSubstreams];

        slice->setNextSlice(false);
        sliceEncoder->compressSlice(pic, frameEncoder);  // The bulk of the real work

        // SAO parameter estimation using non-deblocked pixels for LCU bottom and right boundary areas
        if (m_pcCfg->getSaoLcuBasedOptimization() && m_pcCfg->getSaoLcuBoundary())
        {
            sao->resetStats();
            sao->calcSaoStatsCu_BeforeDblk(pic);
        }

        //-- Loop filter
        Bool bLFCrossTileBoundary = m_cPPS.getLoopFilterAcrossTilesEnabledFlag();
        loopFilter->setCfg(bLFCrossTileBoundary);
        loopFilter->loopFilterPic(pic);

        if (m_cSPS.getUseSAO())
        {
            pic->createNonDBFilterInfo(slice->getSliceCurEndCUAddr(), 0, bLFCrossTileBoundary);
            sao->createPicSaoInfo(pic);
        }
        entropyCoder->setEntropyCoder(cavlcCoder, slice);

        /* write various header sets. */

        if (pocLast == 0)
        {
            /* headers for start of bitstream */
            OutputNALUnit nalu(NAL_UNIT_VPS);
            entropyCoder->setBitstream(&nalu.m_Bitstream);
            entropyCoder->encodeVPS(m_pcEncTop->getVPS());
            writeRBSPTrailingBits(nalu.m_Bitstream);
            accessUnit.push_back(new NALUnitEBSP(nalu));
            actualTotalBits += UInt(accessUnit.back()->m_nalUnitData.str().size()) * 8;

            nalu = NALUnit(NAL_UNIT_SPS);
            entropyCoder->setBitstream(&nalu.m_Bitstream);
            m_cSPS.setNumLongTermRefPicSPS(m_numLongTermRefPicSPS);
            for (Int k = 0; k < m_numLongTermRefPicSPS; k++)
            {
                m_cSPS.setLtRefPicPocLsbSps(k, m_ltRefPicPocLsbSps[k]);
                m_cSPS.setUsedByCurrPicLtSPSFlag(k, m_ltRefPicUsedByCurrPicFlag[k]);
            }

            if (m_pcCfg->getPictureTimingSEIEnabled() || m_pcCfg->getDecodingUnitInfoSEIEnabled())
            {
                // CHECK_ME: maybe HM's bug
                UInt maxCU = 1500 >> (m_cSPS.getMaxCUDepth() << 1);
                UInt numDU = 0;
                if (pic->getNumCUsInFrame() % maxCU != 0 || numDU == 0)
                {
                    numDU++;
                }
                m_cSPS.getVuiParameters()->getHrdParameters()->setNumDU(numDU);
                m_cSPS.setHrdParameters(m_pcCfg->getFrameRate(), numDU, m_pcCfg->getTargetBitrate(), (m_pcCfg->getIntraPeriod() > 0));
            }
            if (m_pcCfg->getBufferingPeriodSEIEnabled() || m_pcCfg->getPictureTimingSEIEnabled() || m_pcCfg->getDecodingUnitInfoSEIEnabled())
            {
                m_cSPS.getVuiParameters()->setHrdParametersPresentFlag(true);
            }
            entropyCoder->encodeSPS(slice->getSPS());
            writeRBSPTrailingBits(nalu.m_Bitstream);
            accessUnit.push_back(new NALUnitEBSP(nalu));
            actualTotalBits += UInt(accessUnit.back()->m_nalUnitData.str().size()) * 8;

            nalu = NALUnit(NAL_UNIT_PPS);
            entropyCoder->setBitstream(&nalu.m_Bitstream);
            entropyCoder->encodePPS(slice->getPPS());
            writeRBSPTrailingBits(nalu.m_Bitstream);
            accessUnit.push_back(new NALUnitEBSP(nalu));
            actualTotalBits += UInt(accessUnit.back()->m_nalUnitData.str().size()) * 8;

            if (m_pcCfg->getActiveParameterSetsSEIEnabled())
            {
                SEIActiveParameterSets *sei = xCreateSEIActiveParameterSets(slice->getSPS());

                entropyCoder->setBitstream(&nalu.m_Bitstream);
                m_seiWriter.writeSEImessage(nalu.m_Bitstream, *sei, slice->getSPS());
                writeRBSPTrailingBits(nalu.m_Bitstream);
                accessUnit.push_back(new NALUnitEBSP(nalu));
                delete sei;
                bActiveParameterSetSEIPresentInAU = true;
            }

            if (m_pcCfg->getDisplayOrientationSEIAngle())
            {
                SEIDisplayOrientation *sei = xCreateSEIDisplayOrientation();

                nalu = NALUnit(NAL_UNIT_PREFIX_SEI);
                entropyCoder->setBitstream(&nalu.m_Bitstream);
                m_seiWriter.writeSEImessage(nalu.m_Bitstream, *sei, slice->getSPS());
                writeRBSPTrailingBits(nalu.m_Bitstream);
                accessUnit.push_back(new NALUnitEBSP(nalu));
                delete sei;
            }
        }

        if (writeSOP) // write SOP description SEI (if enabled) at the beginning of GOP
        {
            writeSOP = false;
            OutputNALUnit nalu(NAL_UNIT_PREFIX_SEI);
            entropyCoder->setEntropyCoder(cavlcCoder, slice);
            entropyCoder->setBitstream(&nalu.m_Bitstream);

            SEISOPDescription SOPDescriptionSEI;
            SOPDescriptionSEI.m_sopSeqParameterSetId = m_cSPS.getSPSId();

            UInt i = 0;
            UInt prevEntryId = gopIdx;
            Int SOPcurrPOC = pocCurr;
            for (int j = gopIdx; j < gopSize; j++)
            {
                Int deltaPOC = m_pcCfg->getGOPEntry(j).m_POC - m_pcCfg->getGOPEntry(prevEntryId).m_POC;
                if ((SOPcurrPOC + deltaPOC) < m_pcCfg->getFramesToBeEncoded())
                {
                    SOPcurrPOC += deltaPOC;
                    SOPDescriptionSEI.m_sopDescVclNaluType[i] = getNalUnitType(SOPcurrPOC, m_iLastIDR);
                    SOPDescriptionSEI.m_sopDescTemporalId[i] = m_pcCfg->getGOPEntry(j).m_temporalId;
                    SOPDescriptionSEI.m_sopDescStRpsIdx[i] = getReferencePictureSetIdxForSOP(slice, SOPcurrPOC, j);
                    SOPDescriptionSEI.m_sopDescPocDelta[i] = deltaPOC;

                    prevEntryId = j;
                    i++;
                }
            }

            SOPDescriptionSEI.m_numPicsInSopMinus1 = i - 1;

            m_seiWriter.writeSEImessage(nalu.m_Bitstream, SOPDescriptionSEI, slice->getSPS());
            writeRBSPTrailingBits(nalu.m_Bitstream);
            accessUnit.push_back(new NALUnitEBSP(nalu));
        }

        if ((m_pcCfg->getPictureTimingSEIEnabled() || m_pcCfg->getDecodingUnitInfoSEIEnabled()) &&
            (m_cSPS.getVuiParametersPresentFlag()) &&
            ((m_cSPS.getVuiParameters()->getHrdParameters()->getNalHrdParametersPresentFlag())
             || (m_cSPS.getVuiParameters()->getHrdParameters()->getVclHrdParametersPresentFlag())))
        {
            if (m_cSPS.getVuiParameters()->getHrdParameters()->getSubPicCpbParamsPresentFlag())
            {
                UInt numDU = m_cSPS.getVuiParameters()->getHrdParameters()->getNumDU();
                pictureTimingSEI.m_numDecodingUnitsMinus1 = (numDU - 1);
                pictureTimingSEI.m_duCommonCpbRemovalDelayFlag = false;

                if (pictureTimingSEI.m_numNalusInDuMinus1 == NULL)
                {
                    pictureTimingSEI.m_numNalusInDuMinus1 = new UInt[numDU];
                }
                if (pictureTimingSEI.m_duCpbRemovalDelayMinus1 == NULL)
                {
                    pictureTimingSEI.m_duCpbRemovalDelayMinus1  = new UInt[numDU];
                }
                if (accumBitsDU == NULL)
                {
                    accumBitsDU = new UInt[numDU];
                }
                if (accumNalsDU == NULL)
                {
                    accumNalsDU = new UInt[numDU];
                }
            }
            pictureTimingSEI.m_auCpbRemovalDelay = std::max<Int>(1, m_totalCoded - m_lastBPSEI); // Syntax element signaled as minus, hence the .
            pictureTimingSEI.m_picDpbOutputDelay = m_cSPS.getNumReorderPics(0) + slice->getPOC() - m_totalCoded;
            Int factor = m_cSPS.getVuiParameters()->getHrdParameters()->getTickDivisorMinus2() + 2;
            pictureTimingSEI.m_picDpbOutputDuDelay = factor * pictureTimingSEI.m_picDpbOutputDelay;
            if (m_pcCfg->getDecodingUnitInfoSEIEnabled())
            {
                picSptDpbOutputDuDelay = factor * pictureTimingSEI.m_picDpbOutputDelay;
            }
        }

        if ((m_pcCfg->getBufferingPeriodSEIEnabled()) && (slice->getSliceType() == I_SLICE) &&
            (m_cSPS.getVuiParametersPresentFlag()) &&
            ((m_cSPS.getVuiParameters()->getHrdParameters()->getNalHrdParametersPresentFlag())
             || (m_cSPS.getVuiParameters()->getHrdParameters()->getVclHrdParametersPresentFlag())))
        {
            OutputNALUnit nalu(NAL_UNIT_PREFIX_SEI);
            entropyCoder->setEntropyCoder(cavlcCoder, slice);
            entropyCoder->setBitstream(&nalu.m_Bitstream);

            SEIBufferingPeriod sei_buffering_period;

            UInt uiInitialCpbRemovalDelay = (90000 / 2);              // 0.5 sec
            sei_buffering_period.m_initialCpbRemovalDelay[0][0]     = uiInitialCpbRemovalDelay;
            sei_buffering_period.m_initialCpbRemovalDelayOffset[0][0]     = uiInitialCpbRemovalDelay;
            sei_buffering_period.m_initialCpbRemovalDelay[0][1]     = uiInitialCpbRemovalDelay;
            sei_buffering_period.m_initialCpbRemovalDelayOffset[0][1]     = uiInitialCpbRemovalDelay;

            Double dTmp = (Double)m_cSPS.getVuiParameters()->getTimingInfo()->getNumUnitsInTick() / (Double)m_cSPS.getVuiParameters()->getTimingInfo()->getTimeScale();

            UInt uiTmp = (UInt)(dTmp * 90000.0);
            uiInitialCpbRemovalDelay -= uiTmp;
            uiInitialCpbRemovalDelay -= uiTmp / (m_cSPS.getVuiParameters()->getHrdParameters()->getTickDivisorMinus2() + 2);
            sei_buffering_period.m_initialAltCpbRemovalDelay[0][0]  = uiInitialCpbRemovalDelay;
            sei_buffering_period.m_initialAltCpbRemovalDelayOffset[0][0]  = uiInitialCpbRemovalDelay;
            sei_buffering_period.m_initialAltCpbRemovalDelay[0][1]  = uiInitialCpbRemovalDelay;
            sei_buffering_period.m_initialAltCpbRemovalDelayOffset[0][1]  = uiInitialCpbRemovalDelay;

            sei_buffering_period.m_rapCpbParamsPresentFlag = 0;
            //for the concatenation, it can be set to one during splicing.
            sei_buffering_period.m_concatenationFlag = 0;
            //since the temporal layer HRD is not ready, we assumed it is fixed
            sei_buffering_period.m_auCpbRemovalDelayDelta = 1;
            sei_buffering_period.m_cpbDelayOffset = 0;
            sei_buffering_period.m_dpbDelayOffset = 0;

            m_seiWriter.writeSEImessage(nalu.m_Bitstream, sei_buffering_period, slice->getSPS());
            writeRBSPTrailingBits(nalu.m_Bitstream);
            {
                UInt seiPositionInAu = xGetFirstSeiLocation(accessUnit);
                UInt offsetPosition = bActiveParameterSetSEIPresentInAU; // Insert BP SEI after APS SEI
                AccessUnit::iterator it = accessUnit.begin();
                for (int j = 0; j < seiPositionInAu + offsetPosition; j++)
                {
                    it++;
                }

                accessUnit.insert(it, new NALUnitEBSP(nalu));
                bBufferingPeriodSEIPresentInAU = true;
            }

            if (m_pcCfg->getScalableNestingSEIEnabled())
            {
                OutputNALUnit naluTmp(NAL_UNIT_PREFIX_SEI);
                entropyCoder->setEntropyCoder(cavlcCoder, slice);
                entropyCoder->setBitstream(&naluTmp.m_Bitstream);
                scalableNestingSEI.m_nestedSEIs.clear();
                scalableNestingSEI.m_nestedSEIs.push_back(&sei_buffering_period);
                m_seiWriter.writeSEImessage(naluTmp.m_Bitstream, scalableNestingSEI, slice->getSPS());
                writeRBSPTrailingBits(naluTmp.m_Bitstream);
                UInt seiPositionInAu = xGetFirstSeiLocation(accessUnit);
                UInt offsetPosition = bActiveParameterSetSEIPresentInAU + bBufferingPeriodSEIPresentInAU + bPictureTimingSEIPresentInAU; // Insert BP SEI after non-nested APS, BP and PT SEIs
                AccessUnit::iterator it = accessUnit.begin();
                for (int j = 0; j < seiPositionInAu + offsetPosition; j++)
                {
                    it++;
                }

                accessUnit.insert(it, new NALUnitEBSP(naluTmp));
                bNestedBufferingPeriodSEIPresentInAU = true;
            }

            m_lastBPSEI = m_totalCoded;
        }
        if ((m_pcEncTop->getRecoveryPointSEIEnabled()) && (slice->getSliceType() == I_SLICE))
        {
            if (m_pcEncTop->getGradualDecodingRefreshInfoEnabled() && !slice->getRapPicFlag())
            {
                // Gradual decoding refresh SEI
                OutputNALUnit nalu(NAL_UNIT_PREFIX_SEI);
                entropyCoder->setEntropyCoder(cavlcCoder, slice);
                entropyCoder->setBitstream(&nalu.m_Bitstream);

                SEIGradualDecodingRefreshInfo seiGradualDecodingRefreshInfo;
                seiGradualDecodingRefreshInfo.m_gdrForegroundFlag = true; // Indicating all "foreground"

                m_seiWriter.writeSEImessage(nalu.m_Bitstream, seiGradualDecodingRefreshInfo, slice->getSPS());
                writeRBSPTrailingBits(nalu.m_Bitstream);
                accessUnit.push_back(new NALUnitEBSP(nalu));
            }
            // Recovery point SEI
            OutputNALUnit nalu(NAL_UNIT_PREFIX_SEI);
            entropyCoder->setEntropyCoder(cavlcCoder, slice);
            entropyCoder->setBitstream(&nalu.m_Bitstream);

            SEIRecoveryPoint sei_recovery_point;
            sei_recovery_point.m_recoveryPocCnt    = 0;
            sei_recovery_point.m_exactMatchingFlag = (slice->getPOC() == 0) ? (true) : (false);
            sei_recovery_point.m_brokenLinkFlag    = false;

            m_seiWriter.writeSEImessage(nalu.m_Bitstream, sei_recovery_point, slice->getSPS());
            writeRBSPTrailingBits(nalu.m_Bitstream);
            accessUnit.push_back(new NALUnitEBSP(nalu));
        }

        /* use the main bitstream buffer for storing the marshaled picture */
        entropyCoder->setBitstream(NULL);

        if (m_cSPS.getUseSAO())
        {
            // set entropy coder for RD
            entropyCoder->setEntropyCoder(sbacCoder, slice);
            if (m_cSPS.getUseSAO())
            {
                entropyCoder->resetEntropy();
                entropyCoder->setBitstream(bitCounter);

                // CHECK_ME: I think the SAO is use a temp Sbac only, so I always use [0], am I right?
                sao->startSaoEnc(pic, entropyCoder, frameEncoder->getRDSbacCoders(0), frameEncoder->getRDGoOnSbacCoder(0));

                SAOParam& cSaoParam = *slice->getPic()->getPicSym()->getSaoParam();

                sao->SAOProcess(&cSaoParam, pic->getSlice()->getLambdaLuma(), pic->getSlice()->getLambdaChroma(), pic->getSlice()->getDepth());
                sao->endSaoEnc();
                sao->PCMLFDisableProcess(pic);
            }

            if (m_cSPS.getUseSAO())
            {
                pic->getSlice()->setSaoEnabledFlag((slice->getPic()->getPicSym()->getSaoParam()->bSaoFlag[0] == 1) ? true : false);
            }
        }

        slice->setNextSlice(false);
        sliceEncoder->setSliceIdx(0);

        // Reconstruction slice
        slice->setNextSlice(true);
        slice->setRPS(pic->getSlice()->getRPS());
        slice->setRPSidx(pic->getSlice()->getRPSidx());
        sliceEncoder->xDetermineStartAndBoundingCUAddr(pic, true);

        uiInternalAddress = (slice->getSliceCurEndCUAddr() - 1) % pic->getNumPartInCU();
        uiExternalAddress = (slice->getSliceCurEndCUAddr() - 1) / pic->getNumPartInCU();
        uiPosX = (uiExternalAddress % pic->getFrameWidthInCU()) * g_maxCUWidth + g_rasterToPelX[g_zscanToRaster[uiInternalAddress]];
        uiPosY = (uiExternalAddress / pic->getFrameWidthInCU()) * g_maxCUHeight + g_rasterToPelY[g_zscanToRaster[uiInternalAddress]];
        width = m_cSPS.getPicWidthInLumaSamples();
        height = m_cSPS.getPicHeightInLumaSamples();
        while (uiPosX >= width || uiPosY >= height)
        {
            uiInternalAddress--;
            uiPosX = (uiExternalAddress % pic->getFrameWidthInCU()) * g_maxCUWidth + g_rasterToPelX[g_zscanToRaster[uiInternalAddress]];
            uiPosY = (uiExternalAddress / pic->getFrameWidthInCU()) * g_maxCUHeight + g_rasterToPelY[g_zscanToRaster[uiInternalAddress]];
        }

        uiInternalAddress++;
        if (uiInternalAddress == pic->getNumPartInCU())
        {
            uiInternalAddress = 0;
            uiExternalAddress = (uiExternalAddress + 1);
        }
        UInt endAddress = (uiExternalAddress * pic->getNumPartInCU() + uiInternalAddress);
        slice->allocSubstreamSizes(iNumSubstreams);
        for (UInt ui = 0; ui < iNumSubstreams; ui++)
        {
            outStreams[ui].clear();
        }

        entropyCoder->setEntropyCoder(cavlcCoder, slice);
        entropyCoder->resetEntropy();
        /* start slice NALunit */
        OutputNALUnit nalu(slice->getNalUnitType(), slice->getTLayer());
        Bool sliceSegment = (!slice->isNextSlice());
        if (!sliceSegment)
        {
            uiOneBitstreamPerSliceLength = 0; // start of a new slice
        }
        entropyCoder->setBitstream(&nalu.m_Bitstream);
        tmpBitsBeforeWriting = entropyCoder->getNumberOfWrittenBits();
        entropyCoder->encodeSliceHeader(slice);
        actualHeadBits += (entropyCoder->getNumberOfWrittenBits() - tmpBitsBeforeWriting);

        // is it needed?
        if (!sliceSegment)
        {
            pcBitstreamRedirect->writeAlignOne();
        }
        else
        {
            // We've not completed our slice header info yet, do the alignment later.
        }
        sbacCoder->init((TEncBinIf*)binCABAC);
        entropyCoder->setEntropyCoder(sbacCoder, slice);
        entropyCoder->resetEntropy();
        frameEncoder->resetEntropy(slice);

        if (slice->isNextSlice())
        {
            // set entropy coder for writing
            sbacCoder->init((TEncBinIf*)binCABAC);
            {
                frameEncoder->resetEntropy(slice);
                frameEncoder->getSbacCoder(0)->load(sbacCoder);
                entropyCoder->setEntropyCoder(frameEncoder->getSbacCoder(0), slice); //ALF is written in substream #0 with CABAC coder #0 (see ALF param encoding below)
            }
            entropyCoder->resetEntropy();
            // File writing
            if (!sliceSegment)
            {
                entropyCoder->setBitstream(pcBitstreamRedirect);
            }
            else
            {
                entropyCoder->setBitstream(&nalu.m_Bitstream);
            }
            // for now, override the TILES_DECODER setting in order to write substreams.
            entropyCoder->setBitstream(&outStreams[0]);
        }
        slice->setFinalized(true);

        sbacCoder->load(frameEncoder->getSbacCoder(0));

        slice->setTileOffstForMultES(uiOneBitstreamPerSliceLength);
        slice->setTileLocationCount(0);
        sliceEncoder->encodeSlice(pic, outStreams, frameEncoder);

        {
            // Construct the final bitstream by flushing and concatenating substreams.
            // The final bitstream is either nalu.m_Bitstream or pcBitstreamRedirect;
            UInt* puiSubstreamSizes = slice->getSubstreamSizes();
            for (UInt ui = 0; ui < iNumSubstreams; ui++)
            {
                // Flush all substreams -- this includes empty ones.
                // Terminating bit and flush.
                entropyCoder->setEntropyCoder(frameEncoder->getSbacCoder(ui), slice);
                entropyCoder->setBitstream(&outStreams[ui]);
                entropyCoder->encodeTerminatingBit(1);
                entropyCoder->encodeSliceFinish();

                outStreams[ui].writeByteAlignment(); // Byte-alignment in slice_data() at end of sub-stream

                // Byte alignment is necessary between tiles when tiles are independent.
                if (ui + 1 < iNumSubstreams)
                {
                    puiSubstreamSizes[ui] = outStreams[ui].getNumberOfWrittenBits() + (outStreams[ui].countStartCodeEmulations() << 3);
                }
            }

            // Complete the slice header info.
            entropyCoder->setEntropyCoder(cavlcCoder, slice);
            entropyCoder->setBitstream(&nalu.m_Bitstream);
            entropyCoder->encodeTilesWPPEntryPoint(slice);

            // Substreams...
            Int nss = m_cPPS.getEntropyCodingSyncEnabledFlag() ? slice->getNumEntryPointOffsets() + 1 : iNumSubstreams;
            for (Int i = 0; i < nss; i++)
            {
                pcBitstreamRedirect->addSubstream(&outStreams[i]);
            }
        }

        // If current NALU is the first NALU of slice (containing slice header) and more NALUs exist (due to multiple dependent slices) then buffer it.
        // If current NALU is the last NALU of slice and a NALU was buffered, then (a) Write current NALU (b) Update an write buffered NALU at approproate location in NALU list.
        xAttachSliceDataToNalUnit(entropyCoder, nalu, pcBitstreamRedirect);
        accessUnit.push_back(new NALUnitEBSP(nalu));
        actualTotalBits += UInt(accessUnit.back()->m_nalUnitData.str().size()) * 8;
        uiOneBitstreamPerSliceLength += nalu.m_Bitstream.getNumberOfWrittenBits(); // length of bitstream after byte-alignment

        if ((m_pcCfg->getPictureTimingSEIEnabled() || m_pcCfg->getDecodingUnitInfoSEIEnabled()) &&
            (m_cSPS.getVuiParametersPresentFlag()) &&
            ((m_cSPS.getVuiParameters()->getHrdParameters()->getNalHrdParametersPresentFlag())
             || (m_cSPS.getVuiParameters()->getHrdParameters()->getVclHrdParametersPresentFlag())) &&
            (m_cSPS.getVuiParameters()->getHrdParameters()->getSubPicCpbParamsPresentFlag()))
        {
            UInt numNalus = 0;
            UInt numRBSPBytes = 0;
            for (AccessUnit::const_iterator it = accessUnit.begin(); it != accessUnit.end(); it++)
            {
                UInt numRBSPBytes_nal = UInt((*it)->m_nalUnitData.str().size());
                if ((*it)->m_nalUnitType != NAL_UNIT_PREFIX_SEI && (*it)->m_nalUnitType != NAL_UNIT_SUFFIX_SEI)
                {
                    numRBSPBytes += numRBSPBytes_nal;
                    numNalus++;
                }
            }

            accumBitsDU[0] = (numRBSPBytes << 3);
            accumNalsDU[0] = numNalus; // SEI not counted for bit count; hence shouldn't be counted for # of NALUs - only for consistency
        }

        if (m_cSPS.getUseSAO())
        {
            if (m_cSPS.getUseSAO())
            {
                sao->destroyPicSaoInfo();
            }
            pic->destroyNonDBFilterInfo();
        }

        pic->compressMotion();

        const Char* digestStr = NULL;
        if (m_pcCfg->getDecodedPictureHashSEIEnabled())
        {
            /* calculate MD5sum for entire reconstructed picture */
            SEIDecodedPictureHash sei_recon_picture_digest;
            if (m_pcCfg->getDecodedPictureHashSEIEnabled() == 1)
            {
                sei_recon_picture_digest.method = SEIDecodedPictureHash::MD5;
                calcMD5(*pic->getPicYuvRec(), sei_recon_picture_digest.digest);
                digestStr = digestToString(sei_recon_picture_digest.digest, 16);
            }
            else if (m_pcCfg->getDecodedPictureHashSEIEnabled() == 2)
            {
                sei_recon_picture_digest.method = SEIDecodedPictureHash::CRC;
                calcCRC(*pic->getPicYuvRec(), sei_recon_picture_digest.digest);
                digestStr = digestToString(sei_recon_picture_digest.digest, 2);
            }
            else if (m_pcCfg->getDecodedPictureHashSEIEnabled() == 3)
            {
                sei_recon_picture_digest.method = SEIDecodedPictureHash::CHECKSUM;
                calcChecksum(*pic->getPicYuvRec(), sei_recon_picture_digest.digest);
                digestStr = digestToString(sei_recon_picture_digest.digest, 4);
            }
            OutputNALUnit onalu(NAL_UNIT_SUFFIX_SEI, slice->getTLayer());

            /* write the SEI messages */
            entropyCoder->setEntropyCoder(cavlcCoder, slice);
            m_seiWriter.writeSEImessage(onalu.m_Bitstream, sei_recon_picture_digest, slice->getSPS());
            writeRBSPTrailingBits(onalu.m_Bitstream);

            accessUnit.insert(accessUnit.end(), new NALUnitEBSP(onalu));
        }
        if (m_pcCfg->getTemporalLevel0IndexSEIEnabled())
        {
            SEITemporalLevel0Index sei_temporal_level0_index;
            if (slice->getRapPicFlag())
            {
                m_tl0Idx = 0;
                m_rapIdx = (m_rapIdx + 1) & 0xFF;
            }
            else
            {
                m_tl0Idx = (m_tl0Idx + (slice->getTLayer() ? 0 : 1)) & 0xFF;
            }
            sei_temporal_level0_index.tl0Idx = m_tl0Idx;
            sei_temporal_level0_index.rapIdx = m_rapIdx;

            OutputNALUnit onalu(NAL_UNIT_PREFIX_SEI);

            /* write the SEI messages */
            entropyCoder->setEntropyCoder(cavlcCoder, slice);
            m_seiWriter.writeSEImessage(onalu.m_Bitstream, sei_temporal_level0_index, slice->getSPS());
            writeRBSPTrailingBits(onalu.m_Bitstream);

            /* insert the SEI message NALUnit before any Slice NALUnits */
            AccessUnit::iterator it = find_if(accessUnit.begin(), accessUnit.end(), mem_fun(&NALUnit::isSlice));
            accessUnit.insert(it, new NALUnitEBSP(onalu));
        }

        xCalculateAddPSNR(pic, pic->getPicYuvRec(), accessUnit);

        if (digestStr && m_pcCfg->getLogLevel() >= X265_LOG_DEBUG)
        {
            if (m_pcCfg->getDecodedPictureHashSEIEnabled() == 1)
            {
                fprintf(stderr, " [MD5:%s]", digestStr);
            }
            else if (m_pcCfg->getDecodedPictureHashSEIEnabled() == 2)
            {
                fprintf(stderr, " [CRC:%s]", digestStr);
            }
            else if (m_pcCfg->getDecodedPictureHashSEIEnabled() == 3)
            {
                fprintf(stderr, " [Checksum:%s]", digestStr);
            }
        }
        if (m_pcCfg->getUseRateCtrl())
        {
            Double effectivePercentage = m_pcRateCtrl->getRCPic()->getEffectivePercentage();
            Double avgQP     = m_pcRateCtrl->getRCPic()->calAverageQP();
            Double avgLambda = m_pcRateCtrl->getRCPic()->calAverageLambda();
            if (avgLambda < 0.0)
            {
                avgLambda = lambda;
            }
            m_pcRateCtrl->getRCPic()->updateAfterPicture(actualHeadBits, actualTotalBits, avgQP, avgLambda, effectivePercentage);
            m_pcRateCtrl->getRCPic()->addToPictureLsit(m_pcRateCtrl->getPicList());

            m_pcRateCtrl->getRCSeq()->updateAfterPic(actualTotalBits);
            if (slice->getSliceType() != I_SLICE)
            {
                m_pcRateCtrl->getRCGOP()->updateAfterPicture(actualTotalBits);
            }
            else // for intra picture, the estimated bits are used to update the current status in the GOP
            {
                m_pcRateCtrl->getRCGOP()->updateAfterPicture(estimatedBits);
            }
        }

        if ((m_pcCfg->getPictureTimingSEIEnabled() || m_pcCfg->getDecodingUnitInfoSEIEnabled()) &&
            (m_cSPS.getVuiParametersPresentFlag()) &&
            ((m_cSPS.getVuiParameters()->getHrdParameters()->getNalHrdParametersPresentFlag())
             || (m_cSPS.getVuiParameters()->getHrdParameters()->getVclHrdParametersPresentFlag())))
        {
            TComVUI *vui = m_cSPS.getVuiParameters();
            TComHRD *hrd = vui->getHrdParameters();

            if (hrd->getSubPicCpbParamsPresentFlag())
            {
                Int i;
                UInt64 ui64Tmp;
                UInt uiPrev = 0;
                UInt numDU = (pictureTimingSEI.m_numDecodingUnitsMinus1 + 1);
                UInt *pCRD = &pictureTimingSEI.m_duCpbRemovalDelayMinus1[0];
                UInt maxDiff = (hrd->getTickDivisorMinus2() + 2) - 1;

                for (i = 0; i < numDU; i++)
                {
                    pictureTimingSEI.m_numNalusInDuMinus1[i]       = (i == 0) ? (accumNalsDU[i] - 1) : (accumNalsDU[i] - accumNalsDU[i - 1] - 1);
                }

                if (numDU == 1)
                {
                    pCRD[0] = 0; /* don't care */
                }
                else
                {
                    pCRD[numDU - 1] = 0; /* by definition */
                    UInt tmp = 0;
                    UInt accum = 0;

                    for (i = (numDU - 2); i >= 0; i--)
                    {
                        ui64Tmp = (((accumBitsDU[numDU - 1]  - accumBitsDU[i]) * (vui->getTimingInfo()->getTimeScale() / vui->getTimingInfo()->getNumUnitsInTick()) * (hrd->getTickDivisorMinus2() + 2)) / (m_pcCfg->getTargetBitrate()));
                        if ((UInt)ui64Tmp > maxDiff)
                        {
                            tmp++;
                        }
                    }

                    uiPrev = 0;

                    UInt flag = 0;
                    for (i = (numDU - 2); i >= 0; i--)
                    {
                        flag = 0;
                        ui64Tmp = (((accumBitsDU[numDU - 1]  - accumBitsDU[i]) * (vui->getTimingInfo()->getTimeScale() / vui->getTimingInfo()->getNumUnitsInTick()) * (hrd->getTickDivisorMinus2() + 2)) / (m_pcCfg->getTargetBitrate()));

                        if ((UInt)ui64Tmp > maxDiff)
                        {
                            if (uiPrev >= maxDiff - tmp)
                            {
                                ui64Tmp = uiPrev + 1;
                                flag = 1;
                            }
                            else ui64Tmp = maxDiff - tmp + 1;
                        }
                        pCRD[i] = (UInt)ui64Tmp - uiPrev - 1;
                        if ((Int)pCRD[i] < 0)
                        {
                            pCRD[i] = 0;
                        }
                        else if (tmp > 0 && flag == 1)
                        {
                            tmp--;
                        }
                        accum += pCRD[i] + 1;
                        uiPrev = accum;
                    }
                }
            }
            if (m_pcCfg->getPictureTimingSEIEnabled())
            {
                {
                    OutputNALUnit onalu(NAL_UNIT_PREFIX_SEI, slice->getTLayer());
                    entropyCoder->setEntropyCoder(cavlcCoder, slice);
                    m_seiWriter.writeSEImessage(onalu.m_Bitstream, pictureTimingSEI, slice->getSPS());
                    writeRBSPTrailingBits(onalu.m_Bitstream);
                    UInt seiPositionInAu = xGetFirstSeiLocation(accessUnit);
                    // Insert PT SEI after APS and BP SEI
                    UInt offsetPosition = bActiveParameterSetSEIPresentInAU + bBufferingPeriodSEIPresentInAU;
                    AccessUnit::iterator it = accessUnit.begin();
                    for (int j = 0; j < seiPositionInAu + offsetPosition; j++)
                    {
                        it++;
                    }

                    accessUnit.insert(it, new NALUnitEBSP(onalu));
                    bPictureTimingSEIPresentInAU = true;
                }
                if (m_pcCfg->getScalableNestingSEIEnabled()) // put picture timing SEI into scalable nesting SEI
                {
                    OutputNALUnit onalu(NAL_UNIT_PREFIX_SEI, slice->getTLayer());
                    entropyCoder->setEntropyCoder(cavlcCoder, slice);
                    scalableNestingSEI.m_nestedSEIs.clear();
                    scalableNestingSEI.m_nestedSEIs.push_back(&pictureTimingSEI);
                    m_seiWriter.writeSEImessage(onalu.m_Bitstream, scalableNestingSEI, slice->getSPS());
                    writeRBSPTrailingBits(onalu.m_Bitstream);
                    UInt seiPositionInAu = xGetFirstSeiLocation(accessUnit);
                    // Insert PT SEI after APS and BP SEI
                    UInt offsetPosition = bActiveParameterSetSEIPresentInAU +
                        bBufferingPeriodSEIPresentInAU +
                        bPictureTimingSEIPresentInAU +
                        bNestedBufferingPeriodSEIPresentInAU;
                    AccessUnit::iterator it = accessUnit.begin();
                    for (int j = 0; j < seiPositionInAu + offsetPosition; j++)
                    {
                        it++;
                    }

                    accessUnit.insert(it, new NALUnitEBSP(onalu));
                }
            }
            if (m_pcCfg->getDecodingUnitInfoSEIEnabled() && hrd->getSubPicCpbParamsPresentFlag())
            {
                entropyCoder->setEntropyCoder(cavlcCoder, slice);
                for (Int i = 0; i < (pictureTimingSEI.m_numDecodingUnitsMinus1 + 1); i++)
                {
                    OutputNALUnit onalu(NAL_UNIT_PREFIX_SEI, slice->getTLayer());

                    SEIDecodingUnitInfo tempSEI;
                    tempSEI.m_decodingUnitIdx = i;
                    tempSEI.m_duSptCpbRemovalDelay = pictureTimingSEI.m_duCpbRemovalDelayMinus1[i] + 1;
                    tempSEI.m_dpbOutputDuDelayPresentFlag = false;
                    tempSEI.m_picSptDpbOutputDuDelay = picSptDpbOutputDuDelay;

                    AccessUnit::iterator it = accessUnit.begin();
                    // Insert the first one in the right location, before the first slice
                    if (i == 0)
                    {
                        // Insert before the first slice.
                        m_seiWriter.writeSEImessage(onalu.m_Bitstream, tempSEI, slice->getSPS());
                        writeRBSPTrailingBits(onalu.m_Bitstream);
                        UInt seiPositionInAu = xGetFirstSeiLocation(accessUnit);
                        // Insert DU info SEI after APS, BP and PT SEI
                        UInt offsetPosition = bActiveParameterSetSEIPresentInAU +
                            bBufferingPeriodSEIPresentInAU +
                            bPictureTimingSEIPresentInAU;
                        for (int j = 0; j < seiPositionInAu + offsetPosition; j++)
                        {
                            it++;
                        }

                        accessUnit.insert(it, new NALUnitEBSP(onalu));
                    }
                    else
                    {
                        it = accessUnit.begin();
                        // For the second decoding unit onwards we know how many NALUs are present
                        for (Int ctr = 0; it != accessUnit.end(); it++)
                        {
                            if (ctr == accumNalsDU[i - 1])
                            {
                                // Insert before the first slice.
                                m_seiWriter.writeSEImessage(onalu.m_Bitstream, tempSEI, slice->getSPS());
                                writeRBSPTrailingBits(onalu.m_Bitstream);

                                accessUnit.insert(it, new NALUnitEBSP(onalu));
                                break;
                            }
                            if ((*it)->m_nalUnitType != NAL_UNIT_PREFIX_SEI && (*it)->m_nalUnitType != NAL_UNIT_SUFFIX_SEI)
                            {
                                ctr++;
                            }
                        }
                    }
                }
            }
        }

        bActiveParameterSetSEIPresentInAU = false;
        bBufferingPeriodSEIPresentInAU    = false;
        bPictureTimingSEIPresentInAU      = false;
        bNestedBufferingPeriodSEIPresentInAU = false;
        numPicCoded++;
        m_totalCoded++;

        if (m_pcCfg->getLogLevel() >= X265_LOG_DEBUG)
        {
            /* logging: insert a newline at end of picture period */
            fprintf(stderr, "\n");
            fflush(stderr);
        }
        delete[] outStreams;

        if (numPicCoded == numPicRecvd)
            break;
    }

    delete pcBitstreamRedirect;

    if (accumBitsDU != NULL) delete [] accumBitsDU;
    if (accumNalsDU != NULL) delete [] accumNalsDU;

    if (m_pcCfg->getUseRateCtrl())
    {
        m_pcRateCtrl->destroyRCGOP();
    }

    assert(numPicCoded == numPicRecvd);
}

SEIActiveParameterSets* TEncGOP::xCreateSEIActiveParameterSets(TComSPS *sps)
{
    SEIActiveParameterSets *seiActiveParameterSets = new SEIActiveParameterSets();

    seiActiveParameterSets->activeVPSId = m_pcCfg->getVPS()->getVPSId();
    seiActiveParameterSets->m_fullRandomAccessFlag = false;
    seiActiveParameterSets->m_noParamSetUpdateFlag = false;
    seiActiveParameterSets->numSpsIdsMinus1 = 0;
    seiActiveParameterSets->activeSeqParamSetId.resize(seiActiveParameterSets->numSpsIdsMinus1 + 1);
    seiActiveParameterSets->activeSeqParamSetId[0] = sps->getSPSId();
    return seiActiveParameterSets;
}

SEIDisplayOrientation* TEncGOP::xCreateSEIDisplayOrientation()
{
    SEIDisplayOrientation *seiDisplayOrientation = new SEIDisplayOrientation();

    seiDisplayOrientation->cancelFlag = false;
    seiDisplayOrientation->horFlip = false;
    seiDisplayOrientation->verFlip = false;
    seiDisplayOrientation->anticlockwiseRotation = m_pcCfg->getDisplayOrientationSEIAngle();
    return seiDisplayOrientation;
}

// This is a function that
// determines what Reference Picture Set to use
// for a specific slice (with POC = POCCurr)
Void TEncGOP::selectReferencePictureSet(TComSlice* slice, Int POCCurr, Int GOPid)
{
    slice->setRPSidx(GOPid);
    UInt intraPeriod = m_pcCfg->getIntraPeriod();
    Int gopSize = m_pcCfg->getGOPSize();

    for (Int extraNum = gopSize; extraNum < m_pcCfg->getExtraRPSs() + gopSize; extraNum++)
    {
        if (intraPeriod > 0 && m_pcCfg->getDecodingRefreshType() > 0)
        {
            Int POCIndex = POCCurr % intraPeriod;
            if (POCIndex == 0)
            {
                POCIndex = intraPeriod;
            }
            if (POCIndex == m_pcCfg->getGOPEntry(extraNum).m_POC)
            {
                slice->setRPSidx(extraNum);
            }
        }
        else
        {
            if (POCCurr == m_pcCfg->getGOPEntry(extraNum).m_POC)
            {
                slice->setRPSidx(extraNum);
            }
        }
    }

    slice->setRPS(m_cSPS.getRPSList()->getReferencePictureSet(slice->getRPSidx()));
    slice->getRPS()->setNumberOfPictures(slice->getRPS()->getNumberOfNegativePictures() + slice->getRPS()->getNumberOfPositivePictures());
}

Int TEncGOP::getReferencePictureSetIdxForSOP(TComSlice* slice, Int POCCurr, Int GOPid)
{
    int rpsIdx = GOPid;
    UInt intraPeriod = m_pcCfg->getIntraPeriod();
    Int gopSize = m_pcCfg->getGOPSize();

    for (Int extraNum = gopSize; extraNum < m_pcCfg->getExtraRPSs() + gopSize; extraNum++)
    {
        if (intraPeriod > 0 && m_pcCfg->getDecodingRefreshType() > 0)
        {
            Int POCIndex = POCCurr % intraPeriod;
            if (POCIndex == 0)
            {
                POCIndex = intraPeriod;
            }
            if (POCIndex == m_pcCfg->getGOPEntry(extraNum).m_POC)
            {
                rpsIdx = extraNum;
            }
        }
        else
        {
            if (POCCurr == m_pcCfg->getGOPEntry(extraNum).m_POC)
            {
                rpsIdx = extraNum;
            }
        }
    }

    return rpsIdx;
}

// ====================================================================================================================
// Protected member functions
// ====================================================================================================================

#define VERBOSE_RATE 0
#if VERBOSE_RATE
static const Char* nalUnitTypeToString(NalUnitType type)
{
    switch (type)
    {
    case NAL_UNIT_CODED_SLICE_TRAIL_R:    return "TRAIL_R";
    case NAL_UNIT_CODED_SLICE_TRAIL_N:    return "TRAIL_N";
    case NAL_UNIT_CODED_SLICE_TLA_R:      return "TLA_R";
    case NAL_UNIT_CODED_SLICE_TSA_N:      return "TSA_N";
    case NAL_UNIT_CODED_SLICE_STSA_R:     return "STSA_R";
    case NAL_UNIT_CODED_SLICE_STSA_N:     return "STSA_N";
    case NAL_UNIT_CODED_SLICE_BLA_W_LP:   return "BLA_W_LP";
    case NAL_UNIT_CODED_SLICE_BLA_W_RADL: return "BLA_W_RADL";
    case NAL_UNIT_CODED_SLICE_BLA_N_LP:   return "BLA_N_LP";
    case NAL_UNIT_CODED_SLICE_IDR_W_RADL: return "IDR_W_RADL";
    case NAL_UNIT_CODED_SLICE_IDR_N_LP:   return "IDR_N_LP";
    case NAL_UNIT_CODED_SLICE_CRA:        return "CRA";
    case NAL_UNIT_CODED_SLICE_RADL_R:     return "RADL_R";
    case NAL_UNIT_CODED_SLICE_RASL_R:     return "RASL_R";
    case NAL_UNIT_VPS:                    return "VPS";
    case NAL_UNIT_SPS:                    return "SPS";
    case NAL_UNIT_PPS:                    return "PPS";
    case NAL_UNIT_ACCESS_UNIT_DELIMITER:  return "AUD";
    case NAL_UNIT_EOS:                    return "EOS";
    case NAL_UNIT_EOB:                    return "EOB";
    case NAL_UNIT_FILLER_DATA:            return "FILLER";
    case NAL_UNIT_PREFIX_SEI:             return "SEI";
    case NAL_UNIT_SUFFIX_SEI:             return "SEI";
    default:                              return "UNK";
    }
}

#endif // if VERBOSE_RATE

// TODO:
//   1 - as a performance optimization, if we're not reporting PSNR we do not have to measure PSNR
//       (we do not yet have a switch to disable PSNR reporting)
//   2 - it would be better to accumulate SSD of each CTU at the end of processCTU() while it is cache-hot
//       in fact, we almost certainly are already measuring the CTU distortion and not accumulating it
static UInt64 computeSSD(Pel *fenc, Pel *pRec, Int iStride, Int width, Int height)
{
    UInt64 uiSSD = 0;

    if ((width | height) & 3)
    {
        /* Slow Path */
        for (Int y = 0; y < height; y++)
        {
            for (Int x = 0; x < width; x++)
            {
                Int iDiff = (Int)(fenc[x] - pRec[x]);
                uiSSD += iDiff * iDiff;
            }

            fenc += iStride;
            pRec += iStride;
        }

        return uiSSD;
    }
    Int y = 0;
    /* Consume Y in chunks of 64 */
    for (; y + 64 <= height; y += 64)
    {
        Int x = 0;
        for (; x + 64 <= width; x += 64)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_64x64]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        for (; x + 16 <= width; x += 16)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_16x64]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        for (; x + 4 <= width; x += 4)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_4x64]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        fenc += iStride * 64;
        pRec += iStride * 64;
    }

    /* Consume Y in chunks of 16 */
    for (; y + 16 <= height; y += 16)
    {
        Int x = 0;
        for (; x + 64 <= width; x += 64)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_64x16]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        for (; x + 16 <= width; x += 16)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_16x16]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        for (; x + 4 <= width; x += 4)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_4x16]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        fenc += iStride * 16;
        pRec += iStride * 16;
    }

    /* Consume Y in chunks of 4 */
    for (; y + 4 <= height; y += 4)
    {
        Int x = 0;
        for (; x + 64 <= width; x += 64)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_64x4]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        for (; x + 16 <= width; x += 16)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_16x4]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        for (; x + 4 <= width; x += 4)
        {
            uiSSD += x265::primitives.sse_pp[x265::PARTITION_4x4]((pixel*)fenc + x, (intptr_t)iStride, (pixel*)pRec + x, iStride);
        }

        fenc += iStride * 4;
        pRec += iStride * 4;
    }

    return uiSSD;
}

Void TEncGOP::xCalculateAddPSNR(TComPic* pcPic, TComPicYuv* pcPicD, const AccessUnit& accessUnit)
{
    //===== calculate PSNR =====
    Int iStride = pcPicD->getStride();
    Int width  = pcPicD->getWidth() - m_pcEncTop->getPad(0);
    Int height = pcPicD->getHeight() - m_pcEncTop->getPad(1);
    Int iSize = width * height;

    UInt64 uiSSDY = computeSSD(pcPic->getPicYuvOrg()->getLumaAddr(), pcPicD->getLumaAddr(), iStride, width, height);

    height >>= 1;
    width  >>= 1;
    iStride = pcPicD->getCStride();

    UInt64 uiSSDU = computeSSD(pcPic->getPicYuvOrg()->getCbAddr(), pcPicD->getCbAddr(), iStride, width, height);
    UInt64 uiSSDV = computeSSD(pcPic->getPicYuvOrg()->getCrAddr(), pcPicD->getCrAddr(), iStride, width, height);

    Int maxvalY = 255 << (g_bitDepthY - 8);
    Int maxvalC = 255 << (g_bitDepthC - 8);
    Double fRefValueY = (Double)maxvalY * maxvalY * iSize;
    Double fRefValueC = (Double)maxvalC * maxvalC * iSize / 4.0;
    Double dYPSNR = (uiSSDY ? 10.0 * log10(fRefValueY / (Double)uiSSDY) : 99.99);
    Double dUPSNR = (uiSSDU ? 10.0 * log10(fRefValueC / (Double)uiSSDU) : 99.99);
    Double dVPSNR = (uiSSDV ? 10.0 * log10(fRefValueC / (Double)uiSSDV) : 99.99);

    /* calculate the size of the access unit, excluding:
     *  - any AnnexB contributions (start_code_prefix, zero_byte, etc.,)
     *  - SEI NAL units
     */
    UInt numRBSPBytes = 0;
    for (AccessUnit::const_iterator it = accessUnit.begin(); it != accessUnit.end(); it++)
    {
        UInt numRBSPBytes_nal = UInt((*it)->m_nalUnitData.str().size());
#if VERBOSE_RATE
        printf("*** %6s numBytesInNALunit: %u\n", nalUnitTypeToString((*it)->m_nalUnitType), numRBSPBytes_nal);
#endif
        if ((*it)->m_nalUnitType != NAL_UNIT_PREFIX_SEI && (*it)->m_nalUnitType != NAL_UNIT_SUFFIX_SEI)
        {
            numRBSPBytes += numRBSPBytes_nal;
        }
    }

    UInt uibits = numRBSPBytes * 8;

    /* Acquire encoder global lock to accumulate statistics and print debug info to console */
    x265::ScopedLock(m_pcEncTop->m_statLock);

    //===== add PSNR =====
    m_pcEncTop->m_gcAnalyzeAll.addResult(dYPSNR, dUPSNR, dVPSNR, (Double)uibits);
    TComSlice*  pcSlice = pcPic->getSlice();
    if (pcSlice->isIntra())
    {
        m_pcEncTop->m_gcAnalyzeI.addResult(dYPSNR, dUPSNR, dVPSNR, (Double)uibits);
    }
    if (pcSlice->isInterP())
    {
        m_pcEncTop->m_gcAnalyzeP.addResult(dYPSNR, dUPSNR, dVPSNR, (Double)uibits);
    }
    if (pcSlice->isInterB())
    {
        m_pcEncTop->m_gcAnalyzeB.addResult(dYPSNR, dUPSNR, dVPSNR, (Double)uibits);
    }

    if (m_pcCfg->getLogLevel() < X265_LOG_DEBUG)
        return;

    Char c = (pcSlice->isIntra() ? 'I' : pcSlice->isInterP() ? 'P' : 'B');

    if (!pcSlice->isReferenced())
        c += 32; // lower case if unreferenced

    printf("\rPOC %4d TId: %1d ( %c-SLICE, nQP %d QP %d ) %10d bits",
           pcSlice->getPOC(),
           pcSlice->getTLayer(),
           c,
           pcSlice->getSliceQpBase(),
           pcSlice->getSliceQp(),
           uibits);

    printf(" [Y:%6.2lf U:%6.2lf V:%6.2lf]", dYPSNR, dUPSNR, dVPSNR);

    if (pcSlice->isIntra())
        return;
    Int numLists = pcSlice->isInterP() ? 1 : 2;
    for (Int refList = 0; refList < numLists; refList++)
    {
        printf(" [L%d ", refList);
        for (Int iRefIndex = 0; iRefIndex < pcSlice->getNumRefIdx(RefPicList(refList)); iRefIndex++)
        {
            printf("%d ", pcSlice->getRefPOC(RefPicList(refList), iRefIndex) - pcSlice->getLastIDR());
        }

        printf("]");
    }
}

/** Function for deciding the nal_unit_type.
 * \param pocCurr POC of the current picture
 * \returns the nal unit type of the picture
 * This function checks the configuration and returns the appropriate nal_unit_type for the picture.
 */
NalUnitType TEncGOP::getNalUnitType(Int pocCurr, Int lastIDR)
{
    if (pocCurr == 0)
    {
        return NAL_UNIT_CODED_SLICE_IDR_W_RADL;
    }
    if (pocCurr % m_pcCfg->getIntraPeriod() == 0)
    {
        if (m_pcCfg->getDecodingRefreshType() == 1)
        {
            return NAL_UNIT_CODED_SLICE_CRA;
        }
        else if (m_pcCfg->getDecodingRefreshType() == 2)
        {
            return NAL_UNIT_CODED_SLICE_IDR_W_RADL;
        }
    }
    if (m_pocCRA > 0)
    {
        if (pocCurr < m_pocCRA)
        {
            // All leading pictures are being marked as TFD pictures here since current encoder uses all
            // reference pictures while encoding leading pictures. An encoder can ensure that a leading
            // picture can be still decodable when random accessing to a CRA/CRANT/BLA/BLANT picture by
            // controlling the reference pictures used for encoding that leading picture. Such a leading
            // picture need not be marked as a TFD picture.
            return NAL_UNIT_CODED_SLICE_RASL_R;
        }
    }
    if (lastIDR > 0)
    {
        if (pocCurr < lastIDR)
        {
            return NAL_UNIT_CODED_SLICE_RADL_R;
        }
    }
    return NAL_UNIT_CODED_SLICE_TRAIL_R;
}

/** Attaches the input bitstream to the stream in the output NAL unit
    Updates rNalu to contain concatenated bitstream. rpcBitstreamRedirect is cleared at the end of this function call.
 *  \param codedSliceData contains the coded slice data (bitstream) to be concatenated to rNalu
 *  \param rNalu          target NAL unit
 */
Void TEncGOP::xAttachSliceDataToNalUnit(TEncEntropy* pcEntropyCoder, OutputNALUnit& rNalu, TComOutputBitstream*& codedSliceData)
{
    // Byte-align
    rNalu.m_Bitstream.writeByteAlignment(); // Slice header byte-alignment

    // Perform bitstream concatenation
    if (codedSliceData->getNumberOfWrittenBits() > 0)
    {
        rNalu.m_Bitstream.addSubstream(codedSliceData);
    }

    pcEntropyCoder->setBitstream(&rNalu.m_Bitstream);

    codedSliceData->clear();
}

// Function will arrange the long-term pictures in the decreasing order of poc_lsb_lt,
// and among the pictures with the same lsb, it arranges them in increasing delta_poc_msb_cycle_lt value
Void TEncGOP::arrangeLongtermPicturesInRPS(TComSlice *pcSlice)
{
    TComReferencePictureSet *rps = pcSlice->getRPS();

    if (!rps->getNumberOfLongtermPictures())
    {
        return;
    }

    // Arrange long-term reference pictures in the correct order of LSB and MSB,
    // and assign values for pocLSBLT and MSB present flag
    Int longtermPicsPoc[MAX_NUM_REF_PICS], longtermPicsLSB[MAX_NUM_REF_PICS], indices[MAX_NUM_REF_PICS];
    Int longtermPicsMSB[MAX_NUM_REF_PICS];
    Bool mSBPresentFlag[MAX_NUM_REF_PICS];
    ::memset(longtermPicsPoc, 0, sizeof(longtermPicsPoc));  // Store POC values of LTRP
    ::memset(longtermPicsLSB, 0, sizeof(longtermPicsLSB));  // Store POC LSB values of LTRP
    ::memset(longtermPicsMSB, 0, sizeof(longtermPicsMSB));  // Store POC LSB values of LTRP
    ::memset(indices, 0, sizeof(indices));                  // Indices to aid in tracking sorted LTRPs
    ::memset(mSBPresentFlag, 0, sizeof(mSBPresentFlag));    // Indicate if MSB needs to be present

    // Get the long-term reference pictures
    Int offset = rps->getNumberOfNegativePictures() + rps->getNumberOfPositivePictures();
    Int i, ctr = 0;
    Int maxPicOrderCntLSB = 1 << m_cSPS.getBitsForPOC();
    for (i = rps->getNumberOfPictures() - 1; i >= offset; i--, ctr++)
    {
        longtermPicsPoc[ctr] = rps->getPOC(i);                              // LTRP POC
        longtermPicsLSB[ctr] = getLSB(longtermPicsPoc[ctr], maxPicOrderCntLSB); // LTRP POC LSB
        indices[ctr] = i;
        longtermPicsMSB[ctr] = longtermPicsPoc[ctr] - longtermPicsLSB[ctr];
    }

    Int numLongPics = rps->getNumberOfLongtermPictures();
    assert(ctr == numLongPics);

    // Arrange pictures in decreasing order of MSB;
    for (i = 0; i < numLongPics; i++)
    {
        for (Int j = 0; j < numLongPics - 1; j++)
        {
            if (longtermPicsMSB[j] < longtermPicsMSB[j + 1])
            {
                std::swap(longtermPicsPoc[j], longtermPicsPoc[j + 1]);
                std::swap(longtermPicsLSB[j], longtermPicsLSB[j + 1]);
                std::swap(longtermPicsMSB[j], longtermPicsMSB[j + 1]);
                std::swap(indices[j], indices[j + 1]);
            }
        }
    }

    for (i = 0; i < numLongPics; i++)
    {
        // Check if MSB present flag should be enabled.
        // Check if the buffer contains any pictures that have the same LSB.
        TComList<TComPic*>::iterator iterPic = m_cListPic.begin();
        TComPic* pcPic;
        while (iterPic != m_cListPic.end())
        {
            pcPic = *iterPic;
            if ((getLSB(pcPic->getPOC(), maxPicOrderCntLSB) == longtermPicsLSB[i])   && // Same LSB
                (pcPic->getSlice()->isReferenced()) &&                                  // Reference picture
                (pcPic->getPOC() != longtermPicsPoc[i]))                                // Not the LTRP itself
            {
                mSBPresentFlag[i] = true;
                break;
            }
            iterPic++;
        }
    }

    // tempArray for usedByCurr flag
    Bool tempArray[MAX_NUM_REF_PICS];
    ::memset(tempArray, 0, sizeof(tempArray));
    for (i = 0; i < numLongPics; i++)
    {
        tempArray[i] = rps->getUsed(indices[i]);
    }

    // Now write the final values;
    ctr = 0;
    Int currMSB = 0, currLSB = 0;
    // currPicPoc = currMSB + currLSB
    currLSB = getLSB(pcSlice->getPOC(), maxPicOrderCntLSB);
    currMSB = pcSlice->getPOC() - currLSB;

    for (i = rps->getNumberOfPictures() - 1; i >= offset; i--, ctr++)
    {
        rps->setPOC(i, longtermPicsPoc[ctr]);
        rps->setDeltaPOC(i, -pcSlice->getPOC() + longtermPicsPoc[ctr]);
        rps->setUsed(i, tempArray[ctr]);
        rps->setPocLSBLT(i, longtermPicsLSB[ctr]);
        rps->setDeltaPocMSBCycleLT(i, (currMSB - (longtermPicsPoc[ctr] - longtermPicsLSB[ctr])) / maxPicOrderCntLSB);
        rps->setDeltaPocMSBPresentFlag(i, mSBPresentFlag[ctr]);

        assert(rps->getDeltaPocMSBCycleLT(i) >= 0); // Non-negative value
    }

    for (i = rps->getNumberOfPictures() - 1, ctr = 1; i >= offset; i--, ctr++)
    {
        for (Int j = rps->getNumberOfPictures() - 1 - ctr; j >= offset; j--)
        {
            // Here at the encoder we know that we have set the full POC value for the LTRPs, hence we
            // don't have to check the MSB present flag values for this constraint.
            assert(rps->getPOC(i) != rps->getPOC(j)); // If assert fails, LTRP entry repeated in RPS!!!
        }
    }
}

/** Function for finding the position to insert the first of APS and non-nested BP, PT, DU info SEI messages.
 * \param accessUnit Access Unit of the current picture
 * This function finds the position to insert the first of APS and non-nested BP, PT, DU info SEI messages.
 */
Int TEncGOP::xGetFirstSeiLocation(AccessUnit &accessUnit)
{
    // Find the location of the first SEI message
    AccessUnit::iterator it;
    Int seiStartPos = 0;

    for (it = accessUnit.begin(); it != accessUnit.end(); it++, seiStartPos++)
    {
        if ((*it)->isSei() || (*it)->isVcl())
        {
            break;
        }
    }

//  assert(it != accessUnit.end());  // Triggers with some legit configurations
    return seiStartPos;
}

//! \}
