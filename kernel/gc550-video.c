// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * AVerMedia Live Gamer Extreme (GC550) - V4L2 Video Capture & Stream Parser
 *
 * Copyright (C) 2026 Øystein Lie <oeystein@gengartv.com>
 *
 * Credits & Attribution:
 * - BT.1120 SAV/EAV framing, header checksum formulas, cross-URB straddle
 *   handling, and 16 KiB FX3 DMA swap detection are based on the protocol
 *   reverse-engineering and reference implementation by Chris Sawczuk (ChrisAJS)
 *   and contributors in `lgx2userspace` (https://github.com/ChrisAJS/lgx2userspace, MIT License).
 *
 * Stream framing protocol on Bulk IN 0x83:
 * - Video frame start (SAV): [00 FF FF C0] + 4-byte header [b0 b1 b2 b3]
 *   Valid iff: b3 == (b0 + b1 + b2 - 0x40) & 0xFF
 * - Video payload: 1920 * 1080 * 2 = 4,147,200 bytes of YUYV (YUY2)
 * - Video frame end (EAV):   [00 FF FF C1] + 5-byte header [c0 c1 c2 c3 c4]
 *   Valid iff: c4 == (c0 + c1 + c2 + c3 - 0x3F) & 0xFF
 * - Audio packet start:      [00 FF FF 58] + 4-byte BE32 length [L3 L2 L1 L0]
 *   Followed by `length` bytes of 16-bit LE stereo PCM @ 48 kHz.
 *
 * Stream resilience features:
 * 1. Cross-URB marker straddle reassembly (`marker_stage` 0..3):
 *    Holds back partial `[00]`, `[00 FF]`, `[00 FF FF]` prefixes across URB
 *    boundaries so split markers never corrupt pixel buffers or miss frames.
 * 2. 16 KiB FX3 ping-pong DMA swap detector & reorderer:
 *    On warm restarts or HDMI signal drops, the FX3's dual 16 KiB DMA buffers
 *    can flip phase (delivering the second 16 KiB half of each 32 KiB pair
 *    first, resulting in an EAV from frame N-1 appearing 18,432 bytes after
 *    SAV(N)). When detected across two consecutive frames, `swap_mode` flips
 *    each 32 KiB pair back into proper order.
 */

#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/media-bus-format.h>
#include <media/i2c/adv7511.h>
#include <media/i2c/adv7604.h>
#include <media/v4l2-common.h>
#include <media/v4l2-dv-timings.h>
#include "gc550.h"

#define GC550_MAX_FRAME_SPAN	(GC550_FRAME_SIZE + 0x4000)

static int subdev_mode;
module_param(subdev_mode, int, 0444);
MODULE_PARM_DESC(subdev_mode,
		 "V4L2 subdevice registration mode (0=disabled/legacy default, 1=register adv7604 and adv7511 subdevices)");

/*
 * ADV7604 platform data:
 * - IO[0xFB] defaults to 0x68 (HDMI sub-map address 0x34 << 1), which the
 *   upstream adv7604 driver checks during probe.
 * - ESDP (0x2b) and DPP (0x3b) are reassigned away from 0x38 and 0x3c (and
 *   avoiding reserved/alternate transmitter address 0x39) to prevent 7-bit
 *   I2C address collisions with ADV7513 Packet Memory (0x38) and CEC (0x3c).
 * - InfoFrame (0x46) and TEST (0x29) match the GC550 hardware sub-map layout.
 */
static struct adv76xx_platform_data gc550_adv7604_pdata = {
	.disable_pwrdnb = 0,
	.disable_cable_det_rst = 0,
	.default_input = 0,
	.ain_sel = ADV7604_AIN1_2_3_NC_SYNC_1_2,
	.bus_order = ADV7604_BUS_ORDER_BGR,
	.op_format_mode_sel = ADV7604_OP_FORMAT_MODE0,
	.int1_config = ADV76XX_INT1_CONFIG_DISABLED,
	.alt_gamma = 0,
	.blank_data = 1,
	.insert_av_codes = 1,
	.replicate_av_codes = 0,
	.inv_vs_pol = 1,
	.inv_hs_pol = 1,
	.inv_llc_pol = 0,
	.dr_str_data = ADV76XX_DR_STR_HIGH,
	.dr_str_clk = ADV76XX_DR_STR_HIGH,
	.dr_str_sync = ADV76XX_DR_STR_HIGH,
	.output_bus_lsb_to_msb = 0,
	.hdmi_free_run_mode = 0,
	.i2c_addresses = {
		[ADV7604_PAGE_AVLINK]    = 0x42,
		[ADV76XX_PAGE_CEC]       = 0x40,
		[ADV76XX_PAGE_INFOFRAME] = 0x46,
		[ADV7604_PAGE_ESDP]      = 0x2b,
		[ADV7604_PAGE_DPP]       = 0x3b,
		[ADV76XX_PAGE_AFE]       = 0x26,
		[ADV76XX_PAGE_REP]       = 0x32,
		[ADV76XX_PAGE_EDID]      = 0x36,
		[ADV76XX_PAGE_HDMI]      = 0x34,
		[ADV76XX_PAGE_TEST]      = 0x29,
		[ADV76XX_PAGE_CP]        = 0x22,
		[ADV7604_PAGE_VDP]       = 0x24,
	},
};

static struct i2c_board_info gc550_adv7604_board_info = {
	I2C_BOARD_INFO("adv7604", GC550_I2C_ADV7604_IO),
	.platform_data = &gc550_adv7604_pdata,
};

static struct adv7511_platform_data gc550_adv7511_pdata = {
	.i2c_edid = 0x3f,
	.i2c_cec = 0x3c,
	.i2c_pktmem = 0x38,
	.cec_clk = 12000000,
};

static struct i2c_board_info gc550_adv7511_board_info = {
	I2C_BOARD_INFO("adv7511-v4l2", GC550_I2C_ADV7511_TX),
	.platform_data = &gc550_adv7511_pdata,
};

static int gc550_configure_adv7604(struct gc550_dev *dev)
{
	u32 width, height;
	struct v4l2_subdev_format fmt;
	int ret;

	if (!dev || !dev->sd_adv7604)
		return -ENODEV;

	width = dev->pix_fmt.width ? dev->pix_fmt.width : GC550_DEFAULT_WIDTH;
	height = dev->pix_fmt.height ? dev->pix_fmt.height : GC550_DEFAULT_HEIGHT;

	memset(&fmt, 0, sizeof(fmt));
	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = ADV7604_PAD_SOURCE;
	fmt.format.width = width;
	fmt.format.height = height;
	fmt.format.code = MEDIA_BUS_FMT_YUYV8_1X16;
	fmt.format.field = V4L2_FIELD_NONE;
	fmt.format.colorspace = dev->pix_fmt.colorspace ?
		dev->pix_fmt.colorspace : V4L2_COLORSPACE_REC709;
	fmt.format.ycbcr_enc = dev->pix_fmt.ycbcr_enc ?
		dev->pix_fmt.ycbcr_enc : V4L2_YCBCR_ENC_709;
	fmt.format.quantization = dev->pix_fmt.quantization ?
		dev->pix_fmt.quantization : V4L2_QUANTIZATION_LIM_RANGE;
	fmt.format.xfer_func = dev->pix_fmt.xfer_func ?
		dev->pix_fmt.xfer_func : V4L2_XFER_FUNC_709;

	ret = v4l2_subdev_call(dev->sd_adv7604, pad, set_fmt, NULL, &fmt);
	if (ret < 0) {
		dev_err(&dev->intf->dev,
			"Failed to configure adv7604 source pad format: %d\n",
			ret);
		return ret;
	}

	dev_info(&dev->intf->dev,
		 "Configured adv7604 pad %u format: code=0x%04x %ux%u\n",
		 ADV7604_PAD_SOURCE, fmt.format.code,
		 fmt.format.width, fmt.format.height);

	return 0;
}

static const u8 gc550_yuyv_black[4] = { 16, 128, 16, 128 };

static const struct v4l2_frmsize_discrete gc550_frame_sizes[] = {
	{ GC550_DEFAULT_WIDTH, GC550_DEFAULT_HEIGHT },
	{ 1280, 720 },
	{ 720, 576 },
	{ 720, 480 },
	{ 640, 480 },
};

static const struct v4l2_frmsize_discrete *
gc550_find_frame_size(u32 width, u32 height)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gc550_frame_sizes); i++) {
		if (gc550_frame_sizes[i].width == width &&
		    gc550_frame_sizes[i].height == height)
			return &gc550_frame_sizes[i];
	}

	return NULL;
}

