// SPDX-License-Identifier: GPL-2.0
/*
 * Renesas RIIC driver
 *
 * Copyright (C) 2013 Wolfram Sang <wsa@sang-engineering.com>
 * Copyright (C) 2013 Renesas Solutions Corp.
 */

/*
 * This i2c core has a lot of interrupts, namely 8. We use their chaining as
 * some kind of state machine.
 *
 * 1) The main xfer routine kicks off a transmission by putting the start bit
 * (or repeated start) on the bus and enabling the transmit interrupt (TIE)
 * since we need to send the slave address + RW bit in every case.
 *
 * 2) TIE sends slave address + RW bit and selects how to continue.
 *
 * 3a) Write case: We keep utilizing TIE as long as we have data to send. If we
 * are done, we switch over to the transmission done interrupt (TEIE) and mark
 * the message as completed (includes sending STOP) there.
 *
 * 3b) Read case: We switch over to receive interrupt (RIE). One dummy read is
 * needed to start clocking, then we keep receiving until we are done. Note
 * that we use the RDRFS mode all the time, i.e. we ACK/NACK every byte by
 * writing to the ACKBT bit. I tried using the RDRFS mode only at the end of a
 * message to create the final NACK as sketched in the datasheet. This caused
 * some subtle races (when byte n was processed and byte n+1 was already
 * waiting), though, and I started with the safe approach.
 *
 * 4) If we got a NACK somewhere, we flag the error and stop the transmission
 * via NAKIE.
 *
 * Also check the comments in the interrupt routines for some gory details.
 */

#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#define ICFER_FMPE	0x80
#define ICFER_SCLE	0x40
#define ICFER_NFE	0x20

#define ICCR1_ICE	0x80
#define ICCR1_IICRST	0x40
#define ICCR1_SOWP	0x10
#define ICCR1_SDAO	0x04
#define ICCR1_SDAI	0x01

#define ICCR2_BBSY	0x80
#define ICCR2_SP	0x08
#define ICCR2_RS	0x04
#define ICCR2_ST	0x02

#define ICMR1_CKS_MASK	0x70
#define ICMR1_BCWP	0x08
#define ICMR1_CKS(_x)	((((_x) << 4) & ICMR1_CKS_MASK) | ICMR1_BCWP)

#define ICMR3_RDRFS	0x20
#define ICMR3_ACKWP	0x10
#define ICMR3_ACKBT	0x08

#define ICIER_TIE	0x80
#define ICIER_TEIE	0x40
#define ICIER_RIE	0x20
#define ICIER_NAKIE	0x10
#define ICIER_SPIE	0x08

#define ICSR2_TDRE	0x80
#define ICSR2_TEND	0x40
#define ICSR2_RDRF	0x20
#define ICSR2_NACKF	0x10
#define ICSR2_STAT	0x02

#define ICBR_RESERVED	0xe0 /* Should be 1 on writes */

#define RIIC_INIT_MSG	-1

struct riic_regs {
	u8 iccr1;
	u8 iccr2;
	u8 icmr1;
	u8 icmr3;
	u8 icfer;
	u8 icser;
	u8 icier;
	u8 icsr2;
	u8 icbrl;
	u8 icbrh;
	u8 icdrt;
	u8 icdrr;
};

struct riic_platform_info {
	unsigned int max_speed;
	const struct riic_regs *regs;
};

struct riic_dev {
	void __iomem *base;
	u8 *buf;
	struct i2c_msg *msg;
	int bytes_left;
	int err;
	int is_last;
	struct completion msg_done;
	struct i2c_adapter adapter;
	struct clk *clk;
	struct reset_control *rstc;

	struct riic_platform_info *info;
};

struct riic_irq_desc {
	int res_num;
	irq_handler_t isr;
	char *name;
};

static inline void riic_clear_set_bit(struct riic_dev *riic, u8 clear, u8 set, u8 reg)
{
	writeb((readb(riic->base + reg) & ~clear) | set, riic->base + reg);
}

