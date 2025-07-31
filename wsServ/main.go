package main

import (
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"strings"
	"sync"

	"github.com/gorilla/websocket"
)

const (
	SdpPlaceHolder = "SDP_PLACEHOLDER"
)

// 升级 HTTP 请求为 WebSocket 的 upgrader
var upgrader = websocket.Upgrader{
	CheckOrigin: func(r *http.Request) bool {
		return true // 允许所有跨域请求（开发用）
	},
}

const (
	MsgTypeUnknown           = 0
	MsgTypeOk                = 1
	MsgTypeRegisterSdp       = 2
	MsgTypeRegisterSdpReply  = 3
	MsgTypeRegisterFile      = 4
	MsgTypeRegisterFileReply = 5
	MsgTypeQueryPeer         = 6
	MsgTypeQueryPeerReply    = 7
	MsgTypeQueryPeerAsk      = 8
)

type RegisterSdpMsgPayload struct {
	Sdp string `json:"sdp"`
}

type RegisterSdpReplyMsgPayload struct {
	PeerId uint64 `json:"pid"`
}

type RegisterFileMsgPayload struct {
	Name string `json:"name"`
	Size uint64 `json:"size"`
	Mode uint32 `json:"mode"`
}

type RegisterFileReplyMsgPayload struct {
	FileId uint64 `json:"fid"`
}

type QueryPeerMsgPayload struct {
	FileId    uint64 `json:"fid"`
	IsWebPeer bool   `json:"isWebPeer"`
}

type QueryPeerReplyMsgPayload struct {
	Sid       uint64 `json:"sid"`
	Ufrag     string `json:"ufrag"`
	Pwd       string `json:"pwd"`
	FileId    uint64 `json:"fid"`
	Name      string `json:"name"`
	Size      uint64 `json:"size"`
	Mode      uint32 `json:"mode"`
	LocalSdp  string `json:"localSdp"`
	RemoteSdp string `json:"remoteSdp"`
	PeerId    uint64 `json:"pid"`
}

type QueryPeerAskMsgPayload struct {
	Sid       uint64 `json:"sid"`
	Ufrag     string `json:"ufrag"`
	Pwd       string `json:"pwd"`
	FileId    uint64 `json:"fid"`
	LocalSdp  string `json:"localSdp"`
	RemoteSdp string `json:"remoteSdp"`
	PeerId    uint64 `json:"pid"`
}

type WrapMsg struct {
	Type    int             `json:"type"`
	Id      uint64          `json:"id"`
	Payload json.RawMessage `json:"payload"`
}

type WrapReply struct {
	Type    int    `json:"type"`
	Id      uint64 `json:"id"`
	Payload any    `json:"payload"`
}

type Peer struct {
	Id     uint64 `json:"id"`
	center *Center

	sdp   string
	descs []string
	files []uint64

	mtx sync.Mutex

	recvCh chan []byte
	sendCh chan []byte
}

func NewPeer(c *Center, id uint64) *Peer {
	p := &Peer{
		center: c,
		Id:     id,
		recvCh: make(chan []byte),
		sendCh: make(chan []byte),
	}
	return p
}

func (p *Peer) GetSdp(ufrag, pwd string, passive, isWebPeer bool) string {
	p.mtx.Lock()
	sdp := p.sdp
	p.mtx.Unlock()
	if isWebPeer {
		return sdp
	}
	if len(sdp) > 0 {
		items := strings.Split(sdp, "\r\n")
		filled := false
		n := 0
		for i, item := range items {
			if strings.HasPrefix(item, "a=setup:") ||
				strings.HasPrefix(item, "a=ice-ufrag:") ||
				strings.HasPrefix(item, "a=ice-pwd:") {
				if !filled {
					filled = true
					items[n] = SdpPlaceHolder
					n++
				}
			} else if strings.TrimSpace(item) != "" {
				if i != n {
					items[n] = item
				}
				n++
			}
		}
		items = items[:n]
		sdp = strings.Join(items, "\r\n")
	}
	s := "a=setup:actpass\r\n"
	if !passive {
		s = "a=setup:active\r\n"
	}
	s += "a=ice-ufrag:" + ufrag + "\r\n"
	s += "a=ice-pwd:" + pwd
	sdp = strings.Replace(sdp, SdpPlaceHolder, s, 1)
	if !strings.HasSuffix(sdp, "\r\n") {
		sdp += "\r\n"
	}
	return sdp
}

func (p *Peer) SendMsg(msg []byte) {
	p.sendCh <- msg
}

func (p *Peer) Handle(conn *websocket.Conn) {
	go func() {
		for {
			messageType, message, err := conn.ReadMessage()
			if err != nil {
				slog.Info("读取消息出错:", "err", err)
				break
			}
			slog.Info("收到消息：", "msg", message, "type", messageType)
			p.recvCh <- message
		}
	}()
	var err error
	for {
		select {
		case message := <-p.recvCh:
			var msg WrapMsg
			err = json.Unmarshal(message, &msg)
			if err != nil {
				msg.Type = MsgTypeUnknown
			}

			switch msg.Type {
			case MsgTypeRegisterSdp:
				p.handleRegisterSdp(conn, msg)
			case MsgTypeRegisterFile:
				p.handleRegisterFile(conn, msg)
			case MsgTypeQueryPeer:
				p.handleQueryPeer(conn, msg)
			default:
				msg.Type = MsgTypeUnknown
				dat, _ := json.Marshal(msg)
				//response := fmt.Sprintf("服务端回声：%s", message)
				err = conn.WriteMessage(websocket.TextMessage, dat)
				if err != nil {
					slog.Warn("websocket write response msg error:", "err", err)
					break
				}
			}
		case message := <-p.sendCh:
			err = conn.WriteMessage(websocket.TextMessage, message)
			if err != nil {
				slog.Warn("websocket write msg error:", "err", err)
				break
			}
		}
	}
}

