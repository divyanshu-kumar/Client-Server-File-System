#define FUSE_USE_VERSION 30

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <experimental/filesystem>
#include <signal.h>
namespace fs = std::experimental::filesystem;
#include <iostream>
#include <mutex>
#include <thread>

#include "AfsClient.h"

enum DebugLevel { LevelInfo = 0, LevelError = 1, LevelNone = 2 };

const DebugLevel debugMode = LevelNone;

const unsigned long parallel_close_file_size_thresh = 
    167772160;  // should be set in bytes, currently 16 Megabytes
const bool enableTempFileWrites =
    true;  // whether to enable creation of temporary files while writing
const bool shouldClearCacheOnExit = 
    false;

static struct options {
    AfsClient *afsclient;
    int show_help;
} options;

void closeOnServer(const char *path);
void consumer();

struct BoundedBuffer {
    string *buffer;
    int capacity;

    int front;
    int rear;
    int count;

    std::mutex lock;

    std::condition_variable not_full;
    std::condition_variable not_empty;

    bool notDone;

    BoundedBuffer(int capacity);

    ~BoundedBuffer();

    void deposit(string path);

    string fetch();

    void submitRequest(string path);

    void consumer();

    void cleanupBuffer() {
        notDone = false;
        not_empty.notify_one();
    }
};

thread *close_thread;
BoundedBuffer *closeBuffer;

inline void get_time(struct timespec *ts);
inline double get_time_diff(struct timespec *before, struct timespec *after);
void printFileTimeFields(const char *func, int fd);
void printFileTimeFields(const char *func, const char *path);
int cp(const char *to, const char *from);

class Cache {
    string cachedRoot;
    unordered_map<int, std::string> tempFdToPathMap;

   public:
    Cache(string currentWorkDir, string cachedFolderName);

    void makeCacheFolder();

    string getCachedPath(const char *path, bool tempPath = false, int fd = -1);

    void correctStaleness(const char *path, struct stat *buffer);

    bool isCached(const char *path);

    void mirrorDirectoryStructure(const char *path);

    void fetchFile(const char *path);

    void cacheFile(const char *path);

    string createRecoveryPath(int fd);

    string getRecoveryCachedPath(int fd);

    void recurseDirectoryTraversal(string path);

    void setTimeFileNameWithFd(int fd, string &path) {
        tempFdToPathMap[fd] = path;
    }

    bool isTempFile(int fd) {
        return tempFdToPathMap.find(fd) != tempFdToPathMap.end();
    }

    void clearTempFile(int fd) { tempFdToPathMap.erase(fd); }
};

Cache *cache;
int crashSite = 0;

#define OPTION(t, p) \
    { t, offsetof(struct options, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("-h", show_help), OPTION("--help", show_help), FUSE_OPT_END};

static void show_help(const char *progname) {
    printf("%s \n", __func__);
    std::cout
        << "usage: " << progname
        << " [-s -d] <mountpoint> [--server=ip:port, Default = localhost]\n\n";
}

static void *client_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \n", __func__);
    }
    closeBuffer = new BoundedBuffer(100);
    close_thread = new thread(&BoundedBuffer::consumer, closeBuffer);
    (void)conn;
    cache->recurseDirectoryTraversal(cache->getCachedPath(""));
    return NULL;
}

static void client_destroy(void *privateData) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \n", __func__);
    }
    closeBuffer->cleanupBuffer();
    close_thread->join();
    if (shouldClearCacheOnExit) {
        string command = "rm -rf " + cache->getCachedPath("");
        int res = system(command.c_str());
        if (res == 0) {
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t: Successfully cleared cache!\n", __func__);
            }
        } else {
            if (debugMode <= DebugLevel::LevelError) {
                printf("%s \t: Not able to clear cache!\n", __func__);
                printf("%s \t : \n", __func__);
                perror(strerror(errno));
            }
        }
    }
    delete cache;
}

static int client_getattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }

    memset(stbuf, 0, sizeof(struct stat));

    int res = 0;

    if (fi != NULL) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Local with fd = %lu, file = %s\n", __func__, fi->fh,
                   path);
        }
        res = fstat(fi->fh, stbuf);
    } else {
        if (cache->isCached(path)) {
            res = lstat(cache->getCachedPath(path).c_str(), stbuf);
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t: Request handled locally = %s\n", __func__, path);
            }
        } else {
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t: Server with path = %s\n", __func__, path);
            }
            return options.afsclient->rpc_getattr(path, stbuf);
        }
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
        return -errno;
    }

    return 0;
}

static int client_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s \n", __func__, path);
    }

    int res = options.afsclient->rpc_readdir(path, buf, filler);

    return res;
}

