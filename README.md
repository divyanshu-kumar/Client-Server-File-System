# AFS (like) Distributed File System

To install the dependencies : 
```
source installation_script.sh
```

Then switch to AFS directory which contains all the code and instructions for building the project.

To make the executables, in AFS directory, execute the following:
```
source make.sh
```

This will generate the ececutables, afsfuse_client and afsfuse_server

To run server:
```
sudo ./afsfuse_server
```
(It will create a folder 'server' automatically in the executables directory)

To run client:
```
sudo ./afsfuse_client -f client/ --server=[IP Address of SERVER]:50051
```
(Here client/ is the folder to be mounted which will be made if not present, and --server option is optional and the default value is localhost)

To make and use benchmarking code:
```
g++ -pthread -o bench bench.cpp
sudo ./bench [Number of concurrent applications to test]
```

Rubrics:

1.1 POSIX Semantics: Implemented the Fuse equivalent in afsfuse_client.cc and the RPC calls of client are defined in AfsClient.h and the RPC calls of server are implemented in afsfuse_server.cc.

1.2 The interface between client and server is defined in afsfuse.proto file
      Whole file caching - Implemented in afsfuse_client.cc (Supporting function - fetchFile())
      Client rechecks for file modification time on server every time an open is called on a file.
      Update Visibility - Flush on Close and Last Writer Wins - Both are supported by the function client_release() in afsfuse_client.cc
      Dealing with Stale Cache - On each open() call the modification time of file is stat'd from server (refer client_open() in afsfuse_client.cc)

1.3 Crash Consistency -
      Client renames files before saving on close (atomic operation ensures recovery files can be distinguished from half written files).
      Functionality implemented in client_release()
      File system consistency configuration chosen - ext2 - On reboot, scans the whole File system to check for recovery files and garbage collect temp files which were not closed.
      Design for server persistence - Server also fetches file in a temp file and renames after. If server crashes before complete file is streamed to server, the client retries the complete streaming operation from starting.
    Crash Recovery Protocol -
      If client crashes, it scans the file system after restarting for any recovery file and sends them to server when found.
      If server crashes, no state is maintained on server but client has a retry mechanism with exponential backoff timeout mechanism. 
      Distinguishing RPC errors from operation failure - Server always sends RPC::Status::OK on execution of a RPC. So if the client receives any other status error message, it knows the RPC failed instead of the intended operation. The ensuing recovery is handled differently for idempotent and non-idempotent operations.

2.1 Performance (Benchmarking code is present in bench.cpp) - 
      Details present in both presentation and report.
      Performance improvement ideas implemented - 
            a. Pre-fetching files before open based on current directory heuristic
            b. Making close non-blocking by using a concurrent thread to send files to server on close
            c. Distributing fsync randomly amongst writes so that it doesn't take too long to flush on close()
            d. Making file transfer between client and server as streaming with 4 MB chunk size.
            e. Not transferring unmodified files from client to server and vice versa.

2.2 Reliability-
      Various controlled crash points are added to demonstrate reliability.
      Reliability measures - If Client crashes, files with close in progress are recovered. If server crashes then RPC operations are retrues. Temp files (copy of original files made on open()) are discarded when executing the recovery mechanism. Last writer wins (file flush/write is atomic), multiple writes are supported parallely and only atomic file flush persists at the end.      

-Various videos added in folder "Reliability-Demo-Videos" to show the behavior of system under these conditions.

Hardware platform used for experiments - (CloudLab, 4 node cluster configuration)
Architecture:        x86_64
CPU op-mode(s):      32-bit, 64-bit
CPU(s):              40
Thread(s) per core:  2
Model name:          Intel(R) Xeon(R) CPU E5-2660 v3 @ 2.60GHz
CPU MHz:             1197.416
CPU max MHz:         3300.0000
CPU min MHz:         1200.0000
Virtualization:      VT-x
L1d cache:           32K
L1i cache:           32K
L2 cache:            256K
L3 cache:            25600K