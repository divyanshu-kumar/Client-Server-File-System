#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <grpc++/grpc++.h>

#include "afsfuse.grpc.pb.h"
#include "file_reader_into_stream.h"
#include "sequential_file_writer.h"

#define READ_MAX 10000000

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerWriter;
using grpc::Status;
using grpc::StatusCode;

using namespace afsfuse;

using namespace std;

struct sdata {
    int a;
    char b[10] = {};
};

void translatePath(const char * client_path, char * server_path) {
    printf("%s \n", __func__);
    strcat(server_path, "./server");
    strcat(server_path + 8, client_path);
    server_path[strlen(server_path)] = '\0';
}

class AfsServiceImpl final: public AFS::Service {
    Status afsfuse_getattr(ServerContext * context,
        const String * s,
            Stat * reply) override {
        //cout<<"[DEBUG] : lstat: "<<s->str().c_str()<<endl;
        printf("%s \n", __func__);
        struct stat st;
        char server_path[512] = {
            0
        };
        translatePath(s -> str().c_str(), server_path);
        int res = lstat(server_path, & st);
        if (res == -1) {
            perror(strerror(errno));
            //cout<<"errno: "<<errno<<endl;
            reply -> set_err(errno);
        } else {
            reply -> set_ino(st.st_ino);
            reply -> set_mode(st.st_mode);
            reply -> set_nlink(st.st_nlink);
            reply -> set_uid(st.st_uid);
            reply -> set_gid(st.st_gid);

            reply -> set_size(st.st_size);
            reply -> set_blksize(st.st_blksize);
            reply -> set_blocks(st.st_blocks);
            reply -> set_atime(st.st_atime);
            reply -> set_mtime(st.st_mtime);
            reply -> set_ctime(st.st_ctime);

            reply -> set_err(0);
        }

        return Status::OK;

    }

    Status afsfuse_readdir(ServerContext * context,
        const String * s,
            ServerWriter < Dirent > * writer) override {
        //cout<<"[DEBUG] : readdir: "<<s->str().c_str()<<endl;
        printf("%s \n", __func__);
        DIR * dp;
        struct dirent * de;
        Dirent directory;
        char server_path[512] = {
            0
        };
        translatePath(s -> str().c_str(), server_path);

        dp = opendir(server_path);
        if (dp == NULL) {
            //cout<<"[DEBUG] : readdir: "<<"dp == NULL"<<endl;
            perror(strerror(errno));
            directory.set_err(errno);
            return Status::OK;
        }

        while ((de = readdir(dp)) != NULL) {
            directory.set_dino(de -> d_ino);
            directory.set_dname(de -> d_name);
            directory.set_dtype(de -> d_type);
            writer -> Write(directory);
        }
        directory.set_err(0);

        closedir(dp);

        return Status::OK;
    }

    Status afsfuse_open(ServerContext * context,
        const FuseFileInfo * fi_req,
            FuseFileInfo * fi_reply) override {
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };

        translatePath(fi_req -> path().c_str(), server_path);
        //cout<<"[DEBUG] : afsfuse_open: path "<<server_path<<endl;
        //cout<<"[DEBUG] : afsfuse_open: flag "<<fi_req->flags()<<endl;

        int fh = open(server_path, fi_req -> flags());

        //cout<<"[DEBUG] : afsfuse_open: fh"<<fh<<endl;
        if (fh == -1) {
            fi_reply -> set_err(errno);
        } else {
            fi_reply -> set_fh(fh);
            fi_reply -> set_err(0);
            close(fh);
        }