static int client_open(const char *path, struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }
    std::string s_path(cache->getCachedPath(path));

    if (cache->isCached(path) == false) {
        cache->cacheFile(path);
    }

    unsigned long fd = -1;

    if (enableTempFileWrites) {
        string tempFileName = cache->getCachedPath(path, true, -1);
        int res = cp(tempFileName.c_str(),
                     s_path.c_str());

        if (res != 0) {
            if (debugMode <= DebugLevel::LevelError) {
                printf("%s \t : Failed to copy file %s! Trying again..\n", __func__, path);
                string copyCommand = "cp " + s_path + " " + tempFileName;
                res = system(copyCommand.c_str());
                if (res == -1) {
                    printf("%s \t : Failed to copy file %s even after retrying :(\n", __func__, path);
                }
            }
        }

        struct stat st_buf;
        if (lstat(s_path.c_str(), &st_buf) != 0) {
            if (debugMode <= DebugLevel::LevelError) {
                printf("%s \t: Failed to stat local file = %s\n", __func__,
                       path);
            }
            return 0;
        }

        struct timespec ts[2];
        ts[0].tv_sec = st_buf.st_atim.tv_sec;
        ts[0].tv_nsec = st_buf.st_atim.tv_nsec;
        ts[1].tv_sec = st_buf.st_mtim.tv_sec;
        ts[1].tv_nsec = st_buf.st_mtim.tv_nsec;

        res =
            utimensat(AT_FDCWD, tempFileName.c_str(), ts, AT_SYMLINK_NOFOLLOW);

        fd = open(tempFileName.c_str(), fi->flags);

        cache->setTimeFileNameWithFd(fd, tempFileName);
    } else {
        fd = open(s_path.c_str(), fi->flags);
    }

    if (fd == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Failed to open File. Path = %s\n", __func__, path);
        }
    }

    else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: File openend successfully. Fd = %lu\n", __func__,
                   fd);
        }
    }

    fi->fh = fd;

    return 0;
}

static int client_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {
    int fd = -1;
    if (fi) {
        fd = fi->fh;
    } else {
        fd = open(cache->getCachedPath(path).c_str(), O_RDONLY);
    }
    if (fd == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: File opened failed in read, file = %s\n", __func__,
                   cache->getCachedPath(path).c_str());
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
        return -1;
    }
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: File = %s, fd = %d \n", __func__, path, fd);
    }
    int res = pread(fd, buf, size, offset);
    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
        return -1;
    }

    return res;
}

static int client_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
    int fd = -1;
    if (debugMode <= DebugLevel::LevelInfo) {
        printFileTimeFields(__func__, fi->fh);
    }
    if (fi) {
        fd = fi->fh;
    } else {
        fd = open(cache->getCachedPath(path).c_str(), O_RDWR | O_APPEND);
    }
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: File to write = %s, fd = %d\n", __func__, path, fd);
    }
    if (fd == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Filed opened failed in write, file = %s\n", __func__,
                   cache->getCachedPath(path).c_str());
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
        return -1;
    }
    
    if (debugMode <= DebugLevel::LevelInfo) {
        printFileTimeFields(__func__, fd);
    }
    
    int res = pwrite(fd, buf, size, offset);

    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Finished pwrite, wrote %d bytes, fd = %d \n", __func__, res, fd);
        printFileTimeFields(__func__, fd);
    }

    bool shouldFlush = (rand() % 100) >= 90;
    if (shouldFlush) {
        fdatasync(fd);
    }
    
    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : Failed to write to %s : \n", __func__, path);
            perror(strerror(errno));
        }
        return -1;
    }
    return res;
}

static int client_mkdir(const char *path, mode_t mode) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s \n", __func__, path);
    }

    int res = options.afsclient->rpc_mkdir(path, mode);

    if (res == 0) {
        res = mkdir(cache->getCachedPath(path).c_str(), mode);
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : Error making %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    return res;
}

static int client_rmdir(const char *path) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t : Path = %s\n", __func__, path);
    }
    int res = options.afsclient->rpc_rmdir(path);

    if (res == 0) {
        res = rmdir(cache->getCachedPath(path).c_str());
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : Error removing dir %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    return res;
}

