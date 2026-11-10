/*
 * GHL-PS4onPS3 - use a Guitar Hero Live PS4 dongle (1430:07bb) with the PS3
 * game (BLUS31556, update EBOOT), from inside the game process.
 *
 * The game drives the PS3 dongle (12ba:074b) itself through libusbd: it
 * registers an extra LDD ("GH_Guitar") for vendor 0x12BA, products
 * 0x0100-0x074B. Its probe and attach never check the vendor or product id,
 * only that the device is HID with one IN and one OUT interrupt endpoint -
 * which the PS4 dongle also is. So:
 *
 *  1. register the game's own probe/attach/detach for 1430:07bb as well;
 *  2. on that device, rewrite the game's SET_REPORT "poke" into the PS4 one
 *     and repeat it every 8 s, since the PS4 dongle times out sooner;
 *  3. convert each 64-byte PS4 input report into the 27-byte PS3 layout the
 *     game parses before handing it back;
 *  4. fix what the game forwards to the XMB: it feeds its virtual pad with its
 *     own menu auto-repeat pulses for up/down, and the XMB repeats on top of
 *     that, so one press skipped several items. Those two bits are replaced
 *     with the guitar's real d-pad and strum state.
 *
 * Loaded into the running game with PS3MAPI (webMAN MOD); replug the dongle
 * afterwards so it attaches to the new LDD.
 */

#include <stdint.h>
#include <string.h>
#include <sys/ppu_thread.h>
#include <sys/prx.h>
#include <sys/timer.h>
#include <sys/synchronization.h>
#include <sys/syscall.h>
#include <cell/cell_fs.h>
#include <cell/usbd.h>
#include <cell/pad.h>

SYS_MODULE_INFO(GHL_PS4onPS3, 0, 1, 0);
SYS_MODULE_START(ghl_start);
SYS_MODULE_STOP(ghl_stop);

#define LOG_FILE        "/dev_hdd0/tmp/ghl_ps4onps3.txt"
#define GAME_OPS_ADDR   0x0165CA40u     /* BLUS31556 update EBOOT */
#define PS4_VID         0x1430
#define PS4_PID         0x07BB
#define POKE_PERIOD_MS  8000
#define MAX_SLOTS       8

/* ------------------------------------------------------------------------ */
/* Log                                                                       */
/* ------------------------------------------------------------------------ */

static void log_write(const char *s, int n)
{
	int fd;
	uint64_t w;

	if (cellFsOpen(LOG_FILE, CELL_FS_O_WRONLY | CELL_FS_O_CREAT |
	               CELL_FS_O_APPEND, &fd, NULL, 0) == CELL_FS_SUCCEEDED) {
		cellFsWrite(fd, s, (uint64_t)n, &w);
		cellFsClose(fd);
	}
}

static void log_hex(const char *tag, uint32_t v)
{
	static const char hx[] = "0123456789ABCDEF";
	char line[80];
	int i = 0, k;

	while (*tag && i < 60)
		line[i++] = *tag++;
	line[i++] = '=';
	for (k = 28; k >= 0; k -= 4)
		line[i++] = hx[(v >> k) & 0xF];
	line[i++] = '\n';
	log_write(line, i);
}

static void log_msg(const char *s)
{
	char line[80];
	int i = 0;

	while (*s && i < 78)
		line[i++] = *s++;
	line[i++] = '\n';
	log_write(line, i);
}

/* ------------------------------------------------------------------------ */
/* OPD detours (same technique as ds45pad's hook.c)                          */
/* ------------------------------------------------------------------------ */

typedef struct {
	uint32_t *opd;
	uint32_t  saved[2];
} detour_t;

static int detour(void *target, void *replacement, detour_t *d)
{
	uint32_t *opd = (uint32_t *)target, *rep = (uint32_t *)replacement;

	if (!opd || !rep)
		return -1;
	d->opd = opd;
	d->saved[0] = opd[0];
	d->saved[1] = opd[1];
	if ((((uintptr_t)opd | (uintptr_t)rep) & 7) == 0) {
		*(volatile uint64_t *)opd = *(const uint64_t *)rep;
	} else {
		opd[0] = rep[0];
		opd[1] = rep[1];
	}
	__asm__ volatile ("sync" ::: "memory");
	return 0;
}

