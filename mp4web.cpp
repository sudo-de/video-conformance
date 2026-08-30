// mp4web.cpp — HTTP API server for the video conformance pipeline.
// Reuses mp4probe.hpp for container analysis, shells out to ffmpeg to measure
// what the audio actually sounds like, and again to convert.
//
//   POST /api/upload        multipart/form-data  -> {"jobs":[{id,name}]}
//   GET  /api/events        SSE stream of the full job list
//   GET  /api/download/:id  the fixed file
//   GET  /api/captions/:id  WebVTT sidecar for the fixed file, when it has one
//   GET  /api/report.csv    audit report for every job
//   DELETE /api/job/:id     forget one job and delete its files
//   DELETE /api/jobs        forget all of them
//   GET  /*                 static files from web/dist (production build)
//
// Build: make mp4web        Run: ./mp4web [port] [--dist web/dist]
#include "mp4probe.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

// ------------------------------------------------------------------ helpers

static std::string g_dist = "web/dist";
static const std::string kWork = ".mp4web";

static std::string shq(const std::string &s) {
    std::string o = "'";
    for (char c : s) {
        if (c == '\'') o += "'\\''";
        else o += c;
    }
    return o + "'";
}

static std::string jesc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += char(c);
        }
    }
    return o;
}

static std::string db1(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

static std::string jarr(const std::vector<std::string> &v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) o += (i ? ",\"" : "\"") + jesc(v[i]) + "\"";
    return o + "]";
}

