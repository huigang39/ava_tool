#ifndef MMAP_VECTOR_HPP
#define MMAP_VECTOR_HPP

#include "app_log.hpp"
#include "module.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

// Global counter shared across ALL MmapVector<T> instantiations.
// Must NOT live inside the template — otherwise each T gets its own
// counter starting from 0, causing filename collisions (ERROR_SHARING_VIOLATION).
inline u64
mmapVectorNextId()
{
        static std::atomic<u64> counter{0};
        return counter.fetch_add(1);
}

// User-configurable base directory for the disk cache. Empty => system temp.
// Set once at startup (before heavy sampling) via mmapVectorSetCacheDir().
inline std::string &
mmapVectorCacheDirRaw()
{
        static std::string dir; // empty => system temp
        return dir;
}
inline void
mmapVectorSetCacheDir(const std::string &d)
{
        mmapVectorCacheDirRaw() = d;
}
inline std::string
mmapVectorCacheDir()
{
        return mmapVectorCacheDirRaw();
}

// Resolve (and create) the directory that holds the ".tmp" cache files.
inline std::string
mmapVectorCacheRoot()
{
        std::string base = mmapVectorCacheDirRaw();
#ifdef _WIN32
        if (base.empty()) {
                char tempPath[MAX_PATH];
                GetTempPathA(MAX_PATH, tempPath);
                base = tempPath;
        }
        if (!base.empty() && base.back() != '\\' && base.back() != '/')
                base += "\\";
        std::string dir = base + "ava_tool_mmap_cache";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir;
#else
        if (base.empty())
                base = "/tmp";
        if (!base.empty() && base.back() != '/')
                base += "/";
        std::string dir = base + "ava_tool_mmap_cache";
        mkdir(dir.c_str(), 0777);
        return dir;
#endif
}

// Clean up stale mmap files from previous runs (e.g. after a crash).
// Called once on first MmapVector construction.
inline void
mmapVectorCleanupStale()
{
        static bool done = false;
        if (done)
                return;
        done = true;
#ifdef _WIN32
        std::string      dirPath = mmapVectorCacheRoot();
        std::string      pattern = dirPath + "\\ava_tool_mmap_*.tmp";
        WIN32_FIND_DATAA fd;
        HANDLE           hFind = FindFirstFileA(pattern.c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
                do {
                        std::string fullPath = dirPath + "\\" + fd.cFileName;
                        DeleteFileA(fullPath.c_str()); // silently fails if still locked
                } while (FindNextFileA(hFind, &fd));
                FindClose(hFind);
        }
#endif
}

