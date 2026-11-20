// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
#include "soundtrack.h"

#include "../runtime/game.h"
#include "decoders.h"
#include "disc_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace port {

namespace {
    constexpr int kRate = 44100; // (the sound chip's CD audio input's)
    constexpr size_t kKeep = 3;  // songs whose samples are kept

    std::filesystem::path TextPath(const std::string &text) {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }
    std::string PathText(const std::filesystem::path &path) {
        const auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::string Lower(std::string text) {
        for (char &c : text) {
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        }
        return text;
    }

    std::string Trim(const std::string &text) {
        const size_t from = text.find_first_not_of(" \t\r\n");
        if (from == std::string::npos) {
            return "";
        }
        return text.substr(from, text.find_last_not_of(" \t\r\n") - from + 1);
    }

    // "1:02.5", "62.5": seconds
    double Seconds(const std::string &text) {
        double seconds = 0;
        size_t at = 0;
        while (true) {
            const size_t colon = text.find(':', at);
            seconds = seconds * 60 + atof(text.substr(at, colon == std::string::npos ? std::string::npos : colon - at).c_str());
            if (colon == std::string::npos) {
                return seconds;
            }
            at = colon + 1;
        }
    }

    // soundtrack.ini: [section] lines and key = value lines. A line that starts with ';' or '#' is a remark, and so is
    // the rest of a line from one, but for a file's or disc's name (which may have them)
    std::map<std::string, std::map<std::string, std::string>> ReadIni(const std::filesystem::path &path) {
        std::map<std::string, std::map<std::string, std::string>> sections;
        std::ifstream file(path);
        std::string section, line;
        while (std::getline(file, line)) {
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') {
                continue;
            }
            if (line.front() == '[') {
                const size_t end = line.find(']');
                if (end != std::string::npos) {
                    section = Lower(Trim(line.substr(1, end - 1)));
                }
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                continue;
            }
            const std::string key = Lower(Trim(line.substr(0, eq)));
            std::string value = line.substr(eq + 1);
            if (key != "file" && key != "disc") {
                value = value.substr(0, value.find_first_of(";#"));
            }
            sections[section][key] = Trim(value);
        }
        return sections;
    }

    // The names a track's song goes by in the folder: the game's name for it, and its number (14, track14)
    std::vector<std::string> NamesOf(int track) {
        std::vector<std::string> names;
        for (const satrecomp::Song &song : satrecomp::TheGame().songs) {
            if (song.track == track) {
                names.push_back(Lower(song.name));
            }
        }
        names.push_back(std::to_string(track));
        names.push_back("0" + std::to_string(track));
        names.push_back("track" + std::to_string(track));
        names.push_back("track0" + std::to_string(track));
        return names;
    }

    int16_t Clamp(float v) {
        return static_cast<int16_t>(std::clamp(std::lround(v), -32768L, 32767L));
    }
} // namespace

// A song's samples at the CD's rate, stereo: a file's, read into memory by a thread of its own as fast as it can
// (`ready` of them so far), or another disc's track, read a sector at a time when wanted
struct Soundtrack::Samples {
    std::vector<int16_t> data;
    std::atomic<uint64_t> ready{0};
    std::atomic<bool> done{false}, cancel{false};
    std::atomic<uint64_t> length{0}; // in samples (made shorter by the reader if the file ends sooner)
    uint64_t loopStart = 0, loopEnd = 0; // in samples (loopEnd 0: the end)
    int volume = 100;
    std::thread reader;
    // (another disc's track)
    DiscImage *disc = nullptr;
    const void *discTrack = nullptr;
    int64_t sectorHeld = -1;
    uint8_t sector[2352];

    ~Samples() {
        cancel = true;
        if (reader.joinable()) {
            reader.join();
        }
    }