void gc550_parser_reset(struct gc550_stream_parser *parser)
{
	parser->state = GC550_STATE_IDLE;
	parser->sav_b0 = 0;
	parser->hdr_pos = 0;
	parser->audio_rem = 0;
	parser->video_pos = 0;
	parser->marker_stage = 0;
	parser->pair_pos = 0;
}

static bool gc550_sav_hdr_valid(const u8 *h)
{
	u8 expected = (u8)((h[0] + h[1] + h[2] - 0x40) & 0xff);

	return h[3] == expected;
}

static bool gc550_eav_hdr_valid(const u8 *h)
{
	u8 expected = (u8)((h[0] + h[1] + h[2] + h[3] - 0x3f) & 0xff);

	return h[4] == expected;
}

static void gc550_finish_buffer(struct gc550_dev *dev,
				struct gc550_buffer *buf,
				enum vb2_buffer_state state)
{
	struct gc550_stream_parser *p = &dev->parser;
	size_t frame_size = dev->pix_fmt.sizeimage;
	size_t payload;

	if (!buf)
		return;

	if (state == VB2_BUF_STATE_DONE) {
		payload = frame_size;
	} else {
		if (p->video_pos == 0) {
			u8 *vbuf = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

			if (vbuf) {
				memcpy(vbuf, gc550_yuyv_black,
				       sizeof(gc550_yuyv_black));
				payload = sizeof(gc550_yuyv_black);
			} else {
				payload = 0;
			}
		} else {
			payload = min_t(size_t, (size_t)p->video_pos,
					frame_size);
		}
	}

	buf->vb.vb2_buf.timestamp = ktime_get_ns();
	buf->vb.field = V4L2_FIELD_NONE;
	buf->vb.sequence = p->current_frame_sequence;
	vb2_set_plane_payload(&buf->vb.vb2_buf, 0, payload);
	vb2_buffer_done(&buf->vb.vb2_buf, state);
}

#ifdef CONFIG_GC550_DIAG
static void gc550_abort_video_frame(struct gc550_dev *dev,
				    const struct gc550_dispatch_ctx *dctx,
				    u8 term_reason, bool is_pair_buf)
#else
static void gc550_abort_video_frame(struct gc550_dev *dev)
#endif
{
	struct gc550_stream_parser *p = &dev->parser;
	struct gc550_buffer *abort_buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf) {
		abort_buf = dev->cur_buf;
		dev->cur_buf = NULL;
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	if (abort_buf) {
#ifdef CONFIG_GC550_DIAG
		gc550_diag_emit_frame_complete(dev, dctx, abort_buf,
					       VB2_BUF_STATE_ERROR,
					       term_reason, is_pair_buf);
#endif
		gc550_finish_buffer(dev, abort_buf, VB2_BUF_STATE_ERROR);
	}

	p->video_pos = 0;
}

#ifdef CONFIG_GC550_DIAG
static void gc550_reserve_video_frame(struct gc550_dev *dev,
				      const struct gc550_dispatch_ctx *dctx,
				      bool is_pair_buf)
#else
static void gc550_reserve_video_frame(struct gc550_dev *dev)
#endif
{
	struct gc550_stream_parser *p = &dev->parser;
	struct gc550_buffer *abort_buf = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf) {
		abort_buf = dev->cur_buf;
		dev->cur_buf = NULL;
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	if (abort_buf) {
#ifdef CONFIG_GC550_DIAG
		gc550_diag_emit_frame_complete(dev, dctx, abort_buf,
					       VB2_BUF_STATE_ERROR,
					       GC550_TERM_UNCLOSED_SAV_ABORT,
					       is_pair_buf);
#endif
		gc550_finish_buffer(dev, abort_buf, VB2_BUF_STATE_ERROR);
	}

	p->current_frame_sequence = p->frame_sequence++;
	p->sav_b0 = p->hdr_buf[0];
	p->video_pos = 0;
	p->state = GC550_STATE_VIDEO_DATA;

#ifdef CONFIG_GC550_DIAG
	gc550_diag_emit_marker_sav(dev, dctx, p->hdr_buf, true, is_pair_buf);
#endif

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (!list_empty(&dev->buf_list)) {
		dev->cur_buf = list_first_entry(&dev->buf_list,
						struct gc550_buffer, list);
		list_del_init(&dev->cur_buf->list);
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);
}

static void gc550_append_video_bytes(struct gc550_dev *dev,
				     const u8 *src, size_t len)
{
	struct gc550_stream_parser *p = &dev->parser;
	unsigned long flags;
	void *vbuf;
	size_t frame_size = dev->pix_fmt.sizeimage;
	size_t room;

	if (!len)
		return;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf && p->video_pos < frame_size) {
		vbuf = vb2_plane_vaddr(&dev->cur_buf->vb.vb2_buf, 0);
		room = frame_size - p->video_pos;
		if (vbuf)
			memcpy((u8 *)vbuf + p->video_pos, src, min(len, room));
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	/* A missing EAV must not wrap the span counter into a valid frame size. */
	if (p->video_pos <= GC550_MAX_FRAME_SPAN)
		p->video_pos = min_t(size_t, (size_t)p->video_pos + len,
				     GC550_MAX_FRAME_SPAN + 1);
}

#ifdef CONFIG_GC550_DIAG
static void gc550_complete_video_frame(struct gc550_dev *dev, u8 eav_c0,
				       const struct gc550_dispatch_ctx *dctx,
				       bool is_pair_buf)
