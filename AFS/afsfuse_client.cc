#define FUSE_USE_VERSION 30

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "AfsClient.h"

static struct options {
    AfsClient *afsclient;
    int show_help;
} options;

class Cache {
    string cachedRoot;
    unordered_map<int, string> tmpPathMap;

   public:
    Cache(string currentWorkDir, string cachedFolderName) {
        printf("Current Working Dir : %s\n", currentWorkDir.c_str());
        cachedRoot = currentWorkDir + "/" + cachedFolderName;
        makeCacheFolder();
    }

    void makeCacheFolder() {
        struct stat buffer;
        if (stat(cachedRoot.c_str(), &buffer) == 0) {
            printf("%s : Cached folder already exists. Path = %s\n", __func__,
                   cachedRoot.c_str());
        } else {
            int status = mkdir(cachedRoot.c_str(), 0777);

            if (status != 0) {
                printf("Failed to create cached directory!\n");
            } else {
                printf("Successfully created cached directory.\n");
            }
        }
    }

    string getCachedPath(const char *path) {
        printf("%s : Path = %s\n", __func__, path);
        printf("%s : Returned %s \n", __func__, (cachedRoot + "/" + string(path)).c_str());
        return cachedRoot + "/" + string(path);
    }
    
    void setTmpCachedPath(int fd, const char *path) {
        tmpPathMap[fd] = path;
    }

    string getTmpCachedPath(int fd, const char *path) {
        printf("%s : Path = %s\n", __func__, path);
	if (fd < 0) {
	    string tmpPath = cachedRoot + "/" + string(path) + ".tmp" + std::to_string(rand() % 10000);
	    printf("%s : TmpCachePath: %s\n", __func__, tmpPath.c_str());
	    return tmpPath; 
	} else {
	    printf("%s : TmpCachePath: %s\n", __func__, tmpPathMap[fd]);
	    return tmpPathMap[fd];
	}
    }
	
    void clearTmpCachedPath(int fd) {
 	tmpPathMap.erase(fd);
    }

    bool isCached(const char *path) {
        std::string s_path(getCachedPath(path));
        printf("%s : %s, Cached = ", __func__, s_path.c_str());
        struct stat buffer;
        if (stat(s_path.c_str(), &buffer) != 0) {
            printf("false\n");
            return false;
        }
        printf("true\n");
        return true;
    }

    void cacheFile(const char *path) {
        std::string s_path(path);
        std::size_t prevPos = 0, pos = s_path.find('/');
        while (pos != std::string::npos) {
            prevPos = pos;
            pos = s_path.find('/', pos + 1);
            if (pos == std::string::npos) {
                // It's a file, break the loop
                break;
            }
            string folderName = s_path.substr(prevPos + 1, pos - prevPos - 1);
            printf("Making new directory in .cached - %s\n", folderName.c_str());
            struct stat buffer;
            string tempPath = cachedRoot + s_path.substr(0, pos);
            printf("New Directory in .cached - %s\n", tempPath.c_str());
            if (stat(tempPath.c_str(), &buffer) != 0) {
                int res = mkdir(tempPath.c_str(), 0777);
                if (res == -1) {
                    printf("Failed creating new folder!\n");
                }
                else {
                    printf("Created folder successfully!\n");
                }
            }
            else {
                printf("Folder already exists!\n");
            }
        }
        printf("%s : Fetching file %s from server.\n", __func__, path);
        options.afsclient->rpc_getFile(cachedRoot.c_str(), path);
        struct stat buffer;
        if (stat(getCachedPath(path).c_str(), &buffer) == 0) {
            printf("Cached file %s successfully\n",
                   getCachedPath(path).c_str());
        } else {
            printf("Failed to cache file %s\n", getCachedPath(path).c_str());
            perror(strerror(errno));
        }
    }
};

Cache *cache;

#define OPTION(t, p) \
    { t, offsetof(struct options, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("-h", show_help), OPTION("--help", show_help), FUSE_OPT_END};

static void show_help(const char *progname) {
    printf("%s \n", __func__);
    std::cout << "usage: " << progname << " [-s -d] <mountpoint> <server_hostname>\n\n";
}

static void *client_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
    printf("%s \n", __func__);
    (void)conn;
    return NULL;
}


static void client_destroy(void *privateData) {
    string command = "rm -rf " + cache->getCachedPath("");
    int res = system(command.c_str());
    if (res == 0) {
        printf("%s : Successfully cleared cache!\n");
    }
    else {
        printf("%s : Not able to cleare cache!\n");
        perror(strerror(errno));
    }
    delete cache;
}