static int riic_xfer_atomic(struct i2c_adapter *adap, struct i2c_msg msgs[],
			    int num)
{
	struct riic_dev *riic = i2c_get_adapdata(adap);
	unsigned long time_left;
	int i;
	u8 start_bit, val;
	int ret;

	pm_runtime_get_sync(adap->dev.parent);

	if (readb(riic->base + riic->info->regs->iccr2) & ICCR2_BBSY) {
		riic->err = -EBUSY;
		goto out;
	}

	riic->err = 0;

	writeb(0, riic->base + riic->info->regs->icsr2);

	for (i = 0, start_bit = ICCR2_ST; i < num; i++) {
		riic->bytes_left = RIIC_INIT_MSG;
		riic->buf = msgs[i].buf;
		riic->msg = &msgs[i];
		riic->is_last = (i == num - 1);

		writeb(start_bit, riic->base + riic->info->regs->iccr2);

		/*
		 * Before setting slave address to ICDRT:
		 * - STAT and TDRE should be raised
		 * - SDAO and SDAI should be at low level.
		 */
		ret = readb_poll_timeout_atomic(riic->base + riic->info->regs->icsr2,
						val, (val & ICSR2_TDRE) && (val & ICSR2_TDRE), 10, 1000);
		ret |= readb_poll_timeout_atomic(riic->base + riic->info->regs->iccr1,
						val, !(val & (ICCR1_SDAO | ICCR1_SDAI)), 10, 1000);
		if (ret) {
			riic->err = -ETIMEDOUT;
			break;
		}

		/* Write data to I2C Bus Transmit Data Register */
		val = i2c_8bit_addr_from_msg(riic->msg);
		writeb(val, riic->base + riic->info->regs->icdrt);

		if (riic->msg->flags & I2C_M_RD) {
			/* On read */
			ret = readb_poll_timeout_atomic(riic->base + riic->info->regs->icsr2,
							val, val & ICSR2_RDRF, 10, 1000);
			if (ret) {
				riic->err = -ETIMEDOUT;
				break;
			}

			val = readb(riic->base + riic->info->regs->icdrr);	/* dummy read */
			riic->bytes_left = riic->msg->len;

			while (riic->bytes_left) {
				ret = readb_poll_timeout_atomic(riic->base + riic->info->regs->icsr2,
								val, val & ICSR2_RDRF, 10, 1000);
				if (ret) {
					riic->err = -ETIMEDOUT;
					break;
				}

				if (riic->bytes_left == 1) {
					if (riic->is_last) {
						 /* STOP must come before we set ACKBT! */
						writeb(ICCR2_SP, riic->base + riic->info->regs->iccr2);
					}
					riic_clear_set_bit(riic, 0, ICMR3_ACKBT, riic->info->regs->icmr3);
				} else
					riic_clear_set_bit(riic, ICMR3_ACKBT, 0, riic->info->regs->icmr3);

				*riic->buf = readb(riic->base + riic->info->regs->icdrr);
				riic->bytes_left--;
				riic->buf++;
			}

			break;
		} else {
			/* On write, initialize length */
			riic->bytes_left = riic->msg->len;

			while (riic->bytes_left) {
				ret = readb_poll_timeout_atomic(riic->base + riic->info->regs->icsr2,
								val, val & ICSR2_TDRE, 10, 1000);
				if (ret) {
					riic->err = -ETIMEDOUT;
					break;
				}

				val = *riic->buf;
				riic->buf++;
				riic->bytes_left--;
				writeb(val, riic->base + riic->info->regs->icdrt);
			}


			ret = readb_poll_timeout_atomic(riic->base + riic->info->regs->icsr2,
							val, val & ICSR2_TEND, 10, 1000);
			if (ret) {
				riic->err = -ETIMEDOUT;
				break;
			}

			if (riic->is_last || riic->err)
				writeb(ICCR2_SP, riic->base + riic->info->regs->iccr2);
		}

		if (riic->err)
			break;

		start_bit = ICCR2_RS;
		writeb(0, riic->base + riic->info->regs->icsr2);
		readb(riic->base + riic->info->regs->icsr2);
	}

	writeb(0, riic->base + riic->info->regs->icsr2);
	readb(riic->base + riic->info->regs->icsr2);

	/* Should check bus state after finishing transfer */
	if (!riic->err) {
		time_left = readb_poll_timeout_atomic(riic->base + riic->info->regs->iccr2,
						      val, !(val & ICCR2_BBSY), 10, 1000);
		if (time_left)
			dev_warn(riic->adapter.dev.parent,
				 "The i2c bus is still busy\n");
	}

out:
	pm_runtime_put(adap->dev.parent);

	return riic->err ?: num;
}

