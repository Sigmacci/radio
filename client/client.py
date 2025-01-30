import tornado.ioloop
import tornado.web
import tornado.iostream
import json
import socket
import asyncio

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setblocking(False)
stream = tornado.iostream.IOStream(sock)

async def connect_socket():
    await stream.connect(("localhost", 1234))
    await stream.write(b'PLAY song1')
    print("Connected to socket server and sent PLAY command")

class AudioStreamHandler(tornado.web.RequestHandler):
    def set_default_headers(self):
        self.set_header("Access-Control-Allow-Origin", "*")
        self.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.set_header("Access-Control-Allow-Headers", "Content-Type")

    def options(self):
        self.set_status(204)
        self.finish()

    async def get(self):
        print("Client connected, waiting for ACK to start audio stream...")
        self.set_status(200)
        self.set_header("Content-Type", "audio/mpeg")
        self.set_header("Transfer-Encoding", "chunked")

        try:
            while True:
                chunk = await stream.read_bytes(16000, partial=True)
                if not chunk:
                    print("No more data from socket server, ending stream.")
                    break

                print("Received chunk, sending to browser...")
                self.write(chunk)
                await self.flush()

        except tornado.iostream.StreamClosedError:
            print("Client disconnected, stopping stream.")
        except Exception as e:
            print(f"Streaming error: {e}")

    async def post(self):
        try:
            args = json.loads(self.request.body)
            cmd = args.get("cmd")
            if cmd == 'PLAY':
                await stream.write(bytes("PLAY " + args.get("track"), "utf-8"))
            else:
                await stream.write(bytes(cmd, "utf-8"))

            self.write("Command received")

        except json.JSONDecodeError:
            self.set_status(400)
            self.write("Invalid JSON")

class TrackListHandler(tornado.web.RequestHandler):
    def set_default_headers(self):
        self.set_header("Access-Control-Allow-Origin", "*")
        self.set_header("Access-Control-Allow-Methods", "POST, GET, OPTIONS")
        self.set_header("Access-Control-Allow-Headers", "Content-Type")

    def options(self):
        self.set_status(204)
        self.finish()

    async def get(self):
        await stream.write(b"TRACKS")
        tracks = await stream.read_bytes(10000, partial=True)

        decoded = ''
        print(tracks)
        for b in tracks:
            try:
                decoded += chr(b)
            except UnicodeDecodeError:
                pass
            finally:
                continue
        self.write(json.dumps(tracks.decode("utf-8").split("\n")))
        self.finish()

def make_app():
    return tornado.web.Application([
        (r"/audio", AudioStreamHandler),
        (r"/tracks", TrackListHandler),
    ])

if __name__ == "__main__":
    app = make_app()
    app.listen(8080)
    asyncio.get_event_loop().run_until_complete(connect_socket())
    tornado.ioloop.IOLoop.current().start()