    // The file's samples, from its rate to the CD's (straight lines between them), stereo
    void Read(std::unique_ptr<Decoder> decoder) {
        const int channels = decoder->channels;
        const double step = static_cast<double>(decoder->rate) / kRate; // (the file's samples for one of the CD's)
        std::vector<float> window;  // the file's samples from `base` on
        uint64_t base = 0;
        bool end = false;
        std::vector<float> chunk(4096 * static_cast<size_t>(channels));
        uint64_t made = 0;
        for (; made < length && !cancel; made++) {
            const double at = made * step;
            const uint64_t first = static_cast<uint64_t>(at);
            const float part = static_cast<float>(at - static_cast<double>(first));
            // (the window to hold samples first and first + 1)
            while (!end && base + window.size() / channels <= first + 1) {
                if (first > base + 8192) {
                    const uint64_t drop = std::min<uint64_t>(first - base, window.size() / channels);
                    window.erase(window.begin(), window.begin() + static_cast<ptrdiff_t>(drop * channels));
                    base += drop;
                }
                const size_t got = decoder->Read(chunk.data(), 4096);
                window.insert(window.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got * channels));
                end = got == 0;
            }
            const uint64_t have = base + window.size() / channels;
            if (first >= have) {
                break; // (the file ended before its length said)
            }
            const size_t a = static_cast<size_t>(first - base) * channels;
            const size_t b = first + 1 < have ? a + channels : a;
            const float la = window[a], lb = window[b];
            const float ra = channels > 1 ? window[a + 1] : la, rb = channels > 1 ? window[b + 1] : lb;
            data[made * 2] = Clamp((la + (lb - la) * part) * 32767.0f);
            data[made * 2 + 1] = Clamp((ra + (rb - ra) * part) * 32767.0f);
            if ((made & 1023) == 1023) {
                ready.store(made + 1, std::memory_order_release);
            }
        }
        if (made < length) {
            length = made;
        }
        ready.store(made, std::memory_order_release);
        done.store(true, std::memory_order_release);
    }
};

Soundtrack::Soundtrack() = default;
Soundtrack::~Soundtrack() {
    Close();
}

void Soundtrack::Close() {
    m_loaded.clear();
    m_recent.clear();
    m_songs.clear();
    m_byTrack.clear();
    m_discs.clear();
}

std::string Soundtrack::SongList() {
    std::string text = "# The music a custom soundtrack can replace: name a file in this folder as the first column (or the\n"
                       "# track's number, the second), with .ogg, .flac, .mp3 or .wav; or name it in soundtrack.ini.\n";
    for (const satrecomp::Song &song : satrecomp::TheGame().songs) {
        char line[256];
        snprintf(line, sizeof(line), "%-18s %3d  %s\n", song.name, song.track, song.title);
        text += line;
    }
    return text;
}

