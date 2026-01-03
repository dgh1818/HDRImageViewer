#include "pch.h"
#include "ImageLoader.h"
#include "Common\DirectXHelper.h"
#include "DirectXTex.h"
#include "DirectXTex\DirectXTexEXR.h"
#include <iostream>
#include <Windows.h>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

using namespace DXRenderer;

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace Platform;
using namespace std;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;

static const unsigned int sc_MaxBytesPerPixel = 16; // Covers all supported image formats (128bpp).
static bool lutInitialized = false;
static float sRGBToLinearLUT[256];

static bool IsAsciiSpace(BYTE value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static bool TryParseHeadroomValue(const BYTE* data, size_t len, const char* key, float& outValue)
{
    if (!data || !key)
    {
        return false;
    }

    const size_t keyLen = std::strlen(key);
    if (keyLen == 0 || len < keyLen)
    {
        return false;
    }

    for (size_t i = 0; i + keyLen < len; ++i)
    {
        if (std::memcmp(data + i, key, keyLen) != 0)
        {
            continue;
        }

        size_t pos = i + keyLen;
        while (pos < len && IsAsciiSpace(data[pos]))
        {
            ++pos;
        }

        if (pos >= len || data[pos] != '=')
        {
            continue;
        }

        ++pos;
        while (pos < len && IsAsciiSpace(data[pos]))
        {
            ++pos;
        }

        if (pos >= len || (data[pos] != '"' && data[pos] != '\''))
        {
            continue;
        }

        const BYTE quote = data[pos++];
        char numberBuf[64] = {};
        size_t count = 0;

        while (pos < len && data[pos] != quote && count + 1 < ARRAYSIZE(numberBuf))
        {
            const char c = static_cast<char>(data[pos]);
            if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
            {
                numberBuf[count++] = c;
            }
            else if (count > 0)
            {
                break;
            }
            ++pos;
        }

        if (count == 0)
        {
            continue;
        }

        char* endPtr = nullptr;
        const float parsed = strtof(numberBuf, &endPtr);
        if (endPtr == numberBuf)
        {
            continue;
        }

        outValue = parsed;
        return true;
    }

    return false;
}

static bool TryParseGainMapHeadroomFromBytes(const BYTE* data, size_t len, float& outValue)
{
    static const char* kHeadroomKeys[] =
    {
        "HDRGainMapHeadroom",
        "hdrgm:HDRCapacityMax",
        "HDRCapacityMax",
    };

    for (const char* key : kHeadroomKeys)
    {
        if (TryParseHeadroomValue(data, len, key, outValue))
        {
            return true;
        }
    }

    return false;
}

static void TryUpdateHdrGainMapHeadroom(ImageInfo& info, const BYTE* data, size_t len)
{
    if (info.hasHdrGainMapHeadroom)
    {
        return;
    }

    float headroom = 0.0f;
    if (TryParseGainMapHeadroomFromBytes(data, len, headroom) && headroom > 0.0f)
    {
        info.hasHdrGainMapHeadroom = true;
        info.hdrGainMapHeadroom = headroom;
    }
}

static bool TryParseAttributeValue(const BYTE* data, size_t len, const char* key, char* outValue, size_t outValueLen)
{
    if (!data || !key || !outValue || outValueLen == 0)
    {
        return false;
    }

    const size_t keyLen = std::strlen(key);
    if (keyLen == 0 || len < keyLen)
    {
        return false;
    }

    for (size_t i = 0; i + keyLen < len; ++i)
    {
        if (std::memcmp(data + i, key, keyLen) != 0)
        {
            continue;
        }

        size_t pos = i + keyLen;
        while (pos < len && IsAsciiSpace(data[pos]))
        {
            ++pos;
        }

        if (pos >= len || data[pos] != '=')
        {
            continue;
        }

        ++pos;
        while (pos < len && IsAsciiSpace(data[pos]))
        {
            ++pos;
        }

        if (pos >= len || (data[pos] != '"' && data[pos] != '\''))
        {
            continue;
        }

        const BYTE quote = data[pos++];
        size_t count = 0;
        while (pos < len && data[pos] != quote && count + 1 < outValueLen)
        {
            outValue[count++] = static_cast<char>(data[pos]);
            ++pos;
        }

        if (count == 0)
        {
            continue;
        }

        outValue[count] = '\0';
        return true;
    }

    return false;
}

static size_t ParseFloatList(const char* text, float* values, size_t maxCount)
{
    if (!text || !values || maxCount == 0)
    {
        return 0;
    }

    size_t count = 0;
    const char* cursor = text;
    while (*cursor != '\0' && count < maxCount)
    {
        while (*cursor != '\0' && (IsAsciiSpace(static_cast<BYTE>(*cursor)) || *cursor == ','))
        {
            ++cursor;
        }

        if (*cursor == '\0')
        {
            break;
        }

        char* endPtr = nullptr;
        float parsed = strtof(cursor, &endPtr);
        if (endPtr == cursor)
        {
            break;
        }

        values[count++] = parsed;
        cursor = endPtr;
    }

    return count;
}

static bool TryParseFloatAttributeList(const BYTE* data, size_t len, const char* key, float* outValues, size_t maxCount)
{
    char valueBuf[128] = {};
    if (!TryParseAttributeValue(data, len, key, valueBuf, ARRAYSIZE(valueBuf)))
    {
        return false;
    }

    float parsedValues[3] = {};
    size_t parsedCount = ParseFloatList(valueBuf, parsedValues, ARRAYSIZE(parsedValues));
    if (parsedCount == 0)
    {
        return false;
    }

    float fillValue = parsedValues[0];
    for (size_t i = 0; i < maxCount; ++i)
    {
        if (parsedCount == 1)
        {
            outValues[i] = fillValue;
        }
        else if (parsedCount == 2)
        {
            outValues[0] = parsedValues[0];
            outValues[1] = parsedValues[1];
            outValues[2] = parsedValues[1];
            break;
        }
        else
        {
            outValues[0] = parsedValues[0];
            outValues[1] = parsedValues[1];
            outValues[2] = parsedValues[2];
            break;
        }
    }

    return true;
}

static bool TryParseFloatAttributeValue(const BYTE* data, size_t len, const char* key, float& outValue)
{
    char valueBuf[64] = {};
    if (!TryParseAttributeValue(data, len, key, valueBuf, ARRAYSIZE(valueBuf)))
    {
        return false;
    }

    char* endPtr = nullptr;
    float parsed = strtof(valueBuf, &endPtr);
    if (endPtr == valueBuf)
    {
        return false;
    }

    outValue = parsed;
    return true;
}

static bool TryParseBoolAttributeValue(const BYTE* data, size_t len, const char* key, bool& outValue)
{
    char valueBuf[16] = {};
    if (!TryParseAttributeValue(data, len, key, valueBuf, ARRAYSIZE(valueBuf)))
    {
        return false;
    }

    if (_stricmp(valueBuf, "true") == 0)
    {
        outValue = true;
        return true;
    }

    if (_stricmp(valueBuf, "false") == 0)
    {
        outValue = false;
        return true;
    }

    return false;
}

void ImageLoader::TryUpdateGainMapMetadataFromBytes(const BYTE* data, size_t len)
{
    if (!data || len == 0)
    {
        return;
    }

    TryUpdateHdrGainMapHeadroom(m_imageInfo, data, len);

    if (m_gainMapMetadata.hasMetadata)
    {
        return;
    }

    GainMapMetadata updated = m_gainMapMetadata;
    const float headroom = (m_imageInfo.hasHdrGainMapHeadroom && m_imageInfo.hdrGainMapHeadroom > 0.0f)
        ? m_imageInfo.hdrGainMapHeadroom
        : 4.0f;

    updated.hasMetadata = false;
    updated.baseRenditionIsHdr = false;
    for (int c = 0; c < 3; ++c)
    {
        updated.gainMapMin[c] = 0.0f;
        updated.gainMapMax[c] = headroom;
        updated.gainMapGamma[c] = 1.0f;
        updated.offsetSdr[c] = 0.0f;
        updated.offsetHdr[c] = 0.0f;
    }
    updated.hdrCapacityMin = 0.0f;
    updated.hdrCapacityMax = headroom;

    bool found = false;

    if (TryParseFloatAttributeList(data, len, "hdrgm:GainMapMin", updated.gainMapMin, 3) ||
        TryParseFloatAttributeList(data, len, "GainMapMin", updated.gainMapMin, 3))
    {
        found = true;
    }

    if (TryParseFloatAttributeList(data, len, "hdrgm:GainMapMax", updated.gainMapMax, 3) ||
        TryParseFloatAttributeList(data, len, "GainMapMax", updated.gainMapMax, 3))
    {
        found = true;
    }

    if (TryParseFloatAttributeList(data, len, "hdrgm:Gamma", updated.gainMapGamma, 3) ||
        TryParseFloatAttributeList(data, len, "Gamma", updated.gainMapGamma, 3))
    {
        found = true;
    }

    if (TryParseFloatAttributeList(data, len, "hdrgm:OffsetSDR", updated.offsetSdr, 3) ||
        TryParseFloatAttributeList(data, len, "OffsetSDR", updated.offsetSdr, 3))
    {
        found = true;
    }

    if (TryParseFloatAttributeList(data, len, "hdrgm:OffsetHDR", updated.offsetHdr, 3) ||
        TryParseFloatAttributeList(data, len, "OffsetHDR", updated.offsetHdr, 3))
    {
        found = true;
    }

    if (TryParseFloatAttributeValue(data, len, "hdrgm:HDRCapacityMin", updated.hdrCapacityMin) ||
        TryParseFloatAttributeValue(data, len, "HDRCapacityMin", updated.hdrCapacityMin))
    {
        found = true;
    }

    if (TryParseFloatAttributeValue(data, len, "hdrgm:HDRCapacityMax", updated.hdrCapacityMax) ||
        TryParseFloatAttributeValue(data, len, "HDRCapacityMax", updated.hdrCapacityMax))
    {
        found = true;
    }

    bool baseIsHdr = false;
    if (TryParseBoolAttributeValue(data, len, "hdrgm:BaseRenditionIsHDR", baseIsHdr) ||
        TryParseBoolAttributeValue(data, len, "BaseRenditionIsHDR", baseIsHdr))
    {
        updated.baseRenditionIsHdr = baseIsHdr;
        found = true;
    }

    updated.hasMetadata = found;
    m_gainMapMetadata = updated;
}

ImageLoader::ImageLoader(const std::shared_ptr<DeviceResources>& deviceResources, ImageLoaderOptions& options) :
    m_deviceResources(deviceResources),
    m_state(ImageLoaderState::NotInitialized),
    m_imageInfo{},
    m_customOrDerivedColorProfile{},
    m_options(options),
    // Data extracted from Xbox console HDR screen capture image
    m_xboxHdrIccSize(2676),
    m_xboxHdrIccHeaderBytes {
        0x00, 0x00, 0x0A, 0x74, 0x00, 0x00, 0x00, 0x00, 0x02, 0x40, 0x00, 0x00,
        0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42, 0x20, 0x58, 0x59, 0x5A, 0x20,
        0x07, 0xE1, 0x00, 0x08, 0x00, 0x1E, 0x00, 0x0C, 0x00, 0x06, 0x00, 0x34,
        0x61, 0x63, 0x73, 0x70, 0x4D, 0x53, 0x46, 0x54, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xD6,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD3, 0x2D, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    },

    // TODO: the APP2 MP Extensions block isn't guaranteed to be static, but
    // assuming Apple doesn't change the format there should be basically no variation
    // in these bytes apart from the 3 DWORDs of "dynamic bytes".
    // Note that this APP2 block isn't really unique to Apple HDR gainmaps - it basically
    // states there are two images, one primary and one unspecified secondary. The
    // unspecified secondary type is the most unique and excludes things like stereo 3D images.
    m_appleApp2MPBlock{
        0xFF, 0xE2, 0x00, 0x58, 0x4D, 0x50, 0x46, 0x00, 0x4D, 0x4D, 0x00, 0x2A,
        0x00, 0x00, 0x00, 0x08, 0x00, 0x03, 0xB0, 0x00, 0x00, 0x07, 0x00, 0x00,
        0x00, 0x04, 0x30, 0x31, 0x30, 0x30, 0xB0, 0x01, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0xB0, 0x02, 0x00, 0x07, 0x00, 0x00,
        0x00, 0x20, 0x00, 0x00, 0x00, 0x32, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // All 0xFF's represent "dynamic bytes".
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00
    },
    // These bytes are expected to change from image to image so we exlude them from the memcmp.
    m_appleApp2MPBlockDynamicBytes{
        62, 63, 64, 65, // MPEntry 0: Count bytes of SOI to EOI (size of primary individual image)
        78, 79, 80, 81, // MPEntry 1: Count bytes of SOI to EOI (size of gain map/second individual image)
        82, 83, 84, 85  // MPEntry 1: Offset to SOI of second individual image
    },
    m_appleApp2MPBlockMagicOffset(62)
{
}

ImageLoader::~ImageLoader()
{
}

void ImageLoader::ResetGainMapMetadata()
{
    m_gainMapMetadata.hasMetadata = false;
    m_gainMapMetadata.baseRenditionIsHdr = false;
    for (int c = 0; c < 3; ++c)
    {
        m_gainMapMetadata.gainMapMin[c] = 0.0f;
        m_gainMapMetadata.gainMapMax[c] = 4.0f;
        m_gainMapMetadata.gainMapGamma[c] = 1.0f;
        m_gainMapMetadata.offsetSdr[c] = 0.0f;
        m_gainMapMetadata.offsetHdr[c] = 0.0f;
    }
    m_gainMapMetadata.hdrCapacityMin = 0.0f;
    m_gainMapMetadata.hdrCapacityMax = 4.0f;
}

static bool IsHeifHdrTransferFromStream(IStream* imageStream)
{
    if (!imageStream)
    {
        return false;
    }

    STATSTG stats = {};
    if (FAILED(imageStream->Stat(&stats, STATFLAG_NONAME)))
    {
        return false;
    }

    const size_t sizeBytes = static_cast<size_t>(stats.cbSize.QuadPart);
    if (sizeBytes != stats.cbSize.QuadPart || sizeBytes == 0)
    {
        return false;
    }

    std::vector<byte> fileBuf;
    fileBuf.resize(sizeBytes);

    LARGE_INTEGER zero = {};
    if (FAILED(imageStream->Seek(zero, STREAM_SEEK_SET, nullptr)))
    {
        return false;
    }

    ULONG cbRead = 0;
    if (FAILED(imageStream->Read(fileBuf.data(), static_cast<ULONG>(fileBuf.size()), &cbRead)))
    {
        return false;
    }

    if (cbRead != fileBuf.size())
    {
        return false;
    }

    CHeifContext ctx;
    heif_error herr = heif_context_read_from_memory_without_copy(ctx.ptr, fileBuf.data(), fileBuf.size(), nullptr);
    if (herr.code != heif_error_code::heif_error_Ok)
    {
        return false;
    }

    CHeifHandle mainHandle;
    herr = heif_context_get_primary_image_handle(ctx.ptr, &mainHandle.ptr);
    if (herr.code != heif_error_code::heif_error_Ok)
    {
        return false;
    }

    heif_color_profile_nclx* nclx = nullptr;
    herr = heif_image_handle_get_nclx_color_profile(mainHandle.ptr, &nclx);
    if (herr.code != heif_error_code::heif_error_Ok || !nclx)
    {
        if (nclx)
        {
            heif_nclx_color_profile_free(nclx);
        }
        return false;
    }

    bool isHdrTransfer =
        nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_PQ ||
        nclx->transfer_characteristics == heif_transfer_characteristic_ITU_R_BT_2100_0_HLG;

    heif_nclx_color_profile_free(nclx);
    return isHdrTransfer;
}

ImageInfo ImageLoader::LoadImageFromWic(_In_ IStream* imageStream)
{
    LoadImageFromWicInt(imageStream);

    return m_imageInfo;
}

/// <summary>
/// Internal method is needed because IFRIMG macro methods must return void.
/// If any failure occurs during image loading, immediately exits with
/// m_state and imageinfo set to failed.
/// </summary>
void ImageLoader::LoadImageFromWicInt(_In_ IStream* imageStream)
{
    EnforceStates(1, ImageLoaderState::NotInitialized);

    auto wicFactory = m_deviceResources->GetWicImagingFactory();

    // Decode the image using WIC.
    ComPtr<IWICBitmapDecoder> decoder;
    IFRIMG(wicFactory->CreateDecoderFromStream(
        imageStream,
        nullptr,
        WICDecodeMetadataCacheOnDemand,
        &decoder));

    ComPtr<IWICBitmapFrameDecode> frame;
    IFRIMG(decoder->GetFrame(0, &frame));

    GUID fmt;
    IFRIMG(decoder->GetContainerFormat(&fmt));

    ResetGainMapMetadata();
    m_imageInfo.hasHdrGainMapHeadroom = false;
    m_imageInfo.hdrGainMapHeadroom = 0.0f;
    m_imageInfo.hasIsoHeicHdrGainMap = false;
    m_imageInfo.hasAppleHeicHdrGainMap = false;

    // Perform initial detection and handling of special case WIC decoders.
    if (fmt == GUID_ContainerFormatHeif)
    {
        m_imageInfo.isHeif = true;

        // Some HEIF decoders do not support scaler-based decode checks.

        bool decoderSupportsHdr10 = false;

        // HEIF/HEVC supports GUID_WICPixelFormat32bppR10G10B10A2HDR10.
        // We must specifically detect and request HDR10 via IWICBitmapSourceTransform.
        ComPtr<IWICBitmapSourceTransform> sourceTransform;
        if (SUCCEEDED(frame->QueryInterface(IID_PPV_ARGS(&sourceTransform))))
        {
            GUID checkHDR10Fmt = GUID_WICPixelFormat32bppR10G10B10A2HDR10;
            if (SUCCEEDED(sourceTransform->GetClosestPixelFormat(&checkHDR10Fmt)) &&
                checkHDR10Fmt == GUID_WICPixelFormat32bppR10G10B10A2HDR10)
            {
                decoderSupportsHdr10 = true;
            }
        }

        // NOTE: Pixel resolution check can't be done until the main image has been decoded (LoadImageCommon).
        int gainmapType = TryLoadHdrGainMapHeic(imageStream);
        m_imageInfo.hasAppleHdrGainMap = false;
        m_imageInfo.hasIsoHeicHdrGainMap = gainmapType == 5;
        m_imageInfo.hasAppleHeicHdrGainMap = gainmapType == 6;

        bool hasHdrTransfer = IsHeifHdrTransferFromStream(imageStream);
        bool baseIsHdr = (gainmapType != 0) && m_gainMapMetadata.baseRenditionIsHdr;
        m_imageInfo.forceBT2100ColorSpace = decoderSupportsHdr10 && (hasHdrTransfer || baseIsHdr);

    }
    else if (fmt == GUID_ContainerFormatWmp)
    {
        // Xbox One HDR screenshots have to be specially detected and are always HDR10/BT.2100.
        if (IsImageXboxHdrScreenshot(frame.Get()))
        {
            m_imageInfo.forceBT2100ColorSpace = true;
        }
    }
    else if (fmt == GUID_ContainerFormatJpeg)
    {
        m_imageInfo.hasCuvaHdrGainMap = false;
        m_imageInfo.hasHuaweiIsoJpegHdrGainMap = false;
        m_imageInfo.hasIsoJpegHdrGainMap = false;

        int gainmapType = 0;
        if (TryLoadIsoHdrGainMapJpegMpo(imageStream, frame.Get()))
        {
            gainmapType = 3;
        }
        else
        {
            gainmapType = TryLoadCuvaHdrGainMapJpegMpo(imageStream, frame.Get());
        }


        m_imageInfo.hasCuvaHdrGainMap = gainmapType == 1;
        m_imageInfo.hasHuaweiIsoJpegHdrGainMap = gainmapType == 2;
        m_imageInfo.hasIsoJpegHdrGainMap = gainmapType == 3;

        m_imageInfo.hasAppleHdrGainMap = gainmapType != 0;
        if (!m_imageInfo.hasAppleHdrGainMap)
        {
            if (TryLoadAppleHdrGainMapJpegMpo(imageStream, frame.Get()))
            {
                gainmapType = 4;
                m_imageInfo.hasAppleHdrGainMap = true;
            }
        }
        
        if(m_imageInfo.hasAppleHdrGainMap) {
           m_imageInfo.forceBT2100ColorSpace = true;
        }
    }

    LoadImageCommon(frame.Get());
}

/// <summary>
/// Performs CPU-side decoding of an image using DirectXTex and reads key image parameters.
/// </summary>
/// <remarks>
/// Supports OpenEXR, Radiance RGBE, and certain DDS files - this is designed for a Direct2D-based
/// renderer, so we use WIC as an intermediate step which only supports some DDS DXGI_FORMAT values.
/// </remarks>
/// <param name="filename">The file path must be accessible from the sandbox, e.g. from the app's temp folder.</param>
/// <param name="extension">File extension with leading period. Needed as DirectXTex doesn't auto-detect codec type.</param>
ImageInfo ImageLoader::LoadImageFromDirectXTex(String^ filename, String^ extension)
{
    LoadImageFromDirectXTexInt(filename, extension);

    return m_imageInfo;
}

/// <summary>
/// Internal method is needed because IFRIMG macro methods must return void.
/// If any failure occurs during image loading, immediately exits with
/// m_state and imageinfo set to failed.
/// </summary>
void ImageLoader::LoadImageFromDirectXTexInt(String^ filename, String^ extension)
{
    EnforceStates(1, ImageLoaderState::NotInitialized);

    ResetGainMapMetadata();
    m_imageInfo.hasHdrGainMapHeadroom = false;
    m_imageInfo.hdrGainMapHeadroom = 0.0f;
    m_imageInfo.hasIsoHeicHdrGainMap = false;
    m_imageInfo.hasAppleHeicHdrGainMap = false;

    ComPtr<IWICBitmapSource> decodedSource;

    auto dxtScratch = new ScratchImage();
    auto filestr = filename->Data();

    if (extension == L".EXR" || extension == L".exr")
    {
        EXRChromaticities exrChromaticities;
        IFRIMG(LoadFromEXRFile(filestr, nullptr, &exrChromaticities, *dxtScratch));
        if (exrChromaticities.Valid)
        {
            m_imageInfo.countColorProfiles = 1;
            m_imageInfo.hasEXRChromaticitiesInfo = true;
            m_customOrDerivedColorProfile.redPrimary = D2D1::Point2F(exrChromaticities.RedX, exrChromaticities.RedY);
            m_customOrDerivedColorProfile.bluePrimary = D2D1::Point2F(exrChromaticities.BlueX, exrChromaticities.BlueY);
            m_customOrDerivedColorProfile.greenPrimary = D2D1::Point2F(exrChromaticities.GreenX, exrChromaticities.GreenY);
            m_customOrDerivedColorProfile.whitePointXZ = D2D1::Point2F(exrChromaticities.WhiteX, exrChromaticities.WhiteZ);
            m_customOrDerivedColorProfile.gamma = D2D1_GAMMA1_G10; // OpenEXR is linear
        }
    }
    else if (extension == L".HDR" || extension == L".hdr")
    {
        IFRIMG(LoadFromHDRFile(filestr, nullptr, *dxtScratch));
    }
    else
    {
        IFRIMG(LoadFromDDSFile(filestr, DDS_FLAGS_NONE, nullptr, *dxtScratch));
    }

    auto image = dxtScratch->GetImage(0, 0, 0); // Always get the first image.

    // Decompress if the image uses block compression. This does not use WIC and Direct2D's
    // native support for BC1, BC2, and BC3 formats.
    auto decompScratch = new ScratchImage();
    if (DirectX::IsCompressed(image->format))
    {
        IFRIMG(DirectX::Decompress(*image, DXGI_FORMAT_UNKNOWN, *decompScratch));

        // Memory for each Image is managed by ScratchImage.
        image = decompScratch->GetImage(0, 0, 0);
    }

    GUID wicFmt = TranslateDxgiFormatToWic(image->format);

    // Fail if we don't know how to load in WIC.
    IFRIMG(wicFmt == GUID_WICPixelFormatUndefined ? WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT : S_OK);

    ComPtr<IWICBitmap> dxtWicBitmap;
    auto fact = m_deviceResources->GetWicImagingFactory();
    IFRIMG(fact->CreateBitmapFromMemory(
        static_cast<UINT>(image->width),
        static_cast<UINT>(image->height),
        wicFmt,
        static_cast<UINT>(image->rowPitch),
        static_cast<UINT>(image->slicePitch),
        image->pixels,
        &dxtWicBitmap));

    LoadImageCommon(dxtWicBitmap.Get());

    // TODO: Common code to check file type?
    if (extension == L".HDR" || extension == L".hdr")
    {
        // Manually fix up Radiance RGBE image file bit depth as DirectXTex expands it to 128bpp.
        // 16 bpc is not strictly accurate but best preserves the intent of RGBE.
        m_imageInfo.bitsPerPixel = 32;
        m_imageInfo.bitsPerChannel = 16;
    }
}

/// <summary>
/// After initial decode, obtains image information and do common setup.
/// Populates all members of ImageInfo.
/// </summary>
void ImageLoader::LoadImageCommon(_In_ IWICBitmapSource* source)
{
    EnforceStates(1, ImageLoaderState::NotInitialized);

    // Overrides apply to all images.
    switch (m_options.type)
    {
    case ImageLoaderOptionsType::ForceBT2100:
        m_imageInfo.forceBT2100ColorSpace = true;
        break;

    case ImageLoaderOptionsType::CustomSdrColorSpace:
        m_imageInfo.hasOverriddenColorProfile = true;
        m_customOrDerivedColorProfile.redPrimary = D2D1::Point2F(m_options.customColorSpace.red.X, m_options.customColorSpace.red.Y);
        m_customOrDerivedColorProfile.greenPrimary = D2D1::Point2F(m_options.customColorSpace.green.X, m_options.customColorSpace.green.Y);
        m_customOrDerivedColorProfile.bluePrimary = D2D1::Point2F(m_options.customColorSpace.blue.X, m_options.customColorSpace.blue.Y);
        m_customOrDerivedColorProfile.whitePointXZ = D2D1::Point2F(m_options.customColorSpace.whitePt_XZ.X, m_options.customColorSpace.whitePt_XZ.Y);

        switch (m_options.customColorSpace.Gamma)
        {
        case CustomGamma::Gamma10:
            m_customOrDerivedColorProfile.gamma = D2D1_GAMMA1_G10;
            break;

        case CustomGamma::Gamma22:
        default:
            m_customOrDerivedColorProfile.gamma = D2D1_GAMMA1_G22;
            break;
        }

        break;

    default:
        break;
    }

    auto wicFactory = m_deviceResources->GetWicImagingFactory();

    WICPixelFormatGUID imageFmt;
    IFRIMG(source->GetPixelFormat(&imageFmt));

    if (m_imageInfo.forceBT2100ColorSpace == true &&
        m_imageInfo.isHeif == true)
    {
        // For compat, IWICBitmapSource::GetPixelFormat() always returns 8bpc,
        // the caller must specifically ask for 10bpc data; see CreateHeifHdr10CpuResources.
        imageFmt = GUID_WICPixelFormat32bppR10G10B10A2HDR10;
    }

    PopulatePixelFormatInfo(m_imageInfo, imageFmt);
    PopulateImageInfoACKind(m_imageInfo, source);

    

    UINT width = 0, height = 0;
    IFRIMG(source->GetSize(&width, &height));
    m_imageInfo.pixelSize = Size(static_cast<float>(width), static_cast<float>(height));

    // Gainmaps generally are 1/2 pixel size of the main image, but we don't restrict this.

    if (m_imageInfo.isHeif == true &&
        m_imageInfo.forceBT2100ColorSpace == true)
    {
        CreateHeifHdr10CpuResources(source);

        if (m_state == ImageLoaderState::LoadingFailed) return;
    }
    else
    {
        // Attempt to read the embedded color profile from the image; only valid for WIC images.
        // If CustomSdrColorSpace is set, any WIC profile is ignored.
        ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(source->QueryInterface(IID_PPV_ARGS(&frame))))
        {
            IFRIMG(wicFactory->CreateColorContext(&m_wicColorContext));

            IFRIMG(frame->GetColorContexts(
                1,
                m_wicColorContext.GetAddressOf(),
                &m_imageInfo.countColorProfiles));
        }

        // When decoding, preserve the numeric representation (float vs. non-float)
        // of the native image data. This avoids WIC performing an implicit gamma conversion
        // which occurs when converting between a fixed-point/integer pixel format (sRGB gamma)
        // and a float-point pixel format (linear gamma). Gamma adjustment, if specified by
        // the ICC profile, will be performed by the Direct2D color management effect.

        WICPixelFormatGUID fmt = {};
        if (m_imageInfo.isFloat)
        {
            fmt = GUID_WICPixelFormat64bppPRGBAHalf; // Equivalent to DXGI_FORMAT_R16G16B16A16_FLOAT.
        }
        else
        {
            fmt = GUID_WICPixelFormat64bppPRGBA; // Equivalent to DXGI_FORMAT_R16G16B16A16_UNORM.
                                                 // Many SDR images (e.g. JPEG) use <=32bpp, so it
                                                 // is possible to further optimize this for memory usage.
        }

        if (m_imageInfo.isHeif == true && m_imageInfo.forceBT2100ColorSpace == false)
        {
            fmt = GUID_WICPixelFormat32bppPBGRA;
        }

        if (m_imageInfo.hasAppleHdrGainMap == true || m_imageInfo.hasIsoHeicHdrGainMap == true || m_imageInfo.hasAppleHeicHdrGainMap == true) {
            UINT mapwidth = 0, mapheight = 0;
            IFRIMG(m_appleHdrGainMap.wicSource->GetSize(&mapwidth, &mapheight));
            m_imageInfo.gainMapPixelSize = Size(static_cast<float>(mapwidth), static_cast<float>(mapheight));
            fmt = GUID_WICPixelFormat32bppPBGRA;
        }

        ComPtr<IWICFormatConverter> format;
        IFRIMG(wicFactory->CreateFormatConverter(&format));

        IFRIMG(format->Initialize(
            source,
            fmt,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom));

        IFRIMG(format.As(&m_wicCachedSource));
    }

    m_state = ImageLoaderState::NeedDeviceResources;

    CreateDeviceDependentResourcesInternal();

    m_imageInfo.isValid = true;

    if (m_imageInfo.hasAppleHdrGainMap == true || m_imageInfo.hasIsoHeicHdrGainMap == true || m_imageInfo.hasAppleHeicHdrGainMap == true) {
        CreateCpuMergedBitmap();
    }
}

