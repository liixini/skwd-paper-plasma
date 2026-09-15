#pragma once

#include <QCoreApplication>
#include <QProcess>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <linux/close_range.h>
#include <sys/syscall.h>
#include <unistd.h>

inline bool isolateSkwdProcessDescriptors(int first)
{
    // Keep QProcess's error pipe open until exec, including on older Qt versions.
    if (::syscall(SYS_close_range, first, UINT_MAX, CLOSE_RANGE_CLOEXEC) == 0) return true;
    const int directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    struct Entry {
        uint64_t inode;
        int64_t offset;
        unsigned short size;
        unsigned char type;
        char name[1];
    };
    alignas(Entry) char entries[4096];
    bool ok = true;
    while (ok) {
        long bytes;
        do {
            bytes = ::syscall(SYS_getdents64, directory, entries, sizeof(entries));
        } while (bytes < 0 && errno == EINTR);
        if (bytes <= 0) { ok = bytes == 0; break; }
        for (long offset = 0; offset < bytes;) {
            const auto *entry = reinterpret_cast<const Entry *>(entries + offset);
            offset += entry->size;
            if (entry->name[0] < '0' || entry->name[0] > '9') continue;
            int fd = 0;
            for (const char *digit = entry->name; *digit; ++digit) fd = fd * 10 + (*digit - '0');
            if (fd < first) continue;
            int result;
            do {
                result = ::fcntl(fd, F_SETFD, FD_CLOEXEC);
            } while (result < 0 && errno == EINTR);
            if (result < 0 && errno != EBADF) { ok = false; break; }
        }
    }
    ::close(directory);
    return ok;
}

inline void retireSkwdProcess(QProcess *process)
{
    if (!process) return;
    process->disconnect();
    process->setParent(QCoreApplication::instance());
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
    QObject::connect(process, &QProcess::errorOccurred, process, [process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) process->deleteLater();
    });
    process->kill();
}
