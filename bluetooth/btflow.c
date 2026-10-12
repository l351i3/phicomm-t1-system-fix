/* btflow.c - BCM4345 UART 全流程初始化测试 (T1)
 *
 * 用法: btflow <init_baud> <target_baud> <hcd_path> [flow] [skip_gpio]
 *   init_baud   : 初始速率 (T1 实测 89760)
 *   target_baud : 固件下载/运行速率; == init_baud 时全低速流程(不切速)
 *   hcd_path    : 固件 .hcd 文件路径
 *   flow        : 0=无流控(默认) 1=CRTSCTS
 *   skip_gpio   : 1=不操作 GPIO619 (外部已上电)
 *
 * 流程(仿内核 hci_bcm/btbcm + BlueZ hciattach_bcm43xx):
 *   @init : Reset -> ReadLocalVersion -> ReadLocalName
 *   [切速] : 0xFC18 @init -> host 切 target -> Reset@target 验证
 *   @target: 0xFC2E -> hcd 逐块下载 -> wait 400ms -> Reset -> ReadLocalVersion
 *   探测  : [target, init, 115200] 各发 Reset 记录应答
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/serial.h>
#include <asm/termbits.h>

#ifndef TCSETS2
#define TCGETS2  _IOR('T', 0x2A, struct termios2)
#define TCSETS2  _IOW('T', 0x2B, struct termios2)
#define TCSETSW2 _IOW('T', 0x2C, struct termios2)
#define TCSETSF2 _IOW('T', 0x2D, struct termios2)
#endif

/* HCI UART line discipline 常量 (源自 BlueZ hciattach.h / 内核 hci_uart.h)
 * scene23: 下载完固件后原位挂 N_HCI ldisc + H4 协议, 交内核栈成为 hci0 */
#ifndef N_HCI
#define N_HCI 15
#endif
#ifndef HCIUARTSETPROTO
#define HCIUARTSETPROTO _IOW('U', 200, int)
#endif
#ifndef HCI_UART_H4
#define HCI_UART_H4 0
#endif

#define TTY      "/dev/ttyAML1"
#define GP  "/sys/class/gpio/gpio619"

static long long now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static long long T0;

static double ms(void)
{
	return (now_us() - T0) / 1000.0;
}

static void hd(const char *tag, const unsigned char *b, int n)
{
	int i;
	printf("  [+%9.1fms] %-22s %3d B: ", ms(), tag, n);
	for (i = 0; i < n; i++)
		printf("%02x ", b[i]);
	if (n == 0)
		printf("(无)");
	printf("\n");
	fflush(stdout);
}

static int gpio_val(int v)
{
	int fd = open(GP "/value", O_WRONLY);
	if (fd < 0)
		return -1;
	if (write(fd, v ? "1" : "0", 1) < 0) {}
	close(fd);
	return 0;
}

static int gpio_setup(void)
{
	int fd;
	fd = open("/sys/class/gpio/export", O_WRONLY);
	if (fd >= 0) { if (write(fd, "619", 3) < 0) {} close(fd); }
	usleep(50000);
	fd = open(GP "/direction", O_WRONLY);
	if (fd < 0) {
		fprintf(stderr, "no %s/direction: %s\n", GP, strerror(errno));
		return -1;
	}
	if (write(fd, "out", 3) < 0) {}
	close(fd);
	return 0;
}

/* 读到"首字节后连续 quiet_ms 静默"或 first_wait 超时; 返回字节数 */
static int drain(int fd, unsigned char *buf, int maxlen, int first_wait_ms, int quiet_ms){
	int n = 0;
	long long t0 = now_us(), last = 0;
	while (1) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r;
		long long now;
		if (n == 0) {
			if (now_us() - t0 > (long long)first_wait_ms * 1000)
				break;
		} else {
			if (now_us() - last > (long long)quiet_ms * 1000)
				break;
		}
		r = poll(&p, 1, 20);
		if (r <= 0)
			continue;
		r = read(fd, buf + n, maxlen - n);
		if (r <= 0)
			continue;
		now = now_us();
		if (n == 0)
			printf("  [+%9.1fms]   (首字节延迟 %lldus)\n", ms(), now - t0);
		n += r;
		last = now;
		if (n >= maxlen)
			break;
	}
	return n;
}

/* 超长观察窗: 按"总时长 total_ms"接收, 逐次 read 打时间戳; quiet_ms 静默提前结束 */
static int drain_trace(int fd, unsigned char *buf, int maxlen, int total_ms, int quiet_ms)
{
	int n = 0;
	long long t0 = now_us(), last = 0;
	while (1) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r;
		if ((now_us() - t0) / 1000 > total_ms)
			break;
		if (n > 0 && (now_us() - last) / 1000 > quiet_ms)
			break;
		r = poll(&p, 1, 20);
		if (r <= 0)
			continue;
		r = read(fd, buf + n, maxlen - n);
		if (r <= 0)
			continue;
		printf("  [+%9.1fms] TRACE %d B @窗口+%lldms (累计 %d)\n",
			ms(), r, (now_us() - t0) / 1000, n + r);
		n += r;
		last = now_us();
		if (n >= maxlen)
			break;
	}
	printf("  [TRACE] 窗口 %dms 结束: 共 %d B\n", total_ms, n);
	fflush(stdout);
	return n;
}

static int set_baud(int fd, unsigned int baud, int flow)
{
	struct termios2 t2;
	memset(&t2, 0, sizeof t2);
	t2.c_cflag = CS8 | CREAD | CLOCAL | BOTHER;
	if (flow)
		t2.c_cflag |= CRTSCTS;
	t2.c_ispeed = t2.c_ospeed = baud;
	if (ioctl(fd, TCSETSF2, &t2) < 0) {
		fprintf(stderr, "TCSETSF2 %u failed: %s\n", baud, strerror(errno));
		return -1;
	}
	return 0;
}

/* 读一个字节(带截止时间); 0=成功 */
static int read_byte_to(int fd, unsigned char *c, long long deadline_us)
{
	while (now_us() < deadline_us) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		int r;
		if (poll(&p, 1, 20) <= 0)
			continue;
		r = read(fd, c, 1);
		if (r == 1)
			return 0;
	}
	return -1;
}

/* 按 H4 事件帧精确读取: 找 0x04 头 -> evt+len -> payload
 * 返回事件总字节数; -1=超时; skipped=被跳过的垃圾字节数 */
static int read_event(int fd, unsigned char *buf, int max, int timeout_ms, int *skipped)
{
	long long deadline = now_us() + (long long)timeout_ms * 1000;
	int count = 0, len, i;

	if (read_byte_to(fd, &buf[0], deadline))
		return -1;
	while (buf[0] != 0x04) {
		(*skipped)++;
		if (read_byte_to(fd, &buf[0], deadline))
			return -1;
	}
	count = 1;
	while (count < 3) {
		if (read_byte_to(fd, &buf[count], deadline))
			return -1;
		count++;
	}
	len = buf[2];
	if (len > max - 3)
		len = max - 3;
	for (i = 0; i < len; i++) {
		if (read_byte_to(fd, &buf[3 + i], deadline))
			return -1;
		count++;
	}
	return count;
}

static void clear_rx(int fd)
{
	unsigned char b[512];
	usleep(50000);
	while (read(fd, b, sizeof b) > 0) {}
}

static const char *evdesc(const unsigned char *b, int n, char *out, int outsz)
{
	/* 极简 HCI 事件解析 */
	if (n >= 7 && b[0] == 0x04 && b[1] == 0x0e) {
		int op = b[4] | (b[5] << 8);
		int st = b[6];
		snprintf(out, outsz, "CommandComplete op=0x%04x status=0x%02x", op, st);
	} else if (n >= 4 && b[0] == 0x04 && b[1] == 0x10) {
		snprintf(out, outsz, "HARDWARE-ERROR code=0x%02x", b[3]);
	} else if (n == 0) {
		snprintf(out, outsz, "TIMEOUT/空");
	} else {
		snprintf(out, outsz, "未识别事件流");
	}
	return out;
}

static int step(int fd, const char *label, const unsigned char *cmd, int len,
		int timeout_ms, int quiet, unsigned char *rx, int rxmax)
{
	int n, sk = 0;
	char d[128];
	printf("[%9.1fms] >> %s (TX %d B)\n", ms(), label, len);
	if (write(fd, cmd, len) < 0)
		perror("write");
	n = read_event(fd, rx, rxmax, timeout_ms, &sk);
	hd(label, rx, n > 24 ? 24 : (n < 0 ? 0 : n));
	if (sk)
		printf("             (跳过垃圾 %d 字节)\n", sk);
	printf("             -> %s\n", evdesc(rx, n < 0 ? 0 : n, d, sizeof d));
	fflush(stdout);
	return n;
}

