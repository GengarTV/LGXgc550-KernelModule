// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) - USB Command & I2C Bridge
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Credits & Attribution:
 * - Command bridge protocol, board-state detection (ADV7604 IO 0xFA / PCA9539A),
 *   automatic input color space fix (ADV7604 IO 0x02), and AT88SC CryptoMemory
 *   safety research by Chris Sawczuk (ChrisAJS) and contributors to
 *   `lgx2userspace` (https://github.com/ChrisAJS/lgx2userspace, MIT License).
 *
 * Protocol notes:
 * - Commands are sent over Bulk OUT 0x01 and replies received on Bulk IN 0x81.
 * - SAFETY WARNING: Opcode 0x05/0x06/0x07 communicate with an Atmel AT88SC
 *   CryptoMemory chip. Never replay recorded authentication challenges (0xB8/0xBA),
 *   as failed challenge attempts permanently decrement the hardware counter and
 *   lock the chip for official Windows drivers. The hardware streams video and
 *   audio normally without touching the CryptoMemory authentication zones.
 */

#include <linux/lockdep.h>
#include <linux/unaligned.h>
#include "gc550.h"
#include "gc550-regtables.h"

int gc550_cmd_init(struct gc550_dev *dev)
{
	dev->cmd_out_buf = kmalloc(GC550_CMD_MAX_PKT_SIZE, GFP_KERNEL);
	if (!dev->cmd_out_buf)
		return -ENOMEM;

	dev->cmd_in_buf = kmalloc(GC550_CMD_MAX_PKT_SIZE, GFP_KERNEL);
	if (!dev->cmd_in_buf) {
		kfree(dev->cmd_out_buf);
		dev->cmd_out_buf = NULL;
		return -ENOMEM;
	}

	return 0;
}

void gc550_cmd_free(struct gc550_dev *dev)
{
	mutex_lock(&dev->cmd_lock);
	kfree(dev->cmd_in_buf);
	kfree(dev->cmd_out_buf);
	dev->cmd_in_buf = NULL;
	dev->cmd_out_buf = NULL;
	mutex_unlock(&dev->cmd_lock);
}

/**
 * gc550_cmd_drain_responses - Drain any stale replies sitting in Bulk IN 0x81
 */
static void gc550_cmd_drain_responses(struct gc550_dev *dev)
{
	int actual_len = 0;
	int i;

	lockdep_assert_held(&dev->cmd_lock);
	if (READ_ONCE(dev->disconnected) || !dev->cmd_in_buf)
		return;

	for (i = 0; i < 4; i++) {
		int ret;

		if (READ_ONCE(dev->disconnected))
			break;
		ret = usb_bulk_msg(dev->udev,
				   usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN),
				   dev->cmd_in_buf, GC550_CMD_MAX_PKT_SIZE,
				   &actual_len, 20);
		if (ret < 0)
			break;
	}
}