static int client_getattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi) {
    memset(stbuf, 0, sizeof(struct stat));
    
    int res = 0;

	if(fi != NULL) {
        printf("%s : Local with fd = %lu, file = %s\n", __func__, fi -> fh, path);
		res = fstat(fi->fh, stbuf);
    }
	else {
        printf("%s : Server with path = %s\n", __func__, path);
        // res = lstat(cache->getCachedPath(path).c_str(), stbuf);
		return options.afsclient->rpc_getattr(path, stbuf);
    }

	if (res == -1) {
        perror(strerror(errno));
		return -errno;
    }

	return 0;
}

static int client_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags) {
    printf("%s \n", __func__);
    vector<string> readPaths;
    int res = options.afsclient->rpc_readdir(path, buf, filler, readPaths);

    // for (auto &dir : readPaths) {
    //     cache->cacheFile(dir.c_str());
    // }

    return res;
}

static int client_open(const char *path, struct fuse_file_info *fi) {
    if (cache->isCached(path) == false) {
        cache->cacheFile(path);
    }

    int fd = -1;
    string tmpCachePath = cache->getTmpCachedPath(fd, path);
    string command = "cp " + cache->getCachedPath(path) + " " + tmpCachePath;
    int res = system(command.c_str());

    printf("%s: Fuse Flags: %d\n", __func__, fi->flags);
    fd = open(tmpCachePath.c_str(), fi->flags);
    printf("%s : File openend successfully. Fd = %d, File = %s\n", __func__, fd, tmpCachePath.c_str());
    fi->fh = fd;
    cache->setTmpCachedPath(fd, tmpCachePath.c_str());

    return 0;
}

static int client_read(const char *path, char *buf, size_t size, off_t offset,
                       struct fuse_file_info *fi) {
    // if (cache->isCached(path) == false) {
    //     cache->cacheFile(path);
    // }
    int fd = -1;//fi->fh;  // open(cache->getCachedPath(path).c_str(), O_RDONLY);
    int res = -1;
    string tmpCachedPath = "";
    if (fi) {
        fd = fi -> fh;
    }
    else {
	tmpCachedPath = cache->getTmpCachedPath(fd, path);
	string command = "cp " + cache->getCachedPath(path) + " " + tmpCachedPath;
        res = system(command.c_str());

        fd = open(tmpCachedPath.c_str(), O_RDONLY);
	cache->setTmpCachedPath(fd, tmpCachedPath.c_str());
    }
    if (fd == -1) {
        printf("%s : Filed opened failed in read, file = %s\n", __func__,
               tmpCachedPath.c_str());
        perror(strerror(errno));
        return -1;
    }
    printf("%s : File = %s, fd = %d \n", __func__, tmpCachedPath.c_str(), fd);
    
    res = pread(fd, buf, size, offset);
    if (res == -1) {
        perror(strerror(errno));
        return -1;
    }

    return res;
}

// static int client_pread(const char *path, char *buf, size_t size, off_t offset,
//                        struct fuse_file_info *fi) {
//     // if (cache->isCached(path) == false) {
//     //     cache->cacheFile(path);
//     // }
//     int fd = -1;//fi->fh;  // open(cache->getCachedPath(path).c_str(), O_RDONLY);
//     if (fi) {
//         fd = fi -> fh;
//     }
//     else {
//         fd = open(cache->getCachedPath(path).c_str(), O_RDONLY);
//     }
//     if (fd == -1) {
//         printf("%s : Filed opened failed in read, file = %s\n", __func__,
//                cache->getCachedPath(path).c_str());
//         perror(strerror(errno));
//         return -1;
//     }
//     printf("%s : File = %s, fd = %d \n", __func__, path, fd);
    
//     int res = pread(fd, buf, size, offset);
//     if (res == -1) {
//         perror(strerror(errno));
//         return -1;
//     }

//     return res;
// }

static int client_write(const char *path, const char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
    
    int fd = -1;//fi->fh;  // open(cache->getCachedPath(path).c_str(), O_RDONLY);
    int res = -1;
    string tmpCachedPath = "";
    if (fi) {
        fd = fi -> fh;
    }
    else {
        tmpCachedPath = cache->getTmpCachedPath(fd, path);
        string command = "cp " + cache->getCachedPath(path) + " " + cache->getTmpCachedPath(fd, path);
        res = system(command.c_str());
        fd = open(tmpCachedPath.c_str(), O_WRONLY);
	cache->setTmpCachedPath(fd, tmpCachedPath.c_str());
    }
    printf("%s : File to write = %s, fd = %d\n", __func__, tmpCachedPath.c_str(), fd);
    if (fd == -1) {
        printf("%s : Filed opened failed in write, file = %s\n", __func__,
               tmpCachedPath.c_str());
        perror(strerror(errno));
        return -1;
    }
    printf("%s : Starting to pwrite\n", __func__);
    res = pwrite(fd, buf, size, offset);
    printf("%s : Finished pwrite, wrote %d bytes \n", __func__, res);
    //fsync(fd);
    if (res == -1) {
        perror(strerror(errno));
        return -1;
    }
    return res;
}

