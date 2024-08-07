/*
 * Copyright(c) 2015, BlackBerry Limited
 * Copyright(c) 2014, Analogix Semiconductor. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "slimport_private.h"
#include "slimport_tx_drv.h"
#include "slimport_tx_reg.h"
#ifdef QUICK_CHARGE_SUPPORT
#include "quick_charge.h"
#endif

/* Enable or Disable HDCP by default */
/* hdcp_enable = 1: Enable,  0: Disable */
int hdcp_enable = 1;

/* HDCP switch for external block*/
int external_block_en = 0;

/* to access global platform data */
static struct anx7816_platform_data *g_pdata;

/* Use device tree structure data when defined "CONFIG_OF"  */
/*#define CONFIG_OF
#define SP_REGISTER_SET_TEST*/

#ifdef SP_REGISTER_SET_TEST
/*//For Slimport swing&pre-emphasis test*/
unchar val_SP_TX_LT_CTRL_REG0;
unchar val_SP_TX_LT_CTRL_REG10;
unchar val_SP_TX_LT_CTRL_REG11;
unchar val_SP_TX_LT_CTRL_REG2;
unchar val_SP_TX_LT_CTRL_REG12;
unchar val_SP_TX_LT_CTRL_REG1;
unchar val_SP_TX_LT_CTRL_REG6;
unchar val_SP_TX_LT_CTRL_REG16;
unchar val_SP_TX_LT_CTRL_REG5;
unchar val_SP_TX_LT_CTRL_REG8;
unchar val_SP_TX_LT_CTRL_REG15;
unchar val_SP_TX_LT_CTRL_REG18;
#endif

#define TRUE 1
#define FALSE 0

static int slimport7816_vdd_power(unsigned int onoff);
void cable_disconnect(void *data);

struct i2c_client *anx7816_client;

struct anx7816_platform_data {
	int gpio_p_dwn;
	int gpio_reset;
	int gpio_cbl_det;
	struct platform_device *hdmi_pdev;
	struct regulator *vdd_10;
	int (*vdd_power) (unsigned int onoff);
	int power_status;
	int gpio_v10_ctrl;
	int gpio_v33_ctrl;
	int external_ldo_control;
	struct regulator *avdd_10;
	struct regulator *dvdd_10;
	spinlock_t lock;
};

struct anx7816_data {
	struct anx7816_platform_data *pdata;
	struct delayed_work work;
	struct workqueue_struct *workqueue;
	struct mutex lock;
	struct wake_lock slimport_lock;
	struct msm_hdmi_slimport_ops hdmi_ops;
	struct switch_dev hdcp_cap_sdev;
	struct switch_dev online_sdev;
	bool initialized;
	bool update_chg_type;
};

#ifdef QUICK_CHARGE_SUPPORT
extern struct blocking_notifier_head *get_notifier_list_head(void);
#endif

static struct class *slimport_class;
static struct device *slimport_class_dev;

/* Module parameter */
int phy_tuning[NUM_TUNING_VALS] = {0};

void slimport_publish_hdcp_cap(unchar hdcp_cap)
{
	static int first = 1;
	struct anx7816_data *anx7816 = NULL;
	anx7816 = i2c_get_clientdata(anx7816_client);

	if (first) {
		anx7816->hdcp_cap_sdev.state = 1 - hdcp_cap;
		first = 0;
	}
	switch_set_state(&anx7816->hdcp_cap_sdev, hdcp_cap);
	DEV_DBG("%s: hdcp cap switch = %d", __func__, hdcp_cap);
}

void slimport_publish_online(int online)
{
	struct anx7816_data *anx7816 = NULL;
	anx7816 = i2c_get_clientdata(anx7816_client);

	if (!anx7816)
		return;

	if (online != anx7816->online_sdev.state) {
		switch_set_state(&anx7816->online_sdev, online);
		DEV_DBG("%s: online switch = %d", __func__, online);
	}

	if (anx7816->hdmi_ops.out_set_upstream_hpd) {
		anx7816->hdmi_ops.out_set_upstream_hpd(
						    g_pdata->hdmi_pdev, online);
	}
}

static void sp_tx_limit_upstream_pclk(void)
{
	struct anx7816_data *anx7816 =
			i2c_get_clientdata(anx7816_client);

	if (anx7816->hdmi_ops.out_set_slimport_max_pclk) {
		int max_pclk;
		switch (sp_tx_cur_bw()) {
		case LINK_1P62G:
			max_pclk = 53000;
			break;
		case LINK_2P7G:
			max_pclk = 89000;
			break;
		case LINK_6P75G:
			max_pclk = 297000;
			break;
		case LINK_5P4G:
		default:
			max_pclk = 180000;
			break;
		}
		anx7816->hdmi_ops.out_set_slimport_max_pclk(
					anx7816->pdata->hdmi_pdev, max_pclk);
	}
}

struct completion init_aux_ch_completion;
static uint32_t sp_tx_chg_current_ma = NORMAL_CHG_I_MA;

void handle_chg_query(void)
{
	struct anx7816_data *anx7816 = NULL;
	anx7816 = i2c_get_clientdata(anx7816_client);

	if (anx7816->update_chg_type && anx7816->pdata->power_status) {
		sp_tx_chg_current_ma = sp_tx_get_chg_current();
		anx7816->update_chg_type = false;
		complete_all(&init_aux_ch_completion);
	}
}

uint32_t slimport_get_chg_current(void)
{
	struct anx7816_data *anx7816 = NULL;
	int ret;

	if (!anx7816_client) {
		DEV_NOTICE("%s: device not initialized", __func__);
		return false;
	}

	anx7816 = i2c_get_clientdata(anx7816_client);

	if (!anx7816 || !anx7816->initialized)
		return 0;

	INIT_COMPLETION(init_aux_ch_completion);
	anx7816->update_chg_type = true;

	ret = wait_for_completion_timeout(&init_aux_ch_completion,
					msecs_to_jiffies(2000));
	if (!ret) {
		DEV_ERR("failed to access charger type\n");
		return NORMAL_CHG_I_MA;
	}
	DEV_NOTICE("slimport current = %d", sp_tx_chg_current_ma);

	return sp_tx_chg_current_ma;
}
EXPORT_SYMBOL(slimport_get_chg_current);