/// <summary>
/// Special codepath to generate a WIC software cache of an HDR10 HEIF image.
/// </summary>
/// <param name="source">Must be a valid HEIF HDR10 IWICBitmapFrameDecode.</param>
/// <remarks>
/// GUID_WICPixelFormat32bppR10G10B10A2HDR10 has very limited support in WIC and D2D, so instead
/// we must create a full resolution WIC bitmap cache and drop into D3D to upload to GPU.
///
/// Needs to be paired with CreateHeifHdr10GpuResources.
/// </remarks>
void ImageLoader::CreateHeifHdr10CpuResources(IWICBitmapSource* source)
{
    // Sanity checks
    IFRIMG(m_imageInfo.isHeif == true ? S_OK : WINCODEC_ERR_INVALIDPARAMETER);
    IFRIMG(m_imageInfo.forceBT2100ColorSpace == true ? S_OK : WINCODEC_ERR_INVALIDPARAMETER);

    auto fact = m_deviceResources->GetWicImagingFactory();

    UINT width, height = 0;
    IFRIMG(source->GetSize(&width, &height));

    ComPtr<IWICBitmapFrameDecode> frame;
    IFRIMG(source->QueryInterface(IID_PPV_ARGS(&frame)));

    ComPtr<IWICBitmapSourceTransform> sourceTransform;
    IFRIMG(frame.As(&sourceTransform));

    GUID hdr10Fmt = GUID_WICPixelFormat32bppR10G10B10A2HDR10;

    ComPtr<IWICBitmap> hdr10Bitmap;
    IFRIMG(fact->CreateBitmap(
        width,
        height,
        hdr10Fmt,
        WICBitmapCacheOnLoad,
        &hdr10Bitmap));

    ComPtr<IWICBitmapLock> lock;
    IFRIMG(hdr10Bitmap->Lock({}, WICBitmapLockWrite, &lock));

    UINT lockStride, lockSize = 0;
    WICInProcPointer lockData = nullptr;
    IFRIMG(lock->GetStride(&lockStride));
    IFRIMG(lock->GetDataPointer(&lockSize, &lockData));

    IFRIMG(sourceTransform->CopyPixels(
        {},
        width,
        height,
        &hdr10Fmt, // Assumes we have already checked GetClosestPixelFormat
        WICBitmapTransformRotate0,
        lockStride,
        lockSize,
        lockData));

    IFRIMG(hdr10Bitmap.As(&m_wicCachedSource));
}

