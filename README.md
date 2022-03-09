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

The benchmarking code is capable of testing and reporting the time it takes to create, write to, read a, open(cold cache + warm cache) and close a file. 
It does the same test 3 times per application count and reports the numbers for each file size mentioned in the program.
Sample benchmarking output:
Starting thread with id = 0.
Made directory ./client/0_129/
0_129 : Create, write test..
0_129 : Cold Open, Read test..
0_129 : Warm Open, Close test..
0_129 : Create, write test..
0_129 : Cold Open, Read test..
0_129 : Warm Open, Close test..
0_129 : Create, write test..
0_129 : Cold Open, Read test..
0_129 : Warm Open, Close test..
Joined thread with id = 0.
*****Proc id = 0******
Create = 2.26     	 Write = 0.11     	 Close = 18.45    	 First Open = 3.28     	 Cached Open = 2.25     	 Read = 0.10     	Read Close = 0.82     	 File Size = 1024      
Create = 2.59     	 Write = 0.13     	 Close = 15.92    	 First Open = 3.16     	 Cached Open = 2.04     	 Read = 0.11     	Read Close = 0.98     	 File Size = 10240     
Create = 2.74     	 Write = 0.28     	 Close = 20.10    	 First Open = 3.94     	 Cached Open = 2.29     	 Read = 0.36     	Read Close = 0.92     	 File Size = 102400    
Create = 3.21     	 Write = 1.21     	 Close = 4.89     	 First Open = 6.56     	 Cached Open = 3.28     	 Read = 0.77     	Read Close = 0.88     	 File Size = 512000    
Create = 4.08     	 Write = 1.89     	 Close = 15.69    	 First Open = 9.88     	 Cached Open = 4.61     	 Read = 1.16     	Read Close = 0.77     	 File Size = 1048576   
Create = 3.26     	 Write = 79.66    	 Close = 8.57     	 First Open = 22.62    	 Cached Open = 13.63    	 Read = 4.14     	Read Close = 0.72     	 File Size = 5242880   
Create = 3.45     	 Write = 194.61   	 Close = 7.79     	 First Open = 38.82    	 Cached Open = 21.41    	 Read = 8.08     	Read Close = 0.92     	 File Size = 10485760  




RUBRICS:

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
