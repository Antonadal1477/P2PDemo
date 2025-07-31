'use strict';

const MsgTypeUnknown = 0;
const MsgTypeOk = 1;
const MsgTypeRegisterSdp = 2;
const MsgTypeRegisterSdpReply = 3;
const MsgTypeRegisterFile = 4;
const MsgTypeRegisterFileReply = 5;
const MsgTypeQueryPeer = 6;
const MsgTypeQueryPeerReply = 7;
const MsgTypeQueryPeerAsk = 8;

let ws;
let peerId;
let pc;
let dc;
let fileInfo = {
    name: '',
    size: 0,
    sid: 0
};
let startTime;
let receivedBytes = 0;
let lastUpdateTime = 0;
let fileChunks = [];
let transferSpeed = 0;
let speedSamples = [];

const connectBtn = document.getElementById('connectBtn');
const startBtn = document.getElementById('startBtn');
const disconnectBtn = document.getElementById('disconnectBtn');
const downloadBtn = document.getElementById('downloadBtn');
const statusIndicator = document.getElementById('statusIndicator');
const statusText = document.getElementById('statusText');
const statusBox = document.getElementById('statusBox');
const progressFill = document.getElementById('progressFill');
const progressPercent = document.getElementById('progressPercent');
const fileNameElement = document.getElementById('fileName');
const fileSizeElement = document.getElementById('fileSize');
const receivedSizeElement = document.getElementById('receivedSize');
const transferSpeedElement = document.getElementById('transferSpeed');
const serverUrlInput = document.getElementById('serverUrl');
const fileIdInput = document.getElementById('fileId');

document.addEventListener('DOMContentLoaded', () => {
    connectBtn.addEventListener('click', connectWebSocket);
    startBtn.addEventListener('click', startDownload);
    disconnectBtn.addEventListener('click', disconnect);
    downloadBtn.addEventListener('click', saveFile);

    loadSettings();
});

function connectWebSocket() {
    const serverUrl = serverUrlInput.value.trim();
    if (!serverUrl) {
        addStatusMessage('请输入有效的服务器地址', 'error');
        return;
    }

    if (pc && pc.signalingState !== 'closed') {
        addStatusMessage('已有活跃连接', 'warning');
        return;
    }

    saveSettings();

    try {
        ws = new WebSocket(serverUrl);
        updateConnectionStatus('connecting', '连接中...');

        ws.onopen = () => {
            addStatusMessage('WebSocket连接已建立', 'success');
            updateConnectionStatus('connected', '已连接');
            registerPeer();
            enableControls(true);
        };

        ws.onmessage = handleMessage;
        ws.onerror = (error) => {
            addStatusMessage('WebSocket错误: ' + error.message, 'error');
            updateConnectionStatus('disconnected', '连接错误');
        };

        ws.onclose = () => {
            addStatusMessage('WebSocket连接已关闭', 'info');
            updateConnectionStatus('disconnected', '未连接');
            enableControls(false);
        };
    } catch (error) {
        addStatusMessage('连接失败: ' + error.message, 'error');
        updateConnectionStatus('disconnected', '连接失败');
    }
}

function registerPeer() {
    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addStatusMessage('WebSocket未连接，无法注册', 'error');
        return;
    }

    const config = {
        iceServers: [{ urls: 'stun:39.106.141.70:8347' }],
        iceTransportPolicy: 'all'
    };

    pc = new RTCPeerConnection(config);
    addStatusMessage('创建RTCPeerConnection', 'info');

    dc = pc.createDataChannel('dc');

    setupDataChannel();

    pc.onicecandidate = (event) => {
        if (event.candidate) {
            addStatusMessage('ICE候选: ' + JSON.stringify(event.candidate), 'info');
        } else {
            addStatusMessage('ICE候选收集完成', 'success');

            ws.send(JSON.stringify({
                type: MsgTypeRegisterSdp,
                id: 0,
                payload: {
                    sdp: pc.localDescription.sdp,
                }
            }));
        }
    };

    pc.onconnectionstatechange = () => {
        addStatusMessage('连接状态: ' + pc.connectionState, 'info');
        if (pc.connectionState === 'disconnected' || pc.connectionState === 'failed') {
            addStatusMessage('连接断开', 'error');
        }
    };

    pc.oniceconnectionstatechange = () => {
        addStatusMessage('ICE连接状态: ' + pc.iceConnectionState, 'info');
    };

    pc.createOffer().then(offer => {
        addStatusMessage('创建SDP offer:', offer);
        return pc.setLocalDescription(offer);
    }).then(() => {
        addStatusMessage('本地描述设置完成，等待ICE候选收集...', 'info');
    }).catch(error => {
        addStatusMessage('创建offer失败: ' + error.message, 'error');
    });

}

