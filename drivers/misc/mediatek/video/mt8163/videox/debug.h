/*
 * Copyright (C) 2018 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __MTKFB_DEBUG_H
#define __MTKFB_DEBUG_H

#include "ddp_ovl.h"
#include "disp_drv_platform.h"
#include "lcm_drv.h"
#include "primary_display.h"
#include <linux/wait.h>
#include <mmprofile.h>

extern unsigned int EnableVSyncLog;
/* ---------------------------------------------------------------------------
 */
/* External variable declarations */
/* ---------------------------------------------------------------------------
 */
extern struct LCM_DRIVER *lcm_drv;
extern struct OVL_CONFIG_STRUCT cached_layer_config[DDP_OVL_LAYER_MUN];

extern unsigned char DSCRead(uint8_t cmd);
extern long tpd_last_down_time;
extern int tpd_start_profiling;
extern void mtkfb_log_enable(int enable);
extern void disp_log_enable(int enable);
extern void mtkfb_vsync_log_enable(int enable);
extern void mtkfb_capture_fb_only(bool enable);
extern void esd_recovery_pause(bool en);
extern int mtkfb_set_backlight_mode(unsigned int mode);
extern void mtkfb_pan_disp_test(void);
extern void mtkfb_show_sem_cnt(void);
extern void mtkfb_hang_test(bool en);
extern void mtkfb_switch_normal_to_factory(void);
extern void mtkfb_switch_factory_to_normal(void);
extern int mtkfb_get_debug_state(char *stringbuf, int buf_len);
extern unsigned int mtkfb_fm_auto_test(void);
extern int DAL_Clean(void);
extern int DAL_Printf(const char *fmt, ...);
extern void DSI_ChangeClk(enum DISP_MODULE_ENUM module, uint32_t clk);
extern void smp_inner_dcache_flush_all(void);
extern void mtkfb_clear_lcm(void);
extern void hdmi_force_init(void);
extern int DSI_BIST_Pattern_Test(enum DISP_MODULE_ENUM module,
				 struct cmdqRecStruct *cmdq, bool enable,
				 unsigned int color);

extern unsigned int gCaptureLayerEnable;
extern unsigned int gCaptureLayerDownX;
extern unsigned int gCaptureLayerDownY;
extern unsigned int gCaptureOvlThreadEnable;
extern unsigned int gCaptureOvlDownX;
extern unsigned int gCaptureOvlDownY;
extern struct task_struct *captureovl_task;
extern int mtkfb_fence_get_debug_info(int buf_len, unsigned char *stringbuf);

extern unsigned int gCaptureFBEnable;
extern unsigned int gCaptureFBDownX;
extern unsigned int gCaptureFBDownY;
extern unsigned int gCaptureFBPeriod;
extern struct task_struct *capturefb_task;
extern wait_queue_head_t gCaptureFBWQ;

extern unsigned int gCapturePriLayerEnable;
extern unsigned int gCaptureWdmaLayerEnable;
extern unsigned int gCapturePriLayerDownX;
extern unsigned int gCapturePriLayerDownY;
extern unsigned int gCapturePriLayerNum;

#ifdef MTKFB_DEBUG_FS_CAPTURE_LAYER_CONTENT_SUPPORT
extern struct OVL_CONFIG_STRUCT cached_layer_config[DDP_OVL_LAYER_MUN];
#endif

void DBG_Init(void);
void DBG_Deinit(void);

void DBG_OnTriggerLcd(void);
void DBG_OnTeDelayDone(void);
void DBG_OnLcdDone(void);

#include "mmprofile.h"
extern struct MTKFB_MMP_Events_t {
	MMP_Event MTKFB;
	MMP_Event CreateSyncTimeline;
	MMP_Event PanDisplay;
	MMP_Event SetOverlayLayer;
	MMP_Event SetOverlayLayers;
	MMP_Event SetMultipleLayers;
	MMP_Event CreateSyncFence;
	MMP_Event IncSyncTimeline;
	MMP_Event SignalSyncFence;
	MMP_Event TrigOverlayOut;
	MMP_Event UpdateScreenImpl;
	MMP_Event VSync;
	MMP_Event UpdateConfig;
	MMP_Event ConfigOVL;
	MMP_Event ConfigAAL;
	MMP_Event ConfigMemOut;
	MMP_Event ScreenUpdate;
	MMP_Event CaptureFramebuffer;
	MMP_Event RegUpdate;
	MMP_Event EarlySuspend;
	MMP_Event DispDone;
	MMP_Event DSICmd;
	MMP_Event DSIIRQ;
	MMP_Event EsdCheck;
	MMP_Event WaitVSync;
	MMP_Event LayerDump;
	MMP_Event Layer[4];
	MMP_Event OvlDump;
	MMP_Event FBDump;
	MMP_Event DSIRead;
	MMP_Event GetLayerInfo;
	MMP_Event LayerInfo[4];
	MMP_Event IOCtrl;
	MMP_Event Debug;
} MTKFB_MMP_Events;

