/**
 * ============================================================================
 * um-ddsmmp - High-Performance Evil Islands DDS <-> MMP Texture Converter
 * ============================================================================
 *
 * Description:
 *   Optimized CLI tool to convert textures between:
 *     1. Standard DirectDraw Surface textures (.dds)
 *     2. Proprietary Evil Islands Mip-Mapped Pictures (.mmp)
 *
 * Supported Pixel Formats:
 *   - S3TC DXT1 Compressed (FourCC: DXT1, 4 bpp)
 *   - S3TC DXT3 Compressed (FourCC: DXT3, 8 bpp)
 *   - S3TC DXT5 Compressed (FourCC: DXT5, 16 bpp)
 *   - Uncompressed 32-bit BGRA with aligned Zero-RLE (FourCC: PNT3, 32 bpp)
 *   - Uncompressed 16-bit RGBA 5551 (FourCC: QU\0\0, 16 bpp)
 *   - Uncompressed 16-bit RGB 565 (FourCC: PV\0\0, 16 bpp)
 *
 * Features:
 *   - Fast single-pass binary conversion in native C++17.
 *   - Multithreaded batch processing for directories (-m / --multi).
 *   - 100% exact byte compatibility with original game textures.
 *   - Safe read-only file access (never modifies input files).
 *   - Custom output directory / file support (-o / --output).
 *   - Dry-run simulation mode (--dry-run).
 *   - Linux native and Windows cross-compilable (.exe).
 *
 * Usage:
 *   um-ddsmmp [options] <path/to/texture.dds|texture.mmp>
 *   um-ddsmmp [options] -d <path/to/directory>
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

namespace fs = std::filesystem;

// Program metadata
static constexpr const char* PROGRAM_VERSION = "0.1";
static constexpr const char* PROGRAM_NAME = "um-ddsmmp";

// Magic constants
static constexpr uint32_t DDS_MAGIC = 0x20534444u; // "DDS "
static constexpr uint32_t MMP_MAGIC = 0x00504D4Du; // "MMP\0"

// DDS Flags & Caps Constants
static constexpr uint32_t DDSD_CAPS        = 0x00000001u;
static constexpr uint32_t DDSD_HEIGHT      = 0x00000002u;
static constexpr uint32_t DDSD_WIDTH       = 0x00000004u;
static constexpr uint32_t DDSD_PITCH       = 0x00000008u;
static constexpr uint32_t DDSD_PIXELFORMAT = 0x00001000u;
static constexpr uint32_t DDSD_MIPMAPCOUNT = 0x00020000u;
static constexpr uint32_t DDSD_LINEARSIZE  = 0x00080000u;

static constexpr uint32_t DDPF_ALPHAPIXELS = 0x00000001u;
static constexpr uint32_t DDPF_FOURCC      = 0x00000004u;
static constexpr uint32_t DDPF_RGB         = 0x00000040u;

static constexpr uint32_t DDSCAPS_COMPLEX  = 0x00000008u;
static constexpr uint32_t DDSCAPS_TEXTURE  = 0x00001000u;
static constexpr uint32_t DDSCAPS_MIPMAP   = 0x00400000u;

// ============================================================================
// Header Structures
// ============================================================================

#pragma pack(push, 1)

struct DdsPixelFormat {
    uint32_t dwSize;
    uint32_t dwFlags;
    char     dwFourCC[4];
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};

struct DdsHeader {
    uint32_t       dwMagic;
    uint32_t       dwSize;
    uint32_t       dwFlags;
    uint32_t       dwHeight;
    uint32_t       dwWidth;
    uint32_t       dwPitchOrLinearSize;
    uint32_t       dwDepth;
    uint32_t       dwMipMapCount;
    uint32_t       dwReserved1[11];
    DdsPixelFormat ddspf;
    uint32_t       dwCaps;
    uint32_t       dwCaps2;
    uint32_t       dwCaps3;
    uint32_t       dwCaps4;
    uint32_t       dwReserved2;
};

struct MmpHeader {
    char     magic[4];       // "MMP\0"
    uint32_t width;          // Texture width
    uint32_t height;         // Texture height
    uint32_t mipsOrDataLen;  // For PNT3: compressed byte count; otherwise: mipmap level count
    char     fourcc[4];      // Format identifier ("DXT1", "DXT3", "PNT3", "QU\0\0", "PV\0\0")
    uint32_t bitDepth;       // Bits per pixel (4, 8, 16, 32)
    uint32_t alphaMask;
    uint32_t alphaShift;
    uint32_t alphaBits;
    uint32_t redMask;
    uint32_t redShift;
    uint32_t redBits;
    uint32_t greenMask;
    uint32_t greenShift;
    uint32_t greenBits;
    uint32_t blueMask;
    uint32_t blueShift;
    uint32_t blueBits;
    uint32_t reserved;       // Reserved / unused (0)
};

#pragma pack(pop)

static_assert(sizeof(DdsHeader) == 128, "DdsHeader must be exactly 128 bytes");
static_assert(sizeof(MmpHeader) == 76,  "MmpHeader must be exactly 76 bytes");

// ============================================================================
// Little-Endian Buffer Helpers
// ============================================================================

static inline uint32_t ReadUint32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
          (static_cast<uint32_t>(p[1]) << 8) |
          (static_cast<uint32_t>(p[2]) << 16) |
          (static_cast<uint32_t>(p[3]) << 24);
}

static inline void WriteUint32LE(uint8_t* p, uint32_t val) {
    p[0] = static_cast<uint8_t>(val & 0xFF);
    p[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((val >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((val >> 24) & 0xFF);
}

// ============================================================================
// PNT3 16-Byte Aligned Zero Run-Length Encoding / Decoding
// ============================================================================

/**
 * Encodes uncompressed 32-bit BGRA pixel data into Evil Islands PNT3 format.
 * Runs of transparent pixels (0x00000000) are compressed into 16-byte aligned skips.
 */
