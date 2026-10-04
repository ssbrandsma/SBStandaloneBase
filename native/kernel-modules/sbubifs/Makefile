obj-m := sbubifs.o

sbubifs-y += shrinker.o journal.o file.o dir.o super.o sb.o io.o
sbubifs-y += tnc.o master.o scan.o replay.o log.o commit.o gc.o orphan.o
sbubifs-y += budget.o find.o tnc_commit.o compress.o lpt.o lprops.o
sbubifs-y += recovery.o ioctl.o lpt_commit.o tnc_misc.o

# The target Logitech configuration disables UBIFS debug and xattr support.
# Do not silently build a profile that differs from the running kernel.

