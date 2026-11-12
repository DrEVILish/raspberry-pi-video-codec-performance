# libcutepi-cfhd.so: the GoPro CineForm SDK (decoder and encoder) as one shared library for CuTePi's
# cutepicfhddec element (gsp/cfhd), which loads it at run time. Builds natively on x86-64 (the SDK's own SSE2) and
# on AArch64 (compat/: SSE2 through sse2neon, MMX through mmx2neon.h). No cmake needed.
#   make && sudo make install      (PREFIX=/usr/local -> $(PREFIX)/lib/cutepi/libcutepi-cfhd.so)
PREFIX ?= /usr/local
ARCH := $(shell uname -m)
SRCS := $(wildcard Codec/*.c Codec/*.cpp WarpLib/*.c DecoderSDK/*.cpp ConvertLib/*.cpp EncoderSDK/*.cpp) Common/ThreadMessage.cpp Common/MessageQueue.cpp Common/ThreadPool.cpp
OBJS := $(patsubst %,obj/%.o,$(SRCS))
FLAGS := -O3 -fPIC -fsigned-char -I Common -I Codec -I ConvertLib -I WarpLib -I DecoderSDK -I EncoderSDK -D__STDC_LIMIT_MACROS -D_REENTRANT -w -fpermissive
ifeq ($(ARCH),aarch64)
FLAGS += -I compat -flax-vector-conversions
endif
LIB := libcutepi-cfhd.so
all: $(LIB)
obj/%.c.o: %.c
	@mkdir -p $(dir $@); $(CC) $(FLAGS) -Drand=cutepi_rand -std=gnu11 -c $< -o $@
obj/%.cpp.o: %.cpp
	@mkdir -p $(dir $@); $(CXX) $(FLAGS) -std=gnu++14 -c $< -o $@
$(LIB): $(OBJS)
	$(CXX) -shared -o $@ $^ -lpthread -luuid -Wl,--no-undefined
install: $(LIB)
	install -D -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/cutepi/$(LIB)
clean:
	rm -rf obj $(LIB)
.PHONY: all install clean
