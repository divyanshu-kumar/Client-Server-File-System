#include <grpc++/grpc++.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <vector>

#include "afsfuse.grpc.pb.h"
#include "file_reader_into_stream.h"
#include "sequential_file_writer.h"
#include "utils.h"

#define MAX_NUM_RETRIES (7)
#define INITIAL_BACKOFF_MS (100)
#define MULTIPLIER (2)

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;

using namespace afsfuse;

using namespace std;

struct sdata {
    int a;
    char b[10];
};

class AfsClient {
   public:
    AfsClient(std::shared_ptr<Channel> channel)
        : stub_(AFS::NewStub(channel)) {}

    int rpc_getattr(string path, struct stat* output) {
        Stat result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            ClientContext context;
            String p;
            p.set_str(path);
            memset(output, 0, sizeof(struct stat));

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            Status status = stub_->afsfuse_getattr(&context, p, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            printf("%s \t : %s\n", __func__, path.c_str());
            perror(strerror(result.err()));
            return -result.err();
        }

        output->st_ino = result.ino();
        output->st_mode = result.mode();
        output->st_nlink = result.nlink();
        output->st_uid = result.uid();
        output->st_gid = result.gid();
        output->st_size = result.size();
        output->st_blksize = result.blksize();
        output->st_blocks = result.blocks();
        output->st_atime = result.atime();
        output->st_mtime = result.mtime();
        output->st_ctime = result.ctime();
        output->st_atim.tv_sec = result.atimtvsec();
        output->st_atim.tv_nsec = result.atimtvnsec();
        output->st_mtim.tv_sec = result.mtimtvsec();
        output->st_mtim.tv_nsec = result.mtimtvnsec();
        return 0;
    }

    int rpc_readdir(string p, void* buf, fuse_fill_dir_t filler) {
        Dirent result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            String path;
            path.set_str(p);
            dirent de;
            Status status;
            ClientContext ctx;

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            ctx.set_wait_for_ready(true);
            ctx.set_deadline(deadline);

            std::unique_ptr<ClientReader<Dirent>> reader(
            stub_->afsfuse_readdir(&ctx, path));
            while (reader->Read(&result)) {
                struct stat st;
                memset(&st, 0, sizeof(st));

                de.d_ino = result.dino();
                strcpy(de.d_name, result.dname().c_str());
                de.d_type = result.dtype();

                st.st_ino = de.d_ino;
                st.st_mode = de.d_type << 12;

                if (filler(buf, de.d_name, &st, 0,
                        static_cast<fuse_fill_dir_flags>(0)))
                    break;
            }

            status = reader->Finish();

            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        return -result.err();
    }

    int rpc_open(const char* path, struct fuse_file_info* fi) {
        FuseFileInfo fi_res;

        int numRetriesLeft = MAX_NUM_RETRIES;
        int currentBackoff = INITIAL_BACKOFF_MS;
        bool isDone = false;
        while (!isDone) {
            ClientContext ctx;
            FuseFileInfo fi_req;

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            ctx.set_wait_for_ready(true);
            ctx.set_deadline(deadline);

            fi_req.set_path(path);
            fi_req.set_flags(fi->flags);
            // printf("%s \t : File = %s\n", __func__, path);
            Status status = stub_->afsfuse_open(&ctx, fi_req, &fi_res);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (fi_res.err() == 0) fi->fh = fi_res.fh();

        return -fi_res.err();
    }

    int rpc_read(const char* path, char* buf, size_t size, off_t offset,
                 struct fuse_file_info* fi) {
        ReadResult rres;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            ClientContext clientContext;
            ReadRequest rr;
            rr.set_path(path);
            rr.set_size(size);
            rr.set_offset(offset);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            clientContext.set_wait_for_ready(true);
            clientContext.set_deadline(deadline);

            Status status = stub_->afsfuse_read(&clientContext, rr, &rres);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }
        
        if (rres.err() == 0) {
            strcpy(buf, rres.buffer().c_str());
            return rres.bytesread();
        } else {
            return -rres.err();
        }
    }

    int rpc_write(const char* path, const char* buf, size_t size, off_t offset,
                  struct fuse_file_info* fi) {
        WriteResult wres;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            ClientContext ctx;
            WriteRequest wreq;
            wreq.set_path(path);
            wreq.set_size(size);
            wreq.set_offset(offset);
            wreq.set_buffer(buf);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            ctx.set_wait_for_ready(true);
            ctx.set_deadline(deadline);
            
            Status status = stub_->afsfuse_write(&ctx, wreq, &wres);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }
        
