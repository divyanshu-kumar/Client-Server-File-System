#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "AfsClient.h"

const char *rootDir = "/users/dkumar27/AFS2/client/";
const char *CachedFolderName = ".cached";

static struct options {
    AfsClient * afsclient;
    int show_help;
} options;

class Cache {
        string cachedRoot;
public:
        Cache() {
                string currentWorkDir = getCurrentWorkingDir();
                printf("Current Working Dir : %s\n", currentWorkDir.c_str());
                cachedRoot = currentWorkDir + "/" + CachedFolderName;
                makeCacheFolder();
        }

        void makeCacheFolder() {
                struct stat buffer;
                if (stat(cachedRoot.c_str(), &buffer) == 0) {
                        printf("%s : Cached folder already exists. Path = %s\n", __func__, cachedRoot.c_str());
                }
                else {
                        int status = mkdir(cachedRoot.c_str(), 0777);

                        if (status != 0) {
                                printf("Failed to create cached directory!\n");
                        }
                        else {
                                printf("Successfully created cached directory.\n");
                        }
                }
        }

        string getCachedPath(const char * path) {
                return cachedRoot + "/" + string(path);
        }


        string getCurrentWorkingDir() {
                char arg1[20];
                char exepath[PATH_MAX + 1] = {0};

                sprintf( arg1, "/proc/%d/exe", getpid() );
                readlink( arg1, exepath, 1024 );
                std::string s_path(exepath);
                std::size_t lastPos = s_path.find_last_of("/");
                return s_path.substr(0, lastPos);
        }

	bool isCached(const char * path) {
		std::string s_path(getCachedPath(path));
    		printf("%s : %s \n", __func__, s_path.c_str());
    		struct stat buffer;
    		if (stat(s_path.c_str(), &buffer) != 0) {
			return false;
    		}
    		return true;
	}

	void cacheFile(const char * path) {
		printf("%s : Fetching file %s from server.\n", __func__, path);
        	options.afsclient -> rpc_getFile(cachedRoot.c_str(), path);
	}
};

Cache cache;


#define OPTION(t, p)                           \
       { t, offsetof(struct options, p), 1 }

static const struct fuse_opt option_spec[] = {
    OPTION("-h", show_help),
    OPTION("--help", show_help),
    FUSE_OPT_END
};

static void show_help(const char * progname) {
    printf("%s \n", __func__);
    std::cout << "usage: " << progname << " [-s -d] <mountpoint>\n\n";
}

static void * client_init(struct fuse_conn_info * conn,
    struct fuse_config * cfg) {
    printf("%s \n", __func__);
    (void) conn;
    return NULL;
}

static int client_getattr(const char * path, struct stat * stbuf,
    struct fuse_file_info * fi) {
    printf("%s : \t  %s\n", __func__, path);
    memset(stbuf, 0, sizeof(struct stat));
    return options.afsclient -> rpc_getattr(path, stbuf);
}

static int client_readdir(const char * path, void * buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info * fi,
    enum fuse_readdir_flags flags) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_readdir(path, buf, filler);
}

static int client_open(const char * path, struct fuse_file_info * fi) {
    std::string s_path(cache.getCachedPath(path));
    
    /*printf("%s : %s \n", __func__, s_path.c_str());
    struct stat buffer;
    if (stat(s_path.c_str(), &buffer) != 0) {
	printf("%s : Fetching file %s from server.\n", __func__, path);
	options.afsclient -> rpc_getFile(cache.getPath().c_str(), path);
    }
    else {
	printf("%s : File %s found locally. Opening file..\n", __func__, s_path.c_str());
    }
    */
    if (cache.isCached(path) == false) {
    	cache.cacheFile(path);
    }
    int fd = open(s_path.c_str(), O_RDONLY);
    printf("%s : File openend successfully. Fd = %d", __func__, fd);
    fi->fh = fd;
    return 0;
    //options.afsclient -> rpc_getFile(rootDir, path);
    //return options.afsclient -> rpc_open(path, fi);
}

static int client_read(const char * path, char * buf, size_t size, off_t offset,
    struct fuse_file_info * fi) {
    printf("%s \n", __func__);
    //if (cache.isCached)
    return options.afsclient -> rpc_read(path, buf, size, offset, fi);
}

static int client_write(const char * path,
    const char * buf, size_t size,
        off_t offset, struct fuse_file_info * fi) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_write(path, buf, size, offset, fi);
}

static int client_mkdir(const char * path, mode_t mode) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_mkdir(path, mode);
}

static int client_rmdir(const char * path) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_rmdir(path);
}

static int client_create(const char * path, mode_t mode, struct fuse_file_info * fi) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_create(path, mode, fi);
}

static int client_unlink(const char * path) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_unlink(path);
}

static int client_rename(const char * from,
    const char * to, unsigned int flags) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_rename(from, to, flags);
}

static int client_utimens(const char * path,
    const struct timespec ts[2],
        struct fuse_file_info * fi) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_utimens(path, ts, fi);
}

static int client_mknod(const char * path, mode_t mode, dev_t rdev) {
    printf("%s \n", __func__);
    return options.afsclient -> rpc_mknod(path, mode, rdev);
}

static int client_flush(const char * path, struct fuse_file_info * fi) {
    string s_path(cache.getCachedPath(path));
    printf("%s : File = %s\n", __func__, s_path.c_str());
    int res = close(fi -> fh);
    if (res == -1) {
	printf("%s : Failed to flush file.\n", __func__);
	return -errno;
    }
    else {
	printf("%s : Successfully flushed file.\n", __func__);
    }
    return 0;
}

static struct client_operations: fuse_operations {
    client_operations() {
        init = client_init;
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
    }

} client_oper;


int main(int argc, char * argv[]) {
    printf("%s \n", __func__);
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    options.afsclient = new AfsClient(grpc::CreateChannel(
        "0.0.0.0:50051", grpc::InsecureChannelCredentials()));

    if (fuse_opt_parse( & args, & options, option_spec, NULL) == -1)
        return 1;

    if (options.show_help) {
        show_help(argv[0]);
        assert(fuse_opt_add_arg( & args, "--help") == 0);
        args.argv[0] = (char * )
        "";
    }
    return fuse_main(argc, argv, & client_oper, & options);
}
