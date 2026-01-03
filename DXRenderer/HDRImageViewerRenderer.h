//*********************************************************
// HDRImageViewerRenderer
//
// Main manager of all native DirectX image rendering resources.
//
//*********************************************************

#pragma once

#include "Common\DeviceResources.h"
#include "RenderEffects\SdrOverlayEffect.h"
#include "RenderEffects\LuminanceHeatmapEffect.h"
#include "RenderEffects\SphereMapEffect.h"
#include "RenderEffects\MaxLuminanceEffect.h"
#include "RenderOptions.h"
#include "ImageLoader.h"
#include "Matrix.h"
#include <DirectXMath.h> // Used for XMConvertFloatToHalf.
#include <d2d1_1.h>

#include <string>
#include <vector> // Used for std::vector.

namespace DXRenderer
{
    [Windows::Foundation::Metadata::WebHostHidden]
    public ref class HDRImageViewerRenderer sealed : public DXRenderer::IDeviceNotify
    {
    public:
        HDRImageViewerRenderer(Windows::UI::Xaml::Controls::SwapChainPanel^ panel);

        // DeviceResources wrapper methods for Windows Runtime Component
        void SetSwapChainPanel(Windows::UI::Xaml::Controls::SwapChainPanel^ panel) { m_deviceResources->SetSwapChainPanel(panel); }
        void SetLogicalSize(Windows::Foundation::Size logicalSize)                 { m_deviceResources->SetLogicalSize(logicalSize); }
        void SetCurrentOrientation(Windows::Graphics::Display::DisplayOrientations currentOrientation)
                                                                                   { m_deviceResources->SetCurrentOrientation(currentOrientation); }
        void SetDpi(float dpi)                                                     { m_deviceResources->SetDpi(dpi); }
        void SetCompositionScale(float compositionScaleX, float compositionScaleY) { m_deviceResources->SetCompositionScale(compositionScaleX, compositionScaleY); }
        void ValidateDevice()                                                      { m_deviceResources->ValidateDevice(); }
        void HandleDeviceLost()                                                    { m_deviceResources->HandleDeviceLost(); }
        void Trim()                                                                { m_deviceResources->Trim(); }
        void Present()                                                             { m_deviceResources->Present(); }

        void CreateDeviceIndependentResources();
        void CreateDeviceDependentResources();
        void CreateWindowSizeDependentResources();
        void ReleaseDeviceDependentResources();

        void Draw();

        void CreateImageDependentResources();
        void ReleaseImageDependentResources();
        void SetTargetCpuReadbackSupport(bool value);
        Windows::Foundation::Numerics::float4 GetPixelColorValue(Windows::Foundation::Point point);

        void UpdateManipulationState(_In_ Windows::UI::Input::ManipulationUpdatedEventArgs^ args);

        // Returns the computed MaxCLL and AvgCLL of the image in nits. While HDR metadata is a
        // property of the image (and is independent of rendering), our implementation
        // can't compute it until this point.
        ImageCLL FitImageToWindow(bool computeMetadata);

        void SetRenderOptions(
            RenderEffectKind effect,
            float exposureAdjustment,
            float dispMaxCllOverride,
            Windows::Graphics::Display::AdvancedColorInfo^ acInfo,
            bool constrainGamut
            );

        ImageInfo LoadImageFromWic(_In_ Windows::Storage::Streams::IRandomAccessStream^ imageStream, ImageLoaderOptions options);
        ImageInfo LoadImageFromDirectXTex(_In_ Platform::String^ filename, _In_ Platform::String^ extension, ImageLoaderOptions options);
        void      ExportImageToSdr(_In_ Windows::Storage::Streams::IRandomAccessStream^ outputStream, Platform::Guid wicFormat);
        void      ExportAsDdsTest(_In_ Windows::Storage::Streams::IRandomAccessStream^ outputStream);
        void      ExportImageToJxr(_In_ Windows::Storage::Streams::IRandomAccessStream^ outputStream);
        void      ExportImageToISOJpeg(_In_ Windows::Storage::Streams::IRandomAccessStream^ outputStream);

        void      startEncodeISOJpeg();

        // IDeviceNotify methods handle device lost and restored.
        virtual void OnDeviceLost();
        virtual void OnDeviceRestored();

    private:
        inline static float Clamp(float v, float bound1, float bound2)
        {
            float low = min(bound1, bound2);
            float high = max(bound1, bound2);
            return (v < low) ? low : (v > high) ? high : v;
        }

        ~HDRImageViewerRenderer();

        void CreateHistogramResources();
        void UpdateWhiteLevelScale(float brightnessAdjustment, float sdrWhiteLevel);
        void UpdateImageTransformState();
        void ComputeHdrMetadata();
        void EmitHdrMetadata();
        void UpdateGamutTransforms();

        float GetBestDispMaxLuminance();

        // Cached pointer to device resources.
        std::shared_ptr<DeviceResources>                        m_deviceResources;
        std::unique_ptr<ImageLoader>                            m_imageLoader;

        // WIC and Direct2D resources.
        Microsoft::WRL::ComPtr<ID2D1Image>                      m_loadedImage;
        Microsoft::WRL::ComPtr<ID2D1Image>                      m_loadedGainMap;
        Microsoft::WRL::ComPtr<ID2D1Image>                      m_loadedMergedImage;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_gainmapLinearEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_imageScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_gainmapRefWhiteEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_gainMapMergeEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_colorManagementEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_whiteScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_sdrWhiteScaleEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_hdrTonemapEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_sdrOverlayEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_heatmapEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_maxLuminanceEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_sphereMapEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_histogramPrescale;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_histogramEffect;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_mapGamutToPanel;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_mapGamutToScRGB;
        Microsoft::WRL::ComPtr<ID2D1Effect>                     m_finalOutput;

        std::vector<float>                                      m_renderTargetCpuPixels;

        // Other renderer members.
        RenderEffectKind                                        m_renderEffectKind;
        float                                                   m_zoom;
        float                                                   m_minZoom;
        D2D1_POINT_2F                                           m_imageOffset;
        D2D1_POINT_2F                                           m_pointerPos;
        ImageCLL                                                m_imageCLL;
        float                                                   m_exposureAdjust;
        Windows::Graphics::Display::AdvancedColorInfo^          m_dispInfo;
        ImageInfo                                               m_imageInfo;
        bool                                                    m_isComputeSupported;
        float                                                   m_dispMaxCLLOverride;
        bool                                                    m_enableTargetCpuReadback;

        bool                                                    m_constrainGamut;

        std::vector<BYTE>                                       outputImage;
    };
}

