/*
 * nnie_detect.c - SSD 人形检测封装（S2-2）
 *
 * 数据流: NV21(320x320,VPSS chn2) --CPU转换--> BGR planar(300x300) --> NNIE Forward
 *         --> Ssd_GetResult(公共库) --> person 解析
 *
 * 依赖(公共库直接链接,无需拷贝):
 *   SAMPLE_COMM_SVP_NNIE_LoadModel / ParamInit / ParamDeinit  (sample_comm_nnie.c)
 *   SAMPLE_SVP_NNIE_Ssd_GetResult / Ssd_GetResultTmpBuf       (sample_svp_nnie_software.c)
 *   SAMPLE_COMM_SVP_MallocCached / FlushCache / MmzFree       (sample_comm_svp.c)
 * 拷贝自 sample_nnie.c(static,必须拷):
 *   SSD SoftwareInit 的参数配置 / 简化版 Forward
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "sample_svp_nnie_software.h"
#include "hi_common.h"
#include "hi_comm_region.h"
#include "mpi_region.h"

#define DET_MODEL_DEFAULT   "/mnt/nnie/inst_ssd_cycle.wk"
#define DET_NNIE_W          300
#define DET_NNIE_H          300
#define DET_SSD_CLASS_NUM   21
#define DET_PERSON_CLASS    15
#define DET_CONF_PRINT      0.50f

static SAMPLE_SVP_NNIE_MODEL_S              s_stDetModel = {0};
static SAMPLE_SVP_NNIE_PARAM_S              s_stDetParam = {0};
static SAMPLE_SVP_NNIE_SSD_SOFTWARE_PARAM_S s_stDetSoft  = {0};
static HI_BOOL                              s_bDetInit   = HI_FALSE;

/* 最近一次推理结果(模型输入 300x300 坐标系) */
typedef struct hiDET_BOX_S {
    HI_FLOAT f32Score;
    HI_S32   s32XMin, s32YMin, s32XMax, s32YMax;
} DET_BOX_S;
static DET_BOX_S s_astBoxes[8];
static HI_U32    s_u32BoxNum = 0;

