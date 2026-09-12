/**
 * ============================================================================
 * um-restool - High-Performance Evil Islands RES Archive Tool
 * ============================================================================
 *
 * Description:
 *   Optimized CLI tool to pack and unpack Evil Islands .res resource archives:
 *     1. Unpack: .res archive -> Directory of extracted files
 *     2. Pack:   Directory of files -> .res archive
 *
 * Supported Features:
 *   - Automatic format detection (.res -> unpack, directory -> pack).
 *   - 100% exact binary parity and full compatibility with Evil Islands game engine.
 *   - Intelligent payload deduplication across identical files.
 *   - File modification timestamp preservation.
 *   - Multithreaded batch processing for directories (-m / --multi).
 *   - Safe read-only file access (never modifies input files).
 *   - Custom output directory / file support (-o / --output).
 *   - Dry-run simulation mode (--dry-run).
 *   - Linux native and Windows cross-compilable (.exe).
 *
 * Usage:
 *   um-restool [options] <path/to/archive.res|path/to/folder>
 *   um-restool [options] -d <path/to/directory>
 *
 * Version:
 *   0.1
 * ============================================================================
 */

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdint>
#include <cctype>
#include <thread>
#include <mutex>
#include <atomic>
#include <filesystem>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <unordered_map>
#include <map>

namespace fs = std::filesystem;

// Program metadata
static constexpr const char* PROGRAM_VERSION = "0.1";
static constexpr const char* PROGRAM_NAME = "um-restool";

// Magic constant
static constexpr uint32_t RES_MAGIC = 0x019CE23Cu; // "3c e2 9c 01"

// ============================================================================
// Header Structures
// ============================================================================

#pragma pack(push, 1)

struct ResHeader {
    uint32_t magic;      // 0x019CE23C
    uint32_t numFiles;   // Total files in archive
    uint32_t dataSize;   // Total bytes in contiguous payload section
    uint32_t dirSize;    // Total bytes in trailing names block
};

#pragma pack(pop)

static_assert(sizeof(ResHeader) == 16, "ResHeader must be exactly 16 bytes");

// ============================================================================
// Little-Endian Binary Helpers
// ============================================================================