namespace jpegR {

    std::string Name(const std::string& prefix, const std::string& suffix) {
        std::stringstream ss;
        ss << prefix << ":" << suffix;
        return ss.str();
    }

    typedef enum uhdr_codec_err {

        /*!\brief Operation completed without error */
        UHDR_CODEC_OK,

        /*!\brief Generic codec error, refer detail field for more information */
        UHDR_CODEC_ERROR,

        /*!\brief Unknown error, refer detail field for more information */
        UHDR_CODEC_UNKNOWN_ERROR,

        /*!\brief An application-supplied parameter is not valid. */
        UHDR_CODEC_INVALID_PARAM,

        /*!\brief Memory operation failed */
        UHDR_CODEC_MEM_ERROR,

        /*!\brief An application-invoked operation is not valid */
        UHDR_CODEC_INVALID_OPERATION,

        /*!\brief The library does not implement a feature required for the operation */
        UHDR_CODEC_UNSUPPORTED_FEATURE,

        /*!\brief Not for usage, indicates end of list */
        UHDR_CODEC_LIST_END,

    } uhdr_codec_err_t; /**< alias for enum uhdr_codec_err */

    std::vector<BYTE> output;

    typedef enum uhdr_color_gamut {
        UHDR_CG_UNSPECIFIED = -1, /**< Unspecified */
        UHDR_CG_BT_709 = 0,       /**< BT.709 */
        UHDR_CG_DISPLAY_P3 = 1,   /**< Display P3 */
        UHDR_CG_BT_2100 = 2,      /**< BT.2100 */
    } uhdr_color_gamut_t;       /**< alias for enum uhdr_color_gamut */

    typedef enum uhdr_color_transfer {
        UHDR_CT_UNSPECIFIED = -1, /**< Unspecified */
        UHDR_CT_LINEAR = 0,       /**< Linear */
        UHDR_CT_HLG = 1,          /**< Hybrid log gamma */
        UHDR_CT_PQ = 2,           /**< Perceptual Quantizer */
        UHDR_CT_SRGB = 3,         /**< Gamma */
    } uhdr_color_transfer_t;    /**< alias for enum uhdr_color_transfer */

    typedef enum uhdr_color_range {
        UHDR_CR_UNSPECIFIED = -1,  /**< Unspecified */
        UHDR_CR_LIMITED_RANGE = 0, /**< Y {[16..235], UV [16..240]} * pow(2, (bpc - 8)) */
        UHDR_CR_FULL_RANGE = 1,    /**< YUV/RGB {[0..255]} * pow(2, (bpc - 8)) */
    } uhdr_color_range_t;        /**< alias for enum uhdr_color_range */

    typedef struct uhdr_compressed_image {
        void* data;               /**< Pointer to a block of data to decode */
        size_t data_sz;           /**< size of the data buffer */
        size_t capacity;          /**< maximum size of the data buffer */
        uhdr_color_gamut_t cg;    /**< Color Gamut */
        uhdr_color_transfer_t ct; /**< Color Transfer */
        uhdr_color_range_t range; /**< Color Range */
    } uhdr_compressed_image_t;  /**< alias for struct uhdr_compressed_image */

