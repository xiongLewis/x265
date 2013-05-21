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

/** \file     TAppEncTop.cpp
    \brief    Encoder application class
*/

#include <list>
#include <fstream>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <assert.h>

#include "x265enc.h"
#include "TLibEncoder/AnnexBwrite.h"
#include "PPA/ppa.h"

using namespace std;

//! \ingroup TAppEncoder
//! \{

// ====================================================================================================================
// Constructor / destructor / initialization / destroy
// ====================================================================================================================

TAppEncTop::TAppEncTop()
{
    m_iFrameRcvd = 0;
    m_totalBytes = 0;
    m_essentialBytes = 0;
}

TAppEncTop::~TAppEncTop()
{}

Void TAppEncTop::xInitLibCfg()
{
    TComVPS vps;

    vps.setMaxTLayers(m_maxTempLayer);
    if (m_maxTempLayer == 1)
    {
        vps.setTemporalNestingFlag(true);
    }

    vps.setMaxLayers(1);
    for (Int i = 0; i < MAX_TLAYER; i++)
    {
        vps.setNumReorderPics(m_numReorderPics[i], i);
        vps.setMaxDecPicBuffering(m_maxDecPicBuffering[i], i);
    }

    m_cTEncTop.setVPS(&vps);

    m_cTEncTop.setProfile(m_profile);
    m_cTEncTop.setLevel(m_levelTier, m_level);
    m_cTEncTop.setProgressiveSourceFlag(m_progressiveSourceFlag);
    m_cTEncTop.setInterlacedSourceFlag(m_interlacedSourceFlag);
    m_cTEncTop.setNonPackedConstraintFlag(m_nonPackedConstraintFlag);
    m_cTEncTop.setFrameOnlyConstraintFlag(m_frameOnlyConstraintFlag);

    m_cTEncTop.setFrameRate(iFrameRate);
    m_cTEncTop.setSourceWidth(iSourceWidth);
    m_cTEncTop.setSourceHeight(iSourceHeight);
    m_cTEncTop.setIntraPeriod(iIntraPeriod);
    m_cTEncTop.setQP(iQP);
    m_cTEncTop.setUseAMP(enableAMP);
    m_cTEncTop.setUseRectInter(enableRectInter);

    //====== Loop/Deblock Filter ========
    m_cTEncTop.setLoopFilterDisable(bLoopFilterDisable);
    m_cTEncTop.setLoopFilterOffsetInPPS(loopFilterOffsetInPPS);
    m_cTEncTop.setLoopFilterBetaOffset(loopFilterBetaOffsetDiv2);
    m_cTEncTop.setLoopFilterTcOffset(loopFilterTcOffsetDiv2);
    m_cTEncTop.setDeblockingFilterControlPresent(DeblockingFilterControlPresent);
    m_cTEncTop.setDeblockingFilterMetric(DeblockingFilterMetric);

    //====== Motion search ========
    m_cTEncTop.setSearchMethod(searchMethod);
    m_cTEncTop.setSearchRange(iSearchRange);
    m_cTEncTop.setBipredSearchRange(bipredSearchRange);

    //====== Quality control ========
    m_cTEncTop.setMaxCuDQPDepth(iMaxCuDQPDepth);
    m_cTEncTop.setChromaCbQpOffset(cbQpOffset);
    m_cTEncTop.setChromaCrQpOffset(crQpOffset);

    m_cTEncTop.setUseAdaptQpSelect(bUseAdaptQpSelect);
    Int lowestQP = -6 * (g_bitDepthY - 8); // XXX: check
    if ((iQP == lowestQP) && useLossless)
    {
        bUseAdaptiveQP = 0;
    }
    m_cTEncTop.setUseAdaptiveQP(bUseAdaptiveQP);
    m_cTEncTop.setQPAdaptationRange(iQPAdaptationRange);
    m_cTEncTop.setUseRDOQ(useRDOQ);
    m_cTEncTop.setUseRDOQTS(useRDOQTS);
    m_cTEncTop.setRDpenalty(rdPenalty);
    m_cTEncTop.setQuadtreeTULog2MaxSize(uiQuadtreeTULog2MaxSize);
    m_cTEncTop.setQuadtreeTULog2MinSize(uiQuadtreeTULog2MinSize);
    m_cTEncTop.setQuadtreeTUMaxDepthInter(uiQuadtreeTUMaxDepthInter);
    m_cTEncTop.setQuadtreeTUMaxDepthIntra(uiQuadtreeTUMaxDepthIntra);
    m_cTEncTop.setUseFastDecisionForMerge(useFastDecisionForMerge);
    m_cTEncTop.setUseCbfFastMode(bUseCbfFastMode);
    m_cTEncTop.setUseEarlySkipDetection(useEarlySkipDetection);
    m_cTEncTop.setUseTransformSkip(useTransformSkip);
    m_cTEncTop.setUseTransformSkipFast(useTransformSkipFast);
    m_cTEncTop.setUseConstrainedIntraPred(bUseConstrainedIntraPred);
    m_cTEncTop.setPCMLog2MinSize(uiPCMLog2MinSize);
    m_cTEncTop.setUsePCM(usePCM);
    m_cTEncTop.setPCMLog2MaxSize(pcmLog2MaxSize);
    m_cTEncTop.setMaxNumMergeCand(maxNumMergeCand);

    //====== Weighted Prediction ========
    m_cTEncTop.setUseWP(useWeightedPred);
    m_cTEncTop.setWPBiPred(useWeightedBiPred);

    //====== Parallel Merge Estimation ========
    m_cTEncTop.setLog2ParallelMergeLevelMinus2(log2ParallelMergeLevel - 2);

    m_cTEncTop.setUseSAO(bUseSAO);
    m_cTEncTop.setMaxNumOffsetsPerPic(maxNumOffsetsPerPic);

    m_cTEncTop.setSaoLcuBoundary(saoLcuBoundary);
    m_cTEncTop.setSaoLcuBasedOptimization(saoLcuBasedOptimization);
    m_cTEncTop.setPCMInputBitDepthFlag(bPCMInputBitDepthFlag);
    m_cTEncTop.setPCMFilterDisableFlag(bPCMFilterDisableFlag);
    m_cTEncTop.setWaveFrontSynchro(iWaveFrontSynchro);
    m_cTEncTop.setTMVPModeId(TMVPModeId);
    m_cTEncTop.setSignHideFlag(signHideFlag);
    m_cTEncTop.setUseRateCtrl(RCEnableRateControl);
    m_cTEncTop.setTargetBitrate(RCTargetBitrate);
    m_cTEncTop.setKeepHierBit(RCKeepHierarchicalBit);
    m_cTEncTop.setLCULevelRC(RCLCULevelRC);
    m_cTEncTop.setUseLCUSeparateModel(RCUseLCUSeparateModel);
    m_cTEncTop.setInitialQP(RCInitialQP);
    m_cTEncTop.setForceIntraQP(RCForceIntraQP);
    m_cTEncTop.setTransquantBypassEnableFlag(TransquantBypassEnableFlag);
    m_cTEncTop.setCUTransquantBypassFlagValue(CUTransquantBypassFlagValue);
    m_cTEncTop.setUseStrongIntraSmoothing(useStrongIntraSmoothing);
    m_cTEncTop.setUseLossless(useLossless);

    m_cTEncTop.setFrameSkip(m_FrameSkip);
    m_cTEncTop.setConformanceWindow(0, 0, 0, 0);
    m_cTEncTop.setFramesToBeEncoded(m_framesToBeEncoded);
    int nullpad[2] = { 0, 0 };
    m_cTEncTop.setPad(nullpad);

    //====== Coding Structure ========
    m_cTEncTop.setDecodingRefreshType(m_iDecodingRefreshType);
    m_cTEncTop.setGOPSize(m_iGOPSize);
    m_cTEncTop.setGopList(m_GOPList);
    m_cTEncTop.setExtraRPSs(m_extraRPSs);
    for (Int i = 0; i < MAX_TLAYER; i++)
    {
        m_cTEncTop.setNumReorderPics(m_numReorderPics[i], i);
        m_cTEncTop.setMaxDecPicBuffering(m_maxDecPicBuffering[i], i);
    }

    for (UInt uiLoop = 0; uiLoop < MAX_TLAYER; ++uiLoop)
    {
        m_cTEncTop.setLambdaModifier(uiLoop, m_adLambdaModifier[uiLoop]);
    }
    m_cTEncTop.setMaxTempLayer(m_maxTempLayer);


    //====== Tool list ========
    m_cTEncTop.setUseASR(m_bUseASR);
    m_cTEncTop.setUseHADME(m_bUseHADME);
    m_cTEncTop.setdQPs(m_aidQP);
    m_cTEncTop.setDecodedPictureHashSEIEnabled(m_decodedPictureHashSEIEnabled);
    m_cTEncTop.setRecoveryPointSEIEnabled(m_recoveryPointSEIEnabled);
    m_cTEncTop.setBufferingPeriodSEIEnabled(m_bufferingPeriodSEIEnabled);
    m_cTEncTop.setPictureTimingSEIEnabled(m_pictureTimingSEIEnabled);
    
    m_cTEncTop.setDisplayOrientationSEIAngle(m_displayOrientationSEIAngle);
    m_cTEncTop.setTemporalLevel0IndexSEIEnabled(m_temporalLevel0IndexSEIEnabled);
    m_cTEncTop.setGradualDecodingRefreshInfoEnabled(m_gradualDecodingRefreshInfoEnabled);
    m_cTEncTop.setDecodingUnitInfoSEIEnabled(m_decodingUnitInfoSEIEnabled);
    m_cTEncTop.setSOPDescriptionSEIEnabled(m_SOPDescriptionSEIEnabled);
    m_cTEncTop.setScalableNestingSEIEnabled(m_scalableNestingSEIEnabled);
    m_cTEncTop.setUseScalingListId(m_useScalingListId);
    m_cTEncTop.setScalingListFile(m_scalingListFile);
    m_cTEncTop.setUseRecalculateQPAccordingToLambda(m_recalculateQPAccordingToLambda);

    m_cTEncTop.setActiveParameterSetsSEIEnabled(m_activeParameterSetsSEIEnabled);
    m_cTEncTop.setVuiParametersPresentFlag(m_vuiParametersPresentFlag);
    m_cTEncTop.setAspectRatioIdc(m_aspectRatioIdc);
    m_cTEncTop.setSarWidth(m_sarWidth);
    m_cTEncTop.setSarHeight(m_sarHeight);
    m_cTEncTop.setOverscanInfoPresentFlag(m_overscanInfoPresentFlag);
    m_cTEncTop.setOverscanAppropriateFlag(m_overscanAppropriateFlag);
    m_cTEncTop.setVideoSignalTypePresentFlag(m_videoSignalTypePresentFlag);
    m_cTEncTop.setVideoFormat(m_videoFormat);
    m_cTEncTop.setVideoFullRangeFlag(m_videoFullRangeFlag);
    m_cTEncTop.setColourDescriptionPresentFlag(m_colourDescriptionPresentFlag);
    m_cTEncTop.setColourPrimaries(m_colourPrimaries);
    m_cTEncTop.setTransferCharacteristics(m_transferCharacteristics);
    m_cTEncTop.setMatrixCoefficients(m_matrixCoefficients);
    m_cTEncTop.setChromaLocInfoPresentFlag(m_chromaLocInfoPresentFlag);
    m_cTEncTop.setChromaSampleLocTypeTopField(m_chromaSampleLocTypeTopField);
    m_cTEncTop.setChromaSampleLocTypeBottomField(m_chromaSampleLocTypeBottomField);
    m_cTEncTop.setNeutralChromaIndicationFlag(m_neutralChromaIndicationFlag);
    m_cTEncTop.setDefaultDisplayWindow(m_defDispWinLeftOffset,
                                       m_defDispWinRightOffset,
                                       m_defDispWinTopOffset,
                                       m_defDispWinBottomOffset);
    m_cTEncTop.setFrameFieldInfoPresentFlag(m_frameFieldInfoPresentFlag);
    m_cTEncTop.setPocProportionalToTimingFlag(m_pocProportionalToTimingFlag);
    m_cTEncTop.setNumTicksPocDiffOneMinus1(m_numTicksPocDiffOneMinus1);
    m_cTEncTop.setBitstreamRestrictionFlag(m_bitstreamRestrictionFlag);
    m_cTEncTop.setMotionVectorsOverPicBoundariesFlag(m_motionVectorsOverPicBoundariesFlag);
    m_cTEncTop.setMinSpatialSegmentationIdc(m_minSpatialSegmentationIdc);
    m_cTEncTop.setMaxBytesPerPicDenom(m_maxBytesPerPicDenom);
    m_cTEncTop.setMaxBitsPerMinCuDenom(m_maxBitsPerMinCuDenom);
    m_cTEncTop.setLog2MaxMvLengthHorizontal(m_log2MaxMvLengthHorizontal);
    m_cTEncTop.setLog2MaxMvLengthVertical(m_log2MaxMvLengthVertical);
}