static std::vector<uint8_t> EncodePnt3(const uint8_t* data, size_t dataLen) {
    std::vector<uint8_t> out;
    out.reserve(dataLen);

    size_t pos = 0;
    while (pos < dataLen) {
        if (pos + 4 > dataLen) {
            out.insert(out.end(), data + pos, data + dataLen);
            break;
        }

        uint32_t val = ReadUint32LE(data + pos);
        if (val == 0) {
            if ((pos % 16) == 0) {
                // Pos is 16-byte aligned, count contiguous zeroes
                size_t zStart = pos;
                while (pos + 4 <= dataLen && ReadUint32LE(data + pos) == 0) {
                    pos += 4;
                }
                size_t totalZeroes = pos - zStart;
                size_t chunk16 = (totalZeroes / 16) * 16;

                if (chunk16 > 0) {
                    uint8_t lenBuf[4];
                    WriteUint32LE(lenBuf, static_cast<uint32_t>(chunk16));
                    out.insert(out.end(), lenBuf, lenBuf + 4);
                    pos = zStart + chunk16;
                } else {
                    uint8_t zeroBuf[4] = {0, 0, 0, 0};
                    out.insert(out.end(), zeroBuf, zeroBuf + 4);
                    pos = zStart + 4;
                }
            } else {
                // Not 16-byte aligned, write literal zero pixel
                uint8_t zeroBuf[4] = {0, 0, 0, 0};
                out.insert(out.end(), zeroBuf, zeroBuf + 4);
                pos += 4;
            }
        } else {
            out.insert(out.end(), data + pos, data + pos + 4);
            pos += 4;
        }
    }

    return out;
}

/**
 * Decodes Evil Islands PNT3 compressed payload into standard 32-bit BGRA pixels.
 */
static std::vector<uint8_t> DecodePnt3(const uint8_t* payload, size_t payloadLen, size_t expectedByteCount) {
    std::vector<uint8_t> out;
    out.reserve(expectedByteCount);

    size_t pos = 0;
    while (pos < payloadLen && out.size() < expectedByteCount) {
        if (pos + 4 > payloadLen) {
            out.insert(out.end(), payload + pos, payload + payloadLen);
            break;
        }

        uint32_t val = ReadUint32LE(payload + pos);
        pos += 4;

        // Check if value is a 16-byte aligned skip count (alpha byte is 0)
        if ((out.size() % 16) == 0 && val > 0 && (val % 16) == 0 && (val >> 24) == 0) {
            out.resize(out.size() + val, 0);
        } else {
            uint8_t pixelBuf[4];
            WriteUint32LE(pixelBuf, val);
            out.insert(out.end(), pixelBuf, pixelBuf + 4);
        }
    }

    if (out.size() < expectedByteCount) {
        out.resize(expectedByteCount, 0);
    }

    return out;
}

