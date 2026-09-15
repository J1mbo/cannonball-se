/***************************************************************************
    Binary File Loader.

    Handles loading an individual binary file to memory.
    Supports reading bytes, words and longs from this area of memory.

    Copyright Chris White.
    See license.txt for more details.

    Refactored to remove Boost and Dirent dependency.
    Uses std::filesystem for directory scanning and a small CRC32 impl.

    ZIP archive support is based on work by Endprodukt (cannonball-dx) and
    has been extended for CannonBall-SE: multi-location discovery (ROM
    directory, current directory, executable directory), collision logging,
    a name-mode fallback to the content-addressed index, and diagnostics
    query functions.
***************************************************************************/

#include <iostream>
#include <fstream>
#include <cstddef>
#include <unordered_map>
#include <set>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include <cctype>

#include <SDL.h>
#include <miniz.h>

#include "stdint.hpp"
#include "romloader.hpp"
#include "main.hpp"             // cannonball::verbose (-verbose command line option)
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

// Loose ROM candidates larger than this are never read into memory. No
// OutRun ROM chip exceeds 0x20000 bytes; this just stops a stray large
// file (or an archive with the wrong extension) from being slurped in.
constexpr uintmax_t kMaxLooseFileSize = 0x100000;

// Archive names accepted when searching the current working directory and
// the executable directory. Matched case-insensitively. We deliberately do
// NOT accept arbitrary *.zip files in those two locations, so we never open
// an unrelated archive found in, say, a Downloads folder or repo root.
static const char* const kKnownZipNames[] = {
    "outrun.zip",
    "outrunra.zip",
    "outrundx.zip",
    "outrundxa.zip",
    "outrundxj.zip",
    "outruneh.zip",
    "outruneha.zip",
    "outrundxeh.zip",
    "outrundxeha.zip",
    "outrunb.zip"
};

std::string to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool is_zip_path(const std::filesystem::path& path)
{
    return to_lower(path.extension().string()) == ".zip";
}

bool is_known_zip_name(const std::filesystem::path& path)
{
    const std::string name = to_lower(path.filename().string());
    for (const char* known : kKnownZipNames)
    {
        if (name == known)
            return true;
    }
    return false;
}

// Returns the regular files directly inside 'dir', sorted by filename.
// Any filesystem error (missing directory, permission denied, a broken
// entry mid-iteration, ...) simply truncates the result; this never throws.
std::vector<std::filesystem::path> list_regular_files_sorted(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> result;

    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return result;

    const fs::directory_iterator end;
    while (!ec && it != end)
    {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !entry_ec)
            result.push_back(it->path());
        it.increment(ec);
    }

    std::sort(result.begin(), result.end(),
        [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });

    return result;
}

struct RomKey
{
    uint32_t crc;
    uint32_t size;

    bool operator==(const RomKey& other) const
    {
        return crc == other.crc && size == other.size;
    }
};

struct RomKeyHash
{
    std::size_t operator()(const RomKey& key) const
    {
        // Multiplicative mix so the CRC contributes fully even when size_t is 32-bit.
        return static_cast<std::size_t>(key.crc * 2654435761u) ^
               static_cast<std::size_t>(key.size);
    }
};

enum class RomSourceType
{
    LooseFile,
    ZipEntry
};

struct RomSource
{
    RomSourceType type = RomSourceType::LooseFile;
    std::string path;          // loose file path, or archive path
    mz_uint file_index = 0;    // ZipEntry only
    std::string entry_name;    // ZipEntry only (name inside the archive)

    // Human-readable origin used by RomLoader::sources_used(): a loose
    // directory path, or an archive path.
    std::string source_label;
};

static std::unordered_map<RomKey, RomSource, RomKeyHash> rom_map;
static bool map_created = false;
static std::string mapped_rom_path;

// Distinct sources that have actually supplied a ROM since the index was
// last (re)built. Populated by successful loads, cleared in create_map().
static std::set<std::string> sources_used_set;

// Human-readable description of every location searched while building the
// index, in search order. Populated in create_map(), used for diagnostics
// on failure as well as for the success summary line.
static std::vector<std::string> search_location_list;