static int client_create(const char *path, mode_t mode,
                         struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }
    int res = 0;

    res = options.afsclient->rpc_create(path, mode, fi);

    int fd = -1;

    if (res == 0) {
        cache->mirrorDirectoryStructure(path);
        fd = open(cache->getCachedPath(path).c_str(), fi->flags, mode);
        {  // Sync access and modified time of server with local create
            struct stat remoteFileStatBuffer;
            int res =
                options.afsclient->rpc_getattr(path, &remoteFileStatBuffer);
            if (res != -1) {
                struct timespec ts[2];
                ts[0].tv_sec = remoteFileStatBuffer.st_atim.tv_sec;
                ts[0].tv_nsec = remoteFileStatBuffer.st_atim.tv_nsec;

                ts[1].tv_sec = remoteFileStatBuffer.st_mtim.tv_sec;
                ts[1].tv_nsec = remoteFileStatBuffer.st_mtim.tv_nsec;

                res = utimensat(AT_FDCWD, (cache->getCachedPath(path)).c_str(),
                                ts, AT_SYMLINK_NOFOLLOW);
            }
        }
        if (fd == -1) {
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t : %s\n", __func__, path);
                perror(strerror(errno));
                printf("%s \t: Trying to open now in read and write mode.\n",
                       __func__);
            }

            fd = open(cache->getCachedPath(path).c_str(),
                      (fi->flags) & ~(O_CREAT));

            if (fd == -1) {
                res = fd;
                if (debugMode <= DebugLevel::LevelError) {
                    printf("%s \t: Again failed to open file = %s.\n", __func__,
                           cache->getCachedPath(path).c_str());
                }
            } else {
                fi->fh = fd;
            }
        } else {
            fi->fh = fd;
        }
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Failed to create file = %s, fd = %d\n", __func__,
                   cache->getCachedPath(path).c_str(), fd);
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
    } else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Created file = %s, fd = %d\n", __func__,
                   cache->getCachedPath(path).c_str(), fd);
            printFileTimeFields(__func__, fi->fh);
        }
    }

    return res;
}

static int client_unlink(const char *path) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: File = %s \n", __func__,
               cache->getCachedPath(path).c_str());
    }

    int res = options.afsclient->rpc_unlink(path);

    if (res == 0) {
        res = unlink(cache->getCachedPath(path).c_str());
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    return res;
}

static int client_rename(const char *from, const char *to, unsigned int flags) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: From = %s, To = %s \n", __func__, from, to);
    }

    int res = options.afsclient->rpc_rename(from, to, flags);

    if (res == 0) {
        res = rename(cache->getCachedPath(from).c_str(),
                     cache->getCachedPath(to).c_str());
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s : Renamed failed at client. From = %s, To = %s\n",
                   __func__, from, to);
        }
    }

    return 0;
}

static int client_utimens(const char *path, const struct timespec ts[2],
                          struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }

    auto is_regular_file = [&](const char *path) -> bool {
        struct stat path_stat;
        stat(path, &path_stat);
        return S_ISREG(path_stat.st_mode);
    };

    int res = 0;
    // For regular file, utimensat should not be called as
    // anyways on close it is changed
    if (!is_regular_file(cache->getCachedPath(path).c_str())) {
        res = options.afsclient->rpc_utimens(path, ts, fi);
    }

    res = utimensat(AT_FDCWD, cache->getCachedPath(path).c_str(), ts,
                    AT_SYMLINK_NOFOLLOW);

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    return res;
}

static int client_mknod(const char *path, mode_t mode, dev_t rdev) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }

    int res = options.afsclient->rpc_mknod(path, mode, rdev);

    if (res == 0) {
        if (S_ISFIFO(mode))
            res = mkfifo(cache->getCachedPath(path).c_str(), mode);
        else
            res = mknod(cache->getCachedPath(path).c_str(), mode, rdev);
    }

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    return res;
}

bool isFileModified(const char *path, struct fuse_file_info *fi) {
    bool isModified = true;
    struct stat server_buf;

    int res = options.afsclient->rpc_getattr(path, &server_buf);

    if (res == 0) {
        struct stat local_buf;

        res = fstat(fi->fh, &local_buf);

        if (res == 0) {
            if (local_buf.st_mtim.tv_nsec == server_buf.st_mtim.tv_nsec) {
                if (debugMode <= DebugLevel::LevelInfo) {
                    printf(
                        "%s \t : No writes done, so no need to send this "
                        "file!\n",
                        __func__);
                }
                isModified = false;
            } else {
                if (debugMode <= DebugLevel::LevelInfo) {
                    printf(
                        "%s \t : Need to send this file as modified time is "
                        "different!\n",
                        __func__);
                }
            }
        } else {
            if (debugMode <= DebugLevel::LevelError) {
                printf("%s \t : GetAttr failed locally\n", __func__);
                printf("%s \t : %s\n", __func__, path);
                perror(strerror(errno));
            }
        }

    } else {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : GetAttr failed on server\n", __func__);
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    return isModified;
}

static int client_flush(const char *path, struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }

    struct timespec ts_close_start, ts_close_end;
    if (debugMode <= DebugLevel::LevelInfo) {
        get_time(&ts_close_start);
    }

    string s_path(cache->getCachedPath(path));
    if (fi == NULL) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s : File handle is NULL!!!!!! File = %s", __func__, path);
        }
        return 0;
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: File = %s, fd = %lu\n", __func__, s_path.c_str(),
               fi->fh);
    }

    if (isFileModified(path, fi)) {
        fdatasync(fi->fh);
    }

    int res = close(dup(fi->fh));

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Failed to flush file with fd = %lu.\n", __func__,
                   fi->fh);
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
        return -1;
    } else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Successfully flushed file with fd = %lu.\n",
                   __func__, fi->fh);
        }
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        get_time(&ts_close_end);
        printf("%s \t : Diff (ms) = %f \n", __func__,
               get_time_diff(&ts_close_start, &ts_close_end));
    }

    return res;
}

