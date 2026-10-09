/*
 * latprobe: camera-side latency probe for OpenIPC cameras.
 *
 * Runs on the camera. Steps one sensor register over I2C (by default IMX335
 * analog gain, 0x30E8) and timestamps, on the camera's own clock:
 *   S  each register write
 *   R  each access unit arriving over loopback RTSP (RTP over TCP)
 *   W  each /ws/video (MSE) fragment, with the prft capture stamp majestic adds
 * It also saves the RTSP elementary stream, so a host can decode it and find
 * the first frame the step changed. Everything shares CLOCK_REALTIME, so no
 * clock sync with the host is needed and the network between them does not
 * enter the numbers.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int tcp(int port)
{
	int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port),
				.sin_addr.s_addr = htonl(0x7f000001)};
	setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	if (connect(s, (void *)&a, sizeof a)) {
		perror("connect");
		exit(1);
	}
	return s;
}

static char hdr[8192];
static char body[8192];

/* Send a request, read the header and any Content-Length body into hdr/body. */
static void req(int s, const char *r)
{
	if (write(s, r, strlen(r)) < 0)
		exit(2);
	int n = 0;
	while (n < (int)sizeof hdr - 1) {
		if (read(s, hdr + n, 1) != 1)
			exit(2);
		hdr[++n] = 0;
		if (n >= 4 && !memcmp(hdr + n - 4, "\r\n\r\n", 4))
			break;
	}
	body[0] = 0;
	char *cl = strcasestr(hdr, "Content-Length:");
	if (cl) {
		int len = atoi(cl + 15), got = 0;
		char sink[512];
		while (got < len) {
			int room = (int)sizeof body - 1 - got;
			int want = len - got;
			char *dst = room > 0 ? body + got : sink;
			if (room <= 0)
				room = sizeof sink;
			int r = read(s, dst, want < room ? want : room);
			if (r <= 0)
				exit(3);
			got += r;
		}
		body[len < (int)sizeof body - 1 ? len : (int)sizeof body - 1] = 0;
	}
}

static int hevc;
static int udp_port; /* 0: RTP interleaved on the RTSP connection; else RTP to this loopback port */
static int udp_fd = -1;

static int rtsp_open(const char *path, const char *auth)
{
	int s = tcp(554);
	char r[1024], sess[128] = "", base[256], au[300] = "";
	if (*auth)
		snprintf(au, sizeof au, "Authorization: Basic %s\r\n", auth);
	snprintf(r, sizeof r, "DESCRIBE rtsp://127.0.0.1/%s RTSP/1.0\r\nCSeq: 1\r\n%sAccept: application/sdp\r\n\r\n", path, au);
	req(s, r);
	if (!strstr(hdr, " 200 ")) {
		fprintf(stderr, "DESCRIBE failed: %.80s\n", hdr);
		exit(1);
	}
	hevc = strcasestr(body, "H265/") != NULL;
	char *cb = strcasestr(hdr, "Content-Base:");
	if (cb)
		sscanf(cb + 13, " %255s", base);
	else
		snprintf(base, sizeof base, "rtsp://127.0.0.1/%s/", path);
	char *ctl = strstr(body, "m=video"), track[128] = "trackID=0";
	if (ctl && (ctl = strstr(ctl, "a=control:")))
		sscanf(ctl + 10, "%127[^\r\n]", track);
	char transport[96] = "RTP/AVP/TCP;unicast;interleaved=0-1";
	if (udp_port) {
		udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
		/* FORCE: SO_RCVBUF is capped at net.core.rmem_max, 160 KB on these
		 * cameras, and a frame sent while this process waits for the core
		 * overflowed it. A lost datagram leaves a partial access unit,
		 * which is skipped in the R lines but still decoded, and shifts
		 * every later frame's luma onto the wrong timestamp. */
		int big = 4 << 20;
		if (setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof big))
			setsockopt(udp_fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
		struct sockaddr_in ua = {.sin_family = AF_INET, .sin_port = htons(udp_port),
					 .sin_addr.s_addr = htonl(0x7f000001)};
		if (bind(udp_fd, (void *)&ua, sizeof ua)) {
			perror("bind udp");
			exit(1);
		}
		snprintf(transport, sizeof transport, "RTP/AVP;unicast;client_port=%d-%d", udp_port, udp_port + 1);
	}
	if (!strncmp(track, "rtsp://", 7))
		snprintf(r, sizeof r, "SETUP %s RTSP/1.0\r\nCSeq: 2\r\n%sTransport: %s\r\n\r\n", track, au, transport);
	else
		snprintf(r, sizeof r, "SETUP %s%s%s RTSP/1.0\r\nCSeq: 2\r\n%sTransport: %s\r\n\r\n",
			 base, base[strlen(base) - 1] == '/' ? "" : "/", track, au, transport);
	req(s, r);
	char *se = strcasestr(hdr, "Session:");
	if (se)
		sscanf(se + 8, " %127[^;\r\n]", sess);
	snprintf(r, sizeof r, "PLAY rtsp://127.0.0.1/%s RTSP/1.0\r\nCSeq: 3\r\n%sSession: %s\r\nRange: npt=0.000-\r\n\r\n", path, au, sess);
	req(s, r);
	fprintf(stderr, "rtsp: %s, PLAY %.15s\n", hevc ? "H.265" : "H.264", hdr);
	printf("C %s\n", hevc ? "hevc" : "h264");
	return s;
}