static void undetour(detour_t *d)
{
	if (!d->opd)
		return;
	if (((uintptr_t)d->opd & 7) == 0)
		*(volatile uint64_t *)d->opd =
			((uint64_t)d->saved[0] << 32) | d->saved[1];
	else {
		d->opd[0] = d->saved[0];
		d->opd[1] = d->saved[1];
	}
	__asm__ volatile ("sync" ::: "memory");
	d->opd = NULL;
}

static detour_t d_open, d_ctrl, d_intr, d_ldd;

typedef int32_t (*open_t)(int32_t, UsbEndpointDescriptor *);
typedef int32_t (*ctrl_t)(int32_t, UsbDeviceRequest *, void *,
                          CellUsbdDoneCallback, void *);
typedef int32_t (*intr_t)(int32_t, void *, int32_t, CellUsbdDoneCallback,
                          void *);

#define ORIG_OPEN ((open_t)(uintptr_t)d_open.saved)
#define ORIG_CTRL ((ctrl_t)(uintptr_t)d_ctrl.saved)
#define ORIG_INTR ((intr_t)(uintptr_t)d_intr.saved)

typedef int32_t (*ldd_insert_t)(int32_t, CellPadData *);
#define ORIG_LDD ((ldd_insert_t)(uintptr_t)d_ldd.saved)

/* ------------------------------------------------------------------------ */
/* State                                                                     */
/* ------------------------------------------------------------------------ */

static volatile int running;
static sys_ppu_thread_t thread_id = (sys_ppu_thread_t)-1;
static sys_lwmutex_t lock;

static volatile int32_t ps4_dev = -1;
static volatile int32_t ps4_ctrl_pipe = -1;
static volatile int32_t ps4_in_pipe = -1;
static volatile int log_reports = 3;

typedef struct {
	int used;
	uint8_t *buf;
	CellUsbdDoneCallback cb;
	void *arg;
} slot_t;
static slot_t slots[MAX_SLOTS];

static uint8_t last_ps3[27];

/* Diagnostics: bytes that change outside the known ones (whammy, Pause...),
 * queued by the USB callback and written to the log by the plugin thread -
 * file I/O in the callback would stall the input path. */
#define UNK_RING 64
#ifdef GHL_DEBUG
static uint8_t raw_prev[64];
static volatile uint32_t unk_head, unk_tail;
static uint16_t unk_ring[UNK_RING];
static int unk_logged;

static void scan_unknown(const uint8_t *r, int n)
{
	int i;

	if (n > 64)
		n = 64;
	for (i = 1; i < n; i++) {
		uint8_t mask = 0xFF, a, b;

		switch (i) {
		case 2: case 3: case 5: case 19: case 21: case 22: case 24:
			mask = 0;
			break;
		case 6:
			mask = (uint8_t)~(0x01 | 0x02 | 0x40 | 0x80);
			break;
		case 7:
			mask = 0x02;            /* skip PS bit and the counter */
			break;
		}
		a = r[i] & mask;
		b = raw_prev[i] & mask;
		if (a != b && unk_head - unk_tail < UNK_RING) {
			unk_ring[unk_head % UNK_RING] = (uint16_t)((i << 8) | r[i]);
			unk_head++;
		}
		raw_prev[i] = r[i];
	}
}
#endif

static const uint8_t ps4_poke[9] = {
	0x30, 0x02, 0x08, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00
};
static uint8_t poke_buf[16] __attribute__((aligned(16)));
static UsbDeviceRequest poke_req;

static int is_ps4_dongle(int32_t dev_id)
{
	const uint8_t *d = (const uint8_t *)
		cellUsbdScanStaticDescriptor(dev_id, NULL, USB_DESCRIPTOR_TYPE_DEVICE);

	/* Raw descriptor: idVendor/idProduct are little endian at 8..11. */
	return d && d[8] == (PS4_VID & 0xFF) && d[9] == (PS4_VID >> 8) &&
	       d[10] == (PS4_PID & 0xFF) && d[11] == (PS4_PID >> 8);
}

