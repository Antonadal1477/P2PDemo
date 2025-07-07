#!/usr/bin/env python3

import sys
import asyncio
import websockets

async def run(uri):
    async with websockets.connect(uri) as ws:
        while True:
            msg = input("输入要发送的消息: ")
            await ws.send(msg)
            response = await ws.recv()
            print("收到响应:", response)

if __name__ == "__main__":
    uri = sys.argv[1] if len(sys.argv) > 1 else 'ws://localhost:8080/ws'

    asyncio.run(run(uri))