static std::string sanitize(const std::string &fn) {
    std::string base = fs::path(fn).filename().string();
    std::string o;
    for (char c : base) o += (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_' || c == ' ') ? c : '_';
    if (o.empty() || o == "." || o == "..") o = "upload.bin";
    return o;
}

static std::string runCapture(const std::string &cmd) {
    std::string out;
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (fgets(buf, sizeof buf, p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static std::string ffprobeField(const std::string &sel, const std::string &entry, const std::string &f) {
    return runCapture("ffprobe -v error -select_streams " + sel + " -show_entries stream=" + entry +
                      " -of csv=p=0 " + shq(f) + " 2>/dev/null | head -1");
}
static double ffprobeDuration(const std::string &f) {
    return atof(runCapture("ffprobe -v error -show_entries format=duration -of default=nw=1:nk=1 " +
                           shq(f) + " 2>/dev/null").c_str());
}

// ------------------------------------------------ what the ear actually hears
//
// The container can only say that an audio track exists. It cannot say whether
// that track makes a sound: a file can be textbook-conformant — AAC-LC, 48 kHz,
// stereo, track enabled, tkhd volume 1.0, faststart — and still play as silence.
// The only way to know is to decode it and measure.
//
//   peak / mean (volumedetect)  catch a track that is empty or pure noise floor
//   integrated  (EBU R128)      predicts what a listener hears, because it gates
//                               silence out: a clip holding one second of speech
//                               and three of room tone scores on the speech, where
//                               a flat mean would write it off as too quiet.
struct Loud {
    bool measured = false;
    long long samples = 0;  // 0 -> the track decoded to nothing at all
    double peakDb = 0;      // dBFS
    double lufs = 0;        // EBU R128 integrated

    static constexpr double kSilentPeak = -50.0;  // under this it is dither, not content
    static constexpr double kInaudible = -40.0;   // LUFS; lost beneath any room noise
    static constexpr double kQuiet = -30.0;       // LUFS; far under any delivery target

    bool empty()     const { return measured && samples == 0; }
    bool silent()    const { return measured && samples > 0 && peakDb <= kSilentPeak; }
    bool inaudible() const { return measured && samples > 0 && !silent() && lufs < kInaudible; }
    bool quiet()     const { return measured && samples > 0 && !silent() && !inaudible() && lufs < kQuiet; }
    bool audible()   const { return measured && !empty() && !silent() && !inaudible(); }

    // The one line a human needs to settle "I still cannot hear it".
    std::string verdict() const {
        if (!measured)   return "not measured";
        if (empty())     return "decodes to nothing";
        if (silent())    return "silent";
        if (inaudible()) return "inaudible";
        if (quiet())     return "quiet";
        return "audible";
    }

    std::string json() const {
        if (!measured) return "null";
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(1);
        o << "{\"verdict\":\"" << verdict() << "\",\"samples\":" << samples
          << ",\"peakDb\":" << peakDb << ",\"lufs\":" << lufs << "}";
        return o.str();
    }
};

// Both filters print a zero-length flush pass before the real one, so take the last.
static bool lastNum(const std::string &s, const std::string &key, double &out) {
    size_t p = s.rfind(key);
    if (p == std::string::npos) return false;
    out = atof(s.c_str() + p + key.size());
    return true;
}

static Loud measureLoudness(const std::string &f) {
    Loud l;
    // One decode feeds both filters; the grep keeps R128's per-frame spam out of
    // the pipe. Anchoring "I:" to the line start picks the summary, not a frame.
    std::string out = runCapture(
        "ffmpeg -nostdin -hide_banner -i " + shq(f) +
        " -map 0:a:0 -af ebur128=peak=true,volumedetect -f null - 2>&1"
        " | grep -E 'n_samples:|max_volume:|^ +I: '");
    double v = 0;
    if (!lastNum(out, "n_samples:", v)) return l;  // the filters never ran
    l.measured = true;
    l.samples = (long long)v;
    auto db = [](double x) { return std::isfinite(x) ? std::max(-120.0, std::min(0.0, x)) : -120.0; };
    if (lastNum(out, "max_volume:", v))  l.peakDb = db(v);
    if (lastNum(out, "I:", v))           l.lufs   = db(v);
    return l;
}

// --------------------------------------------------------------- job model

struct Snap {
    std::string container, video, audio, aacProfile, verdict;
    std::string capCodec, capLang;
    int rate = 0, ch = 0, width = 0, height = 0, capCount = 0;
    double seconds = 0, vkbps = 0, akbps = 0;
    bool faststart = false, hasAudio = false;
    Loud loud;
    std::vector<std::string> fail, warn;

    static Snap from(const Probe &pr) {
        Snap s;
        s.container = pr.container;
        s.fail = pr.fail;
        s.warn = pr.warn;
        s.verdict = pr.fail.empty() ? "PASS" : "FAIL";
        s.faststart = pr.moovOff >= 0 && pr.mdatOff >= 0 && pr.moovOff < pr.mdatOff;
        for (const auto &t : pr.tracks) {
            if (t.handler == "vide" && s.video.empty()) {
                s.video = t.codec + (t.avcProfile > 0 ? " " + std::string(avcProfileName(t.avcProfile)) : "");
                s.width = t.width; s.height = t.height;
                s.seconds = t.seconds(); s.vkbps = t.kbps();
            } else if (t.handler == "soun" && s.audio.empty()) {
                s.hasAudio = true;
                s.audio = t.codec;
                s.aacProfile = aotName(t.aot);
                s.rate = int(t.rate); s.ch = t.chan; s.akbps = t.kbps();
            }
        }
        for (const Track *c : captionTracks(pr)) {
            s.capCount++;
            if (s.capCodec.empty()) { s.capCodec = c->codec; s.capLang = c->language; }
        }
        return s;
    }

    // Fold the measurement into the verdict: a file nobody can hear is not a pass.
    void applyLoudness(const Loud &l) {
        loud = l;
        if (!l.measured) return;
        if (l.empty())
            fail.push_back("audio track decodes to no samples at all");
        else if (l.silent())
            fail.push_back("audio is silent (peak " + db1(l.peakDb) + " dBFS)");
        else if (l.inaudible())
            fail.push_back("audio is inaudible (" + db1(l.lufs) + " LUFS integrated)");
        else if (l.quiet())
            warn.push_back("audio is quiet (" + db1(l.lufs) + " LUFS integrated; -23 to -16 is typical)");
        verdict = fail.empty() ? "PASS" : "FAIL";
    }

    std::string json() const {
        std::ostringstream o;
        o << "{\"container\":\"" << jesc(container) << "\",\"video\":\"" << jesc(video)
          << "\",\"audio\":\"" << jesc(audio) << "\",\"aacProfile\":\"" << jesc(aacProfile)
          << "\",\"verdict\":\"" << verdict << "\",\"rate\":" << rate << ",\"ch\":" << ch
          << ",\"width\":" << width << ",\"height\":" << height
          << ",\"seconds\":" << seconds << ",\"vkbps\":" << int(vkbps) << ",\"akbps\":" << int(akbps)
          << ",\"capCount\":" << capCount << ",\"capCodec\":\"" << jesc(capCodec)
          << "\",\"capLang\":\"" << jesc(capLang) << "\""
          << ",\"faststart\":" << (faststart ? "true" : "false")
          << ",\"hasAudio\":" << (hasAudio ? "true" : "false")
          << ",\"loudness\":" << loud.json()
          << ",\"fail\":" << jarr(fail) << ",\"warn\":" << jarr(warn) << "}";
        return o.str();
    }
};

struct Job {
    int id = 0;
    std::string name, inPath, outPath, outName, vttPath;
    std::string status = "queued";  // queued|analyzing|converting|done|failed
    std::string message, plan;
    double pct = 0;
    uint64_t inBytes = 0, outBytes = 0;
    Snap before, after;
    bool hasBefore = false, hasAfter = false, downloadable = false;

    std::string json() const {
        std::ostringstream o;
        o << "{\"id\":" << id << ",\"name\":\"" << jesc(name) << "\",\"status\":\"" << status
          << "\",\"pct\":" << int(pct) << ",\"message\":\"" << jesc(message)
          << "\",\"plan\":\"" << jesc(plan) << "\",\"inBytes\":" << inBytes
          << ",\"outBytes\":" << outBytes << ",\"downloadable\":" << (downloadable ? "true" : "false")
          << ",\"captions\":" << (vttPath.empty() ? "false" : "true")
          << ",\"before\":" << (hasBefore ? before.json() : "null")
          << ",\"after\":" << (hasAfter ? after.json() : "null") << "}";
        return o.str();
    }
};

static std::mutex g_mu;
static std::map<int, Job> g_jobs;
static std::atomic<int> g_nextId{1};
static std::atomic<uint64_t> g_rev{0};

static void touch() { g_rev++; }
static void update(int id, const std::function<void(Job &)> &fn) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_jobs.find(id);
    if (it != g_jobs.end()) fn(it->second);
    touch();
}
static bool jobAlive(int id) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_jobs.count(id) != 0;
}

// Forgetting the job is the cancel: a running ffmpeg keeps its open input fd, so
// it finishes into a file the worker then throws away (see jobAlive below).
static bool removeJob(int id) {
    std::string in, out, vtt;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_jobs.find(id);
        if (it == g_jobs.end()) return false;
        in = it->second.inPath;
        out = it->second.outPath;
        vtt = it->second.vttPath;
        g_jobs.erase(it);
    }
    std::error_code ec;
    if (!in.empty()) fs::remove(in, ec);
    if (!out.empty()) fs::remove(out, ec);
    if (!vtt.empty()) fs::remove(vtt, ec);
    touch();
    return true;
}

static int removeAllJobs() {
    std::vector<int> ids;
    { std::lock_guard<std::mutex> lk(g_mu); for (auto &kv : g_jobs) ids.push_back(kv.first); }
    int n = 0;
    for (int id : ids) n += removeJob(id) ? 1 : 0;
    return n;
}

static std::string jobsJson() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string o = "[";
    bool first = true;
    for (auto &kv : g_jobs) { if (!first) o += ","; first = false; o += kv.second.json(); }
    return o + "]";
}