#else
static void gc550_complete_video_frame(struct gc550_dev *dev, u8 eav_c0)
#endif
{
	struct gc550_stream_parser *p = &dev->parser;
	struct gc550_buffer *buf = NULL;
	size_t frame_size = dev->pix_fmt.sizeimage;
	bool valid_span;
	unsigned long flags;
	size_t i;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf) {
		buf = dev->cur_buf;
		dev->cur_buf = NULL;
	}
	spin_unlock_irqrestore(&dev->queue_lock, flags);

	/*
	 * Detect 16 KiB FX3 ping-pong DMA swap:
	 * In swapped state, EAV(N-1) arrives ~18,432 bytes after SAV(N) with
	 * eav_c0 != sav_b0. If two consecutive SAV->EAV pairs exhibit this,
	 * toggle swap_mode to un-swap 16 KiB pairs.
	 */
	if (p->video_pos <= GC550_SWAP_SPAN_LIMIT && eav_c0 != p->sav_b0) {
		p->swap_votes++;
#ifdef CONFIG_GC550_DIAG
		gc550_diag_emit_swap_vote(dev, dctx, p->sav_b0, eav_c0,
					  p->swap_votes, is_pair_buf);
#endif
		if (p->swap_votes >= 2) {
			p->swap_mode = !p->swap_mode;
			p->swap_votes = 0;
			p->pair_pos = 0;
#ifdef CONFIG_GC550_DIAG
			gc550_diag_emit_swap_toggle(dev, dctx, !p->swap_mode,
						    p->swap_mode, is_pair_buf);
#endif
			dev_info_ratelimited(&dev->intf->dev,
					     "FX3 16 KiB DMA buffer order switched to %s\n",
					     p->swap_mode ? "SWAPPED" : "NORMAL");
		}
		if (buf) {
#ifdef CONFIG_GC550_DIAG
			gc550_diag_emit_frame_complete(dev, dctx, buf,
						       VB2_BUF_STATE_ERROR,
						       GC550_TERM_SWAP_CANDIDATE_ERROR,
						       is_pair_buf);
#endif
			gc550_finish_buffer(dev, buf, VB2_BUF_STATE_ERROR);
		}
		p->video_pos = 0;
		return;
	}

	p->swap_votes = 0;

	/*
	 * Accept a span within one 16 KiB FX3 DMA quantum of the selected
	 * V4L2 frame size.
	 */
	valid_span = (p->video_pos >= frame_size - GC550_FRAME_SPAN_TOLERANCE &&
		      p->video_pos <= frame_size + GC550_FRAME_SPAN_TOLERANCE);

	if (!valid_span) {
		if (buf) {
#ifdef CONFIG_GC550_DIAG
			gc550_diag_emit_frame_complete(dev, dctx, buf,
						       VB2_BUF_STATE_ERROR,
						       GC550_TERM_INVALID_SPAN_ERROR,
						       is_pair_buf);
#endif
			gc550_finish_buffer(dev, buf, VB2_BUF_STATE_ERROR);
		}
		p->video_pos = 0;
		return;
	}

	if (buf) {
		if (p->video_pos < frame_size) {
			u8 *vbuf = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);

			if (vbuf)
				for (i = p->video_pos; i < frame_size; i++)
					vbuf[i] = gc550_yuyv_black[i & 3];
		}

#ifdef CONFIG_GC550_DIAG
		gc550_diag_emit_frame_complete(dev, dctx, buf,
					       VB2_BUF_STATE_DONE,
					       GC550_TERM_NORMAL_DONE,
					       is_pair_buf);
#endif
		gc550_finish_buffer(dev, buf, VB2_BUF_STATE_DONE);
	}

	p->video_pos = 0;
}

static bool gc550_is_marker_type(u8 b)
{
	return b == GC550_MARKER_SAV ||
	       b == GC550_MARKER_EAV ||
	       b == GC550_MARKER_AUDIO;
}

static void gc550_handle_marker(struct gc550_dev *dev, u8 marker_type)
{
	struct gc550_stream_parser *p = &dev->parser;

	p->hdr_pos = 0;

	switch (marker_type) {
	case GC550_MARKER_SAV:
		p->state = GC550_STATE_SAV_HEADER;
		break;
	case GC550_MARKER_EAV:
		if (p->state == GC550_STATE_VIDEO_DATA)
			p->state = GC550_STATE_EAV_HEADER;
		else
			p->state = GC550_STATE_IDLE;
		break;
	case GC550_MARKER_AUDIO:
		p->state = GC550_STATE_AUDIO_HEADER;
		break;
	default:
		p->state = GC550_STATE_IDLE;
		break;
	}
}

/**
 * gc550_parse_ordered_bytes - Core byte-stream parser with cross-URB marker straddle support
 */
#ifdef CONFIG_GC550_DIAG
static void gc550_parse_ordered_bytes(struct gc550_dev *dev,
				      const u8 *data, size_t len,
				      const struct gc550_dispatch_ctx *dctx,
				      bool is_pair_buf)
#else
static void gc550_parse_ordered_bytes(struct gc550_dev *dev,
				      const u8 *data, size_t len)
#endif
{
	static const u8 prefix_bytes[3] = { 0x00, 0xff, 0xff };
	struct gc550_stream_parser *p = &dev->parser;
	size_t i = 0;

	while (i < len) {
		switch (p->state) {
		case GC550_STATE_SAV_HEADER:
			p->hdr_buf[p->hdr_pos++] = data[i++];
			if (p->hdr_pos == 4) {
				if (gc550_sav_hdr_valid(p->hdr_buf)) {
#ifdef CONFIG_GC550_DIAG
					gc550_reserve_video_frame(dev, dctx, is_pair_buf);
#else
					gc550_reserve_video_frame(dev);
#endif
				} else {
					p->desync_count++;
#ifdef CONFIG_GC550_DIAG
					gc550_diag_emit_marker_sav(dev, dctx, p->hdr_buf, false, is_pair_buf);
					gc550_diag_emit_desync(dev, dctx, GC550_DESYNC_SAV_CHECKSUM,
							       GC550_MARKER_SAV, p->hdr_buf[0], is_pair_buf);
#endif
					p->state = GC550_STATE_IDLE;
#ifdef CONFIG_GC550_DIAG
					gc550_abort_video_frame(dev, dctx, GC550_TERM_PARSER_ABORT_ERROR, is_pair_buf);
#else
					gc550_abort_video_frame(dev);
#endif
				}
			}
			continue;

		case GC550_STATE_EAV_HEADER:
			p->hdr_buf[p->hdr_pos++] = data[i++];
			if (p->hdr_pos == 5) {
				if (gc550_eav_hdr_valid(p->hdr_buf)) {
#ifdef CONFIG_GC550_DIAG
					gc550_diag_emit_marker_eav(dev, dctx, p->hdr_buf, true, is_pair_buf);
#endif
					if (p->hdr_buf[0] != p->sav_b0) {
						p->desync_count++;
#ifdef CONFIG_GC550_DIAG
						gc550_diag_emit_desync(dev, dctx, GC550_DESYNC_SEQ_MISMATCH,
								       GC550_MARKER_EAV, p->hdr_buf[0], is_pair_buf);
#endif
					}
#ifdef CONFIG_GC550_DIAG
					gc550_complete_video_frame(dev, p->hdr_buf[0], dctx, is_pair_buf);
#else
					gc550_complete_video_frame(dev, p->hdr_buf[0]);
#endif
					p->state = GC550_STATE_POST_EAV;
				} else {
					p->desync_count++;
#ifdef CONFIG_GC550_DIAG
					gc550_diag_emit_marker_eav(dev, dctx, p->hdr_buf, false, is_pair_buf);
					gc550_diag_emit_desync(dev, dctx, GC550_DESYNC_EAV_CHECKSUM,
							       GC550_MARKER_EAV, p->hdr_buf[0], is_pair_buf);
#endif
					p->state = GC550_STATE_IDLE;
#ifdef CONFIG_GC550_DIAG
					gc550_abort_video_frame(dev, dctx, GC550_TERM_PARSER_ABORT_ERROR, is_pair_buf);
#else
					gc550_abort_video_frame(dev);
#endif
				}
			}
			continue;

		case GC550_STATE_AUDIO_HEADER:
			p->hdr_buf[p->hdr_pos++] = data[i++];
			if (p->hdr_pos == 4) {
				p->audio_rem = get_unaligned_be32(p->hdr_buf);
#ifdef CONFIG_GC550_DIAG
				gc550_diag_emit_audio_header(dev, dctx, p->hdr_buf, p->audio_rem, is_pair_buf);
#endif
				if (p->audio_rem > 0 && p->audio_rem <= 65536)
					p->state = GC550_STATE_AUDIO_DATA;
				else
					p->state = GC550_STATE_IDLE;
			}
			continue;

		case GC550_STATE_AUDIO_DATA: {
			size_t avail = len - i;
			size_t take = min_t(size_t, avail, p->audio_rem);

			p->audio_bytes += take;
			gc550_audio_push_samples(dev, &data[i], take);
			i += take;
			p->audio_rem -= take;
			if (p->audio_rem == 0)
				p->state = GC550_STATE_IDLE;
			continue;
		}

		case GC550_STATE_IDLE:
		case GC550_STATE_POST_EAV:
		case GC550_STATE_VIDEO_DATA:
		default:
			break;
		}

		/*
		 * If we are in the middle of a potential [00 FF FF T] marker
		 * (including one that started at the tail of the previous URB),
		 * advance or flush the prefix state machine one byte at a time.
		 */
		if (p->marker_stage > 0) {
			u8 b = data[i];

			if (p->marker_stage == 1) {
				if (b == 0xff) {
					p->marker_stage = 2;
					i++;
					continue;
				}
			} else if (p->marker_stage == 2) {
				if (b == 0xff) {
					p->marker_stage = 3;
					i++;
					continue;
				}
			} else if (p->marker_stage == 3) {
				if (gc550_is_marker_type(b)) {
					p->marker_stage = 0;
					i++;
					gc550_handle_marker(dev, b);
					continue;
				}
			}

			/*
			 * False alarm: the held-back prefix bytes were actually
			 * regular video data. Flush them to the frame buffer.
			 */
			if (p->state == GC550_STATE_VIDEO_DATA)
				gc550_append_video_bytes(dev, prefix_bytes,
							 p->marker_stage);
			p->marker_stage = 0;
			/* Re-evaluate current byte `data[i]` from stage 0 */
		}

		/* Fast path when marker_stage == 0 */
		if (p->state == GC550_STATE_VIDEO_DATA) {
			const u8 *zero = memchr(&data[i], 0x00, len - i);

			if (!zero) {
				gc550_append_video_bytes(dev, &data[i], len - i);
				break;
			}

			if (zero > &data[i]) {
				size_t span = (size_t)(zero - &data[i]);

				gc550_append_video_bytes(dev, &data[i], span);
				i += span;
			}

			/* Now data[i] == 0x00 */
			p->marker_stage = 1;
			i++;
		} else {
			/* IDLE or POST_EAV: scan for 0x00 */
			const u8 *zero = memchr(&data[i], 0x00, len - i);

			if (!zero)
				break;

			i = (size_t)(zero - data);
			i++;
			p->marker_stage = 1;
		}
	}
}

