# Compile eBPF code with the makefile from bpf-examples
eBPF_DIR	:= ./

BPF_TARGETS	:= $(eBPF_DIR)/main $(eBPF_DIR)/counted
BPF_HEADERS	:= $(wildcard $(eBPF_DIR)/*.h) ../../../common/intf/intf_ebpf.h ../../../common/intf/capacity.h ../../../common/tran_def/tcp.h ../*.h main.c

BPF_CFLAGS := -Wno-address-of-packed-member -I../../../intf -I../../../common/

LIB_DIR = ../../../../lib
include $(LIB_DIR)/common.mk

main.o: BPF_CFLAGS += -DETRAN_STATIC_QID_STATS=0