Void TAppEncTop::xCreateLib()
{
    m_cTEncTop.create();
}

Void TAppEncTop::xDestroyLib()
{
    m_cTEncTop.destroy();
}

Void TAppEncTop::xInitLib()
{
    m_cTEncTop.init();
}

// ====================================================================================================================
// Public member functions
// ====================================================================================================================

/**
 - create internal class
 - initialize internal variable
 - until the end of input YUV file, call encoding function in TEncTop class
 - delete allocated buffers
 - destroy internal class
 .
 */
Void TAppEncTop::encode()
{
    fstream bitstreamFile(m_pchBitstreamFile, fstream::binary | fstream::out);

    if (!bitstreamFile)
    {
        fprintf(stderr, "\nfailed to open bitstream file `%s' for writing\n", m_pchBitstreamFile);
        exit(EXIT_FAILURE);
    }

    TComPicYuv       *pcPicYuvOrg = new TComPicYuv;
    TComPicYuv       *pcPicYuvRec = NULL;

    // initialize internal class & member variables
    xInitLibCfg();
    xCreateLib();
    xInitLib();

    // main encoder loop
    Int   iNumEncoded = 0;
    Bool  bEos = false;

    list<AccessUnit> outputAccessUnits; ///< list of access units to write out.  is populated by the encoding process

    // allocate original YUV buffer
    pcPicYuvOrg->create(iSourceWidth, iSourceHeight, uiMaxCUSize, uiMaxCUSize, uiMaxCUDepth);

    while (!bEos)
    {
        // get buffers
        xGetBuffer(pcPicYuvRec);

        // read input YUV file
        x265_picture_t pic;
        Bool flush = false;
        if (m_input->readPicture(pic))
        {
            m_iFrameRcvd++;
            bEos = (m_iFrameRcvd == m_framesToBeEncoded);
        }
        else
        {
            flush = true;
            bEos = true;
            m_cTEncTop.setFramesToBeEncoded(m_iFrameRcvd);
        }

        // call encoding function for one frame
        PPAStartCpuEventFunc(encode_frame);
        m_cTEncTop.encode(bEos, flush ? 0 : &pic, m_cListPicYuvRec, outputAccessUnits, iNumEncoded);
        PPAStopCpuEventFunc(encode_frame);

        // write bistream to file if necessary
        if (iNumEncoded > 0)
        {
            PPAStartCpuEventFunc(bitstream_write);
            xWriteOutput(bitstreamFile, iNumEncoded, outputAccessUnits);
            outputAccessUnits.clear();
            PPAStopCpuEventFunc(bitstream_write);
        }
    }

    m_cTEncTop.printSummary();

    // delete original YUV buffer
    pcPicYuvOrg->destroy();
    delete pcPicYuvOrg;
    pcPicYuvOrg = NULL;

    // delete used buffers in encoder class
    m_cTEncTop.deletePicBuffer();

    // delete buffers & classes
    xDeleteBuffer();
    xDestroyLib();

    printRateSummary();
}