int Soundtrack::Open(const std::string &folder) {
    Close();
    std::error_code error;
    const std::filesystem::path dir = TextPath(folder);
    std::filesystem::create_directories(dir, error);
    // The folder's sound files by their names without the ending, in small letters
    std::map<std::string, std::string> stems;
    for (std::filesystem::directory_iterator each(dir, error); !error && each != std::filesystem::directory_iterator(); each.increment(error)) {
        const std::string ending = Lower(PathText(each->path().extension()));
        if (ending == ".ogg" || ending == ".oga" || ending == ".flac" || ending == ".mp3" || ending == ".wav") {
            stems.emplace(Lower(PathText(each->path().stem())), PathText(each->path()));
        }
    }
    auto ini = ReadIni(dir / "soundtrack.ini");
    auto inFolder = [&](const std::string &name) {
        const std::filesystem::path path = TextPath(name);
        return PathText(path.is_absolute() ? path : dir / path);
    };
    m_volume = ini[""].count("volume") ? std::clamp(atoi(ini[""]["volume"].c_str()), 0, 400) : 100;
    const std::string commonDisc = ini[""].count("disc") ? inFolder(ini[""]["disc"]) : "";
    // Discs the game knows the music of (Game::songDiscs), by their product numbers: each disc image (.cue, .chd) in the
    // folder or a folder in it, and soundtrack.ini's disc
    std::map<int, std::pair<std::string, int>> known; // this game's track: a disc, and its track
    std::vector<std::string> cues;
    if (!commonDisc.empty()) {
        cues.push_back(commonDisc);
    }
    for (std::filesystem::recursive_directory_iterator each(
             dir, std::filesystem::directory_options::skip_permission_denied | std::filesystem::directory_options::follow_directory_symlink, error);
         !error && each != std::filesystem::recursive_directory_iterator(); each.increment(error)) {
        if (each.depth() > 1) {
            each.disable_recursion_pending();
        }
        if (const std::string ext = Lower(PathText(each->path().extension())); ext == ".cue" || ext == ".chd") {
            cues.push_back(PathText(each->path()));
        }
    }
    std::sort(cues.begin() + (commonDisc.empty() ? 0 : 1), cues.end());
    m_discsFound.clear();
    for (const std::string &cue : cues) {
        const DiscImage *image = Disc(cue);
        uint8_t header[2048] = {};
        const bool read = image && image->ReadUserData(150, header);
        const std::string product = read ? Trim(std::string(reinterpret_cast<char *>(header) + 0x20, 10)) : "";
        const std::string title = read ? Trim(std::string(reinterpret_cast<char *>(header) + 0x60, 112)) : "";
        const satrecomp::SongDisc *disc = nullptr;
        for (const satrecomp::SongDisc &other : satrecomp::TheGame().songDiscs) {
            if (other.product ? !product.empty() && product.rfind(other.product, 0) == 0 : other.title && title == other.title) {
                disc = &other;
            }
        }
        const std::string file = PathText(TextPath(cue).filename());
        if (!image) {
            m_discsFound.push_back(file + ": cannot be read as a disc image");
        } else if (!disc) {
            m_discsFound.push_back(file + ": not a disc this game knows the music of" + (product.empty() ? "" : " (" + product + ")") +
                                   "; soundtrack.ini can still take its tracks by number");
        } else {
            m_discNames[cue] = disc->name;
            int taken = 0;
            for (const auto &[mine, theirs] : disc->tracks) {
                if (known.emplace(mine, std::make_pair(cue, theirs)).second) {
                    taken++;
                }
            }
            m_discsFound.push_back(file + ": " + disc->name + ", " +
                                   (taken ? std::to_string(taken) + (taken == 1 ? " song" : " songs") : std::string("nothing more (its songs come from a disc above)")));
        }
    }

    // Every track a song might be: the game's songs' and the numbers 2 to 99
    for (int track = 1; track < 100; track++) {
        Song song;
        const std::map<std::string, std::string> *own = nullptr;
        for (const std::string &name : NamesOf(track)) {
            if (!own && ini.count(name)) {
                own = &ini[name];
            }
            if (song.file.empty() && stems.count(name)) {
                song.file = stems[name];
            }
        }
        if (own) {
            if (own->count("file")) {
                song.file = inFolder(own->at("file"));
            }
            if (own->count("track")) {
                song.file.clear();
                song.discTrack = atoi(own->at("track").c_str());
                song.disc = own->count("disc") ? inFolder(own->at("disc")) : commonDisc;
            }
            if (own->count("volume")) {
                song.volume = std::clamp(atoi(own->at("volume").c_str()), 0, 400);
            }
            if (own->count("loop")) {
                const std::string loop = Lower(own->at("loop"));
                const size_t dash = loop.find('-');
                if (loop == "no" || loop == "off" || loop == "once") {
                    song.once = 1;
                } else if (loop == "yes" || loop == "on") {
                    song.once = 0;
                } else if (dash != std::string::npos) {
                    song.once = 0;
                    song.loopStart = Seconds(Trim(loop.substr(0, dash)));
                    song.loopEnd = Seconds(Trim(loop.substr(dash + 1)));
                    if (song.loopEnd <= song.loopStart) {
                        song.loopStart = song.loopEnd = -1;
                    }
                }
            }
        }
        if (song.file.empty() && song.discTrack == 0 && known.count(track)) {
            song.disc = known[track].first;
            song.discTrack = known[track].second;
        }
        if (!song.disc.empty() && song.discTrack > 0) {
            const DiscImage *image = Disc(song.disc);
            const bool found = image && std::any_of(image->Tracks().begin(), image->Tracks().end(),
                                                    [&](const DiscImage::Track &t) { return t.number == song.discTrack && t.audio; });
            if (!found) {
                if (image) {
                    fprintf(stderr, "soundtrack: %s has no audio track %d\n", song.disc.c_str(), song.discTrack);
                }
                continue;
            }
        } else if (song.file.empty() || !std::filesystem::is_regular_file(TextPath(song.file), error)) {
            continue;
        }
        if (m_byTrack.size() <= static_cast<size_t>(track)) {
            m_byTrack.resize(track + 1u, -1);
        }
        m_byTrack[track] = static_cast<int>(m_songs.size());
        m_songs.push_back(song);
    }
    if (!satrecomp::TheGame().songs.empty()) {
        std::ofstream(dir / "songs.txt", std::ios::binary) << Status();
    }
    return static_cast<int>(m_songs.size());
}