unsigned long getFileSize(const char *path) {
    unsigned long size = 0;
    struct stat buf;
    if (lstat(cache->getCachedPath(path).c_str(), &buf) == 0) {
        size = buf.st_size;
    }
    return size;
}

void closeOnServer(const char *path) {
    struct timespec ts_send_start, ts_send_end;
    if (debugMode <= DebugLevel::LevelInfo) {
        get_time(&ts_send_start);
    }

    int res =
        options.afsclient->rpc_putFile(cache->getCachedPath("").c_str(), path);

    if (debugMode <= DebugLevel::LevelInfo) {
        get_time(&ts_send_end);
    }

    if ((res < 0) && (debugMode <= DebugLevel::LevelError)) {
        printf("%s \t: File failed to send to server.\n", __func__);
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        printf(
            "%s : \t Time to send (ms) = %f \t size (bytes) = "
            "%lu, path = %s\n",
            __func__, get_time_diff(&ts_send_start, &ts_send_end),
            getFileSize(path), path);
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Hopefully file is sent to server.\n", __func__);
    }
}

void renameRecoveryFileDuringRelease(string tempFileName, string originalFile) {
    int tempRes = rename(tempFileName.c_str(), originalFile.c_str());        
    if (tempRes != -1) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Renamed from %s to %s.\n", __func__,
                tempFileName.c_str(), originalFile.c_str());
        }
    }
    else {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Failed to rename from %s to %s.\n", __func__,
                tempFileName.c_str(), originalFile.c_str());
            perror(strerror(errno));
        }
    }
}

static int client_release(const char *path, struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }
    string s_path(cache->getCachedPath(path));

    struct timespec ts_close_start, ts_close_end;
    if (debugMode <= DebugLevel::LevelInfo) {
        get_time(&ts_close_start);
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        printf("Called close at %lu s, %lu ns.\n", ts_close_start.tv_sec,
               ts_close_start.tv_nsec);
    }

    if (fi == NULL) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: File handle is NULL! File = %s\n", __func__, path);
        }
        return 0;
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: File to release = %s, fd = %lu\n", __func__,
               s_path.c_str(), fi->fh);
    }

    int res = 0;
    bool needToSend = isFileModified(path, fi);
    struct stat server_buf;

    string recovery_path; 
    if (needToSend) {        
        fdatasync(fi->fh);
        if (enableTempFileWrites && cache->isTempFile(fi -> fh)) {
            if (crashSite == 5) {
                raise(SIGSEGV);
            }
            recovery_path = cache->createRecoveryPath(fi -> fh);
        }        
    }

    int tempFd = -1;
    string tempFileName;
    bool isTempFile = cache->isTempFile(fi->fh);

    if (enableTempFileWrites && isTempFile) {
        tempFd = fi->fh;
        tempFileName = cache->getCachedPath(path, true, tempFd);
    }
    res = close(fi->fh);

    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Failed to release file with fd = %lu.\n", __func__,
                   fi->fh);
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
        return -1;
    } else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf(" %s \t: Successfully released file with fd = %lu.\n",
                   __func__, fi->fh);
        }
    }

    if (needToSend) {
        if (getFileSize(path) > parallel_close_file_size_thresh) {
            if (enableTempFileWrites && isTempFile) {
                renameRecoveryFileDuringRelease(recovery_path, cache->getCachedPath(path));
                cache->clearTempFile(tempFd);
            }
            closeBuffer->submitRequest(path);
        } else {
            if (enableTempFileWrites && isTempFile) {
                std::size_t lastPos = recovery_path.find_last_of("/");
                string originalFile = recovery_path.substr(lastPos, recovery_path.length());
                if (crashSite == 2) {
                    raise(SIGSEGV);
                }
                closeOnServer(originalFile.c_str());
                if (crashSite == 1) {
                    raise(SIGSEGV);
                }
                renameRecoveryFileDuringRelease(recovery_path, cache->getCachedPath(path));
                cache->clearTempFile(tempFd);
            } else {
                closeOnServer(path);
            }         
        }
    }

    if (debugMode <= DebugLevel::LevelInfo) {
        get_time(&ts_close_end);
        printf("%s \t : Diff (ms) = %f \t size (bytes) = %lu \n", __func__,
               get_time_diff(&ts_close_start, &ts_close_end),
               getFileSize(path));
    }

    return 0;
}