    typedef struct uhdr_mem_block {
        void* data;       /**< Pointer to a block of data to decode */
        size_t data_sz;   /**< size of the data buffer */
        size_t capacity;  /**< maximum size of the data buffer */
    } uhdr_mem_block_t; /**< alias for struct uhdr_mem_block */

    typedef struct uhdr_gainmap_metadata {
        float max_content_boost[3]; /**< Value to control how much brighter an image can get, when shown
                                    on an HDR display, relative to the SDR rendition. This is constant for
                                    a given image. Value MUST be in linear scale. */
        float min_content_boost[3]; /**< Value to control how much darker an image can get, when shown on
                                    an HDR display, relative to the SDR rendition. This is constant for a
                                    given image. Value MUST be in linear scale. */
        float gamma[3];             /**< Encoding Gamma of the gainmap image. */
        float offset_sdr[3];    /**< The offset to apply to the SDR pixel values during gainmap generation
                                and application. */
        float offset_hdr[3];    /**< The offset to apply to the HDR pixel values during gainmap generation
                                and application. */
        float hdr_capacity_min; /**< Minimum display boost value for which the map is applied completely.
                                   Value MUST be in linear scale. */
        float hdr_capacity_max; /**< Maximum display boost value for which the map is applied completely.
                                   Value MUST be in linear scale. */
        int use_base_cg;         /**< Is gainmap application space same as base image color space */
    } uhdr_gainmap_metadata_t; /**< alias for struct uhdr_gainmap_metadata */

    typedef struct uhdr_gainmap_metadata_ext : uhdr_gainmap_metadata {
        uhdr_gainmap_metadata_ext() {}

        uhdr_gainmap_metadata_ext(std::string ver) : version(ver) {}

        uhdr_gainmap_metadata_ext(uhdr_gainmap_metadata& metadata, std::string ver)
            : uhdr_gainmap_metadata_ext(ver) {
            std::copy(metadata.max_content_boost, metadata.max_content_boost + 3, max_content_boost);
            std::copy(metadata.min_content_boost, metadata.min_content_boost + 3, min_content_boost);
            std::copy(metadata.gamma, metadata.gamma + 3, gamma);
            std::copy(metadata.offset_sdr, metadata.offset_sdr + 3, offset_sdr);
            std::copy(metadata.offset_hdr, metadata.offset_hdr + 3, offset_hdr);
            hdr_capacity_min = metadata.hdr_capacity_min;
            hdr_capacity_max = metadata.hdr_capacity_max;
            use_base_cg = metadata.use_base_cg;
        }

        bool are_all_channels_identical() const {
            return max_content_boost[0] == max_content_boost[1] &&
                max_content_boost[0] == max_content_boost[2] &&
                min_content_boost[0] == min_content_boost[1] &&
                min_content_boost[0] == min_content_boost[2] && gamma[0] == gamma[1] &&
                gamma[0] == gamma[2] && offset_sdr[0] == offset_sdr[1] &&
                offset_sdr[0] == offset_sdr[2] && offset_hdr[0] == offset_hdr[1] &&
                offset_hdr[0] == offset_hdr[2];
        }

        std::string version;         /**< Ultra HDR format version */
    } uhdr_gainmap_metadata_ext_t; /**< alias for struct uhdr_gainmap_metadata */

    const bool kWriteXmpMetadata = true;
    const bool kWriteIso21496_1Metadata = true;

    const std::string kXmpNameSpace = "http://ns.adobe.com/xap/1.0/";
    const std::string kIsoNameSpace = "urn:iso:std:iso:ts:21496:-1";

    const std::string kContainerPrefix = "Container";
    const std::string kConItem = Name(kContainerPrefix, "Item");
    const std::string kContainerUri = "http://ns.google.com/photos/1.0/container/";

    const std::string kConDirectory = Name(kContainerPrefix, "Directory");
    const std::string kGainMapUri = "http://ns.adobe.com/hdr-gain-map/1.0/";
    const std::string kGainMapPrefix = "hdrgm";

    // GainMap XMP constants - element and attribute names
    const std::string kMapVersion = Name(kGainMapPrefix, "Version");
    const std::string kMapGainMapMin = Name(kGainMapPrefix, "GainMapMin");
    const std::string kMapGainMapMax = Name(kGainMapPrefix, "GainMapMax");
    const std::string kMapGamma = Name(kGainMapPrefix, "Gamma");
    const std::string kMapOffsetSdr = Name(kGainMapPrefix, "OffsetSDR");
    const std::string kMapOffsetHdr = Name(kGainMapPrefix, "OffsetHDR");
    const std::string kMapHDRCapacityMin = Name(kGainMapPrefix, "HDRCapacityMin");
    const std::string kMapHDRCapacityMax = Name(kGainMapPrefix, "HDRCapacityMax");
    const std::string kMapBaseRenditionIsHDR = Name(kGainMapPrefix, "BaseRenditionIsHDR");