        return Status::OK;
    }

    Status afsfuse_read(ServerContext * context,
        const ReadRequest * rr,
            ReadResult * reply) override {
        printf("%s \n", __func__);
        char path[512];
        char * buf = new char[rr -> size()];
        translatePath(rr -> path().c_str(), path);
        //cout<<"[DEBUG] : afsfuse_read: "<<path<<endl;

        int fd = open(path, O_RDONLY);
        //cout<<"[DEBUG] : afsfuse_read: fd "<<fd<<endl;
        if (fd == -1) {
            reply -> set_err(errno);
            perror(strerror(errno));
            return Status::OK;
        }

        int res = pread(fd, buf, rr -> size(), rr -> offset());
        if (res == -1) {
            reply -> set_err(errno);
            perror(strerror(errno));
            return Status::OK;
        }

        reply -> set_bytesread(res);
        reply -> set_buffer(buf);
        reply -> set_err(0);

        if (fd > 0)
            close(fd);
        free(buf);

        return Status::OK;
    }

    Status afsfuse_write(ServerContext * context,
        const WriteRequest * wr,
            WriteResult * reply) override {
        printf("%s \n", __func__);
        char path[512] = {
            0
        };
        translatePath(wr -> path().c_str(), path);
        int fd = open(path, O_WRONLY);
        //cout<<"[DEBUG] : afsfuse_write: path "<<path<<endl;
        //cout<<"[DEBUG] : afsfuse_write: fd "<<fd<<endl;
        if (fd == -1) {
            reply -> set_err(errno);
            perror(strerror(errno));
            return Status::OK;
        }

        int res = pwrite(fd, wr -> buffer().c_str(), wr -> size(), wr -> offset());
        //cout<<"[DEBUG] : afsfuse_write: res"<<res<<endl;

        fsync(fd);

        if (res == -1) {
            reply -> set_err(errno);
            perror(strerror(errno));
            return Status::OK;
        }

        reply -> set_nbytes(res);
        reply -> set_err(0);

        if (fd > 0)
            close(fd);

        return Status::OK;
    }

    Status afsfuse_create(ServerContext * context,
        const CreateRequest * req,
            CreateResult * reply) override {
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };
        translatePath(req -> path().c_str(), server_path);

        //cout<<"[DEBUG] : afsfuse_create: path "<<server_path<<endl;
        //cout<<"[DEBUG] : afsfuse_create: flag "<<req->flags()<<endl;

        int fh = open(server_path, req -> flags(), req -> mode());

        //cout<<"[DEBUG] : afsfuse_create: fh"<<fh<<endl;
        if (fh == -1) {
            reply -> set_err(errno);
            return Status::OK;
        } else {
            reply -> set_fh(fh);
            reply -> set_err(0);
            close(fh);
            return Status::OK;
        }
    }

    Status afsfuse_mkdir(ServerContext * context,
        const MkdirRequest * input,
            OutputInfo * reply) override {
        //cout<<"[DEBUG] : mkdir: " << endl;
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };
        translatePath(input -> s().c_str(), server_path);

        int res = mkdir(server_path, input -> mode());

        if (res == -1) {
            perror(strerror(errno));
            reply -> set_err(errno);
            return Status::OK;
        } else {
            reply -> set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_rmdir(ServerContext * context,
        const String * input,
            OutputInfo * reply) override {
        //cout<<"[DEBUG] : rmdir: " << endl;
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };
        translatePath(input -> str().c_str(), server_path);

        int res = rmdir(server_path);

        if (res == -1) {
            perror(strerror(errno));
            reply -> set_err(errno);
            return Status::OK;
        } else {
            reply -> set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_unlink(ServerContext * context,
        const String * input,
            OutputInfo * reply) override {
        //cout<<"[DEBUG] : unlink " << endl;
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };
        translatePath(input -> str().c_str(), server_path);
        // cout << "server path: " << server_path << endl;
        int res = unlink(server_path);
        if (res == -1) {
            perror(strerror(errno));
            reply -> set_err(errno);
            return Status::OK;
        } else {
            reply -> set_err(0);
        }
        return Status::OK;
    }

    Status afsfuse_rename(ServerContext * context,
        const RenameRequest * input,
            OutputInfo * reply) override {
        //cout<<"[DEBUG] : rename " << endl;
        printf("%s \n", __func__);
        if (input -> flag()) {
            perror(strerror(errno));
            reply -> set_err(EINVAL);
            reply -> set_str("rename fail");
            return Status::OK;
        }

        char from_path[512] = {
            0
        };
        char to_path[512] = {
            0
        };
        translatePath(input -> fp().c_str(), from_path);
        translatePath(input -> tp().c_str(), to_path);

        int res = rename(from_path, to_path);
        if (res == -1) {
            perror(strerror(errno));
            reply -> set_err(errno);
            return Status::OK;
        } else {
            reply -> set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_utimens(ServerContext * context,
        const UtimensRequest * input,
            OutputInfo * reply) override {
        //cout<<"[DEBUG] : utimens " << endl;
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };
        translatePath(input -> path().c_str(), server_path);

        struct timespec ts[2];
        long oo;
        int ii;

        ts[0].tv_sec = input -> sec();
        ts[0].tv_nsec = input -> nsec();

        ts[1].tv_sec = input -> sec2();
        ts[1].tv_nsec = input -> nsec2();

        int res = utimensat(AT_FDCWD, server_path, ts, AT_SYMLINK_NOFOLLOW);

        if (res == -1) {
            perror(strerror(errno));
            reply -> set_err(errno);
            return Status::OK;
        }
        reply -> set_err(0);
        return Status::OK;
    }

    Status afsfuse_mknod(ServerContext * context,
        const MknodRequest * input,
            OutputInfo * reply) override {
        // cout<<"[DEBUG] : mknod " << endl;
        printf("%s \n", __func__);
        char server_path[512] = {
            0
        };
        translatePath(input -> path().c_str(), server_path);

        mode_t mode = input -> mode();
        dev_t rdev = input -> rdev();

        int res;

        if (S_ISFIFO(mode))
            res = mkfifo(server_path, mode);
        else
            res = mknod(server_path, mode, rdev);

        if (res == -1) {
            reply -> set_err(errno);
            return Status::OK;
        }

        reply -> set_err(0);
        return Status::OK;
    }

    Status afsfuse_getFile(
        ServerContext * context,
        const File * file,
            ServerWriter < FileContent > * writer) override {
        const string filepath = std::string(file -> path());
        std::cout << __func__ << " : " << filepath << endl;
        struct stat buffer;
        if (stat(filepath.c_str(), & buffer) == 0) {
            printf("%s: File exists\n", __func__);
        }
        try {
            FileReaderIntoStream < ServerWriter < FileContent > > reader(filepath, * writer);
            const size_t chunk_size = 1UL << 20; // Hardcoded to 1MB, which seems to be recommended from experience.
            reader.Read(chunk_size);
        } catch (const std::exception & ex) {
            std::ostringstream sts;
            sts << "Error sending the file " << filepath << " : " << ex.what();
            std::cerr << sts.str() << std::endl;
            return Status(StatusCode::ABORTED, sts.str());
        }
        return Status::OK;
    }
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    AfsServiceImpl service;
    printf("%s \n", __func__);
    ServerBuilder builder;

    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    builder.RegisterService( & service);

    std::unique_ptr < Server > server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server -> Wait();
}

int main(int argc, char ** argv) {
    RunServer();
    printf("%s \n", __func__);
    return 0;
}