static int gc550_cmd_send(struct gc550_dev *dev, const u8 *tx_buf, int tx_len)
{
	int actual_len = 0;
	int ret;

	lockdep_assert_held(&dev->cmd_lock);
	if (READ_ONCE(dev->disconnected) || !dev->cmd_out_buf)
		return -ENODEV;

	if (tx_len > GC550_CMD_MAX_PKT_SIZE)
		return -EINVAL;

	/* Hard safety gate: block any AT88SC CryptoMemory write/auth opcodes */
	if (tx_len > 0 && (tx_buf[0] == GC550_OP_CRYPTO_HDR ||
			   tx_buf[0] == GC550_OP_CRYPTO_READ ||
			   tx_buf[0] == GC550_OP_CRYPTO_DATA ||
			   tx_buf[0] == 0x09)) {
		dev_err(&dev->intf->dev,
			"Blocked unsafe CryptoMemory opcode 0x%02x!\n",
			tx_buf[0]);
		return -EPERM;
	}

	memcpy(dev->cmd_out_buf, tx_buf, tx_len);

	ret = usb_bulk_msg(dev->udev,
			   usb_sndbulkpipe(dev->udev, GC550_EP_CMD_OUT),
			   dev->cmd_out_buf, tx_len, &actual_len,
			   GC550_CMD_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Bulk OUT 0x01 failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int gc550_cmd_xfer(struct gc550_dev *dev, const u8 *tx_buf, int tx_len,
			  u8 *rx_buf, int rx_expected)
{
	int actual_len = 0;
	int ret;

	lockdep_assert_held(&dev->cmd_lock);
	if (READ_ONCE(dev->disconnected) || !dev->cmd_out_buf || !dev->cmd_in_buf)
		return -ENODEV;

	if (rx_expected <= 0 || rx_expected > GC550_CMD_MAX_PKT_SIZE)
		return -EINVAL;

	ret = gc550_cmd_send(dev, tx_buf, tx_len);
	if (ret < 0)
		return ret;

	/*
	 * Always read up to GC550_CMD_MAX_PKT_SIZE (wMaxPacketSize = 1024)
	 * to prevent USB babble / overflow if a previous reply was left queued.
	 */
	ret = usb_bulk_msg(dev->udev,
			   usb_rcvbulkpipe(dev->udev, GC550_EP_CMD_IN),
			   dev->cmd_in_buf, GC550_CMD_MAX_PKT_SIZE,
			   &actual_len, GC550_CMD_TIMEOUT_MS);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Bulk IN 0x81 failed: %d\n", ret);
		return ret;
	}

	if (actual_len != rx_expected) {
		ret = actual_len < rx_expected ? -EREMOTEIO : -EPROTO;
		dev_err(&dev->intf->dev,
			"Bulk IN 0x81 length mismatch: got %d, expected %d (%d)\n",
			actual_len, rx_expected, ret);
		return ret;
	}

	if (rx_buf)
		memcpy(rx_buf, dev->cmd_in_buf, rx_expected);

	return actual_len;
}

/**
 * gc550_i2c_write - Write up to 32 bytes to a 7-bit I2C slave over USB bridge
 * Wire format: [0x01] [slave_addr] [0x01 (reg addr len)] [reg] [data...]
 */
int gc550_i2c_write(struct gc550_dev *dev, u8 slave_addr, u8 reg,
		    const u8 *data, u8 len)
{
	u8 pkt[4 + 32];
	int ret;

	if (len > 32 || (len && !data))
		return -EINVAL;

	pkt[0] = GC550_OP_I2C_WRITE;
	pkt[1] = slave_addr;
	pkt[2] = 0x01; /* 1-byte register address */
	pkt[3] = reg;
	if (len > 0 && data)
		memcpy(&pkt[4], data, len);

	mutex_lock(&dev->cmd_lock);
	ret = gc550_cmd_send(dev, pkt, 4 + len);
	mutex_unlock(&dev->cmd_lock);

	return ret;
}

int gc550_i2c_write_byte(struct gc550_dev *dev, u8 slave_addr, u8 reg, u8 val)
{
	return gc550_i2c_write(dev, slave_addr, reg, &val, 1);
}

/**
 * gc550_i2c_read - Read bytes from a 7-bit I2C slave over USB bridge
 * Wire format: [0x02] [slave_addr] [0x01 (reg addr len)] [len] [reg]
 */
int gc550_i2c_read(struct gc550_dev *dev, u8 slave_addr, u8 reg,
		   u8 *data, u8 len)
{
	u8 pkt[5];
	int ret;

	if (!data || len == 0 || len > 64)
		return -EINVAL;

	pkt[0] = GC550_OP_I2C_READ;
	pkt[1] = slave_addr;
	pkt[2] = 0x01; /* 1-byte register address */
	pkt[3] = len;
	pkt[4] = reg;

	mutex_lock(&dev->cmd_lock);
	ret = gc550_cmd_xfer(dev, pkt, sizeof(pkt), data, len);
	mutex_unlock(&dev->cmd_lock);

	return (ret < 0) ? ret : 0;
}

int gc550_i2c_read_byte(struct gc550_dev *dev, u8 slave_addr, u8 reg, u8 *val)
{
	return gc550_i2c_read(dev, slave_addr, reg, val, 1);
}

/**
 * gc550_detect_board_state - Check if the GC550 was already bootstrapped
 *
 * The ADV7604 and PCA9539A retain their state across warm restarts as long as
 * USB power is maintained:
 * - ADV7604 IO 0xFA (EDID map address) is 0x00 at cold reset, 0x6C when configured.
 * - PCA9539A (0x74) config regs 0x06/0x07 are 00 89 at cold power-up, 00 09 when configured.
 */
int gc550_detect_board_state(struct gc550_dev *dev, bool *already_configured)
{
	u8 edid_map = 0;
	u8 gpio_cfg[2] = { 0, 0 };
	u8 op_fmt = 0, esdp_map = 0, dpp_map = 0, tx_pwr = 0;
	int ret;

	*already_configured = false;

	mutex_lock(&dev->cmd_lock);
	gc550_cmd_drain_responses(dev);
	mutex_unlock(&dev->cmd_lock);

	ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0xfa, &edid_map);
	if (ret < 0)
		return ret;

	ret = gc550_i2c_read(dev, GC550_I2C_PCA9539A,
			     GC550_PCA9539A_REG_CONFIG0, gpio_cfg, 2);
	if (ret < 0)
		return ret;

	dev_info(&dev->intf->dev,
		 "Board state probe: ADV7604 IO[0xFA]=0x%02x, PCA9539A CFG=0x%02x 0x%02x\n",
		 edid_map, gpio_cfg[0], gpio_cfg[1]);

	if (edid_map == 0x6c && gpio_cfg[0] == 0x00 && gpio_cfg[1] == 0x09) {
		/*
		 * Upstream adv76xx_probe() reinitializes ADV7604 IO[0x03] to
		 * 0x00 (8-bit SDR 4:2:2 via adv76xx_core_init()) and writes
		 * IO[0xFA] = 0x6c, while adv7511_probe() powers down ADV7513
		 * (Main[0x41] |= 0x40). Verify that the pre-probe capture
		 * pipeline and collision-free ESDP/DPP sub-map addresses are
		 * genuinely active before skipping bootstrap replay.
		 */
		ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x03,
					  &op_fmt);
		if (ret < 0)
			return ret;

		ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0xf6,
					  &esdp_map);
		if (ret < 0)
			return ret;

		ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0xf7,
					  &dpp_map);
		if (ret < 0)
			return ret;

		ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7511_TX, 0x41,
					  &tx_pwr);
		if (ret < 0)
			return ret;

		dev_info(&dev->intf->dev,
			 "Subdev pre-probe state: ADV7604 IO[0x03]=0x%02x IO[0xF6]=0x%02x IO[0xF7]=0x%02x, ADV7513 Main[0x41]=0x%02x\n",
			 op_fmt, esdp_map, dpp_map, tx_pwr);

		if (op_fmt == 0x80 && esdp_map == 0x56 && dpp_map == 0x76 &&
		    (tx_pwr & 0x40) == 0)
			*already_configured = true;
	}

	return 0;
}

