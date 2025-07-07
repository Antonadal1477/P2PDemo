#include <condition_variable>
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <deque>
#include <map>
#include <cstdint>
#include <cstring>
#include <string>
#include <variant>
#include <memory>
#include <chrono>
#include <thread>
#include <mutex>
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <sys/stat.h>
#include "Cert.h"
using json = nlohmann::json;

using namespace std::chrono_literals;

enum MsgType {
    MsgTypeUnknown           = 0,
    MsgTypeOk                = 1,
    MsgTypeRegisterSdp       = 2,
    MsgTypeRegisterSdpReply  = 3,
    MsgTypeRegisterFile      = 4,
    MsgTypeRegisterFileReply = 5,
    MsgTypeQueryPeer         = 6,
    MsgTypeQueryPeerReply    = 7,
    MsgTypeQueryPeerAsk      = 8,
};

struct Event
{
    std::shared_ptr<json> msg;
    std::shared_ptr<json> src;
};

class P2PTransfer
{
public:
    P2PTransfer(const std::string& stunHost, uint16_t stunPort, const std::string& wsUrl);
    void send(const char* fn);
    void recv(uint64_t fid, const char* fn);
private:
    void start();
    void run();
    void runWebSkt();
    void runGather();

    void handleEvent(Event evt);
    void postEvent(Event evt);
    uint64_t postMsg(std::shared_ptr<json> msg);
    void handleQueryPeerReply(json& msg, json& src);
    void handleAsk(json& msg);
private:
    volatile uint64_t mPeerId;
    std::shared_ptr<std::thread> mWebSktThread;
    std::shared_ptr<std::thread> mGatherThread;
    CertificatePair mCert;
    rtc::Configuration mConfig;
    std::string mWsUrl;
    std::shared_ptr<rtc::WebSocket> mWebSkt;
    std::shared_ptr<rtc::PeerConnection> mPC;

    std::mutex mMtx;
    std::condition_variable mCv;
    std::string mDesc;
    std::deque<Event> mEvents;
    uint64_t mMsgId;
    std::deque<std::shared_ptr<json>> mPendingMsgs;
    std::map<uint64_t, std::shared_ptr<json>> mMsgs;
    std::atomic<uint64_t> mSentMsgId;

    std::map<uint64_t, std::string> mFiles;
};

P2PTransfer::P2PTransfer(const std::string& stunHost, uint16_t stunPort, const std::string& wsUrl):
    mPeerId(0),
    mWsUrl(wsUrl),
    mMsgId(0),
    mSentMsgId(0)
{
    auto cp = generate_ecdsa_certificate();
    std::cout << "cert:\n"<< cp.certPem << std::endl;
    std::cout << "key:\n"<< cp.keyPem << std::endl;
    mCert = cp;

    rtc::Configuration config;
    //rtc::IceServer serv("39.106.141.70", 8347);
    rtc::IceServer serv(stunHost, stunPort);
    serv.type = rtc::IceServer::Type::Stun;
    config.iceServers.push_back(serv);
    config.enableIceUdpMux = true;
    config.certificatePemFile = mCert.certPem;
    config.keyPemFile = mCert.keyPem;
    mConfig = config;

}

void P2PTransfer::send(const char* fn)
{
    start();

    struct stat st;
    int ret = stat(fn, &st);
    if (ret != 0) {
        std::cerr << "fail to get file info for: " << fn << std::endl;
        return;
    }

    auto msg = std::make_shared<json>(json{
        {"type", MsgTypeRegisterFile},
        {"payload", {
                        {"name", std::string(fn)},
                        {"size", (uint64_t)st.st_size},
                        {"mode", (unsigned)st.st_mode},
                    }
        },
    });
    mMtx.lock();
    mPendingMsgs.push_back(msg);
    mMtx.unlock();

    run();
}

void P2PTransfer::recv(uint64_t fid, const char* fn)
{
    start();

    auto msg = std::make_shared<json>(json{
        {"type", MsgTypeQueryPeer},
        {"payload", {
                        {"fid", fid},
                        {"name", std::string(fn ? fn : "")},
                    }
        },
    });
    mMtx.lock();
    mPendingMsgs.push_back(msg);
    mMtx.unlock();

    run();
}

void P2PTransfer::start()
{
    mWebSktThread = std::make_shared<std::thread>([&] {
        runWebSkt();
    });
    mGatherThread = std::make_shared<std::thread>([&] {
        runGather();
    });

    //while (mPeerId == 0) {
    //    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    //}
}