static int ws_open(int stream)
{
	int s = tcp(80);
	char r[512];
	snprintf(r, sizeof r, "GET /ws/video?stream=%d HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
		 "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", stream);
	req(s, r);
	fprintf(stderr, "ws: %.32s\n", hdr);
	return s;
}

static uint8_t rb[1 << 20], wb[1 << 22];
static int rn, wn;
static FILE *es;
static const uint8_t sc[4] = {0, 0, 0, 1};
static uint32_t au_size, au_ts;
static double au_first;
static int au_key, au_open;

static void nal_out(const uint8_t *p, int n)
{
	fwrite(sc, 1, 4, es);
	fwrite(p, 1, n, es);
}

/* Depacketize one RTP packet into Annex B and account it to its access unit. */
static void rtsp_pkt(uint8_t *p, int len, double t)
{
	if (len < 13)
		return;
	int cc = p[0] & 15, m = p[1] >> 7, x = p[0] & 16;
	uint32_t ts = (uint32_t)p[4] << 24 | p[5] << 16 | p[6] << 8 | p[7];
	uint8_t *pl = p + 12 + 4 * cc;
	int pll = len - 12 - 4 * cc;
	if (x && pll >= 4) {
		int xl = 4 + 4 * (pl[2] << 8 | pl[3]);
		pl += xl;
		pll -= xl;
	}
	if (pll < 2)
		return;
	if (au_open && ts != au_ts) {
		printf("R %.6f %.6f %u %u %d 0\n", au_first, t, au_ts, au_size, au_key);
		au_open = 0;
	}
	if (!au_open) {
		au_open = 1;
		au_size = 0;
		au_key = 0;
		au_ts = ts;
		au_first = t;
	}
	if (!hevc) {
		int nt = pl[0] & 31;
		if (nt >= 1 && nt <= 23) {
			nal_out(pl, pll);
		} else if (nt == 28 && pll > 2) {
			nt = pl[1] & 31;
			if (pl[1] & 0x80) {
				uint8_t h = (pl[0] & 0xe0) | nt;
				nal_out(&h, 1);
			}
			fwrite(pl + 2, 1, pll - 2, es);
		} else if (nt == 24) {
			for (int o = 1; o + 2 < pll;) {
				int n = pl[o] << 8 | pl[o + 1];
				o += 2;
				if (o + n > pll)
					break;
				if ((pl[o] & 31) == 5 || (pl[o] & 31) == 7)
					au_key = 1;
				nal_out(pl + o, n);
				o += n;
			}
		}
		if (nt == 5 || nt == 7)
			au_key = 1;
	} else {
		int nt = (pl[0] >> 1) & 63;
		if (nt < 48) {
			nal_out(pl, pll);
		} else if (nt == 49 && pll > 3) {
			nt = pl[2] & 63;
			if (pl[2] & 0x80) {
				uint8_t h[2] = {(uint8_t)((pl[0] & 0x81) | nt << 1), pl[1]};
				nal_out(h, 2);
			}
			fwrite(pl + 3, 1, pll - 3, es);
		} else if (nt == 48) {
			for (int o = 2; o + 2 < pll;) {
				int n = pl[o] << 8 | pl[o + 1];
				o += 2;
				if (o + n > pll)
					break;
				int t2 = (pl[o] >> 1) & 63;
				if ((t2 >= 16 && t2 <= 21) || t2 == 32)
					au_key = 1;
				nal_out(pl + o, n);
				o += n;
			}
		}
		if ((nt >= 16 && nt <= 21) || nt == 32)
			au_key = 1;
	}
	au_size += pll;
	if (m) {
		printf("R %.6f %.6f %u %u %d 1\n", au_first, t, au_ts, au_size, au_key);
		au_open = 0;
	}
}

