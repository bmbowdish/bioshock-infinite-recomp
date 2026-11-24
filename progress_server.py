#!/usr/bin/env python3
"""Corner-of-the-screen progress widget for the BioShock Infinite recomp.
Run:   cd ~/Downloads/bioshock-infinite-recomp && source .venv/bin/activate && python3 progress_server.py
Open:  http://localhost:8765   (resize the window small and park it on monitor 2)
Reads run_out.txt, build.log, build/BioShock3, gen2/funcs.h, starts.txt from this folder. Binds to 127.0.0.1 only."""
import http.server, json, os, re, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
P = lambda *a: os.path.join(HERE, *a)
HIST = P("progress_history.json")
PORT = 8765

# The stops we already fixed earlier in this session (seeded from the chat; no line counts for these).
SEED = [
    "table pointer corrupted (import list overflowed)",
    "call through a blank allocator vtable",
    "std::string::_M_mutate missing",
    "Deque_base init (game fn routed to stub)",
    "___bzero", "pthread_mutexattr_destroy", "memset_pattern16", "strlcpy",
    "pthread_key_create", "wcslen", "CFStringCreateWithCharacters",
    "jump to unknown function 0x809ca2",
]

def read(path, tail=None):
    try:
        with open(path, "rb") as f:
            if tail:
                f.seek(0, 2); n = f.tell(); f.seek(max(0, n - tail))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""

def mtime(path):
    try: return os.path.getmtime(path)
    except OSError: return 0

def short_reason(text):
    lines = [l for l in text.splitlines() if l.strip()]
    for l in reversed(lines):
        if l.startswith("[rt]"):
            s = l[4:].strip()
            m = re.match(r"unimplemented import (\S+)", s)
            if m: return "missing import " + m.group(1)
            m = re.match(r"indirect call to unknown (0x[0-9a-f]+)", s)
            if m: return "jump to unknown function " + m.group(1)
            return s[:90]
    if re.search(r"signal 11|EXC_BAD_ACCESS|Segmentation", text): return "crash (signal 11)"
    return "no abort message" if lines else "no output yet"

# ---- history of stops ----
hist_lock = threading.Lock()
def load_hist():
    try: return json.load(open(HIST))
    except Exception: return None
def save_hist(h):
    try: json.dump(h, open(HIST, "w"))
    except OSError: pass

def update_hist():
    with hist_lock:
        h = load_hist()
        if h is None:
            h = {"last_mtime": 0, "runs": [{"reason": r, "seed": True} for r in SEED]}
        mt = mtime(P("run_out.txt"))
        if mt and mt != h["last_mtime"] and time.time() - mt > 3:   # only once the run has stopped writing
            txt = read(P("run_out.txt"))
            h["runs"].append({"reason": short_reason(txt), "t": mt, "lines": txt.count("\n"),
                              "watch": len(re.findall(r"^\[watch\]", txt, re.M))})
            h["runs"] = h["runs"][-80:]
            h["last_mtime"] = mt
        save_hist(h)
        return h

# ---- import coverage (needs `lief`, which the project .venv has) ----
cov = {"state": "idle", "real": None, "total": None, "key": None}
def compute_cov():
    try:
        import lief
        used = set(re.findall(r"void I_(\w+)\(CPU \*c\);", read(P("gen2", "funcs.h"))))
        b = lief.parse(P("build", "BioShock3"))
        syms = sorted((s.value, s.name[3:]) for s in b.symbols if s.name.startswith("_I_") and s.value)
        clusters, cur = [], [syms[0]]
        for a, n in syms[1:]:
            if a - cur[-1][0] <= 96: cur.append((a, n))
            else: clusters.append(cur); cur = [(a, n)]
        clusters.append(cur)
        weak = {n for a, n in max(clusters, key=len)}   # the block of default "unimplemented" stubs
        cov.update(state="ok", total=len(used), real=sum(1 for u in used if u not in weak))
    except Exception as e:
        cov.update(state="n/a", err=str(e)[:80])