// ------------------------------------------------------- conversion worker

static std::mutex g_slotMu;
static std::condition_variable g_slotCv;
static int g_slots = std::max(1u, std::thread::hardware_concurrency() / 4);

struct Slot {
    Slot()  { std::unique_lock<std::mutex> lk(g_slotMu); g_slotCv.wait(lk, [] { return g_slots > 0; }); g_slots--; }
    ~Slot() { { std::lock_guard<std::mutex> lk(g_slotMu); g_slots++; } g_slotCv.notify_one(); }
};

// Text subtitles become mov_text. Bitmap tracks (PGS, DVD, DVB) have no MP4
// text form, and mapping them makes ffmpeg abort the whole encode.
static bool isTextSub(const std::string &codec) {
    return codec == "mov_text" || codec == "subrip" || codec == "srt" || codec == "text" ||
           codec == "ass" || codec == "ssa" || codec == "webvtt" || codec == "ttml" ||
           codec == "stl" || codec == "subviewer" || codec == "eia_608";
}

// ffmpeg writes the WEBVTT header even for a track with no cues, so a bare
// header means there is nothing to display and a <track> would only add an
// empty entry to the player's caption menu.
static bool vttHasCues(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line))
        if (line.find("-->") != std::string::npos) return true;
    return false;
}

// A carried-through mov_text track still shows nothing: no browser renders
// in-band timed text inside <video>. Extracting it once as a WebVTT sidecar is
// what makes the captions actually appear. Taken from the fixed file, so what
// the player shows is what the download contains. Returns "" when there is
// nothing to show.
static std::string extractVtt(const std::string &mp4) {
    std::string out = fs::path(mp4).replace_extension(".vtt").string();
    std::string cmd = "ffmpeg -nostdin -y -v error -i " + shq(mp4) +
                      " -map 0:s:0 -c:s webvtt " + shq(out) + " >/dev/null 2>&1";
    if (system(cmd.c_str()) == 0 && vttHasCues(out)) return out;
    std::error_code ec;
    fs::remove(out, ec);
    return "";
}

