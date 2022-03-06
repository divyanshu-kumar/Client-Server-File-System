// basic file operations
#include <assert.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <ctime>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

using namespace std;

const int num_runs = 3;
const int one_kb = 1024;
const int one_mb = one_kb * one_kb;
const vector<int> file_sizes = {
    one_kb,
    one_kb * 10,
    one_kb * 100,
    one_kb * 500,
    one_mb,
    one_mb * 5,
    one_mb * 10,
    //one_mb * 25,
    one_mb * 50,
    //one_mb * 75,
    one_mb * 100,
    //one_mb * 200,
    one_mb * 500,
    one_mb * 1024
};

struct time_statistics {
    int file_size;
    double create_time, write_time, close_time, first_open_time,
        cached_open_time, read_time, close_without_write_time,
        unlink_time;
};

void get_time(struct timespec *ts);
double get_time_diff(struct timespec *before, struct timespec *after);
int msleep(long msec);
void getRandomText(vector<string> &data, int file_size);
void clearDirectory(string directory);
int getFileSize(int i) {
    return file_sizes[i];
}

const string cacheDirectory = "./.cached/";
const string mountDirectory = "./client/";
const int max_File_Size = file_sizes.size();

vector<vector<const char *>> data_c_str(max_File_Size);
vector<vector<string>> data(max_File_Size);
void fillData() {
      for (int i = 0; i < max_File_Size; i++) {
          int file_size = getFileSize(i);
          getRandomText(data[i], file_size);
          for (auto &s : data[i]) {
              data_c_str[i].emplace_back(s.c_str());
          }
      }
}

void benchmarkApplication(string userId, vector<struct time_statistics> &ts) {
    ts = vector<struct time_statistics>(max_File_Size);

    struct stat buf;
    string cachedFolder = cacheDirectory + userId + "/";
    string currentUserFolder = mountDirectory + userId + "/";

    if (lstat(currentUserFolder.c_str(), &buf) == 0) {
        clearDirectory(currentUserFolder);
    } else {
        int res = mkdir(currentUserFolder.c_str(), 0777);
        if (res != 0) {
            printf("Failed to make directory %s\n", currentUserFolder.c_str());
        } else {
            printf("Made directory %s\n", currentUserFolder.c_str());
        }
    }

    for (int run = 0; run < num_runs; run++) {
        msleep(rand()%50);
        printf("%s : Create, write test..\n", userId.c_str());
        for (int i = 0; i < max_File_Size; i++) {
            int file_size = getFileSize(i);

            string fileName =
                currentUserFolder + "testFile_" + to_string(file_size) + ".txt";

            struct timespec ts_open_start, ts_open_end;

            get_time(&ts_open_start);

            int fd = open(fileName.c_str(), O_CREAT | O_WRONLY);

            get_time(&ts_open_end);

            if (fd == -1) {
                continue;
            }

            msleep(rand()%50);
            struct timespec ts_write_start, ts_write_end;

            get_time(&ts_write_start);

            int cur_offset = 0;
            for (int chunk = 0; chunk < data_c_str[i].size(); chunk++) {
                int res = pwrite(fd, data_c_str[i][chunk], data[i][chunk].size(), cur_offset);
                cur_offset += data[i][chunk].size();
            }

            get_time(&ts_write_end);

            struct timespec ts_close_start, ts_close_end;

            get_time(&ts_close_start);

            int res = close(fd);

            get_time(&ts_close_end);

            if (res == -1) {
                printf("Failed to close the file %s.\n", fileName.c_str());
            }

            ts[i].file_size = file_size;
            if (run == 0) {
                ts[i].create_time = get_time_diff(&ts_open_start, &ts_open_end);
                ts[i].write_time = get_time_diff(&ts_write_start, &ts_write_end);
                ts[i].close_time = get_time_diff(&ts_close_start, &ts_close_end);
            }
            else {
                ts[i].create_time = min(ts[i].create_time, get_time_diff(&ts_open_start, &ts_open_end));
                ts[i].write_time = min(ts[i].write_time, get_time_diff(&ts_write_start, &ts_write_end));
                ts[i].close_time = min(ts[i].close_time, get_time_diff(&ts_close_start, &ts_close_end));
            }
        }

        msleep(500 + (rand()%500));
        
        string clearCommand = "rm " + cachedFolder + "*";
        int tempres = system(clearCommand.c_str());
        if (tempres != 0) {
            printf("Failed to delete the cache folder %s.\n", cachedFolder.c_str());
        }
        // for (int i = 0; i < max_File_Size; i++) {
        //     int file_size = getFileSize(i);

        //     string fileName =
        //         cachedFolder + "testFile_" + to_string(file_size) + ".txt";
        //     int res = unlink(fileName.c_str());
        //     if (res == -1) {
        //         printf("Failed to delete the file %s.\n", fileName.c_str());
        //     }
        // }

        msleep(500 + (rand()%500));

        printf("%s : Cold Open, Read test..\n", userId.c_str());
        for (int i = 0; i < max_File_Size; i++) {
            msleep(rand()%50);
            int file_size = getFileSize(i);

            string fileName =
                currentUserFolder + "testFile_" + to_string(file_size) + ".txt";

            struct timespec ts_open_start, ts_open_end;

            get_time(&ts_open_start);

            int fd = open(fileName.c_str(), O_RDONLY);

            get_time(&ts_open_end);

            if (fd == -1) {
                continue;
            }

            int cur_offset = 0;
            int num_bytes_read = 0;
            const int buf_size = 131072;  // 1 MB
            char buf[buf_size + 1];

            struct timespec ts_read_start, ts_read_end;

            get_time(&ts_read_start);

            while (num_bytes_read < file_size) {
                int res = pread(fd, buf, buf_size, cur_offset);
                cur_offset += res;
                num_bytes_read += res;
            }

            get_time(&ts_read_end);

            struct timespec ts_close_start, ts_close_end;

            get_time(&ts_close_start);

            int res = close(fd);

            get_time(&ts_close_end);

            if (res == -1) {
                printf("Failed to close file %s\n", fileName.c_str());
            }

            msleep(rand()%100);

            if (run == 0) {
                ts[i].first_open_time = get_time_diff(&ts_open_start, &ts_open_end);
                ts[i].read_time = get_time_diff(&ts_read_start, &ts_read_end);
                ts[i].close_without_write_time =
                    get_time_diff(&ts_close_start, &ts_close_end);
            }
            else {
                ts[i].first_open_time = min(ts[i].first_open_time, get_time_diff(&ts_open_start, &ts_open_end));
                ts[i].read_time = min(ts[i].read_time, get_time_diff(&ts_read_start, &ts_read_end));
                ts[i].close_without_write_time = min(ts[i].close_without_write_time,
                    get_time_diff(&ts_close_start, &ts_close_end));
            }
        }
	msleep(500 + (rand()%500));
        printf("%s : Warm Open, Close test..\n", userId.c_str());
        for (int i = 0; i < max_File_Size; i++) {
            msleep(rand()%100);
            int file_size = getFileSize(i);

            string fileName =
                currentUserFolder + "testFile_" + to_string(file_size) + ".txt";

            struct timespec ts_open_start, ts_open_end;

            get_time(&ts_open_start);

            int fd = open(fileName.c_str(), O_RDONLY);

            get_time(&ts_open_end);

            if (fd == -1) {
                continue;
            }

            int res = close(fd);

            if (res == -1) {
                printf("Failed to close file %s\n", fileName.c_str());
            }

            msleep(rand()%100);

            struct timespec ts_unlink_start, ts_unlink_end;

            get_time(&ts_unlink_start);

            res = unlink(fileName.c_str());

            get_time(&ts_unlink_end);

            if (res == -1) {
                printf("Failed to delete file %s.\n", fileName.c_str());
            }

            if (run == 0) {
                ts[i].cached_open_time = get_time_diff(&ts_open_start, &ts_open_end);
                ts[i].unlink_time = get_time_diff(&ts_unlink_start, &ts_unlink_end);
            }
            else {
                ts[i].cached_open_time = min(ts[i].cached_open_time, get_time_diff(&ts_open_start, &ts_open_end));
                ts[i].unlink_time = min(ts[i].unlink_time, get_time_diff(&ts_unlink_start, &ts_unlink_end));
            }
        }
        msleep(500 + (rand()%500));
    }
    msleep(500 + (rand()%500));
    clearDirectory(cachedFolder.c_str());
}

