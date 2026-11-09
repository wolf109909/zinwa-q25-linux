CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror

.PHONY: all check check-sanitize verify-provenance verify-reg-provenance clean

all: build/libims_channel_frame.a build/libims_reg_request.a

build:
	mkdir -p $@

build/ims_channel_frame.o: ims_channel_frame.c ims_channel_frame.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c $< -o $@

build/libims_channel_frame.a: build/ims_channel_frame.o
	$(AR) rcs $@ $<

build/test_ims_channel_frame: test_ims_channel_frame.c ims_channel_frame.h build/libims_channel_frame.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< build/libims_channel_frame.a -o $@

build/ims_reg_request.o: ims_reg_request.c ims_reg_request.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -c $< -o $@

build/libims_reg_request.a: build/ims_reg_request.o
	$(AR) rcs $@ $<

build/test_ims_reg_request: test_ims_reg_request.c ims_reg_request.h ims_channel_frame.h build/libims_reg_request.a build/libims_channel_frame.a
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $< build/libims_reg_request.a build/libims_channel_frame.a -o $@

check: build/test_ims_channel_frame build/test_ims_reg_request
	./build/test_ims_channel_frame
	./build/test_ims_reg_request

check-sanitize: | build
	$(CC) $(CPPFLAGS) $(WARNINGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer ims_channel_frame.c test_ims_channel_frame.c -o build/test_ims_channel_frame_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_ims_channel_frame_sanitized
	$(CC) $(CPPFLAGS) $(WARNINGS) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer ims_reg_request.c ims_channel_frame.c test_ims_reg_request.c -o build/test_ims_reg_request_sanitized
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=halt_on_error=1 ./build/test_ims_reg_request_sanitized

# Optional static evidence checks; never load or execute vendor code.
verify-provenance:
	python3 -B verify_provenance.py "$(VENDOR_LIB)"

verify-reg-provenance:
	python3 -B verify_reg_provenance.py "$(STOCK_BIN_DIR)"

clean:
	rm -rf build
