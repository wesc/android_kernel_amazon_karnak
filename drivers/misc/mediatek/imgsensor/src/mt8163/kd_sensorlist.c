/*
 * Copyright (C) 2015 MediaTek Inc.
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

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/atomic.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>	/* proc file use */
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <sync_write.h>
#include <linux/types.h>
#include <linux/iio/consumer.h>
#include "kd_camera_hw.h"
#include "kd_camera_typedef.h"

#define MTKCAM_USING_CCF
#ifdef MTKCAM_USING_CCF
#include <linux/clk.h>
#else
#error " MTKCAM_USING_CCF is not defined"
#include <mach/mt_clkmgr.h>
#endif

#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_camera_feature.h"
#include "kd_imgsensor_errcode.h"
#include "kd_sensorlist.h"

#undef CONFIG_MTK_LEGACY
#ifdef CONFIG_OF
/* device tree */
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#endif
/* #define CONFIG_COMPAT */
#ifdef CONFIG_COMPAT
/* 64 bit */
#include <linux/fs.h>
#include <linux/compat.h>
#endif

/*K.S. kernel standard*/
#if !defined(CONFIG_MTK_LEGACY)
#include <linux/regulator/consumer.h>
#endif /* !defined(CONFIG_MTK_LEGACY) */

/* Camera information */
#define PROC_CAMERA_INFO "driver/camera_info"
#define camera_info_size 128
#define PDAF_DATA_SIZE 4096
char mtk_ccm_name[camera_info_size] = { 0 };

static unsigned int gDrvIndex;

static DEFINE_SPINLOCK(kdsensor_drv_lock);

static struct iio_channel *g_adc_id_iio_channel;
static struct platform_device *adc_pdev;

unsigned long TFlashSwitchValue;

#ifndef SUPPORT_I2C_BUS_NUM1
#define SUPPORT_I2C_BUS_NUM1	0
#endif
#ifndef SUPPORT_I2C_BUS_NUM2
#if defined(CONFIG_CAMERA_MULTIMODAL)
#define SUPPORT_I2C_BUS_NUM2	0
#else
#define SUPPORT_I2C_BUS_NUM2	2
#endif
#endif


#define CAMERA_HW_DRVNAME1  "kd_camera_hw"
#define CAMERA_HW_DRVNAME2  "kd_camera_hw_bus2"

#if defined(CONFIG_MTK_LEGACY)
static struct i2c_board_info i2c_devs1 __initdata = {
	I2C_BOARD_INFO(CAMERA_HW_DRVNAME1, 0xfe>>1)
};
static struct i2c_board_info i2c_devs2 __initdata = {
	I2C_BOARD_INFO(CAMERA_HW_DRVNAME2, 0xfe>>1)
};
#endif

/* Camera Power Regulator Framework */
#define MTKCAM_USING_PWRREG
#ifdef MTKCAM_USING_PWRREG

struct cam_power {
	struct regulator *vcama;
	struct regulator *vcamd;
	struct regulator *vcamio;
	struct regulator *vcamaf;
	struct regulator *vcami2c;
};
struct cam_power g_cam[2];

#endif


/* Common Clock Framework (CCF) */
#ifdef MTKCAM_USING_CCF
struct clk *g_camclk_camtg_sel;
struct clk *g_camclk_univpll_d26;
struct clk *g_camclk_univpll2_d2;
#endif

struct device *sensor_device;

#define SENSOR_WR32(addr, data)	mt65xx_reg_sync_writel(data, addr)
/* #define SENSOR_WR32(addr, data)	iowrite32(data, addr) */
#define SENSOR_RD32(addr)	  ioread32(addr)
/*****************************************************************/
/* Debug configuration */
/*****************************************************************/
#define PFX "[kd_sensorlist]"
#define PK_DBG_NONE(fmt, arg...)    do {} while (0)
#define PK_DBG_FUNC(fmt, arg...)    pr_debug(PFX "[%s] " fmt, __func__, ##arg)
#define PK_INFO(fmt, arg...)    pr_debug(PFX " [%s] " fmt, __func__, ##arg)

#undef DEBUG_CAMERA_HW_K
//#define DEBUG_CAMERA_HW_K
#ifdef DEBUG_CAMERA_HW_K
#define PK_DBG    PK_DBG_FUNC
#define PK_ERR(fmt, arg...)         pr_debug(PFX "[%s] " fmt, __func__, ##arg)
#define PK_XLOG_INFO(fmt, args...) \
	pr_debug(PFX "[%s] " fmt, __func__, ##args)
#else
#define PK_DBG(fmt, arg...)
#define PK_ERR(fmt, arg...)             pr_debug(PFX "[%s] " fmt, __func__, ##arg)
#define PK_XLOG_INFO(fmt, args...)

#endif
/*****************************************************************/
/* Proifling */
/*****************************************************************/
#define PROFILE 1
#if PROFILE
static struct timeval tv1, tv2;
inline void KD_IMGSENSOR_PROFILE_INIT(void)
{
	do_gettimeofday(&tv1);
}

inline void KD_IMGSENSOR_PROFILE(char *tag)
{
	unsigned long TimeIntervalUS;

	spin_lock(&kdsensor_drv_lock);

	do_gettimeofday(&tv2);
	TimeIntervalUS = (tv2.tv_sec - tv1.tv_sec) * 1000000 +
		(tv2.tv_usec - tv1.tv_usec);
	tv1 = tv2;

	spin_unlock(&kdsensor_drv_lock);
	PK_DBG("[%s]Profile = %lu\n", tag, TimeIntervalUS);
}
#else
static inline void KD_IMGSENSOR_PROFILE_INIT(void)
{
}

static inline void KD_IMGSENSOR_PROFILE(char *tag)
{
}
#endif

/*****************************************************************************
 *
 *****************************************************************************/

static struct platform_device camerahw_platform_device = {
	.name = "image_sensor",
	.id = 0,
	.dev = {
		.coherent_dma_mask = DMA_BIT_MASK(32),
		}
};

static struct i2c_client *g_pstI2Cclient;
static struct i2c_client *g_pstI2Cclient2;

/* 81 is used for V4L driver */
static dev_t g_CAMERA_HWdevno = MKDEV(250, 0);
static dev_t g_CAMERA_HWdevno2;
static struct cdev *g_pCAMERA_HW_CharDrv;
static struct cdev *g_pCAMERA_HW_CharDrv2;
static struct class *sensor_class;
static struct class *sensor2_class;

static atomic_t g_CamHWOpend;
static atomic_t g_CamHWOpend2;
static atomic_t g_CamHWOpening;
static atomic_t g_CamDrvOpenCnt;
static atomic_t g_CamDrvOpenCnt2;

/* static u32 gCurrI2CBusEnableFlag = 0; */
static u32 gI2CBusNum = SUPPORT_I2C_BUS_NUM1;

#define SET_I2CBUS_FLAG(_x_)	((1<<_x_)|(gCurrI2CBusEnableFlag))
#define CLEAN_I2CBUS_FLAG(_x_)	((~(1<<_x_))&(gCurrI2CBusEnableFlag))

static DEFINE_MUTEX(kdCam_Mutex);
static BOOL bSesnorVsyncFlag = FALSE;
static struct ACDK_KD_SENSOR_SYNC_STRUCT g_NewSensorExpGain = {
	128, 128, 128, 128, 1000, 640, 0xFF, 0xFF, 0xFF, 0 };

struct MULTI_SENSOR_FUNCTION_STRUCT2 kd_MultiSensorFunc;
static struct MULTI_SENSOR_FUNCTION_STRUCT2 *g_pSensorFunc =
	&kd_MultiSensorFunc;
BOOL g_bEnableDriver[KDIMGSENSOR_MAX_INVOKE_DRIVERS] = { FALSE, FALSE };
struct SENSOR_FUNCTION_STRUCT
	*g_pInvokeSensorFunc[KDIMGSENSOR_MAX_INVOKE_DRIVERS] = { NULL, NULL };
enum CAMERA_DUAL_CAMERA_SENSOR_ENUM
	g_invokeSocketIdx[KDIMGSENSOR_MAX_INVOKE_DRIVERS] = {
	DUAL_CAMERA_NONE_SENSOR, DUAL_CAMERA_NONE_SENSOR };
char g_invokeSensorNameStr[KDIMGSENSOR_MAX_INVOKE_DRIVERS][32] = {
	KDIMGSENSOR_NOSENSOR, KDIMGSENSOR_NOSENSOR };

/* static int g_SensorExistStatus[3]={0,0,0}; */
static wait_queue_head_t kd_sensor_wait_queue;
bool setExpGainDoneFlag;
static unsigned int g_CurrentSensorIdx;
static unsigned int g_IsSearchSensor;

static ssize_t TFlashSwitch_set(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t ret = -EINVAL;

	ret = kstrtoul(buf, 10, &TFlashSwitchValue);

	return count;
}

static ssize_t TFlashSwitch_get(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%ld", TFlashSwitchValue);
}

static DEVICE_ATTR(TFlashSwitch, 0644, TFlashSwitch_get, TFlashSwitch_set);

static struct attribute *dev_attrs[] = {
	&dev_attr_TFlashSwitch.attr,
	NULL
};

static struct attribute_group dev_attr_grp = {
	.attrs = dev_attrs,
};


static const struct i2c_device_id CAMERA_HW_i2c_id[] = {
	{CAMERA_HW_DRVNAME1, 0}, {} };
static const struct i2c_device_id CAMERA_HW_i2c_id2[] = {
	{CAMERA_HW_DRVNAME2, 0}, {} };

MUINT32 kdGetSensorInitFuncList(
	struct ACDK_KD_SENSOR_INIT_FUNCTION_STRUCT **ppSensorList)
{
	if (ppSensorList == NULL) {
		PK_ERR("[kdGetSensorInitFuncList]ERROR: NULL ppSensorList\n");
		return 1;
	}
	*ppSensorList = &kdSensorList[0];
	return 0;
}				/* kdGetSensorInitFuncList() */


int iMultiReadReg(u16 a_u2Addr, u8 *a_puBuff, u16 i2cId, u8 number)
{
	int i4RetValue = 0;
	char puReadCmd[2] = { (char)(a_u2Addr >> 8), (char)(a_u2Addr & 0xFF) };

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		spin_lock(&kdsensor_drv_lock);

		g_pstI2Cclient->addr = (i2cId >> 1);

		spin_unlock(&kdsensor_drv_lock);

		/*  */
		i4RetValue = i2c_master_send(g_pstI2Cclient, puReadCmd, 2);
		if (i4RetValue != 2) {
			PK_ERR("I2C send failed, addr = 0x%x, data = 0x%x!\n",
				a_u2Addr, *a_puBuff);
			return -1;
		}
		/*  */
		i4RetValue = i2c_master_recv(g_pstI2Cclient,
			(char *)a_puBuff, number);
		if (i4RetValue != 1) {
			PK_ERR("[CAMERA SENSOR] I2C read failed!!\n");
			return -1;
		}
	} else {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->addr = (i2cId >> 1);
		spin_unlock(&kdsensor_drv_lock);
		/*  */
		i4RetValue = i2c_master_send(g_pstI2Cclient2, puReadCmd, 2);
		if (i4RetValue != 2) {
			PK_ERR("I2C send failed, addr = 0x%x, data = 0x%x!\n",
				a_u2Addr, *a_puBuff);
			return -1;
		}
		/*  */
		i4RetValue = i2c_master_recv(g_pstI2Cclient2,
			(char *)a_puBuff, number);
		if (i4RetValue != 1) {
			PK_ERR("[CAMERA SENSOR] I2C read failed!!\n");
			return -1;
		}
	}
	return 0;
}


int iReadReg(u16 a_u2Addr, u8 *a_puBuff, u16 i2cId)
{
	int i4RetValue = 0;
	char puReadCmd[2] = { (char)(a_u2Addr >> 8), (char)(a_u2Addr & 0xFF) };

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		spin_lock(&kdsensor_drv_lock);

		g_pstI2Cclient->addr = (i2cId >> 1);

		spin_unlock(&kdsensor_drv_lock);

		/*  */
		i4RetValue = i2c_master_send(g_pstI2Cclient, puReadCmd, 2);
		if (i4RetValue != 2) {
			PK_ERR("I2C send failed, addr = 0x%x, data = 0x%x!\n",
				a_u2Addr, *a_puBuff);
			return -1;
		}
		/*  */
		i4RetValue = i2c_master_recv(g_pstI2Cclient,
			(char *)a_puBuff, 1);
		if (i4RetValue != 1) {
			PK_ERR("[CAMERA SENSOR] I2C read failed!!\n");
			return -1;
		}
	} else {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->addr = (i2cId >> 1);

		spin_unlock(&kdsensor_drv_lock);
		/*  */
		i4RetValue = i2c_master_send(g_pstI2Cclient2, puReadCmd, 2);
		if (i4RetValue != 2) {
			PK_ERR("I2C send failed, addr = 0x%x, data = 0x%x!\n",
				a_u2Addr, *a_puBuff);
			return -1;
		}
		/*  */
		i4RetValue = i2c_master_recv(g_pstI2Cclient2,
			(char *)a_puBuff, 1);
		if (i4RetValue != 1) {
			PK_ERR("[CAMERA SENSOR] I2C read failed!!\n");
			return -1;
		}
	}
	return 0;
}

int iReadRegI2C(u8 *a_pSendData, u16 a_sizeSendData, u8 *a_pRecvData,
		u16 a_sizeRecvData, u16 i2cId)
{
	int i4RetValue = 0;

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient->addr = (i2cId >> 1);

		spin_unlock(&kdsensor_drv_lock);
		/*  */
		i4RetValue = i2c_master_send(g_pstI2Cclient,
			a_pSendData, a_sizeSendData);
		if (i4RetValue != a_sizeSendData) {
			PK_ERR("I2C send failed!!, Addr = 0x%x\n",
				a_pSendData[0]);
			return -1;
		}

		i4RetValue = i2c_master_recv(g_pstI2Cclient,
			(char *)a_pRecvData, a_sizeRecvData);
		if (i4RetValue != a_sizeRecvData) {
			PK_ERR("[CAMERA SENSOR] I2C read failed!!\n");
			return -1;
		}
	} else {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->addr = (i2cId >> 1);

		spin_unlock(&kdsensor_drv_lock);
		i4RetValue = i2c_master_send(g_pstI2Cclient2,
			a_pSendData, a_sizeSendData);
		if (i4RetValue != a_sizeSendData) {
			PK_ERR("I2C send failed!!, Addr = 0x%x\n",
				a_pSendData[0]);
			return -1;
		}

		i4RetValue = i2c_master_recv(g_pstI2Cclient2,
			(char *)a_pRecvData, a_sizeRecvData);
		if (i4RetValue != a_sizeRecvData) {
			PK_ERR("[CAMERA SENSOR] I2C read failed!!\n");
			return -1;
		}
	}
	return 0;
}


int iWriteReg(u16 a_u2Addr, u32 a_u4Data, u32 a_u4Bytes, u16 i2cId)
{
	int i4RetValue = 0;
	int u4Index = 0;
	u8 *puDataInBytes = (u8 *) &a_u4Data;
	int retry = 3;

	char puSendCmd[6] = { (char)(a_u2Addr >> 8), (char)(a_u2Addr & 0xFF),
		0, 0, 0, 0
	};

	/* KD_IMGSENSOR_PROFILE_INIT(); */
	spin_lock(&kdsensor_drv_lock);

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1)
		g_pstI2Cclient->addr = (i2cId >> 1);
	else
		g_pstI2Cclient2->addr = (i2cId >> 1);

	spin_unlock(&kdsensor_drv_lock);


	if (a_u4Bytes > 2) {
		PK_ERR("[CAMERA SENSOR] exceed 2 bytes\n");
		return -1;
	}

	if (a_u4Data >> (a_u4Bytes << 3))
		PK_ERR("[CAMERA SENSOR] warning!! some data is not sent!!\n");

	for (u4Index = 0; u4Index < a_u4Bytes; u4Index += 1)
		puSendCmd[(u4Index + 2)] =
			puDataInBytes[(a_u4Bytes - u4Index - 1)];
	/*  */
	do {
		if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1)
			i4RetValue = i2c_master_send(g_pstI2Cclient,
				puSendCmd, (a_u4Bytes + 2));
		else
			i4RetValue = i2c_master_send(g_pstI2Cclient2,
				puSendCmd, (a_u4Bytes + 2));

		if (i4RetValue != (a_u4Bytes + 2)) {
			PK_ERR("[CAMERA SENSOR] I2C send failed addr = 0x%x, data = 0x%x !!\n",
				a_u2Addr, a_u4Data);
		} else {
			break;
		}
		uDELAY(50);
	} while ((retry--) > 0);
	/* KD_IMGSENSOR_PROFILE("iWriteReg"); */
	return 0;
}

int kdSetI2CBusNum(u32 i2cBusNum)
{

	if ((i2cBusNum != SUPPORT_I2C_BUS_NUM2)
		&& (i2cBusNum != SUPPORT_I2C_BUS_NUM1)) {
		PK_ERR("[kdSetI2CBusNum] i2c bus number is not correct(%d)\n",
			i2cBusNum);
		return -1;
	}
	spin_lock(&kdsensor_drv_lock);
	gI2CBusNum = i2cBusNum;
	spin_unlock(&kdsensor_drv_lock);

	return 0;
}

void kdSetI2CSpeed(u32 i2cSpeed)
{
#if 0 /* AOSP doesn't support mt_i2c */
	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient->timing = i2cSpeed;
		spin_unlock(&kdsensor_drv_lock);
	} else {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->timing = i2cSpeed;
		spin_unlock(&kdsensor_drv_lock);
	}
#endif
}

int kdReleaseI2CTriggerLock(void)
{
	int ret = 0;

	/* ret = mt_wait4_i2c_complete(); */

	/* if (ret < 0 ) { */
	/* PK_ERR("[error]wait i2c fail\n"); */
	/* } */

	return ret;
}

#define MAX_CMD_LEN	  255
int iBurstWriteReg_multi(u8 *pData, u32 bytes, u16 i2cId, u16 transfer_length)
{

	uintptr_t phyAddr;
	u8 *buf = NULL;
	u32 old_addr = 0;
	int ret = 0;
	int retry = 0;

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		if (bytes > MAX_CMD_LEN) {
			PK_ERR("%s exceed the max write length\n", __func__);
			return 1;
		}

		phyAddr = 0;

		buf = dma_alloc_coherent(&(camerahw_platform_device.dev), bytes,
			(dma_addr_t *) &phyAddr, GFP_KERNEL);

		if (buf == NULL) {
			PK_ERR("[iBurstWriteReg] Not enough memory\n");
			return -1;
		}
		memset(buf, 0, bytes);
		memcpy(buf, pData, bytes);

		old_addr = g_pstI2Cclient->addr;
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient->addr = (i2cId >> 1);
		spin_unlock(&kdsensor_drv_lock);

		ret = 0;
		retry = 3;
		do {
			ret = i2c_master_send(g_pstI2Cclient, (u8 *)phyAddr,
				bytes == transfer_length ? transfer_length :
				((bytes / transfer_length) << 16) |
				transfer_length);
			retry--;
			if ((ret & 0xffff) != transfer_length)
				PK_ERR("Error sent I2C ret = %d\n", ret);

		} while (((ret & 0xffff) != transfer_length) && (retry > 0));

		dma_free_coherent(&(camerahw_platform_device.dev), bytes,
			buf, phyAddr);
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient->addr = old_addr;
		spin_unlock(&kdsensor_drv_lock);
	} else {
		if (bytes > MAX_CMD_LEN) {
			PK_ERR("%s exceed the max write length\n", __func__);
			return 1;
		}
		phyAddr = 0;
		buf = dma_alloc_coherent(&(camerahw_platform_device.dev), bytes,
			(dma_addr_t *) &phyAddr, GFP_KERNEL);

		if (buf == NULL) {
			PK_ERR("[%s] Not enough memory\n", __func__);
			return -1;
		}
		memset(buf, 0, bytes);
		memcpy(buf, pData, bytes);

		old_addr = g_pstI2Cclient2->addr;
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->addr = (i2cId >> 1);
		spin_unlock(&kdsensor_drv_lock);
		ret = 0;
		retry = 3;
		do {
			ret = i2c_master_send(g_pstI2Cclient2, (u8 *)phyAddr,
				bytes == transfer_length ? transfer_length :
				((bytes / transfer_length) << 16) |
				transfer_length);
			retry--;
			if ((ret & 0xffff) != transfer_length)
				PK_ERR("Error sent I2C ret = %d\n", ret);

		} while (((ret & 0xffff) != transfer_length) && (retry > 0));


		dma_free_coherent(&(camerahw_platform_device.dev), bytes,
			buf, phyAddr);
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->addr = old_addr;
		spin_unlock(&kdsensor_drv_lock);
	}
	return 0;
}

int iBurstWriteReg(u8 *pData, u32 bytes, u16 i2cId)
{
	return iBurstWriteReg_multi(pData, bytes, i2cId, bytes);
}