bool slimport_dongle_is_connected(void)
{
	struct anx7816_platform_data *pdata = NULL;
	bool result = false;

	if (!anx7816_client)
		return false;

#ifdef CONFIG_OF
	pdata = g_pdata;
#else
	pdata = anx7816_client->dev.platform_data;
#endif

	if (!pdata)
		return false;

	if (gpio_get_value(pdata->gpio_cbl_det) == DONGLE_CABLE_INSERT
			/*gpio_get_value_cansleep(pdata->gpio_cbl_det)*/) {
			pr_info("%s %s : Slimport Dongle is detected\n",
				LOG_TAG, __func__);
			result = true;
		}

	return result;
}

static int slimport7816_vdd_power(unsigned int onoff)
{
#ifdef CONFIG_OF
	struct anx7816_platform_data *pdata = g_pdata;
#else
	struct anx7816_platform_data *pdata = anx7816_client->dev.platform_data;
#endif
	int rc = 0;

	if (regulator_is_enabled(pdata->vdd_10) == onoff) {
		pr_info("%s %s: vdd power is already %s",
				LOG_TAG, __func__, onoff ? "on" : "off");
		return rc;
	}

	if (onoff) {
		pr_info("%s %s: vdd power on\n", LOG_TAG, __func__);
		rc = regulator_enable(pdata->vdd_10);
		if (rc < 0) {
			pr_err("%s %s: failed to enable vdd regulator rc=%d\n",
				   LOG_TAG, __func__, rc);
		}
	} else {
			pr_info("%s %s: vdd power off\n", LOG_TAG, __func__);
			rc = regulator_disable(pdata->vdd_10);
	}

	return rc;
}

int g_emergency;
static ssize_t emergency_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct anx7816_data *anx7816 = NULL;
	int ret;
	long val;

	if (!anx7816_client) {
		DEV_NOTICE("%s: device not initialized", __func__);
		return -EINVAL;
	}

	anx7816 = i2c_get_clientdata(anx7816_client);

	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;

	if (val != 0 && val != 1) {
		DEV_ERR("Invalid emergency write value: %ld", val);
		return -EINVAL;
	}

	if (val != g_emergency) {
		g_emergency = val;
		if (g_emergency) {
			DEV_ERR("Emergency call, disconnect cable");
			cable_disconnect(anx7816);
		} else {
			DEV_ERR("Emergency call done");
		}
	}
	return count;
}

/* for control from user space */
static struct device_attribute slimport_class_attrs[] = {
	__ATTR(emergency, S_IWUSR, NULL, emergency_store),
	__ATTR_NULL
};

bool is_phy_tuning_set()
{
	int i;
	static bool phy_tuning_set;

	if (phy_tuning_set)
		return true;

	for (i = 0; i < NUM_TUNING_VALS; i++)
		if (phy_tuning[i]) {
			phy_tuning_set = true;
			return true;
		}

	return false;
}

ssize_t slimport7816_rev_check_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	int cmd;
	int result;

	result = sscanf(buf, "%d", &cmd);
	switch (cmd) {
	case 1:
		/*//sp_tx_chip_located();*/
		break;
	}
	return count;
}

static ssize_t slimport7816_rev_check_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", "anx7816");
}

/*sysfs interface : Enable or Disable HDCP by default*/
ssize_t sp_hdcp_feature_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", hdcp_enable);
}

ssize_t sp_hdcp_feature_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	hdcp_enable = val;
	pr_info(" hdcp_enable = %d\n", hdcp_enable);
	return count;
}

/*sysfs  interface : HDCP switch for VGA dongle*/
ssize_t sp_external_block_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", external_block_en);
}

ssize_t sp_external_block_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	external_block_en = val;
	return count;
}

/*sysfs  interface : i2c_master_read_reg, i2c_master_write_reg
anx7730 addr id:: DP_rx(0x50:0, 0x8c:1) HDMI_tx(0x72:5, 0x7a:6, 0x70:7)
ex:read ) 05df   = read:0  id:5 reg:0xdf
ex:write) 15df5f = write:1 id:5 reg:0xdf val:0x5f
*/
ssize_t anx7730_write_reg_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int ret = 0;
	char op, i;
	char r[3];
	char v[3];
	unchar tmp;
	int id, reg, val = 0;

	/*if (sp_tx_cur_states() != STATE_PLAY_BACK) {
		pr_err("%s: error!, Not STATE_PLAY_BACK\n", LOG_TAG);
		return -EINVAL;
	}*/

	/*if (sp_tx_cur_cable_type() != DWN_STRM_IS_HDMI_7730) {
		pr_err("%s: error!, rx is not anx7730\n", LOG_TAG);
		return -EINVAL;
	}*/

	if (count != 7 && count != 5) {
		pr_err("%s: cnt:%zu, invalid input!\n", LOG_TAG, count - 1);
		pr_err("%s: ex) 05df   -> op:0(read)  id:5 reg:0xdf\n",
		       LOG_TAG);
		pr_err("%s: ex) 15df5f -> op:1(wirte) id:5 reg:0xdf val:0x5f\n",
		       LOG_TAG);
		return -EINVAL;
	}

	ret = snprintf(&op, 2, buf);
	ret = snprintf(&i, 2, buf + 1);
	ret = snprintf(r, 3, buf + 2);

	id = simple_strtoul(&i, NULL, 10);
	reg = simple_strtoul(r, NULL, 16);

	if ((id != 0 && id != 1 && id != 5 && id != 6 && id != 7)) {
		pr_err("%s: invalid addr id! (id:0,1,5,6,7)\n", LOG_TAG);
		return -EINVAL;
	}

	switch (op) {
	case 0x30:		/* "0" -> read */
		i2c_master_read_reg(id, reg, &tmp);
		pr_info("%s: anx7730 read(%d,0x%x)= 0x%x\n", LOG_TAG, id, reg,
			tmp);
		break;

	case 0x31:		/* "1" -> write */
		ret = snprintf(v, 3, buf + 4);
		val = simple_strtoul(v, NULL, 16);

		i2c_master_write_reg(id, reg, val);
		i2c_master_read_reg(id, reg, &tmp);
		pr_info("%s: anx7730 write(%d,0x%x,0x%x)\n", LOG_TAG, id, reg,
			tmp);
		break;

	default:
		pr_err("%s: invalid operation code! (0:read, 1:write)\n",
		       LOG_TAG);
		return -EINVAL;
	}

	return count;
}