/* ------------------------------------------------------------------------ */
/* PS4 report -> PS3 report                                                  */
/*                                                                           */
/* PS4 (with report id 1 at [0]): [2] strum 00 up/FF down, [3]=[19] tilt,     */
/* [5] low nibble hat, [5] 0x10 W1 0x20 B1 0x40 B2 0x80 B3, [6] 0x01 W2       */
/* 0x02 W3 0x40 GHTV 0x80 Hero Power, [7] 0x01 PS button.                     */
/* [6] 0x20 Pause; [4] whammy. Layouts match TheNathannator/PlasticBand.    */
/* PS3: layout from RPCS3's GHLtar emulation.                                 */
/* ------------------------------------------------------------------------ */

static void ps4_to_ps3(const uint8_t *r, uint8_t *o)
{
	uint8_t b5 = r[5], b6 = r[6], b7 = r[7];

	memset(o, 0, 27);
	o[0] = (b5 & 0x10 ? 0x01 : 0) | (b5 & 0x20 ? 0x02 : 0) |
	       (b5 & 0x40 ? 0x04 : 0) | (b5 & 0x80 ? 0x08 : 0) |
	       (b6 & 0x01 ? 0x10 : 0) | (b6 & 0x02 ? 0x20 : 0);
	o[1] = (b6 & 0x80 ? 0x01 : 0) | (b6 & 0x20 ? 0x02 : 0) |
	       (b6 & 0x40 ? 0x04 : 0) | (b7 & 0x01 ? 0x10 : 0);
	/* Hat: PS4 is neutral at 8, the PS3 dongle at 15 (0x0F). Passing 8 through
	 * reads as a held direction and keeps the XMB scrolling. */
	o[2] = (b5 & 0x0F) >= 8 ? 0x0F : (b5 & 0x0F);
	o[3] = 0x80;
	o[4] = r[2];
	o[5] = 0x80;
	o[6] = r[4];            /* whammy, 0x80..0xFF on both (PlasticBand docs) */
	o[19] = r[19];
	o[22] = 0x01;
	o[24] = 0x02;
	o[26] = 0x02;
	if (o[19] >= 0xF0)
		o[5] = 0xFF;
	else if (o[19] <= 0x10)
		o[5] = 0x00;
}

/* ------------------------------------------------------------------------ */
/* Hooks                                                                     */
/* ------------------------------------------------------------------------ */

static int32_t open_hook(int32_t dev_id, UsbEndpointDescriptor *ed)
{
	int32_t pipe = ORIG_OPEN(dev_id, ed);

	if (pipe >= 0 && is_ps4_dongle(dev_id)) {
		ps4_dev = dev_id;
		if (!ed) {
			ps4_ctrl_pipe = pipe;
			log_hex("ctrl_pipe", (uint32_t)pipe);
		} else if (((const uint8_t *)ed)[2] & 0x80) {
			ps4_in_pipe = pipe;
			log_hex("in_pipe", (uint32_t)pipe);
		} else {
			log_hex("out_pipe", (uint32_t)pipe);
		}
	}
	return pipe;
}

static int32_t ctrl_hook(int32_t pipe, UsbDeviceRequest *dr, void *buf,
                         CellUsbdDoneCallback cb, void *arg)
{
	/* The game's SET_REPORT poke for the PS3 dongle: send the PS4 one. */
	if (pipe == ps4_ctrl_pipe && pipe >= 0 && dr &&
	    dr->bmRequestType == 0x21 && dr->bRequest == 0x09) {
		static UsbDeviceRequest req;
		static uint8_t data[16] __attribute__((aligned(16)));

		memcpy(data, ps4_poke, sizeof(ps4_poke));
		req.bmRequestType = 0x21;
		req.bRequest = 0x09;
		req.wValue = 0x0200 | ps4_poke[0];
		req.wIndex = 0;
		req.wLength = sizeof(ps4_poke);
		return ORIG_CTRL(pipe, &req, data, cb, arg);
	}
	return ORIG_CTRL(pipe, dr, buf, cb, arg);
}