/******************************************************************************
* SSD 软件参数初始化 —— 拷自 sample_nnie.c SAMPLE_SVP_NNIE_Ssd_SoftwareInit
* (该函数为 static,参数全部与 inst_ssd_cycle.wk 配套,一字未改)
******************************************************************************/
static HI_S32 DET_SoftwareInit(SAMPLE_SVP_NNIE_CFG_S* pstCfg,
    SAMPLE_SVP_NNIE_PARAM_S *pstNnieParam, SAMPLE_SVP_NNIE_SSD_SOFTWARE_PARAM_S* pstSoftWareParam)
{
    HI_U32 i = 0;
    HI_S32 s32Ret = HI_SUCCESS;
    HI_U32 u32ClassNum = 0;
    HI_U32 u32TotalSize = 0;
    HI_U32 u32DstRoiSize = 0;
    HI_U32 u32DstScoreSize = 0;
    HI_U32 u32ClassRoiNumSize = 0;
    HI_U32 u32TmpBufTotalSize = 0;
    HI_U64 u64PhyAddr = 0;
    HI_U8* pu8VirAddr = NULL;
    (void)pstCfg;

    for(i = 0; i < 12; i++)
    {
        pstSoftWareParam->au32ConvHeight[i] = pstNnieParam->pstModel->astSeg[0].astDstNode[i].unShape.stWhc.u32Chn;
        pstSoftWareParam->au32ConvWidth[i] = pstNnieParam->pstModel->astSeg[0].astDstNode[i].unShape.stWhc.u32Height;
        pstSoftWareParam->au32ConvChannel[i] = pstNnieParam->pstModel->astSeg[0].astDstNode[i].unShape.stWhc.u32Width;
        if(i%2==1)
        {
            pstSoftWareParam->au32ConvStride[i/2] = SAMPLE_SVP_NNIE_ALIGN16(pstSoftWareParam->au32ConvChannel[i]*sizeof(HI_U32))/sizeof(HI_U32);
        }
    }

    pstSoftWareParam->au32PriorBoxWidth[0] = 38;
    pstSoftWareParam->au32PriorBoxWidth[1] = 19;
    pstSoftWareParam->au32PriorBoxWidth[2] = 10;
    pstSoftWareParam->au32PriorBoxWidth[3] = 5;
    pstSoftWareParam->au32PriorBoxWidth[4] = 3;
    pstSoftWareParam->au32PriorBoxWidth[5] = 1;

    pstSoftWareParam->au32PriorBoxHeight[0] = 38;
    pstSoftWareParam->au32PriorBoxHeight[1] = 19;
    pstSoftWareParam->au32PriorBoxHeight[2] = 10;
    pstSoftWareParam->au32PriorBoxHeight[3] = 5;
    pstSoftWareParam->au32PriorBoxHeight[4] = 3;
    pstSoftWareParam->au32PriorBoxHeight[5] = 1;

    pstSoftWareParam->u32OriImHeight = pstNnieParam->astSegData[0].astSrc[0].unShape.stWhc.u32Height;
    pstSoftWareParam->u32OriImWidth = pstNnieParam->astSegData[0].astSrc[0].unShape.stWhc.u32Width;

    pstSoftWareParam->af32PriorBoxMinSize[0][0] = 30.0f;
    pstSoftWareParam->af32PriorBoxMinSize[1][0] = 60.0f;
    pstSoftWareParam->af32PriorBoxMinSize[2][0] = 111.0f;
    pstSoftWareParam->af32PriorBoxMinSize[3][0] = 162.0f;
    pstSoftWareParam->af32PriorBoxMinSize[4][0] = 213.0f;
    pstSoftWareParam->af32PriorBoxMinSize[5][0] = 264.0f;

    pstSoftWareParam->af32PriorBoxMaxSize[0][0] = 60.0f;
    pstSoftWareParam->af32PriorBoxMaxSize[1][0] = 111.0f;
    pstSoftWareParam->af32PriorBoxMaxSize[2][0] = 162.0f;
    pstSoftWareParam->af32PriorBoxMaxSize[3][0] = 213.0f;
    pstSoftWareParam->af32PriorBoxMaxSize[4][0] = 264.0f;
    pstSoftWareParam->af32PriorBoxMaxSize[5][0] = 315.0f;

    pstSoftWareParam->u32MinSizeNum = 1;
    pstSoftWareParam->u32MaxSizeNum = 1;
    pstSoftWareParam->bFlip= HI_TRUE;
    pstSoftWareParam->bClip= HI_FALSE;

    pstSoftWareParam->au32InputAspectRatioNum[0] = 1;
    pstSoftWareParam->au32InputAspectRatioNum[1] = 2;
    pstSoftWareParam->au32InputAspectRatioNum[2] = 2;
    pstSoftWareParam->au32InputAspectRatioNum[3] = 2;
    pstSoftWareParam->au32InputAspectRatioNum[4] = 1;
    pstSoftWareParam->au32InputAspectRatioNum[5] = 1;

    pstSoftWareParam->af32PriorBoxAspectRatio[0][0] = 2;
    pstSoftWareParam->af32PriorBoxAspectRatio[0][1] = 0;
    pstSoftWareParam->af32PriorBoxAspectRatio[1][0] = 2;
    pstSoftWareParam->af32PriorBoxAspectRatio[1][1] = 3;
    pstSoftWareParam->af32PriorBoxAspectRatio[2][0] = 2;
    pstSoftWareParam->af32PriorBoxAspectRatio[2][1] = 3;
    pstSoftWareParam->af32PriorBoxAspectRatio[3][0] = 2;
    pstSoftWareParam->af32PriorBoxAspectRatio[3][1] = 3;
    pstSoftWareParam->af32PriorBoxAspectRatio[4][0] = 2;
    pstSoftWareParam->af32PriorBoxAspectRatio[4][1] = 0;
    pstSoftWareParam->af32PriorBoxAspectRatio[5][0] = 2;
    pstSoftWareParam->af32PriorBoxAspectRatio[5][1] = 0;

    pstSoftWareParam->af32PriorBoxStepWidth[0] = 8;
    pstSoftWareParam->af32PriorBoxStepWidth[1] = 16;
    pstSoftWareParam->af32PriorBoxStepWidth[2] = 32;
    pstSoftWareParam->af32PriorBoxStepWidth[3] = 64;
    pstSoftWareParam->af32PriorBoxStepWidth[4] = 100;
    pstSoftWareParam->af32PriorBoxStepWidth[5] = 300;

    pstSoftWareParam->af32PriorBoxStepHeight[0] = 8;
    pstSoftWareParam->af32PriorBoxStepHeight[1] = 16;
    pstSoftWareParam->af32PriorBoxStepHeight[2] = 32;
    pstSoftWareParam->af32PriorBoxStepHeight[3] = 64;
    pstSoftWareParam->af32PriorBoxStepHeight[4] = 100;
    pstSoftWareParam->af32PriorBoxStepHeight[5] = 300;

    pstSoftWareParam->f32Offset = 0.5f;

    pstSoftWareParam->as32PriorBoxVar[0] = (HI_S32)(0.1f*SAMPLE_SVP_NNIE_QUANT_BASE);
    pstSoftWareParam->as32PriorBoxVar[1] = (HI_S32)(0.1f*SAMPLE_SVP_NNIE_QUANT_BASE);
    pstSoftWareParam->as32PriorBoxVar[2] = (HI_S32)(0.2f*SAMPLE_SVP_NNIE_QUANT_BASE);
    pstSoftWareParam->as32PriorBoxVar[3] = (HI_S32)(0.2f*SAMPLE_SVP_NNIE_QUANT_BASE);

    pstSoftWareParam->u32SoftMaxInHeight = 21;
    pstSoftWareParam->au32SoftMaxInChn[0] = 121296;
    pstSoftWareParam->au32SoftMaxInChn[1] = 45486;
    pstSoftWareParam->au32SoftMaxInChn[2] = 12600;
    pstSoftWareParam->au32SoftMaxInChn[3] = 3150;
    pstSoftWareParam->au32SoftMaxInChn[4] = 756;
    pstSoftWareParam->au32SoftMaxInChn[5] = 84;

    pstSoftWareParam->u32ConcatNum = 6;
    pstSoftWareParam->u32SoftMaxOutWidth = 1;
    pstSoftWareParam->u32SoftMaxOutHeight = 21;
    pstSoftWareParam->u32SoftMaxOutChn = 8732;

    pstSoftWareParam->u32ClassNum = 21;
    pstSoftWareParam->u32TopK = 400;
    pstSoftWareParam->u32KeepTopK = 200;
    pstSoftWareParam->u32NmsThresh = (HI_U32)(0.3f*SAMPLE_SVP_NNIE_QUANT_BASE);
    pstSoftWareParam->u32ConfThresh = (HI_U32)(0.000245f*SAMPLE_SVP_NNIE_QUANT_BASE);
    pstSoftWareParam->au32DetectInputChn[0] = 23104;
    pstSoftWareParam->au32DetectInputChn[1] = 8664;
    pstSoftWareParam->au32DetectInputChn[2] = 2400;
    pstSoftWareParam->au32DetectInputChn[3] = 600;
    pstSoftWareParam->au32DetectInputChn[4] = 144;
    pstSoftWareParam->au32DetectInputChn[5] = 16;

    u32ClassNum = pstSoftWareParam->u32ClassNum;
    u32TotalSize = SAMPLE_SVP_NNIE_Ssd_GetResultTmpBuf(pstNnieParam,pstSoftWareParam);
    u32DstRoiSize = SAMPLE_SVP_NNIE_ALIGN16(u32ClassNum*pstSoftWareParam->u32TopK*sizeof(HI_U32)*SAMPLE_SVP_NNIE_COORDI_NUM);
    u32DstScoreSize = SAMPLE_SVP_NNIE_ALIGN16(u32ClassNum*pstSoftWareParam->u32TopK*sizeof(HI_U32));
    u32ClassRoiNumSize = SAMPLE_SVP_NNIE_ALIGN16(u32ClassNum*sizeof(HI_U32));
    u32TotalSize = u32TotalSize+u32DstRoiSize+u32DstScoreSize+u32ClassRoiNumSize;
    s32Ret = SAMPLE_COMM_SVP_MallocCached("DET_SSD_INIT",NULL,(HI_U64*)&u64PhyAddr,
        (void**)&pu8VirAddr,u32TotalSize);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DET] soft init malloc %u failed:%#x\n", u32TotalSize, s32Ret);
        return s32Ret;
    }
    memset(pu8VirAddr,0, u32TotalSize);
    SAMPLE_COMM_SVP_FlushCache(u64PhyAddr,(void*)pu8VirAddr,u32TotalSize);

    /* Ssd_GetResultTmpBuf 已填好三个 u32Size 字段,这里只设地址 */
    pstSoftWareParam->stPriorBoxTmpBuf.u64PhyAddr = u64PhyAddr;
    pstSoftWareParam->stPriorBoxTmpBuf.u64VirAddr = (HI_U64)((HI_UL)pu8VirAddr);

    pstSoftWareParam->stSoftMaxTmpBuf.u64PhyAddr = u64PhyAddr+
        pstSoftWareParam->stPriorBoxTmpBuf.u32Size;
    pstSoftWareParam->stSoftMaxTmpBuf.u64VirAddr = (HI_U64)((HI_UL)pu8VirAddr+
        pstSoftWareParam->stPriorBoxTmpBuf.u32Size);

    pstSoftWareParam->stGetResultTmpBuf.u64PhyAddr = u64PhyAddr+
        pstSoftWareParam->stPriorBoxTmpBuf.u32Size+pstSoftWareParam->stSoftMaxTmpBuf.u32Size;
    pstSoftWareParam->stGetResultTmpBuf.u64VirAddr = (HI_U64)((HI_UL)pu8VirAddr+
        pstSoftWareParam->stPriorBoxTmpBuf.u32Size+ pstSoftWareParam->stSoftMaxTmpBuf.u32Size);

    u32TmpBufTotalSize = pstSoftWareParam->stPriorBoxTmpBuf.u32Size+
        pstSoftWareParam->stSoftMaxTmpBuf.u32Size + pstSoftWareParam->stGetResultTmpBuf.u32Size;

    pstSoftWareParam->stDstRoi.enType = SVP_BLOB_TYPE_S32;
    pstSoftWareParam->stDstRoi.u64PhyAddr = u64PhyAddr+u32TmpBufTotalSize;
    pstSoftWareParam->stDstRoi.u64VirAddr = (HI_U64)((HI_UL)pu8VirAddr+u32TmpBufTotalSize);
    pstSoftWareParam->stDstRoi.u32Stride = SAMPLE_SVP_NNIE_ALIGN16(u32ClassNum*
        pstSoftWareParam->u32TopK*sizeof(HI_U32)*SAMPLE_SVP_NNIE_COORDI_NUM);
    pstSoftWareParam->stDstRoi.u32Num = 1;
    pstSoftWareParam->stDstRoi.unShape.stWhc.u32Chn = 1;
    pstSoftWareParam->stDstRoi.unShape.stWhc.u32Height = 1;
    pstSoftWareParam->stDstRoi.unShape.stWhc.u32Width = u32ClassNum*
        pstSoftWareParam->u32TopK*SAMPLE_SVP_NNIE_COORDI_NUM;

    pstSoftWareParam->stDstScore.enType = SVP_BLOB_TYPE_S32;
    pstSoftWareParam->stDstScore.u64PhyAddr = u64PhyAddr+u32TmpBufTotalSize+u32DstRoiSize;
    pstSoftWareParam->stDstScore.u64VirAddr = (HI_U64)((HI_UL)pu8VirAddr+u32TmpBufTotalSize+u32DstRoiSize);
    pstSoftWareParam->stDstScore.u32Stride = SAMPLE_SVP_NNIE_ALIGN16(u32ClassNum*
        pstSoftWareParam->u32TopK*sizeof(HI_U32));
    pstSoftWareParam->stDstScore.u32Num = 1;
    pstSoftWareParam->stDstScore.unShape.stWhc.u32Chn = 1;
    pstSoftWareParam->stDstScore.unShape.stWhc.u32Height = 1;
    pstSoftWareParam->stDstScore.unShape.stWhc.u32Width = u32ClassNum*
        pstSoftWareParam->u32TopK;

    pstSoftWareParam->stClassRoiNum.enType = SVP_BLOB_TYPE_S32;
    pstSoftWareParam->stClassRoiNum.u64PhyAddr = u64PhyAddr+u32TmpBufTotalSize+
        u32DstRoiSize+u32DstScoreSize;
    pstSoftWareParam->stClassRoiNum.u64VirAddr = (HI_U64)((HI_UL)pu8VirAddr+u32TmpBufTotalSize+
        u32DstRoiSize+u32DstScoreSize);
    pstSoftWareParam->stClassRoiNum.u32Stride = SAMPLE_SVP_NNIE_ALIGN16(u32ClassNum*sizeof(HI_U32));
    pstSoftWareParam->stClassRoiNum.u32Num = 1;
    pstSoftWareParam->stClassRoiNum.unShape.stWhc.u32Chn = 1;
    pstSoftWareParam->stClassRoiNum.unShape.stWhc.u32Height = 1;
    pstSoftWareParam->stClassRoiNum.unShape.stWhc.u32Width = u32ClassNum;

    return s32Ret;
}

