# P2PDemo
a p2p demo with libdatachannel

## wsServ
signaling server

## client
a p2p demo client

## web
a p2p web demo client

## Build

```bash
#  dependency
brew install openssl
# 解压deps/src/libdatachannel-0.23.1.tar.bz2到压缩包的同级目录
cd deps/src/
tar -xjf libdatachannel-0.23.1.tar.bz2
# 回到项目根目录
cd ../../
mkdir build
cd build
cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ..  
make -j8
``` 
