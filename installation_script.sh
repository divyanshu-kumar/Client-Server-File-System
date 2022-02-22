sudo apt-get update
sudo apt-get -y install python3-pip
pip3 install --user meson
sudo apt-get -y install meson
sudo apt-get -y install ninja-build
wget https://github.com/libfuse/libfuse/releases/download/fuse-3.10.5/fuse-3.10.5.tar.xz
tar -xvf fuse-3.10.5.tar.xz
cd fuse-3.10.5
mkdir build
cd build
meson ..
ninja
pip3 install pytest
sudo python3 -m pytest test/
sudo ninja install
sudo ln -s /usr/local/lib/x86_64-linux-gnu/libfuse3.so.3.10.5 /lib/x86_64-linux-gnu/libfuse3.so.3
echo '*********************FUSE installed*********************'

cd 
export MY_INSTALL_DIR=$HOME/.local
mkdir -p $MY_INSTALL_DIR
export PATH="$MY_INSTALL_DIR/bin:$PATH"
wget -q -O cmake-linux.sh https://github.com/Kitware/CMake/releases/download/v3.19.6/cmake-3.19.6-Linux-x86_64.sh
sh cmake-linux.sh -- --skip-license --prefix=$MY_INSTALL_DIR
rm cmake-linux.sh
cmake --version
sudo apt install -y build-essential autoconf libtool pkg-config
git clone --recurse-submodules -b v1.43.0 https://github.com/grpc/grpc

cd grpc
mkdir -p cmake/build
pushd cmake/build
cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      ../..
make -j
make install
popd
cd examples/cpp/helloworld
mkdir -p cmake/build
pushd cmake/build
cmake -DCMAKE_PREFIX_PATH=$MY_INSTALL_DIR ../..
make -j