template <typename T> class MmapVector
{
      private:
        usize       capacity_{0};
        usize       head_{0};
        usize       tail_{0};
        T          *data_{nullptr};
        std::string path_;
#ifdef _WIN32
        HANDLE hFile_{INVALID_HANDLE_VALUE};
        HANDLE hMap_{NULL};
#else
        int fd_{-1};
#endif

        static std::string generateTempPath()
        {
                // Built from std::string (not a fixed buffer) so a long, user-chosen
                // cache directory can't truncate the path.
                const std::string dir = mmapVectorCacheRoot();
#ifdef _WIN32
                return dir + "\\ava_tool_mmap_" + std::to_string(GetCurrentProcessId()) + "_" +
                       std::to_string(mmapVectorNextId()) + ".tmp";
#else
                return dir + "/ava_tool_mmap_" + std::to_string(getpid()) + "_" + std::to_string(mmapVectorNextId()) + ".tmp";
#endif
        }

        void unmap()
        {
#ifdef _WIN32
                if (data_) {
                        UnmapViewOfFile(data_);
                        data_ = nullptr;
                }
                if (hMap_) {
                        CloseHandle(hMap_);
                        hMap_ = NULL;
                }
#else
                if (data_ && capacity_ > 0) {
                        munmap(data_, capacity_ * sizeof(T));
                        data_ = nullptr;
                }
#endif
        }

        void closeFile()
        {
                unmap();
#ifdef _WIN32
                if (hFile_ != INVALID_HANDLE_VALUE) {
                        CloseHandle(hFile_);
                        hFile_ = INVALID_HANDLE_VALUE;
                }
                if (!path_.empty()) {
                        DeleteFileA(path_.c_str());
                }
#else
                if (fd_ != -1) {
                        close(fd_);
                        fd_ = -1;
                }
                if (!path_.empty()) {
                        unlink(path_.c_str());
                }
#endif
        }

        // Shift live elements [head_, tail_) down to the front so head_ becomes 0.
        // Reclaims the dead "head space" left by pop_front without freeing the
        // backing file — that's what makes the underlying file size stable.
        void compact_()
        {
                if (head_ == 0 || !data_)
                        return;
                const usize n = tail_ - head_;
                if (n > 0)
                        memmove(data_, data_ + head_, n * sizeof(T));
                tail_ = n;
                head_ = 0;
        }

        // Poison the buffer: live data is unrecoverable, but every accessor stays
        // safe (size()==0, empty()==true, operator[]/front/back return a sentinel).
        // Used as graceful degradation when an OS call inside grow() fails.
        void poison_()
        {
                head_     = 0;
                tail_     = 0;
                capacity_ = 0;
                data_     = nullptr;
        }

        void grow(usize newCapacity)
        {
                if (newCapacity <= capacity_)
                        return;
                usize bytes = newCapacity * sizeof(T);
                unmap();

#ifdef _WIN32
                if (hFile_ == INVALID_HANDLE_VALUE) {
                        hFile_ = CreateFileA(path_.c_str(),
                                             GENERIC_READ | GENERIC_WRITE,
                                             0,
                                             NULL,
                                             CREATE_ALWAYS,
                                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                             NULL);
                        if (hFile_ == INVALID_HANDLE_VALUE) {
                                LOG_E("[MmapVector] CreateFile failed: %lu (path=%s)", GetLastError(), path_.c_str());
                                log_flush(&g_log_ops);
                                poison_();
                                return;
                        }
                }

                LARGE_INTEGER li;
                li.QuadPart = bytes;
                if (!SetFilePointerEx(hFile_, li, NULL, FILE_BEGIN) || !SetEndOfFile(hFile_)) {
                        LOG_E("[MmapVector] SetEndOfFile(%zu) failed: %lu", bytes, GetLastError());
                        log_flush(&g_log_ops);
                        poison_();
                        return;
                }

                hMap_ =
                    CreateFileMappingA(hFile_, NULL, PAGE_READWRITE, (DWORD)(bytes >> 32), (DWORD)(bytes & 0xFFFFFFFF), NULL);
                if (hMap_ == NULL) {
                        LOG_E("[MmapVector] CreateFileMapping(%zu) failed: %lu", bytes, GetLastError());
                        log_flush(&g_log_ops);
                        poison_();
                        return;
                }

                data_ = (T *)MapViewOfFile(hMap_, FILE_MAP_ALL_ACCESS, 0, 0, bytes);
                if (!data_) {
                        LOG_E("[MmapVector] MapViewOfFile(%zu) failed: %lu", bytes, GetLastError());
                        log_flush(&g_log_ops);
                        CloseHandle(hMap_);
                        hMap_ = NULL;
                        poison_();
                        return;
                }
#else
                if (fd_ == -1) {
                        fd_ = open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
                        if (fd_ == -1) {
                                poison_();
                                return;
                        }
                }
                if (ftruncate(fd_, bytes) != 0) {
                        poison_();
                        return;
                }
                data_ = (T *)mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
                if (data_ == MAP_FAILED) {
                        data_ = nullptr;
                        poison_();
                        return;
                }
#endif
                capacity_ = newCapacity;
        }

      public:
        MmapVector() : path_(generateTempPath()) { mmapVectorCleanupStale(); }

        ~MmapVector() { closeFile(); }

        // Non-copyable
        MmapVector(const MmapVector &)            = delete;
        MmapVector &operator=(const MmapVector &) = delete;

        // Movable
        MmapVector(MmapVector &&other) noexcept
        {
                capacity_ = other.capacity_;
                head_     = other.head_;
                tail_     = other.tail_;
                data_     = other.data_;
                path_     = std::move(other.path_);
#ifdef _WIN32
                hFile_       = other.hFile_;
                hMap_        = other.hMap_;
                other.hFile_ = INVALID_HANDLE_VALUE;
                other.hMap_  = NULL;
#else
                fd_       = other.fd_;
                other.fd_ = -1;
#endif
                other.data_     = nullptr;
                other.head_     = 0;
                other.tail_     = 0;
                other.capacity_ = 0;
                other.path_     = "";
        }

        MmapVector &operator=(MmapVector &&other) noexcept
        {
                if (this != &other) {
                        closeFile();
                        capacity_ = other.capacity_;
                        head_     = other.head_;
                        tail_     = other.tail_;
                        data_     = other.data_;
                        path_     = std::move(other.path_);
#ifdef _WIN32
                        hFile_       = other.hFile_;
                        hMap_        = other.hMap_;
                        other.hFile_ = INVALID_HANDLE_VALUE;
                        other.hMap_  = NULL;
#else
                        fd_       = other.fd_;
                        other.fd_ = -1;
#endif
                        other.data_     = nullptr;
                        other.head_     = 0;
                        other.tail_     = 0;
                        other.capacity_ = 0;
                        other.path_     = "";
                }
                return *this;
        }

        void push_back(const T &val)
        {
                if (tail_ >= capacity_) {
                        // Reclaim dead head space first — for a steady-state ring this
                        // usually keeps tail_ bounded and avoids ever calling grow() again.
                        if (head_ > 0)
                                compact_();
                        if (tail_ >= capacity_) {
                                usize next = capacity_ == 0 ? 4096 : (capacity_ * 2);
                                grow(next);
                        }
                }
                if (data_) {
                        data_[tail_++] = val;
                }
        }

        void pop_front()
        {
                if (head_ < tail_) {
                        ++head_;
                        // Amortized-O(1) compaction: when half the buffer is dead space
                        // we memmove it down. Without this, tail_ grows unboundedly even
                        // though size() stays bounded — eventually filling the disk.
                        if (capacity_ > 0 && head_ >= capacity_ / 2)
                                compact_();
                }
        }

        void clear()
        {
                head_ = 0;
                tail_ = 0;
        }

        usize size() const { return tail_ - head_; }
        bool  empty() const { return head_ == tail_; }

        // Accessors return a zero-initialized sentinel when the backing store has
        // been poisoned (grow() failed). Callers that already check empty() never
        // hit this path; the guard exists to keep the process alive instead of
        // segfaulting on a stale read after an OS allocation failure.
        const T &operator[](usize i) const
        {
                static const T sentinel{};
                if (!data_)
                        return sentinel;
                return data_[head_ + i];
        }
        const T &front() const
        {
                static const T sentinel{};
                if (!data_)
                        return sentinel;
                return data_[head_];
        }
        const T &back() const
        {
                static const T sentinel{};
                if (!data_ || tail_ == 0)
                        return sentinel;
                return data_[tail_ - 1];
        }

        // Iterators
        const T *begin() const { return data_ ? data_ + head_ : nullptr; }
        const T *end() const { return data_ ? data_ + tail_ : nullptr; }
};

#endif // MMAP_VECTOR_HPP
