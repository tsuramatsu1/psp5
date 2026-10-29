/*
 * psp5 - asking whatever is running as root to let the title list folders.
 *
 * Copyright (C) 2026 the psp5 authors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The protocol, the request names and the crash guard follow SwanStationPS5's
 * src/platform/ps5_unlock.c (Copyright (C) 2026 darkxex) and PS5SX2's
 * ProsperoHenJailbreak.cpp (Copyright (C) 2026 swordpdf), both
 * GPL-3.0-or-later. See PS5Unlock.h for what this does and does not do.
 *
 * C rather than C++ because it runs before the C++ runtime has anything set
 * up that it needs, and because there is nothing here worth a destructor.
 */

#include "PS5Unlock.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int sceKernelUsleep(unsigned microseconds);
int sceKernelDebugOutText(int channel, const char *text);

/* psp5's own trace is not open yet - it needs the paths, which need the
 * listing this is asking for - so these few lines go straight to klog. */
static void note(const char *format, ...)
    __attribute__((format(printf, 1, 2)));
static void
note(const char *format, ...)
{
   char line[256];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line) - 2, format, args);
   va_end(args);
   strcat(line, "\n");
   sceKernelDebugOutText(0, line);
}

/* The names a resident root process watches for. etaHEN, OnionHEN and LegacyJB
 * take the first; OnionHEN also answers the second; the PS5SX2 Helper answers
 * the first for the title ids in its /data/whitelist.txt. */
static const char *const kRequests[] = {
    "/download0/etahen_jailbreak",
    "/download0/onionhen_jailbreak",
};
#define REQUEST_COUNT ((int)(sizeof(kRequests) / sizeof(kRequests[0])))

#define CRASH_MARKER "/download0/psp5_unlock_attempt"

/* Whether psp5 can see the console's own filesystem, which is the whole point
 * of the unlock: the games live under /mnt/usb0/PSP, and a jailed title has no
 * /mnt worth the name.
 *
 * This used to open /data and take that as proof. It is not proof. A jailed
 * title is handed /data anyway - etaHEN and the rest live there, and the unlock
 * request below is written into it - so the check passed whether or not psp5
 * had been let out. psp5 then reported "already unlocked", never asked, and sat
 * in front of an empty shelf on a console whose drives were full. A check that
 * cannot fail is not a check, and this one hid the failure it was meant to
 * catch: the title had worked the run before, so nothing in the log suggested
 * the jail had closed again.
 *
 * /mnt is both the thing wanted and the thing withheld. Outside the jail it
 * holds the drives, the disc and the sandbox folder; inside, it is empty. So
 * the test is emptiness, not whether opendir() succeeds - the directory itself
 * is there either way, which is exactly how this went unnoticed.
 */
int
PS5_FoldersListable(void)
{
   DIR *dir = opendir("/mnt");
   if (!dir) {
      return 0;
   }
   int found = 0;
   const struct dirent *entry;
   while ((entry = readdir(dir)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
         continue;
      }
      found = 1;
      break;
   }
   closedir(dir);
   return found;
}

/* Written whole or not at all: a daemon that reads a half-written body takes
 * the request and does nothing with it, and the request is gone either way. */
static int
publish(const char *path, int pid)
{
   char staged[96];
   snprintf(staged, sizeof(staged), "%s.tmp", path);
   unlink(staged);
   unlink(path);

   const int fd = open(staged, O_WRONLY | O_CREAT | O_EXCL, 0666);
   if (fd < 0) {
      return 0;
   }
   fchmod(fd, 0666);
   char body[32];
   const int length = snprintf(body, sizeof(body), "{\"PID\":%d}\n", pid);
   const int written = write(fd, body, (size_t)length) == length && fsync(fd) == 0;
   close(fd);
   if (!written || rename(staged, path) != 0) {
      unlink(staged);
      return 0;
   }
   return 1;
}

/* etaHEN's legacy command server, for consoles where the file is not watched.
 * The layout is its HijackerCommand. */
struct HijackerCommand {
   int magic; /* 0xDEADBEEF */
   int cmd;   /* 5: jailbreak */
   int pid;
   int ret;   /* -1337 until it answers */
   char msg1[0x500];
   char msg2[0x500];
};

