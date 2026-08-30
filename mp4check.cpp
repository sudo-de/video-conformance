// mp4check.cpp — CLI front-end for mp4probe.hpp
//
// Requirement being enforced:
//   Container : MP4 (ISO BMFF)   Video: H.264 'avc1'   Audio: AAC-LC 'mp4a' (OTI 0x40, AOT 2)
//   Streaming : 'moov' before 'mdat' (faststart)
//
// Build: make          Usage: ./mp4check <file|dir> [--csv] [--script fix.sh] [--no-color]
#include "mp4probe.hpp"

#include <unistd.h>

// ------------------------------------------------------------------ report

// Why a PASS is a PASS — the target format, not just the absence of failures.
// On WebGL Unity has no decoder of its own: VideoPlayer takes a URL and the
// browser's <video> element decodes it. That is what makes H.264/AAC the safe
// target — it is the intersection of what every browser will play.
static const char *kWorksNote =
    "MP4 / H.264 / AAC-LC is the universal standard — on WebGL the browser decodes it "
    "for Unity, and Android/iOS decode it in hardware.";

// Text captions are carried through as mov_text. Bitmap or CEA-608 tracks are
// left out: they cannot become mov_text, and mapping them fails the whole encode.
static bool isTextCaption(const std::string &codec) {
    return codec == "tx3g" || codec == "text" || codec == "wvtt" || codec == "stpp";
}

static std::string ffmpegCmd(const Probe &pr) {
    std::string in = pr.path;
    fs::path p(in);
    std::string out = (p.parent_path() / ("FIXED_" + p.stem().string() + ".mp4")).string();
    std::string sopt;
    for (const Track *c : captionTracks(pr))
        if (isTextCaption(c->codec)) { sopt = "-map 0:s? -c:s mov_text "; break; }
    return "ffmpeg -y -i \"" + in + "\" -map 0:v:0 -map 0:a:0? " + sopt +
           "-c:v libx264 -profile:v high -level 4.0 -pix_fmt yuv420p -crf 21 -preset medium "
           "-c:a aac -profile:a aac_low -b:a 192k -ar 48000 -ac 2 "
           "-movflags +faststart \"" + out + "\"";
}

static void report(const Probe &pr, bool csv) {
    bool ok = pr.fail.empty();
    if (csv) {
        const Track *v = nullptr, *a = nullptr;
        for (auto &t : pr.tracks) {
            if (t.handler == "vide" && !v) v = &t;
            if (t.handler == "soun" && !a) a = &t;
        }
        std::string reasons;
        for (auto &r : pr.fail) reasons += (reasons.empty() ? "" : " | ") + r;
        auto caps = captionTracks(pr);
        std::string capcol = caps.empty() ? "none" : caps[0]->codec;
        if (caps.size() > 1) capcol += "+" + std::to_string(caps.size() - 1);
        std::printf("\"%s\",%s,%s,%s,%s,%u,%d,%s,\"%s\"\n", pr.path.c_str(), ok ? "PASS" : "FAIL",
                    v ? v->codec.c_str() : "none", a ? a->codec.c_str() : "none",
                    a ? aotName(a->aot) : "-", a ? a->rate : 0, a ? a->chan : 0,
                    capcol.c_str(), reasons.c_str());
        return;
    }

    std::printf("\n%s %s\n", ok ? col(C_GRN, "[PASS]").c_str() : col(C_RED, "[FAIL]").c_str(),
                col(C_BLD, fs::path(pr.path).filename().string()).c_str());
    std::printf("  %s  %.2f MB   brand=%s [%s]%s\n", pr.container.c_str(),
                double(pr.bytes) / 1048576.0, pr.majorBrand.c_str(), pr.compatBrands.c_str(),
                pr.fragmented ? "  fragmented" : "");

    for (const auto &t : pr.tracks) {
        if (t.handler == "vide") {
            std::printf("  %s %s  %dx%d  %s@%.1f  %.2fs  %u frames  %.0f kbps\n",
                        col(C_CYN, "video:").c_str(), t.codec.c_str(), t.width, t.height,
                        avcProfileName(t.avcProfile), t.avcLevel / 10.0, t.seconds(),
                        t.sampleCount, t.kbps());
        } else if (t.handler == "soun") {
            std::printf("  %s %s  %s / %s%s  %u Hz  %dch  %.2fs  %u frames\n",
                        col(C_CYN, "audio:").c_str(), t.codec.c_str(), otiName(t.oti),
                        aotName(t.aot), t.sbr ? "+SBR" : "", t.rate, t.chan, t.seconds(),
                        t.sampleCount);
            std::printf("  %s id=%u flags=0x%06X (%s) volume=%.2f elst=%d  %.0f kbps (%llu bytes)\n",
                        col(C_CYN, "atrak:").c_str(), t.trackId, t.tkhdFlags,
                        (t.tkhdFlags & 0x1) ? "enabled" : "DISABLED", t.volume, t.elstEntries,
                        t.kbps(), (unsigned long long)t.mediaBytes);

        }
    }
    for (const Track *c : captionTracks(pr))
        std::printf("  %s %s  %s  %s  %.2fs  %u cues%s\n", col(C_CYN, "capts:").c_str(),
                    c->codec.c_str(), capCodecName(c->codec).c_str(),
                    c->language.empty() ? "--" : c->language.c_str(), c->seconds(),
                    c->sampleCount, (c->tkhdFlags & 0x1) ? "" : "  DISABLED");
    if (pr.moovOff >= 0 && pr.mdatOff >= 0)
        std::printf("  %s moov@%lld mdat@%lld -> %s\n", col(C_CYN, "layout:").c_str(),
                    (long long)pr.moovOff, (long long)pr.mdatOff,
                    pr.moovOff < pr.mdatOff ? col(C_GRN, "faststart OK").c_str()
                                            : col(C_RED, "NOT faststart").c_str());

    for (auto &m : pr.fail) std::printf("  %s %s\n", col(C_RED, "x").c_str(), m.c_str());
    for (auto &m : pr.warn) std::printf("  %s %s\n", col(C_YEL, "!").c_str(), m.c_str());
    if (ok) std::printf("  %s %s\n", col(C_GRN, "ok:").c_str(), col(C_DIM, kWorksNote).c_str());
    else std::printf("  %s %s\n", col(C_DIM, "fix:").c_str(), col(C_DIM, ffmpegCmd(pr)).c_str());
}

