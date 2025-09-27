/***************************************************************************
    Binary File Loader. 
    
    Handles loading an individual binary file to memory.
    Supports reading bytes, words and longs from this area of memory.

    Copyright Chris White.
    See license.txt for more details.

    Refactored to remove Boost and Dirent dependency.
    Uses std::filesystem for directory scanning and a small CRC32 impl.
***************************************************************************/

#include <iostream>
#include <fstream>
#include <cstddef>
#include <unordered_map>
#include <cstdint>
#include <filesystem>

#include "stdint.hpp"
#include "romloader.hpp"
#include "frontend/config.hpp"

// -------------------------------
// Local CRC32 (IEEE 802.3) impl
// -------------------------------
namespace {

inline const uint32_t* crc32_table()
{
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    return table;
}

inline uint32_t crc32(const void* data, std::size_t n)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t c = 0xFFFFFFFFu;
    const uint32_t* T = crc32_table();
    for (std::size_t i = 0; i < n; ++i)
        c = T[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

} // namespace

static std::unordered_map<int, std::string> map;
static bool map_created;

RomLoader::RomLoader()
{
    rom = NULL;
    map_created = false;
    loaded = false;
}

RomLoader::~RomLoader()
{
    if (rom != NULL)
        delete[] rom;
}

void RomLoader::init(const uint32_t length)
{
    load = config.data.crc32 ? &RomLoader::load_crc32 : &RomLoader::load_rom;
    this->length = length;
    rom = new uint8_t[length];
}

void RomLoader::unload(void)
{
    delete[] rom;
    rom = NULL;
}

int RomLoader::load_rom(const char* filename, const int offset, const int length, const int expected_crc, const uint8_t interleave, const bool verbose)
{
    std::string path = config.data.rom_path;
    path += std::string(filename);

    std::ifstream src(path, std::ios::in | std::ios::binary);
    if (!src)
    {
        if (verbose) std::cout << "cannot open rom: " << path << std::endl;
        loaded = false;
        return 1;
    }

    char* buffer = new char[length];
    src.read(buffer, length);

    const uint32_t crc = crc32(buffer, static_cast<std::size_t>(src.gcount()));

    if (expected_crc != static_cast<int>(crc))
    {
        if (verbose)
            std::cout << std::hex
                      << filename << " has incorrect checksum.\nExpected: "
                      << expected_crc << " Found: " << crc << std::endl;

        delete[] buffer;
        src.close();
        return 1;
    }

    for (int i = 0; i < length; i++)
    {
        rom[(i * interleave) + offset] = buffer[i];
    }

    delete[] buffer;
    src.close();
    loaded = true;
    return 0;
}

int RomLoader::create_map()
{
    map_created = true;
    namespace fs = std::filesystem;

    std::string path = config.data.rom_path;
    fs::path dir(path);

    if (!fs::exists(dir) || !fs::is_directory(dir)) {
        std::cout << "Warning: Could not open ROM directory - " << path << std::endl;
        return 1;
    }

    for (auto& entry : fs::directory_iterator(dir))
    {
        if (!entry.is_regular_file()) continue;
        std::ifstream src(entry.path(), std::ios::in | std::ios::binary);
        if (!src) continue;

        char* buffer = new char[length];
        src.read(buffer, length);
        const uint32_t c = crc32(buffer, static_cast<std::size_t>(src.gcount()));
        map.insert({ static_cast<int>(c), entry.path().string() });

        delete[] buffer;
        src.close();
    }

    if (map.empty())
        std::cout << "Warning: Could not create CRC32 Map. Did you copy the ROM files into the directory? " << std::endl;

    return 0;
}

int RomLoader::load_crc32(const char* debug, const int offset, const int length, const int expected_crc, const uint8_t interleave, const bool verbose)
{
    if (!map_created)
        create_map();

    if (map.empty())
        return 1;

    auto search = map.find(expected_crc);

    if (search == map.end())
    {
        if (verbose) std::cout << "Unable to locate rom in path: " << config.data.rom_path
                                << " possible name: " << debug << " crc32: 0x" << std::hex << expected_crc << std::endl;
        loaded = false;
        return 1;
    }

    std::string file = search->second;
    std::ifstream src(file, std::ios::in | std::ios::binary);
    if (!src)
    {
        if (verbose) std::cout << "cannot open rom: " << file << std::endl;
        loaded = false;
        return 1;
    }

    char* buffer = new char[length];
    src.read(buffer, length);

    for (int i = 0; i < length; i++)
        rom[(i * interleave) + offset] = buffer[i];

    delete[] buffer;
    src.close();
    loaded = true;
    return 0;
}

int RomLoader::load_binary(const char* filename)
{
    std::ifstream src(filename, std::ios::in | std::ios::binary);
    if (!src)
    {
        std::cout << "cannot open file: " << filename << std::endl;
        loaded = false;
        return 1;
    }

    length = filesize(filename);
    char* buffer = new char[length];
    src.read(buffer, length);
    rom = (uint8_t*) buffer;
    src.close();
    loaded = true;
    return 0;
}

int RomLoader::filesize(const char* filename)
{
    std::ifstream in(filename, std::ifstream::in | std::ifstream::binary);
    in.seekg(0, std::ifstream::end);
    int size = (int) in.tellg();
    in.close();
    return size; 
}