/**
 * gc550_set_auto_input_colorspace - Set ADV7604 input color space to automatic
 *
 * Prevents green/orange tint on YCbCr/RGB HDMI sources by letting the ADV7604
 * choose input color space from the HDMI AVI InfoFrame (IO reg 0x02 bits 7:4 = 0xF)
 * and resetting CP picture adjustments (0x3A..0x3D) to neutral.
 */
static int gc550_set_auto_input_colorspace(struct gc550_dev *dev)
{
	static const u8 neutral_cp[4] = { 0x80, 0x80, 0x00, 0x00 };
	u8 io_02 = 0;
	int ret;

	ret = gc550_i2c_read_byte(dev, GC550_I2C_ADV7604_IO, 0x02, &io_02);
	if (ret == 0) {
		u8 new_io_02 = (io_02 & 0x0f) | 0xf0;

		if (new_io_02 != io_02) {
			ret = gc550_i2c_write_byte(dev, GC550_I2C_ADV7604_IO,
						   0x02, new_io_02);
			if (ret < 0)
				return ret;
		}
	}

	/* Reset CP contrast/saturation/brightness/hue to neutral */
	return gc550_i2c_write(dev, GC550_I2C_ADV7604_CP, 0x3a,
			       neutral_cp, sizeof(neutral_cp));
}

/*
 * gc550_translate_subdev_init_cmd - Translate colliding ADV7604 sub-map writes
 *
 * Translate the two colliding ADV7604 sub-map slave address assignments in
 * gc550_init_blob without altering the static blob:
 *   - ADV7604 0x20 reg 0xF6 (ESDP): 0x70 (0x38) -> 0x56 (0x2B)
 *   - ADV7604 0x20 reg 0xF7 (DPP):  0x78 (0x3C) -> 0x76 (0x3B)
 */
