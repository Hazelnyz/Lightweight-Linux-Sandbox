.RECIPEPREFIX = >
CC ?= gcc
CFLAGS = -Wall -Wextra -O2

.PHONY: all build test demo report clean

all: build

minibox: minibox.c
> $(CC) $(CFLAGS) -o minibox minibox.c

stress: stress.c
> $(CC) -O2 -static -o stress stress.c

build: minibox stress
> cp stress rootfs/bin/stress

test: build
> sudo ./minibox run --mem 64M --name memtest /bin/stress mem 300
> sudo ./minibox run --pids 20 --name forktest /bin/stress fork 100
> sudo ./minibox run --cpus 0.5 --name cpu50 /bin/sh -c "timeout 5 /bin/stress cpu; true"

demo: build
> bash scripts/demo.sh

report:
> python3 tools/make_report.py

clean:
> rm -f minibox stress minibox-*.csv
> rm -rf results