bool read_entire_file(const std::filesystem::path& path, std::vector<uint8_t>& buffer)
{
    namespace fs = std::filesystem;

    std::error_code ec;
    const uintmax_t file_size = fs::file_size(path, ec);
    if (ec || file_size > static_cast<uintmax_t>(std::numeric_limits<uint32_t>::max()))
        return false;

    buffer.resize(static_cast<std::size_t>(file_size));

    std::ifstream src(path, std::ios::in | std::ios::binary);
    if (!src)
        return false;

    if (!buffer.empty())
        src.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    return src.good() || (src.eof() && static_cast<std::size_t>(src.gcount()) == buffer.size());
}

bool read_exact_file(const std::filesystem::path& path, const int expected_length, std::vector<uint8_t>& buffer)
{
    if (expected_length < 0)
        return false;

    std::error_code ec;
    const uintmax_t file_size = std::filesystem::file_size(path, ec);
    if (ec || file_size != static_cast<uintmax_t>(expected_length))
        return false;

    buffer.resize(static_cast<std::size_t>(expected_length));

    std::ifstream src(path, std::ios::in | std::ios::binary);
    if (!src)
        return false;

    if (!buffer.empty())
        src.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));

    return static_cast<std::size_t>(src.gcount()) == buffer.size();
}

void add_loose_file_to_map(const std::filesystem::path& path, const std::string& source_label)
{
    std::vector<uint8_t> buffer;
    if (!read_entire_file(path, buffer))
        return;

    const RomKey key {
        crc32(buffer.data(), buffer.size()),
        static_cast<uint32_t>(buffer.size())
    };

    const auto existing = rom_map.find(key);
    if (existing != rom_map.end())
    {
        if (cannonball::verbose)
            std::cout << "ROM index: ignoring " << path.string()
                      << " (already provided by " << existing->second.path << ")" << std::endl;
        return;
    }

    RomSource source;
    source.type = RomSourceType::LooseFile;
    source.path = path.string();
    source.source_label = source_label;
    rom_map.emplace(key, std::move(source));
}

void add_zip_to_map(const std::filesystem::path& archive_path, const std::string& source_label)
{
    mz_zip_archive zip {};
    if (!mz_zip_reader_init_file(&zip, archive_path.string().c_str(), 0))
    {
        std::cerr << "Warning: Could not open ROM archive - " << archive_path.string() << std::endl;
        return;
    }

    const mz_uint file_count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < file_count; ++i)
    {
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat))
            continue;
        if (stat.m_is_directory || !stat.m_is_supported || stat.m_is_encrypted)
            continue;
        if (stat.m_uncomp_size > static_cast<mz_uint64>(std::numeric_limits<uint32_t>::max()))
            continue;

        const RomKey key {
            static_cast<uint32_t>(stat.m_crc32),
            static_cast<uint32_t>(stat.m_uncomp_size)
        };

        const auto existing = rom_map.find(key);
        if (existing != rom_map.end())
        {
            if (cannonball::verbose)
                std::cout << "ROM index: ignoring " << archive_path.string() << " -> " << stat.m_filename
                          << " (already provided by " << existing->second.path << ")" << std::endl;
            continue;
        }

        RomSource source;
        source.type = RomSourceType::ZipEntry;
        source.path = archive_path.string();
        source.file_index = i;
        source.entry_name = stat.m_filename;
        source.source_label = source_label;
        rom_map.emplace(key, std::move(source));
    }

    mz_zip_reader_end(&zip);
}

bool load_source(const RomSource& source, const int expected_length, std::vector<uint8_t>& buffer, const bool verbose)
{
    if (expected_length < 0)
        return false;

    if (source.type == RomSourceType::LooseFile)
    {
        if (!read_exact_file(source.path, expected_length, buffer))
        {
            if (verbose)
                std::cerr << "cannot read rom: " << source.path << std::endl;
            return false;
        }
        return true;
    }

    mz_zip_archive zip {};
    if (!mz_zip_reader_init_file(&zip, source.path.c_str(), 0))
    {
        if (verbose)
            std::cerr << "cannot open rom archive: " << source.path << std::endl;
        return false;
    }

    mz_zip_archive_file_stat stat {};
    if (!mz_zip_reader_file_stat(&zip, source.file_index, &stat) ||
        stat.m_is_directory || !stat.m_is_supported || stat.m_is_encrypted ||
        stat.m_uncomp_size != static_cast<mz_uint64>(expected_length))
    {
        if (verbose)
            std::cerr << "invalid rom entry in archive: " << source.path
                      << " -> " << source.entry_name << std::endl;
        mz_zip_reader_end(&zip);
        return false;
    }

    buffer.resize(static_cast<std::size_t>(expected_length));
    const mz_bool extracted = mz_zip_reader_extract_to_mem(
        &zip,
        source.file_index,
        buffer.data(),
        buffer.size(),
        0);

    if (!extracted && verbose)
    {
        const mz_zip_error err = mz_zip_get_last_error(&zip);
        std::cerr << "cannot extract rom from archive: " << source.path
                  << " -> " << source.entry_name
                  << " (" << mz_zip_get_error_string(err) << ")" << std::endl;
    }

    mz_zip_reader_end(&zip);
    return extracted == MZ_TRUE;
}

