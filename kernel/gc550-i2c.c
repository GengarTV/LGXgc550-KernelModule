// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) - USB-Tunneled Linux I2C Adapter
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Exposes the GC550 onboard I2C bus through the Linux I2C core using the
 * Cypress EZ-USB FX3 (CYUSB3014) Bulk Endpoint 0x01/0x81 command bridge.
 *
 * Capabilities & Wire Protocol Constraints:
 * - 7-bit slave addressing only (10-bit addressing rejected with -EAFNOSUPPORT).
 * - 1-byte register addressing prefix required for all operations.
 * - Single register-prefixed writes: 1-byte register offset followed by 0 to 32
 *   payload data bytes (total message length 1 to 33 bytes).
 * - Combined write-then-read: 1-byte register address write (msg 0) followed by
 *   read of 1 to 64 bytes (msg 1).
 * - Unsupported: raw reads without register offset, zero-length messages,
 *   multi-message transfers (num > 2), and cross-address repeated starts.
 * - SMBus operations supported through I2C-core emulation:
 *   * I2C_FUNC_SMBUS_BYTE_DATA (1-byte reg + 1-byte val)
 *   * I2C_FUNC_SMBUS_WORD_DATA (1-byte reg + 2-byte val, little-endian)
 *   * I2C_FUNC_SMBUS_I2C_BLOCK (1-byte reg + up to 32-byte data block)
 * - Validation Status: Source-level verified against kernel I2C core;
 *   physical hardware validation on live silicon pending authorization.
 *
 * Credits & Acknowledgments:
 * - Upstream protocol and USB-to-I2C wire framing reverse-engineered by
 *   Chris Sawczuk (ChrisAJS) and contributors at:
 *   https://github.com/ChrisAJS/lgx2userspace (MIT License).
 * - Multi-map subdevice architecture validated against Linux media drivers
 *   (`drivers/media/i2c/adv7604.c` & `drivers/media/i2c/adv7511-v4l2.c`).
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/i2c.h>
#include "gc550.h"

/**
 * gc550_i2c_master_xfer - Execute I2C transactions over FX3 USB command bridge
 * @adap: Pointer to Linux I2C adapter
 * @msgs: Array of I2C messages to execute
 * @num:  Number of messages in the array
 *
 * Translates standard Linux I2C message patterns into FX3 USB command packets.
 *
 * Supported transaction patterns:
 * 1. Single write message (num == 1, !(flags & I2C_M_RD)):
 *    - msgs[0].buf[0] = 8-bit register address.
 *    - msgs[0].buf[1..len-1] = payload data (0 to 32 bytes).
 *    Translated to FX3 Opcode 0x01 (GC550_OP_I2C_WRITE).
 *
 * 2. Combined register write followed by read (num == 2):
 *    - msgs[0] = 1-byte write specifying 8-bit register address.
 *    - msgs[1] = read of 1 to 64 bytes into msgs[1].buf.
 *    Translated to FX3 Opcode 0x02 (GC550_OP_I2C_READ).
 *
 * Return: Number of messages processed on success, negative error code on failure.
 */
static int gc550_i2c_master_xfer(struct i2c_adapter *adap,
				 struct i2c_msg *msgs, int num)
{
	struct gc550_dev *dev = i2c_get_adapdata(adap);
	int ret;

	might_sleep();

	if (READ_ONCE(dev->disconnected))
		return -ENODEV;

	if (!msgs || num <= 0)
		return -EINVAL;

	/*
	 * Reject 10-bit addressing: the FX3 command wire protocol strictly
	 * supports 7-bit slave addresses. Per Linux I2C fault code guidance
	 * (Documentation/i2c/fault-codes.rst), return -EAFNOSUPPORT.
	 */
	if (msgs[0].flags & I2C_M_TEN)
		return -EAFNOSUPPORT;

	if (msgs[0].addr > 0x7F)
		return -EINVAL;