static int client_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t : Path = %s , isDataSync = %d\n", __func__, path, isdatasync);
    }
    int res = 0;
    if (isdatasync != 0) {
        res = fdatasync(fi->fh);
    } else {
        res = fsync(fi->fh);
    }
    return res;
}

static struct client_operations : fuse_operations {
    client_operations() {
        init = client_init;
        destroy = client_destroy;
        getattr = client_getattr;
        readdir = client_readdir;
        open = client_open;
        read = client_read;
        write = client_write;
        create = client_create;
        mkdir = client_mkdir;
        rmdir = client_rmdir;
        unlink = client_unlink;
        rename = client_rename;
        utimens = client_utimens;
        mknod = client_mknod;
        flush = client_flush;
        release = client_release;
        fsync = client_fsync;
    }

} client_oper;

string getCurrentWorkingDir() {
    char arg1[20];
    char exepath[PATH_MAX + 1] = {0};

    sprintf(arg1, "/proc/%d/exe", getpid());
    int res = readlink(arg1, exepath, 1024);
    std::string s_path(exepath);
    std::size_t lastPos = s_path.find_last_of("/");
    return s_path.substr(0, lastPos);
}

int main(int argc, char *argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    cout.tie(nullptr);
    srand(time(NULL));
    string cachedFolderName = ".cached";
    string rootDir = getCurrentWorkingDir();
    string clientFolderPath = rootDir + "/client";

    string unmountCommand = "umount -f -l " + clientFolderPath;
    int result = system(unmountCommand.c_str());

    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: %s\n", __func__, argv[0]);
    }

    bool isServerArgPassed = false;
    bool isCrashSiteArgPassed = false;
    std::string server_address = "localhost:50051";
    if (argc > 2) {
        size_t pos = std::string(argv[argc - 2]).rfind("--server=", 0);
        if (pos == 0) {
            isServerArgPassed = true;
            server_address = std::string(argv[argc - 2]).substr(string("--server=").length());
        }

        pos = std::string(argv[argc - 1]).rfind("--crash=", 0);
        if (pos == 0) {
            isCrashSiteArgPassed = true;
            crashSite = stoi(std::string(argv[argc - 1]).substr(string("--crash=").length()));
        }        
    } else if (argc > 1) {
        size_t pos = std::string(argv[argc - 1]).rfind("--server=", 0);
        if (pos == 0) {
            isServerArgPassed = true;
            server_address = std::string(argv[argc - 1])
                                 .substr(string("--server=").length());
        }
    }

    printf("%s \t: Connecting to server at %s...\n", __func__,
           server_address.c_str());

    if (isServerArgPassed) {
        argc -= 1;
    }
    if (isCrashSiteArgPassed) {
        argc -= 1;
    }

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.afsclient = new AfsClient(grpc::CreateChannel(
        server_address.c_str(), grpc::InsecureChannelCredentials()));

    if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) return 1;

    if (options.show_help) {
        show_help(argv[0]);
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0] = (char *)"";
    }

    struct stat buffer;
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: CurrentWorkingDir = %s\n", __func__, rootDir.c_str());
    }
    cache = new Cache(rootDir, cachedFolderName);

    if (stat(clientFolderPath.c_str(), &buffer) == 0) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Folder %s exists.\n", __func__,
                   clientFolderPath.c_str());
        }
    } else {
        int res = mkdir(clientFolderPath.c_str(), 0777);
        if (res == 0) {
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t: Folder %s created successfully!\n", __func__,
                       clientFolderPath.c_str());
            }
        } else {
            if (debugMode <= DebugLevel::LevelError) {
                printf("%s \t: Failed to create folder %s !\n", __func__,
                       clientFolderPath.c_str());
            }
        }
    }

    return fuse_main(argc, argv, &client_oper, &options);
}

void BoundedBuffer::consumer() {
    while (notDone) {
        string path = fetch();
        if (notDone) {
            closeOnServer(path.c_str());
        }
    }
}

void BoundedBuffer::submitRequest(string path) { deposit(path); }

inline void get_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}

