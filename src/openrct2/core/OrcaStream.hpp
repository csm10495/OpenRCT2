/*****************************************************************************
 * Copyright (c) 2014-2019 OpenRCT2 developers
 *
 * For a complete list of all authors, please refer to contributors.md
 * Interested in contributing? Visit https://github.com/OpenRCT2/OpenRCT2
 *
 * OpenRCT2 is licensed under the GNU General Public License version 3.
 *****************************************************************************/

#pragma once

#include "Crypt.h"
#include "FileStream.h"
#include "MemoryStream.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stack>
#include <vector>

namespace OpenRCT2
{
    class OrcaStream
    {
    public:
        enum class Mode
        {
            READING,
            WRITING,
        };

        static constexpr uint32_t COMPRESSION_NONE = 0;
        static constexpr uint32_t COMPRESSION_GZIP = 1;

    private:
#pragma pack(push, 1)
        struct Header
        {
            uint32_t Magic{};
            uint32_t TargetVersion{};
            uint32_t MinVersion{};
            uint32_t NumChunks{};
            uint64_t UncompressedSize{};
            uint32_t Compression{};
            uint64_t CompressedSize{};
            std::array<uint8_t, 20> Sha1{};
            uint8_t padding[8];
        };
        static_assert(sizeof(Header) == 64, "Header should be 64 bytes");

        struct ChunkEntry
        {
            uint32_t Id{};
            uint64_t Offset{};
            uint64_t Length{};
        };
#pragma pack(pop)

        IStream* _stream;
        Mode _mode;
        Header _header;
        std::vector<ChunkEntry> _chunks;
        MemoryStream _buffer;
        ChunkEntry _currentChunk;

    public:
        OrcaStream(IStream& stream, Mode mode)
        {
            _stream = &stream;
            _mode = mode;
            if (mode == Mode::READING)
            {
                _header = _stream->ReadValue<Header>();

                _chunks.clear();
                for (uint32_t i = 0; i < _header.NumChunks; i++)
                {
                    auto entry = _stream->ReadValue<ChunkEntry>();
                    _chunks.push_back(entry);
                }

                // Read compressed data into buffer (read in blocks)
                _buffer = MemoryStream{};
                uint8_t temp[2048];
                uint64_t bytesLeft = _header.CompressedSize;
                do
                {
                    auto readLen = std::min(bytesLeft, sizeof(temp));
                    _stream->Read(temp, readLen);
                    _buffer.Write(temp, readLen);
                    bytesLeft -= readLen;
                } while (bytesLeft > 0);

                // Uncompress
                if (_header.Compression == COMPRESSION_GZIP)
                {
                    size_t outUncompressedSize{};
                    auto uncompressedData = util_zlib_inflate(reinterpret_cast<const uint8_t*>(_buffer.GetData()), _buffer.GetLength(), &outUncompressedSize);
                    if (_header.UncompressedSize != outUncompressedSize)
                    {
                        // Warning?
                    }
                    _buffer.Clear();
                    _buffer.Write(uncompressedData, outUncompressedSize);
                    std::free(uncompressedData);
                }
            }
            else
            {
                _header = {};
                _header.Compression = COMPRESSION_GZIP;

                _buffer = MemoryStream{};
            }
        }

        OrcaStream(const OrcaStream&) = delete;

        ~OrcaStream()
        {
            if (_mode == Mode::READING)
            {
            }
            else
            {
                const void* uncompressedData = _buffer.GetData();
                const uint64_t uncompressedSize = _buffer.GetLength();

                _header.NumChunks = static_cast<uint32_t>(_chunks.size());
                _header.UncompressedSize = uncompressedSize;
                _header.CompressedSize = uncompressedSize;
                _header.Sha1 = Crypt::SHA1(uncompressedData, uncompressedSize);

                // Compress data
                std::optional<std::vector<uint8_t>> compressedBytes;
                if (_header.Compression == COMPRESSION_GZIP)
                {
                    compressedBytes = util_zlib_deflate(reinterpret_cast<const uint8_t*>(uncompressedData), uncompressedSize);
                    if (compressedBytes)
                    {
                        _header.CompressedSize = compressedBytes->size();
                    }
                    else
                    {
                        // Compression failed
                        _header.Compression = COMPRESSION_NONE;
                    }
                }

                // Write header and chunk table
                _stream->WriteValue(_header);
                for (const auto& chunk : _chunks)
                {
                    _stream->WriteValue(chunk);
                }

                // Write chunk data
                if (compressedBytes)
                {
                    _stream->Write(compressedBytes->data(), compressedBytes->size());
                }
                else
                {
                    _stream->Write(uncompressedData, uncompressedSize);
                }
            }
        }

