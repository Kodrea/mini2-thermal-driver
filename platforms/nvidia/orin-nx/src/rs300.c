// SPDX-License-Identifier: GPL-2.0
/*
 * Mini2 / WN2 Uncooled Microbolometer Thermal Camera driver
 *
 * Copyright (C) 2017 Fuzhou Rockchip Electronics Co., Ltd.
 * Copyright (C) 2026 Kodrea
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/clk.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/media.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/videodev2.h>
#include <linux/version.h>
#include <media/camera_common.h>
#include <media/media-entity.h>
#include <media/tegra-v4l2-camera.h>
#include <media/tegracam_core.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mediabus.h>
#include <media/v4l2-subdev.h>


#define DRIVER_VERSION			KERNEL_VERSION(0, 0x01, 0x1)
#define DRIVER_NAME "rs300"
//80M (clk)* 2(double ) *2 (lan) /8

#define RS300_LINK_RATE (80 * 1000 * 1000)       /* 80MHz link rate matching device tree */
#define RS300_PIXEL_RATE	(40 * 1000 * 1000)   /* 80 MHz D-PHY clock, 2 lanes, 8-bit bus */
#define RS300_PIXEL_RATE_16BIT	(20 * 1000 * 1000)
#define RS300_UYVY_MBUS_CODE MEDIA_BUS_FMT_UYVY8_2X8
#define RS300_DEFAULT_MBUS_CODE RS300_UYVY_MBUS_CODE
#define RS300_OUTPUT_MODE_YUV 0
#define RS300_OUTPUT_MODE_Y16 1
#define RS300_OUTPUT_MODE_DEFAULT RS300_OUTPUT_MODE_Y16
#define RS300_BRIGHTNESS_MIN 0
#define RS300_BRIGHTNESS_MAX 100
#define RS300_BRIGHTNESS_STEP 10
#define RS300_BRIGHTNESS_DEFAULT 50
#define V4L2_CID_CUSTOM_BASE (V4L2_CID_USER_BASE + 1000 )
#define RS300_CID_BRIGHTNESS (V4L2_CID_CUSTOM_BASE + 19)
#define RS300_CID_CONTRAST (V4L2_CID_CUSTOM_BASE + 20)
#define RS300_STREAM_START_RETRIES 3

#ifndef TEGRA_CAMERA_CID_BASE
#define TEGRA_CAMERA_CID_BASE		(V4L2_CTRL_CLASS_CAMERA | 0x2000)
#define TEGRA_CAMERA_CID_SENSOR_MODE_ID		(TEGRA_CAMERA_CID_BASE + 8)
#define TEGRA_CAMERA_CID_VI_BYPASS_MODE		(TEGRA_CAMERA_CID_BASE + 100)
#define TEGRA_CAMERA_CID_OVERRIDE_ENABLE	(TEGRA_CAMERA_CID_BASE + 101)
#define TEGRA_CAMERA_CID_VI_HEIGHT_ALIGN	(TEGRA_CAMERA_CID_BASE + 102)
#define TEGRA_CAMERA_CID_VI_SIZE_ALIGN		(TEGRA_CAMERA_CID_BASE + 103)
#define TEGRA_CAMERA_CID_LOW_LATENCY		(TEGRA_CAMERA_CID_BASE + 109)
#define TEGRA_CAMERA_CID_VI_PREFERRED_STRIDE	(TEGRA_CAMERA_CID_BASE + 110)
#define TEGRA_CAMERA_CID_VI_CAPTURE_TIMEOUT	(TEGRA_CAMERA_CID_BASE + 111)
#endif

/* Define colormap menu items with the actual names */
static const char * const colormap_menu[] = {
    "White Hot",           /* 0 */
    "Reserved",            /* 1 */
    "Sepia",               /* 2 */
    "Ironbow",             /* 3 */
    "Rainbow",             /* 4 */
    "Night",               /* 5 */
    "Aurora",              /* 6 */
    "Red Hot",             /* 7 */
    "Jungle",              /* 8 */
    "Medical",             /* 9 */
    "Black Hot",           /* 10 */
    "Golden Red Glory_Hot", /* 11 */
    NULL
};

/* Define scene mode menu items */
static const char * const scene_mode_menu[] = {
    "Low",                /* 0 */
    "Linear Stretch",     /* 1 */
    "Low Contrast",       /* 2 */
    "General Mode",       /* 3 */
    "High Contrast",      /* 4 */
    "Highlight",          /* 5 */
    "Reserved 1",         /* 6 */
    "Reserved 2",         /* 7 */
    "Reserved 3",         /* 8 */
    "Outline Mode",       /* 9 */
    NULL
};

/* Define output mode menu items */
static const char * const output_mode_menu[] = {
    "YUV Output",         /* 0 - 8-bit YUV (default) */
    "Y16 Output",         /* 1 - raw 16-bit thermal */
    NULL
};

#define NUM_COLORMAP_ITEMS (ARRAY_SIZE(colormap_menu) - 1) // Account for NULL terminator

/* Physical Mini2/WN2 modules have a fixed resolution; select it at load time. */
static int mode = 0; // 0-640; 1-256; 2-384
static int fps = 60;
static int set_fps_cmd = 0;
static int type = 16;
static int output_source = -1;
static int yuv_order = 2;
static int start_path = 1;
static int start_dst = 1;
static int start_width = 0;
static int start_height = 0;
static int advertise_width = 0;
static int advertise_height = 0;
static int probe_prime_ms = 0;
static int probe_prime_delay_ms = 0;
static int probe_prime_stop_ms = 300;
static int prestart_prime_cycles = 0;
static int prestart_prime_ms = 1000;
static int prestart_prime_stop_ms = 300;
static int power_on_delay_ms = 8000;
static int auto_shutter = 0;
static int startup_ffc = 0;
static int native_y16 = 0;
module_param(mode, int, 0644);
module_param(fps, int, 0644);
module_param(set_fps_cmd, int, 0644);
module_param(type, int, 0644);
module_param(output_source, int, 0644);
module_param(yuv_order, int, 0644);
module_param(start_path, int, 0644);
module_param(start_dst, int, 0644);
module_param(start_width, int, 0644);
module_param(start_height, int, 0644);
module_param(advertise_width, int, 0644);
module_param(advertise_height, int, 0644);
module_param(probe_prime_ms, int, 0644);
module_param(probe_prime_delay_ms, int, 0644);
module_param(probe_prime_stop_ms, int, 0644);
module_param(prestart_prime_cycles, int, 0644);
module_param(prestart_prime_ms, int, 0644);
module_param(prestart_prime_stop_ms, int, 0644);
module_param(power_on_delay_ms, int, 0644);
module_param(auto_shutter, int, 0644);
module_param(startup_ffc, int, 0644);
module_param(native_y16, int, 0644);
MODULE_PARM_DESC(mode, "Sensor mode index: 0=640x512, 1=256x192, 2=384x288");
MODULE_PARM_DESC(fps, "Frame rate sent to the RS300 start packet");
MODULE_PARM_DESC(set_fps_cmd, "Send the separate RS300 FPS command before START");
MODULE_PARM_DESC(type, "RS300 start-packet source/type byte");
MODULE_PARM_DESC(output_source, "RS300 output-source command value: -1=skip, 0..5=send value");
MODULE_PARM_DESC(yuv_order, "RS300 YUV order command: -1=skip, 0=UYVY, 1=VYUY, 2=YUYV, 3=YVYU");
MODULE_PARM_DESC(start_path, "RS300 start-packet path byte");
MODULE_PARM_DESC(start_dst, "RS300 start-packet destination byte");
MODULE_PARM_DESC(start_width, "Optional RS300 start-packet width override; 0 uses current mode width");
MODULE_PARM_DESC(start_height, "Optional RS300 start-packet height override; 0 uses current mode height");
MODULE_PARM_DESC(advertise_width, "Optional V4L2/Tegra advertised width override; 0 uses selected sensor mode width");
MODULE_PARM_DESC(advertise_height, "Optional V4L2/Tegra advertised height override; 0 uses selected sensor mode height");
MODULE_PARM_DESC(probe_prime_ms, "Start camera for this many ms during probe, then stop; 0 disables");
MODULE_PARM_DESC(probe_prime_delay_ms, "Delay before probe-time camera prime");
MODULE_PARM_DESC(probe_prime_stop_ms, "Delay after probe-time STOP command");
MODULE_PARM_DESC(prestart_prime_cycles, "START/STOP cycles inside first stream start before final START");
MODULE_PARM_DESC(prestart_prime_ms, "Delay after each prestart prime START command");
MODULE_PARM_DESC(prestart_prime_stop_ms, "Delay after each prestart prime STOP command");
MODULE_PARM_DESC(power_on_delay_ms, "Delay after asserting power GPIO/regulators before I2C/CSI use");
MODULE_PARM_DESC(auto_shutter, "Initial auto-shutter state: -1=leave camera default, 0=off, 1=on");
MODULE_PARM_DESC(startup_ffc, "Attempt one manual FFC/shutter calibration before the first stream start");
MODULE_PARM_DESC(native_y16, "Experimental: request V4L2_PIX_FMT_Y16 for output_mode=1 over the stable UYVY CSI transport; 0 keeps UYVY fallback");

/*
 * rs300 register definitions
 */
//The get command is not only for reading but also for writing, all of which require _IOWR
//_IOWR/_IOR will automatically make a shallow copy to user space. If any parameter type contains a pointer, you need to call copy_to_user yourself.
//_IOW will automatically copy user space parameters to the kernel and pointers will also be copied, without calling copy_from_user.

#define CMD_MAGIC 0xEF //Define magic number
#define CMD_MAX_NR 3 //Defines the maximum ordinal number of commands
#define CMD_GET _IOWR(CMD_MAGIC, 1,struct ioctl_data)
#define CMD_SET _IOW(CMD_MAGIC, 2,struct ioctl_data)
#define CMD_KBUF _IO(CMD_MAGIC, 3)
//This is the private command configuration recommended by the v4l2 standard. You can also use custom commands directly here.
//#define CMD_GET _IOWR('V', BASE_VIDIOC_PRIVATE + 11,struct ioctl_data)
//#define CMD_SET _IOW('V', BASE_VIDIOC_PRIVATE + 12,struct ioctl_data)

//The structure is consistent with usb-i2c, and the valid bits are Register address: wIndex Data pointer: data Data length: wLength
//(from original driver)
struct ioctl_data{
	unsigned char bRequestType;
	unsigned char bRequest;
	unsigned short wValue;
	unsigned short wIndex;
	unsigned char* data;
	unsigned short wLength;
	unsigned int timeout;		///< unit:ms
};

#define REG_NULL			0xFFFF	/* Array end token */

#define I2C_VD_BUFFER_RW			0x1D00
#define I2C_VD_BUFFER_HLD			0x9D00
#define I2C_VD_CHECK_ACCESS			0x8000
#define I2C_VD_BUFFER_DATA_LEN		256
#define I2C_OUT_BUFFER_MAX			64 // IN buffer set equal to I2C_VD_BUFFER_DATA_LEN(256)
#define I2C_TRANSFER_WAIT_TIME_2S	2000
#define MAX_I2C_TRANSFER_SIZE		256  /* Maximum I2C transfer size (security limit) */

#define I2C_VD_BUFFER_STATUS			0x0200
#define VCMD_BUSY_STS_BIT				0x01
#define VCMD_RST_STS_BIT				0x02
#define VCMD_ERR_STS_BIT				0xFC

#define VCMD_BUSY_STS_IDLE				0x00
#define VCMD_BUSY_STS_BUSY				0x01
#define VCMD_RST_STS_PASS				0x00
#define VCMD_RST_STS_FAIL				0x01

#define VCMD_ERR_STS_SUCCESS				0x00
#define VCMD_ERR_STS_LEN_ERR				0x04
#define VCMD_ERR_STS_UNKNOWN_CMD_ERR		0x08
#define VCMD_ERR_STS_HW_ERR					0x0C
#define VCMD_ERR_STS_UNKNOWN_SUBCMD_ERR		0x10
#define VCMD_ERR_STS_PARAM_ERR				0x14