inline double get_time_diff(struct timespec *before, struct timespec *after) {
    double delta_s = after->tv_sec - before->tv_sec;
    double delta_ns = after->tv_nsec - before->tv_nsec;

    return (delta_s + (delta_ns * 1e-9)) * ((double)1e3);
}
void printFileTimeFields(const char *func, int fd) {
    struct stat buff;
    struct timespec ts[3];
    memset(&buff, 0, sizeof(struct stat));
    int res = fstat(fd, &buff);
    if (res != 0) {
        return;
    }
    ts[0].tv_sec = buff.st_atim.tv_sec;
    ts[0].tv_nsec = buff.st_atim.tv_nsec;
    ts[1].tv_sec = buff.st_mtim.tv_sec;
    ts[1].tv_nsec = buff.st_mtim.tv_nsec;
    ts[2].tv_sec = buff.st_ctim.tv_sec;
    ts[2].tv_nsec = buff.st_ctim.tv_nsec;
    printf(
        "%s : last modified time = %lu s %lu ns, last accessed time = %lu s "
        "%lu ns, "
        "created time = %lu s %lu ns, res = %d\n",
        func, ts[1].tv_sec, ts[1].tv_nsec, ts[0].tv_sec, ts[0].tv_nsec,
        ts[2].tv_sec, ts[2].tv_nsec, res);
}
void printFileTimeFields(const char *func, const char *path) {
    struct stat buff;
    struct timespec ts[3];
    memset(&buff, 0, sizeof(struct stat));
    int res = lstat(path, &buff);
    if (res != 0) {
        return;
    }
    ts[0].tv_sec = buff.st_atim.tv_sec;
    ts[0].tv_nsec = buff.st_atim.tv_nsec;
    ts[1].tv_sec = buff.st_mtim.tv_sec;
    ts[1].tv_nsec = buff.st_mtim.tv_nsec;
    ts[2].tv_sec = buff.st_ctim.tv_sec;
    ts[2].tv_nsec = buff.st_ctim.tv_nsec;
    printf(
        "%s : last modified time = %lu s %lu ns, last accessed time = %lu s "
        "%lu ns, "
        "created time = %lu s %lu ns, res = %d\n",
        func, ts[1].tv_sec, ts[1].tv_nsec, ts[0].tv_sec, ts[0].tv_nsec,
        ts[2].tv_sec, ts[2].tv_nsec, res);
}

BoundedBuffer::BoundedBuffer(int capacity)
    : capacity(capacity), front(0), rear(0), count(0), notDone(true) {
    buffer = new string[capacity];
    cout << "Shared queue system created." << endl;
}

BoundedBuffer::~BoundedBuffer() {
    notDone = false;
    delete[] buffer;
}

void BoundedBuffer::deposit(string path) {
    std::unique_lock<std::mutex> l(lock);

    not_full.wait(l, [this]() { return count != capacity; });

    buffer[rear] = path;
    rear = (rear + 1) % capacity;
    ++count;

    l.unlock();
    not_empty.notify_one();
}

string BoundedBuffer::fetch() {
    std::unique_lock<std::mutex> l(lock);

    not_empty.wait(l, [this]() { return (count != 0 || notDone == false); });

    if (notDone == false) {
        return "";
    }
    string path = buffer[front];
    front = (front + 1) % capacity;
    --count;

    l.unlock();
    not_full.notify_one();

    return path;
}

Cache::Cache(string currentWorkDir, string cachedFolderName) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Current Working Dir : %s\n", __func__,
               currentWorkDir.c_str());
    }
    cachedRoot = currentWorkDir + "/" + cachedFolderName;
    makeCacheFolder();
}

void Cache::makeCacheFolder() {
    struct stat buffer;
    if (stat(cachedRoot.c_str(), &buffer) == 0) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Cached folder already exists. Path = %s\n", __func__,
                   cachedRoot.c_str());
        }
    } else {
        int status = mkdir(cachedRoot.c_str(), 0777);

        if (status != 0) {
            if (debugMode <= DebugLevel::LevelError) {
                printf("%s \t: Failed to create cached directory!\n", __func__);
            }
        } else {
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t: Successfully created cached directory.\n",
                       __func__);
            }
        }
    }
}

string Cache::getCachedPath(const char *path, bool tempPath, int fd) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Requested for %s , Returned %s \n", __func__, path,
               (cachedRoot + string(path)).c_str());
    }
    if (enableTempFileWrites && tempPath == true) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Requested for %s , Returned %s \n", __func__, path,
                   (cachedRoot + string(path)).c_str());
        }
        string tempFileName;
        if (fd == -1) {
            tempFileName = cachedRoot + string(path) + ".temp." +
                           std::to_string(rand() % 10000);
        } else {
            auto it = tempFdToPathMap.find(fd);
            if (it == tempFdToPathMap.end()) {
                if (debugMode <= DebugLevel::LevelError) {
                    printf(
                        "%s \t: Requested temp path for fd = %d"
                        " but it is not in map.\n",
                        __func__, fd);
                }
            } else {
                tempFileName = it->second;
            }
        }
        return tempFileName;
    }
    return cachedRoot + string(path);
}