int iMultiWriteReg(u8 *pData, u16 lens, u16 i2cId)
{
	int ret = 0;

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		g_pstI2Cclient->addr = (i2cId >> 1);
		ret = i2c_master_send(g_pstI2Cclient, pData, lens);
	} else {
		g_pstI2Cclient2->addr = (i2cId >> 1);
		ret = i2c_master_send(g_pstI2Cclient2, pData, lens);
	}

	if (ret != lens)
		PK_ERR("Error sent I2C ret = %d\n", ret);

	return 0;
}


int iWriteRegI2C(u8 *a_pSendData, u16 a_sizeSendData, u16 i2cId)
{
	int i4RetValue = 0;
	int retry = 3;

	/* PK_DBG("Addr : 0x%x,Val : 0x%x\n",a_u2Addr,a_u4Data); */

	/* KD_IMGSENSOR_PROFILE_INIT(); */
	spin_lock(&kdsensor_drv_lock);
	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1)
		g_pstI2Cclient->addr = (i2cId >> 1);
	else
		g_pstI2Cclient2->addr = (i2cId >> 1);

	spin_unlock(&kdsensor_drv_lock);
	/*  */

	do {
		if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
			i4RetValue = i2c_master_send(g_pstI2Cclient,
				a_pSendData, a_sizeSendData);
		} else {
			i4RetValue = i2c_master_send(g_pstI2Cclient2,
				a_pSendData, a_sizeSendData);
		}
		if (i4RetValue != a_sizeSendData) {
			PK_DBG_NONE("I2C send failed!!, Addr = 0x%x, Data = 0x%x\n",
				a_pSendData[0], a_pSendData[1]);
		} else {
			break;
		}
		uDELAY(50);
	} while ((retry--) > 0);
	/* KD_IMGSENSOR_PROFILE("iWriteRegI2C"); */
	return 0;
}

#define KD_MULTI_FUNCTION_ENTRY() /* PK_INFO("[%s]:E\n",__FUNCTION__) */
#define KD_MULTI_FUNCTION_EXIT() /* PK_INFO("[%s]:X\n",__FUNCTION__) */
/*  */
MUINT32 kdSetI2CSlaveID(MINT32 i, MUINT32 socketIdx, MUINT32 firstSet)
{
	unsigned long long FeaturePara[4];
	MUINT32 FeatureParaLen = 0;

	FeaturePara[0] = socketIdx;
	FeaturePara[1] = firstSet;
	FeatureParaLen = sizeof(unsigned long long) * 2;
	return g_pInvokeSensorFunc[i]->SensorFeatureControl(
		SENSOR_FEATURE_SET_SLAVE_I2C_ID,
		(MUINT8 *) FeaturePara,
		(MUINT32 *) &FeatureParaLen);
}

/*  */
MUINT32 kd_MultiSensorOpen(void)
{
	MUINT32 ret = ERROR_NONE;
	MINT32 i = 0;

	KD_MULTI_FUNCTION_ENTRY();
	/* from hear to tail */
	/* for ( i = KDIMGSENSOR_INVOKE_DRIVER_0 ; */
	/* i < KDIMGSENSOR_MAX_INVOKE_DRIVERS ; i++ ) { */
	/* from tail to head. */
	for (i = (KDIMGSENSOR_MAX_INVOKE_DRIVERS - 1);
		i >= KDIMGSENSOR_INVOKE_DRIVER_0; i--) {
		if (g_bEnableDriver[i] && g_pInvokeSensorFunc[i]) {
			if (0 != (g_CurrentSensorIdx & g_invokeSocketIdx[i])) {
#ifndef CONFIG_FPGA_EARLY_PORTING

				/* turn on power */
				ret =
					kdCISModulePowerOn(
					(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM)
					g_invokeSocketIdx[i],
					(char *)g_invokeSensorNameStr[i], true,
					CAMERA_HW_DRVNAME1);
#endif
				if (ret != ERROR_NONE) {
					PK_ERR("[%s]", __func__);
					return ret;
				}
				/* wait for power stable */
				mDELAY(10);
				KD_IMGSENSOR_PROFILE("kdModulePowerOn");

#if 1
				if (DUAL_CAMERA_MAIN_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_MAIN_2_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorOpen: switch I2C BUS%d\n",
						gI2CBusNum);
				}
#else
				if (DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM2;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorOpen: switch I2C BUS2\n");
				} else {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorOpen: switch I2C BUS1\n");
				}
#endif
				/*  */
				ret = g_pInvokeSensorFunc[i]->SensorOpen();
				if (ret != ERROR_NONE) {
#ifndef CONFIG_FPGA_EARLY_PORTING
					kdCISModulePowerOn((enum
						CAMERA_DUAL_CAMERA_SENSOR_ENUM)
						g_invokeSocketIdx[i],
						(char *)
						g_invokeSensorNameStr[i], false,
						CAMERA_HW_DRVNAME1);
#endif
					PK_ERR("SensorOpen");
					return ret;
				}
			}
		}
	}
	KD_MULTI_FUNCTION_EXIT();
	return ERROR_NONE;
}

/*  */

MUINT32
kd_MultiSensorGetInfo(MUINT32 *pScenarioId[2],
			MSDK_SENSOR_INFO_STRUCT *pSensorInfo[2],
			MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData[2])
{
	MUINT32 ret = ERROR_NONE;
	u32 i = 0;
	MSDK_SENSOR_INFO_STRUCT SensorInfo[2];
	MSDK_SENSOR_CONFIG_STRUCT SensorConfigData[2];

	memset(&SensorInfo[0], 0, 2 * sizeof(MSDK_SENSOR_INFO_STRUCT));
	memset(&SensorConfigData[0], 0, 2 * sizeof(MSDK_SENSOR_CONFIG_STRUCT));


	KD_MULTI_FUNCTION_ENTRY();
	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
			i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
		if (g_bEnableDriver[i] && g_pInvokeSensorFunc[i]) {
			if (g_invokeSocketIdx[i] == DUAL_CAMERA_MAIN_SENSOR) {
				ret =
					g_pInvokeSensorFunc[i]->
					SensorGetInfo(
					(enum MSDK_SCENARIO_ID_ENUM)
					(*pScenarioId[0]), &SensorInfo[0],
					&SensorConfigData[0]);
			} else if ((g_invokeSocketIdx[i] ==
					DUAL_CAMERA_MAIN_2_SENSOR)
				   || (g_invokeSocketIdx[i] ==
					DUAL_CAMERA_SUB_SENSOR)) {
				ret =
					g_pInvokeSensorFunc[i]->
					SensorGetInfo(
					(enum MSDK_SCENARIO_ID_ENUM)
					(*pScenarioId[1]), &SensorInfo[1],
					&SensorConfigData[1]);
			}

			if (ret != ERROR_NONE) {
				PK_ERR("[%s]\n", __func__);
				return ret;
			}

		}
	}
	memcpy(pSensorInfo[0], &SensorInfo[0],
		sizeof(MSDK_SENSOR_INFO_STRUCT));
	memcpy(pSensorInfo[1], &SensorInfo[1],
		sizeof(MSDK_SENSOR_INFO_STRUCT));
	memcpy(pSensorConfigData[0], &SensorConfigData[0],
		sizeof(MSDK_SENSOR_CONFIG_STRUCT));
	memcpy(pSensorConfigData[1], &SensorConfigData[1],
		sizeof(MSDK_SENSOR_CONFIG_STRUCT));

	KD_MULTI_FUNCTION_EXIT();
	return ERROR_NONE;
}

/*  */

MUINT32 kd_MultiSensorGetResolution(
		MSDK_SENSOR_RESOLUTION_INFO_STRUCT * pSensorResolution[2])
{
	MUINT32 ret = ERROR_NONE;
	u32 i = 0;

	KD_MULTI_FUNCTION_ENTRY();
	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
			i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
		if (g_bEnableDriver[i] && g_pInvokeSensorFunc[i]) {
			if (g_invokeSocketIdx[i] == DUAL_CAMERA_MAIN_SENSOR) {
				ret =
					g_pInvokeSensorFunc[i]->
					SensorGetResolution(
					pSensorResolution[0]);
			} else if ((g_invokeSocketIdx[i] ==
					DUAL_CAMERA_MAIN_2_SENSOR)
				   || (g_invokeSocketIdx[i] ==
					   DUAL_CAMERA_SUB_SENSOR)) {
				ret =
					g_pInvokeSensorFunc[i]->
					SensorGetResolution(
					pSensorResolution[1]);
			}

			if (ret != ERROR_NONE) {
				PK_ERR("[%s]\n", __func__);
				return ret;
			}
		}
	}

	KD_MULTI_FUNCTION_EXIT();
	return ERROR_NONE;
}


/*  */
MUINT32
kd_MultiSensorFeatureControl(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM InvokeCamera,
				MSDK_SENSOR_FEATURE_ENUM FeatureId,
				MUINT8 *pFeaturePara, MUINT32 *pFeatureParaLen)
{
	MUINT32 ret = ERROR_NONE;
	u32 i = 0;

	/*KD_MULTI_FUNCTION_ENTRY();*/
	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
			i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {

		if (g_bEnableDriver[i] && g_pInvokeSensorFunc[i]) {


			if (InvokeCamera == g_invokeSocketIdx[i]) {


#if 1
				if (DUAL_CAMERA_MAIN_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_MAIN_2_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
					PK_DBG_NONE("kd_MultiSensorOpen: switch I2C BUS%d\n",
						gI2CBusNum);
				}
#else
				if (DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM2;
					spin_unlock(&kdsensor_drv_lock);
				} else {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
				}
#endif
				/*  */
				ret =
					g_pInvokeSensorFunc[i]->
					SensorFeatureControl(FeatureId,
					pFeaturePara, pFeatureParaLen);
				if (ret != ERROR_NONE) {
					PK_ERR("[%s]\n", __func__);
					return ret;
				}
			}
		}
	}
	/*KD_MULTI_FUNCTION_EXIT();*/
	return ERROR_NONE;
}

/*  */
MUINT32
kd_MultiSensorControl(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM InvokeCamera,
			enum MSDK_SCENARIO_ID_ENUM ScenarioId,
			MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *pImageWindow,
			MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData)
{
	MUINT32 ret = ERROR_NONE;
	u32 i = 0;

	KD_MULTI_FUNCTION_ENTRY();
	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
			i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
		if (g_bEnableDriver[i] && g_pInvokeSensorFunc[i]) {
			if (InvokeCamera == g_invokeSocketIdx[i]) {

#if 1
				if (DUAL_CAMERA_MAIN_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_MAIN_2_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorOpen: switch I2C BUS%d\n",
						gI2CBusNum);
				}
#else
				if (DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM2;
					spin_unlock(&kdsensor_drv_lock);
				} else {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
				}
#endif
				/*  */
				g_pInvokeSensorFunc[i]->ScenarioId = ScenarioId;
			memcpy(&g_pInvokeSensorFunc[i]->imageWindow,
				pImageWindow, sizeof(struct
				ACDK_SENSOR_EXPOSURE_WINDOW_STRUCT));
			memcpy(&g_pInvokeSensorFunc[i]->sensorConfigData,
				pSensorConfigData, sizeof(struct
				ACDK_SENSOR_CONFIG_STRUCT));
				ret = g_pInvokeSensorFunc[i]->SensorControl(
					ScenarioId, pImageWindow,
					pSensorConfigData);
				if (ret != ERROR_NONE) {
					PK_ERR("ERR:SensorControl(), i =%d\n",
						i);
					return ret;
				}
			}
		}
	}
	KD_MULTI_FUNCTION_EXIT();


	/* js_tst FIXME */
	/* if (DUAL_CHANNEL_I2C) { */
	/* trigger dual channel i2c */
	/* } */
	/* else { */
	if (g_bEnableDriver[1]) { /* drive 2 or more sensor simultaneously */
		MUINT8 frameSync = 0;
		MUINT32 frameSyncSize = 0;

		kd_MultiSensorFeatureControl(g_invokeSocketIdx[1],
			SENSOR_FEATURE_SUSPEND,
			&frameSync, &frameSyncSize);
		mDELAY(10);
		kd_MultiSensorFeatureControl(g_invokeSocketIdx[1],
			SENSOR_FEATURE_RESUME,
			&frameSync, &frameSyncSize);
	}
	/* } */


	return ERROR_NONE;
}

/*  */
MUINT32 kd_MultiSensorClose(void)
{
	MUINT32 ret = ERROR_NONE;
	u32 i = 0;

	KD_MULTI_FUNCTION_ENTRY();
	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
			i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
		if (g_bEnableDriver[i] && g_pInvokeSensorFunc[i]) {
			if (0 != (g_CurrentSensorIdx & g_invokeSocketIdx[i])) {
#if 1
				if (DUAL_CAMERA_MAIN_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]
					|| DUAL_CAMERA_MAIN_2_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorClose: switch I2C BUS%d\n",
						gI2CBusNum);
				}
#else
				if (DUAL_CAMERA_SUB_SENSOR ==
					g_invokeSocketIdx[i]) {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM2;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorClose: switch I2C BUS2\n");
				} else {
					spin_lock(&kdsensor_drv_lock);
					gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
					spin_unlock(&kdsensor_drv_lock);
					PK_INFO("kd_MultiSensorClose: switch I2C BUS1\n");
				}
#endif
				ret = g_pInvokeSensorFunc[i]->SensorClose();

#ifndef CONFIG_FPGA_EARLY_PORTING
				kdCISModulePowerOn(
					(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM)
					g_invokeSocketIdx[i],
					(char *)g_invokeSensorNameStr[i], false,
					CAMERA_HW_DRVNAME1);
#endif
				if (ret != ERROR_NONE) {
					PK_ERR("[%s]", __func__);
					return ret;
				}
			}
		}
	}
	KD_MULTI_FUNCTION_EXIT();
	return ERROR_NONE;
}

/*  */
struct MULTI_SENSOR_FUNCTION_STRUCT2 kd_MultiSensorFunc = {
	kd_MultiSensorOpen,
	kd_MultiSensorGetInfo,
	kd_MultiSensorGetResolution,
	kd_MultiSensorFeatureControl,
	kd_MultiSensorControl,
	kd_MultiSensorClose
};


int kdModulePowerOn(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM
		socketIdx[KDIMGSENSOR_MAX_INVOKE_DRIVERS],
		char sensorNameStr[KDIMGSENSOR_MAX_INVOKE_DRIVERS][32],
		BOOL On, char *mode_name)
{
	MINT32 ret = ERROR_NONE;
	u32 i = 0;

	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
			i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
		if (g_bEnableDriver[i]) {
			ret = kdCISModulePowerOn(socketIdx[i],
					sensorNameStr[i], On, mode_name);
			if (ret != ERROR_NONE) {
				PK_ERR("[%s]", __func__);
				return ret;
			}
		}
	}
	return ERROR_NONE;
}

int kdSetDriver(unsigned int *pDrvIndex)
{
	struct ACDK_KD_SENSOR_INIT_FUNCTION_STRUCT *pSensorList = NULL;
	u32 drvIdx[KDIMGSENSOR_MAX_INVOKE_DRIVERS] = { 0, 0 };
	u32 i;

	PK_INFO("pDrvIndex:0x%08x/0x%08x\n",
		pDrvIndex[KDIMGSENSOR_INVOKE_DRIVER_0],
			pDrvIndex[KDIMGSENSOR_INVOKE_DRIVER_1]);
	/* set driver for MAIN or SUB sensor */
	/* Camera information */
	gDrvIndex = pDrvIndex[KDIMGSENSOR_INVOKE_DRIVER_0];

	if (kdGetSensorInitFuncList(&pSensorList) != 0) {
		PK_ERR("ERROR:kdGetSensorInitFuncList()\n");
		return -EIO;
	}

	for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
		i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
		/*  */
		spin_lock(&kdsensor_drv_lock);
		g_bEnableDriver[i] = FALSE;
		g_invokeSocketIdx[i] =
			(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM)
			((pDrvIndex[i] & KDIMGSENSOR_DUAL_MASK_MSB) >>
			KDIMGSENSOR_DUAL_SHIFT);
		spin_unlock(&kdsensor_drv_lock);
		drvIdx[i] = (pDrvIndex[i] & KDIMGSENSOR_DUAL_MASK_LSB);
		/*  */
		if (g_invokeSocketIdx[i] == DUAL_CAMERA_NONE_SENSOR)
			continue;
#if 1
		if (g_invokeSocketIdx[i] == DUAL_CAMERA_MAIN_SENSOR
			|| g_invokeSocketIdx[i] == DUAL_CAMERA_SUB_SENSOR
			|| g_invokeSocketIdx[i] == DUAL_CAMERA_MAIN_2_SENSOR) {
			spin_lock(&kdsensor_drv_lock);
			gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
			spin_unlock(&kdsensor_drv_lock);
			PK_XLOG_INFO("kd_MultiSensorOpen: switch I2C BUS%d\n",
				gI2CBusNum);
		}
#else

		if (g_invokeSocketIdx[i] == DUAL_CAMERA_SUB_SENSOR) {
			spin_lock(&kdsensor_drv_lock);
			gI2CBusNum = SUPPORT_I2C_BUS_NUM2;
			spin_unlock(&kdsensor_drv_lock);
			PK_XLOG_INFO("Sub cam uses I2C BUS#: %d\n",
				gI2CBusNum);
		} else {
			spin_lock(&kdsensor_drv_lock);
			gI2CBusNum = SUPPORT_I2C_BUS_NUM1;
			spin_unlock(&kdsensor_drv_lock);
			PK_XLOG_INFO("Main cam uses I2C BUS#: %d\n",
				gI2CBusNum);
		}
#endif
		PK_INFO("[%s]g_invokeSocketIdx[%d] = %d, drvIdx[%d] = %d\n",
			__func__, i, g_invokeSocketIdx[i], i, drvIdx[i]);
		/*  */
		if (drvIdx[i] < MAX_NUM_OF_SUPPORT_SENSOR) {
			if (pSensorList[drvIdx[i]].SensorInit == NULL) {
				PK_ERR("ERROR:%s\n", __func__);
				return -EIO;
			}

			pSensorList[drvIdx[i]].SensorInit(
				&g_pInvokeSensorFunc[i]);
			if (g_pInvokeSensorFunc[i] == NULL) {
				PK_ERR("ERROR:NULL g_pSensorFunc[%d]\n", i);
				return -EIO;
			}
			/*  */
			spin_lock(&kdsensor_drv_lock);
			g_bEnableDriver[i] = TRUE;
			spin_unlock(&kdsensor_drv_lock);
			/* get sensor name */
			memcpy((char *)g_invokeSensorNameStr[i],
				(char *)pSensorList[drvIdx[i]].drvname,
				sizeof(pSensorList[drvIdx[i]].drvname));
			/* return sensor ID */
			/* pDrvIndex[0] = */
			/* (unsigned int)pSensorList[drvIdx].SensorId; */
			PK_INFO("[%s] :[%d][%d][%d][%s][%d]\n",
				__func__, i, g_bEnableDriver[i],
				g_invokeSocketIdx[i], g_invokeSensorNameStr[i],
				(int)sizeof(pSensorList[drvIdx[i]].drvname));
		}
	}
	return 0;
}

int kdSetCurrentSensorIdx(unsigned int idx)
{
	g_CurrentSensorIdx = idx;
	return 0;
}

int kdGetSocketPostion(unsigned int *pSocketPos)
{
	PK_XLOG_INFO("[%s][%d]\n", __func__, *pSocketPos);
	switch (*pSocketPos) {
	case DUAL_CAMERA_MAIN_SENSOR:
		/* ->this is a HW layout dependent */
		/* ToDo */
		*pSocketPos = IMGSENSOR_SOCKET_POS_RIGHT;
		break;
	case DUAL_CAMERA_MAIN_2_SENSOR:
		*pSocketPos = IMGSENSOR_SOCKET_POS_LEFT;
		break;
	default:
	case DUAL_CAMERA_SUB_SENSOR:
		*pSocketPos = IMGSENSOR_SOCKET_POS_NONE;
		break;
	}
	return 0;
}

int kdSetSensorSyncFlag(BOOL bSensorSync)
{
	spin_lock(&kdsensor_drv_lock);

	bSesnorVsyncFlag = bSensorSync;
	spin_unlock(&kdsensor_drv_lock);
	/* PK_DBG("[Sensor] kdSetSensorSyncFlag:%d\n", bSesnorVsyncFlag); */

	/* strobe_VDIrq(); //cotta : added for high current solution */

	return 0;
}
EXPORT_SYMBOL(kdSetSensorSyncFlag);

int kdCheckSensorPowerOn(void)
{
	if (atomic_read(&g_CamHWOpening) == 0) {
		return 0;
	} else { /* sensor power on */
		return 1;
	}
}