/******************************************************************************
* 简化版 Forward(仅 seg0) —— 拷自 sample_nnie.c SAMPLE_SVP_NNIE_Forward
* SSD 单段网络,去掉跨段 src 拼接与 PERF 统计
******************************************************************************/
static HI_S32 DET_ForwardSeg0(SAMPLE_SVP_NNIE_PARAM_S *pstNnieParam, HI_BOOL bInstant)
{
    HI_S32 s32Ret = HI_SUCCESS;
    HI_BOOL bFinish = HI_FALSE;
    SVP_NNIE_HANDLE hSvpNnieHandle = 0;
    HI_U32 i = 0;

    SAMPLE_COMM_SVP_FlushCache(pstNnieParam->astForwardCtrl[0].stTskBuf.u64PhyAddr,
        SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_VOID,pstNnieParam->astForwardCtrl[0].stTskBuf.u64VirAddr),
        pstNnieParam->astForwardCtrl[0].stTskBuf.u32Size);

    for(i = 0; i < pstNnieParam->astForwardCtrl[0].u32DstNum; i++)
    {
        if(SVP_BLOB_TYPE_SEQ_S32 != pstNnieParam->astSegData[0].astDst[i].enType)
        {
            SAMPLE_COMM_SVP_FlushCache(pstNnieParam->astSegData[0].astDst[i].u64PhyAddr,
                SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_VOID,pstNnieParam->astSegData[0].astDst[i].u64VirAddr),
                pstNnieParam->astSegData[0].astDst[i].u32Num*
                pstNnieParam->astSegData[0].astDst[i].unShape.stWhc.u32Chn*
                pstNnieParam->astSegData[0].astDst[i].unShape.stWhc.u32Height*
                pstNnieParam->astSegData[0].astDst[i].u32Stride);
        }
    }

    s32Ret = HI_MPI_SVP_NNIE_Forward(&hSvpNnieHandle,
        pstNnieParam->astSegData[0].astSrc,
        pstNnieParam->pstModel, pstNnieParam->astSegData[0].astDst,
        &pstNnieParam->astForwardCtrl[0], bInstant);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DET] HI_MPI_SVP_NNIE_Forward failed:%#x\n", s32Ret);
        return s32Ret;
    }

    if(bInstant)
    {
        while(HI_ERR_SVP_NNIE_QUERY_TIMEOUT == (s32Ret = HI_MPI_SVP_NNIE_Query(
            pstNnieParam->astForwardCtrl[0].enNnieId, hSvpNnieHandle, &bFinish, HI_TRUE)))
        {
            usleep(100);
        }
        if (HI_SUCCESS != s32Ret)
        {
            printf("[DET] NNIE query failed:%#x\n", s32Ret);
            return s32Ret;
        }
    }

    for(i = 0; i < pstNnieParam->astForwardCtrl[0].u32DstNum; i++)
    {
        if(SVP_BLOB_TYPE_SEQ_S32 != pstNnieParam->astSegData[0].astDst[i].enType)
        {
            SAMPLE_COMM_SVP_FlushCache(pstNnieParam->astSegData[0].astDst[i].u64PhyAddr,
                SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_VOID,pstNnieParam->astSegData[0].astDst[i].u64VirAddr),
                pstNnieParam->astSegData[0].astDst[i].u32Num*
                pstNnieParam->astSegData[0].astDst[i].unShape.stWhc.u32Chn*
                pstNnieParam->astSegData[0].astDst[i].unShape.stWhc.u32Height*
                pstNnieParam->astSegData[0].astDst[i].u32Stride);
        }
    }

    return HI_SUCCESS;
}