static const u8 *gc550_translate_subdev_init_cmd(const u8 *tx_data, u8 tx_len,
						 u16 rx_len, u8 buf[5])
{
	if (rx_len != 0 || tx_len != 5)
		return tx_data;

	if (tx_data[0] != GC550_OP_I2C_WRITE ||
	    tx_data[1] != GC550_I2C_ADV7604_IO ||
	    tx_data[2] != 0x01)
		return tx_data;

	if (tx_data[3] == 0xf6 && tx_data[4] == 0x70) {
		memcpy(buf, tx_data, 4);
		buf[4] = 0x56;
		return buf;
	}

	if (tx_data[3] == 0xf7 && tx_data[4] == 0x78) {
		memcpy(buf, tx_data, 4);
		buf[4] = 0x76;
		return buf;
	}

	return tx_data;
}

/*
 * Symbolic Board-Level Initialization Helpers (U2-B01-S1)
 *
 * The tables and helpers below structuralize all 172 verified board-level I2C
 * initialization steps in gc550_init_blob for the three authorized devices:
 *   - NXP PCA9539A GPIO expander (I2C 0x74): 132 steps across 7 step ranges
 *   - TI TLV320AIC3x audio codec (I2C 0x18):  16 steps in 1 contiguous range
 *   - Lattice ECP3 FPGA          (I2C 0x10):  24 steps across 6 step ranges
 *
 * Original execution order, register addresses, register values, discarded
 * read-before-write transactions, and interleaving with surrounding init
 * operations are preserved identically.
 */
struct gc550_board_reg_op {
	u8 op;
	u8 reg;
	u8 val;
};

#define GC550_BOARD_REG_WRITE(_reg, _val) \
	{ .op = GC550_OP_I2C_WRITE, .reg = (_reg), .val = (_val) }
#define GC550_BOARD_REG_READ(_reg) \
	{ .op = GC550_OP_I2C_READ, .reg = (_reg), .val = 0x00 }

static int gc550_cmd_i2c_write_reg_locked(struct gc550_dev *dev, u8 slave_addr,
					  u8 reg, u8 val)
{
	const u8 pkt[5] = {
		GC550_OP_I2C_WRITE,
		slave_addr,
		0x01,
		reg,
		val,
	};

	lockdep_assert_held(&dev->cmd_lock);
	return gc550_cmd_send(dev, pkt, sizeof(pkt));
}

static int gc550_cmd_i2c_read_reg_discard_locked(struct gc550_dev *dev,
						 u8 slave_addr, u8 reg)
{
	const u8 pkt[5] = {
		GC550_OP_I2C_READ,
		slave_addr,
		0x01,
		0x01,
		reg,
	};
	int ret;

	lockdep_assert_held(&dev->cmd_lock);
	ret = gc550_cmd_xfer(dev, pkt, sizeof(pkt), NULL, 1);
	return (ret < 0) ? ret : 0;
}

static int gc550_run_board_reg_ops_locked(struct gc550_dev *dev, u8 slave_addr,
					  const struct gc550_board_reg_op *ops,
					  size_t num_ops)
{
	size_t i;
	int ret;

	lockdep_assert_held(&dev->cmd_lock);

	for (i = 0; i < num_ops; i++) {
		if (READ_ONCE(dev->disconnected))
			return -ENODEV;

		if (ops[i].op == GC550_OP_I2C_WRITE)
			ret = gc550_cmd_i2c_write_reg_locked(dev, slave_addr,
							     ops[i].reg,
							     ops[i].val);
		else if (ops[i].op == GC550_OP_I2C_READ)
			ret = gc550_cmd_i2c_read_reg_discard_locked(dev,
								    slave_addr,
								    ops[i].reg);
		else
			return -EINVAL;

		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * PCA9539A (0x74) Stage 1: Steps 2..17 (offsets 13..140, 16 operations)
 * Configures Port 0 as all-outputs (CONFIG0 = 0x00) and toggles Port 0
 * output state 0x31 -> 0x33 -> 0x33 -> 0x31.
 */
static const struct gc550_board_reg_op gc550_pca9539a_init_stage1_ops[] = {
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x31),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x33),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x33),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x31),
};

/*
 * PCA9539A (0x74) Stage 2: Steps 20..51 (offsets 161..416, 32 operations)
 * Reads Port 0/1 output and config registers, sets Port 1 output/direction
 * (OUTPUT1 = 0x4f, CONFIG1 = 0x09), and sequences Port 0 output state
 * 0x71 -> 0x31 -> 0x71 -> 0x71 -> 0x51 -> 0x71.
 */
