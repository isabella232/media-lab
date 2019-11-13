/*
// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/
#include "DecodeThreadBlock.h"
#include "common.h"
#include "Statistics.h"
#include "MfxSessionMgr.h"
#include <iostream>

DecodeThreadBlock::DecodeThreadBlock(uint32_t channel):
    m_channel(channel),
    m_decodeRefNum(1),
    m_vpRefNum(1),
    m_vpRatio(1),
    m_bEnableDecPostProc(false),
    m_mfxSession(nullptr),
    m_mfxAllocator(nullptr),
    m_mfxDecode(nullptr),
    m_mfxVpp(nullptr),
    m_decodeSurfaces(nullptr),
    m_vpInSurface(nullptr),
    m_vpOutSurfaces(nullptr),
    m_vpOutBuffers(nullptr),
    m_decodeSurfNum(0),
    m_vpInSurfNum(0),
    m_vpOutSurfNum(0),
    m_decodeExtBuf(nullptr),
    m_vpExtBuf(nullptr),
    m_buffer(nullptr),
    m_bufferOffset(0),
    m_bufferLength(0),
    m_vpOutFormat(MFX_FOURCC_RGBP),
    m_vpOutWidth(0),
    m_vpOutHeight(0),
    m_decOutRefs(nullptr),
    m_vpOutRefs(nullptr),
    m_vpOutDump(false),
    m_vpMemOutTypeVideo(false),
    m_decodeOutputWithVP(false)
{
    memset(&m_decParams, 0, sizeof(m_decParams));
    memset(&m_vppParams, 0, sizeof(m_vppParams));
    memset(&m_scalingConfig, 0, sizeof(m_scalingConfig));
    memset(&m_decVideoProcConfig, 0, sizeof(m_decVideoProcConfig));
    // allocate the buffer
    m_buffer = new uint8_t[1024 * 1024];
}

DecodeThreadBlock::DecodeThreadBlock(uint32_t channel, MFXVideoSession *externalMfxSession, mfxFrameAllocator *mfxAllocator):
    DecodeThreadBlock(channel)
{
    m_mfxSession = externalMfxSession;
    m_mfxAllocator = mfxAllocator;
}

DecodeThreadBlock::~DecodeThreadBlock()
{
    MfxSessionMgr::getInstance().Clear(m_channel);
    delete[] m_buffer;
}

int DecodeThreadBlock::PrepareInternal()
{
    mfxStatus sts;
    if (m_mfxSession == nullptr)
    {
        m_mfxSession = MfxSessionMgr::getInstance().GetSession(m_channel);
    }
    if (m_mfxAllocator == nullptr)
    {
        m_mfxAllocator = MfxSessionMgr::getInstance().GetAllocator(m_channel);
    }

    m_mfxDecode = new MFXVideoDECODE(*m_mfxSession);
    m_mfxVpp = new MFXVideoVPP(*m_mfxSession);

    m_decParams.mfx.CodecId = MFX_CODEC_AVC;
    m_decParams.IOPattern = MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    printf("\t\t.. Suppport H.264(AVC) and use video memory.\n");

    // Prepare media sdk bit stream buffer
    // - Arbitrary buffer size for this example        
    memset(&m_mfxBS, 0, sizeof(m_mfxBS));
    m_mfxBS.MaxLength = 1024 * 1024;
    m_mfxBS.Data = new mfxU8[m_mfxBS.MaxLength];
    MSDK_CHECK_POINTER(m_mfxBS.Data, MFX_ERR_MEMORY_ALLOC);

    // Prepare media sdk decoder parameters
    ReadBitStreamData();

    sts = m_mfxDecode->DecodeHeader(&m_mfxBS, &m_decParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);

    printf("\t. Done preparing video parameters.\n");

    // Prepare media sdk vp parameters
    m_vppParams.ExtParam = (mfxExtBuffer **)&m_vpExtBuf;
    m_scalingConfig.Header.BufferId = MFX_EXTBUFF_VPP_SCALING;
    m_scalingConfig.Header.BufferSz = sizeof(mfxExtVPPScaling);
    m_scalingConfig.ScalingMode = MFX_SCALING_MODE_LOWPOWER;
    m_vppParams.ExtParam[0] = (mfxExtBuffer*)&(m_scalingConfig);
    m_vppParams.NumExtParam = 1;

    // Video processing input data format / decoded frame information
    m_vppParams.vpp.In.FourCC         = MFX_FOURCC_NV12;
    m_vppParams.vpp.In.ChromaFormat   = MFX_CHROMAFORMAT_YUV420;
    m_vppParams.vpp.In.CropX          = 0;
    m_vppParams.vpp.In.CropY          = 0;
    m_vppParams.vpp.In.CropW          = m_decParams.mfx.FrameInfo.CropW;
    m_vppParams.vpp.In.CropH          = m_decParams.mfx.FrameInfo.CropH;
    m_vppParams.vpp.In.PicStruct      = MFX_PICSTRUCT_PROGRESSIVE;
    m_vppParams.vpp.In.FrameRateExtN  = 30;
    m_vppParams.vpp.In.FrameRateExtD  = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    m_vppParams.vpp.In.Width  = MSDK_ALIGN16(m_vppParams.vpp.In.CropW);
    m_vppParams.vpp.In.Height = (MFX_PICSTRUCT_PROGRESSIVE == m_vppParams.vpp.In.PicStruct)?
                              MSDK_ALIGN16(m_vppParams.vpp.In.CropH) : MSDK_ALIGN32(m_vppParams.vpp.In.CropH);
    
    // Video processing output data format / resized frame information for inference engine
    m_vppParams.vpp.Out.FourCC        = m_vpOutFormat;
    m_vppParams.vpp.Out.ChromaFormat  = MFX_CHROMAFORMAT_YUV444;
    /* Extra Check for chroma format */
    if (MFX_FOURCC_NV12 == m_vppParams.vpp.Out.FourCC)
        m_vppParams.vpp.Out.ChromaFormat  = MFX_CHROMAFORMAT_YUV420;
    m_vppParams.vpp.Out.CropX         = 0;
    m_vppParams.vpp.Out.CropY         = 0;
    m_vppParams.vpp.Out.CropW         = m_vpOutWidth;
    m_vppParams.vpp.Out.CropH         = m_vpOutHeight;
    m_vppParams.vpp.Out.PicStruct     = MFX_PICSTRUCT_PROGRESSIVE;
    m_vppParams.vpp.Out.FrameRateExtN = 30;
    m_vppParams.vpp.Out.FrameRateExtD = 1;
    // width must be a multiple of 16
    // height must be a multiple of 16 in case of frame picture and a multiple of 32 in case of field picture
    m_vppParams.vpp.Out.Width  = MSDK_ALIGN16(m_vppParams.vpp.Out.CropW);
    m_vppParams.vpp.Out.Height = (MFX_PICSTRUCT_PROGRESSIVE == m_vppParams.vpp.Out.PicStruct)?
                               MSDK_ALIGN16(m_vppParams.vpp.Out.CropH) : MSDK_ALIGN32(m_vppParams.vpp.Out.CropH);
    
    m_vppParams.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY | MFX_IOPATTERN_OUT_VIDEO_MEMORY;

    if (m_bEnableDecPostProc)
    {
        if ( (MFX_CODEC_AVC == m_decParams.mfx.CodecId) && /* Only for AVC */
             (MFX_PICSTRUCT_PROGRESSIVE == m_decParams.mfx.FrameInfo.PicStruct)) /* ...And only for progressive!*/
        {   /* it is possible to use decoder's post-processing */
        	m_decVideoProcConfig.Header.BufferId    = MFX_EXTBUFF_DEC_VIDEO_PROCESSING;
        	m_decVideoProcConfig.Header.BufferSz    = sizeof(mfxExtDecVideoProcessing);
        	m_decVideoProcConfig.In.CropX = 0;
        	m_decVideoProcConfig.In.CropY = 0;
        	m_decVideoProcConfig.In.CropW = m_decParams.mfx.FrameInfo.CropW;
        	m_decVideoProcConfig.In.CropH = m_decParams.mfx.FrameInfo.CropH;

        	m_decVideoProcConfig.Out.FourCC = m_decParams.mfx.FrameInfo.FourCC;
        	m_decVideoProcConfig.Out.ChromaFormat = m_decParams.mfx.FrameInfo.ChromaFormat;
        	m_decVideoProcConfig.Out.Width = MSDK_ALIGN16(m_vppParams.vpp.Out.Width);
        	m_decVideoProcConfig.Out.Height = MSDK_ALIGN16(m_vppParams.vpp.Out.Height);
        	m_decVideoProcConfig.Out.CropX = 0;
        	m_decVideoProcConfig.Out.CropY = 0;
        	m_decVideoProcConfig.Out.CropW = m_vppParams.vpp.Out.CropW;
        	m_decVideoProcConfig.Out.CropH = m_vppParams.vpp.Out.CropH;
            m_decParams.ExtParam = (mfxExtBuffer **)&m_decodeExtBuf;
            m_decParams.ExtParam[0] = (mfxExtBuffer*)&(m_decVideoProcConfig);
            m_decParams.NumExtParam = 1;
            //std::cout << "\t.Decoder's post-processing is used for resizing\n"<< std::endl;
            printf("\t.Decoder's post-processing is used for resizing\n");

            /* need to correct VPP params: re-size done after decoding
             * So, VPP for CSC NV12->RGBP only */
            m_vppParams.vpp.In.Width = m_decVideoProcConfig.Out.Width;
            m_vppParams.vpp.In.Height = m_decVideoProcConfig.Out.Height;
            m_vppParams.vpp.In.CropW = m_vppParams.vpp.Out.CropW;
            m_vppParams.vpp.In.CropH = m_vppParams.vpp.Out.CropH;
            /* scaling is off (it was configured via extended buffer)*/
            m_vppParams.NumExtParam = 0;
            m_vppParams.ExtParam = NULL;
        }
    }

    // [decoder]
    // Query number of required surfaces
    mfxFrameAllocRequest DecRequest = { 0 };
    sts = m_mfxDecode->QueryIOSurf(&m_decParams, &DecRequest);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    
    DecRequest.Type = MFX_MEMTYPE_EXTERNAL_FRAME | MFX_MEMTYPE_FROM_DECODE | MFX_MEMTYPE_FROM_VPPIN;
    DecRequest.Type |= MFX_MEMTYPE_VIDEO_MEMORY_DECODER_TARGET;
    
    // Try to add two more surfaces
    DecRequest.NumFrameMin +=2;
    DecRequest.NumFrameSuggested +=2;

    // memory allocation
    mfxFrameAllocResponse DecResponse = { 0 };
    sts = m_mfxAllocator->Alloc(m_mfxAllocator->pthis, &DecRequest, &DecResponse);
    if(MFX_ERR_NONE > sts)
    {
        MSDK_PRINT_RET_MSG(sts);
        return sts;
    }
    m_decodeSurfNum = DecResponse.NumFrameActual;
    // this surface will be used when decodes encoded stream
    m_decodeSurfaces = new mfxFrameSurface1 * [m_decodeSurfNum];
    if(!m_decodeSurfaces)
    {
        MSDK_PRINT_RET_MSG(MFX_ERR_MEMORY_ALLOC);            
        return -1;
    }

    for (int i = 0; i < m_decodeSurfNum; i++)
    {
        m_decodeSurfaces[i] = new mfxFrameSurface1;
        memset(m_decodeSurfaces[i], 0, sizeof(mfxFrameSurface1));
        memcpy(&(m_decodeSurfaces[i]->Info), &(m_decParams.mfx.FrameInfo), sizeof(mfxFrameInfo));

        // external allocator used - provide just MemIds
        m_decodeSurfaces[i]->Data.MemId = DecResponse.mids[i];
    }

    // [VPP]
    // query input and output surface number
    mfxFrameAllocRequest VPPRequest[2];// [0] - in, [1] - out
    memset(&VPPRequest, 0, sizeof(mfxFrameAllocRequest) * 2);
    sts = m_mfxVpp->QueryIOSurf(&m_vppParams, VPPRequest);

    // [VPP input]
    // use decode output as vpp input
    // each vp input (decode output) has an external reference count
    m_decOutRefs = new int[m_decodeSurfNum];
    memset(m_decOutRefs, 0, sizeof(int)*m_decodeSurfNum);

    // [VPP output]
    mfxFrameAllocResponse VPP_Out_Response = { 0 };
    memcpy(&VPPRequest[1].Info, &(m_vppParams.vpp.Out), sizeof(mfxFrameInfo));    // allocate VPP output frame information

    sts = m_mfxAllocator->Alloc(m_mfxAllocator->pthis, &(VPPRequest[1]), &VPP_Out_Response);

    if(MFX_ERR_NONE > sts)
    {
        MSDK_PRINT_RET_MSG(sts);
        return 1;
    }
    m_vpOutSurfNum = VPP_Out_Response.NumFrameActual;
    m_vpOutSurfaces = new mfxFrameSurface1 * [m_vpOutSurfNum];
    m_vpOutRefs = new int[m_vpOutSurfNum];
    memset(m_vpOutRefs, 0, sizeof(int)*m_vpOutSurfNum);
    m_vpOutBuffers = new uint8_t *[m_vpOutSurfNum];
    memset(m_vpOutBuffers, 0, sizeof(uint8_t *)*m_vpOutSurfNum);
    
    if(!m_vpOutSurfaces)
    {
        MSDK_PRINT_RET_MSG(MFX_ERR_MEMORY_ALLOC);            
        return 1;
    }
    
    for (int i = 0; i < m_vpOutSurfNum; i++)
    {
        m_vpOutSurfaces[i] = new mfxFrameSurface1;
        memset(m_vpOutSurfaces[i], 0, sizeof(mfxFrameSurface1));
        memcpy(&(m_vpOutSurfaces[i]->Info), &(m_vppParams.vpp.Out), sizeof(mfxFrameInfo));
    
        // external allocator used - provide just MemIds
        m_vpOutSurfaces[i]->Data.MemId = VPP_Out_Response.mids[i];

        // allocate system buffer to store VP output
        m_vpOutBuffers[i] = new uint8_t[3*m_vpOutWidth*m_vpOutHeight]; // RGBP
        memset(m_vpOutBuffers[i], 0, 3*m_vpOutWidth*m_vpOutHeight);
    }

    // Initialize MSDK decoder
    sts = m_mfxDecode->Init(&m_decParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
        
    if(MFX_ERR_NONE > sts)
    {
        MSDK_PRINT_RET_MSG(sts);
        return -1;
    }

    // Initialize MSDK VP
    sts = m_mfxVpp->Init(&m_vppParams);
    MSDK_IGNORE_MFX_STS(sts, MFX_WRN_PARTIAL_ACCELERATION);
    MSDK_CHECK_RESULT(sts, MFX_ERR_NONE, sts);

    return 0;
}