static void rtsp_data(int s)
{
	int r = read(s, rb + rn, sizeof rb - rn);
	if (r <= 0)
		exit(4);
	rn += r;
	double t = now();
	int o = 0;
	for (;;) {
		if (rn - o < 4)
			break;
		if (rb[o] != '$') { /* an RTSP reply interleaved with the data */
			uint8_t *d = memchr(rb + o + 1, '$', rn - o - 1);
			if (!d) {
				o = rn;
				break;
			}
			o = d - rb;
			continue;
		}
		int len = rb[o + 2] << 8 | rb[o + 3];
		if (rn - o < 4 + len)
			break;
		if (rb[o + 1] == 0)
			rtsp_pkt(rb + o + 4, len, t);
		o += 4 + len;
	}
	memmove(rb, rb + o, rn - o);
	rn -= o;
}

/* Drain to EAGAIN rather than one datagram per poll(). */
static void udp_data(int s)
{
	static uint8_t d[65536];
	for (;;) {
		ssize_t n = recv(s, d, sizeof d, MSG_DONTWAIT);
		if (n <= 0)
			break;
		rtsp_pkt(d, (int)n, now());
	}
}

static void ws_data(int s)
{
	int r = read(s, wb + wn, sizeof wb - wn);
	if (r <= 0)
		exit(5);
	wn += r;
	double t = now();
	int o = 0;
	for (;;) {
		if (wn - o < 2)
			break;
		uint8_t *f = wb + o;
		uint64_t len = f[1] & 127;
		int h = 2;
		if (len == 126) {
			if (wn - o < 4)
				break;
			len = f[2] << 8 | f[3];
			h = 4;
		} else if (len == 127) {
			if (wn - o < 10)
				break;
			len = 0;
			for (int i = 0; i < 8; i++)
				len = len << 8 | f[2 + i];
			h = 10;
		}
		if ((uint64_t)(wn - o) < h + len)
			break;
		uint8_t *m = f + h;
		int op = f[0] & 15;
		if (op == 2 && len >= 24 && !memcmp(m + 4, "prft", 4)) {
			uint32_t sec = (uint32_t)m[16] << 24 | m[17] << 16 | m[18] << 8 | m[19];
			uint32_t fr = (uint32_t)m[20] << 24 | m[21] << 16 | m[22] << 8 | m[23];
			printf("W %.6f %llu %.6f\n", t, (unsigned long long)len, (double)sec - 2208988800.0 + fr / 4294967296.0);
		} else if (op == 2) {
			printf("W %.6f %llu 0\n", t, (unsigned long long)len);
		}
		o += h + len;
	}
	memmove(wb, wb + o, wn - o);
	wn -= o;
}

static int i2c, addr16 = 1;
static unsigned reg = 0x30e8;