void copy_interleaved(uint8_t* destination, const std::vector<uint8_t>& buffer,
                      const int offset, const uint8_t interleave)
{
    for (std::size_t i = 0; i < buffer.size(); ++i)
        destination[(i * interleave) + static_cast<std::size_t>(offset)] = buffer[i];
}

} // namespace

RomLoader::RomLoader()
{
    rom = NULL;
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
    namespace fs = std::filesystem;

    const fs::path base(config.data.rom_path);
    const fs::path path = base / filename;

    std::error_code ec;
    const bool exists = fs::exists(path, ec) && !ec;

    if (exists)
    {
        std::vector<uint8_t> buffer;
        if (read_exact_file(path, length, buffer))
        {
            const uint32_t crc = crc32(buffer.data(), buffer.size());
            if (static_cast<uint32_t>(expected_crc) == crc)
            {
                copy_interleaved(rom, buffer, offset, interleave);
                loaded = true;
                sources_used_set.insert(base.string());
                return 0;
            }

            if (verbose)
                std::cout << std::hex
                          << filename << " has incorrect checksum.\nExpected: "
                          << static_cast<uint32_t>(expected_crc) << " Found: " << crc
                          << std::dec << ". Trying the ROM index instead." << std::endl;
        }
        else if (verbose)
        {
            std::cerr << "cannot read rom or unexpected size: " << path.string()
                      << ". Trying the ROM index instead." << std::endl;
        }
    }

    // Named file missing, wrong size, or wrong checksum: a stale/incorrect
    // loose file must not mask a good ROM elsewhere, so fall back to the
    // content-addressed index (this also lets ZIP archives satisfy
    // filename-mode loading when data.crc32 is disabled).
    return load_crc32(filename, offset, length, expected_crc, interleave, verbose);
}

int RomLoader::create_map()
{
    namespace fs = std::filesystem;

    rom_map.clear();
    sources_used_set.clear();
    search_location_list.clear();
    mapped_rom_path = config.data.rom_path;
    map_created = true;

    std::error_code ec;
    const fs::path rom_path(config.data.rom_path);

    // data.rompath may itself name a ZIP file (kept from the DX behaviour).
    // In that case only that one archive is indexed.
    if (fs::is_regular_file(rom_path, ec) && !ec && is_zip_path(rom_path))
    {
        search_location_list.push_back(rom_path.string() + " (archive)");
        add_zip_to_map(rom_path, rom_path.string());
        if (rom_map.empty())
        {
            std::cerr << "Warning: Could not create ROM map from archive - " << rom_path.string() << std::endl;
            return 1;
        }
        return 0;
    }

    ec.clear();
    const bool have_rom_dir = fs::is_directory(rom_path, ec) && !ec;
    if (!have_rom_dir)
        std::cerr << "Warning: Could not open ROM directory - " << config.data.rom_path << std::endl;

    // Directories already scanned, used to avoid indexing the same archive
    // twice (e.g. when the current working directory or executable
    // directory is the same directory as data.rompath).
    std::vector<fs::path> scanned_dirs;

    // 1. Loose files directly in rom_path, sorted by filename. Oversize
    //    files are skipped outright (never read into memory). Loose files
    //    are indexed before any archive, so they always win ties.
    // 2. Every *.zip directly in rom_path, sorted by filename.
    if (have_rom_dir)
    {
        scanned_dirs.push_back(rom_path);
        search_location_list.push_back(rom_path.string() + " (loose files)");
        search_location_list.push_back(rom_path.string() + " (*.zip archives)");

        std::vector<fs::path> archives;
        for (const fs::path& entry : list_regular_files_sorted(rom_path))
        {
            if (is_zip_path(entry))
            {
                archives.push_back(entry);
                continue;
            }

            std::error_code size_ec;
            const uintmax_t size = fs::file_size(entry, size_ec);
            if (size_ec)
                continue;

            if (size > kMaxLooseFileSize)
            {
                if (cannonball::verbose)
                    std::cout << "Skipping oversize loose ROM candidate (> 0x"
                              << std::hex << kMaxLooseFileSize << std::dec << " bytes): "
                              << entry.string() << std::endl;
                continue;
            }

            add_loose_file_to_map(entry, rom_path.string());
        }

        for (const fs::path& archive : archives)
            add_zip_to_map(archive, archive.string());
    }

    // 3. Known MAME set names in the current working directory, then the
    //    executable directory. Directories already scanned are skipped.
    std::vector<fs::path> extra_dirs;

    fs::path cwd = fs::current_path(ec);
    if (!ec)
        extra_dirs.push_back(cwd);

    char* base_path = SDL_GetBasePath();
    if (base_path != nullptr)
    {
        extra_dirs.push_back(fs::path(base_path));
        SDL_free(base_path);
    }

    for (const fs::path& dir : extra_dirs)
    {
        bool already_scanned = false;
        for (const fs::path& prior : scanned_dirs)
        {
            std::error_code eq_ec;
            if (fs::equivalent(dir, prior, eq_ec) && !eq_ec)
            {
                already_scanned = true;
                break;
            }
        }
        if (already_scanned)
            continue;

        scanned_dirs.push_back(dir);
        search_location_list.push_back(dir.string() + " (known MAME archive names)");

        for (const fs::path& entry : list_regular_files_sorted(dir))
        {
            if (is_known_zip_name(entry))
                add_zip_to_map(entry, entry.string());
        }
    }

    if (rom_map.empty())
    {
        std::cerr << "Warning: Could not create ROM map. No loose ROM files or MAME ZIP "
                  << "archives were found in any searched location." << std::endl;
        return 1;
    }

    return 0;
}

int RomLoader::load_crc32(const char* debug, const int offset, const int length, const int expected_crc, const uint8_t interleave, const bool verbose)
{
    if (!map_created || mapped_rom_path != config.data.rom_path)
        create_map();

    if (rom_map.empty())
        return 1;

    const RomKey key {
        static_cast<uint32_t>(expected_crc),
        static_cast<uint32_t>(length)
    };

    const auto search = rom_map.find(key);

    if (search == rom_map.end())
    {
        if (verbose)
            std::cerr << "Unable to locate rom in path: " << config.data.rom_path
                      << " possible name: " << debug
                      << " crc32: 0x" << std::hex << static_cast<uint32_t>(expected_crc)
                      << " size: 0x" << length << std::dec << std::endl;
        loaded = false;
        return 1;
    }

    std::vector<uint8_t> buffer;
    if (!load_source(search->second, length, buffer, verbose))
    {
        loaded = false;
        return 1;
    }

    // Re-check the uncompressed bytes. ZIP central-directory CRCs are useful
    // for indexing, but this keeps the same data-integrity guarantee as loose
    // ROM loading and catches a modified/corrupt archive after indexing.
    const uint32_t crc = crc32(buffer.data(), buffer.size());
    if (crc != static_cast<uint32_t>(expected_crc))
    {
        if (verbose)
            std::cerr << "ROM checksum changed while loading " << debug
                      << ". Expected: " << std::hex << static_cast<uint32_t>(expected_crc)
                      << " Found: " << crc << std::dec << std::endl;
        loaded = false;
        return 1;
    }

    copy_interleaved(rom, buffer, offset, interleave);
    loaded = true;
    sources_used_set.insert(search->second.source_label);

    if (cannonball::verbose)
        std::cout << debug << " loaded from " << search->second.source_label << std::endl;

    return 0;
}

std::string RomLoader::sources_used()
{
    if (sources_used_set.empty())
        return "(none)";

    std::string result;
    for (const std::string& source : sources_used_set)
    {
        if (!result.empty())
            result += ", ";
        result += source;
    }
    return result;
}

std::string RomLoader::search_locations()
{
    if (search_location_list.empty())
        return "(none)";

    std::string result;
    for (const std::string& location : search_location_list)
    {
        if (!result.empty())
            result += ", ";
        result += location;
    }
    return result;
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
    rom = reinterpret_cast<uint8_t*>(buffer);
    src.close();
    loaded = true;
    return 0;
}

int RomLoader::filesize(const char* filename)
{
    std::ifstream in(filename, std::ifstream::in | std::ifstream::binary);
    in.seekg(0, std::ifstream::end);
    int size = static_cast<int>(in.tellg());
    in.close();
    return size;
}
