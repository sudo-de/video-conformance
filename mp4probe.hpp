// mp4probe.hpp — dependency-free ISO-BMFF (MP4) parser + Unity conformance rules.
// Shared by the mp4check CLI and the mp4web upload server.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- utilities

inline bool g_color = true;
inline const char *C_RED   = "\033[31m", *C_GRN = "\033[32m", *C_YEL = "\033[33m",
                  *C_CYN   = "\033[36m", *C_DIM = "\033[2m",  *C_BLD = "\033[1m",
                  *C_RST   = "\033[0m";
inline std::string col(const char *c, const std::string &s) {
    return g_color ? std::string(c) + s + C_RST : s;
}

inline std::string fourcc(uint32_t t) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; i++) {
        char c = char((t >> (8 * (3 - i))) & 0xFF);
        s[i] = (c >= 32 && c < 127) ? c : '.';
    }
    return s;
}
inline constexpr uint32_t FCC(const char (&s)[5]) {
    return (uint32_t(uint8_t(s[0])) << 24) | (uint32_t(uint8_t(s[1])) << 16) |
           (uint32_t(uint8_t(s[2])) << 8) | uint32_t(uint8_t(s[3]));
}

inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t be64(const uint8_t *p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

// ------------------------------------------------------- AAC bitstream bits

struct Bits {
    const std::vector<uint8_t> &d;
    size_t pos = 0;  // in bits
    uint32_t get(int n) {
        uint32_t v = 0;
        while (n-- > 0) {
            size_t byte = pos >> 3;
            int bit = 7 - int(pos & 7);
            uint32_t b = byte < d.size() ? ((d[byte] >> bit) & 1u) : 0u;
            v = (v << 1) | b;
            ++pos;
        }
        return v;
    }
};

inline const uint32_t kAacRates[16] = {96000, 88200, 64000, 48000, 44100, 32000,
                                       24000, 22050, 16000, 12000, 11025, 8000,
                                       7350,  0,     0,     0};

inline const char *aotName(int aot) {
    switch (aot) {
        case 1:  return "AAC Main";
        case 2:  return "AAC-LC";
        case 3:  return "AAC-SSR";
        case 4:  return "AAC-LTP";
        case 5:  return "HE-AAC (SBR)";
        case 23: return "AAC-LD";
        case 29: return "HE-AACv2 (PS)";
        case 42: return "xHE-AAC/USAC";
        default: return "unknown";
    }
}
inline const char *otiName(int oti) {
    switch (oti) {
        case 0x40: return "MPEG-4 Audio (AAC)";
        case 0x66: return "MPEG-2 AAC Main";
        case 0x67: return "MPEG-2 AAC-LC";
        case 0x68: return "MPEG-2 AAC-SSR";
        case 0x69: return "MPEG-2 Audio (MP3)";
        case 0x6B: return "MPEG-1 Audio (MP3)";
        default:   return "non-AAC";
    }
}
inline const char *avcProfileName(int p) {
    switch (p) {
        case 66:  return "Baseline";
        case 77:  return "Main";
        case 88:  return "Extended";
        case 100: return "High";
        case 110: return "High 10";
        case 122: return "High 4:2:2";
        case 244: return "High 4:4:4";
        default:  return "?";
    }
}

// ---------------------------------------------------------------- captions

// mdhd packs ISO-639-2/T as three 5-bit letters offset from 0x60. The high bit
// marks a legacy Macintosh language code, which is not a letter triple at all.
inline std::string isoLang(uint16_t v) {
    if (v & 0x8000) return "";
    std::string s;
    for (int i = 2; i >= 0; i--) {
        char c = char(((v >> (5 * i)) & 0x1F) + 0x60);
        if (c < 'a' || c > 'z') return "";
        s += c;
    }
    return s;
}

// 'text' is timed text in MP4 and QuickTime, 'sbtl'/'subt' subtitles, 'clcp'
// closed captions. A 'text' track can also be a chapter list — see chapterTrack.
inline bool isCaptionHandler(const std::string &h) {
    return h == "text" || h == "sbtl" || h == "subt" || h == "clcp";
}

inline std::string capCodecName(const std::string &c) {
    if (c == "tx3g") return "3GPP timed text";
    if (c == "text") return "QuickTime text";
    if (c == "wvtt") return "WebVTT";
    if (c == "stpp") return "TTML";
    if (c == "c608") return "CEA-608";
    if (c == "c708") return "CEA-708";
    return c;
}

// ------------------------------------------------------------------- model

struct Track {
    std::string handler;    // "vide" / "soun" / "text" / "sbtl" / "subt" / "clcp"
    std::string codec;      // fourcc of the sample entry
    uint32_t timescale = 0;
    uint64_t duration = 0;
    uint32_t sampleCount = 0;
    uint64_t mediaBytes = 0;
    int width = 0, height = 0;
    int avcProfile = -1, avcLevel = -1;
    int chan = 0;
    uint32_t rate = 0;
    int oti = -1, aot = -1;
    bool sbr = false;
    uint32_t trackId = 0;
    std::string language;   // ISO-639-2/T from mdhd, empty when not a letter code
    uint32_t tkhdFlags = 0xFFFFFFFF;  // bit0 enabled, bit1 in-movie, bit2 in-preview
    double volume = -1.0;             // 8.8 fixed, only meaningful for audio
    int elstEntries = 0;
    int64_t elstEmptyDur = 0;         // duration of a leading empty edit (movie timescale)
    bool sawTkhd = false;
    double seconds() const { return timescale ? double(duration) / timescale : 0.0; }
    double kbps() const { double s = seconds(); return s > 0 ? mediaBytes * 8.0 / s / 1000.0 : 0.0; }
};

struct Probe {
    std::string path;
    uintmax_t bytes = 0;
    std::string container = "unknown";
    std::string majorBrand;
    std::string compatBrands;
    bool isMp4 = false;
    bool fragmented = false;
    uint32_t movieTimescale = 0;
    int64_t moovOff = -1, mdatOff = -1;
    std::vector<Track> tracks;
    std::vector<uint32_t> chapterTracks;  // track ids named by a 'tref chap' reference
    std::vector<std::string> fail;  // hard blockers
    std::vector<std::string> warn;  // will probably work, but risky
};

// -------------------------------------------------------------- ISO parser

struct Parser {
    std::ifstream &f;
    Probe &pr;
    Parser(std::ifstream &fi, Probe &p) : f(fi), pr(p) {}

    std::vector<uint8_t> readAt(uint64_t off, uint64_t n) {
        std::vector<uint8_t> b{};
        b.resize(size_t(n));
        f.clear();
        f.seekg(std::streamoff(off));
        f.read(reinterpret_cast<char *>(b.data()), std::streamsize(n));
        b.resize(size_t(f.gcount()));
        return b;
    }

    static bool isContainer(uint32_t t) {
        return t == FCC("moov") || t == FCC("trak") || t == FCC("mdia") ||
               t == FCC("minf") || t == FCC("stbl") || t == FCC("edts") ||
               t == FCC("moof") || t == FCC("traf") || t == FCC("mvex");
    }

    void walk(uint64_t start, uint64_t end, int depth, Track *cur) {
        uint64_t off = start;
        while (off + 8 <= end && depth < 12) {
            auto hdr = readAt(off, 16);
            if (hdr.size() < 8) return;
            uint64_t size = be32(hdr.data());
            uint32_t type = be32(hdr.data() + 4);
            uint64_t hdrLen = 8;
            if (size == 1) {
                if (hdr.size() < 16) return;
                size = be64(hdr.data() + 8);
                hdrLen = 16;
            } else if (size == 0) {
                size = end - off;  // extends to end of file
            }
            if (size < hdrLen || off + size > end) return;  // truncated / bogus

            if (depth == 0) {
                if (type == FCC("moov") && pr.moovOff < 0) pr.moovOff = int64_t(off);
                if (type == FCC("mdat") && pr.mdatOff < 0) pr.mdatOff = int64_t(off);
                if (type == FCC("ftyp")) parseFtyp(off + hdrLen, size - hdrLen);
                if (type == FCC("moof")) pr.fragmented = true;
            }

            if (type == FCC("trak")) {
                pr.tracks.emplace_back();
                walk(off + hdrLen, off + size, depth + 1, &pr.tracks.back());
            } else if (isContainer(type)) {
                walk(off + hdrLen, off + size, depth + 1, cur);
            } else if (type == FCC("mvhd")) {
                auto b = readAt(off + hdrLen, 32);
                if (b.size() >= 20) pr.movieTimescale = (b[0] == 1) ? be32(b.data() + 20) : be32(b.data() + 12);
            } else if (cur) {
                parseLeaf(type, off + hdrLen, size - hdrLen, *cur);
            }
            off += size;
        }
    }

    void parseFtyp(uint64_t off, uint64_t len) {
        auto b = readAt(off, std::min<uint64_t>(len, 64));
        if (b.size() >= 4) pr.majorBrand = fourcc(be32(b.data()));
        for (size_t p = 8; p + 4 <= b.size(); p += 4) {
            if (!pr.compatBrands.empty()) pr.compatBrands += ",";
            pr.compatBrands += fourcc(be32(b.data() + p));
        }
    }

    void parseLeaf(uint32_t type, uint64_t off, uint64_t len, Track &t) {
        if (type == FCC("tkhd")) {
            auto b = readAt(off, std::min<uint64_t>(len, 96));
            if (b.size() < 4) return;
            int ver = b[0];
            t.tkhdFlags = (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
            size_t idOff = (ver == 1) ? 4 + 8 + 8 : 4 + 4 + 4;
            if (b.size() >= idOff + 4) t.trackId = be32(b.data() + idOff);
            // tkhd volume is 8.8 fixed: v0 payload offset 36, v1 offset 48
            size_t volOff = (ver == 1) ? 48 : 36;
            if (b.size() >= volOff + 2) t.volume = be16(b.data() + volOff) / 256.0;
            t.sawTkhd = true;
        } else if (type == FCC("elst")) {
            auto b = readAt(off, std::min<uint64_t>(len, 256));
            if (b.size() < 8) return;
            int ver = b[0];
            uint32_t n = be32(b.data() + 4);
            t.elstEntries = int(n);
            size_t p = 8;
            for (uint32_t i = 0; i < n; i++) {
                int64_t segDur, mediaTime;
                if (ver == 1) {
                    if (p + 20 > b.size()) break;
                    segDur = int64_t(be64(b.data() + p));
                    mediaTime = int64_t(be64(b.data() + p + 8));
                    p += 20;
                } else {
                    if (p + 12 > b.size()) break;
                    segDur = int64_t(be32(b.data() + p));
                    mediaTime = int32_t(be32(b.data() + p + 4));
                    p += 12;
                }
                if (mediaTime == -1) t.elstEmptyDur += segDur;
            }
        } else if (type == FCC("hdlr")) {
            auto b = readAt(off, std::min<uint64_t>(len, 16));
            if (b.size() >= 12) t.handler = fourcc(be32(b.data() + 8));
        } else if (type == FCC("mdhd")) {
            auto b = readAt(off, std::min<uint64_t>(len, 40));
            if (b.size() < 4) return;
            int ver = b[0];
            size_t langOff;
            if (ver == 1 && b.size() >= 28) {
                t.timescale = be32(b.data() + 20);
                t.duration = be64(b.data() + 24);
                langOff = 32;
            } else if (b.size() >= 20) {
                t.timescale = be32(b.data() + 12);
                t.duration = be32(b.data() + 16);
                langOff = 20;
            } else {
                return;
            }
            if (b.size() >= langOff + 2) t.language = isoLang(be16(b.data() + langOff));
        } else if (type == FCC("tref")) {
            // A 'text' track referenced as 'chap' is a chapter list, not captions.
            auto b = readAt(off, std::min<uint64_t>(len, 256));
            size_t p = 0;
            while (p + 8 <= b.size()) {
                uint64_t sz = be32(b.data() + p);
                uint32_t ty = be32(b.data() + p + 4);
                if (sz < 8 || p + sz > b.size()) break;
                if (ty == FCC("chap"))
                    for (size_t q = p + 8; q + 4 <= p + sz; q += 4)
                        pr.chapterTracks.push_back(be32(b.data() + q));
                p += size_t(sz);
            }
        } else if (type == FCC("stsz")) {
            auto b = readAt(off, len);
            if (b.size() < 12) return;
            uint32_t uniform = be32(b.data() + 4);
            t.sampleCount = be32(b.data() + 8);
            if (uniform) {
                t.mediaBytes = uint64_t(uniform) * t.sampleCount;
            } else {
                for (uint32_t i = 0; i < t.sampleCount && 12 + 4 * (i + 1) <= b.size(); i++)
                    t.mediaBytes += be32(b.data() + 12 + 4 * i);
            }
        } else if (type == FCC("stsd")) {
            parseStsd(off, len, t);
        }
    }

    void parseStsd(uint64_t off, uint64_t len, Track &t) {
        auto b = readAt(off, std::min<uint64_t>(len, 8));
        if (b.size() < 8) return;
        uint32_t count = be32(b.data() + 4);
        uint64_t p = off + 8, end = off + len;
        for (uint32_t i = 0; i < count && p + 8 <= end; i++) {
            auto e = readAt(p, 8);
            if (e.size() < 8) return;
            uint64_t esz = be32(e.data());
            uint32_t fmt = be32(e.data() + 4);
            if (esz < 8 || p + esz > end) return;
            if (t.codec.empty()) t.codec = fourcc(fmt);
            if (t.handler == "vide") parseVisual(p, esz, t);
            else if (t.handler == "soun") parseAudio(p, esz, t);
            p += esz;
        }
    }

    void parseVisual(uint64_t off, uint64_t size, Track &t) {
        auto b = readAt(off, std::min<uint64_t>(size, 86));
        if (b.size() >= 36) {
            t.width = be16(b.data() + 32);
            t.height = be16(b.data() + 34);
        }
        // children of the visual sample entry start at +86 (avcC / hvcC / pasp ...)
        uint64_t p = off + 86, end = off + size;
        while (p + 8 <= end) {
            auto h = readAt(p, 8);
            if (h.size() < 8) return;
            uint64_t sz = be32(h.data());
            uint32_t ty = be32(h.data() + 4);
            if (sz < 8 || p + sz > end) return;
            if (ty == FCC("avcC")) {
                auto c = readAt(p + 8, std::min<uint64_t>(sz - 8, 4));
                if (c.size() >= 4) {
                    t.avcProfile = c[1];
                    t.avcLevel = c[3];
                }
            }
            p += sz;
        }
    }

    void parseAudio(uint64_t off, uint64_t size, Track &t) {
        auto b = readAt(off, std::min<uint64_t>(size, 64));
        if (b.size() < 36) return;
        uint16_t ver = be16(b.data() + 16);
        t.chan = be16(b.data() + 24);
        t.rate = be16(b.data() + 32);  // 16.16 fixed point -> integer part
        uint64_t childOff = 36;
        if (ver == 1) childOff = 36 + 16;
        else if (ver == 2) childOff = 36 + 36;
        uint64_t p = off + childOff, end = off + size;
        while (p + 8 <= end) {
            auto h = readAt(p, 8);
            if (h.size() < 8) return;
            uint64_t sz = be32(h.data());
            uint32_t ty = be32(h.data() + 4);
            if (sz < 8 || p + sz > end) return;
            if (ty == FCC("esds")) parseEsds(readAt(p + 8, sz - 8), t);
            p += sz;
        }
    }

    static uint32_t descLen(const std::vector<uint8_t> &d, size_t &p) {
        uint32_t len = 0;
        for (int i = 0; i < 4 && p < d.size(); i++) {
            uint8_t c = d[p++];
            len = (len << 7) | (c & 0x7F);
            if (!(c & 0x80)) break;
        }
        return len;
    }

    static void parseEsds(const std::vector<uint8_t> &b, Track &t) {
        size_t p = 4;  // skip version+flags
        if (p < b.size() && b[p] == 0x03) {  // ES_Descriptor
            p++;
            descLen(b, p);
            if (p + 3 > b.size()) return;
            p += 2;                 // ES_ID
            uint8_t fl = b[p++];    // flags
            if (fl & 0x80) p += 2;  // dependsOn_ES_ID
            if (fl & 0x40) { if (p < b.size()) { uint8_t l = b[p++]; p += l; } }  // URL
            if (fl & 0x20) p += 2;  // OCR_ES_ID
        }
        if (p >= b.size() || b[p] != 0x04) return;  // DecoderConfigDescriptor
        p++;
        descLen(b, p);
        if (p >= b.size()) return;
        t.oti = b[p];
        p += 1 + 1 + 3 + 4 + 4;  // oti, streamType, bufferSize, max/avg bitrate
        if (p >= b.size() || b[p] != 0x05) return;  // DecoderSpecificInfo
        p++;
        uint32_t l = descLen(b, p);
        size_t a = std::min(p, b.size()), z = std::min(p + l, b.size());
        std::vector<uint8_t> asc(b.begin() + a, b.begin() + z);
        if (asc.empty()) return;
        Bits bits{asc};
        uint32_t aot = bits.get(5);
        if (aot == 31) aot = 32 + bits.get(6);
        t.aot = int(aot);
        uint32_t sfi = bits.get(4);
        uint32_t sr = (sfi == 15) ? bits.get(24) : kAacRates[sfi & 15];
        uint32_t ch = bits.get(4);
        if (sr) t.rate = sr;
        if (ch) t.chan = int(ch);
        if (aot == 5 || aot == 29) {
            t.sbr = true;
            uint32_t esfi = bits.get(4);
            if (esfi == 15) bits.get(24);
            uint32_t base = bits.get(5);
            if (base == 31) base = 32 + bits.get(6);
            t.aot = int(base);  // report the core object type, SBR flagged separately
        }
    }
};

// ------------------------------------------------------------ verification

inline void detectContainer(Probe &pr, std::ifstream &f) {
    auto magic = std::vector<uint8_t>(16);
    f.seekg(0);
    f.read(reinterpret_cast<char *>(magic.data()), 16);
    size_t n = size_t(f.gcount());
    if (n >= 8 && be32(magic.data() + 4) == FCC("ftyp")) {
        pr.container = "MP4 / ISO-BMFF";
        pr.isMp4 = true;
    } else if (n >= 4 && be32(magic.data()) == 0x1A45DFA3u) {
        pr.container = "Matroska / WebM";
    } else if (n >= 12 && be32(magic.data()) == FCC("RIFF") && be32(magic.data() + 8) == FCC("AVI ")) {
        pr.container = "AVI";
    } else if (n >= 4 && be32(magic.data()) == FCC("FLV\x01")) {
        pr.container = "FLV";
    } else if (n >= 4 && (magic[0] == 0x00 && magic[1] == 0x00 && magic[2] == 0x01)) {
        pr.container = "MPEG elementary/PS";
    }
}

// Caption tracks, chapter lists excluded. Shared so the CLI, the server and the
// rules below can never disagree about what counts as a caption.
inline std::vector<const Track *> captionTracks(const Probe &pr) {
    std::vector<const Track *> out;
    for (const auto &t : pr.tracks) {
        if (!isCaptionHandler(t.handler)) continue;
        bool chapter = false;
        for (uint32_t id : pr.chapterTracks)
            if (id == t.trackId) chapter = true;
        if (!chapter) out.push_back(&t);
    }
    return out;
}

inline void verify(Probe &pr) {
    if (!pr.isMp4) {
        pr.fail.push_back("Container is " + pr.container + " — Unity WebGL/Android need MP4.");
        return;
    }
    const Track *v = nullptr, *a = nullptr;
    for (auto &t : pr.tracks) {
        if (t.handler == "vide" && !v) v = &t;
        if (t.handler == "soun" && !a) a = &t;
    }

    // ---- video
    if (!v) {
        pr.fail.push_back("No video track found.");
    } else if (v->codec == "hvc1" || v->codec == "hev1" || v->codec == "dvh1" || v->codec == "dvhe") {
        pr.fail.push_back("Video is H.265/HEVC (" + v->codec + ") — WebGL cannot decode it. Re-encode to H.264.");
    } else if (v->codec == "av01") {
        pr.fail.push_back("Video is AV1 — not supported. Re-encode to H.264.");
    } else if (v->codec == "vp09" || v->codec == "vp08") {
        pr.fail.push_back("Video is VP8/VP9 — not supported. Re-encode to H.264.");
    } else if (v->codec != "avc1") {
        pr.fail.push_back("Video codec '" + v->codec + "' is not H.264/avc1.");
    } else {
        if (v->avcProfile > 100)
            pr.fail.push_back("H.264 profile " + std::string(avcProfileName(v->avcProfile)) +
                              " (10-bit/4:2:2) is not hardware-decodable on mobile. Use High or Baseline 8-bit.");
        if (v->width % 2 || v->height % 2)
            pr.warn.push_back("Odd frame dimensions — some decoders reject non-even width/height.");
    }

    // ---- audio (the reported symptom)
    if (!a) {
        pr.fail.push_back("NO AUDIO TRACK in the file — the video was exported without audio.");
    } else if (a->codec != "mp4a") {
        pr.fail.push_back("Audio codec '" + a->codec + "' is not AAC/mp4a" +
                          std::string(a->codec == "Opus" || a->codec == "opus" ? " (Opus is WebM-era, unsupported)"
                                                                              : "") + ".");
    } else if (a->oti != 0x40 && a->oti != 0x67 && a->oti != -1) {
        pr.fail.push_back("Audio is " + std::string(otiName(a->oti)) +
                          " inside an MP4 box — Unity will show video with silent audio. Re-encode to AAC-LC.");
    } else if (a->aot > 0 && a->aot != 2) {
        pr.fail.push_back("Audio object type is " + std::string(aotName(a->aot)) +
                          " — Unity's decoder only handles AAC-LC (AOT 2).");
    } else if (a->sbr) {
        pr.fail.push_back("Audio is HE-AAC (SBR/PS). WebGL decodes it as silence or half-rate. Use plain AAC-LC.");
    }
    if (a) {
        if (a->sampleCount == 0 && !pr.fragmented)
            pr.fail.push_back("Audio track has 0 samples — the track exists but carries no sound.");
        if (a->rate && a->rate != 44100 && a->rate != 48000)
            pr.warn.push_back("Audio sample rate is " + std::to_string(a->rate) +
                              " Hz; use 48000 (or 44100) for reliable playback.");
        if (a->chan > 2)
            pr.warn.push_back("Audio has " + std::to_string(a->chan) +
                              " channels — downmix to stereo, multichannel AAC is unreliable on mobile.");
        if (a->sawTkhd && !(a->tkhdFlags & 0x1))
            pr.fail.push_back("Audio track is DISABLED in tkhd (flags=0x" +
                              [](uint32_t f){ char b[16]; std::snprintf(b, sizeof b, "%06X", f); return std::string(b); }(a->tkhdFlags) +
                              ") — players skip it entirely. This plays video with no sound.");
        else if (a->sawTkhd && !(a->tkhdFlags & 0x2))
            pr.warn.push_back("Audio track is not flagged 'in movie' (track_in_movie bit clear).");
        double abr = a->kbps();
        if (a->sampleCount > 0 && abr > 0 && abr < 12.0)
            pr.fail.push_back("Audio stream is only " + std::to_string(int(abr + 0.5)) +
                              " kbps — that is encoded SILENCE, not speech. The source had no audio "
                              "or was muted before export.");
        else if (abr > 0 && abr < 40.0)
            pr.warn.push_back("Audio bitrate is low (" + std::to_string(int(abr + 0.5)) +
                              " kbps); check the mix level is audible.");
        if (a->volume >= 0.0 && a->volume < 0.01)
            pr.fail.push_back("Audio track volume in tkhd is 0.0 — the track is muted at container level.");
        if (a->elstEmptyDur > 0 && pr.movieTimescale) {
            double delay = double(a->elstEmptyDur) / pr.movieTimescale;
            if (delay > 0.25)
                pr.fail.push_back("Audio has a leading empty edit of " + std::to_string(delay) +
                                  "s — Unity ignores edit lists, so audio drifts or never starts.");
        }
        if (a->elstEntries > 1)
            pr.warn.push_back("Audio has " + std::to_string(a->elstEntries) +
                              " edit-list entries; Unity ignores edit lists. Re-mux without them.");
        if (v && v->seconds() > 1.0 && a->seconds() < v->seconds() * 0.5)
            pr.fail.push_back("Audio track is only " + std::to_string(int(a->seconds())) + "s vs " +
                              std::to_string(int(v->seconds())) + "s of video — audio is truncated/empty.");
    }

    // ---- closed captions (accessibility, never a blocker)
    auto caps = captionTracks(pr);
    if (caps.empty()) {
        pr.warn.push_back("No closed-caption track — nothing to display for viewers who need "
                          "captions. Add one, or ship a WebVTT sidecar next to the MP4.");
    }
    for (const Track *c : caps) {
        std::string what = capCodecName(c->codec) + " (" + c->codec + ")" +
                           (c->language.empty() ? "" : ", " + c->language);
        if (c->sampleCount == 0 && !pr.fragmented) {
            pr.warn.push_back("Closed-caption track is empty — " + what + " with 0 samples.");
            continue;
        }
        if (c->sawTkhd && !(c->tkhdFlags & 0x1))
            pr.warn.push_back("Closed-caption track is DISABLED in tkhd — players skip it.");
        if (c->codec == "c608" || c->codec == "c708")
            pr.warn.push_back("Captions are " + what + ". Safari renders these; Chrome and "
                              "Firefox do not, so WebGL viewers see nothing. Ship a WebVTT sidecar.");
        else if (c->codec == "wvtt" || c->codec == "stpp")
            pr.warn.push_back("Captions are in-band " + what + ", which only plays through a "
                              "DASH/fragmented-MP4 pipeline. Ship a WebVTT sidecar for <video>.");
        else
            pr.warn.push_back("Captions are " + what + ". No browser renders in-band timed text "
                              "in <video>, so Unity WebGL shows none — ship a WebVTT sidecar "
                              "and add it with <track>. The track still survives the re-encode.");
    }

    // ---- streaming layout
    if (pr.moovOff >= 0 && pr.mdatOff >= 0 && pr.moovOff > pr.mdatOff)
        pr.fail.push_back("'moov' atom is at the END of the file (not faststart) — streaming from AWS "
                          "stalls or plays without audio until fully buffered. Add -movflags +faststart.");
    if (pr.moovOff < 0 && !pr.fragmented)
        pr.fail.push_back("No 'moov' atom — file is truncated or corrupt.");
}

// ------------------------------------------------- top-level entry points

inline Probe probeFile(const fs::path &p) {
    Probe pr;
    pr.path = p.string();
    std::error_code ec;
    pr.bytes = fs::file_size(p, ec);
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        pr.fail.push_back("Cannot open file.");
        return pr;
    }
    detectContainer(pr, f);
    if (pr.isMp4) {
        Parser parser(f, pr);
        parser.walk(0, pr.bytes, 0, nullptr);
    }
    verify(pr);
    return pr;
}

inline bool isVideoExt(const fs::path &p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == ".mp4" || e == ".mov" || e == ".m4v" || e == ".webm" || e == ".mkv" ||
           e == ".avi" || e == ".flv" || e == ".mpg" || e == ".mpeg" || e == ".wmv" || e == ".ts";
}