    typedef struct uhdr_error_info {
        uhdr_codec_err_t error_code; /**< error code */
        int has_detail;              /**< has detailed error logs. 0 - no, else - yes */
        char detail[256];            /**< error logs */
    } uhdr_error_info_t;           /**< alias for struct uhdr_error_info */

    typedef enum {
        ULTRAHDR_COLORGAMUT_UNSPECIFIED = -1,
        ULTRAHDR_COLORGAMUT_BT709,
        ULTRAHDR_COLORGAMUT_P3,
        ULTRAHDR_COLORGAMUT_BT2100,
        ULTRAHDR_COLORGAMUT_MAX = ULTRAHDR_COLORGAMUT_BT2100,
    } ultrahdr_color_gamut;


    struct jpegr_compressed_struct {
        // Pointer to the data location.
        void* data;
        // Used data length in bytes.
        size_t length;
        // Maximum available data length in bytes.
        size_t maxLength;
        // Color gamut.
        ultrahdr_color_gamut colorGamut;
    };


    struct uhdr_gainmap_metadata_frac {
        int32_t gainMapMinN[3];
        uint32_t gainMapMinD[3];
        int32_t gainMapMaxN[3];
        uint32_t gainMapMaxD[3];
        uint32_t gainMapGammaN[3];
        uint32_t gainMapGammaD[3];

        int32_t baseOffsetN[3];
        uint32_t baseOffsetD[3];
        int32_t alternateOffsetN[3];
        uint32_t alternateOffsetD[3];

        uint32_t baseHdrHeadroomN;
        uint32_t baseHdrHeadroomD;
        uint32_t alternateHdrHeadroomN;
        uint32_t alternateHdrHeadroomD;

        bool backwardDirection;
        bool useBaseColorSpace;

        bool allChannelsIdentical() const {
            return gainMapMinN[0] == gainMapMinN[1] && gainMapMinN[0] == gainMapMinN[2] &&
                gainMapMinD[0] == gainMapMinD[1] && gainMapMinD[0] == gainMapMinD[2] &&
                gainMapMaxN[0] == gainMapMaxN[1] && gainMapMaxN[0] == gainMapMaxN[2] &&
                gainMapMaxD[0] == gainMapMaxD[1] && gainMapMaxD[0] == gainMapMaxD[2] &&
                gainMapGammaN[0] == gainMapGammaN[1] && gainMapGammaN[0] == gainMapGammaN[2] &&
                gainMapGammaD[0] == gainMapGammaD[1] && gainMapGammaD[0] == gainMapGammaD[2] &&
                baseOffsetN[0] == baseOffsetN[1] && baseOffsetN[0] == baseOffsetN[2] &&
                baseOffsetD[0] == baseOffsetD[1] && baseOffsetD[0] == baseOffsetD[2] &&
                alternateOffsetN[0] == alternateOffsetN[1] && alternateOffsetN[0] == alternateOffsetN[2] &&
                alternateOffsetD[0] == alternateOffsetD[1] && alternateOffsetD[0] == alternateOffsetD[2];
        }
    };

    std::string generateXmpForSecondaryImage(uhdr_gainmap_metadata_ext_t& metadata);
    //static void copyJpegWithoutExif(uhdr_compressed_image_t* pDest, uhdr_compressed_image_t* pSource,
    //    size_t exif_pos, size_t exif_size);

    struct ParseResult {
        BYTE* exif_ptr = nullptr;
        size_t exif_size = -1;
        uhdr_compressed_image_t new_jpg_image;
        bool has_exif = false;
        size_t exif_pos = -1;
    };

    ParseResult parse_result;



    //bool parse_image(uhdr_compressed_image_t* source_image, uhdr_compressed_image_t* new_jpg_image);
    bool parse_image(uhdr_compressed_image_t* source, ParseResult* result);
    void* exif_ptr = nullptr;
    size_t exif_size = 0;

    uhdr_error_info_t appendGainMap(uhdr_compressed_image_t* sdr_intent_compressed,
        uhdr_compressed_image_t* gainmap_compressed,
        uhdr_mem_block_t* pExif, void* pIcc, size_t icc_size,
        uhdr_gainmap_metadata_ext_t* metadata,
        uhdr_compressed_image_t* dest);

    void encodeISOJpeg(
        int width,
        int height,
        std::vector<BYTE> sdrImage,   // Replaces BYTE* sdrData and int sdrSize.
        std::vector<BYTE> gainmapImage, // Replaces BYTE* gainmapData and int gainmapSize.
        float maxContentBoost,
        float minContentBoost,
        float gamma,
        float offsetSdr,
        float offsetHdr,
        float hdrCapacityMin,
        float hdrCapacityMax);


#define UHDR_ERR_CHECK(x)                               \
            {                                                   \
                uhdr_error_info_t status = (x);                 \
                if (status.error_code != UHDR_CODEC_OK) {       \
                    return status;                              \
                }                                               \
            }

    static uhdr_error_info_t gainmapMetadataFloatToFraction(const uhdr_gainmap_metadata_ext_t* from,
        uhdr_gainmap_metadata_frac* to);