/*sysfs  interface : sp_read_reg, sp_write_reg
anx7816 addr id:: HDMI_rx(0x7e:0, 0x80:1) DP_tx(0x72:5, 0x7a:6, 0x70:7)
ex:read ) 05df   = read:0  id:5 reg:0xdf
ex:write) 15df5f = write:1 id:5 reg:0xdf val:0x5f
*/
static int anx7816_id_change(int id)
{
	int chg_id = 0;

	switch (id) {
	case 0:
		chg_id = RX_P0;
		break;
	case 1:
		chg_id = RX_P1;
		break;
	case 5:
		chg_id = TX_P2;
		break;
	case 6:
		chg_id = TX_P1;
		break;
	case 7:
		chg_id = TX_P0;
		break;
	}
	return chg_id;
}

static ssize_t anx7816_write_reg_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int ret = 0;
	char op, i;
	char r[3];
	char v[3];
	unchar tmp;
	int id, reg, val = 0;

	/*if (sp_tx_cur_states() != STATE_PLAY_BACK) {
		pr_err("%s: error!, Not STATE_PLAY_BACK\n", LOG_TAG);
		return -EINVAL;
	}*/

	if (count != 7 && count != 5) {
		pr_err("%s: cnt:%zu, invalid input!\n", LOG_TAG, count - 1);
		pr_err("%s: ex) 05df   -> op:0(read)  id:5 reg:0xdf\n",
		       LOG_TAG);
		pr_err("%s: ex) 15df5f -> op:1(wirte) id:5 reg:0xdf val:0x5f\n",
		       LOG_TAG);
		return -EINVAL;
	}

	ret = snprintf(&op, 2, buf);
	ret = snprintf(&i, 2, buf + 1);
	ret = snprintf(r, 3, buf + 2);

	id = simple_strtoul(&i, NULL, 10);
	reg = simple_strtoul(r, NULL, 16);

	if ((id != 0 && id != 1 && id != 5 && id != 6 && id != 7)) {
		pr_err("%s: invalid addr id! (id:0,1,5,6,7)\n", LOG_TAG);
		return -EINVAL;
	}

	id = anx7816_id_change(id);	/*//ex) 5 -> 0x72*/

	switch (op) {
	case 0x30:		/* "0" -> read */
		sp_read_reg(id, reg, &tmp);
		pr_info("%s: anx7816 read(0x%x,0x%x)= 0x%x\n", LOG_TAG, id,
			reg, tmp);
		break;

	case 0x31:		/* "1" -> write */
		ret = snprintf(v, 3, buf + 4);
		val = simple_strtoul(v, NULL, 16);

		sp_write_reg(id, reg, val);
		sp_read_reg(id, reg, &tmp);
		pr_info("%s: anx7816 write(0x%x,0x%x,0x%x)\n", LOG_TAG, id, reg,
			tmp);
		break;

	default:
		pr_err("%s: invalid operation code! (0:read, 1:write)\n",
		       LOG_TAG);
		return -EINVAL;
	}

	return count;
}

static ssize_t anx_dpcd_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	int ret = 0;
	char op;
	char offset[3];
	char val[3];
	unchar tmp, write_val;
	int reg;
	ret = snprintf(&op, 1, buf);
	ret = snprintf(offset, 2, buf+1);
	reg = simple_strtoul(offset, NULL, 16);
	switch (op) {
	case 0x30:
		/*read*/
		sp_tx_aux_dpcdread_bytes(0, 5, reg, 1, &tmp);
		pr_info("%s: anx7816 read(0x05,0x%x)= 0x%x\n", LOG_TAG,
			reg, tmp);
		break;
	case 0x31:
		/*write*/
		ret = snprintf(val, 2, buf+3);
		write_val = simple_strtoul(val, NULL, 16);
		sp_tx_aux_dpcdwrite_bytes(0, 5, reg, 1, &write_val);
		sp_tx_aux_dpcdread_bytes(0, 5, reg, 1, &tmp);
		pr_info("%s: anx7816 write(0x05,0x%x,0x%x)= 0x%x\n", LOG_TAG,
			reg, write_val,	tmp);
		break;
	}
	return ret;
}


#ifdef SP_REGISTER_SET_TEST	/*//Slimport test*/
/*sysfs read interface*/
static ssize_t ctrl_reg0_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG0);
}

/*sysfs write interface*/
static ssize_t ctrl_reg0_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG0 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg10_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG10);
}

/*sysfs write interface*/
static ssize_t ctrl_reg10_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG10 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg11_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG11);
}

/*sysfs write interface*/
static ssize_t ctrl_reg11_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG11 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg2_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG2);
}

/*sysfs write interface*/
static ssize_t ctrl_reg2_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG2 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg12_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG12);
}

/*sysfs write interface*/
static ssize_t ctrl_reg12_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG12 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg1_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG1);
}

/*sysfs write interface*/
static ssize_t ctrl_reg1_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG1 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg6_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG6);
}

/*sysfs write interface*/
static ssize_t ctrl_reg6_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG6 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg16_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG16);
}

/*sysfs write interface*/
static ssize_t ctrl_reg16_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG16 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg5_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG5);
}

/*sysfs write interface*/
static ssize_t ctrl_reg5_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG5 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg8_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG8);
}