static unsigned short do_crc(unsigned char *ptr, int len)
{
    unsigned int i;
    unsigned short crc = 0x0000;

    while(len--)
    {
        crc ^= (unsigned short)(*ptr++) << 8;
        for (i = 0; i < 8; ++i)
        {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }

    return crc;
}

/* Forward declarations */
static int read_regs(struct i2c_client *client, u32 reg, u8 *val, int len);
static int write_regs(struct i2c_client *client, u32 reg, u8 *val, int len);

/* Data structures - must be defined before rs300_send_command */
enum pad_types {
	IMAGE_PAD,
	NUM_PADS
};

struct rs300_mode {
	unsigned int width;
	unsigned int height;
	struct v4l2_fract max_fps;
	u32 code;
};

struct pll_ctrl_reg {
	unsigned int div;
	unsigned char reg;
};

static const char * const rs300_supply_names[] = {
	"VANA",	/* Digital I/O power */
	"VDIG",		/* Analog power */
	"VDDL",		/* Digital core power */
};

#define rs300_NUM_SUPPLIES ARRAY_SIZE(rs300_supply_names)

static const u32 codes[] = {
	/*
	 * The RS300 stream is YUV422 on CSI-2. Keep one advertised bus code on
	 * Jetson so the Tegra bridge cannot negotiate back to the 1X16 variant.
	 */
	RS300_DEFAULT_MBUS_CODE,
};

/*
 * The WN2640-like module can put 16-bit thermal samples into the same 2-byte
 * CSI transport that Tegra already captures reliably as UYVY. Jetson's camera
 * format table does not expose a native mono16 CSI pixel_t for this path, so
 * native_y16 re-labels the V4L2 video node as Y16 while keeping the proven
 * UYVY media bus code and packet timing.
 */
static const struct camera_common_colorfmt rs300_native_y16_colorfmt = {
	.code = RS300_UYVY_MBUS_CODE,
	.colorspace = V4L2_COLORSPACE_RAW,
	.pix_fmt = V4L2_PIX_FMT_Y16,
	.xfer_func = V4L2_XFER_FUNC_NONE,
	.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT,
	.quantization = V4L2_QUANTIZATION_FULL_RANGE,
};

struct rs300 {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad[NUM_PADS];
	struct camera_common_data *s_data;
	struct tegracam_device *tc_dev;

	struct v4l2_mbus_framefmt fmt;

	unsigned int xvclk_frequency;
	struct clk *xvclk;

	struct gpio_desc *power_gpio;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[rs300_NUM_SUPPLIES];
	bool supplies_present;
	bool supplies_enabled;
	bool power_enabled;
	bool startup_ffc_done;
	bool auto_shutter_initialized;
	bool stream_defaults_initialized;

	struct v4l2_ctrl_handler ctrl_handler;
	/* V4L2 Controls */
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_frequency;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *analogue_gain;
	struct v4l2_ctrl *brightness;
	struct v4l2_ctrl *shutter_cal;  /* Shutter calibration button */
	struct v4l2_ctrl *colormap;  /* Colormap selection control */
	struct v4l2_ctrl *zoom;  // Custom zoom control
	struct v4l2_ctrl *scene_mode;  /* Scene mode selection control */
	struct v4l2_ctrl *dde;
	struct v4l2_ctrl *contrast;
	struct v4l2_ctrl *spatial_nr;
	struct v4l2_ctrl *temporal_nr;
	struct v4l2_ctrl *autoshutter;  /* Auto shutter enable/disable */
	struct v4l2_ctrl *autoshutter_temp;  /* Auto shutter temperature threshold */
	struct v4l2_ctrl *autoshutter_min_interval;  /* Auto shutter minimum interval */
	struct v4l2_ctrl *autoshutter_max_interval;  /* Auto shutter maximum interval */
	struct v4l2_ctrl *output_mode;  /* Output mode selection control */
	struct v4l2_ctrl *camera_sleep;  /* Camera sleep/wake control */
	struct v4l2_ctrl *antiburn;  /* Anti-burn protection enable/disable */
	struct v4l2_ctrl *shutter;  /* Shutter open/close control */
	struct v4l2_ctrl *hook_edge;  /* Hook edge position control */
	struct v4l2_ctrl *frame_rate;  /* Detector frame rate control */
	struct v4l2_ctrl *analog_output_fmt;  /* Digital-Analog output format control */
	struct v4l2_ctrl *yuv_format;  /* CSI YUV byte order control */
	struct v4l2_ctrl *sensor_mode_id;
	struct v4l2_ctrl *vi_bypass_mode;
	struct v4l2_ctrl *override_enable;
	struct v4l2_ctrl *vi_height_align;
	struct v4l2_ctrl *vi_size_align;
	struct v4l2_ctrl *low_latency;
	struct v4l2_ctrl *vi_preferred_stride;
	struct v4l2_ctrl *vi_capture_timeout;

	/* Current mode */
	const struct rs300_mode *mode;
	struct rs300_mode runtime_mode;
	struct camera_common_frmfmt runtime_frmfmt;

	/* Mode filtering - only advertise modes supported by physical hardware */
	const struct rs300_mode *available_modes;  /* Pointer to single supported mode */
	unsigned int num_modes;  /* Always 1 - only one resolution per physical module */

	/*
	 * Mutex for serialized access:
	 * Protect sensor module set pad format and start/stop streaming safely.
	 */
	struct mutex mutex;

	/* Streaming on/off */
	bool streaming;
	bool start_primed;

	/* Deferred YUV format configuration (set on first stream start) */
	bool yuv_format_configured;
};

/* Autoshutter function prototypes (after struct rs300 definition) */
static int rs300_set_autoshutter(struct rs300 *rs300, int enable);
static int rs300_get_autoshutter(struct rs300 *rs300, int *value);
static int rs300_set_autoshutter_params(struct rs300 *rs300, int param_type, int value);
static int rs300_set_output_mode(struct rs300 *rs300, int value);
static int rs300_set_yuv_format(struct rs300 *rs300, int format);
static int rs300_set_fps(struct rs300 *rs300, int fps);
static int rs300_power_on_state(struct rs300 *rs300);
static int rs300_force_tegracam_bus_code(struct rs300 *rs300);

struct rs300_command {
    u8 class;
    u8 module;
    u8 subcmd;
    u8 reserved;
    const u8 *params;
    size_t param_len;
    u8 *result;
    size_t result_len;
    unsigned int timeout_ms;
    const char *name;
};

static const char *rs300_error_name(u8 error_code)
{
    switch (error_code) {
    case 0x00:
        return "success";
    case 0x01:
        return "length error";
    case 0x02:
        return "unknown command";
    case 0x03:
        return "hardware error";
    case 0x04:
        return "command not enabled";
    case 0x05:
    case 0x06:
    case 0x07:
        return "CRC check error";
    default:
        return "unknown error";
    }
}

/**
 * rs300_exec_command - Send an I2C command and optionally read its result.
 * @rs300: RS300 device structure
 * @cmd: Command descriptor
 *
 * Builds the 18-byte command packet, calculates CRC, polls the status
 * register, decodes camera errors, and reads register 0x1d00 for GET-style
 * commands when a result buffer is provided.
 */
static int rs300_exec_command(struct rs300 *rs300,
                              const struct rs300_command *cmd)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    unsigned char buf[18];
    unsigned char status[2];
    unsigned short crc;
    unsigned int timeout_ms;
    const char *name;
    int ret, i;
    int attempt;
    int max_polls;

    if (!cmd)
        return -EINVAL;

    if (cmd->param_len > 12) {
        dev_err(&client->dev, "Parameter length %zu exceeds maximum 12\n",
                cmd->param_len);
        return -EINVAL;
    }

    if (cmd->param_len && !cmd->params) {
        dev_err(&client->dev, "Parameter length set without parameter buffer\n");
        return -EINVAL;
    }

    timeout_ms = cmd->timeout_ms ? cmd->timeout_ms : 500;
    name = cmd->name ? cmd->name : "command";

    /* Build 18-byte command packet */
    memset(buf, 0, sizeof(buf));
    buf[0] = cmd->class;
    buf[1] = cmd->module;
    buf[2] = cmd->subcmd;
    buf[3] = cmd->reserved;

    /* Copy parameters (buf[4-15]) */
    if (cmd->param_len > 0)
        memcpy(&buf[4], cmd->params, cmd->param_len);

    /* Calculate and append CRC-16 */
    crc = do_crc(buf, 16);
    buf[16] = crc & 0xff;
    buf[17] = (crc >> 8) & 0xff;

    dev_dbg(&client->dev, "%s command buffer: %*ph",
            name, (int)sizeof(buf), buf);

    /*
     * Some modules NACK the command window briefly after another command or
     * internal FFC activity. Retry only the initial write; status/result reads
     * still fail fast so protocol errors remain visible.
     */
    ret = 0;
    for (attempt = 0; attempt < 3; attempt++) {
        ret = write_regs(client, I2C_VD_BUFFER_RW, buf, sizeof(buf));
        if (!ret)
            break;
        if (ret != -EREMOTEIO && ret != -EIO)
            break;
        msleep(50);
    }
    if (ret < 0) {
        dev_err(&client->dev, "%s I2C write failed after %d attempt(s): %d\n",
                name, attempt < 3 ? attempt + 1 : 3, ret);
        return ret;
    }

    /* Initial delay for camera processing */
    msleep(20);

    /* Poll for completion status */
    max_polls = (timeout_ms + 9) / 10;
    if (max_polls < 1)
        max_polls = 1;

    for (i = 0; i < max_polls; i++) {
        bool is_busy;
        bool reset_failed;
        u8 error_field;
        u8 error_code;

        ret = read_regs(client, I2C_VD_BUFFER_STATUS, status, 2);
        if (ret < 0) {
            dev_err(&client->dev, "%s status read failed: %d\n", name, ret);
            return ret;
        }

        is_busy = (status[0] & VCMD_BUSY_STS_BIT) != 0;
        reset_failed = (status[0] & VCMD_RST_STS_BIT) != 0;
        error_field = status[0] & VCMD_ERR_STS_BIT;
        error_code = error_field >> 2;

        if (is_busy) {
            msleep(10);
            continue;
        }

        if (reset_failed || error_field != VCMD_ERR_STS_SUCCESS) {
            dev_err(&client->dev,
                    "%s 0x%02x:0x%02x failed: status=0x%02x error=0x%02x (%s)\n",
                    name, cmd->module, cmd->subcmd, status[0], error_code,
                    rs300_error_name(error_code));
            return -EIO;
        }

        if (cmd->result && cmd->result_len > 0) {
            ret = read_regs(client, I2C_VD_BUFFER_RW,
                            cmd->result, cmd->result_len);
            if (ret < 0) {
                dev_err(&client->dev, "%s result read failed: %d\n",
                        name, ret);
                return ret;
            }
            dev_dbg(&client->dev, "%s result buffer: %*ph",
                    name, (int)cmd->result_len, cmd->result);
        }

        dev_dbg(&client->dev,
                "%s 0x%02x:0x%02x succeeded after %dms\n",
                name, cmd->module, cmd->subcmd, (i + 2) * 10);
        return 0;
    }

    dev_err(&client->dev,
            "%s 0x%02x:0x%02x timeout after %dms\n",
            name, cmd->module, cmd->subcmd, timeout_ms);
    return -ETIMEDOUT;
}

static int rs300_send_command(struct rs300 *rs300,
                              u8 class,
                              u8 module,
                              u8 subcmd,
                              const u8 *params,
                              size_t param_len,
                              unsigned int timeout_ms)
{
    struct rs300_command cmd = {
        .class = class,
        .module = module,
        .subcmd = subcmd,
        .params = params,
        .param_len = param_len,
        .timeout_ms = timeout_ms,
        .name = "SET",
    };

    return rs300_exec_command(rs300, &cmd);
}

static int read_regs(struct i2c_client *client,  u32 reg, u8 *val ,int len )
{
	struct i2c_msg msg[2];
	unsigned char data[4] = { 0, 0, 0, 0 };
    int ret;

	if (!val || len <= 0 || len > MAX_I2C_TRANSFER_SIZE)
		return -EINVAL;

	msg[0].addr = client->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = data;

	msg[1].addr = client->addr;
	msg[1].flags = 1;
	msg[1].len = len;
	msg[1].buf = val;
	/* High byte goes out first */
	data[0] = reg>>8;
	data[1] = reg&0xff;
    
    ret = i2c_transfer(client->adapter, msg, 2);
    if (ret != 2) {
        dev_err(&client->dev, "i2c read error at reg 0x%04x: %d\n", reg, ret);
        return ret < 0 ? ret : -EIO;
    }
    
    return 0;
}

static int write_regs(struct i2c_client *client,  u32 reg, u8 *val,int len)
{
	struct i2c_msg msg[1];
	unsigned char *outbuf;
    int ret;

	if (!val || len <= 0 || len > MAX_I2C_TRANSFER_SIZE)
		return -EINVAL;

	outbuf = (unsigned char *)kmalloc(sizeof(unsigned char)*(len+2), GFP_KERNEL);
    if (!outbuf) {
        dev_err(&client->dev, "Failed to allocate memory for I2C write\n");
        return -ENOMEM;
    }

	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = len+2;
	msg->buf = outbuf;
	outbuf[0] = reg>>8;
    outbuf[1] = reg&0xff;
	memcpy(outbuf+2, val, len);
    
    ret = i2c_transfer(client->adapter, msg, 1);
    if (ret != 1) {
        dev_err(&client->dev, "i2c write error at reg 0x%04x: %d\n", reg, ret);
        kfree(outbuf);
        return ret < 0 ? ret : -EIO;
    }
    
    kfree(outbuf);
    return 0;
	// if (reg & I2C_VD_CHECK_ACCESS){
	// 	return ret;
	// }else
	// {
	// 	return check_access_done(client,2000);//命令超时控制，由于应用层已经控制这里不需要了
	// }
}

/* Duplicate struct definitions removed - now defined earlier in file */

static struct rs300_mode supported_modes[] = {
    { /* 640 - Primary mode for Pi 5 */
        .width      = 640,
        .height     = 512,
        .max_fps = {
            .numerator = 60,
            .denominator = 1,
        },
        .code = RS300_DEFAULT_MBUS_CODE,
    },
    {
        .width      = 256,
        .height     = 192,
        .max_fps = {
            .numerator = 25,
            .denominator = 1,
        },
        .code = RS300_DEFAULT_MBUS_CODE,
    },
        { /* 384*/
        .width      = 384,
        .height     = 288,
        .max_fps = {
            .numerator = 60,
            .denominator = 1,
        },
        .code = RS300_DEFAULT_MBUS_CODE,
    }

};

static const struct rs300_mode *rs300_apply_runtime_mode(struct rs300 *rs300)
{
    struct device *dev = rs300->s_data && rs300->s_data->dev ?
                         rs300->s_data->dev : &rs300->client->dev;
    int idx = mode;

    if (idx < 0 || idx >= ARRAY_SIZE(supported_modes)) {
        dev_warn(dev, "Invalid mode=%d, falling back to mode=0", idx);
        idx = 0;
    }

    rs300->runtime_mode = supported_modes[idx];
    if (advertise_width > 0)
        rs300->runtime_mode.width = advertise_width;
    if (advertise_height > 0)
        rs300->runtime_mode.height = advertise_height;

    rs300->mode = &rs300->runtime_mode;
    rs300->available_modes = &rs300->runtime_mode;
    rs300->num_modes = 1;

    dev_info(dev,
             "Mode filtering: selected mode=%d sensor=%ux%u advertised=%ux%u",
             idx, supported_modes[idx].width, supported_modes[idx].height,
             rs300->runtime_mode.width, rs300->runtime_mode.height);

    return rs300->mode;
}

static const struct regmap_config rs300_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0)
	.use_single_rw = true,
#else
	.use_single_read = true,
	.use_single_write = true,
#endif
};

static const int rs300_60fps[] = {
	60,
};

static const struct camera_common_frmfmt rs300_frmfmt[] = {
	{
		.size = {
			.width = 384,
			.height = 288,
		},
		.framerates = rs300_60fps,
		.num_framerates = ARRAY_SIZE(rs300_60fps),
		.hdr_en = false,
		.mode = 0,
	},
};

static void rs300_prepare_runtime_frmfmt(struct rs300 *rs300)
{
	const struct rs300_mode *selected_mode = rs300_apply_runtime_mode(rs300);

	rs300->runtime_frmfmt = rs300_frmfmt[0];
	rs300->runtime_frmfmt.size.width = selected_mode->width;
	rs300->runtime_frmfmt.size.height = selected_mode->height;
}

static inline struct rs300 *to_rs300(struct v4l2_subdev *sd)
{
	return container_of(sd, struct rs300, sd);
}

static int rs300_current_output_mode(struct rs300 *rs300)
{
	if (output_source >= 0)
		return output_source;

	if (rs300 && rs300->output_mode)
		return rs300->output_mode->val;

	return RS300_OUTPUT_MODE_DEFAULT;
}

static bool rs300_should_expose_native_y16(struct rs300 *rs300)
{
	return native_y16 && rs300_current_output_mode(rs300) == RS300_OUTPUT_MODE_Y16;
}

static u32 rs300_get_format_code(struct rs300 *rs300, u32 code)
{
	unsigned int i;
	struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);

	lockdep_assert_held(&rs300->mutex);	

	dev_dbg(&client->dev, "rs300_get_format_code: input code=0x%x", code);

	for (i = 0; i < ARRAY_SIZE(codes); i++) {
		dev_dbg(&client->dev, "  Checking supported code[%d]=0x%x", i, codes[i]);
		if (codes[i] == code)
			break;
	}

	if (i >= ARRAY_SIZE(codes)) {
		dev_warn(&client->dev, "Format code 0x%x not found, defaulting to 0x%x", code, codes[0]);
		i = 0; /* Default to native YUYV. */
	}

	dev_dbg(&client->dev, "rs300_get_format_code: returning code=0x%x", codes[i]);
	return codes[i];
}