/**
 * gc550_parse_stream_chunk - Top-level entry for Bulk IN 0x83 data with 16 KiB swap support
 */
#ifdef CONFIG_GC550_DIAG
void gc550_parse_stream_chunk(struct gc550_dev *dev, const u8 *data, size_t len,
			      const struct gc550_dispatch_ctx *dctx)
#else
void gc550_parse_stream_chunk(struct gc550_dev *dev, const u8 *data, size_t len)
#endif
{
	struct gc550_stream_parser *p = &dev->parser;

	if (!p->swap_mode || !p->pair_buf) {
#ifdef CONFIG_GC550_DIAG
		gc550_parse_ordered_bytes(dev, data, len, dctx, false);
#else
		gc550_parse_ordered_bytes(dev, data, len);
#endif
		return;
	}

	/*
	 * Swapped FX3 DMA mode: accumulate 32 KiB pairs and emit the second
	 * 16 KiB half before the first 16 KiB half.
	 */
	while (len > 0) {
		size_t space = GC550_PAIR_BUF_SIZE - p->pair_pos;
		size_t copy = min(len, space);

		memcpy(p->pair_buf + p->pair_pos, data, copy);
		p->pair_pos += copy;
		data += copy;
		len -= copy;

		if (p->pair_pos == GC550_PAIR_BUF_SIZE) {
			p->pair_pos = 0;

			/* Dispatch Half 1 first */
#ifdef CONFIG_GC550_DIAG
			gc550_parse_ordered_bytes(dev,
						  p->pair_buf + GC550_FX3_DMA_BUF_SIZE,
						  GC550_FX3_DMA_BUF_SIZE,
						  dctx, true);
#else
			gc550_parse_ordered_bytes(dev,
						  p->pair_buf + GC550_FX3_DMA_BUF_SIZE,
						  GC550_FX3_DMA_BUF_SIZE);
#endif

			/* Dispatch Half 0 second */
#ifdef CONFIG_GC550_DIAG
			gc550_parse_ordered_bytes(dev,
						  p->pair_buf,
						  GC550_FX3_DMA_BUF_SIZE,
						  dctx, true);
#else
			gc550_parse_ordered_bytes(dev,
						  p->pair_buf,
						  GC550_FX3_DMA_BUF_SIZE);
#endif
		}
	}
}

/* USB Bulk Stream URB Completion Callback */
static void gc550_urb_complete(struct urb *urb)
{
#ifdef CONFIG_GC550_DIAG
	struct gc550_urb_ctx *ctx = urb->context;
	struct gc550_dev *dev = ctx ? ctx->dev : NULL;
	u16 slot_index = ctx ? ctx->slot_index : 0;
	u64 sub_id = ctx ? ctx->submission_attempt_id : 0;
	u64 entry_seq = 0;
	int active = 0, max = 0;
	u64 start_time = 0;
	u16 cpu_id = 0;
	struct gc550_dispatch_ctx dctx;
#else
	struct gc550_dev *dev = urb->context;
#endif
	unsigned long flags;
	int ret;

	if (!dev)
		return;

#ifdef CONFIG_GC550_DIAG
	active = atomic_inc_return(&dev->diag_concurrency.active_callbacks);
	max = atomic_read(&dev->diag_concurrency.max_simultaneous);
	while (active > max && !atomic_try_cmpxchg(&dev->diag_concurrency.max_simultaneous, &max, active))
		;
	entry_seq = (u64)atomic64_inc_return(&dev->diag_concurrency.callback_entry_seq);
	start_time = ktime_get_raw_ns();
	cpu_id = (u16)raw_smp_processor_id();

	dctx.submission_generation = sub_id;
	dctx.completion_seq = entry_seq;
	dctx.slot_index = slot_index;
	dctx.cpu_id = cpu_id;
	dctx.active_callbacks = (u8)min(active, 255);
#endif

	spin_lock_irqsave(&dev->stream_lock, flags);
	if (!dev->streaming || dev->disconnected) {
		spin_unlock_irqrestore(&dev->stream_lock, flags);
#ifdef CONFIG_GC550_DIAG
		gc550_diag_emit_urb_complete(dev, ctx, urb, entry_seq,
					     active, start_time, cpu_id);
#endif
		goto out;
	}
	spin_unlock_irqrestore(&dev->stream_lock, flags);

#ifdef CONFIG_GC550_DIAG
	gc550_diag_emit_urb_complete(dev, ctx, urb, entry_seq,
				     active, start_time, cpu_id);
#endif

	switch (urb->status) {
	case 0:
		if (urb->actual_length > 0) {
			dev_dbg_ratelimited(&dev->intf->dev,
					    "URB rx: %d bytes (video_pos=%u, state=%d)\n",
					    urb->actual_length,
					    dev->parser.video_pos,
					    dev->parser.state);
#ifdef CONFIG_GC550_DIAG
			gc550_parse_stream_chunk(dev, urb->transfer_buffer,
						 urb->actual_length, &dctx);
#else
			gc550_parse_stream_chunk(dev, urb->transfer_buffer,
						 urb->actual_length);
#endif
		}
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		goto out;
	default:
		dev_warn_ratelimited(&dev->intf->dev,
				     "Stream URB status: %d (actual_len=%d)\n",
				     urb->status, urb->actual_length);
		/* Gap / USB transfer error: reset parser state */
#ifdef CONFIG_GC550_DIAG
		gc550_abort_video_frame(dev, &dctx, GC550_TERM_PARSER_ABORT_ERROR, false);
		gc550_parser_reset(&dev->parser);
		gc550_diag_emit_parser_reset(dev, &dctx, GC550_RESET_USB_TRANSFER_ERROR);
#else
		gc550_abort_video_frame(dev);
		gc550_parser_reset(&dev->parser);
#endif
		break;
	}

	/*
	 * Keep the gate lock across callback resubmission. GFP_ATOMIC is required
	 * here; disconnect/STREAMOFF takes the same lock before killing URBs.
	 */
	spin_lock_irqsave(&dev->stream_lock, flags);
	if (dev->streaming && !dev->disconnected) {
#ifdef CONFIG_GC550_DIAG
		sub_id = (u64)atomic64_inc_return(&dev->diag_submission_counter);
		if (ctx)
			ctx->submission_attempt_id = sub_id;
#endif
		ret = usb_submit_urb(urb, GFP_ATOMIC);
	} else {
		ret = -ESHUTDOWN;
	}
	spin_unlock_irqrestore(&dev->stream_lock, flags);
	if (ret < 0 && ret != -ENODEV && ret != -ESHUTDOWN) {
#ifdef CONFIG_GC550_DIAG
		gc550_diag_emit_submission_failure(dev, slot_index, sub_id, ret);
#endif
		dev_err_ratelimited(&dev->intf->dev,
				    "Failed to resubmit stream URB: %d\n", ret);
	}

out:
#ifdef CONFIG_GC550_DIAG
	atomic_dec(&dev->diag_concurrency.active_callbacks);
	atomic64_inc(&dev->diag_concurrency.callback_exit_seq);
#else
	;
#endif
}