static int
ask_over_tcp(int pid)
{
   const int sock = socket(AF_INET, SOCK_STREAM, 0);
   if (sock < 0) {
      return 0;
   }
   struct timeval timeout = {2, 0};
   setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
   setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

   struct sockaddr_in to;
   memset(&to, 0, sizeof(to));
   to.sin_len = sizeof(to);
   to.sin_family = AF_INET;
   to.sin_port = htons(9028);
   to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   int answered = 0;
   if (connect(sock, (struct sockaddr *)&to, sizeof(to)) == 0) {
      /* Static: two and a half kilobytes is more than this thread's stack
       * wants to carry, and there is only ever one of these in flight. */
      static struct HijackerCommand command;
      memset(&command, 0, sizeof(command));
      command.magic = (int)0xDEADBEEF;
      command.cmd = 5;
      command.pid = pid;
      command.ret = -1337;
      if (send(sock, &command, sizeof(command), 0) == (ssize_t)sizeof(command)) {
         size_t got = 0;
         while (got < sizeof(command)) {
            const ssize_t n = recv(sock, (char *)&command + got, sizeof(command) - got, 0);
            if (n <= 0) {
               break;
            }
            got += (size_t)n;
         }
         answered = got >= 16 && (command.ret == 0 || command.ret == -1337);
      }
   }
   close(sock);
   return answered;
}

static int
wait_listable(int polls)
{
   for (int poll = 0; poll < polls; ++poll) {
      sceKernelUsleep(16667); /* a frame, so the wait is countable in frames */
      if (PS5_FoldersListable()) {
         return 1;
      }
   }
   return 0;
}

PS5UnlockResult
PS5_Unlock(void)
{
   if (PS5_FoldersListable()) {
      note("[psp5] unlock: folders already list");
      return PS5_UNLOCK_ALREADY;
   }

   /* The last launch asked and did not come back. Skip one launch rather than
    * leave a console that cannot start psp5 at all. */
   if (access(CRASH_MARKER, F_OK) == 0) {
      unlink(CRASH_MARKER);
      note("[psp5] unlock: skipped once, the last request closed psp5");
      return PS5_UNLOCK_AFTER_CRASH;
   }
   const int marker = open(CRASH_MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0666);
   if (marker >= 0) {
      fsync(marker);
      close(marker);
   }

   seteuid(geteuid()); /* its own credential, for the daemon to change */

   PS5UnlockResult result = PS5_UNLOCK_NO_ANSWER;
   const int pid = (int)getpid();
   for (int round = 0; round < 3 && result != PS5_UNLOCK_GRANTED; ++round) {
      int published = 0;
      for (int i = 0; i < REQUEST_COUNT; ++i) {
         published += publish(kRequests[i], pid);
      }
      if (published == 0) {
         result = PS5_UNLOCK_CANNOT_ASK;
         break;
      }
      /* About a second and a half for something to notice and act. */
      if (wait_listable(90)) {
         result = PS5_UNLOCK_GRANTED;
      }
      for (int i = 0; i < REQUEST_COUNT; ++i) {
         unlink(kRequests[i]);
      }
      if (result != PS5_UNLOCK_GRANTED && round == 0 && ask_over_tcp(pid) &&
          wait_listable(60)) {
         result = PS5_UNLOCK_GRANTED;
      }
   }

   unlink(CRASH_MARKER); /* psp5 survived the asking, whatever the answer */
   note("[psp5] unlock: %s", PS5_UnlockDescribe(result));
   return result;
}

const char *
PS5_UnlockDescribe(PS5UnlockResult result)
{
   switch (result) {
      case PS5_UNLOCK_ALREADY: return "already unlocked";
      case PS5_UNLOCK_GRANTED: return "granted";
      case PS5_UNLOCK_NO_ANSWER:
         return "nothing answered (etaHEN, OnionHEN, LegacyJB, PS5SX2 Helper)";
      case PS5_UNLOCK_CANNOT_ASK: return "the request could not be written";
      case PS5_UNLOCK_AFTER_CRASH:
         return "skipped once after the last request closed psp5; start again to retry";
   }
   return "?";
}