/******************************************************************************
* NV21(Y+VU) -> BGR planar 300x300
* BT.601 limited range;最近邻缩放;demo 图片为 .bgr 即 plane 顺序 B,G,R
******************************************************************************/
static HI_VOID DET_Nv21ToBgrPlanar(const HI_U8 *pu8Yuv, HI_U32 u32W, HI_U32 u32H,
    HI_U32 u32Stride, HI_U8 *pu8Dst, HI_U32 u32DstStride)
{
    const HI_U8 *pu8Y  = pu8Yuv;
    const HI_U8 *pu8VU = pu8Yuv + (HI_U64)u32Stride * u32H;
    HI_U32 dx, dy;
    HI_U8 *pB = pu8Dst;
    HI_U8 *pG = pu8Dst + (HI_U64)u32DstStride * DET_NNIE_H;
    HI_U8 *pR = pu8Dst + (HI_U64)u32DstStride * DET_NNIE_H * 2;

    for (dy = 0; dy < DET_NNIE_H; dy++)
    {
        HI_U32 y = dy * u32H / DET_NNIE_H;
        const HI_U8 *pYLine = pu8Y + (HI_U64)y * u32Stride;
        const HI_U8 *pUVLine = pu8VU + (HI_U64)(y >> 1) * u32Stride;
        for (dx = 0; dx < DET_NNIE_W; dx++)
        {
            HI_U32 x = dx * u32W / DET_NNIE_W;
            HI_S32 Y = pYLine[x];
            HI_S32 U = pUVLine[(x & ~1) + 1] - 128;
            HI_S32 V = pUVLine[x & ~1] - 128;
            HI_S32 C = Y - 16;
            HI_S32 R = (298 * C + 409 * V + 128) >> 8;
            HI_S32 G = (298 * C - 100 * U - 208 * V + 128) >> 8;
            HI_S32 B = (298 * C + 516 * U + 128) >> 8;
            pB[dy * u32DstStride + dx] = (HI_U8)((B < 0) ? 0 : ((B > 255) ? 255 : B));
            pG[dy * u32DstStride + dx] = (HI_U8)((G < 0) ? 0 : ((G > 255) ? 255 : G));
            pR[dy * u32DstStride + dx] = (HI_U8)((R < 0) ? 0 : ((R > 255) ? 255 : R));
        }
    }
}

