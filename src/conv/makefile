current:
	echo make pd_linux

clean: ; rm -f *.pd_linux *.o


# ----------------------- LINUX i386 -----------------------

pd_linux: conv~.pd_linux

.SUFFIXES: .pd_linux

LINUXCFLAGS = -DPD -DUNIX -DHAVE_LRINT -DHAVE_LRINTF -DINPLACE -DTHREADS -O2 \
	-funroll-loops -fPIC -fomit-frame-pointer \
	-ftree-vectorize -Wall -W -Wshadow -Wstrict-prototypes \
	-Wno-unused -Wno-parentheses -Wno-switch

LINUXINCLUDE =  -I/usr/include/pd

.c.pd_linux:
	cc $(LINUXCFLAGS) $(LINUXINCLUDE) -o $*.o -c $*.c
	ld -export_dynamic -shared -o $*.pd_linux $*.o -lc -lm -lfftw3f
	#strip --strip-unneeded $*.pd_linux
	rm $*.o