// static int client_pwrite(const char *path, const char *buf, size_t size,
//                         off_t offset, struct fuse_file_info *fi) {
    
//     int fd = -1;//fi->fh;  // open(cache->getCachedPath(path).c_str(), O_RDONLY);
//     if (fi) {
//         fd = fi -> fh;
//     }
//     else {
//         fd = open(cache->getCachedPath(path).c_str(), O_RDONLY);
//     }
//     printf("%s : File to write = %s, fd = %d\n", __func__, path, fd);
//     if (fd == -1) {
//         printf("%s : Filed opened failed in write, file = %s\n", __func__,
//                cache->getCachedPath(path).c_str());
//         perror(strerror(errno));
//         return -1;
//     }
//     printf("%s : Starting to pwrite\n", __func__);
//     int res = pwrite(fd, buf, size, offset);
//     printf("%s : Finished pwrite, wrote %d bytes \n", __func__, res);
//     //fsync(fd);
//     if (res == -1) {
//         perror(strerror(errno));
//         return -1;
//     }
//     return res;
// }

static int client_mkdir(const char *path, mode_t mode) {
    printf("%s : Path = %s \n", __func__, path);
    // TODO change order of mkdir calls after mirroring
    int res = mkdir(cache->getCachedPath(path).c_str(), mode);

    if (res == -1) {
        perror(strerror(errno));
        printf("%s : Error making dir = %s\n", __func__, cache->getCachedPath(path).c_str());
    } else {
        printf("%s : Made dir = %s\n", __func__, cache->getCachedPath(path).c_str());
    }

    return options.afsclient->rpc_mkdir(path, mode);
}

static int client_rmdir(const char *path) {
    printf("%s \n", __func__);
    // TODO change order of mkdir calls after mirroring
    int res = rmdir(cache->getCachedPath(path).c_str());

    if (res == -1) {
        perror(strerror(errno));
        printf("%s : Error removing dir = %s\n", __func__, cache->getCachedPath(path).c_str());
    } else {
        printf("%s : Removed dir = %s\n", __func__, cache->getCachedPath(path).c_str());
    }
    return options.afsclient->rpc_rmdir(path);
}

static int client_create(const char *path, mode_t mode,
                         struct fuse_file_info *fi) {
    printf("%s : File = %s\n", __func__, path);
    int res = options.afsclient->rpc_create(path, mode, fi);
    string tmpCachePath = "";
    // TODO check if create actually succeeded
    int fd = -1;
    if (res == 0) {
        cache->cacheFile(path);
	tmpCachePath = cache->getTmpCachedPath(fd, path);
	printf("%s : Opening tmp file = %s\n", __func__, tmpCachePath.c_str());
        fd = open(tmpCachePath.c_str(), fi->flags, mode);
        fi -> fh = fd;
	cache->setTmpCachedPath(fd, tmpCachePath.c_str());	
    }
    if (res == -1) {
        printf("%s : Failed to create file = %s, fd = %d\n",
                    __func__, cache->getCachedPath(path).c_str(), fd);
        perror(strerror(errno));
    }
    else {
        printf("%s : Created file = %s, fd = %d\n",
                    __func__, cache->getCachedPath(path).c_str(), fd);
    }
    return res;
}


static int client_unlink(const char *path) {
    printf("%s : File = %s \n", __func__, cache->getCachedPath(path).c_str());
    // TODO change order of unlink calls after mirroring
    int res = unlink(cache->getCachedPath(path).c_str());
    if (res == -1) {
        perror(strerror(errno));
    }
    //return res;

    return options.afsclient->rpc_unlink(path);
}

static int client_rename(const char *from, const char *to, unsigned int flags) {
    printf("%s \n", __func__);
    // TODO change order of rename calls after mirroring
    int res = rename(cache->getCachedPath(from).c_str(),
                     cache->getCachedPath(to).c_str());
    if (res == -1) {
        printf("%s : Renamed failed at client. From = %s, To = %s\n", __func__,
               from, to);
    }
    return options.afsclient->rpc_rename(from, to, flags);
}

static int client_utimens(const char *path, const struct timespec ts[2],
                          struct fuse_file_info *fi) {
    printf("%s \n", __func__);
    // TODO change order of utimens calls after mirroring
    int res = utimensat(AT_FDCWD, cache->getCachedPath(path).c_str(), ts, AT_SYMLINK_NOFOLLOW);

    if (res == -1) {
        perror(strerror(errno));
    }

    return options.afsclient->rpc_utimens(path, ts, fi);
}

