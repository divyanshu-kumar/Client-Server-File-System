#include <dirent.h>
#include <fcntl.h>
#include <grpc++/grpc++.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <ctime>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include "afsfuse.grpc.pb.h"
#include "file_reader_into_stream.h"
#include "sequential_file_writer.h"

#define READ_MAX 10000000

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using grpc::StatusCode;

using namespace afsfuse;

using namespace std;

inline void get_time(struct timespec* ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}
inline double get_time_diff(struct timespec* before, struct timespec* after) {
    double delta_s = after->tv_sec - before->tv_sec;
    double delta_ns = after->tv_nsec - before->tv_nsec;

    return (delta_s + (delta_ns * 1e-9)) * ((double)1e3);
}
string getCurrentWorkingDir() {
    char arg1[20];
    char exepath[PATH_MAX + 1] = {0};

    sprintf(arg1, "/proc/%d/exe", getpid());
    int res = readlink(arg1, exepath, 1024);
    std::string s_path(exepath);
    std::size_t lastPos = s_path.find_last_of("/");
    return s_path.substr(0, lastPos);
}

string rootDir;

struct sdata {
    int a;
    char b[10] = {};
};

void translatePath(const char* client_path, char* server_path) {
    string path;
    if (client_path[0] == '/')
        path = rootDir + client_path;
    else
        path = rootDir + "/" + client_path;
    strcat(server_path, path.c_str());
    // strcat(server_path, "./server");
    // strcat(server_path + 8, client_path);
    // printf("%s : Client path = %s, server path = %s\n", __func__,
    // client_path,
    //        server_path);
    // server_path[strlen(server_path)] = '\0';
}