void P2PTransfer::run()
{
    std::deque<Event> evts;
    while (true) {
        do {
            std::unique_lock<std::mutex> lck(mMtx);
            while (mEvents.empty()) {
                mCv.wait(lck);
            }
            evts.swap(mEvents);
        } while (false);

        for (auto& evt : evts) {
            //try {
                handleEvent(evt);
            //} catch (std::exception& excp) {
            //    std::cout << "handleEvent excp:" << excp.what() << std::endl;
            //}
        }
        evts.clear();

        if (mPeerId > 0) {
            std::deque<std::shared_ptr<json>> msgs;
            mMtx.lock();
            if (!mPendingMsgs.empty()) {
                msgs.swap(mPendingMsgs);
            }
            mMtx.unlock();
            while (!msgs.empty()) {
                postMsg(msgs.front());
                msgs.pop_front();
            }
        }
    }
}

void P2PTransfer::handleEvent(Event evt)
{
    auto msg = evt.msg;
    auto it = msg->find("type");
    if (it == msg->end()) {
        return;
    }
    unsigned mtype = it.value();
    switch (mtype) {
    case MsgTypeRegisterSdpReply:
        if (evt.src) {
            uint64_t pid = (*evt.msg)["payload"]["pid"];
            mPeerId = pid;
            std::cout << "SetPeerId: " << pid << std::endl;
        }
        break;
    case MsgTypeRegisterFileReply:
        if (evt.src) {
            auto payload = (*msg)["payload"];
            uint64_t fid = payload["fid"];
            std::string fn = (*evt.src)["payload"]["name"];
            std::cout << "RegisterFile: " << fn << " Fid: " << fid << std::endl;
            std::unique_lock lck(mMtx);
            mFiles[fid] = fn;
        }
        break;
    case MsgTypeQueryPeerReply:
        if (evt.src) {
            //std::cout << "QueryPeerResp: " << evt.msg->dump() << std::endl;
            handleQueryPeerReply(*evt.msg, *evt.src);
        }
        break;
    case MsgTypeQueryPeerAsk:
        handleAsk(*evt.msg);
        break;
    default:
        break;
    }
}

void P2PTransfer::postEvent(Event evt)
{
    std::unique_lock lck(mMtx);
    mEvents.push_back(evt);
    mCv.notify_all();
}

void P2PTransfer::handleQueryPeerReply(json& msg, json& src)
{
    volatile bool ready = false;
    volatile bool finish = false;
    auto config = mConfig;
    auto payload = msg["payload"];
    uint64_t offset = 0;

    std::string fn = src["payload"]["name"];
    if (fn.empty()) {
        fn = payload["name"];
    }
    //std::ofstream out(fn, std::ios::binary|std::ios::app);

    config.iceUfrag = payload["ufrag"];
    config.icePwd = payload["pwd"];
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    std::shared_ptr<rtc::DataChannel> dc;
    dc = pc->createDataChannel("dc");

    dc->onOpen([&]() {
        std::cout << "DataChannel open:" << dc->label() << std::endl;
        struct stat st;
        int ret = stat(fn.c_str(), &st);
        if (ret == 0) {
            offset = st.st_size;
        }
        char msg[128];
        snprintf(msg, sizeof(msg), "ask:%llu", (unsigned long long)offset);
        dc->send(msg);
        ready = true;
    });

    pc->setRemoteDescription(std::string(payload["remoteSdp"]));
    while (!ready) {
        std::cout << "wait dc ready" << std::endl;
        std::this_thread::sleep_for(1s);
    }

    long long total = 0;
    long long lastTotal = 0;
    long long lastTime = 0;
    std::ofstream out(fn, std::ios::binary|std::ios::app);
    if (!out) {
        std::cout << "fail to create file:" << fn << std::endl;
        exit(1);
    }
    std::string recvEnd;
    while (recvEnd.empty()) {
        if (dc->availableAmount() > 0) {
            auto v = dc->receive();
            if (std::holds_alternative<std::string>(v.value())) {
                dc->send("end");
                recvEnd = std::get<std::string>(v.value());;
            } else {
                auto data = std::get<rtc::binary>(v.value());
                total += data.size();
                out.write((const char*)data.data(), data.size());
                out.flush();
            }
        } else {
            std::this_thread::sleep_for(2ms);
        }

        auto now = std::chrono::system_clock::now();
        long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count();
        if (lastTime + 1000 <= ms || !recvEnd.empty()) {
            long long e = (ms > lastTime ? (ms - lastTime) : 1);
            long long speed = (total - lastTotal) * 1000 / e;
            printf("recv total:%lld speed:%lld B/s\n", total, speed);
            fflush(stdout);
            lastTotal = total;
            lastTime = ms;
            char ack[128];
            snprintf(ack, sizeof(ack), "ack:%llu", total);
            dc->send(ack);
        }
    }
    std::cout << "recv " << recvEnd << std::endl;
    exit(0);
}