// ============================================================================
// Core Conversion Logic: DDS -> MMP
// ============================================================================

static bool ConvertDdsToMmp(
    const uint8_t* ddsData,
    size_t ddsSize,
    std::vector<uint8_t>& mmpOut,
    std::string& err)
{
    if (ddsSize < sizeof(DdsHeader)) {
        err = "File is too small to be a valid DDS texture";
        return false;
    }

    const DdsHeader* dds = reinterpret_cast<const DdsHeader*>(ddsData);
    if (dds->dwMagic != DDS_MAGIC || dds->dwSize != 124) {
        err = "Invalid DDS magic or header size";
        return false;
    }

    const uint8_t* payload = ddsData + sizeof(DdsHeader);
    size_t payloadLen = ddsSize - sizeof(DdsHeader);

    MmpHeader mmp;
    std::memset(&mmp, 0, sizeof(mmp));
    std::memcpy(mmp.magic, "MMP\0", 4);
    mmp.width = dds->dwWidth;
    mmp.height = dds->dwHeight;
    mmp.mipsOrDataLen = (dds->dwMipMapCount > 0) ? dds->dwMipMapCount : 1;

    std::vector<uint8_t> finalPayload;

    if (dds->ddspf.dwFlags & DDPF_FOURCC) {
        if (std::memcmp(dds->ddspf.dwFourCC, "DXT1", 4) == 0) {
            std::memcpy(mmp.fourcc, "DXT1", 4);
            mmp.bitDepth   = 4;
            mmp.alphaMask  = 0x00008000; mmp.alphaShift = 15; mmp.alphaBits = 1;
            mmp.redMask    = 0x00007C00; mmp.redShift   = 10; mmp.redBits   = 5;
            mmp.greenMask  = 0x000003E0; mmp.greenShift = 5;  mmp.greenBits = 5;
            mmp.blueMask   = 0x0000001F; mmp.blueShift  = 0;  mmp.blueBits  = 5;
            finalPayload.assign(payload, payload + payloadLen);
        } else if (std::memcmp(dds->ddspf.dwFourCC, "DXT3", 4) == 0) {
            std::memcpy(mmp.fourcc, "DXT3", 4);
            mmp.bitDepth   = 8;
            mmp.alphaMask  = 0x0000F000; mmp.alphaShift = 12; mmp.alphaBits = 4;
            mmp.redMask    = 0x00000F00; mmp.redShift   = 8;  mmp.redBits   = 4;
            mmp.greenMask  = 0x000000F0; mmp.greenShift = 4;  mmp.greenBits = 4;
            mmp.blueMask   = 0x0000000F; mmp.blueShift  = 0;  mmp.blueBits  = 4;
            finalPayload.assign(payload, payload + payloadLen);
        } else if (std::memcmp(dds->ddspf.dwFourCC, "DXT5", 4) == 0) {
            mmp.fourcc[0] = 'P'; mmp.fourcc[1] = 'V'; mmp.fourcc[2] = '\0'; mmp.fourcc[3] = '\0';
            mmp.bitDepth   = 16;
            mmp.alphaMask  = 0x00000000; mmp.alphaShift = 0;  mmp.alphaBits = 0;
            mmp.redMask    = 0x0000F800; mmp.redShift   = 11; mmp.redBits   = 5;
            mmp.greenMask  = 0x000007E0; mmp.greenShift = 5;  mmp.greenBits = 6;
            mmp.blueMask   = 0x0000001F; mmp.blueShift  = 0;  mmp.blueBits  = 5;
            finalPayload.assign(payload, payload + payloadLen);
        } else {
            err = "Unsupported DDS FourCC format: " + std::string(dds->ddspf.dwFourCC, 4);
            return false;
        }
    } else if (dds->ddspf.dwRGBBitCount == 32) {
        // Uncompressed 32-bit BGRA -> PNT3 with zero RLE (base mipmap only)
        std::memcpy(mmp.fourcc, "PNT3", 4);
        mmp.bitDepth = 32;
        size_t baseLevelBytes = static_cast<size_t>(dds->dwWidth) * dds->dwHeight * 4;
        size_t bytesToEncode = std::min(payloadLen, baseLevelBytes);
        finalPayload = EncodePnt3(payload, bytesToEncode);
        mmp.mipsOrDataLen = static_cast<uint32_t>(finalPayload.size());
    } else if (dds->ddspf.dwRGBBitCount == 16) {
        if (dds->ddspf.dwRBitMask == 0x7C00 && dds->ddspf.dwABitMask == 0x8000) {
            // RGBA 5551
            mmp.fourcc[0] = 'Q'; mmp.fourcc[1] = 'U'; mmp.fourcc[2] = '\0'; mmp.fourcc[3] = '\0';
            mmp.bitDepth   = 16;
            mmp.alphaMask  = 0x00008000; mmp.alphaShift = 15; mmp.alphaBits = 1;
            mmp.redMask    = 0x00007C00; mmp.redShift   = 10; mmp.redBits   = 5;
            mmp.greenMask  = 0x000003E0; mmp.greenShift = 5;  mmp.greenBits = 5;
            mmp.blueMask   = 0x0000001F; mmp.blueShift  = 0;  mmp.blueBits  = 5;
        } else {
            // RGB 565
            mmp.fourcc[0] = 'P'; mmp.fourcc[1] = 'V'; mmp.fourcc[2] = '\0'; mmp.fourcc[3] = '\0';
            mmp.bitDepth   = 16;
            mmp.alphaMask  = 0x00000000; mmp.alphaShift = 0;  mmp.alphaBits = 0;
            mmp.redMask    = 0x0000F800; mmp.redShift   = 11; mmp.redBits   = 5;
            mmp.greenMask  = 0x000007E0; mmp.greenShift = 5;  mmp.greenBits = 6;
            mmp.blueMask   = 0x0000001F; mmp.blueShift  = 0;  mmp.blueBits  = 5;
        }
        finalPayload.assign(payload, payload + payloadLen);
    } else {
        err = "Unsupported DDS pixel format (bitcount=" + std::to_string(dds->ddspf.dwRGBBitCount) + ")";
        return false;
    }

    mmpOut.resize(sizeof(MmpHeader) + finalPayload.size());
    std::memcpy(mmpOut.data(), &mmp, sizeof(MmpHeader));
    std::memcpy(mmpOut.data() + sizeof(MmpHeader), finalPayload.data(), finalPayload.size());

    return true;
}