/*sysfs write interface*/
static ssize_t ctrl_reg8_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG8 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg15_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG15);
}

/*sysfs write interface*/
static ssize_t ctrl_reg15_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG15 = val;
	return count;
}

/*sysfs read interface*/
static ssize_t ctrl_reg18_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%x\n", val_SP_TX_LT_CTRL_REG18);
}

/*sysfs write interface*/
static ssize_t ctrl_reg18_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret;
	long val;
	ret = strict_strtol(buf, 10, &val);
	if (ret)
		return ret;
	val_SP_TX_LT_CTRL_REG18 = val;
	return count;
}
#endif
/* for debugging */
static struct device_attribute slimport_device_attrs[] = {
	__ATTR(rev_check, S_IRUGO | S_IWUSR,
			slimport7816_rev_check_show, slimport7816_rev_check_store),
	__ATTR(hdcp_force, S_IRUGO | S_IWUSR,
			sp_external_block_show, sp_external_block_store),
	__ATTR(hdcp_en, S_IRUGO | S_IWUSR, sp_hdcp_feature_show,
			sp_hdcp_feature_store),
	__ATTR(anx7730, S_IWUSR, NULL, anx7730_write_reg_store),
	__ATTR(anx7816, S_IWUSR, NULL, anx7816_write_reg_store),
	__ATTR(DPCD, S_IWUSR, NULL, anx_dpcd_store),
#ifdef SP_REGISTER_SET_TEST	/*//slimport test*/
	__ATTR(ctrl_reg0, S_IRUGO | S_IWUSR, ctrl_reg0_show, ctrl_reg0_store),
	__ATTR(ctrl_reg10, S_IRUGO | S_IWUSR, ctrl_reg10_show,
	       ctrl_reg10_store),
	__ATTR(ctrl_reg11, S_IRUGO | S_IWUSR, ctrl_reg11_show,
	       ctrl_reg11_store),
	__ATTR(ctrl_reg2, S_IRUGO | S_IWUSR, ctrl_reg2_show, ctrl_reg2_store),
	__ATTR(ctrl_reg12, S_IRUGO | S_IWUSR, ctrl_reg12_show,
	       ctrl_reg12_store),
	__ATTR(ctrl_reg1, S_IRUGO | S_IWUSR, ctrl_reg1_show, ctrl_reg1_store),
	__ATTR(ctrl_reg6, S_IRUGO | S_IWUSR, ctrl_reg6_show, ctrl_reg6_store),
	__ATTR(ctrl_reg16, S_IRUGO | S_IWUSR, ctrl_reg16_show,
	       ctrl_reg16_store),
	__ATTR(ctrl_reg5, S_IRUGO | S_IWUSR, ctrl_reg5_show, ctrl_reg5_store),
	__ATTR(ctrl_reg8, S_IRUGO | S_IWUSR, ctrl_reg8_show, ctrl_reg8_store),
	__ATTR(ctrl_reg15, S_IRUGO | S_IWUSR, ctrl_reg15_show,
	       ctrl_reg15_store),
	__ATTR(ctrl_reg18, S_IRUGO | S_IWUSR, ctrl_reg18_show,
	       ctrl_reg18_store),
#endif
};

static int create_sysfs_interfaces(struct device *dev)
{
	int i;
	for (i = 0; i < ARRAY_SIZE(slimport_device_attrs); i++)
		if (device_create_file(dev, &slimport_device_attrs[i]))
			goto error;
	return 0;
error:
	for (; i >= 0; i--)
		device_remove_file(dev, &slimport_device_attrs[i]);
	pr_err("%s %s: Unable to create interface", LOG_TAG, __func__);
	return -EINVAL;
}

int sp_read_reg_byte(uint8_t slave_addr, uint8_t offset)
{
	int ret = 0;

	if (!g_pdata->power_status) {
		return -EIO;
	}

	anx7816_client->addr = (slave_addr >> 1);
	ret = i2c_smbus_read_byte_data(anx7816_client, offset);
	if (ret < 0) {
		pr_err("%s %s: failed to read i2c addr=%x\n", LOG_TAG,
		       __func__, slave_addr);
		return ret;
	}
	return 0;
}

int sp_read_reg(uint8_t slave_addr, uint8_t offset, uint8_t *buf)
{
	int ret = 0;

	if (!g_pdata->power_status) {
		return -EIO;
	}

	anx7816_client->addr = (slave_addr >> 1);
	ret = i2c_smbus_read_byte_data(anx7816_client, offset);
	if (ret < 0) {
		pr_err("%s %s: failed to read i2c addr=%x\n", LOG_TAG,
		       __func__, slave_addr);
		return ret;
	}
	*buf = (uint8_t) ret;

	return 0;
}

int sp_write_reg(uint8_t slave_addr, uint8_t offset, uint8_t value)
{
	int ret = 0;

	if (!g_pdata->power_status) {
		return -EIO;
	}

	anx7816_client->addr = (slave_addr >> 1);
	ret = i2c_smbus_write_byte_data(anx7816_client, offset, value);
	if (ret < 0) {
		pr_err("%s %s: failed to write i2c addr=%x\n", LOG_TAG,
		       __func__, slave_addr);
	}
	return ret;
}

void sp_tx_hardware_poweron(void)
{
#ifdef CONFIG_OF
	struct anx7816_platform_data *pdata = g_pdata;
#else
	struct anx7816_platform_data *pdata = anx7816_client->dev.platform_data;
#endif

	gpio_set_value(pdata->gpio_reset, 0);
	mdelay(1);
	pdata->vdd_power(1);
	mdelay(1);
	if (pdata->external_ldo_control) {
		/* Enable 1.0V LDO */
		gpio_set_value(pdata->gpio_v10_ctrl, 1);
		mdelay(1);
	}
	gpio_set_value(pdata->gpio_p_dwn, 0);
	mdelay(1);
	gpio_set_value(pdata->gpio_reset, 1);

	pdata->power_status = 1;
	pr_info("%s %s: anx7816 power on\n", LOG_TAG, __func__);
}