static int reg_write(int v)
{
	uint8_t b[3];
	int n = 0;
	if (addr16)
		b[n++] = reg >> 8;
	b[n++] = reg & 0xff;
	b[n++] = v;
	return write(i2c, b, n) == n ? 0 : -1;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: latprobe [options]\n"
		"  -d SEC     run time (default 45)\n"
		"  -b BUS     I2C bus number (default 0)\n"
		"  -a ADDR    sensor 7-bit I2C address (default 0x1a, IMX335)\n"
		"  -r REG     register to step (default 0x30e8, IMX335 analog gain low byte)\n"
		"  -8         register address is 8-bit (default 16-bit)\n"
		"  -v VAL     value written for the bright half of each step (default 0x3c)\n"
		"  -s N       stream number: rtsp://127.0.0.1/stream=N and /ws/video?stream=N (default 0)\n"
		"  -u B64     RTSP Basic auth, base64 of user:password (default none)\n"
		"  -W         skip the /ws/video client\n"
		"  -U PORT    take RTP over UDP on loopback PORT (and PORT+1) instead of interleaved TCP\n"
		"  -o FILE    elementary stream output (default /tmp/latprobe.es)\n"
		"  -p MS      minimum gap between steps in ms (default 800; a random 0-400 ms is added)\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int dur = 45, bus = 0, addr = 0x1a, hi = 0x3c, stream = 0, usews = 1, gap = 800, c;
	const char *out = "/tmp/latprobe.es", *auth = "";
	while ((c = getopt(argc, argv, "d:b:a:r:8v:s:u:WU:o:p:h")) != -1) {
		switch (c) {
		case 'd': dur = atoi(optarg); break;
		case 'b': bus = atoi(optarg); break;
		case 'a': addr = strtol(optarg, 0, 0); break;
		case 'r': reg = strtoul(optarg, 0, 0); break;
		case '8': addr16 = 0; break;
		case 'v': hi = strtol(optarg, 0, 0); break;
		case 's': stream = atoi(optarg); break;
		case 'u': auth = optarg; break;
		case 'W': usews = 0; break;
		case 'U': udp_port = atoi(optarg); break;
		case 'o': out = optarg; break;
		case 'p': gap = atoi(optarg); break;
		default: usage();
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	char dev[32];
	snprintf(dev, sizeof dev, "/dev/i2c-%d", bus);
	i2c = open(dev, O_RDWR);
	if (i2c < 0) {
		perror(dev);
		return 1;
	}

	/* Read the register's current value so it can be put back on exit. */
	uint8_t a[2] = {reg >> 8, reg & 0xff}, orig = 0;
	struct i2c_msg msg[2] = {{addr, 0, addr16 ? 2 : 1, addr16 ? a : a + 1}, {addr, I2C_M_RD, 1, &orig}};
	struct i2c_rdwr_ioctl_data rd = {msg, 2};
	if (ioctl(i2c, I2C_RDWR, &rd) < 0) {
		perror("i2c read: is -b/-a right?");
		return 1;
	}
	if (ioctl(i2c, I2C_SLAVE_FORCE, addr) < 0) {
		perror("I2C_SLAVE_FORCE");
		return 1;
	}
	printf("I bus=%d addr=0x%02x reg=0x%04x orig=0x%02x hi=0x%02x\n", bus, addr, reg, orig, hi);

	es = fopen(out, "w");
	if (!es) {
		perror(out);
		return 1;
	}
	char path[32];
	snprintf(path, sizeof path, "stream=%d", stream);
	int rs = rtsp_open(path, auth), ws = usews ? ws_open(stream) : -1;
	double start = now(), next = start + 3;
	int v = orig;
	srand(time(0));
	while (now() - start < dur) {
		struct pollfd pf[3] = {{rs, POLLIN, 0}, {ws, POLLIN, 0}, {udp_fd, POLLIN, 0}};
		int to = (int)((next - now()) * 1000);
		if (to < 0)
			to = 0;
		poll(pf, 3, to);
		if (pf[2].revents)
			udp_data(udp_fd);
		if (pf[0].revents)
			rtsp_data(rs);
		if (usews && pf[1].revents)
			ws_data(ws);
		if (now() >= next) {
			v = v == hi ? orig : hi;
			double t0 = now();
			int w = reg_write(v);
			double t1 = now();
			printf("S %.6f %.6f %d %d\n", t0, t1, v, w);
			next = t1 + (gap + rand() % 400) / 1000.0;
		}
	}
	reg_write(orig);
	fclose(es);
	return 0;
}
