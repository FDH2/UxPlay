"""Real HTTP/HLS playback: filtering, redirects, signed relative URIs and audio."""
import http.server
import pathlib
import subprocess
import sys
import threading
import urllib.parse

MEDIA = pathlib.Path(__file__).parent / "media" / "hls"
requests = []
errors = []
require_cookie = True


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        path = urllib.parse.urlsplit(self.path)
        requests.append((path.path, path.query, self.headers.get("User-Agent")))
        if path.path == "/entry":
            self.send_response(302)
            self.send_header("Location", "/nested/master.m3u8?sig=master")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if path.query != "sig=master" and path.path.endswith("master.m3u8"):
            self.send_error(403)
            return
        if (require_cookie and not path.path.endswith("master.m3u8")
                and "uxplay_hls_test=ok" not in self.headers.get("Cookie", "")):
            errors.append((self.path, str(self.headers)))
            self.send_error(403, "Missing manifest cookie")
            return
        if path.path.endswith("master.m3u8"):
            text = '#EXTM3U\n#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="a",NAME="Original",DEFAULT=YES,AUTOSELECT=YES,URI="../audio/media.m3u8?sig=audio"\n'
            for name, codec, size, bandwidth in [
                ("avc-low", "avc1.64001f", "1280x720", 100000),
                ("avc-hd", "avc1.640028", "1920x1080", 500000),
                ("hevc-hd", "hvc1.2.4.L123.90", "1920x1080", 600000),
                ("avc-uhd", "avc1.640034", "3840x2160", 900000),
            ]:
                text += f'#EXT-X-STREAM-INF:BANDWIDTH={bandwidth},CODECS="{codec},mp4a.40.2",RESOLUTION={size},AUDIO="a"\n../{name}/media.m3u8?sig={name}\n'
            data = text.encode()
        elif path.path == "/plain.mp4":
            data = (MEDIA / "avc-init.mp4").read_bytes() + (MEDIA / "avc-0.m4s").read_bytes()
        elif path.path.endswith("/media.m3u8"):
            name = path.path.split("/")[1]
            if path.query != "sig=" + name:
                self.send_error(403)
                return
            prefix = "audio" if name == "audio" else "hevc" if name.startswith("hevc") else "avc"
            # Media playlists themselves contain relative EXT-X-MAP and segment URLs.
            data = (MEDIA / (prefix + ".m3u8")).read_bytes()
        else:
            file = MEDIA / pathlib.PurePosixPath(path.path).name
            if not file.is_file():
                self.send_error(404)
                return
            data = file.read_bytes()
        self.send_response(200)
        if path.path.endswith("master.m3u8"):
            self.send_header("Set-Cookie", "uxplay_hls_test=ok; Path=/")
        self.send_header("Content-Type", "application/vnd.apple.mpegurl" if path.path.endswith(".m3u8") else "video/mp4")
        # No Content-Length: exercise a genuinely chunked HTTP response.
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        try:
            for offset in range(0, len(data), 37):
                chunk = data[offset:offset + 37]
                self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass  # failed selection/cancellation legitimately closes the source


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
try:
    url = f"http://127.0.0.1:{server.server_port}/entry"
    # With the filter's limits, GStreamer takes the highest-bandwidth variant left (connection-speed is high).
    cases = [
        ("AVC:0;HEVC:1080,1080", "hevc-hd"),
        ("AVC:1080,1080;HEVC:0", "avc-hd"),
        ("AVC:720,720;HEVC:0", "avc-low"),
        ("", "avc-uhd"),
        ("AVC:0;HEVC:0", "error"),
    ]
    for policy, expected in cases:
        requests.clear()
        errors.clear()
        try:
            result = subprocess.run([sys.argv[1], url, policy, expected], capture_output=True, text=True, timeout=50)
        except subprocess.TimeoutExpired as exc:
            raise AssertionError((policy, exc.stdout, exc.stderr, requests, errors)) from exc
        if result.returncode == 77:
            print(result.stderr)
            sys.exit(77)
        assert result.returncode == 0, (result.stdout, result.stderr, requests, errors)
        variants = {path.split("/")[1] for path, _, _ in requests if path.endswith("/media.m3u8") and not path.startswith("/audio/")}
        assert variants == (set() if expected == "error" else {expected}), (policy, variants, requests)
        assert sum(path == "/nested/master.m3u8" for path, _, _ in requests) == 2, requests
        if expected != "error":
            assert any(path == "/audio/media.m3u8" for path, _, _ in requests), requests
            assert all(ua == "UxPlay-HLS-test" for _, _, ua in requests), requests
        print(f"HTTP HLS: {policy} -> {expected}, two sessions passed")
    require_cookie = False
    for path in ("/avc-hd/media.m3u8?sig=avc-hd", "/plain.mp4"):
        result = subprocess.run([sys.argv[1], url.removesuffix("/entry") + path,
                                 "AVC:0;HEVC:0", "video-only"], capture_output=True, text=True, timeout=50)
        assert result.returncode == 0, result.stdout + result.stderr
        print(f"Passthrough: {path}, two sessions passed")
finally:
    server.shutdown()
    server.server_close()