static long rs300_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ioctl_data ioctl_data_kernel;  /* Kernel copy - SECURITY CRITICAL */
	unsigned char *data = NULL;
	unsigned char *user_data_ptr;
	long ret = 0;

	/*
	 * IMPORTANT: Validate command BEFORE copying from userspace.
	 * When V4L2 tools probe for supported ioctls, arg may not point to
	 * a valid ioctl_data structure. Return silently for unsupported commands.
	 */
	if ((cmd != CMD_GET) && (cmd != CMD_SET)) {
		return -ENOIOCTLCMD;  /* Translated to -ENOTTY by V4L2 core */
	}

	/* Copy the ioctl payload into kernel memory before validation/use. */
	if (copy_from_user(&ioctl_data_kernel, (struct ioctl_data __user *)arg,
			   sizeof(struct ioctl_data))) {
		dev_err(&client->dev, "Failed to copy ioctl data from userspace\n");
		return -EFAULT;
	}

	/* Validate the userspace buffer pointer and transfer bounds. */
	if (ioctl_data_kernel.data == NULL) {
		dev_err(&client->dev, "NULL data pointer in ioctl\n");
		return -EINVAL;
	}

	if (ioctl_data_kernel.wLength == 0 ||
	    ioctl_data_kernel.wLength > MAX_I2C_TRANSFER_SIZE) {
		dev_err(&client->dev,
			"Invalid I2C transfer length: %u (max %d)\n",
			ioctl_data_kernel.wLength, MAX_I2C_TRANSFER_SIZE);
		return -EINVAL;
	}

	if (ioctl_data_kernel.wIndex > 0xFFFF) {
		dev_err(&client->dev, "Invalid I2C register address: 0x%x\n",
			ioctl_data_kernel.wIndex);
		return -EINVAL;
	}

	dev_dbg(&client->dev, "rs300 ioctl: cmd=%d reg=0x%x len=%u\n",
		 cmd, ioctl_data_kernel.wIndex, ioctl_data_kernel.wLength);

	switch (cmd) {
	case CMD_GET:
		data = kmalloc(ioctl_data_kernel.wLength, GFP_KERNEL);
		if (!data) {
			dev_err(&client->dev,
				"Failed to allocate %u byte transfer buffer\n",
				ioctl_data_kernel.wLength);
			return -ENOMEM;
		}

		/* Read I2C registers into kernel buffer */
		ret = read_regs(client, ioctl_data_kernel.wIndex, data,
				ioctl_data_kernel.wLength);
		if (ret) {
			dev_err(&client->dev, "I2C read failed: %ld\n", ret);
			kfree(data);
			return ret;
		}

		/* Copy kernel buffer to userspace */
		if (copy_to_user(ioctl_data_kernel.data, data,
				 ioctl_data_kernel.wLength)) {
			dev_err(&client->dev, "Failed to copy data to userspace\n");
			kfree(data);
			return -EFAULT;
		}

		kfree(data);
		break;

	case CMD_SET:
		data = kmalloc(ioctl_data_kernel.wLength, GFP_KERNEL);
		if (!data) {
			dev_err(&client->dev,
				"Failed to allocate %u byte transfer buffer\n",
				ioctl_data_kernel.wLength);
			return -ENOMEM;
		}

		/* Save userspace pointer before copying */
		user_data_ptr = ioctl_data_kernel.data;

		/* Copy data from userspace to kernel buffer */
		if (copy_from_user(data, user_data_ptr, ioctl_data_kernel.wLength)) {
			dev_err(&client->dev, "Failed to copy data from userspace\n");
			kfree(data);
			return -EFAULT;
		}

		/* Write kernel buffer to I2C registers */
		ret = write_regs(client, ioctl_data_kernel.wIndex, data,
				 ioctl_data_kernel.wLength);
		if (ret) {
			dev_err(&client->dev, "I2C write failed: %ld\n", ret);
		}

		kfree(data);
		break;

	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}
static void rs300_reset_colorspace(struct v4l2_mbus_framefmt *fmt)
{
	switch (fmt->code) {
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_UYVY8_1X16:
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
		/* YUV formats - video colorspace for ISP processing */
		fmt->colorspace = V4L2_COLORSPACE_SMPTE170M;
		fmt->ycbcr_enc = V4L2_YCBCR_ENC_601;
		fmt->quantization = V4L2_QUANTIZATION_LIM_RANGE;
		fmt->xfer_func = V4L2_XFER_FUNC_709;
		break;
	default:
		/* Default to raw colorspace */
		fmt->colorspace = V4L2_COLORSPACE_RAW;
		fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
		fmt->xfer_func = V4L2_XFER_FUNC_NONE;
		break;
	}
}

/* Calculate pixel rate based on format */
static u64 rs300_get_pixel_rate(u32 format_code)
{
	switch (format_code) {
	case MEDIA_BUS_FMT_YUYV8_1X16:
	case MEDIA_BUS_FMT_UYVY8_1X16:
		return RS300_PIXEL_RATE_16BIT;
	case MEDIA_BUS_FMT_YUYV8_2X8:
	case MEDIA_BUS_FMT_UYVY8_2X8:
		/* 8-bit dual lane formats use base pixel rate */
		return RS300_PIXEL_RATE;
	default:
		/* Default to 16-bit rate for unknown formats */
		return RS300_PIXEL_RATE_16BIT;
	}
}

static void rs300_set_default_format(struct rs300 *rs300)
{
    struct v4l2_mbus_framefmt *fmt;
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    const struct rs300_mode *selected_mode;
    
    dev_dbg(&client->dev, "rs300_set_default_format");

    selected_mode = rs300_apply_runtime_mode(rs300);
    
    /* Initialize the default format */
    fmt = &rs300->fmt;
    fmt->code = selected_mode->code;
    fmt->width = selected_mode->width;
    fmt->height = selected_mode->height;
    fmt->field = V4L2_FIELD_NONE;
    rs300_reset_colorspace(fmt);
    
    dev_dbg(&client->dev, "Default format set: code=0x%x, %dx%d",
        fmt->code, fmt->width, fmt->height);
}	

/*
 * V4L2 subdev video and pad level operations
 */
static int rs300_set_test_pattern(struct rs300 *rs300, int value)
{
	return 0;
}

static int rs300_get_brightness(struct rs300 *rs300, int *brightness_value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    u8 result_buffer[18];
    struct rs300_command cmd = {
        .class = 0x10,
        .module = 0x04,
        .subcmd = 0x87,
        .params = params,
        .param_len = 9,
        .result = result_buffer,
        .result_len = sizeof(result_buffer),
        .timeout_ms = 1000,
        .name = "GET brightness",
    };
    int ret;

    params[0] = 0x01;
    params[8] = 0x01;

    ret = rs300_exec_command(rs300, &cmd);
    if (ret)
        return ret;

    *brightness_value = result_buffer[4];
    dev_dbg(&client->dev, "Current brightness value: %d", *brightness_value);

    return 0;
}

static int rs300_set_dde(struct rs300 *rs300, int value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting DDE to %d", value);

    /* Validate value range */
    if (value < 0 || value > 100) {
        dev_err(&client->dev, "Invalid DDE value: %d (valid range: 0-100)", value);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = value;

    return rs300_send_command(rs300, 0x10, 0x04, 0x45, params, 1, 500);
}

static int rs300_set_output_mode(struct rs300 *rs300, int value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting output mode to %d", value);

    if (value < RS300_OUTPUT_MODE_YUV || value > RS300_OUTPUT_MODE_Y16) {
        dev_err(&client->dev, "Invalid output mode value: %d (valid: 0=YUV, 1=Y16)", value);
        return -EINVAL;
    }

    params[0] = value;

    return rs300_send_command(rs300, 0x10, 0x10, 0x45, params, 1, 500);
}

static int rs300_set_yuv_format(struct rs300 *rs300, int format)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    struct rs300_command cmd = {
        .class = 0x10,
        .module = 0x03,
        .subcmd = 0x4D,
        .reserved = format,
        .timeout_ms = 500,
        .name = "SET YUV format",
    };
    
    dev_dbg(&client->dev, "Setting YUV format to %d (0=UYVY, 1=VYUY, 2=YUYV, 3=YVYU)", format);
    
    if (format < 0 || format > 3) {
        dev_err(&client->dev, "Invalid YUV format: %d (valid range: 0-3)", format);
        return -EINVAL;
    }

    return rs300_exec_command(rs300, &cmd);
}

/* Anti-burn Protection SET command (0x10/0x03/0x4B with hardcoded CRC) */
static int rs300_set_antiburn(struct rs300 *rs300, int enable)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting anti-burn protection: %s", enable ? "ON" : "OFF");

    if (enable != 0 && enable != 1) {
        dev_err(&client->dev, "Invalid anti-burn value: %d (valid: 0 or 1)", enable);
        return -EINVAL;
    }

    params[0] = enable;

    return rs300_send_command(rs300, 0x10, 0x03, 0x4B, params, 1, 500);
}

/* Shutter Control SET command (0x01/0x0F/0x45 with hardcoded CRC) */
static int rs300_set_shutter(struct rs300 *rs300, int state)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting shutter to %s", state ? "OPEN" : "CLOSED");

    if (state != 0 && state != 1) {
        dev_err(&client->dev, "Invalid shutter state: %d (valid: 0=close, 1=open)", state);
        return -EINVAL;
    }

    params[0] = state;

    return rs300_send_command(rs300, 0x01, 0x0F, 0x45, params, 1, 500);
}

/* Hook Edge Position SET command (0x10/0x04/0x4E with dynamic CRC) */
static int rs300_set_hook_edge(struct rs300 *rs300, int position)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting hook edge position to %d (0=No Hook, 1=1st Gear, 2=2 Levels)", position);

    if (position < 0 || position > 2) {
        dev_err(&client->dev, "Invalid hook edge position: %d (valid: 0-2)", position);
        return -EINVAL;
    }

    params[0] = position;

    /* Use dynamic CRC calculation via rs300_send_command */
    return rs300_send_command(rs300, 0x10, 0x04, 0x4E, params, 1, 500);
}

/* Detector Frame Rate SET command (0x10/0x10/0x44 with dynamic CRC) */
static int rs300_set_frame_rate(struct rs300 *rs300, int rate_index)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    u8 rate_value;

    /* Map menu index to frame rate parameter value */
    static const u8 frame_rate_values[] = {
        0x19,  /* Index 0: 25Hz */
        0x1E,  /* Index 1: 30Hz */
        0x32,  /* Index 2: 50Hz */
        0x3C,  /* Index 3: 60Hz */
    };

    if (rate_index < 0 || rate_index > 3) {
        dev_err(&client->dev, "Invalid frame rate index: %d (valid: 0-3)", rate_index);
        return -EINVAL;
    }

    rate_value = frame_rate_values[rate_index];
    dev_dbg(&client->dev, "Setting detector frame rate to index %d (value: 0x%02X)", rate_index, rate_value);

    params[0] = rate_value;

    /* Use dynamic CRC calculation via rs300_send_command with extended timeout for frame rate switching */
    return rs300_send_command(rs300, 0x10, 0x10, 0x44, params, 1, 2500);
}

/* Digital-Analog Output Format SET command (0x10/0x10/0x49 with hardcoded CRC) */
static int rs300_set_analog_output_fmt(struct rs300 *rs300, int enable)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting digital-analog output format (enable: %d)", enable);

    if (enable != 0 && enable != 1) {
        dev_err(&client->dev, "Invalid analog output format value: %d", enable);
        return -EINVAL;
    }

    params[0] = enable;

    return rs300_send_command(rs300, 0x10, 0x10, 0x49, params, 1, 500);
}

static int rs300_set_contrast(struct rs300 *rs300, int value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting contrast to %d", value);

    /* Validate value range */
    if (value < 0 || value > 100) {
        dev_err(&client->dev, "Invalid contrast value: %d (valid range: 0-100)", value);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = value;

    return rs300_send_command(rs300, 0x10, 0x04, 0x4A, params, 1, 500);
}

static int rs300_set_spatial_nr(struct rs300 *rs300, int value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting spatial noise reduction to %d", value);

    /* Validate value range */
    if (value < 0 || value > 100) {
        dev_err(&client->dev, "Invalid spatial NR value: %d (valid range: 0-100)", value);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = value;

    return rs300_send_command(rs300, 0x10, 0x04, 0x4B, params, 1, 500);
}

static int rs300_set_temporal_nr(struct rs300 *rs300, int value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting temporal noise reduction to %d", value);

    /* Validate value range */
    if (value < 0 || value > 100) {
        dev_err(&client->dev, "Invalid temporal NR value: %d (valid range: 0-100)", value);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = value;

    return rs300_send_command(rs300, 0x10, 0x04, 0x4C, params, 1, 500);
}

static int rs300_get_colormap(struct rs300 *rs300, int *colormap_value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    u8 result_buffer[18];
    struct rs300_command cmd = {
        .class = 0x10,
        .module = 0x03,
        .subcmd = 0x85,
        .params = params,
        .param_len = 9,
        .result = result_buffer,
        .result_len = sizeof(result_buffer),
        .timeout_ms = 500,
        .name = "GET colormap",
    };
    int ret;

    params[8] = 0x01;

    ret = rs300_exec_command(rs300, &cmd);
    if (ret)
        return ret;

    *colormap_value = result_buffer[4];
    dev_dbg(&client->dev, "Current colormap value: %d", *colormap_value);

    return 0;
}

static int rs300_set_colormap(struct rs300 *rs300, int colormap_value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    int ret;
    int current_colormap;

    dev_dbg(&client->dev, "Setting colormap to %d", colormap_value);

    /* Validate colormap value range */
    if (colormap_value < 0 || colormap_value > 11) {
        dev_err(&client->dev, "Invalid colormap value: %d (valid range: 0-11)",
                colormap_value);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = 0x00;            /* Parameter 1 (0x00) */
    params[1] = colormap_value;  /* Parameter 2 (0-11) */

    /* Send command */
    ret = rs300_send_command(rs300, 0x10, 0x03, 0x45, params, 2, 500);
    if (ret)
        return ret;

    /* Verify colormap was set correctly */
    msleep(100);
    ret = rs300_get_colormap(rs300, &current_colormap);
    if (ret) {
        dev_warn(&client->dev, "Failed to get current colormap: %d", ret);
    } else {
        if (current_colormap == colormap_value) {
            dev_dbg(&client->dev, "Colormap successfully set and verified: %d", current_colormap);
        } else {
            dev_warn(&client->dev, "Colormap mismatch! Set: %d, Got: %d",
                     colormap_value, current_colormap);
        }
    }

    return 0;
}

static int rs300_shutter_cal(struct rs300 *rs300)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);

    dev_dbg(&client->dev, "Triggering shutter calibration (FFC)");

    /* FFC requires longer timeout due to physical shutter movement */
    return rs300_send_command(rs300, 0x10, 0x02, 0x43, NULL, 0, 5000);
}

static int rs300_brightness_correct(struct rs300 *rs300, int brightness_value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 brightness_param;
    u8 params[12] = {0};
    int ret;
    int current_brightness;

    if (brightness_value < RS300_BRIGHTNESS_MIN ||
        brightness_value > RS300_BRIGHTNESS_MAX)
        return -EINVAL;

    brightness_param = brightness_value;

    dev_dbg(&client->dev, "Setting brightness correctly to %d (param: 0x%02X)",
             brightness_value, brightness_param);

    params[0] = brightness_param;
    ret = rs300_send_command(rs300, 0x10, 0x04, 0x47, params, 1, 500);
    if (ret)
        return ret;

    msleep(100);
    ret = rs300_get_brightness(rs300, &current_brightness);
    if (ret)
        dev_warn(&client->dev, "Failed to verify brightness: %d", ret);
    else if (current_brightness != brightness_param)
        dev_warn(&client->dev, "Brightness verify mismatch: set=%u got=%d",
                 brightness_param, current_brightness);

    return 0;
}

static int rs300_set_zoom(struct rs300 *rs300, int zoom_level)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting zoom to %dx", zoom_level);

    /* Validate zoom level */
    if (zoom_level < 1 || zoom_level > 8) {
        dev_err(&client->dev, "Invalid zoom level: %d (valid range: 1-8)", zoom_level);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = 0x00;                /* Fixed value */
    params[1] = zoom_level * 10;     /* Convert zoom level to command value (10, 20, ... 80) */

    /* Note: Previously used hardcoded CRC values (0x06, 0x0A) - now properly calculated */
    /* Zoom uses class 0x01 instead of the standard 0x10 */
    return rs300_send_command(rs300, 0x01, 0x31, 0x42, params, 2, 500);
}

static int rs300_set_scene_mode(struct rs300 *rs300, int scene_mode_value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting scene mode to %d", scene_mode_value);

    /* Validate scene mode value range */
    if (scene_mode_value < 0 || scene_mode_value > 9) {
        dev_err(&client->dev, "Invalid scene mode value: %d (valid range: 0-9)",
                scene_mode_value);
        return -EINVAL;
    }

    /* Pack parameters */
    params[0] = scene_mode_value;

    return rs300_send_command(rs300, 0x10, 0x04, 0x42, params, 1, 500);
}

/* Autoshutter control functions */
static int rs300_set_autoshutter(struct rs300 *rs300, int enable)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting autoshutter: %s", enable ? "ON" : "OFF");

    /* Pack parameters: P1 = enable (0=off, 1=on) */
    params[0] = enable ? 0x01 : 0x00;

    /* Command: Class=0x10, Module=0x02, SubCmd=0x41 */
    return rs300_send_command(rs300, 0x10, 0x02, 0x41, params, 1, 500);
}

static int rs300_get_autoshutter(struct rs300 *rs300, int *value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    u8 result_buffer[18];
    int ret;

    dev_dbg(&client->dev, "Getting autoshutter state");

    /* Pack parameters: P9=0x01, Len=0x0001 */
    params[8] = 0x01;  /* P9 = 0x01 */
    /* Len field is in bytes 12-13, but rs300_send_command handles this */

    /* Command: Class=0x10, Module=0x02, SubCmd=0x81 */
    ret = rs300_send_command(rs300, 0x10, 0x02, 0x81, params, 9, 500);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to get autoshutter state: %d", ret);
        return ret;
    }

    /* Read result from camera (register 0x1d00 contains the result) */
    ret = read_regs(client, 0x1d00, result_buffer, 18);
    if (ret < 0) {
        dev_err(&client->dev, "Failed to read autoshutter result: %d", ret);
        return ret;
    }

    /* Extract result from buffer (P1 contains the state) */
    *value = result_buffer[4];  /* P1 is at byte 4 */

    dev_dbg(&client->dev, "Autoshutter state: %s", *value ? "ON" : "OFF");

    return 0;
}