/******************************************************************************
* 对外接口
******************************************************************************/
HI_S32 nnie_detect_init(const HI_CHAR *pszModelFile)
{
    HI_S32 s32Ret;
    SAMPLE_SVP_NNIE_CFG_S stCfg = {0};
    const HI_CHAR *pszModel = (pszModelFile != NULL) ? pszModelFile : DET_MODEL_DEFAULT;

    if (s_bDetInit)
    {
        return HI_SUCCESS;
    }

    stCfg.u32MaxInputNum = 1;
    stCfg.u32MaxRoiNum   = 0;
    stCfg.aenNnieCoreId[0] = SVP_NNIE_ID_0;
    stCfg.pszPic = NULL;

    s32Ret = SAMPLE_COMM_SVP_NNIE_LoadModel(pszModel, &s_stDetModel);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DET] LoadModel(%s) failed:%#x\n", pszModel, s32Ret);
        return s32Ret;
    }

    s_stDetParam.pstModel = &s_stDetModel.stModel;
    s32Ret = SAMPLE_COMM_SVP_NNIE_ParamInit(&stCfg, &s_stDetParam);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DET] ParamInit failed:%#x\n", s32Ret);
        goto FAIL_0;
    }

    s32Ret = DET_SoftwareInit(&stCfg, &s_stDetParam, &s_stDetSoft);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DET] SoftwareInit failed:%#x\n", s32Ret);
        goto FAIL_1;
    }

    s_bDetInit = HI_TRUE;
    printf("[DET] NNIE init ok: model=%s src=%ux%ux%u stride=%u\n", pszModel,
        s_stDetParam.astSegData[0].astSrc[0].unShape.stWhc.u32Width,
        s_stDetParam.astSegData[0].astSrc[0].unShape.stWhc.u32Height,
        s_stDetParam.astSegData[0].astSrc[0].unShape.stWhc.u32Chn,
        s_stDetParam.astSegData[0].astSrc[0].u32Stride);
    return HI_SUCCESS;