// -------------------------------------------------------------------- main

int main(int argc, char **argv) {
    std::vector<fs::path> inputs;
    bool csv = false;
    std::string scriptPath;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--csv") { csv = true; g_color = false; }
        else if (a == "--no-color") g_color = false;
        else if (a == "--script" && i + 1 < argc) scriptPath = argv[++i];
        else inputs.emplace_back(a);
    }
    if (inputs.empty()) {
        std::fprintf(stderr,
                     "usage: mp4check <file|dir> [...] [--csv] [--script fix.sh] [--no-color]\n"
                     "checks: MP4 container, H.264 video, AAC-LC audio, faststart moov\n");
        return 2;
    }
    if (!isatty(1)) g_color = false;

    std::vector<fs::path> files;
    for (auto &in : inputs) {
        std::error_code ec;
        if (fs::is_directory(in, ec)) {
            for (auto &e : fs::recursive_directory_iterator(in, fs::directory_options::skip_permission_denied, ec))
                if (e.is_regular_file(ec) && isVideoExt(e.path())) files.push_back(e.path());
        } else if (fs::exists(in, ec)) {
            files.push_back(in);
        } else {
            std::fprintf(stderr, "not found: %s\n", in.c_str());
        }
    }
    std::sort(files.begin(), files.end());

    if (csv) std::printf("file,verdict,video,audio,aac_profile,sample_rate,channels,captions,reasons\n");

    int pass = 0, failn = 0;
    std::vector<std::string> fixes;
    for (auto &p : files) {
        Probe pr = probeFile(p);
        report(pr, csv);
        if (pr.fail.empty()) pass++;
        else { failn++; fixes.push_back(ffmpegCmd(pr)); }
    }

    if (!csv) {
        std::printf("\n%s %d file(s): %s, %s\n", col(C_BLD, "Summary:").c_str(), pass + failn,
                    col(C_GRN, std::to_string(pass) + " pass").c_str(),
                    col(failn ? C_RED : C_DIM, std::to_string(failn) + " fail").c_str());
    }
    if (!scriptPath.empty() && !fixes.empty()) {
        std::ofstream s(scriptPath);
        s << "#!/bin/sh\n# Re-encode every non-conforming file to H.264 + AAC-LC MP4 (faststart)\nset -e\n";
        for (auto &c : fixes) s << c << "\n";
        s.close();
        fs::permissions(scriptPath, fs::perms::owner_exec, fs::perm_options::add);
        std::fprintf(stderr, "wrote %zu fix command(s) to %s\n", fixes.size(), scriptPath.c_str());
    }
    return failn ? 1 : 0;
}