	if (num == 1) {
		u8 reg;
		const u8 *payload;
		u8 payload_len;

		if (!msgs[0].buf)
			return -EINVAL;

		/*
		 * Raw read without a preceding register write is unsupported
		 * by the FX3 Opcode 0x02 protocol, which mandates an 8-bit
		 * register target offset.
		 */
		if (msgs[0].flags & I2C_M_RD) {
			dev_dbg_ratelimited(&dev->intf->dev,
					    "I2C raw read without register offset unsupported (addr=0x%02x)\n",
					    msgs[0].addr);
			return -EOPNOTSUPP;
		}

		/*
		 * A valid write transaction must contain at least the 1-byte
		 * register offset.
		 */
		if (msgs[0].len < 1)
			return -EOPNOTSUPP;

		/*
		 * If msgs[0].len == 1, the single byte is treated as the register
		 * offset with zero payload bytes (payload_len = 0, payload = NULL).
		 * This delegates to gc550_i2c_write() to send a 4-byte Opcode 0x01
		 * packet: [0x01, addr, 0x01, reg]. The physical wire response of
		 * the FX3 bridge for zero-payload writes remains unverified on
		 * live silicon pending hardware validation.
		 */
		reg = msgs[0].buf[0];
		payload_len = msgs[0].len - 1;
		payload = payload_len > 0 ? &msgs[0].buf[1] : NULL;

		/* FX3 Opcode 0x01 packet supports at most 32 payload data bytes */
		if (payload_len > 32) {
			dev_err_ratelimited(&dev->intf->dev,
					    "I2C write payload too large (%u > 32 bytes, addr=0x%02x)\n",
					    payload_len, msgs[0].addr);
			return -EOPNOTSUPP;
		}

		ret = gc550_i2c_write(dev, msgs[0].addr, reg, payload,
				      payload_len);
		if (ret < 0)
			return ret;

		return 1;
	}

	if (num == 2) {
		u8 reg;

		if (!msgs[0].buf || !msgs[1].buf)
			return -EINVAL;

		/* Second message must target the same 7-bit slave address */
		if (msgs[1].flags & I2C_M_TEN)
			return -EAFNOSUPPORT;

		if (msgs[0].addr != msgs[1].addr)
			return -EINVAL;

		/* First message must be write, second message must be read */
		if ((msgs[0].flags & I2C_M_RD) || !(msgs[1].flags & I2C_M_RD))
			return -EOPNOTSUPP;

		/* First message must specify exactly a 1-byte register offset */
		if (msgs[0].len != 1) {
			dev_dbg_ratelimited(&dev->intf->dev,
					    "I2C read prefix write len != 1 (%u bytes, addr=0x%02x)\n",
					    msgs[0].len, msgs[0].addr);
			return -EOPNOTSUPP;
		}

		/* FX3 Opcode 0x02 supports reading 1 to 64 bytes */
		if (msgs[1].len == 0 || msgs[1].len > 64) {
			dev_err_ratelimited(&dev->intf->dev,
					    "I2C read length out of range (%u bytes, addr=0x%02x)\n",
					    msgs[1].len, msgs[1].addr);
			return -EOPNOTSUPP;
		}

		reg = msgs[0].buf[0];
		ret = gc550_i2c_read(dev, msgs[0].addr, reg, msgs[1].buf,
				     msgs[1].len);
		if (ret < 0)
			return ret;

		return 2;
	}

	/* Multi-message sequences with num > 2 are not supported */
	return -EOPNOTSUPP;
}

