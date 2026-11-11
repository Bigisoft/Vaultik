// Disk images on POSIX: virtual disk files work; physical disks are Windows only for now.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "vaultik/image/disk.hpp"

namespace vaultik::image {

namespace {

[[noreturn]] void windows_only() {
    throw Error("disk images of physical disks are only available on Windows");
}

class PosixRandomFile : public RandomFile {
public:
    explicit PosixRandomFile(int fd) : fd_(fd) {}
    ~PosixRandomFile() override { ::close(fd_); }
    void write_at(std::uint64_t offset, ByteSpan data) override {
        std::size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = ::pwrite(fd_, data.data() + done, data.size() - done, static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;
                throw Error(std::string("writing the disk file: ") + std::strerror(errno));
            }
            done += static_cast<std::size_t>(n);
        }
    }
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override {
        std::size_t done = 0;
        while (done < length) {
            const ssize_t n = ::pread(fd_, dst + done, length - done, static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) throw Error("reading the disk file: cut short");
            done += static_cast<std::size_t>(n);
        }
    }
    std::uint64_t size() override {
        struct stat st{};
        if (::fstat(fd_, &st) != 0) throw Error(std::string("reading the size of the disk file: ") + std::strerror(errno));
        return static_cast<std::uint64_t>(st.st_size);
    }
    void resize(std::uint64_t size) override {
        if (::ftruncate(fd_, static_cast<off_t>(size)) != 0)
            throw Error(std::string("growing the disk file: ") + std::strerror(errno));
    }
    void flush() override { ::fsync(fd_); }

private:
    int fd_;
};

}  // namespace

std::vector<DiskInfo> list_disks() {
    return {};
}

std::vector<int> system_disks() {
    return {};
}

std::vector<std::unique_ptr<DiskSource>> open_disk_sources(const std::vector<int>&, bool,
                                                           const std::function<void(const std::string&)>&) {
    windows_only();
}

std::unique_ptr<DiskTarget> open_disk_target(int) {
    windows_only();
}

std::unique_ptr<RandomFile> create_random_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0) throw Error("creating " + path + ": " + std::strerror(errno));
    return std::make_unique<PosixRandomFile>(fd);
}

std::unique_ptr<RandomFile> open_random_file(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw Error("opening " + path + ": " + std::strerror(errno));
    return std::make_unique<PosixRandomFile>(fd);
}

std::unique_ptr<AttachedDisk> attach_vhdx(const std::string&, bool) {
    windows_only();
}

std::string repair_boot(int) {
    windows_only();
}

}  // namespace vaultik::image
