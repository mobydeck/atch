VERSION ?= dev
CC = gcc
CFLAGS = -g -O2 -W -Wall -I. -DPACKAGE_VERSION=\"$(VERSION)\"
LDFLAGS =
LIBS = -lutil

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  STATIC_FLAG =
else
  STATIC_FLAG = -static
endif

LIBOBJ = attach.o master.o atch.o atch_paths.o atch_session.o atch_cmd.o atch_cli_opts.o atch_cli_runtime.o
OBJ = main.o $(LIBOBJ)
SRC = main.c attach.c master.c atch.c atch_paths.c atch_session.c atch_cmd.c atch_cli_opts.c atch_cli_runtime.c

IMAGE = atch-builder
BUILDDIR ?= .

archs = amd64 arm64
arch ?= $(shell arch)

.DEFAULT_GOAL := atch

libatch.a: $(LIBOBJ)
	ar rcs $@ $(LIBOBJ)

atch: main.o libatch.a
	$(CC) -o $(BUILDDIR)/$@ $(STATIC_FLAG) $(LDFLAGS) main.o libatch.a $(LIBS)

atch.1.md: README.md scripts/readme2man.sh
	bash scripts/readme2man.sh $< > $@

atch.1: atch.1.md
	pandoc --standalone -t man $< -o $@

man: atch.1

clean:
	rm -f atch libatch.a $(OBJ) *.1.md *.c~

.PHONY: fmt
fmt:
	docker run --rm -v "$$PWD":/src -w /src alpine:latest sh -c "apk add --no-cache indent && indent -linux $(SRCS) && indent -linux $(SRCS)"

.PHONY: fmt-all
fmt-all:
	$(MAKE) fmt SRCS="*.c"


main.o: ./main.c ./atch_cli.h
attach.o: ./attach.c ./atch.h config.h
master.o: ./master.c ./atch.h config.h
atch.o: ./atch.c ./atch.h config.h ./atch_cli.h ./atch_paths.h ./atch_session.h ./atch_cmd.h ./atch_cli_opts.h ./atch_cli_runtime.h
atch_paths.o: ./atch_paths.c ./atch.h config.h ./atch_paths.h
atch_session.o: ./atch_session.c ./atch.h config.h ./atch_session.h
atch_cmd.o: ./atch_cmd.c ./atch_cmd.h
atch_cli_opts.o: ./atch_cli_opts.c ./atch.h config.h ./atch_cli_opts.h
atch_cli_runtime.o: ./atch_cli_runtime.c ./atch.h config.h ./atch_cli_opts.h ./atch_session.h

.PHONY: build-image
build-image:
	docker build -t $(IMAGE):$(arch) --platform linux/$(arch) -f build.dockerfile .

build-docker: build-image
	$(MAKE) clean
	docker run --rm -v "$$PWD":/src -e VERSION=$(VERSION) -w /src \
		--platform linux/$(arch) $(IMAGE):$(arch) ./build.sh

.PHONY: test
test: build-docker
	docker run --rm -v "$$PWD":/src \
		--platform linux/$(arch) $(IMAGE):$(arch) \
		sh /src/tests/test.sh /src/build/atch

.PHONY: release
release: man $(archs)

$(archs):
	mkdir -p release
	$(MAKE) build-docker arch=$@ VERSION=$(VERSION)
	export COPYFILE_DISABLE=true; \
	tar -czf ./release/atch-linux-$@.tgz README.md atch.1 -C ./build atch; \
