# Cross-compile the probe with zig: one static binary for any ARMv7 OpenIPC camera,
# musl or glibc userland alike. `make docker` does it without a local zig.
TARGET ?= arm-linux-musleabihf
CPU    ?= cortex_a7
ZIG    ?= python3 -m ziglang

probe/latprobe: probe/latprobe.c
	$(ZIG) cc -target $(TARGET) -mcpu=$(CPU) -O2 -static -Wall -o $@ $<

image:
	docker build -t latency-probe .

docker: image
	docker run --rm -u $$(id -u):$$(id -g) -e HOME=/tmp -v $(CURDIR):/w latency-probe make probe/latprobe

clean:
	rm -f probe/latprobe

.PHONY: image docker clean