/* 阻塞式写全 (处理 EAGAIN) */
static int write_full(int fd, const void *buf, int len)
{
	const unsigned char *p = buf;
	int off = 0;
	while (off < len) {
		int w = write(fd, p + off, len - off);
		if (w > 0) { off += w; continue; }
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pf = { .fd = fd, .events = POLLOUT };
			poll(&pf, 1, 100);
			continue;
		}
		return -1;
	}
	return 0;
}

/* 发送 hcd 中某个块 (off 指向 op_lo) */
static void send_blk(int fd, const unsigned char *hcd, long off, int len2, int gap_ms)
{
	unsigned char h4buf[1200];
	h4buf[0] = 0x01;
	h4buf[1] = hcd[off];
	h4buf[2] = hcd[off + 1];
	h4buf[3] = (unsigned char)len2;
	memcpy(h4buf + 4, hcd + off + 3, len2);
	if (gap_ms > 0)
		usleep(gap_ms * 1000);
	printf("[%9.1fms] >> 发块 op=0x%04x len=%d (hcd@%ld)\n",
		ms(), hcd[off] | (hcd[off + 1] << 8), len2, off);
	if (write(fd, h4buf, 4 + len2) < 0)
		perror("write");
	fflush(stdout);
}

/* 长观察窗: 打印每次收到的原始字节(带时间戳); 结束后模式统计+dump */
static int obs_sec(int fd, const char *tag, int secs)
{
	unsigned char buf[4096];
	static unsigned char all[262144];
	int alln = 0;
	long long t0 = now_us();
	int total = 0;
	printf("[%9.1fms] --- 观察 [%s] %d 秒 ---\n", ms(), tag, secs);
	while (now_us() - t0 < (long long)secs * 1000000LL) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		if (poll(&p, 1, 20) > 0) {
			int r = read(fd, buf, sizeof buf), k;
			if (r > 0) {
				printf("  [+%9.1fms] RX %d B:", ms(), r);
				for (k = 0; k < r && k < 96; k++)
					printf(" %02x", buf[k]);
				if (r > 96)
					printf(" ...(+%d)", r - 96);
				printf("\n");
				fflush(stdout);
				if (alln + r <= (int)sizeof all) {
					memcpy(all + alln, buf, r);
					alln += r;
				}
				total += r;
			}
		}
	}
	{
		int i2, er = 0, cc = 0;
		for (i2 = 0; i2 + 3 < alln; i2++) {
			if (all[i2] == 4 && all[i2 + 1] == 0x10 && all[i2 + 2] == 1 && all[i2 + 3] == 0) {
				er++;
				i2 += 3;
			} else if (all[i2] == 4 && all[i2 + 1] == 0x0e) {
				cc++;
			}
		}
		printf("[%9.1fms] --- 观察结束 [%s]: 共 %d B (ER≈%d, CC≈%d) ---\n",
			ms(), tag, total, er, cc);
		{
			static int dumpn = 0;
			char path[64];
			FILE *df;
			snprintf(path, sizeof path, "/tmp/obs_dump_%d.bin", dumpn++);
			df = fopen(path, "wb");
			if (df) {
				fwrite(all, 1, alln, df);
				fclose(df);
				printf("  [dump] %s (%d B)\n", path, alln);
			}
		}
	}
	return total;
}