/// <summary>
/// Special codepath to generate a D2D image source backed by an HDR10 HEIF image.
/// </summary>
/// <remarks>
/// GUID_WICPixelFormat32bppR10G10B10A2HDR10 has very limited support in WIC and D2D, so instead
/// we must create a full resolution WIC bitmap cache and drop into D3D to upload to GPU.
///
/// Needs to be paired with CreateHeifHdr10CpuResources.
/// </remarks>
void ImageLoader::CreateHeifHdr10GpuResources()
{
    ComPtr<IWICBitmap> wicBitmap;
    ComPtr<IWICBitmapLock> wicLock;
    IFRIMG(m_wicCachedSource.As(&wicBitmap));
    IFRIMG(wicBitmap->Lock({}, WICBitmapLockRead, &wicLock));

    UINT lockStride, lockSize = 0;
    WICInProcPointer lockData = nullptr;
    IFRIMG(wicLock->GetStride(&lockStride));
    IFRIMG(wicLock->GetDataPointer(&lockSize, &lockData));

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = lockData;
    initData.SysMemPitch = lockStride;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<unsigned int>(m_imageInfo.pixelSize.Width);
    desc.Height = static_cast<unsigned int>(m_imageInfo.pixelSize.Height);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    auto d3dDevice = m_deviceResources->GetD3DDevice();
    ComPtr<ID3D11Texture2D> tex;
    IFRIMG(d3dDevice->CreateTexture2D(&desc, &initData, tex.GetAddressOf()));

    ComPtr<IDXGISurface> dxgiSurface;
    IFRIMG(tex.As(&dxgiSurface));
    IDXGISurface* arrSurfaces[] = { dxgiSurface.Get() };

    auto context = m_deviceResources->GetD2DDeviceContext();
    
    IFRIMG(context->CreateImageSourceFromDxgi(
        arrSurfaces,
        ARRAYSIZE(arrSurfaces),
        // Image source doesn't support assigning to the BT.2100 color space.
        // Instead, we must do this ourselves in GetImageColorContext().
        DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
        D2D1_IMAGE_SOURCE_FROM_DXGI_OPTIONS_NONE,
        &m_imageSource));
}
void ImageLoader::CreateHeifSdrGpuResources()
{

    auto context = m_deviceResources->GetD2DDeviceContext();
    auto wicFactory = m_deviceResources->GetWicImagingFactory();

    UINT width = 0, height = 0;
    IFRIMG(m_wicCachedSource->GetSize(&width, &height));

    WICPixelFormatGUID srcFmt = {};
    IFRIMG(m_wicCachedSource->GetPixelFormat(&srcFmt));

    IWICBitmapSource* source = m_wicCachedSource.Get();
    ComPtr<IWICFormatConverter> converter;
    if (srcFmt != GUID_WICPixelFormat32bppPBGRA)
    {
        IFRIMG(wicFactory->CreateFormatConverter(&converter));
        IFRIMG(converter->Initialize(
            source,
            GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone,
            nullptr,
            0.0f,
            WICBitmapPaletteTypeCustom));
        source = converter.Get();
    }

    size_t stride = static_cast<size_t>(width) * 4;
    size_t bufferSize = stride * static_cast<size_t>(height);
    IFRIMG(bufferSize <= UINT_MAX ? S_OK : E_FAIL);

    std::vector<BYTE> pixels;
    pixels.resize(bufferSize);

    IFRIMG(source->CopyPixels(
        nullptr,
        static_cast<UINT>(stride),
        static_cast<UINT>(bufferSize),
        pixels.data()));

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    IFRIMG(context->CreateBitmap(
        D2D1::SizeU(width, height),
        pixels.data(),
        static_cast<UINT>(stride),
        &props,
        &m_heifBitmap));

}


namespace
{
    struct IsoItemInfo
    {
        std::string type;
        std::string name;
    };

    struct IsoBoxHeader
    {
        size_t offset = 0;
        uint64_t size = 0;
        size_t headerSize = 0;
        char type[5] = {};
    };