// Runs one encode, feeding -progress back into the job. -1 means ffmpeg never started.
static int runEncode(int id, const std::string &cmd, double dur) {
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return -1;
    char line[512];
    while (fgets(line, sizeof line, p)) {
        std::string l(line);
        auto eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        double us = -1;
        if (k == "out_time_us" || k == "out_time_ms") us = atof(v.c_str());
        if (k == "out_time_ms") us *= 1000.0;  // ffmpeg reports ms key in microseconds historically
        if (us >= 0 && dur > 0) {
            double pctv = std::min(99.0, (us / 1e6) / dur * 100.0);
            update(id, [&](Job &j) { if (pctv > j.pct) j.pct = pctv; });
        }
    }
    return pclose(p);
}

static void worker(int id) {
    std::string in, outPath, outName;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        in = g_jobs[id].inPath; outPath = g_jobs[id].outPath; outName = g_jobs[id].outName;
    }
    update(id, [](Job &j) { j.status = "analyzing"; j.pct = 0; });

    Probe pr = probeFile(in);
    Snap before = Snap::from(pr);
    // ffprobe fills in what the container parser cannot see (non-MP4 inputs)
    std::string vc = ffprobeField("v:0", "codec_name", in);
    std::string px = ffprobeField("v:0", "pix_fmt", in);
    std::string ac = ffprobeField("a:0", "codec_name", in);
    std::string chs = ffprobeField("a:0", "channels", in);
    std::string sc = ffprobeField("s:0", "codec_name", in);
    // non-MP4 inputs have no ISO caption track to parse, but ffprobe still sees one
    if (before.capCount == 0 && !sc.empty()) { before.capCount = 1; before.capCodec = sc; }
    if (before.video.empty() && !vc.empty()) before.video = vc;
    if (before.audio.empty() && !ac.empty()) { before.audio = ac; before.hasAudio = true; before.ch = atoi(chs.c_str()); }
    double dur = ffprobeDuration(in);
    if (before.seconds <= 0) before.seconds = dur;

    if (ac.empty()) {
        update(id, [&](Job &j) {
            j.before = before; j.hasBefore = true;
            j.status = "failed";
            j.message = "No audio track at all — encoding cannot create sound that was never recorded. Re-export from the source project.";
        });
        return;
    }

    Slot slot;  // bound concurrent ffmpeg processes

    // Decode and measure before promising anything. Every setting this tool fixes
    // is a container property; none of them add level. Re-encoding silence at
    // 192 kbps stereo yields conformant silence, and the caller downloads a green
    // PASS that still plays as nothing.
    before.applyLoudness(measureLoudness(in));
    update(id, [&](Job &j) { j.before = before; j.hasBefore = true; });

    if (before.loud.measured && !before.loud.audible()) {  // unmeasurable -> convert anyway
        update(id, [&](Job &j) {
            j.status = "failed";
            j.message = before.loud.empty()
                ? "The audio track decodes to no samples — the file carries an empty track. Re-export from the source project."
                : "The audio measures " + db1(before.loud.lufs) + " LUFS, peak " + db1(before.loud.peakDb) +
                      " dBFS — there is nothing on it to hear. Encoding cannot add level that was never recorded; re-export from the source project.";
        });
        return;
    }

    // video: copy when already conformant H.264 8-bit (no generation loss), else transcode
    std::string vopt, plan;
    if (vc == "h264" && px == "yuv420p") { vopt = "-c:v copy"; plan = "video: copy"; }
    else { vopt = "-c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p -crf 21 -preset medium";
           plan = "video: " + vc + "/" + px + " -> h264"; }

    // mono -> stereo at unity gain; ffmpeg's default upmix would silently drop 3 dB
    int ch = atoi(chs.c_str());
    std::string aopt;
    if (ch == 1)      { aopt = "-af " + shq("pan=stereo|c0=c0|c1=c0"); plan += ", audio: mono->stereo"; }
    else if (ch > 2)  { aopt = "-af " + shq("pan=stereo|c0=0.5*FL+0.3*FC|c1=0.5*FR+0.3*FC"); plan += ", audio: " + chs + "ch->stereo"; }
    else              { plan += ", audio: stereo"; }

    // closed captions: keep the track instead of letting -map drop it silently
    std::string sopt, capNote;
    if (isTextSub(sc))    { sopt = "-map " + shq("0:s?") + " -c:s mov_text "; plan += ", captions: kept"; }
    else if (!sc.empty()) { plan += ", captions: " + sc + " (no MP4 text form — dropped)"; }

    update(id, [&](Job &j) { j.status = "converting"; j.plan = plan; });

    auto build = [&](const std::string &subs) {
        return "ffmpeg -nostdin -y -v error -progress pipe:1 -i " + shq(in) +
               " -map 0:v:0 -map 0:a:0 " + subs + vopt + " " + aopt +
               " -c:a aac -profile:a aac_low -b:a 192k -ar 48000 -movflags +faststart " +
               shq(outPath) + " 2>/dev/null";
    };

    int rc = runEncode(id, build(sopt), dur);
    if ((rc != 0 || !fs::exists(outPath)) && !sopt.empty()) {
        // the caption track broke the encode — the video is worth more than the captions
        capNote = "Captions could not be converted to mov_text and were dropped; the video and audio are fixed.";
        update(id, [](Job &j) { j.pct = 0; });
        rc = runEncode(id, build(""), dur);
    }
    if (rc == -1) {
        update(id, [](Job &j) { j.status = "failed"; j.message = "could not start ffmpeg"; });
        return;
    }
    if (rc != 0 || !fs::exists(outPath)) {
        update(id, [](Job &j) { j.status = "failed"; j.message = "ffmpeg failed to produce an output file"; });
        return;
    }

    if (!jobAlive(id)) {  // removed from the UI while it was converting
        std::error_code ec;
        fs::remove(outPath, ec);
        return;
    }

    Probe after = probeFile(outPath);
    Snap sa = Snap::from(after);
    sa.applyLoudness(measureLoudness(outPath));  // prove the fix kept the sound
    std::string vtt = sa.capCount > 0 ? extractVtt(outPath) : "";
    std::error_code ec;
    uint64_t osz = fs::file_size(outPath, ec);

    update(id, [&](Job &j) {
        j.after = sa; j.hasAfter = true; j.outBytes = osz; j.vttPath = vtt;
        j.pct = 100; j.status = "done"; j.downloadable = true;
        if (!sa.fail.empty()) j.message = sa.fail[0];
        else if (!capNote.empty()) j.message = capNote;
    });
    if (!vtt.empty() && !jobAlive(id)) fs::remove(vtt, ec);  // removed while extracting
}

