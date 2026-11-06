#ifndef __NNIE_DETECT_H__
#define __NNIE_DETECT_H__

#include "hi_type.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hiDET_BOX_S {
    HI_FLOAT f32Score;
    HI_S32   s32XMin, s32YMin, s32XMax, s32YMax;  /* 模型输入 300x300 坐标系 */
} DET_BOX_S;

HI_S32 nnie_detect_init(const HI_CHAR *pszModelFile);
HI_VOID nnie_detect_exit(HI_VOID);
HI_BOOL nnie_detect_ready(HI_VOID);
HI_S32  nnie_detect_run(const HI_U8 *pu8Yuv, HI_U32 u32W, HI_U32 u32H, HI_U32 u32Stride);
const DET_BOX_S *nnie_detect_boxes(HI_U32 *pu32Num);

/* S2-4: RGN 画框上屏(挂 VENC 通道, ARGB1555 全屏 overlay) */
HI_S32  nnie_draw_init(HI_S32 s32VencChn);
HI_VOID nnie_draw_update(HI_VOID);
HI_VOID nnie_draw_exit(HI_VOID);

#ifdef __cplusplus
}
#endif

#endif