// ====================================================================================================================
// Protected member functions
// ====================================================================================================================

/**
 - application has picture buffer list with size of GOP
 - picture buffer list acts as ring buffer
 - end of the list has the latest picture
 .
 */
Void TAppEncTop::xGetBuffer(TComPicYuv*& rpcPicYuvRec)
{
    assert(m_iGOPSize > 0);

    // org. buffer
    if (m_cListPicYuvRec.size() == (UInt)m_iGOPSize)
    {
        rpcPicYuvRec = m_cListPicYuvRec.popFront();
    }
    else
    {
        rpcPicYuvRec = new TComPicYuv;

        rpcPicYuvRec->create(iSourceWidth, iSourceHeight, uiMaxCUSize, uiMaxCUSize, uiMaxCUDepth);
    }

    m_cListPicYuvRec.pushBack(rpcPicYuvRec);
}

Void TAppEncTop::xDeleteBuffer()
{
    TComList<TComPicYuv *>::iterator iterPicYuvRec  = m_cListPicYuvRec.begin();

    Int iSize = Int(m_cListPicYuvRec.size());

    for (Int i = 0; i < iSize; i++)
    {
        TComPicYuv  *pcPicYuvRec  = *(iterPicYuvRec++);
        pcPicYuvRec->destroy();
        delete pcPicYuvRec;
        pcPicYuvRec = NULL;
    }
}