function handleMessage(event) {
    try {
        const msg = JSON.parse(event.data);
        addStatusMessage('收到消息: ' + JSON.stringify(msg.type), 'info');
        switch (msg.type) {
            case MsgTypeRegisterSdpReply:
                handleRegisterReply(msg.payload);
                break;
            case MsgTypeQueryPeerReply:
                handleQueryReply(msg.payload);
                break;
            default:
                addStatusMessage('未知消息类型: ' + msg.type, 'warning');
        }
    } catch (error) {
        addStatusMessage('消息处理错误: ' + error.message, 'error');
    }
}

function handleRegisterReply(payload) {
    peerId = payload.pid;
    addStatusMessage('已注册为对等端，ID: ' + peerId, 'success');
    startBtn.disabled = false;
}

function startDownload() {
    const fileId = fileIdInput.value.trim();
    if (!fileId) {
        addStatusMessage('请输入有效的文件ID', 'error');
        return;
    }

    if (!ws || ws.readyState !== WebSocket.OPEN) {
        addStatusMessage('WebSocket未连接', 'error');
        return;
    }

    addStatusMessage('请求文件ID: ' + fileId, 'info');
    resetTransferState();

    ws.send(JSON.stringify({
        type: MsgTypeQueryPeer,
        id: 1,
        payload: {
            fid: parseInt(fileId),
            isWebPeer: true, // 标记为Web端请求
        }
    }));
}

function handleQueryReply(payload) {
    if (!payload) {
        addStatusMessage('无效的查询回复', 'error');
        return;
    }

    fileInfo = {
        name: payload.name || 'unknown_file',
        size: payload.size || 0,
        sid: payload.sid || 0
    };

    updateFileInfo();
    addStatusMessage(`开始接收文件: ${fileInfo.name} (${formatFileSize(fileInfo.size)})`, 'success');

    setupPeerConnection(payload)
}

async function setupPeerConnection(payload) {
    addStatusMessage('设置远程描述: ' + payload.remoteSdp.substring(0, 50) + '...', 'info');

    try {
        await pc.setRemoteDescription({
            type: 'answer',
            sdp: payload.remoteSdp
        })
    } catch (error) {
        addStatusMessage('设置远程描述错误: ' + error.message, 'error');
    }
}

