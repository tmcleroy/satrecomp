// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Tommy McLeroy. Part of satrecomp: https://github.com/tmcleroy/satrecomp
//
// Android: the game is a library (lib<id>.so) that its activity (android/, GameActivity.java) loads in a process of
// its own and calls the main of, with the launcher's arguments. What a program started elsewhere gets from the
// launcher, the activity gives as variables: RECOMP_FOLDER, the player's folder for the game (the current folder
// elsewhere), and RECOMP_LOG, the file its output goes to (logs/game.log). Without a log, the output goes to the
// system's log (adb logcat, tag satrecomp).
#include "port.h"

#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace port {

namespace {

// The standard output and error, a line at a time to the system's log
void ToSystemLog() {
    static int pipes[2];
    if (pipe(pipes) != 0) {
        return;
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(pipes[1], 1);
    dup2(pipes[1], 2);
    std::thread([] {
        std::string line;
        char buffer[512];
        ssize_t got;
        while ((got = read(pipes[0], buffer, sizeof buffer)) > 0) {
            for (ssize_t i = 0; i < got; i++) {
                if (buffer[i] == '\n') {
                    __android_log_write(ANDROID_LOG_INFO, "satrecomp", line.c_str());
                    line.clear();
                } else {
                    line += buffer[i];
                }
            }
        }
    }).detach();
}

} // namespace

void StartOnAndroid() {
    if (const char *folder = getenv("RECOMP_FOLDER"); folder && *folder) {
        if (chdir(folder) != 0) {
            __android_log_print(ANDROID_LOG_WARN, "satrecomp", "no folder %s", folder);
        }
    }
    // (not freopen: the standard output's descriptor is the system's, and fdsan stops a process that closes it)
    const char *log = getenv("RECOMP_LOG");
    const int file = log && *log ? open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (file >= 0) {
        dup2(file, 1);
        dup2(file, 2);
        close(file);
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    } else {
        ToSystemLog();
    }
}

} // namespace port

