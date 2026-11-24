
CC=cl.exe
RC=rc.exe

RCFLAGS= /R /Fo"trimcheck.res" "trimcheck.rc"
CFLAGS= /O1 /GF /GS- 
SOURCE= trimcheck.c trimcheck.res
LDFLAGS= /SUBSYSTEM:CONSOLE /OUT:trimcheck.exe /FIXED
LIBS=kernel32.lib advapi32.lib ole32.lib shell32.lib propsys.lib
DEL=trimcheck.obj trimcheck.res


all:
	$(RC) $(RCFLAGS)
	$(CC) $(CFLAGS) $(SOURCE) /link $(LIBS) $(LDFLAGS)
	@for %%i in ($(DEL)) do @if exist %%i del %%i

clean:
	@if exist trimcheck.exe del trimcheck.exe
	@if exist trimcheck.obj del trimcheck.obj
	@if exist trimcheck.res del trimcheck.res
