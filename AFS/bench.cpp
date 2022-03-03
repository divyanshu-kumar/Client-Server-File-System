// basic file operations
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace std;

void    get_time(struct timespec *ts);
double  get_time_diff(struct timespec *before, struct timespec *after);
int     msleep(long msec);
void    getRandomText(vector<string> &data, int file_size);
void    clearDirectory(string directory);

int main() {
    bool cleanup = false;
    const int max_File_Size = 26; // 2's power. 2^30 = 1 GB file
    
    clearDirectory("server");
    clearDirectory(".cached");
    
    for (int i = 0; i < max_File_Size; i++) {
        int file_size = (1 << i);
        string fileName = "./client/test" + to_string(file_size) + ".txt";

        struct timespec ts_open_start, ts_open_end;

        get_time(&ts_open_start);

        int fd = open(fileName.c_str(), O_CREAT | O_WRONLY);
        
        get_time(&ts_open_end);

        if (fd == -1) {
            continue;
        }

        vector<string> data;
        getRandomText(data, file_size);

        struct timespec ts_write_start, ts_write_end;

        get_time(&ts_write_start);

        int cur_offset = 0;
        for (auto &s : data) {
            int res = pwrite(fd, s.c_str(), s.size(), cur_offset); // write(fd, s.c_str(), s.size()); 
            if (file_size < 10) {
                printf("%s\n", s.c_str());
            }
            cur_offset += s.size();
        }

        get_time(&ts_write_end);

        struct timespec ts_close_start, ts_close_end;

        get_time(&ts_close_start);

        int res = close(fd);

        get_time(&ts_close_end);
        
        printf(
            "Create (ms) : %.3f \t Write (ms) : %.3f \t Close (ms) : "
            "%.3f \t",
            get_time_diff(&ts_open_start, &ts_open_end),
            get_time_diff(&ts_write_start, &ts_write_end),
            get_time_diff(&ts_close_start, &ts_close_end));

        printf("File size (bytes) = %d \n", file_size);

        //msleep(100);

        if (cleanup) {
            string deleteCommand = "rm " + fileName;
            system(deleteCommand.c_str());
        }
    }

    msleep(1000);
    clearDirectory(".cached");

    cleanup = false;
    
    for (int i = 0; i < max_File_Size; i++) {
        int file_size = (1 << i);
        string fileName = "./client/test" + to_string(file_size) + ".txt";

        struct timespec ts_open_start, ts_open_end;

        get_time(&ts_open_start);

        int fd = open(fileName.c_str(), O_RDONLY);
        
        get_time(&ts_open_end);

        if (fd == -1) {
            continue;
        }

        struct timespec ts_read_start, ts_read_end;

        get_time(&ts_read_start);

        int cur_offset = 0;
        int num_bytes_read = 0;
        const int buf_size = 131072; // 1 MB
        char buf[buf_size + 1];
        while (num_bytes_read < file_size) {
            int res = pread(fd, buf, buf_size, cur_offset); //read(fd, buf, 131072);
            cur_offset += res;
            num_bytes_read += res;
            if (file_size < 10) {
                printf("%s\n", buf);
            }
        }

        get_time(&ts_read_end);

        struct timespec ts_close_start, ts_close_end;

        get_time(&ts_close_start);

        int res = close(fd);

        get_time(&ts_close_end);

        if (fd == -1) {
            printf("Failed to close file %s\n", fileName);
        }

        msleep(100);
        
        printf(
            "Open   (ms) : %.3f \t Read (ms) : %.3f \t Close (ms) : %.3f \t",
            get_time_diff(&ts_open_start, &ts_open_end),
            get_time_diff(&ts_read_start, &ts_read_end),
            get_time_diff(&ts_close_start, &ts_close_end));

        printf("File size (bytes) = %d \n", file_size);

        if (cleanup) {
            string deleteCommand = "rm " + fileName;
            system(deleteCommand.c_str());
        }
    }
    return 0;
}


inline void get_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC, ts);
}
inline double get_time_diff(struct timespec *before, struct timespec *after) {
    double delta_s = after->tv_sec - before->tv_sec;
    double delta_ns = after->tv_nsec - before->tv_nsec;

    return (delta_s + (delta_ns * 1e-9)) * ((double)1e3);
}
int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0) {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

void getRandomText(vector<string> &data, int file_size) {
    int num_bytes_written = 0;
    while (num_bytes_written < file_size) {
        string s;
        for (int i = 0; i < 131072 && num_bytes_written < file_size;
             i++, num_bytes_written++) {
            s.push_back((rand() % 26) + 'a');
        }
        data.push_back(s);
    }
}

void clearDirectory(string directory) {
    string deleteCommand = "rm -rf " + directory;
    system(deleteCommand.c_str());
    string mkdirCommand = "mkdir " + directory;
    system(mkdirCommand.c_str());
}