static const struct gc550_board_reg_op gc550_pca9539a_init_stage2_ops[] = {
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT1),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG1),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT1, 0x4f),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG1, 0x09),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x31),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x51),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
};

/*
 * PCA9539A (0x74) Stage 3: Steps 318..341 (offsets 2796..2987, 24 operations)
 * Sequences Port 0 output state 0x71 -> 0x31 -> 0x71 -> 0x71 -> 0x75 -> 0x71
 * prior to ADV7604/ADV7511 initialization.
 */
static const struct gc550_board_reg_op gc550_pca9539a_init_stage3_ops[] = {
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x31),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x75),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
};

/*
 * PCA9539A (0x74) Port 1 Pulse: Steps 1153..1164 (offsets 9480..9575, 12 operations)
 * Reaffirms Port 1 direction (CONFIG1 = 0x09) and pulses Port 1 output
 * 0xcf -> 0x46 -> 0xcf.
 */
static const struct gc550_board_reg_op gc550_pca9539a_port1_pulse_ops[] = {
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG1, 0x09),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT1, 0xcf),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG1, 0x09),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT1, 0x46),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG1, 0x09),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT1),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT1, 0xcf),
};

/*
 * PCA9539A (0x74) Port 0 Strobe:
 * Executed at three identical 16-operation spans in gc550_init_blob:
 *   - Steps 1387..1402 (offsets 11352..11479)
 *   - Steps 1416..1431 (offsets 11584..11711)
 *   - Steps 2586..2601 (offsets 21226..21353)
 * Strobes Port 0 output 0x71 -> 0x73 -> 0x73 -> 0x71 with CONFIG0 = 0x00.
 */
static const struct gc550_board_reg_op gc550_pca9539a_port0_strobe_ops[] = {
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x73),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x73),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_CONFIG0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_CONFIG0, 0x00),
	GC550_BOARD_REG_READ(GC550_PCA9539A_REG_OUTPUT0),
	GC550_BOARD_REG_WRITE(GC550_PCA9539A_REG_OUTPUT0, 0x71),
};

/*
 * TI TLV320AIC3x (0x18) Audio Codec Initialization:
 * Steps 52..67 (offsets 417..544, 16 write operations).
 * Configures PLL registers (PLL_PROGA..D, OVRF_STATUS_AND_PLLR), serial
 * audio interface format (ASD_INTF_CTRLA/B), digital filter control
 * (CODEC_DFILT_CTRL), and stereo LINE1L/LINE1R to Left/Right ADC routing
 * and PGA gain (LADC_VOL, RADC_VOL).
 */
static const struct gc550_board_reg_op gc550_tlv320aic3x_init_ops[] = {
	GC550_BOARD_REG_WRITE(GC550_AIC3X_PLL_PROGA_REG, 0x91),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_PLL_PROGB_REG, 0x10),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_PLL_PROGC_REG, 0x00),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_PLL_PROGD_REG, 0x00),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_OVRF_STATUS_AND_PLLR_REG, 0x01),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_ASD_INTF_CTRLA, 0xc0),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_ASD_INTF_CTRLB, 0x30),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_CODEC_DFILT_CTRL, 0x00),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_LINE1R_2_LADC_CTRL, 0x78),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_LINE1L_2_RADC_CTRL, 0x78),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_LADC_VOL, 0x0d),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_LINE1R_2_RADC_CTRL, 0x1c),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_RADC_VOL, 0x0d),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_LINE1L_2_LADC_CTRL, 0x1c),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_LADC_VOL, 0x0d),
	GC550_BOARD_REG_WRITE(GC550_AIC3X_RADC_VOL, 0x0d),
};

/*
 * Lattice ECP3 FPGA (0x10) Stage 1 Initialization:
 * Steps 1096..1106 (offsets 9020..9107, 11 operations).
 * Reads FPGA version register (0x00), clears register 0x06 (with readback),
 * and writes initial configuration registers 0x01, 0x02, 0x87, 0x89, 0x80,
 * 0x81, and 0x86.
 */
