VERSION ?= dev
CC = gcc
CFLAGS = -g -O2 -W -Wall -I. -DPACKAGE_VERSION=\"$(VERSION)\"
LDFLAGS =
LIBS = -lutil

PREFIX ?= /usr/local

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  STATIC_FLAG =
else
  STATIC_FLAG = -static
endif

OBJ = attach.o master.o atch.o
SRC = attach.c master.c atch.c

IMAGE = atch-builder
BUILDDIR ?= .

archs = amd64 arm64
arch ?= $(shell arch)

atch: $(OBJ)
	$(CC) -o $(BUILDDIR)/$@ $(STATIC_FLAG) $(LDFLAGS) $(OBJ) $(LIBS)

atch.1.md: README.md scripts/readme2man.sh
	bash scripts/readme2man.sh $< > $@

atch.1: atch.1.md
	pandoc --standalone -t man $< -o $@

man: atch.1

install: atch
	install -d $(PREFIX)/bin
	install -m 755 atch $(PREFIX)/bin/atch
	install -d $(PREFIX)/share/man/man1
	install -m 644 atch.1 $(PREFIX)/share/man/man1/atch.1

clean:
	rm -f atch $(OBJ) *.1.md *.c~

.PHONY: install fmt
fmt:
	docker run --rm -v "$$PWD":/src -w /src alpine:latest sh -c "apk add --no-cache indent && indent -linux $(SRCS) && indent -linux $(SRCS)"

.PHONY: fmt-all
fmt-all:
	$(MAKE) fmt SRCS="*.c"


attach.o: ./attach.c ./atch.h config.h
master.o: ./master.c ./atch.h config.h
atch.o: ./atch.c ./atch.h config.h

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