#ifdef MTKFB_DBG
#include "disp_drv_log.h"

#define DBG_BUF_SIZE 2048
#define MAX_DBG_INDENT_LEVEL 5
#define DBG_INDENT_SIZE 3
#define MAX_DBG_MESSAGES 0

static int dbg_indent;
static int dbg_cnt;
static char dbg_buf[DBG_BUF_SIZE];
static spinlock_t dbg_spinlock = SPIN_LOCK_UNLOCKED;

static inline void dbg_print(int level, const char *fmt, ...)
{
	if (level <= MTKFB_DBG) {
		if (!MAX_DBG_MESSAGES || dbg_cnt < MAX_DBG_MESSAGES) {
			va_list args;
			int ind = dbg_indent;
			unsigned long flags;

			spin_lock_irqsave(&dbg_spinlock, flags);
			dbg_cnt++;
			if (ind > MAX_DBG_INDENT_LEVEL)
				ind = MAX_DBG_INDENT_LEVEL;

			pr_info("DISP/DBG %*s", ind * DBG_INDENT_SIZE, "");
			va_start(args, fmt);
			vsnprintf(dbg_buf, sizeof(dbg_buf), fmt, args);
			pr_info("DISP/DBG " dbg_buf);
			va_end(args);
			spin_unlock_irqrestore(&dbg_spinlock, flags);
		}
	}
}

#define DBGPRINT dbg_print

#define DBGENTER(level)                                                        \
	do {                                                                   \
		dbg_print(level, "%s: Enter\n", __func__);                     \
		dbg_indent++;                                                  \
	} while (0)

#define DBGLEAVE(level)                                                        \
	do {                                                                   \
		dbg_indent--;                                                  \
		dbg_print(level, "%s: Leave\n", __func__);                     \
	} while (0)

/* Debug Macros */

#define MTKFB_DBG_EVT_NONE 0x00000000
#define MTKFB_DBG_EVT_FUNC 0x00000001 /* Function Entry     */
#define MTKFB_DBG_EVT_ARGU 0x00000002 /* Function Arguments */
#define MTKFB_DBG_EVT_INFO 0x00000003 /* Information        */

#define MTKFB_DBG_EVT_MASK (MTKFB_DBG_EVT_NONE)

#define MSG(evt, fmt, args...)                                                 \
	do {                                                                   \
		if ((MTKFB_DBG_EVT_##evt) & MTKFB_DBG_EVT_MASK) {              \
			pr_info("DISP/DBG " fmt, ##args);                      \
		}                                                              \
	} while (0)

#define MSG_FUNC_ENTER(f) MSG(FUNC, "<FB_ENTER>: %s\n", __func__)
#define MSG_FUNC_LEAVE(f) MSG(FUNC, "<FB_LEAVE>: %s\n", __func__)

#else /* MTKFB_DBG */

#define DBGPRINT(level, format, ...)
#define DBGENTER(level)
#define DBGLEAVE(level)

/* Debug Macros */

#define MSG(evt, fmt, args...)
#define MSG_FUNC_ENTER()
#define MSG_FUNC_LEAVE()
void _debug_pattern(unsigned int mva, unsigned long va, unsigned int w,
		    unsigned int h, unsigned int linepitch, unsigned int color,
		    unsigned int layerid, unsigned int bufidx);
void _debug_fps_meter(unsigned int mva, unsigned long va, unsigned int w,
		      unsigned int h, unsigned int linepitch,
		      unsigned int color, unsigned int layerid,
		      unsigned int bufidx);

bool get_ovl1_to_mem_on(void);

#endif /* MTKFB_DBG */

#endif /* __MTKFB_DEBUG_H */