// ------------------------------------------------------------ http plumbing

static bool sendAll(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w; n -= size_t(w);
    }
    return true;
}
static bool sendStr(int fd, const std::string &s) { return sendAll(fd, s.data(), s.size()); }

static void respond(int fd, const std::string &status, const std::string &ctype,
                    const std::string &body, const std::string &extra = "") {
    std::ostringstream h;
    h << "HTTP/1.1 " << status << "\r\nContent-Type: " << ctype << "\r\nContent-Length: " << body.size()
      << "\r\nAccess-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n" << extra << "Connection: close\r\n\r\n";
    sendStr(fd, h.str());
    sendAll(fd, body.data(), body.size());
}

static std::string mimeOf(const std::string &p) {
    auto ext = fs::path(p).extension().string();
    if (ext == ".html") return "text/html; charset=utf-8";
    if (ext == ".js" || ext == ".mjs") return "text/javascript";
    if (ext == ".css") return "text/css";
    if (ext == ".json") return "application/json";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".woff2") return "font/woff2";
    if (ext == ".mp4") return "video/mp4";
    if (ext == ".vtt") return "text/vtt; charset=utf-8";
    return "application/octet-stream";
}

// Media elements do not stream from a plain 200: Safari and every iOS browser
// probe with "Range: bytes=0-1" and will not start playback unless the server
// answers 206 with Content-Range. Chrome plays but cannot seek.

struct ByteRange {
    enum Kind { Whole, Partial, Unsatisfiable } kind = Whole;
    uint64_t start = 0, end = 0;
    uint64_t len(uint64_t size) const { return kind == Partial ? end - start + 1 : size; }
};

// RFC 7233 single range: "bytes=a-b", "bytes=a-" (open-ended), "bytes=-n" (suffix).
// Multi-range is not offered; no player needs it, and Accept-Ranges does not promise it.
static ByteRange parseRange(const std::string &hdr, uint64_t size) {
    ByteRange r;
    if (hdr.rfind("bytes=", 0) != 0 || size == 0) return r;

    std::string spec = hdr.substr(6);
    auto dash = spec.find('-');
    if (dash == std::string::npos) { r.kind = ByteRange::Unsatisfiable; return r; }
    std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);

    r.end = size - 1;
    if (a.empty()) {
        if (b.empty()) { r.kind = ByteRange::Unsatisfiable; return r; }
        uint64_t n = strtoull(b.c_str(), nullptr, 10);        // suffix: last n bytes
        r.start = n >= size ? 0 : size - n;
    } else {
        r.start = strtoull(a.c_str(), nullptr, 10);
        if (!b.empty()) r.end = strtoull(b.c_str(), nullptr, 10);
    }
    if (r.end >= size) r.end = size - 1;
    r.kind = (r.start < size && r.start <= r.end) ? ByteRange::Partial : ByteRange::Unsatisfiable;
    return r;
}