void P2PTransfer::handleAsk(json& msg)
{
    volatile bool ready = false;
    volatile bool finish = false;
    uint64_t offset = 0;
    auto config = mConfig;
    auto payload = msg["payload"];

    std::string fn;
    uint64_t fid = payload["fid"];
    mMtx.lock();
    auto it = mFiles.find(fid);
    if (it != mFiles.end()) {
        fn = it->second;
    }
    mMtx.unlock();
    if (fn.empty()) {
        std::cout << "unknown fid:" << fid << std::endl;
        exit(1);
    }
    std::ifstream in(fn, std::ios::binary);
    if (!in) {
        std::cout << "fail to open file:" << fn << std::endl;
        exit(1);
    }

    config.iceUfrag = payload["ufrag"];
    config.icePwd = payload["pwd"];
    auto pc = std::make_shared<rtc::PeerConnection>(config);
    std::shared_ptr<rtc::DataChannel> dc;
    volatile long long ack = 0;
    pc->onDataChannel([&](std::shared_ptr<rtc::DataChannel> _dc) {
        dc = _dc;

        dc->onMessage([&](auto data) {
            if (std::holds_alternative<std::string>(data)) {
                auto s = std::get<std::string>(data);
                //std::cout << "[Received: " << s << "]" << std::endl;
                if (strncmp(s.c_str(), "ask:", 4) == 0) {
                    offset = strtoull(s.c_str()+4, nullptr, 10);
                    ready = true;
                } else if (strncmp(s.c_str(), "ack:", 4) == 0) {
                    ack = strtoull(s.c_str()+4, nullptr, 10);
                } else if (s == "end") {
                    finish = true;
                }
            }
        });

        dc->onClosed([&]() {
        });
    });

    pc->setRemoteDescription(std::string(payload["remoteSdp"]));
    while (!ready) {
        std::cout << "wait dc ready" << std::endl;
        std::this_thread::sleep_for(1s);
    }

    if (offset > 0) {
        std::cout << "set file offset: " << offset << std::endl;
        in.seekg(offset);
        if (!in) {
            std::cout << "fail to set file offset: " << offset << std::endl;
            exit(1);
        }
    }

    char data[1024];
    long long total = 0;
    long long lastTotal = 0;
    long long lastTime = 0;
    while (in) {
        in.read(data, sizeof(data));
        int n = in.gcount();
        if (n > 0) {
            dc->send((const std::byte*)data, n);
            total += n;
        }
        while (true) {
            int amt = dc->bufferedAmount();
            auto now = std::chrono::system_clock::now();
            long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()).count();
            if (lastTime + 1000 <= ms) {
                long long e = ms - lastTime;
                long long speed = (total - lastTotal) * 1000 / e;
                printf("send total:%lld ack:%lld, bufferAmount:%d speed:%lld B/s\n", total, ack, amt, speed);
                fflush(stdout);
                lastTotal = total;
                lastTime = ms;
            }
            if (amt >= 10485760) {
                std::this_thread::sleep_for(5ms);
            } else if (in || amt == 0) {
                break;
            }
        }
    }
    if (in.eof()) {
        dc->send(std::string("end"));
        std::cout << "finish send total:" << total << std::endl;
    } else if (in.bad()) {
        std::cout << "read file bad" << std::endl; 
        exit(1);
    }
    while (!finish) {
        std::cout << "wait peer finish" << std::endl;
        std::this_thread::sleep_for(1s);
    }

    exit(0);
}

uint64_t P2PTransfer::postMsg(std::shared_ptr<json> msg)
{
    std::unique_lock lck(mMtx);
    uint64_t mid = ++mMsgId;
    (*msg)["id"] = mid;
    mMsgs[mid] = msg;
    mCv.notify_all();
    return mid;
}