    bool floatToSignedFraction(float v, int32_t* numerator, uint32_t* denominator);

    static bool floatToUnsignedFractionImpl(float v, uint32_t maxNumerator, uint32_t* numerator,
        uint32_t* denominator);

    bool floatToUnsignedFraction(float v, uint32_t* numerator, uint32_t* denominator);

    static bool floatToUnsignedFractionImpl(float v, uint32_t maxNumerator, uint32_t* numerator,
        uint32_t* denominator);

    static const uhdr_error_info_t g_no_error = { UHDR_CODEC_OK, 0, "" };

    uhdr_error_info_t encodeGainmapMetadata(
        const uhdr_gainmap_metadata_frac* in_metadata, std::vector<uint8_t>& out_data);

    void streamWriteU16(std::vector<uint8_t>& data, uint16_t value) {
        data.push_back((value >> 8) & 0xff);
        data.push_back(value & 0xff);
    }

    constexpr uint8_t kIsMultiChannelMask = (1u << 7);
    constexpr uint8_t kUseBaseColorSpaceMask = (1u << 6);

    void streamWriteU8(std::vector<uint8_t>& data, uint8_t value) { data.push_back(value); }
    void streamWriteU32(std::vector<uint8_t>& data, uint32_t value) {
        data.push_back((value >> 24) & 0xff);
        data.push_back((value >> 16) & 0xff);
        data.push_back((value >> 8) & 0xff);
        data.push_back(value & 0xff);
    }
    void streamWriteS32(std::vector<uint8_t>& data, int32_t value) {
        data.push_back((value >> 24) & 0xff);
        data.push_back((value >> 16) & 0xff);
        data.push_back((value >> 8) & 0xff);
        data.push_back(value & 0xff);
    }

    typedef enum {
        PARSE_STREAM = (1 << 0),   /**< Parse jpeg header, APPn markers (Exif, Icc, Xmp, Iso) */
        DECODE_STREAM = (1 << 16), /**< Single channel images are decoded to Grayscale format and multi
                                      channel images are decoded to RGB format */
        DECODE_TO_YCBCR_CS = (1 << 17), /**< Decode image to YCbCr Color Space  */
        DECODE_TO_RGB_CS = (1 << 18),   /**< Decode image to RGB Color Space  */
    } decode_mode_t;

    typedef unsigned char JOCTET;
#define GETJOCTET(value)  (value)

    std::vector<JOCTET> mResultBuffer;       // buffer to store decoded data
    std::vector<JOCTET> mXMPBuffer;          // buffer to store xmp data
    std::vector<JOCTET> mEXIFBuffer;         // buffer to store exif data
    std::vector<JOCTET> mICCBuffer;          // buffer to store icc data
    std::vector<JOCTET> mIsoMetadataBuffer;  // buffer to store iso data

    typedef enum uhdr_img_fmt {
        UHDR_IMG_FMT_UNSPECIFIED = -1,   /**< Unspecified */
        UHDR_IMG_FMT_24bppYCbCrP010 = 0, /**< 10-bit-per component 4:2:0 YCbCr semiplanar format.
                                     Each chroma and luma component has 16 allocated bits in
                                     little-endian configuration with 10 MSB of actual data.*/
        UHDR_IMG_FMT_12bppYCbCr420 = 1,  /**< 8-bit-per component 4:2:0 YCbCr planar format */
        UHDR_IMG_FMT_8bppYCbCr400 = 2,   /**< 8-bit-per component Monochrome format */
        UHDR_IMG_FMT_32bppRGBA8888 =
        3, /**< 32 bits per pixel RGBA color format, with 8-bit red, green, blue
          and alpha components. Using 32-bit little-endian representation,
          colors stored as Red 7:0, Green 15:8, Blue 23:16, Alpha 31:24. */
        UHDR_IMG_FMT_64bppRGBAHalfFloat =
        4, /**< 64 bits per pixel, 16 bits per channel, half-precision floating point RGBA color
              format. colors stored as Red 15:0, Green 31:16, Blue 47:32, Alpha 63:48. In a pixel
              even though each channel has storage space of 16 bits, the nominal range is expected to
              be [0.0..(10000/203)] */
        UHDR_IMG_FMT_32bppRGBA1010102 = 5, /**< 32 bits per pixel RGBA color format, with 10-bit red,
                                          green,   blue, and 2-bit alpha components. Using 32-bit
                                          little-endian   representation, colors stored as Red 9:0, Green
                                          19:10, Blue   29:20, and Alpha 31:30. */
        UHDR_IMG_FMT_24bppYCbCr444 = 6,    /**< 8-bit-per component 4:4:4 YCbCr planar format */
        UHDR_IMG_FMT_16bppYCbCr422 = 7,    /**< 8-bit-per component 4:2:2 YCbCr planar format */
        UHDR_IMG_FMT_16bppYCbCr440 = 8,    /**< 8-bit-per component 4:4:0 YCbCr planar format */
        UHDR_IMG_FMT_12bppYCbCr411 = 9,    /**< 8-bit-per component 4:1:1 YCbCr planar format */
        UHDR_IMG_FMT_10bppYCbCr410 = 10,   /**< 8-bit-per component 4:1:0 YCbCr planar format */
        UHDR_IMG_FMT_24bppRGB888 = 11,     /**< 8-bit-per component RGB interleaved format */
        UHDR_IMG_FMT_30bppYCbCr444 = 12,   /**< 10-bit-per component 4:4:4 YCbCr planar format */
    } uhdr_img_fmt_t;                    /**< alias for enum uhdr_img_fmt */

