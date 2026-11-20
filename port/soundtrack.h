// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
// A custom soundtrack: the player's own music played in place of the disc's audio tracks, through the sound
// chip's CD audio input as the disc's would be (runtime/music.h, Replacement), so the game's own volume, fades and
// effects apply to it. The game sees its disc's track play as before; only what is heard changes.
//
// The music is in a folder of the player's (--soundtrack DIR; the launcher's game page chooses it; none: the
// "soundtrack" folder beside settings.ini), one file a song, named by the song's name (vf2_akira.flac: the game's
// sources name them, runtime/game.h Game::songs, and the folder's songs.txt lists them) or its track's number
// (14.ogg, track14.ogg), as Ogg Vorbis, FLAC, MP3 or WAV. soundtrack.ini in the folder can name another file for a
// song, or a track of another disc image the player has (another game's music, say), its loop and its volume:
//
//   volume = 80                  ; every song, in percent
//   disc = Fighting Vipers.cue   ; another disc, for the songs below that name a track and no disc of their own;
//                                ; and, where the game knows that disc (Game::songDiscs), for the songs it has
//
//   [fv_bahn]                    ; a song by its name (or its track: [11])
//   track = 7                    ; that disc's audio track 7
//
//   [vf2_akira]
//   file = 05 Akira.flac
//   loop = 0:12.5 - 2:47.25      ; the part that repeats: start - end (minutes:seconds, or seconds); "no": once
//   volume = 90
//
// A song repeats as the game repeats its track (a stage's music, over and over; a fanfare once). A file repeats
// whole unless it says otherwise: by `loop`, or the loop tags many soundtrack packs have (LOOPSTART, with LOOPEND
// or LOOPLENGTH, in samples). A song longer than the disc's track plays on to its end after the track would have
// stopped, unless the game asks for other music first.
//
// Another game's disc the game knows (Game::songDiscs) needs no soundtrack.ini: its disc image (a .cue or .chd, in the folder or
// a folder in it) is found by its product number, and plays for the songs it has. songs.txt, written at every start,
// says how to give the game music, which discs were found, and what plays for each song.
//
// A file is read on a thread of its own, from its start, as soon as the game asks for its song (the samples kept
// in memory, the last few songs' of them); the sound waits for it only in the moment the file is opened.
#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace port {

class DiscImage;

class Soundtrack {
public:
    Soundtrack();
    ~Soundtrack();

    // Reads the folder (its files' names and soundtrack.ini) and writes songs.txt in it. Returns how many songs it
    // has music for.
    int Open(const std::string &folder);
    void Close();
    // The game's songs, one a line (--list-songs)
    static std::string SongList();

    // (runtime/music.h, Replacement)
    bool Has(int track) const {
        return on && Playing(track);
    }
    bool Sample(int track, uint64_t n, bool repeat, int16_t &left, int16_t &right);

    bool on = false; // the feature's switch (the port sets it each frame)

private:
    struct Song {
        std::string file;              // a sound file, or
        std::string disc; int discTrack = 0; // another disc's audio track
        double loopStart = -1, loopEnd = -1; // seconds (-1: from the file's tags, or the whole of it)
        int once = -1;                 // 1: played once, 0: repeated, -1: as the game plays its track
        int volume = 100;
    };
    struct Samples;
    std::vector<Song> m_songs;
    std::vector<int> m_byTrack; // the song for each track of the disc (-1: none)
    int m_volume = 100;
    std::map<int, std::shared_ptr<Samples>> m_loaded; // by song
    std::list<int> m_recent;                          // the songs loaded, the last used first
    std::map<std::string, std::unique_ptr<DiscImage>> m_discs;
    std::vector<std::string> m_discsFound;              // (for songs.txt)
    std::map<std::string, std::string> m_discNames;     // a known disc's name, by its path
    Samples *Load(int song);
    DiscImage *Disc(const std::string &path);
    bool Playing(int track) const {
        return track >= 0 && static_cast<size_t>(track) < m_byTrack.size() && m_byTrack[track] >= 0;
    }
    std::string Status() const;
};

} // namespace port