    static bool ReadU16BE(const BYTE* data, size_t len, size_t offset, uint16_t& outValue)
    {
        if (!data || offset + 2 > len)
        {
            return false;
        }

        outValue = static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);
        return true;
    }

    static bool ReadU32BE(const BYTE* data, size_t len, size_t offset, uint32_t& outValue)
    {
        if (!data || offset + 4 > len)
        {
            return false;
        }

        outValue = (static_cast<uint32_t>(data[offset]) << 24) |
            (static_cast<uint32_t>(data[offset + 1]) << 16) |
            (static_cast<uint32_t>(data[offset + 2]) << 8) |
            static_cast<uint32_t>(data[offset + 3]);
        return true;
    }

    static bool ReadU64BE(const BYTE* data, size_t len, size_t offset, uint64_t& outValue)
    {
        uint32_t high = 0;
        uint32_t low = 0;
        if (!ReadU32BE(data, len, offset, high) || !ReadU32BE(data, len, offset + 4, low))
        {
            return false;
        }

        outValue = (static_cast<uint64_t>(high) << 32) | low;
        return true;
    }

    static bool ReadIsoBoxHeader(const BYTE* data, size_t len, size_t offset, IsoBoxHeader& outBox)
    {
        if (!data || offset + 8 > len)
        {
            return false;
        }

        uint32_t size32 = 0;
        if (!ReadU32BE(data, len, offset, size32))
        {
            return false;
        }

        std::memcpy(outBox.type, data + offset + 4, 4);
        outBox.type[4] = '\0';

        uint64_t boxSize = size32;
        size_t headerSize = 8;
        if (size32 == 1)
        {
            if (!ReadU64BE(data, len, offset + 8, boxSize))
            {
                return false;
            }

            headerSize = 16;
        }
        else if (size32 == 0)
        {
            boxSize = len - offset;
        }

        if (boxSize < headerSize || offset + boxSize > len)
        {
            return false;
        }

        outBox.offset = offset;
        outBox.size = boxSize;
        outBox.headerSize = headerSize;
        return true;
    }

    static std::string ToLowerAscii(const std::string& value)
    {
        std::string out = value;
        for (char& c : out)
        {
            if (c >= 'A' && c <= 'Z')
            {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return out;
    }

    static bool ContainsAscii(const std::string& haystack, const char* needle)
    {
        if (!needle || !*needle)
        {
            return false;
        }

        std::string lowered = ToLowerAscii(haystack);
        std::string needleLower = ToLowerAscii(needle);
        return lowered.find(needleLower) != std::string::npos;
    }

    static void ParseIsoInfeBox(const BYTE* data, size_t len, const IsoBoxHeader& box, std::unordered_map<heif_item_id, IsoItemInfo>& itemInfo)
    {
        const size_t boxEnd = box.offset + static_cast<size_t>(box.size);
        size_t cursor = box.offset + box.headerSize;
        if (cursor + 4 > len || cursor + 4 > boxEnd)
        {
            return;
        }

        const uint8_t version = data[cursor];
        cursor += 4; // version + flags

        heif_item_id itemId = 0;
        if (version == 2)
        {
            uint16_t id16 = 0;
            if (!ReadU16BE(data, len, cursor, id16))
            {
                return;
            }
            itemId = static_cast<heif_item_id>(id16);
            cursor += 2;
            cursor += 2; // item_protection_index
        }
        else if (version == 3)
        {
            uint32_t id32 = 0;
            if (!ReadU32BE(data, len, cursor, id32))
            {
                return;
            }
            itemId = static_cast<heif_item_id>(id32);
            cursor += 4;
            cursor += 2; // item_protection_index
        }
        else
        {
            return;
        }

        if (cursor + 4 > len || cursor + 4 > boxEnd)
        {
            return;
        }

        std::string type(reinterpret_cast<const char*>(data + cursor), 4);
        cursor += 4;

        size_t nameEnd = cursor;
        while (nameEnd < boxEnd && data[nameEnd] != 0)
        {
            ++nameEnd;
        }

        std::string name;
        if (nameEnd > cursor)
        {
            name.assign(reinterpret_cast<const char*>(data + cursor), nameEnd - cursor);
        }

        IsoItemInfo info = {};
        info.type = ToLowerAscii(type);
        info.name = ToLowerAscii(name);
        itemInfo[itemId] = info;
    }

    static void ParseIsoIrefDimgBox(const BYTE* data, size_t len, const IsoBoxHeader& box, uint8_t version,
        std::unordered_map<heif_item_id, std::vector<heif_item_id>>& dimgRefs)
    {
        const size_t boxEnd = box.offset + static_cast<size_t>(box.size);
        size_t cursor = box.offset + box.headerSize;

        while (cursor < boxEnd)
        {
            if (version == 0)
            {
                uint16_t fromId = 0;
                uint16_t refCount = 0;
                if (!ReadU16BE(data, len, cursor, fromId) || !ReadU16BE(data, len, cursor + 2, refCount))
                {
                    break;
                }
                cursor += 4;

                if (cursor + static_cast<size_t>(refCount) * 2 > boxEnd)
                {
                    break;
                }

                auto& refs = dimgRefs[static_cast<heif_item_id>(fromId)];
                for (uint16_t i = 0; i < refCount; ++i)
                {
                    uint16_t toId = 0;
                    if (!ReadU16BE(data, len, cursor, toId))
                    {
                        return;
                    }
                    cursor += 2;
                    refs.push_back(static_cast<heif_item_id>(toId));
                }
            }
            else if (version == 1)
            {
                uint32_t fromId = 0;
                uint16_t refCount = 0;
                if (!ReadU32BE(data, len, cursor, fromId) || !ReadU16BE(data, len, cursor + 4, refCount))
                {
                    break;
                }
                cursor += 6;

                if (cursor + static_cast<size_t>(refCount) * 4 > boxEnd)
                {
                    break;
                }

                auto& refs = dimgRefs[static_cast<heif_item_id>(fromId)];
                for (uint16_t i = 0; i < refCount; ++i)
                {
                    uint32_t toId = 0;
                    if (!ReadU32BE(data, len, cursor, toId))
                    {
                        return;
                    }
                    cursor += 4;
                    refs.push_back(static_cast<heif_item_id>(toId));
                }
            }
            else
            {
                break;
            }
        }
    }

    static bool TryFindIsoTmapGainMapItemId(const BYTE* data, size_t len, heif_item_id& outGainMapId)
    {
        outGainMapId = 0;
        if (!data || len < 16)
        {
            return false;
        }

        heif_item_id primaryId = 0;
        std::unordered_map<heif_item_id, IsoItemInfo> itemInfo;
        std::unordered_map<heif_item_id, std::vector<heif_item_id>> dimgRefs;

        size_t offset = 0;
        while (offset + 8 <= len)
        {
            IsoBoxHeader box = {};
            if (!ReadIsoBoxHeader(data, len, offset, box))
            {
                break;
            }

            if (std::memcmp(box.type, "meta", 4) == 0)
            {
                const size_t metaEnd = box.offset + static_cast<size_t>(box.size);
                size_t childOffset = box.offset + box.headerSize;
                if (childOffset + 4 <= metaEnd)
                {
                    childOffset += 4; // full box header
                }

                while (childOffset + 8 <= metaEnd)
                {
                    IsoBoxHeader child = {};
                    if (!ReadIsoBoxHeader(data, len, childOffset, child))
                    {
                        break;
                    }

                    const size_t childEnd = child.offset + static_cast<size_t>(child.size);
                    if (std::memcmp(child.type, "pitm", 4) == 0)
                    {
                        size_t cursor = child.offset + child.headerSize;
                        if (cursor + 4 <= childEnd)
                        {
                            const uint8_t version = data[cursor];
                            cursor += 4;
                            if (version == 0)
                            {
                                uint16_t id16 = 0;
                                if (ReadU16BE(data, len, cursor, id16))
                                {
                                    primaryId = static_cast<heif_item_id>(id16);
                                }
                            }
                            else if (version == 1)
                            {
                                uint32_t id32 = 0;
                                if (ReadU32BE(data, len, cursor, id32))
                                {
                                    primaryId = static_cast<heif_item_id>(id32);
                                }
                            }
                        }
                    }
                    else if (std::memcmp(child.type, "iinf", 4) == 0)
                    {
                        size_t iinfCursor = child.offset + child.headerSize;
                        if (iinfCursor + 4 <= childEnd)
                        {
                            const uint8_t version = data[iinfCursor];
                            iinfCursor += 4;
                            if (version == 0)
                            {
                                iinfCursor += 2;
                            }
                            else
                            {
                                iinfCursor += 4;
                            }

                            while (iinfCursor + 8 <= childEnd)
                            {
                                IsoBoxHeader infe = {};
                                if (!ReadIsoBoxHeader(data, len, iinfCursor, infe))
                                {
                                    break;
                                }

                                if (std::memcmp(infe.type, "infe", 4) == 0)
                                {
                                    ParseIsoInfeBox(data, len, infe, itemInfo);
                                }

                                iinfCursor += static_cast<size_t>(infe.size);
                            }
                        }
                    }
                    else if (std::memcmp(child.type, "iref", 4) == 0)
                    {
                        size_t irefCursor = child.offset + child.headerSize;
                        if (irefCursor + 4 <= childEnd)
                        {
                            const uint8_t version = data[irefCursor];
                            irefCursor += 4;

                            while (irefCursor + 8 <= childEnd)
                            {
                                IsoBoxHeader refBox = {};
                                if (!ReadIsoBoxHeader(data, len, irefCursor, refBox))
                                {
                                    break;
                                }

                                if (std::memcmp(refBox.type, "dimg", 4) == 0)
                                {
                                    ParseIsoIrefDimgBox(data, len, refBox, version, dimgRefs);
                                }

                                irefCursor += static_cast<size_t>(refBox.size);
                            }
                        }
                    }

                    childOffset += static_cast<size_t>(child.size);
                }
            }

            offset += static_cast<size_t>(box.size);
        }

        std::vector<heif_item_id> tmapItems;
        for (const auto& entry : itemInfo)
        {
            if (entry.second.type == "tmap")
            {
                tmapItems.push_back(entry.first);
            }
        }

        for (heif_item_id tmapId : tmapItems)
        {
            auto refsIt = dimgRefs.find(tmapId);
            if (refsIt == dimgRefs.end())
            {
                continue;
            }

            const auto& refs = refsIt->second;
            heif_item_id candidate = 0;
            for (heif_item_id refId : refs)
            {
                auto infoIt = itemInfo.find(refId);
                if (infoIt != itemInfo.end() && ContainsAscii(infoIt->second.name, "gain"))
                {
                    candidate = refId;
                    break;
                }
            }

            if (candidate == 0 && primaryId != 0)
            {
                for (heif_item_id refId : refs)
                {
                    if (refId != primaryId)
                    {
                        candidate = refId;
                        break;
                    }
                }
            }

            if (candidate != 0)
            {
                outGainMapId = candidate;
                return true;
            }
        }

        return false;
    }
}

/// <summary>
/// Checks if the HEIC image contains an HDR gainmap. If true, initializes the gainmap bitmap.
/// </summary>
/// <param name="imageStream"></param>
/// <returns>0 if none, 5 for ISO HDR, 6 for Apple HDR (HEIC).</returns>
int ImageLoader::TryLoadHdrGainMapHeic(IStream* imageStream)
{

    STATSTG stats = {};
    HRESULT hr = imageStream->Stat(&stats, STATFLAG_NONAME);
    const bool hasStat = SUCCEEDED(hr);
    if (!hasStat)

    if (hasStat && stats.cbSize.QuadPart > UINT_MAX)
    {
        return 0;
    }

    ULARGE_INTEGER seeked = {};
    hr = imageStream->Seek({}, STREAM_SEEK_SET, &seeked);
    if (FAILED(hr))
    {
        return 0;
    }

    const size_t kMaxHeifBytes = 512u * 1024u * 1024u;
    std::vector<byte> fileBuf;
    if (hasStat && stats.cbSize.QuadPart > 0 &&
        stats.cbSize.QuadPart <= static_cast<ULONGLONG>(kMaxHeifBytes))
    {
        fileBuf.reserve(static_cast<size_t>(stats.cbSize.QuadPart));
    }

    const ULONG kChunkSize = 1024u * 1024u;
    std::vector<byte> chunk(kChunkSize);
    for (;;)
    {
        ULONG cbRead = 0;
        hr = imageStream->Read(chunk.data(), kChunkSize, &cbRead);
        if (FAILED(hr))
        {
            return 0;
        }

        if (cbRead == 0)
        {
            break;
        }

        if (fileBuf.size() + cbRead > kMaxHeifBytes)
        {
            return 0;
        }

        fileBuf.insert(fileBuf.end(), chunk.data(), chunk.data() + cbRead);
    }

    if (fileBuf.empty())
    {
        return 0;
    }


    imageStream->Seek({}, STREAM_SEEK_SET, nullptr);

    CHeifContext ctx;
    heif_error herr = heif_context_read_from_memory_without_copy(ctx.ptr, fileBuf.data(), fileBuf.size(), nullptr);
    if (herr.code != heif_error_code::heif_error_Ok)
    {

        CHeifContext ctxCopy;
        herr = heif_context_read_from_memory(ctxCopy.ptr, fileBuf.data(), fileBuf.size(), nullptr);
        if (herr.code != heif_error_code::heif_error_Ok)
        {
            return 0;
        }

        heif_context* tmp = ctx.ptr;
        ctx.ptr = ctxCopy.ptr;
        ctxCopy.ptr = tmp;
    }

    CHeifHandle mainHandle;
    herr = heif_context_get_primary_image_handle(ctx.ptr, &mainHandle.ptr);
    if (herr.code != heif_error_code::heif_error_Ok)
    {
        return 0;
    }

    int countAux = heif_image_handle_get_number_of_auxiliary_images(mainHandle.ptr, 0);
    std::vector<heif_item_id> auxIds(countAux);
    heif_image_handle_get_list_of_auxiliary_image_IDs(mainHandle.ptr, 0, auxIds.data(), static_cast<int>(auxIds.size()));

    auto tryDecodeGainMapHandle = [&](heif_image_handle* handle) -> bool
    {
        if (!handle)
        {
            return false;
        }

        auto fact = m_deviceResources->GetWicImagingFactory();

        auto createWicFromGray = [&](int width, int height, int stride, uint8_t* data) -> bool
        {
            ComPtr<IWICBitmap> bitmap;
            if (FAILED(fact->CreateBitmapFromMemory(
                width,
                height,
                GUID_WICPixelFormat8bppGray,
                stride,
                stride * height,
                static_cast<BYTE*>(data),
                &bitmap)))
            {
                return false;
            }

            ComPtr<IWICFormatConverter> fmt;
            if (FAILED(fact->CreateFormatConverter(&fmt)))
            {
                return false;
            }
            if (FAILED(fmt->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
            {
                return false;
            }

            return SUCCEEDED(fmt.As(&m_appleHdrGainMap.wicSource));
        };

        auto createWicFromRgb = [&](int width, int height, int stride, uint8_t* data) -> bool
        {
            ComPtr<IWICBitmap> bitmap;
            if (FAILED(fact->CreateBitmapFromMemory(
                width,
                height,
                GUID_WICPixelFormat24bppRGB,
                stride,
                stride * height,
                static_cast<BYTE*>(data),
                &bitmap)))
            {
                return false;
            }

            ComPtr<IWICFormatConverter> fmt;
            if (FAILED(fact->CreateFormatConverter(&fmt)))
            {
                return false;
            }
            if (FAILED(fmt->Initialize(bitmap.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom)))
            {
                return false;
            }

            return SUCCEEDED(fmt.As(&m_appleHdrGainMap.wicSource));
        };

        heif_error decodeErr = heif_decode_image(handle, &m_appleHdrGainMap.ptr, heif_colorspace_monochrome, heif_chroma_monochrome, 0);
        if (decodeErr.code == heif_error_code::heif_error_Ok)
        {
            int width = heif_image_get_primary_width(m_appleHdrGainMap.ptr);
            int height = heif_image_get_primary_height(m_appleHdrGainMap.ptr);
            int bitdepth = heif_image_get_bits_per_pixel_range(m_appleHdrGainMap.ptr, heif_channel_Y);


            int stride = 0;
            uint8_t* data = heif_image_get_plane(m_appleHdrGainMap.ptr, heif_channel_Y, &stride);

            if (bitdepth == 8 && data != nullptr && createWicFromGray(width, height, stride, data))
            {
                return true;
            }

            if (m_appleHdrGainMap.ptr != nullptr)
            {
                heif_image_release(m_appleHdrGainMap.ptr);
                m_appleHdrGainMap.ptr = nullptr;
            }
        }

        decodeErr = heif_decode_image(handle, &m_appleHdrGainMap.ptr, heif_colorspace_RGB, heif_chroma_interleaved_RGB, 0);
        if (decodeErr.code != heif_error_code::heif_error_Ok)
        {
            return false;
        }

        int width = heif_image_get_primary_width(m_appleHdrGainMap.ptr);
        int height = heif_image_get_primary_height(m_appleHdrGainMap.ptr);
        int bitdepth = heif_image_get_bits_per_pixel_range(m_appleHdrGainMap.ptr, heif_channel_interleaved);
        int stride = 0;
        uint8_t* data = heif_image_get_plane(m_appleHdrGainMap.ptr, heif_channel_interleaved, &stride);


        if (bitdepth != 8 || data == nullptr)
        {
            return false;
        }

        return createWicFromRgb(width, height, stride, data);
    };

    for (auto i : auxIds)
    {
        CHeifHandle auxHandle;
        IFRF(HEIFHR(heif_image_handle_get_auxiliary_image_handle(mainHandle.ptr, i, &auxHandle.ptr)));

        CHeifAuxType type;
        IFRF(HEIFHR(heif_image_handle_get_auxiliary_type(auxHandle.ptr, &type.ptr)));

        const bool isAppleGainMap = type.IsAppleHdrGainMap();
        const bool isIsoGainMap = type.IsIsoHdrGainMap();

        if (isAppleGainMap || isIsoGainMap)
        {
            if (!tryDecodeGainMapHandle(auxHandle.ptr))
            {
                return 0;
            }

            if (!fileBuf.empty())
            {
                TryUpdateGainMapMetadataFromBytes(reinterpret_cast<const BYTE*>(fileBuf.data()), fileBuf.size());
            }

            return isAppleGainMap ? 6 : 5;
        }
    }

    heif_item_id gainMapItemId = 0;
    if (TryFindIsoTmapGainMapItemId(reinterpret_cast<const BYTE*>(fileBuf.data()), fileBuf.size(), gainMapItemId))
    {

        CHeifHandle gainHandle;
        heif_error herr = heif_context_get_image_handle(ctx.ptr, gainMapItemId, &gainHandle.ptr);
        if (herr.code != heif_error_code::heif_error_Ok)
        {
            return 0;
        }

        if (!tryDecodeGainMapHandle(gainHandle.ptr))
        {
            return 0;
        }

        if (!fileBuf.empty())
        {
            TryUpdateGainMapMetadataFromBytes(reinterpret_cast<const BYTE*>(fileBuf.data()), fileBuf.size());
        }

        return 5;
    }

    return 0;
}

/// <summary>
/// Checks if a JPEG image contains an Apple HDR gainmap stored in an MPO (Multi picture object). If true, initializes the gainmap bitmap.
/// </summary>
/// <param name="imageStream">Underlying stream is needed since we have to manually setup WIC to read the second Individual Image.</param>
/// <param name="frame"></param>
/// <returns></returns>
int ImageLoader::TryLoadCuvaHdrGainMapJpegMpo(IStream* imageStream, IWICBitmapFrameDecode* frame)
{
    
    auto fact = m_deviceResources->GetWicImagingFactory();
    STATSTG stats = {};
    IFRF(imageStream->Stat(&stats, STATFLAG_NONAME));



    // Heuristic: Allow any Apple manufactured device.
    ComPtr<IWICMetadataQueryReader> query;
    CPropVariant cuvaMftr;
    CPropVariant propvarTitle;

    if (SUCCEEDED(frame->GetMetadataQueryReader(&query)))
    {
        query->GetMetadataByName(L"/app1/ifd/{ushort=270}", &propvarTitle);
        query->GetMetadataByName(L"/app1/ifd/{ushort=271}", &cuvaMftr);
    }

    LARGE_INTEGER zero = {};
    imageStream->Seek(zero, STREAM_SEEK_SET, nullptr);
    

    jpegData.resize(stats.cbSize.QuadPart);
    ULONG read = 0;
    imageStream->Read(jpegData.data(), stats.cbSize.QuadPart, &read);

    int len = stats.cbSize.QuadPart;
    firstStart = -1, firstEnd = -1;
    secondStart = -1, secondEnd = -1;

    mainImageStart = -1, mainImageEnd = -1;
    thumbnailStart = -1, thumbnailEnd = -1;

    sdrSize -1;
    gainSize = -1;

    int gainmap_sos = -1;
    int gainmap_eoi = -1;
    int app2_start = -1, app2_end = -1;
    int gainmap_start = -1;

    const BYTE exif_signature[6] = { 0x45, 0x78, 0x69, 0x66, 0x00, 0x00 };
    size_t exif_pos = -1;
    exif_result.has_exif = false;
    exif_result.exif_ptr = nullptr;

    int gainmapType = 0;

    for (int i = 0; i < len - 10; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xE1) {

            // Validate EXIF signature.
            if (i + 9 < len &&
                memcmp(&jpegData[i + 4], exif_signature, sizeof(exif_signature)) == 0) {

                // Set EXIF position.
                exif_pos = i + 4;
                exif_result.exif_pos = i + 4;

                exif_result.exif_ptr = &jpegData[exif_pos];
                exif_result.has_exif = true;

                break;
            }
        }
    }
    
    for (int i = 0; i < len - 3; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD8 &&
            jpegData[i + 2] == 0xFF && jpegData[i + 3] == 0xE0)
        {
            if (exif_result.has_exif)
            {
                exif_result.exif_size = i - exif_result.exif_pos;
            }
            
            firstStart = i;
            thumbnailStart = i + 2;
            break;
        }
    }

    if (firstStart < 0)
    {
        for (int i = 0; i < len - 1; ++i)
        {
            if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD8)
            {
                firstStart = i;
                thumbnailStart = i + 2;
                break;
            }
        }
    }

    if (firstStart < 0)
    {
        return 0;
    }

    for (int i = firstStart+ 5; i < len - 1; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xE0)
        {
            thumbnailEnd = i - 1;
            mainImageStart = i;
            
            break;
        }
    }


    for (int i = firstStart; i < len - 3; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD8 &&
            jpegData[i + 2] == 0xFF && jpegData[i + 3] == 0xE2)
        {
            mainImageEnd = i - 3;
            firstEnd = i - 1;
            secondStart = i;
            gainmapType = 2;
            break;
        }
    }

    if (secondStart == -1) {
        bool isHuawei = false;
        if (cuvaMftr.vt == VT_LPSTR && cuvaMftr.pszVal != nullptr)
        {
            isHuawei = _stricmp(cuvaMftr.pszVal, "HUAWEI") == 0;
        }
        else if (cuvaMftr.vt == VT_LPWSTR && cuvaMftr.pwszVal != nullptr)
        {
            isHuawei = _wcsicmp(cuvaMftr.pwszVal, L"HUAWEI") == 0;
        }

        if (!isHuawei) return 0;

        for (int i = firstStart; i < len - 3; ++i)
        {
            if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD8 &&
                jpegData[i + 2] == 0xFF && jpegData[i + 3] == 0xE5)
            {
                mainImageEnd = i - 3;
                firstEnd = i - 1;
                secondStart = i;
                gainmapType = 1;
                break;
            }
        }
    }

    for (int i = secondStart; i < len - 1; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD9)
        {
            secondEnd = i + 1;
            break;
        }
    }

    if (secondStart == -1 || secondEnd == -1) {
        return 0;
    }

    sdrSize = firstEnd - firstStart + 1;
    gainSize = secondEnd - secondStart + 1;

    sdrData.clear();
    sdrData.resize(sdrSize);

    sdrData_changed.clear();
    sdrData_changed.resize(sdrSize);
    
    gainmapData.clear();
    gainmapData.resize(gainSize);
    
    std::memcpy(sdrData.data(), jpegData.data() + firstStart, sdrSize);
    std::memcpy(gainmapData.data(), jpegData.data() + secondStart, gainSize);

    ULARGE_INTEGER ignore = {};

    ULARGE_INTEGER gainmapOffset_cuva;
    gainmapOffset_cuva.QuadPart = static_cast<ULONGLONG>(secondStart);

    // Separate streams are needed because we have two live decoders.
    ULARGE_INTEGER region = {};
    region.QuadPart = stats.cbSize.QuadPart - gainmapOffset_cuva.QuadPart;
    ComPtr<IWICStream> gainmapStream;
    IFRF(fact->CreateStream(&gainmapStream));
    IFRF(gainmapStream->InitializeFromIStreamRegion(imageStream, gainmapOffset_cuva, region));

    ComPtr<IWICBitmapDecoder> gainmapDecoder;
    IFRF(fact->CreateDecoderFromStream(gainmapStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &gainmapDecoder));
    ComPtr<IWICBitmapFrameDecode> gainmapFrame;
    IFRF(gainmapDecoder->GetFrame(0, &gainmapFrame));
    ComPtr<IWICMetadataQueryReader> gainmapQuery;
    IFRF(gainmapFrame->GetMetadataQueryReader(&gainmapQuery));

    UINT width = 0, height = 0;
    HRESULT hr = gainmapFrame->GetSize(&width, &height);
    if (SUCCEEDED(hr))
    {
        wchar_t buf[100] = {};
        // %u 用于无符号整数
        swprintf(buf, ARRAYSIZE(buf), L"gainmapFrame Size = %u x %u\n", width, height);
        OutputDebugString(buf);
    }

    ComPtr<IWICFormatConverter> fmt;
    IFRF(fact->CreateFormatConverter(&fmt));
    GUID pixelFormat = {};
    hr = gainmapFrame->GetPixelFormat(&pixelFormat);

    // Convert to 32bpp PBGRA with premultiplied alpha.
    hr = fmt->Initialize(
        gainmapFrame.Get(),
        GUID_WICPixelFormat32bppPBGRA,   // Target format.
        WICBitmapDitherTypeNone,         // No dithering.
        nullptr,                         // No palette.
        0.0f,                            // Alpha threshold.
        WICBitmapPaletteTypeCustom       // Custom palette.
    );

    IFRF(fmt.As(&m_appleHdrGainMap.wicSource));

    if (gainmapType != 0 && !jpegData.empty())
    {
        TryUpdateGainMapMetadataFromBytes(jpegData.data(), jpegData.size());
    }


    return gainmapType;
}