// songs.txt: how to give the game music, the discs found, and what plays for each song now
std::string Soundtrack::Status() const {
    const satrecomp::Game &game = satrecomp::TheGame();
    std::string text = "The music of this game, and what plays for each piece of it while the Custom soundtrack feature is on.\n"
                       "(Written each time the game starts: change this folder, then start the game again to see it here.)\n\n"
                       "To give a song your own music, put a file here named as its first column below, with .ogg, .flac, .mp3\n"
                       "or .wav: for instance " +
                       std::string(game.songs.empty() ? "selector" : game.songs[game.songs.size() * 3 / 4].name) + ".flac.\n";
    if (!game.songDiscs.empty()) {
        text += "\nTo take music from another game's disc, put its disc image here: its .chd, or the .cue file with its .bin files, or\n"
                "the folder they are in. The game knows these discs' music:\n";
        std::vector<std::string> said;
        for (const satrecomp::SongDisc &disc : game.songDiscs) {
            if (std::find(said.begin(), said.end(), disc.name) == said.end()) {
                said.push_back(disc.name);
                text += "  " + std::string(disc.name) + (disc.what ? std::string(": ") + disc.what : "") + "\n";
            }
        }
        text += "A file of your own for a song goes before a disc's.\n";
    }
    text += "\nsoundtrack.ini in this folder can say more: another name for a song's file, its loop, its volume, or any\n"
            "disc's track by number.\n\n";
    text += "Discs found here:\n";
    if (m_discsFound.empty()) {
        text += "  (none)\n";
    }
    for (const std::string &line : m_discsFound) {
        text += "  " + line + "\n";
    }
    text += "\n";
    char line[512];
    snprintf(line, sizeof(line), "%-16s %5s  %-34s %s\n", "song", "track", "what it is", "plays");
    text += line;
    for (const satrecomp::Song &song : game.songs) {
        std::string plays = "the game's own";
        if (Playing(song.track)) {
            const Song &mine = m_songs[m_byTrack[song.track]];
            if (!mine.disc.empty()) {
                const auto name = m_discNames.find(mine.disc);
                plays = (name != m_discNames.end() ? name->second : PathText(TextPath(mine.disc).filename())) + ", track " +
                        std::to_string(mine.discTrack);
            } else {
                plays = PathText(TextPath(mine.file).filename());
            }
        }
        snprintf(line, sizeof(line), "%-16s %5d  %-34s %s\n", song.name, song.track, song.title, plays.c_str());
        text += line;
    }
    return text;
}

// A disc image named in soundtrack.ini, opened once (null: it cannot be read)
DiscImage *Soundtrack::Disc(const std::string &path) {
    if (!m_discs.count(path)) {
        auto image = std::make_unique<DiscImage>();
        if (!image->Open(path)) {
            fprintf(stderr, "soundtrack: cannot read the disc image %s\n", path.c_str());
            image.reset();
        }
        m_discs[path] = std::move(image);
    }
    return m_discs[path].get();
}

