ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS} -I m4

SUBDIRS = .

AM_CPPFLAGS += \
-I$(srcdir)/../../include \
-I$(srcdir)/../pna_nic \
-I$(srcdir)/../simple_switch \
-I$(srcdir)/../../PI \
-isystem$(srcdir)/../../third_party/spdlog

if WITH_THRIFT
AM_CPPFLAGS += -DWITH_THRIFT \
-I$(builddir)/../pna_nic/gen-cpp
endif

bin_PROGRAMS = pna_nic_grpc

pna_nic_grpc_SOURCES = main.cpp

pna_nic_grpc_LDADD = \
libpna_nic_grpc.la

# We follow this tutorial to link with grpc++_reflection:
# https://github.com/grpc/grpc/blob/master/doc/server_reflection_tutorial.md
pna_nic_grpc_LDFLAGS = \
-Wl,--no-as-needed,-lgrpc++_reflection,--as-needed \
-rdynamic

noinst_LTLIBRARIES = libpna_nic_grpc.la

libpna_nic_grpc_la_SOURCES = \
switch_runner.cpp switch_runner.h

libpna_nic_grpc_la_LIBADD = \
$(builddir)/../pna_nic/libpnanic.la \
$(builddir)/../simple_switch/libsimpleswitch.la \
$(builddir)/../../PI/libbmpi.la

if WITH_THRIFT
libpna_nic_grpc_la_LIBADD += \
$(builddir)/../../src/bm_runtime/libbmruntime.la \
$(builddir)/../../thrift_src/libruntimestubs.la \
$(builddir)/../pna_nic/libpnanic_thrift.la
endif

libpna_nic_grpc_la_LIBADD += \
-lpifeproto -lpigrpcserver -lpi -lpip4info \
$(GRPC_LIBS) $(PROTOBUF_LIBS)