void P2PTransfer::runWebSkt()
{
    std::atomic<bool> closed = true;
    while (true) {
        if (!mWebSkt) {
            rtc::WebSocket::Configuration config;
            config.disableTlsVerification = true;
            auto ws = std::make_shared<rtc::WebSocket>(std::move(config));
            ws->onOpen([]() {
                std::cout << "websocket opened" << std::endl;
            });
            ws->onError([&closed](std::string error) {
                std::cout << "WebSocket: Error: " << error << std::endl;
                closed = true;
            });
            ws->onClosed([&closed]() {
                std::cout << "WebSocket: Closed" << std::endl;
                closed = true;
            });
            ws->onMessage([&](std::variant<rtc::binary, std::string> message) {
                if (!std::holds_alternative<std::string>(message)) {
                    return;
                }
                std::string str = std::move(std::get<std::string>(message));
                std::cout << "websocket recv: " << str << std::endl;
                try {
                    json msg = json::parse(str);
                    uint64_t mid = 0;
                    auto it = msg.find("id");
                    if (it != msg.end()) {
                        mid = it.value();
                    }
                    Event evt;
                    evt.msg = std::make_shared<json>(std::move(msg));
                    if (mid != 0) {
                        std::unique_lock lck(mMtx);
                        auto it = mMsgs.find(mid);
                        if (it != mMsgs.end()) {
                            evt.src = it->second;
                            mMsgs.erase(it);
                        }
                    }
                    postEvent(evt);
                } catch (std::exception& excp) {
                    std::cout << "prase msg excp:" << excp.what() << std::endl;
                }
            });
            mWebSkt = ws;
            closed = false;
            mWebSkt->open(mWsUrl);
        }
        while (!closed && !mWebSkt->isOpen()) {
            std::this_thread::sleep_for(10ms);
        }
        if (closed) {
            mWebSkt = nullptr;
            std::cout << "WebSocket closed, sleep sometimes to retry" << std::endl;
            std::this_thread::sleep_for(10s);
            continue;
        }

        do {
            std::unique_lock lck(mMtx);
            while (mMsgs.empty() || (mSentMsgId >= mMsgs.rbegin()->first)) {
                mCv.wait(lck);
            }
        } while (false);
        while (true) {
            uint64_t mid = mSentMsgId + 1;
            mMtx.lock();
            auto it = mMsgs.find(mid);
            auto msg = it == mMsgs.end() ? nullptr : it->second;
            mMtx.unlock();
            if (!msg) {
                break;
            }
            try {
                mWebSkt->send(msg->dump());
                ++mSentMsgId;
            } catch (std::exception& excp) {
                std::cout << "WebSocket send excp:" << excp.what() << std::endl;
                closed = true;
                mWebSkt = nullptr;
                std::unique_lock lck(mMtx);
                while (!mMsgs.empty()) {
                    auto it = mMsgs.begin();
                    if (it->first <= mSentMsgId) {
                        mMsgs.erase(it);
                    }
                }
                break;
            }
        }
    }
}

void P2PTransfer::runGather()
{
    unsigned cnt = 0;
    while (true) {
        auto pc = std::make_shared<rtc::PeerConnection>(mConfig);
        bool gatherDone = false;
        pc->onGatheringStateChange([&gatherDone](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                gatherDone = true;
            }
        });
        std::ostringstream oss;
        oss << "detectChannel" << ++cnt;
        auto dc = pc->createDataChannel(oss.str());
        while (!gatherDone) {
            std::this_thread::sleep_for(10ms);
        }
        auto dsOpt = pc->localDescription();
        if (dsOpt) {
            std::string sds(dsOpt.value());
            std::cout << "LocalDescrption:\n" << sds << std::endl;
            mDesc = sds;
            std::shared_ptr<json> msg = std::make_shared<json>(json{
                {"type", MsgTypeRegisterSdp},
                {"payload", {{"sdp", sds}}},
            });
            postMsg(msg);
        }
        std::this_thread::sleep_for(10s);
        mPC = pc;
    }
}

void usage(const char* cmd)
{
    std::cerr << "Usage:" << std::endl;
    std::cerr << cmd << " send [--stun host:port] [--ws url] <file>" << std::endl;
    std::cerr << cmd << " recv [--stun host:port] [--ws url] <fileId> [fileName]" << std::endl;
}

int main(int argc, char* argv[])
{
    //rtc::InitLogger(rtc::LogLevel::Debug);
    rtc::InitLogger(rtc::LogLevel::Error);
    //std::string wsUrl = "ws://localhost:8080/ws";
    std::string stunHost = "39.106.141.70";
    uint16_t stunPort = 8347;
    std::string wsUrl = "ws://39.106.141.70:6180/ws";
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (a[0] == '-' && a[1] == '-') {
            if (strcmp(a+2, "ws") == 0 && i + 1 < argc) {
                wsUrl = argv[++i];
            } else if (strcmp(a+2, "stun") == 0 && i + 1 < argc) {
                const char* s = argv[++i];
                const char* p = strchr(s, ':');
                if (!p) {
                    std::cerr << "invalid stun option:" << s << std::endl;
                    return 1;
                }
                stunHost.assign(s, p - s);
                stunPort = atoi(p+1);
            } else {
                std::cerr << "invalid option:" << argv[i] << std::endl;
                usage(argv[0]);
                return 1;
            }
        } else {
            args.push_back(a);
        }
    }
    if (args.size() < 2) {
        usage(argv[0]);
        return 1;
    }
    std::string& act(args[0]);
    if (act != "send" && act != "recv") {
        usage(argv[0]);
        return 1;
    }
    P2PTransfer p(stunHost, stunPort, wsUrl);
    if (act == "send") {
        p.send(args[1].c_str());
    } else if (act == "recv") {
        long long fid = atoll(args[1].c_str());
        p.recv(fid, args.size() > 2 ? args[2].c_str() : nullptr);
    }
    return 0;
}