bool ImageLoader::TryLoadIsoHdrGainMapJpegMpo(IStream* imageStream, IWICBitmapFrameDecode* frame)
{
    UNREFERENCED_PARAMETER(frame);

    auto fact = m_deviceResources->GetWicImagingFactory();
    STATSTG stats = {};
    IFRF(imageStream->Stat(&stats, STATFLAG_NONAME));

    if (stats.cbSize.QuadPart <= 4) return false;

    LARGE_INTEGER zero = {};
    imageStream->Seek(zero, STREAM_SEEK_SET, nullptr);

    jpegData.clear();
    jpegData.resize(static_cast<size_t>(stats.cbSize.QuadPart));
    ULONG read = 0;
    IFRF(imageStream->Read(jpegData.data(), static_cast<ULONG>(jpegData.size()), &read));
    if (read != jpegData.size()) return false;

    const size_t len = jpegData.size();
    const BYTE exif_signature[6] = { 0x45, 0x78, 0x69, 0x66, 0x00, 0x00 };

    exif_result.has_exif = false;
    exif_result.exif_ptr = nullptr;
    exif_result.exif_size = 0;
    exif_result.exif_pos = 0;

    for (size_t i = 0; i + 10 < len; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xE1)
        {
            uint16_t segLen = static_cast<uint16_t>(jpegData[i + 2] << 8 | jpegData[i + 3]);
            if (segLen < 2) continue;
            size_t segStart = i + 4;
            size_t segDataLen = segLen - 2;
            if (segStart + segDataLen > len) continue;

            if (segDataLen >= sizeof(exif_signature) &&
                memcmp(&jpegData[segStart], exif_signature, sizeof(exif_signature)) == 0)
            {
                exif_result.exif_pos = segStart;
                exif_result.exif_ptr = &jpegData[segStart];
                exif_result.exif_size = segDataLen;
                exif_result.has_exif = true;
                break;
            }
        }
    }

    auto read16 = [&](size_t offset, bool bigEndian) -> uint16_t
    {
        if (bigEndian)
        {
            return static_cast<uint16_t>(jpegData[offset] << 8 | jpegData[offset + 1]);
        }
        return static_cast<uint16_t>(jpegData[offset] | (jpegData[offset + 1] << 8));
    };

    auto read32 = [&](size_t offset, bool bigEndian) -> uint32_t
    {
        if (bigEndian)
        {
            return static_cast<uint32_t>(jpegData[offset] << 24 |
                jpegData[offset + 1] << 16 |
                jpegData[offset + 2] << 8 |
                jpegData[offset + 3]);
        }
        return static_cast<uint32_t>(jpegData[offset] |
            (jpegData[offset + 1] << 8) |
            (jpegData[offset + 2] << 16) |
            (jpegData[offset + 3] << 24));
    };

    auto hasIsoApp2 = [&](size_t imageStart, size_t imageEnd) -> bool
    {
        static const char kIsoNamespace[] = "urn:iso:std:iso:ts:21496:-1";
        const size_t namespaceLen = sizeof(kIsoNamespace);

        if (imageEnd <= imageStart + 4) return false;
        size_t pos = imageStart + 2;
        const size_t end = imageEnd + 1;

        while (pos + 4 <= end)
        {
            if (jpegData[pos] != 0xFF)
            {
                pos++;
                continue;
            }

            BYTE marker = jpegData[pos + 1];
            if (marker == 0xDA) break;
            if (marker == 0xD8 || marker == 0xD9)
            {
                pos += 2;
                continue;
            }
            if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7))
            {
                pos += 2;
                continue;
            }
            if (pos + 4 > end) break;

            uint16_t segLen = static_cast<uint16_t>(jpegData[pos + 2] << 8 | jpegData[pos + 3]);
            if (segLen < 2) break;

            size_t segStart = pos + 4;
            size_t segDataLen = segLen - 2;
            if (segStart + segDataLen > end) break;

            if (marker == 0xE2 && segDataLen >= namespaceLen &&
                memcmp(jpegData.data() + segStart, kIsoNamespace, namespaceLen) == 0)
            {
                return true;
            }

            pos = segStart + segDataLen;
        }

        return false;
    };

    auto findSecondaryStartFromMpf = [&](size_t* outStart) -> bool
    {
        for (size_t i = 0; i + 10 < len; ++i)
        {
            if (jpegData[i] != 0xFF || jpegData[i + 1] != 0xE2) continue;

            uint16_t segLen = static_cast<uint16_t>(jpegData[i + 2] << 8 | jpegData[i + 3]);
            if (segLen < 2) return false;

            size_t segStart = i + 4;
            size_t segDataLen = segLen - 2;
            size_t segEnd = segStart + segDataLen;
            if (segEnd > len) return false;

            if (segDataLen >= 4 && memcmp(jpegData.data() + segStart, "MPF\0", 4) == 0)
            {
                size_t tiffStart = segStart + 4;
                if (tiffStart + 8 > segEnd) return false;

                bool bigEndian = jpegData[tiffStart] == 0x4D && jpegData[tiffStart + 1] == 0x4D;
                bool littleEndian = jpegData[tiffStart] == 0x49 && jpegData[tiffStart + 1] == 0x49;
                if (!bigEndian && !littleEndian) return false;

                uint32_t ifdOffset = read32(tiffStart + 4, bigEndian);
                size_t ifdStart = tiffStart + ifdOffset;
                if (ifdStart + 2 > segEnd) return false;

                uint16_t entryCount = read16(ifdStart, bigEndian);
                size_t entryPos = ifdStart + 2;
                size_t entryBytes = static_cast<size_t>(entryCount) * 12;
                if (entryPos + entryBytes > segEnd) return false;

                uint32_t mpEntryOffset = 0;
                uint32_t mpEntryCount = 0;
                for (uint16_t e = 0; e < entryCount; ++e)
                {
                    size_t base = entryPos + static_cast<size_t>(e) * 12;
                    uint16_t tag = read16(base, bigEndian);
                    if (tag == 0xB002)
                    {
                        mpEntryCount = read32(base + 4, bigEndian);
                        mpEntryOffset = read32(base + 8, bigEndian);
                        break;
                    }
                }

                if (mpEntryOffset == 0 || mpEntryCount < 32) return false;

                size_t mpEntryStart = tiffStart + mpEntryOffset;
                if (mpEntryStart + 32 > segEnd) return false;

                size_t secondEntry = mpEntryStart + 16;
                uint32_t secondOffset = read32(secondEntry + 8, bigEndian);
                size_t candidate = tiffStart + secondOffset;
                if (candidate + 1 >= len) return false;
                if (jpegData[candidate] != 0xFF || jpegData[candidate + 1] != 0xD8) return false;

                *outStart = candidate;
                return true;
            }

            i = segEnd > 0 ? segEnd - 1 : i;
        }

        return false;
    };

    size_t firstSoi = 0;
    bool foundSoi = false;
    for (size_t i = 0; i + 1 < len; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD8)
        {
            firstSoi = i;
            foundSoi = true;
            break;
        }
    }
    if (!foundSoi) return false;

    size_t secondSoi = 0;
    bool mpfFromMpf = findSecondaryStartFromMpf(&secondSoi);
    bool hasSecond = mpfFromMpf;
    if (!hasSecond)
    {
        size_t firstEoiFallback = 0;
        for (size_t i = firstSoi + 2; i + 1 < len; ++i)
        {
            if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD9)
            {
                firstEoiFallback = i + 1;
                break;
            }
        }
        if (firstEoiFallback == 0) return false;

        for (size_t i = firstEoiFallback + 1; i + 1 < len; ++i)
        {
            if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD8)
            {
                secondSoi = i;
                hasSecond = true;
                break;
            }
        }
        if (!hasSecond) return false;
    }

    size_t firstEoi = 0;
    if (secondSoi > 1)
    {
        for (size_t i = secondSoi; i-- > 1;)
        {
            if (jpegData[i - 1] == 0xFF && jpegData[i] == 0xD9)
            {
                firstEoi = i;
                break;
            }
        }
    }
    if (firstEoi == 0)
    {
        for (size_t i = firstSoi + 2; i + 1 < len; ++i)
        {
            if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD9)
            {
                firstEoi = i + 1;
                break;
            }
        }
    }
    if (firstEoi == 0) return false;

    size_t secondEoi = 0;
    for (size_t i = secondSoi + 2; i + 1 < len; ++i)
    {
        if (jpegData[i] == 0xFF && jpegData[i + 1] == 0xD9)
        {
            secondEoi = i + 1;
            break;
        }
    }
    if (secondEoi == 0) return false;

    if (!hasIsoApp2(secondSoi, secondEoi)) return false;

    if (!jpegData.empty())
    {
        TryUpdateGainMapMetadataFromBytes(jpegData.data(), jpegData.size());
    }

    firstStart = static_cast<int>(firstSoi);
    firstEnd = static_cast<int>(firstEoi);
    secondStart = static_cast<int>(secondSoi);
    secondEnd = static_cast<int>(secondEoi);

    sdrSize = static_cast<size_t>(firstEnd - firstStart + 1);
    gainSize = static_cast<size_t>(secondEnd - secondStart + 1);

    sdrData.clear();
    sdrData.resize(sdrSize);

    sdrData_changed.clear();
    sdrData_changed.resize(sdrSize);

    gainmapData.clear();
    gainmapData.resize(gainSize);

    std::memcpy(sdrData.data(), jpegData.data() + firstStart, sdrSize);
    std::memcpy(gainmapData.data(), jpegData.data() + secondStart, gainSize);

    ULARGE_INTEGER gainmapOffset = {};
    gainmapOffset.QuadPart = static_cast<ULONGLONG>(secondStart);

    ULARGE_INTEGER region = {};
    region.QuadPart = stats.cbSize.QuadPart - gainmapOffset.QuadPart;

    ComPtr<IWICStream> gainmapStream;
    IFRF(fact->CreateStream(&gainmapStream));
    IFRF(gainmapStream->InitializeFromIStreamRegion(imageStream, gainmapOffset, region));

    ComPtr<IWICBitmapDecoder> gainmapDecoder;
    IFRF(fact->CreateDecoderFromStream(gainmapStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &gainmapDecoder));
    ComPtr<IWICBitmapFrameDecode> gainmapFrame;
    IFRF(gainmapDecoder->GetFrame(0, &gainmapFrame));

    ComPtr<IWICFormatConverter> fmt;
    IFRF(fact->CreateFormatConverter(&fmt));
    IFRF(fmt->Initialize(
        gainmapFrame.Get(),
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr,
        0.0f,
        WICBitmapPaletteTypeCustom));

    IFRF(fmt.As(&m_appleHdrGainMap.wicSource));

    return true;
}