void Cache::correctStaleness(const char *path, struct stat *buffer) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Path = %s\n", __func__, path);
    }

    struct stat remoteFileStatBuffer;
    int res = options.afsclient->rpc_getattr(path, &remoteFileStatBuffer);
    if (res == -1) {
        return;
    }

    struct timespec lastModifiedTime;
    lastModifiedTime.tv_sec = remoteFileStatBuffer.st_mtim.tv_sec;
    lastModifiedTime.tv_nsec = remoteFileStatBuffer.st_mtim.tv_nsec;

    if ((buffer->st_mtim.tv_sec < lastModifiedTime.tv_sec) 
        || 
        ((buffer->st_mtim.tv_sec == lastModifiedTime.tv_sec) 
          && 
        (buffer->st_mtim.tv_nsec < lastModifiedTime.tv_nsec)
        )
       ) 
    {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: File is old. Refreshing this file : %s.\n", 
                    __func__, path);
        }
        
        fetchFile(path);
    }
}

bool Cache::isCached(const char *path) {
    std::string s_path(getCachedPath(path));
    struct stat buffer;
    if (lstat(s_path.c_str(), &buffer) != 0) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: File = %s, Cached = false\n", __func__,
                   s_path.c_str());
        }
        return false;
    }
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: File = %s, Cached = true\n", __func__, s_path.c_str());
    }
    if (S_ISDIR(buffer.st_mode)) {
        return true;
    }
    correctStaleness(path, &buffer);
    return true;
}

void Cache::mirrorDirectoryStructure(const char *path) {
    std::string s_path(path);
    std::size_t lastPos = s_path.find_last_of("/");
    struct stat tempStatBuffer;
    if (stat(s_path.substr(0, lastPos).c_str(), &tempStatBuffer) == 0) {
        return;
    }
    std::size_t prevPos = 0, pos = s_path.find('/');
    while (pos != std::string::npos) {
        prevPos = pos;
        pos = s_path.find('/', pos + 1);
        if (pos == std::string::npos) {
            // It's a file, break the loop
            break;
        }
        string folderName = s_path.substr(prevPos + 1, pos - prevPos - 1);

        struct stat buffer;
        string tempPath = cachedRoot + s_path.substr(0, pos);
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Directory in .cached - %s\n", __func__,
                   tempPath.c_str());
        }
        if (stat(tempPath.c_str(), &buffer) != 0) {
            int res = mkdir(tempPath.c_str(), 0777);
            if (res == -1) {
                if (debugMode <= DebugLevel::LevelError) {
                    printf("%s \t: Failed creating new folder!\n", __func__);
                }
            } else {
                if (debugMode <= DebugLevel::LevelInfo) {
                    printf("%s \t: Created folder successfully!\n", __func__);
                }
            }
        } else {
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s \t: Folder already exists!\n", __func__);
            }
        }
    }
}

void Cache::fetchFile(const char *path) {
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s \t: Fetching file %s from server.\n", __func__, path);
    }
    options.afsclient->rpc_getFile(cachedRoot.c_str(), path);

    struct stat buffer;
    if (stat((getCachedPath(path)).c_str(), &buffer) == 0) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Cached file %s successfully\n", __func__,
                   (getCachedPath(path)).c_str());
        }
    } else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Failed to cache file %s\n", __func__,
                   (getCachedPath(path)).c_str());
        }
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t : %s\n", __func__, path);
            perror(strerror(errno));
        }
    }

    struct stat remoteFileStatBuffer;
    int res = options.afsclient->rpc_getattr(path, &remoteFileStatBuffer);
    if (res == -1) {
        return;
    }

    struct timespec ts[2];
    ts[0].tv_sec = remoteFileStatBuffer.st_atim.tv_sec;
    ts[0].tv_nsec = remoteFileStatBuffer.st_atim.tv_nsec;

    ts[1].tv_sec = remoteFileStatBuffer.st_mtim.tv_sec;
    ts[1].tv_nsec = remoteFileStatBuffer.st_mtim.tv_nsec;

    res = utimensat(AT_FDCWD, (getCachedPath(path)).c_str(), ts,
                    AT_SYMLINK_NOFOLLOW);
    if (debugMode <= DebugLevel::LevelInfo) {
        printFileTimeFields(__func__, (getCachedPath(path)).c_str());
    }
    if (res == -1 && debugMode <= DebugLevel::LevelError) {
        printf("%s \t: Failed to set the atime and mtime of %s file.\n",
               __func__, (getCachedPath(path)).c_str());
        printf("%s \t : %s\n", __func__, path);
        perror(strerror(errno));
    }
}

void Cache::cacheFile(const char *path) {
    mirrorDirectoryStructure(path);

    fetchFile(path);
}