/** \param iNumEncoded  number of encoded frames
 */
Void TAppEncTop::xWriteOutput(std::ostream &bitstreamFile, Int iNumEncoded, const std::list<AccessUnit>& accessUnits)
{
    Int i;

    TComList<TComPicYuv *>::iterator iterPicYuvRec = m_cListPicYuvRec.end();
    list<AccessUnit>::const_iterator iterBitstream = accessUnits.begin();

    for (i = 0; i < iNumEncoded; i++)
    {
        --iterPicYuvRec;
    }

    x265_picture_t pic;
    for (i = 0; i < iNumEncoded; i++)
    {
        if (m_recon)
        {
            TComPicYuv  *pcPicYuvRec  = *(iterPicYuvRec++);
            pic.planes[0] = pcPicYuvRec->getLumaAddr(); pic.stride[0] = pcPicYuvRec->getStride();
            pic.planes[1] = pcPicYuvRec->getCbAddr();   pic.stride[1] = pcPicYuvRec->getCStride();
            pic.planes[2] = pcPicYuvRec->getCrAddr();   pic.stride[2] = pcPicYuvRec->getCStride();
            pic.bitDepth = sizeof(Pel)*8;
            m_recon->writePicture(pic);
        }

        const AccessUnit &au = *(iterBitstream++);
        const vector<UInt>& stats = writeAnnexB(bitstreamFile, au);
        rateStatsAccum(au, stats);
    }
}