def maybe_cov():
    key = (mtime(P("build", "BioShock3")), mtime(P("gen2", "funcs.h")))
    if key != cov["key"] and cov["state"] != "busy" and key[0]:
        cov.update(key=key, state="busy")
        threading.Thread(target=compute_cov, daemon=True).start()

def loadj(path):
    try: return json.load(open(path))
    except Exception: return None

def status():
    h = update_hist(); maybe_cov()
    out = read(P("run_out.txt")); blog = read(P("build.log"))
    now = time.time()
    return {
        "now": now,
        "stop": short_reason(out), "stop_full": next((l for l in reversed(out.splitlines()) if l.startswith("[rt]")), ""),
        "run_age": now - mtime(P("run_out.txt")) if mtime(P("run_out.txt")) else None,
        "build_age": now - mtime(P("build.log")) if mtime(P("build.log")) else None,
        "build_errors": len(re.findall(r"error:", blog)), "binary": bool(mtime(P("build", "BioShock3"))),
        "watch": len(re.findall(r"^\[watch\]", out, re.M)),
        "starts": len([l for l in read(P("starts.txt")).splitlines() if l.strip()]),
        "cov": {k: cov.get(k) for k in ("state", "real", "total")},
        "runs": h["runs"],
        "stage": loadj(P("progress_stage.json")), "notes": loadj(P("progress_notes.json")),
    }