    // temporary storage
    static constexpr int kMaxNumComponents = 3;
    std::unique_ptr<uint8_t[]> mPlanesMCURow[kMaxNumComponents];

    uhdr_img_fmt_t mOutFormat;
    unsigned int mNumComponents;
    unsigned int mPlaneWidth[kMaxNumComponents];
    unsigned int mPlaneHeight[kMaxNumComponents];
    unsigned int mPlaneHStride[kMaxNumComponents];
    unsigned int mPlaneVStride[kMaxNumComponents];

    long mExifPayLoadOffset;  // Position of EXIF package, default value is -1 which means no EXIF
    // package appears.


#define jpeg_common_fields \
            struct jpeg_error_mgr *err;   /* Error handler module */ \
            struct jpeg_memory_mgr *mem;  /* Memory manager module */ \
            struct jpeg_progress_mgr *progress; /* Progress monitor, or NULL if none */ \
            void *client_data;            /* Available for use by application */ \
            boolean is_decompressor;      /* So common code can tell which is which */ \
            int global_state              /* For checking call sequence validity */

    struct jpeg_common_struct {
        jpeg_common_fields;           /* Fields common to both master struct types */
        /* Additional fields follow in an actual jpeg_compress_struct or
         * jpeg_decompress_struct.  All three structs must agree on these
         * initial fields!  (This would be a lot cleaner in C++.)
         */
    };


    typedef struct jpeg_common_struct* j_common_ptr;
    typedef struct jpeg_compress_struct* j_compress_ptr;
    typedef struct jpeg_decompress_struct* j_decompress_ptr;


#define UHDR_ERR_CHECK(x)                     \
        {                                             \
            uhdr_error_info_t status = (x);           \
            if (status.error_code != UHDR_CODEC_OK) { \
            return status;                            \
            }                                         \
        }

    static const size_t kLength = 2;

    /// The offset from the start of the JpegMarker that contains the marker type.
    static const size_t kTypeOffset = 1;

    /// The special byte value that may start a marker.
    static const BYTE kStart = 0xFF;

    static const BYTE kZERO = 0;
    static const BYTE kSOS = 0xDA;
    static const BYTE kSOI = 0xD8;
    static const BYTE kEOI = 0xD9;
    static const BYTE kAPP0 = 0xE0;
    static const BYTE kAPP1 = 0xE1;
    static const BYTE kAPP2 = 0xE2;
    static const BYTE kFILL = 0xFF;

    uhdr_error_info_t Write(uhdr_compressed_image_t* destination, const void* source, size_t length,
        size_t& position);

    std::string generateXmpForPrimaryImage(size_t secondary_image_length,
        uhdr_gainmap_metadata_ext_t& metadata);

    const std::string containerName = "rdf:Description";
    // Item XMP constants - URI and namespace prefix
    const std::string kItemUri = "http://ns.google.com/photos/1.0/container/item/";
    const std::string kItemPrefix = "Item";

    // Item XMP constants - element and attribute names
    const std::string kItemLength = Name(kItemPrefix, "Length");
    const std::string kItemMime = Name(kItemPrefix, "Mime");
    const std::string kItemSemantic = Name(kItemPrefix, "Semantic");

    // Item XMP constants - element and attribute values
    const std::string kSemanticPrimary = "Primary";
    const std::string kSemanticGainMap = "GainMap";
    const std::string kMimeImageJpeg = "image/jpeg";

    constexpr size_t kNumPictures = 2;
    constexpr size_t kMpEndianSize = 4;
    constexpr uint16_t kTagSerializedCount = 3;
    constexpr uint32_t kTagSize = 12;

    static constexpr uint8_t kMpfSig[] = { 'M', 'P', 'F', '\0' };
    static constexpr uint8_t kMpLittleEndian[kMpEndianSize] = { 0x49, 0x49, 0x2A, 0x00 };
    static constexpr uint8_t kMpBigEndian[kMpEndianSize] = { 0x4D, 0x4D, 0x00, 0x2A };

    constexpr uint32_t kMPEntrySize = 16;

    constexpr uint16_t kTypeUndefined = 0x7;

    constexpr uint16_t kVersionTag = 0xB000;
    constexpr uint16_t kVersionType = kTypeUndefined;
    constexpr uint32_t kVersionCount = 4;
    constexpr size_t kVersionSize = 4;
    constexpr uint8_t kVersionExpected[kVersionSize] = { '0', '1', '0', '0' };

