#include "skwdvideoitem.h"

#include <QGuiApplication>
#include <QDir>
#include <QSocketNotifier>
#include <QtEndian>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

class WidgetSnapshotTest {
public:
    static std::array<uchar, 32> packet(quint32 count = 1, quint64 revision = 1)
    {
        std::array<uchar, 32> bytes {};
        std::memcpy(bytes.data(), "SKDG", 4);
        bytes[4] = 9;
        qToLittleEndian(quint32(1), bytes.data() + 8);
        qToLittleEndian(count, bytes.data() + 12);
        qToLittleEndian(quint32(16), bytes.data() + 16);
        qToLittleEndian(quint32(16), bytes.data() + 20);
        qToLittleEndian(revision, bytes.data() + 24);
        return bytes;
    }

    static int backing(quint32 x = 1, quint32 width = 2, quint64 offset = 24,
                       int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL,
                       size_t fileSize = 40)
    {
        const int fd = ::memfd_create("skwd-widget-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (fd < 0) return -1;
        std::array<uchar, 40> bytes {};
        qToLittleEndian(x, bytes.data());
        qToLittleEndian(quint32(1), bytes.data() + 4);
        qToLittleEndian(width, bytes.data() + 8);
        qToLittleEndian(quint32(2), bytes.data() + 12);
        qToLittleEndian(offset, bytes.data() + 16);
        for (size_t index = 24; index < bytes.size(); index += 4) {
            std::memcpy(bytes.data() + index, "\x40\x00\x00\x80", 4);
        }
        if (::write(fd, bytes.data(), bytes.size()) != ssize_t(bytes.size())
            || ::ftruncate(fd, fileSize) != 0
            || ::fcntl(fd, F_ADD_SEALS, seals) < 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    }

    static bool accepted(SkwdVideoItem &item, const std::array<uchar, 32> &bytes, int &fd)
    {
        item.m_targetSize = QSize(16, 16);
        item.m_slots[bytes[5]].width = 16;
        return item.receiveWidgets(bytes.data(), fd);
    }

    static bool valid()
    {
        SkwdVideoItem item;
        auto bytes = packet();
        int fd = backing();
        if (fd < 0 || !accepted(item, bytes, fd) || fd != -1) return false;
        const auto &snapshot = item.m_stagedWidgets[0];
        if (!snapshot || !item.m_widgetStaged[0] || snapshot->layers.size() != 1
            || snapshot->length != 40 || snapshot->revision != 1
            || snapshot->layers[0].rect != QRectF(1, 1, 2, 2)
            || std::memcmp(static_cast<const uchar *>(snapshot->pixels) + 24,
                           "\x40\x00\x00\x80", 4) != 0) {
            return false;
        }
        int duplicate = backing();
        const bool rejected = duplicate >= 0 && !accepted(item, bytes, duplicate);
        if (duplicate >= 0) ::close(duplicate);
        return rejected;
    }

    static bool empty()
    {
        SkwdVideoItem item;
        auto bytes = packet(0);
        int fd = -1;
        if (!accepted(item, bytes, fd) || !item.m_stagedWidgets[0]
            || !item.m_stagedWidgets[0]->layers.empty()) return false;
        SkwdVideoItem other;
        fd = backing();
        const bool rejected = fd >= 0 && !accepted(other, bytes, fd);
        if (fd >= 0) ::close(fd);
        return rejected;
    }

    static bool rejectsBadBacking()
    {
        auto bytes = packet();
        for (int variant = 0; variant < 6; ++variant) {
            SkwdVideoItem item;
            int fd = -1;
            if (variant == 0) fd = backing(1, 2, 24, F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL);
            if (variant == 1) fd = backing(15);
            if (variant == 2) fd = backing(1, 2, 25);
            if (variant == 3) fd = backing(1, 3);
            if (variant == 4) fd = backing(1, 2, 24,
                F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL, 16 * 1024 * 1024 + 1);
            if (variant == 5) fd = backing(1, 2, 24,
                F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL, 39);
            if (fd < 0) return false;
            const bool rejected = !accepted(item, bytes, fd);
            ::close(fd);
            if (!rejected) return false;
        }
        return true;
    }

    static bool rejectsBadPacket()
    {
        for (int variant = 0; variant < 6; ++variant) {
            SkwdVideoItem item;
            auto bytes = packet();
            int fd = backing();
            if (fd < 0) return false;
            if (variant == 0) qToLittleEndian(quint32(2), bytes.data() + 8);
            if (variant == 1) qToLittleEndian(quint32(17), bytes.data() + 12);
            if (variant == 2) qToLittleEndian(quint32(15), bytes.data() + 16);
            if (variant == 3) qToLittleEndian(quint64(0), bytes.data() + 24);
            if (variant == 4) item.m_outstandingSlots[0] = true;
            if (variant == 5) item.m_latestWidgetRevision = 2;
            const bool rejected = !accepted(item, bytes, fd);
            ::close(fd);
            if (!rejected) return false;
        }
        return true;
    }

    static bool slotTransport()
    {
        SkwdVideoItem item;
        item.setStreamWidth(16);
        item.setStreamHeight(16);
        const int child = item.openStream();
        if (child < 0) return false;
        item.m_slots[0].width = 16;
        auto bytes = packet();
        int fd = backing();
        if (fd < 0) return false;
        std::array<uchar, CMSG_SPACE(sizeof(int))> control {};
        iovec iov {bytes.data(), bytes.size()};
        msghdr message {};
        message.msg_iov = &iov;
        message.msg_iovlen = 1;
        message.msg_control = control.data();
        message.msg_controllen = control.size();
        auto *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
        if (::sendmsg(child, &message, MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        ::close(fd);
        item.consumeDmabuf();
        if (!item.m_widgetStaged[0] || !item.m_stagedWidgets[0]) return false;
        bytes = packet();
        bytes[4] = 2;
        if (::send(child, bytes.data(), bytes.size(), MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        item.consumeDmabuf();
        auto first = item.m_widgetSlots[0];
        if (!first || item.m_widgetStaged[0] || item.m_pendingSlots.size() != 1) return false;
        item.m_pendingSlots.clear();
        item.acknowledge(0);
        if (::send(child, bytes.data(), bytes.size(), MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        item.consumeDmabuf();
        if (item.m_widgetSlots[0] != first || item.m_pendingSlots.size() != 1) return false;
        item.m_slots[1].width = 16;
        bytes = packet(0, 2);
        bytes[5] = 1;
        if (::send(child, bytes.data(), bytes.size(), MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        item.consumeDmabuf();
        if (!item.m_widgetStaged[1] || item.m_widgetSlots[0] != first) return false;
        item.m_pendingSlots.clear();
        item.acknowledge(0);
        if (item.m_widgetSlots[0] || !first->pixels) return false;
        const int oldFd = first->fd;
        first.reset();
        if (::fcntl(oldFd, F_GETFD) != -1 || errno != EBADF) return false;
        bytes[4] = 2;
        if (::send(child, bytes.data(), bytes.size(), MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        item.consumeDmabuf();
        if (!item.m_widgetSlots[1] || item.m_widgetSlots[1]->revision != 2
            || !item.m_widgetSlots[1]->layers.empty()) return false;
        bytes[4] = 7;
        qToLittleEndian(quint16(1), bytes.data() + 6);
        if (::send(child, bytes.data(), bytes.size(), MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        item.consumeDmabuf();
        if (item.m_epoch != 1 || item.m_widgetSlots[0] || item.m_latestWidgetRevision != 0) return false;
        const int before = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        bytes = packet();
        fd = backing();
        if (fd < 0) return false;
        std::memcpy(CMSG_DATA(header), &fd, sizeof(fd));
        if (::sendmsg(child, &message, MSG_NOSIGNAL) != ssize_t(bytes.size())) return false;
        ::close(fd);
        item.consumeDmabuf();
        const int after = QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        ::close(child);
        return !item.m_widgetSlots[0] && !item.m_stagedWidgets[0] && before == after;
    }

    static bool malformedTransport()
    {
        for (int variant = 0; variant < 4; ++variant) {
            SkwdVideoItem item;
            item.setStreamWidth(16);
            item.setStreamHeight(16);
            const int child = item.openStream();
            if (child < 0) return false;
            item.m_slots[0].width = 16;
            auto bytes = packet();
            if (variant == 0) bytes[5] = 9;
            std::array<int, 2> descriptors {backing(), backing()};
            if (descriptors[0] < 0 || descriptors[1] < 0) return false;
            std::array<uchar, CMSG_SPACE(sizeof(int) * 2)> control {};
            iovec iov {bytes.data(), variant == 1 ? bytes.size() - 1 : bytes.size()};
            msghdr message {};
            message.msg_iov = &iov;
            message.msg_iovlen = 1;
            if (variant != 2) {
                message.msg_control = control.data();
                message.msg_controllen = control.size();
                auto *header = CMSG_FIRSTHDR(&message);
                header->cmsg_level = SOL_SOCKET;
                header->cmsg_type = SCM_RIGHTS;
                header->cmsg_len = CMSG_LEN(sizeof(int) * (variant == 3 ? 2 : 1));
                std::memcpy(CMSG_DATA(header), descriptors.data(), sizeof(int) * (variant == 3 ? 2 : 1));
            }
            const bool sent = ::sendmsg(child, &message, MSG_NOSIGNAL) == ssize_t(iov.iov_len);
            for (int fd : descriptors) ::close(fd);
            if (!sent) return false;
            item.consumeDmabuf();
            ::close(child);
            if (item.m_error.isEmpty() || item.m_stagedWidgets[0]
                || item.m_socketNotifier->isEnabled()) return false;
        }
        return true;
    }
};

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    if (!WidgetSnapshotTest::valid()) return 1;
    if (!WidgetSnapshotTest::empty()) return 2;
    if (!WidgetSnapshotTest::rejectsBadBacking()) return 3;
    if (!WidgetSnapshotTest::rejectsBadPacket()) return 4;
    if (!WidgetSnapshotTest::slotTransport()) return 5;
    if (!WidgetSnapshotTest::malformedTransport()) return 6;
    return 0;
}