FAIL_1:
    SAMPLE_COMM_SVP_NNIE_ParamDeinit(&s_stDetParam);
FAIL_0:
    SAMPLE_COMM_SVP_NNIE_UnloadModel(&s_stDetModel);
    memset(&s_stDetModel, 0, sizeof(s_stDetModel));
    memset(&s_stDetParam, 0, sizeof(s_stDetParam));
    return s32Ret;
}

HI_VOID nnie_detect_exit(HI_VOID)
{
    if (!s_bDetInit)
    {
        return;
    }
    s_bDetInit = HI_FALSE;
    SAMPLE_COMM_SVP_NNIE_ParamDeinit(&s_stDetParam);
    SAMPLE_COMM_SVP_NNIE_UnloadModel(&s_stDetModel);
    SAMPLE_SVP_MMZ_FREE(s_stDetSoft.stPriorBoxTmpBuf.u64PhyAddr,
        s_stDetSoft.stPriorBoxTmpBuf.u64VirAddr);
    memset(&s_stDetModel, 0, sizeof(s_stDetModel));
    memset(&s_stDetParam, 0, sizeof(s_stDetParam));
    memset(&s_stDetSoft, 0, sizeof(s_stDetSoft));
}

HI_BOOL nnie_detect_ready(HI_VOID)
{
    return s_bDetInit;
}

/* 输入一帧 NV21,推理一次,更新 s_astBoxes */
HI_S32 nnie_detect_run(const HI_U8 *pu8Yuv, HI_U32 u32W, HI_U32 u32H, HI_U32 u32Stride)
{
    HI_S32 s32Ret;
    SVP_BLOB_S *pstSrc;
    HI_U8 *pu8Src;
    HI_U32 u32BlobStride, u32Chn, u32BlobH, i;
    HI_S32 *ps32Score, *ps32Roi, *ps32ClassRoiNum;
    HI_U32 u32RoiNumBias = 0;

    if (!s_bDetInit)
    {
        return HI_ERR_SVP_NNIE_ILLEGAL_PARAM;
    }

    /* 1. NV21 -> BGR planar 写入 src blob */
    pstSrc = &s_stDetParam.astSegData[0].astSrc[0];
    u32BlobStride = pstSrc->u32Stride;
    u32BlobH      = pstSrc->unShape.stWhc.u32Height;
    u32Chn        = pstSrc->unShape.stWhc.u32Chn;
    pu8Src        = SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_U8, pstSrc->u64VirAddr);
    DET_Nv21ToBgrPlanar(pu8Yuv, u32W, u32H, u32Stride, pu8Src, u32BlobStride);

    SAMPLE_COMM_SVP_FlushCache(pstSrc->u64PhyAddr, pu8Src,
        pstSrc->u32Num * u32Chn * u32BlobH * u32BlobStride);

    /* 2. 硬件推理 */
    s32Ret = DET_ForwardSeg0(&s_stDetParam, HI_TRUE);
    if (HI_SUCCESS != s32Ret)
    {
        return s32Ret;
    }

    /* 3. 软件后处理(公共库) */
    s32Ret = SAMPLE_SVP_NNIE_Ssd_GetResult(&s_stDetParam, &s_stDetSoft);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DET] Ssd_GetResult failed:%#x\n", s32Ret);
        return s32Ret;
    }

    /* 4. 解析 person(class 15) */
    ps32Score      = SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_S32, s_stDetSoft.stDstScore.u64VirAddr);
    ps32Roi        = SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_S32, s_stDetSoft.stDstRoi.u64VirAddr);
    ps32ClassRoiNum= SAMPLE_SVP_NNIE_CONVERT_64BIT_ADDR(HI_S32, s_stDetSoft.stClassRoiNum.u64VirAddr);

    s_u32BoxNum = 0;
    for (i = 0; i < DET_PERSON_CLASS; i++)
    {
        u32RoiNumBias += (HI_U32)ps32ClassRoiNum[i];
    }
    for (i = 0; i < (HI_U32)ps32ClassRoiNum[DET_PERSON_CLASS] && s_u32BoxNum < 8; i++)
    {
        HI_FLOAT f32Score = (HI_FLOAT)ps32Score[u32RoiNumBias + i] / SAMPLE_SVP_NNIE_QUANT_BASE;
        if (f32Score < DET_CONF_PRINT)
        {
            break;
        }
        s_astBoxes[s_u32BoxNum].f32Score = f32Score;
        s_astBoxes[s_u32BoxNum].s32XMin = ps32Roi[(u32RoiNumBias + i) * SAMPLE_SVP_NNIE_COORDI_NUM];
        s_astBoxes[s_u32BoxNum].s32YMin = ps32Roi[(u32RoiNumBias + i) * SAMPLE_SVP_NNIE_COORDI_NUM + 1];
        s_astBoxes[s_u32BoxNum].s32XMax = ps32Roi[(u32RoiNumBias + i) * SAMPLE_SVP_NNIE_COORDI_NUM + 2];
        s_astBoxes[s_u32BoxNum].s32YMax = ps32Roi[(u32RoiNumBias + i) * SAMPLE_SVP_NNIE_COORDI_NUM + 3];
        s_u32BoxNum++;
    }

    return HI_SUCCESS;
}