static const struct gc550_board_reg_op gc550_fpga_init_stage1_ops[] = {
	GC550_BOARD_REG_READ(GC550_FPGA_REG_VERSION),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_06, 0x00),
	GC550_BOARD_REG_READ(GC550_FPGA_REG_06),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_06, 0x00),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_01, 0x0c),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_02, 0x00),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_87, 0x03),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_89, 0x03),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_80, 0x21),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_81, 0x64),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_86, 0x11),
};

/*
 * Lattice ECP3 FPGA (0x10) Stage 2 Initialization:
 * Steps 2605..2613 (offsets 21378..21449, 9 operations).
 * Reads register 0x22, sets register 0x06 = 0x01 (with readback), writes
 * registers 0x01 = 0x0c and 0x02 = 0x00, reads register 0x05, and writes
 * registers 0x05 = 0x02 and 0x07 = 0x0f immediately prior to stream arming.
 */
static const struct gc550_board_reg_op gc550_fpga_init_stage2_ops[] = {
	GC550_BOARD_REG_READ(GC550_FPGA_REG_22),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_06, 0x01),
	GC550_BOARD_REG_READ(GC550_FPGA_REG_06),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_06, 0x01),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_01, 0x0c),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_02, 0x00),
	GC550_BOARD_REG_READ(GC550_FPGA_REG_05),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_05, 0x02),
	GC550_BOARD_REG_WRITE(GC550_FPGA_REG_07, 0x0f),
};

static int gc550_pca9539a_init_stage1_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_PCA9539A,
					      gc550_pca9539a_init_stage1_ops,
					      ARRAY_SIZE(gc550_pca9539a_init_stage1_ops));
}

static int gc550_pca9539a_init_stage2_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_PCA9539A,
					      gc550_pca9539a_init_stage2_ops,
					      ARRAY_SIZE(gc550_pca9539a_init_stage2_ops));
}

static int gc550_tlv320aic3x_init_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_TLV320AIC3X,
					      gc550_tlv320aic3x_init_ops,
					      ARRAY_SIZE(gc550_tlv320aic3x_init_ops));
}

static int gc550_board_init_stage2_locked(struct gc550_dev *dev)
{
	int ret;

	ret = gc550_pca9539a_init_stage2_locked(dev);
	if (ret < 0)
		return ret;

	return gc550_tlv320aic3x_init_locked(dev);
}

static int gc550_pca9539a_init_stage3_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_PCA9539A,
					      gc550_pca9539a_init_stage3_ops,
					      ARRAY_SIZE(gc550_pca9539a_init_stage3_ops));
}

static int gc550_fpga_init_stage1_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_FPGA,
					      gc550_fpga_init_stage1_ops,
					      ARRAY_SIZE(gc550_fpga_init_stage1_ops));
}

static int gc550_pca9539a_port1_pulse_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_PCA9539A,
					      gc550_pca9539a_port1_pulse_ops,
					      ARRAY_SIZE(gc550_pca9539a_port1_pulse_ops));
}

static int gc550_pca9539a_port0_strobe_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_PCA9539A,
					      gc550_pca9539a_port0_strobe_ops,
					      ARRAY_SIZE(gc550_pca9539a_port0_strobe_ops));
}

/* Step 2171 (offset 17790): single read of FPGA register 0x22 */
static int gc550_fpga_poll_reg22_locked(struct gc550_dev *dev)
{
	static const struct gc550_board_reg_op op =
		GC550_BOARD_REG_READ(GC550_FPGA_REG_22);

	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_FPGA, &op, 1);
}

/* Step 2534 (offset 20775): single write to FPGA register 0x80 = 0x21 */
static int gc550_fpga_write_reg80_locked(struct gc550_dev *dev)
{
	static const struct gc550_board_reg_op op =
		GC550_BOARD_REG_WRITE(GC550_FPGA_REG_80, 0x21);

	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_FPGA, &op, 1);
}

/* Step 2536 (offset 20791): single write to FPGA register 0x81 = 0x64 */
static int gc550_fpga_write_reg81_locked(struct gc550_dev *dev)
{
	static const struct gc550_board_reg_op op =
		GC550_BOARD_REG_WRITE(GC550_FPGA_REG_81, 0x64);

	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_FPGA, &op, 1);
}

/* Step 2538 (offset 20807): single write to FPGA register 0x86 = 0x11 */
static int gc550_fpga_write_reg86_locked(struct gc550_dev *dev)
{
	static const struct gc550_board_reg_op op =
		GC550_BOARD_REG_WRITE(GC550_FPGA_REG_86, 0x11);

	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_FPGA, &op, 1);
}