static void gc550_free_stream_urbs(struct gc550_dev *dev)
{
	int i;

	for (i = 0; i < GC550_NUM_URBS; i++) {
		if (dev->urbs[i]) {
			usb_kill_urb(dev->urbs[i]);
			usb_free_urb(dev->urbs[i]);
			dev->urbs[i] = NULL;
		}
		kfree(dev->urb_bufs[i]);
		dev->urb_bufs[i] = NULL;
	}

	kfree(dev->parser.pair_buf);
	dev->parser.pair_buf = NULL;
}

static int gc550_alloc_stream_urbs(struct gc550_dev *dev)
{
	int i;

	dev->parser.pair_buf = kmalloc(GC550_PAIR_BUF_SIZE, GFP_KERNEL);
	if (!dev->parser.pair_buf)
		return -ENOMEM;

	for (i = 0; i < GC550_NUM_URBS; i++) {
		dev->urb_bufs[i] = kmalloc(GC550_URB_BUF_SIZE, GFP_KERNEL);
		if (!dev->urb_bufs[i]) {
			gc550_free_stream_urbs(dev);
			return -ENOMEM;
		}

		dev->urbs[i] = usb_alloc_urb(0, GFP_KERNEL);
		if (!dev->urbs[i]) {
			gc550_free_stream_urbs(dev);
			return -ENOMEM;
		}

#ifdef CONFIG_GC550_DIAG
		dev->urb_ctx[i].dev = dev;
		dev->urb_ctx[i].slot_index = (u16)i;
		dev->urb_ctx[i].submission_attempt_id = 0;

		usb_fill_bulk_urb(dev->urbs[i], dev->udev,
				  usb_rcvbulkpipe(dev->udev, GC550_EP_STREAM_IN),
				  dev->urb_bufs[i], GC550_URB_BUF_SIZE,
				  gc550_urb_complete, &dev->urb_ctx[i]);
#else
		usb_fill_bulk_urb(dev->urbs[i], dev->udev,
				  usb_rcvbulkpipe(dev->udev, GC550_EP_STREAM_IN),
				  dev->urb_bufs[i], GC550_URB_BUF_SIZE,
				  gc550_urb_complete, dev);
#endif
	}

	return 0;
}

static int gc550_submit_stream_urbs(struct gc550_dev *dev)
{
	int i, ret;
#ifdef CONFIG_GC550_DIAG
	u64 sub_id;
#endif

	for (i = 0; i < GC550_NUM_URBS; i++) {
#ifdef CONFIG_GC550_DIAG
		sub_id = (u64)atomic64_inc_return(&dev->diag_submission_counter);
		dev->urb_ctx[i].submission_attempt_id = sub_id;
#endif
		ret = usb_submit_urb(dev->urbs[i], GFP_KERNEL);
		if (ret < 0) {
#ifdef CONFIG_GC550_DIAG
			gc550_diag_emit_submission_failure(dev, (u16)i, sub_id, ret);
#endif
			dev_err(&dev->intf->dev,
				"Failed to submit stream URB %d: %d\n", i, ret);
			return ret;
		}
	}

	return 0;
}

/* Videobuf2 Queue Operations */
static int gc550_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct gc550_dev *dev = vb2_get_drv_priv(vq);

	if (*nplanes)
		return sizes[0] < dev->pix_fmt.sizeimage ? -EINVAL : 0;

	if (*nbuffers < 2)
		*nbuffers = 2;

	*nplanes = 1;
	sizes[0] = dev->pix_fmt.sizeimage;
	return 0;
}

static int gc550_buf_prepare(struct vb2_buffer *vb)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	unsigned long size = dev->pix_fmt.sizeimage;

	if (vb2_plane_size(vb, 0) < size) {
		dev_err(&dev->intf->dev,
			"Buffer too small (%lu < %lu)\n",
			vb2_plane_size(vb, 0), size);
		return -EINVAL;
	}

	if (!vb2_plane_vaddr(vb, 0))
		return -EFAULT;

	vb2_set_plane_payload(vb, 0, size);
	return 0;
}

static void gc550_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct gc550_dev *dev = vb2_get_drv_priv(vb->vb2_queue);
	struct gc550_buffer *buf = container_of(vbuf, struct gc550_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&dev->queue_lock, flags);
	list_add_tail(&buf->list, &dev->buf_list);
	spin_unlock_irqrestore(&dev->queue_lock, flags);
}

static void gc550_return_all_buffers(struct gc550_dev *dev,
				     enum vb2_buffer_state state)
{
	struct gc550_buffer *buf, *node;
	struct gc550_buffer *cur = NULL;
	LIST_HEAD(done_list);
	unsigned long flags;

	spin_lock_irqsave(&dev->queue_lock, flags);
	if (dev->cur_buf) {
		cur = dev->cur_buf;
		list_add_tail(&dev->cur_buf->list, &done_list);
		dev->cur_buf = NULL;
	}
	list_splice_tail_init(&dev->buf_list, &done_list);
	spin_unlock_irqrestore(&dev->queue_lock, flags);

#ifdef CONFIG_GC550_DIAG
	if (cur && state == VB2_BUF_STATE_ERROR)
		gc550_diag_emit_frame_complete(dev, NULL, cur, state,
					       GC550_TERM_STREAMOFF_TEARDOWN, false);
#endif

	list_for_each_entry_safe(buf, node, &done_list, list) {
		list_del_init(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
}

static void gc550_stop_stream_resources(struct gc550_dev *dev,
					enum vb2_buffer_state state)
{
	unsigned long flags;

#ifdef CONFIG_GC550_DIAG
	gc550_diag_emit_lifecycle(dev, GC550_EVT_STREAM_STOP_BEGIN);
#endif

	/* The callback resubmit gate is closed before kill waits for callbacks. */
	mutex_lock(&dev->stream_mutex);
	spin_lock_irqsave(&dev->stream_lock, flags);
	dev->streaming = false;
	spin_unlock_irqrestore(&dev->stream_lock, flags);
	gc550_free_stream_urbs(dev);
	mutex_unlock(&dev->stream_mutex);

	gc550_return_all_buffers(dev, state);

#ifdef CONFIG_GC550_DIAG
	gc550_diag_emit_lifecycle(dev, GC550_EVT_STREAM_STOP_COMPLETE);
#endif
}

void gc550_video_stop_device(struct gc550_dev *dev)
{
	gc550_stop_stream_resources(dev, VB2_BUF_STATE_ERROR);
}

static int gc550_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vq);
	unsigned long flags;
	int ret;

#ifdef CONFIG_GC550_DIAG
	dev->diag_session_epoch++;
	gc550_diag_emit_lifecycle(dev, GC550_EVT_STREAM_START_BEGIN);
	gc550_diag_emit_parser_reset(dev, NULL, GC550_RESET_STREAMON);
#endif