/* ToDo: How to separate main/main2....who is caller? */
int kdSensorSyncFunctionPtr(void)
{
	unsigned int FeatureParaLen = 0;
#if 0
	PK_DBG("[Sensor] kdSensorSyncFunctionPtr1:%d %d %d\n",
	   g_NewSensorExpGain.uSensorExpDelayFrame,
	   g_NewSensorExpGain.uSensorGainDelayFrame,
	   g_NewSensorExpGain.uISPGainDelayFrame);
#endif
	mutex_lock(&kdCam_Mutex);
	if (g_pSensorFunc == NULL) {
		PK_ERR("ERROR:NULL g_pSensorFunc\n");
		mutex_unlock(&kdCam_Mutex);
		return -EIO;
	}
#if 0
	PK_DBG("[Sensor] Exposure time:%d, Gain = %d\n",
		g_NewSensorExpGain.u2SensorNewExpTime,
		g_NewSensorExpGain.u2SensorNewGain);
#endif
	/* exposure time */
	if (g_NewSensorExpGain.uSensorExpDelayFrame == 0) {
		FeatureParaLen = 2;
		g_pSensorFunc->SensorFeatureControl(DUAL_CAMERA_MAIN_SENSOR,
			SENSOR_FEATURE_SET_ESHUTTER,
			(unsigned char *)&g_NewSensorExpGain.
			u2SensorNewExpTime,
			(unsigned int *)&FeatureParaLen);
		g_NewSensorExpGain.uSensorExpDelayFrame = 0xFF;
	} else if (g_NewSensorExpGain.uSensorExpDelayFrame != 0xFF) {
		g_NewSensorExpGain.uSensorExpDelayFrame--;
	}
	/* exposure gain */
	if (g_NewSensorExpGain.uSensorGainDelayFrame == 0) {
		FeatureParaLen = 2;
		g_pSensorFunc->SensorFeatureControl(DUAL_CAMERA_MAIN_SENSOR,
			SENSOR_FEATURE_SET_GAIN,
			(unsigned char *)&g_NewSensorExpGain.
			u2SensorNewGain,
			(unsigned int *)&FeatureParaLen);
		g_NewSensorExpGain.uSensorGainDelayFrame = 0xFF;
	} else if (g_NewSensorExpGain.uSensorGainDelayFrame != 0xFF) {
		g_NewSensorExpGain.uSensorGainDelayFrame--;
	}
	/* if the delay frame is 0 or 0xFF, stop to count */
	if ((g_NewSensorExpGain.uISPGainDelayFrame != 0xFF)
		&& (g_NewSensorExpGain.uISPGainDelayFrame != 0)) {
		g_NewSensorExpGain.uISPGainDelayFrame--;
	}
	mutex_unlock(&kdCam_Mutex);
	return 0;
}
EXPORT_SYMBOL(kdSensorSyncFunctionPtr);

int kdGetRawGainInfoPtr(MUINT16 *pRAWGain)
{
	*pRAWGain = 0x00;
	*(pRAWGain + 1) = 0x00;
	*(pRAWGain + 2) = 0x00;
	*(pRAWGain + 3) = 0x00;

	if (g_NewSensorExpGain.uISPGainDelayFrame == 0) {
		*pRAWGain = g_NewSensorExpGain.u2ISPNewRGain;
		*(pRAWGain + 1) = g_NewSensorExpGain.u2ISPNewGrGain;
		*(pRAWGain + 2) = g_NewSensorExpGain.u2ISPNewGbGain;
		*(pRAWGain + 3) = g_NewSensorExpGain.u2ISPNewBGain;
		spin_lock(&kdsensor_drv_lock);
		g_NewSensorExpGain.uISPGainDelayFrame = 0xFF;	/* disable */
		spin_unlock(&kdsensor_drv_lock);
	}

	return 0;
}
EXPORT_SYMBOL(kdGetRawGainInfoPtr);

int kdSetExpGain(enum CAMERA_DUAL_CAMERA_SENSOR_ENUM InvokeCamera)
{
	unsigned int FeatureParaLen = 0;

	PK_DBG("[kd_sensorlist]enter %s\n", __func__);
	if (g_pSensorFunc == NULL) {
		PK_ERR("ERROR:NULL g_pSensorFunc\n");
		return -EIO;
	}

	setExpGainDoneFlag = 0;
	FeatureParaLen = 2;
	g_pSensorFunc->SensorFeatureControl(InvokeCamera,
		SENSOR_FEATURE_SET_ESHUTTER,
		(unsigned char *)&g_NewSensorExpGain.u2SensorNewExpTime,
		(unsigned int *)&FeatureParaLen);
	g_pSensorFunc->SensorFeatureControl(InvokeCamera,
		SENSOR_FEATURE_SET_GAIN,
		(unsigned char *)&g_NewSensorExpGain.u2SensorNewGain,
		(unsigned int *)&FeatureParaLen);

	setExpGainDoneFlag = 1;
	PK_DBG("[kd_sensorlist]before wake_up_interruptible\n");
	wake_up_interruptible(&kd_sensor_wait_queue);
	PK_DBG("[kd_sensorlist]after wake_up_interruptible\n");

	return 0;		/* No error. */

}

static MUINT32 ms_to_jiffies(MUINT32 ms)
{
	return ((ms * HZ + 512) >> 10);
}


int kdSensorSetExpGainWaitDone(int *ptime)
{
	int timeout;

	PK_DBG("[kd_sensorlist]enter %s: time: %d\n", __func__, *ptime);
	timeout = wait_event_interruptible_timeout(kd_sensor_wait_queue,
		(setExpGainDoneFlag & 1), ms_to_jiffies(*ptime));

	PK_DBG("[kd_sensorlist]after wait_event_interruptible_timeout\n");
	if (timeout == 0) {
		PK_ERR("[kd_sensorlist] %s: timeout=%d\n", __func__, *ptime);
		return -EAGAIN;
	}

	return 0;		/* No error. */

}

static inline int adopt_CAMERA_HW_Open(void)
{
	MUINT32 err = 0;

	KD_IMGSENSOR_PROFILE_INIT();
	/* power on sensor */
	/* if (atomic_read(&g_CamHWOpend) == 0  ) { */
	/* move into SensorOpen() for 2on1 driver */
	/* turn on power */
	/* kdModulePowerOn((CAMERA_DUAL_CAMERA_SENSOR_ENUM*) */
	/* g_invokeSocketIdx, */
	/* g_invokeSensorNameStr,true, CAMERA_HW_DRVNAME); */
	/* wait for power stable */
	/* mDELAY(10); */
	/* KD_IMGSENSOR_PROFILE("kdModulePowerOn"); */
	/*  */
	if (g_pSensorFunc) {
		err = g_pSensorFunc->SensorOpen();
		if (err != ERROR_NONE)
			kdModulePowerOn((enum CAMERA_DUAL_CAMERA_SENSOR_ENUM *)
					g_invokeSocketIdx,
					g_invokeSensorNameStr, false,
					CAMERA_HW_DRVNAME1);
	} else
		PK_ERR(" ERROR:NULL g_pSensorFunc\n");

	KD_IMGSENSOR_PROFILE("SensorOpen");

	return err ? -EIO : err;
}				/* adopt_CAMERA_HW_Open() */

static inline int adopt_CAMERA_HW_CheckIsAlive(void)
{
	MUINT32 err = 0;
	MUINT32 err1 = 0;
	MUINT32 i = 0;
	MUINT32 sensorID = 0;
	MUINT32 retLen = 0;
	static char mtk_ccm_name_tmp[camera_info_size] = { 0 };

	KD_IMGSENSOR_PROFILE_INIT();
	/* power on sensor */
	err =
		kdModulePowerOn((enum CAMERA_DUAL_CAMERA_SENSOR_ENUM *)
				g_invokeSocketIdx,
				g_invokeSensorNameStr, true,
				CAMERA_HW_DRVNAME1);
	/* Bypass redundant search operation of getting sensor ID, */
	/* if power on failed */
	if (err != ERROR_NONE) {
		PK_ERR("%s\n",
			err ==
			-ENODEV ? "No device in this socket position" :
			"kdModulePowerOn failed");
		return err;
	}
	/* wait for power stable */
	mDELAY(10);
	KD_IMGSENSOR_PROFILE("kdModulePowerOn");

	/* initial for search sensor function */
	g_CurrentSensorIdx = 0;
	/* Search sensor keep i2c debug log */
	g_IsSearchSensor = 1;
	/* Camera information */
	if (gDrvIndex == 0x10000)
		memset(mtk_ccm_name, 0, camera_info_size);


	if (g_pSensorFunc) {
		for (i = KDIMGSENSOR_INVOKE_DRIVER_0;
				i < KDIMGSENSOR_MAX_INVOKE_DRIVERS; i++) {
			if (g_invokeSocketIdx[i] != DUAL_CAMERA_NONE_SENSOR) {
				err =
					g_pSensorFunc->SensorFeatureControl(
					g_invokeSocketIdx[i],
					SENSOR_FEATURE_CHECK_SENSOR_ID,
					(MUINT8 *) &sensorID,
					&retLen);
				if (sensorID == 0) {
					PK_ERR("Not implement!!, use old open function to check\n");
					err = ERROR_SENSOR_CONNECT_FAIL;
				} else if (sensorID == 0xFFFFFFFF) {
					PK_ERR("No Sensor Found");
					err = ERROR_SENSOR_CONNECT_FAIL;
				} else {

					PK_DBG("Sensor found ID = 0x%x\n",
						sensorID);
					memset(mtk_ccm_name_tmp, 0, camera_info_size);
					snprintf(mtk_ccm_name_tmp,
						 camera_info_size,
						 "%s CAM[%d]:%s;", mtk_ccm_name,
						 g_invokeSocketIdx[i],
						 g_invokeSensorNameStr[i]);
					memcpy(mtk_ccm_name, mtk_ccm_name_tmp, camera_info_size);
					err = ERROR_NONE;
				}
				if (err != ERROR_NONE) {
					PK_ERR("ERROR:%s, No imgsensor alive\n",
						__func__);
				}
			}
		}
	} else {
		PK_ERR("ERROR:NULL g_pSensorFunc\n");
	}

	/* reset sensor state after power off */
	if (g_pSensorFunc)
		err1 = g_pSensorFunc->SensorClose();
	if (err1 != ERROR_NONE)
		PK_ERR("SensorClose\n");
	/*  */
	kdModulePowerOn((enum CAMERA_DUAL_CAMERA_SENSOR_ENUM *)
			g_invokeSocketIdx, g_invokeSensorNameStr,
			false, CAMERA_HW_DRVNAME1);
	/*  */
	KD_IMGSENSOR_PROFILE("CheckIsAlive");

	g_IsSearchSensor = 0;

	return err ? -EIO : err;
}				/* adopt_CAMERA_HW_Open() */


static inline int adopt_CAMERA_HW_GetResolution(void *pBuf)
{
	/* ToDo: remove print */
	struct ACDK_SENSOR_PRESOLUTION_STRUCT *pBufResolution =
		(struct ACDK_SENSOR_PRESOLUTION_STRUCT *) pBuf;
	struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT *pRes[2] = { NULL, NULL };

	PK_XLOG_INFO("[%s] pBuf: %p\n", __func__, pBuf);
	pRes[0] = (struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT *)
		kmalloc(sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT),
		GFP_KERNEL);
	if (pRes[0] == NULL) {
		PK_ERR(" ioctl allocate mem failed\n");
		return -ENOMEM;
	}
	pRes[1] = (struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT *)
		kmalloc(sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT),
		GFP_KERNEL);
	if (pRes[1] == NULL) {
		kfree(pRes[0]);
		PK_ERR(" ioctl allocate mem failed\n");
		return -ENOMEM;
	}

	if (g_pSensorFunc) {
		g_pSensorFunc->SensorGetResolution(pRes);
		if (copy_to_user((void __user *)
			(pBufResolution->pResolution[0]), (void *)pRes[0],
			sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT))) {
			PK_ERR("copy to user failed\n");
		}
		if (copy_to_user((void __user *)
			(pBufResolution->pResolution[1]), (void *)pRes[1],
			sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT))) {
			PK_ERR("copy to user failed\n");
		}
	} else
		PK_ERR("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");

	if (pRes[0] != NULL)
		kfree(pRes[0]);

	if (pRes[1] != NULL)
		kfree(pRes[1]);

	return 0;
}				/* adopt_CAMERA_HW_GetResolution() */


static inline int adopt_CAMERA_HW_GetInfo(void *pBuf)
{
	struct ACDK_SENSOR_GETINFO_STRUCT *pSensorGetInfo =
		(struct ACDK_SENSOR_GETINFO_STRUCT *) pBuf;
	MSDK_SENSOR_INFO_STRUCT info[2], *pInfo[2];
	MSDK_SENSOR_CONFIG_STRUCT config[2], *pConfig[2];
	MUINT32 *pScenarioId[2];
	u32 i = 0;

	for (i = 0; i < 2; i++) {
		pInfo[i] = &info[i];
		pConfig[i] = &config[i];
		pScenarioId[i] = &(pSensorGetInfo->ScenarioId[i]);
	}


	if (pSensorGetInfo == NULL) {
		PK_ERR("[CAMERA_HW] NULL arg.\n");
		return -EFAULT;
	}

	if ((pSensorGetInfo->pInfo[0] == NULL) ||
			(pSensorGetInfo->pInfo[1] == NULL) ||
		(pSensorGetInfo->pConfig[0] == NULL) ||
			(pSensorGetInfo->pConfig[1] == NULL)) {
		PK_ERR("[CAMERA_HW] NULL arg.\n");
		return -EFAULT;
	}

	if (g_pSensorFunc)
		g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo, pConfig);
	else
		PK_ERR("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");

	for (i = 0; i < 2; i++) {
		/* SenorInfo */
		if (copy_to_user
			((void __user *)(pSensorGetInfo->pInfo[i]),
			(void *)pInfo[i],
			sizeof(MSDK_SENSOR_INFO_STRUCT))) {
			PK_ERR("[%s][info] ioctl copy to user failed\n",
				__func__);
			return -EFAULT;
		}
		/* SensorConfig */
		if (copy_to_user
			((void __user *)(pSensorGetInfo->pConfig[i]),
			(void *)pConfig[i],
			sizeof(MSDK_SENSOR_CONFIG_STRUCT))) {
			PK_ERR("[%s][config] ioctl copy to user failed\n",
				__func__);
			return -EFAULT;
		}
	}
	return 0;
}				/* adopt_CAMERA_HW_GetInfo() */