int DecodeThreadBlock::ReadBitStreamData()
{
    memmove(m_mfxBS.Data, m_mfxBS.Data + m_mfxBS.DataOffset, m_mfxBS.DataLength);
    m_mfxBS.DataOffset = 0;
    uint32_t targetLen = m_mfxBS.MaxLength - m_mfxBS.DataLength;

    if (m_bufferLength > targetLen)
    {
        memcpy(m_mfxBS.Data + m_mfxBS.DataLength, m_buffer + m_bufferOffset, targetLen);
        uint32_t remainingSize = m_bufferLength - targetLen;
        m_bufferOffset += targetLen;
        m_bufferLength = remainingSize;
        m_mfxBS.DataLength += targetLen;
        return 0;
    }

    uint32_t copiedLen = 0;
    if (m_bufferLength > 0)
    {
        memcpy(m_mfxBS.Data + m_mfxBS.DataLength, m_buffer + m_bufferOffset, m_bufferLength);
        m_bufferLength = 0;
        m_bufferOffset = 0;
        m_mfxBS.DataLength += m_bufferLength;
        copiedLen += m_bufferLength;
    }

    while (m_mfxBS.DataLength < m_mfxBS.MaxLength)
    {
        VADataPacket *packet = AcquireInput();
        VAData *data = packet->front();
        uint8_t *src = data->GetSurfacePointer();
        uint32_t len, offset;
        data->GetBufferInfo(&offset, &len);
        ReleaseInput(packet);
        if (len == 0)
        {
            data->DeRef();
            return (copiedLen == 0)?MFX_ERR_MORE_DATA:0;
        }
        if (len > (m_mfxBS.MaxLength - m_mfxBS.DataLength))
        {
            uint32_t copyLen = m_mfxBS.MaxLength - m_mfxBS.DataLength;
            uint32_t remainingLen = len - copyLen;
            memcpy(m_mfxBS.Data + m_mfxBS.DataLength, src + offset, copyLen);
            m_mfxBS.DataLength += copyLen;
            copiedLen += copyLen;
            memcpy(m_buffer, src + offset + copyLen, remainingLen);
            m_bufferLength = remainingLen;
        }
        else
        {
            memcpy(m_mfxBS.Data + m_mfxBS.DataLength, src + offset, len);
            m_mfxBS.DataLength += len;
            copiedLen += len;
        }
        data->DeRef();
    }

    return 0;
}

