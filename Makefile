
ldflags := -L. -Wl,--whole-archive -lluaobj -Wl,--no-whole-archive -Wl,-E

cflags += -g

ldflags += $(shell pkg-config --libs luajit)
cflags += $(shell pkg-config --cflags luajit)

ldflags += -L/opt/libuv/lib
ldflags += -luv -Wl,-rpath -Wl,/opt/libuv/lib -Wl,-E
cflags += -I/opt/libuv/include

luasrc += main.lua server.lua 
luasrc += native.lua native_h.lua

luaobj += $(patsubst %.lua,%_lua.o,${luasrc})

cobj := main.o strbuf.o

all: server

test:

clean:
	rm -rf *.h.lua *.o server

%.o: %.c
	${CC} -c -o $@ $< ${cflags}

%_h.lua: %.h
	echo "return [[" > $@
	cat $< >> $@
	echo "]]" >> $@

%_lua.o: %.lua
	luajit -bg $< $@

libluaobj.a: ${luaobj}
	rm -rf $@
	ar rcus $@ ${luaobj}

server: ${cobj} libluaobj.a
	${CC} -o $@ ${cobj} ${ldflags}