void sp_tx_hardware_powerdown(void)
{
#ifdef CONFIG_OF
	struct anx7816_platform_data *pdata = g_pdata;
#else
	struct anx7816_platform_data *pdata = anx7816_client->dev.platform_data;
#endif

	gpio_set_value(pdata->gpio_reset, 0);
	mdelay(1);
	slimport_publish_online(0);
	pdata->vdd_power(0);
	mdelay(1);
	if (pdata->external_ldo_control) {
		gpio_set_value(pdata->gpio_v10_ctrl, 0);
		mdelay(1);
	}
	gpio_set_value(pdata->gpio_p_dwn, 1);
	mdelay(1);

	pdata->power_status = 0;
	pr_info("%s %s: anx7816 power down\n", LOG_TAG, __func__);
}

int slimport_read_edid_block(int block, uint8_t *edid_buf)
{
	if (block == 0) {
		sp_tx_limit_upstream_pclk();
		memcpy(edid_buf, edid_blocks, 128 * sizeof(char));
	} else if (block == 1) {
		memcpy(edid_buf, (edid_blocks + 128), 128 * sizeof(char));
	} else {
		pr_err("%s %s: block number %d is invalid\n",
		       LOG_TAG, __func__, block);
		return -EINVAL;
	}

	return 0;
}

static void anx7816_free_gpio(struct anx7816_data *anx7816)
{
	gpio_free(anx7816->pdata->gpio_cbl_det);
	gpio_free(anx7816->pdata->gpio_reset);
	gpio_free(anx7816->pdata->gpio_p_dwn);
	if (anx7816->pdata->external_ldo_control) {
		gpio_free(anx7816->pdata->gpio_v10_ctrl);
		gpio_free(anx7816->pdata->gpio_v33_ctrl);
	}
}

static int anx7816_init_gpio(struct anx7816_data *anx7816)
{
	int ret = 0;

	pr_info("%s %s: anx7816 init gpio\n", LOG_TAG, __func__);
	/*  gpio for chip power down  */
	ret = gpio_request(anx7816->pdata->gpio_p_dwn, "anx7816_p_dwn_ctl");
	if (ret) {
		pr_err("%s : failed to request gpio %d\n", __func__,
		       anx7816->pdata->gpio_p_dwn);
		goto err0;
	}
	gpio_direction_output(anx7816->pdata->gpio_p_dwn, 1);
	/*  gpio for chip reset  */
	ret = gpio_request(anx7816->pdata->gpio_reset, "anx7816_reset_n");
	if (ret) {
		pr_err("%s : failed to request gpio %d\n", __func__,
		       anx7816->pdata->gpio_reset);
		goto err1;
	}
	gpio_direction_output(anx7816->pdata->gpio_reset, 0);
	/*  gpio for slimport cable detect  */
	ret = gpio_request(anx7816->pdata->gpio_cbl_det, "anx7816_cbl_det");
	if (ret) {
		pr_err("%s : failed to request gpio %d\n", __func__,
		       anx7816->pdata->gpio_cbl_det);
		goto err2;
	}
	gpio_direction_input(anx7816->pdata->gpio_cbl_det);
	/*  gpios for power control */
	if (anx7816->pdata->external_ldo_control) {
		/* V10 power control */
		ret = gpio_request(anx7816->pdata->gpio_v10_ctrl,
				   "anx7816_v10_ctrl");
		if (ret) {
			pr_err("%s : failed to request gpio %d\n",
			       __func__, anx7816->pdata->gpio_v10_ctrl);
			goto err3;
		}
		gpio_direction_output(anx7816->pdata->gpio_v10_ctrl, 0);
		/* V33 power control */
		ret = gpio_request(anx7816->pdata->gpio_v33_ctrl,
				   "anx7816_v33_ctrl");
		if (ret) {
			pr_err("%s : failed to request gpio %d\n",
			       __func__, anx7816->pdata->gpio_v33_ctrl);
			goto err4;
		}
		gpio_direction_output(anx7816->pdata->gpio_v33_ctrl, 0);

	}

	goto out;

err4:
	gpio_free(anx7816->pdata->gpio_v33_ctrl);
err3:
	gpio_free(anx7816->pdata->gpio_v10_ctrl);
err2:
	gpio_free(anx7816->pdata->gpio_cbl_det);
err1:
	gpio_free(anx7816->pdata->gpio_reset);
err0:
	gpio_free(anx7816->pdata->gpio_p_dwn);
out:
	return ret;
}

static int anx7816_system_init(void)
{
	int ret = 0;

	ret = slimport_chip_detect();
	if (ret == 0) {
		sp_tx_hardware_powerdown();
		pr_err("%s : failed to detect anx7816\n", __func__);
		return -ENODEV;
	}

	slimport_chip_initial();
	return 0;
}

void cable_disconnect(void *data)
{
	struct anx7816_data *anx7816 = data;
	cancel_delayed_work_sync(&anx7816->work);
	flush_workqueue(anx7816->workqueue);
#ifdef QUICK_CHARGE_SUPPORT
	disable_otg();
	pmic_recovery();
	reset_process();
#endif
	sp_tx_hardware_powerdown();
	sp_tx_clean_state_machine();
	wake_unlock(&anx7816->slimport_lock);
	wake_lock_timeout(&anx7816->slimport_lock, 2 * HZ);

}

/*JIRA: CLD-110,
Software patch for cable det pin has glitch before stable "High"*/
#define CABLE_DET_PIN_HAS_GLITCH
static unsigned char confirmed_cable_det(void *data)
{
	struct anx7816_data *anx7816 = data;
	#ifdef CABLE_DET_PIN_HAS_GLITCH
	unsigned char count = 10;
	unsigned char cable_det_count = 0;
	do {
		if (gpio_get_value(anx7816->pdata->gpio_cbl_det)
				== DONGLE_CABLE_INSERT)
			cable_det_count++;
		mdelay(5);
	} while (count--);
	return (cable_det_count > 5) ? DONGLE_CABLE_INSERT : 1-DONGLE_CABLE_INSERT;
	#else
	return gpio_get_value(anx7816->pdata->gpio_cbl_det);
	#endif
}