int main(int argc, char **argv)
{
	unsigned int init_baud = 89760, target_baud = 89760;
	const char *hcd_path = NULL;
	int flow = 0, skip_gpio = 0, gap_ms = 0, scene = 0;
	int s7ret = -1, s7ok = 0;
	long boff[1024];
	int blen[1024], nblk = 0;
	int fd, i, n;
	unsigned char rx[4096];
	unsigned char *hcd = NULL;
	long hcd_len = 0;
	FILE *f;
	struct serial_icounter_struct ic0, ic1;

	unsigned char reset[]    = { 0x01, 0x03, 0x0c, 0x00 };
	unsigned char rdver[]    = { 0x01, 0x01, 0x10, 0x00 };
	unsigned char rdname[]   = { 0x01, 0x14, 0x0c, 0x00 };
	unsigned char dlmode[]   = { 0x01, 0x2e, 0xfc, 0x00 };
	unsigned char setspeed[] = { 0x01, 0x18, 0xfc, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (argc < 4) {
		fprintf(stderr, "usage: %s <init_baud> <target_baud> <hcd_path> [flow] [skip_gpio]\n", argv[0]);
		return 1;
	}
	init_baud = (unsigned)strtoul(argv[1], NULL, 10);
	target_baud = (unsigned)strtoul(argv[2], NULL, 10);
	hcd_path = argv[3];
	if (argc > 4) flow = atoi(argv[4]);
	if (argc > 5) skip_gpio = atoi(argv[5]);
	if (argc > 6) gap_ms = atoi(argv[6]);
	if (argc > 7) scene = atoi(argv[7]);

	T0 = now_us();

	/* 读固件文件 */
	f = fopen(hcd_path, "rb");
	if (!f) { perror(hcd_path); return 1; }
	fseek(f, 0, SEEK_END); hcd_len = ftell(f); fseek(f, 0, SEEK_SET);
	hcd = malloc(hcd_len);
	if (fread(hcd, 1, hcd_len, f) != (size_t)hcd_len) { perror("fread"); return 1; }
	fclose(f);
	printf("=== btflow: init=%u target=%u flow=%d scene=%d ===\n", init_baud, target_baud, flow, scene);
	printf("hcd: %s (%ld B), 首 16B: ", hcd_path, hcd_len);
	for (i = 0; i < 16; i++) printf("%02x ", hcd[i]);
	printf("\n");

	/* 块索引 */
	{
		long p = 0;
		while (p + 3 <= hcd_len && nblk < 1024) {
			int l2 = hcd[p + 2];
			if (l2 > 1020 || p + 3 + (long)l2 > hcd_len)
				break;
			boff[nblk] = p;
			blen[nblk] = l2;
			nblk++;
			p += 3 + (long)l2;
		}
		printf("[init] hcd 共解析 %d 块\n", nblk);
	}
	printf("\n");

	/* GPIO 上电 (断电 2s 彻底复位) */
	if (!skip_gpio) {
		const char *e;
		int w;
		if (gpio_setup() < 0) return 1;
		e = getenv("BTFLOW_PWR_WAIT_MS");
		w = e ? atoi(e) : 500;
		printf("[gpio %.1fms] 619: 0 (2s) -> 1 (%dms)\n", ms(), w);
		gpio_val(0); usleep(2000000);
		gpio_val(1); usleep(w * 1000);
	}

	fd = open(TTY, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) { perror("open " TTY); return 1; }
	if (set_baud(fd, init_baud, flow) < 0) return 2;
	clear_rx(fd);

	/* 静默基线 */
	n = drain(fd, rx, sizeof rx, 250, 50);
	printf("[基线] 250ms 静默检查: %d B\n\n", n);

	/* S1 Reset @init */
	step(fd, "S1 Reset@init", reset, 4, 2000, 60, rx, sizeof rx);

	/* S2 Read Local Version @init */
	step(fd, "S2 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);

	/* S3 Read Local Name @init */
	n = step(fd, "S3 ReadLocalName", rdname, 4, 2000, 60, rx, sizeof rx);

	if (scene == 20) {
		/* 0xFC18 速率接受性测试: 单值, 环境变量 BTFLOW_SPEED */
		unsigned int testv = 921600;
		unsigned char c18[10] = { 0x01, 0x18, 0xfc, 0x06, 0, 0, 0, 0, 0, 0 };
		int sk20 = 0, got20;
		double phys;
		{
			const char *e20 = getenv("BTFLOW_SPEED");
			if (e20) testv = (unsigned)strtoul(e20, NULL, 10);
		}
		phys = testv * 37.4 / 48.0;
		c18[4] = (unsigned char)(testv & 0xff);
		c18[5] = (unsigned char)((testv >> 8) & 0xff);
		c18[6] = (unsigned char)((testv >> 16) & 0xff);
		c18[7] = (unsigned char)((testv >> 24) & 0xff);
		printf("[scene20] 测试 0xFC18 值=%u (物理=%.0f)\n", testv, phys);
		ioctl(fd, 0x540B, 2);
		if (write_full(fd, c18, 10) < 0) perror("write");
		got20 = read_event(fd, rx, sizeof rx, 2000, &sk20);
		{
			char d20[128];
			int go_fast = 0;
			hd("FC18resp", rx, got20 > 24 ? 24 : (got20 < 0 ? 0 : got20));
			printf("             -> %s\n",
				evdesc(rx, got20 < 0 ? 0 : got20, d20, sizeof d20));
			if (got20 >= 7 && rx[1] == 0x0e &&
			    (rx[4] | (rx[5] << 8)) == 0xfc18 && rx[6] == 0x00) {
				go_fast = 1;
			} else {
				const char *ef = getenv("BTFLOW_FORCE");
				if (ef && atoi(ef) == 1) {
					printf("[scene20] 未确认接受, FORCE=1 强切验证\n");
					go_fast = 1;
				}
			}
			if (go_fast) {
				printf("[scene20] host 切 %.0f 并验证\n", phys);
				if (set_baud(fd, (unsigned int)(phys + 0.5), flow) < 0)
					return 2;
				usleep(100000);
				ioctl(fd, 0x540B, 2);
				step(fd, "验证 Reset@new", reset, 4, 2000, 60, rx, sizeof rx);
				if (set_baud(fd, init_baud, flow) < 0) return 2;
				usleep(50000);
				ioctl(fd, 0x540B, 2);
			} else {
				printf("[scene20] 拒绝或超时 (不切换)\n");
			}
		}
		goto tail;
	}

	if (scene == 10) {
		/* 完整 hciattach 复刻: 0xFC18(逻辑3000000) -> host 物理2337500
		 * -> 下载(每块 write->read_event->tcflush) -> 切回 init 验证
		 * 逻辑/物理分离: 芯片逻辑速率 * 37.4/48 = host 物理速率 */
		const unsigned int LOGI = 3000000u;  /* 芯片逻辑速率(芯片观念值) */
		const unsigned int PHYF = 2337500u;  /* host 物理 = 3000000*37.4/48 */
		unsigned char c18[10] = { 0x01, 0x18, 0xfc, 0x06, 0, 0, 0, 0, 0, 0 };
		int sk2 = 0, k2 = 0, got2;
		int cc2 = 0, er2 = 0, to2 = 0, ot2 = 0, consec = 0;
		unsigned char *h4b = malloc(1200);

		c18[4] = LOGI & 0xff; c18[5] = (LOGI >> 8) & 0xff;
		c18[6] = (LOGI >> 16) & 0xff; c18[7] = (LOGI >> 24) & 0xff;

		/* (a) 0xFC18 切芯片逻辑速率 */
		ioctl(fd, 0x540B, 2);   /* TCFLSH TCIOFLUSH */
		printf("[scene10] (a) 0xFC18 芯片逻辑速率 -> %u\n", LOGI);
		if (write_full(fd, c18, 10) < 0) perror("write");
		sk2 = 0;
		got2 = read_event(fd, rx, sizeof rx, 2000, &sk2);
		{
			char d[128];
			hd("S4 SetSpeed(FC18)", rx, got2 > 24 ? 24 : (got2 < 0 ? 0 : got2));
			printf("             -> %s (跳%d)\n",
				evdesc(rx, got2 < 0 ? 0 : got2, d, sizeof d), sk2);
		}

		/* (b) host 切 2337500 */
		printf("[scene10] (b) host BOTHER -> %u\n", PHYF);
		if (set_baud(fd, PHYF, flow) < 0) return 2;
		usleep(50000);
		ioctl(fd, 0x540B, 2);

		/* (c) Reset@2337500 验证切速 */
		step(fd, "S6 Reset@2337500", reset, 4, 2000, 60, rx, sizeof rx);

		/* (d) 0xFC2E @2337500 */
		step(fd, "S7 DownloadMinidriver@fast", dlmode, 4, 2000, 60, rx, sizeof rx);
		usleep(50000);
		ioctl(fd, 0x540B, 2);

		/* (e) hciattach 节奏逐块下载: write -> read_event -> tcflush */
		printf("[scene10] (e) 逐块下载 %d 块 @%u (write->event->flush)\n", nblk, PHYF);
		for (k2 = 0; k2 < nblk && h4b; k2++) {
			int l2 = blen[k2];
			long off = boff[k2];
			int n2;
			h4b[0] = 0x01;
			h4b[1] = hcd[off];
			h4b[2] = hcd[off + 1];
			h4b[3] = (unsigned char)l2;
			memcpy(h4b + 4, hcd + off + 3, l2);
			if (write_full(fd, h4b, 4 + l2) < 0) { perror("write"); break; }
			sk2 = 0;
			n2 = read_event(fd, rx, sizeof rx, 2000, &sk2);
			if (n2 < 0) { to2++; consec++; }
			else if (rx[1] == 0x0e && n2 >= 7 &&
				 (rx[4] | (rx[5] << 8)) == (hcd[off] | (hcd[off + 1] << 8))) {
				cc2++; consec = 0;
			} else if (rx[1] == 0x10) { er2++; consec++; }
			else { ot2++; consec++; }
			ioctl(fd, 0x540B, 2);   /* tcflush */
			if (k2 < 3 || (k2 + 1) % 20 == 0) {
				printf("  [scene10] #%d/%d op=0x%04x len=%d -> %s | 累计 CC=%d ER=%d TO=%d 其它=%d\n",
					k2 + 1, nblk, hcd[off] | (hcd[off + 1] << 8), l2,
					n2 < 0 ? "TO" : (rx[1] == 0x0e ? "CC" : (rx[1] == 0x10 ? "ER" : "?")),
					cc2, er2, to2, ot2);
				fflush(stdout);
			}
			if (consec > 40) {
				printf("  [scene10] 连续 %d 块无 CC, 提前中止\n", consec);
				break;
			}
		}
		free(h4b);
		printf("[scene10] 下载统计: 执行 %d 块, CC=%d ER=%d TO=%d 其它=%d\n",
			k2, cc2, er2, to2, ot2);

		/* (f) 200ms 等固件就绪 */
		usleep(200000);

		/* (g) host 切回 89760 */
		printf("[scene10] (g) host 切回 %u\n", init_baud);
		if (set_baud(fd, init_baud, flow) < 0) return 2;
		usleep(50000);
		ioctl(fd, 0x540B, 2);

		/* (h) @89760 验证 */
		got2 = step(fd, "S10 Reset@89760", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion@89760", rdver, 4, 2000, 60, rx, sizeof rx);

		/* (i) 若 @89760 无响应, 试 @2337500 (固件可能未重置速率) */
		if (got2 < 0) {
			printf("[scene10] (i) @89760 无响应 -> 试 %u\n", PHYF);
			if (set_baud(fd, PHYF, flow) < 0) return 2;
			usleep(50000);
			ioctl(fd, 0x540B, 2);
			step(fd, "S10b Reset@fast", reset, 4, 2000, 60, rx, sizeof rx);
			step(fd, "S11b ReadLocalVersion@fast", rdver, 4, 2000, 60, rx, sizeof rx);
			if (set_baud(fd, init_baud, flow) < 0) return 2;
			usleep(50000);
			ioctl(fd, 0x540B, 2);
		}
		goto tail;
	}

	if (target_baud != init_baud) {
		/* S4 0xFC18 切芯片速率 @init */
		setspeed[6] = (unsigned char)(target_baud);
		setspeed[7] = (unsigned char)(target_baud >> 8);
		setspeed[8] = (unsigned char)(target_baud >> 16);
		setspeed[9] = (unsigned char)(target_baud >> 24);
		step(fd, "S4 SetSpeed(0xFC18)", setspeed, 10, 2000, 60, rx, sizeof rx);

		/* S5 host 切 target */
		printf("[%9.1fms] << host 切 %u\n", ms(), target_baud);
		if (set_baud(fd, target_baud, flow) < 0) return 2;
		clear_rx(fd);

		/* S6 Reset @target 验证 */
		step(fd, "S6 Reset@target", reset, 4, 2000, 60, rx, sizeof rx);
	}

	if (scene == 3) {
		printf("[scene3] 无 0xFC2E, 直接发块1\n");
		if (nblk >= 1) {
			send_blk(fd, hcd, boff[0], blen[0], 0);
			obs_sec(fd, "块1(无FC2E)", 8);
		}
		goto tail;
	}

	/* S7 下载模式 0xFC2E */
	s7ret = step(fd, "S7 DownloadMinidriver", dlmode, 4, 2000, 60, rx, sizeof rx);
	s7ok = (s7ret >= 7 && rx[1] == 0x0e &&
		(rx[4] | (rx[5] << 8)) == 0xfc2e && rx[6] == 0x00);
	printf("[health] S7 %s\n", s7ok ? "健康" : "退化(!)");

	/* S7.5 观察窗: CC 之后是否还有额外字节 (Android "2-byte confirmation"?) */
	{
		int extra = drain(fd, rx, sizeof rx, 800, 200);
		printf("[S7.5] CC 后额外字节: %d B", extra);
		for (i = 0; i < extra && i < 16; i++)
			printf(" %02x", rx[i]);
		printf("\n");
		fflush(stdout);
	}
	usleep(100000);

	if (scene == 1) {
		printf("[scene1] 静默 3s 后发块1\n");
		sleep(3);
		if (nblk >= 1) {
			send_blk(fd, hcd, boff[0], blen[0], 0);
			obs_sec(fd, "块1(静默3s后)", 8);
		}
		goto tail;
	} else if (scene == 2) {
		if (nblk >= 1) {
			send_blk(fd, hcd, boff[0], blen[0], 0);
			obs_sec(fd, "块1 第1次", 6);
			send_blk(fd, hcd, boff[0], blen[0], 0);
			obs_sec(fd, "块1 第2次", 6);
		}
		goto tail;
	} else if (scene == 4) {
		if (nblk >= 2) {
			send_blk(fd, hcd, boff[1], blen[1], 0);
			obs_sec(fd, "块2(跳过块1)", 8);
		}
		goto tail;
	} else if (scene == 5) {
		/* 连续流: 复刻 hciattach load_firmware 的行为(不等响应, 全速灌) */
		unsigned char *h4buf = malloc(1200);
		int k;
		long long t0 = now_us();
		printf("[scene5] 连续流下载全部 %d 块...\n", nblk);
		if (h4buf) {
			for (k = 0; k < nblk; k++) {
				int l2 = blen[k];
				long off = boff[k];
				h4buf[0] = 0x01;
				h4buf[1] = hcd[off];
				h4buf[2] = hcd[off + 1];
				h4buf[3] = (unsigned char)l2;
				memcpy(h4buf + 4, hcd + off + 3, l2);
				if (write_full(fd, h4buf, 4 + l2) < 0)
					perror("write");
				if (k % 32 == 0) {
					printf("  [scene5] %d/%d 块, 用时 %.1fs\n", k, nblk, (now_us() - t0) / 1e6);
					fflush(stdout);
				}
				usleep(1000);
			}
			printf("[scene5] 全部 %d 块发出, 用时 %.1fs\n", nblk, (now_us() - t0) / 1e6);
			free(h4buf);
		}
		obs_sec(fd, "连续流后", 20);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 6 || scene == 7) {
		/* 纯观察 15s: CC 是否在 ~9.000s 自行到达? */
		int bi = (scene == 6) ? 0 : 1;
		if (nblk > bi) {
			send_blk(fd, hcd, boff[bi], blen[bi], 0);
			obs_sec(fd, scene == 6 ? "块1+纯观察15s" : "块2+纯观察15s", 15);
		}
		goto tail;
	} else if (scene == 8) {
		/* 块组合签名: 连续发前 cnt 块, 观察响应模式 */
		int cnt = (argc > 8) ? atoi(argv[8]) : 3;
		int k;
		unsigned char *h4buf = malloc(1200);
		if (cnt <= 0 || cnt > nblk)
			cnt = nblk;
		printf("[scene8] 连续发前 %d/%d 块\n", cnt, nblk);
		if (h4buf) {
			for (k = 0; k < cnt; k++) {
				int l2 = blen[k];
				long off = boff[k];
				h4buf[0] = 0x01;
				h4buf[1] = hcd[off];
				h4buf[2] = hcd[off + 1];
				h4buf[3] = (unsigned char)l2;
				memcpy(h4buf + 4, hcd + off + 3, l2);
				write_full(fd, h4buf, 4 + l2);
			}
			printf("[scene8] 已发 %d 块\n", cnt);
			free(h4buf);
		}
		obs_sec(fd, "块组合后", 8);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 9) {
		/* 逐块演变: 同一次上电内, 发一块 -> 即时读窗 -> 下一块 */
		int cnt = (argc > 8) ? atoi(argv[8]) : 20;
		int k;
		unsigned char *h4buf = malloc(1200);
		if (cnt <= 0 || cnt > nblk)
			cnt = nblk;
		printf("[scene9] 逐块演变 %d 块, 每块后即时读窗\n", cnt);
		for (k = 0; k < cnt && h4buf; k++) {
			int l2 = blen[k];
			long off = boff[k];
			int got;
			h4buf[0] = 0x01;
			h4buf[1] = hcd[off];
			h4buf[2] = hcd[off + 1];
			h4buf[3] = (unsigned char)l2;
			memcpy(h4buf + 4, hcd + off + 3, l2);
			printf("=== 块%d (op=0x%04x len=%d) ===\n", k,
				hcd[off] | (hcd[off + 1] << 8), l2);
			write_full(fd, h4buf, 4 + l2);
			got = drain(fd, rx, sizeof rx, 500, 200);
			if (got == 0) {
				printf("    -> 窗口内无响应 (憋)\n");
			} else {
				char d2[128];
				printf("    -> %d B: ", got);
				for (i = 0; i < got && i < 24; i++)
					printf("%02x ", rx[i]);
				printf("\n       %s\n", evdesc(rx, got, d2, sizeof d2));
			}
			fflush(stdout);
		}
		free(h4buf);
		obs_sec(fd, "逐块后观察", 4);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 11) {
		/* 最小单元 Write RAM 测试:
		 * (A) 单条 8B 数据 (B) 续写 3x8B (C) 原块0 (D) 块1 拆 4x50B (E) 块1 完整 */
		int k3, n3;
		unsigned char *hb = malloc(1200);
		unsigned int a0, a1;

		if (!hb) goto tail;
		a0 = hcd[boff[0]+3] | (hcd[boff[0]+4]<<8) |
			(hcd[boff[0]+5]<<16) | ((unsigned)hcd[boff[0]+6]<<24);
		a1 = hcd[boff[1]+3] | (hcd[boff[1]+4]<<8) |
			(hcd[boff[1]+5]<<16) | ((unsigned)hcd[boff[1]+6]<<24);
		printf("[scene11] 块0 addr=0x%08x len=%d, 块1 addr=0x%08x len=%d\n",
			a0, blen[0], a1, blen[1]);

#define S11_SEND(ADDR, DATA, DLEN, TAG, WAITMS) do { \
			int l3 = 4 + (DLEN); \
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3; \
			hb[4]=(unsigned char)(ADDR); hb[5]=(unsigned char)((ADDR)>>8); \
			hb[6]=(unsigned char)((ADDR)>>16); hb[7]=(unsigned char)((ADDR)>>24); \
			memcpy(hb+8, (DATA), (DLEN)); \
			printf("[%9.1fms] >> %s (TX %d B @0x%08x)\n", ms(), TAG, l3+4, (unsigned)(ADDR)); \
			write_full(fd, hb, 4+l3); \
			n3 = drain(fd, rx, sizeof rx, (WAITMS), 150); \
			{ char d3[128]; \
			  hd(TAG, rx, n3>24?24:n3); \
			  printf("             -> %s\n", evdesc(rx, n3, d3, sizeof d3)); } \
			fflush(stdout); \
		} while (0)

		/* (A) 单条 8B 数据 @块0地址 */
		printf("[scene11] (A) 单条 8B 数据\n");
		S11_SEND(a0, hcd + boff[0] + 7, 8, "A:8B@blk0", 3000);

		/* (B) 续写 3x8B */
		printf("[scene11] (B) 续写 3x8B\n");
		S11_SEND(a0 + 8,  hcd + boff[0] + 15, 8, "B1:8B@+8", 2500);
		S11_SEND(a0 + 16, hcd + boff[0] + 23, 8, "B2:8B@+16", 2500);
		S11_SEND(a0 + 24, hcd + boff[0] + 31, 8, "B3:8B@+24", 2500);

		/* (C) 原块0 完整 */
		printf("[scene11] (C) 原块0 (len=%d)\n", blen[0]);
		send_blk(fd, hcd, boff[0], blen[0], 0);
		n3 = drain(fd, rx, sizeof rx, 3000, 150);
		{ char d3[128]; hd("C:blk0完整", rx, n3>24?24:n3);
		  printf("             -> %s\n", evdesc(rx, n3, d3, sizeof d3)); }

		/* (D) 块1 拆分 4x50B */
		printf("[scene11] (D) 块1 拆分为 4x50B\n");
		for (k3 = 0; k3 < 4; k3++) {
			char tg[32];
			snprintf(tg, sizeof tg, "D%d:50B@+%d", k3, k3 * 50);
			S11_SEND(a1 + k3 * 50, hcd + boff[1] + 7 + k3 * 50, 50, tg, 2500);
		}

		/* (E) 块1 完整 */
		printf("[scene11] (E) 块1 完整 (len=%d)\n", blen[1]);
		send_blk(fd, hcd, boff[1], blen[1], 0);
		n3 = drain(fd, rx, sizeof rx, 3000, 150);
		{ char d3[128]; hd("E:blk1完整", rx, n3>24?24:n3);
		  printf("             -> %s\n", evdesc(rx, n3, d3, sizeof d3)); }

		free(hb);
#undef S11_SEND
		obs_sec(fd, "scene11 后观察", 3);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 12) {
		/* 单条 8B 超长观察: 30s 全窗 + 中性字节踢 + 重发命令
		 * 判明"憋住的响应"是"永不来"还是"极慢" */
		unsigned char *hb = malloc(1200);
		unsigned int a0;
		int n12;

		if (!hb) goto tail;
		a0 = hcd[boff[0]+3] | (hcd[boff[0]+4]<<8) |
			(hcd[boff[0]+5]<<16) | ((unsigned)hcd[boff[0]+6]<<24);
		printf("[scene12] 单条 8B 超长观察 @0x%08x\n", a0);

		/* 发 8B 命令 (同 scene11 A) */
		{
			int l3 = 4 + 8;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)a0; hb[5]=(unsigned char)(a0>>8);
			hb[6]=(unsigned char)(a0>>16); hb[7]=(unsigned char)(a0>>24);
			memcpy(hb+8, hcd + boff[0] + 7, 8);
			printf("[%9.1fms] >> 发 8B 命令\n", ms());
			write_full(fd, hb, 4 + l3);
		}
		n12 = drain_trace(fd, rx, sizeof rx, 30000, 200);

		if (n12 == 0) {
			unsigned char z = 0x00;
			printf("[scene12] 30s 零字节 -> 写 1 字节 0x00 踢一脚\n");
			if (write_full(fd, &z, 1) < 0) {}
			n12 = drain_trace(fd, rx, sizeof rx, 5000, 200);
		}
		if (n12 == 0) {
			/* 重发同一命令, 验证"新命令释放旧响应" */
			int l3 = 4 + 8;
			printf("[scene12] 仍 0 字节 -> 重发同一命令\n");
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)a0; hb[5]=(unsigned char)(a0>>8);
			hb[6]=(unsigned char)(a0>>16); hb[7]=(unsigned char)(a0>>24);
			memcpy(hb+8, hcd + boff[0] + 7, 8);
			write_full(fd, hb, 4 + l3);
			n12 = drain_trace(fd, rx, sizeof rx, 6000, 200);
		}
		if (n12 > 0) {
			char d3[128];
			hd("scene12 总响应", rx, n12 > 32 ? 32 : n12);
			printf("             -> %s\n", evdesc(rx, n12, d3, sizeof d3));
		}
		free(hb);
		obs_sec(fd, "scene12 后观察", 2);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 13) {
		/* 连续小命令衰减曲线: N 条 8B 顺序写, 每条等 CC (5s) */
		unsigned char *hb = malloc(1200);
		unsigned int a1;
		int k13, n13, sk13;
		int N13 = (argc > 8) ? atoi(argv[8]) : 24;
		int cc13 = 0, er13 = 0, to13 = 0, ot13 = 0, consec13 = 0;

		if (!hb) goto tail;
		a1 = hcd[boff[1]+3] | (hcd[boff[1]+4]<<8) |
			(hcd[boff[1]+5]<<16) | ((unsigned)hcd[boff[1]+6]<<24);
		printf("[scene13] 连发 %d 条 8B (地址 0x%08x 起, 数据=块1数据区)\n", N13, a1);
		for (k13 = 0; k13 < N13; k13++) {
			int l3 = 4 + 8;
			long long t1 = now_us();
			unsigned int addr = a1 + k13 * 8;
			const char *rr;
			long long dt;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)addr; hb[5]=(unsigned char)(addr>>8);
			hb[6]=(unsigned char)(addr>>16); hb[7]=(unsigned char)(addr>>24);
			memcpy(hb+8, hcd + boff[1] + 7 + k13 * 8, 8);
			write_full(fd, hb, 4 + l3);
			sk13 = 0;
			n13 = read_event(fd, rx, sizeof rx, 5000, &sk13);
			dt = (now_us() - t1) / 1000;
			rr = n13 < 0 ? "TO" : (rx[1]==0x0e ? "CC" : (rx[1]==0x10 ? "ER" : "?"));
			if (n13 < 0) { to13++; consec13++; }
			else if (rx[1]==0x0e) { cc13++; consec13 = 0; }
			else if (rx[1]==0x10) { er13++; consec13++; }
			else { ot13++; consec13++; }
			printf("  #%02d @0x%08x -> %s (+%lldms, sk=%d) | CC=%d ER=%d TO=%d 其它=%d\n",
				k13, addr, rr, dt, sk13, cc13, er13, to13, ot13);
			fflush(stdout);
			if (consec13 >= 8) {
				printf("  [scene13] 连续 %d 条坏, 中止\n", consec13);
				break;
			}
		}
		free(hb);
		obs_sec(fd, "scene13 后观察", 3);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 14) {
		/* 健康起点 + 8B 连发容量测试
		 * S7 不健康 -> 输出 REGEN 直接退出 (由外层脚本重上电重试) */
		unsigned char *hb = malloc(1200);
		unsigned int a1;
		int k14, n14, sk14;
		int N14 = (argc > 8) ? atoi(argv[8]) : 6;
		int cc14 = 0, er14 = 0, to14 = 0, ot14 = 0, consec14 = 0;

		if (!s7ok) {
			printf("[scene14] REGEN: S7 不健康 (ret=%d), 放弃本轮\n", s7ret);
			if (hb) free(hb);
			goto tail;
		}
		if (!hb) goto tail;
		a1 = hcd[boff[1]+3] | (hcd[boff[1]+4]<<8) |
			(hcd[boff[1]+5]<<16) | ((unsigned)hcd[boff[1]+6]<<24);
		printf("[scene14] 健康起点 OK, 先发试航条 #0\n");
		{
			int l3 = 4 + 8, n0x, sk0 = 0;
			long long t1 = now_us(), dt;
			unsigned int addr = a1;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)addr; hb[5]=(unsigned char)(addr>>8);
			hb[6]=(unsigned char)(addr>>16); hb[7]=(unsigned char)(addr>>24);
			memcpy(hb+8, hcd + boff[1] + 7, 8);
			write_full(fd, hb, 4 + l3);
			n0x = read_event(fd, rx, sizeof rx, 5000, &sk0);
			dt = (now_us() - t1) / 1000;
			if (n0x >= 7 && rx[1] == 0x0e && (rx[4] | (rx[5] << 8)) == 0xfc4c) {
				printf("  #00 试航 -> CC ok (+%lldms)\n", dt);
				cc14++;
				if (gap_ms > 0) usleep(gap_ms * 1000);
			} else {
				char d0[96];
				printf("  #00 试航 -> %s (+%lldms, %d B)\n",
					n0x < 0 ? "TO" : evdesc(rx, n0x, d0, sizeof d0),
					dt, n0x < 0 ? 0 : n0x);
				printf("[scene14] REGEN: 试航条未 CC, 放弃本轮\n");
				free(hb);
				goto tail;
			}
		}
		printf("[scene14] 继续容量测试 #1..#%d\n", N14);
		for (k14 = 1; k14 <= N14; k14++) {
			int l3 = 4 + 8;
			long long t1 = now_us();
			unsigned int addr = a1 + k14 * 8;
			const char *rr;
			long long dt;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)addr; hb[5]=(unsigned char)(addr>>8);
			hb[6]=(unsigned char)(addr>>16); hb[7]=(unsigned char)(addr>>24);
			memcpy(hb+8, hcd + boff[1] + 7 + k14 * 8, 8);
			write_full(fd, hb, 4 + l3);
			sk14 = 0;
			n14 = read_event(fd, rx, sizeof rx, 5000, &sk14);
			dt = (now_us() - t1) / 1000;
			rr = n14 < 0 ? "TO" : (rx[1]==0x0e ? "CC" : (rx[1]==0x10 ? "ER" : "?"));
			if (n14 < 0) { to14++; consec14++; }
			else if (rx[1]==0x0e) { cc14++; consec14 = 0; }
			else if (rx[1]==0x10) { er14++; consec14++; }
			else { ot14++; consec14++; }
			printf("  #%02d @0x%08x -> %s (+%lldms, sk=%d) | CC=%d ER=%d TO=%d 其它=%d\n",
				k14, addr, rr, dt, sk14, cc14, er14, to14, ot14);
			fflush(stdout);
			if (gap_ms > 0) usleep(gap_ms * 1000);
			if (consec14 >= 8) {
				printf("  [scene14] 连续 %d 条坏, 中止\n", consec14);
				break;
			}
		}
		free(hb);
		obs_sec(fd, "scene14 后观察", 3);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 16) {
		/* 等安静模式: 每条 8B 后 drain 到流安静(300ms)再发下一条;
		 * 末尾用"读名字"命令作触发器释放滞后响应 */
		unsigned char *hb = malloc(1200);
		unsigned int a1;
		int k16, n16, j16;
		int N16 = (argc > 8) ? atoi(argv[8]) : 24;
		int cc16 = 0, er16 = 0, to16 = 0, consec16 = 0;
		int wait16 = 4000;
		{
			const char *e16 = getenv("BTFLOW_WAIT_MS");
			if (e16) wait16 = atoi(e16);
		}

		if (!s7ok) {
			printf("[scene16] REGEN: S7 不健康 (ret=%d)\n", s7ret);
			if (hb) free(hb);
			goto tail;
		}
		if (!hb) goto tail;
		a1 = hcd[boff[1]+3] | (hcd[boff[1]+4]<<8) |
			(hcd[boff[1]+5]<<16) | ((unsigned)hcd[boff[1]+6]<<24);
		printf("[scene16] 等安静模式, 试航 #0\n");
		{
			int l3 = 4 + 8, n0x;
			long long t1 = now_us(), dt;
			unsigned int addr = a1;
			int hc0 = 0;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)addr; hb[5]=(unsigned char)(addr>>8);
			hb[6]=(unsigned char)(addr>>16); hb[7]=(unsigned char)(addr>>24);
			memcpy(hb+8, hcd + boff[1] + 7, 8);
			write_full(fd, hb, 4 + l3);
			n0x = drain(fd, rx, sizeof rx, wait16, 300);
			dt = (now_us() - t1) / 1000;
			for (j16 = 0; j16 + 7 <= n0x; j16++)
				if (rx[j16]==0x04 && rx[j16+1]==0x0e &&
				    rx[j16+4]==0x4c && rx[j16+5]==0xfc) hc0 = 1;
			if (hc0) {
				printf("  #00 -> 含CC (+%lldms, %dB)\n", dt, n0x);
				cc16++;
			} else {
				printf("  #00 -> 无CC (+%lldms, %dB)\n", dt, n0x);
				printf("[scene16] REGEN: 试航无CC, 放弃本轮\n");
				free(hb);
				goto tail;
			}
		}
		printf("[scene16] 等安静循环 #1..#%d\n", N16);
		for (k16 = 1; k16 <= N16; k16++) {
			int l3 = 4 + 8;
			long long t1 = now_us(), dt;
			unsigned int addr = a1 + k16 * 8;
			int hc = 0, erc = 0;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)addr; hb[5]=(unsigned char)(addr>>8);
			hb[6]=(unsigned char)(addr>>16); hb[7]=(unsigned char)(addr>>24);
			memcpy(hb+8, hcd + boff[1] + 7 + k16 * 8, 8);
			write_full(fd, hb, 4 + l3);
			n16 = drain(fd, rx, sizeof rx, wait16, 300);
			dt = (now_us() - t1) / 1000;
			for (j16 = 0; j16 + 7 <= n16; j16++) {
				if (rx[j16]==0x04 && rx[j16+1]==0x0e &&
				    rx[j16+4]==0x4c && rx[j16+5]==0xfc) hc = 1;
				if (rx[j16]==0x04 && rx[j16+1]==0x10) { erc++; j16 += 2; }
			}
			if (hc) { cc16++; consec16 = 0; }
			else if (n16 > 0) { er16++; consec16++; }
			else { to16++; consec16++; }
			printf("  #%02d @0x%08x -> CC:%s ER:%d 字节:%d (+%lldms) | 累计 CC=%d 坏=%d 空=%d\n",
				k16, addr, hc ? "Y" : "N", erc, n16, dt, cc16, er16, to16);
			fflush(stdout);
			if (consec16 >= 5) {
				printf("  [scene16] 连续 %d 条无 CC, 中止\n", consec16);
				break;
			}
		}
		/* 收尾触发器: 读名字 (无副作用), 释放滞后响应 */
		printf("[scene16] 收尾触发器: ReadLocalName\n");
		write_full(fd, rdname, 4);
		n16 = drain(fd, rx, sizeof rx, 2000, 300);
		{
			int hc = 0, erc = 0;
			for (j16 = 0; j16 + 7 <= n16; j16++) {
				if (rx[j16]==0x04 && rx[j16+1]==0x0e &&
				    rx[j16+4]==0x4c && rx[j16+5]==0xfc) { hc = 1; cc16++; }
				if (rx[j16]==0x04 && rx[j16+1]==0x10) { erc++; j16 += 2; }
			}
			printf("  [收尾] 含CC:%s ER:%d 字节:%d | 最终 CC=%d\n",
				hc ? "Y" : "N", erc, n16, cc16);
		}
		free(hb);
		obs_sec(fd, "scene16 后观察", 3);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 19) {
		/* 流水线模式: 固定短收割窗(不等长), 维持数据流不断
		 * BTFLOW_BYTE_US>0 时改为"逐字节发送+字节间间隔"模式 */
		unsigned char *hb = malloc(1200);
		unsigned int a1;
		int k19, j19;
		int N19 = (argc > 8) ? atoi(argv[8]) : 50;
		int win19 = 60;
		int cc19 = 0, er19 = 0, b19 = 0, zc19 = 0;
		int byte_us = 0;
		long long t19 = now_us();
		{
			const char *e19 = getenv("BTFLOW_PIPE_MS");
			if (e19) win19 = atoi(e19);
			e19 = getenv("BTFLOW_BYTE_US");
			if (e19) byte_us = atoi(e19);
		}
		if (!s7ok) {
			printf("[scene19] REGEN: S7 不健康\n");
			if (hb) free(hb);
			goto tail;
		}
		if (!hb) goto tail;
		a1 = hcd[boff[1]+3] | (hcd[boff[1]+4]<<8) |
			(hcd[boff[1]+5]<<16) | ((unsigned)hcd[boff[1]+6]<<24);
		printf("[scene19] 流水线 %d 条 8B, 收割窗 %dms, 逐字节间隔 %dus\n",
			N19, win19, byte_us);
		for (k19 = 0; k19 < N19; k19++) {
			int l3 = 4 + 8, n19, hc = 0, erc = 0;
			unsigned int addr = a1 + k19 * 8;
			long dso = (k19 * 8) % 192;
			hb[0]=0x01; hb[1]=0x4c; hb[2]=0xfc; hb[3]=(unsigned char)l3;
			hb[4]=(unsigned char)addr; hb[5]=(unsigned char)(addr>>8);
			hb[6]=(unsigned char)(addr>>16); hb[7]=(unsigned char)(addr>>24);
			memcpy(hb+8, hcd + boff[1] + 7 + dso, 8);
			if (byte_us > 0) {
				int bi2;
				for (bi2 = 0; bi2 < 4 + l3; bi2++) {
					unsigned char c = hb[bi2];
					if (write_full(fd, &c, 1) < 0) { perror("write"); break; }
					usleep(byte_us);
				}
			} else {
				write_full(fd, hb, 4 + l3);
			}
			n19 = drain(fd, rx, sizeof rx, win19, 30);
			for (j19 = 0; j19 + 7 <= n19; j19++) {
				if (rx[j19]==0x04 && rx[j19+1]==0x0e &&
				    rx[j19+4]==0x4c && rx[j19+5]==0xfc) { hc = 1; cc19++; }
				if (rx[j19]==0x04 && rx[j19+1]==0x10) { er19++; erc++; j19 += 2; }
			}
			b19 += n19;
			if (n19 == 0) zc19++;
			if (k19 < 3 || hc || erc || (k19+1) % 10 == 0) {
				printf("  #%03d -> %dB%s%s | 总 CC=%d ER=%d 零窗=%d 字节=%d\n",
					k19, n19, hc ? " [CC]" : "", erc ? " [ER]" : "",
					cc19, er19, zc19, b19);
				fflush(stdout);
			}
		}
		printf("[scene19] 完成 %d 条, 用时 %.1fs, CC=%d ER=%d 零窗=%d 字节=%d\n",
			N19, (now_us() - t19) / 1e6, cc19, er19, zc19, b19);
		free(hb);
		obs_sec(fd, "scene19 后观察", 3);
		step(fd, "S10 Reset", reset, 4, 2000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion", rdver, 4, 2000, 60, rx, sizeof rx);
		goto tail;
	} else if (scene == 22) {
		/* 完整下载: 全部块逐字节慢发 (BTFLOW_BYTE_US 控制间隔, 默认 2000us) */
		int k22, n22, j22;
		int cc22 = 0, er22 = 0, b22 = 0;
		int byte_us22 = 2000;
		long long t22 = now_us();
		{
			const char *e22 = getenv("BTFLOW_BYTE_US");
			if (e22) byte_us22 = atoi(e22);
		}
		if (!s7ok) {
			printf("[scene22] REGEN: S7 不健康\n");
			goto tail;
		}
		printf("[scene22] 完整下载 %d 块, 逐字节 %dus\n", nblk, byte_us22);
		for (k22 = 0; k22 < nblk; k22++) {
			int l2 = blen[k22];
			long off = boff[k22];
			int total = 4 + l2, bi2, hc = 0, erc = 0;
			unsigned char h4b2[1200];
			h4b2[0] = 0x01;
			h4b2[1] = hcd[off];
			h4b2[2] = hcd[off + 1];
			h4b2[3] = (unsigned char)l2;
			memcpy(h4b2 + 4, hcd + off + 3, l2);
			for (bi2 = 0; bi2 < total; bi2++) {
				unsigned char c = h4b2[bi2];
				if (write_full(fd, &c, 1) < 0) { perror("write"); break; }
				usleep(byte_us22);
			}
			n22 = drain(fd, rx, sizeof rx, 2000, 50);
			for (j22 = 0; j22 + 7 <= n22; j22++) {
				if (rx[j22]==0x04 && rx[j22+1]==0x0e &&
				    rx[j22+4]==0x4c && rx[j22+5]==0xfc) { hc = 1; cc22++; }
				if (rx[j22]==0x04 && rx[j22+1]==0x10) { erc++; j22 += 2; }
			}
			er22 += erc;
			b22 += n22;
			if (k22 < 3 || hc || erc || (k22+1) % 20 == 0) {
				printf("  块%03d/%d op=0x%04x len=%d -> %dB%s%s | 总 CC=%d ER=%d\n",
					k22 + 1, nblk, hcd[off] | (hcd[off+1]<<8), l2, n22,
					hc ? " [CC]" : "", erc ? " [ER]" : "", cc22, er22);
				fflush(stdout);
			}
		}
		printf("[scene22] 下载完 %d 块: CC=%d ER=%d 字节=%d, 用时 %.1fs\n",
			nblk, cc22, er22, b22, (now_us() - t22) / 1e6);
		obs_sec(fd, "下载后观察", 2);
		/* 关键: 固件启动后芯片物理速率变为 115200, host 切换验证 */
		printf("[scene22] host 切 115200 验证固件...\n");
		if (set_baud(fd, 115200, flow) < 0) return 2;
		usleep(100000);
		ioctl(fd, 0x540B, 2);
		step(fd, "S10 Reset@115200", reset, 4, 3000, 60, rx, sizeof rx);
		step(fd, "S11 ReadLocalVersion@115200", rdver, 4, 3000, 60, rx, sizeof rx);
		step(fd, "S12 ReadLocalName@115200", rdname, 4, 2000, 60, rx, sizeof rx);
		/* 固件模式 0xFC18 切速测试 */
		{
			unsigned char c23[10] = { 0x01, 0x18, 0xfc, 0x06, 0, 0, 0, 0, 0, 0 };
			unsigned int v23 = 2000000;
			int sk23 = 0, got23;
			char d23[128];
			const char *e23 = getenv("BTFLOW_SPEED2");
			if (e23) v23 = (unsigned)strtoul(e23, NULL, 10);
			c23[4] = (unsigned char)(v23 & 0xff);
			c23[5] = (unsigned char)((v23 >> 8) & 0xff);
			c23[6] = (unsigned char)((v23 >> 16) & 0xff);
			c23[7] = (unsigned char)((v23 >> 24) & 0xff);
			printf("[scene22] 固件模式 0xFC18(%u) 测试:\n", v23);
			write_full(fd, c23, 10);
			got23 = read_event(fd, rx, sizeof rx, 2000, &sk23);
			hd("FC18@fw", rx, got23 > 24 ? 24 : (got23 < 0 ? 0 : got23));
			printf("             -> %s\n",
				evdesc(rx, got23 < 0 ? 0 : got23, d23, sizeof d23));
			if (set_baud(fd, 115200, flow) < 0) return 2;
			usleep(50000);
			ioctl(fd, 0x540B, 2);
		}
		goto tail;
	} else if (scene == 23) {
		/* scene23: 完整下载(逐字节) + 切 115200 固件验证 + 原位挂 N_HCI/H4
		 * 成功: fork 出常驻子进程持有 fd, 父进程打印 "OK ATTACHED holder_pid=<pid>" 后退出
		 * 失败: 打印原因 goto tail, 由外层 shell 重试(每次全新上电)
		 * 前提: 已 modprobe hci_uart (否则 TIOCSETD 报 ENODEV) */
		int k23, n23, j23, rv23;
		int cc23 = 0, er23 = 0, b23 = 0;
		int byte_us23 = 500;
		int fwok23 = 0;
		long long t23 = now_us();
		{
			const char *e23c = getenv("BTFLOW_BYTE_US");
			if (e23c) byte_us23 = atoi(e23c);
		}
		if (!s7ok) {
			printf("[scene23] REGEN: S7 不健康\n");
			goto tail;
		}
		printf("[scene23] 完整下载 %d 块, 逐字节 %dus\n", nblk, byte_us23);
		for (k23 = 0; k23 < nblk; k23++) {
			int l2 = blen[k23];
			long off = boff[k23];
			int total = 4 + l2, bi2, hc = 0, erc = 0;
			unsigned char h4b2[1200];
			h4b2[0] = 0x01;
			h4b2[1] = hcd[off];
			h4b2[2] = hcd[off + 1];
			h4b2[3] = (unsigned char)l2;
			memcpy(h4b2 + 4, hcd + off + 3, l2);
			for (bi2 = 0; bi2 < total; bi2++) {
				unsigned char c = h4b2[bi2];
				if (write_full(fd, &c, 1) < 0) { perror("write"); break; }
				usleep(byte_us23);
			}
			n23 = drain(fd, rx, sizeof rx, 2000, 50);
			for (j23 = 0; j23 + 7 <= n23; j23++) {
				if (rx[j23]==0x04 && rx[j23+1]==0x0e &&
				    rx[j23+4]==0x4c && rx[j23+5]==0xfc) { hc = 1; cc23++; }
				if (rx[j23]==0x04 && rx[j23+1]==0x10) { erc++; j23 += 2; }
			}
			er23 += erc;
			b23 += n23;
			if (k23 < 3 || hc == 0 || erc || (k23+1) % 40 == 0) {
				printf("  块%03d/%d op=0x%04x len=%d -> %dB%s%s | 总 CC=%d ER=%d\n",
					k23 + 1, nblk, hcd[off] | (hcd[off+1]<<8), l2, n23,
					hc ? " [CC]" : " [无CC]", erc ? " [ER]" : "", cc23, er23);
				fflush(stdout);
			}
		}
		printf("[scene23] 下载完 %d 块: CC=%d ER=%d 用时 %.1fs\n",
			nblk, cc23, er23, (now_us() - t23) / 1e6);
		if (cc23 < 279 || er23 > 0) {
			printf("[scene23] 下载不完整 -> 失败退出 (外层重试)\n");
			goto tail;
		}
		obs_sec(fd, "下载后观察", 2);
		printf("[scene23] host 切 115200 验证固件...\n");
		if (set_baud(fd, 115200, flow) < 0) return 2;
		usleep(100000);
		ioctl(fd, 0x540B, 2);
		step(fd, "S10 Reset@115200", reset, 4, 3000, 60, rx, sizeof rx);
		rv23 = step(fd, "S11 ReadLocalVersion@115200", rdver, 4, 3000, 60, rx, sizeof rx);
		if (rv23 >= 8 && rx[7] == 0x08) fwok23 = 1;
		printf("[scene23] 固件激活判据: rx[7](hci_ver)=0x%02x -> %s\n",
			rv23 >= 8 ? rx[7] : 0xff, fwok23 ? "固件模式 OK" : "未激活(!)");
		if (!fwok23) {
			printf("[scene23] 固件未激活 -> 失败退出 (外层重试)\n");
			goto tail;
		}
		step(fd, "S12 ReadLocalName@115200", rdname, 4, 2000, 60, rx, sizeof rx);

		/* ===== 关键一步: 原位挂 N_HCI ldisc + H4 协议, 交内核 hci_uart ===== */
		usleep(100000);
		ioctl(fd, 0x540B, 2);	/* TCFLSH TCIOFLUSH */
		{
			/* termios 恢复为标准 115200 (B115200 而非 BOTHER), 内核按此同步 */
			struct termios2 t2;
			memset(&t2, 0, sizeof t2);
			t2.c_cflag = CS8 | CREAD | CLOCAL | B115200;
			if (flow)
				t2.c_cflag |= CRTSCTS;
			t2.c_ispeed = t2.c_ospeed = 115200;
			t2.c_cc[VMIN] = 1;
			if (ioctl(fd, TCSETSF2, &t2) < 0)
				perror("[scene23] termios 标准 115200");
		}
		printf("[scene23] 挂 N_HCI ldisc(%d) + H4 proto(%d)...\n", N_HCI, HCI_UART_H4);
		{
			int ld23 = N_HCI;
			int pr23 = HCI_UART_H4;
			if (ioctl(fd, TIOCSETD, &ld23) < 0) {
				perror("[scene23] TIOCSETD N_HCI (hci_uart 未加载?)");
				goto tail;
			}
			if (ioctl(fd, HCIUARTSETPROTO, pr23) < 0) {
				perror("[scene23] HCIUARTSETPROTO H4");
				goto tail;
			}
		}
		printf("[scene23] 内核已接管 (hci_uart H4)\n");
		fflush(stdout);
		{
			int c23 = (int)fork();
			if (c23 < 0) {
				perror("fork");
				goto tail;
			}
			if (c23 > 0) {
				/* 父进程: 稍等确保子进程完成 setsid, 打印后退出让 ssh 返回 */
				usleep(300000);
				printf("[scene23] OK ATTACHED holder_pid=%d\n", c23);
				fflush(stdout);
				return 0;
			}
			/* 子进程: 脱离会话持有 fd 常驻, 维持 tty/ldisc/hci0 存活 */
			setsid();
			{
				int dn23 = open("/dev/null", O_RDWR);
				if (dn23 >= 0) {
					dup2(dn23, 0); dup2(dn23, 1); dup2(dn23, 2);
					if (dn23 > 2) close(dn23);
				}
			}
			for (;;) pause();
		}
		goto tail;	/* 不可达 */
	}

	/* S8 hcd 逐块下载 (hcd 格式: op_lo op_hi len payload, 无 H4 头) */
	memset(&ic0, 0, sizeof ic0);
	ioctl(fd, TIOCGICOUNT, &ic0);
	{
		long p = 0;
		int blk = 0, ok = 0, mism = 0, bad = 0, badshow = 0, mismshow = 0, sk;
		long long t_start = now_us();
		unsigned char h4buf[1200];
		while (p + 3 <= hcd_len) {
			int op, len2, total, got;
			op = hcd[p] | (hcd[p + 1] << 8);
			len2 = hcd[p + 2];
			if (len2 > 1020) {
				printf("[S8] 块长度异常 @%ld: op=0x%04x len=%d, 中止\n", p, op, len2);
				break;
			}
			total = 4 + len2;
			if (p + 3 + (long)len2 > hcd_len) {
				printf("[S8] 截断块 @%ld (remain=%ld), 中止\n", p, hcd_len - p);
				break;
			}
			if (gap_ms > 0)
				usleep(gap_ms * 1000);
			h4buf[0] = 0x01;
			h4buf[1] = hcd[p];
			h4buf[2] = hcd[p + 1];
			h4buf[3] = (unsigned char)len2;
			memcpy(h4buf + 4, hcd + p + 3, len2);
			if (write(fd, h4buf, total) < 0)
				perror("write");
			sk = 0;
			got = read_event(fd, rx, sizeof rx, 1500, &sk);
			blk++;
			if (got < 0) {
				bad++;
				if (badshow < 8) {
					printf("  [S8] #%d op=0x%04x len=%d -> 超时无应答 (跳垃圾%dB)\n",
						blk, op, len2, sk);
					badshow++;
				}
			} else if (rx[1] != 0x0e || (rx[4] | (rx[5] << 8)) != op) {
				mism++;
				if (mismshow < 8) {
					printf("  [S8] #%d op=0x%04x len=%d -> 错位事件 %d B:", blk, op, len2, got);
					for (i = 0; i < got && i < 12; i++) printf(" %02x", rx[i]);
					printf(" (跳垃圾%dB)\n", sk);
					mismshow++;
				}
			} else {
				ok++;
				if (blk <= 3)
					printf("  [S8] #%d op=0x%04x len=%d -> CC ok\n", blk, op, len2);
			}
			if ((bad + mism) > 20) {
				printf("  [S8] 失步过多 (bad=%d mism=%d), 提前中止\n", bad, mism);
				break;
			}
			if (blk % 32 == 0) {
				printf("  [S8] 进度 %d 块 (ok=%d), 用时 %.1fs\n", blk, ok, (now_us() - t_start) / 1e6);
				fflush(stdout);
			}
			p += 3 + (long)len2;
		}
		printf("[S8] 完成: %d 块, ok=%d 错位=%d 无应答=%d, 用时 %.1fs\n",
			blk, ok, mism, bad, (now_us() - t_start) / 1e6);
	}
	memset(&ic1, 0, sizeof ic1);
	ioctl(fd, TIOCGICOUNT, &ic1);
	printf("[S8] icount delta: rx=%d frame=%d overrun=%d brk=%d buf_over=%d\n",
		ic1.rx - ic0.rx, ic1.frame - ic0.frame, ic1.overrun - ic0.overrun,
		ic1.brk - ic0.brk, ic1.buf_overrun - ic0.buf_overrun);

tail:
	/* S9 等待固件就绪 (1s, 参照 RPi 社区补丁) */
	usleep(1000000);

	/* S10 Reset @target */
	step(fd, "S10 Reset@target(下载后)", reset, 4, 2000, 60, rx, sizeof rx);

	/* S11 Read Local Version @target */
	step(fd, "S11 ReadLocalVersion(下载后)", rdver, 4, 2000, 60, rx, sizeof rx);

	/* S12 速率探测 */
	{
		unsigned int probe[] = { target_baud, init_baud, 115200, 57600, 230400 };
		int np = (int)(sizeof probe / sizeof probe[0]);
		int pi;
		printf("\n=== S12 速率探测 (host 切速 -> Reset -> 记录应答) ===\n");
		for (pi = 0; pi < np; pi++) {
			char d[128];
			if (set_baud(fd, probe[pi], flow) < 0)
				continue;
			clear_rx(fd);
			if (write(fd, reset, 4) < 0)
				perror("write");
			n = drain(fd, rx, sizeof rx, 500, 60);
			hd("probe", rx, n > 8 ? 8 : n);
			printf("             -> @%-7u %s\n", probe[pi], evdesc(rx, n, d, sizeof d));
		}
	}

	printf("\n[%9.1fms] 结束\n", ms());
	close(fd);
	return 0;
}