// ============================================================================
// Core Conversion Logic: MMP -> DDS
// ============================================================================

static bool ConvertMmpToDds(
    const uint8_t* mmpData,
    size_t mmpSize,
    std::vector<uint8_t>& ddsOut,
    std::string& err)
{
    if (mmpSize < sizeof(MmpHeader)) {
        err = "File is too small to be a valid MMP texture";
        return false;
    }

    const MmpHeader* mmp = reinterpret_cast<const MmpHeader*>(mmpData);
    if (std::memcmp(mmp->magic, "MMP\0", 4) != 0) {
        err = "Invalid MMP magic header";
        return false;
    }

    const uint8_t* payload = mmpData + sizeof(MmpHeader);
    size_t payloadLen = mmpSize - sizeof(MmpHeader);

    DdsHeader dds;
    std::memset(&dds, 0, sizeof(dds));
    dds.dwMagic = DDS_MAGIC;
    dds.dwSize = 124;
    dds.dwFlags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
    dds.dwHeight = mmp->height;
    dds.dwWidth = mmp->width;
    dds.ddspf.dwSize = 32;
    dds.dwCaps = DDSCAPS_TEXTURE;

    uint32_t mipCount = mmp->mipsOrDataLen;
    std::vector<uint8_t> finalPayload;

    if (std::memcmp(mmp->fourcc, "DXT1", 4) == 0) {
        dds.dwFlags |= DDSD_LINEARSIZE;
        dds.dwPitchOrLinearSize = mmp->width * mmp->height / 2;
        dds.ddspf.dwFlags = DDPF_FOURCC;
        std::memcpy(dds.ddspf.dwFourCC, "DXT1", 4);
        finalPayload.assign(payload, payload + payloadLen);
    } else if (std::memcmp(mmp->fourcc, "DXT3", 4) == 0) {
        dds.dwFlags |= DDSD_LINEARSIZE;
        dds.dwPitchOrLinearSize = mmp->width * mmp->height;
        dds.ddspf.dwFlags = DDPF_FOURCC;
        std::memcpy(dds.ddspf.dwFourCC, "DXT3", 4);
        finalPayload.assign(payload, payload + payloadLen);
    } else if (std::memcmp(mmp->fourcc, "PNT3", 4) == 0) {
        // Uncompressed 32-bit BGRA
        mipCount = 0;
        dds.dwFlags |= DDSD_PITCH;
        dds.dwPitchOrLinearSize = mmp->width * 4;
        dds.ddspf.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        dds.ddspf.dwRGBBitCount = 32;
        dds.ddspf.dwRBitMask = 0x00FF0000;
        dds.ddspf.dwGBitMask = 0x0000FF00;
        dds.ddspf.dwBBitMask = 0x000000FF;
        dds.ddspf.dwABitMask = 0xFF000000;

        size_t expectedBytes = static_cast<size_t>(mmp->width) * mmp->height * 4;
        finalPayload = DecodePnt3(payload, payloadLen, expectedBytes);
    } else if (mmp->fourcc[0] == 'Q' && mmp->fourcc[1] == 'U') {
        // RGBA 5551
        dds.dwFlags |= DDSD_PITCH;
        dds.dwPitchOrLinearSize = mmp->width * 2;
        dds.ddspf.dwFlags = DDPF_RGB | DDPF_ALPHAPIXELS;
        dds.ddspf.dwRGBBitCount = 16;
        dds.ddspf.dwRBitMask = 0x7C00;
        dds.ddspf.dwGBitMask = 0x03E0;
        dds.ddspf.dwBBitMask = 0x001F;
        dds.ddspf.dwABitMask = 0x8000;
        finalPayload.assign(payload, payload + payloadLen);
    } else if (mmp->fourcc[0] == 'P' && mmp->fourcc[1] == 'V') {
        // RGB 565
        dds.dwFlags |= DDSD_PITCH;
        dds.dwPitchOrLinearSize = mmp->width * 2;
        dds.ddspf.dwFlags = DDPF_RGB;
        dds.ddspf.dwRGBBitCount = 16;
        dds.ddspf.dwRBitMask = 0xF800;
        dds.ddspf.dwGBitMask = 0x07E0;
        dds.ddspf.dwBBitMask = 0x001F;
        dds.ddspf.dwABitMask = 0x0000;
        finalPayload.assign(payload, payload + payloadLen);
    } else {
        err = "Unknown MMP FourCC format: " + std::string(mmp->fourcc, 4);
        return false;
    }

    if (mipCount > 1) {
        dds.dwFlags |= DDSD_MIPMAPCOUNT;
        dds.dwMipMapCount = mipCount;
        dds.dwCaps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
    }

    ddsOut.resize(sizeof(DdsHeader) + finalPayload.size());
    std::memcpy(ddsOut.data(), &dds, sizeof(DdsHeader));
    std::memcpy(ddsOut.data() + sizeof(DdsHeader), finalPayload.data(), finalPayload.size());

    return true;
}