static int riic_xfer(struct i2c_adapter *adap, struct i2c_msg msgs[], int num)
{
	struct riic_dev *riic = i2c_get_adapdata(adap);
	unsigned long time_left;
	int i;
	u8 start_bit, val;

	pm_runtime_get_sync(adap->dev.parent);

	if (readb(riic->base + riic->info->regs->iccr2) & ICCR2_BBSY) {
		riic->err = -EBUSY;
		goto out;
	}

	reinit_completion(&riic->msg_done);
	riic->err = 0;

	writeb(0, riic->base + riic->info->regs->icsr2);

	for (i = 0, start_bit = ICCR2_ST; i < num; i++) {
		riic->bytes_left = RIIC_INIT_MSG;
		riic->buf = msgs[i].buf;
		riic->msg = &msgs[i];
		riic->is_last = (i == num - 1);

		writeb(ICIER_NAKIE | ICIER_TIE, riic->base + riic->info->regs->icier);

		writeb(start_bit, riic->base + riic->info->regs->iccr2);

		time_left = wait_for_completion_timeout(&riic->msg_done, riic->adapter.timeout);
		if (time_left == 0)
			riic->err = -ETIMEDOUT;

		if (riic->err)
			break;

		start_bit = ICCR2_RS;
	}

	/* Should check bus state after finishing transfer */
	if (!riic->err) {
		time_left = readb_relaxed_poll_timeout(riic->base + riic->info->regs->iccr2,
						       val, !(val & ICCR2_BBSY), 10, 100);
		if (time_left)
			dev_warn(riic->adapter.dev.parent,
				 "The i2c bus is still busy\n");
	}

 out:
	pm_runtime_put(adap->dev.parent);

	return riic->err ?: num;
}

static irqreturn_t riic_tdre_isr(int irq, void *data)
{
	struct riic_dev *riic = data;
	u8 val;

	if (!riic->bytes_left)
		return IRQ_NONE;

	if (riic->bytes_left == RIIC_INIT_MSG) {
		if (riic->msg->flags & I2C_M_RD)
			/* On read, switch over to receive interrupt */
			riic_clear_set_bit(riic, ICIER_TIE, ICIER_RIE,
					   riic->info->regs->icier);
		else
			/* On write, initialize length */
			riic->bytes_left = riic->msg->len;

		val = i2c_8bit_addr_from_msg(riic->msg);
	} else {
		val = *riic->buf;
		riic->buf++;
		riic->bytes_left--;
	}

	/*
	 * Switch to transmission ended interrupt when done. Do check here
	 * after bytes_left was initialized to support SMBUS_QUICK (new msg has
	 * 0 length then)
	 */
	if (riic->bytes_left == 0)
		riic_clear_set_bit(riic, ICIER_TIE, ICIER_TEIE,
				   riic->info->regs->icier);

	/*
	 * This acks the TIE interrupt. We get another TIE immediately if our
	 * value could be moved to the shadow shift register right away. So
	 * this must be after updates to ICIER (where we want to disable TIE)!
	 */
	writeb(val, riic->base + riic->info->regs->icdrt);

	return IRQ_HANDLED;
}

static irqreturn_t riic_tend_isr(int irq, void *data)
{
	struct riic_dev *riic = data;

	if (readb(riic->base + riic->info->regs->icsr2) & ICSR2_NACKF) {
		/* We got a NACKIE */
		readb(riic->base + riic->info->regs->icdrr);	/* dummy read */
		riic_clear_set_bit(riic, ICSR2_NACKF, 0, riic->info->regs->icsr2);
		riic->err = -ENXIO;
	} else if (riic->bytes_left) {
		return IRQ_NONE;
	}

	if (riic->is_last || riic->err) {
		riic_clear_set_bit(riic, ICIER_TEIE, ICIER_SPIE, riic->info->regs->icier);
		writeb(ICCR2_SP, riic->base + riic->info->regs->iccr2);
	} else {
		/* Transfer is complete, but do not send STOP */
		riic_clear_set_bit(riic, ICIER_TEIE, 0, riic->info->regs->icier);
		complete(&riic->msg_done);
	}

	return IRQ_HANDLED;
}

