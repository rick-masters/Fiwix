/*
 * fiwix/drivers/block/floppy.c
 *
 * Copyright 2018, Jordi Sanfeliu. All rights reserved.
 * Distributed under the terms of the Fiwix License.
 */

#include <fiwix/asm.h>
#include <fiwix/floppy.h>
#include <fiwix/ioctl.h>
#include <fiwix/devices.h>
#include <fiwix/part.h>
#include <fiwix/fs.h>
#include <fiwix/buffer.h>
#include <fiwix/sleep.h>
#include <fiwix/timer.h>
#include <fiwix/sched.h>
#include <fiwix/errno.h>
#include <fiwix/pic.h>
#include <fiwix/cmos.h>
#include <fiwix/dma.h>
#include <fiwix/stdio.h>
#include <fiwix/string.h>

#define WAIT_MOTOR_OFF	(3 * HZ)	/* time waiting to turn the motor off */
#define WAIT_FDC	WAIT_MOTOR_OFF

#define INVALID_TRACK	-1

#define DEV_TYPE_SHIFT	2		/* right shift to match with the floppy
					   type when minor > 3 */

static int need_reset = 0;
static int fdc_wait_interrupt = 0;
static int fdc_timeout = 0;
static unsigned char fdc_results[MAX_FDC_RESULTS];
static struct resource floppy_resource = { NULL, NULL };

static struct fddt fdd_type[] = {
/*
 * R (data rate): 0 = 500Kb/s, 2 = 250Kb/s, 3 = 1Mb/s
 * SPEC(IFY) 0xAF: SRT = 6ms, HUT = 240ms (500Kb/s)
 * SPEC(IFY) 0xD7: SRT = 6ms, HUT = 240ms (250Kb/s)
 * SPEC(IFY) 0xDF: SRT = 3ms, HUT = 240ms (500Kb/s)
 * Head Load Time 0x02: HLT = 4ms (500Kb/s), Non-DMA = 0 (DMA enabled)
 *
 *        SIZE    KB   T   S  H  G_RW  G_FM  R  SPEC  HLT   NAME
 *        ---------------------------------------------------------------- */
	{    0,    0,  0,  0, 0, 0x00, 0x00, 0, 0x00, 0x00, NULL           },
	{  720,  360, 40,  9, 2, 0x2A, 0x50, 2, 0xD7, 0x02, "360KB 5.25\"" },
	{ 2400, 1200, 80, 15, 2, 0x2A, 0x50, 0, 0xAF, 0x02, "1.2MB 5.25\"" },
	{ 1440,  720, 80,  9, 2, 0x1B, 0x54, 2, 0xD7, 0x02, "720KB 3.5\""  },
	{ 2880, 1440, 80, 18, 2, 0x1B, 0x54, 0, 0xAF, 0x02, "1.44MB 3.5\"" },
/*	{ 5760, 2880, 80, 36, 2, 0x38, 0x53, 3, 0xDF, 0x02, "2.88MB 3.5\"" },*/
};

/* maximum size of a track for floppy types of 1.44MB */
extern char _fdc_transfer_area[BPS * 2 * 18];

struct fdd_status {
	char type;		/* floppy disk drive type */
	char motor;
	char recalibrated;
	char current_track;
};

static struct fdd_status fdd_status[] = {
	{ 0, 0, 0, INVALID_TRACK },
	{ 0, 0, 0, INVALID_TRACK },
};

static unsigned char current_fdd = 0;
static struct fddt *current_fdd_type;
static unsigned int fdd_sizes[256];

static struct fs_operations fdc_driver_fsop = {
	0,
	0,

	fdc_open,
	fdc_close,
	NULL,			/* read */
	NULL,			/* write */
	fdc_ioctl,
	fdc_lseek,
	NULL,			/* readdir */
	NULL,			/* mmap */
	NULL,			/* select */

	NULL,			/* readlink */
	NULL,			/* followlink */
	NULL,			/* bmap */
	NULL,			/* lockup */
	NULL,			/* rmdir */
	NULL,			/* link */
	NULL,			/* unlink */
	NULL,			/* symlink */
	NULL,			/* mkdir */
	NULL,			/* mknod */
	NULL,			/* truncate */
	NULL,			/* create */
	NULL,			/* rename */

