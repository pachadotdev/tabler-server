#include "admin.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "http.hpp"
#include "log.hpp"
#include "metrics.hpp"

namespace ts {

namespace {

bool send_all(int fd, const char *data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    sent += static_cast<size_t>(n);
  }
  return true;
}

bool send_all(int fd, const std::string &s) {
  return send_all(fd, s.data(), s.size());
}

std::string http_response(int status, const std::string &reason,
                          const std::string &ctype, const std::string &body) {
  return "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n" +
         "Content-Type: " + ctype + "\r\n" +
         "Content-Length: " + std::to_string(body.size()) + "\r\n" +
         "Cache-Control: no-store\r\n" +
         "Connection: close\r\n\r\n" + body;
}

long clk_tck() {
  static long v = ::sysconf(_SC_CLK_TCK);
  return v > 0 ? v : 100;
}

long num_cpus() {
  static long v = ::sysconf(_SC_NPROCESSORS_ONLN);
  return v > 0 ? v : 1;
}

long page_size() {
  static long v = ::sysconf(_SC_PAGESIZE);
  return v > 0 ? v : 4096;
}

// Aggregate CPU jiffies from the first line of /proc/stat.
// Returns total ticks and, via `idle_out`, the idle+iowait ticks.
uint64_t read_system_cpu(uint64_t &idle_out) {
  idle_out = 0;
  std::ifstream f("/proc/stat");
  if (!f) return 0;
  std::string cpu;
  f >> cpu;
  if (cpu != "cpu") return 0;
  uint64_t total = 0, idle = 0, val = 0;
  int field = 0;
  while (f >> val) {
    total += val;
    // fields (0-based after "cpu"): 3=idle, 4=iowait
    if (field == 3 || field == 4) idle += val;
    ++field;
    if (field >= 10) break;
  }
  idle_out = idle;
  return total;
}

// utime+stime (ticks) for a process from /proc/<pid>/stat, or 0 if unreadable.
uint64_t read_proc_cpu_ticks(pid_t pid) {
  std::string path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream f(path);
  if (!f) return 0;
  std::string content((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  // The comm field is wrapped in parentheses and may contain spaces; parse the
  // fields after the final ')'.
  size_t rp = content.rfind(')');
  if (rp == std::string::npos) return 0;
  std::istringstream rest(content.substr(rp + 1));
  std::string tok;
  int idx = 0;  // token[0] == field 3 (state)
  uint64_t utime = 0, stime = 0;
  while (rest >> tok) {
    if (idx == 11) utime = std::strtoull(tok.c_str(), nullptr, 10);  // field 14
    if (idx == 12) {                                                 // field 15
      stime = std::strtoull(tok.c_str(), nullptr, 10);
      break;
    }
    ++idx;
  }
  return utime + stime;
}

// Resident set size (bytes) from /proc/<pid>/statm, or 0 if unreadable.
uint64_t read_proc_rss(pid_t pid) {
  std::string path = "/proc/" + std::to_string(pid) + "/statm";
  std::ifstream f(path);
  if (!f) return 0;
  uint64_t size_pages = 0, rss_pages = 0;
  f >> size_pages >> rss_pages;
  return rss_pages * static_cast<uint64_t>(page_size());
}

// System memory (bytes) from /proc/meminfo. Sets total and used (= total-avail).
void read_system_mem(uint64_t &total_out, uint64_t &used_out) {
  total_out = 0;
  used_out = 0;
  std::ifstream f("/proc/meminfo");
  if (!f) return;
  std::string key;
  uint64_t total_kb = 0, avail_kb = 0;
  uint64_t val = 0;
  std::string unit;
  while (f >> key >> val >> unit) {
    if (key == "MemTotal:") total_kb = val;
    else if (key == "MemAvailable:") avail_kb = val;
    if (total_kb && avail_kb) break;
  }
  total_out = total_kb * 1024;
  used_out = (total_kb > avail_kb ? total_kb - avail_kb : 0) * 1024;
}

std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string num(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f", v);
  return buf;
}

// Extracts `key`'s value from a request target's query string ("/path?a=b&c=d"),
// or "" if absent. No percent-decoding: sids are restricted to [a-z0-9].
std::string query_param(const std::string &target, const std::string &key) {
  size_t q = target.find('?');
  if (q == std::string::npos) return "";
  std::string qs = target.substr(q + 1);
  size_t pos = 0;
  while (pos < qs.size()) {
    size_t amp = qs.find('&', pos);
    size_t end = amp == std::string::npos ? qs.size() : amp;
    size_t eq = qs.find('=', pos);
    if (eq != std::string::npos && eq < end && qs.compare(pos, eq - pos, key) == 0) {
      return qs.substr(eq + 1, end - eq - 1);
    }
    if (amp == std::string::npos) break;
    pos = amp + 1;
  }
  return "";
}

// Session ids (see proxy.cpp's random_sid()) are always 24 lowercase
// alphanumeric characters; reject anything else before using one as a map key.
bool is_valid_sid(const std::string &sid) {
  if (sid.empty() || sid.size() > 64) return false;
  for (char c : sid) {
    if (!std::isalnum(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

const char *kDashboardHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>tabler-server monitor</title>
<style>
  :root { --bg:#0b0f14; --panel:#111821; --fg:#c9d5e0; --dim:#6b7a8a;
          --grn:#3fb950; --yel:#d29922; --red:#f85149; --blu:#58a6ff; --accent:#238636; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg);
         font:13px/1.4 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace; }
  header { display:flex; align-items:baseline; gap:1rem; padding:.6rem 1rem;
           border-bottom:1px solid #1c2530; }
  header h1 { font-size:14px; margin:0; font-weight:600; letter-spacing:.02em; }
  header .meta { color:var(--dim); font-size:12px; }
  .wrap { padding:1rem; display:grid; gap:1rem; }
  .gauges { display:grid; grid-template-columns:repeat(auto-fit,minmax(240px,1fr)); gap:1rem; }
  .card { background:var(--panel); border:1px solid #1c2530; border-radius:8px; padding:.75rem .9rem; }
  .card h2 { margin:0 0 .5rem; font-size:11px; text-transform:uppercase;
             letter-spacing:.08em; color:var(--dim); font-weight:600; }
  .big { font-size:20px; font-weight:600; }
  .row { display:flex; justify-content:space-between; align-items:baseline; gap:.5rem; }
  .bar { height:10px; background:#0c1219; border-radius:5px; overflow:hidden; margin-top:.4rem; }
  .bar > span { display:block; height:100%; width:0; transition:width .4s ease; background:var(--grn); }
  canvas { width:100%; height:48px; display:block; margin-top:.4rem; }
  table { width:100%; border-collapse:collapse; }
  .card.table { padding:0; overflow:hidden; }
  .card.table h2 { padding:.75rem .9rem 0; }
  th,td { text-align:right; padding:.4rem .6rem; white-space:nowrap; }
  th:first-child, td:first-child, th:nth-child(2), td:nth-child(2) { text-align:left; }
  thead th { color:var(--dim); font-weight:600; font-size:11px; text-transform:uppercase;
             letter-spacing:.05em; border-bottom:1px solid #1c2530; }
  tbody tr:nth-child(even) { background:#0e141c; }
  tbody td { border-bottom:1px solid #131b24; }
  .dim { color:var(--dim); }
  .dot { display:inline-block; width:8px; height:8px; border-radius:50%; margin-right:.35rem; vertical-align:middle; }
  .up { background:var(--grn); } .idle { background:var(--yel); }
  .empty { padding:1.5rem; text-align:center; color:var(--dim); }
  .mini { font-size:11px; }
  .actions { text-align:right; white-space:nowrap; }
  .btn { font:inherit; font-size:11px; padding:.2rem .5rem; margin-left:.35rem;
         border-radius:5px; border:1px solid #2a3542; background:#161e28;
         color:var(--fg); cursor:pointer; }
  .btn:hover { border-color:#3a4a5c; }
  .btn:disabled { opacity:.5; cursor:default; }
  .btn-reload:hover { color:var(--blu); border-color:var(--blu); }
  .btn-stop:hover { color:var(--red); border-color:var(--red); }
</style>
</head>
<body>
<header>
  <h1>tabler-server</h1>
  <span class="meta" id="meta">connecting…</span>
</header>
<div class="wrap">
  <div class="gauges">
    <div class="card">
      <h2>System CPU</h2>
      <div class="row"><span class="big" id="sys-cpu">–</span><span class="dim mini" id="sys-cpu-n"></span></div>
      <div class="bar"><span id="sys-cpu-bar"></span></div>
      <canvas id="c-syscpu"></canvas>
    </div>
    <div class="card">
      <h2>System Memory</h2>
      <div class="row"><span class="big" id="sys-mem">–</span><span class="dim mini" id="sys-mem-n"></span></div>
      <div class="bar"><span id="sys-mem-bar"></span></div>
      <canvas id="c-sysmem"></canvas>
    </div>
    <div class="card">
      <h2>Workers CPU / Mem</h2>
      <div class="row"><span class="big" id="w-cpu">–</span><span class="dim mini" id="w-mem"></span></div>
      <div class="row mini dim"><span id="w-count">0 workers</span><span id="w-conns">0 conns</span></div>
      <canvas id="c-wcpu"></canvas>
    </div>
    <div class="card">
      <h2>Network</h2>
      <div class="row"><span class="big" style="color:var(--blu)" id="net-in">↓ –</span>
                       <span class="big" style="color:var(--grn)" id="net-out">↑ –</span></div>
      <div class="row mini dim"><span id="net-in-t"></span><span id="net-out-t"></span></div>
      <canvas id="c-net"></canvas>
    </div>
  </div>

  <div class="card table">
    <h2>Running apps</h2>
    <table>
      <thead><tr>
        <th>App</th><th>Session</th><th>PID</th><th>Uptime</th>
        <th>Mem</th><th>CPU%</th><th>Conns</th><th>↓ In</th><th>↑ Out</th><th></th>
      </tr></thead>
      <tbody id="rows"><tr><td class="empty" colspan="10">No running apps</td></tr></tbody>
    </table>
  </div>
</div>

<script>
const HIST = 60;
const series = { syscpu:[], sysmem:[], wcpu:[], netin:[], netout:[] };
let prev = null, prevT = 0;

function fmtBytes(b){
  if (b < 1024) return b + " B";
  const u = ["KB","MB","GB","TB"]; let i=-1;
  do { b/=1024; i++; } while (b>=1024 && i<u.length-1);
  return b.toFixed(b<10?1:0)+" "+u[i];
}
function fmtRate(bps){ return fmtBytes(bps)+"/s"; }
function fmtDur(s){
  s=Math.floor(s);
  const h=Math.floor(s/3600), m=Math.floor(s%3600/60), sec=s%60;
  if (h) return h+"h"+String(m).padStart(2,"0")+"m";
  if (m) return m+"m"+String(sec).padStart(2,"0")+"s";
  return sec+"s";
}
function push(arr,v){ arr.push(v); if (arr.length>HIST) arr.shift(); }

async function sessionAction(path, sid, btn){
  if (btn) btn.disabled = true;
  try {
    await fetch(path+"?sid="+encodeURIComponent(sid), {method:"POST"});
  } catch(e) { /* dashboard poll will reflect the current state regardless */ }
  tick();
}
function reloadApp(sid, btn){ sessionAction("api/reload", sid, btn); }
function stopApp(sid, btn){
  if (!confirm("Stop this app? Unsaved state for this session will be lost.")) return;
  sessionAction("api/stop", sid, btn);
}

function draw(id, arr, color, max){
  const cv = document.getElementById(id);
  const dpr = window.devicePixelRatio||1;
  const w = cv.clientWidth, h = cv.clientHeight;
  cv.width = w*dpr; cv.height = h*dpr;
  const ctx = cv.getContext("2d"); ctx.scale(dpr,dpr);
  ctx.clearRect(0,0,w,h);
  if (!arr.length) return;
  const top = max || Math.max(1, ...arr);
  const step = w/(HIST-1);
  ctx.beginPath();
  arr.forEach((v,i)=>{
    const x = w - (arr.length-1-i)*step;
    const y = h - (v/top)*(h-2) - 1;
    i? ctx.lineTo(x,y): ctx.moveTo(x,y);
  });
  ctx.lineWidth = 1.5; ctx.strokeStyle = color; ctx.stroke();
  ctx.lineTo(w, h); ctx.lineTo(w-(arr.length-1)*step, h); ctx.closePath();
  ctx.globalAlpha = 0.12; ctx.fillStyle = color; ctx.fill(); ctx.globalAlpha = 1;
}
const COLORS = { grn:"#3fb950", yel:"#d29922", red:"#f85149", blu:"#58a6ff" };
function cpuColor(p){ return p>85?COLORS.red:p>60?COLORS.yel:COLORS.grn; }

async function tick(){
  let d;
  try { d = await (await fetch("stats.json",{cache:"no-store"})).json(); }
  catch(e){ document.getElementById("meta").textContent="disconnected"; return; }

  const now = d.time, dt = prev ? Math.max(0.001, now - prevT) : 0;
  document.getElementById("meta").textContent =
    d.ncpu+" CPUs · "+new Date(now*1000).toLocaleTimeString();

  // system cpu
  const sc = d.system.cpu_pct;
  document.getElementById("sys-cpu").textContent = sc.toFixed(1)+"%";
  const scBar = document.getElementById("sys-cpu-bar");
  scBar.style.width = Math.min(100,sc)+"%"; scBar.style.background = cpuColor(sc);
  push(series.syscpu, sc); draw("c-syscpu", series.syscpu, cpuColor(sc), 100);

  // system mem
  const mp = d.system.mem_total? d.system.mem_used/d.system.mem_total*100:0;
  document.getElementById("sys-mem").textContent = mp.toFixed(0)+"%";
  document.getElementById("sys-mem-n").textContent =
    fmtBytes(d.system.mem_used)+" / "+fmtBytes(d.system.mem_total);
  const smBar = document.getElementById("sys-mem-bar");
  smBar.style.width = Math.min(100,mp)+"%"; smBar.style.background = cpuColor(mp);
  push(series.sysmem, mp); draw("c-sysmem", series.sysmem, COLORS.grn, 100);

  // workers totals
  document.getElementById("w-cpu").textContent = d.totals.cpu_pct.toFixed(1)+"%";
  document.getElementById("w-mem").textContent = fmtBytes(d.totals.mem);
  document.getElementById("w-count").textContent = d.totals.workers+" workers";
  document.getElementById("w-conns").textContent = d.totals.conns+" conns";
  push(series.wcpu, d.totals.cpu_pct);
  draw("c-wcpu", series.wcpu, cpuColor(d.totals.cpu_pct), 100*d.ncpu);

  // network rates
  let inRate=0, outRate=0;
  if (prev && dt>0){
    inRate = Math.max(0,(d.net.bytes_in - prev.net.bytes_in)/dt);
    outRate = Math.max(0,(d.net.bytes_out - prev.net.bytes_out)/dt);
  }
  document.getElementById("net-in").textContent = "↓ "+fmtRate(inRate);
  document.getElementById("net-out").textContent = "↑ "+fmtRate(outRate);
  document.getElementById("net-in-t").textContent = fmtBytes(d.net.bytes_in)+" total";
  document.getElementById("net-out-t").textContent = fmtBytes(d.net.bytes_out)+" total";
  push(series.netin, inRate); push(series.netout, outRate);
  const netMax = Math.max(1, ...series.netin, ...series.netout);
  const cv=document.getElementById("c-net");
  const dpr=window.devicePixelRatio||1, w=cv.clientWidth, h=cv.clientHeight;
  cv.width=w*dpr; cv.height=h*dpr; const ctx=cv.getContext("2d"); ctx.scale(dpr,dpr);
  ctx.clearRect(0,0,w,h);
  const line=(arr,color)=>{
    const step=w/(HIST-1);
    ctx.beginPath();
    arr.forEach((v,i)=>{const x=w-(arr.length-1-i)*step,y=h-(v/netMax)*(h-2)-1; i?ctx.lineTo(x,y):ctx.moveTo(x,y);});
    ctx.lineWidth=1.5; ctx.strokeStyle=color; ctx.stroke();
  };
  line(series.netin,COLORS.blu); line(series.netout,COLORS.grn);

  // table
  const rows = document.getElementById("rows");
  if (!d.workers.length){
    rows.innerHTML = '<tr><td class="empty" colspan="10">No running apps</td></tr>';
  } else {
    rows.innerHTML = d.workers.map(w=>{
      const active = w.conns>0;
      const sid = w.sid.replace(/'/g,"");
      return "<tr>"+
        "<td><span class='dot "+(active?"up":"idle")+"'></span>"+w.app+"</td>"+
        "<td class='dim'>"+w.sid.slice(0,8)+"</td>"+
        "<td>"+w.pid+"</td>"+
        "<td>"+fmtDur(w.uptime)+"</td>"+
        "<td>"+fmtBytes(w.mem)+"</td>"+
        "<td style='color:"+cpuColor(w.cpu_pct)+"'>"+w.cpu_pct.toFixed(1)+"</td>"+
        "<td>"+w.conns+"</td>"+
        "<td>"+fmtBytes(w.bytes_in)+"</td>"+
        "<td>"+fmtBytes(w.bytes_out)+"</td>"+
        "<td class='actions'>"+
          "<button class='btn btn-reload' onclick=\"reloadApp('"+sid+"',this)\">Reload</button>"+
          "<button class='btn btn-stop' onclick=\"stopApp('"+sid+"',this)\">Stop</button>"+
        "</td>"+
      "</tr>";
    }).join("");
  }


  prev = d; prevT = now;
}
tick();
setInterval(tick, 2000);
</script>
</body>
</html>)HTML";

}  // namespace

AdminServer::AdminServer(const Config &cfg, WorkerManager &workers)
    : cfg_(cfg), workers_(workers) {}

void AdminServer::stop() {
  running_ = false;
  if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
}

std::string AdminServer::build_stats_json() {
  auto workers = workers_.snapshot();

  // System CPU % over the interval since the last poll.
  uint64_t sys_idle = 0;
  uint64_t sys_total = read_system_cpu(sys_idle);
  double sys_cpu_pct = 0.0;
  {
    std::lock_guard<std::mutex> lock(cpu_mutex_);
    auto it = cpu_samples_.find(0);  // pid 0 == system aggregate sample
    if (it != cpu_samples_.end()) {
      uint64_t dtot = sys_total - it->second.total_ticks;
      uint64_t didle = sys_idle - it->second.proc_ticks;
      if (dtot > 0) sys_cpu_pct = 100.0 * (1.0 - static_cast<double>(didle) / dtot);
    }
  }

  uint64_t sys_mem_total = 0, sys_mem_used = 0;
  read_system_mem(sys_mem_total, sys_mem_used);

  const double ncpu = static_cast<double>(num_cpus());
  const double ticks = static_cast<double>(clk_tck());
  (void)ticks;  // per-process %CPU uses the system jiffy delta, not HZ directly.

  // Build a fresh CPU sample map so stale pids get dropped automatically.
  std::map<pid_t, CpuSample> new_samples;
  new_samples[0] = CpuSample{sys_idle, sys_total};

  double tot_cpu = 0.0;
  uint64_t tot_mem = 0, tot_in = 0, tot_out = 0;
  int tot_conns = 0;

  std::string wjson = "[";
  bool first = true;
  {
    std::lock_guard<std::mutex> lock(cpu_mutex_);
    for (const auto &w : workers) {
      uint64_t rss = read_proc_rss(w.pid);
      uint64_t proc_ticks = read_proc_cpu_ticks(w.pid);
      double cpu_pct = 0.0;
      auto it = cpu_samples_.find(w.pid);
      if (it != cpu_samples_.end()) {
        uint64_t dproc = proc_ticks - it->second.proc_ticks;
        uint64_t dtot = sys_total - it->second.total_ticks;
        if (dtot > 0)
          cpu_pct = 100.0 * ncpu * static_cast<double>(dproc) / dtot;
      }
      new_samples[w.pid] = CpuSample{proc_ticks, sys_total};

      tot_cpu += cpu_pct;
      tot_mem += rss;
      tot_in += w.bytes_in;
      tot_out += w.bytes_out;
      tot_conns += w.active_conns;

      if (!first) wjson += ",";
      first = false;
      wjson += "{\"app\":\"" + json_escape(w.app) + "\",";
      wjson += "\"sid\":\"" + json_escape(w.sid) + "\",";
      wjson += "\"pid\":" + std::to_string(w.pid) + ",";
      wjson += "\"port\":" + std::to_string(w.port) + ",";
      wjson += "\"conns\":" + std::to_string(w.active_conns) + ",";
      wjson += "\"ws\":" + std::string(w.ws_seen ? "true" : "false") + ",";
      wjson += "\"uptime\":" + num(w.uptime_s) + ",";
      wjson += "\"idle\":" + num(w.idle_s) + ",";
      wjson += "\"cpu_pct\":" + num(cpu_pct) + ",";
      wjson += "\"mem\":" + std::to_string(rss) + ",";
      wjson += "\"bytes_in\":" + std::to_string(w.bytes_in) + ",";
      wjson += "\"bytes_out\":" + std::to_string(w.bytes_out) + "}";
    }
    cpu_samples_.swap(new_samples);
  }
  wjson += "]";

  uint64_t net_in = metrics().bytes_in.load(std::memory_order_relaxed);
  uint64_t net_out = metrics().bytes_out.load(std::memory_order_relaxed);

  std::ostringstream js;
  js << "{";
  js << "\"time\":" << ::time(nullptr) << ",";
  js << "\"ncpu\":" << num_cpus() << ",";
  js << "\"system\":{\"cpu_pct\":" << num(sys_cpu_pct)
     << ",\"mem_used\":" << sys_mem_used
     << ",\"mem_total\":" << sys_mem_total << "},";
  js << "\"net\":{\"bytes_in\":" << net_in
     << ",\"bytes_out\":" << net_out << "},";
  js << "\"totals\":{\"workers\":" << workers.size()
     << ",\"cpu_pct\":" << num(tot_cpu)
     << ",\"mem\":" << tot_mem
     << ",\"conns\":" << tot_conns
     << ",\"bytes_in\":" << tot_in
     << ",\"bytes_out\":" << tot_out << "},";
  js << "\"workers\":" << wjson;
  js << "}";
  return js.str();
}

std::string AdminServer::handle_action(const std::string &target, bool reload) {
  std::string sid = query_param(target, "sid");
  if (!is_valid_sid(sid)) {
    return http_response(400, "Bad Request", "application/json",
                         "{\"ok\":false,\"error\":\"invalid sid\"}");
  }

  bool ok = reload ? workers_.restart_session(sid) != nullptr
                   : workers_.kill_session(sid);
  std::string body =
      ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"no such session\"}";
  return http_response(ok ? 200 : 404, ok ? "OK" : "Not Found",
                       "application/json", body);
}

void AdminServer::handle_connection(int client_fd) {
  std::string head, leftover;
  if (!read_http_head(client_fd, head, leftover)) return;

  HttpRequest req;
  if (!parse_http_request(head, req)) {
    send_all(client_fd,
             http_response(400, "Bad Request", "text/plain", "Bad Request"));
    return;
  }

  std::string path = req.path();
  if (req.method == "POST" && path == "/api/stop") {
    send_all(client_fd, handle_action(req.target, false));
  } else if (req.method == "POST" && path == "/api/reload") {
    send_all(client_fd, handle_action(req.target, true));
  } else if (path == "/stats.json") {
    send_all(client_fd, http_response(200, "OK", "application/json",
                                      build_stats_json()));
  } else if (path == "/" || path.empty()) {
    send_all(client_fd, http_response(200, "OK", "text/html; charset=utf-8",
                                      kDashboardHtml));
  } else {
    send_all(client_fd,
             http_response(404, "Not Found", "text/plain", "Not Found"));
  }
}

bool AdminServer::listen_and_serve() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    TS_ERROR("admin socket() failed: %s", std::strerror(errno));
    return false;
  }
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(cfg_.admin_port);
  if (inet_pton(AF_INET, cfg_.admin_listen.c_str(), &addr.sin_addr) != 1) {
    TS_ERROR("invalid admin_listen address: %s", cfg_.admin_listen.c_str());
    return false;
  }
  if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    TS_ERROR("admin bind %s:%u failed: %s", cfg_.admin_listen.c_str(),
             cfg_.admin_port, std::strerror(errno));
    return false;
  }
  if (::listen(listen_fd_, 16) != 0) {
    TS_ERROR("admin listen() failed: %s", std::strerror(errno));
    return false;
  }
  TS_INFO("admin dashboard on http://%s:%u", cfg_.admin_listen.c_str(),
          cfg_.admin_port);

  while (running_) {
    int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      if (!running_) break;
      if (errno == EINTR) continue;
      continue;
    }
    std::thread([this, client] {
      handle_connection(client);
      ::close(client);
    }).detach();
  }
  ::close(listen_fd_);
  listen_fd_ = -1;
  return true;
}

}  // namespace ts