static irqreturn_t anx7816_cbl_det_isr(int irq, void *data)
{
	struct anx7816_data *anx7816 = data;
	int cable_connected = 0;
	cable_connected = confirmed_cable_det(data);
	pr_info("%s %s : detect cable insertion, cable_connected = %d\n",
					LOG_TAG, __func__, cable_connected);
	if (cable_connected == DONGLE_CABLE_INSERT) {
		wake_lock(&anx7816->slimport_lock);
#ifdef QUICK_CHARGE_SUPPORT
		reset_process();
#endif
		pr_info("%s %s : detect cable insertion\n", LOG_TAG, __func__);
		queue_delayed_work(anx7816->workqueue, &anx7816->work, 0);
	} else {
		pr_info("%s %s : detect cable removal\n", LOG_TAG, __func__);
		cable_disconnect(anx7816);
		/*msleep(1000);*/
	}
	return IRQ_HANDLED;
}

static void anx7816_work_func(struct work_struct *work)
{
	struct anx7816_data *td = container_of(work, struct anx7816_data,
					       work.work);
	int workqueu_timer = 0;
	if (sp_tx_cur_states() >= STATE_AUDIO_OUTPUT)
		workqueu_timer = 500;
	else
		workqueu_timer = 100;
	mutex_lock(&td->lock);
	slimport_main_process();
#ifdef QUICK_CHARGE_SUPPORT
	if (sp_tx_cur_states() >= STATE_SINK_CONNECTION) {
		quick_charge_main_process();
	}
#endif
	mutex_unlock(&td->lock);
	queue_delayed_work(td->workqueue, &td->work,
			   msecs_to_jiffies(workqueu_timer));
}

#ifdef CONFIG_OF
int anx7816_regulator_configure(
	struct device *dev, struct anx7816_platform_data *pdata)
{
	int rc = 0;

	pdata->vdd_10 = regulator_get(dev, "vdd10");

	if (IS_ERR(pdata->vdd_10)) {
		rc = PTR_ERR(pdata->vdd_10);
		pr_err("%s %s : Regulator get failed vdd10 rc=%d\n",
			   LOG_TAG, __func__, rc);
		return rc;
	}

	if (regulator_count_voltages(pdata->vdd_10) > 0) {
		rc = regulator_set_voltage(pdata->vdd_10, 1000000,
							1000000);
		if (rc) {
			pr_err("%s %s : Regulator set_vtg failed rc=%d\n",
				   LOG_TAG, __func__, rc);
			goto error_set_vtg_vdd_10;
		}
		pr_err("%s: set voltage to 1M succeeded", LOG_TAG);
	}

	return 0;

error_set_vtg_vdd_10:
	regulator_put(pdata->vdd_10);

	return rc;
}

static int anx7816_parse_dt(struct device *dev,
			    struct anx7816_platform_data *pdata)
{
	int rc = 0;
	struct device_node *np = dev->of_node;
	struct platform_device *hdmi_pdev = NULL;
	struct device_node *hdmi_tx_node = NULL;

	pdata->gpio_p_dwn =
	    of_get_named_gpio_flags(np, "analogix,p-dwn-gpio", 0, NULL);

	pdata->gpio_reset =
	    of_get_named_gpio_flags(np, "analogix,reset-gpio", 0, NULL);

	pdata->gpio_cbl_det =
	    of_get_named_gpio_flags(np, "analogix,cbl-det-gpio", 0, NULL);

	pr_info(
	       "%s gpio p_dwn : %d, reset : %d,  gpio_cbl_det %d\n",
	       LOG_TAG, pdata->gpio_p_dwn,
	       pdata->gpio_reset, pdata->gpio_cbl_det);
	/*
	 * if "external-ldo-control" property is not exist, we
	 * assume that it is used in board.
	 * if don't use external ldo control,
	 * please use "external-ldo-control=<0>" in dtsi
	 */
	rc = of_property_read_u32(np, "analogix,external-ldo-control",
				  &pdata->external_ldo_control);
	if (rc == -EINVAL)
		pdata->external_ldo_control = 1;

	if (pdata->external_ldo_control) {
		pdata->gpio_v10_ctrl =
		    of_get_named_gpio_flags(np, "analogix,v10-ctrl-gpio", 0,
					    NULL);

		pdata->gpio_v33_ctrl =
		    of_get_named_gpio_flags(np, "analogix,v33-ctrl-gpio", 0,
					    NULL);

		pr_info("%s gpio_v10_ctrl %d avdd33-en-gpio %d\n",
		       LOG_TAG, pdata->gpio_v10_ctrl, pdata->gpio_v33_ctrl);
	}

	if (anx7816_regulator_configure(dev, pdata) < 0) {
		pr_err("%s %s: parsing dt for anx7816 is failed.\n",
		       LOG_TAG, __func__);
		return rc;
	}

	/* connects function nodes which are not provided with dts */
	pdata->vdd_power = slimport7816_vdd_power;

	/* parse phandle for hdmi tx */
	hdmi_tx_node = of_parse_phandle(np, "qcom,hdmi-tx-map", 0);
	if (!hdmi_tx_node) {
		pr_err("%s: can't find hdmi phandle\n", __func__);
		return -EINVAL;
	}

	hdmi_pdev = of_find_device_by_node(hdmi_tx_node);
	if (!hdmi_pdev) {
		pr_err("%s: can't find the device by node\n", __func__);
		return -EINVAL;
	}

	pdata->hdmi_pdev = hdmi_pdev;