// A song's samples: kept from before, or begun now (another song's let go when more than kKeep are kept)
Soundtrack::Samples *Soundtrack::Load(int index) {
    if (auto kept = m_loaded.find(index); kept != m_loaded.end()) {
        if (m_recent.front() != index) {
            m_recent.remove(index);
            m_recent.push_front(index);
        }
        return kept->second.get();
    }
    const Song &song = m_songs[index];
    auto samples = std::make_shared<Samples>();
    samples->volume = song.volume * m_volume / 100;
    if (!song.disc.empty()) {
        DiscImage *image = m_discs[song.disc].get();
        for (const DiscImage::Track &t : image->Tracks()) {
            if (t.number == song.discTrack) {
                samples->disc = image;
                samples->discTrack = &t;
                samples->length = static_cast<uint64_t>(t.endFAD - t.startFAD + 1) * 588;
            }
        }
        samples->done = true;
        samples->ready = samples->length.load();
    } else {
        std::unique_ptr<Decoder> decoder = OpenDecoder(song.file);
        if (!decoder || decoder->length == 0) {
            fprintf(stderr, "soundtrack: %s is not a sound file this reads (Ogg Vorbis, FLAC, MP3, WAV)\n", song.file.c_str());
            samples->done = true;
        } else {
            const double step = static_cast<double>(decoder->rate) / kRate;
            samples->length = static_cast<uint64_t>((decoder->length - 1) / step) + 1;
            if (song.loopStart >= 0) {
                samples->loopStart = static_cast<uint64_t>(song.loopStart * kRate);
                samples->loopEnd = static_cast<uint64_t>(song.loopEnd * kRate);
            } else if (decoder->loopStart >= 0) {
                samples->loopStart = static_cast<uint64_t>(decoder->loopStart / step);
                samples->loopEnd = decoder->loopEnd > decoder->loopStart ? static_cast<uint64_t>(decoder->loopEnd / step) : 0;
            }
            samples->data.resize(samples->length.load() * 2);
            Samples *s = samples.get();
            samples->reader = std::thread([s, d = std::move(decoder)]() mutable { s->Read(std::move(d)); });
        }
    }
    m_loaded[index] = samples;
    m_recent.push_front(index);
    while (m_recent.size() > kKeep) {
        m_loaded.erase(m_recent.back());
        m_recent.pop_back();
    }
    return samples.get();
}

bool Soundtrack::Sample(int track, uint64_t n, bool repeat, int16_t &left, int16_t &right) {
    if (!Has(track)) {
        return false;
    }
    const int index = m_byTrack[track];
    Samples &s = *Load(index);
    const Song &song = m_songs[index];
    const uint64_t length = s.length.load();
    if (length == 0) {
        return false;
    }
    uint64_t at = n;
    const bool loops = song.once < 0 ? repeat : song.once == 0;
    if (!loops) {
        if (n >= length) {
            return false;
        }
    } else {
        const uint64_t end = s.loopEnd && s.loopEnd < length ? s.loopEnd : length;
        const uint64_t start = s.loopStart < end ? s.loopStart : 0;
        if (n >= end) {
            at = start + (n - start) % (end - start);
        }
    }
    if (s.disc) {
        const auto &t = *static_cast<const DiscImage::Track *>(s.discTrack);
        const int64_t sector = static_cast<int64_t>(at / 588);
        if (sector != s.sectorHeld) {
            if (!s.disc->ReadRaw(t, static_cast<uint32_t>(sector), s.sector)) {
                return false;
            }
            s.sectorHeld = sector;
        }
        const uint8_t *p = &s.sector[(at % 588) * 4];
        left = static_cast<int16_t>(p[0] | (p[1] << 8));
        right = static_cast<int16_t>(p[2] | (p[3] << 8));
    } else {
        // (the reader is far ahead of the sound but at a song's start: waits for it there)
        while (s.ready.load(std::memory_order_acquire) <= at && !s.done.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (at >= s.ready.load(std::memory_order_acquire)) {
            return false;
        }
        left = s.data[at * 2];
        right = s.data[at * 2 + 1];
    }
    if (s.volume != 100) {
        left = Clamp(left * (s.volume / 100.0f));
        right = Clamp(right * (s.volume / 100.0f));
    }
    return true;
}

} // namespace port