MSDK_SENSOR_INFO_STRUCT ginfo[2];
MSDK_SENSOR_INFO_STRUCT ginfo1[2];
MSDK_SENSOR_INFO_STRUCT ginfo2[2];
MSDK_SENSOR_INFO_STRUCT ginfo3[2];
MSDK_SENSOR_INFO_STRUCT ginfo4[2];
/* adopt_CAMERA_HW_GetInfo() */
static inline int adopt_CAMERA_HW_GetInfo2(void *pBuf)
{
	struct IMAGESENSOR_GETINFO_STRUCT *pSensorGetInfo =
		(struct IMAGESENSOR_GETINFO_STRUCT *) pBuf;
	struct ACDK_SENSOR_INFO2_STRUCT SensorInfo = { 0 };
	MUINT32 IDNum = 0;
	MSDK_SENSOR_INFO_STRUCT *pInfo[2];
	MSDK_SENSOR_CONFIG_STRUCT config[2], *pConfig[2];
	MSDK_SENSOR_INFO_STRUCT *pInfo1[2];
	MSDK_SENSOR_CONFIG_STRUCT config1[2], *pConfig1[2];
	MSDK_SENSOR_INFO_STRUCT *pInfo2[2];
	MSDK_SENSOR_CONFIG_STRUCT config2[2], *pConfig2[2];
	MSDK_SENSOR_INFO_STRUCT *pInfo3[2];
	MSDK_SENSOR_CONFIG_STRUCT config3[2], *pConfig3[2];
	MSDK_SENSOR_INFO_STRUCT *pInfo4[2];
	MSDK_SENSOR_CONFIG_STRUCT config4[2], *pConfig4[2];
	MSDK_SENSOR_RESOLUTION_INFO_STRUCT SensorResolution[2],
		*psensorResolution[2];

	MUINT32 ScenarioId[2], *pScenarioId[2];
	u32 i = 0;

	PK_DBG("[adopt_CAMERA_HW_GetInfo2]Entry\n");
	for (i = 0; i < 2; i++) {
		pInfo[i] = &ginfo[i];
		pConfig[i] = &config[i];
		pInfo1[i] = &ginfo1[i];
		pConfig1[i] = &config1[i];
		pInfo2[i] = &ginfo2[i];
		pConfig2[i] = &config2[i];
		pInfo3[i] = &ginfo3[i];
		pConfig3[i] = &config3[i];
		pInfo4[i] = &ginfo4[i];
		pConfig4[i] = &config4[i];
		psensorResolution[i] = &SensorResolution[i];
		pScenarioId[i] = &ScenarioId[i];
	}

	if (pSensorGetInfo == NULL) {
		PK_ERR("[CAMERA_HW] NULL arg.\n");
		return -EFAULT;
	}
	if (g_pSensorFunc == NULL) {
		PK_ERR("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");
		return -EFAULT;
	}

	PK_DBG("[CAMERA_HW][Resolution] %p\n",
		pSensorGetInfo->pSensorResolution);

	/* TO get preview value */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CAMERA_PREVIEW;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo, pConfig);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo1, pConfig1);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_VIDEO_PREVIEW;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo2, pConfig2);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_HIGH_SPEED_VIDEO;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo3, pConfig3);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_SLIM_VIDEO;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo4, pConfig4);
	/* To set sensor information */
	if (pSensorGetInfo->SensorId == DUAL_CAMERA_MAIN_SENSOR)
		IDNum = 0;
	else
		IDNum = 1;

	PK_DBG("pSensorGetInfo->SensorId = %d , IDNum = %d\n",
		pSensorGetInfo->SensorId, IDNum);
	/* Basic information */
	SensorInfo.SensorPreviewResolutionX =
		pInfo[IDNum]->SensorPreviewResolutionX;
	SensorInfo.SensorPreviewResolutionY =
		pInfo[IDNum]->SensorPreviewResolutionY;
	SensorInfo.SensorFullResolutionX =
		pInfo[IDNum]->SensorFullResolutionX;
	SensorInfo.SensorFullResolutionY =
		pInfo[IDNum]->SensorFullResolutionY;
	SensorInfo.SensorClockFreq =
		pInfo[IDNum]->SensorClockFreq;
	SensorInfo.SensorCameraPreviewFrameRate =
		pInfo[IDNum]->SensorCameraPreviewFrameRate;
	SensorInfo.SensorVideoFrameRate =
		pInfo[IDNum]->SensorVideoFrameRate;
	SensorInfo.SensorStillCaptureFrameRate =
		pInfo[IDNum]->SensorStillCaptureFrameRate;
	SensorInfo.SensorWebCamCaptureFrameRate =
		pInfo[IDNum]->SensorWebCamCaptureFrameRate;
	SensorInfo.SensorClockPolarity =
		pInfo[IDNum]->SensorClockPolarity;
	SensorInfo.SensorClockFallingPolarity =
		pInfo[IDNum]->SensorClockFallingPolarity;
	SensorInfo.SensorClockRisingCount =
		pInfo[IDNum]->SensorClockRisingCount;
	SensorInfo.SensorClockFallingCount =
		pInfo[IDNum]->SensorClockFallingCount;
	SensorInfo.SensorClockDividCount =
		pInfo[IDNum]->SensorClockDividCount;
	SensorInfo.SensorPixelClockCount =
		pInfo[IDNum]->SensorPixelClockCount;
	SensorInfo.SensorDataLatchCount =
		pInfo[IDNum]->SensorDataLatchCount;
	SensorInfo.SensorHsyncPolarity =
		pInfo[IDNum]->SensorHsyncPolarity;
	SensorInfo.SensorVsyncPolarity =
		pInfo[IDNum]->SensorVsyncPolarity;
	SensorInfo.SensorInterruptDelayLines =
		pInfo[IDNum]->SensorInterruptDelayLines;
	SensorInfo.SensorResetActiveHigh =
		pInfo[IDNum]->SensorResetActiveHigh;
	SensorInfo.SensorResetDelayCount =
		pInfo[IDNum]->SensorResetDelayCount;
	SensorInfo.SensroInterfaceType =
		pInfo[IDNum]->SensroInterfaceType;
	SensorInfo.SensorOutputDataFormat =
		pInfo[IDNum]->SensorOutputDataFormat;

	PK_DBG("SensorInfo.SensorOutputDataFormat = 0x%x , pinfo[0]=0x%x , pinfo[1]=0x%x\n",
			SensorInfo.SensorOutputDataFormat,
			pInfo[0]->SensorOutputDataFormat,
			pInfo[1]->SensorOutputDataFormat);

	SensorInfo.SensorMIPILaneNumber = pInfo[IDNum]->SensorMIPILaneNumber;
	SensorInfo.CaptureDelayFrame = pInfo[IDNum]->CaptureDelayFrame;
	SensorInfo.PreviewDelayFrame = pInfo[IDNum]->PreviewDelayFrame;
	SensorInfo.VideoDelayFrame = pInfo[IDNum]->VideoDelayFrame;
	SensorInfo.HighSpeedVideoDelayFrame =
		pInfo[IDNum]->HighSpeedVideoDelayFrame;
	SensorInfo.SlimVideoDelayFrame = pInfo[IDNum]->SlimVideoDelayFrame;
	SensorInfo.Custom1DelayFrame = pInfo[IDNum]->Custom1DelayFrame;
	SensorInfo.Custom2DelayFrame = pInfo[IDNum]->Custom2DelayFrame;
	SensorInfo.Custom3DelayFrame = pInfo[IDNum]->Custom3DelayFrame;
	SensorInfo.Custom4DelayFrame = pInfo[IDNum]->Custom4DelayFrame;
	SensorInfo.Custom5DelayFrame = pInfo[IDNum]->Custom5DelayFrame;
	SensorInfo.YUVAwbDelayFrame = pInfo[IDNum]->YUVAwbDelayFrame;
	SensorInfo.YUVEffectDelayFrame = pInfo[IDNum]->YUVEffectDelayFrame;
	SensorInfo.SensorGrabStartX_PRV = pInfo[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_PRV = pInfo[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_CAP = pInfo1[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_CAP = pInfo1[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_VD = pInfo2[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_VD = pInfo2[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_VD1 = pInfo3[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_VD1 = pInfo3[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_VD2 = pInfo4[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_VD2 = pInfo4[IDNum]->SensorGrabStartY;
	SensorInfo.SensorDrivingCurrent = pInfo[IDNum]->SensorDrivingCurrent;
	SensorInfo.SensorMasterClockSwitch =
		pInfo[IDNum]->SensorMasterClockSwitch;
	SensorInfo.AEShutDelayFrame = pInfo[IDNum]->AEShutDelayFrame;
	SensorInfo.AESensorGainDelayFrame =
		pInfo[IDNum]->AESensorGainDelayFrame;
	SensorInfo.AEISPGainDelayFrame = pInfo[IDNum]->AEISPGainDelayFrame;
	SensorInfo.MIPIDataLowPwr2HighSpeedTermDelayCount =
		pInfo[IDNum]->MIPIDataLowPwr2HighSpeedTermDelayCount;
	SensorInfo.MIPIDataLowPwr2HighSpeedSettleDelayCount =
		pInfo[IDNum]->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	SensorInfo.MIPIDataLowPwr2HSSettleDelayM0 =
		pInfo[IDNum]->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	SensorInfo.MIPIDataLowPwr2HSSettleDelayM1 =
		pInfo1[IDNum]->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	SensorInfo.MIPIDataLowPwr2HSSettleDelayM2 =
		pInfo2[IDNum]->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	SensorInfo.MIPIDataLowPwr2HSSettleDelayM3 =
		pInfo3[IDNum]->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	SensorInfo.MIPIDataLowPwr2HSSettleDelayM4 =
		pInfo4[IDNum]->MIPIDataLowPwr2HighSpeedSettleDelayCount;
	SensorInfo.MIPICLKLowPwr2HighSpeedTermDelayCount =
		pInfo[IDNum]->MIPICLKLowPwr2HighSpeedTermDelayCount;
	SensorInfo.SensorWidthSampling = pInfo[IDNum]->SensorWidthSampling;
	SensorInfo.SensorHightSampling = pInfo[IDNum]->SensorHightSampling;
	SensorInfo.SensorPacketECCOrder = pInfo[IDNum]->SensorPacketECCOrder;
	SensorInfo.MIPIsensorType = pInfo[IDNum]->MIPIsensorType;
	SensorInfo.IHDR_LE_FirstLine = pInfo[IDNum]->IHDR_LE_FirstLine;
	SensorInfo.IHDR_Support = pInfo[IDNum]->IHDR_Support;
	SensorInfo.SensorModeNum = pInfo[IDNum]->SensorModeNum;
	SensorInfo.SettleDelayMode = pInfo[IDNum]->SettleDelayMode;
	SensorInfo.PDAF_Support = pInfo[IDNum]->PDAF_Support;
	SensorInfo.IMGSENSOR_DPCM_TYPE_PRE = pInfo[IDNum]->DPCM_INFO;
	SensorInfo.IMGSENSOR_DPCM_TYPE_CAP = pInfo1[IDNum]->DPCM_INFO;
	SensorInfo.IMGSENSOR_DPCM_TYPE_VD = pInfo2[IDNum]->DPCM_INFO;
	SensorInfo.IMGSENSOR_DPCM_TYPE_VD1 = pInfo3[IDNum]->DPCM_INFO;
	SensorInfo.IMGSENSOR_DPCM_TYPE_VD2 = pInfo4[IDNum]->DPCM_INFO;
	/*Per-Frame conrol support or not */
	SensorInfo.PerFrameCTL_Support = pInfo[IDNum]->PerFrameCTL_Support;
	/*SCAM number */
	SensorInfo.SCAM_DataNumber = pInfo[IDNum]->SCAM_DataNumber;
	SensorInfo.SCAM_DDR_En = pInfo[IDNum]->SCAM_DDR_En;
	SensorInfo.SCAM_CLK_INV = pInfo[IDNum]->SCAM_CLK_INV;
	/* TO get preview value */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CUSTOM1;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo, pConfig);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CUSTOM2;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo1, pConfig1);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CUSTOM3;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo2, pConfig2);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CUSTOM4;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo3, pConfig3);
	/*  */
	ScenarioId[0] = ScenarioId[1] = MSDK_SCENARIO_ID_CUSTOM5;
	g_pSensorFunc->SensorGetInfo(pScenarioId, pInfo4, pConfig4);
	/* To set sensor information */
	if (pSensorGetInfo->SensorId == DUAL_CAMERA_MAIN_SENSOR)
		IDNum = 0;
	else
		IDNum = 1;

	SensorInfo.SensorGrabStartX_CST1 = pInfo[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_CST1 = pInfo[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_CST2 = pInfo1[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_CST2 = pInfo1[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_CST3 = pInfo2[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_CST3 = pInfo2[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_CST4 = pInfo3[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_CST4 = pInfo3[IDNum]->SensorGrabStartY;
	SensorInfo.SensorGrabStartX_CST5 = pInfo4[IDNum]->SensorGrabStartX;
	SensorInfo.SensorGrabStartY_CST5 = pInfo4[IDNum]->SensorGrabStartY;

	if (copy_to_user
		((void __user *)(pSensorGetInfo->pInfo), (void *)(&SensorInfo),
		sizeof(struct ACDK_SENSOR_INFO2_STRUCT))) {
		PK_ERR("[CAMERA_HW][info] ioctl copy to user failed\n");
		return -EFAULT;
	}

	/* Step2 : Get Resolution */
	g_pSensorFunc->SensorGetResolution(psensorResolution);
	PK_DBG("[CAMERA_HW][Pre]w=0x%x, h = 0x%x\n",
		SensorResolution[0].SensorPreviewWidth,
		SensorResolution[0].SensorPreviewHeight);
	PK_DBG("[CAMERA_HW][Full]w=0x%x, h = 0x%x\n",
		SensorResolution[0].SensorFullWidth,
		SensorResolution[0].SensorFullHeight);
	PK_DBG("[CAMERA_HW][VD]w=0x%x, h = 0x%x\n",
		SensorResolution[0].SensorVideoWidth,
		SensorResolution[0].SensorVideoHeight);

	if (pSensorGetInfo->SensorId == DUAL_CAMERA_MAIN_SENSOR) {
		/* Resolution */
		PK_DBG("[%s] Resolution\n", __func__);
		if (copy_to_user
			((void __user *)(pSensorGetInfo->pSensorResolution),
			(void *)psensorResolution[0],
			sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT))) {
			PK_ERR("[Resolution] ioctl copy to user failed\n");
			return -EFAULT;
		}
	} else {
		/* Resolution */
		PK_DBG("Sub cam, copy_to_user\n");
		if (copy_to_user
			((void __user *)(pSensorGetInfo->pSensorResolution),
			(void *)psensorResolution[1],
			sizeof(MSDK_SENSOR_RESOLUTION_INFO_STRUCT))) {
			PK_ERR("[Resolution] ioctl copy to user failed\n");
			return -EFAULT;
		}
	}

	return 0;
}				/* adopt_CAMERA_HW_GetInfo() */


static inline int adopt_CAMERA_HW_Control(void *pBuf)
{
	int ret = 0;
	struct ACDK_SENSOR_CONTROL_STRUCT *pSensorCtrl =
		(struct ACDK_SENSOR_CONTROL_STRUCT *) pBuf;
	MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT imageWindow;
	MSDK_SENSOR_CONFIG_STRUCT sensorConfigData;

	memset(&imageWindow, 0,
		sizeof(struct ACDK_SENSOR_EXPOSURE_WINDOW_STRUCT));
	memset(&sensorConfigData, 0, sizeof(struct ACDK_SENSOR_CONFIG_STRUCT));

	if (pSensorCtrl == NULL) {
		PK_ERR("[CAMERA_HW] NULL arg.\n");
		return -EFAULT;
	}

	if (pSensorCtrl->pImageWindow == NULL ||
			pSensorCtrl->pSensorConfigData == NULL) {
		PK_ERR("[CAMERA_HW] NULL arg.\n");
		return -EFAULT;
	}

	if (copy_from_user
		((void *)&imageWindow, (void *)pSensorCtrl->pImageWindow,
		sizeof(struct ACDK_SENSOR_EXPOSURE_WINDOW_STRUCT))) {
		PK_ERR("[pFeatureData32] ioctl copy from user failed\n");
		return -EFAULT;
	}

	if (copy_from_user
		((void *)&sensorConfigData,
		(void *)pSensorCtrl->pSensorConfigData,
		sizeof(struct ACDK_SENSOR_CONFIG_STRUCT))) {
		PK_ERR("[pFeatureData32] ioctl copy from user failed\n");
		return -EFAULT;
	}

	/*  */
	if (g_pSensorFunc)
		ret =
			g_pSensorFunc->SensorControl(pSensorCtrl->InvokeCamera,
				pSensorCtrl->ScenarioId,
				&imageWindow, &sensorConfigData);
	else
		PK_ERR("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");


	/*  */
	if (copy_to_user
		((void __user *)pSensorCtrl->pImageWindow, (void *)&imageWindow,
		sizeof(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT))) {
		PK_ERR(
			"[CAMERA_HW][imageWindow] ioctl copy to user failed\n");
		return -EFAULT;
	}

	/*  */
	if (copy_to_user
		((void __user *)pSensorCtrl->pSensorConfigData,
		(void *)&sensorConfigData,
		sizeof(MSDK_SENSOR_CONFIG_STRUCT))) {
		PK_ERR(
			"[CAMERA_HW][imageWindow] ioctl copy to user failed\n");
		return -EFAULT;
	}
	return ret;
}				/* adopt_CAMERA_HW_Control */

static inline int adopt_CAMERA_HW_FeatureControl(void *pBuf)
{
	struct ACDK_SENSOR_FEATURECONTROL_STRUCT *pFeatureCtrl =
		(struct ACDK_SENSOR_FEATURECONTROL_STRUCT *) pBuf;
	unsigned int FeatureParaLen = 0;
	void *pFeaturePara = NULL;

	/* ACDK_SENSOR_GROUP_INFO_STRUCT *pSensorGroupInfo = NULL; */
	struct ACDK_KD_SENSOR_SYNC_STRUCT *pSensorSyncInfo = NULL;
	/* char kernelGroupNamePtr[128]; */
	/* unsigned char *pUserGroupNamePtr = NULL; */
	signed int ret = 0;

	if (pFeatureCtrl == NULL) {
		PK_ERR(" NULL arg.\n");
		return -EFAULT;
	}

	if (pFeatureCtrl->FeatureId == SENSOR_FEATURE_SINGLE_FOCUS_MODE ||
		pFeatureCtrl->FeatureId == SENSOR_FEATURE_CANCEL_AF ||
		pFeatureCtrl->FeatureId == SENSOR_FEATURE_CONSTANT_AF ||
		pFeatureCtrl->FeatureId == SENSOR_FEATURE_INFINITY_AF) {
	} else {
		if (pFeatureCtrl->pFeaturePara == NULL ||
				pFeatureCtrl->pFeatureParaLen == NULL) {
			PK_ERR(" NULL arg.\n");
			return -EFAULT;
		}
	}

	if (copy_from_user
		((void *)&FeatureParaLen, (void *)pFeatureCtrl->pFeatureParaLen,
		sizeof(unsigned int))) {
		PK_ERR(" ioctl copy from user failed\n");
		return -EFAULT;
	}

	pFeaturePara = kmalloc(FeatureParaLen, GFP_KERNEL);
	if (pFeaturePara == NULL) {
		PK_ERR(" ioctl allocate mem failed\n");
		return -ENOMEM;
	}
	memset(pFeaturePara, 0x0, FeatureParaLen);

	/* copy from user */
	switch (pFeatureCtrl->FeatureId) {
	case SENSOR_FEATURE_SET_ESHUTTER:
		/* FALLTHROUGH */
	case SENSOR_FEATURE_SET_GAIN:
		/* reset the delay frame flag */
		spin_lock(&kdsensor_drv_lock);
		g_NewSensorExpGain.uSensorExpDelayFrame = 0xFF;
		g_NewSensorExpGain.uSensorGainDelayFrame = 0xFF;
		g_NewSensorExpGain.uISPGainDelayFrame = 0xFF;
		spin_unlock(&kdsensor_drv_lock);
		/* FALLTHROUGH */
	case SENSOR_FEATURE_SET_ISP_MASTER_CLOCK_FREQ:
		/* FALLTHROUGH */
	case SENSOR_FEATURE_SET_REGISTER:
	case SENSOR_FEATURE_GET_REGISTER:
	case SENSOR_FEATURE_SET_CCT_REGISTER:
	case SENSOR_FEATURE_SET_ENG_REGISTER:
	case SENSOR_FEATURE_SET_ITEM_INFO:
	case SENSOR_FEATURE_GET_ITEM_INFO:
	case SENSOR_FEATURE_GET_ENG_INFO:
	case SENSOR_FEATURE_SET_VIDEO_MODE:
	case SENSOR_FEATURE_SET_YUV_CMD:
	case SENSOR_FEATURE_MOVE_FOCUS_LENS:
	case SENSOR_FEATURE_SET_AF_WINDOW:
	case SENSOR_FEATURE_SET_CALIBRATION_DATA:
	case SENSOR_FEATURE_SET_AUTO_FLICKER_MODE:
	case SENSOR_FEATURE_GET_EV_AWB_REF:
	case SENSOR_FEATURE_GET_SHUTTER_GAIN_AWB_GAIN:
	case SENSOR_FEATURE_SET_AE_WINDOW:
	case SENSOR_FEATURE_GET_EXIF_INFO:
	case SENSOR_FEATURE_GET_DELAY_INFO:
	case SENSOR_FEATURE_GET_AE_AWB_LOCK_INFO:
	case SENSOR_FEATURE_SET_MAX_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_SET_TEST_PATTERN:
	case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE:
	case SENSOR_FEATURE_GET_SENSOR_ID:
	case SENSOR_FEATURE_SET_OB_LOCK:
	case SENSOR_FEATURE_SET_SENSOR_OTP_AWB_CMD:
	case SENSOR_FEATURE_SET_SENSOR_OTP_LSC_CMD:
	case SENSOR_FEATURE_GET_TEMPERATURE_VALUE:
	case SENSOR_FEATURE_SET_FRAMERATE:
	case SENSOR_FEATURE_SET_HDR:
	case SENSOR_FEATURE_GET_CROP_INFO:
	case SENSOR_FEATURE_GET_VC_INFO:
	case SENSOR_FEATURE_SET_IHDR_SHUTTER_GAIN:
	case SENSOR_FEATURE_SET_HDR_SHUTTER:
	case SENSOR_FEATURE_GET_AE_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_GET_TRIGGER_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_SET_YUV_3A_CMD:
	case SENSOR_FEATURE_SET_AWB_GAIN:
	case SENSOR_FEATURE_SET_MIN_MAX_FPS:
	case SENSOR_FEATURE_GET_PDAF_INFO:
	case SENSOR_FEATURE_GET_PDAF_DATA:
	case SENSOR_FEATURE_GET_SENSOR_PDAF_CAPACITY:
		/*  */
		if (copy_from_user
			((void *)pFeaturePara,
			(void *)pFeatureCtrl->pFeaturePara, FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_ERR(
				"[pFeaturePara] ioctl copy from user failed\n");
			return -EFAULT;
		}
		break;
	case SENSOR_FEATURE_SET_SENSOR_SYNC:
		if (copy_from_user
			((void *)pFeaturePara,
			(void *)pFeatureCtrl->pFeaturePara, FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_ERR(
				"[pFeaturePara] ioctl copy from user failed\n");
			return -EFAULT;
		}
		/* keep the information to wait Vsync synchronize */
		pSensorSyncInfo =
			(struct ACDK_KD_SENSOR_SYNC_STRUCT *) pFeaturePara;
		spin_lock(&kdsensor_drv_lock);
		g_NewSensorExpGain.u2SensorNewExpTime =
			pSensorSyncInfo->u2SensorNewExpTime;
		g_NewSensorExpGain.u2SensorNewGain =
			pSensorSyncInfo->u2SensorNewGain;
		g_NewSensorExpGain.u2ISPNewRGain =
			pSensorSyncInfo->u2ISPNewRGain;
		g_NewSensorExpGain.u2ISPNewGrGain =
			pSensorSyncInfo->u2ISPNewGrGain;
		g_NewSensorExpGain.u2ISPNewGbGain =
			pSensorSyncInfo->u2ISPNewGbGain;
		g_NewSensorExpGain.u2ISPNewBGain =
			pSensorSyncInfo->u2ISPNewBGain;
		g_NewSensorExpGain.uSensorExpDelayFrame =
			pSensorSyncInfo->uSensorExpDelayFrame;
		g_NewSensorExpGain.uSensorGainDelayFrame =
			pSensorSyncInfo->uSensorGainDelayFrame;
		g_NewSensorExpGain.uISPGainDelayFrame =
			pSensorSyncInfo->uISPGainDelayFrame;
		/* AE smooth not change shutter to speed up */
		if ((g_NewSensorExpGain.u2SensorNewExpTime == 0)
			|| (g_NewSensorExpGain.u2SensorNewExpTime == 0xFFFF))
			g_NewSensorExpGain.uSensorExpDelayFrame = 0xFF;

		if (g_NewSensorExpGain.uSensorExpDelayFrame == 0) {
			FeatureParaLen = 2;
			g_pSensorFunc->SensorFeatureControl(
				pFeatureCtrl->InvokeCamera,
				SENSOR_FEATURE_SET_ESHUTTER,
				(unsigned char *)&g_NewSensorExpGain.
				u2SensorNewExpTime,
				(unsigned int *)&FeatureParaLen);
			g_NewSensorExpGain.uSensorExpDelayFrame = 0xFF;
		} else if (g_NewSensorExpGain.uSensorExpDelayFrame != 0xFF)
			g_NewSensorExpGain.uSensorExpDelayFrame--;

		/* exposure gain */
		if (g_NewSensorExpGain.uSensorGainDelayFrame == 0) {
			FeatureParaLen = 2;
			g_pSensorFunc->SensorFeatureControl(
				pFeatureCtrl->InvokeCamera,
				SENSOR_FEATURE_SET_GAIN,
				(unsigned char *)&g_NewSensorExpGain.
				u2SensorNewGain,
				(unsigned int *)&FeatureParaLen);
			g_NewSensorExpGain.uSensorGainDelayFrame = 0xFF;
		} else if (g_NewSensorExpGain.uSensorGainDelayFrame != 0xFF)
			g_NewSensorExpGain.uSensorGainDelayFrame--;

		/* if the delay frame is 0 or 0xFF, stop to count */
		if ((g_NewSensorExpGain.uISPGainDelayFrame != 0xFF)
			&& (g_NewSensorExpGain.uISPGainDelayFrame != 0))
			g_NewSensorExpGain.uISPGainDelayFrame--;

		break;
#if 0
	case SENSOR_FEATURE_GET_GROUP_INFO:
		if (copy_from_user
			((void *)pFeaturePara,
			(void *)pFeatureCtrl->pFeaturePara,
			FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_ERR("[pFeaturePara] ioctl copy from user failed\n");
			return -EFAULT;
		}
		pSensorGroupInfo =
			(ACDK_SENSOR_GROUP_INFO_STRUCT *) pFeaturePara;
		pUserGroupNamePtr = pSensorGroupInfo->GroupNamePtr;
		/*  */
		if (pUserGroupNamePtr == NULL) {
			kfree(pFeaturePara);
			PK_ERR("[CAMERA_HW] NULL arg.\n");
			return -EFAULT;
		}
		pSensorGroupInfo->GroupNamePtr = kernelGroupNamePtr;
		break;
#endif
	case SENSOR_FEATURE_SET_ESHUTTER_GAIN:
		if (copy_from_user((void *)pFeaturePara,
			(void *)pFeatureCtrl->pFeaturePara,
			FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_ERR("[pFeaturePara] ioctl copy from user failed\n");
			return -EFAULT;
		}
		/* keep the information to wait Vsync synchronize */
		pSensorSyncInfo =
			(struct ACDK_KD_SENSOR_SYNC_STRUCT *) pFeaturePara;
		spin_lock(&kdsensor_drv_lock);
		g_NewSensorExpGain.u2SensorNewExpTime =
			pSensorSyncInfo->u2SensorNewExpTime;
		g_NewSensorExpGain.u2SensorNewGain =
			pSensorSyncInfo->u2SensorNewGain;
		spin_unlock(&kdsensor_drv_lock);
		kdSetExpGain(pFeatureCtrl->InvokeCamera);
		break;
		/* copy to user */
	case SENSOR_FEATURE_GET_RESOLUTION:
	case SENSOR_FEATURE_GET_PERIOD:
	case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
	case SENSOR_FEATURE_GET_REGISTER_DEFAULT:
	case SENSOR_FEATURE_GET_CONFIG_PARA:
	case SENSOR_FEATURE_GET_GROUP_COUNT:
	case SENSOR_FEATURE_GET_LENS_DRIVER_ID:
		/* do nothing */
	case SENSOR_FEATURE_CAMERA_PARA_TO_SENSOR:
	case SENSOR_FEATURE_SENSOR_TO_CAMERA_PARA:
	case SENSOR_FEATURE_SINGLE_FOCUS_MODE:
	case SENSOR_FEATURE_CANCEL_AF:
	case SENSOR_FEATURE_CONSTANT_AF:
	default:
		break;
	}

	/*  */
	switch (pFeatureCtrl->FeatureId) {
	case SENSOR_FEATURE_GET_CROP_INFO:
		{
			struct SENSOR_WINSIZE_INFO_STRUCT *pCrop = NULL;
			unsigned long long *pFeaturePara_64 = (unsigned long long *)pFeaturePara;
			void *usr_ptr = (void *)(uintptr_t) (*(pFeaturePara_64 + 1));

			PK_ERR(" get_crop_info \n");
			pCrop = kmalloc(sizeof(struct SENSOR_WINSIZE_INFO_STRUCT), GFP_KERNEL);
			if (pCrop == NULL) {
				PK_ERR(" ioctl allocate mem failed\n");
				kfree(pFeaturePara);
				return -ENOMEM;
			}
			memset(pCrop, 0x0, sizeof(struct SENSOR_WINSIZE_INFO_STRUCT));
			*(pFeaturePara_64 + 1) = (uintptr_t)pCrop;
			if (g_pSensorFunc) {
				ret =
				    g_pSensorFunc->SensorFeatureControl(pFeatureCtrl->InvokeCamera,
									pFeatureCtrl->FeatureId,
									(unsigned char *)
									pFeaturePara,
									(unsigned int *)
									&FeatureParaLen);
			} else {
				PK_DBG("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");
			}
			if (copy_to_user
			    ((void __user *)usr_ptr, (void *)pCrop,
			     sizeof(struct SENSOR_WINSIZE_INFO_STRUCT))) {
				PK_DBG("[CAMERA_HW]ERROR: copy_to_user fail\n");
			}
			kfree(pCrop);
			*(pFeaturePara_64 + 1) = (uintptr_t)usr_ptr;
		}
		break;
	default:
		if (g_pSensorFunc) {
			ret =
				g_pSensorFunc->SensorFeatureControl(
				pFeatureCtrl->InvokeCamera, pFeatureCtrl->FeatureId,
				(unsigned char *)pFeaturePara,
				(unsigned int *)&FeatureParaLen);
		} else {
			PK_ERR("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");
		}
		break;
	}

	/* copy to user */
	switch (pFeatureCtrl->FeatureId) {
	case SENSOR_FEATURE_SET_ESHUTTER:
	case SENSOR_FEATURE_SET_GAIN:
	case SENSOR_FEATURE_SET_GAIN_AND_ESHUTTER:
	case SENSOR_FEATURE_SET_ISP_MASTER_CLOCK_FREQ:
	case SENSOR_FEATURE_SET_REGISTER:
	case SENSOR_FEATURE_SET_CCT_REGISTER:
	case SENSOR_FEATURE_SET_ENG_REGISTER:
	case SENSOR_FEATURE_SET_ITEM_INFO:
		/* do nothing */
	case SENSOR_FEATURE_CAMERA_PARA_TO_SENSOR:
	case SENSOR_FEATURE_SENSOR_TO_CAMERA_PARA:
	case SENSOR_FEATURE_GET_PDAF_DATA:
		break;
		/* copy to user */
	case SENSOR_FEATURE_GET_EV_AWB_REF:
	case SENSOR_FEATURE_GET_SHUTTER_GAIN_AWB_GAIN:
	case SENSOR_FEATURE_GET_EXIF_INFO:
	case SENSOR_FEATURE_GET_DELAY_INFO:
	case SENSOR_FEATURE_GET_AE_AWB_LOCK_INFO:
	case SENSOR_FEATURE_GET_RESOLUTION:
	case SENSOR_FEATURE_GET_PERIOD:
	case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:
	case SENSOR_FEATURE_GET_REGISTER:
	case SENSOR_FEATURE_GET_REGISTER_DEFAULT:
	case SENSOR_FEATURE_GET_CONFIG_PARA:
	case SENSOR_FEATURE_GET_GROUP_COUNT:
	case SENSOR_FEATURE_GET_LENS_DRIVER_ID:
	case SENSOR_FEATURE_GET_ITEM_INFO:
	case SENSOR_FEATURE_GET_ENG_INFO:
	case SENSOR_FEATURE_GET_AF_STATUS:
	case SENSOR_FEATURE_GET_AE_STATUS:
	case SENSOR_FEATURE_GET_AWB_STATUS:
	case SENSOR_FEATURE_GET_AF_INF:
	case SENSOR_FEATURE_GET_AF_MACRO:
	case SENSOR_FEATURE_GET_AF_MAX_NUM_FOCUS_AREAS:
	case SENSOR_FEATURE_GET_TRIGGER_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_SET_YUV_3A_CMD:
	case SENSOR_FEATURE_GET_AE_FLASHLIGHT_INFO:
	case SENSOR_FEATURE_GET_AE_MAX_NUM_METERING_AREAS:
	case SENSOR_FEATURE_CHECK_SENSOR_ID:
	case SENSOR_FEATURE_GET_DEFAULT_FRAME_RATE_BY_SCENARIO:
	case SENSOR_FEATURE_SET_TEST_PATTERN:
	case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE:
	case SENSOR_FEATURE_GET_SENSOR_ID:
	case SENSOR_FEATURE_GET_TEMPERATURE_VALUE:
	case SENSOR_FEATURE_SET_FRAMERATE:
	case SENSOR_FEATURE_SET_HDR:
	case SENSOR_FEATURE_SET_IHDR_SHUTTER_GAIN:
	case SENSOR_FEATURE_SET_HDR_SHUTTER:
	case SENSOR_FEATURE_GET_CROP_INFO:
	case SENSOR_FEATURE_GET_VC_INFO:
	case SENSOR_FEATURE_SET_MIN_MAX_FPS:
	case SENSOR_FEATURE_GET_PDAF_INFO:
	case SENSOR_FEATURE_GET_SENSOR_PDAF_CAPACITY:
		/*  */
		if (copy_to_user((void __user *)pFeatureCtrl->pFeaturePara,
			(void *)pFeaturePara, FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_ERR(
				"[pSensorRegData] ioctl copy to user failed\n");
			return -EFAULT;
		}
		break;
#if 0
		/* copy from and to user */
	case SENSOR_FEATURE_GET_GROUP_INFO:
		/* copy 32 bytes */
		if (copy_to_user((void __user *)pUserGroupNamePtr,
			(void *)kernelGroupNamePtr,
			sizeof(char) * 32)) {
			kfree(pFeaturePara);
			PK_ERR("[CAMERA_HW] ioctl copy to user failed\n");
			return -EFAULT;
		}
		pSensorGroupInfo->GroupNamePtr = pUserGroupNamePtr;
		if (copy_to_user((void __user *)pFeatureCtrl->pFeaturePara,
			(void *)pFeaturePara, FeatureParaLen)) {
			kfree(pFeaturePara);
			PK_ERR("[CAMERA_HW] ioctl copy to user failed\n");
			return -EFAULT;
		}
		break;
#endif
	default:
		break;
	}

	kfree(pFeaturePara);
	if (copy_to_user((void __user *)pFeatureCtrl->pFeatureParaLen,
		(void *)&FeatureParaLen,
		sizeof(unsigned int))) {
		PK_ERR("[CAMERA_HW] ioctl copy to user failed\n");
		return -EFAULT;
	}
	return ret;
}				/* adopt_CAMERA_HW_FeatureControl() */


static inline int adopt_CAMERA_HW_Close(void)
{
	/* if (atomic_read(&g_CamHWOpend) == 0) { */
	/* return 0; */
	/* } */
	/* else if(atomic_read(&g_CamHWOpend) == 1) { */
	if (g_pSensorFunc)
		g_pSensorFunc->SensorClose();
	else
		PK_ERR("[CAMERA_HW]ERROR:NULL g_pSensorFunc\n");

	atomic_set(&g_CamHWOpening, 0);

	/* reset the delay frame flag */
	spin_lock(&kdsensor_drv_lock);
	g_NewSensorExpGain.uSensorExpDelayFrame = 0xFF;
	g_NewSensorExpGain.uSensorGainDelayFrame = 0xFF;
	g_NewSensorExpGain.uISPGainDelayFrame = 0xFF;
	spin_unlock(&kdsensor_drv_lock);

	return 0;
}				/* adopt_CAMERA_HW_Close() */

#ifdef MTKCAM_USING_CCF

static inline void Get_ccf_clk(struct platform_device *pdev)
{
	if (pdev == NULL) {
		PK_ERR("[%s] pdev is null\n", __func__);
		return;
	}
	/* get all possible using clocks */
	g_camclk_camtg_sel = devm_clk_get(&pdev->dev, "TOP_CAMTG_SEL");
	if (IS_ERR(g_camclk_camtg_sel))
		PK_ERR("get g_camclk_camtg_sel : invalid...\n");
	g_camclk_univpll_d26 = devm_clk_get(&pdev->dev, "TOP_UNIVPLL_D26");
	if (IS_ERR(g_camclk_univpll_d26))
		PK_ERR("get g_camclk_univpll_d26 : invalid...\n");
	g_camclk_univpll2_d2 = devm_clk_get(&pdev->dev, "TOP_UNIVPLL2_D2");
	if (IS_ERR(g_camclk_univpll2_d2))
		PK_ERR("get g_camclk_univpll2_d2 : invalid...\n");

}

static inline void Check_ccf_clk(void)
{
	if (IS_ERR(g_camclk_camtg_sel))
		PK_ERR("g_camclk_camtg_sel invalid...\n");
	if (IS_ERR(g_camclk_univpll_d26))
		PK_ERR("get g_camclk_univpll_d26 : invalid...\n");
	if (IS_ERR(g_camclk_univpll2_d2))
		PK_ERR("get g_camclk_univpll2_d2 : invalid...\n");
}

#endif

static inline int kdSetSensorMclk(int *pBuf)
{


/* #ifndef CONFIG_ARM64 */
	int ret = 0;

	struct ACDK_SENSOR_MCLK_STRUCT *pSensorCtrl =
		(struct ACDK_SENSOR_MCLK_STRUCT *) pBuf;

	PK_DBG("[CAMERA SENSOR] %s on=%d, freq= %d\n", __func__,
		pSensorCtrl->on, pSensorCtrl->freq);
#ifdef MTKCAM_USING_CCF
	PK_DBG("========= MTKCAM_USING_CCF =======\n");
	Check_ccf_clk();
	if (pSensorCtrl->on == 1) {
		clk_prepare_enable(g_camclk_camtg_sel);
		if (pSensorCtrl->freq == 1 /*CAM_PLL_48_GROUP */)
			clk_set_parent(g_camclk_camtg_sel,
				g_camclk_univpll_d26);
		else if (pSensorCtrl->freq == 2 /*CAM_PLL_52_GROUP */)
			clk_set_parent(g_camclk_camtg_sel,
				g_camclk_univpll2_d2);
	} else {
		clk_disable_unprepare(g_camclk_camtg_sel);
	}
	return ret;

#else
	PK_DBG("========= Old Clock =======\n");

	if (pSensorCtrl->on == 1) {
		enable_mux(MT_MUX_CAMTG, "CAMERA_SENSOR");
		clkmux_sel(MT_MUX_CAMTG, pSensorCtrl->freq, "CAMERA_SENSOR");
	} else {

		disable_mux(MT_MUX_CAMTG, "CAMERA_SENSOR");
	}
	return ret;
#endif
/* #endif */
}

static inline int kdSetSensorGpio(int *pBuf)
{
#ifndef GPIO_CMDAT0
#define GPIO_CMDAT0		(GPIO42 | 0x80000000)
#define GPIO_CMDAT1		(GPIO43 | 0x80000000)
#define GPIO_CMPCLK		(GPIO44 | 0x80000000)
#endif
#ifndef GPIO_CMDAT0_M_CMDAT
#define GPIO_CMDAT0_M_CMDAT	(GPIO_MODE_01)
#define GPIO_CMDAT1_M_CMDAT	(GPIO_MODE_01)
#define GPIO_CMPCLK_M_CLK	(GPIO_MODE_01)
#define GPIO_CMPCLK_M_GPIO	(GPIO_MODE_00)
#endif
	int ret = 0;
	struct IMGSENSOR_GPIO_STRUCT *pSensorgpio =
		(struct IMGSENSOR_GPIO_STRUCT *) pBuf;

	PK_INFO("[CAMERA SENSOR] kdSetSensorGpio enable=%d, type=%d\n",
		pSensorgpio->GpioEnable, pSensorgpio->SensroInterfaceType);
#if 0/*defined CONFIG_MTK_LEGACY*/
#ifndef CONFIG_MTK_FPGA
	if (pSensorgpio->SensroInterfaceType == SENSORIF_PARALLEL) {
		if (pSensorgpio->GpioEnable == 1) {
			mt_set_gpio_mode(GPIO_CAMERA_RDP0_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_CMHSYNC);
			mt_set_gpio_mode(GPIO_CAMERA_RDN0_A_PIN,
				GPIO_CAMERA_RDP0_A_PIN_M_CMVSYNC);
			mt_set_gpio_mode(GPIO_CAMERA_RDP1_A_PIN,
				GPIO_CAMERA_RDN1_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RDN1_A_PIN,
				GPIO_CAMERA_RDP1_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RCP_A_PIN,
				GPIO_CAMERA_RCN_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RCN_A_PIN,
				GPIO_CAMERA_RCP_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RDP2_A_PIN,
				GPIO_CAMERA_RDN2_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RDN2_A_PIN,
				GPIO_CAMERA_RDP2_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RDP3_A_PIN,
				GPIO_CAMERA_RDN3_A_PIN_M_CMDAT);
			mt_set_gpio_mode(GPIO_CAMERA_RDN3_A_PIN,
				GPIO_CAMERA_RDP3_A_PIN_M_CMDAT);

			if (pSensorgpio->SensorIndataformat == DATA_10BIT_FMT) {
				mt_set_gpio_mode(GPIO_CMDAT0,
					GPIO_CMDAT0_M_CMDAT);
				mt_set_gpio_mode(GPIO_CMDAT1,
					GPIO_CMDAT1_M_CMDAT);
			}
			mt_set_gpio_mode(GPIO_CMPCLK, GPIO_CMPCLK_M_CLK);
		} else {
			mt_set_gpio_mode(GPIO_CAMERA_RDP0_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_RDN0_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN0_A_PIN,
				GPIO_CAMERA_RDP0_A_PIN_M_RDP0_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDP1_A_PIN,
				GPIO_CAMERA_RDN1_A_PIN_M_RDN1_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN1_A_PIN,
				GPIO_CAMERA_RDP1_A_PIN_M_RDP1_A);
			mt_set_gpio_mode(GPIO_CAMERA_RCP_A_PIN,
				GPIO_CAMERA_RCN_A_PIN_M_RCN_A);
			mt_set_gpio_mode(GPIO_CAMERA_RCN_A_PIN,
				GPIO_CAMERA_RCP_A_PIN_M_RCP_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDP2_A_PIN,
				GPIO_CAMERA_RDN2_A_PIN_M_RDN2_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN2_A_PIN,
				GPIO_CAMERA_RDP2_A_PIN_M_RDP2_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDP3_A_PIN,
				GPIO_CAMERA_RDN3_A_PIN_M_RDN3_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN3_A_PIN,
				GPIO_CAMERA_RDP3_A_PIN_M_RDP3_A);
			mt_set_gpio_mode(GPIO_CMDAT0, GPIO_CMDAT0_M_CMDAT);
			mt_set_gpio_mode(GPIO_CMDAT1, GPIO_CMDAT1_M_CMDAT);
			mt_set_gpio_mode(GPIO_CMPCLK, GPIO_CMPCLK_M_GPIO);
		}
	} else if (pSensorgpio->SensroInterfaceType == SENSORIF_SERIAL) {
		if (pSensorgpio->GpioEnable == 1) {
			mt_set_gpio_mode(GPIO_CAMERA_RDP0_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_CMCSD);
			mt_set_gpio_mode(GPIO_CAMERA_RDN0_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_CMCSD);
			mt_set_gpio_mode(GPIO_CAMERA_RDP1_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_CMCSD);
			mt_set_gpio_mode(GPIO_CAMERA_RDN1_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_CMCSD);
			mt_set_gpio_mode(GPIO_CMPCLK, GPIO_CMPCLK_M_CMCSK);
		} else {
			mt_set_gpio_mode(GPIO_CAMERA_RDP0_A_PIN,
				GPIO_CAMERA_RDN0_A_PIN_M_RDN0_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN0_A_PIN,
				GPIO_CAMERA_RDP0_A_PIN_M_RDP0_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDP1_A_PIN,
				GPIO_CAMERA_RDN1_A_PIN_M_RDN1_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN1_A_PIN,
				GPIO_CAMERA_RDP1_A_PIN_M_RDP1_A);
			mt_set_gpio_mode(GPIO_CAMERA_RCP_A_PIN,
				GPIO_CAMERA_RCN_A_PIN_M_RCN_A);
			mt_set_gpio_mode(GPIO_CAMERA_RCN_A_PIN,
				GPIO_CAMERA_RCP_A_PIN_M_RCP_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDP2_A_PIN,
				GPIO_CAMERA_RDN2_A_PIN_M_RDN2_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN2_A_PIN,
				GPIO_CAMERA_RDP2_A_PIN_M_RDP2_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDP3_A_PIN,
				GPIO_CAMERA_RDN3_A_PIN_M_RDN3_A);
			mt_set_gpio_mode(GPIO_CAMERA_RDN3_A_PIN,
				GPIO_CAMERA_RDP3_A_PIN_M_RDP3_A);
			mt_set_gpio_mode(GPIO_CMDAT0, GPIO_CMDAT0_M_CMDAT);
			mt_set_gpio_mode(GPIO_CMDAT1, GPIO_CMDAT1_M_CMDAT);
			mt_set_gpio_mode(GPIO_CMPCLK, GPIO_CMPCLK_M_GPIO);
		}
	}
#endif
#endif/*End of mtk legacy*/
	return ret;
}


#if 0 /*!defined(CONFIG_MTK_LEGACY)*/
bool Get_Cam_Regulator(void)
{
	const char *name = NULL;
	struct device_node *node = NULL, *kd_node;

	if (1) {
		/* check if customer camera node defined */
		node = of_find_compatible_node(NULL, NULL,
			"mediatek,mt8163-camera_hw");
		if (node) {
			name = of_get_property(node, "vcama_sub", NULL);
			if (name == NULL) {
				if (regVCAMA == NULL)
					regVCAMA = regulator_get(sensor_device,
						"vcama");
				if (regVCAMD == NULL)
					regVCAMD = regulator_get(sensor_device,
						"vcamd");
				if (regVCAMIO == NULL)
					regVCAMIO = regulator_get(sensor_device,
						"vcamio");
				if (regVCAMAF == NULL)
					regVCAMAF = regulator_get(sensor_device,
						"vcamaf");
			} else {
				PK_DBG(
					"Camera customer regulator name =%s!\n",
					name);
				/* backup original dev.of_node */
				kd_node = sensor_device->of_node;
				sensor_device->of_node =
					of_find_compatible_node(NULL, NULL,
					"mediatek,mt8163-camera_hw");
				#if 0
				/* you can add sub cam */
				if (regVCAMA == NULL)
					regVCAMA_SUB =
						regulator_get(sensor_device,
						"SUB_CAMERA_POWER_A");
				#endif
				if (regVCAMA == NULL)
					regVCAMA =
						regulator_get(sensor_device,
						"vcama");
				if (regVCAMD == NULL)
					regVCAMD =
						regulator_get(sensor_device,
						"vcamd");
				if (regSubVCAMD == NULL)
					regSubVCAMD =
						regulator_get(sensor_device,
						"vcamd_sub");
				if (regVCAMIO == NULL)
					regVCAMIO =
						regulator_get(sensor_device,
						"vcamio");
				if (regVCAMAF == NULL)
					regVCAMAF =
						regulator_get(sensor_device,
						"vcamaf");
				/* restore original dev.of_node */
				sensor_device->of_node = kd_node;
			}
		} else {
			PK_DBG("regulator get cust camera node failed!\n");
			return FALSE;
		}

		return TRUE;
	}
	return FALSE;
}

bool _hwPowerOn(KD_REGULATOR_TYPE_T type, int powerVolt)
{
	bool ret = FALSE;
	struct regulator *reg = NULL;

	if (type == VCAMA)
		reg = regVCAMA;
	else if (type == VCAMD)
		reg = regVCAMD;
	else if (type == VCAMIO)
		reg = regVCAMIO;
	else if (type == VCAMAF)
		reg = regVCAMAF;
	else
		return ret;

	if (reg != NULL && !IS_ERR(reg)) {
		if (regulator_set_voltage(reg, powerVolt, powerVolt) != 0) {
			PK_ERR("%s fail to regulator_set_voltage, powertype:%d powerId:%d\n",
				__func__, type, powerVolt);
			return ret;
		}
		if (regulator_enable(reg) != 0) {
			PK_ERR("%s fail to regulator_enable, powertype:%d powerId:%d\n",
				__func__, type, powerVolt);
			return ret;
		}
		ret = true;
	} else {
		PK_ERR("[%s] IS_ERR_OR_NULL powertype:%d\n", __func__, type);
		return ret;
	}

	return ret;
}

bool _hwPowerDown(KD_REGULATOR_TYPE_T type)
{
	bool ret = FALSE;
	struct regulator *reg = NULL;

	if (type == VCAMA)
		reg = regVCAMA;
	else if (type == VCAMD)
		reg = regVCAMD;
	else if (type == VCAMIO)
		reg = regVCAMIO;
	else if (type == VCAMAF)
		reg = regVCAMAF;
	else
		return ret;

	if (!IS_ERR(reg)) {
		if (regulator_is_enabled(reg) != 0)
			PK_ERR("[%s] %d is enabled\n", __func__, type);
		if (regulator_disable(reg) != 0) {
			PK_ERR("[%s] fail to regulator_disable, powertype: %d\n",
				__func__, type);
			return ret;
		}
		ret = true;
	} else {
		PK_ERR("[%s] %d fail to power down, due to regVCAM == NULL\n",
			__func__, type);
		return ret;
	}
	return ret;
}
#endif
/*******************************************************************************
 * Camera Regulator Power Control
 * vol = VOL_1800 = 1800
 ******************************************************************************/
#ifdef MTKCAM_USING_PWRREG
int cam_power_init(struct platform_device *pdev, int PinIdx)
{
	PK_DBG(" == MTKCAM_USING_PWRREG : cam_power_init(%d) ==", PinIdx);

	g_cam[PinIdx].vcama = devm_regulator_get(&pdev->dev, "reg-vcama");
	g_cam[PinIdx].vcamd = devm_regulator_get(&pdev->dev, "reg-vcamd");
	g_cam[PinIdx].vcamio = devm_regulator_get(&pdev->dev, "reg-vcamio");
	g_cam[PinIdx].vcamaf = devm_regulator_get(&pdev->dev, "reg-vcamaf");
	g_cam[PinIdx].vcami2c = devm_regulator_get(&pdev->dev, "reg-vcami2c");

	if (IS_ERR(g_cam[PinIdx].vcama)) {
		PK_ERR("Get regulator fail,[vcama] is null, errno:%ld",
			PTR_ERR(g_cam[PinIdx].vcama));
		return PTR_ERR(g_cam[PinIdx].vcama);
	}

	if (IS_ERR(g_cam[PinIdx].vcamd)) {
		PK_ERR("Get regulator fail,[vcamd] is null, errno:%ld",
			PTR_ERR(g_cam[PinIdx].vcamd));
		return PTR_ERR(g_cam[PinIdx].vcamd);
	}

	if (IS_ERR(g_cam[PinIdx].vcamio)) {
		PK_ERR("Get regulator fail,[vcamio] is null, errno:%ld",
			PTR_ERR(g_cam[PinIdx].vcamio));
		return PTR_ERR(g_cam[PinIdx].vcamio);
	}

	if (IS_ERR(g_cam[PinIdx].vcamaf)) {
		PK_ERR("Get regulator fail,[vcamaf] is null, errno:%ld",
			PTR_ERR(g_cam[PinIdx].vcamaf));
		return PTR_ERR(g_cam[PinIdx].vcamaf);
	}

	if (IS_ERR(g_cam[PinIdx].vcami2c)) {
		PK_ERR("Get regulator fail,[vcami2c] is null, errno:%ld",
			PTR_ERR(g_cam[PinIdx].vcami2c));
		return PTR_ERR(g_cam[PinIdx].vcami2c);
	}
	return 0;
}

bool CAMERA_Regulator_PowerOnOFF(struct regulator *pwrreg, BOOL IsOn, int vol)
{
	int ret = 0;
	int voltage_count = 0, high_bound_voltage = 0, low_bound_voltage = 0;

	PK_DBG("IsOn:%d , vol:%d", IsOn, vol);

	if (IS_ERR(pwrreg)) {
		PK_ERR("camera power regulator is null, ret:%ld",
			PTR_ERR(pwrreg));
		return FALSE;
	}

	if (IsOn) {		/* Power ON */

		voltage_count = regulator_count_voltages(pwrreg);
		if (voltage_count <= 0) {
			PK_ERR("Fails to count, voltage_count = %d",
				voltage_count);
			return FALSE;
		}

		high_bound_voltage = regulator_list_voltage(pwrreg,
			voltage_count - 1);
		PK_DBG("high_bound_voltage = %d", high_bound_voltage);

		if (high_bound_voltage <= 0) {
			PK_ERR("Fails to list, high_bound_voltage = %d",
				high_bound_voltage);
			return FALSE;
		}

		ret = regulator_set_voltage(pwrreg, vol, high_bound_voltage);
		if (ret != 0) {
			PK_ERR("Fails to set vol = %d , ret = 0x%x", vol, ret);
			return FALSE;
		}

		ret = regulator_enable(pwrreg);
		if (ret != 0) {
			PK_ERR("Fails to enabled, ret = 0x%x", ret);
			return FALSE;
		}
	} else {		/* Power OFF */
		voltage_count = regulator_count_voltages(pwrreg);
		if (voltage_count <= 0) {
			PK_ERR("Fails to count, voltage_count = %d",
				voltage_count);
			return FALSE;
		}

		high_bound_voltage = regulator_list_voltage(pwrreg,
			voltage_count - 1);
		PK_DBG("high_bound_voltage = %d", high_bound_voltage);

		if (high_bound_voltage <= 0) {
			PK_ERR("Fails to list, high_bound_voltage = %d",
				high_bound_voltage);
			return FALSE;
		}

		low_bound_voltage = regulator_list_voltage(pwrreg, 0);
		PK_DBG("low_bound_voltage = %d", low_bound_voltage);

		if (low_bound_voltage <= 0) {
			PK_ERR("Fails to list, low_bound_voltage = %d",
				low_bound_voltage);
			return FALSE;
		}
		ret = regulator_set_voltage(pwrreg, low_bound_voltage,
			high_bound_voltage);
		if (ret != 0) {
			PK_ERR("Fails to set High(%d) ~ Low(%d) voltage",
				high_bound_voltage, low_bound_voltage);
			return FALSE;
		}

		ret = regulator_disable(pwrreg);
		if (ret != 0) {
			PK_ERR("Fails to disable, ret = 0x%x", ret);
			return FALSE;
		}
	}

	return TRUE;
}

bool _hwPowerOn(int PinIdx, enum KD_REGULATOR_TYPE_T PwrType, int Voltage)
{
	struct regulator *pwr;

	PK_DBG("PinIndex=%d , PowerType = %d , Voltage = %d", PinIdx,
		PwrType, Voltage);

	if ((PinIdx != 0) && (PinIdx != 1)) {
		PK_ERR("PinIndex=%d is invalid\n", PinIdx);
		return FALSE;
	}

	switch (PwrType) {
	case VCAMA:
		pwr = g_cam[PinIdx].vcama;
		break;
	case VCAMD:
		pwr = g_cam[PinIdx].vcamd;
		break;
	case VCAMIO:
		pwr = g_cam[PinIdx].vcamio;
		break;
	case VCAMAF:
		pwr = g_cam[PinIdx].vcamaf;
		break;
	case VCAMI2C:
		pwr = g_cam[PinIdx].vcami2c;
		break;
	default:
		PK_ERR("PwrType=%d is invalid\n", PwrType);
		pwr = NULL;
		break;
	}
	if (pwr != NULL)
		return CAMERA_Regulator_PowerOnOFF(pwr, 1, Voltage);
	else
		return FALSE;
}
EXPORT_SYMBOL(_hwPowerOn);


bool _hwPowerDown(int PinIdx, enum KD_REGULATOR_TYPE_T PwrType)
{
	struct regulator *pwr;

	PK_DBG("PinIndex=%d , PowerType = %d", PinIdx, PwrType);

	if ((PinIdx != 0) && (PinIdx != 1)) {
		PK_ERR("PinIndex=%d is invalid\n", PinIdx);
		return FALSE;
	}

	switch (PwrType) {
	case VCAMA:
		pwr = g_cam[PinIdx].vcama;
		break;
	case VCAMD:
		pwr = g_cam[PinIdx].vcamd;
		break;
	case VCAMIO:
		pwr = g_cam[PinIdx].vcamio;
		break;
	case VCAMAF:
		pwr = g_cam[PinIdx].vcamaf;
		break;
	case VCAMI2C:
		pwr = g_cam[PinIdx].vcami2c;
		break;
	default:
		PK_ERR("PwrType=%d is invalid\n", PwrType);
		pwr = NULL;
		break;
	}
	if (pwr != NULL)
		return CAMERA_Regulator_PowerOnOFF(pwr, 0, 0);
	else
		return FALSE;
}
EXPORT_SYMBOL(_hwPowerDown);

#endif

#ifdef CONFIG_COMPAT

static int compat_get_acdk_sensor_getinfo_struct(
	struct COMPAT_ACDK_SENSOR_GETINFO_STRUCT __user *data32,
	struct ACDK_SENSOR_GETINFO_STRUCT __user *data)
{
	compat_uint_t i;
	compat_uptr_t p;
	int err;

	err = get_user(i, &data32->ScenarioId[0]);
	err |= put_user(i, &data->ScenarioId[0]);
	err = get_user(i, &data32->ScenarioId[1]);
	err |= put_user(i, &data->ScenarioId[1]);
	err = get_user(p, &data32->pInfo[0]);
	err |= put_user(compat_ptr(p), &data->pInfo[0]);
	err = get_user(p, &data32->pInfo[1]);
	err |= put_user(compat_ptr(p), &data->pInfo[1]);
	err = get_user(p, &data32->pInfo[0]);
	err |= put_user(compat_ptr(p), &data->pConfig[0]);
	err = get_user(p, &data32->pInfo[1]);
	err |= put_user(compat_ptr(p), &data->pConfig[1]);

	return err;
}

static int compat_put_acdk_sensor_getinfo_struct(
	struct COMPAT_ACDK_SENSOR_GETINFO_STRUCT __user *data32,
	struct ACDK_SENSOR_GETINFO_STRUCT __user *data)
{
	compat_uint_t i;
	int err;

	err = get_user(i, &data->ScenarioId[0]);
	err |= put_user(i, &data32->ScenarioId[0]);
	err = get_user(i, &data->ScenarioId[1]);
	err |= put_user(i, &data32->ScenarioId[1]);
	return err;
}

static int compat_get_imagesensor_getinfo_struct(
	struct COMPAT_IMAGESENSOR_GETINFO_STRUCT __user *data32,
	struct IMAGESENSOR_GETINFO_STRUCT __user *data)
{
	compat_uptr_t p;
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->SensorId);
	err |= put_user(i, &data->SensorId);
	err |= get_user(p, &data32->pInfo);
	err |= put_user(compat_ptr(p), &data->pInfo);
	err |= get_user(p, &data32->pSensorResolution);
	err |= put_user(compat_ptr(p), &data->pSensorResolution);
	return err;
}

static int compat_put_imagesensor_getinfo_struct(
	struct COMPAT_IMAGESENSOR_GETINFO_STRUCT __user *data32,
	struct IMAGESENSOR_GETINFO_STRUCT __user *data)
{
	/* compat_uptr_t p; */
	compat_uint_t i;
	int err;

	err = get_user(i, &data->SensorId);
	err |= put_user(i, &data32->SensorId);
	/* Assume pointer is not change */
	#if 0
	err |= get_user(p, &data->pInfo);
	err |= put_user(p, &data32->pInfo);
	err |= get_user(p, &data->pSensorResolution);
	err |= put_user(p, &data32->pSensorResolution);
	*/
	#endif
	return err;
}

static int compat_get_acdk_sensor_featurecontrol_struct(
	struct COMPAT_ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data32,
	struct ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data)
{
	compat_uptr_t p;
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->InvokeCamera);
	err |= put_user(i, &data->InvokeCamera);
	err |= get_user(i, &data32->FeatureId);
	err |= put_user(i, &data->FeatureId);
	err |= get_user(p, &data32->pFeaturePara);
	err |= put_user(compat_ptr(p), &data->pFeaturePara);
	err |= get_user(p, &data32->pFeatureParaLen);
	err |= put_user(compat_ptr(p), &data->pFeatureParaLen);
	return err;
}

static int compat_put_acdk_sensor_featurecontrol_struct(
	struct COMPAT_ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data32,
	struct ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data)
{
	MUINT8 *p;
	MUINT32 *q;
	compat_uint_t i;
	int err;

	err = get_user(i, &data->InvokeCamera);
	err |= put_user(i, &data32->InvokeCamera);
	err |= get_user(i, &data->FeatureId);
	err |= put_user(i, &data32->FeatureId);
	/* Assume pointer is not change */

	err |= get_user(p, &data->pFeaturePara);
	err |= put_user(ptr_to_compat(p), &data32->pFeaturePara);
	err |= get_user(q, &data->pFeatureParaLen);
	err |= put_user(ptr_to_compat(q), &data32->pFeatureParaLen);

	return err;
}

static int compat_get_acdk_sensor_control_struct(
	struct COMPAT_ACDK_SENSOR_CONTROL_STRUCT __user *data32,
	struct ACDK_SENSOR_CONTROL_STRUCT __user *data)
{
	compat_uptr_t p;
	compat_uint_t i;
	int err;

	err = get_user(i, &data32->InvokeCamera);
	err |= put_user(i, &data->InvokeCamera);
	err |= get_user(i, &data32->ScenarioId);
	err |= put_user(i, &data->ScenarioId);
	err |= get_user(p, &data32->pImageWindow);
	err |= put_user(compat_ptr(p), &data->pImageWindow);
	err |= get_user(p, &data32->pSensorConfigData);
	err |= put_user(compat_ptr(p), &data->pSensorConfigData);
	return err;
}

static int compat_put_acdk_sensor_control_struct(
	struct COMPAT_ACDK_SENSOR_CONTROL_STRUCT __user *data32,
	struct ACDK_SENSOR_CONTROL_STRUCT __user *data)
{
	/* compat_uptr_t p; */
	compat_uint_t i;
	int err;

	err = get_user(i, &data->InvokeCamera);
	err |= put_user(i, &data32->InvokeCamera);
	err |= get_user(i, &data->ScenarioId);
	err |= put_user(i, &data32->ScenarioId);
	/* Assume pointer is not change */
#if 0
	err |= get_user(p, &data->pImageWindow);
	err |= put_user(p, &data32->pImageWindow);
	err |= get_user(p, &data->pSensorConfigData);
	err |= put_user(p, &data32->pSensorConfigData);
#endif
	return err;
}

static int compat_get_acdk_sensor_resolution_info_struct(
	struct COMPAT_ACDK_SENSOR_PRESOLUTION_STRUCT __user *data32,
	struct ACDK_SENSOR_PRESOLUTION_STRUCT __user *data)
{
	int err;
	compat_uptr_t p;

	err = get_user(p, &data32->pResolution[0]);
	err |= put_user(compat_ptr(p), &data->pResolution[0]);
	err = get_user(p, &data32->pResolution[1]);
	err |= put_user(compat_ptr(p), &data->pResolution[1]);

	#if 0
	err = copy_from_user((void *)data, (void *)data32,
		sizeof(compat_uptr_t) * 2);
	err = copy_from_user((void *)data[0], (void *)data32[0],
		sizeof(struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT));
	err = copy_from_user((void *)data[1], (void *)data32[1],
		sizeof(struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT));
	#endif
	return err;
}

static int compat_put_acdk_sensor_resolution_info_struct(
	struct COMPAT_ACDK_SENSOR_PRESOLUTION_STRUCT __user *data32,
	struct ACDK_SENSOR_PRESOLUTION_STRUCT __user *data)
{
	int err = 0;
	#if 0
	err = copy_to_user((void *)data, (void *)data32,
		sizeof(compat_uptr_t) * 2);
	err = copy_to_user((void *)data[0], (void *)data32[0],
		sizeof(struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT));
	err = copy_to_user((void *)data[1], (void *)data32[1],
		sizeof(struct ACDK_SENSOR_RESOLUTION_INFO_STRUCT));
	#endif
	return err;
}



static long CAMERA_HW_Ioctl_Compat(struct file *filp,
	unsigned int cmd, unsigned long arg)
{
	long ret;

	if (!filp->f_op || !filp->f_op->unlocked_ioctl)
		return -ENOTTY;


	switch (cmd) {
	case COMPAT_KDIMGSENSORIOC_X_GETINFO:
		{
			struct COMPAT_ACDK_SENSOR_GETINFO_STRUCT __user *data32;
			struct ACDK_SENSOR_GETINFO_STRUCT __user *data;
			int err;

			PK_DBG("[CAMERA SENSOR] COMPAT_KDIMGSENSORIOC_X_GETINFO E\n");
			data32 = compat_ptr(arg);
			data = compat_alloc_user_space(sizeof(*data));
			if (data == NULL)
				return -EFAULT;

			err = compat_get_acdk_sensor_getinfo_struct(
				data32, data);
			if (err)
				return err;

			ret = filp->f_op->unlocked_ioctl(filp,
				KDIMGSENSORIOC_X_GETINFO,
				(unsigned long)data);
			err = compat_put_acdk_sensor_getinfo_struct(
				data32, data);

			if (err != 0)
				PK_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
			return ret;
		}
	case COMPAT_KDIMGSENSORIOC_X_FEATURECONCTROL:
		{
			struct COMPAT_ACDK_SENSOR_FEATURECONTROL_STRUCT
				__user *data32;
			struct ACDK_SENSOR_FEATURECONTROL_STRUCT __user *data;
			int err;

			PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_FEATURECONCTROL\n");
			data32 = compat_ptr(arg);
			data = compat_alloc_user_space(sizeof(*data));
			if (data == NULL)
				return -EFAULT;

			err = compat_get_acdk_sensor_featurecontrol_struct(
				data32, data);
			if (err)
				return err;

			ret = filp->f_op->unlocked_ioctl(
				filp, KDIMGSENSORIOC_X_FEATURECONCTROL,
				(unsigned long)data);
			err = compat_put_acdk_sensor_featurecontrol_struct(
				data32, data);


			if (err != 0)
				PK_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
			return ret;
		}
	case COMPAT_KDIMGSENSORIOC_X_CONTROL:
		{
			struct COMPAT_ACDK_SENSOR_CONTROL_STRUCT __user *data32;
			struct ACDK_SENSOR_CONTROL_STRUCT __user *data;
			int err;

			PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_CONTROL\n");
			data32 = compat_ptr(arg);
			data = compat_alloc_user_space(sizeof(*data));
			if (data == NULL)
				return -EFAULT;

			err = compat_get_acdk_sensor_control_struct(
				data32, data);
			if (err)
				return err;
			ret = filp->f_op->unlocked_ioctl(
				filp, KDIMGSENSORIOC_X_CONTROL,
				(unsigned long)data);
			err = compat_put_acdk_sensor_control_struct(
				data32, data);

			if (err != 0)
				PK_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
			return ret;
		}
	case COMPAT_KDIMGSENSORIOC_X_GETINFO2:
		{
			struct COMPAT_IMAGESENSOR_GETINFO_STRUCT __user *data32;
			struct IMAGESENSOR_GETINFO_STRUCT __user *data;
			int err;

			PK_DBG("[CAMERA SENSOR] CAOMPAT_KDIMGSENSORIOC_X_GETINFO2\n");

			data32 = compat_ptr(arg);
			data = compat_alloc_user_space(sizeof(*data));
			if (data == NULL)
				return -EFAULT;

			err = compat_get_imagesensor_getinfo_struct(
				data32, data);
			if (err)
				return err;
			ret = filp->f_op->unlocked_ioctl(
				filp, KDIMGSENSORIOC_X_GETINFO2,
				(unsigned long)data);
			err = compat_put_imagesensor_getinfo_struct(
				data32, data);

			if (err != 0)
				PK_ERR("[CAMERA SENSOR] compat_put_acdk_sensor_getinfo_struct failed\n");
			return ret;
		}
	case COMPAT_KDIMGSENSORIOC_X_GETRESOLUTION2:
		{

			struct COMPAT_ACDK_SENSOR_PRESOLUTION_STRUCT
				__user *data32;
			struct ACDK_SENSOR_PRESOLUTION_STRUCT __user *data;
			int err;

			PK_DBG("[CAMERA SENSOR] KDIMGSENSORIOC_X_GETRESOLUTION\n");
			data32 = compat_ptr(arg);
			data = compat_alloc_user_space(sizeof(data));
			if (data == NULL)
				return -EFAULT;
			PK_DBG("[CAMERA SENSOR] compat_get_acdk_sensor_resolution_info_struct\n");
			err = compat_get_acdk_sensor_resolution_info_struct(
				data32, data);
			if (err)
				return err;
			PK_DBG("[CAMERA SENSOR] unlocked_ioctl\n");
			ret = filp->f_op->unlocked_ioctl(
				filp, KDIMGSENSORIOC_X_GETRESOLUTION2,
				(unsigned long)data);

			err = compat_put_acdk_sensor_resolution_info_struct(
				data32, data);
			if (err != 0)
				PK_ERR("[CAMERA SENSOR] compat_get_acdk_sensor resolution_info_struct failed\n");
			return ret;
		}
	case KDIMGSENSORIOC_T_OPEN:
	case KDIMGSENSORIOC_T_CLOSE:
	case KDIMGSENSORIOC_T_CHECK_IS_ALIVE:
	case KDIMGSENSORIOC_X_SET_DRIVER:
	case KDIMGSENSORIOC_X_GET_SOCKET_POS:
	case KDIMGSENSORIOC_X_SET_I2CBUS:
	case KDIMGSENSORIOC_X_RELEASE_I2C_TRIGGER_LOCK:
	case KDIMGSENSORIOC_X_SET_SHUTTER_GAIN_WAIT_DONE:
	case KDIMGSENSORIOC_X_SET_MCLK_PLL:
	case KDIMGSENSORIOC_X_SET_CURRENT_SENSOR:
	case KDIMGSENSORIOC_X_SET_GPIO:
	case KDIMGSENSORIOC_X_GET_ISP_CLK:
		return filp->f_op->unlocked_ioctl(filp, cmd, arg);

	default:
		return -ENOIOCTLCMD;
	}
}


#endif

static long CAMERA_HW_Ioctl(struct file *a_pstFile,
	unsigned int a_u4Command, unsigned long a_u4Param)
{

	int i4RetValue = 0;
	void *pBuff = NULL;
	u32 *pIdx = NULL;

	mutex_lock(&kdCam_Mutex);

	if (_IOC_DIR(a_u4Command) == _IOC_NONE) {
		i4RetValue =  -EFAULT;
		PK_ERR("__IOC_NONE No such command\n");
		//goto CAMERA_HW_Ioctl_EXIT;
	} else {
		pBuff = kmalloc(_IOC_SIZE(a_u4Command), GFP_KERNEL);
		if (pBuff == NULL) {
			PK_ERR("[CAMERA SENSOR] ioctl allocate mem failed\n");
			i4RetValue = -ENOMEM;
			goto CAMERA_HW_Ioctl_EXIT;
		}

		if (_IOC_WRITE & _IOC_DIR(a_u4Command)) {
			if (copy_from_user(pBuff, (void *)a_u4Param,
					_IOC_SIZE(a_u4Command))) {
				kfree(pBuff);
				pBuff = NULL;
				PK_ERR("[CAMERA SENSOR] ioctl copy from user failed\n");
				i4RetValue =  -EFAULT;
				goto CAMERA_HW_Ioctl_EXIT;
			}
		}
	}


	pIdx = (u32 *) pBuff;
	switch (a_u4Command) {
	case KDIMGSENSORIOC_X_SET_DRIVER:
		i4RetValue = kdSetDriver((unsigned int *)pBuff);
		break;
	case KDIMGSENSORIOC_T_OPEN:
		i4RetValue = adopt_CAMERA_HW_Open();
		break;
	case KDIMGSENSORIOC_X_GETINFO:
		i4RetValue = adopt_CAMERA_HW_GetInfo(pBuff);
		break;
	case KDIMGSENSORIOC_X_GETRESOLUTION2:
		i4RetValue = adopt_CAMERA_HW_GetResolution(pBuff);
		break;
	case KDIMGSENSORIOC_X_GETINFO2:
		i4RetValue = adopt_CAMERA_HW_GetInfo2(pBuff);
		break;
	case KDIMGSENSORIOC_X_FEATURECONCTROL:
		i4RetValue = adopt_CAMERA_HW_FeatureControl(pBuff);
		break;
	case KDIMGSENSORIOC_X_CONTROL:
		i4RetValue = adopt_CAMERA_HW_Control(pBuff);
		break;
	case KDIMGSENSORIOC_T_CLOSE:
		i4RetValue = adopt_CAMERA_HW_Close();
		break;
	case KDIMGSENSORIOC_T_CHECK_IS_ALIVE:
#ifndef CONFIG_CAMERA_MULTIMODAL

		i4RetValue = adopt_CAMERA_HW_CheckIsAlive();
#else
                /*
                 * Ignore anyway to avoid hardware power-on before GATING state
                 * is restored
                 */
		PK_ERR("[CAMERA SENSOR] warning: ignore ioctl for CheckIsAlive\n");
#endif
		break;
	case KDIMGSENSORIOC_X_GET_SOCKET_POS:
		i4RetValue = kdGetSocketPostion((unsigned int *)pBuff);
		break;
	case KDIMGSENSORIOC_X_SET_I2CBUS:
		/* i4RetValue = kdSetI2CBusNum(*pIdx); */
		break;
	case KDIMGSENSORIOC_X_RELEASE_I2C_TRIGGER_LOCK:
		/* i4RetValue = kdReleaseI2CTriggerLock(); */
		break;

	case KDIMGSENSORIOC_X_SET_SHUTTER_GAIN_WAIT_DONE:
		/* i4RetValue = kdSensorSetExpGainWaitDone((int *)pBuff); */
		break;

	case KDIMGSENSORIOC_X_SET_CURRENT_SENSOR:
		i4RetValue = kdSetCurrentSensorIdx(*pIdx);
		break;

	case KDIMGSENSORIOC_X_SET_MCLK_PLL:
		i4RetValue = kdSetSensorMclk(pBuff);
		break;

	case KDIMGSENSORIOC_X_SET_GPIO:
		i4RetValue = kdSetSensorGpio(pBuff);
		break;

	case KDIMGSENSORIOC_X_GET_ISP_CLK:
		/* PK_DBG("get_isp_clk=%d\n",get_isp_clk()); */
		/* *(unsigned int*)pBuff = get_isp_clk(); */
		break;

	default:
		PK_ERR("No such command\n");
		i4RetValue = -EPERM;
		goto CAMERA_HW_Ioctl_EXIT;
		break;

	}

	if (_IOC_READ & _IOC_DIR(a_u4Command)) {
		if (copy_to_user((void __user *)a_u4Param, pBuff,
				_IOC_SIZE(a_u4Command))) {
			kfree(pBuff);
			pBuff = NULL;
			PK_ERR("[CAMERA SENSOR] ioctl copy to user failed\n");
			i4RetValue = -EFAULT;
			goto CAMERA_HW_Ioctl_EXIT;
		}
	}

CAMERA_HW_Ioctl_EXIT:
	if (pBuff != NULL) {
		kfree(pBuff);
		pBuff = NULL;
	}
	mutex_unlock(&kdCam_Mutex);
	return i4RetValue;
}

/*  */
/* below is for linux driver system call */
/* change prefix or suffix only */
/*  */

static int CAMERA_HW_Open(struct inode *a_pstInode, struct file *a_pstFile)
{
	/* reset once in multi-open */
	if (atomic_read(&g_CamDrvOpenCnt) == 0) {
		/* default OFF state */
		/* MUST have */
		/* kdCISModulePowerOn(DUAL_CAMERA_MAIN_SENSOR,"", */
		/* true,CAMERA_HW_DRVNAME1); */
		/* kdCISModulePowerOn(DUAL_CAMERA_SUB_SENSOR,"", */
		/* true,CAMERA_HW_DRVNAME1); */

		/* kdCISModulePowerOn(DUAL_CAMERA_MAIN_SENSOR,"", */
		/* false,CAMERA_HW_DRVNAME1); */
		/* kdCISModulePowerOn(DUAL_CAMERA_SUB_SENSOR,"", */
		/* false,CAMERA_HW_DRVNAME1); */

	}

	/*  */
	atomic_inc(&g_CamDrvOpenCnt);
	return 0;
}

static int CAMERA_HW_Release(struct inode *a_pstInode, struct file *a_pstFile)
{
	atomic_dec(&g_CamDrvOpenCnt);
	/* PK_DBG("[%s] g_CamDrvOpenCnt %d\n", __func__, g_CamDrvOpenCnt); */
	/* if (atomic_read(&g_CamDrvOpenCnt) == 0) */
	checkPowerBeforClose(0, CAMERA_HW_DRVNAME1);
	checkPowerBeforClose(1, CAMERA_HW_DRVNAME1);

	return 0;
}

static const struct file_operations g_stCAMERA_HW_fops = {
	.owner = THIS_MODULE,
	.open = CAMERA_HW_Open,
	.release = CAMERA_HW_Release,
	.unlocked_ioctl = CAMERA_HW_Ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = CAMERA_HW_Ioctl_Compat,
#endif

};

#define CAMERA_HW_DYNAMIC_ALLOCATE_DEVNO 1
static inline int RegisterCAMERA_HWCharDrv(void)
{
	sensor_device = NULL;

#if CAMERA_HW_DYNAMIC_ALLOCATE_DEVNO
	if (alloc_chrdev_region(&g_CAMERA_HWdevno, 0, 1, CAMERA_HW_DRVNAME1)) {
		PK_ERR("[CAMERA SENSOR] Allocate device no failed\n");
		return -EAGAIN;
	}
#else
	if (register_chrdev_region(g_CAMERA_HWdevno, 1, CAMERA_HW_DRVNAME1)) {
		PK_ERR("[CAMERA SENSOR] Register device no failed\n");
		return -EAGAIN;
	}
#endif

	/* Allocate driver */
	g_pCAMERA_HW_CharDrv = cdev_alloc();

	if (g_pCAMERA_HW_CharDrv == NULL) {
		unregister_chrdev_region(g_CAMERA_HWdevno, 1);
		PK_ERR("[CAMERA SENSOR] Allocate mem for kobject failed\n");
		return -ENOMEM;
	}

	/* Attatch file operation. */
	cdev_init(g_pCAMERA_HW_CharDrv, &g_stCAMERA_HW_fops);

	g_pCAMERA_HW_CharDrv->owner = THIS_MODULE;

	/* Add to system */
	if (cdev_add(g_pCAMERA_HW_CharDrv, g_CAMERA_HWdevno, 1)) {
		PK_ERR("[mt6516_IDP] Attatch file operation failed\n");
		unregister_chrdev_region(g_CAMERA_HWdevno, 1);
		return -EAGAIN;
	}

	sensor_class = class_create(THIS_MODULE, "sensordrv");
	if (IS_ERR(sensor_class)) {
		int ret = PTR_ERR(sensor_class);
		PK_ERR("Unable to create class, err = %d\n", ret);
		return ret;
	}
	sensor_device =
		device_create(sensor_class, NULL, g_CAMERA_HWdevno, NULL,
			CAMERA_HW_DRVNAME1);

	return 0;
}

static inline void UnregisterCAMERA_HWCharDrv(void)
{
	/* Release char driver */
	cdev_del(g_pCAMERA_HW_CharDrv);

	unregister_chrdev_region(g_CAMERA_HWdevno, 1);

	device_destroy(sensor_class, g_CAMERA_HWdevno);
	class_destroy(sensor_class);
}

static int CAMERA_HW_i2c_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int i4RetValue = 0;

	PK_DBG("[CAMERA_HW] Attach I2C\n");

	/* get sensor i2c client */
	spin_lock(&kdsensor_drv_lock);
	g_pstI2Cclient = client;
	/* set I2C clock rate */
	/* g_pstI2Cclient->timing = 100;//100k */

	spin_unlock(&kdsensor_drv_lock);

	/* Register char driver */
	i4RetValue = RegisterCAMERA_HWCharDrv();

	if (i4RetValue) {
		PK_ERR("[CAMERA_HW] register char device failed!\n");
		return i4RetValue;
	}

	/* spin_lock_init(&g_CamHWLock); */
#if 0 /*!defined(CONFIG_MTK_LEGACY)*/
	Get_Cam_Regulator();
#endif

	PK_DBG("[CAMERA_HW] Attached!!\n");
	return 0;
}


static int CAMERA_HW_i2c_remove(struct i2c_client *client)
{
	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id CAMERA_HW_i2c_of_ids[] = {
	{ .compatible = "mediatek,camera_main", },
	{}
};
#endif

struct i2c_driver CAMERA_HW_i2c_driver = {
	.probe = CAMERA_HW_i2c_probe,
	.remove = CAMERA_HW_i2c_remove,
	.driver = {
		   .name = CAMERA_HW_DRVNAME1,
		   .owner = THIS_MODULE,

#ifdef CONFIG_OF
		   .of_match_table = CAMERA_HW_i2c_of_ids,
#endif
		   },
	.id_table = CAMERA_HW_i2c_id,
};

static int CAMERA_HW_Open2(struct inode *a_pstInode, struct file *a_pstFile)
{
	/*  */
	if (atomic_read(&g_CamDrvOpenCnt2) == 0) {
		/* kdCISModulePowerOn(DUAL_CAMERA_MAIN_2_SENSOR,"", */
		/* true,CAMERA_HW_DRVNAME2); */

		/* kdCISModulePowerOn(DUAL_CAMERA_MAIN_2_SENSOR,"", */
		/* false,CAMERA_HW_DRVNAME2); */
	}
	atomic_inc(&g_CamDrvOpenCnt2);
	return 0;
}

static int CAMERA_HW_Release2(struct inode *a_pstInode, struct file *a_pstFile)
{
	atomic_dec(&g_CamDrvOpenCnt2);

	checkPowerBeforClose(1, CAMERA_HW_DRVNAME2);

	return 0;
}

static const struct file_operations g_stCAMERA_HW_fops0 = {
	.owner = THIS_MODULE,
	.open = CAMERA_HW_Open2,
	.release = CAMERA_HW_Release2,
	.unlocked_ioctl = CAMERA_HW_Ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = CAMERA_HW_Ioctl_Compat,
#endif

};

static inline int RegisterCAMERA_HWCharDrv2(void)
{
	struct device *sensor_device = NULL;
	UINT32 major;

#if CAMERA_HW_DYNAMIC_ALLOCATE_DEVNO
	if (alloc_chrdev_region(&g_CAMERA_HWdevno2, 0, 1, CAMERA_HW_DRVNAME2)) {
		PK_ERR("[CAMERA SENSOR] Allocate device no failed\n");
		return -EAGAIN;
	}
#else
	if (register_chrdev_region(g_CAMERA_HWdevno2, 1, CAMERA_HW_DRVNAME2)) {
		PK_ERR("[CAMERA SENSOR] Register device no failed\n");
		return -EAGAIN;
	}
#endif

	major = MAJOR(g_CAMERA_HWdevno2);
	g_CAMERA_HWdevno2 = MKDEV(major, 0);

	/* Allocate driver */
	g_pCAMERA_HW_CharDrv2 = cdev_alloc();

	if (g_pCAMERA_HW_CharDrv2 == NULL) {
		unregister_chrdev_region(g_CAMERA_HWdevno2, 1);
		PK_ERR("[CAMERA SENSOR] Allocate mem for kobject failed\n");
		return -ENOMEM;
	}

	/* Attatch file operation. */
	cdev_init(g_pCAMERA_HW_CharDrv2, &g_stCAMERA_HW_fops0);

	g_pCAMERA_HW_CharDrv2->owner = THIS_MODULE;

	/* Add to system */
	if (cdev_add(g_pCAMERA_HW_CharDrv2, g_CAMERA_HWdevno2, 1)) {
		PK_ERR("[mt6516_IDP] Attatch file operation failed\n");
		unregister_chrdev_region(g_CAMERA_HWdevno2, 1);
		return -EAGAIN;
	}

	sensor2_class = class_create(THIS_MODULE, "sensordrv2");
	if (IS_ERR(sensor2_class)) {
		int ret = PTR_ERR(sensor2_class);
		PK_ERR("Unable to create class, err = %d\n", ret);
		return ret;
	}
	sensor_device =
		device_create(sensor2_class, NULL, g_CAMERA_HWdevno2, NULL,
			CAMERA_HW_DRVNAME2);

	return 0;
}

static inline void UnregisterCAMERA_HWCharDrv2(void)
{
	/* Release char driver */
	cdev_del(g_pCAMERA_HW_CharDrv2);

	unregister_chrdev_region(g_CAMERA_HWdevno2, 1);

	device_destroy(sensor2_class, g_CAMERA_HWdevno2);
	class_destroy(sensor2_class);
}

static int CAMERA_HW_i2c_probe2(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	int i4RetValue = 0;

	PK_DBG("[CAMERA_HW] Attach I2C0\n");

	spin_lock(&kdsensor_drv_lock);

	/* get sensor i2c client */
	g_pstI2Cclient2 = client;

	/* set I2C clock rate */
	/*g_pstI2Cclient2->timing = 100;*/	/* 100k */
	spin_unlock(&kdsensor_drv_lock);

	/* Register char driver */
	i4RetValue = RegisterCAMERA_HWCharDrv2();

	if (i4RetValue) {
		PK_ERR("[CAMERA_HW] register char device failed!\n");
		return i4RetValue;
	}

	/* spin_lock_init(&g_CamHWLock); */

	PK_DBG("[CAMERA_HW] Attached!!\n");
	return 0;
}

static int CAMERA_HW_i2c_remove2(struct i2c_client *client)
{
	return 0;
}


/*******************************************************************************
 * I2C Driver structure
 ******************************************************************************/
#if 1
#ifdef CONFIG_OF
static const struct of_device_id CAMERA_HW2_i2c_driver_of_ids[] = {
	{ .compatible = "mediatek,camera_sub", },
	{}
};
#endif
#endif

struct i2c_driver CAMERA_HW_i2c_driver2 = {
	.probe = CAMERA_HW_i2c_probe2,
	.remove = CAMERA_HW_i2c_remove2,
	.driver = {
		   .name = CAMERA_HW_DRVNAME2,
		   .owner = THIS_MODULE,
#if 1
#ifdef CONFIG_OF
		   .of_match_table = CAMERA_HW2_i2c_driver_of_ids,
#endif
#endif
		   },
	.id_table = CAMERA_HW_i2c_id2,
};

static int get_adc_pin_id_volt(int index, int *volt)
{
	struct iio_channel *channel;
	int *val = volt;
	int ret;

	if (!g_adc_id_iio_channel){
		pr_err("g_adc_id_iio_channel is null\n");
		*volt = 0;
		return -EINVAL;
	}

	channel = &g_adc_id_iio_channel[index];
	if (!channel)
		return -EINVAL;

	ret = iio_read_channel_processed(channel, val);
	if (ret < 0) {
		pr_err("IIO channel read failed %d\n", ret);
		return -EINVAL;
	}

	*val = *val * 1500 / 4096;
	return 0;
}

/*
  Get camera ID pin voltage. The value will be 0mv and 900mv.
  If voltage value is 0mv or return error, it will call 1st sensor driver.
*/

int get_adc_id_status(void)
{
	int id_volt;
	int ret;

	g_adc_id_iio_channel = iio_channel_get_all(&adc_pdev->dev);
	if (IS_ERR(g_adc_id_iio_channel)) {
		ret = PTR_ERR(g_adc_id_iio_channel);
		dev_err(&adc_pdev->dev, "zhj IIO channel not found: %d\n", ret);
		return -EPROBE_DEFER;
	}

	ret = get_adc_pin_id_volt(0, &id_volt);
	if(ret < 0){
		pr_err("get adc pin id failed %d!\n", ret);
		return -EINVAL;
	}

	return id_volt;
}

static int CAMERA_HW_probe(struct platform_device *pdev)
{
	struct device_node *np;
	int ret = 0;
	adc_pdev = pdev;

#ifdef MTKCAM_USING_PWRREG


	ret = cam_power_init(pdev, 0);

	if (ret < 0) {
		if (ret == (-EPROBE_DEFER))
			PK_ERR("Camera HW driver is probed earlier than PMIC, let's deferring probe");
		return ret;
	}

#endif

#ifdef MTKCAM_USING_CCF
	np = pdev->dev.of_node;
	if (of_find_property(np, "cmmclk-always-on", NULL)) {
		PK_XLOG_INFO("Camera HW: Setting up camera clocks\n");

		/* depend on ISP module ready and open */
		ISP_EnableClock(1, 1);
		/* end */

		/* Turn on sensor MCLK once clocks are available */
		/* depend on ISP module ready and open */
		ISP_MCLK1_EN(1);
		/* end */

		/* Setup the clock parent for 26MHz parent */
		Get_ccf_clk(pdev);
		Check_ccf_clk();

		ret = clk_prepare_enable(g_camclk_camtg_sel);
		if (ret)
			PK_ERR("Unable to prepare g_camclk_camtg_sel clock, err:%d\n",
				ret);

		/*CAM_PLL_48_GROUP */
		ret = clk_set_parent(g_camclk_camtg_sel, g_camclk_univpll_d26);
		if (ret)
			PK_ERR("Unable to set parent as g_camclk_univpll_d26 clock, err:%d\n",
				ret);

		/* depend on ISP module ready and open */
		ISP_MCLK1_EN(1);
		ISP_set_mclk1(7);
		/* end */

		/* TG1_SEN_CK.CLKCNT = 48M/9.6M -1 = 4 */
		/* depend on ISP module ready and open */
		ISP_set_mclk1(4);
		/* end */

	}
#endif

#if !defined(CONFIG_MTK_LEGACY)
	mtkcam_gpio_init(pdev);
#endif

#if 1 //T_flash
	if (sysfs_create_group(&pdev->dev.kobj, &dev_attr_grp))
		PK_ERR("T_flash seitch create fail!! %s:%d\n",
			__func__, __LINE__);
	else
		PK_ERR("T_flash seitch create successfully!!%s:%d\n",
			__func__, __LINE__);

#endif
	return i2c_add_driver(&CAMERA_HW_i2c_driver);
}

/*******************************************************************************
 * CAMERA_HW_remove()
 ******************************************************************************/
static int CAMERA_HW_remove(struct platform_device *pdev)
{
	i2c_del_driver(&CAMERA_HW_i2c_driver);
	return 0;
}

static int CAMERA_HW_suspend(struct platform_device *pdev, pm_message_t mesg)
{
	return 0;
}

static int CAMERA_HW_resume(struct platform_device *pdev)
{
	return 0;
}

static int CAMERA_HW_probe2(struct platform_device *pdev)
{

#ifdef MTKCAM_USING_PWRREG

	int ret = 0;

	ret = cam_power_init(pdev, 1);

	if (ret < 0) {
		if (ret == (-EPROBE_DEFER))
			PK_ERR("Camera HW driver is probed earlier than PMIC, let's deferring probe");
		return ret;
	}

#endif


	return i2c_add_driver(&CAMERA_HW_i2c_driver2);
}

static int CAMERA_HW_remove2(struct platform_device *pdev)
{
	i2c_del_driver(&CAMERA_HW_i2c_driver2);
	return 0;
}

static int CAMERA_HW_suspend2(struct platform_device *pdev, pm_message_t mesg)
{
	return 0;
}

static int CAMERA_HW_resume2(struct platform_device *pdev)
{
	return 0;
}


#if 1
#ifdef CONFIG_OF
static const struct of_device_id CAMERA_HW2_of_ids[] = {
	{.compatible = "mediatek,mt8163-camera_hw2",},
	{}
};
#endif
#endif

static struct platform_driver g_stCAMERA_HW_Driver2 = {
	.probe = CAMERA_HW_probe2,
	.remove = CAMERA_HW_remove2,
	.suspend = CAMERA_HW_suspend2,
	.resume = CAMERA_HW_resume2,
	.driver = {
		.name = "image_sensor_bus2",
		.owner = THIS_MODULE,
#if 1
#ifdef CONFIG_OF
		.of_match_table = CAMERA_HW2_of_ids,
#endif
#endif

	}
};

#if 0
int iWriteTriggerReg(u16 a_u2Addr, u32 a_u4Data, u32 a_u4Bytes, u16 i2cId)
{
	int i4RetValue = 0;
	int u4Index = 0;
	u8 *puDataInBytes = (u8 *) &a_u4Data;
	int retry = 3;
	char puSendCmd[6] = { (char)(a_u2Addr >> 8),
		(char)(a_u2Addr & 0xFF), 0, 0, 0, 0 };



	SET_I2CBUS_FLAG(gI2CBusNum);

	if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient->addr = (i2cId >> 1);
		spin_unlock(&kdsensor_drv_lock);
	} else {
		spin_lock(&kdsensor_drv_lock);
		g_pstI2Cclient2->addr = (i2cId >> 1);
		spin_unlock(&kdsensor_drv_lock);
	}


	if (a_u4Bytes > 2) {
		PK_ERR("[CAMERA SENSOR] exceed 2 bytes\n");
		return -1;
	}

	if (a_u4Data >> (a_u4Bytes << 3))
		PK_ERR("[CAMERA SENSOR] warning!! some data is not sent!!\n");

	for (u4Index = 0; u4Index < a_u4Bytes; u4Index += 1) {
		puSendCmd[(u4Index + 2)] =
			puDataInBytes[(a_u4Bytes - u4Index - 1)];
	}

	do {
		if (gI2CBusNum == SUPPORT_I2C_BUS_NUM1) {
			i4RetValue =
				mt_i2c_master_send(g_pstI2Cclient, puSendCmd,
				(a_u4Bytes + 2),
				I2C_3DCAMERA_FLAG);
			if (i4RetValue < 0) {
				PK_ERR("[ERROR]set i2c bus 1 master fail\n");
				CLEAN_I2CBUS_FLAG(gI2CBusNum);
				break;
			}
		} else {
			i4RetValue =
				mt_i2c_master_send(g_pstI2Cclient2, puSendCmd,
				(a_u4Bytes + 2), I2C_3DCAMERA_FLAG);
			if (i4RetValue < 0) {
				PK_ERR("[CAMERA SENSOR][ERROR] set i2c bus 0 master fail\n");
				CLEAN_I2CBUS_FLAG(gI2CBusNum);
				break;
			}
		}

		if (i4RetValue != (a_u4Bytes + 2))
			PK_ERR("[CAMERA SENSOR] I2C send failed addr = 0x%x, data = 0x%x !!\n",
				a_u2Addr, a_u4Data);
		else
			break;
		uDELAY(50);
	} while ((retry--) > 0);

	return i4RetValue;
}
#endif
#if 0				/* linux-3.10 procfs API changed */
static int CAMERA_HW_Read_Main_Camera_Status(
	char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	char *p = page;
	int len = 0;

	p += sprintf(page, "%d\n", g_SensorExistStatus[0]);

	PK_DBG("g_SensorExistStatus[0] = %d\n", g_SensorExistStatus[0]);
	*start = page + off;
	len = p - page;
	if (len > off)
		len -= off;
	else
		len = 0;
	return len < count ? len : count;

}

static int CAMERA_HW_Read_Sub_Camera_Status(
	char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	char *p = page;
	int len = 0;

	p += sprintf(page, "%d\n", g_SensorExistStatus[1]);

	PK_DBG(" g_SensorExistStatus[1] = %d\n", g_SensorExistStatus[1]);
	*start = page + off;
	len = p - page;
	if (len > off)
		len -= off;
	else
		len = 0;
	return len < count ? len : count;

}

static int CAMERA_HW_Read_3D_Camera_Status(
	char *page, char **start, off_t off,
	int count, int *eof, void *data)
{
	char *p = page;
	int len = 0;

	p += sprintf(page, "%d\n", g_SensorExistStatus[2]);

	PK_DBG("g_SensorExistStatus[2] = %d\n", g_SensorExistStatus[2]);
	*start = page + off;
	len = p - page;
	if (len > off)
		len -= off;
	else
		len = 0;
	return len < count ? len : count;

}
#endif


static ssize_t CAMERA_HW_DumpReg_To_Proc(struct file *file,
	char __user *data, size_t len,
	loff_t *ppos)
{
	return 0;
}

static ssize_t CAMERA_HW_DumpReg_To_Proc2(struct file *file,
	char __user *data, size_t len,
	loff_t *ppos)
{
	return 0;
}

static ssize_t CAMERA_HW_DumpReg_To_Proc3(struct file *file,
	char __user *data, size_t len,
	loff_t *ppos)
{
	return 0;
}

static ssize_t CAMERA_HW_Reg_Debug(struct file *file,
	const char *buffer, size_t count, loff_t *data)
{
	char regBuf[64] = { '\0' };
	u32 u4CopyBufSize = (count < (sizeof(regBuf) - 1)) ?
		(count) : (sizeof(regBuf) - 1);

	MSDK_SENSOR_REG_INFO_STRUCT sensorReg;
	MSDK_SENSOR_DBG_IMGSENSOR_INFO_STRUCT debugSensor;

	memset(&sensorReg, 0, sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
	memset(&debugSensor, 0, sizeof(MSDK_SENSOR_DBG_IMGSENSOR_INFO_STRUCT));

	if (copy_from_user(regBuf, buffer, u4CopyBufSize))
		return -EFAULT;

	if ((kstrtou32(regBuf, 0, &sensorReg.RegAddr) == 1) &&
			(kstrtou32(regBuf, 0, &sensorReg.RegData) == 1)) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_SENSOR,
				SENSOR_FEATURE_SET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_SENSOR,
				SENSOR_FEATURE_GET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			PK_DBG("write addr = 0x%08x, data = 0x%08x\n",
				sensorReg.RegAddr,
				sensorReg.RegData);
		}
	} else if (kstrtou32(regBuf, 0, &sensorReg.RegAddr) == 1) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_SENSOR,
				SENSOR_FEATURE_GET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			PK_DBG("read addr = 0x%08x, data = 0x%08x\n",
				sensorReg.RegAddr,
				sensorReg.RegData);
		}
	} else if ((kstrtou8(regBuf, 0, debugSensor.debugStruct) == 1) &&
		(kstrtou8(regBuf, 0, debugSensor.debugSubstruct) == 1) &&
		(kstrtou32(regBuf, 0, &debugSensor.isGet) == 1) &&
		(kstrtou32(regBuf, 0, &debugSensor.value) == 1)) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_SENSOR,
				SENSOR_FEATURE_DEBUG_IMGSENSOR,
				(MUINT8 *) &debugSensor,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_DBG_IMGSENSOR_INFO_STRUCT));
			PK_DBG("debug imgsensor = 0x%x, data = 0x%x\n",
				debugSensor.isGet,
				debugSensor.value);
		}
	}

	return count;
}


static ssize_t CAMERA_HW_Reg_Debug2(struct file *file, const char *buffer,
	size_t count, loff_t *data)
{
	char regBuf[64] = { '\0' };
	u32 u4CopyBufSize = (count < (sizeof(regBuf) - 1)) ?
		(count) : (sizeof(regBuf) - 1);

	MSDK_SENSOR_REG_INFO_STRUCT sensorReg;

	memset(&sensorReg, 0, sizeof(MSDK_SENSOR_REG_INFO_STRUCT));

	if (copy_from_user(regBuf, buffer, u4CopyBufSize))
		return -EFAULT;

	if ((kstrtou32(regBuf, 0, &sensorReg.RegAddr) == 1) &&
			(kstrtou32(regBuf, 0, &sensorReg.RegData) == 1)) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_2_SENSOR,
				SENSOR_FEATURE_SET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_2_SENSOR,
				SENSOR_FEATURE_GET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			PK_DBG("write addr = 0x%08x, data = 0x%08x\n",
				sensorReg.RegAddr,
				sensorReg.RegData);
		}
	} else if (kstrtou32(regBuf, 0, &sensorReg.RegAddr) == 1) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_MAIN_2_SENSOR,
				SENSOR_FEATURE_GET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			PK_DBG("read addr = 0x%08x, data = 0x%08x\n",
				sensorReg.RegAddr,
				sensorReg.RegData);
		}
	}

	return count;
}

static ssize_t CAMERA_HW_Reg_Debug3(struct file *file, const char *buffer,
		size_t count, loff_t *data)
{
	char regBuf[64] = { '\0' };
	u32 u4CopyBufSize = (count < (sizeof(regBuf) - 1)) ?
		(count) : (sizeof(regBuf) - 1);

	MSDK_SENSOR_REG_INFO_STRUCT sensorReg;

	memset(&sensorReg, 0, sizeof(MSDK_SENSOR_REG_INFO_STRUCT));

	if (copy_from_user(regBuf, buffer, u4CopyBufSize))
		return -EFAULT;

	if ((kstrtou32(regBuf, 0, &sensorReg.RegAddr) == 1) &&
			(kstrtou32(regBuf, 0, &sensorReg.RegData) == 1)) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_SUB_SENSOR,
				SENSOR_FEATURE_SET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_SUB_SENSOR,
				SENSOR_FEATURE_GET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			PK_DBG("write addr = 0x%08x, data = 0x%08x\n",
				sensorReg.RegAddr,
				sensorReg.RegData);
		}
	} else if (kstrtou32(regBuf, 0, &sensorReg.RegAddr) == 1) {
		if (g_pSensorFunc != NULL) {
			g_pSensorFunc->SensorFeatureControl(
				DUAL_CAMERA_SUB_SENSOR,
				SENSOR_FEATURE_GET_REGISTER,
				(MUINT8 *) &sensorReg,
				(MUINT32 *)
				sizeof(MSDK_SENSOR_REG_INFO_STRUCT));
			PK_DBG("read addr = 0x%08x, data = 0x%08x\n",
				sensorReg.RegAddr,
				sensorReg.RegData);
		}
	}

	return count;
}