static inline uint16_t ReadUint16LE(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static inline uint32_t ReadUint32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

static inline int32_t ReadInt32LE(const uint8_t* p) {
    return static_cast<int32_t>(ReadUint32LE(p));
}

static inline void WriteUint16LE(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

static inline void WriteUint32LE(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

static inline void WriteInt32LE(std::vector<uint8_t>& buf, int32_t val) {
    WriteUint32LE(buf, static_cast<uint32_t>(val));
}

// ============================================================================
// Hash Calculation
// ============================================================================

static inline uint32_t CalculateResHash(const std::string& name, uint32_t bucketCount) {
    if (bucketCount == 0) return 0;
    uint32_t sum = 0;
    for (unsigned char c : name) {
        sum += static_cast<unsigned char>(std::tolower(c));
    }
    return sum % bucketCount;
}

// ============================================================================
// Archive Entry Representation
// ============================================================================

struct ArchiveFile {
    std::string relativePath;
    std::vector<uint8_t> payload;
    uint32_t timestamp = 0;
};

// ============================================================================
// Unpack Operation: .res -> Directory
// ============================================================================

static bool UnpackResArchive(
    const uint8_t* resData,
    size_t resSize,
    const fs::path& outDir,
    bool dryRun,
    size_t& fileCount,
    size_t& totalExtractedBytes,
    std::string& err)
{
    if (resSize < sizeof(ResHeader)) {
        err = "File is too small to contain a valid RES header";
        return false;
    }

    const ResHeader* hdr = reinterpret_cast<const ResHeader*>(resData);
    if (hdr->magic != RES_MAGIC) {
        err = "Invalid RES magic header (expected 0x019CE23C)";
        return false;
    }

    uint32_t numFiles = hdr->numFiles;
    uint32_t dataSize = hdr->dataSize;
    uint32_t dirSize  = hdr->dirSize;

    if (16 + dataSize > resSize || dirSize > resSize || 16 + dataSize + dirSize > resSize) {
        err = "Corrupted archive header (data/dir offsets extend beyond file size)";
        return false;
    }

    size_t descStart = 16 + dataSize;
    size_t namesStart = resSize - dirSize;

    fileCount = numFiles;
    totalExtractedBytes = 0;

    size_t pos = descStart;
    for (uint32_t i = 0; i < numFiles; ++i) {
        if (pos + 6 > namesStart) {
            err = "Reached unexpected end of file descriptors at index " + std::to_string(i);
            return false;
        }

        uint16_t nameLen = ReadUint16LE(resData + pos);
        uint32_t nameOff = ReadUint32LE(resData + pos + 2);
        pos += 6;

        if (namesStart + nameOff + nameLen > resSize) {
            err = "Invalid filename offset in names block for file index " + std::to_string(i);
            return false;
        }

        std::string fileName(reinterpret_cast<const char*>(resData + namesStart + nameOff), nameLen);

        uint32_t dlen = 0;
        uint32_t doff = 0;
        uint32_t time = 0;

        if (numFiles == 1 && pos == namesStart) {
            // Single-file archive with 6-byte header
            dlen = dataSize;
            doff = 16;
            time = 0;
        } else if (pos + 16 <= namesStart) {
            // int32_t nextIdx = ReadInt32LE(resData + pos);
            dlen = ReadUint32LE(resData + pos + 4);
            doff = ReadUint32LE(resData + pos + 8);
            time = ReadUint32LE(resData + pos + 12);
            pos += 16;
        } else {
            // 0-byte file (no data payload)
            dlen = 0;
            doff = 0;
            time = 0;
        }

        if (dlen > 0 && doff + dlen > resSize) {
            err = "Invalid data payload offset for file: " + fileName;
            return false;
        }

        totalExtractedBytes += dlen;

        if (!dryRun) {
            fs::path filePath = outDir / fileName;
            if (filePath.has_parent_path()) {
                std::error_code ec;
                fs::create_directories(filePath.parent_path(), ec);
            }

            std::ofstream out(filePath, std::ios::binary);
            if (!out.is_open()) {
                err = "Failed to create extracted file: " + filePath.string();
                return false;
            }
            if (dlen > 0) {
                out.write(reinterpret_cast<const char*>(resData + doff), dlen);
            }
            out.close();

            if (time > 0) {
                std::error_code ec;
                auto ftime = fs::file_time_type(std::chrono::seconds(time));
                fs::last_write_time(filePath, ftime, ec);
            }
        }
    }

    return true;
}

// ============================================================================
// Pack Operation: Directory -> .res
// ============================================================================

static bool PackResArchive(
    const fs::path& inDir,
    std::vector<uint8_t>& resOut,
    size_t& fileCount,
    std::string& err)
{
    std::vector<ArchiveFile> files;

    // Collect all files recursively
    try {
        for (const auto& entry : fs::recursive_directory_iterator(inDir)) {
            if (!entry.is_regular_file()) continue;

            fs::path relPath = fs::relative(entry.path(), inDir);
            std::string relStr = relPath.generic_string();

            std::ifstream in(entry.path(), std::ios::binary | std::ios::ate);
            if (!in.is_open()) {
                err = "Failed to open file for packing: " + entry.path().string();
                return false;
            }
            std::streamsize size = in.tellg();
            in.seekg(0, std::ios::beg);

            std::vector<uint8_t> payload(static_cast<size_t>(size));
            if (size > 0) {
                in.read(reinterpret_cast<char*>(payload.data()), size);
            }
            in.close();

            uint32_t mtime = 0;
            std::error_code ec;
            auto lwt = fs::last_write_time(entry.path(), ec);
            if (!ec) {
                auto s = std::chrono::duration_cast<std::chrono::seconds>(lwt.time_since_epoch()).count();
                mtime = static_cast<uint32_t>(s);
            }

            files.push_back({relStr, std::move(payload), mtime});
        }
    } catch (const std::exception& e) {
        err = "Error reading source directory: " + std::string(e.what());
        return false;
    }

    if (files.empty()) {
        err = "Source directory is empty (no files to pack)";
        return false;
    }

    fileCount = files.size();

    // 1. Pack data payloads with deduplication and 16-byte alignment
    struct FileRecordMeta {
        std::string name;
        uint32_t nameOffset = 0;
        uint32_t dataOffset = 0;
        uint32_t dataLength = 0;
        uint32_t timestamp  = 0;
        bool isDuplicate    = false;
    };

    std::vector<FileRecordMeta> records;
    records.reserve(files.size());

    std::vector<uint8_t> dataBlock;
    std::map<std::vector<uint8_t>, std::pair<uint32_t, uint32_t>> payloadCache;

    for (auto& file : files) {
        FileRecordMeta meta;
        meta.name = file.relativePath;
        meta.timestamp = file.timestamp;
        meta.dataLength = static_cast<uint32_t>(file.payload.size());

        auto it = payloadCache.find(file.payload);
        if (it != payloadCache.end()) {
            meta.dataOffset  = it->second.first;
            meta.dataLength  = it->second.second;
            meta.isDuplicate = true;
        } else {
            uint32_t off = static_cast<uint32_t>(16 + dataBlock.size());
            meta.dataOffset  = off;
            meta.isDuplicate = false;
            payloadCache[file.payload] = {off, meta.dataLength};

            dataBlock.insert(dataBlock.end(), file.payload.begin(), file.payload.end());

            // Pad to 16-byte alignment
            size_t rem = dataBlock.size() % 16;
            if (rem != 0) {
                dataBlock.resize(dataBlock.size() + (16 - rem), 0);
            }
        }
        records.push_back(meta);
    }

    // 2. Build names block
    std::vector<uint8_t> namesBlock;
    for (auto& rec : records) {
        rec.nameOffset = static_cast<uint32_t>(namesBlock.size());
        namesBlock.insert(namesBlock.end(), rec.name.begin(), rec.name.end());
    }

    // 3. Build collision hash table and place descriptors into buckets
    uint32_t numFiles = static_cast<uint32_t>(records.size());
    std::vector<FileRecordMeta*> table(numFiles, nullptr);
    std::vector<int32_t> nextIndexTable(numFiles, -1);

    for (uint32_t i = 0; i < numFiles; ++i) {
        uint32_t bucket = CalculateResHash(records[i].name, numFiles);
        if (table[bucket] == nullptr) {
            table[bucket] = &records[i];
            nextIndexTable[bucket] = -1;
        } else {
            uint32_t curr = bucket;
            while (nextIndexTable[curr] != -1) {
                curr = static_cast<uint32_t>(nextIndexTable[curr]);
            }

            int freeIdx = static_cast<int>(numFiles) - 1;
            while (freeIdx >= 0 && table[freeIdx] != nullptr) {
                --freeIdx;
            }

            if (freeIdx >= 0) {
                nextIndexTable[curr] = freeIdx;
                table[freeIdx] = &records[i];
                nextIndexTable[freeIdx] = -1;
            }
        }
    }

    // 4. Build descriptors block (22 bytes per slot)
    std::vector<uint8_t> descBlock;
    descBlock.reserve(numFiles * 22);

    for (uint32_t i = 0; i < numFiles; ++i) {
        const auto* rec = table[i];
        if (rec) {
            WriteUint16LE(descBlock, static_cast<uint16_t>(rec->name.size()));
            WriteUint32LE(descBlock, rec->nameOffset);
            WriteInt32LE(descBlock, nextIndexTable[i]);
            WriteUint32LE(descBlock, rec->dataLength);
            WriteUint32LE(descBlock, rec->dataOffset);
            WriteUint32LE(descBlock, rec->timestamp);
        } else {
            // Empty slot fallback
            WriteUint16LE(descBlock, 0);
            WriteUint32LE(descBlock, 0);
            WriteInt32LE(descBlock, -1);
            WriteUint32LE(descBlock, 0);
            WriteUint32LE(descBlock, 0);
            WriteUint32LE(descBlock, 0);
        }
    }

    // 5. Construct final archive
    ResHeader hdr;
    hdr.magic    = RES_MAGIC;
    hdr.numFiles = numFiles;
    hdr.dataSize = static_cast<uint32_t>(dataBlock.size());
    hdr.dirSize  = static_cast<uint32_t>(namesBlock.size());

    resOut.clear();
    resOut.resize(sizeof(ResHeader));
    std::memcpy(resOut.data(), &hdr, sizeof(ResHeader));

    resOut.insert(resOut.end(), dataBlock.begin(), dataBlock.end());
    resOut.insert(resOut.end(), descBlock.begin(), descBlock.end());
    resOut.insert(resOut.end(), namesBlock.begin(), namesBlock.end());

    return true;
}

// ============================================================================
// File Processing Orchestration
// ============================================================================

enum class ToolAction {
    Auto,
    Pack,
    Unpack
};

struct ToolResult {
    bool success = false;
    fs::path inputPath;
    fs::path outputPath;
    size_t inSize = 0;
    size_t outSize = 0;
    size_t fileCount = 0;
    std::string errorMessage;
};

static ToolResult ProcessTarget(
    const fs::path& inputPath,
    const fs::path& outputTarget,
    ToolAction action,
    bool stripExt,
    const std::string& customExt,
    bool dryRun)
{
    ToolResult res;
    res.inputPath = inputPath;

    std::error_code ec;
    bool isDir = fs::is_directory(inputPath, ec);

    ToolAction actualAction = action;
    if (actualAction == ToolAction::Auto) {
        actualAction = isDir ? ToolAction::Pack : ToolAction::Unpack;
    }

    if (actualAction == ToolAction::Unpack) {
        // UNPACK MODE: .res/.mq -> Directory
        std::ifstream in(inputPath, std::ios::binary | std::ios::ate);
        if (!in.is_open()) {
            res.errorMessage = "Failed to open archive file for reading: " + inputPath.string();
            return res;
        }
        std::streamsize size = in.tellg();
        res.inSize = static_cast<size_t>(size);
        in.seekg(0, std::ios::beg);

        std::vector<uint8_t> inBuf(res.inSize);
        if (res.inSize > 0) {
            in.read(reinterpret_cast<char*>(inBuf.data()), size);
        }
        in.close();

        // Determine destination folder (matching eipacker naming)
        if (outputTarget.empty()) {
            std::string stem = inputPath.stem().string();
            std::string ext = inputPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

            if (ext == ".mq") {
                res.outputPath = inputPath.parent_path() / (stem + "_mq");
            } else {
                res.outputPath = inputPath.parent_path() / (stem + "_res");
            }
        } else {
            res.outputPath = outputTarget;
        }

        std::string err;
        size_t fCount = 0;
        size_t extractedBytes = 0;
        if (!UnpackResArchive(inBuf.data(), inBuf.size(), res.outputPath, dryRun, fCount, extractedBytes, err)) {
            res.errorMessage = err;
            return res;
        }

        res.fileCount = fCount;
        res.outSize = extractedBytes;
        res.success = true;
        return res;
    } else {
        // PACK MODE: Directory -> .res/.mq archive
        std::string dirName = inputPath.filename().string();
        if (dirName.empty() || dirName == ".") {
            dirName = inputPath.parent_path().filename().string();
        }

        std::string baseStem = dirName;
        std::string targetExt = customExt.empty() ? ".res" : customExt;
        if (targetExt.front() != '.') targetExt = "." + targetExt;

        // Smart suffix stripping matching eipacker conventions
        if (dirName.size() > 3 && dirName.rfind("_mq") == dirName.size() - 3) {
            baseStem = dirName.substr(0, dirName.size() - 3);
            if (customExt.empty()) targetExt = ".mq";
        } else if (dirName.size() > 4 && dirName.rfind("_res") == dirName.size() - 4) {
            baseStem = dirName.substr(0, dirName.size() - 4);
            if (customExt.empty()) targetExt = ".res";
        }

        if (!stripExt && (dirName.rfind("_mq") == std::string::npos && dirName.rfind("_res") == std::string::npos)) {
            baseStem = dirName;
        }

        if (outputTarget.empty()) {
            res.outputPath = inputPath.parent_path() / (baseStem + targetExt);
        } else {
            bool isOutDir = fs::is_directory(outputTarget, ec) || outputTarget.extension().empty();
            if (isOutDir) {
                res.outputPath = outputTarget / (baseStem + targetExt);
            } else {
                res.outputPath = outputTarget;
            }
        }

        std::vector<uint8_t> resOut;
        std::string err;
        size_t fCount = 0;
        if (!PackResArchive(inputPath, resOut, fCount, err)) {
            res.errorMessage = err;
            return res;
        }

        res.fileCount = fCount;
        res.outSize = resOut.size();

        if (dryRun) {
            res.success = true;
            return res;
        }

        if (res.outputPath.has_parent_path()) {
            fs::create_directories(res.outputPath.parent_path(), ec);
        }

        std::ofstream out(res.outputPath, std::ios::binary);
        if (!out.is_open()) {
            res.errorMessage = "Failed to open output archive for writing: " + res.outputPath.string();
            return res;
        }
        out.write(reinterpret_cast<const char*>(resOut.data()), resOut.size());
        if (!out) {
            res.errorMessage = "Failed to write archive bytes to disk: " + res.outputPath.string();
            return res;
        }

        res.success = true;
        return res;
    }
}

// ============================================================================
// CLI Argument Parsing & Batch Execution
// ============================================================================

struct CliOptions {
    bool showHelp = false;
    bool showVersion = false;
    bool multiThread = false;
    bool isDirMode = false;
    bool dryRun = false;
    bool stripExt = true;
    ToolAction action = ToolAction::Auto;
    std::string customExt;
    fs::path inputPath;
    fs::path outputTarget;
};

static void PrintVersion() {
    std::cout << PROGRAM_NAME << " version " << PROGRAM_VERSION << "\n";
}

static void PrintHelp() {
    std::cout << "um-restool - High-Performance Evil Islands RES/MQ Archive Tool\n\n"
              << "Usage:\n"
              << "  um-restool [options] <path/to/archive.res|archive.mq|path/to/folder>\n"
              << "  um-restool [options] -d <path/to/directory>\n\n"
              << "Options:\n"
              << "  -o, --output <path>   Set output archive file or destination folder\n"
              << "  -d, --dir <dir>       Process all archives or subdirectories in directory\n"
              << "  -m, --multi           Process multiple archives/folders in parallel across CPU cores\n"
              << "  -s, --strip-ext       Strip _res and _mq directory suffixes when packing (default: on)\n"
              << "  --no-strip-ext        Do not strip directory suffixes when packing\n"
              << "  --ext <extension>     Override output archive extension (e.g. .mq, .res)\n"
              << "  --pack                Force pack directory -> archive\n"
              << "  --unpack              Force unpack archive -> directory\n"
              << "  --dry-run             Show what the program would do without writing files\n"
              << "  -v, --version         Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help            Print this help message\n\n"
              << "Examples:\n"
              << "  um-restool database.res                 # Unpacks to ./database_res/\n"
              << "  um-restool z3q1.mq                      # Unpacks to ./z3q1_mq/\n"
              << "  um-restool ./figures_res                # Packs to ./figures.res\n"
              << "  um-restool ./z3q1_mq                    # Packs to ./z3q1.mq\n"
              << "  um-restool -d ./res_unpacked -o ./res -m # Batch packs all folders in parallel\n"
              << "  um-restool -d ./mq-eng -m               # Batch packs all *_mq folders to *.mq\n";
}

static bool ParseCommandLine(int argc, char* argv[], CliOptions& opt) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            opt.showHelp = true;
            return true;
        } else if (arg == "-v" || arg == "--version") {
            opt.showVersion = true;
            return true;
        } else if (arg == "--dry-run") {
            opt.dryRun = true;
        } else if (arg == "-m" || arg == "--multi") {
            opt.multiThread = true;
        } else if (arg == "-s" || arg == "--strip-ext" || arg == "--strip") {
            opt.stripExt = true;
        } else if (arg == "--no-strip-ext" || arg == "--no-strip") {
            opt.stripExt = false;
        } else if (arg == "--pack") {
            opt.action = ToolAction::Pack;
        } else if (arg == "--unpack") {
            opt.action = ToolAction::Unpack;
        } else if (arg == "--ext") {
            if (i + 1 < argc) {
                opt.customExt = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires an extension argument (e.g. .mq, .res).\n";
                return false;
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                opt.outputTarget = argv[++i];
            } else {
                std::cerr << "Error: " << arg << " requires a path argument.\n";
                return false;
            }
        } else if (arg.rfind("--output=", 0) == 0) {
            opt.outputTarget = arg.substr(9);
        } else if (arg == "-d" || arg == "--dir") {
            opt.isDirMode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                opt.inputPath = argv[++i];
            }
        } else if (arg.rfind("--dir=", 0) == 0) {
            opt.isDirMode = true;
            opt.inputPath = arg.substr(6);
        } else if (arg[0] == '-') {
            std::cerr << "Error: Unknown option '" << arg << "'. Use -h / --help for usage.\n";
            return false;
        } else {
            if (opt.inputPath.empty()) {
                opt.inputPath = arg;
            } else {
                std::cerr << "Error: Unexpected additional positional argument '" << arg << "'.\n";
                return false;
            }
        }
    }

    return true;
}

int main(int argc, char* argv[]) {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);

    CliOptions opt;
    if (!ParseCommandLine(argc, argv, opt)) {
        return 1;
    }

    if (opt.showHelp) {
        PrintHelp();
        return 0;
    }

    if (opt.showVersion) {
        PrintVersion();
        return 0;
    }

    if (opt.inputPath.empty()) {
        std::cerr << "Error: No input file or directory specified.\n"
                  << "Try '" << PROGRAM_NAME << " --help' for more information.\n";
        return 1;
    }

    std::error_code ec;
    if (!fs::exists(opt.inputPath, ec)) {
        std::cerr << "Error: Input path does not exist: " << opt.inputPath.string() << "\n";
        return 1;
    }

    // Single Target Mode
    if (!opt.isDirMode) {
        auto start = std::chrono::high_resolution_clock::now();
        ToolResult res = ProcessTarget(opt.inputPath, opt.outputTarget, opt.action, opt.stripExt, opt.customExt, opt.dryRun);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (!res.success) {
            std::cerr << "[ERROR] " << res.inputPath.string() << ": " << res.errorMessage << "\n";
            return 1;
        }

        if (opt.dryRun) {
            std::cout << "[DRY-RUN] " << res.inputPath.string() << "\n"
                      << "  Would write: " << res.outputPath.string() << " (" << res.outSize << " bytes, " << res.fileCount << " files)\n"
                      << "  Processed in " << std::fixed << std::setprecision(2) << elapsedMs << " ms\n";
        } else {
            std::cout << "[SUCCESS] " << res.inputPath.filename().string()
                      << " -> " << res.outputPath.filename().string()
                      << " (" << res.fileCount << " files, " << res.outSize << " bytes in "
                      << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
        }

        return 0;
    }

    // Directory Mode
    std::vector<fs::path> targetPaths;
    for (const auto& entry : fs::directory_iterator(opt.inputPath)) {
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (opt.action == ToolAction::Unpack && (ext == ".res" || ext == ".mq")) {
            targetPaths.push_back(entry.path());
        } else if (opt.action == ToolAction::Pack && entry.is_directory()) {
            targetPaths.push_back(entry.path());
        } else if (opt.action == ToolAction::Auto) {
            if (ext == ".res" || ext == ".mq" || entry.is_directory()) {
                targetPaths.push_back(entry.path());
            }
        }
    }

    std::sort(targetPaths.begin(), targetPaths.end());

    if (targetPaths.empty()) {
        std::cout << "No matching files or directories found in: " << opt.inputPath.string() << "\n";
        return 0;
    }

    std::cout << "Found " << targetPaths.size() << " target(s) in " << opt.inputPath.string() << "\n";

    unsigned int threadCount = 1;
    if (opt.multiThread) {
        unsigned int hw = std::thread::hardware_concurrency();
        threadCount = std::max(1u, hw > 0 ? hw : 1u);
        threadCount = std::min<unsigned int>(threadCount, static_cast<unsigned int>(targetPaths.size()));
        std::cout << "Using " << threadCount << " worker thread(s) for parallel processing.\n";
    }

    auto startTime = std::chrono::high_resolution_clock::now();

    std::atomic<size_t> fileIndex{0};
    std::atomic<size_t> successCount{0};
    std::atomic<size_t> failCount{0};
    std::mutex printMutex;

    auto workerFunc = [&]() {
        while (true) {
            size_t idx = fileIndex.fetch_add(1);
            if (idx >= targetPaths.size()) break;

            const fs::path& path = targetPaths[idx];
            ToolResult res = ProcessTarget(path, opt.outputTarget, opt.action, opt.stripExt, opt.customExt, opt.dryRun);

            std::lock_guard<std::mutex> lock(printMutex);
            if (res.success) {
                successCount++;
                if (opt.dryRun) {
                    std::cout << "[DRY-RUN " << (idx + 1) << "/" << targetPaths.size() << "] "
                              << path.filename().string() << " -> " << res.outputPath.filename().string()
                              << " (" << res.fileCount << " files, " << res.outSize << " B)\n";
                } else {
                    std::cout << "[" << (idx + 1) << "/" << targetPaths.size() << "] "
                              << path.filename().string() << " -> " << res.outputPath.filename().string()
                              << " (" << res.fileCount << " files)\n";
                }
            } else {
                failCount++;
                std::cerr << "[ERROR " << (idx + 1) << "/" << targetPaths.size() << "] "
                          << path.filename().string() << ": " << res.errorMessage << "\n";
            }
        }
    };

    if (threadCount > 1) {
        std::vector<std::thread> workers;
        workers.reserve(threadCount);
        for (unsigned int i = 0; i < threadCount; ++i) {
            workers.emplace_back(workerFunc);
        }
        for (auto& w : workers) {
            w.join();
        }
    } else {
        workerFunc();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    double totalMs = std::chrono::duration<double, std::milli>(endTime - startTime).count();

    std::cout << "\nFinished in " << std::fixed << std::setprecision(2) << totalMs << " ms.\n"
              << "Total: " << targetPaths.size()
              << ", Succeeded: " << successCount.load()
              << ", Failed: " << failCount.load() << "\n";

    return (failCount.load() == 0) ? 0 : 1;
}