static void intr_done(int32_t result, int32_t count, void *arg)
{
	slot_t *s = (slot_t *)arg;
	uint8_t *buf = s->buf;
	CellUsbdDoneCallback cb = s->cb;
	void *cb_arg = s->arg;

	if (result == 0 && count >= 20 && buf[0] == 0x01) {
#ifdef GHL_DEBUG
		if (log_reports > 0) {
			log_reports--;
			log_hex("report_len", (uint32_t)count);
		}
		scan_unknown(buf, count);
#endif
		ps4_to_ps3(buf, last_ps3);
	}
	if (result == 0) {
		memcpy(buf, last_ps3, sizeof(last_ps3));
		count = sizeof(last_ps3);
	}

	sys_lwmutex_lock(&lock, 0);
	s->used = 0;
	sys_lwmutex_unlock(&lock);

	cb(result, count, cb_arg);
}

static int32_t intr_hook(int32_t pipe, void *buf, int32_t len,
                         CellUsbdDoneCallback cb, void *arg)
{
	slot_t *s = NULL;
	int i;

	if (pipe != ps4_in_pipe || pipe < 0 || len < 64)
		return ORIG_INTR(pipe, buf, len, cb, arg);

	sys_lwmutex_lock(&lock, 0);
	for (i = 0; i < MAX_SLOTS; i++)
		if (!slots[i].used) {
			s = &slots[i];
			s->used = 1;
			break;
		}
	sys_lwmutex_unlock(&lock);

	if (!s)
		return ORIG_INTR(pipe, buf, len, cb, arg);

	s->buf = (uint8_t *)buf;
	s->cb = cb;
	s->arg = arg;
	i = ORIG_INTR(pipe, buf, 64, intr_done, s);
	if (i != 0)
		s->used = 0;
	return i;
}

/* The game's virtual pad (what the XMB sees). digital1 is button[2]; the game
 * leaves len at 0, so it is not checked. */
static int32_t ldd_insert_hook(int32_t handle, CellPadData *data)
{
	if (ps4_dev >= 0 && data) {
		uint8_t hat = last_ps3[2], strum = last_ps3[4];
		uint16_t d = data->button[CELL_PAD_BTN_OFFSET_DIGITAL1];

		/* Only up/down carry the repeat pulses; the game sends left/right
		 * as plain held state from other inputs, which its own menus read
		 * back, so those are left alone. */
		d &= (uint16_t)~(CELL_PAD_CTRL_UP | CELL_PAD_CTRL_DOWN);
		if (hat == 7 || hat == 0 || hat == 1 || strum == 0x00)
			d |= CELL_PAD_CTRL_UP;
		if (hat == 3 || hat == 4 || hat == 5 || strum == 0xFF)
			d |= CELL_PAD_CTRL_DOWN;
		data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = d;
	}
	return ORIG_LDD(handle, data);
}

/* ------------------------------------------------------------------------ */
/* LDD: the game's own callbacks, registered for the PS4 dongle              */
/* ------------------------------------------------------------------------ */

typedef int32_t (*ldd_fn)(int32_t);
static CellUsbdLddOps *game_ops;
static CellUsbdLddOps our_ops;
static int ldd_registered;

static int32_t our_probe(int32_t dev)
{
	int32_t r = ((ldd_fn)game_ops->probe)(dev);
	log_hex("probe", (uint32_t)r);
	return r;
}

static int32_t our_attach(int32_t dev)
{
	int32_t r = ((ldd_fn)game_ops->attach)(dev);
	log_hex("attach", (uint32_t)r);
	return r;
}

static int32_t our_detach(int32_t dev)
{
	int32_t r = ((ldd_fn)game_ops->detach)(dev);
	log_hex("detach", (uint32_t)r);
	if (dev == ps4_dev) {
		ps4_dev = ps4_ctrl_pipe = ps4_in_pipe = -1;
		log_reports = 3;
	}
	return r;
}

static void poke_done(int32_t result, int32_t count, void *arg)
{
	(void)count;
	(void)arg;
	if (result != 0)
		log_hex("poke_result", (uint32_t)result);
}

/* ------------------------------------------------------------------------ */
/* Thread and module entry points                                            */
/* ------------------------------------------------------------------------ */