	fdc_read,
	fdc_write,

	NULL,			/* read_inode */
	NULL,			/* write_inode */
	NULL,			/* ialloc */
	NULL,			/* ifree */
	NULL,			/* statfs */
	NULL,			/* read_superblock */
	NULL,			/* remount_fs */
	NULL,			/* write_superblock */
	NULL			/* release_superblock */
};

static struct device floppy_device = {
	"floppy",
	FLOPPY_IRQ,
	FDC_MAJOR,
	{ 0, 0, 0, 0, 0, 0, 0, 0 },
	BLKSIZE_1K,
	&fdd_sizes,
	&fdc_driver_fsop,
};

static int fdc_in(void)
{
	int n;
	unsigned char status;

	if(need_reset) {
		return -1;
	}

	for(n = 0; n < 10000; n++) {
		status = inport_b(FDC_MSR) & (FDC_RQM | FDC_DIO);
		if(status == FDC_RQM) {
			return 0;
		}
		if(status == (FDC_RQM | FDC_DIO)) {
			return inport_b(FDC_DATA);
		}
	}
	need_reset = 1;
	printk("WARNING: %s(): fd%d: timeout on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
	return -1;
}

static void fdc_out(unsigned char value)
{
	int n;
	unsigned char status;

	if(need_reset) {
		return;
	}

	for(n = 0; n < 10000; n++) {
		status = inport_b(FDC_MSR) & (FDC_RQM | FDC_DIO);
		if(status == FDC_RQM) {
			outport_b(FDC_DATA, value);
			return;
		}
	}

	need_reset = 1;
	printk("WARNING: %s(): fd%d: unable to send byte 0x%x on %s.\n", __FUNCTION__, current_fdd, value, floppy_device.name);
}

static void fdc_get_results(void)
{
	int n;

	memset_b(fdc_results, 0, sizeof(fdc_results));
	for(n = 0; n < MAX_FDC_RESULTS; n++) {
		fdc_results[n] = fdc_in();
	}
	return;
}

static int fdc_motor_on(void)
{
	struct callout_req creq;
	int errno;

	if(fdd_status[current_fdd].motor) {
		return 0;
	}

	/* select floppy disk drive and turn on its motor */
	outport_b(FDC_DOR, (FDC_DRIVE0 << current_fdd) | FDC_DMA_ENABLE | FDC_ENABLE | current_fdd);
	fdd_status[current_fdd].motor = 1;
	fdd_status[!current_fdd].motor = 0;

	/* fixed spin-up time of 500ms for 3.5" and 5.25" */
	creq.fn = fdc_timer;
	creq.arg = FDC_TR_MOTOR;
	add_callout(&creq, HZ / 2);
	sleep(&fdc_motor_on, PROC_UNINTERRUPTIBLE);

	errno = 0;

	/* check for a disk change */
	if(inport_b(FDC_DIR) & 0x80) {
		errno = 1;
	}

	return errno;
}

static void do_motor_off(unsigned int fdd)
{
	outport_b(FDC_DOR, FDC_DMA_ENABLE | FDC_ENABLE | fdd);
	fdd_status[fdd].motor = 0;
	fdd_status[0].motor = fdd_status[1].motor = 0;
}

static void fdc_motor_off(void)
{
	struct callout_req creq;

	creq.fn = do_motor_off;
	creq.arg = current_fdd;
	add_callout(&creq, WAIT_FDC);
}

static void fdc_reset(void)
{
	int n;
	struct callout_req creq;

	need_reset = 0;

	fdc_wait_interrupt = FDC_RESET;
	outport_b(FDC_DOR, 0);			/* enter in reset mode */
/*	outport_b(FDC_DOR, FDC_DMA_ENABLE); */
	for(n = 0; n < 1000; n++) {		/* recovery time */
		NOP();
	}
	outport_b(FDC_DOR, FDC_DMA_ENABLE | FDC_ENABLE);

	creq.fn = fdc_timer;
	creq.arg = FDC_TR_DEFAULT;
	add_callout(&creq, WAIT_FDC);
	sleep(&irq_floppy, PROC_UNINTERRUPTIBLE);
	if(fdc_timeout) {
		need_reset = 1;
		printk("WARNING: %s(): fd%d: timeout on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
	}
	del_callout(&creq);

	fdd_status[0].motor = fdd_status[1].motor = 0;
	fdd_status[current_fdd].recalibrated = 0;

	/* assumes drive polling mode is ON (by default) */
	for(n = 0; n < 4; n++) {
		fdc_out(FDC_SENSEI);
		fdc_get_results();
	}

	/* keeps controller informed on the drive about to use */
	fdc_out(FDC_SPECIFY);
	fdc_out(current_fdd_type->spec);
	fdc_out(current_fdd_type->hlt);

	/* set data rate */
	outport_b(FDC_CCR, current_fdd_type->rate);
}

static int fdc_recalibrate(void)
{
	struct callout_req creq;

	if(need_reset) {
		return 1;
	}

	fdc_wait_interrupt = FDC_RECALIBRATE;
	fdc_motor_on();
	fdc_out(FDC_RECALIBRATE);
	fdc_out(current_fdd);

	if(need_reset) {
		return 1;
	}

	creq.fn = fdc_timer;
	creq.arg = FDC_TR_DEFAULT;
	add_callout(&creq, WAIT_FDC);
	sleep(&irq_floppy, PROC_UNINTERRUPTIBLE);
	if(fdc_timeout) {
		need_reset = 1;
		printk("WARNING: %s(): fd%d: timeout on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
		return 1;
	}

	del_callout(&creq);
	fdc_out(FDC_SENSEI);
	fdc_get_results();

	/* PCN must be 0 indicating a successful position to track 0 */
	if((fdc_results[ST0] & (ST0_IC | ST0_SE | ST0_UC | ST0_NR)) != ST0_RECALIBRATE || fdc_results[ST_PCN]) {
		need_reset = 1;
		printk("WARNING: %s(): fd%d: unable to recalibrate on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
		return 1;
	}

	fdd_status[current_fdd].current_track = INVALID_TRACK;
	fdd_status[current_fdd].recalibrated = 1;
	fdc_motor_off();
	return 0;
}

static int fdc_seek(int track, int head)
{
	struct callout_req creq;

	if(need_reset) {
		return 1;
	}

	if(!fdd_status[current_fdd].recalibrated) {
		if(fdc_recalibrate()) {
			return 1;
		}
	}

	if(fdd_status[current_fdd].current_track == track) {
		return 0;
	}

	fdc_wait_interrupt = FDC_SEEK;
	fdc_motor_on();
	fdc_out(FDC_SEEK);
	fdc_out((head << 2) | current_fdd);
	fdc_out(track);

	if(need_reset) {
		return 1;
	}

	creq.fn = fdc_timer;
	creq.arg = FDC_TR_DEFAULT;
	add_callout(&creq, WAIT_FDC);
	sleep(&irq_floppy, PROC_UNINTERRUPTIBLE);
	if(fdc_timeout) {
		need_reset = 1;
		printk("WARNING: %s(): fd%d: timeout on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
		return 1;
	}

	del_callout(&creq);
	fdc_out(FDC_SENSEI);
	fdc_get_results();

	if((fdc_results[ST0] & (ST0_IC | ST0_SE | ST0_UC | ST0_NR)) != ST0_SEEK || fdc_results[ST_PCN] != track) {
		need_reset = 1;
		printk("WARNING: %s(): fd%d: unable to seek on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
		return 1;
	}

	fdc_motor_off();
	fdd_status[current_fdd].current_track = track;
	return 0;
}

static int fdc_get_chip(void)
{
	unsigned char version, fifo, id;

	fdc_out(FDC_VERSION);
	version = fdc_in();
	fdc_out(FDC_LOCK);
	fifo = fdc_in();
	fdc_out(FDC_PARTID);
	id = fdc_in();

	if(version == 0x80) {
		if(fifo == 0x80) {
			printk("(NEC D765/Intel 8272A/compatible)\n");
			return 0;
		}
		if(fifo == 0) {
			printk("(Intel 82072)\n");
			return 0;
		}
	}

	if(version == 0x81) {
		printk("(Very Early Intel 82077/compatible)\n");
		return 0;
	}

	if(version == 0x90) {
		if(fifo == 0x80) {
			printk("(Old Intel 82077, no FIFO)\n");
			return 0;
		}
		if(fifo == 0) {
			if(id == 0x80) {
				printk("(New Intel 82077)\n");
				return 0;
			}
			if(id == 0x41) {
				printk("(Intel 82078)\n");
				return 0;
			}
			if(id == 0x73) {
				printk("(National Semiconductor PC87306)\n");
				return 0;
			}
			printk("(Intel 82078 compatible)\n");
			return 0;
		}
		printk("(NEC 72065B)\n");
		return 0;
	}

	if(version == 0xA0) {
		printk("(SMC FDC37c65C+)\n");
		return 0;
	}
	printk("(unknown controller chip)\n");
	return 1;
}

static int fdc_block2chs(__blk_t block, int blksize, int *cyl, int *head, int *sector)
{
	int spb = blksize / FDC_SECTSIZE;

	*cyl = (block * spb) / (current_fdd_type->spt * current_fdd_type->heads);
	*head = ((block * spb) % (current_fdd_type->spt * current_fdd_type->heads)) / current_fdd_type->spt;
	*sector = (((block * spb) % (current_fdd_type->spt * current_fdd_type->heads)) % current_fdd_type->spt) + 1;

	if(*cyl >= current_fdd_type->tracks || *head >= current_fdd_type->heads || *sector > current_fdd_type->spt) {
		return 1;
	}

	return 0;
}

static void set_current_fdd_type(int minor)
{
	current_fdd = minor & 1;

	/* minors 0 and 1 are directly assigned */
	if(minor < 2) {
		current_fdd_type = &fdd_type[(int)fdd_status[current_fdd].type];
	} else {
		current_fdd_type = &fdd_type[minor >> DEV_TYPE_SHIFT];
	}
}

void irq_floppy(void)
{
	if(!fdc_wait_interrupt) {
		printk("WARNING: %s(): fd%d: unexpected interrupt on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
		need_reset = 1;
	} else {
		fdc_timeout = fdc_wait_interrupt = 0;
		wakeup(&irq_floppy);
	}
}

void fdc_timer(unsigned int reason)
{
	switch(reason) {
		case FDC_TR_DEFAULT:
			fdc_timeout = 1;
			fdc_wait_interrupt = 0;
			wakeup(&irq_floppy);
			break;
		case FDC_TR_MOTOR:
			wakeup(&fdc_motor_on);
			break;
	}
}

int fdc_open(struct inode *i, struct fd *fd_table)
{
	unsigned char minor;

	minor = MINOR(i->rdev);
	if(!TEST_MINOR(floppy_device.minors, minor)) {
		return -ENXIO;
	}

	lock_resource(&floppy_resource);
	set_current_fdd_type(minor);
	unlock_resource(&floppy_resource);

	return 0;
}

int fdc_close(struct inode *i, struct fd *fd_table)
{
	unsigned char minor;

	minor = MINOR(i->rdev);
	if(!TEST_MINOR(floppy_device.minors, minor)) {
		return -ENXIO;
	}

	lock_resource(&floppy_resource);
	set_current_fdd_type(minor);
	unlock_resource(&floppy_resource);

	return 0;
}

int fdc_read(__dev_t dev, __blk_t block, char *buffer, int blksize)
{
	unsigned char minor;
	unsigned int sectors_read;
	int cyl, head, sector;
	int retries;
	struct callout_req creq;
	struct device *d;

	minor = MINOR(dev);
	if(!TEST_MINOR(floppy_device.minors, minor)) {
		return -ENXIO;
	}

	if(!blksize) {
		if(!(d = get_device(BLK_DEV, MAJOR(dev)))) {
			return -EINVAL;
		}
		blksize = d->blksize;
	}
	blksize = blksize ? blksize : BLKSIZE_1K;

	lock_resource(&floppy_resource);
	set_current_fdd_type(minor);

	if(fdc_block2chs(block, blksize, &cyl, &head, &sector)) {
		printk("WARNING: %s(): fd%d: invalid block number %d on %s device %d,%d.\n", __FUNCTION__, current_fdd, block, floppy_device.name, MAJOR(dev), MINOR(dev));
		unlock_resource(&floppy_resource);
		return -EINVAL;
	}

	for(retries = 0; retries < MAX_FDC_ERR; retries++) {
		if(need_reset) {
			fdc_reset();
		}
		if(fdc_motor_on()) {
			printk("%s(): %s disk was changed in device %d,%d!\n", __FUNCTION__, floppy_device.name, MAJOR(dev), MINOR(dev));
			invalidate_buffers(dev);
			fdd_status[current_fdd].recalibrated = 0;
		}

		if(fdc_seek(cyl, head)) {
			printk("WARNING: %s(): fd%d: seek error on %s device %d,%d during read operation.\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
			continue;
		}

		start_dma(FLOPPY_DMA, _fdc_transfer_area, blksize, DMA_MODE_WRITE | DMA_MODE_SINGLE);

		/* send READ command */
		fdc_wait_interrupt = FDC_READ;
		fdc_out(FDC_READ);
		fdc_out((head << 2) | current_fdd);
		fdc_out(cyl);
		fdc_out(head);
		fdc_out(sector);
		fdc_out(2);	/* sector size is 512 bytes */
		fdc_out(current_fdd_type->spt);
		fdc_out(current_fdd_type->gpl1);
		fdc_out(0xFF);	/* sector size is 512 bytes */

		if(need_reset) {
			printk("WARNING: %s(): fd%d: needs reset on %s device %d,%d during read operation.\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
			continue;
		}
		creq.fn = fdc_timer;
		creq.arg = FDC_TR_DEFAULT;
		add_callout(&creq, WAIT_FDC);
		sleep(&irq_floppy, PROC_UNINTERRUPTIBLE);
		if(fdc_timeout) {
			need_reset = 1;
			printk("WARNING: %s(): fd%d: timeout on %s device %d,%d.\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
			continue;
		}
		del_callout(&creq);
		fdc_get_results();
		if(fdc_results[ST0] & (ST0_IC | ST0_UC | ST0_NR)) {
			need_reset = 1;
			continue;
		}
		break;
	}

	if(retries >= MAX_FDC_ERR) {
		printk("WARNING: %s(): fd%d: error on %s device %d,%d during read operation,\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
		printk("\tblock=%d, sector=%d, cylinder/head=%d/%d\n", block, sector, cyl, head);
		unlock_resource(&floppy_resource);
		fdc_motor_off();
		return -EIO;
	}

	fdc_motor_off();
	sectors_read = (fdc_results[ST_CYL] - cyl) * (current_fdd_type->heads * current_fdd_type->spt);
	sectors_read += (fdc_results[ST_HEAD] - head) * current_fdd_type->spt;
	sectors_read += fdc_results[ST_SECTOR] - sector;
	if(sectors_read * BPS != blksize) {
		printk("WARNING: %s(): fd%d: read error on %s device %d,%d (%d sectors read).\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev), sectors_read);
		printk("\tblock=%d, sector=%d, cylinder/head=%d/%d\n", block, sector, cyl, head);
		unlock_resource(&floppy_resource);
		fdc_motor_off();
		return -EIO;
	}

	memcpy_b(buffer, (void *)_fdc_transfer_area, blksize);

	unlock_resource(&floppy_resource);
	return sectors_read * BPS;
}

int fdc_write(__dev_t dev, __blk_t block, char *buffer, int blksize)
{
	unsigned char minor;
	unsigned int sectors_written;
	int cyl, head, sector;
	int retries;
	struct callout_req creq;
	struct device *d;

	minor = MINOR(dev);
	if(!TEST_MINOR(floppy_device.minors, minor)) {
		return -ENXIO;
	}

	if(!blksize) {
		if(!(d = get_device(BLK_DEV, MAJOR(dev)))) {
			return -EINVAL;
		}
		blksize = d->blksize;
	}
	blksize = blksize ? blksize : BLKSIZE_1K;

	lock_resource(&floppy_resource);
	set_current_fdd_type(minor);

	if(fdc_block2chs(block, blksize, &cyl, &head, &sector)) {
		printk("WARNING: %s(): fd%d: invalid block number %d on %s device %d,%d.\n", __FUNCTION__, current_fdd, block, floppy_device.name, MAJOR(dev), MINOR(dev));
		unlock_resource(&floppy_resource);
		return -EINVAL;
	}

	for(retries = 0; retries < MAX_FDC_ERR; retries++) {
		if(need_reset) {
			fdc_reset();
		}
		if(fdc_motor_on()) {
			printk("%s(): %s disk was changed in device %d,%d!\n", __FUNCTION__, floppy_device.name, MAJOR(dev), MINOR(dev));
			invalidate_buffers(dev);
			fdd_status[current_fdd].recalibrated = 0;
		}

		if(fdc_seek(cyl, head)) {
			printk("WARNING: %s(): fd%d: seek error on %s device %d,%d during write operation.\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
			continue;
		}

		start_dma(FLOPPY_DMA, _fdc_transfer_area, blksize, DMA_MODE_READ | DMA_MODE_SINGLE);
		memcpy_b((void *)_fdc_transfer_area, buffer, blksize);

		/* send WRITE command */
		fdc_wait_interrupt = FDC_WRITE;
		fdc_out(FDC_WRITE);
		fdc_out((head << 2) | current_fdd);
		fdc_out(cyl);
		fdc_out(head);
		fdc_out(sector);
		fdc_out(2);	/* sector size is 512 bytes */
		fdc_out(current_fdd_type->spt);
		fdc_out(current_fdd_type->gpl1);
		fdc_out(0xFF);	/* sector size is 512 bytes */

		if(need_reset) {
			printk("WARNING: %s(): fd%d: needs reset on %s device %d,%d during write operation.\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
			continue;
		}
		creq.fn = fdc_timer;
		creq.arg = FDC_TR_DEFAULT;
		add_callout(&creq, WAIT_FDC);
		sleep(&irq_floppy, PROC_UNINTERRUPTIBLE);
		if(fdc_timeout) {
			need_reset = 1;
			printk("WARNING: %s(): fd%d: timeout on %s device %d,%d.\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
			continue;
		}
		del_callout(&creq);
		fdc_get_results();
		if(fdc_results[ST1] & ST1_NW) {
			unlock_resource(&floppy_resource);
			fdc_motor_off();
			return -EROFS;
		}
		if(fdc_results[ST0] & (ST0_IC | ST0_UC | ST0_NR)) {
			need_reset = 1;
			continue;
		}
		break;
	}

	if(retries >= MAX_FDC_ERR) {
		printk("WARNING: %s(): fd%d: error on %s device %d,%d during write operation,\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev));
		printk("\tblock=%d, sector=%d, cylinder/head=%d/%d\n", block, sector, cyl, head);
		unlock_resource(&floppy_resource);
		fdc_motor_off();
		return -EIO;
	}

	fdc_motor_off();
	sectors_written = (fdc_results[ST_CYL] - cyl) * (current_fdd_type->heads * current_fdd_type->spt);
	sectors_written += (fdc_results[ST_HEAD] - head) * current_fdd_type->spt;
	sectors_written += fdc_results[ST_SECTOR] - sector;
	if(sectors_written * BPS != blksize) {
		printk("WARNING: %s(): fd%d: write error on %s device %d,%d (%d sectors written).\n", __FUNCTION__, current_fdd, floppy_device.name, MAJOR(dev), MINOR(dev), sectors_written);
		printk("\tblock=%d, sector=%d, cylinder/head=%d/%d\n", block, sector, cyl, head);
		unlock_resource(&floppy_resource);
		fdc_motor_off();
		return -EIO;
	}

	unlock_resource(&floppy_resource);
	return sectors_written * BPS;
}

int fdc_ioctl(struct inode *i, int cmd, unsigned long int arg)
{
	unsigned char minor;
	struct hd_geometry *geom;
	int errno;

	minor = MINOR(i->rdev);
	if(!TEST_MINOR(floppy_device.minors, minor)) {
		return -ENXIO;
	}

	lock_resource(&floppy_resource);
	set_current_fdd_type(minor);
	unlock_resource(&floppy_resource);

	switch(cmd) {
		case HDIO_GETGEO:
			if((errno = check_user_area(VERIFY_WRITE, (void *)arg, sizeof(struct hd_geometry)))) {
				return errno;
			}
			geom = (struct hd_geometry *)arg;
			geom->heads = current_fdd_type->heads;
			geom->sectors = current_fdd_type->spt;
			geom->cylinders = current_fdd_type->tracks;
			geom->start = 0;
			break;
		case BLKRRPART:
			break;
		case BLKGETSIZE:
			if((errno = check_user_area(VERIFY_WRITE, (void *)arg, sizeof(unsigned int)))) {
				return errno;
			}
			*(int *)arg = fdd_sizes[MINOR(i->rdev)] * 2;
			break;
		default:
			return -EINVAL;
	}
	return 0;
}

int fdc_lseek(struct inode *i, __off_t offset)
{
	unsigned char minor;

	minor = MINOR(i->rdev);
	if(!TEST_MINOR(floppy_device.minors, minor)) {
		return -ENXIO;
	}

	lock_resource(&floppy_resource);
	set_current_fdd_type(minor);
	unlock_resource(&floppy_resource);

	return offset;
}

void floppy_init(void)
{
	short int cmosval, master, slave;

	cmosval = cmos_read(CMOS_FDDTYPE);
	set_current_fdd_type(0);	/* sets /dev/fd0 by default */

	/* the high nibble describes the 'master' floppy drive */
	master = cmosval >> 4;
	if(master) {
		if(!register_irq(FLOPPY_IRQ, floppy_device.name, irq_floppy)) {
			enable_irq(FLOPPY_IRQ);
		}
		printk("fd0       0x%04X-0x%04X   %2d    ", FDC_SRA, FDC_CCR, FLOPPY_IRQ);
		printk("%s ", fdd_type[master].name);
		fdd_status[0].type = fdd_status[1].type = master;
		SET_MINOR(floppy_device.minors, 0);
		SET_MINOR(floppy_device.minors, 4);
		SET_MINOR(floppy_device.minors, 8);
		SET_MINOR(floppy_device.minors, 12);
		SET_MINOR(floppy_device.minors, 16);
		fdd_sizes[0] = fdd_type[master].sizekb;
		fdd_sizes[4] = fdd_type[1].sizekb;
		fdd_sizes[8] = fdd_type[2].sizekb;
		fdd_sizes[12] = fdd_type[3].sizekb;
		fdd_sizes[16] = fdd_type[4].sizekb;
		fdc_reset();
		fdc_get_chip();
	}

	/* the low nibble is for the 'slave' floppy drive */
	slave = cmosval & 0x0F;
	if(slave) {
		if(!master) {
			if(!register_irq(FLOPPY_IRQ, floppy_device.name, irq_floppy)) {
				enable_irq(FLOPPY_IRQ);
			}
		}
		printk("fd1       0x%04X-0x%04X   %2d    ", FDC_SRA, FDC_CCR, FLOPPY_IRQ);
		printk("%s  ", fdd_type[slave].name);
		fdd_status[1].type = slave;
		SET_MINOR(floppy_device.minors, 1);
		SET_MINOR(floppy_device.minors, 5);
		SET_MINOR(floppy_device.minors, 9);
		SET_MINOR(floppy_device.minors, 13);
		SET_MINOR(floppy_device.minors, 17);
		fdd_sizes[1] = fdd_type[slave].sizekb;
		fdd_sizes[5] = fdd_type[1].sizekb;
		fdd_sizes[9] = fdd_type[2].sizekb;
		fdd_sizes[13] = fdd_type[3].sizekb;
		fdd_sizes[17] = fdd_type[4].sizekb;
		if(!master) {
			fdc_get_chip();
		} else {
			printk("\n");
		}
	}

	if(master || slave) {
		need_reset = 1;
		dma_init();
		if(dma_register(FLOPPY_DMA, floppy_device.name)) {
			printk("WARNING: %s(): fd%d: unable to register DMA channel on %s.\n", __FUNCTION__, current_fdd, floppy_device.name);
		} else  {
			if(!register_device(BLK_DEV, &floppy_device)) {
				do_motor_off(current_fdd);
			}
		}
	}
}