PAGE = r"""<!doctype html><meta charset=utf-8><title>BioShock recomp</title>
<style>
:root{--bg:#0f1115;--card:#171a21;--tx:#e6e8ee;--mu:#8a90a2;--ok:#4cc38a;--warn:#f5a524;--bad:#ef5b5b;--acc:#6ea8fe}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--tx);font:13px/1.35 -apple-system,system-ui,sans-serif;padding:10px}
.w{max-width:360px;margin:0 auto;display:grid;gap:8px}
.c{background:var(--card);border-radius:10px;padding:10px 12px}
h1{font-size:12px;margin:0;color:var(--mu);font-weight:600;letter-spacing:.04em;text-transform:uppercase;display:flex;justify-content:space-between}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;background:var(--warn)}
.stop{font:600 14px ui-monospace,Menlo,monospace;word-break:break-all;margin-top:6px}
.mu{color:var(--mu);font-size:11px}
.bar{height:8px;background:#242936;border-radius:5px;overflow:hidden;margin:6px 0 2px}.bar i{display:block;height:100%;background:var(--acc)}
.row{display:flex;justify-content:space-between;align-items:baseline}.big{font-size:20px;font-weight:700}
ul{list-style:none;margin:6px 0 0;padding:0}li{padding:2px 0;font:11.5px ui-monospace,Menlo,monospace;display:flex;gap:6px}
li .k{color:var(--ok)}li.cur{color:var(--warn);font-weight:700}li.cur .k{color:var(--warn)}
.pill{display:none;margin-top:8px;font-size:11.5px;color:var(--acc);font-weight:600}.pill.on{display:block}.pill.on:before{content:"";display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--acc);margin-right:6px;animation:p 1s infinite}@keyframes p{50%{opacity:.25}}.note{font-size:13px}
.ms li{font:13px -apple-system,system-ui,sans-serif}.ms .k{width:14px}
svg{width:100%;height:36px;display:block;margin-top:4px}
</style>
<div class=w>
 <div class=c><h1><span><span class=dot id=dot></span>BioShock Infinite recomp</span><span id=ago></span></h1>
  <div class=stop id=stop>…</div><div class=mu id=sub></div><div class=pill id=stg></div></div>
 <div class=c id=notesc style="display:none"><h1>Now</h1><div id=now class=note></div><div class=mu id=next style="margin-top:4px"></div></div>
 <div class=c><div class=row><span class=mu>Import shims implemented</span><span class=big id=covn>–</span></div>
  <div class=bar><i id=covb style="width:0"></i></div><div class=mu id=covt></div></div>
 <div class=c><div class=row><span class=mu>Fixes so far</span><span class=big id=rounds>–</span></div>
  <div class=mu>how far startup ran each run (log lines)</div><svg id=spark viewBox="0 0 300 36" preserveAspectRatio=none></svg></div>
 <div class=c><h1>Trail</h1><ul id=trail></ul></div>
 <div class=c><h1>Milestones</h1><ul class=ms id=ms></ul></div>
</div>
<script>
const $=id=>document.getElementById(id);
const age=s=>s==null?'–':s<60?Math.round(s)+'s ago':s<3600?Math.round(s/60)+'m ago':Math.round(s/3600)+'h ago';
const esc=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
async function tick(){
 let d;try{d=await (await fetch('/status',{cache:'no-store'})).json()}catch(e){$('stop').textContent='server not reachable';return}
 $('stop').textContent=d.stop;$('stop').title=d.stop_full;
 $('sub').textContent='run '+age(d.run_age)+' · build '+age(d.build_age)+(d.build_errors?' · '+d.build_errors+' build errors':' · build ok');
 $('dot').style.background=d.build_errors?'var(--bad)':(d.run_age!=null&&d.run_age<8?'var(--ok)':'var(--warn)');
 $('ago').textContent=new Date(d.now*1000).toLocaleTimeString([], {hour:'2-digit',minute:'2-digit'});
 const c=d.cov;if(c.state==='ok'){const p=c.real/c.total*100;$('covn').textContent=Math.round(p)+'%';$('covb').style.width=p+'%';$('covt').textContent=c.real+' of '+c.total+' imports the game uses (approx.)'}
 else{$('covn').textContent='–';$('covt').textContent=c.state==='busy'?'computing…':'needs lief: run with the project .venv'}
 const runs=d.runs;$('rounds').textContent=runs.length-1;
 const pts=runs.filter(r=>!r.seed).slice(-14).map(r=>r.lines);
 if(pts.length>1){const mx=Math.max(...pts,1),w=300/(pts.length-1);
  $('spark').innerHTML='<polyline fill=none stroke="#6ea8fe" stroke-width=2 points="'+pts.map((v,i)=>(i*w).toFixed(1)+','+(34-v/mx*32).toFixed(1)).join(' ')+'"/>'}
 else $('spark').innerHTML='';
 const last=runs.slice(-7);$('trail').innerHTML=last.map((r,i)=>'<li class="'+(i==last.length-1?'cur':'')+'"><span class=k>'+(i==last.length-1?'▸':'✓')+'</span><span>'+esc(r.reason)+'</span></li>').join('');
 const n=d.notes||{};
 $('notesc').style.display=n.now?'block':'none';$('now').textContent=n.now||'';$('next').textContent=n.next?'Next: '+n.next:'';
 const ms=n.milestones||[{t:'Builds and links',s:d.binary&&!d.build_errors?'done':'todo'},{t:'Startup code runs',s:'doing'},{t:'Engine entry points found',s:'todo'},{t:'Window opens',s:'todo'},{t:'First frame renders',s:'todo'},{t:'Main menu',s:'todo'}];
 $('ms').innerHTML=ms.map(m=>'<li><span class=k>'+(m.s=='done'?'✓':m.s=='doing'?'◐':'○')+'</span><span'+(m.s=='done'?'':' style="color:var(--mu)"')+'>'+esc(m.t)+'</span></li>').join('');
 const g=d.stage,pill=$('stg');
 if(g&&g.stage&&g.stage!=='Idle'&&d.now-g.t<900){pill.className='pill on';pill.textContent=g.stage+' · '+age(d.now-g.t).replace(' ago','')}
 else if(g&&g.exit!=null&&d.now-g.t<900){pill.className='pill';pill.textContent=''}
 else pill.className='pill';
}
tick();setInterval(tick,2000);
</script>"""

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path.split("?")[0] == "/status":
            body, ct = json.dumps(status()).encode(), "application/json"
        elif self.path.split("?")[0] in ("/", "/index.html"):
            body, ct = PAGE.encode(), "text/html; charset=utf-8"
        else:
            self.send_response(404); self.end_headers(); return
        self.send_response(200); self.send_header("Content-Type", ct)
        self.send_header("Cache-Control", "no-store"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)

if __name__ == "__main__":
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("Progress widget: http://localhost:%d   (Ctrl-C to stop)" % PORT)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass
