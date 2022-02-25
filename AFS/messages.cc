#include "messages.h"

afsfuse::File MakeFile(std::string path)
{
    afsfuse::File file;
    file.set_path(path);
    return file;
}

afsfuse::FileContent MakeFileContent(std::string name, const void* data, size_t data_len)
{
    afsfuse::FileContent fc;
    fc.set_name(std::move(name));
    fc.set_content(data, data_len);
    return fc;
}