static int rs300_set_autoshutter_params(struct rs300 *rs300, int param_type, int value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting autoshutter param type %d to value %d",
             param_type, value);

    /* Validate parameter type */
    if (param_type < 0 || param_type > 2) {
        dev_err(&client->dev, "Invalid parameter type: %d (valid range: 0-2)",
                param_type);
        return -EINVAL;
    }

    /* Pack parameters:
     * P1[0] = param_type (0=temp threshold, 1=min interval, 2=max interval)
     * P1[2:1] = value (16-bit little-endian)
     */
    params[0] = param_type;
    params[1] = value & 0xFF;        /* Low byte */
    params[2] = (value >> 8) & 0xFF; /* High byte */

    /* Command: Class=0x10, Module=0x02, SubCmd=0x42 */
    return rs300_send_command(rs300, 0x10, 0x02, 0x42, params, 3, 500);
}

/* Camera sleep/wake control functions */
static int rs300_set_sleep(struct rs300 *rs300, int enable)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};

    dev_dbg(&client->dev, "Setting camera sleep: %s", enable ? "ON" : "OFF");

    /* Para1: 0x01 = sleep, 0x00 = wake */
    params[0] = enable ? 0x01 : 0x00;

    /* Command: Class=0x10, Module=0x10, SubCmd=0x48
     * After sleeping, video freezes and camera only responds to wake-up command.
     */
    return rs300_send_command(rs300, 0x10, 0x10, 0x48, params, 1, 500);
}

static int rs300_get_sleep(struct rs300 *rs300, int *value)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    u8 result_buffer[18];
    struct rs300_command cmd = {
        .class = 0x10,
        .module = 0x10,
        .subcmd = 0x88,
        .params = params,
        .param_len = 9,
        .result = result_buffer,
        .result_len = sizeof(result_buffer),
        .timeout_ms = 500,
        .name = "GET sleep",
    };
    int ret;

    dev_dbg(&client->dev, "Getting camera sleep state");

    params[8] = 0x01;

    ret = rs300_exec_command(rs300, &cmd);
    if (ret)
        return ret;

    *value = result_buffer[5];
    dev_dbg(&client->dev, "Camera sleep state: %d (0=awake, 1=asleep)", *value);

    return 0;
}

static int rs300_get_ctrl(struct v4l2_ctrl *ctrl)
{
    struct rs300 *rs300 =
        container_of(ctrl->handler, struct rs300, ctrl_handler);
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    int value = 0;
    int ret;

    if (!rs300->power_enabled) {
        ret = rs300_power_on_state(rs300);
        if (ret)
            return ret;
        if (rs300->s_data && rs300->s_data->power)
            rs300->s_data->power->state = SWITCH_ON;
    }

    switch (ctrl->id) {
    case RS300_CID_BRIGHTNESS:
        ret = rs300_get_brightness(rs300, &value);
        break;
    case V4L2_CID_CUSTOM_BASE + 1:
        ret = rs300_get_colormap(rs300, &value);
        break;
    case V4L2_CID_CUSTOM_BASE + 8:
        ret = rs300_get_autoshutter(rs300, &value);
        break;
    case V4L2_CID_CUSTOM_BASE + 12:
        ret = rs300_get_sleep(rs300, &value);
        break;
    default:
        return -EINVAL;
    }

    if (ret) {
        dev_dbg(&client->dev,
                "GET control 0x%x failed (%d); keeping cached value %d",
                ctrl->id, ret, ctrl->val);
        return 0;
    }

    ctrl->val = value;

    return 0;
}

static bool rs300_ctrl_uses_i2c(u32 id)
{
    switch (id) {
    case V4L2_CID_TEST_PATTERN:
    case RS300_CID_BRIGHTNESS:
    case V4L2_CID_CUSTOM_BASE + 1:
    case V4L2_CID_CUSTOM_BASE + 2:
    case V4L2_CID_ZOOM_ABSOLUTE:
    case V4L2_CID_CUSTOM_BASE + 3:
    case RS300_CID_CONTRAST:
    case V4L2_CID_CUSTOM_BASE + 4:
    case V4L2_CID_CUSTOM_BASE + 5:
    case V4L2_CID_CUSTOM_BASE + 6:
    case V4L2_CID_CUSTOM_BASE + 7:
    case V4L2_CID_CUSTOM_BASE + 8:
    case V4L2_CID_CUSTOM_BASE + 9:
    case V4L2_CID_CUSTOM_BASE + 10:
    case V4L2_CID_CUSTOM_BASE + 11:
    case V4L2_CID_CUSTOM_BASE + 12:
    case V4L2_CID_CUSTOM_BASE + 13:
    case V4L2_CID_CUSTOM_BASE + 14:
    case V4L2_CID_CUSTOM_BASE + 15:
    case V4L2_CID_CUSTOM_BASE + 16:
    case V4L2_CID_CUSTOM_BASE + 17:
    case V4L2_CID_CUSTOM_BASE + 18:
        return true;
    default:
        return false;
    }
}