        Mode GetMode() const
        {
            return _mode;
        }

        Header& GetHeader()
        {
            return _header;
        }

        const Header& GetHeader() const
        {
            return _header;
        }

        template<typename TFunc> bool ReadWriteChunk(uint32_t chunkId, TFunc f)
        {
            if (_mode == Mode::READING)
            {
                if (SeekChunk(chunkId))
                {
                    ChunkStream stream(_buffer, _mode);
                    f(stream);
                    return true;
                }
                else
                {
                    return false;
                }
            }
            else
            {
                _currentChunk.Id = chunkId;
                _currentChunk.Offset = _buffer.GetPosition();
                _currentChunk.Length = 0;
                ChunkStream stream(_buffer, _mode);
                f(stream);
                _currentChunk.Length = static_cast<uint64_t>(_buffer.GetPosition()) - _currentChunk.Offset;
                _chunks.push_back(_currentChunk);
                return true;
            }
        }

    private:
        bool SeekChunk(uint32_t id)
        {
            auto result = std::find_if(_chunks.begin(), _chunks.end(), [id](const ChunkEntry& e) { return e.Id == id; });
            if (result != _chunks.end())
            {
                auto offset = result->Offset;
                _buffer.SetPosition(offset);
                return true;
            }
            return false;
        }

    public:
        class ChunkStream
        {
        private:
            struct ArrayState
            {
                std::streampos StartPos{};
                std::streampos LastPos{};
                size_t Count{};
                size_t ElementSize{};
            };

            MemoryStream& _buffer;
            Mode _mode;
            std::stack<ArrayState> _arrayStack;

        public:
            ChunkStream(MemoryStream& buffer, Mode mode)
                : _buffer(buffer)
                , _mode(mode)
            {
            }

            Mode GetMode() const
            {
                return _mode;
            }

            MemoryStream& GetStream()
            {
                return _buffer;
            }

            void ReadWrite(void* addr, size_t len)
            {
                if (_mode == Mode::READING)
                {
                    ReadBuffer(addr, len);
                }
                else
                {
                    WriteBuffer(addr, len);
                }
            }

            template<typename T> void ReadWrite(T& v)
            {
                ReadWrite(const_cast<void*>(reinterpret_cast<const void*>(&v)), sizeof(T));
            }

            template<typename TMem, typename TSave> void ReadWriteAs(TMem& v)
            {
                TSave sv;
                if (_mode != Mode::READING)
                {
                    sv = v;
                }
                ReadWrite(reinterpret_cast<void*>(&sv), sizeof(TSave));
                if (_mode == Mode::READING)
                {
                    v = static_cast<TMem>(sv);
                }
            }

            template<typename T> T Read()
            {
                T v{};
                ReadWrite(v);
                return v;
            }

#if defined(_MSC_VER)
            template<> void ReadWrite(std::string_view& v) = delete;

            template<>
#endif
            void ReadWrite(std::string& v)
            {
                if (_mode == Mode::READING)
                {
                    v = ReadString();
                }
                else
                {
                    WriteString(v);
                }
            }

            template<typename T> void Write(const T& v)
            {
                if (_mode == Mode::READING)
                {
                    T temp;
                    ReadWrite(temp);
                }
                else
                {
                    ReadWrite(v);
                }
            }

#if defined(_MSC_VER)
            template<>
#endif
            void Write(const std::string_view& v)
            {
                if (_mode == Mode::READING)
                {
                    std::string temp;
                    ReadWrite(temp);
                }
                else
                {
                    WriteString(v);
                }
            }

#if defined(_MSC_VER)
            template<>
#endif
            void Write(const std::string& v)
            {
                Write(std::string_view(v));
            }