function setupDataChannel() {
    addStatusMessage('设置数据通道');
    dc.binaryType = 'arraybuffer';
    receivedBytes = 0;
    startTime = Date.now();
    lastUpdateTime = startTime;
    fileChunks = [];
    speedSamples = [];

    dc.onopen = (event) => {
        addStatusMessage('数据通道已打开', 'success');
        dc.send('ask:0');
        updateProgress();
    };

    dc.onmessage = (event) => {
        if (typeof event.data === 'string') {
            if (event.data === 'end') {
                addStatusMessage('文件传输完成', 'success');
                downloadBtn.style.display = 'block';
                dc.send('end');
                saveFile();
                return;
            }
            addStatusMessage('收到消息: ' + event.data, 'info');
            return;
        }

        const now = Date.now();
        receivedBytes += event.data.byteLength;
        fileChunks.push(event.data);

        const timeDiff = (now - lastUpdateTime) / 1000;
        if (timeDiff > 0) {
            const bytesDiff = receivedBytes - (speedSamples.length > 0 ? speedSamples[speedSamples.length - 1].bytes : 0);
            const currentSpeed = bytesDiff / timeDiff;

            speedSamples.push({
                time: now,
                bytes: receivedBytes,
                speed: currentSpeed
            });

            if (speedSamples.length > 10) {
                speedSamples.shift();
            }

            if (speedSamples.length > 1) {
                const totalTime = (speedSamples[speedSamples.length - 1].time - speedSamples[0].time) / 1000;
                const totalBytes = speedSamples[speedSamples.length - 1].bytes - speedSamples[0].bytes;
                transferSpeed = totalBytes / totalTime;
            }
        }

        lastUpdateTime = now;
        updateProgress();
    };

    dc.onclose = () => {
        addStatusMessage('数据通道已关闭', 'info');
        if (receivedBytes < fileInfo.size) {
            addStatusMessage('传输中断', 'warning');
        }
    };

    dc.onerror = (error) => {
        addStatusMessage('数据通道错误: ' + error.message, 'error');
    };
}

function updateProgress() {
    const progress = fileInfo.size > 0 ? (receivedBytes / fileInfo.size) * 100 : 0;
    progressFill.style.width = `${progress}%`;
    progressPercent.textContent = `${progress.toFixed(1)}%`;

    fileNameElement.textContent = fileInfo.name;
    fileSizeElement.textContent = formatFileSize(fileInfo.size);
    receivedSizeElement.textContent = formatFileSize(receivedBytes);
    transferSpeedElement.textContent = `${formatFileSize(transferSpeed)}/s`;
}

function saveFile() {
    if (fileChunks.length === 0) {
        addStatusMessage('没有可保存的文件数据', 'error');
        return;
    }

    const blob = new Blob(fileChunks);
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = fileInfo.name;
    document.body.appendChild(a);
    a.click();

    setTimeout(() => {
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        addStatusMessage('文件已保存: ' + fileInfo.name, 'success');
    }, 100);
}

function disconnect() {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.close();
    }

    if (pc) {
        pc.close();
        pc = null;
    }

    if (dc) {
        dc.close();
        dc = null;
    }

    updateConnectionStatus('disconnected', '未连接');
    enableControls(false);
    addStatusMessage('已断开连接', 'info');
}

function resetTransferState() {
    receivedBytes = 0;
    fileChunks = [];
    speedSamples = [];
    transferSpeed = 0;

    progressFill.style.width = '0%';
    progressPercent.textContent = '0%';
    receivedSizeElement.textContent = '0 B';
    transferSpeedElement.textContent = '0 KB/s';

    downloadBtn.style.display = 'none';
}

function updateFileInfo() {
    fileNameElement.textContent = fileInfo.name;
    fileSizeElement.textContent = formatFileSize(fileInfo.size);
}

function updateConnectionStatus(state, text) {
    statusIndicator.className = 'status-indicator';
    if (state === 'connected') {
        statusIndicator.classList.add('connected');
    } else if (state === 'connecting') {
        statusIndicator.classList.add('connecting');
    }
    statusText.textContent = text;
}

function enableControls(connected) {
    connectBtn.disabled = connected;
    startBtn.disabled = !connected;
    disconnectBtn.disabled = !connected;
    fileIdInput.disabled = !connected;
}

function addStatusMessage(message, type) {
    const now = new Date();
    const timeStr = now.toLocaleTimeString();
    const item = document.createElement('div');
    item.className = `status-item ${type}`;
    item.textContent = `[${timeStr}] ${message}`;
    statusBox.appendChild(item);
    statusBox.scrollTop = statusBox.scrollHeight;
}

function formatFileSize(bytes) {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
}

function saveSettings() {
    localStorage.setItem('serverUrl', serverUrlInput.value);
}

function loadSettings() {
    const savedUrl = localStorage.getItem('serverUrl');
    if (savedUrl) {
        serverUrlInput.value = savedUrl;
    }
}