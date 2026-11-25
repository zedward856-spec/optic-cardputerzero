#!/usr/bin/env python3
"""Velvet browser bridge: lets the Velvet web demo (e.g. on your phone) use a real Chromium
running on this PC as its FULL engine, so Google and every other site show *inside* the emulator.

    python3 tools/velvet-bridge.py            # listens on this PC only (127.0.0.1:8766)
    adb reverse tcp:8766 tcp:8766             # phone over USB: the demo finds it at localhost:8766
    python3 tools/velvet-bridge.py --lan      # also on Wi-Fi: anyone on your network could drive this
                                              # browser, so only on a network you trust; open the demo
                                              # with ?bridge=http://<PC address>:8766

Endpoints (all GET, CORS open so the demo page can call them):
    /state                          {"ready","loading","url","title","height","loads"}
    /nav?url=U   /back   /reload
    /shot?y=Y                       PNG of the 512x384 viewport scrolled to Y
                                    (headers X-Tile-Y, X-Height, X-Url, X-Title)
    /click?x=X&y=Y                  left click at document coordinates
    /type?text=T&enter=1            insert text (and press Enter)
    /probe?x=X&y=Y                  "" | "INPUT" | "BUTTON" | "LINK <url>"
Only this PC's Chromium profile is used: ~/.cache/velvet/bridge-chromium.
"""
import base64, html as htmllib, json, os, re, shutil, subprocess, sys, threading, time, urllib.parse, urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

W, H = 512, 768
UA = "Mozilla/5.0 (Linux; CardputerZero) Velvet/1.0"  # Velvet's own, honest identity

# Search engines treat a headless Chromium as a bot (robot checks, empty results). So searches
# are fetched as a plain request, the way Velvet's FAST engine does, from DuckDuckGo's lite
# page (made for simple browsers); the result page is then shown in Chromium, and the sites you
# open from it load in Chromium normally.
HOME_HTML = """<!doctype html><meta name=viewport content="width=512"><title>Velvet</title>
<style>body{font:16px/1.5 sans-serif;margin:24px;color:#15161c}h1{font:900 34px Impact,sans-serif;margin:0 0 8px}
p{color:#555}a{color:#1f3fd6}li{margin:6px 0}</style>
<h1>VELVET BROWSER</h1><p>Press <b>TAB</b> and type words to search (DuckDuckGo), or an address.</p>
<ul><li><a href="https://en.wikipedia.org/wiki/Special:Random">Wikipedia: random article</a></li>
<li><a href="https://news.ycombinator.com/">Hacker News</a></li><li><a href="https://www.reddit.com/">Reddit</a></li>
<li><a href="https://r6.tracker.network/">R6 Tracker</a></li><li><a href="https://text.npr.org/">NPR text news</a></li></ul>"""


def ddg_unwrap(href):
    """//duckduckgo.com/l/?uddg=<target>&... -> target"""
    m = re.search(r"[?&]uddg=([^&]+)", htmllib.unescape(href))
    return urllib.parse.unquote(m.group(1)) if m else href


def search_page(q):
    req = urllib.request.Request("https://lite.duckduckgo.com/lite/?q=" + urllib.parse.quote_plus(q),
                                 headers={"User-Agent": UA})
    page = urllib.request.urlopen(req, timeout=20).read().decode("utf-8", "replace")
    page = re.sub(r'href="(//duckduckgo\.com/l/\?[^"]*)"', lambda m: 'href="' + htmllib.escape(ddg_unwrap(m.group(1))) + '"', page)
    page = page.replace("<head>", '<head><base href="https://lite.duckduckgo.com/lite/">', 1)
    return page