/**
 * gc550_i2c_functionality - Report verified I2C/SMBus capabilities
 * @adap: Pointer to Linux I2C adapter
 *
 * CRITICAL UPSTREAM COMPLIANCE REQUIREMENT:
 * Deliberately advertises only verified transaction primitives supported by
 * the FX3 command bridge and required by in-tree media subdevice drivers:
 * - I2C_FUNC_I2C: Supports master_xfer for standard single/combined messages.
 * - I2C_FUNC_SMBUS_BYTE_DATA: Required by adv7604 (1-byte reg + 1-byte val).
 * - I2C_FUNC_SMBUS_WORD_DATA: 1-byte reg + 2-byte val (little-endian).
 * - I2C_FUNC_SMBUS_I2C_BLOCK: Sequential multi-byte read/write with 1-byte reg.
 *
 * Deliberately omitted unsupported SMBus/I2C features:
 * - I2C_FUNC_SMBUS_QUICK: 0-byte bus probing is not supported by FX3 firmware.
 * - I2C_FUNC_SMBUS_BYTE: Raw read/write without register byte is not supported.
 * - I2C_FUNC_SMBUS_BLOCK_DATA: SMBus 1.1 length-prefixed blocks not supported.
 * - I2C_FUNC_SMBUS_PROC_CALL / PEC: Not supported by hardware or firmware.
 * - I2C_FUNC_10BIT_ADDR / NOSTART: Arbitrary START/addressing not exposed.
 * - I2C_FUNC_SMBUS_EMUL: OMITTED because it bundles unsupported features (e.g. QUICK).
 */
static u32 gc550_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C |
	       I2C_FUNC_SMBUS_BYTE_DATA |
	       I2C_FUNC_SMBUS_WORD_DATA |
	       I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_algorithm gc550_i2c_algo = {
	.master_xfer   = gc550_i2c_master_xfer,
	.functionality = gc550_i2c_functionality,
};

/**
 * gc550_i2c_quirks - Linux I2C core adapter constraints
 *
 * Formally advertises the hardware transfer constraints of the Cypress FX3
 * command bridge to the Linux I2C core:
 * - I2C_AQ_COMB_WRITE_THEN_READ: Combined transfers must be write-then-read
 *   targeting the exact same 7-bit slave address.
 * - I2C_AQ_NO_ZERO_LEN: Zero-length messages are unsupported by the bridge.
 * - max_num_msgs = 2: Maximum 2 messages per transfer.
 * - max_write_len = 33: Maximum write message length (1-byte reg + 32-byte payload).
 * - max_comb_1st_msg_len = 1: First combined message must be 1-byte register offset.
 * - max_comb_2nd_msg_len = 64: Second combined message read bounded to 64 bytes.
 */
static const struct i2c_adapter_quirks gc550_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ | I2C_AQ_NO_ZERO_LEN,
	.max_num_msgs = 2,
	.max_write_len = 33,
	.max_comb_1st_msg_len = 1,
	.max_comb_2nd_msg_len = 64,
};

/**
 * gc550_i2c_adapter_init - Initialize and register the virtual I2C adapter
 * @dev: Main GC550 device structure
 *
 * Return: 0 on success, negative error code on failure.
 */
int gc550_i2c_adapter_init(struct gc550_dev *dev)
{
	struct i2c_adapter *adap = &dev->i2c_adap;
	int ret;

	memset(adap, 0, sizeof(*adap));
	adap->owner = THIS_MODULE;
	adap->class = 0;
	adap->algo = &gc550_i2c_algo;
	adap->quirks = &gc550_i2c_quirks;
	adap->dev.parent = &dev->intf->dev;
	i2c_set_adapdata(adap, dev);
	snprintf(adap->name, sizeof(adap->name), "gc550-i2c (%s)",
		 dev_name(&dev->intf->dev));

	ret = i2c_add_adapter(adap);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "i2c_add_adapter failed: %d\n", ret);
		return ret;
	}

	dev->i2c_registered = true;
	dev_info(&dev->intf->dev, "Registered I2C adapter %s as i2c-%d\n",
		 adap->name, adap->nr);
	return 0;
}

/**
 * gc550_i2c_adapter_free - Unregister the virtual I2C adapter
 * @dev: Main GC550 device structure
 */
void gc550_i2c_adapter_free(struct gc550_dev *dev)
{
	if (!dev->i2c_registered)
		return;

	i2c_del_adapter(&dev->i2c_adap);
	dev->i2c_registered = false;
	dev_dbg(&dev->intf->dev, "Unregistered I2C adapter\n");
}