static void ghl_thread(uint64_t arg)
{
	sys_lwmutex_attribute_t attr;
	uint32_t since_poke = 0;
	int r;

	(void)arg;
	log_msg("start 1.1.1");

	game_ops = (CellUsbdLddOps *)(uintptr_t)GAME_OPS_ADDR;
	if (!game_ops->name || strcmp(game_ops->name, "GH_Guitar") != 0) {
		log_msg("game_ops_not_found_wrong_eboot");
		sys_ppu_thread_exit(0);
	}

	sys_lwmutex_attribute_initialize(attr);
	sys_lwmutex_create(&lock, &attr);
	memset(last_ps3, 0, sizeof(last_ps3));
	last_ps3[2] = 0x0F;
	last_ps3[3] = last_ps3[4] = last_ps3[5] = last_ps3[6] = last_ps3[19] = 0x80;
	last_ps3[22] = 0x01;
	last_ps3[24] = last_ps3[26] = 0x02;

	detour((void *)cellUsbdOpenPipe, (void *)open_hook, &d_open);
	detour((void *)cellUsbdControlTransfer, (void *)ctrl_hook, &d_ctrl);
	detour((void *)cellUsbdInterruptTransfer, (void *)intr_hook, &d_intr);
	detour((void *)cellPadLddDataInsert, (void *)ldd_insert_hook, &d_ldd);
	log_msg("hooks_installed");

	our_ops.name = "GHL_PS4";
	our_ops.probe = our_probe;
	our_ops.attach = our_attach;
	our_ops.detach = our_detach;
	r = cellUsbdRegisterExtraLdd2(&our_ops, PS4_VID, PS4_PID, PS4_PID);
	log_hex("register_ldd", (uint32_t)r);
	ldd_registered = (r == 0);

	while (running) {
		sys_timer_usleep(250 * 1000);
		since_poke += 250;
#ifdef GHL_DEBUG
		while (unk_tail != unk_head) {
			uint16_t e = unk_ring[unk_tail % UNK_RING];

			unk_tail++;
			if (unk_logged < 300) {
				unk_logged++;
				log_hex("raw_byte_idx_val", e);
			}
		}
#endif
		if (since_poke >= POKE_PERIOD_MS && ps4_ctrl_pipe >= 0) {
			since_poke = 0;
			memcpy(poke_buf, ps4_poke, sizeof(ps4_poke));
			poke_req.bmRequestType = 0x21;
			poke_req.bRequest = 0x09;
			poke_req.wValue = 0x0200 | ps4_poke[0];
			poke_req.wIndex = 0;
			poke_req.wLength = sizeof(ps4_poke);
			ORIG_CTRL(ps4_ctrl_pipe, &poke_req, poke_buf, poke_done, NULL);
		}
	}

	/* Unregistering runs the game's detach, which closes the pipes, so any
	 * transfer still pending completes through intr_done while this module
	 * is still loaded. Only then remove the hooks. */
	log_msg("stopping");
	if (ldd_registered) {
		r = cellUsbdUnregisterExtraLdd(&our_ops);
		log_hex("unregister_ldd", (uint32_t)r);
	}
	sys_timer_usleep(200 * 1000);
	undetour(&d_ldd);
	undetour(&d_intr);
	undetour(&d_ctrl);
	undetour(&d_open);
	sys_timer_usleep(300 * 1000);
	log_msg("stopped");
	sys_ppu_thread_exit(0);
}

static inline void thread_exit(uint64_t v)
{
	system_call_1(41, v);
}

int ghl_start(uint64_t arg)
{
	(void)arg;
	running = 1;
	if (sys_ppu_thread_create(&thread_id, ghl_thread, 0, 1000, 0x4000,
	                          SYS_PPU_THREAD_CREATE_JOINABLE, "ghl_ps4") != 0) {
		running = 0;
		return SYS_PRX_NO_RESIDENT;
	}
	thread_exit(0);
	return SYS_PRX_RESIDENT;
}

static void stop_thread(uint64_t arg)
{
	uint64_t code;

	(void)arg;
	running = 0;
	if (thread_id != (sys_ppu_thread_t)-1)
		sys_ppu_thread_join(thread_id, &code);
	sys_ppu_thread_exit(0);
}

int ghl_stop(void)
{
	sys_ppu_thread_t t;
	uint64_t code;

	sys_ppu_thread_create(&t, stop_thread, 0, 1000, 0x4000,
	                      SYS_PPU_THREAD_CREATE_JOINABLE, "ghl_stop");
	sys_ppu_thread_join(t, &code);
	thread_exit(0);
	return SYS_PRX_STOP_OK;
}