static irqreturn_t riic_rdrf_isr(int irq, void *data)
{
	struct riic_dev *riic = data;

	if (!riic->bytes_left)
		return IRQ_NONE;

	if (riic->bytes_left == RIIC_INIT_MSG) {
		riic->bytes_left = riic->msg->len;
		readb(riic->base + riic->info->regs->icdrr);	/* dummy read */
		return IRQ_HANDLED;
	}

	if (riic->bytes_left == 1) {
		/* STOP must come before we set ACKBT! */
		if (riic->is_last) {
			riic_clear_set_bit(riic, 0, ICIER_SPIE, riic->info->regs->icier);
			writeb(ICCR2_SP, riic->base + riic->info->regs->iccr2);
		}

		riic_clear_set_bit(riic, 0, ICMR3_ACKBT, riic->info->regs->icmr3);

	} else {
		riic_clear_set_bit(riic, ICMR3_ACKBT, 0, riic->info->regs->icmr3);
	}

	/* Reading acks the RIE interrupt */
	*riic->buf = readb(riic->base + riic->info->regs->icdrr);
	riic->buf++;
	riic->bytes_left--;

	return IRQ_HANDLED;
}

static irqreturn_t riic_stop_isr(int irq, void *data)
{
	struct riic_dev *riic = data;

	/* read back registers to confirm writes have fully propagated */
	writeb(0, riic->base + riic->info->regs->icsr2);
	readb(riic->base + riic->info->regs->icsr2);
	writeb(0, riic->base + riic->info->regs->icier);
	readb(riic->base + riic->info->regs->icier);

	complete(&riic->msg_done);

	return IRQ_HANDLED;
}

static u32 riic_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm riic_algo = {
	.master_xfer		= riic_xfer,
	.master_xfer_atomic	= riic_xfer_atomic,
	.functionality		= riic_func,
};

static const struct i2c_adapter_quirks riic_quirks = {
	.flags = I2C_AQ_NO_ZERO_LEN,
};

static int riic_init_hw(struct riic_dev *riic, struct i2c_timings *t)
{
	int ret = 0;
	unsigned long rate;
	int total_ticks, cks, brl, brh;

	pm_runtime_get_sync(riic->adapter.dev.parent);

	if (t->bus_freq_hz > riic->info->max_speed) {
		dev_err(riic->adapter.dev.parent,
			"unsupported bus speed (%dHz). %d max\n",
			t->bus_freq_hz, riic->info->max_speed);
		ret = -EINVAL;
		goto out;
	}

	if (t->bus_freq_hz == I2C_MAX_FAST_MODE_PLUS_FREQ)
		riic_clear_set_bit(riic, ICFER_FMPE, ICFER_FMPE,
				   riic->info->regs->icfer);

	rate = clk_get_rate(riic->clk);

	riic_clear_set_bit(riic, 0, ICFER_SCLE | ICFER_NFE,
				riic->info->regs->icfer);
	/*
	 * Assume the default register settings:
	 *  FER.SCLE = 1 (SCL sync circuit enabled, adds 2 or 3 cycles)
	 *  FER.NFE = 1 (noise circuit enabled)
	 *  MR3.NF = 0 (1 cycle of noise filtered out)
	 *
	 * Freq (CKS=000) = (I2CCLK + tr + tf)/ (BRH + 3 + 1) + (BRL + 3 + 1)
	 * Freq (CKS!=000) = (I2CCLK + tr + tf)/ (BRH + 2 + 1) + (BRL + 2 + 1)
	 */

	/*
	 * Determine reference clock rate. We must be able to get the desired
	 * frequency with only 62 clock ticks max (31 high, 31 low).
	 * Aim for a duty of:
	 * - Below 50kHz: 50% LOW, 50% HIGH.
	 * - Above 50kHz: 60% LOW, 40% HIGH
	 */
	total_ticks = DIV_ROUND_UP(rate, t->bus_freq_hz);

	for (cks = 0; cks < 8; cks++) {
		/*
		 * Period of low time (60% or 50%) must be less than BRL + 2 + 1
		 * BRL max register value is 0x1F.
		 */
		brl = ((total_ticks * ((t->bus_freq_hz >= 50000) ? 6: 5)) / 10);
		if (brl <= (0x1F + 3))
			break;

		total_ticks /= 2;
		rate /= 2;
	}

	if (brl > (0x1F + 3)) {
		dev_err(riic->adapter.dev.parent, "invalid speed (%lu). Too slow.\n",
			(unsigned long)t->bus_freq_hz);
		ret = -EINVAL;
		goto out;
	}

	brh = total_ticks - brl;

	/* Remove automatic clock ticks for sync circuit and NF */
	if (cks == 0) {
		brl -= 4;
		brh -= 4;
	} else {
		brl -= 3;
		brh -= 3;
	}

	/*
	 * Remove clock ticks for rise and fall times. Convert ns to clock
	 * ticks.
	 */
	brl -= t->scl_fall_ns / (1000000000 / rate);
	brh -= t->scl_rise_ns / (1000000000 / rate);

	/* Adjust for min register values for when SCLE=1 and NFE=1 */
	if (brl < 1)
		brl = 1;
	if (brh < 1)
		brh = 1;

	pr_debug("i2c-riic: freq=%lu, duty=%d, fall=%lu, rise=%lu, cks=%d, brl=%d, brh=%d\n",
		 rate / total_ticks, ((brl + 3) * 100) / (brl + brh + 6),
		 t->scl_fall_ns / (1000000000 / rate),
		 t->scl_rise_ns / (1000000000 / rate), cks, brl, brh);

	/* Changing the order of accessing IICRST and ICE may break things! */
	writeb(ICCR1_IICRST | ICCR1_SOWP, riic->base + riic->info->regs->iccr1);
	riic_clear_set_bit(riic, 0, ICCR1_ICE, riic->info->regs->iccr1);

	writeb(ICMR1_CKS(cks), riic->base + riic->info->regs->icmr1);
	writeb(brh | ICBR_RESERVED, riic->base + riic->info->regs->icbrh);
	writeb(brl | ICBR_RESERVED, riic->base + riic->info->regs->icbrl);

	writeb(0, riic->base + riic->info->regs->icser);
	writeb(ICMR3_ACKWP | ICMR3_RDRFS, riic->base + riic->info->regs->icmr3);

	riic_clear_set_bit(riic, ICCR1_IICRST, 0, riic->info->regs->iccr1);

out:
	pm_runtime_put(riic->adapter.dev.parent);
	return ret;
}