    constexpr uint16_t kTypeLong = 0x4;

    constexpr uint16_t kNumberOfImagesTag = 0xB001;
    constexpr uint16_t kNumberOfImagesType = kTypeLong;
    constexpr uint32_t kNumberOfImagesCount = 1;

    constexpr uint16_t kMPEntryTag = 0xB002;
    constexpr uint16_t kMPEntryType = kTypeUndefined;

    constexpr uint32_t kMPEntryAttributeFormatJpeg = 0x0000000;
    constexpr uint32_t kMPEntryAttributeTypePrimary = 0x030000;


#undef Endian_SwapBE32
#undef Endian_SwapBE16
#if USE_BIG_ENDIAN_IN_MPF
#define Endian_SwapBE32(n) EndianSwap32(n)
#define Endian_SwapBE16(n) EndianSwap16(n)
#else
#define Endian_SwapBE32(n) (n)
#define Endian_SwapBE16(n) (n)
#endif


    size_t calculateMpfSize() {
        return sizeof(kMpfSig) +                 // Signature
            kMpEndianSize +                   // Endianness
            sizeof(uint32_t) +                // Index IFD Offset
            sizeof(uint16_t) +                // Tag count
            kTagSerializedCount * kTagSize +  // 3 tags at 12 bytes each
            sizeof(uint32_t) +                // Attribute IFD offset
            kNumPictures * kMPEntrySize;      // MP Entries for each image
    }

    class DataStruct {
    private:
        void* data;
        size_t writePos;
        size_t length;

    public:
        DataStruct(size_t s);
        ~DataStruct();

        void* getData();
        size_t getLength();
        size_t getBytesWritten();
        bool write8(uint8_t value);
        bool write16(uint16_t value);
        bool write32(uint32_t value);
        bool write(const void* src, size_t size);

#define ALOGE(...) ((void)0)
    };

    std::shared_ptr<DataStruct> generateMpf(size_t primary_image_size, size_t primary_image_offset,
        size_t secondary_image_size, size_t secondary_image_offset);
}

#include <sstream>
#include <vector>

namespace photos_editing_formats {
    namespace image_io {

        /// A very simple writer forXML that frees client code from worries about XML
        /// formatting and bracket issues.
        ///
        /// The intended sequence of operations this writer supports is as follows:
        /// 1. Start writing an element.
        /// 2. Write any and all attribute names and values to that element.
        /// 3. Write any content, or add a child element by starting to write another
        ///    element (i.e., go to step 1). The "context" of the current element you
        ///    are writing is saved on a stack. Once you start writing content or
        ///    child elements you cannot add attribute names and values and expect to
        ///    see them as such in the resulting XML.
        /// 4. When you are done with the element, finish writing it. The element
        ///    context stack is popped and you continue where you left off.
        ///
        /// When writing element content and attribute values no XML escaping of any
        /// kind is done. If you need to do that, do it yourself.
        class XmlWriter {
        public:
            /// @param os The stream to which the XML is written.
            explicit XmlWriter(std::ostream& os);

            /// @return The number of elements that have been written.
            size_t GetElementCount() const { return element_count_; }

            /// @return The depth of the element stack.
            size_t GetElementDepth() const { return element_data_.size(); }

            /// @return The quote mark used when writing attribute values. The default
            /// value set up by the constructor is the double quote (").
            char GetQuoteMark() const { return quote_mark_; }

            /// @param quote_park The new quote mark to use when writing attribute values.
            void SetQuoteMark(char quote_mark) { quote_mark_ = quote_mark; }

            /// @return The leading indent written before the current element.
            const std::string& GetIndent() const { return indent_; }

            /// Once you are done writing your elements, you can call this function to
            /// finish writing of all open elements. After this call, the string contained
            /// in the ostream you passed to the constructor is fully formed XML.
            void FinishWriting() { FinishWritingElementsToDepth(0); }

            /// @return Whether the writing of XML can be considered done.
            bool IsDone() const { return indent_.empty(); }

            /// Writes an xmlns attribute to the currently open element.
            /// @param prefix The prefix you intend to use for elements/attributes.
            /// @param uri The uri of the namespace.
            void WriteXmlns(const std::string& prefix, const std::string& uri);

            /// Starts writing a new child element of the current element. Immediately
            /// after this function you can add attributes to the element using one of the
            /// AddAttributeNameAndValue() functions.
            /// @param element_name The name of the element to write.
            /// @return The number of open elements on the stack at the start of this
            /// function. You can use this value with the FinishWritingElementToDepth()
            /// function to finish writing this element and any open descendents.
            size_t StartWritingElement(const std::string& element_name);

            /// Finishes writing the element and returns the "context" to the previously
            /// open element so that you can continue adding child elements (via a call to
            /// StartWritingElement()) or content (via a call to WriteContent()).
            void FinishWritingElement();