// Closes on every exit path, including the 416 return.
struct Fd {
    int f;
    explicit Fd(int v) : f(v) {}
    ~Fd() { if (f >= 0) close(f); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};

static void sendFile(int fd, const std::string &path, const std::string &downloadName = "",
                     const std::string &rangeHdr = "") {
    Fd in(open(path.c_str(), O_RDONLY));
    if (in.f < 0) { respond(fd, "404 Not Found", "text/plain", "not found"); return; }
    struct stat st{};
    fstat(in.f, &st);
    const uint64_t size = uint64_t(st.st_size);
    const ByteRange r = parseRange(rangeHdr, size);

    if (r.kind == ByteRange::Unsatisfiable) {
        respond(fd, "416 Range Not Satisfiable", "text/plain", "",
                "Content-Range: bytes */" + std::to_string(size) + "\r\nAccept-Ranges: bytes\r\n");
        return;
    }

    const bool partial = r.kind == ByteRange::Partial;
    std::ostringstream h;
    h << "HTTP/1.1 " << (partial ? "206 Partial Content" : "200 OK")
      << "\r\nContent-Type: " << mimeOf(path) << "\r\nContent-Length: " << r.len(size)
      << "\r\nAccept-Ranges: bytes\r\nAccess-Control-Allow-Origin: *\r\n";
    if (partial) h << "Content-Range: bytes " << r.start << "-" << r.end << "/" << size << "\r\n";
    if (!downloadName.empty())
        h << "Content-Disposition: attachment; filename=\"" << jesc(downloadName) << "\"\r\n";
    h << "Connection: close\r\n\r\n";
    sendStr(fd, h.str());

    if (partial && lseek(in.f, off_t(r.start), SEEK_SET) < 0) return;
    char buf[65536];
    uint64_t left = r.len(size);
    while (left) {
        ssize_t n = read(in.f, buf, std::min<uint64_t>(left, sizeof buf));
        if (n <= 0) break;
        if (!sendAll(fd, buf, size_t(n))) break;
        left -= uint64_t(n);
    }
}

// find needle in haystack range
static const char *memfind(const char *h, size_t hn, const char *n, size_t nn) {
    if (nn > hn) return nullptr;
    for (size_t i = 0; i + nn <= hn; i++)
        if (h[i] == n[0] && !memcmp(h + i, n, nn)) return h + i;
    return nullptr;
}

// ---------------------------------------------------------------- routing

static void handleUpload(int fd, const std::string &ctype, uint64_t clen,
                         const std::string &pre, int sock) {
    auto bpos = ctype.find("boundary=");
    if (bpos == std::string::npos) { respond(fd, "400 Bad Request", "text/plain", "no boundary"); return; }
    std::string boundary = "--" + ctype.substr(bpos + 9);
    if (!boundary.empty() && boundary.back() == '\r') boundary.pop_back();
    if (auto sc = boundary.find(';'); sc != std::string::npos) boundary = boundary.substr(0, sc);

    // stream the body to a temp file so multi-GB uploads never sit in RAM
    fs::create_directories(kWork + "/tmp");
    std::string tmp = kWork + "/tmp/body-" + std::to_string(g_nextId.load()) + ".bin";
    int tf = open(tmp.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (tf < 0) { respond(fd, "500", "text/plain", "tmp"); return; }
    uint64_t got = 0;
    if (!pre.empty()) { ssize_t w = write(tf, pre.data(), pre.size()); (void)w; got = pre.size(); }
    char buf[65536];
    while (got < clen) {
        ssize_t n = recv(sock, buf, std::min<uint64_t>(sizeof buf, clen - got), 0);
        if (n <= 0) break;
        ssize_t w = write(tf, buf, size_t(n)); (void)w;
        got += uint64_t(n);
    }
    off_t total = lseek(tf, 0, SEEK_END);
    if (total <= 0) { close(tf); fs::remove(tmp); respond(fd, "400", "text/plain", "empty"); return; }
    void *map = mmap(nullptr, size_t(total), PROT_READ, MAP_PRIVATE, tf, 0);
    close(tf);
    if (map == MAP_FAILED) { fs::remove(tmp); respond(fd, "500", "text/plain", "mmap"); return; }

    const char *base = static_cast<const char *>(map);
    size_t len = size_t(total);
    std::string resp = "{\"jobs\":[";
    bool first = true;
    std::vector<int> started;

    const char *p = memfind(base, len, boundary.data(), boundary.size());
    while (p) {
        const char *partStart = p + boundary.size();
        if (size_t(partStart - base) + 2 <= len && partStart[0] == '-' && partStart[1] == '-') break;  // closing
        const char *hdrEnd = memfind(partStart, len - size_t(partStart - base), "\r\n\r\n", 4);
        if (!hdrEnd) break;
        std::string headers(partStart, size_t(hdrEnd - partStart));
        const char *body = hdrEnd + 4;
        const char *next = memfind(body, len - size_t(body - base), boundary.data(), boundary.size());
        if (!next) break;
        size_t blen = size_t(next - body);
        if (blen >= 2) blen -= 2;  // trailing CRLF

        auto fnp = headers.find("filename=\"");
        if (fnp != std::string::npos) {
            auto e = headers.find('"', fnp + 10);
            std::string fname = headers.substr(fnp + 10, e - fnp - 10);
            if (!fname.empty() && blen > 0) {
                int id = g_nextId++;
                std::string clean = sanitize(fname);
                fs::create_directories(kWork + "/in");
                fs::create_directories(kWork + "/out");
                std::string inPath = kWork + "/in/" + std::to_string(id) + "_" + clean;
                std::string outName = fs::path(clean).stem().string() + ".mp4";
                std::string outPath = kWork + "/out/" + std::to_string(id) + "_" + outName;
                int of = open(inPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
                if (of >= 0) {
                    size_t off = 0;
                    while (off < blen) {
                        ssize_t w = write(of, body + off, std::min<size_t>(1 << 20, blen - off));
                        if (w <= 0) break;
                        off += size_t(w);
                    }
                    close(of);
                    Job j;
                    j.id = id; j.name = clean; j.inPath = inPath;
                    j.outPath = outPath; j.outName = outName; j.inBytes = blen;
                    { std::lock_guard<std::mutex> lk(g_mu); g_jobs[id] = j; }
                    touch();
                    started.push_back(id);
                    resp += (first ? "" : ",");
                    resp += "{\"id\":" + std::to_string(id) + ",\"name\":\"" + jesc(clean) + "\"}";
                    first = false;
                }
            }
        }
        p = next;
    }
    munmap(map, len);
    fs::remove(tmp);
    resp += "]}";
    respond(fd, "200 OK", "application/json", resp);
    for (int id : started) std::thread(worker, id).detach();
}

static void handleEvents(int fd) {
    sendStr(fd, "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n"
                "Connection: keep-alive\r\nX-Accel-Buffering: no\r\n\r\n");
    uint64_t seen = ~0ull;
    int ticks = 0;
    while (true) {
        uint64_t r = g_rev.load();
        if (r != seen) {
            seen = r;
            if (!sendStr(fd, "data: " + jobsJson() + "\n\n")) return;
        } else if (++ticks % 40 == 0) {
            if (!sendStr(fd, ": ping\n\n")) return;  // keep the connection warm
        }
        usleep(250000);
    }
}

static std::string csvReport() {
    std::string o = "file,verdict_before,video_before,audio_before,ch_before,captions_before,faststart_before,"
                    "heard_before,lufs_before,peak_before,"
                    "verdict_after,video_after,audio_after,ch_after,captions_after,faststart_after,"
                    "heard_after,lufs_after,peak_after,issues\n";
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &kv : g_jobs) {
        const Job &j = kv.second;
        std::string issues;
        for (auto &f : j.before.fail) issues += (issues.empty() ? "" : " | ") + f;
        auto caps = [](const Snap &s) {
            if (s.capCount == 0) return std::string("none");
            std::string c = s.capCodec + (s.capLang.empty() ? "" : "/" + s.capLang);
            return s.capCount > 1 ? c + "+" + std::to_string(s.capCount - 1) : c;
        };
        auto loud = [](const Snap &s) {
            return s.loud.verdict() + "," + (s.loud.measured ? db1(s.loud.lufs) : "") + "," +
                   (s.loud.measured ? db1(s.loud.peakDb) : "");
        };
        o += "\"" + j.name + "\"," + (j.hasBefore ? j.before.verdict : "?") + "," + j.before.video + "," +
             j.before.audio + "," + std::to_string(j.before.ch) + "," + caps(j.before) + "," +
             (j.before.faststart ? "yes" : "no") + "," + loud(j.before) + "," +
             (j.hasAfter ? j.after.verdict : "-") + "," + j.after.video + "," + j.after.audio + "," +
             std::to_string(j.after.ch) + "," + caps(j.after) + "," +
             (j.after.faststart ? "yes" : "no") + "," +
             loud(j.after) + ",\"" + issues + "\"\n";
    }
    return o;
}

static void handleClient(int fd) {
    std::string buf;
    char tmp[8192];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { close(fd); return; }
        buf.append(tmp, size_t(n));
        if (buf.size() > 1 << 20) break;
    }
    auto he = buf.find("\r\n\r\n");
    if (he == std::string::npos) { close(fd); return; }
    std::string head = buf.substr(0, he), pre = buf.substr(he + 4);

    std::istringstream hs(head);
    std::string method, path, ver;
    hs >> method >> path >> ver;

    std::string ctype, range;
    uint64_t clen = 0;
    std::string line;
    while (std::getline(hs, line)) {
        auto c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = line.substr(0, c), v = line.substr(c + 1);
        while (!v.empty() && (v[0] == ' ' || v[0] == '\t')) v.erase(0, 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
        std::string lk;
        for (char ch : k) lk += char(std::tolower((unsigned char)ch));
        if (lk == "content-type") ctype = v;
        else if (lk == "content-length") clen = strtoull(v.c_str(), nullptr, 10);
        else if (lk == "range") range = v;
    }

    if (method == "OPTIONS") {
        respond(fd, "204 No Content", "text/plain", "",
                "Access-Control-Allow-Methods: GET,POST,DELETE,OPTIONS\r\nAccess-Control-Allow-Headers: *\r\n");
    } else if (method == "POST" && path == "/api/upload") {
        handleUpload(fd, ctype, clen, pre, fd);
    } else if (path == "/api/events") {
        handleEvents(fd);
    } else if (path.rfind("/api/download/", 0) == 0 || path.rfind("/api/preview/", 0) == 0) {
        // same bytes, two dispositions: /download attaches, /preview streams in a <video>
        bool inlinePlay = path.rfind("/api/preview/", 0) == 0;
        int id = atoi(path.c_str() + (inlinePlay ? 13 : 14));
        std::string op, on;
        { std::lock_guard<std::mutex> lk(g_mu);
          auto it = g_jobs.find(id);
          if (it != g_jobs.end() && it->second.downloadable) { op = it->second.outPath; on = it->second.outName; } }
        if (op.empty()) respond(fd, "404 Not Found", "text/plain", "not ready");
        else sendFile(fd, op, inlinePlay ? "" : on, range);
    } else if (path.rfind("/api/captions/", 0) == 0) {
        int id = atoi(path.c_str() + 14);
        std::string vp;
        { std::lock_guard<std::mutex> lk(g_mu);
          auto it = g_jobs.find(id);
          if (it != g_jobs.end()) vp = it->second.vttPath; }
        if (vp.empty()) respond(fd, "404 Not Found", "text/plain", "no captions");
        else sendFile(fd, vp);
    } else if (method == "DELETE" && path.rfind("/api/job/", 0) == 0) {
        int id = atoi(path.c_str() + 9);
        bool gone = removeJob(id);
        respond(fd, gone ? "200 OK" : "404 Not Found", "application/json",
                std::string("{\"removed\":") + (gone ? "1" : "0") + "}");
    } else if (method == "DELETE" && path == "/api/jobs") {
        respond(fd, "200 OK", "application/json",
                "{\"removed\":" + std::to_string(removeAllJobs()) + "}");
    } else if (path == "/api/report.csv") {
        respond(fd, "200 OK", "text/csv", csvReport(),
                "Content-Disposition: attachment; filename=\"report.csv\"\r\n");
    } else if (path == "/api/jobs") {
        respond(fd, "200 OK", "application/json", jobsJson());
    } else if (method == "GET") {
        std::string rel = path == "/" ? "/index.html" : path;
        if (auto q = rel.find('?'); q != std::string::npos) rel = rel.substr(0, q);
        if (rel.find("..") != std::string::npos) { respond(fd, "400", "text/plain", "bad path"); close(fd); return; }
        std::string full = g_dist + rel;
        if (fs::exists(full) && fs::is_regular_file(full)) sendFile(fd, full, "", range);
        else if (fs::exists(g_dist + "/index.html")) sendFile(fd, g_dist + "/index.html");  // SPA fallback
        else respond(fd, "404 Not Found", "text/plain",
                     "Frontend not built. Run: cd web && npm install && npm run build");
    } else {
        respond(fd, "405 Method Not Allowed", "text/plain", "no");
    }
    close(fd);
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    int port = 8787;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--dist" && i + 1 < argc) g_dist = argv[++i];
        else if (a[0] != '-') port = atoi(argv[i]);
    }
    if (system("command -v ffmpeg >/dev/null 2>&1") != 0)
        std::fprintf(stderr, "warning: ffmpeg not found on PATH — conversions will fail\n");

    fs::create_directories(kWork + "/in");
    fs::create_directories(kWork + "/out");

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(port));
    if (bind(s, (sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    if (listen(s, 64) < 0) { perror("listen"); return 1; }

    std::printf("mp4web listening on http://localhost:%d  (serving %s, %d concurrent encodes)\n",
                port, g_dist.c_str(), g_slots);
    while (true) {
        int c = accept(s, nullptr, nullptr);
        if (c < 0) continue;
        int nod = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &nod, sizeof nod);
        std::thread(handleClient, c).detach();
    }
}
