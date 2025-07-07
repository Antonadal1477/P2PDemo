
OPT?=-O3

INCFLAGS = \
		   -I../deps/include \

LIBFLAGS = \
		   -L../deps/lib -ldatachannel -lsrtp2 -lusrsctp -ljuice \

PLT ?= $(shell uname)
ifeq ($(PLT), Darwin)
	OpenSslDir=$(shell brew --prefix openssl@3)
	INCFLAGS += -I$(OpenSslDir)/include
    LIBFLAGS += -L$(OpenSslDir)/lib -lssl -lcrypto
else
    LIBFLAGS += -lssl -lcrypto
endif	

targets = \
		  p2ptransfer \

all: $(targets)
	@echo Done

debug:
	make OPT=-g

p2ptransfer: Cert.o P2PTransfer.o
	g++ $(OPT) -o $@ $^ $(LIBFLAGS)

%.o : %.c
	gcc $(OPT) -c -o $@ $(INCFLAGS) $<

%.o : %.cpp
	g++ -std=c++17 $(OPT) -c -o $@ $(INCFLAGS) $<

clean:
	rm -rf *.o $(targets)
