# Client-Server-File-System

Cloudlab experiment : Group18

FUSE Setup steps : 

sudo apt-get update
sudo apt-get -y install python3-pip
pip3 install --user meson
sudo apt-get install meson
sudo apt-get install ninja-build
wget https://github.com/libfuse/libfuse/releases/download/fuse-3.10.5/fuse-3.10.5.tar.xz
tar -xvf fuse-3.10.5.tar.xz
cd fuse-3.10.5
mkdir build
cd build
meson ..
ninja
sudo python3 -m pytest test/