	if (!is_phy_tuning_set()) {
		struct property *prop;
		int len;
		prop = of_find_property(np, "oem,phy_tuning", NULL);
		if (!prop) {
			pr_err("Unable to find oem,phy_tuning");
			return 0;
		}

		len = prop->length / sizeof(int);
		if (len != NUM_TUNING_VALS) {
			pr_err("Invalid size for phy_tuning: %d (actual) != %d",
					len, NUM_TUNING_VALS);
			return 0;
		}

		rc = of_property_read_u32_array(np, "oem,phy_tuning",
				(u32 *) phy_tuning, prop->length / sizeof(u32));

		if (rc)
			pr_err("Unable to read oem,phy_tuning");
	}

	return 0;
}
#else
static int anx7816_parse_dt(struct device *dev,
			    struct anx7816_platform_data *pdata)
{
	return -ENODEV;
}
#endif

static int anx7816_i2c_probe(struct i2c_client *client,
			     const struct i2c_device_id *id)
{

	struct anx7816_data *anx7816;
	struct anx7816_platform_data *pdata;
	int ret = 0;

	pr_info("%s %s start\n", LOG_TAG, __func__);

#ifdef SP_REGISTER_SET_TEST
	val_SP_TX_LT_CTRL_REG0 = 0x19;
	val_SP_TX_LT_CTRL_REG10 = 0x00;
	val_SP_TX_LT_CTRL_REG11 = 0x00;
	val_SP_TX_LT_CTRL_REG2 = 0x36;
	val_SP_TX_LT_CTRL_REG12 = 0x00;
	val_SP_TX_LT_CTRL_REG1 = 0x26;
	val_SP_TX_LT_CTRL_REG6 = 0x3c;
	val_SP_TX_LT_CTRL_REG16 = 0x18;
	val_SP_TX_LT_CTRL_REG5 = 0x28;
	val_SP_TX_LT_CTRL_REG8 = 0x2F;
	val_SP_TX_LT_CTRL_REG15 = 0x10;
	val_SP_TX_LT_CTRL_REG18 = 0x1F;
#endif
	if (!i2c_check_functionality(client->adapter,
	I2C_FUNC_SMBUS_I2C_BLOCK)) {
		pr_err("%s: i2c bus does not support the anx7816\n", __func__);
		ret = -ENODEV;
		goto exit;
	}

	anx7816 = kzalloc(sizeof(struct anx7816_data), GFP_KERNEL);
	if (!anx7816) {
		pr_err("%s: failed to allocate driver data\n", __func__);
		ret = -ENOMEM;
		goto exit;
	}

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
				     sizeof(struct anx7816_platform_data),
				     GFP_KERNEL);
		if (!pdata) {
			pr_err("%s: Failed to allocate memory\n", __func__);
			return -ENOMEM;
		}
		client->dev.platform_data = pdata;
		/* device tree parsing function call */
		ret = anx7816_parse_dt(&client->dev, pdata);
		if (ret != 0)	/* if occurs error */
			goto err0;

		anx7816->pdata = pdata;
	} else {
		anx7816->pdata = client->dev.platform_data;
	}

	if (anx7816->pdata->hdmi_pdev) {
		struct msm_hdmi_slimport_ops *ops = &anx7816->hdmi_ops;
		ops->in_read_edid_block = slimport_read_edid_block;
		ret = msm_hdmi_register_slimport(anx7816->pdata->hdmi_pdev,
			ops, NULL);
		if (ret) {
			ret = -EPROBE_DEFER;
			DEV_ERR("Failed to register with msm hdmi: %d", ret);
			goto err0;
		}
		DEV_ERR("Registered with msm hdmi");

		if (ops->out_set_slimport_max_pclk) {
			/* Max pclk of 180MHz chosen based on logic
			 * in sp_tx_bw_lc_sel which determines that
			 * we are over supported bandwidth if
			 * the input pclk is greater than 180MHz
			 */
			/* Setting very low for now while debugging issues
			with higher resolutions */
			ops->out_set_slimport_max_pclk(
					anx7816->pdata->hdmi_pdev, 180000);
		}
	}

	init_completion(&init_aux_ch_completion);

	/* to access global platform data */
	g_pdata = anx7816->pdata;

	anx7816_client = client;

	mutex_init(&anx7816->lock);

	if (!anx7816->pdata) {
		ret = -EINVAL;
		goto err0;
	}

	ret = anx7816_init_gpio(anx7816);
	if (ret) {
		pr_err("%s: failed to initialize gpio\n", __func__);
		ret = -EPROBE_DEFER;
		goto err0;
	}

	INIT_DELAYED_WORK(&anx7816->work, anx7816_work_func);

	anx7816->workqueue = create_singlethread_workqueue("anx7816_work");
	if (anx7816->workqueue == NULL) {
		pr_err("%s: failed to create work queue\n", __func__);
		ret = -ENOMEM;
		goto err1;
	}
	/*anx7816->pdata->avdd_power(1);
	anx7816->pdata->dvdd_power(1);*/

	ret = anx7816_system_init();
	if (ret) {
		pr_err("%s: failed to initialize anx7816\n", __func__);
		goto err2;
	}

	client->irq = gpio_to_irq(anx7816->pdata->gpio_cbl_det);
	if (client->irq < 0) {
		pr_err("%s : failed to get gpio irq\n", __func__);
		goto err2;
	}

	wake_lock_init(&anx7816->slimport_lock,
		       WAKE_LOCK_SUSPEND, "slimport_wake_lock");

	ret = request_threaded_irq(client->irq, NULL, anx7816_cbl_det_isr,
				   IRQF_TRIGGER_RISING
				   | IRQF_TRIGGER_FALLING
				   | IRQF_ONESHOT, "anx7816", anx7816);
	if (ret < 0) {
		pr_err("%s : failed to request irq\n", __func__);
		goto err2_5;
	}

	ret = irq_set_irq_wake(client->irq, 1);
	if (ret < 0) {
		pr_err("%s : Request irq for cable detect", __func__);
		pr_err("interrupt wake set fail\n");
		goto err3;
	}

	ret = enable_irq_wake(client->irq);
	if (ret < 0) {
		pr_err("%s : Enable irq for cable detect", __func__);
		pr_err("interrupt wake enable fail\n");
		goto err3;
	}

	ret = create_sysfs_interfaces(&client->dev);
	if (ret < 0) {
		pr_err("%s : sysfs register failed", __func__);
		goto err3;
	}

	anx7816->hdcp_cap_sdev.name = "hdmi_bridge_secure";
	ret = switch_dev_register(&anx7816->hdcp_cap_sdev);
	if (ret < 0) {
		DEV_ERR("secure switch registration failed %d\n", ret);
		goto err4;
	}

	anx7816->online_sdev.name = "hdmi_bridge";
	ret = switch_dev_register(&anx7816->online_sdev);
	if (ret < 0) {
		DEV_ERR("online switch registration failed %d\n", ret);
		goto err5;
	}

	if (slimport_class) {
		slimport_class_dev = device_create(slimport_class, &client->dev,
								0, NULL,
								"anx78xx");
		if (slimport_class_dev == NULL)
			DEV_ERR("Failed to create slimport class device");
	}
	i2c_set_clientdata(client, anx7816);
	anx7816->initialized = true;

	/*QC2.0*/