            /// Finishes writing any elements that exist in the stack of open elements
            /// above the depth value parameter.
            /// @param depth The depth above which to finish writing open elements.
            void FinishWritingElementsToDepth(size_t depth);

            /// Starts writing the elements in the vector, leaving the last open for you
            /// to add attributes or other elements to.
            /// @param element_names The array of element names to start writing.
            /// @return The number of open elements on the stack at the start of this
            /// function. You can use this value with the FinishWritingElementToDepth()
            /// function to finish writing this element and any open descendents.
            size_t StartWritingElements(const std::vector<std::string>& element_names);

            /// A template method function that allows you to start an element, add the
            /// value as its content and then finish writing the element. This is useful
            /// if you are writing property values as elements.
            /// @param element_name The name of the element to write.
            /// @param value The value that is converted to a string and written as the
            /// element's content.
            template <class T>
            void WriteElementAndContent(const std::string& element_name, const T& value) {
                std::stringstream ss;
                ss << value;
                WriteElementAndContent(element_name, ss.str());
            }

            /// Starts writing an element with the given name, adds the string value as
            /// its content and then finishes writing the element. This is useful
            /// if you are writing property values as elements.
            /// @param element_name The name of the element to write.
            /// @param value The value to use as the element's content.
            void WriteElementAndContent(const std::string& element_name,
                const std::string& content);

            /// Writes the string as the currently open element's content. Note that if
            /// you add child elements to the open element, the content you will see when
            /// you read your element will have the whitespace due to the indent string.
            /// @param content The content to write to the currently open element.
            void WriteContent(const std::string& content);

            /// A template method function that allows you to add an attribute name and
            /// value to a just-opened element. Attributes must be added to an element
            /// before adding content or child elements.
            /// @param name The name of the attribute to add.
            /// @param value The value of the attribute. This value is converted to a
            /// string and enclosed in the quote marks from the GetQuoteMark() function.
            template <class T>
            void WriteAttributeNameAndValue(const std::string& name, const T& value) {
                std::stringstream ss;
                ss << GetQuoteMark() << value << GetQuoteMark();
                WriteAttributeNameAndValue(name, ss.str(), false);
            }

            /// Adds an attribute name and value to a just-opened element. Attributes must
            /// be added to an element before adding content or child elements.
            /// @param name The name of the attribute to add.
            /// @param value The value of the attribute.
            /// @param add_quote_marks Whether quote marks should be added before and
            /// after the value. If this value is false, it is assumed that the client
            /// code has added them before calling this function.
            void WriteAttributeNameAndValue(const std::string& name,
                const std::string& value,
                bool add_quote_marks = true);

            /// Adds an attribute name and equal sign to the just-opened element.
            /// Attributes must be added to an element before adding content or child
            /// elements. Clients that use this function must call WriteAttributeValue()
            /// with appropriate values to define a legally quoted value. This function
            /// is useful for writing attribute with extremely long values that might not
            /// be efficient to store as a single string value.
            /// @param name The name of the attribute to add.
            void WriteAttributeName(const std::string& name);

            /// Writes the attribute value with optional quote marks on either side. This
            /// function may be repeatedly called with appropriate valeus for the leading
            /// and trailing quote mark flags to write extremely long attribute values.
            /// @param add_leading_quote_mark Whether to add a leading quote mark.
            /// @param value The (probably partial) value to write.
            /// @param add_trailing_quote_mark Whether to add a trailing quote mark.
            void WriteAttributeValue(bool add_leading_quote_mark,
                const std::string& value,
                bool add_trailing_quote_mark);

            /// Writes a comment to the xml stream. Note that writing a comment is like
            /// adding a child node/element to the current element. If the current element
            /// is still open for names/values, it will be closed before writing it - i.e.
            /// you can't add attributes to an element after calling this function.
            /// @param comment The text of the comment to write.
            void WriteComment(const std::string& comment);

        private:
            /// The data that is known about each element on the stack.
            struct ElementData {
                ElementData(const std::string& name_)
                    : name(name_),
                    has_attributes(false),
                    has_content(false),
                    has_children(false) {
                }
                std::string name;
                bool has_attributes;
                bool has_content;
                bool has_children;
            };

            /// Determines if the start element syntax of the current element needs to
            /// be closed with a bracket so that content or child elements or comments
            /// can be added to the element.
            /// @param with_trailing_newline Whether a newline is added after the bracket.
            /// @return Whether the element's start syntax was closed with a bracket.
            bool MaybeWriteCloseBracket(bool with_trailing_newline);

            /// The stream to which everything is written.
            std::ostream& os_;

            /// The indent to write before elements and attribute names/values.
            std::string indent_;

            /// The currently open elements being written.
            std::vector<ElementData> element_data_;

            /// The number of elements that have been written.
            size_t element_count_;

            /// The quote mark to use around attribute values by default.
            char quote_mark_;
        };
    }  // namespace image_io
}  // namespace photos_editing_formats