static int rs300_set_ctrl(struct v4l2_ctrl *ctrl)
{
    struct rs300 *rs300 =
        container_of(ctrl->handler, struct rs300, ctrl_handler);
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    int ret = 0;

    /* Add debug info */
    dev_dbg(&client->dev, "Setting control ID 0x%x to value %d\n",
            ctrl->id, ctrl->val);

    if (rs300_ctrl_uses_i2c(ctrl->id) && !rs300->power_enabled) {
        ret = rs300_power_on_state(rs300);
        if (ret)
            return ret;
        if (rs300->s_data && rs300->s_data->power)
            rs300->s_data->power->state = SWITCH_ON;
    }

    switch (ctrl->id) {
    case V4L2_CID_TEST_PATTERN:
        ret = rs300_set_test_pattern(rs300, ctrl->val);
        break;
    case RS300_CID_BRIGHTNESS:
        ret = rs300_brightness_correct(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 1:
        /* This is our colormap selection control */
        ret = rs300_set_colormap(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 2:
        /* This is our FFC (Flat Field Correction) button */
        dev_dbg(&client->dev, "FFC trigger received\n");
        if (ctrl->val == 0) {
            ret = rs300_shutter_cal(rs300);
        }
        break;
    case V4L2_CID_ZOOM_ABSOLUTE:
        ret = rs300_set_zoom(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 3:
        /* This is our scene mode selection control */
        ret = rs300_set_scene_mode(rs300, ctrl->val);
        break;
    case RS300_CID_CONTRAST:
        ret = rs300_set_contrast(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 4:  /* DDE */
        ret = rs300_set_dde(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 5:  /* Spatial NR */
        ret = rs300_set_spatial_nr(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 6:  /* Temporal NR */
        ret = rs300_set_temporal_nr(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 7:  /* Output Mode */
        ret = rs300_set_output_mode(rs300, ctrl->val);
        if (!ret)
            rs300_force_tegracam_bus_code(rs300);
        break;
    case V4L2_CID_CUSTOM_BASE + 8:  /* Autoshutter enable/disable */
        ret = rs300_set_autoshutter(rs300, ctrl->val);
        if (!ret)
            rs300->auto_shutter_initialized = true;
        break;
    case V4L2_CID_CUSTOM_BASE + 9:  /* Autoshutter temperature threshold */
        ret = rs300_set_autoshutter_params(rs300, 0, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 10:  /* Autoshutter min interval */
        ret = rs300_set_autoshutter_params(rs300, 1, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 11:  /* Autoshutter max interval */
        ret = rs300_set_autoshutter_params(rs300, 2, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 12:  /* Camera sleep */
        dev_dbg(&client->dev, "Setting camera sleep: %s", ctrl->val ? "ON" : "OFF");
        ret = rs300_set_sleep(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 13:  /* Anti-burn Protection */
        ret = rs300_set_antiburn(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 14:  /* Shutter State */
        ret = rs300_set_shutter(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 15:  /* Hook Edge Position */
        ret = rs300_set_hook_edge(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 16:  /* Detector Frame Rate */
        ret = rs300_set_frame_rate(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 17:  /* Digital-Analog Output Format */
        ret = rs300_set_analog_output_fmt(rs300, ctrl->val);
        break;
    case V4L2_CID_CUSTOM_BASE + 18:  /* YUV Format */
        ret = rs300_set_yuv_format(rs300, ctrl->val);
        if (!ret)
            rs300->yuv_format_configured = true;
        break;
    case V4L2_CID_EXPOSURE:
        /* Exposure control for libcamera compatibility */
        /* Thermal camera exposure is stored but not actively controlled via I2C */
        dev_dbg(&client->dev, "Exposure set to %d lines (stored for libcamera)", ctrl->val);
        ret = 0;  /* Success - value stored in control framework */
        break;
    case TEGRA_CAMERA_CID_SENSOR_MODE_ID:
    case TEGRA_CAMERA_CID_VI_BYPASS_MODE:
    case TEGRA_CAMERA_CID_OVERRIDE_ENABLE:
    case TEGRA_CAMERA_CID_VI_HEIGHT_ALIGN:
    case TEGRA_CAMERA_CID_VI_SIZE_ALIGN:
    case TEGRA_CAMERA_CID_LOW_LATENCY:
    case TEGRA_CAMERA_CID_VI_PREFERRED_STRIDE:
    case TEGRA_CAMERA_CID_VI_CAPTURE_TIMEOUT:
        dev_dbg(&client->dev, "Tegra camera control 0x%x set to %d",
                ctrl->id, ctrl->val);
        ret = 0;
        break;
    default:
        dev_err(&client->dev, "Invalid control %d", ctrl->id);
        ret = -EINVAL;
    }

    return ret;
}

static const struct v4l2_ctrl_ops rs300_ctrl_ops = {
	.s_ctrl = rs300_set_ctrl,
	.g_volatile_ctrl = rs300_get_ctrl,
};

static int rs300_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct rs300 *rs300 = to_rs300(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	
	dev_dbg(&client->dev, "rs300_enum_mbus_code: pad=%d, index=%d", code->pad, code->index);
	
	if (code->pad >= NUM_PADS) {
		dev_err(&client->dev, "Invalid pad %d (max %d)", code->pad, NUM_PADS-1);
		return -EINVAL;
	}

	if (code->pad == IMAGE_PAD) {
		if (code->index >= ARRAY_SIZE(codes))
			return -EINVAL;

		mutex_lock(&rs300->mutex);
		code->code = codes[code->index];
		dev_dbg(&client->dev, "Returning format code[%d]: 0x%x (%s)",
			 code->index, code->code,
			 code->code == MEDIA_BUS_FMT_YUYV8_1X16 ? "YUYV8_1X16" :
			 code->code == MEDIA_BUS_FMT_UYVY8_1X16 ? "UYVY8_1X16" :
			 code->code == MEDIA_BUS_FMT_YUYV8_2X8 ? "YUYV8_2X8" :
			 code->code == MEDIA_BUS_FMT_UYVY8_2X8 ? "UYVY8_2X8" : "OTHER");
		mutex_unlock(&rs300->mutex);
	} else {
		dev_err(&client->dev, "Invalid pad %d", code->pad);
		return -EINVAL;
	}
	
	return 0;
}

static int rs300_enum_frame_sizes(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	//struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct rs300 *rs300 = to_rs300(sd);
	u32 code;

	if (fse->pad >= NUM_PADS)
		return -EINVAL;

	if (fse->pad == IMAGE_PAD) {
		/* Only enumerate modes supported by this physical hardware module */
		if (fse->index >= rs300->num_modes)
			return -EINVAL;

		mutex_lock(&rs300->mutex);
		code = rs300_get_format_code(rs300, fse->code);
		if (code != fse->code) {
			mutex_unlock(&rs300->mutex);
			return -EINVAL;
		}
		mutex_unlock(&rs300->mutex);

		/* Use filtered mode list - only the resolution this hardware supports */
		fse->min_width  = rs300->available_modes[fse->index].width;
		fse->max_width  = fse->min_width;
		fse->min_height = rs300->available_modes[fse->index].height;
		fse->max_height = fse->min_height;
	} else {
		return -EINVAL;
	}

	return 0;
}

static void rs300_update_image_pad_format(struct rs300 *rs300,
					   const struct rs300_mode *mode,
					   struct v4l2_subdev_format *fmt)
{
	if (!fmt->format.code)
		fmt->format.code = RS300_DEFAULT_MBUS_CODE;

	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	rs300_reset_colorspace(&fmt->format);
}

// Restore and fix __rs300_get_pad_fmt
static int __rs300_get_pad_fmt(struct rs300 *rs300,
                               struct v4l2_subdev_state *sd_state,
                               struct v4l2_subdev_format *fmt)
{
        struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
        struct v4l2_mbus_framefmt *try_fmt;
        if (fmt->pad >= NUM_PADS)
                return -EINVAL;

        dev_dbg(&client->dev, "rs300_get_pad_fmt: pad=%d, which=%d",
                fmt->pad, fmt->which);

        if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
                try_fmt = v4l2_subdev_get_try_format(&rs300->sd,
                                                      sd_state, fmt->pad);
                if (!try_fmt)
                        return -EINVAL;

                fmt->format = *try_fmt;
		if (!fmt->format.code || !fmt->format.width ||
		    !fmt->format.height) {
			fmt->format = rs300->fmt;
			if (!fmt->format.code)
				fmt->format.code = RS300_DEFAULT_MBUS_CODE;
			if (!fmt->format.width || !fmt->format.height) {
				fmt->format.width = supported_modes[mode].width;
				fmt->format.height = supported_modes[mode].height;
			}
			fmt->format.field = V4L2_FIELD_NONE;
			rs300_reset_colorspace(&fmt->format);
		}

		dev_dbg(&client->dev, "Get TRY format: code=0x%x, %dx%d",
			fmt->format.code, fmt->format.width, fmt->format.height);
	} else {
		/* Return the active format */
		if (fmt->pad == IMAGE_PAD) {
			fmt->format = rs300->fmt;
			if (!fmt->format.code)
				fmt->format.code = RS300_DEFAULT_MBUS_CODE;
			if (!fmt->format.width || !fmt->format.height) {
				fmt->format.width = supported_modes[mode].width;
				fmt->format.height = supported_modes[mode].height;
			}
			fmt->format.field = V4L2_FIELD_NONE;
			rs300_reset_colorspace(&fmt->format);

			dev_dbg(&client->dev, "Get ACTIVE format: code=0x%x, %dx%d",
				fmt->format.code, fmt->format.width, fmt->format.height);

			// Debug current active mode
			if (rs300->mode) {
				dev_dbg(&client->dev, "Current active mode: %dx%d @ %d/%d fps",
					rs300->mode->width, rs300->mode->height,
					rs300->mode->max_fps.denominator, rs300->mode->max_fps.numerator);
			} else {
				dev_dbg(&client->dev, "No active mode set yet");
			}
		} else {
			dev_err(&client->dev, "Invalid pad %d", fmt->pad);
			return -EINVAL;
		}
	}
	return 0;
}

// Fix rs300_get_pad_fmt to call __rs300_get_pad_fmt
static int rs300_get_pad_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	//struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct rs300 *rs300 = to_rs300(sd);
	int ret;
	
	mutex_lock(&rs300->mutex);
	ret = __rs300_get_pad_fmt(rs300, sd_state, fmt);
	mutex_unlock(&rs300->mutex);
	return ret;
}

// Fix rs300_set_pad_fmt to use v4l2_subdev_get_fmt
static int rs300_set_pad_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *sd_state,
			  struct v4l2_subdev_format *fmt)
{
	struct rs300 *rs300 = to_rs300(sd);
	const struct rs300_mode *mode;
	struct v4l2_mbus_framefmt *framefmt;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	unsigned int i;

	if (fmt->pad >= NUM_PADS)
		return -EINVAL;

	mutex_lock(&rs300->mutex);

	dev_dbg(&client->dev, "rs300_set_pad_fmt input: pad=%d, which=%d, code=0x%x, width=%d, height=%d",
		fmt->pad, fmt->which, fmt->format.code, fmt->format.width, fmt->format.height);

	if (fmt->pad == IMAGE_PAD) {
		/* Find the closest supported format code */
		for (i = 0; i < ARRAY_SIZE(codes); i++)
			if (codes[i] == fmt->format.code)
				break;
		if (i >= ARRAY_SIZE(codes))
			i = 0; /* Default to first supported code if not found */

		fmt->format.code = rs300_get_format_code(rs300, codes[i]);

		/* Find the closest supported resolution */
		dev_dbg(&client->dev, "rs300_set_pad_fmt searching for nearest mode to %dx%d",
			fmt->format.width, fmt->format.height);

		/* Print all supported modes for debugging (only shows hardware-supported mode) */
		for (i = 0; i < rs300->num_modes; i++) {
			dev_dbg(&client->dev, "Supported mode[%d]: %dx%d",
				i, rs300->available_modes[i].width, rs300->available_modes[i].height);
		}

		/*
		 * Use filtered mode list - only one mode for this hardware.
		 * Since num_modes=1, v4l2_find_nearest_size will always return the single mode.
		 * This prevents libcamera from attempting unsupported resolution changes.
		 */
		mode = v4l2_find_nearest_size(rs300->available_modes,
					      rs300->num_modes,
					      width, height,
					      fmt->format.width, fmt->format.height);

		/* Update the format with the selected mode */
		dev_dbg(&client->dev, "rs300_set_pad_fmt selected mode: width=%d, height=%d",
			mode->width, mode->height);

		rs300_update_image_pad_format(rs300, mode, fmt);

                if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
                        framefmt = v4l2_subdev_get_try_format(sd, sd_state,
                                                               fmt->pad);
                        if (!framefmt) {
                                mutex_unlock(&rs300->mutex);
                                return -EINVAL;
                        }
                        *framefmt = fmt->format;
			dev_dbg(&client->dev, "Set TRY format: code=0x%x, %dx%d",
				framefmt->code, framefmt->width, framefmt->height);
		} else {
			/* Update the active format and mode */
			rs300->fmt = fmt->format;
			rs300->mode = mode;
			
			/* Update pixel rate control based on new format */
			if (rs300->pixel_rate) {
				u64 new_pixel_rate = rs300_get_pixel_rate(rs300->fmt.code);
				__v4l2_ctrl_s_ctrl_int64(rs300->pixel_rate, new_pixel_rate);
				dev_dbg(&client->dev, "Updated pixel rate to %llu for format 0x%x",
					 new_pixel_rate, rs300->fmt.code);
			}
			
			dev_info(&client->dev, "Set ACTIVE format: code=0x%x, %dx%d",
				rs300->fmt.code, rs300->fmt.width, rs300->fmt.height);
		}
	} else {
		dev_err(&client->dev, "Invalid pad %d", fmt->pad);
		mutex_unlock(&rs300->mutex);
		return -EINVAL;
	}

	mutex_unlock(&rs300->mutex);
	return 0;
}

static int rs300_set_framefmt(struct rs300 *rs300)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    
    dev_dbg(&client->dev, "Setting frame format: code=0x%x, %dx%d",
             rs300->fmt.code, rs300->fmt.width, rs300->fmt.height);
    
    switch (rs300->fmt.code) {
    case MEDIA_BUS_FMT_YUYV8_1X16:
        dev_dbg(&client->dev, "Using YUYV8_1X16 format (16-bit packed, preferred for RP1-CFE)");
        /* 16-bit packed format - should be compatible with Pi 5 RP1-CFE */
        return 0;
    case MEDIA_BUS_FMT_UYVY8_1X16:
        dev_dbg(&client->dev, "Using UYVY8_1X16 format (16-bit packed alternative)");
        return 0;
    case MEDIA_BUS_FMT_YUYV8_2X8:
        dev_dbg(&client->dev, "Using YUYV8_2X8 format (8-bit dual lane, legacy)");
        return 0;
    case MEDIA_BUS_FMT_UYVY8_2X8:
        dev_dbg(&client->dev, "Using UYVY8_2X8 format (8-bit dual lane, legacy)");
        return 0;
    default:
        dev_err(&client->dev, "Unsupported format code: 0x%x", rs300->fmt.code);
        dev_err(&client->dev, "Supported formats: YUYV8_1X16(0x%x), UYVY8_1X16(0x%x), YUYV8_2X8(0x%x), UYVY8_2X8(0x%x)",
                MEDIA_BUS_FMT_YUYV8_1X16, MEDIA_BUS_FMT_UYVY8_1X16,
                MEDIA_BUS_FMT_YUYV8_2X8, MEDIA_BUS_FMT_UYVY8_2X8);
        return -EINVAL;
    }        
}

static void rs300_build_start_packet(struct rs300 *rs300,
                                     u8 start_regs[28],
                                     unsigned int *packet_width,
                                     unsigned int *packet_height)
{
    unsigned short crcdata;
    static const u8 start_template[28] = {
        0x01, 0x30, 0xc1, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x0a, 0x00,
        0x00, 0x00,
        0x00, 0x00,
        0x00,
        0x16,
        0x03,
        0x3c,
        0x80, 0x02,
        0x00, 0x02,
        0x00, 0x00
    };

    memcpy(start_regs, start_template, sizeof(start_template));

    *packet_width = start_width > 0 ? start_width : rs300->mode->width;
    *packet_height = start_height > 0 ? start_height : rs300->mode->height;

    start_regs[18] = start_path & 0xff;
    start_regs[19] = type & 0xff;
    start_regs[20] = start_dst & 0xff;
    start_regs[21] = fps & 0xff;
    start_regs[22] = *packet_width & 0xff;
    start_regs[23] = (*packet_width >> 8) & 0xff;
    start_regs[24] = *packet_height & 0xff;
    start_regs[25] = (*packet_height >> 8) & 0xff;

    crcdata = do_crc((uint8_t *)(start_regs + 18), 10);
    start_regs[14] = crcdata & 0xff;
    start_regs[15] = crcdata >> 8;

    crcdata = do_crc((uint8_t *)start_regs, 16);
    start_regs[16] = crcdata & 0xff;
    start_regs[17] = crcdata >> 8;
}

static void rs300_stop_streaming(struct rs300 *rs300)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);

    /* Keep the stop packet local to the current stream operation. */
    u8 stop_regs[28] = {
        0x01, 0x30, 0xc2, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x0a, 0x00,
        0x00, 0x00, //crc [14]
        0x2F, 0x0D, //crc [16]
        0x01, //path [18]
        0x16, //src [19]
        0x00, //dst [20]
        0x3c, //fps [21]
        0x80, 0x02, //width&0xff, width>>8 [22-23]
        0x00, 0x02, //height&0xff, height>>8 [24-25]
        0x00, 0x00
    };

    dev_dbg(&client->dev, "Stopping streaming");

    /* Write stop registers */
    if (write_regs(client, I2C_VD_BUFFER_RW, stop_regs, sizeof(stop_regs)) < 0) {
        dev_err(&client->dev, "Error writing stop registers");
    }

    dev_dbg(&client->dev, "Streaming stopped");
}

static int rs300_send_start_packet(struct rs300 *rs300,
                                   const u8 start_regs[28],
                                   const char *label)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);

    if (write_regs(client, I2C_VD_BUFFER_RW, (u8 *)start_regs, 28) < 0) {
        dev_err(&client->dev, "Error writing %s start registers", label);
        return -EIO;
    }

    return 0;
}

static int rs300_prime_i2c_only(struct rs300 *rs300)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    unsigned int packet_width;
    unsigned int packet_height;
    u8 start_regs[28];
    int ret;

    if (probe_prime_ms <= 0)
        return 0;

    if (rs300->start_primed)
        return 0;

    if (probe_prime_delay_ms > 0) {
        dev_info(&client->dev, "RS300 probe prime delay: %d ms",
                 probe_prime_delay_ms);
        msleep(probe_prime_delay_ms);
    }

    rs300_build_start_packet(rs300, start_regs, &packet_width, &packet_height);
    dev_info(&client->dev,
             "RS300 probe prime: start for %d ms, packet=%ux%u",
             probe_prime_ms, packet_width, packet_height);

    ret = rs300_set_output_mode(rs300, rs300_current_output_mode(rs300));
    if (ret)
        dev_warn(&client->dev,
                 "Probe prime output mode %d failed: %d (continuing)",
                 rs300_current_output_mode(rs300), ret);

    if (yuv_order >= 0) {
        ret = rs300_set_yuv_format(rs300, yuv_order);
        if (ret)
            dev_warn(&client->dev,
                     "Probe prime YUV order %d failed: %d (continuing)",
                     yuv_order, ret);
    }

    if (set_fps_cmd) {
        ret = rs300_set_fps(rs300, fps);
        if (ret)
            dev_warn(&client->dev, "Probe prime FPS %d failed: %d (continuing)",
                     fps, ret);
    }

    ret = rs300_send_start_packet(rs300, start_regs, "probe prime");
    if (ret)
        return ret;

    msleep(probe_prime_ms);
    rs300_stop_streaming(rs300);

    if (probe_prime_stop_ms > 0)
        msleep(probe_prime_stop_ms);

    rs300->start_primed = true;
    dev_info(&client->dev, "RS300 probe prime complete");

    return 0;
}

static int rs300_prestart_prime(struct rs300 *rs300,
                                const u8 start_regs[28])
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    int cycle;
    int ret;

    if (prestart_prime_cycles <= 0)
        return 0;

    if (rs300->start_primed)
        return 0;

    for (cycle = 0; cycle < prestart_prime_cycles; cycle++) {
        dev_info(&client->dev,
                 "RS300 prestart prime cycle %d/%d: start %d ms",
                 cycle + 1, prestart_prime_cycles, prestart_prime_ms);

        ret = rs300_send_start_packet(rs300, start_regs, "prestart prime");
        if (ret)
            return ret;

        if (prestart_prime_ms > 0)
            msleep(prestart_prime_ms);

        rs300_stop_streaming(rs300);

        if (prestart_prime_stop_ms > 0)
            msleep(prestart_prime_stop_ms);
    }

    rs300->start_primed = true;
    dev_info(&client->dev, "RS300 prestart prime complete");

    return 0;
}

static int rs300_set_fps(struct rs300 *rs300, int fps)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 params[12] = {0};
    int ret;

    /* Validate FPS value (25, 30, 50, or 60) */
    if (fps != 25 && fps != 30 && fps != 50 && fps != 60) {
        dev_warn(&client->dev, "Invalid FPS value: %d", fps);
        return 0;
    }

    dev_dbg(&client->dev, "Setting camera to %d fps", fps);

    /* Pack parameters */
    params[0] = 0x01;  /* Enable */
    params[1] = 0x03;  /* MIPI Progressive */
    params[2] = fps;   /* FPS value */

    /* FPS command needs longer timeout (4500ms total via 15 retries × 300ms) */
    ret = rs300_send_command(rs300, 0x10, 0x10, 0x46, params, 3, 4500);

    /* Note: FPS command always returns 0 even on error (legacy behavior) */
    if (ret)
        dev_warn(&client->dev, "FPS command failed: %d", ret);

    return 0;
}

static int rs300_set_stream_state(struct rs300 *rs300, int enable)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    u8 status_buffer[1];
    int ret = 0;

    dev_dbg(&client->dev, "rs300_set_stream: enable=%d, streaming=%d, fmt=0x%x %dx%d",
            enable, rs300->streaming, rs300->fmt.code, rs300->fmt.width, rs300->fmt.height);
    
    // Add detailed format info when streaming starts
    if (enable) {
        /*
         * IMPORTANT: Camera warm-up timing requirement
         *
         * The RS300 thermal camera requires approximately 2 seconds (60 frames at 30fps)
         * of warm-up time after stream start before outputting valid thermal data.
         *
         * - Frames 1-60: Initialization data (constant patterns like 0x36 0x80)
         * - Frame 60+: Valid thermal data (>1000 unique patterns)
         *
         * User-space applications should either:
         * 1. Capture 90+ frames and extract frame 60+ for processing
         * 2. Start stream, wait 2+ seconds, then begin capturing
         *
         * See documentation: ~/rs300-extra-documentation/test-reports/CAMERA_QUIRKS.txt
         */
        dev_dbg(&client->dev, "Stream start: fmt=0x%x %dx%d, mode=%dx%d @ %d/%d fps",
            rs300->fmt.code, rs300->fmt.width, rs300->fmt.height,
            rs300->mode->width, rs300->mode->height,
            rs300->mode->max_fps.denominator, rs300->mode->max_fps.numerator);
        dev_dbg(&client->dev, "Pixel rate: %llu Hz, colorspace: %d",
            rs300_get_pixel_rate(rs300->fmt.code), rs300->fmt.colorspace);
    } else {
        dev_dbg(&client->dev, "Stream stop requested");
    }

    mutex_lock(&rs300->mutex);
    if (rs300->streaming == enable) {
        dev_dbg(&client->dev, "Stream already in desired state");
        mutex_unlock(&rs300->mutex);
        return 0;
    }

    if (enable) {
        /* Keep packet buffers local so parallel users cannot share state. */
        static const int poll_timeout_ms[RS300_STREAM_START_RETRIES] = { 1500, 2500, 5000 };
        unsigned int packet_width;
        unsigned int packet_height;
        u8 start_regs[28];
        u8 verify_regs[28];
        int stream_attempt;
        int stream_success = 0;

        rs300_build_start_packet(rs300, start_regs, &packet_width, &packet_height);

        /*
         * Keep these camera-side MIPI settings overridable so we can sweep
         * them without recompiling while the Tegra bus code comes from DT.
	 */
        dev_info(&client->dev,
                 "RS300 stream params: output_source=%d yuv_order=%d path=%d type=%d dst=%d fps=%d packet=%ux%u",
                 output_source, yuv_order, start_path, type, start_dst, fps,
                 packet_width, packet_height);

        {
            int output_mode_value = rs300_current_output_mode(rs300);

            ret = rs300_set_output_mode(rs300, output_mode_value);
            if (ret)
                dev_warn(&client->dev, "Output mode %d set failed: %d (continuing)",
                         output_mode_value, ret);
        }

        if (yuv_order >= 0) {
            ret = rs300_set_yuv_format(rs300, yuv_order);
            if (ret)
                dev_warn(&client->dev, "YUV order %d set failed: %d (continuing)",
                         yuv_order, ret);
            else
                rs300->yuv_format_configured = true;
        }

        if (!rs300->stream_defaults_initialized) {
            if (rs300->dde) {
                ret = rs300_set_dde(rs300, rs300->dde->val);
                if (ret)
                    dev_warn(&client->dev, "DDE default %d set failed: %d (continuing)",
                             rs300->dde->val, ret);
            }

            if (rs300->spatial_nr) {
                ret = rs300_set_spatial_nr(rs300, rs300->spatial_nr->val);
                if (ret)
                    dev_warn(&client->dev,
                             "Spatial NR default %d set failed: %d (continuing)",
                             rs300->spatial_nr->val, ret);
            }

            if (rs300->temporal_nr) {
                ret = rs300_set_temporal_nr(rs300, rs300->temporal_nr->val);
                if (ret)
                    dev_warn(&client->dev,
                             "Temporal NR default %d set failed: %d (continuing)",
                             rs300->temporal_nr->val, ret);
            }

            if (rs300->frame_rate) {
                ret = rs300_set_frame_rate(rs300, rs300->frame_rate->val);
                if (ret)
                    dev_warn(&client->dev,
                             "Detector frame-rate default %d set failed: %d (continuing)",
                             rs300->frame_rate->val, ret);
            }

            rs300->stream_defaults_initialized = true;
        }

        if (set_fps_cmd) {
            ret = rs300_set_fps(rs300, fps);
            if (ret) {
                dev_err(&client->dev, "Failed to set camera to %d fps: %d", fps, ret);
                goto error_unlock;
            }
            dev_dbg(&client->dev, "FPS is set to %d", fps);
        } else {
            dev_info(&client->dev, "Skipping separate FPS command; START fps byte=%d", fps);
        }

        if (startup_ffc && !rs300->startup_ffc_done) {
            ret = rs300_shutter_cal(rs300);
            if (ret) {
                dev_warn(&client->dev,
                         "Startup FFC failed before stream start: %d (continuing)",
                         ret);
            } else {
                rs300->startup_ffc_done = true;
            }
        }

        if (auto_shutter >= 0 && !rs300->auto_shutter_initialized) {
            if (auto_shutter > 1) {
                dev_warn(&client->dev,
                         "Invalid auto_shutter=%d; expected -1, 0 or 1",
                         auto_shutter);
            } else {
                int auto_shutter_value = rs300->autoshutter ?
                    rs300->autoshutter->val : auto_shutter;

                ret = rs300_set_autoshutter(rs300, auto_shutter_value);
                if (ret)
                    dev_warn(&client->dev,
                             "Auto-shutter setup failed: %d (continuing)",
                             ret);
                else
                    rs300->auto_shutter_initialized = true;
            }
        }

        dev_dbg(&client->dev, "Start registers with CRC: %*ph", (int)sizeof(start_regs), start_regs);
        dev_dbg(&client->dev, "Writing start registers to device");

        ret = rs300_prestart_prime(rs300, start_regs);
        if (ret) {
            dev_err(&client->dev, "Prestart prime failed: %d", ret);
            goto error_unlock;
        }

        ret = rs300_send_start_packet(rs300, start_regs, "stream");
        if (ret) {
            dev_err(&client->dev, "error start rs300\n");
            goto error_unlock;
        }

        if (read_regs(client, I2C_VD_BUFFER_RW, verify_regs, sizeof(verify_regs)) == 0) {
            dev_dbg(&client->dev, "Read back registers: %*ph", (int)sizeof(verify_regs), verify_regs);
            if (memcmp(start_regs, verify_regs, sizeof(start_regs)) != 0) {
                dev_err(&client->dev, "Register verification failed!");
            }
        }

        //check if device is ready
 

        ret = rs300_set_framefmt(rs300);
        if (ret) {
            dev_err(&client->dev, "error set framefmt\n");
            goto error_unlock;
        }
        
        dev_dbg(&client->dev, "Stream registers written successfully");

        /*
         * Poll timeouts escalate because the WN2640-like module may report a
         * transient command error before CSI data is actually stable.
         */
        for (stream_attempt = 0; stream_attempt < RS300_STREAM_START_RETRIES; stream_attempt++) {
            int retry = 0;
            int timeout_ms = poll_timeout_ms[stream_attempt];
            int max_retries = timeout_ms / 100;
            int got_error = 0;

            dev_dbg(&client->dev, "Attempt %d/%d - polling for %dms",
                     stream_attempt + 1, RS300_STREAM_START_RETRIES, timeout_ms);

            while (retry < max_retries) {
                ret = read_regs(client, I2C_VD_BUFFER_STATUS, status_buffer, 1);
                if (ret == 0) {
                    dev_dbg(&client->dev, "Attempt %d/%d - Status check %d: 0x%02x",
                             stream_attempt + 1, RS300_STREAM_START_RETRIES, retry, status_buffer[0]);

                    if (!(status_buffer[0] & VCMD_BUSY_STS_BIT) &&
                        !(status_buffer[0] & VCMD_ERR_STS_BIT)) {
                        dev_dbg(&client->dev, "Busy bit cleared, no error");
                        break;
                    }

                    // Reset bit failure is a hard error - don't retry
                    if (status_buffer[0] & VCMD_RST_STS_BIT) {
                        dev_err(&client->dev, "Camera reset failed (hard error)");
                        ret = -EIO;
                        goto error_unlock;
                    }

                    // Error bit during polling - skip sleep, go straight to retry
                    if (status_buffer[0] & VCMD_ERR_STS_BIT) {
                        dev_warn(&client->dev, "Camera error 0x%02x on attempt %d/%d",
                                 status_buffer[0], stream_attempt + 1, RS300_STREAM_START_RETRIES);
                        got_error = 1;
                        break;
                    }
                }

                msleep(100);
                retry++;
            }

            // Busy timeout is a hard error - don't retry
            if (retry >= max_retries && !got_error) {
                dev_err(&client->dev, "Camera remained busy after %dms (hard error)", timeout_ms);
                ret = -ETIMEDOUT;
                goto error_unlock;
            }

            // Error detected - skip stabilization sleep, retry immediately
            if (got_error) {
                if (stream_attempt < RS300_STREAM_START_RETRIES - 1) {
                    dev_warn(&client->dev, "Retrying stream start (attempt %d/%d)...",
                             stream_attempt + 2, RS300_STREAM_START_RETRIES);
                    if (rs300_send_start_packet(rs300, start_regs, "stream retry") < 0) {
                        dev_err(&client->dev, "Failed to re-send start command");
                        ret = -EIO;
                        goto error_unlock;
                    }
                    continue;
                }

                // Final attempt failed - warn but don't close stream
                dev_err(&client->dev,
                        "Camera reported error after %d attempts (status: 0x%02x). "
                        "Stream may still be functional.",
                        RS300_STREAM_START_RETRIES, status_buffer[0]);
                break;
            }

            // Clean success - stabilization sleep only here
            msleep(stream_attempt < RS300_STREAM_START_RETRIES - 1 ? 2000 : 1000);
            dev_dbg(&client->dev, "Stream started successfully on attempt %d/%d",
                     stream_attempt + 1, RS300_STREAM_START_RETRIES);
            stream_success = 1;
            break;
        }

        // Set streaming flag regardless - camera may still be delivering frames
        // even if status register reported an error
        rs300->streaming = true;
        if (stream_success)
            dev_info(&client->dev, "Stream started successfully");
        else
            dev_warn(&client->dev, "Stream started with errors - check output");
    } else {
        dev_dbg(&client->dev, "Stopping stream");
        rs300_stop_streaming(rs300);
        rs300->streaming = false;
        dev_dbg(&client->dev, "Stream stopped, streaming flag cleared");
    }

    dev_dbg(&client->dev, "rs300_set_stream complete: streaming=%d, ret=%d",
            rs300->streaming, ret);
    mutex_unlock(&rs300->mutex);

    return ret;

error_unlock:
    dev_err(&client->dev, "=== STREAM ERROR EXIT: ret=%d ===", ret);
    /* Send STOP command to clean up camera state after failed START attempts */
    if (enable) {
        dev_info(&client->dev, "Sending STOP command to clean up after failed START");
        rs300_stop_streaming(rs300);
    }
    mutex_unlock(&rs300->mutex);
    return ret;
}

static int rs300_set_stream(struct v4l2_subdev *sd, int enable)
{
    return rs300_set_stream_state(to_rs300(sd), enable);
}

static const s64 link_freq_menu_items[] = {
	RS300_LINK_RATE,//80m
};
/* -----------------------------------------------------------------------------
 * V4L2 subdev internal operations
 */

static int rs300_power_on_state(struct rs300 *rs300)
{
	    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
	    struct device *dev = &client->dev;
	    int ret;

	    if (rs300->power_enabled)
		    return 0;

	    dev_dbg(dev, "Powering on rs300");

	    if (rs300->power_gpio) {
		    gpiod_set_value_cansleep(rs300->power_gpio, 1);
		    dev_info(dev, "Power GPIO asserted");
	    }

	    if (rs300->supplies_present) {
		    ret = regulator_bulk_enable(rs300_NUM_SUPPLIES, rs300->supplies);
		    if (ret) {
		        dev_err(dev, "failed to enable regulators\n");
		        if (rs300->power_gpio)
			        gpiod_set_value_cansleep(rs300->power_gpio, 0);
		        return ret;
		    }
		    rs300->supplies_enabled = true;
	    } else {
		    dev_dbg(dev, "No regulators configured, assuming board power is already enabled");
	    }

    if (rs300->reset_gpio) {
        gpiod_set_value_cansleep(rs300->reset_gpio, 1);
        msleep(20);
        gpiod_set_value_cansleep(rs300->reset_gpio, 0);
    }

    if (power_on_delay_ms > 0)
        msleep(power_on_delay_ms);

    rs300->power_enabled = true;
    dev_dbg(dev, "Power on complete");

    return 0;
}

static int rs300_power_off_state(struct rs300 *rs300)
{
	struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
	struct device *dev = &client->dev;

	if (rs300->reset_gpio) {
		gpiod_set_value_cansleep(rs300->reset_gpio, 1); //logic high -> device tree defines reset: logic high = 0V (active low)
		dev_dbg(dev, "Resetting rs300");
	} else {
		dev_dbg(dev, "No reset GPIO configured, skipping reset");
	}

	if (rs300->supplies_enabled) {
		regulator_bulk_disable(rs300_NUM_SUPPLIES, rs300->supplies);
		rs300->supplies_enabled = false;
		dev_dbg(dev, "Regulators disabled");
	}

	if (rs300->power_gpio) {
		gpiod_set_value_cansleep(rs300->power_gpio, 0);
		dev_info(dev, "Power GPIO deasserted");
	}

	rs300->power_enabled = false;
	rs300->auto_shutter_initialized = false;
	rs300->stream_defaults_initialized = false;
	rs300->yuv_format_configured = false;

	return 0;
}

static int rs300_get_gpio_from_of(struct rs300 *rs300,
				  const char *consumer,
				  const char *property,
				  struct gpio_desc **desc)
{
	struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
	struct device *dev = &client->dev;
	int gpio;
	int ret;

	if (*desc || !dev->of_node)
		return 0;

	gpio = of_get_named_gpio(dev->of_node, property, 0);
	if (gpio == -ENOENT)
		return 0;
	if (gpio < 0)
		return gpio;

	ret = devm_gpio_request_one(dev, gpio, GPIOF_OUT_INIT_LOW, consumer);
	if (ret)
		return ret;

	*desc = gpio_to_desc(gpio);
	if (!*desc)
		return -EINVAL;

	dev_info(dev, "Acquired %s from %s as GPIO %d", consumer, property, gpio);

	return 0;
}

static int rs300_get_regulators(struct rs300 *rs300)
{
	struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
	unsigned int i;
	int ret;

	for (i = 0; i < rs300_NUM_SUPPLIES; i++)
		rs300->supplies[i].supply = rs300_supply_names[i];

	ret = devm_regulator_bulk_get(&client->dev,
				      rs300_NUM_SUPPLIES,
				      rs300->supplies);
	if (ret == -ENODEV) {
		dev_warn(&client->dev,
			 "No regulator supplies configured; assuming RS300 is externally powered\n");
		rs300->supplies_present = false;
		return 0;
	}

	if (!ret)
		rs300->supplies_present = true;

	return ret;
}

static const struct v4l2_subdev_core_ops rs300_subdev_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
	.ioctl = rs300_ioctl, //NEEDED?
};

static const struct v4l2_subdev_video_ops rs300_subdev_video_ops = {
	.s_stream = rs300_set_stream,
};

static int rs300_get_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel)
{
	struct rs300 *rs300 = to_rs300(sd);
	const struct rs300_mode *mode;

	/* Validate pad */
	if (sel->pad >= NUM_PADS)
		return -EINVAL;

	if (sel->pad != IMAGE_PAD)
		return -EINVAL;

	mutex_lock(&rs300->mutex);
	mode = rs300->mode;

	/* Defensive check - mode should always be set during normal operation */
	if (!mode) {
		dev_err(sd->dev->parent, "get_selection: mode is NULL\n");
		mutex_unlock(&rs300->mutex);
		return -EINVAL;
	}

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		/* All targets return active sensor dimensions */
		/* (Different physical sensors: 640×512, 384×288, 256×192) */
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = mode->width;
		sel->r.height = mode->height;
		mutex_unlock(&rs300->mutex);
		return 0;

	default:
		mutex_unlock(&rs300->mutex);
		return -EINVAL;
	}
}