/* You can refer to CAMERA_HW_probe & CAMERA_HW_i2c_probe */
#ifdef CONFIG_OF
static const struct of_device_id CAMERA_HW_of_ids[] = {
	{.compatible = "mediatek,mt8163-camera_hw",},
	{}
};
#endif

static struct platform_driver g_stCAMERA_HW_Driver = {
	.probe = CAMERA_HW_probe,
	.remove = CAMERA_HW_remove,
	.suspend = CAMERA_HW_suspend,
	.resume = CAMERA_HW_resume,
	.driver = {
		.name = "image_sensor",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = CAMERA_HW_of_ids,
#endif
	}
};





static const struct file_operations fcamera_proc_fops = {
	.read = CAMERA_HW_DumpReg_To_Proc,
	.write = CAMERA_HW_Reg_Debug
};

static const struct file_operations fcamera_proc_fops2 = {
	.read = CAMERA_HW_DumpReg_To_Proc2,
	.write = CAMERA_HW_Reg_Debug2
};

static const struct file_operations fcamera_proc_fops3 = {
	.read = CAMERA_HW_DumpReg_To_Proc3,
	.write = CAMERA_HW_Reg_Debug3
};

/* Camera information */
static int subsys_camera_info_read(struct seq_file *m, void *v)
{
	PK_ERR("subsys_camera_info_read %s\n", mtk_ccm_name);
	seq_printf(m, "%s\n", mtk_ccm_name);
	return 0;
};