// ============================================================================
// File Processing & CLI Handling
// ============================================================================

enum class ConvertMode {
    Auto,
    DdsToMmp,
    MmpToDds
};

struct ConvertResult {
    bool success = false;
    fs::path inputPath;
    fs::path outputPath;
    size_t inSize = 0;
    size_t outSize = 0;
    std::string errorMessage;
};

static ConvertResult ProcessTextureFile(
    const fs::path& inputPath,
    const fs::path& outputDirOrFile,
    ConvertMode mode,
    bool dryRun)
{
    ConvertResult res;
    res.inputPath = inputPath;

    // Read input file into memory strictly read-only
    std::ifstream in(inputPath, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        res.errorMessage = "Failed to open input file for reading";
        return res;
    }
    std::streamsize size = in.tellg();
    res.inSize = static_cast<size_t>(size);
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> inBuffer(res.inSize);
    if (res.inSize > 0) {
        if (!in.read(reinterpret_cast<char*>(inBuffer.data()), size)) {
            res.errorMessage = "Failed to read input file bytes";
            return res;
        }
    }
    in.close();

    // Auto-detect conversion mode if requested
    ConvertMode actualMode = mode;
    if (actualMode == ConvertMode::Auto) {
        std::string ext = inputPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (ext == ".dds") {
            actualMode = ConvertMode::DdsToMmp;
        } else if (ext == ".mmp") {
            actualMode = ConvertMode::MmpToDds;
        } else {
            if (inBuffer.size() >= 4 && ReadUint32LE(inBuffer.data()) == DDS_MAGIC) {
                actualMode = ConvertMode::DdsToMmp;
            } else if (inBuffer.size() >= 4 && ReadUint32LE(inBuffer.data()) == MMP_MAGIC) {
                actualMode = ConvertMode::MmpToDds;
            } else {
                actualMode = ConvertMode::DdsToMmp;
            }
        }
    }

    std::string targetExt = (actualMode == ConvertMode::DdsToMmp) ? ".mmp" : ".dds";

    // Determine output file path
    if (outputDirOrFile.empty()) {
        res.outputPath = inputPath.parent_path() / (inputPath.stem().string() + targetExt);
    } else {
        std::error_code ec;
        bool isDir = fs::is_directory(outputDirOrFile, ec) || outputDirOrFile.extension().empty();
        if (isDir) {
            res.outputPath = outputDirOrFile / (inputPath.stem().string() + targetExt);
        } else {
            res.outputPath = outputDirOrFile;
        }
    }

    std::vector<uint8_t> outBuffer;
    std::string err;

    if (actualMode == ConvertMode::DdsToMmp) {
        if (!ConvertDdsToMmp(inBuffer.data(), inBuffer.size(), outBuffer, err)) {
            res.errorMessage = err;
            return res;
        }
    } else {
        if (!ConvertMmpToDds(inBuffer.data(), inBuffer.size(), outBuffer, err)) {
            res.errorMessage = err;
            return res;
        }
    }

    res.outSize = outBuffer.size();

    if (dryRun) {
        res.success = true;
        return res;
    }

    // Ensure output directory exists
    if (res.outputPath.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(res.outputPath.parent_path(), ec);
    }

    // Write output file
    std::ofstream out(res.outputPath, std::ios::binary);
    if (!out.is_open()) {
        res.errorMessage = "Failed to open output file for writing: " + res.outputPath.string();
        return res;
    }
    if (!outBuffer.empty()) {
        out.write(reinterpret_cast<const char*>(outBuffer.data()), outBuffer.size());
    }
    if (!out) {
        res.errorMessage = "Failed to complete writing output file: " + res.outputPath.string();
        return res;
    }

    res.success = true;
    return res;
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
    ConvertMode mode = ConvertMode::Auto;
    fs::path inputPath;
    fs::path outputTarget;
};