class AfsServiceImpl final : public AFS::Service {
    Status afsfuse_getattr(ServerContext* context, const String* s,
                           Stat* reply) override {
        // cout<<"[DEBUG] : lstat: "<<s->str().c_str()<<endl;
        // printf("%s \n", __func__);
        struct stat st;
        char server_path[512] = {0};
        translatePath(s->str().c_str(), server_path);
        int res = lstat(server_path, &st);
        if (res == -1) {
            // printf("%s \n", __func__);perror(strerror(errno));
            // cout<<"errno: "<<errno<<endl;
            reply->set_err(errno);
        } else {
            reply->set_ino(st.st_ino);
            reply->set_mode(st.st_mode);
            reply->set_nlink(st.st_nlink);
            reply->set_uid(st.st_uid);
            reply->set_gid(st.st_gid);

            reply->set_size(st.st_size);
            reply->set_blksize(st.st_blksize);
            reply->set_blocks(st.st_blocks);
            reply->set_atime(st.st_atime);
            reply->set_atimtvsec(st.st_atim.tv_sec);
            reply->set_atimtvnsec(st.st_atim.tv_nsec);
            reply->set_mtime(st.st_mtime);
            reply->set_mtimtvsec(st.st_mtim.tv_sec);
            reply->set_mtimtvnsec(st.st_mtim.tv_nsec);
            reply->set_ctime(st.st_ctime);

            reply->set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_readdir(ServerContext* context, const String* s,
                           ServerWriter<Dirent>* writer) override {
        // cout<<"[DEBUG] : readdir: "<<s->str().c_str()<<endl;
        // printf("%s \n", __func__);
        DIR* dp;
        struct dirent* de;
        Dirent directory;
        char server_path[512] = {0};
        translatePath(s->str().c_str(), server_path);

        dp = opendir(server_path);
        if (dp == NULL) {
            // cout<<"[DEBUG] : readdir: "<<"dp == NULL"<<endl;
            printf("%s \n", __func__);
            perror(strerror(errno));
            directory.set_err(errno);
            return Status::OK;
        }

        while ((de = readdir(dp)) != NULL) {
            directory.set_dino(de->d_ino);
            directory.set_dname(de->d_name);
            directory.set_dtype(de->d_type);
            writer->Write(directory);
        }
        directory.set_err(0);

        closedir(dp);

        return Status::OK;
    }

    Status afsfuse_open(ServerContext* context, const FuseFileInfo* fi_req,
                        FuseFileInfo* fi_reply) override {
        printf("%s : %s\n", __func__, fi_req->path().c_str());
        char server_path[512] = {0};

        translatePath(fi_req->path().c_str(), server_path);
        // cout<<"[DEBUG] : afsfuse_open: path "<<server_path<<endl;
        // cout<<"[DEBUG] : afsfuse_open: flag "<<fi_req->flags()<<endl;

        int fh = open(server_path, fi_req->flags());

        // cout<<"[DEBUG] : afsfuse_open: fh"<<fh<<endl;
        if (fh == -1) {
            fi_reply->set_err(errno);
        } else {
            fi_reply->set_fh(fh);
            fi_reply->set_err(0);
            close(fh);
        }

        return Status::OK;
    }

    Status afsfuse_read(ServerContext* context, const ReadRequest* rr,
                        ReadResult* reply) override {
        // printf("%s \n", __func__);
        char path[512];
        char* buf = new char[rr->size()];
        translatePath(rr->path().c_str(), path);
        // cout<<"[DEBUG] : afsfuse_read: "<<path<<endl;

        int fd = open(path, O_RDONLY);
        // cout<<"[DEBUG] : afsfuse_read: fd "<<fd<<endl;
        if (fd == -1) {
            reply->set_err(errno);
            printf("%s \n", __func__);
            perror(strerror(errno));
            return Status::OK;
        }

        int res = pread(fd, buf, rr->size(), rr->offset());
        if (res == -1) {
            reply->set_err(errno);
            printf("%s \n", __func__);
            perror(strerror(errno));
            return Status::OK;
        }

        reply->set_bytesread(res);
        reply->set_buffer(buf);
        reply->set_err(0);

        if (fd > 0) close(fd);
        free(buf);

        return Status::OK;
    }

    Status afsfuse_write(ServerContext* context, const WriteRequest* wr,
                         WriteResult* reply) override {
        // printf("%s \n", __func__);
        char path[512] = {0};
        translatePath(wr->path().c_str(), path);
        int fd = open(path, O_WRONLY);
        // cout<<"[DEBUG] : afsfuse_write: path "<<path<<endl;
        // cout<<"[DEBUG] : afsfuse_write: fd "<<fd<<endl;
        if (fd == -1) {
            reply->set_err(errno);
            printf("%s \n", __func__);
            perror(strerror(errno));
            return Status::OK;
        }

        int res = pwrite(fd, wr->buffer().c_str(), wr->size(), wr->offset());
        // cout<<"[DEBUG] : afsfuse_write: res"<<res<<endl;

        fsync(fd);

        if (res == -1) {
            reply->set_err(errno);
            printf("%s \n", __func__);
            perror(strerror(errno));
            return Status::OK;
        }

        reply->set_nbytes(res);
        reply->set_err(0);

        if (fd > 0) close(fd);

        return Status::OK;
    }

    bool doesPathExist(std::string server_path) {
        std::size_t lastPos = server_path.find_last_of("/");
        struct stat tempStatBuffer;
        return stat(server_path.substr(0, lastPos).c_str(), &tempStatBuffer) == 0;
    }

    bool createPath(std::string server_path) {
        printf("%s : Path does not exist\n Creating Path", __func__);
        std::size_t lastPos = server_path.find_last_of("/");
        std::string toBeCreatedPath = server_path.substr(0, lastPos);
        string command = "mkdir -p " + toBeCreatedPath;
        int res = system(command.c_str());		
        return res == 0;
    }

    Status afsfuse_create(ServerContext* context, const CreateRequest* req,
                          CreateResult* reply) override {
        char server_path[512] = {0};
        translatePath(req->path().c_str(), server_path);

        if (!doesPathExist(server_path)) {
            bool pathCreated = createPath(server_path);
            if (pathCreated) {
                printf("%s : %s path Creation Success\n", __func__, server_path);
            } else {
                printf("%s : %s path Creation Failed\n", __func__, server_path);
                reply->set_err(errno);
                return Status::OK;
            }
        }	

        // cout<<"[DEBUG] : afsfuse_create: path "<<server_path<<endl;
        // cout<<"[DEBUG] : afsfuse_create: flag "<<req->flags()<<endl;

        int fh = open(server_path, req->flags(), req->mode());

        // cout<<"[DEBUG] : afsfuse_create: fh"<<fh<<endl;
        if (fh == -1) {
            reply->set_err(errno);
            return Status::OK;
        } else {
            struct timespec ts[2];  // ts[0] - access, ts[1] - mod
            get_time(&ts[0]);
            ts[1].tv_sec = ts[0].tv_sec;
            ts[1].tv_nsec = ts[0].tv_nsec;
            utimensat(AT_FDCWD, server_path, ts, AT_SYMLINK_NOFOLLOW);
            reply->set_fh(fh);
            reply->set_err(0);
            close(fh);
            return Status::OK;
        }
    }

    Status afsfuse_mkdir(ServerContext* context, const MkdirRequest* input,
                         OutputInfo* reply) override {
        // cout<<"[DEBUG] : mkdir: " << endl;
        // printf("%s \n", __func__);
        char server_path[512] = {0};
        translatePath(input->s().c_str(), server_path);

        int res = mkdir(server_path, input->mode());

        if (res == -1) {
            printf("%s \n", __func__);
            perror(strerror(errno));
            reply->set_err(errno);
            return Status::OK;
        } else {
            reply->set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_rmdir(ServerContext* context, const String* input,
                         OutputInfo* reply) override {
        // cout<<"[DEBUG] : rmdir: " << endl;
        // printf("%s \n", __func__);
        char server_path[512] = {0};
        translatePath(input->str().c_str(), server_path);

        int res = rmdir(server_path);

        if (res == -1) {
            printf("%s \n", __func__);
            perror(strerror(errno));
            reply->set_err(errno);
            return Status::OK;
        } else {
            reply->set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_unlink(ServerContext* context, const String* input,
                          OutputInfo* reply) override {
        // cout<<"[DEBUG] : unlink " << endl;
        // printf("%s \n", __func__);
        char server_path[512] = {0};
        translatePath(input->str().c_str(), server_path);
        // cout << "server path: " << server_path << endl;
        int res = unlink(server_path);
        if (res == -1) {
            printf("%s \n", __func__);
            perror(strerror(errno));
            reply->set_err(errno);
            return Status::OK;
        } else {
            reply->set_err(0);
        }
        return Status::OK;
    }

    Status afsfuse_rename(ServerContext* context, const RenameRequest* input,
                          OutputInfo* reply) override {
        // cout<<"[DEBUG] : rename " << endl;
        // printf("%s \n", __func__);
        if (input->flag()) {
            printf("%s \n", __func__);
            perror(strerror(errno));
            reply->set_err(EINVAL);
            reply->set_str("rename fail");
            return Status::OK;
        }

        char from_path[512] = {0};
        char to_path[512] = {0};
        translatePath(input->fp().c_str(), from_path);
        translatePath(input->tp().c_str(), to_path);

        int res = rename(from_path, to_path);
        if (res == -1) {
            printf("%s \n", __func__);
            perror(strerror(errno));
            reply->set_err(errno);
            return Status::OK;
        } else {
            reply->set_err(0);
        }

        return Status::OK;
    }

    Status afsfuse_utimens(ServerContext* context, const UtimensRequest* input,
                           OutputInfo* reply) override {
        // cout<<"[DEBUG] : utimens " << endl;
        // printf("%s \n", __func__);
        char server_path[512] = {0};
        translatePath(input->path().c_str(), server_path);

        struct timespec ts[2];
        long oo;
        int ii;

        ts[0].tv_sec = input->sec();
        ts[0].tv_nsec = input->nsec();

        ts[1].tv_sec = input->sec2();
        ts[1].tv_nsec = input->nsec2();

        int res = utimensat(AT_FDCWD, server_path, ts, AT_SYMLINK_NOFOLLOW);

        if (res == -1) {
            printf("%s \n", __func__);
            perror(strerror(errno));
            reply->set_err(errno);
            return Status::OK;
        }
        reply->set_err(0);
        return Status::OK;
    }

    Status afsfuse_mknod(ServerContext* context, const MknodRequest* input,
                         OutputInfo* reply) override {
        // cout<<"[DEBUG] : mknod " << endl;
        // printf("%s \n", __func__);
        char server_path[512] = {0};
        translatePath(input->path().c_str(), server_path);

        mode_t mode = input->mode();
        dev_t rdev = input->rdev();

        int res;

        if (S_ISFIFO(mode))
            res = mkfifo(server_path, mode);
        else
            res = mknod(server_path, mode, rdev);

        if (res == -1) {
            reply->set_err(errno);
            return Status::OK;
        }

        reply->set_err(0);
        return Status::OK;
    }

    Status afsfuse_getFile(ServerContext* context, const File* file,
                           ServerWriter<FileContent>* writer) override {
        struct stat buffer;
        string filepath = rootDir.c_str() + file->path();
        //std::cout << __func__ << " : " << filepath.c_str() << endl;
        //if (stat(filepath.c_str(), &buffer) == 0) {
        //    printf("%s: File exists\n", __func__);
        //}
        try {
            FileReaderIntoStream<ServerWriter<FileContent> > reader(
                rootDir, file->path(), *writer);
            
            const size_t chunk_size =
                1UL << 20;  // Hardcoded to 1MB, which seems to be recommended
                            // from experience.
            reader.Read(chunk_size);
            //std::cout << "Sending chunk of size 1 MB from server to client"
            //          << std::endl;
        } catch (const std::exception& ex) {
            std::ostringstream sts;
            sts << "Error sending the file " << filepath.c_str() << " : "
                << ex.what();
            std::cerr << sts.str() << std::endl;
            return Status(StatusCode::ABORTED, sts.str());
        }
        //std::cout << __func__ << " : DONE " << std::endl;
        return Status::OK;
    }

    Status afsfuse_putFile(ServerContext* context,
                           ServerReader<FileContent>* reader,
                           OutputInfo* reply) override {
        // printf("%s : Begin\n", __func__);        	
        string final_path, temp_path;
        FileContent contentPart;
        SequentialFileWriter writer;
        struct timespec ts_start, ts_end;
        get_time(&ts_start);
        while (reader->Read(&contentPart)) {
            try {
                if (temp_path.empty()) {
                    string name = contentPart.name();
                    if (name.empty() == false && name.at(0) == '/') {
                        name = name.substr(1);
                    }
                    final_path = (rootDir + "/" + name);
                    temp_path = (rootDir + "/" + name + ".tmp" + std::to_string(rand() % 1000));
                }

                if (!doesPathExist(final_path)) {
                    bool pathCreated = createPath(final_path);
                    if (pathCreated) {
                        printf("%s : %s path Creation Success\n", __func__, final_path.c_str());
                    } else {
                        printf("%s : %s path Creation Failed\n", __func__, final_path.c_str());
                        reply->set_err(errno);
                        return Status::OK;
                    }
                }
                writer.OpenIfNecessary(temp_path);
                auto* const data = contentPart.mutable_content();
                // std::cout << "Received data at server " << std::endl;
                writer.Write(*data);
                reply->set_err(0);
            } catch (const std::system_error& ex) {
                printf("%s : ERROR getting file on server!!\n", __func__);
                const auto status_code = writer.NoSpaceLeft()
                                             ? StatusCode::RESOURCE_EXHAUSTED
                                             : StatusCode::ABORTED;
                return Status(status_code, ex.what());
            }
        }

        int res = rename(temp_path.c_str(), final_path.c_str());

        if (res == -1) {
            printf("%s \t : Renaming failed! From = %s to %s\n", 
                __func__, temp_path.c_str(), final_path.c_str());
            perror(strerror(errno));
            reply->set_err(errno);
        }
        else {
            reply->set_err(0);
        }

        get_time(&ts_end);
        printf("Time to receive (ms) : %f \n",
               get_time_diff(&ts_start, &ts_end));
        
        return Status::OK;
    }
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    AfsServiceImpl service;
    // printf("%s \n", __func__);
    ServerBuilder builder;

    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;

    server->Wait();
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    cout.tie(nullptr);
    srand(time(NULL));
    struct stat buffer;
    rootDir = getCurrentWorkingDir();
    printf("CurrentWorkingDir: %s\n", rootDir.c_str());
    string serverFolderPath = rootDir + "/" + "server";
    if (stat(serverFolderPath.c_str(), &buffer) == 0) {
        printf("%s : Folder %s exists.\n", __func__, serverFolderPath.c_str());
    } else {
        int res = mkdir(serverFolderPath.c_str(), 0777);
        if (res == 0) {
            printf("%s : Folder %s created successfully!\n", __func__,
                   serverFolderPath.c_str());
        } else {
            printf("%s : Failed to create folder %s!\n", __func__,
                   serverFolderPath.c_str());
        }
    }
    rootDir = serverFolderPath;
    printf("RootDIR = %s\n", rootDir.c_str());
    RunServer();
    // printf("%s \n", __func__);
    return 0;
}