static int gc550_fpga_init_stage2_locked(struct gc550_dev *dev)
{
	return gc550_run_board_reg_ops_locked(dev, GC550_I2C_FPGA,
					      gc550_fpga_init_stage2_ops,
					      ARRAY_SIZE(gc550_fpga_init_stage2_ops));
}

static int gc550_run_blob_slice_locked(struct gc550_dev *dev,
				       size_t slice_offset, size_t slice_len)
{
	size_t offset = slice_offset;
	size_t end_offset;
	int ret = 0;

	lockdep_assert_held(&dev->cmd_lock);

	if (slice_offset > GC550_INIT_BLOB_SIZE ||
	    slice_len > GC550_INIT_BLOB_SIZE - slice_offset)
		return -EINVAL;

	end_offset = slice_offset + slice_len;

	while (offset < end_offset) {
		u8 tx_len;
		u16 rx_len;
		const u8 *tx_data;
		const u8 *cmd_buf;
		u8 translated_cmd[5];

		if (READ_ONCE(dev->disconnected))
			return -ENODEV;

		if (offset + 3 > end_offset)
			return -EINVAL;

		tx_len = gc550_init_blob[offset];
		rx_len = get_unaligned_le16(&gc550_init_blob[offset + 1]);

		if (tx_len == 0 || offset + 3 + tx_len > end_offset)
			return -EINVAL;

		tx_data = &gc550_init_blob[offset + 3];
		cmd_buf = gc550_translate_subdev_init_cmd(tx_data, tx_len,
							  rx_len,
							  translated_cmd);

		if (rx_len > 0)
			ret = gc550_cmd_xfer(dev, cmd_buf, tx_len, NULL, rx_len);
		else
			ret = gc550_cmd_send(dev, cmd_buf, tx_len);

		if (ret < 0)
			return ret;

		offset += 3 + tx_len;
	}

	if (offset != end_offset)
		return -EINVAL;

	return 0;
}

enum gc550_stage_type {
	GC550_STAGE_BLOB_SLICE,
	GC550_STAGE_BOARD,
};

struct gc550_init_stage {
	enum gc550_stage_type type;
	const char *name;
	union {
		struct {
			size_t offset;
			size_t len;
		} blob;
		int (*board_func)(struct gc550_dev *dev);
	};
};

#define GC550_STAGE_SLICE(_name, _off, _len) \
	{ \
		.type = GC550_STAGE_BLOB_SLICE, \
		.name = (_name), \
		.blob = { \
			.offset = (_off), \
			.len = (_len), \
		}, \
	}

#define GC550_STAGE_BOARD_FUNC(_name, _func) \
	{ \
		.type = GC550_STAGE_BOARD, \
		.name = (_name), \
		.board_func = (_func), \
	}

/*
 * Stage Sequence Descriptor Table (U2-B01-S2)
 *
 * Replaces transitional raw-step-index dispatch with an explicit 27-stage schedule:
 * 13 symbolic board-level stages and 14 raw subdevice/bridge slices.
 * Total: 2,615 operations, 21,458 bytes equivalent coverage.
 */