string Cache::createRecoveryPath(int fd) {
    string tempPath = getCachedPath("", true, fd);
    string recoveryPath = tempPath + ".recover";
    if (debugMode <= DebugLevel::LevelInfo) {
        printf("%s\t : Recovery Path: %s\n", __func__, recoveryPath.c_str());
    }
    int res = rename(tempPath.c_str(), recoveryPath.c_str());
    
    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Renaming %s to %s Failed\n", __func__, tempPath.c_str(), recoveryPath.c_str());
        }
    } else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Renaming %s to %s Success\n", __func__, tempPath.c_str(), recoveryPath.c_str());
        }
    }
    return recoveryPath;
}

void removePath(string path) {
    string command = "rm " + path;
    int res = system(command.c_str());
    if (res == -1) {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Path: %s Removal Failed\n", __func__, path.c_str());
        }
    } else {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Path: %s Removal Success\n", __func__, path.c_str());
        }
    }
}

string Cache::getRecoveryCachedPath(int fd) {
    auto it = tempFdToPathMap.find(fd);
    string recovery_path;
    if (it != tempFdToPathMap.end()) {
        recovery_path = it->second + ".recover";
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Recovery Path: %s\n", __func__, recovery_path.c_str());
        }
    } else {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Requested temp path for fd = %d"
                    " but it is not in map.\n", __func__, fd);
        }
    }
    return recovery_path;
}

    bool pathExists(string pathType, string path) {
    return path.find(pathType, 0) != string::npos;
}

string translatePath(string recoveryFile) {
    string::size_type loc = recoveryFile.find(".temp", 0);
    return recoveryFile.substr(0, loc);
}

int renameFile(string tempFileName, string originalFile) {
    int tempRes = rename(tempFileName.c_str(), originalFile.c_str());        
    if (tempRes != -1) {
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s \t: Renamed from %s to %s.\n", __func__,
                tempFileName.c_str(), originalFile.c_str());
        }
    }
    else {
        if (debugMode <= DebugLevel::LevelError) {
            printf("%s \t: Failed to rename from %s to %s.\n", __func__,
                tempFileName.c_str(), originalFile.c_str());
            perror(strerror(errno));
            removePath(tempFileName);
        }
    }
}

void Cache::recurseDirectoryTraversal(string path) {
    string recover(".recover");
    string tmp(".temp");
    int crashTextFlag = 0;
    for (auto entry : fs::recursive_directory_iterator(path)) {                          
        string path = entry.path();
        if (debugMode <= DebugLevel::LevelInfo) {
            printf("%s\t : path: %s\n", __func__, path.c_str());         
        }

        // Handling .recover files  
        if (path.find(".recover") != string::npos) {     
            if (crashTextFlag == 0) {
                printf("Crash Detected.... \nRebooting Client\n");
                crashTextFlag = 1;
            }
            string recoveryPath = path;                           
            string originalPath = translatePath(recoveryPath);
            printf("Recovering File %s\n", originalPath.c_str());
            renameFile(recoveryPath, originalPath);

            // Need to put check to send file to server after checking modification time
            std::size_t lastPos = originalPath.find_last_of("/");
            string originalFile = originalPath.substr(lastPos, originalFile.length() - lastPos + 1);
            int res = options.afsclient->rpc_putFile(cache->getCachedPath("").c_str(), originalFile.c_str());
            if ((res < 0) && (debugMode <= DebugLevel::LevelError)) {
                printf("%s \t: File failed to send to server.\n", __func__);
            }
            
            printf("Sending file :%s to Server\n", originalFile.c_str());
            if (debugMode <= DebugLevel::LevelInfo) {
                printf("%s : \t File sent to server\n", __func__);
            }
            printf("File sent successfully\n");
        } 
        // Handling tmp files with no recover files
        else if (path.find(".temp") != string::npos) {
            if (crashTextFlag == 0) {
                printf("Crash Detected.... \nRebooting Client\n");
                crashTextFlag = 1;
            }
            printf("System crashed while writing. Discarding File %s\n", path.c_str());
            removePath(path);
        }                      
    }
}

int cp(const char *to, const char *from) {
    int fd_to, fd_from;
    char buf[131072];
    ssize_t nread;
    int saved_errno;

    fd_from = open(from, O_RDONLY);
    if (fd_from < 0) return -1;

    fd_to = open(to, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd_to < 0) goto out_error;

    while (nread = read(fd_from, buf, sizeof buf), nread > 0) {
        char *out_ptr = buf;
        ssize_t nwritten;

        do {
            nwritten = write(fd_to, out_ptr, nread);

            if (nwritten >= 0) {
                nread -= nwritten;
                out_ptr += nwritten;
            } else if (errno != EINTR) {
                goto out_error;
            }
        } while (nread > 0);
    }

    if (nread == 0) {
        if (close(fd_to) < 0) {
            fd_to = -1;
            goto out_error;
        }
        close(fd_from);

        return 0;
    }

out_error:
    saved_errno = errno;

    close(fd_from);
    if (fd_to >= 0) close(fd_to);

    errno = saved_errno;
    return -1;
}