static int client_mknod(const char *path, mode_t mode, dev_t rdev) {
    printf("%s \n", __func__);
    // TODO change order of mknod calls after mirroring
    int res;

    if (S_ISFIFO(mode))
        res = mkfifo(cache->getCachedPath(path).c_str(), mode);
    else
        res = mknod(cache->getCachedPath(path).c_str(), mode, rdev);

    if (res == -1) {
        perror(strerror(errno));
    }

    return options.afsclient->rpc_mknod(path, mode, rdev);
}

static int client_flush(const char *path, struct fuse_file_info *fi) {
    string s_path(cache->getTmpCachedPath(fi->fh, path));
    if (fi == NULL) {
        printf("%s : File handle is NULL!!!!!! File = %s", __func__, s_path.c_str());
        return 0;
    }
    printf("%s : File = %s, fd = %lu\n", __func__, s_path.c_str(), fi -> fh);
    int res = close(dup(fi->fh));
    if (res == -1) {
        printf("%s : Failed to flush file with fd = %lu.\n", __func__, fi->fh);
        perror(strerror(errno));
        return -1;
    } else {
        printf("%s : Successfully flushed file with fd = %lu.\n", __func__,
               fi->fh);
    }
    return 0;
}

static int client_release(const char *path, struct fuse_file_info *fi) {
    string tmpCachedPath(cache->getTmpCachedPath(fi->fh, path));
    if (fi == NULL) {
        printf("%s : File handle is NULL!!!!!! File = %s", __func__, tmpCachedPath.c_str());
        return 0;
    }
    printf("%s : File = %s, fd = %lu\n", __func__, tmpCachedPath.c_str(), fi -> fh);
    fsync(fi->fh);
    int res = rename(tmpCachedPath.c_str(), cache->getCachedPath(path).c_str());
    if (res == -1) {
	printf("%s : Renaming Failed for fd = %lu.\n", __func__, fi->fh);
    } else {
	printf("%s : %s successfully renamed to %s\n", __func__, tmpCachedPath.c_str(), cache->getCachedPath(path).c_str());
    }
    res = close(fi->fh);
    cache->clearTmpCachedPath(fi->fh);
    if (res == -1) {
        printf("%s : Failed to release file with fd = %lu.\n", __func__,
               fi->fh);
        perror(strerror(errno));
        return -1;
    } else {
        printf("%s : Successfully released file with fd = %lu.\n", __func__,
               fi->fh);
    }
    printf("%s : Preparing to flush file %s to server..\n", __func__,
           path);
    options.afsclient->rpc_putFile(cache->getCachedPath(path).c_str());
    printf("%s : Hopefully file is sent to server :p\n", __func__);
    return 0;
}

static struct client_operations : fuse_operations {
    client_operations() {
        init = client_init;
        destroy = client_destroy;
        getattr = client_getattr;
        readdir = client_readdir;
        open = client_open;
        read = client_read;
        // pread = client_pread;
        write = client_write;
        // pwrite = client_pwrite;
        create = client_create;
        mkdir = client_mkdir;
        rmdir = client_rmdir;
        unlink = client_unlink;
        rename = client_rename;
        utimens = client_utimens;
        mknod = client_mknod;
        flush = client_flush;
        release = client_release;
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
    printf("%s : %s\n", __func__, argv[0]);
    std::string server_hostname = argv[argc-1];
    std::string connection_string = server_hostname + ":50051";
    std::cout << "connection string: " << connection_string << std::endl;
    argc -= 1;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.afsclient = new AfsClient(grpc::CreateChannel(
        connection_string, grpc::InsecureChannelCredentials()));

    if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1) return 1;

    if (options.show_help) {
        show_help(argv[0]);
        assert(fuse_opt_add_arg(&args, "--help") == 0);
        args.argv[0] = (char *)"";
    }

    string cachedFolderName = ".cached";
    string rootDir = getCurrentWorkingDir();

    struct stat buffer;
    printf("CurrentWorkingDir = %s\n", rootDir.c_str());
    cache = new Cache(rootDir, cachedFolderName);

    string clientFolderPath = rootDir + "/client";

    if (stat(clientFolderPath.c_str(), &buffer) == 0) {
        printf("%s : Folder %s exists.\n", __func__, clientFolderPath.c_str());
    } else {
        int res = mkdir(clientFolderPath.c_str(), 0777);
        if (res == 0) {
            printf("%s : Folder %s created successfully!\n", __func__,
                   clientFolderPath.c_str());
        } else {
            printf("%s : Failed to create folder %s !\n", __func__,
                   clientFolderPath.c_str());
        }
    }
    return fuse_main(argc, argv, &client_oper, &options);
}