bool ImageLoader::TryLoadAppleHdrGainMapJpegMpo(IStream* imageStream, IWICBitmapFrameDecode* frame)
{
    auto fact = m_deviceResources->GetWicImagingFactory();

    // Heuristic: Allow any Apple manufactured device.
    ComPtr<IWICMetadataQueryReader> query;
    CPropVariant appleMftr;

    IFRF(frame->GetMetadataQueryReader(&query));
    IFRF(query->GetMetadataByName(L"/app1/ifd/{ushort=271}", &appleMftr));

    if (appleMftr.vt != VT_LPSTR) return false;
    if (strcmp("Apple", appleMftr.pszVal) != 0) return false;

    ULARGE_INTEGER gainmapOffset = {};
    std::vector<BYTE> fileBuf;
    auto tryFindMpfOffset = [&]() -> bool
    {
        STATSTG stats = {};
        if (FAILED(imageStream->Stat(&stats, STATFLAG_NONAME))) return false;
        if (stats.cbSize.QuadPart <= 4) return false;

        fileBuf.resize(static_cast<size_t>(stats.cbSize.QuadPart));

        LARGE_INTEGER zero = {};
        imageStream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        if (FAILED(imageStream->Read(fileBuf.data(), static_cast<ULONG>(fileBuf.size()), &read))) return false;
        if (read != fileBuf.size()) return false;

        const size_t len = fileBuf.size();

        auto read16 = [&](size_t offset, bool bigEndian) -> uint16_t
        {
            if (bigEndian)
            {
                return static_cast<uint16_t>(fileBuf[offset] << 8 | fileBuf[offset + 1]);
            }
            return static_cast<uint16_t>(fileBuf[offset] | (fileBuf[offset + 1] << 8));
        };

        auto read32 = [&](size_t offset, bool bigEndian) -> uint32_t
        {
            if (bigEndian)
            {
                return static_cast<uint32_t>(fileBuf[offset] << 24 |
                    fileBuf[offset + 1] << 16 |
                    fileBuf[offset + 2] << 8 |
                    fileBuf[offset + 3]);
            }
            return static_cast<uint32_t>(fileBuf[offset] |
                (fileBuf[offset + 1] << 8) |
                (fileBuf[offset + 2] << 16) |
                (fileBuf[offset + 3] << 24));
        };

        for (size_t i = 0; i + 10 < len; ++i)
        {
            if (fileBuf[i] != 0xFF || fileBuf[i + 1] != 0xE2) continue;

            uint16_t segLen = static_cast<uint16_t>(fileBuf[i + 2] << 8 | fileBuf[i + 3]);
            if (segLen < 2) continue;

            size_t segStart = i + 4;
            size_t segDataLen = segLen - 2;
            size_t segEnd = segStart + segDataLen;
            if (segEnd > len) continue;

            if (segDataLen >= 4 && memcmp(fileBuf.data() + segStart, "MPF\0", 4) == 0)
            {
                size_t tiffStart = segStart + 4;
                if (tiffStart + 8 > segEnd) continue;

                bool bigEndian = fileBuf[tiffStart] == 0x4D && fileBuf[tiffStart + 1] == 0x4D;
                bool littleEndian = fileBuf[tiffStart] == 0x49 && fileBuf[tiffStart + 1] == 0x49;
                if (!bigEndian && !littleEndian) continue;

                uint32_t ifdOffset = read32(tiffStart + 4, bigEndian);
                size_t ifdStart = tiffStart + ifdOffset;
                if (ifdStart + 2 > segEnd) continue;

                uint16_t entryCount = read16(ifdStart, bigEndian);
                size_t entryPos = ifdStart + 2;
                size_t entryBytes = static_cast<size_t>(entryCount) * 12;
                if (entryPos + entryBytes > segEnd) continue;

                uint32_t mpEntryOffset = 0;
                uint32_t mpEntryCount = 0;
                for (uint16_t e = 0; e < entryCount; ++e)
                {
                    size_t base = entryPos + static_cast<size_t>(e) * 12;
                    uint16_t tag = read16(base, bigEndian);
                    if (tag == 0xB002)
                    {
                        mpEntryCount = read32(base + 4, bigEndian);
                        mpEntryOffset = read32(base + 8, bigEndian);
                        break;
                    }
                }

                if (mpEntryOffset == 0 || mpEntryCount < 32) continue;

                size_t mpEntryStart = tiffStart + mpEntryOffset;
                if (mpEntryStart + 32 > segEnd) continue;

                size_t secondEntry = mpEntryStart + 16;
                uint32_t secondOffset = read32(secondEntry + 8, bigEndian);
                size_t candidate = tiffStart + secondOffset;
                if (candidate + 1 >= len) continue;
                if (fileBuf[candidate] != 0xFF || fileBuf[candidate + 1] != 0xD8) continue;

                gainmapOffset.QuadPart = static_cast<ULONGLONG>(candidate);
                imageStream->Seek(zero, STREAM_SEEK_SET, nullptr);
                return true;
            }
        }

        imageStream->Seek(zero, STREAM_SEEK_SET, nullptr);
        return false;
    };

    if (!tryFindMpfOffset())
    {
        // Find the APP2 MP Extensions block
        ComPtr<IWICMetadataBlockReader> blockReader;
        IFRF(frame->QueryInterface(IID_PPV_ARGS(&blockReader)));

        UINT count = 0;
        IFRF(blockReader->GetCount(&count));

        // WIC doesn't natively understand the APP2 MPF block so we have to iterate and look for it ourselves.
        for (UINT i = 0; i < count; i++)
        {
            ComPtr<IWICMetadataReader> reader;
            IFRF(blockReader->GetReaderByIndex(i, &reader));

            // NOTE: From this point in the loop, any failures should just continue to the next block.
            GUID metaFmt = {};
            IFRF(reader->GetMetadataFormat(&metaFmt));
            if (metaFmt != GUID_MetadataFormatUnknown) continue;

            CPropVariant id, value;
            IFRF(reader->GetValueByIndex(0, nullptr, &id, &value));
            if (value.vt != 65) continue; // VT_BLOB
            if (value.blob.cbSize != sizeof(m_appleApp2MPBlock)) continue;

            // Grab the offset before it's wiped out by the validity check.
            assert(m_appleApp2MPBlockMagicOffset < value.blob.cbSize);

            // The known APP2 header specifies Big Endian.
            ULARGE_INTEGER tempOffset = {};
            tempOffset.QuadPart =
                value.blob.pBlobData[m_appleApp2MPBlockMagicOffset + 0] << 24 |
                value.blob.pBlobData[m_appleApp2MPBlockMagicOffset + 1] << 16 |
                value.blob.pBlobData[m_appleApp2MPBlockMagicOffset + 2] << 8 |
                value.blob.pBlobData[m_appleApp2MPBlockMagicOffset + 3];

            // Fill in the known dynamic bytes with dummy values (0xFF).
            for (int j = 0; j < ARRAYSIZE(m_appleApp2MPBlockDynamicBytes); j++)
            {
                assert(m_appleApp2MPBlockDynamicBytes[j] < value.blob.cbSize);
                value.blob.pBlobData[m_appleApp2MPBlockDynamicBytes[j]] = 0xFF;
            }

            // A not so robust check against magic values since this is much simpler than a true parser.
            if (memcmp(value.blob.pBlobData, m_appleApp2MPBlock, sizeof(m_appleApp2MPBlock)) != 0) continue;

            // If we get here we've validated all of the data we can in the primary image and should move to the second image.
            gainmapOffset = tempOffset;
            break;
        }
    }

    if (gainmapOffset.QuadPart == 0) return false;

    // Initialize the secondary image (HDR gainmap) and validate it.
    // TODO: Apple MPO images may have a gap between the primary image EOI and second image SOI.
    ULARGE_INTEGER ignore = {};
    STATSTG stats = {};
    IFRF(imageStream->Stat(&stats, STATFLAG_NONAME));

    // Separate streams are needed because we have two live decoders.
    ULARGE_INTEGER region = {};
    region.QuadPart = stats.cbSize.QuadPart - gainmapOffset.QuadPart;
    ComPtr<IWICStream> gainmapStream;
    IFRF(fact->CreateStream(&gainmapStream));
    IFRF(gainmapStream->InitializeFromIStreamRegion(imageStream, gainmapOffset, region));

    ComPtr<IWICBitmapDecoder> gainmapDecoder;
    IFRF(fact->CreateDecoderFromStream(gainmapStream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &gainmapDecoder));
    ComPtr<IWICBitmapFrameDecode> gainmapFrame;
    IFRF(gainmapDecoder->GetFrame(0, &gainmapFrame));
    ComPtr<IWICMetadataQueryReader> gainmapQuery;
    IFRF(gainmapFrame->GetMetadataQueryReader(&gainmapQuery));

    CPropVariant gainmapAuxType;
    IFRF(gainmapQuery->GetMetadataByName(L"/xmp/{wstr=http://ns.apple.com/pixeldatainfo/1.0/}:AuxiliaryImageType", &gainmapAuxType));
    if (wcscmp(gainmapAuxType.pwszVal, L"urn:com:apple:photo:2020:aux:hdrgainmap") != 0) return false;

    CPropVariant gainmapVersion;
    IFRF(gainmapQuery->GetMetadataByName(L"/xmp/{wstr=http://ns.apple.com/HDRGainMap/1.0/}:HDRGainMapVersion", &gainmapVersion));
    if (gainmapVersion.vt != VT_LPWSTR || gainmapVersion.pwszVal == nullptr) return false;
    wchar_t* end = nullptr;
    long versionValue = wcstol(gainmapVersion.pwszVal, &end, 10);
    if (end == gainmapVersion.pwszVal || versionValue <= 0) return false;

    if (!fileBuf.empty())
    {
        TryUpdateGainMapMetadataFromBytes(fileBuf.data(), fileBuf.size());
    }

    // All validated, now grab the data.
    ComPtr<IWICFormatConverter> fmt;
    IFRF(fact->CreateFormatConverter(&fmt));
    IFRF(fmt->Initialize(gainmapFrame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom));

    // Just stuff the WIC pointer in here even though we don't have an associated heif_image.
    IFRF(fmt.As(&m_appleHdrGainMap.wicSource));

    return true;
}

/// <summary>
/// (Re)initializes all long-lived device dependent resources.
/// </summary>
void ImageLoader::CreateDeviceDependentResourcesInternal()
{
    EnforceStates(2, ImageLoaderState::NotInitialized, ImageLoaderState::NeedDeviceResources);

    auto d2dFactory = m_deviceResources->GetD2DFactory();
    auto context = m_deviceResources->GetD2DDeviceContext();

    // Load the image from WIC using ID2D1ImageSource.
    if (m_imageInfo.isHeif == true &&
        m_imageInfo.forceBT2100ColorSpace == true)
    {
        CreateHeifHdr10GpuResources();
    }
    else if (m_imageInfo.isHeif == true)
    {
        CreateHeifSdrGpuResources();
    }
    else
    {
        ComPtr<ID2D1ImageSourceFromWic> wicImageSource;
        IFRIMG(context->CreateImageSourceFromWic(m_wicCachedSource.Get(), &wicImageSource));
        IFRIMG(wicImageSource.As(&m_imageSource));
    }

    if (m_imageInfo.hasAppleHdrGainMap || m_imageInfo.hasIsoHeicHdrGainMap || m_imageInfo.hasAppleHeicHdrGainMap)
    {
        ComPtr<ID2D1ImageSourceFromWic> wicGainMapSource;
        IFRIMG(context->CreateImageSourceFromWic(m_appleHdrGainMap.wicSource.Get(), &wicGainMapSource));
        IFRIMG(wicGainMapSource.As(&m_hdrGainMapSource));
    }

    // Xbox One HDR screenshots and HEIF HDR images use the HDR10/BT.2100 colorspace, but this is not represented
    // in a WIC color context so we must manually set behavior.
    if (m_imageInfo.forceBT2100ColorSpace)
    {
        // TODO: Need consistent rules for using IFRIMG vs. IFT (when are errors exceptional?).
        ComPtr<ID2D1ColorContext1> colorContext1;
        IFT(context->CreateColorContextFromDxgiColorSpace(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &colorContext1));

        IFT(colorContext1.As(&m_colorContext));
    }
    // Both OpenEXR chromaticities or override uses this code path
    else if (m_imageInfo.hasOverriddenColorProfile || m_imageInfo.hasEXRChromaticitiesInfo)
    {
        ComPtr<ID2D1ColorContext1> color1;
        IFT(context->CreateColorContextFromSimpleColorProfile(m_customOrDerivedColorProfile, &color1));
        IFT(color1.As(&m_colorContext));
    }
    else if (m_imageInfo.countColorProfiles >= 1)
    {
        IFT(context->CreateColorContextFromWicColorContext(
            m_wicColorContext.Get(),
            &m_colorContext));
    }
    // If no other info is available, select a default color profile based on pixel format:
    // floating point == scRGB, others == sRGB.
    else
    {
        IFT(context->CreateColorContext(
            m_imageInfo.isFloat ? D2D1_COLOR_SPACE_SCRGB : D2D1_COLOR_SPACE_SRGB,
            nullptr,
            0,
            &m_colorContext));
    }

    m_state = ImageLoaderState::LoadingSucceeded;
}

/// <summary>
/// Gets the Direct2D image representing decoded image data.
/// </summary>
/// <param name="selectAppleHdrGainMap">If true, provides the gainmap aux image instead of the main image.</param>
/// <remarks>Call this every time a new zoom factor is desired
/// If the gainmap is returned, it is pre-scaled to match the resolution of the main image.</remarks>
ID2D1TransformedImageSource* ImageLoader::GetLoadedImage(float zoom, bool selectAppleHdrGainMap)
{
    EnforceStates(1, ImageLoaderState::LoadingSucceeded);

    ID2D1ImageSource* source = m_imageSource.Get();

    if (selectAppleHdrGainMap == true)
    {
        if (m_imageInfo.hasAppleHdrGainMap == false && m_imageInfo.hasIsoHeicHdrGainMap == false && m_imageInfo.hasAppleHeicHdrGainMap == false) return nullptr;
        zoom *= m_imageInfo.pixelSize.Width / m_imageInfo.gainMapPixelSize.Width; // Typically is 2x.
        source = m_hdrGainMapSource.Get();
    }

    if (!source)
    {
        return nullptr;
    }

    // When using ID2D1ImageSource, the recommend method of scaling is to use
    // ID2D1TransformedImageSource. It is inexpensive to recreate this object.
    D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES props =
    {
        D2D1_ORIENTATION_DEFAULT,
        zoom,
        zoom,
        D2D1_INTERPOLATION_MODE_LINEAR, // This is ignored when using DrawImage.
        D2D1_TRANSFORMED_IMAGE_SOURCE_OPTIONS_NONE
    };

    ComPtr<ID2D1TransformedImageSource> output;

    HRESULT hr = m_deviceResources->GetD2DDeviceContext()->CreateTransformedImageSource(
        source,
        &props,
        &output);
    if (FAILED(hr))
    {
        return nullptr;
    }

    return output.Detach();
}

