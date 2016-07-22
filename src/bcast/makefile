current:
	echo make pd_linux, pd_nt, pd_irix, pd_darwin

clean: ; rm -f *.pd_linux *.o

# ----------------------- NT -----------------------

pd_nt: bcreceive~.dll bcast~.dll

.SUFFIXES: .dll

PDNTCFLAGS = /W3 /WX /DNT /D_WINDOWS /DPD /nologo
VC="C:\Programme\Microsoft Visual Studio\VC98"

PDNTINCLUDE = /I. /Ic:\pd\tcl\include /Ic:\pd\src /I$(VC)\include

PDNTLDIR = $(VC)\Lib
PDNTLIB = $(PDNTLDIR)\libc.lib \
	$(PDNTLDIR)\oldnames.lib \
	$(PDNTLDIR)\kernel32.lib \
	$(PDNTLDIR)\user32.lib \
	$(PDNTLDIR)\uuid.lib \
	$(PDNTLDIR)\ws2_32.lib \
	c:\pd\bin\pthreadVC.lib \
	c:\pd\bin\pd.lib

.c.dll:
	cl $(PDNTCFLAGS) $(PDNTINCLUDE) /c netreceive~.c
	cl $(PDNTCFLAGS) $(PDNTINCLUDE) /c netsend~.c
	link /dll /export:netreceive_tilde_setup netreceive~.obj $(PDNTLIB)
	link /dll /export:netsend_tilde_setup netsend~.obj $(PDNTLIB)


# -------------------- IRIX 6.x (GCC) ---------------------

pd_irix6: bcreceive~.pd_irix6 bcast~.pd_irix6

.SUFFIXES: .pd_irix6

# adjust path to PD:
SGIINCLUDE =  -I../../src

SGICFLAGS6 = -mabi=n32 -DPD -DUNIX -DIRIX -DN32 -O3 \
    -funroll-loops -fomit-frame-pointer \
    -Wall -W -Wshadow -Werror \
    -Wno-unused -Wno-parentheses -Wno-switch -mips4

.c.pd_irix6:
	gcc $(SGICFLAGS6) $(SGIINCLUDE) -o $*.o -c $*.c
	ld -n32 -IPA -shared -rdata_shared -o $*.pd_irix6 $*.o
	rm $*.o

# ----------------------- MAX OS X -------------------

pd_darwin: bcreceive~.pd_darwin bcast~.pd_darwin

.SUFFIXES: .pd_darwin

DARWINCFLAGS = -DPD -DUNIX -DMACOSX -O2 \
    -Wall -W \
    -Wno-unused -Wno-parentheses -Wno-switch

#DARWININCLUDE =  -I../../src -Iinclude

.c.pd_darwin:
	cc $(DARWINCFLAGS) $(DARWININCLUDE) -o $*.o -c $*.c
	cc -bundle -undefined suppress -flat_namespace -o $*.pd_darwin $*.o
	rm -f $*.o ../$*.pd_darwin
	ln -s $*/$*.pd_darwin ..

# ----------------------- LINUX ARMv7 -----------------------

pd_linux: bcreceive~.pd_linux bcast~.pd_linux

.SUFFIXES: .pd_linux

LINUXCFLAGS = -DPD -DUNIX -DHAVE_LRINT -DHAVE_LRINTF -O2 \
	-funroll-loops -fPIC -fomit-frame-pointer \
	-march=armv7ve -mtune=cortex-a7 -ftree-vectorize \
	-mfpu=neon -mfloat-abi=hard \
	-Wall -W -Wshadow -Wstrict-prototypes \
	-Wno-unused -Wno-parentheses -Wno-switch

LINUXINCLUDE =  -I/usr/include/pd

.c.pd_linux:
	arm-linux-gnueabihf-gcc $(LINUXCFLAGS) $(LINUXINCLUDE) -o $*.o -c $*.c
	ld -export_dynamic -shared -o $*.pd_linux $*.o -lc -lm
	#strip --strip-unneeded $*.pd_linux
	rm $*.o