const DET_BOX_S *nnie_detect_boxes(HI_U32 *pu32Num)
{
    if (NULL != pu32Num)
    {
        *pu32Num = s_u32BoxNum;
    }
    return s_astBoxes;
}

/******************************************************************************
* S2-4: RGN 画框上屏
* 全屏 ARGB1555 overlay(1920x1080, canvas=2)挂 VENC 主通道,
* 每次推理后重绘画布: 全透明底 + 红色空心矩形(坐标 300x300 -> 1920x1080)
* 注意: 300 坐标是 chn2(320x320,由 1080p 直接拉伸而来)的等比映射,
*       反算 x_1080p = x_300 * 1920 / 300
******************************************************************************/
#define DRAW_HANDLE      1
#define DRAW_W           1920
#define DRAW_H           1080
#define DRAW_COLOR_RED   0xFC00     /* ARGB1555: A=1 R=11111 G=00000 B=00000 */
#define DRAW_THICK       4

static HI_BOOL s_bDrawInit  = HI_FALSE;
static HI_S32  s_s32DrawChn = 0;
static HI_U16 *s_pDrawBmp   = HI_NULL;

static HI_VOID DRAW_Rect(HI_U16 *pBmp, HI_S32 x1, HI_S32 y1,
    HI_S32 x2, HI_S32 y2, HI_U16 u16Color, HI_U32 u32Thick)
{
    HI_S32 x, y, t;

    if (x1 < 0)          x1 = 0;
    if (y1 < 0)          y1 = 0;
    if (x2 >= DRAW_W)    x2 = DRAW_W - 1;
    if (y2 >= DRAW_H)    y2 = DRAW_H - 1;
    if (x2 <= x1 || y2 <= y1)
    {
        return;
    }

    /* 上下边 */
    for (t = 0; t < (HI_S32)u32Thick; t++)
    {
        HI_S32 s32Top = y1 + t;
        HI_S32 s32Bot = y2 - t;
        if (s32Top > s32Bot)
        {
            break;
        }
        for (x = x1; x <= x2; x++)
        {
            pBmp[s32Top * DRAW_W + x] = u16Color;
            if (s32Bot != s32Top)
            {
                pBmp[s32Bot * DRAW_W + x] = u16Color;
            }
        }
    }
    /* 左右边 */
    for (t = 0; t < (HI_S32)u32Thick; t++)
    {
        HI_S32 s32L = x1 + t;
        HI_S32 s32R = x2 - t;
        if (s32L > s32R)
        {
            break;
        }
        for (y = y1; y <= y2; y++)
        {
            pBmp[y * DRAW_W + s32L] = u16Color;
            if (s32R != s32L)
            {
                pBmp[y * DRAW_W + s32R] = u16Color;
            }
        }
    }
}