# Runs in every page before its own scripts: links and window.open stay in this one tab
# (a headless browser has nowhere to show a new tab).
SAME_TAB_JS = r"""
(() => {
  // the real address behind a search engine's click-tracking link (Bing /ck/a?...&u=a1<base64>)
  window.__vh = a => {
    try {
      const h = new URL(a.href);
      if (/(^|\.)bing\.com$/.test(h.hostname) && h.pathname === '/ck/a') {
        const u = h.searchParams.get('u');
        if (u && u.startsWith('a1')) {
          let b = u.slice(2).replace(/-/g, '+').replace(/_/g, '/');
          while (b.length % 4) b += '=';
          const d = atob(b);
          return d.startsWith('/') ? h.origin + d : d;
        }
      }
    } catch (e) {}
    return a.href;
  };
  const fix = e => {
    const a = e.target && e.target.closest && e.target.closest('a[href]');
    if (!a) return;
    if (a.target && a.target !== '_self') a.target = '_self';  // no new tabs: nowhere to show them
    const real = window.__vh(a);
    if (real !== a.href) a.href = real;
  };
  document.addEventListener('click', fix, true);
  document.addEventListener('auxclick', fix, true);
  const open = window.open;
  window.open = function (u) { if (u) { location.href = u; return null; } return open.apply(this, arguments); };
})();
"""
PORT = int(os.environ.get("VELVET_BRIDGE_PORT", "8766"))
_here = os.path.dirname(os.path.abspath(__file__))
VELVET_HTML = [os.environ.get("VELVET_HTML", ""), "/sdcard/Download/velvet.html",
               os.path.join(_here, "velvet.html"), os.path.join(_here, "..", "velvet-share.html")]