	gc550_parser_reset(&dev->parser);
	dev->parser.frame_sequence = 0;
	dev->parser.current_frame_sequence = 0;
	dev->parser.audio_bytes = 0;
	if (READ_ONCE(dev->disconnected)) {
		gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
		return -ENODEV;
	}

	if (dev->subdev_mode) {
		u32 input_status = 0;

		/*
		 * Subdevice mode (subdev_mode=1):
		 * Verify that hardware bootstrap completed and both expected
		 * subdevices are registered. Never replay gc550_bootstrap_hw().
		 */
		if (!dev->is_bootstrapped || !dev->sd_adv7604 || !dev->sd_adv7511) {
			dev_err(&dev->intf->dev,
				"Cannot stream in subdevice mode: unbootstrapped or missing subdevices (is_bootstrapped=%d, adv7604=%p, adv7511=%p)\n",
				dev->is_bootstrapped, dev->sd_adv7604, dev->sd_adv7511);
			gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
			return -EIO;
		}

		ret = gc550_configure_adv7604(dev);
		if (ret < 0) {
			dev_err(&dev->intf->dev,
				"Failed to reapply adv7604 format on stream start: %d\n",
				ret);
			gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
			return ret;
		}

		ret = v4l2_subdev_call(dev->sd_adv7604, video, g_input_status,
				       &input_status);
		if (ret == 0) {
			dev_info(&dev->intf->dev,
				 "ADV7604 input status: 0x%08x (%s%s)\n",
				 input_status,
				 (input_status == 0) ? "locked / signal OK" :
				 ((input_status & V4L2_IN_ST_NO_SIGNAL) ? "no signal " : ""),
				 (input_status & V4L2_IN_ST_NO_POWER) ? "no 5V power" : "");
		} else {
			dev_warn(&dev->intf->dev,
				 "ADV7604 g_input_status query failed: %d\n", ret);
		}
	} else {
		/*
		 * Legacy mode (subdev_mode=0):
		 * Fail-closed if any subdevice pointer is unexpectedly present.
		 */
		if (dev->sd_adv7604 || dev->sd_adv7511) {
			dev_err(&dev->intf->dev,
				"Cannot stream in legacy mode: subdevices unexpectedly registered\n");
			gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
			return -EIO;
		}

		ret = gc550_bootstrap_hw(dev);
		if (ret < 0) {
			gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
			return ret;
		}
		gc550_log_signal_status(dev);
	}

	mutex_lock(&dev->stream_mutex);
	ret = gc550_alloc_stream_urbs(dev);
	if (ret == 0) {
		spin_lock_irqsave(&dev->stream_lock, flags);
		if (dev->disconnected)
			ret = -ENODEV;
		else
			dev->streaming = true;
		spin_unlock_irqrestore(&dev->stream_lock, flags);
	}
	if (ret == 0)
		ret = gc550_submit_stream_urbs(dev);
	if (ret < 0) {
		spin_lock_irqsave(&dev->stream_lock, flags);
		dev->streaming = false;
		spin_unlock_irqrestore(&dev->stream_lock, flags);
		gc550_free_stream_urbs(dev);
	}
	mutex_unlock(&dev->stream_mutex);

	if (ret < 0) {
		gc550_return_all_buffers(dev, VB2_BUF_STATE_QUEUED);
		return ret;
	}

#ifdef CONFIG_GC550_DIAG
	gc550_diag_emit_lifecycle(dev, GC550_EVT_STREAM_START_COMPLETE);
#endif

	return 0;
}

static void gc550_stop_streaming(struct vb2_queue *vq)
{
	struct gc550_dev *dev = vb2_get_drv_priv(vq);

	gc550_stop_stream_resources(dev, VB2_BUF_STATE_ERROR);
	dev_info(&dev->intf->dev,
		 "Stream stopped: frames=%llu, audio_bytes=%llu, desyncs=%u, swap_mode=%d\n",
		 dev->parser.frame_sequence, dev->parser.audio_bytes,
		 dev->parser.desync_count, dev->parser.swap_mode);
}

static const struct vb2_ops gc550_vb2_ops = {
	.queue_setup     = gc550_queue_setup,
	.buf_prepare     = gc550_buf_prepare,
	.buf_queue       = gc550_buf_queue,
	.start_streaming = gc550_start_streaming,
	.stop_streaming  = gc550_stop_streaming,
};

/* V4L2 IOCTL Operations */
static void gc550_v4l2_release(struct v4l2_device *v4l2_dev)
{
	struct gc550_dev *dev = container_of(v4l2_dev, struct gc550_dev,
					     v4l2_dev);

	/* The V4L2 kref owns this device reference until its final put. */
	kref_put(&dev->ref, gc550_dev_release);
}

static int gc550_vidioc_querycap(struct file *file, void *priv,
				 struct v4l2_capability *cap)
{
	struct gc550_dev *dev = video_drvdata(file);

	strscpy(cap->driver, GC550_DRIVER_NAME, sizeof(cap->driver));
	strscpy(cap->card, "AVerMedia Live Gamer Extreme (GC550)",
		sizeof(cap->card));
	usb_make_path(dev->udev, cap->bus_info, sizeof(cap->bus_info));
	return 0;
}

static int gc550_vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
					 struct v4l2_fmtdesc *f)
{
	if (f->index > 0)
		return -EINVAL;

	f->pixelformat = V4L2_PIX_FMT_YUYV;
	return 0;
}

static int gc550_vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct gc550_dev *dev = video_drvdata(file);

	f->fmt.pix = dev->pix_fmt;
	return 0;
}

static int gc550_vidioc_try_fmt_vid_cap(struct file *file, void *priv,
					struct v4l2_format *f)
{
	const struct v4l2_frmsize_discrete *size;

	size = gc550_find_frame_size(f->fmt.pix.width, f->fmt.pix.height);
	if (!size)
		size = &gc550_frame_sizes[0];

	f->fmt.pix.width = size->width;
	f->fmt.pix.height = size->height;
	f->fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = size->width * GC550_BYTES_PER_PIXEL;
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * size->height;
	f->fmt.pix.colorspace = V4L2_COLORSPACE_REC709;
	f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_709;
	f->fmt.pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	f->fmt.pix.xfer_func = V4L2_XFER_FUNC_709;
	return 0;
}

static int gc550_vidioc_s_fmt_vid_cap(struct file *file, void *priv,
				      struct v4l2_format *f)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	gc550_vidioc_try_fmt_vid_cap(file, priv, f);
	dev->pix_fmt = f->fmt.pix;
	return 0;
}

static int gc550_vidioc_enum_framesizes(struct file *file, void *priv,
					struct v4l2_frmsizeenum *fsize)
{
	if (fsize->index >= ARRAY_SIZE(gc550_frame_sizes) ||
	    fsize->pixel_format != V4L2_PIX_FMT_YUYV)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
	fsize->discrete = gc550_frame_sizes[fsize->index];
	return 0;
}

static int gc550_vidioc_enum_frameintervals(struct file *file, void *priv,
					    struct v4l2_frmivalenum *fival)
{
	if (fival->index > 0 ||
	    fival->pixel_format != V4L2_PIX_FMT_YUYV ||
	    !gc550_find_frame_size(fival->width, fival->height))
		return -EINVAL;

	fival->type = V4L2_FRMIVAL_TYPE_DISCRETE;
	fival->discrete.numerator = 1;
	fival->discrete.denominator = 60;
	return 0;
}

static int gc550_vidioc_enum_input(struct file *file, void *priv,
				   struct v4l2_input *inp)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (inp->index > 0)
		return -EINVAL;

	strscpy(inp->name, "HDMI", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	if (dev->subdev_mode && dev->sd_adv7604)
		inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
	return 0;
}