ID2D1Image* ImageLoader::GetLoadedImageSource(bool selectAppleHdrGainMap)
{
    EnforceStates(1, ImageLoaderState::LoadingSucceeded);

    if (selectAppleHdrGainMap == true)
    {
        if (m_imageInfo.hasAppleHdrGainMap == false && m_imageInfo.hasIsoHeicHdrGainMap == false && m_imageInfo.hasAppleHeicHdrGainMap == false)
        {
            return nullptr;
        }

        return m_hdrGainMapSource.Get();
    }

    if (m_heifBitmap)
    {
        return m_heifBitmap.Get();
    }

    return m_imageSource.Get();
}

ID2D1Image* ImageLoader::GetMergedImageSource()
{
    EnforceStates(1, ImageLoaderState::LoadingSucceeded);

    return m_mergedSource.Get();
}

ID2D1TransformedImageSource* ImageLoader::GetMergedImage(float zoom, bool selectAppleHdrGainMap)
{
    EnforceStates(1, ImageLoaderState::LoadingSucceeded);

    //auto context = m_deviceResources->GetD2DDeviceContext();

    //ComPtr<ID2D1ImageSourceFromWic> wicImageSource_2;
    //HRESULT hr = context->CreateImageSourceFromWic(
    //    m_cpuMergedWICBitmapSource.Get(),  // IWICBitmapSource
    //    &wicImageSource_2);
    //ID2D1ImageSource* source = wicImageSource_2.Get();

    ID2D1ImageSource* source = m_mergedSource.Get();

    if (!source)
    {
        return nullptr;
    }

    // When using ID2D1ImageSource, the recommend method of scaling is to use
    // ID2D1TransformedImageSource. It is inexpensive to recreate this object.
    D2D1_TRANSFORMED_IMAGE_SOURCE_PROPERTIES props =
    {
        D2D1_ORIENTATION_DEFAULT,
        zoom,
        zoom,
        D2D1_INTERPOLATION_MODE_LINEAR, // This is ignored when using DrawImage.
        D2D1_TRANSFORMED_IMAGE_SOURCE_OPTIONS_NONE
    };

    ComPtr<ID2D1TransformedImageSource> output;

    HRESULT hr = m_deviceResources->GetD2DDeviceContext()->CreateTransformedImageSource(
        source,
        &props,
        &output);
    if (FAILED(hr))
    {
        return nullptr;
    }

    return output.Detach();
}
/// <summary>
/// Gets the color context of the image.
/// </summary>
/// <returns>Guaranteed to be a valid color context.</returns>
ID2D1ColorContext* ImageLoader::GetImageColorContext()
{
    EnforceStates(1, ImageLoaderState::LoadingSucceeded);

    // Do NOT call GetImageColorContextInternal - it was already called by LoadImageCommon.
    return m_colorContext.Get();
}

/// <summary>
/// Gets ImageInfo.
/// </summary>
ImageInfo ImageLoader::GetImageInfo()
{
    EnforceStates(2, ImageLoaderState::LoadingSucceeded, ImageLoaderState::NeedDeviceResources);

    return m_imageInfo;
}

/// <summary>
/// For testing only. Obtains the cached WIC source.
/// </summary>
IWICBitmapSource* ImageLoader::GetWicSourceTest()
{
    return m_wicCachedSource.Get();
}

/// <summary>
/// Recreates device resources after device lost.
/// </summary>
/// <remarks>
/// ImageLoader doesn't implement IDeviceNotify and relies on the caller to tell it
/// when device resources need to be recreated.
/// Don't call this during normal image load/initialization as this is done automatically.
/// </remarks>
void ImageLoader::CreateDeviceDependentResources()
{
    // Device lost/restored can occur at any time.
    switch (m_state)
    {
    case ImageLoaderState::NotInitialized:
    case ImageLoaderState::LoadingFailed:
        // No-op if there is nothing to be rendered.
        break;

    case ImageLoaderState::NeedDeviceResources:
        CreateDeviceDependentResourcesInternal();
        break;

    case ImageLoaderState::LoadingSucceeded:
    default:
        IFT(WINCODEC_ERR_WRONGSTATE);
        break;
    }
}

/// <summary>
/// Releases (invalid) device resources after device lost.
/// </summary>
/// <remarks>
/// ImageLoader doesn't implement IDeviceNotify and relies on the caller to tell it
/// when device resources need to be recreated.
/// </remarks>
void ImageLoader::ReleaseDeviceDependentResources()
{
    // Device lost/restored can occur at any time.
    switch (m_state)
    {
    case ImageLoaderState::NotInitialized:
    case ImageLoaderState::LoadingFailed:
        // No-op if there is nothing to be rendered.
        break;

    case ImageLoaderState::LoadingSucceeded:
        m_state = ImageLoaderState::NeedDeviceResources;

        m_imageSource.Reset();
        m_heifBitmap.Reset();
        m_colorContext.Reset();
        m_hdrGainMapSource.Reset();
        break;

    case ImageLoaderState::NeedDeviceResources:
    default:
        throw ref new COMException(WINCODEC_ERR_WRONGSTATE);
        break;
    }
}

/// <summary>
/// Determines what advanced color kind the image is.
/// </summary>
/// <param name="info">Requires that pixel format info be populated.</param>
/// <param name="source">For some detection types, IWICBitmapFrameDecode is needed. TODO: Not anymore?</param>
void ImageLoader::PopulateImageInfoACKind(ImageInfo& info, _In_ IWICBitmapSource* source)
{
    UNREFERENCED_PARAMETER(source);

    if (info.bitsPerPixel == 0 ||
        info.bitsPerChannel == 0)
    {
        IFRIMG(WINCODEC_ERR_INVALIDPARAMETER);
    }

    info.imageKind = AdvancedColorKind::StandardDynamicRange;

    // Bit depth > 8bpc or color gamut > sRGB signifies a WCG image.
    // The presence of a color profile is used as an approximation for wide gamut.
    if (info.bitsPerChannel > 8 || info.countColorProfiles >= 1)
    {
        info.imageKind = AdvancedColorKind::WideColorGamut;
    }

    // Currently, all supported floating point images are considered HDR.
    // This includes JPEG XR, OpenEXR, and Radiance RGBE.
    if (info.isFloat == true)
    {
        info.imageKind = AdvancedColorKind::HighDynamicRange;
    }

    // All images using the HDR10/BT.2100 colorspace are HDR. Currently, WIC color contexts cannot
    // represent BT.2100, so all supported BT.2100 images have the force flag set.
    // This includes Xbox One JPEG XR screenshots and HEIF HDR images.
    if (m_imageInfo.forceBT2100ColorSpace == true)
    {
        m_imageInfo.imageKind = AdvancedColorKind::HighDynamicRange;
    }

    if (m_imageInfo.hasAppleHdrGainMap == true || m_imageInfo.hasIsoHeicHdrGainMap == true || m_imageInfo.hasAppleHeicHdrGainMap == true)
    {
        m_imageInfo.imageKind = AdvancedColorKind::HighDynamicRange;
    }
    if (m_imageInfo.hasCuvaHdrGainMap == true)
    {
        m_imageInfo.imageKind = AdvancedColorKind::HighDynamicRange;
    }
}

/// <summary>
/// Fills in the bit depth (channel/pixel) and float fields.
/// </summary>
void ImageLoader::PopulatePixelFormatInfo(ImageInfo& info, WICPixelFormatGUID format)
{
    // This format doesn't support IWICComponentInfo, rely on hardcoded knowledge.
    if (format == GUID_WICPixelFormat32bppR10G10B10A2HDR10)
    {
        info.bitsPerChannel = 10;
        info.bitsPerPixel = 32;
        info.isFloat = false;
    }
    else
    {
        auto wicFactory = m_deviceResources->GetWicImagingFactory();
        ComPtr<IWICComponentInfo> componentInfo;
        IFRIMG(wicFactory->CreateComponentInfo(format, &componentInfo));

        ComPtr<IWICPixelFormatInfo2> pixelFormatInfo;
        IFRIMG(componentInfo.As(&pixelFormatInfo));

        WICPixelFormatNumericRepresentation formatNumber;
        IFRIMG(pixelFormatInfo->GetNumericRepresentation(&formatNumber));

        IFRIMG(pixelFormatInfo->GetBitsPerPixel(&info.bitsPerPixel));

        // Calculate the bits per channel (bit depth) using GetChannelMask.
        // This accounts for nonstandard color channel packing and padding, e.g. 32bppRGB,
        // but assumes each channel has equal bits (e.g. RGB565 doesn't work).
        unsigned char channelMaskBytes[sc_MaxBytesPerPixel];
        ZeroMemory(channelMaskBytes, ARRAYSIZE(channelMaskBytes));
        unsigned int maskSize;

        IFRIMG(pixelFormatInfo->GetChannelMask(
            0,  // Read the first color channel.
            ARRAYSIZE(channelMaskBytes),
            channelMaskBytes,
            &maskSize));

        // Count up the number of bits set in the mask for the first color channel.
        for (unsigned int i = 0; i < maskSize * 8; i++)
        {
            unsigned int byte = i / 8;
            unsigned int bit = i % 8;
            if ((channelMaskBytes[byte] & (1 << bit)) != 0)
            {
                info.bitsPerChannel += 1;
            }
        }

        info.isFloat = (WICPixelFormatNumericRepresentationFloat == formatNumber) ? true : false;

        if (info.hasAppleHdrGainMap || info.hasIsoHeicHdrGainMap || info.hasAppleHeicHdrGainMap) {
            info.bitsPerChannel = 10;
            info.bitsPerPixel = 32;
            info.isFloat = false;
        }
    }
}

/// <summary>
/// Detects if the image is an Xbox console HDR screenshot.
/// </summary>
/// <remarks>
/// Xbox console HDR screenshots use JPEG XR with 10-bit precision and a specially
/// crafted ICC profile and/or EXIF color space to designate that they use BT.2100 PQ.
/// Relies on caller to ensure the container is JPEG XR (IWICBitmapDecoder).
/// </remarks>
bool ImageLoader::IsImageXboxHdrScreenshot(IWICBitmapFrameDecode* frame)
{
    WICPixelFormatGUID fmt = {};
    IFT(frame->GetPixelFormat(&fmt));
    if (fmt != GUID_WICPixelFormat32bppBGR101010)
    {
        return false;
    }

    ComPtr<IWICColorContext> color;
    m_deviceResources->GetWicImagingFactory()->CreateColorContext(&color);

    unsigned int actual = 0;
    IFT(frame->GetColorContexts(1, color.GetAddressOf(), &actual));
    if (actual != 1)
    {
        return false;
    }

    WICColorContextType type = WICColorContextType::WICColorContextUninitialized;
    IFT(color->GetType(&type));

    if (type == WICColorContextType::WICColorContextExifColorSpace)
    {
        unsigned int exif = 0;
        IFT(color->GetExifColorSpace(&exif));
        return (exif == 2084); // This is not a standard EXIF color space.
    }
    else if (type == WICColorContextType::WICColorContextProfile)
    {
        // Compare the profile size and header bytes instead of a full binary or functional check.
        unsigned int profSize = 0;
        IFT(color->GetProfileBytes(0, nullptr, &profSize));
        if (profSize != m_xboxHdrIccSize)
        {
            return false;
        }

        unsigned int ignored = 0;
        std::vector<byte> profBytes;
        profBytes.resize(profSize);
        IFT(color->GetProfileBytes(static_cast<UINT>(profBytes.size()), profBytes.data(), &ignored));

        return (0 == memcmp(m_xboxHdrIccHeaderBytes, profBytes.data(), ARRAYSIZE(m_xboxHdrIccHeaderBytes)));
    }
    else
    {
        return false;
    }
}


/// <summary>
/// Translates DXGI_FORMAT to the best equivalent WIC pixel format.
/// </summary>
/// <remarks>
/// Returns GUID_WICPixelFormatUndefined if we don't know the right WIC pixel format.
/// This list is highly incomplete and only covers the most important DXGI_FORMATs for HDR.
/// </remarks>
GUID ImageLoader::TranslateDxgiFormatToWic(DXGI_FORMAT fmt)
{
    switch (fmt)
    {
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UINT:
        return GUID_WICPixelFormat32bppRGBA;
        break;

    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        // Used by OpenEXR.
        return GUID_WICPixelFormat64bppRGBAHalf;
        break;

    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        // Used by Radiance RGBE; specifically DirectXTex expands out to FP32
        // even though WIC offers a native GUID_WICPixelFormat32bppRGBE.
        return GUID_WICPixelFormat128bppRGBAFloat;
        break;

    default:
        return GUID_WICPixelFormatUndefined;
        break;
    }
}

/// <summary>
/// Some WIC codecs, (HEIF/HEVC, HEIF/AV1, WebP, etc) aren't always present in the OS
/// even though they can be enumerated and created - these are typically loaded from the Store.
/// Attempt to decode a single pixel to ensure the codec is installed.
/// </summary>
/// <returns>
/// Whether the codec was available and decode succeeded.
/// </returns>
bool ImageLoader::CheckCanDecode(_In_ IWICBitmapFrameDecode* frame)
{
    auto fact = m_deviceResources->GetWicImagingFactory();
    ComPtr<IWICBitmap> bitmap;
    
    if (FAILED(fact->CreateBitmapFromSourceRect(frame, 0, 0, 1, 1, &bitmap)))
    {
        return false;
    }
    else
    {
        return true;
    }
}