func (p *Peer) handleRegisterSdp(conn *websocket.Conn, msg WrapMsg) {
	var payload RegisterSdpMsgPayload
	err := json.Unmarshal(msg.Payload, &payload)
	if err != nil {
		slog.Warn("invalid register message")
		return
	}

	p.sdp = payload.Sdp

	reply := WrapReply{
		Type: MsgTypeRegisterSdpReply,
		Id:   msg.Id,
		Payload: &RegisterSdpReplyMsgPayload{
			PeerId: p.Id,
		},
	}
	dat, _ := json.Marshal(reply)
	conn.WriteMessage(websocket.TextMessage, dat)
}

func (p *Peer) handleRegisterFile(conn *websocket.Conn, msg WrapMsg) {
	var payload RegisterFileMsgPayload
	err := json.Unmarshal(msg.Payload, &payload)
	if err != nil {
		slog.Warn("invalid register message")
		return
	}
	fid := p.center.registerFile(p.Id, payload)
	p.files = append(p.files, fid)
	var reply WrapReply
	reply.Type = MsgTypeRegisterFileReply
	reply.Id = msg.Id
	reply.Payload = &RegisterFileReplyMsgPayload{
		FileId: fid,
	}

	dat, _ := json.Marshal(reply)
	conn.WriteMessage(websocket.TextMessage, dat)
}

func (p *Peer) handleQueryPeer(conn *websocket.Conn, msg WrapMsg) {
	var payload QueryPeerMsgPayload
	err := json.Unmarshal(msg.Payload, &payload)
	if err != nil {
		slog.Warn("invalid query peer message")
		return
	}
	res := p.center.schedulePeer(p.Id, payload)
	var reply WrapReply
	reply.Type = MsgTypeQueryPeerReply
	reply.Id = msg.Id
	reply.Payload = res

	dat, _ := json.Marshal(reply)
	conn.WriteMessage(websocket.TextMessage, dat)
}

type FileSt struct {
	FileId uint64
	PeerId uint64
	Name   string
	Size   uint64
	Mode   uint32
}

type Center struct {
	mtx sync.Mutex

	id    uint64
	peers map[uint64]*Peer
	files map[uint64]*FileSt
}

func NewCenter() (c *Center) {
	c = &Center{
		peers: make(map[uint64]*Peer),
		files: make(map[uint64]*FileSt),
	}
	return
}

func (c *Center) handleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		slog.Error("升级失败:", "err", err)
		return
	}
	defer conn.Close()

	slog.Info("客户端连接:", "client", conn.RemoteAddr())

	c.mtx.Lock()
	c.id += 1
	peer := NewPeer(c, c.id)
	c.peers[peer.Id] = peer
	c.mtx.Unlock()

	peer.Handle(conn)

	c.mtx.Lock()
	defer c.mtx.Unlock()
	for _, fid := range peer.files {
		delete(c.files, fid)
	}
	delete(c.peers, peer.Id)
	slog.Info("remove peer:", "id", peer.Id)
}

func (c *Center) registerFile(peerId uint64, payload RegisterFileMsgPayload) uint64 {
	st := &FileSt{
		PeerId: peerId,
		Name:   payload.Name,
		Size:   payload.Size,
		Mode:   payload.Mode,
	}
	c.mtx.Lock()
	defer c.mtx.Unlock()
	c.id += 1
	st.FileId = c.id
	c.files[st.FileId] = st
	return st.FileId
}

func (c *Center) schedulePeer(peerId uint64, payload QueryPeerMsgPayload) (res *QueryPeerReplyMsgPayload) {
	c.mtx.Lock()
	defer c.mtx.Unlock()
	u, ok := c.peers[peerId]
	if !ok {
		return
	}
	f, ok := c.files[payload.FileId]
	if !ok {
		return
	}
	p, ok := c.peers[f.PeerId]
	if !ok {
		return
	}
	c.id += 1
	sid := c.id
	ufrag0 := fmt.Sprintf("User%d/%d", sid, u.Id)
	pwd0 := fmt.Sprintf("Password%08d/%08d", sid, u.Id)
	ufrag1 := fmt.Sprintf("User%d/%d", sid, p.Id)
	pwd1 := fmt.Sprintf("Password%08d/%08d", sid, p.Id)
	res = &QueryPeerReplyMsgPayload{
		Sid:       sid,
		Ufrag:     ufrag0,
		Pwd:       pwd0,
		FileId:    f.FileId,
		Name:      f.Name,
		Size:      f.Size,
		Mode:      f.Mode,
		RemoteSdp: p.GetSdp(ufrag1, pwd1, false, false),
		PeerId:    p.Id,
	}
	ask := &QueryPeerAskMsgPayload{
		Sid:       sid,
		Ufrag:     ufrag1,
		Pwd:       pwd1,
		FileId:    f.FileId,
		RemoteSdp: u.GetSdp(ufrag0, pwd0, true, payload.IsWebPeer),
		PeerId:    peerId,
	}
	var reply WrapReply
	reply.Type = MsgTypeQueryPeerAsk
	reply.Payload = ask
	dat, _ := json.Marshal(reply)
	p.SendMsg(dat)
	return
}

func main() {
	c := NewCenter()
	http.HandleFunc("/ws", c.handleWebSocket)

	serv := ":6180"
	if len(os.Args) > 1 {
		serv = os.Args[1]
	}

	slog.Info("WebSocket start", "listen", serv+"/ws")
	if err := http.ListenAndServe(serv, nil); err != nil {
		slog.Error("ListenAndServe:", "err", err)
	}
}