static int rs300_set_selection(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state,
			     struct v4l2_subdev_selection *sel)
{
	/* We don't support actual cropping, just return the full frame */
	return rs300_get_selection(sd, sd_state, sel);
}

static const struct v4l2_subdev_pad_ops rs300_subdev_pad_ops = {
	.enum_mbus_code = rs300_enum_mbus_code,
	.get_fmt = rs300_get_pad_fmt,
	.set_fmt = rs300_set_pad_fmt,
	.enum_frame_size = rs300_enum_frame_sizes,
	.get_selection = rs300_get_selection,
	.set_selection = rs300_set_selection,
};

static const struct v4l2_subdev_ops rs300_subdev_ops = {
	.core  = &rs300_subdev_core_ops,
	.video = &rs300_subdev_video_ops,
	.pad   = &rs300_subdev_pad_ops,
};

static const struct v4l2_ctrl_config colormap_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 1,
    .name = "Colormap",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = colormap_menu,
    .min = 0,
    .max = 11,
    .def = 0,
};

static const struct v4l2_ctrl_config brightness_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = RS300_CID_BRIGHTNESS,
    .name = "Brightness",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = RS300_BRIGHTNESS_MIN,
    .max = RS300_BRIGHTNESS_MAX,
    .step = RS300_BRIGHTNESS_STEP,
    .def = RS300_BRIGHTNESS_DEFAULT,
};

