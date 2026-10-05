CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
SHLIB_EXT := $(if $(filter Darwin,$(shell uname -s)),dylib,so)

OBJS = mvparse.o h264_cabac_tables.o
REC_OBJS = options.o motion.o mvmap_writer.o session.o log.o
CAM_OBJS = main.o rtsp.o md5.o mp4_reader.o live.o rtp_h264.o mp4_writer.o h264_sps.o
LDLIBS = -lz -lm

all: rtspcam mvdump libmvparse.$(SHLIB_EXT) librcsession.$(SHLIB_EXT)

%.o: %.c *.h
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

rtspcam: $(CAM_OBJS) $(REC_OBJS) $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

mvdump: mvdump.o $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

libmvparse.$(SHLIB_EXT): $(OBJS)
	$(CC) $(CFLAGS) -shared -o $@ $^

# for c/test_session.py
librcsession.$(SHLIB_EXT): test_api.o $(REC_OBJS) $(OBJS) mp4_writer.o h264_sps.o live.o
	$(CC) $(CFLAGS) -shared -o $@ $^ $(LDLIBS)

clean:
	rm -f *.o rtspcam mvdump libmvparse.so libmvparse.dylib librcsession.so librcsession.dylib

.PHONY: all clean
