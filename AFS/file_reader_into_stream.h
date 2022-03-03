#pragma once

#include <cstdint>
#include <string>
#include "sys/errno.h"
#include <iostream>

#include "sequential_file_reader.h"
#include "messages.h"
#include "utils.h"

template <class StreamWriter>
class FileReaderIntoStream : public SequentialFileReader {
public:
    FileReaderIntoStream(const std::string &rootDir, const std::string& filename, StreamWriter& writer)
        : SequentialFileReader(rootDir, filename)
        , m_writer(writer)
    {
    }

    using SequentialFileReader::SequentialFileReader;
    using SequentialFileReader::operator=;

protected:
    virtual void OnChunkAvailable(const void* data, size_t size) override
    {
        const std::string remote_filename = GetFilePath();
        // std::cout << __func__ << " \t : Filename = " << GetFilePath() << " and remote_filename = " << remote_filename << std::endl;
        auto fc = MakeFileContent(GetFilePath(), data, size);
        if (! m_writer.Write(fc)) {
            raise_from_system_error_code("The server aborted the connection.", ECONNRESET);
        }
    }

private:
    StreamWriter& m_writer;
};