static struct riic_irq_desc riic_irqs[] = {
	{ .res_num = 0, .isr = riic_tend_isr, .name = "riic-tend" },
	{ .res_num = 1, .isr = riic_rdrf_isr, .name = "riic-rdrf" },
	{ .res_num = 2, .isr = riic_tdre_isr, .name = "riic-tdre" },
	{ .res_num = 3, .isr = riic_stop_isr, .name = "riic-stop" },
	{ .res_num = 5, .isr = riic_tend_isr, .name = "riic-nack" },
};

static void riic_reset_control_assert(void *data)
{
	reset_control_assert(data);
}

static int riic_i2c_probe(struct platform_device *pdev)
{
	struct riic_dev *riic;
	struct i2c_adapter *adap;
	struct resource *res;
	struct i2c_timings i2c_t;
	struct reset_control *rstc;
	int i, ret;
	struct riic_platform_info *info;

	info = (struct riic_platform_info *)of_device_get_match_data(&pdev->dev);

	riic = devm_kzalloc(&pdev->dev, sizeof(*riic), GFP_KERNEL);
	if (!riic)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	riic->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(riic->base))
		return PTR_ERR(riic->base);

	riic->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(riic->clk)) {
		dev_err(&pdev->dev, "missing controller clock");
		return PTR_ERR(riic->clk);
	}

	rstc = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(rstc))
		return dev_err_probe(&pdev->dev, PTR_ERR(rstc),
				     "Error: missing reset ctrl\n");

	ret = reset_control_deassert(rstc);
	if (ret)
		return ret;

	riic->rstc = rstc;

	ret = devm_add_action_or_reset(&pdev->dev, riic_reset_control_assert, rstc);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(riic_irqs); i++) {
		res = platform_get_resource(pdev, IORESOURCE_IRQ, riic_irqs[i].res_num);
		if (!res)
			return -ENODEV;

		ret = devm_request_irq(&pdev->dev, res->start, riic_irqs[i].isr,
					0, riic_irqs[i].name, riic);
		if (ret) {
			dev_err(&pdev->dev, "failed to request irq %s\n", riic_irqs[i].name);
			return ret;
		}
	}

	riic->info = info;
	adap = &riic->adapter;
	i2c_set_adapdata(adap, riic);
	strlcpy(adap->name, "Renesas RIIC adapter", sizeof(adap->name));
	adap->owner = THIS_MODULE;
	adap->algo = &riic_algo;
	adap->dev.parent = &pdev->dev;
	adap->dev.of_node = pdev->dev.of_node;

	adap->quirks = &riic_quirks;

	init_completion(&riic->msg_done);

	i2c_parse_fw_timings(&pdev->dev, &i2c_t, true);

	pm_runtime_enable(&pdev->dev);

	ret = riic_init_hw(riic, &i2c_t);
	if (ret)
		goto out;

	ret = i2c_add_adapter(adap);
	if (ret)
		goto out;

	platform_set_drvdata(pdev, riic);

	dev_info(&pdev->dev, "registered with %dHz bus speed\n",
		 i2c_t.bus_freq_hz);
	return 0;

out:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static int riic_i2c_remove(struct platform_device *pdev)
{
	struct riic_dev *riic = platform_get_drvdata(pdev);

	pm_runtime_get_sync(&pdev->dev);
	writeb(0, riic->base + riic->info->regs->icier);
	pm_runtime_put(&pdev->dev);
	i2c_del_adapter(&riic->adapter);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct riic_regs common_riic_regs = {
	.iccr1 = 0x00,
	.iccr2 = 0x04,
	.icmr1 = 0x08,
	.icmr3 = 0x10,
	.icfer = 0x14,
	.icser = 0x18,
	.icier = 0x1c,
	.icsr2 = 0x24,
	.icbrl = 0x34,
	.icbrh = 0x38,
	.icdrt = 0x3c,
	.icdrr = 0x40,
};

static const struct riic_regs rzg3s_riic_regs = {
	.iccr1 = 0x00,
	.iccr2 = 0x01,
	.icmr1 = 0x02,
	.icmr3 = 0x04,
	.icfer = 0x05,
	.icser = 0x06,
	.icier = 0x07,
	.icsr2 = 0x09,
	.icbrl = 0x10,
	.icbrh = 0x11,
	.icdrt = 0x12,
	.icdrr = 0x13,
};

static const struct riic_platform_info riic_rz_common_plat_data = {
	.max_speed = I2C_MAX_FAST_MODE_PLUS_FREQ,
	.regs = &common_riic_regs,
};

static const struct riic_platform_info riic_r7s72100_plat_data = {
	.max_speed = I2C_MAX_FAST_MODE_FREQ,
	.regs = &common_riic_regs,
};

static const struct riic_platform_info riic_rzg3s_plat_data = {
	.max_speed = I2C_MAX_FAST_MODE_PLUS_FREQ,
	.regs = &rzg3s_riic_regs,
};

static const struct of_device_id riic_i2c_dt_ids[] = {
	{ .compatible = "renesas,riic-r7s9210", .data = &riic_rz_common_plat_data },
	{ .compatible = "renesas,riic-r7s72100", .data = &riic_r7s72100_plat_data },
	{ .compatible = "renesas,riic-rz", .data = &riic_rz_common_plat_data },
	{ .compatible = "renesas,riic-r9a08g045", .data = &riic_rzg3s_plat_data },
	{ /* Sentinel */ },
};

static int __maybe_unused riic_i2c_suspend(struct device *dev)
{
	struct riic_dev *riic = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&riic->adapter);

	if (riic->rstc)
		reset_control_assert(riic->rstc);

	return 0;
}

static int __maybe_unused riic_i2c_resume(struct device *dev)
{
	struct riic_dev *riic = dev_get_drvdata(dev);
	int ret = 0;
	struct i2c_timings i2c_t;

	if (riic->rstc) {
		ret = reset_control_deassert(riic->rstc);
		if (ret) {
			dev_err(dev, "Failed to reset controller (error %d)\n", ret);
			return ret;
		}
	}

	i2c_parse_fw_timings(dev, &i2c_t, true);
	ret = riic_init_hw(riic, &i2c_t);
	if (ret)
		return ret;

	i2c_mark_adapter_resumed(&riic->adapter);

	return 0;
}

static const struct dev_pm_ops riic_i2c_pm_ops = {
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(riic_i2c_suspend, riic_i2c_resume)
};

static struct platform_driver riic_i2c_driver = {
	.probe		= riic_i2c_probe,
	.remove		= riic_i2c_remove,
	.driver		= {
		.name	= "i2c-riic",
		.of_match_table = riic_i2c_dt_ids,
		.pm	= &riic_i2c_pm_ops,
	},
};

module_platform_driver(riic_i2c_driver);

MODULE_DESCRIPTION("Renesas RIIC adapter");
MODULE_AUTHOR("Wolfram Sang <wsa@sang-engineering.com>");
MODULE_LICENSE("GPL v2");
MODULE_DEVICE_TABLE(of, riic_i2c_dt_ids);