static const struct v4l2_ctrl_config contrast_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = RS300_CID_CONTRAST,
    .name = "Contrast",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 100,
    .step = 1,
    .def = 50,
};

static const struct v4l2_ctrl_config ffc_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 2,
    .name = "FFC Trigger",
    .type = V4L2_CTRL_TYPE_BUTTON,
    .min = 0,
    .max = 0,
    .step = 0,
    .def = 0,
};

static const struct v4l2_ctrl_config scene_mode_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 3,
    .name = "Scene Mode",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = scene_mode_menu,
    .min = 3,
    .max = 3,
    .def = 3,
};

static const struct v4l2_ctrl_config dde_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 4,
    .name = "Digital Detail Enhancement",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 100,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config spatial_nr_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 5,
    .name = "Spatial Noise Reduction",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 100,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config temporal_nr_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 6,
    .name = "Temporal Noise Reduction",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 100,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config autoshutter_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 8,
    .name = "Auto Shutter",
    .type = V4L2_CTRL_TYPE_BOOLEAN,
    .min = 0,
    .max = 1,
    .step = 1,  /* BOOLEAN needs step=1 (unlike BUTTON which uses step=0) */
    .def = 0,
};

static const struct v4l2_ctrl_config autoshutter_temp_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 9,
    .name = "Auto Shutter Temperature",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,    /* Start at 0 for V4L2 validation */
    .max = 100,  /* 3.12°C maximum */
    .step = 1,
    .def = 50,   /* 1.56°C default */
};

static const struct v4l2_ctrl_config autoshutter_min_interval_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 10,
    .name = "Auto Shutter Min Interval",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,    /* Start at 0 for V4L2 validation */
    .max = 300,
    .step = 1,
    .def = 1,    /* 1 second default */
};

static const struct v4l2_ctrl_config autoshutter_max_interval_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 11,
    .name = "Auto Shutter Max Interval",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,    /* Start at 0 for V4L2 validation */
    .max = 600,
    .step = 1,
    .def = 120,  /* 120 seconds default */
};

static const struct v4l2_ctrl_config camera_sleep_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 12,
    .name = "Camera Sleep",
    .type = V4L2_CTRL_TYPE_BOOLEAN,
    .min = 0,
    .max = 1,
    .step = 1,
    .def = 0,  /* Awake by default */
};

static const struct v4l2_ctrl_config antiburn_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 13,
    .name = "Anti-burn Protection",
    .type = V4L2_CTRL_TYPE_BOOLEAN,
    .min = 0,
    .max = 1,
    .step = 1,
    .def = 0,  /* Off by default */
};

static const char * const shutter_menu[] = {
    "Closed",
    "Open",
    NULL
};

static const struct v4l2_ctrl_config shutter_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 14,
    .name = "Shutter State",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = shutter_menu,
    .min = 0,
    .max = 1,
    .def = 1,  /* Open by default */
};

static const char * const hook_edge_menu[] = {
    "No Hook",
    "1st Gear",
    "2 Levels",
    NULL
};

static const struct v4l2_ctrl_config hook_edge_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 15,
    .name = "Hook Edge Position",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = hook_edge_menu,
    .min = 0,
    .max = 2,
    .def = 0,
};

static const char * const frame_rate_menu[] = {
    "25Hz",
    "30Hz",
    "50Hz",
    "60Hz",
    NULL
};

static const struct v4l2_ctrl_config frame_rate_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 16,
    .name = "Detector Frame Rate",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = frame_rate_menu,
    .min = 0,
    .max = 3,
    .def = 3,  /* 60Hz default */
};

static const char * const yuv_format_menu[] = {
    "UYVY",
    "VYUY",
    "YUYV",
    "YVYU",
    NULL
};

static const struct v4l2_ctrl_config yuv_format_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 18,
    .name = "YUV Format",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = yuv_format_menu,
    .min = 0,
    .max = 3,
    .def = 2,
};

static const struct v4l2_ctrl_config analog_output_fmt_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 17,
    .name = "Digital-Analog Output Format",
    .type = V4L2_CTRL_TYPE_BOOLEAN,
    .min = 0,
    .max = 1,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config output_mode_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = V4L2_CID_CUSTOM_BASE + 7,
    .name = "Output Mode",
    .type = V4L2_CTRL_TYPE_MENU,
    .qmenu = output_mode_menu,
    .min = 0,
    .max = 1,
    .def = RS300_OUTPUT_MODE_DEFAULT,
};

static const struct v4l2_ctrl_config tegra_sensor_mode_id_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_SENSOR_MODE_ID,
    .name = "Sensor Mode ID",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 0,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config tegra_vi_bypass_mode_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_VI_BYPASS_MODE,
    .name = "Bypass Mode",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 1,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config tegra_override_enable_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_OVERRIDE_ENABLE,
    .name = "Override Enable",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 1,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config tegra_vi_height_align_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_VI_HEIGHT_ALIGN,
    .name = "VI Height Align",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 1,
    .max = 16,
    .step = 1,
    .def = 1,
};

static const struct v4l2_ctrl_config tegra_vi_size_align_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_VI_SIZE_ALIGN,
    .name = "VI Size Align",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 1,
    .max = 16,
    .step = 1,
    .def = 1,
};

static const struct v4l2_ctrl_config tegra_low_latency_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_LOW_LATENCY,
    .name = "Low Latency",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 1,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config tegra_vi_preferred_stride_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_VI_PREFERRED_STRIDE,
    .name = "Preferred Stride",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 0,
    .max = 4096,
    .step = 1,
    .def = 0,
};

static const struct v4l2_ctrl_config tegra_vi_capture_timeout_ctrl = {
    .ops = &rs300_ctrl_ops,
    .id = TEGRA_CAMERA_CID_VI_CAPTURE_TIMEOUT,
    .name = "Override Capture Timeout ms",
    .type = V4L2_CTRL_TYPE_INTEGER,
    .min = 100,
    .max = 10000,
    .step = 1,
    .def = 2500,
};

static int rs300_init_controls(struct rs300 *rs300)
{
    struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
    struct v4l2_ctrl_handler *ctrl_hdlr;
    // Define link frequency menu items - values must be in ascending order
    static const s64 link_freq_menu[] = {
        RS300_LINK_RATE  // Single link frequency for testing
    };
    int ret;
    u64 pixel_rate;

    dev_dbg(&client->dev, "Initializing controls");

    ctrl_hdlr = &rs300->ctrl_handler;
    ret = v4l2_ctrl_handler_init(ctrl_hdlr, 37);
    if (ret) {
        dev_err(&client->dev, "Failed to init ctrl handler: %d", ret);
        return ret;
    }

    /* Set the lock for the control handler */
    ctrl_hdlr->lock = &rs300->mutex;
    
    /* Add standard controls */
    rs300->link_frequency = v4l2_ctrl_new_int_menu(ctrl_hdlr, NULL,
        V4L2_CID_LINK_FREQ, 
        0, // Maximum index (not array size)
        0, // Default index
        link_freq_menu);
    
    if (rs300->link_frequency)
        rs300->link_frequency->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    /* Initialize pixel rate based on the Jetson default YUYV format. */
    pixel_rate = rs300_get_pixel_rate(RS300_DEFAULT_MBUS_CODE);
    rs300->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
                                      V4L2_CID_PIXEL_RATE,
                                      pixel_rate, pixel_rate, 1, 
                                      pixel_rate);
    
    if (rs300->pixel_rate)
        rs300->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    /* Add mandatory V4L2 controls for libcamera ISP integration */
    /* HBLANK: Horizontal blanking (pixels beyond active area per line) */
    rs300->hblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
                                      V4L2_CID_HBLANK,
                                      100, 100, 1,  /* Fixed at 100 pixels */
                                      100);

    if (rs300->hblank)
        rs300->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    /* VBLANK: Vertical blanking (lines beyond active area per frame) */
    rs300->vblank = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
                                      V4L2_CID_VBLANK,
                                      10, 10, 1,    /* Fixed at 10 lines */
                                      10);

    if (rs300->vblank)
        rs300->vblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    /* EXPOSURE: Mandatory for libcamera integration */
    /* Range: 1 to sensor height (varies by module: 512, 384, or 288) */
    rs300->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &rs300_ctrl_ops,
                                        V4L2_CID_EXPOSURE,
                                        1, rs300->mode->height, 1,
                                        rs300->mode->height);

    /* ANALOGUE_GAIN: Mandatory for libcamera integration */
    /* Fixed at 1 (thermal sensors don't have hardware gain) */
    rs300->analogue_gain = v4l2_ctrl_new_std(ctrl_hdlr, NULL,
                                             V4L2_CID_ANALOGUE_GAIN,
                                             1, 1, 1, 1);

    if (rs300->analogue_gain)
        rs300->analogue_gain->flags |= V4L2_CTRL_FLAG_READ_ONLY;

    /* Add custom controls with simpler configurations */
    rs300->colormap = v4l2_ctrl_new_custom(ctrl_hdlr, &colormap_ctrl, NULL);
    rs300->brightness = v4l2_ctrl_new_custom(ctrl_hdlr, &brightness_ctrl, NULL);
    rs300->contrast = v4l2_ctrl_new_custom(ctrl_hdlr, &contrast_ctrl, NULL);
    rs300->shutter_cal = v4l2_ctrl_new_custom(ctrl_hdlr, &ffc_ctrl, NULL);
    rs300->zoom = v4l2_ctrl_new_std(ctrl_hdlr, &rs300_ctrl_ops,
                    V4L2_CID_ZOOM_ABSOLUTE, 1, 8, 1, 1);
    rs300->scene_mode = v4l2_ctrl_new_custom(ctrl_hdlr, &scene_mode_ctrl, NULL);
    rs300->dde = v4l2_ctrl_new_custom(ctrl_hdlr, &dde_ctrl, NULL);
    rs300->spatial_nr = v4l2_ctrl_new_custom(ctrl_hdlr, &spatial_nr_ctrl, NULL);
    rs300->temporal_nr = v4l2_ctrl_new_custom(ctrl_hdlr, &temporal_nr_ctrl, NULL);
    rs300->output_mode = v4l2_ctrl_new_custom(ctrl_hdlr, &output_mode_ctrl, NULL);
    rs300->autoshutter = v4l2_ctrl_new_custom(ctrl_hdlr, &autoshutter_ctrl, NULL);
    rs300->autoshutter_temp = v4l2_ctrl_new_custom(ctrl_hdlr, &autoshutter_temp_ctrl, NULL);
    rs300->autoshutter_min_interval = v4l2_ctrl_new_custom(ctrl_hdlr, &autoshutter_min_interval_ctrl, NULL);
    rs300->autoshutter_max_interval = v4l2_ctrl_new_custom(ctrl_hdlr, &autoshutter_max_interval_ctrl, NULL);
    rs300->camera_sleep = v4l2_ctrl_new_custom(ctrl_hdlr, &camera_sleep_ctrl, NULL);
    rs300->antiburn = v4l2_ctrl_new_custom(ctrl_hdlr, &antiburn_ctrl, NULL);
    rs300->shutter = v4l2_ctrl_new_custom(ctrl_hdlr, &shutter_ctrl, NULL);
    rs300->hook_edge = v4l2_ctrl_new_custom(ctrl_hdlr, &hook_edge_ctrl, NULL);
    rs300->frame_rate = v4l2_ctrl_new_custom(ctrl_hdlr, &frame_rate_ctrl, NULL);
    rs300->analog_output_fmt = v4l2_ctrl_new_custom(ctrl_hdlr, &analog_output_fmt_ctrl, NULL);
    rs300->yuv_format = v4l2_ctrl_new_custom(ctrl_hdlr, &yuv_format_ctrl, NULL);
    rs300->sensor_mode_id = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_sensor_mode_id_ctrl, NULL);
    rs300->vi_bypass_mode = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_vi_bypass_mode_ctrl, NULL);
    rs300->override_enable = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_override_enable_ctrl, NULL);
    rs300->vi_height_align = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_vi_height_align_ctrl, NULL);
    rs300->vi_size_align = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_vi_size_align_ctrl, NULL);
    rs300->low_latency = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_low_latency_ctrl, NULL);
    rs300->vi_preferred_stride = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_vi_preferred_stride_ctrl, NULL);
    rs300->vi_capture_timeout = v4l2_ctrl_new_custom(ctrl_hdlr, &tegra_vi_capture_timeout_ctrl, NULL);

    if (rs300->colormap)
        rs300->colormap->flags |= V4L2_CTRL_FLAG_VOLATILE;
    if (rs300->camera_sleep)
        rs300->camera_sleep->flags |= V4L2_CTRL_FLAG_VOLATILE;

    /* Check for errors */
    if (ctrl_hdlr->error) {
        ret = ctrl_hdlr->error;
        dev_err(&client->dev, "%s control init failed (%d)\n",
            __func__, ret);
        goto error;
    }
    
    /* Connect the control handler to the subdevice */
    rs300->sd.ctrl_handler = ctrl_hdlr;
    
    dev_dbg(&client->dev, "Control handler initialized successfully\n");

    return 0;