int DecodeThreadBlock::Loop()
{
    mfxStatus sts = MFX_ERR_NONE;
    bool bNeedMore = false;
    mfxSyncPoint syncpDec;
    mfxSyncPoint syncpVPP;
    int nIndexDec = 0;
    int nIndexVpIn = 0;
    int nIndexVpOut = 0;
    uint32_t nDecoded = 0;
    while ((MFX_ERR_NONE <= sts || MFX_ERR_MORE_DATA == sts || MFX_ERR_MORE_SURFACE == sts))
    {
        if (m_stop)
        {
            break;
        }
        if (MFX_WRN_DEVICE_BUSY == sts)
        {
            usleep(1000); // Wait if device is busy, then repeat the same call to DecodeFrameAsync
        }
        if (MFX_ERR_MORE_DATA == sts)
        {
            sts = (mfxStatus)ReadBitStreamData(); // doesn't return if meets the end of stream, try again
            if (sts != 0)
            {
                sts = (mfxStatus)ReadBitStreamData();
            }
            MSDK_BREAK_ON_ERROR(sts);
        }

        if (MFX_ERR_MORE_SURFACE == sts || MFX_ERR_NONE == sts)
        {
            nIndexDec =GetFreeSurface(m_decodeSurfaces, m_decOutRefs, m_decodeSurfNum);
            while(nIndexDec == MFX_ERR_NOT_FOUND)
            {
                usleep(10000);
                nIndexDec = GetFreeSurface(m_decodeSurfaces, m_decOutRefs, m_decodeSurfNum);
            }
        }

        sts = m_mfxDecode->DecodeFrameAsync(&m_mfxBS, m_decodeSurfaces[nIndexDec], &m_vpInSurface, &syncpDec);

        if (sts > MFX_ERR_NONE && syncpDec)
        {
            bNeedMore = false;
            sts = MFX_ERR_NONE;
        }
        else if(MFX_ERR_MORE_DATA == sts)
        {
            bNeedMore = true;
        }
        else if(MFX_ERR_MORE_SURFACE == sts)
        {
            bNeedMore = true;
        }
        if (sts == MFX_ERR_NONE)
        {
            ++ nDecoded;
            //printf("Decode one frame\n");
            Statistics::getInstance().Step(DECODED_FRAMES);
        }
        else
        {
            continue;
        }

        int curDecRef = m_decodeRefNum;
        if (m_decodeOutputWithVP && m_vpRatio && (nDecoded %m_vpRatio) == 0)
        {
            ++ curDecRef;
        }
        // push decoded output surface to output pin
        VADataPacket *outputPacket = DequeueOutput();
        if (curDecRef) // if decoded output is needed
        {
            VAData *vaData = VAData::Create(m_vpInSurface, m_mfxAllocator);
            
            int outputIndex = -1;
            for (int j = 0; j < m_decodeSurfNum; j ++)
            {
                if (m_decodeSurfaces[j] == m_vpInSurface)
                {
                    outputIndex = j;
                    break;
                }
            }
            if (outputIndex == -1)
            {
                printf("Error: decode output surface not one of the working surfaces\n");
                continue;
            }
            vaData->SetExternalRef(&m_decOutRefs[outputIndex]);
            vaData->SetID(m_channel, nDecoded);
            vaData->SetRef(curDecRef);
            outputPacket->push_back(vaData);
        }
        
        if (m_vpRatio && (nDecoded %m_vpRatio) == 0)
        {
            nIndexVpOut = GetFreeSurface(m_vpOutSurfaces, m_vpOutRefs, m_vpOutSurfNum);
            while(nIndexVpOut == MFX_ERR_NOT_FOUND)
            {
                //printf("Channel %d: Not able to find an avaialbe VPP output surface\n", m_channel);
                nIndexVpOut = GetFreeSurface(m_vpOutSurfaces, m_vpOutRefs, m_vpOutSurfNum);
            }

            while (1)
            {
                sts = m_mfxVpp->RunFrameVPPAsync(m_vpInSurface, m_vpOutSurfaces[nIndexVpOut], nullptr, &syncpVPP);

                if (MFX_ERR_NONE < sts && !syncpVPP) // repeat the call if warning and no output
                {
                    if (MFX_WRN_DEVICE_BUSY == sts)
                    {
                        usleep(1000); // wait if device is busy
                    }
                }
                else if (MFX_ERR_NONE < sts && syncpVPP)
                {
                    sts = MFX_ERR_NONE; // ignore warnings if output is available
                    break;
                }
                else{
                    break; // not a warning
                }
            }

            if (sts == MFX_ERR_MORE_DATA)
            {
                continue;
            }
            else if (sts == MFX_ERR_NONE)
            {
                //printf("VP one frame\n");
                sts = m_mfxSession->SyncOperation(syncpVPP, 60000); // Synchronize. Wait until decoded frame is ready

                if (m_vpRefNum)
                {
                    // lock vp surface and pass to next block
                    mfxFrameSurface1 *pSurface = m_vpOutSurfaces[nIndexVpOut];
                    m_mfxAllocator->Lock(m_mfxAllocator->pthis, pSurface->Data.MemId, &(pSurface->Data));
                    mfxFrameInfo *pInfo = &pSurface->Info;
                    mfxFrameData *pData = &pSurface->Data;

                    uint8_t* ptr;
                    uint32_t w, h;
                    if (pInfo->CropH > 0 && pInfo->CropW > 0)
                    {
                        w = pInfo->CropW;
                        h = pInfo->CropH;
                    }
                    else
                    {
                        w = pInfo->Width;
                        h = pInfo->Height;
                    }

                    uint8_t *pTemp = m_vpOutBuffers[nIndexVpOut];
                    ptr   = pData->B + (pInfo->CropX ) + (pInfo->CropY ) * pData->Pitch;

                    for (int i = 0; i < h; i++)
                    {
                       memcpy(pTemp + i*w, ptr + i*pData->Pitch, w);
                    }


                    ptr	= pData->G + (pInfo->CropX ) + (pInfo->CropY ) * pData->Pitch;
                    pTemp = m_vpOutBuffers[nIndexVpOut] + w*h;
                    for(int i = 0; i < h; i++)
                    {
                       memcpy(pTemp  + i*w, ptr + i*pData->Pitch, w);
                    }

                    ptr	= pData->R + (pInfo->CropX ) + (pInfo->CropY ) * pData->Pitch;
                    pTemp = m_vpOutBuffers[nIndexVpOut] + 2*w*h;
                    for(int i = 0; i < h; i++)
                    {
                        memcpy(pTemp  + i*w, ptr + i*pData->Pitch, w);
                    }

                    m_mfxAllocator->Unlock(m_mfxAllocator->pthis, pSurface->Data.MemId, &(pSurface->Data));

                    VAData *vaData = VAData::Create(m_vpOutBuffers[nIndexVpOut], w, h, w, m_vpOutFormat);
                    vaData->SetExternalRef(&m_vpOutRefs[nIndexVpOut]); 
                    vaData->SetID(m_channel, nDecoded);
                    vaData->SetRef(m_vpRefNum);
                    outputPacket->push_back(vaData);
                }
                // Transcoding case: DEC->ENC:
                // ENC: required video memory output!
                if ((m_vpMemOutTypeVideo) && !(m_vpRefNum))
                {
                    VAData *vaData = VAData::Create(m_vpOutSurfaces[nIndexVpOut], m_mfxAllocator);
                    // Increasing Ref counter manually
                    m_vpOutRefs[nIndexVpOut]++;
                    vaData->SetExternalRef(&m_vpOutRefs[nIndexVpOut]);
                    //vaData->SetRef(1);
                    vaData->SetID(m_channel, nDecoded);
                    outputPacket->push_back(vaData);
                }
            }
        }
        EnqueueOutput(outputPacket);

        if (m_vpRatio && (nDecoded %m_vpRatio) == 0 && m_vpOutDump)
        {
            char filename[256];
            sprintf(filename, "VPOut_%d_%d.%dx%d.rgbp", m_channel, nDecoded, m_vpOutWidth, m_vpOutHeight);
            FILE *fp = fopen(filename, "wb");
            fwrite(m_vpOutBuffers[nIndexVpOut], 1, m_vpOutWidth * m_vpOutHeight * 3, fp);
            fclose(fp);
        }
    }
    return 0;
}

int DecodeThreadBlock::GetFreeSurface(mfxFrameSurface1 **surfaces, int *refs, uint32_t count)
{
    if (surfaces)
    {
        for (uint32_t i = 0; i < count; i ++)
        {
            int refNum = (refs == nullptr)?0:refs[i];
            if (surfaces[i]->Data.Locked == 0 && refNum == 0)
            {
                return i;
            }
        }
    }
    return MFX_ERR_NOT_FOUND;
}