HI_S32 nnie_draw_init(HI_S32 s32VencChn)
{
    HI_S32         s32Ret;
    RGN_ATTR_S     stRgnAttr;
    RGN_CHN_ATTR_S stChnAttr;
    MPP_CHN_S      stChn;
    BITMAP_S       stBitmap;

    if (s_bDrawInit)
    {
        return HI_SUCCESS;
    }

    memset(&stRgnAttr, 0, sizeof(stRgnAttr));
    stRgnAttr.enType                              = OVERLAY_RGN;
    stRgnAttr.unAttr.stOverlay.enPixelFmt         = PIXEL_FORMAT_ARGB_1555;
    stRgnAttr.unAttr.stOverlay.stSize.u32Width    = DRAW_W;
    stRgnAttr.unAttr.stOverlay.stSize.u32Height   = DRAW_H;
    stRgnAttr.unAttr.stOverlay.u32BgColor         = 0x000000;
    stRgnAttr.unAttr.stOverlay.u32CanvasNum       = 2;

    s32Ret = HI_MPI_RGN_Create(DRAW_HANDLE, &stRgnAttr);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DRAW] RGN_Create failed:%#x\n", s32Ret);
        return s32Ret;
    }

    s_pDrawBmp = (HI_U16 *)malloc(DRAW_W * DRAW_H * sizeof(HI_U16));
    if (HI_NULL == s_pDrawBmp)
    {
        printf("[DRAW] malloc bitmap failed\n");
        HI_MPI_RGN_Destroy(DRAW_HANDLE);
        return HI_FAILURE;
    }
    memset(s_pDrawBmp, 0, DRAW_W * DRAW_H * sizeof(HI_U16));

    memset(&stBitmap, 0, sizeof(stBitmap));
    stBitmap.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    stBitmap.u32Width      = DRAW_W;
    stBitmap.u32Height     = DRAW_H;
    stBitmap.pData         = s_pDrawBmp;
    s32Ret = HI_MPI_RGN_SetBitMap(DRAW_HANDLE, &stBitmap);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DRAW] first SetBitMap failed:%#x\n", s32Ret);
        free(s_pDrawBmp);
        s_pDrawBmp = HI_NULL;
        HI_MPI_RGN_Destroy(DRAW_HANDLE);
        return s32Ret;
    }

    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.bShow                                 = HI_TRUE;
    stChnAttr.enType                                = OVERLAY_RGN;
    stChnAttr.unChnAttr.stOverlayChn.stPoint.s32X   = 0;
    stChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y   = 0;
    stChnAttr.unChnAttr.stOverlayChn.u32FgAlpha     = 128;   /* A=1: 不透明红框 */
    stChnAttr.unChnAttr.stOverlayChn.u32BgAlpha     = 0;     /* A=0: 全透明底   */
    stChnAttr.unChnAttr.stOverlayChn.u32Layer       = 1;     /* 时间水印是 0 层 */
    stChnAttr.unChnAttr.stOverlayChn.stQpInfo.bAbsQp       = HI_FALSE;
    stChnAttr.unChnAttr.stOverlayChn.stQpInfo.bQpDisable   = HI_FALSE;
    stChnAttr.unChnAttr.stOverlayChn.stInvertColor.bInvColEn = HI_FALSE;

    stChn.enModId  = HI_ID_VENC;
    stChn.s32DevId = 0;
    stChn.s32ChnId = s32VencChn;

    s32Ret = HI_MPI_RGN_AttachToChn(DRAW_HANDLE, &stChn, &stChnAttr);
    if (HI_SUCCESS != s32Ret)
    {
        printf("[DRAW] AttachToChn(chn%d) failed:%#x\n", s32VencChn, s32Ret);
        free(s_pDrawBmp);
        s_pDrawBmp = HI_NULL;
        HI_MPI_RGN_Destroy(DRAW_HANDLE);
        return s32Ret;
    }

    s_s32DrawChn = s32VencChn;
    s_bDrawInit  = HI_TRUE;
    printf("[DRAW] overlay %dx%d attach VENC chn%d ok\n", DRAW_W, DRAW_H, s32VencChn);
    return HI_SUCCESS;
}

HI_VOID nnie_draw_update(HI_VOID)
{
    BITMAP_S          stBitmap;
    HI_U32            i, u32Num = 0;
    HI_U32            u32FailCnt = 0;
    const DET_BOX_S  *pstBoxes;

    if (!s_bDrawInit || HI_NULL == s_pDrawBmp)
    {
        return;
    }

    memset(s_pDrawBmp, 0, DRAW_W * DRAW_H * sizeof(HI_U16));
    pstBoxes = nnie_detect_boxes(&u32Num);
    for (i = 0; i < u32Num; i++)
    {
        HI_S32 x1 = pstBoxes[i].s32XMin * (HI_S32)DRAW_W / (HI_S32)DET_NNIE_W;
        HI_S32 y1 = pstBoxes[i].s32YMin * (HI_S32)DRAW_H / (HI_S32)DET_NNIE_H;
        HI_S32 x2 = pstBoxes[i].s32XMax * (HI_S32)DRAW_W / (HI_S32)DET_NNIE_W;
        HI_S32 y2 = pstBoxes[i].s32YMax * (HI_S32)DRAW_H / (HI_S32)DET_NNIE_H;
        DRAW_Rect(s_pDrawBmp, x1, y1, x2, y2, DRAW_COLOR_RED, DRAW_THICK);
    }

    memset(&stBitmap, 0, sizeof(stBitmap));
    stBitmap.enPixelFormat = PIXEL_FORMAT_ARGB_1555;
    stBitmap.u32Width      = DRAW_W;
    stBitmap.u32Height     = DRAW_H;
    stBitmap.pData         = s_pDrawBmp;
    if (HI_SUCCESS != HI_MPI_RGN_SetBitMap(DRAW_HANDLE, &stBitmap))
    {
        if (u32FailCnt < 3)
        {
            printf("[DRAW] SetBitMap failed\n");
            u32FailCnt++;
        }
    }
}

HI_VOID nnie_draw_exit(HI_VOID)
{
    MPP_CHN_S stChn;

    if (!s_bDrawInit)
    {
        return;
    }
    s_bDrawInit = HI_FALSE;

    stChn.enModId  = HI_ID_VENC;
    stChn.s32DevId = 0;
    stChn.s32ChnId = s_s32DrawChn;
    HI_MPI_RGN_DetachFromChn(DRAW_HANDLE, &stChn);
    HI_MPI_RGN_Destroy(DRAW_HANDLE);

    if (HI_NULL != s_pDrawBmp)
    {
        free(s_pDrawBmp);
        s_pDrawBmp = HI_NULL;
    }
    printf("[DRAW] overlay released\n");
}