/**
 *
 */
void TAppEncTop::rateStatsAccum(const AccessUnit &au, const std::vector<UInt>& annexBsizes)
{
    AccessUnit::const_iterator it_au = au.begin();

    vector<UInt>::const_iterator it_stats = annexBsizes.begin();

    for (; it_au != au.end(); it_au++, it_stats++)
    {
        switch ((*it_au)->m_nalUnitType)
        {
        case NAL_UNIT_CODED_SLICE_TRAIL_R:
        case NAL_UNIT_CODED_SLICE_TRAIL_N:
        case NAL_UNIT_CODED_SLICE_TLA_R:
        case NAL_UNIT_CODED_SLICE_TSA_N:
        case NAL_UNIT_CODED_SLICE_STSA_R:
        case NAL_UNIT_CODED_SLICE_STSA_N:
        case NAL_UNIT_CODED_SLICE_BLA_W_LP:
        case NAL_UNIT_CODED_SLICE_BLA_W_RADL:
        case NAL_UNIT_CODED_SLICE_BLA_N_LP:
        case NAL_UNIT_CODED_SLICE_IDR_W_RADL:
        case NAL_UNIT_CODED_SLICE_IDR_N_LP:
        case NAL_UNIT_CODED_SLICE_CRA:
        case NAL_UNIT_CODED_SLICE_RADL_N:
        case NAL_UNIT_CODED_SLICE_RADL_R:
        case NAL_UNIT_CODED_SLICE_RASL_N:
        case NAL_UNIT_CODED_SLICE_RASL_R:
        case NAL_UNIT_VPS:
        case NAL_UNIT_SPS:
        case NAL_UNIT_PPS:
            m_essentialBytes += *it_stats;
            break;
        default:
            break;
        }

        m_totalBytes += *it_stats;
    }
}

void TAppEncTop::printRateSummary()
{
    Double time = (Double)m_iFrameRcvd / iFrameRate;

    printf("Bytes written to file: %u (%.3f kbps)\n", m_totalBytes, 0.008 * m_totalBytes / time);
#if VERBOSE_RATE
    printf("Bytes for SPS/PPS/Slice (Incl. Annex B): %u (%.3f kbps)\n", m_essentialBytes, 0.008 * m_essentialBytes / time);
#endif
}

//! \}
