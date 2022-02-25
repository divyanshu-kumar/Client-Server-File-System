#pragma once

#include <cstdint>
#include <string>

#include "afsfuse.grpc.pb.h"

afsfuse::File MakeFile(std::string path);
afsfuse::FileContent MakeFileContent(std::string name, const void* data, size_t data_len);