static void PrintVersion() {
    std::cout << PROGRAM_NAME << " version " << PROGRAM_VERSION << "\n";
}

static void PrintHelp() {
    std::cout << "um-ddsmmp - High-Performance Evil Islands DDS <-> MMP Texture Converter\n\n"
              << "Usage:\n"
              << "  um-ddsmmp [options] <path/to/texture.dds|texture.mmp>\n"
              << "  um-ddsmmp [options] -d <path/to/directory>\n\n"
              << "Options:\n"
              << "  -o, --output <path>   Set output directory or destination file\n"
              << "                        (default: same directory with swapped extension)\n"
              << "  -d, --dir <dir>       Process all .dds or .mmp files in the directory\n"
              << "  -m, --multi           Process multiple files in parallel across CPU cores\n"
              << "                        (only active when used together with -d / --dir)\n"
              << "  --dds2mmp             Force DDS -> MMP conversion mode\n"
              << "  --mmp2dds             Force MMP -> DDS conversion mode\n"
              << "  --dry-run             Show what the program would do without writing files\n"
              << "  -v, --version         Print program version (" << PROGRAM_VERSION << ")\n"
              << "  -h, --help            Print this help message\n\n"
              << "Examples:\n"
              << "  um-ddsmmp texture.dds               # Converts texture.dds -> texture.mmp\n"
              << "  um-ddsmmp texture.mmp               # Converts texture.mmp -> texture.dds\n"
              << "  um-ddsmmp -d ./textures_dds -o ./mmp -m  # Batch converts DDS to MMP in parallel\n"
              << "  um-ddsmmp -d ./mmp --mmp2dds -m     # Batch converts MMP back to DDS in parallel\n";
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
        } else if (arg == "--dds2mmp") {
            opt.mode = ConvertMode::DdsToMmp;
        } else if (arg == "--mmp2dds") {
            opt.mode = ConvertMode::MmpToDds;
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

    if (fs::is_directory(opt.inputPath, ec)) {
        opt.isDirMode = true;
    }

    // Single File Mode
    if (!opt.isDirMode) {
        auto start = std::chrono::high_resolution_clock::now();
        ConvertResult res = ProcessTextureFile(opt.inputPath, opt.outputTarget, opt.mode, opt.dryRun);
        auto end = std::chrono::high_resolution_clock::now();
        double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();

        if (!res.success) {
            std::cerr << "[ERROR] " << res.inputPath.string() << ": " << res.errorMessage << "\n";
            return 1;
        }

        if (opt.dryRun) {
            std::cout << "[DRY-RUN] " << res.inputPath.string() << "\n"
                      << "  Would write: " << res.outputPath.string() << " (" << res.outSize << " bytes)\n"
                      << "  Processed in " << std::fixed << std::setprecision(2) << elapsedMs << " ms\n";
        } else {
            std::cout << "[SUCCESS] " << res.inputPath.filename().string()
                      << " -> " << res.outputPath.filename().string()
                      << " (" << res.outSize << " bytes in " << std::fixed << std::setprecision(2) << elapsedMs << " ms)\n";
        }

        return 0;
    }

    // Directory Mode
    std::vector<fs::path> targetFiles;
    for (const auto& entry : fs::directory_iterator(opt.inputPath)) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (opt.mode == ConvertMode::DdsToMmp && ext == ".dds") {
            targetFiles.push_back(entry.path());
        } else if (opt.mode == ConvertMode::MmpToDds && ext == ".mmp") {
            targetFiles.push_back(entry.path());
        } else if (opt.mode == ConvertMode::Auto && (ext == ".dds" || ext == ".mmp")) {
            targetFiles.push_back(entry.path());
        }
    }

    std::sort(targetFiles.begin(), targetFiles.end());

    if (targetFiles.empty()) {
        std::cout << "No matching texture files found in directory: " << opt.inputPath.string() << "\n";
        return 0;
    }

    std::cout << "Found " << targetFiles.size() << " texture file(s) in " << opt.inputPath.string() << "\n";

    unsigned int threadCount = 1;
    if (opt.multiThread) {
        unsigned int hw = std::thread::hardware_concurrency();
        threadCount = std::max(1u, hw > 0 ? hw : 1u);
        threadCount = std::min<unsigned int>(threadCount, static_cast<unsigned int>(targetFiles.size()));
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
            if (idx >= targetFiles.size()) break;

            const fs::path& path = targetFiles[idx];
            ConvertResult res = ProcessTextureFile(path, opt.outputTarget, opt.mode, opt.dryRun);

            std::lock_guard<std::mutex> lock(printMutex);
            if (res.success) {
                successCount++;
                if (opt.dryRun) {
                    std::cout << "[DRY-RUN " << (idx + 1) << "/" << targetFiles.size() << "] "
                              << path.filename().string() << " -> " << res.outputPath.filename().string()
                              << " (" << res.outSize << " B)\n";
                } else {
                    std::cout << "[" << (idx + 1) << "/" << targetFiles.size() << "] Converted "
                              << path.filename().string() << " -> " << res.outputPath.filename().string() << "\n";
                }
            } else {
                failCount++;
                std::cerr << "[ERROR " << (idx + 1) << "/" << targetFiles.size() << "] "
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
              << "Total: " << targetFiles.size()
              << ", Succeeded: " << successCount.load()
              << ", Failed: " << failCount.load() << "\n";

    return (failCount.load() == 0) ? 0 : 1;
}