error:
    v4l2_ctrl_handler_free(ctrl_hdlr);

    return ret;
}

static void rs300_free_controls(struct rs300 *rs300)
{
	v4l2_ctrl_handler_free(&rs300->ctrl_handler);
	rs300->sd.ctrl_handler = NULL;
}

static bool rs300_custom_ctrl_filter(const struct v4l2_ctrl *ctrl)
{
	return ctrl->id >= V4L2_CID_CUSTOM_BASE + 1 &&
	       ctrl->id <= RS300_CID_CONTRAST;
}

static int rs300_add_custom_controls_to_tegracam(struct rs300 *rs300)
{
	struct i2c_client *client = v4l2_get_subdevdata(&rs300->sd);
	struct v4l2_ctrl_handler *tc_hdlr;
	int ret;

	if (!rs300->s_data || !rs300->s_data->ctrl_handler) {
		if (!rs300->s_data || !rs300->s_data->tegracam_ctrl_hdl) {
			dev_warn(&client->dev,
				 "tegracam control handler not available for RS300 controls");
			return 0;
		}
		tc_hdlr = &rs300->s_data->tegracam_ctrl_hdl->ctrl_handler;
	} else {
		tc_hdlr = rs300->s_data->ctrl_handler;
	}

	ret = v4l2_ctrl_add_handler(tc_hdlr, &rs300->ctrl_handler,
				    rs300_custom_ctrl_filter, false);
	if (ret || tc_hdlr->error) {
		ret = ret ? ret : tc_hdlr->error;
		dev_err(&client->dev,
			"failed to add RS300 controls to tegracam handler: %d",
			ret);
		return ret;
	}

	dev_info(&client->dev, "RS300 custom controls added to video node");

	return 0;
}

static int rs300_check_hwcfg(struct device *dev)
{
	struct fwnode_handle *endpoint;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	int ret = -EINVAL;

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint) {
		dev_err(dev, "endpoint node not found\n");
		return -EINVAL;
	}

	if (v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg)) {
		dev_err(dev, "could not parse endpoint\n");
		goto error_out;
	}
	
	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 2) {
		dev_err(dev, "only 2 data lanes are currently supported\n");
		goto error_out;
	}
	
	if (ep_cfg.nr_of_link_frequencies != 1 ||
	    ep_cfg.link_frequencies[0] != RS300_LINK_RATE) {
		dev_err(dev, "Link frequency not supported: %lld\n",
			ep_cfg.link_frequencies[0]);
		goto error_out;
	}

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);
	fwnode_handle_put(endpoint);

	return ret;
}

static struct camera_common_pdata *rs300_parse_dt(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct device_node *np = dev->of_node;
	struct camera_common_pdata *pdata;
	int gpio;

	if (!np)
		return NULL;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio == -EPROBE_DEFER)
		return ERR_PTR(-EPROBE_DEFER);
	if (gpio >= 0)
		pdata->reset_gpio = (unsigned int)gpio;

	if (of_property_read_string(np, "mclk", &pdata->mclk_name))
		dev_dbg(dev, "mclk not present, sensor clock assumed external\n");

	of_property_read_string(np, "avdd-reg", &pdata->regulators.avdd);
	of_property_read_string(np, "iovdd-reg", &pdata->regulators.iovdd);
	of_property_read_string(np, "dvdd-reg", &pdata->regulators.dvdd);

	return pdata;
}

static int rs300_force_tegracam_bus_code(struct rs300 *rs300)
{
	const struct camera_common_colorfmt *fmt;
	struct device *dev;

	if (!rs300 || !rs300->s_data)
		return -EINVAL;

	dev = rs300->s_data->dev ? rs300->s_data->dev : &rs300->client->dev;

	if (rs300_should_expose_native_y16(rs300)) {
		fmt = &rs300_native_y16_colorfmt;
		dev_info(dev,
			 "Requesting RS300 output_mode=1 as V4L2 Y16 over UYVY CSI transport\n");
	} else {
		fmt = camera_common_find_datafmt(RS300_DEFAULT_MBUS_CODE);
		if (!fmt) {
			dev_err(dev, "Tegra does not know RS300 media bus code 0x%x\n",
				RS300_DEFAULT_MBUS_CODE);
			return -EINVAL;
		}
	}

	if (rs300->s_data->colorfmt != fmt)
		dev_info(dev, "Forcing Tegra media bus code 0x%x for pixfmt 0x%x\n",
			 fmt->code, fmt->pix_fmt);

	rs300->s_data->colorfmt = fmt;
	if (rs300->mode) {
		rs300->s_data->fmt_width = rs300->mode->width;
		rs300->s_data->fmt_height = rs300->mode->height;
	}

	return 0;
}

static int rs300_tc_power_on(struct camera_common_data *s_data)
{
	struct rs300 *rs300 = s_data->priv;
	int ret;

	if (!rs300)
		return -EINVAL;

	ret = rs300_power_on_state(rs300);
	if (!ret && s_data->power)
		s_data->power->state = SWITCH_ON;

	return ret;
}

static int rs300_tc_power_off(struct camera_common_data *s_data)
{
	struct rs300 *rs300 = s_data->priv;

	if (!rs300)
		return -EINVAL;

	rs300_power_off_state(rs300);
	if (s_data->power)
		s_data->power->state = SWITCH_OFF;

	return 0;
}

static int rs300_tc_power_get(struct tegracam_device *tc_dev)
{
	if (tc_dev->s_data && tc_dev->s_data->power)
		tc_dev->s_data->power->state = SWITCH_ON;

	return 0;
}

static int rs300_tc_power_put(struct tegracam_device *tc_dev)
{
	return 0;
}

static int rs300_tc_read_reg(struct camera_common_data *s_data, u16 addr, u8 *val)
{
	struct i2c_client *client = to_i2c_client(s_data->dev);

	return read_regs(client, addr, val, 1);
}

static int rs300_tc_write_reg(struct camera_common_data *s_data, u16 addr, u8 val)
{
	struct i2c_client *client = to_i2c_client(s_data->dev);

	return write_regs(client, addr, &val, 1);
}

static int rs300_tc_set_mode(struct tegracam_device *tc_dev)
{
	struct rs300 *rs300 = tegracam_get_privdata(tc_dev);

	if (!rs300)
		return -EINVAL;

	rs300_prepare_runtime_frmfmt(rs300);
	rs300_set_default_format(rs300);
	rs300_force_tegracam_bus_code(rs300);

	return 0;
}

static int rs300_tc_start_streaming(struct tegracam_device *tc_dev)
{
	struct rs300 *rs300 = tegracam_get_privdata(tc_dev);

	if (!rs300)
		return -EINVAL;

	if (!rs300->power_enabled) {
		int ret = rs300_power_on_state(rs300);

		if (ret)
			return ret;
		if (tc_dev->s_data && tc_dev->s_data->power)
			tc_dev->s_data->power->state = SWITCH_ON;
	}

	rs300_force_tegracam_bus_code(rs300);

	return rs300_set_stream_state(rs300, 1);
}

static int rs300_tc_stop_streaming(struct tegracam_device *tc_dev)
{
	struct rs300 *rs300 = tegracam_get_privdata(tc_dev);

	if (!rs300)
		return -EINVAL;

	return rs300_set_stream_state(rs300, 0);
}

static struct camera_common_sensor_ops rs300_common_ops = {
	.numfrmfmts = ARRAY_SIZE(rs300_frmfmt),
	.frmfmt_table = rs300_frmfmt,
	.power_on = rs300_tc_power_on,
	.power_off = rs300_tc_power_off,
	.write_reg = rs300_tc_write_reg,
	.read_reg = rs300_tc_read_reg,
	.parse_dt = rs300_parse_dt,
	.power_get = rs300_tc_power_get,
	.power_put = rs300_tc_power_put,
	.set_mode = rs300_tc_set_mode,
	.start_streaming = rs300_tc_start_streaming,
	.stop_streaming = rs300_tc_stop_streaming,
};

static int rs300_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct rs300 *rs300;
	int ret;

	dev_info(dev, "Starting rs300_probe");
	
	dev_dbg(dev, "driver version: %02x.%02x.%02x",
		DRIVER_VERSION >> 16,
		(DRIVER_VERSION & 0xff00) >> 8,
		DRIVER_VERSION & 0x00ff);

	dev_dbg(dev, "Allocating memory for rs300 structure");
	rs300 = devm_kzalloc(&client->dev, sizeof(*rs300), GFP_KERNEL);
	if (!rs300) {
		dev_err(dev, "Failed to allocate memory for rs300 structure");
		return -ENOMEM;
	}
	dev_dbg(dev, "Memory allocation successful");

	tc_dev = devm_kzalloc(dev, sizeof(*tc_dev), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	rs300->client = client;

	dev_dbg(dev, "Initializing V4L2 subdev");
	v4l2_i2c_subdev_init(&rs300->sd, client, &rs300_subdev_ops);
	dev_dbg(dev, "V4L2 subdev initialization complete");

	/* Check the hardware configuration in device tree */
	dev_dbg(dev, "Checking hardware configuration");
	if (rs300_check_hwcfg(dev)) {
		dev_err(dev, "Hardware configuration check failed");
		return -EINVAL;
	}
	dev_dbg(dev, "Hardware configuration check successful");

	dev_dbg(dev, "Getting regulators");
	ret = rs300_get_regulators(rs300);
	if (ret) {
		dev_err(dev, "Failed to get regulators: %d", ret);
		return ret;
	}
	dev_dbg(dev, "Regulators acquired successfully");

	dev_dbg(dev, "Getting power GPIO");
	rs300->power_gpio = devm_gpiod_get_optional(dev, "power", GPIOD_OUT_LOW);
	if (IS_ERR(rs300->power_gpio)) {
		ret = PTR_ERR(rs300->power_gpio);
		dev_err(dev, "Failed to get power GPIO: %d", ret);
		return ret;
	}
	if (!rs300->power_gpio) {
		ret = rs300_get_gpio_from_of(rs300, "rs300-power", "power-gpios",
					     &rs300->power_gpio);
		if (ret) {
			dev_err(dev, "Failed to get fallback power GPIO: %d", ret);
			return ret;
		}
	}
	if (rs300->power_gpio)
		dev_info(dev, "Power GPIO acquired");
	else
		dev_warn(dev, "No power GPIO configured; assuming external camera power");

	dev_dbg(dev, "Getting reset GPIO");
	rs300->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(rs300->reset_gpio)) {
		ret = PTR_ERR(rs300->reset_gpio);
		dev_err(dev, "Failed to get reset GPIO: %d", ret);
		return ret;
	}
	if (!rs300->reset_gpio) {
		ret = rs300_get_gpio_from_of(rs300, "rs300-reset", "reset-gpios",
					     &rs300->reset_gpio);
		if (ret) {
			dev_err(dev, "Failed to get fallback reset GPIO: %d", ret);
			return ret;
		}
	}

	ret = rs300_power_on_state(rs300);
	if (ret) {
		dev_err(dev, "Failed to power on camera: %d", ret);
		return ret;
	}

	/* Initialize default format */
	rs300_set_default_format(rs300);
	rs300_prepare_runtime_frmfmt(rs300);

	/*
	 * YUV format configuration deferred to first stream start.
	 * The sensor is not ready for I2C commands during probe,
	 * causing -121 (EREMOTEIO) errors on register 0x1d00.
	 */
	rs300->yuv_format_configured = false;

	/* Initialize mutex */
	mutex_init(&rs300->mutex);

	ret = rs300_prime_i2c_only(rs300);
	if (ret)
		dev_warn(dev, "RS300 probe prime failed: %d (continuing)\n", ret);

	ret = rs300_init_controls(rs300);
	if (ret) {
		dev_err(dev, "failed to initialize RS300 controls: %d\n", ret);
		goto error_mutex_destroy;
	}
	rs300->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			    V4L2_SUBDEV_FL_HAS_EVENTS;
	rs300->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	rs300->tc_dev = tc_dev;
	tc_dev->client = client;
	tc_dev->dev = dev;
	strncpy(tc_dev->name, DRIVER_NAME, sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &rs300_regmap_config;
	rs300_common_ops.frmfmt_table = &rs300->runtime_frmfmt;
	rs300_common_ops.numfrmfmts = 1;
	tc_dev->sensor_ops = &rs300_common_ops;
	tc_dev->v4l2sd_internal_ops = NULL;
	tc_dev->tcctrl_ops = NULL;

	ret = tegracam_device_register(tc_dev);
	if (ret) {
		dev_err(dev, "tegra camera driver registration failed: %d\n", ret);
		goto error_controls_free;
	}

	rs300->s_data = tc_dev->s_data;
	tegracam_set_privdata(tc_dev, rs300);

	ret = rs300_force_tegracam_bus_code(rs300);
	if (ret)
		goto error_tegracam_unregister;

	ret = tegracam_v4l2subdev_register(tc_dev, true);
	if (ret) {
		dev_err(dev, "tegra camera subdev registration failed: %d\n", ret);
		goto error_tegracam_unregister;
	}
	rs300_force_tegracam_bus_code(rs300);
	ret = rs300_add_custom_controls_to_tegracam(rs300);
	if (ret)
		goto error_subdev_unregister;

	dev_info(dev, "RS300 tegracam sensor registered\n");

	return 0;

error_subdev_unregister:
	tegracam_v4l2subdev_unregister(tc_dev);

error_tegracam_unregister:
	tegracam_device_unregister(tc_dev);

error_controls_free:
	rs300_free_controls(rs300);

error_mutex_destroy:
	rs300_power_off_state(rs300);
	mutex_destroy(&rs300->mutex);

	return ret;
}

static int rs300_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct rs300 *rs300;

	if (!s_data)
		return -EINVAL;

	rs300 = s_data->priv;
	if (!rs300)
		return -EINVAL;

	tegracam_v4l2subdev_unregister(rs300->tc_dev);
	tegracam_device_unregister(rs300->tc_dev);
	rs300_free_controls(rs300);
	rs300_power_off_state(rs300);
	mutex_destroy(&rs300->mutex);
	dev_info(&client->dev, "RS300 driver removed\n");

	return 0;
}

static const struct i2c_device_id rs300_id[] = {
	{ "rs300", 0 },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(i2c, rs300_id);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id rs300_of_match[] = {
	{ .compatible = "mini2,rs300"  },
	{ .compatible = "infisense,rs300"  },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, rs300_of_match);
#endif

static struct i2c_driver rs300_i2c_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table = of_match_ptr(rs300_of_match),
	},
	.probe_new	= rs300_probe,
	.remove		= rs300_remove,
	.id_table	= rs300_id,
};

static int __init sensor_mod_init(void)
{
	return i2c_add_driver(&rs300_i2c_driver);
}

static void __exit sensor_mod_exit(void)
{
	i2c_del_driver(&rs300_i2c_driver);
}

device_initcall_sync(sensor_mod_init);
module_exit(sensor_mod_exit);

MODULE_AUTHOR("Kodrea; Jetson port by Codex");
MODULE_DESCRIPTION("Mini2/WN2 RS300 microbolometer thermal camera driver for NVIDIA Jetson");
MODULE_LICENSE("GPL v2");