void ImageLoader::CreateCpuMergedBitmap()
{
    ComPtr<IWICImagingFactory2> wicFactory;
    CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory)
    );

    //ComPtr<ID2D1TransformedImageSource> mainTrans;
    //ComPtr<ID2D1TransformedImageSource> gainTrans;
    //mainTrans = m_imageSource;
    //gainTrans = m_appleHdrGainMap.wicSource;

    auto context = m_deviceResources->GetD2DDeviceContext();

    // 3) Get the IWICBitmapSource from ImageSourceFromWic.
    ComPtr<IWICBitmapSource> mainSrc;
    ComPtr<IWICBitmapSource> gainSrc;
    mainSrc= m_wicCachedSource;
    gainSrc = m_appleHdrGainMap.wicSource;

    UINT width = 0, height = 0;
    mainSrc->GetSize(&width, &height);

    UINT gainWidth = 0, gainHeight = 0;
    gainSrc->GetSize(&gainWidth, &gainHeight);

    if (width == 0 || height == 0 || gainWidth == 0 || gainHeight == 0)
    {
        return;
    }

    const UINT bytesPerPixel_main = 4;
    const UINT bytesPerPixel_gain = 4;
    UINT strideMain = width * bytesPerPixel_main;
    UINT strideGain = gainWidth * bytesPerPixel_gain;
    UINT bufferSizeMain = strideMain * height;
    UINT bufferSizeGain = strideGain * gainHeight;
    std::vector<BYTE> bufferMain(bufferSizeMain);
    std::vector<BYTE> bufferGain(bufferSizeGain);
    WICRect rect = { 0, 0, static_cast<INT>(width), static_cast<INT>(height) };
    WICRect rectGain = { 0, 0, static_cast<INT>(gainWidth), static_cast<INT>(gainHeight) };

    IFT(mainSrc->CopyPixels(
        &rect,
        strideMain,
        bufferSizeMain,
        bufferMain.data()));

    IFT(gainSrc->CopyPixels(
        &rectGain,
        strideGain,
        bufferSizeGain,
        bufferGain.data()));

    ComPtr<IWICBitmap> outBitmap;
    IFT(wicFactory->CreateBitmap(
        width, height,
        // GUID_WICPixelFormat64bppRGBAHalf, // Change to RGBA1010102 format.
        GUID_WICPixelFormat64bppPRGBAHalf,
        WICBitmapCacheOnLoad,
        &outBitmap));

    // 2) Lock the FP16 bitmap for writing.
    ComPtr<IWICBitmapLock> lockOut;
    IFT(outBitmap->Lock(&rect, WICBitmapLockWrite, &lockOut));

    UINT strideOut = 0;
    BYTE* dataOut = nullptr;
    UINT bufferSizeOut = 0;
    IFT(lockOut->GetStride(&strideOut));
    IFT(lockOut->GetDataPointer(&bufferSizeOut, &dataOut));

    const UINT bytesPerPixel = 8;
    const UINT safePixelsPerRow = strideOut / bytesPerPixel;
    const UINT pixelsToWrite = min(width, safePixelsPerRow);

    if (dataOut == nullptr)
    {
        return;
    }

    // Per-pixel compute and write into outBitmap.
    float gainMapMin[3] = { m_gainMapMetadata.gainMapMin[0], m_gainMapMetadata.gainMapMin[1], m_gainMapMetadata.gainMapMin[2] };
    float gainMapMax[3] = { m_gainMapMetadata.gainMapMax[0], m_gainMapMetadata.gainMapMax[1], m_gainMapMetadata.gainMapMax[2] };
    float gainMapGamma[3] = { m_gainMapMetadata.gainMapGamma[0], m_gainMapMetadata.gainMapGamma[1], m_gainMapMetadata.gainMapGamma[2] };
    float offsetSdr[3] = { m_gainMapMetadata.offsetSdr[0], m_gainMapMetadata.offsetSdr[1], m_gainMapMetadata.offsetSdr[2] };
    float offsetHdr[3] = { m_gainMapMetadata.offsetHdr[0], m_gainMapMetadata.offsetHdr[1], m_gainMapMetadata.offsetHdr[2] };
    bool applyGainMap = !m_gainMapMetadata.baseRenditionIsHdr;

    if (!m_gainMapMetadata.hasMetadata &&
        m_imageInfo.hasHdrGainMapHeadroom &&
        m_imageInfo.hdrGainMapHeadroom > 0.0f)
    {
        gainMapMax[0] = m_imageInfo.hdrGainMapHeadroom;
        gainMapMax[1] = m_imageInfo.hdrGainMapHeadroom;
        gainMapMax[2] = m_imageInfo.hdrGainMapHeadroom;
    }

    float gainMapDelta[3] = {};
    for (int c = 0; c < 3; ++c)
    {
        if (gainMapGamma[c] <= 0.0f)
        {
            gainMapGamma[c] = 1.0f;
        }

        if (gainMapMax[c] < gainMapMin[c])
        {
            float temp = gainMapMax[c];
            gainMapMax[c] = gainMapMin[c];
            gainMapMin[c] = temp;
        }

        gainMapDelta[c] = gainMapMax[c] - gainMapMin[c];
    }

    //for (UINT y = 0; y < height; y++)
    //{
    //    if (y * strideOut >= bufferSizeOut)
    //    {
    //        wchar_t errorMsg[256];
    //            y, bufferSizeOut, y * strideOut);
    //        break; // Break to avoid a crash.
    //    }

    //    BYTE* mainRow = bufferMain.data() + y * strideMain;
    //    BYTE* gainRow = bufferGain.data() + y * strideGain;
    //    BYTE* rowStart = dataOut + y * strideOut;

    //    for (UINT x = 0; x < width; x++)
    //    {
    //        // Read main pixels (PBGRA8).
    //        float R_main = mainRow[4 * x + 2] / 255.0f;
    //        float G_main = mainRow[4 * x + 1] / 255.0f;
    //        float B_main = mainRow[4 * x + 0] / 255.0f;

    //        R_main = sRGBToLinear(R_main);
    //        G_main = sRGBToLinear(G_main);
    //        B_main = sRGBToLinear(B_main);

    //        // Read gain map pixels (PBGRA8).
    //        float gainB = gainRow[4 * x + 0] / 128.0f; // [0.0, 2.0]
    //        float gainG = gainRow[4 * x + 1] / 128.0f;
    //        float gainR = gainRow[4 * x + 2] / 128.0f;

    //        //   gainB = sRGBToLinear(gainB);
    //        //   gainG = sRGBToLinear(gainG);
    //        //   gainR = sRGBToLinear(gainR);

    //           // Apply the gain map formula.
    //        R_main = powf(2.0f, gainR) * (R_main + eps) - eps;
    //        G_main = powf(2.0f, gainG) * (G_main + eps) - eps;
    //        B_main = powf(2.0f, gainB) * (B_main + eps) - eps;


    //        BYTE* targetPixel = rowStart + x * bytesPerPixel;
    //        if (targetPixel + bytesPerPixel > dataOut + bufferSizeOut)
    //        {
    //            continue; // Skip pixels beyond the output buffer.
    //        }

    //        uint16_t* pixelData = reinterpret_cast<uint16_t*>(targetPixel);
    //        pixelData[0] = FloatToHalf(R_main); // R
    //        pixelData[1] = FloatToHalf(G_main); // G
    //        pixelData[2] = FloatToHalf(B_main); // B
    //        pixelData[3] = FloatToHalf(1.0f);   // A (opaque)

    //    }
    //}

    initLUT();

    const UINT gainWidthMinus1 = gainWidth > 0 ? gainWidth - 1 : 0;
    const UINT gainHeightMinus1 = gainHeight > 0 ? gainHeight - 1 : 0;



    // Enable OpenMP parallelism.
    #pragma omp parallel for
    for (int y = 0; y < static_cast<int>(height); y++)
    {
        BYTE* mainRow = bufferMain.data() + y * strideMain;
        BYTE* rowStart = dataOut + y * strideOut;
        UINT gainY = static_cast<UINT>((static_cast<uint64_t>(y) * gainHeight) / height);
        if (gainY > gainHeightMinus1)
        {
            gainY = gainHeightMinus1;
        }
        BYTE* gainRow = bufferGain.data() + gainY * strideGain;

        for (UINT x = 0; x < width; x++)
        {
            // Read main pixels (PBGRA8) and convert to linear space.
            float R_main = mainRow[4 * x + 2] / 255.0f;
            float G_main = mainRow[4 * x + 1] / 255.0f;
            float B_main = mainRow[4 * x + 0] / 255.0f;

            // Convert using sRGBToLinear.
            // R_main = sRGBToLinear(R_main);
            // G_main = sRGBToLinear(G_main);
            // B_main = sRGBToLinear(B_main);

            R_main = lut_sRGBToLinear(R_main);
            G_main = lut_sRGBToLinear(G_main);
            B_main = lut_sRGBToLinear(B_main);

            // Read gain map pixels (PBGRA8).
            UINT gainX = static_cast<UINT>((static_cast<uint64_t>(x) * gainWidth) / width);
            if (gainX > gainWidthMinus1)
            {
                gainX = gainWidthMinus1;
            }

            BYTE* gainPixel = gainRow + 4 * gainX;
            float gainB = gainPixel[0] / 255.0f; // [0.0, 1.0]
            float gainG = gainPixel[1] / 255.0f;
            float gainR = gainPixel[2] / 255.0f;


            /* gainB = lut_sRGBToLinear(gainB);
             gainG = lut_sRGBToLinear(gainG);
             gainR = lut_sRGBToLinear(gainR);*/


             // Apply the gain map formula.
            if (applyGainMap)
            {
                float gainRValue = gainR;
                float gainGValue = gainG;
                float gainBValue = gainB;

                if (gainRValue < 0.0f) gainRValue = 0.0f;
                if (gainRValue > 1.0f) gainRValue = 1.0f;
                if (gainGValue < 0.0f) gainGValue = 0.0f;
                if (gainGValue > 1.0f) gainGValue = 1.0f;
                if (gainBValue < 0.0f) gainBValue = 0.0f;
                if (gainBValue > 1.0f) gainBValue = 1.0f;

                float gainRLog = gainMapMin[0] + gainMapDelta[0] * powf(gainRValue, gainMapGamma[0]);
                float gainGLog = gainMapMin[1] + gainMapDelta[1] * powf(gainGValue, gainMapGamma[1]);
                float gainBLog = gainMapMin[2] + gainMapDelta[2] * powf(gainBValue, gainMapGamma[2]);

                float gainRFactor = powf(2.0f, gainRLog);
                float gainGFactor = powf(2.0f, gainGLog);
                float gainBFactor = powf(2.0f, gainBLog);

                R_main = (R_main + offsetSdr[0]) * gainRFactor - offsetHdr[0];
                G_main = (G_main + offsetSdr[1]) * gainGFactor - offsetHdr[1];
                B_main = (B_main + offsetSdr[2]) * gainBFactor - offsetHdr[2];
            }

            // Write output pixel (RGBA half).
            BYTE* targetPixel = rowStart + x * bytesPerPixel;
            uint16_t* pixelData = reinterpret_cast<uint16_t*>(targetPixel);

            // Convert to half precision.
            pixelData[0] = FloatToHalf(R_main); // R
            pixelData[1] = FloatToHalf(G_main); // G
            pixelData[2] = FloatToHalf(B_main); // B
            pixelData[3] = 0x3C00;              // A = 1.0 (half precision)
        }
    }
    lockOut.Reset();
    m_cpuMergedWICBitmapSource = outBitmap;

    ComPtr<ID2D1ImageSourceFromWic> ID2D1ImageSource_merged;

    //    m_cpuMergedWICBitmapSource.Get(),  // IWICBitmapSource

    HRESULT hr = context->CreateImageSourceFromWic(m_cpuMergedWICBitmapSource.Get(), &ID2D1ImageSource_merged);
    if (FAILED(hr)) {
    }

    IFRIMG(ID2D1ImageSource_merged.As(&m_mergedSource));

}

uint16_t ImageLoader::FloatToHalf(float value)
{
    // Simple implementation - use an optimized version in production.
    // uint32_t f = *reinterpret_cast<uint32_t*>(&value);
    // uint32_t sign = (f >> 16) & 0x8000;
    // int32_t exp = (f >> 23) & 0xff;
    // uint32_t mant = f & 0x7fffff;

    // if (exp == 0xff) { // NaN/Inf
    //     return sign | 0x7c00 | (mant ? 0x200 | (mant >> 13) : 0);
    // }

    // exp -= 127;
    // if (exp > 15) {
    //     return sign | 0x7c00; // Overflow -> Inf
    // }
    // if (exp < -14) {
    //     return sign; // Underflow -> 0
    // }

    // uint32_t uexp = static_cast<uint32_t>(exp + 15);
    // uint32_t hmant = mant >> 13;
    // if ((mant & 0x1000) != 0) { // Round to nearest
    //     hmant += 1;
    //     if (hmant & 0x0400) {
    //         hmant = 0;
    //         uexp += 1;
    //     }
    // }

    // return static_cast<uint16_t>(sign | (uexp << 10) | hmant);

    __m128 vec = _mm_set_ss(value);
    __m128i half = _mm_cvtps_ph(vec, _MM_FROUND_TO_NEAREST_INT);
    return static_cast<uint16_t>(_mm_extract_epi16(half, 0));
}

float ImageLoader::sRGBToLinear(float color)
{
    if (color <= 0.04045f) {
        return color / 12.92f;
    }
    else {
        return std::pow((color + 0.055f) / 1.055f, 2.4f);
    }
}

void ImageLoader::initLUT() {
    if (!lutInitialized) {
        for (int i = 0; i < 256; i++) {
            float c = i / 255.0f;
            sRGBToLinearLUT[i] = (c <= 0.04045f) ? 
                c / 12.92f : 
                powf((c + 0.055f) / 1.055f, 2.4f);
        }
        lutInitialized = true;
    }
}

float ImageLoader::lut_sRGBToLinear(float c) {
    int index = static_cast<int>(c * 255.0f + 0.5f);
    index = max(0, min(255, index));
    return sRGBToLinearLUT[index];
}

void ImageLoader::generateEncodeSDRimage() {
    // Swap thumbnail and main image when exporting ISO HDR.
    changedImage.clear();
    changedImage = jpegData;
    if (m_imageInfo.hasCuvaHdrGainMap || m_imageInfo.hasHuaweiIsoJpegHdrGainMap)
    {
        std::memcpy(changedImage.data() + thumbnailStart, jpegData.data() + mainImageStart, mainImageEnd - mainImageStart + 1);
        std::memcpy(changedImage.data() + thumbnailStart + mainImageEnd - mainImageStart + 1, jpegData.data() + thumbnailStart, thumbnailEnd - thumbnailStart + 1);
    }

    if (exif_result.has_exif)
    {
        exif_result.exif_ptr = &changedImage[exif_result.exif_pos];
    }

    for (int i = 0; i < changedImage.size(); i++) {
        if (changedImage[i] == 0x5F && changedImage[i + 1] == 0x63 &&
            changedImage[i + 2] == 0x75 && changedImage[i + 3] == 0x76 && changedImage[i + 4] == 0x61)
        {
            changedImage[i] = 0x00;
            changedImage[i + 1] = 0x00;
            changedImage[i + 2] = 0x00;
            changedImage[i + 3] = 0x00;
            changedImage[i + 4] = 0x00;
        }
    }

    std::memcpy(sdrData_changed.data(), changedImage.data() + firstStart, sdrSize);
}