static const struct gc550_init_stage gc550_init_stages[] = {
	GC550_STAGE_SLICE("fx3_bridge_init", 0, 13),
	GC550_STAGE_BOARD_FUNC("pca9539a_stage1", gc550_pca9539a_init_stage1_locked),
	GC550_STAGE_SLICE("fx3_reg65_init1", 13, 20),
	GC550_STAGE_BOARD_FUNC("pca9539a_stage2_and_codec", gc550_board_init_stage2_locked),
	GC550_STAGE_SLICE("adv_subdev_preinit", 33, 2251),
	GC550_STAGE_BOARD_FUNC("pca9539a_stage3", gc550_pca9539a_init_stage3_locked),
	GC550_STAGE_SLICE("adv_subdev_core1", 2284, 6032),
	GC550_STAGE_BOARD_FUNC("fpga_stage1", gc550_fpga_init_stage1_locked),
	GC550_STAGE_SLICE("adv_subdev_core2", 8316, 372),
	GC550_STAGE_BOARD_FUNC("pca9539a_port1_pulse", gc550_pca9539a_port1_pulse_locked),
	GC550_STAGE_SLICE("adv_edid_hdmi_maps", 8688, 1776),
	GC550_STAGE_BOARD_FUNC("pca9539a_port0_strobe1", gc550_pca9539a_port0_strobe_locked),
	GC550_STAGE_SLICE("adv_cp_setup1", 10464, 104),
	GC550_STAGE_BOARD_FUNC("pca9539a_port0_strobe2", gc550_pca9539a_port0_strobe_locked),
	GC550_STAGE_SLICE("adv_timing_routing1", 10568, 6078),
	GC550_STAGE_BOARD_FUNC("fpga_poll_reg22", gc550_fpga_poll_reg22_locked),
	GC550_STAGE_SLICE("adv_timing_routing2", 16646, 2977),
	GC550_STAGE_BOARD_FUNC("fpga_write_reg80", gc550_fpga_write_reg80_locked),
	GC550_STAGE_SLICE("adv_infoframe1", 19623, 8),
	GC550_STAGE_BOARD_FUNC("fpga_write_reg81", gc550_fpga_write_reg81_locked),
	GC550_STAGE_SLICE("adv_infoframe2", 19631, 8),
	GC550_STAGE_BOARD_FUNC("fpga_write_reg86", gc550_fpga_write_reg86_locked),
	GC550_STAGE_SLICE("adv_infoframe3", 19639, 411),
	GC550_STAGE_BOARD_FUNC("pca9539a_port0_strobe3", gc550_pca9539a_port0_strobe_locked),
	GC550_STAGE_SLICE("adv_edid_maps", 20050, 24),
	GC550_STAGE_BOARD_FUNC("fpga_stage2", gc550_fpga_init_stage2_locked),
	GC550_STAGE_SLICE("adv_cp_final", 20074, 8),
};

/**
 * gc550_run_init_sequence - Execute the 27-stage hardware initialization sequence
 */
static int gc550_run_init_sequence(struct gc550_dev *dev)
{
	size_t i;
	int ret = 0;

	mutex_lock(&dev->cmd_lock);
	for (i = 0; i < ARRAY_SIZE(gc550_init_stages); i++) {
		const struct gc550_init_stage *stage = &gc550_init_stages[i];

		if (READ_ONCE(dev->disconnected)) {
			ret = -ENODEV;
			break;
		}

		if (stage->type == GC550_STAGE_BOARD)
			ret = stage->board_func(dev);
		else
			ret = gc550_run_blob_slice_locked(dev,
							  stage->blob.offset,
							  stage->blob.len);

		if (ret < 0) {
			dev_err(&dev->intf->dev,
				"Init sequence failed at stage %zu (%s): %d\n",
				i + 1, stage->name, ret);
			break;
		}
	}

	gc550_cmd_drain_responses(dev);
	mutex_unlock(&dev->cmd_lock);

	return (ret < 0) ? ret : 0;
}

/**
 * gc550_bootstrap_hw - Initialize GC550 I2C peripherals (ADV7604, ADV7511, PCA9539A)
 */
int gc550_bootstrap_hw(struct gc550_dev *dev)
{
	bool already_configured = false;
	int ret;

	if (dev->is_bootstrapped)
		return 0;

	ret = gc550_detect_board_state(dev, &already_configured);
	if (ret == -ENODEV || READ_ONCE(dev->disconnected))
		return -ENODEV;
	if (ret < 0) {
		dev_warn(&dev->intf->dev,
			 "Could not query board state (%d), proceeding with cold bootstrap\n",
			 ret);
	}

	if (already_configured) {
		dev_info(&dev->intf->dev,
			 "GC550 already configured; reusing existing hardware state\n");
		ret = gc550_set_auto_input_colorspace(dev);
		if (ret < 0)
			return ret;
		dev->is_bootstrapped = true;
		return 0;
	}

	dev_info(&dev->intf->dev,
		 "Bootstrapping GC550 (%u commands, CryptoMemory auth stripped)...\n",
		 GC550_INIT_NUM_STEPS);

	ret = gc550_run_init_sequence(dev);
	if (ret < 0)
		return ret;

	ret = gc550_set_auto_input_colorspace(dev);
	if (ret < 0)
		return ret;

	dev_info(&dev->intf->dev, "GC550 hardware bootstrap complete\n");
	dev->is_bootstrapped = true;
	return 0;
}