int main(int argc, char *argv[]) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    cout.tie(nullptr);
    srand(time(NULL));

    int numProcesses = 1;
    if (argc > 1) {
        int proc_arg = atoi(argv[1]);
        if (proc_arg > 0) {
            if (proc_arg < 50) {
                numProcesses = proc_arg;
            } else {
                numProcesses = 50;
            }
        }
    }
    fillData();
    vector<std::thread> threadPool;
    vector<vector<struct time_statistics>> stats(numProcesses);

    vector<string> userIds(numProcesses);
    for (int i = 0; i < userIds.size(); i++) {
        userIds[i] = to_string(i) + "_" + to_string(rand() % 1000);
    }
    for (int i = 0; i < numProcesses; i++) {
        threadPool.push_back(
            std::thread(&benchmarkApplication, userIds[i], std::ref(stats[i])));
        printf("Starting thread with id = %d.\n", i);
    }

    for (int i = 0; i < numProcesses; i++) {
        threadPool[i].join();
        printf("Joined thread with id = %d.\n", i);
    }

    for (int i = 0; i < numProcesses; i++) {
        printf("*****Proc id = %d******\n", i);
        for (int j = 0; j < stats[i].size(); j++) {
            printf(
                "Create = %-8.2f \t Write = %-8.2f \t Close = %-8.2f \t "
                "First Open = %-8.2f \t Cached Open = %-8.2f \t Read = %-8.2f \t"
                "Read Close = %-8.2f \t File Size = %-10d\n",
                stats[i][j].create_time, stats[i][j].write_time,
                stats[i][j].close_time, stats[i][j].first_open_time,
                stats[i][j].cached_open_time, stats[i][j].read_time,
                stats[i][j].close_without_write_time, stats[i][j].file_size);
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