#ifdef QUICK_CHARGE_SUPPORT
	BLOCKING_INIT_NOTIFIER_HEAD(get_notifier_list_head());
#endif
	pr_info("%s %s end\n", LOG_TAG, __func__);
	goto exit;

err5:
	switch_dev_unregister(&anx7816->hdcp_cap_sdev);
err4:
	{
		int i = 0;
		for (i = 0; i < ARRAY_SIZE(slimport_device_attrs); i++)
			device_remove_file(&client->dev,
					&slimport_device_attrs[i]);
	}
err3:
	free_irq(client->irq, anx7816);
err2_5:
	wake_lock_destroy(&anx7816->slimport_lock);
err2:
	destroy_workqueue(anx7816->workqueue);
err1:
	anx7816_free_gpio(anx7816);
err0:
	if (anx7816->pdata->vdd_10)
		regulator_put(anx7816->pdata->vdd_10);
	anx7816_client = NULL;
	kfree(anx7816);
exit:
	return ret;
}

static int anx7816_i2c_remove(struct i2c_client *client)
{
	struct anx7816_data *anx7816 = i2c_get_clientdata(client);
	int i = 0;
	for (i = 0; i < ARRAY_SIZE(slimport_device_attrs); i++)
		device_remove_file(&client->dev, &slimport_device_attrs[i]);
	pr_info("anx7816_i2c_remove\n");
	sp_tx_clean_state_machine();
	destroy_workqueue(anx7816->workqueue);
	sp_tx_hardware_powerdown();
	free_irq(client->irq, anx7816);
	device_destroy(slimport_class, 0);
	switch_dev_unregister(&anx7816->hdcp_cap_sdev);
	switch_dev_unregister(&anx7816->online_sdev);
	regulator_put(anx7816->pdata->vdd_10);
	anx7816_free_gpio(anx7816);
	wake_lock_destroy(&anx7816->slimport_lock);
	kfree(anx7816);
	return 0;
}

bool is_slimport_vga(void)
{
	return ((sp_tx_cur_cable_type() == DWN_STRM_IS_VGA_9832)
		|| (sp_tx_cur_cable_type() == DWN_STRM_IS_ANALOG)) ? 1 : 0;
}

/* 0x01: hdmi device is attached
    0x02: DP device is attached
    0x03: Old VGA device is attached // RX_VGA_9832
    0x04: new combo VGA device is attached // RX_VGA_GEN
    0x00: unknow device            */
bool is_slimport_dp(void)
{
	return (sp_tx_cur_cable_type() == DWN_STRM_IS_DIGITAL) ? TRUE : FALSE;
}

unchar sp_get_link_bw(void)
{
	return sp_tx_cur_bw();
}

void sp_set_link_bw(unchar link_bw)
{
	sp_tx_set_bw(link_bw);
}

static int anx7816_i2c_suspend(struct i2c_client *client, pm_message_t state)
{
	return 0;
}

static int anx7816_i2c_resume(struct i2c_client *client)
{
	return 0;
}

static const struct i2c_device_id anx7816_id[] = {
	{"anx7816", 0},
	{}
};

MODULE_DEVICE_TABLE(i2c, anx7816_id);

#ifdef CONFIG_OF
static struct of_device_id anx_match_table[] = {
	{.compatible = "analogix,anx7816",},
	{},
};
#endif

static struct i2c_driver anx7816_driver = {
	.driver = {
		   .name = "anx7816",
		   .owner = THIS_MODULE,
#ifdef CONFIG_OF
		   .of_match_table = anx_match_table,
#endif
		   },
	.probe = anx7816_i2c_probe,
	.remove = anx7816_i2c_remove,
	.suspend = anx7816_i2c_suspend,
	.resume = anx7816_i2c_resume,
	.id_table = anx7816_id,
};

static void __init anx7816_init_async(void *data, async_cookie_t cookie)
{
	int ret = 0;

	slimport_class = class_create(THIS_MODULE, "slimport");
	if (IS_ERR(slimport_class)) {
		pr_err("%s: Failed to create slimport class: %ld",
							LOG_TAG, PTR_ERR(slimport_class));
		return;
	}

	slimport_class->suspend = NULL;
	slimport_class->resume = NULL;
	slimport_class->dev_attrs = slimport_class_attrs;

	ret = i2c_add_driver(&anx7816_driver);
	if (ret < 0)
		pr_err("%s: failed to register anx7816 i2c drivern", __func__);
}

static int __init anx7816_init(void)
{
	async_schedule(anx7816_init_async, NULL);
	return 0;
}

static void __exit anx7816_exit(void)
{
	i2c_del_driver(&anx7816_driver);
}

module_init(anx7816_init);
module_exit(anx7816_exit);

MODULE_DESCRIPTION("Slimport  transmitter ANX7816 driver");
MODULE_AUTHOR("Junhua Xia <jxia@analogixsemi.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.5");

module_param_array(phy_tuning, int, NULL, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(phy_tuning, "Alter 20-elem phy tuning array");