class Chrome:
    def __init__(self):
        exe = next((shutil.which(n) for n in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable",
                                              "chrome-headless-shell") if shutil.which(n)), None)
        if not exe and os.path.exists(os.path.expandvars("$PREFIX/lib/chromium/chrome")):
            exe = os.path.expandvars("$PREFIX/lib/chromium/chrome")
        if not exe:
            sys.exit("No Chromium / Chrome found on this PC.")
        profile = os.path.expanduser("~/.cache/velvet/bridge-chromium")
        os.makedirs(profile, exist_ok=True)
        to_r, self.to_w = os.pipe()    # we write commands, Chrome reads fd 3
        self.from_r, from_w = os.pipe()  # Chrome writes fd 4, we read

        def child():  # Chrome expects its pipe on fds 3 and 4
            a, b = os.dup(to_r), os.dup(from_w)
            os.dup2(a, 3)
            os.dup2(b, 4)

        # Termux (Android): no user namespaces, so Chromium's sandbox can't start
        termux = "com.termux" in os.environ.get("PREFIX", "")
        extra = ["--no-sandbox", "--no-zygote"] if termux else []
        self.proc = subprocess.Popen(
            [exe, *extra, "--headless=new", "--remote-debugging-pipe", "--disable-gpu", "--no-first-run",
             "--no-default-browser-check", "--disable-extensions", "--mute-audio", "--hide-scrollbars",
             "--force-device-scale-factor=1", "--lang=en-US", f"--window-size={W},{H}",
             f"--user-data-dir={profile}", "about:blank"],
            close_fds=False, preexec_fn=child,  # (close_fds would also close the new fds 3 and 4)
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=(None if os.environ.get("VELVET_BRIDGE_DEBUG") else subprocess.DEVNULL))
        os.close(to_r)
        os.close(from_w)
        self.lock = threading.Lock()
        self.next_id = 1
        self.waiting = {}
        self.session = None
        self.loading = False
        self.loads = 0
        self.url = ""
        threading.Thread(target=self._reader, daemon=True).start()
        targets = self.call("Target.getTargets")["targetInfos"]
        page = next(t["targetId"] for t in targets if t["type"] == "page")
        self.session = self.call("Target.attachToTarget", {"targetId": page, "flatten": True})["sessionId"]
        self.main_frame = None
        self.call("Page.enable", session=True)
        self.call("Page.addScriptToEvaluateOnNewDocument", {"source": SAME_TAB_JS}, session=True)
        self.main_frame = self.call("Page.getFrameTree", session=True)["frameTree"]["frame"]["id"]
        # new tabs that still get opened: show their page here instead, then close them
        self.call("Target.setDiscoverTargets", {"discover": True})
        self.call("Emulation.setDeviceMetricsOverride",
                  {"width": W, "height": H, "deviceScaleFactor": 1, "mobile": False}, session=True)

    def _reader(self):
        buf = b""
        while True:
            chunk = os.read(self.from_r, 1 << 16)
            if not chunk:
                os._exit(1)  # Chrome died
            buf += chunk
            while b"\0" in buf:
                msg, buf = buf.split(b"\0", 1)
                m = json.loads(msg)
                if "id" in m:
                    ev = self.waiting.pop(m["id"], None)
                    if ev:
                        ev[1] = m
                        ev[0].set()
                else:
                    meth = m.get("method", "")
                    p = m.get("params", {})
                    if meth == "Target.targetInfoChanged" or meth == "Target.targetCreated":
                        info = p.get("targetInfo", {})
                        url = info.get("url", "")
                        if info.get("type") == "page" and info.get("openerId") and url.startswith("http"):
                            threading.Thread(target=self._adopt, args=(info["targetId"], url), daemon=True).start()
                        continue
                    if meth in ("Page.frameStartedLoading", "Page.frameRequestedNavigation") and \
                            p.get("frameId") in (None, self.main_frame):  # the page itself, not its frames
                        self.loading = True
                    elif meth == "Page.loadEventFired":
                        self.loading = False
                        self.loads += 1
                    elif meth == "Page.frameNavigated" and "parentId" not in m["params"]["frame"]:
                        self.url = m["params"]["frame"]["url"]

    def _adopt(self, target, url):
        try:
            self.call("Target.closeTarget", {"targetId": target})
            self.loading = True
            self.call("Page.navigate", {"url": url}, session=True)
        except Exception:
            pass

    def call(self, method, params=None, session=False, timeout=30):
        with self.lock:
            i = self.next_id
            self.next_id += 1
            ev = [threading.Event(), None]
            self.waiting[i] = ev
            msg = {"id": i, "method": method}
            if params:
                msg["params"] = params
            if session:
                msg["sessionId"] = self.session
            os.write(self.to_w, json.dumps(msg).encode() + b"\0")
        ev[0].wait(timeout)
        res = ev[1] or {}
        if "error" in res:
            raise RuntimeError(res["error"].get("message", "CDP error"))
        return res.get("result", {})

    def js(self, expr):
        r = self.call("Runtime.evaluate", {"expression": expr, "returnByValue": True}, session=True)
        return r.get("result", {}).get("value")

    def show_html(self, html):
        """Puts our own HTML into the page (search results, the home page)."""
        self.loading = True
        self.call("Page.navigate", {"url": "about:blank"}, session=True)
        for _ in range(50):
            if (self.js("location.href") or "") == "about:blank":
                break
            time.sleep(0.05)
        self.call("Page.setDocumentContent", {"frameId": self.main_frame, "html": html}, session=True)
        self.loading = False
        self.loads += 1

    def info(self):
        v = self.js("JSON.stringify([window.scrollY,Math.max(document.documentElement.scrollHeight,"
                    "document.body?document.body.scrollHeight:0),location.href,document.title])")
        y, h, url, title = json.loads(v) if v else (0, H, self.url, "")
        return int(y), max(H, int(h)), url, title