static int gc550_vidioc_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int gc550_vidioc_s_input(struct file *file, void *priv, unsigned int i)
{
	return (i == 0) ? 0 : -EINVAL;
}

static int gc550_vidioc_g_dv_timings(struct file *file, void *priv,
				     struct v4l2_dv_timings *timings)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (!dev->subdev_mode || !dev->sd_adv7604)
		return -ENODATA;

	return v4l2_subdev_call(dev->sd_adv7604, pad, g_dv_timings,
				ADV76XX_PAD_HDMI_PORT_A, timings);
}

static int gc550_vidioc_s_dv_timings(struct file *file, void *priv,
				     struct v4l2_dv_timings *timings)
{
	struct gc550_dev *dev = video_drvdata(file);
	struct v4l2_dv_timings current_timings;
	const struct v4l2_frmsize_discrete *size;
	int ret;

	if (!dev->subdev_mode || !dev->sd_adv7604)
		return -ENODATA;

	if (vb2_is_busy(&dev->vb_queue))
		return -EBUSY;

	if (timings->type != V4L2_DV_BT_656_1120)
		return -EINVAL;

	size = gc550_find_frame_size(timings->bt.width, timings->bt.height);
	if (!size)
		return -EINVAL;

	ret = v4l2_subdev_call(dev->sd_adv7604, pad, g_dv_timings,
			       ADV76XX_PAD_HDMI_PORT_A, &current_timings);
	if (ret == 0 && v4l2_match_dv_timings(&current_timings, timings, 0, false))
		return 0;

	ret = v4l2_subdev_call(dev->sd_adv7604, pad, s_dv_timings,
			       ADV76XX_PAD_HDMI_PORT_A, timings);
	if (ret < 0)
		return ret;

	dev->pix_fmt.width = timings->bt.width;
	dev->pix_fmt.height = timings->bt.height;
	dev->pix_fmt.bytesperline = dev->pix_fmt.width * GC550_BYTES_PER_PIXEL;
	dev->pix_fmt.sizeimage = dev->pix_fmt.bytesperline * dev->pix_fmt.height;

	/* Reapply ADV7604 pad format so Pad 6 matches the newly set DV timings */
	gc550_configure_adv7604(dev);

	return 0;
}

static int gc550_vidioc_query_dv_timings(struct file *file, void *priv,
					 struct v4l2_dv_timings *timings)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (!dev->subdev_mode || !dev->sd_adv7604)
		return -ENODATA;

	return v4l2_subdev_call(dev->sd_adv7604, pad, query_dv_timings,
				ADV76XX_PAD_HDMI_PORT_A, timings);
}

static int gc550_vidioc_enum_dv_timings(struct file *file, void *priv,
					struct v4l2_enum_dv_timings *timings)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (!dev->subdev_mode || !dev->sd_adv7604)
		return -ENODATA;

	timings->pad = ADV76XX_PAD_HDMI_PORT_A;
	return v4l2_subdev_call(dev->sd_adv7604, pad, enum_dv_timings, timings);
}

static int gc550_vidioc_dv_timings_cap(struct file *file, void *priv,
				       struct v4l2_dv_timings_cap *cap)
{
	struct gc550_dev *dev = video_drvdata(file);

	if (!dev->subdev_mode || !dev->sd_adv7604)
		return -ENODATA;

	cap->pad = ADV76XX_PAD_HDMI_PORT_A;
	return v4l2_subdev_call(dev->sd_adv7604, pad, dv_timings_cap, cap);
}

