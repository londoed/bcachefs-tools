
PREFIX=/usr
INSTALL=install
CFLAGS+=-std=gnu89 -O2 -g -MMD -Wall				\
	-Wno-pointer-sign					\
	-fno-strict-aliasing					\
	-I. -Iinclude						\
	-D_FILE_OFFSET_BITS=64					\
	-D_GNU_SOURCE						\
	-D_LGPL_SOURCE						\
	-DRCU_MEMBARRIER					\
	-DNO_BCACHEFS_CHARDEV					\
	-DNO_BCACHEFS_FS					\
	-DNO_BCACHEFS_SYSFS					\
	$(EXTRA_CFLAGS)
LDFLAGS+=$(CFLAGS)

CC_VERSION=$(shell $(CC) -v 2>&1|grep -E '(gcc|clang) version')

ifneq (,$(findstring gcc,$(CC_VERSION)))
	CFLAGS+=-Wno-unused-but-set-variable
endif

ifneq (,$(findstring clang,$(CC_VERSION)))
	CFLAGS+=-Wno-missing-braces
endif

ifdef D
	CFLAGS+=-Werror
	CFLAGS+=-DCONFIG_BCACHEFS_DEBUG=y
endif

PKGCONFIG_LIBS="blkid uuid liburcu libsodium zlib"
CFLAGS+=`pkg-config --cflags	${PKGCONFIG_LIBS}`
LDLIBS+=`pkg-config --libs	${PKGCONFIG_LIBS}` 		\
	-lm -lpthread -lrt -lscrypt -lkeyutils -laio

ifeq ($(PREFIX),/usr)
	ROOT_SBINDIR=/sbin
else
	ROOT_SBINDIR=$(PREFIX)/sbin
endif

.PHONY: all
all: bcachefs

SRCS=$(shell find . -type f -iname '*.c')
DEPS=$(SRCS:.c=.d)
-include $(DEPS)

OBJS=$(SRCS:.c=.o)
bcachefs: $(OBJS)

.PHONY: install
install: bcachefs
	mkdir -p $(DESTDIR)$(ROOT_SBINDIR)
	mkdir -p $(DESTDIR)$(PREFIX)/share/man/man8/
	$(INSTALL) -m0755 bcachefs	$(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0755 fsck.bcachefs	$(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0755 mkfs.bcachefs	$(DESTDIR)$(ROOT_SBINDIR)
	$(INSTALL) -m0644 bcachefs.8	$(DESTDIR)$(PREFIX)/share/man/man8/

.PHONY: clean
clean:
	$(RM) bcachefs $(OBJS) $(DEPS)

.PHONY: deb
deb: all
	debuild --unsigned-source	\
		--unsigned-changes	\
		--no-pre-clean		\
		--build=binary		\
		--diff-ignore		\
		--tar-ignore

.PHONE: update-bcachefs-sources
update-bcachefs-sources:
	git rm -rf --ignore-unmatch libbcachefs
	cp $(LINUX_DIR)/fs/bcachefs/*.[ch] libbcachefs/
	cp $(LINUX_DIR)/include/trace/events/bcachefs.h include/trace/events/
	echo `cd $(LINUX_DIR); git rev-parse HEAD` > .bcachefs_revision
	git add libbcachefs/*.[ch] include/trace/events/bcachefs.h .bcachefs_revision

.PHONE: update-commit-bcachefs-sources
update-commit-bcachefs-sources: update-bcachefs-sources
	git commit -m "Update bcachefs sources to `cd $(LINUX_DIR); git show --oneline --no-patch`"