        if (wres.err() == 0) {
            return wres.nbytes();
        } else {
            return -wres.err();
        }
    }

    int rpc_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
        CreateResult cres;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            ClientContext ctx;
            CreateRequest creq;
            creq.set_path(path);
            creq.set_mode(mode);
            creq.set_flags(fi->flags);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            ctx.set_wait_for_ready(true);
            ctx.set_deadline(deadline);

            Status status = stub_->afsfuse_create(&ctx, creq, &cres);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        
        if (cres.err() == 0) fi->fh = cres.fh();

        return -cres.err();
    }

    int rpc_mkdir(string path, mode_t mode) {
        
        OutputInfo result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            MkdirRequest input;
            ClientContext context;
            input.set_s(path);
            input.set_mode(mode);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            Status status = stub_->afsfuse_mkdir(&context, input, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            // std::cout << "error: afsfuse_mkdir() fails" << std::endl;
        }

        return -result.err();
    }

    int rpc_rmdir(string path) {
        OutputInfo result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            String input;
            ClientContext context;
            input.set_str(path);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            Status status = stub_->afsfuse_rmdir(&context, input, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            // std::cout << "error: afsfuse_rmdir() fails" << std::endl;
        }
        return -result.err();
    }

    int rpc_unlink(string path) {
        OutputInfo result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            String input;
            ClientContext context;
            input.set_str(path);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            Status status = stub_->afsfuse_unlink(&context, input, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            // std::cout << "error: afsfuse_unlink() fails" << std::endl;
        }
        return -result.err();
    }

    int rpc_rename(const char* from, const char* to, unsigned int flags) {
        OutputInfo result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            RenameRequest input;
            ClientContext context;
            input.set_fp(from);
            input.set_tp(to);
            input.set_flag(flags);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            Status status = stub_->afsfuse_rename(&context, input, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            // std::cout << "error: afsfuse_rename() fails" << std::endl;
        }
        return -result.err();
    }

    int rpc_utimens(const char* path, const struct timespec* ts,
                    struct fuse_file_info* fi) {
        OutputInfo result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            UtimensRequest input;
            ClientContext context;
            input.set_sec(ts[0].tv_sec);
            input.set_nsec(ts[0].tv_nsec);
            input.set_sec2(ts[1].tv_sec);
            input.set_nsec2(ts[1].tv_nsec);

            input.set_path(path);

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            Status status = stub_->afsfuse_utimens(&context, input, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            // std::cout << "error: afsfuse_utimens fails" << std::endl;
        }
        return -result.err();
    }

    int rpc_mknod(const char* path, mode_t mode, dev_t rdev) {
        OutputInfo result;

        bool isDone = false;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (!isDone) {
            MknodRequest input;
            ClientContext context;
            input.set_path(path);
            input.set_mode(mode);
            input.set_rdev(rdev);
        
            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);
            
            Status status = stub_->afsfuse_mknod(&context, input, &result);
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft-- == 0) {
                isDone = true;
            }
            else {
                printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
            }
        }

        if (isDone == false) {
            printf("%s \t : Timed out to contact server.\n", __func__);
        }

        if (result.err() != 0) {
            // std::cout << "error: afsfuse_mknod() fails" << std::endl;
        }
        return -result.err();
    }

    int rpc_putFile(const char* root, const char* path) {
        // printf("%s : %s\n", __func__, path);
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (numRetriesLeft >= 0) {
            OutputInfo returnedFile;
            ClientContext context;

            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            std::unique_ptr<ClientWriter<FileContent>> writer(
            stub_->afsfuse_putFile(&context, &returnedFile));
            try {
                FileReaderIntoStream<ClientWriter<FileContent>> reader(
                    string(root), string(path), *writer);

                // TODO: Make the chunk size configurable
                const size_t chunk_size =
                    1UL << 20;  // Hardcoded to 1MB, which seems to be recommended
                                // from experience.
                reader.Read(chunk_size);
            } catch (const std::exception& ex) {
                std::cerr << "Failed to send the file " << path << ": " << ex.what()
                        << std::endl;
                // FIXME: Indicate to the server that something went wrong and that
                // the trasfer should be aborted.
            }

            writer->WritesDone();
            Status status = writer->Finish();
            numRetriesLeft--;
            // printf("%s \t : Backoff - %dms\n", __func__, currentBackoff);
            currentBackoff *= MULTIPLIER;
            if (status.ok()) {
                return true;
            } 
            
            if (numRetriesLeft == 0) {
                std::cerr << "File Exchange rpc failed: " << status.error_message()
                        << std::endl;
                return false;
            } 
            
            printf("%s \t : Timed out to contact server. Retrying...\n", __func__);
        }
        return false;
    }

    int rpc_getFile(const char* rootDir, const char* path) {
        // std::cout << __func__ << " : " << path << endl;
        unsigned int numRetriesLeft = MAX_NUM_RETRIES;
        unsigned int currentBackoff = INITIAL_BACKOFF_MS;
        while (numRetriesLeft >= 0) {
            File requestedFile;
            FileContent contentPart;
            ClientContext context;
            SequentialFileWriter writer;
            std::string filename;
            
            // Set timeout for API
            std::chrono::system_clock::time_point deadline =
                std::chrono::system_clock::now() +
                std::chrono::milliseconds(currentBackoff);

            context.set_wait_for_ready(true);
            context.set_deadline(deadline);

            requestedFile.set_path(path);
            std::unique_ptr<ClientReader<FileContent>> reader(
                stub_->afsfuse_getFile(&context, requestedFile));
            try {
                while (reader->Read(&contentPart)) {
                    filename = std::string(rootDir) + string(path);
                    writer.OpenIfNecessary(filename);
                    auto* const data = contentPart.mutable_content();
                    writer.Write(*data);
                };
                const auto status = reader->Finish();
                numRetriesLeft--;
                currentBackoff *= MULTIPLIER;

                if (status.ok()) {
                    return true;
                }

                if (status.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED || numRetriesLeft == 0) {
                    std::cerr << "Failed to get the file ";
                    if (!filename.empty()) {
                        std::cerr << filename << ' ';
                    }
                    std::cerr << "with filename " << path << ": "
                            << status.error_message() << std::endl;
                    return false;
                }

                // std::cout << "Finished receiving the file " << path << std::endl;
            } catch (const std::system_error& ex) {
                std::cerr << "Failed to receive " << filename << ": " << ex.what();
                return false;
            }
        }
        return false;
    }

   private:
    std::unique_ptr<AFS::Stub> stub_;
};