static int proc_camera_info_open(struct inode *inode, struct file *file)
{
	return single_open(file, subsys_camera_info_read, NULL);
};

static const struct file_operations fcamera_proc_fops1 = {
	.owner = THIS_MODULE,
	.open = proc_camera_info_open,
	.read = seq_read,
};

static int __init CAMERA_HW_i2C_init(void)
{

#if 0
	struct proc_dir_entry *prEntry;
#endif
#if defined(CONFIG_MTK_LEGACY)
	/* i2c_register_board_info(CAMERA_I2C_BUSNUM, */
	/* &kd_camera_dev, 1); */
	PK_DBG("MTKCAM I2C Bus #%d , #%d\n", SUPPORT_I2C_BUS_NUM1,
			SUPPORT_I2C_BUS_NUM2);
	i2c_register_board_info(SUPPORT_I2C_BUS_NUM1, &i2c_devs1, 1);
	i2c_register_board_info(SUPPORT_I2C_BUS_NUM2, &i2c_devs2, 1);
#endif
	PK_DBG("[camerahw_probe] start\n");

#ifndef CONFIG_OF
	int ret;

	ret = platform_device_register(&camerahw_platform_device);
	if (ret) {
		PK_ERR("[camerahw_probe] platform_device_register fail\n");
		return ret;
	}

	ret = platform_device_register(&camerahw2_platform_device);
	if (ret) {
		PK_ERR("[camerahw2_probe] platform_device_register fail\n");
		return ret;
	}
#endif

	if (platform_driver_register(&g_stCAMERA_HW_Driver)) {
		PK_ERR("failed to register CAMERA_HW driver\n");
		return -ENODEV;
	}
	if (platform_driver_register(&g_stCAMERA_HW_Driver2)) {
		PK_ERR("failed to register CAMERA_HW2 driver\n");
		return -ENODEV;
	}
/* FIX-ME: linux-3.10 procfs API changed */
#if 1
	proc_create("driver/camsensor", 0644, NULL, &fcamera_proc_fops);
	proc_create("driver/camsensor2", 0644, NULL, &fcamera_proc_fops2);
	proc_create("driver/camsensor3", 0644, NULL, &fcamera_proc_fops3);

	/* Camera information */
	memset(mtk_ccm_name, 0, camera_info_size);
	proc_create(PROC_CAMERA_INFO, 0644, NULL, &fcamera_proc_fops1);

#else
	/* Register proc file for main sensor register debug */
	prEntry = create_proc_entry("driver/camsensor", 0, NULL);
	if (prEntry) {
		prEntry->read_proc = CAMERA_HW_DumpReg_To_Proc;
		prEntry->write_proc = CAMERA_HW_Reg_Debug;
	} else
		PK_ERR("add /proc/driver/camsensor entry fail\n");

	/* Register proc file for main_2 sensor register debug */
	prEntry = create_proc_entry("driver/camsensor2", 0, NULL);
	if (prEntry) {
		prEntry->read_proc = CAMERA_HW_DumpReg_To_Proc;
		prEntry->write_proc = CAMERA_HW_Reg_Debug2;
	} else
		PK_ERR("add /proc/driver/camsensor2 entry fail\n");

	/* Register proc file for sub sensor register debug */
	prEntry = create_proc_entry("driver/camsensor3", 0, NULL);
	if (prEntry) {
		prEntry->read_proc = CAMERA_HW_DumpReg_To_Proc;
		prEntry->write_proc = CAMERA_HW_Reg_Debug3;
	} else
		PK_ERR("add /proc/driver/camsensor entry fail\n");

	/* Register proc file for main sensor register debug */
	prEntry = create_proc_entry("driver/maincam_status", 0, NULL);
	if (prEntry) {
		prEntry->read_proc = CAMERA_HW_Read_Main_Camera_Status;
		prEntry->write_proc = NULL;
	} else
		PK_ERR("add /proc/driver/maincam_status entry fail\n");

	/* Register proc file for sub sensor register debug */
	prEntry = create_proc_entry("driver/subcam_status", 0, NULL);
	if (prEntry) {
		prEntry->read_proc = CAMERA_HW_Read_Sub_Camera_Status;
		prEntry->write_proc = NULL;
	} else
		PK_ERR("add /proc/driver/subcam_status entry fail\n");

	/* Register proc file for 3d sensor register debug */
	prEntry = create_proc_entry("driver/3dcam_status", 0, NULL);
	if (prEntry) {
		prEntry->read_proc = CAMERA_HW_Read_3D_Camera_Status;
		prEntry->write_proc = NULL;
	} else
		PK_ERR("add /proc/driver/3dcam_status entry fail\n");

#endif
	atomic_set(&g_CamHWOpend, 0);
	atomic_set(&g_CamHWOpend2, 0);
	atomic_set(&g_CamDrvOpenCnt, 0);
	atomic_set(&g_CamDrvOpenCnt2, 0);
	atomic_set(&g_CamHWOpening, 0);

	return 0;
}

static void __exit CAMERA_HW_i2C_exit(void)
{
	platform_driver_unregister(&g_stCAMERA_HW_Driver);
	platform_driver_unregister(&g_stCAMERA_HW_Driver2);
}


module_init(CAMERA_HW_i2C_init);
module_exit(CAMERA_HW_i2C_exit);

MODULE_DESCRIPTION("CAMERA_HW driver");
MODULE_AUTHOR("Mediatek");
MODULE_LICENSE("GPL v2");

