# rtsp_server —
#   本机验证:  make               → ./file_source (纯 C, 无 GStreamer 依赖)
#   板子生产者: make producer CC=aarch64-buildroot-linux-gnu-gcc \
#         CFLAGS="--sysroot=<SYS>" \
#         GST_CFLAGS="-I<SYS>/usr/include/gstreamer-1.0 -I<SYS>/usr/include/glib-2.0 -I<SYS>/usr/lib/glib-2.0/include" \
#         GST_LIBS="--sysroot=<SYS> -L<SYS>/usr/lib -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgmodule-2.0 -lgstapp-1.0 -lgstvideo-1.0 -lm -ldl"

CC      ?= gcc
CFLAGS  += -O2 -std=c11 -Wall -Wextra
LDLIBS   = -pthread

TARGET   = file_source
OBJS     = file_source.o rtsp_server.o nal.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

producer: producer.o rtsp_server.o nal.o onvif.o
	$(CC) $(CFLAGS) -o $@ $^ $(GST_LIBS) -pthread

producer.o: producer.c
	$(CC) $(CFLAGS) $(GST_CFLAGS) -c $<

onvif.o: onvif.c onvif.h
	$(CC) $(CFLAGS) -c $<

%.o: %.c nal.h rtsp_server.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o $(TARGET) producer