static int gc550_vidioc_g_parm(struct file *file, void *priv,
			       struct v4l2_streamparm *sp)
{
	if (sp->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	sp->parm.capture.capability = V4L2_CAP_TIMEPERFRAME;
	sp->parm.capture.readbuffers = 2;
	sp->parm.capture.timeperframe.numerator = 1;
	sp->parm.capture.timeperframe.denominator = 60;
	return 0;
}

static int gc550_vidioc_s_parm(struct file *file, void *priv,
			       struct v4l2_streamparm *sp)
{
	return gc550_vidioc_g_parm(file, priv, sp);
}

static int gc550_vidioc_log_status(struct file *file, void *priv)
{
	struct gc550_dev *dev = video_drvdata(file);

	v4l2_device_call_all(&dev->v4l2_dev, 0, core, log_status);
	return 0;
}

static const struct v4l2_ioctl_ops gc550_ioctl_ops = {
	.vidioc_querycap            = gc550_vidioc_querycap,
	.vidioc_enum_fmt_vid_cap    = gc550_vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap       = gc550_vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap     = gc550_vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap       = gc550_vidioc_s_fmt_vid_cap,
	.vidioc_enum_framesizes     = gc550_vidioc_enum_framesizes,
	.vidioc_enum_frameintervals = gc550_vidioc_enum_frameintervals,
	.vidioc_enum_input          = gc550_vidioc_enum_input,
	.vidioc_g_input             = gc550_vidioc_g_input,
	.vidioc_s_input             = gc550_vidioc_s_input,
	.vidioc_g_dv_timings        = gc550_vidioc_g_dv_timings,
	.vidioc_s_dv_timings        = gc550_vidioc_s_dv_timings,
	.vidioc_query_dv_timings    = gc550_vidioc_query_dv_timings,
	.vidioc_enum_dv_timings     = gc550_vidioc_enum_dv_timings,
	.vidioc_dv_timings_cap      = gc550_vidioc_dv_timings_cap,
	.vidioc_g_parm              = gc550_vidioc_g_parm,
	.vidioc_s_parm              = gc550_vidioc_s_parm,
	.vidioc_log_status          = gc550_vidioc_log_status,

	.vidioc_reqbufs             = vb2_ioctl_reqbufs,
	.vidioc_create_bufs         = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf         = vb2_ioctl_prepare_buf,
	.vidioc_querybuf            = vb2_ioctl_querybuf,
	.vidioc_qbuf                = vb2_ioctl_qbuf,
	.vidioc_dqbuf               = vb2_ioctl_dqbuf,
	.vidioc_expbuf              = vb2_ioctl_expbuf,
	.vidioc_streamon            = vb2_ioctl_streamon,
	.vidioc_streamoff           = vb2_ioctl_streamoff,
};

static const struct v4l2_file_operations gc550_fops = {
	.owner          = THIS_MODULE,
	.open           = v4l2_fh_open,
	.release        = vb2_fop_release,
	.read           = vb2_fop_read,
	.poll           = vb2_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap           = vb2_fop_mmap,
};

static void gc550_subdevs_unregister(struct gc550_dev *dev)
{
	if (dev->sd_adv7511) {
		v4l2_device_unregister_subdev(dev->sd_adv7511);
		dev->sd_adv7511 = NULL;
	}
	if (dev->sd_adv7604) {
		v4l2_device_unregister_subdev(dev->sd_adv7604);
		dev->sd_adv7604 = NULL;
	}
}

static int gc550_subdevs_register(struct gc550_dev *dev)
{
	int ret;

	if (subdev_mode == 0) {
		dev->subdev_mode = false;
		return 0;
	}

	if (subdev_mode != 1) {
		dev_err(&dev->intf->dev,
			"Invalid subdev_mode=%d (expected 0 or 1)\n",
			subdev_mode);
		return -EINVAL;
	}

	if (!dev->i2c_registered || !dev->v4l2_registered)
		return -EINVAL;

	dev->subdev_mode = true;

	/*
	 * Run the hardware bootstrap before creating either I2C subdevice
	 * client so PCA9539A releases peripheral reset on cold boot and
	 * ADV7604 ESDP/DPP are translated to 0x2B/0x3B throughout replay.
	 */
	ret = gc550_bootstrap_hw(dev);
	if (ret < 0 || !dev->is_bootstrapped) {
		dev_err(&dev->intf->dev,
			"Pre-subdevice hardware bootstrap failed: %d\n",
			ret < 0 ? ret : -EIO);
		dev->is_bootstrapped = false;
		return ret < 0 ? ret : -EIO;
	}

	/*
	 * Upstream probe callbacks modify physical device state:
	 * adv76xx_probe() reinitializes the pixel-bus format via
	 * adv76xx_core_init() (IO[0x03] = 0x00, 8-bit SDR 4:2:2) and
	 * adv7511_probe() powers down the HDMI transmitter (Main[0x41] |= 0x40).
	 * dev->is_bootstrapped remains true to record that hardware bootstrap
	 * completed and prevent any subsequent replay of gc550_init_blob.
	 * The ADV7604 pixel-bus format is reconfigured immediately after probe.
	 */

	dev->sd_adv7604 = v4l2_i2c_new_subdev_board(&dev->v4l2_dev,
						    &dev->i2c_adap,
						    &gc550_adv7604_board_info,
						    NULL);
	if (!dev->sd_adv7604) {
		dev_err(&dev->intf->dev,
			"Failed to register adv7604 subdevice on I2C 0x%02x\n",
			gc550_adv7604_board_info.addr);
		return -ENODEV;
	}

	dev->sd_adv7511 = v4l2_i2c_new_subdev_board(&dev->v4l2_dev,
						    &dev->i2c_adap,
						    &gc550_adv7511_board_info,
						    NULL);
	if (!dev->sd_adv7511) {
		dev_err(&dev->intf->dev,
			"Failed to register adv7511-v4l2 subdevice on I2C 0x%02x\n",
			gc550_adv7511_board_info.addr);
		gc550_subdevs_unregister(dev);
		return -ENODEV;
	}

	dev_info(&dev->intf->dev,
		 "Registered V4L2 subdevices: %s (0x%02x) and %s (0x%02x)\n",
		 dev->sd_adv7604->name, gc550_adv7604_board_info.addr,
		 dev->sd_adv7511->name, gc550_adv7511_board_info.addr);

	ret = gc550_configure_adv7604(dev);
	if (ret < 0) {
		dev_err(&dev->intf->dev,
			"Failed to configure adv7604 post-probe format: %d\n",
			ret);
		gc550_subdevs_unregister(dev);
		return ret;
	}

	return 0;
}

int gc550_video_register(struct gc550_dev *dev)
{
	struct vb2_queue *vq = &dev->vb_queue;
	struct video_device *vdev = &dev->vdev;
	int ret;

	INIT_LIST_HEAD(&dev->buf_list);
	spin_lock_init(&dev->queue_lock);

	/* Initialize default 1080p60 YUYV format */
	dev->pix_fmt.width = GC550_DEFAULT_WIDTH;
	dev->pix_fmt.height = GC550_DEFAULT_HEIGHT;
	dev->pix_fmt.pixelformat = V4L2_PIX_FMT_YUYV;
	dev->pix_fmt.field = V4L2_FIELD_NONE;
	dev->pix_fmt.bytesperline = GC550_DEFAULT_WIDTH * GC550_BYTES_PER_PIXEL;
	dev->pix_fmt.sizeimage = GC550_FRAME_SIZE;
	dev->pix_fmt.colorspace = V4L2_COLORSPACE_REC709;
	dev->pix_fmt.ycbcr_enc = V4L2_YCBCR_ENC_709;
	dev->pix_fmt.quantization = V4L2_QUANTIZATION_LIM_RANGE;
	dev->pix_fmt.xfer_func = V4L2_XFER_FUNC_709;

	/* Initialize Media Device before registering v4l2_dev and subdevices */
	media_device_usb_init(&dev->mdev, dev->udev, "AVerMedia Live Gamer Extreme (GC550)");
	dev->v4l2_dev.mdev = &dev->mdev;

	dev->v4l2_dev.release = gc550_v4l2_release;
	ret = v4l2_device_register(&dev->intf->dev, &dev->v4l2_dev);
	if (ret < 0) {
		media_device_cleanup(&dev->mdev);
		dev->v4l2_dev.mdev = NULL;
		return ret;
	}
	kref_get(&dev->ref);
	dev->v4l2_registered = true;
	dev->v4l2_ref_held = true;

	ret = gc550_subdevs_register(dev);
	if (ret < 0) {
		gc550_video_unregister(dev);
		return ret;
	}

	vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	vq->drv_priv = dev;
	vq->buf_struct_size = sizeof(struct gc550_buffer);
	vq->ops = &gc550_vb2_ops;
	vq->mem_ops = &vb2_vmalloc_memops;
	vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vq->min_queued_buffers = 2;
	vq->lock = &dev->v4l2_lock;

	ret = vb2_queue_init(vq);
	if (ret < 0) {
		gc550_video_unregister(dev);
		return ret;
	}

	strscpy(vdev->name, "AVerMedia GC550", sizeof(vdev->name));
	vdev->v4l2_dev = &dev->v4l2_dev;
	vdev->fops = &gc550_fops;
	vdev->ioctl_ops = &gc550_ioctl_ops;
	vdev->release = video_device_release_empty;
	vdev->vfl_dir = VFL_DIR_RX;
	vdev->queue = vq;
	vdev->lock = &dev->v4l2_lock;
	vdev->device_caps = V4L2_CAP_VIDEO_CAPTURE |
			    V4L2_CAP_STREAMING |
			    V4L2_CAP_READWRITE;
	video_set_drvdata(vdev, dev);

	/* Initialize Media Entity Sink Pad on Video Node */
	vdev->entity.function = MEDIA_ENT_F_IO_V4L;
	dev->pad.flags = MEDIA_PAD_FL_SINK;
	ret = media_entity_pads_init(&vdev->entity, 1, &dev->pad);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to init media entity pad: %d\n", ret);
		gc550_video_unregister(dev);
		return ret;
	}
	dev->vdev_pads_inited = true;

	ret = video_register_device(vdev, VFL_TYPE_VIDEO, -1);
	if (ret < 0) {
		gc550_video_unregister(dev);
		return ret;
	}

	/* Establish Immutable Pad Link from ADV7604 Source Pad 6 to Video Node Sink Pad 0 */
	if (dev->subdev_mode && dev->sd_adv7604) {
		ret = media_create_pad_link(&dev->sd_adv7604->entity,
					    ADV7604_PAD_SOURCE,
					    &vdev->entity,
					    0,
					    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret < 0) {
			dev_err(&dev->intf->dev,
				"Failed to create media link from ADV7604 to video node: %d\n",
				ret);
			gc550_video_unregister(dev);
			return ret;
		}
	}

	/* Register Media Device to Expose /dev/mediaX */
	ret = media_device_register(&dev->mdev);
	if (ret < 0) {
		dev_err(&dev->intf->dev, "Failed to register media device: %d\n", ret);
		gc550_video_unregister(dev);
		return ret;
	}
	dev->mdev_registered = true;

	return 0;
}

void gc550_video_unregister(struct gc550_dev *dev)
{
	if (dev->mdev_registered) {
		media_device_unregister(&dev->mdev);
		dev->mdev_registered = false;
	}
	if (video_is_registered(&dev->vdev))
		video_unregister_device(&dev->vdev);
	if (dev->vdev_pads_inited) {
		media_entity_cleanup(&dev->vdev.entity);
		dev->vdev_pads_inited = false;
	}
	gc550_subdevs_unregister(dev);
	media_device_cleanup(&dev->mdev);
	dev->v4l2_dev.mdev = NULL;
	if (dev->v4l2_registered) {
		v4l2_device_unregister(&dev->v4l2_dev);
		dev->v4l2_registered = false;
	}
	if (dev->v4l2_ref_held) {
		dev->v4l2_ref_held = false;
		v4l2_device_put(&dev->v4l2_dev);
	}
}