chrome = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def cors(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Private-Network", "true")
        self.send_header("Access-Control-Expose-Headers", "X-Tile-Y, X-Height, X-Url, X-Title, X-Scale")
        self.send_header("Cache-Control", "no-store")

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.send_header("Access-Control-Allow-Methods", "GET")
        self.send_header("Access-Control-Allow-Headers", "*")
        self.end_headers()

    def reply(self, body, ctype="application/json", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(200)
        self.cors()
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, urllib.parse.quote(str(v), safe=":/?&=%#@+,;"))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = {k: v[0] for k, v in urllib.parse.parse_qs(u.query).items()}
        num = lambda k: int(float(q.get(k, 0)))
        try:
            if u.path in ("/", "/velvet.html"):
                # Velvet itself, so the page and the bridge share one origin (no cross-site block)
                for f in VELVET_HTML:
                    if os.path.isfile(f):
                        return self.reply(open(f, "rb").read(), "text/html; charset=utf-8")
                return self.reply(b"velvet.html not found", "text/plain")
            if u.path == "/state":
                _, h, url, title = chrome.info()
                ready_state = chrome.js("document.readyState") or ""
                if ready_state == "complete" and chrome.loading:
                    chrome.loading = False
                    chrome.loads += 1
                self.reply({"ready": True, "loading": chrome.loading, "url": url, "title": title,
                            "height": h, "loads": chrome.loads})
            elif u.path == "/nav":
                url = q.get("url", "")
                if not url.startswith(("http://", "https://")):
                    return self.reply({"ok": False})
                chrome.loading = True
                chrome.call("Page.navigate", {"url": url}, session=True)
                self.reply({"ok": True})
            elif u.path == "/search":
                chrome.show_html(search_page(q.get("q", "")))
                self.reply({"ok": True})
            elif u.path == "/home":
                chrome.show_html(HOME_HTML)
                self.reply({"ok": True})
            elif u.path == "/back":
                chrome.js("history.back()")
                self.reply({"ok": True})
            elif u.path == "/reload":
                chrome.loading = True
                chrome.call("Page.reload", session=True)
                self.reply({"ok": True})
            elif u.path == "/shot":
                chrome.js(f"window.scrollTo(0,{max(0, num('y'))})")
                y, h, url, title = chrome.info()
                scale = min(1.0, max(0.25, float(q.get("s", 1))))
                params = {"format": "png"}
                if scale < 1:  # zoomed out: Chrome draws the same page smaller (sharp text, same layout)
                    params["clip"] = {"x": 0, "y": y, "width": W, "height": H, "scale": scale}
                png = base64.b64decode(chrome.call("Page.captureScreenshot", params, session=True)["data"])
                self.reply(png, "image/png", {"X-Tile-Y": y, "X-Height": h, "X-Url": url, "X-Title": title,
                                              "X-Scale": scale})
            elif u.path == "/click":
                y0, _, _, _ = chrome.info()
                x, y = num("x"), num("y") - y0
                for t in ("mouseMoved", "mousePressed", "mouseReleased"):
                    p = {"type": t, "x": x, "y": y}
                    if t != "mouseMoved":
                        p.update(button="left", clickCount=1)
                    chrome.call("Input.dispatchMouseEvent", p, session=True)
                self.reply({"ok": True})
            elif u.path == "/type":
                if q.get("text"):
                    chrome.call("Input.insertText", {"text": q["text"]}, session=True)
                if q.get("enter") == "1":
                    k = {"key": "Enter", "code": "Enter", "windowsVirtualKeyCode": 13}
                    chrome.call("Input.dispatchKeyEvent", {"type": "rawKeyDown", **k}, session=True)
                    chrome.call("Input.dispatchKeyEvent", {"type": "char", "text": "\r"}, session=True)
                    chrome.call("Input.dispatchKeyEvent", {"type": "keyUp", **k}, session=True)
                self.reply({"ok": True})
            elif u.path == "/probe":
                y0, _, _, _ = chrome.info()
                js = ("(()=>{const e=document.elementFromPoint(%d,%d);if(!e)return '';"
                      "const t=e.closest('input,textarea,[contenteditable=true],select');"
                      "if(t&&!/^(submit|button|image|checkbox|radio|reset)$/i.test(t.type||''))return 'INPUT';"
                      "const a=e.closest('a[href]');if(a)return 'LINK '+(window.__vh?window.__vh(a):a.href);"
                      "if(e.tagName==='IFRAME'||e.closest('button,[role=button],input,[onclick],summary,label'))return 'BUTTON';return ''})()"
                      % (num("x"), num("y") - y0))
                self.reply({"hover": chrome.js(js) or ""})
            else:
                self.send_response(404)
                self.cors()
                self.end_headers()
        except Exception as e:  # keep serving
            self.reply({"error": str(e)})


if __name__ == "__main__":
    lan = "--lan" in sys.argv
    chrome = Chrome()
    host = "0.0.0.0" if lan else "127.0.0.1"
    print(f"Velvet bridge ready on {host}:{PORT}. Open http://localhost:{PORT}/velvet.html", flush=True)
    if lan:
        print("Listening on the network too: anyone on this Wi-Fi can use this browser.", flush=True)
    ThreadingHTTPServer((host, PORT), Handler).serve_forever()