            template<typename TVec, typename TFunc> void ReadWriteVector(TVec& vec, TFunc f)
            {
                if (_mode == Mode::READING)
                {
                    auto count = BeginArray();
                    vec.clear();
                    for (size_t i = 0; i < count; i++)
                    {
                        auto& el = vec.emplace_back();
                        f(el);
                        NextArrayElement();
                    }
                    EndArray();
                }
                else
                {
                    BeginArray();
                    for (auto& el : vec)
                    {
                        f(el);
                        NextArrayElement();
                    }
                    EndArray();
                }
            }

            template<typename TArr, size_t TArrSize, typename TFunc> void ReadWriteArray(TArr (&arr)[TArrSize], TFunc f)
            {
                if (_mode == Mode::READING)
                {
                    auto count = BeginArray();
                    for (auto& el : arr)
                    {
                        el = {};
                    }
                    for (size_t i = 0; i < count; i++)
                    {
                        if (i < TArrSize)
                        {
                            f(arr[i]);
                        }
                        NextArrayElement();
                    }
                    EndArray();
                }
                else
                {
                    BeginArray();
                    for (auto& el : arr)
                    {
                        if (f(el))
                        {
                            NextArrayElement();
                        }
                    }
                    EndArray();
                }
            }

        private:
            void ReadBuffer(void* dst, size_t len)
            {
                _buffer.Read(dst, len);
            }

            void WriteBuffer(const void* buffer, size_t len)
            {
                _buffer.Write(buffer, len);
            }

            std::string ReadString()
            {
                std::string buffer;
                buffer.reserve(64);
                while (true)
                {
                    char c;
                    ReadBuffer(&c, sizeof(c));
                    if (c == 0)
                    {
                        break;
                    }
                    buffer.push_back(c);
                }
                buffer.shrink_to_fit();
                return buffer;
            }

            void WriteString(const std::string_view& s)
            {
                char nullt = '\0';
                auto len = s.find('\0');
                if (len == std::string_view::npos)
                {
                    len = s.size();
                }
                _buffer.Write(s.data(), len);
                _buffer.Write(&nullt, sizeof(nullt));
            }

            size_t BeginArray()
            {
                auto& arrayState = _arrayStack.emplace();
                if (_mode == Mode::READING)
                {
                    arrayState.Count = Read<uint32_t>();
                    arrayState.ElementSize = Read<uint32_t>();
                    arrayState.LastPos = _buffer.GetPosition();
                    return arrayState.Count;
                }
                else
                {
                    arrayState.Count = 0;
                    arrayState.ElementSize = 0;
                    arrayState.StartPos = _buffer.GetPosition();
                    Write<uint32_t>(0);
                    Write<uint32_t>(0);
                    arrayState.LastPos = _buffer.GetPosition();
                    return 0;
                }
            }

            bool NextArrayElement()
            {
                auto& arrayState = _arrayStack.top();
                if (_mode == Mode::READING)
                {
                    if (arrayState.Count == 0)
                    {
                        return false;
                    }
                    if (arrayState.ElementSize != 0)
                    {
                        arrayState.LastPos += arrayState.ElementSize;
                        _buffer.SetPosition(arrayState.LastPos);
                    }
                    arrayState.Count--;
                    return arrayState.Count == 0;
                }
                else
                {
                    auto lastElSize = static_cast<size_t>(_buffer.GetPosition()) - arrayState.LastPos;
                    if (arrayState.Count == 0)
                    {
                        // Set array element size based on first element size
                        arrayState.ElementSize = lastElSize;
                    }
                    else if (arrayState.ElementSize != lastElSize)
                    {
                        // Array element size was different from first element so reset it
                        // to dynamic
                        arrayState.ElementSize = 0;
                    }
                    arrayState.Count++;
                    arrayState.LastPos = _buffer.GetPosition();
                    return true;
                }
            }

            void EndArray()
            {
                auto& arrayState = _arrayStack.top();
                if (_mode == Mode::READING)
                {
                }
                else
                {
                    size_t backupPos = _buffer.GetPosition();
                    if (backupPos != static_cast<size_t>(arrayState.StartPos) + 8 && arrayState.Count == 0)
                    {
                        throw std::runtime_error("Array data was written but no elements were added.");
                    }
                    _buffer.SetPosition(arrayState.StartPos);
                    Write(static_cast<uint32_t>(arrayState.Count));
                    Write(static_cast<uint32_t>(arrayState.ElementSize));
                    _buffer.SetPosition(backupPos);
                }
                _arrayStack.pop();
            }
        };
    };
} // namespace OpenRCT2