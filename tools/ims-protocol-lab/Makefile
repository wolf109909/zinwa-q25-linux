CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror

.PHONY: all check check-sanitize verify-provenance clean

all: build/libims_channel_frame.a

build:
	mkdir -p $@

build/ims_channel_frame.o: ims_channel_frame.c ims_channel_frame.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c $< -o $@

build/libims_channel_frame.a: build/ims_channel_frame.o
	$(AR) rcs $@ $<

build/test_ims_channel_frame: test_ims_channel_frame.c ims_channel_frame.h build/libims_channel_frame.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< build/libims_channel_frame.a -o $@

check: build/test_ims_channel_frame
	./build/test_ims_channel_frame

check-sanitize: | build
	$(CC) $(CPPFLAGS) $(WARNINGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer ims_channel_frame.c test_ims_channel_frame.c -o build/test_ims_channel_frame_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_ims_channel_frame_sanitized

# Optional static evidence check; never loads or executes the supplied DSO.
verify-provenance:
	python3 -B verify_provenance.py "$(VENDOR_LIB)"

clean:
	rm -rf build
