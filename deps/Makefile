
target = lib/libdatachannel.a include/nlohmann

all: $(target)
	@echo done

lib/libdatachannel.a: src/libdatachannel-0.23.1/build/libdatachannel.a
	cd src/libdatachannel-0.23.1/build; cmake --install . --prefix $(shell pwd)

include/nlohmann: src/libdatachannel-0.23.1/deps/json/single_include/nlohmann
	cp -r src/libdatachannel-0.23.1/deps/json/single_include/nlohmann include/

src/libdatachannel-0.23.1/build/libdatachannel.a: src/libdatachannel-0.23.1/build/Makefile
	cd src/libdatachannel-0.23.1/build; cmake --build .

src/libdatachannel-0.23.1/build/Makefile: src/libdatachannel-0.23.1/CMakeLists.txt
	cd src/libdatachannel-0.23.1; cmake -B build -DUSE_GNUTLS=0 -DUSE_NICE=0 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=0

src/libdatachannel-0.23.1/CMakeLists.txt: src/libdatachannel-0.23.1.tar.bz2
	cd src; tar jxvf libdatachannel-0.23.1.tar.bz2
