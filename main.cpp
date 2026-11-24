//
//  main.cpp
//  WheelSupportTools
//
//  Created by Annop Prapasapong on 27/11/2012
//  Usability edits Edwin 7/12/2012
//  Copyright (c) 2012 Feral Interactive. All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include "WheelSupports.h"
#include "FFBIPC.h"

extern char **environ;

// Reconnect only Cristian's Magic Trackpad. The keyboard and other Bluetooth
// devices are deliberately left untouched.
static constexpr const char *kMagicTrackpadAddress = "00-81-2a-f1-49-e8";

static bool RunBlueutil(const char *path, const char *operation)
{
    pid_t child = 0;
    char *const arguments[] = {
        const_cast<char *>(path),
        const_cast<char *>(operation),
        const_cast<char *>(kMagicTrackpadAddress),
        nullptr
    };

    const int spawnResult = posix_spawn(
        &child, path, nullptr, nullptr, arguments, environ);
    if (spawnResult != 0) {
        fprintf(stderr, "Warning: could not start blueutil (%d).\n", spawnResult);
        return false;
    }

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);

    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void ReconnectMagicTrackpad()
{
    // Homebrew's standard Apple Silicon and Intel installation locations.
    const char *candidates[] = {
        "/opt/homebrew/bin/blueutil",
        "/usr/local/bin/blueutil"
    };
    const char *blueutilPath = nullptr;

    for (const char *candidate : candidates) {
        if (access(candidate, X_OK) == 0) {
            blueutilPath = candidate;
            break;
        }
    }

    if (!blueutilPath) {
        fprintf(stderr,
                "Notice: blueutil not found; skipping Magic Trackpad reconnection.\n");
        return;
    }

    printf("Reconnecting the configured Magic Trackpad only...\n");
    if (!RunBlueutil(blueutilPath, "--disconnect")) {
        fprintf(stderr,
                "Warning: Magic Trackpad disconnect failed; reconnect was not attempted.\n");
        return;
    }

    sleep(1);
    if (RunBlueutil(blueutilPath, "--connect")) {
        printf("Magic Trackpad reconnected; other Bluetooth devices were not changed.\n");
    } else {
        fprintf(stderr,
                "Warning: Magic Trackpad reconnect failed. You can reconnect it from Bluetooth settings.\n");
    }
}

int main(int argc, const char * argv[])
{
    printf("================================================================================\n");
    printf("=                               Free The Wheel (FTW)                           =\n");
    printf("=                             © Feral Interactive 2012                         =\n");
    printf("=    This program comes with ABSOLUTELY NO WARRANTY; This is free software,    =\n");
    printf("=            You are welcome to redistribute it under certain conditions,      =\n");
    printf("=                            Released under GNU License                        =\n");
    printf("================================================================================\n");

    DeviceMode configMode = DeviceModeFull;
    if (argc > 1)
    {
        if (strcmp(argv[1], "--info") == 0)
        {
            configMode = DeviceModeInfoOnly;
        }
        else if (strcmp(argv[1], "--restore") == 0)
        {
            configMode = DeviceModeStandard;
        }
    }
    else
    {
        printf("=   --info       - display list of supported devices.                          =\n");
        printf("=   --restore    - Restore your wheel to restricted (default) mode.            =\n");
        printf("================================================================================\n");
    }

    switch (configMode) {
        case DeviceModeFull:
            printf("Looking for supported wheels to enable in NATIVE mode. . .\n\n");
            break;
        case DeviceModeStandard:
            printf("Returning wheels to RESTRICTED (Default) mode  . . .\n\n");
            break;
        case DeviceModeInfoOnly:
            printf("Displaying list of supported devices:\n\n");
            break;
    }

    ConfigAllDevices(configMode);
    printf("\nDone.\n");

    // Run only after the native DFGT has been detected and retained. This
    // does not alter FreeTheWheel's HID configuration or input handling.
    if (configMode == DeviceModeFull && IsNativeDFGTAvailable()) {
        ReconnectMagicTrackpad();
    }

    if (configMode == DeviceModeFull && !StartFFBIPCServer()) {
        fprintf(stderr, "Warning: FFB IPC server could not be started.\n");
    }

    printf("FreeTheWheel is running. Press ENTER to exit.\n");
    getchar();
    return 0;
}
