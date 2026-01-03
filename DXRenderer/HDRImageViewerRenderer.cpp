#include "pch.h"
#include "HDRImageViewerRenderer.h"
#include "Common\DirectXHelper.h"
#include "DirectXTex.h"
#include "ImageExporter.h"
#include "MagicConstants.h"
#include "RenderEffects\SimpleTonemapEffect.h"
#include "DirectXTex\DirectXTexEXR.h"
#include <iostream>
#include <Windows.h>

#include <fstream>

using namespace DXRenderer;

using namespace DirectX;
using namespace Microsoft::WRL;
using namespace Platform;
using namespace std;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Graphics::Display;
using namespace Windows::Storage;
using namespace Windows::Storage::Streams;
using namespace Windows::UI::Input;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace jpegR;

HDRImageViewerRenderer::HDRImageViewerRenderer(
    SwapChainPanel^ panel) :
    m_renderEffectKind(RenderEffectKind::None),
    m_zoom(1.0f),
    m_minZoom(1.0f), // Dynamically calculated on window size.
    m_imageOffset(),
    m_pointerPos(),
    m_imageCLL{ -1.0f, -1.0f, -1.0f, false },
    m_exposureAdjust(1.0f),
    m_dispMaxCLLOverride(0.0f),
    m_imageInfo{},
    m_isComputeSupported(false),
    m_enableTargetCpuReadback(false),
    m_constrainGamut(true)
{
    // DeviceResources must be initialized first.
    // TODO: Current architecture does not allow multiple Renderers to share DeviceResources.
    m_deviceResources = std::make_shared<DeviceResources>();
    m_deviceResources->SetSwapChainPanel(panel);

    // Register to be notified if the GPU device is lost or recreated.
    m_deviceResources->RegisterDeviceNotify(this);

    CreateDeviceIndependentResources();
    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();
}

HDRImageViewerRenderer::~HDRImageViewerRenderer()
{
    // Deregister device notification.
    m_deviceResources->RegisterDeviceNotify(nullptr);
}

void HDRImageViewerRenderer::CreateDeviceIndependentResources()
{
    auto fact = m_deviceResources->GetD2DFactory();

    // TODO: This instance never does anything as it gets overwritten upon image load.
    m_imageLoader = std::make_unique<ImageLoader>(m_deviceResources, ImageLoaderOptions{});

    // Register the custom render effects.
    IFT(SimpleTonemapEffect::Register(fact));
    IFT(SdrOverlayEffect::Register(fact));
    IFT(LuminanceHeatmapEffect::Register(fact));
    IFT(MaxLuminanceEffect::Register(fact));
    IFT(SphereMapEffect::Register(fact));
}

void HDRImageViewerRenderer::CreateDeviceDependentResources()
{
    // All this app's device-dependent resources also depend on
    // the loaded image, so they are all created in
    // CreateImageDependentResources.
}

void HDRImageViewerRenderer::ReleaseDeviceDependentResources()
{
    m_imageLoader->ReleaseDeviceDependentResources();
}

// Whenever the app window is resized or changes displays, this method is used
// to update the app's sizing and advanced color state.
void HDRImageViewerRenderer::CreateWindowSizeDependentResources()
{
    // Window size changes don't require recomputing image HDR metadata.
    FitImageToWindow(false);
}

/// <summary>
/// Updates rendering parameters, and draws. If CPU readback is enabled, updates the CPU-side render target cache.
/// Always calls Draw() to refresh visual output.
/// </summary>
/// <param name="effect"></param>
/// <param name="exposureAdjustment">
/// Multiplication factor for color values; allows the user to
/// adjust the brightness of the image on an HDR display.</param>
/// <param name="dispMaxCllOverride">0 indicates no override (use the display's actual MaxCLL).</param>
/// <param name="acInfo">If nullptr, assumes HDR display.</param>
void HDRImageViewerRenderer::SetRenderOptions(
    RenderEffectKind effect,
    float exposureAdjustment,
    float dispMaxCllOverride,
    AdvancedColorInfo^ acInfo,
    bool constrainGamut
    )
{
    m_dispInfo = acInfo;
    m_renderEffectKind = effect;
    m_exposureAdjust = exposureAdjustment;
    m_dispMaxCLLOverride = dispMaxCllOverride;
    m_constrainGamut = constrainGamut;

    float lum;
    if (acInfo == nullptr) {
        lum = 1000;
    }
    else {
        lum = dispMaxCllOverride;
    }

    struct _colors
    {
        float redx, redy, greenx, greeny, bluex, bluey, whitex, whitey;
    };

    // TODO: If using a nullptr acInfo, handle gamut transforms.
    _colors color {};
    if (m_dispInfo)
    {
        color =
        {
            m_dispInfo->RedPrimary.X, m_dispInfo->RedPrimary.Y,
            m_dispInfo->GreenPrimary.X, m_dispInfo->GreenPrimary.Y,
            m_dispInfo->BluePrimary.X, m_dispInfo->BluePrimary.Y,
            m_dispInfo->WhitePoint.X, m_dispInfo->WhitePoint.Y
        };

        UpdateGamutTransforms();
    }

    auto sdrWhite = m_dispInfo ? m_dispInfo->SdrWhiteLevelInNits : D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL;
    auto acKind = m_dispInfo ? m_dispInfo->CurrentAdvancedColorKind : AdvancedColorKind::HighDynamicRange;

    UpdateWhiteLevelScale(m_exposureAdjust, sdrWhite);

    // Adjust the Direct2D effect graph based on RenderEffectKind.
    // Some RenderEffectKind values require us to apply brightness adjustment
    // after the effect as their numerical output is affected by any luminance boost.
    switch (m_renderEffectKind)
    {
    // Effect graph: ImageSource > ColorManagement > [Optional GainMapMerge] > WhiteScale > HDRTonemap > WhiteScale2*
    case RenderEffectKind::HdrTonemap:
        if (acKind != AdvancedColorKind::HighDynamicRange)
        {
            // *Second white scale is needed as an integral part of using the Direct2D HDR
            // tonemapper on SDR/WCG displays to stay within [0, 1] numeric range.
            m_finalOutput = m_sdrWhiteScaleEffect.Get();
        }
        else
        {
            m_finalOutput = m_hdrTonemapEffect.Get();
        }

        m_sdrWhiteScaleEffect->SetInputEffect(0, m_hdrTonemapEffect.Get());
        m_whiteScaleEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());
        break;

    // Effect graph: ImageSource > ColorManagement > [Optional GainMapMerge] > WhiteScale
    case RenderEffectKind::None:
        m_finalOutput = m_whiteScaleEffect.Get();
        m_whiteScaleEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());
        break;

    // Effect graph: ImageSource > ColorManagement > [Optional GainMapMerge] > Heatmap > WhiteScale
    case RenderEffectKind::LuminanceHeatmap:
        m_finalOutput = m_whiteScaleEffect.Get();
        m_whiteScaleEffect->SetInputEffect(0, m_heatmapEffect.Get());
        break;

    // Effect graph: ImageSource > ColorManagement > [Optional GainMapMerge] > MaxLuminance > WhiteScale
    case RenderEffectKind::MaxLuminance:
        m_finalOutput = m_whiteScaleEffect.Get();
        m_whiteScaleEffect->SetInputEffect(0, m_maxLuminanceEffect.Get());

        lum = Clamp(lum, 80.0f, 10000.0f);
        m_maxLuminanceEffect->SetValueByName(L"MaxLuminance", lum);
        break;

    // Effect graph: ImageSource > ColorManagement > [Optional GainMapMerge] > SdrOverlay > WhiteScale
    case RenderEffectKind::SdrOverlay:
        m_finalOutput = m_whiteScaleEffect.Get();
        m_whiteScaleEffect->SetInputEffect(0, m_sdrOverlayEffect.Get());
        break;

    // Effect graph: ImageSource > ColorManagement > [Optional GainMapMerge] > WhiteScale > SphereMap
    case RenderEffectKind::SphereMap:
        m_finalOutput = m_sphereMapEffect.Get();
        m_whiteScaleEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());
        break;

    default:
        throw ref new NotImplementedException();
        break;
    }

    float targetMaxNits = GetBestDispMaxLuminance();

    // Update HDR tonemappers with display information.
    // The custom tonemapper uses mostly the same property definitions as the 1809 Direct2D tonemapper, for simplicity.
    IFT(m_hdrTonemapEffect->SetValue(D2D1_HDRTONEMAP_PROP_OUTPUT_MAX_LUMINANCE, targetMaxNits));

    float maxCLL = m_imageCLL.maxNits != -1.0f ? m_imageCLL.maxNits : sc_DefaultImageMaxCLL;
    maxCLL *= m_exposureAdjust;

    // Very low input max luminance can produce unexpected rendering behavior. Restrict to
    // a reasonable level - the Direct2D tonemapper performs nearly a no-op if input < output max nits.
    maxCLL = max(maxCLL, D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL);

    IFT(m_hdrTonemapEffect->SetValue(D2D1_HDRTONEMAP_PROP_INPUT_MAX_LUMINANCE, maxCLL));

    // Don't use the SDR display tone mapper mode as it raises midtones a lot.
    IFT(m_hdrTonemapEffect->SetValue(D2D1_HDRTONEMAP_PROP_DISPLAY_MODE, D2D1_HDRTONEMAP_DISPLAY_MODE_HDR));

    // If an HDR tonemapper is used on an SDR or WCG display, perform additional white level correction.
    if (acKind != AdvancedColorKind::HighDynamicRange)
    {
        // Both the D2D and custom HDR tonemappers output values in scRGB using scene-referred luminance - a typical SDR display will
        // be around numeric range [0.0, 3.0] corresponding to [0, 240 nits]. To encode correctly for an SDR/WCG display
        // output, we must reinterpret the scene-referred input content (80 nits) as display-referred (targetMaxNits).
        // Some HDR images are dimmer than targetMaxNits, in those cases the tonemapper basically passes through.
        IFT(m_sdrWhiteScaleEffect->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL));
        IFT(m_sdrWhiteScaleEffect->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, min(targetMaxNits, maxCLL)));
    }

    // If the gamut map conversion is enabled, insert the 2 color matrix effects needed to perform that operation.
    // What we're doing here is transforming the colors from scRGB colors to panel-relative colors, and clamping 
    // that output to 0-1, then a second effect converts back to rec 709 colorimetry without clipping.
    if (m_constrainGamut)
    {
        m_mapGamutToPanel->SetInputEffect(0, m_finalOutput.Get());
        m_mapGamutToScRGB->SetInputEffect(0, m_mapGamutToPanel.Get());

        m_finalOutput = m_mapGamutToScRGB.Get();
    }

    Draw();

    if (m_enableTargetCpuReadback)
    {
        // Draw the final rendered output.
        ComPtr<ID2D1Image> image;
        IFT(m_finalOutput.As(&image));

        D2D1_SIZE_U size = m_deviceResources->GetD2DTargetBitmap()->GetPixelSize();
        m_renderTargetCpuPixels = ImageExporter::DumpImageToRGBFloat(m_deviceResources.get(), image.Get(), size);
    }
    else
    {
        m_renderTargetCpuPixels.clear();
    }
}

ImageInfo HDRImageViewerRenderer::LoadImageFromWic(_In_ IRandomAccessStream^ imageStream, ImageLoaderOptions options)
{
    ComPtr<IStream> iStream;
    IFT(CreateStreamOverRandomAccessStream(imageStream, IID_PPV_ARGS(&iStream)));

    m_imageLoader = std::make_unique<ImageLoader>(m_deviceResources, options);
    m_imageInfo = m_imageLoader->LoadImageFromWic(iStream.Get());
    return m_imageInfo;
}

ImageInfo HDRImageViewerRenderer::LoadImageFromDirectXTex(String ^ filename, String ^ extension, ImageLoaderOptions options)
{
    m_imageLoader = std::make_unique<ImageLoader>(m_deviceResources, options);
    m_imageInfo = m_imageLoader->LoadImageFromDirectXTex(filename, extension);
    return m_imageInfo;
}

void HDRImageViewerRenderer::ExportImageToSdr(_In_ IRandomAccessStream^ outputStream, Guid wicFormat)
{
    ComPtr<IStream> iStream;
    IFT(CreateStreamOverRandomAccessStream(outputStream, IID_PPV_ARGS(&iStream)));

    ImageExporter::ExportToSdr(m_imageLoader.get(), m_deviceResources.get(), iStream.Get(), wicFormat);
}

// Test only. Exports to DXGI encoded DDS.
void HDRImageViewerRenderer::ExportAsDdsTest(_In_ IRandomAccessStream^ outputStream)
{
    auto wicSource = m_imageLoader->GetWicSourceTest();
    ComPtr<IWICBitmap> bitmap;
    IFT(wicSource->QueryInterface(IID_PPV_ARGS(&bitmap)));

    ComPtr<IStream> iStream;
    IFT(CreateStreamOverRandomAccessStream(outputStream, IID_PPV_ARGS(&iStream)));

    ImageExporter::ExportToDds(bitmap.Get(), iStream.Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
}

void HDRImageViewerRenderer::ExportImageToISOJpeg(Windows::Storage::Streams::IRandomAccessStream^ outputStream)
{
    startEncodeISOJpeg();

    DataWriter^ writer = ref new DataWriter(outputStream);


    writer->WriteBytes(
        Platform::ArrayReference<byte>(
            static_cast<byte*>(this->outputImage.data()),
            static_cast<unsigned int>(this->outputImage.size())
        )
    );

    concurrency::create_task(writer->StoreAsync())
        .then([writer](unsigned int bytesStored) {
        // 5. Flush the stream.
        return concurrency::create_task(writer->FlushAsync());
            })
        .then([writer](bool flushResult) {
        // 6. Cleanup writer (delete at the end).
        delete writer;
            });
}

/// <summary>
/// Save any supported HDR format as HDR JPEG XR. Not guaranteed to be lossless since we run the
/// full render pipeline.
/// Note: Using new approach for image export where we leverage the renderer itself instead of replicating
/// the render pipeline in ImageExporter.
/// Note: Calls Begin/EndDraw on the context.
/// </summary>
/// <param name="outputStream"></param>
void HDRImageViewerRenderer::ExportImageToJxr(Windows::Storage::Streams::IRandomAccessStream^ outputStream)
{
    // TODO: Keep in sync with any render pipeline changes.

    // Apply temp render pipeline state.
    auto saved_renderEffectKind = m_renderEffectKind;
    auto saved_exposureAdjust = m_exposureAdjust;
    auto saved_dispMaxCLLOverride = m_dispMaxCLLOverride;
    auto saved_constrainGamut = m_constrainGamut;
    auto saved_dispInfo = m_dispInfo;

    // SetRenderOptions sets the member variables and calls Draw().
    // Note the nullptr acInfo to fake an HDR display.
    SetRenderOptions(RenderEffectKind::None, 1.0f, 0.0f, nullptr, false);

    // Apply temp spatial transform state after SetRenderOptions.
    auto saved_zoom = m_zoom;
    auto saved_imageOffset = m_imageOffset;
    auto ctx = m_deviceResources->GetD2DDeviceContext();

    m_zoom = 1.0f;
    m_imageOffset = { 0.0f, 0.0f };
    ctx->SetDpi(96.0f, 96.0f); // Image export always occurs without DPI scaling.

    UpdateImageTransformState();

    ComPtr<IStream> iStream;
    IFT(CreateStreamOverRandomAccessStream(outputStream, IID_PPV_ARGS(&iStream)));

    ComPtr<ID2D1Image> outputImage;
    m_finalOutput->GetOutput(&outputImage);

   /* ImageExporter::ExportToWic(outputImage.Get(),
        m_imageInfo.pixelSize,
        m_deviceResources.get(),
        iStream.Get(),
        GUID_ContainerFormatWmp);*/

    // Restore all state.
    m_zoom = saved_zoom;
    m_imageOffset = saved_imageOffset;
    ctx->SetDpi(m_deviceResources->GetDpi(), m_deviceResources->GetDpi());
    UpdateImageTransformState();

    // Call SetRenderOptions last as it calls Draw.
    SetRenderOptions(saved_renderEffectKind,
        saved_exposureAdjust,
        saved_dispMaxCLLOverride,
        saved_dispInfo,
        saved_constrainGamut);


    startEncodeISOJpeg();

    DataWriter^ writer = ref new DataWriter(outputStream);

   
        writer->WriteBytes(
            Platform::ArrayReference<byte>(
                static_cast<byte*>(this->outputImage.data()),
                static_cast<unsigned int>(this->outputImage.size())
            )
        );

        concurrency::create_task(writer->StoreAsync())
            .then([writer](unsigned int bytesStored) {
            // 5. Flush the stream.
            return concurrency::create_task(writer->FlushAsync());
                })
            .then([writer](bool flushResult) {
            // 6. Cleanup writer (delete at the end).
            delete writer;
                });

    //    // 4. Flush the stream.
    //    auto flushAsync = writer->FlushAsync();
    //    concurrency::create_task(flushAsync).wait();
}

// Configures a Direct2D image pipeline, including source, color management, 
// tonemapping, and white level, based on the loaded image. Also responsible for m_imageLoader.
void HDRImageViewerRenderer::CreateImageDependentResources()
{
    // If we just came from device lost/restored, we need to manually re-setup ImageLoader.
    if (m_imageLoader->GetState() == ImageLoaderState::NeedDeviceResources)
    {
        m_imageLoader->CreateDeviceDependentResources();
    }

    auto d2dFactory = m_deviceResources->GetD2DFactory();
    auto context = m_deviceResources->GetD2DDeviceContext();

    // Configure the app's effect pipeline, consisting of a color management effect
    // followed by a tone mapping effect.

    IFT(context->CreateEffect(CLSID_D2D1ColorManagement, &m_colorManagementEffect));
    // The pipeline input is set in UpdateImageTransformState().

    IFT(m_colorManagementEffect->SetValue(
            D2D1_COLORMANAGEMENT_PROP_QUALITY,
            D2D1_COLORMANAGEMENT_QUALITY_BEST));   // Required for floating point and DXGI color space support.

    // The color management effect takes a source color space and a destination color space,
    // and performs the appropriate math to convert images between them.
    IFT(m_colorManagementEffect->SetValue(
            D2D1_COLORMANAGEMENT_PROP_SOURCE_COLOR_CONTEXT,
            m_imageLoader->GetImageColorContext()));

    // Perceptual (default) intent can introduce gamut compression. This is undesirable at the start of an HDR/WCG
    // render pipeline, as any gamut mapping should occur at the output stage.
    IFT(m_colorManagementEffect->SetValue(
        D2D1_COLORMANAGEMENT_PROP_SOURCE_RENDERING_INTENT,
        D2D1_COLORMANAGEMENT_RENDERING_INTENT_RELATIVE_COLORIMETRIC));

    // The destination color space is the render target's (swap chain's) color space. This app uses an
    // FP16 swap chain, which requires the colorspace to be scRGB.
    ComPtr<ID2D1ColorContext1> destColorContext;
    IFT(context->CreateColorContextFromDxgiColorSpace(
            DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, // scRGB
            &destColorContext));

    IFT(m_colorManagementEffect->SetValue(
            D2D1_COLORMANAGEMENT_PROP_DESTINATION_COLOR_CONTEXT,
            destColorContext.Get()));

    IFT(context->CreateEffect(CLSID_D2D1Scale, &m_imageScaleEffect));
    IFT(m_imageScaleEffect->SetValue(
        D2D1_SCALE_PROP_INTERPOLATION_MODE,
        D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC));

    // Next, merge the Apple HDR gainmap with the main image to recover HDR highlights.
    // This occurs after color management to scRGB but before any further stages which rely on HDR pixel data.
    // The parameters of the gainmap are empirically determined:
    // * 50% of the main image resolution
    // * 8-bit grayscale linear gamma luminance data, but is not calibrated to any absolute scale
    // * A value of 0.5f (or 128) is approximately equal to diffuse white in the scene
    // * Naively multiplying the gainmap by the main image in linear RGB approximates the visual effect
    //   in the iOS Photos app.
    
    if (m_imageInfo.hasAppleHdrGainMap == true || m_imageInfo.hasIsoHeicHdrGainMap == true || m_imageInfo.hasAppleHeicHdrGainMap == true)
    {
        IFT(context->CreateEffect(CLSID_D2D1GammaTransfer, &m_gainmapLinearEffect));

        // Approximate the linearization step by applying gamma of 1/2.2.
        m_gainmapLinearEffect->SetValue(D2D1_GAMMATRANSFER_PROP_RED_EXPONENT, 1.f / 2.2f);
        m_gainmapLinearEffect->SetValue(D2D1_GAMMATRANSFER_PROP_GREEN_EXPONENT, 1.f / 2.2f);
        m_gainmapLinearEffect->SetValue(D2D1_GAMMATRANSFER_PROP_BLUE_EXPONENT, 1.f / 2.2f);

        // Gain map input is set in UpdateImageTransformState().

        // This is treated as an HDR image, so we must use scene-referred luminance and read the system SDR white level.
        IFT(context->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, &m_gainmapRefWhiteEffect));
        m_gainmapRefWhiteEffect->SetInputEffect(0, m_gainmapLinearEffect.Get());

        auto sdrWhite = m_dispInfo ? m_dispInfo->SdrWhiteLevelInNits : D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL;
        IFT(m_gainmapRefWhiteEffect->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_INPUT_WHITE_LEVEL, sdrWhite));
        IFT(m_gainmapRefWhiteEffect->SetValue(D2D1_WHITELEVELADJUSTMENT_PROP_OUTPUT_WHITE_LEVEL, D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL));

        // IFT(context->CreateEffect(CLSID_D2D1ArithmeticComposite, &m_gainMapMergeEffect));

        // m_gainMapMergeEffect->SetInputEffect(0, m_colorManagementEffect.Get());
        // m_gainMapMergeEffect->SetInputEffect(1, m_gainmapRefWhiteEffect.Get());

        // // Coefficients A, B, C, D: Output = A*source*dest + B*source + C*dest + D.
        // m_gainMapMergeEffect->SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_COEFFICIENTS, D2D1::Vector4F(2.f, 0.0f, 0.0f, 0.0f));

         IFT(context->CreateEffect(CLSID_D2D1Composite, &m_gainMapMergeEffect));

         ComPtr<ID2D1Effect> identityPassthrough;
         IFT(context->CreateEffect(CLSID_D2D1ColorMatrix, &identityPassthrough));

         // Use an identity matrix (4x4 + offset).
         D2D1_MATRIX_5X4_F id =
         {
             1, 0, 0, 0,   // R'
             0, 1, 0, 0,   // G'
             0, 0, 1, 0,   // B'
             0, 0, 0, 1,   // A'
             0, 0, 0, 0    // offset
         };
         identityPassthrough->SetValue(
             D2D1_COLORMATRIX_PROP_COLOR_MATRIX,
             id
         );

         // Input is set in UpdateImageTransformState().

         // Assign the identity pass-through to the merge effect.
         m_gainMapMergeEffect = identityPassthrough;
    }
    else
    {
        IFT(m_colorManagementEffect.CopyTo(&m_gainMapMergeEffect)); // Pass-through.
    }

    // White level scale is used to multiply the color values in the image; this allows the user
    // to adjust the brightness of the image on an HDR display.
    IFT(context->CreateEffect(CLSID_D2D1ColorMatrix, &m_whiteScaleEffect));

    // Input to white level scale may be modified in SetRenderOptions.
    m_whiteScaleEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());

    // Set the actual matrix in SetRenderOptions.

    // Instantiate and cache all of the tonemapping/render effects.
    // Some effects are implemented as Direct2D custom effects; see the RenderEffects filter in the
    // Solution Explorer.

    GUID tonemapper = {};
    if (CheckPlatformSupport(Win1809))
    {
        // HDR tonemapper and white level adjust are only available in 1809 and above.
        tonemapper = CLSID_D2D1HdrToneMap;
    }
    else
    {
        // TODO: The custom tonemapper is never used in product code, only for testing.
        tonemapper = CLSID_CustomSimpleTonemapEffect;
    }

    IFT(context->CreateEffect(tonemapper, &m_hdrTonemapEffect));
    IFT(context->CreateEffect(CLSID_D2D1WhiteLevelAdjustment, &m_sdrWhiteScaleEffect));
    IFT(context->CreateEffect(CLSID_CustomSdrOverlayEffect, &m_sdrOverlayEffect));
    IFT(context->CreateEffect(CLSID_CustomLuminanceHeatmapEffect, &m_heatmapEffect));
    IFT(context->CreateEffect(CLSID_CustomMaxLuminanceEffect, &m_maxLuminanceEffect));
    IFT(context->CreateEffect(CLSID_CustomSphereMapEffect, &m_sphereMapEffect));

    IFT(context->CreateEffect(CLSID_D2D1ColorMatrix, &m_mapGamutToPanel));
    IFT(context->CreateEffect(CLSID_D2D1ColorMatrix, &m_mapGamutToScRGB));

    // TEST: border effect to remove seam at the boundary of the image (subpixel sampling)
    // Unclear if we can force D2D_BORDER_MODE_HARD somewhere to avoid the seam.
    ComPtr<ID2D1Effect> border;
    IFT(context->CreateEffect(CLSID_D2D1Border, &border));

    border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_X, D2D1_BORDER_EDGE_MODE_WRAP);
    border->SetValue(D2D1_BORDER_PROP_EDGE_MODE_Y, D2D1_BORDER_EDGE_MODE_WRAP);
    border->SetInputEffect(0, m_whiteScaleEffect.Get());

    m_hdrTonemapEffect->SetInputEffect(0, m_whiteScaleEffect.Get());
    m_sphereMapEffect->SetInputEffect(0, border.Get());

    // SphereMap needs to know the pixel size of the image.
    IFT(m_sphereMapEffect->SetValue(
            SPHEREMAP_PROP_SCENESIZE,
            D2D1::SizeF(m_imageInfo.pixelSize.Width, m_imageInfo.pixelSize.Height)));

    // For the following effects, we want white level scale to be applied after
    // tonemapping (otherwise brightness adjustments will affect numerical values).
    m_heatmapEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());
    m_maxLuminanceEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());
    m_sdrOverlayEffect->SetInputEffect(0, m_gainMapMergeEffect.Get());

    // The remainder of the Direct2D effect graph is constructed in SetRenderOptions based on the
    // selected RenderEffectKind.

    CreateHistogramResources();
}

// Perform histogram pipeline setup; this should occur as part of image resource creation.
// Histogram results in no visual output but is used to calculate HDR metadata for the image.
void HDRImageViewerRenderer::CreateHistogramResources()
{
    auto context = m_deviceResources->GetD2DDeviceContext();

    // We need to preprocess the image data before running the histogram.
    // 1. Spatial downscale to reduce the amount of processing and limit intermediate texture size.
    IFT(context->CreateEffect(CLSID_D2D1Scale, &m_histogramPrescale));

    // Cap histogram pixel size to 2048 along the larger dimension.
    float pixScale = min(0.5f, 2048.0f / max(m_imageInfo.pixelSize.Width, m_imageInfo.pixelSize.Height));

    IFT(m_histogramPrescale->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(pixScale, pixScale)));
    IFT(m_histogramPrescale->SetValue(
        D2D1_SCALE_PROP_INTERPOLATION_MODE,
        D2D1_SCALE_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC));

    // The right place to compute HDR metadata is after color management to the
    // image's native colorspace but before any tonemapping or adjustments for the display.
    m_histogramPrescale->SetInputEffect(0, m_gainMapMergeEffect.Get());

    // 2. Convert scRGB data into luminance (nits).
    // 3. Normalize color values. Histogram operates on [0-1] numeric range,
    //    while FP16 can go up to 65504 (5+ million nits).
    // Both steps are performed in the same color matrix.
    ComPtr<ID2D1Effect> histogramMatrix;
    IFT(context->CreateEffect(CLSID_D2D1ColorMatrix, &histogramMatrix));

    histogramMatrix->SetInputEffect(0, m_histogramPrescale.Get());

    float scale = sc_histMaxNits / D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL;

    D2D1_MATRIX_5X4_F rgbtoYnorm = D2D1::Matrix5x4F(
        0.2126f / scale, 0, 0, 0,
        0.7152f / scale, 0, 0, 0,
        0.0722f / scale, 0, 0, 0,
        0              , 0, 0, 1,
        0              , 0, 0, 0);
    // 1st column: [R] output, contains normalized Y (CIEXYZ).
    // 2nd column: [G] output, unused.
    // 3rd column: [B] output, unused.
    // 4th column: [A] output, alpha passthrough.
    // We explicitly calculate Y; this deviates from the CEA 861.3 definition of MaxCLL
    // which approximates luminance with max(R, G, B).

    IFT(histogramMatrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, rgbtoYnorm));

    // 4. Apply a gamma to allocate more histogram bins to lower luminance levels.
    ComPtr<ID2D1Effect> histogramGamma;
    IFT(context->CreateEffect(CLSID_D2D1GammaTransfer, &histogramGamma));

    histogramGamma->SetInputEffect(0, histogramMatrix.Get());

    // Gamma function offers an acceptable tradeoff between simplicity and efficient bin allocation.
    // A more sophisticated pipeline would use a more perceptually linear function than gamma.
    IFT(histogramGamma->SetValue(D2D1_GAMMATRANSFER_PROP_RED_EXPONENT, sc_histGamma));
    // All other channels are passthrough.
    IFT(histogramGamma->SetValue(D2D1_GAMMATRANSFER_PROP_GREEN_DISABLE, TRUE));
    IFT(histogramGamma->SetValue(D2D1_GAMMATRANSFER_PROP_BLUE_DISABLE, TRUE));
    IFT(histogramGamma->SetValue(D2D1_GAMMATRANSFER_PROP_ALPHA_DISABLE, TRUE));

    // 5. Flush the stream.
    HRESULT hr = context->CreateEffect(CLSID_D2D1Histogram, &m_histogramEffect);
    
    if (hr == D2DERR_INSUFFICIENT_DEVICE_CAPABILITIES)
    {
        // The GPU doesn't support compute shaders and we can't run histogram on it.
        m_isComputeSupported = false;
    }
    else
    {
        IFT(hr);
        m_isComputeSupported = true;

        IFT(m_histogramEffect->SetValue(D2D1_HISTOGRAM_PROP_NUM_BINS, sc_histNumBins));

        m_histogramEffect->SetInputEffect(0, histogramGamma.Get());
    }
}

void HDRImageViewerRenderer::ReleaseImageDependentResources()
{
    // TODO: This method is only called during device lost. In that situation,
    // m_imageLoader should not be reset. Confirm this is the only case we want to call this.

    m_loadedImage.Reset();
    m_loadedGainMap.Reset();
    m_gainmapLinearEffect.Reset();
    m_imageScaleEffect.Reset();
    m_gainmapRefWhiteEffect.Reset();
    m_gainMapMergeEffect.Reset();
    m_colorManagementEffect.Reset();
    m_whiteScaleEffect.Reset();
    m_sdrWhiteScaleEffect.Reset();
    m_hdrTonemapEffect.Reset();
    m_sdrOverlayEffect.Reset();
    m_heatmapEffect.Reset();
    m_maxLuminanceEffect.Reset();
    m_histogramPrescale.Reset();
    m_histogramEffect.Reset();
    m_sphereMapEffect.Reset();
    m_mapGamutToPanel.Reset();
    m_mapGamutToScRGB.Reset();
    m_finalOutput.Reset();
}

/// <summary>
/// Sets whether to enable features that are dependent on having a CPU-cached copy of the render target.
/// Currently mainly used to analyze color values of the rendered output. CPU-cached copy is updated
/// with each call to SetRenderOptions().
/// </summary>
/// <param name="value">Whether support should be enabled or disabled.</param>
void HDRImageViewerRenderer::SetTargetCpuReadbackSupport(bool value)
{
    m_enableTargetCpuReadback = value;

    SetRenderOptions(m_renderEffectKind, m_exposureAdjust, 0.0f, m_dispInfo, m_constrainGamut);
}

/// <summary>
/// Returns the scRGB color value at the position in the render target.
/// </summary>
/// <param name="point">Position in the render target in DIPs (device independent pixels).</param>
/// <returns>If unsupported, all values are set to -1.0f.</returns>
Windows::Foundation::Numerics::float4 HDRImageViewerRenderer::GetPixelColorValue(Point point)
{
    auto color = Windows::Foundation::Numerics::float4(-1.0f);

    if (m_enableTargetCpuReadback)
    {
        auto targetSize = m_deviceResources->GetOutputSize();
        int offset = static_cast<int>(targetSize.Width) * static_cast<int>(point.Y) + static_cast<int>(point.X) * 3; // Channels per pixel.

        color.x = m_renderTargetCpuPixels.at(offset);
        color.y = m_renderTargetCpuPixels.at(offset + 1);
        color.z = m_renderTargetCpuPixels.at(offset + 2);
        color.w = 1.0f;
    }

    return color;
}

void HDRImageViewerRenderer::UpdateManipulationState(_In_ ManipulationUpdatedEventArgs^ args)
{
    Point position = args->Position;
    Point positionDelta = args->Delta.Translation;
    float zoomDelta = args->Delta.Scale;

    if (m_renderEffectKind == RenderEffectKind::SphereMap)
    {
        // For sphere map, panning and zooming is implemented in the effect.
        m_pointerPos.x += positionDelta.X;
        m_pointerPos.y += positionDelta.Y;

        D2D1_SIZE_F targetSize = m_deviceResources->GetD2DDeviceContext()->GetSize();

        // Normalize panning position to pixel dimensions of render target.
        auto x = m_pointerPos.x / targetSize.width;
        auto y = m_pointerPos.y / targetSize.height;

        IFT(m_sphereMapEffect->SetValue(SPHEREMAP_PROP_CENTER, D2D1::Point2F(x, y)));

        m_zoom *= zoomDelta;
        m_zoom = Clamp(m_zoom, sc_MinZoomSphereMap, sc_MaxZoom);

        IFT(m_sphereMapEffect->SetValue(SPHEREMAP_PROP_ZOOM, m_zoom));
    }
    else
    {
        // Normal image pan/zoom for all other render effects.
        m_imageOffset.x += positionDelta.X;
        m_imageOffset.y += positionDelta.Y;

        // We want to have any zoom operation be "centered" around the pointer position, which
        // requires recalculating the view position based on the new zoom and pointer position.
        // Step 1: Calculate the absolute pointer position (image position).
        D2D1_POINT_2F pointerAbsolutePosition = D2D1::Point2F(
            (m_imageOffset.x - position.X) / m_zoom,
            (m_imageOffset.y - position.Y) / m_zoom);

        // Step 2: Apply the zoom; do not allow user to go beyond max zoom level.
        m_zoom *= zoomDelta;
        m_zoom = min(m_zoom, sc_MaxZoom);

        // Step 3: Adjust the view position based on the new m_zoom value.
        m_imageOffset.x = pointerAbsolutePosition.x * m_zoom + position.X;
        m_imageOffset.y = pointerAbsolutePosition.y * m_zoom + position.Y;

        // Step 4: Clamp the translation to the window bounds.
        Size panelSize = m_deviceResources->GetLogicalSize();
        m_imageOffset.x = Clamp(m_imageOffset.x, panelSize.Width - m_imageInfo.pixelSize.Width * m_zoom, 0);
        m_imageOffset.y = Clamp(m_imageOffset.y, panelSize.Height - m_imageInfo.pixelSize.Height * m_zoom, 0);

        UpdateImageTransformState();
    }

    Draw();
}

// Overrides any pan/zoom state set by the user to fit image to the window size.
// Returns the computed content light level (CLL) of the image in nits.
// Recomputing the HDR metadata is only needed when loading a new image.
ImageCLL HDRImageViewerRenderer::FitImageToWindow(bool computeMetadata)
{
    Size panelSize = m_deviceResources->GetLogicalSize();

    // TODO: Root cause why this method is sometimes called before the below prereqs are ready.
    if (m_imageLoader != nullptr && panelSize.Width != 0 && panelSize.Height != 0 &&
        m_imageLoader->GetState() == ImageLoaderState::LoadingSucceeded)
    {
        // Set image to be letterboxed in the window, up to the max allowed scale factor.
        float letterboxZoom = min(
            panelSize.Width / m_imageInfo.pixelSize.Width,
            panelSize.Height / m_imageInfo.pixelSize.Height);

        m_zoom = min(sc_MaxZoom, letterboxZoom);

        // SphereMap needs to know the pixel size of the image.
        IFT(m_sphereMapEffect->SetValue(
                SPHEREMAP_PROP_SCENESIZE,
                D2D1::SizeF(m_imageInfo.pixelSize.Width * m_zoom, m_imageInfo.pixelSize.Height * m_zoom)));

        // Center the image.
        m_imageOffset = D2D1::Point2F(
            (panelSize.Width - (m_imageInfo.pixelSize.Width * m_zoom)) / 2.0f,
            (panelSize.Height - (m_imageInfo.pixelSize.Height * m_zoom)) / 2.0f
        );

        UpdateImageTransformState();

        if (computeMetadata)
        {
            // HDR metadata is supposed to be independent of any rendering options, but
            // we can't compute it until the full effect graph is hooked up, which is here.
            ComputeHdrMetadata();
        }
    }

    return m_imageCLL;
}

// Scale the (linear gamma) brightness/luminance of the image. This is typically used for two reasons:
// 1) When connected to an HDR display, the OS renders SDR content (e.g. 8888 UNORM) at
// a user configurable white level; this typically is around 200-300 nits. It is the responsibility
// of an advanced color app (e.g. FP16 scRGB) to emulate the OS-implemented SDR white level adjustment,
// BUT only for non-HDR content (SDR or WCG).
// 2) Users may want to adjust the exposure of their image to personal preference, typically most useful
// when viewing HDR content on HDR displays.
void HDRImageViewerRenderer::UpdateWhiteLevelScale(float brightnessAdjustment, float sdrWhiteLevel)
{
    float scale = 1.0f;

    switch (m_imageInfo.imageKind)
    {
    case AdvancedColorKind::HighDynamicRange:
        // HDR gainmaps are output-referred and do need to be compensated by SdrWhiteLevel.
        if (m_imageInfo.hasAppleHdrGainMap == true || m_imageInfo.hasIsoHeicHdrGainMap == true || m_imageInfo.hasAppleHeicHdrGainMap == true)
        {
            scale = sdrWhiteLevel / D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL;
        }
        else
        {
            // Scene-referred luminance content should not be compensated by the SdrWhiteLevel parameter.
            // Most HDR images fall into this category.
            scale = 1.0f;
        }
        break;

    case AdvancedColorKind::StandardDynamicRange:
    case AdvancedColorKind::WideColorGamut:
    default:
        scale = sdrWhiteLevel / D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL;
        break;
    }

    // The user may want to manually adjust brightness specifically for this image, on top of any
    // white level adjustment for SDR/WCG content. Brightness adjustment using a linear gamma scale
    // is mainly useful for HDR displays, but can be useful for HDR content tonemapped to an SDR/WCG display.
    scale *= brightnessAdjustment;

    // SDR white level scaling is performing by multiplying RGB color values in linear gamma.
    // We implement this with a Direct2D matrix effect.
    D2D1_MATRIX_5X4_F matrix = D2D1::Matrix5x4F(
        scale, 0, 0, 0,  // [R] Multiply each color channel
        0, scale, 0, 0,  // [G] by the scale factor in 
        0, 0, scale, 0,  // [B] linear gamma space.
        0, 0, 0    , 1,  // [A] Preserve alpha values.
        0, 0, 0    , 0); //     No offset.

    IFT(m_whiteScaleEffect->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, matrix));
}

D2D1_MATRIX_5X4_F MatrixToD2D(Matrix m)
{
    return D2D1::Matrix5x4F(
        (float)m.M[0], (float)m.M[1], (float)m.M[2], 0,
        (float)m.M[3], (float)m.M[4], (float)m.M[5], 0,
        (float)m.M[6], (float)m.M[7], (float)m.M[8], 0,
                    0,             0,             0, 1,
                    0,             0,             0, 0 );

}

// If we need to constrain the gamut of the output to specified colorimetry, calculate and set the matrices
void HDRImageViewerRenderer::UpdateGamutTransforms()
{
    // Clamping the colors in panel space should have the effect of a colorimetric clip
    IFT(m_mapGamutToPanel->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE));
    IFT(m_mapGamutToScRGB->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, FALSE));

    auto M709 = Matrix(3, 3);
    auto XYZDisplay = Matrix(3, 3);
    auto WhiteDisplay = Matrix(1, 3);
    auto MDisplay = Matrix(3, 3);

    M709.M = {
        0.4124564, 0.3575761, 0.1804375,
        0.2126729, 0.7151522, 0.0721750,
        0.0193339, 0.1191920, 0.9503041
    };

    XYZDisplay.M[0] = m_dispInfo->RedPrimary.X / m_dispInfo->RedPrimary.Y;
    XYZDisplay.M[1] = m_dispInfo->GreenPrimary.X / m_dispInfo->GreenPrimary.Y;
    XYZDisplay.M[2] = m_dispInfo->BluePrimary.X / m_dispInfo->BluePrimary.Y;
    XYZDisplay.M[3] = 1.f;
    XYZDisplay.M[4] = 1.f;
    XYZDisplay.M[5] = 1.f;
    XYZDisplay.M[6] = (1.f - m_dispInfo->RedPrimary.X   - m_dispInfo->RedPrimary.Y)   / m_dispInfo->RedPrimary.Y;
    XYZDisplay.M[7] = (1.f - m_dispInfo->GreenPrimary.X - m_dispInfo->GreenPrimary.Y) / m_dispInfo->GreenPrimary.Y;
    XYZDisplay.M[8] = (1.f - m_dispInfo->BluePrimary.X  - m_dispInfo->BluePrimary.Y)  / m_dispInfo->BluePrimary.Y;

    WhiteDisplay.M[0] = m_dispInfo->WhitePoint.X / m_dispInfo->WhitePoint.Y;
    WhiteDisplay.M[1] = 1.f;
    WhiteDisplay.M[2] = (1.f - m_dispInfo->WhitePoint.X - m_dispInfo->WhitePoint.Y) / m_dispInfo->WhitePoint.Y;

    auto S = XYZDisplay.Invert() * WhiteDisplay;

    MDisplay.M[0] = S.M[0] * XYZDisplay.M[0];
    MDisplay.M[1] = S.M[1] * XYZDisplay.M[1];
    MDisplay.M[2] = S.M[2] * XYZDisplay.M[2];
    MDisplay.M[3] = S.M[0] * XYZDisplay.M[3];
    MDisplay.M[4] = S.M[1] * XYZDisplay.M[4];
    MDisplay.M[5] = S.M[2] * XYZDisplay.M[5];
    MDisplay.M[6] = S.M[0] * XYZDisplay.M[6];
    MDisplay.M[7] = S.M[1] * XYZDisplay.M[7];
    MDisplay.M[8] = S.M[2] * XYZDisplay.M[8];

    auto transform = MDisplay.Invert() * M709;

    auto gamutToPanel = MatrixToD2D(transform);
    m_mapGamutToPanel->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, gamutToPanel);

    auto panelToScRGB = MatrixToD2D(transform.Invert());
    m_mapGamutToScRGB->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, panelToScRGB);
}

// Call this after updating any spatial transform state to regenerate the effect graph.
void HDRImageViewerRenderer::UpdateImageTransformState()
{
    if (m_imageLoader->GetState() == ImageLoaderState::LoadingSucceeded)
    {
        // Set the new image as the new source to the effect pipeline.
        const bool useGainMap = m_imageInfo.hasAppleHdrGainMap == true ||
            m_imageInfo.hasIsoHeicHdrGainMap == true ||
            m_imageInfo.hasAppleHeicHdrGainMap == true;

        if (m_imageInfo.isHeif)
        {
            m_loadedImage = m_imageLoader->GetLoadedImageSource(false);
            if (m_imageScaleEffect)
            {
                m_imageScaleEffect->SetInput(0, m_loadedImage.Get());
                m_imageScaleEffect->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(m_zoom, m_zoom));
                m_colorManagementEffect->SetInputEffect(0, m_imageScaleEffect.Get());
            }
            else
            {
                m_colorManagementEffect->SetInput(0, m_loadedImage.Get());
            }

            if (useGainMap)
            {
                m_loadedGainMap = m_imageLoader->GetLoadedImageSource(true);
                //m_gainmapLinearEffect->SetInput(0, m_loadedGainMap.Get());
                m_loadedMergedImage = m_imageLoader->GetMergedImageSource();

                if (m_imageScaleEffect)
                {
                    m_imageScaleEffect->SetInput(0, m_loadedMergedImage.Get());
                    m_imageScaleEffect->SetValue(D2D1_SCALE_PROP_SCALE, D2D1::Vector2F(m_zoom, m_zoom));
                    m_gainMapMergeEffect->SetInputEffect(0, m_imageScaleEffect.Get());
                }
                else
                {
                    m_gainMapMergeEffect->SetInput(0, m_loadedMergedImage.Get());
                }
            }
        }
        else
        {
            m_loadedImage = m_imageLoader->GetLoadedImage(m_zoom, false);
            if (m_loadedImage)
            {
                m_colorManagementEffect->SetInput(0, m_loadedImage.Get());
            }

            if (useGainMap)
            {
                m_loadedGainMap = m_imageLoader->GetLoadedImage(m_zoom, true);
                //m_gainmapLinearEffect->SetInput(0, m_loadedGainMap.Get());
                m_loadedMergedImage = m_imageLoader->GetMergedImage(m_zoom, false);
                if (m_loadedMergedImage)
                {
                    m_gainMapMergeEffect->SetInput(0, m_loadedMergedImage.Get());
                }
            }
        }
    }
}

// Uses a histogram to compute a modified version of maximum content light level/ST.2086 MaxCLL
// and average content light level.
// Performs Begin/EndDraw on the D2D context.
void HDRImageViewerRenderer::ComputeHdrMetadata()
{
    // Initialize with a sentinel value.
    m_imageCLL = { -1.0f, -1.0f, -1.0f, false };

    if (!m_isComputeSupported)
    {
        return;
    }

    // MaxCLL is nominally calculated for the single brightest pixel in a frame.
    // But we take a slightly more conservative definition that takes the 99.99th percentile
    // to account for extreme outliers in the image.
    float maxCLLPercent = 0.9999f;

    auto ctx = m_deviceResources->GetD2DDeviceContext();

    // Histogram rendering should always occur without DPI scaling
    ctx->SetDpi(96.0f, 96.0f);

    ctx->BeginDraw();

    ctx->DrawImage(m_histogramEffect.Get());

    // We ignore D2DERR_RECREATE_TARGET here. This error indicates that the device
    // is lost. It will be handled during the next call to Present.
    HRESULT hr = ctx->EndDraw();
    ctx->SetDpi(m_deviceResources->GetDpi(), m_deviceResources->GetDpi());
    if (hr != D2DERR_RECREATE_TARGET)
    {
        IFT(hr);
    }

    float *histogramData = new float[sc_histNumBins];
    IFT(m_histogramEffect->GetValue(D2D1_HISTOGRAM_PROP_HISTOGRAM_OUTPUT,
            reinterpret_cast<BYTE*>(histogramData),
            sc_histNumBins * sizeof(float)
            )
        );

    unsigned int maxCLLbin = 0;
    unsigned int avgCLLbin = 0; // Average is defined as 50th percentile.
    float runningSum = 0.0f; // Cumulative sum of values in histogram is 1.0.
    for (int i = sc_histNumBins - 1; i >= 0; i--)
    {
        runningSum += histogramData[i];

        // Note the inequality (<) is the opposite of the next if block.
        if (runningSum < 1.0f - maxCLLPercent)
        {
            maxCLLbin = i;
        }

        if (runningSum > 0.5f)
        {
            // Note if the entire histogram is 0, avgCLLbin remains at -1.
            avgCLLbin = i;
            break;
        }
    }

    float binNormMax = static_cast<float>(maxCLLbin) / static_cast<float>(sc_histNumBins);
    float maxNitsScene = powf(binNormMax, 1 / sc_histGamma) * sc_histMaxNits;

    float binNormAvg = static_cast<float>(avgCLLbin) / static_cast<float>(sc_histNumBins);
    float medianNitsScene = powf(binNormAvg, 1 / sc_histGamma) * sc_histMaxNits;

    // Some drivers have a bug where histogram will always return 0. Or some images are pure black.
    // Treat these cases as unknown.
    if (maxNitsScene == 0.0f)
    {
        m_imageCLL = { -1.0f, -1.0f, -1.0f, false };
        return;
    }

    // 1.0 in scRGB corresponds to D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL nits.
    m_imageCLL.maxNits255 = maxNitsScene * (203.0f / D2D1_SCENE_REFERRED_SDR_WHITE_LEVEL);

    // HDR metadata is not meaningful for SDR or WCG images.
    if (m_imageInfo.imageKind == AdvancedColorKind::HighDynamicRange)
    {
        m_imageCLL.maxNits = maxNitsScene;
        m_imageCLL.medianNits = medianNitsScene;

        // Certain HDR image types use recovered luminance and therefore are display/output-referred.
        // You can't interpret the histogram for these images as physical nits; they are only useful
        // to understand relative intensity.
        if (m_imageInfo.hasAppleHdrGainMap == true || m_imageInfo.hasIsoHeicHdrGainMap == true || m_imageInfo.hasAppleHeicHdrGainMap == true)
        {
            m_imageCLL.isSceneReferred = false;
        }
        else
        {
            m_imageCLL.isSceneReferred = true;
        }
    }

    // HDR metadata computation is completed before the app rendering options are known, so don't
    // attempt to draw yet.
}

// Set HDR10 metadata to allow HDR displays to optimize behavior based on our content.
void HDRImageViewerRenderer::EmitHdrMetadata()
{
    // PC apps generally should not use HDR ST.2086 metadata.
    return;
}

// If AdvancedColorInfo does not have valid data, picks an appropriate default value,
// or the manually overridden value.
float HDRImageViewerRenderer::GetBestDispMaxLuminance()
{
    float val = m_dispInfo ? m_dispInfo->MaxLuminanceInNits : 0.0f;
    auto acKind = m_dispInfo ? m_dispInfo->CurrentAdvancedColorKind : AdvancedColorKind::HighDynamicRange;

    if (m_dispMaxCLLOverride != 0.0f)
    {
        val = m_dispMaxCLLOverride;
    }

    if (val == 0.0f)
    {
        if (acKind == AdvancedColorKind::HighDynamicRange)
        {
            // HDR TVs generally don't report metadata, but monitors do.
            val = sc_DefaultHdrDispMaxNits;
        }
        else
        {
            // Almost no SDR displays report HDR metadata. WCG displays generally should report HDR metadata.
            // We assume both SDR and WCG displays have similar peak luminances and use the same constants.
            val = sc_DefaultSdrDispMaxNits;
        }
    }

    return val;
}

// Renders the loaded image with user-specified options.
void HDRImageViewerRenderer::Draw()
{
    auto d2dContext = m_deviceResources->GetD2DDeviceContext();

    d2dContext->BeginDraw();

    d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    d2dContext->SetTransform(m_deviceResources->GetOrientationTransform2D());

    if (m_loadedImage)
    {
        d2dContext->DrawImage(m_finalOutput.Get(), m_imageOffset);

        EmitHdrMetadata();
    }

    // We ignore D2DERR_RECREATE_TARGET here. This error indicates that the device
    // is lost. It will be handled during the next call to Present.
    HRESULT hr = d2dContext->EndDraw();
    if (hr != D2DERR_RECREATE_TARGET)
    {
        IFT(hr);
    }

    m_deviceResources->Present();
}

// Notifies renderers that device resources need to be released.
void HDRImageViewerRenderer::OnDeviceLost()
{
    ReleaseImageDependentResources();
    ReleaseDeviceDependentResources();
}

// Notifies renderers that device resources may now be recreated.
void HDRImageViewerRenderer::OnDeviceRestored()
{
    CreateDeviceDependentResources();
    CreateImageDependentResources();
    CreateWindowSizeDependentResources();

    SetRenderOptions(m_renderEffectKind, m_exposureAdjust, m_dispMaxCLLOverride, m_dispInfo, m_constrainGamut);

    Draw();
}

void HDRImageViewerRenderer::startEncodeISOJpeg() {
    m_imageLoader->generateEncodeSDRimage();

    if(m_imageLoader->exif_result.has_exif) {
        jpegR::parse_result.exif_ptr = m_imageLoader->exif_result.exif_ptr;
        jpegR::parse_result.exif_size = m_imageLoader->exif_result.exif_size;
        jpegR::parse_result.has_exif = m_imageLoader->exif_result.has_exif;
        jpegR::parse_result.exif_pos = m_imageLoader->exif_result.exif_pos;
    }

    jpegR::encodeISOJpeg(
        m_imageInfo.pixelSize.Width,
        m_imageInfo.pixelSize.Height,
        m_imageLoader->sdrData_changed,
        m_imageLoader->gainmapData,
        10.f,
        1.f,
        1.f,
        1.f / 64.f,
        1.f / 64.f,
        1.f,
        10.f
    );

    outputImage = jpegR::output;
}

void jpegR::encodeISOJpeg(
    int width,
    int height,
    std::vector<BYTE> sdrImage,   // Replaces sdrData + sdrSize.
    std::vector<BYTE> gainmapImage, // Replaces gainmapData + gainmapSize.
    float maxContentBoost,
    float minContentBoost,
    float gamma,
    float offsetSdr,
    float offsetHdr,
    float hdrCapacityMin,
    float hdrCapacityMax)
{
    // 1) Pack the SDR and gainmap byte streams for UltraHDR.
    jpegr_compressed_struct jpgSdr = {
        /* data     */ (void*)sdrImage.data(),
        /* length   */ sdrImage.size(),         // Use size() for length.
        /* maxLength*/ sdrImage.size(),
        /* colorGamut */ ULTRAHDR_COLORGAMUT_P3
    };

    jpegr_compressed_struct jpgGainmap = {
        (void*)gainmapImage.data(),  // Use data() for pointer.
        gainmapImage.size(),         // Use size() for length.
        gainmapImage.size(),         // Use size() for length.
        ULTRAHDR_COLORGAMUT_P3
    };

    // 2) generate metadata
    uhdr_gainmap_metadata_ext metadata;
    metadata.version = "1.0";
    for (int i = 0; i < 3; ++i) {
        metadata.max_content_boost[i] = maxContentBoost;
        metadata.min_content_boost[i] = minContentBoost;
        metadata.gamma[i] = gamma;
        metadata.offset_sdr[i] = offsetSdr;
        metadata.offset_hdr[i] = offsetHdr;
    }
    metadata.hdr_capacity_min = hdrCapacityMin;
    metadata.hdr_capacity_max = hdrCapacityMax;
    metadata.use_base_cg = 1;  // Use base image color gamut.

    size_t maxOutSize = (size_t)width * height * 3; // Worst-case output size.
    uhdr_compressed_image jpgOut;
    std::memset(&jpgOut, 0, sizeof(jpgOut));
    jpgOut.data = malloc(maxOutSize);
    jpgOut.capacity = maxOutSize;
    jpgOut.data_sz = 0;
    jpgOut.cg = UHDR_CG_DISPLAY_P3;
    

    appendGainMap(reinterpret_cast<uhdr_compressed_image_t*>(&jpgSdr), reinterpret_cast<uhdr_compressed_image_t*>(&jpgGainmap), /* exif */ nullptr,
        /* icc */ nullptr, /* icc size */ 0, &metadata, reinterpret_cast<uhdr_compressed_image_t*>(&jpgOut));

    // 5) Copy output data.
    /*std::vector<BYTE> output;*/
    output.resize(jpgOut.data_sz);
    std::memcpy(output.data(), jpgOut.data, jpgOut.data_sz);

    // 6) Release temp buffer.
    free(jpgOut.data);
}

jpegR::uhdr_error_info_t jpegR::appendGainMap(uhdr_compressed_image_t* sdr_intent_compressed,
    uhdr_compressed_image_t* gainmap_compressed,
    uhdr_mem_block_t* pExif, void* pIcc, size_t icc_size,
    uhdr_gainmap_metadata_ext_t* metadata,
    uhdr_compressed_image_t* dest) {
    if (kWriteXmpMetadata && !metadata->use_base_cg) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_UNSUPPORTED_FEATURE;
        status.has_detail = 1;
        snprintf(
            status.detail, sizeof status.detail,
            "setting gainmap application space as alternate image space in xmp mode is not supported");
        return status;
    }

    if (kWriteXmpMetadata && !metadata->are_all_channels_identical()) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_UNSUPPORTED_FEATURE;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
            "signalling multichannel gainmap metadata in xmp mode is not supported");
        return status;
    }

    const size_t xmpNameSpaceLength = kXmpNameSpace.size() + 1;  // need to count the null terminator
    const size_t isoNameSpaceLength = kIsoNameSpace.size() + 1;  // need to count the null terminator

    /////////////////////////////////////////////////////////////////////////////////////////////////
    // calculate secondary image length first, because the length will be written into the primary //
    // image xmp                                                                                   //
    /////////////////////////////////////////////////////////////////////////////////////////////////

    // XMP
    string xmp_secondary;
    size_t xmp_secondary_length;
    if (kWriteXmpMetadata) {
        xmp_secondary = generateXmpForSecondaryImage(*metadata);
        // xmp_secondary_length = 2 bytes representing the length of the package +
        //  + xmpNameSpaceLength = 29 bytes length
        //  + length of xmp packet = xmp_secondary.size()
        xmp_secondary_length = 2 + xmpNameSpaceLength + xmp_secondary.size();
    }

    // ISO
    uhdr_gainmap_metadata_frac iso_secondary_metadata;
    std::vector<uint8_t> iso_secondary_data;
    size_t iso_secondary_length;
    if (kWriteIso21496_1Metadata) {
        UHDR_ERR_CHECK(gainmapMetadataFloatToFraction(
            metadata, &iso_secondary_metadata));

        UHDR_ERR_CHECK(encodeGainmapMetadata(&iso_secondary_metadata,
            iso_secondary_data));
        // iso_secondary_length = 2 bytes representing the length of the package +
        //  + isoNameSpaceLength = 28 bytes length
        //  + length of iso metadata packet = iso_secondary_data.size()
        iso_secondary_length = 2 + isoNameSpaceLength + iso_secondary_data.size();
    }

    size_t secondary_image_size = gainmap_compressed->data_sz;
    if (kWriteXmpMetadata) {
        secondary_image_size += 2 /* 2 bytes length of APP1 sign */ + xmp_secondary_length;
    }
    if (kWriteIso21496_1Metadata) {
        secondary_image_size += 2 /* 2 bytes length of APP2 sign */ + iso_secondary_length;
    }

    // Check if EXIF package presents in the JPEG input.
  // If so, extract and remove the EXIF package.
   /* JpegDecoderHelper decoder;
    UHDR_ERR_CHECK(decoder.parseImage(sdr_intent_compressed->data, sdr_intent_compressed->data_sz));*/

    uhdr_mem_block_t exif_from_jpg;
    exif_from_jpg.data = nullptr;
    exif_from_jpg.data_sz = 0;

    parse_result.new_jpg_image.data = nullptr;
    parse_result.new_jpg_image.data_sz = 0;
    parse_result.new_jpg_image.capacity = 0;
    parse_result.new_jpg_image.cg = UHDR_CG_UNSPECIFIED;
    parse_result.new_jpg_image.ct = UHDR_CT_UNSPECIFIED;
    parse_result.new_jpg_image.range = UHDR_CR_UNSPECIFIED;

    if (!parse_image(sdr_intent_compressed, &parse_result)) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail, "Failed to parse JPEG image");
        return status;
    }

    //jpegR::parse_image(sdr_intent_compressed, &parse_result);



    std::unique_ptr<uint8_t[]> dest_data;
    /*if (decoder.getEXIFPos() >= 0) {*/
    if (parse_result.exif_ptr!=nullptr) {
        if (pExif != nullptr) {
            uhdr_error_info_t status;
            status.error_code = UHDR_CODEC_INVALID_PARAM;
            status.has_detail = 1;
            snprintf(status.detail, sizeof status.detail,
                "received exif from uhdr_enc_set_exif_data() while the base image intent already "
                "contains exif, unsure which one to use");
            return status;
        }
        //copyJpegWithoutExif(&new_jpg_image, sdr_intent_compressed, decoder.getEXIFPos(),
        //    decoder.getEXIFSize());
        dest_data.reset(reinterpret_cast<uint8_t*>(parse_result.new_jpg_image.data));
        //exif_from_jpg.data = decoder.getEXIFPtr();
        exif_from_jpg.data = parse_result.exif_ptr;
        //exif_from_jpg.data_sz = decoder.getEXIFSize();
        exif_from_jpg.data_sz = parse_result.exif_size;
        pExif = &exif_from_jpg;
    }

    uhdr_compressed_image_t* final_primary_jpg_image_ptr =
        parse_result.new_jpg_image.data_sz == 0 ? sdr_intent_compressed : &parse_result.new_jpg_image;

    size_t pos = 0;
    // Begin primary image
    // Write SOI
    UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
    UHDR_ERR_CHECK(Write(dest, &kSOI, 1, pos));

    // Write EXIF
    if (pExif != nullptr) {
        const size_t length = 2 + pExif->data_sz;
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP1, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, pExif->data, pExif->data_sz, pos));
    }

    // Prepare and write XMP
    if (kWriteXmpMetadata) {
        const string xmp_primary = generateXmpForPrimaryImage(secondary_image_size, *metadata);
        const size_t length = 2 + xmpNameSpaceLength + xmp_primary.size();
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP1, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)kXmpNameSpace.c_str(), xmpNameSpaceLength, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)xmp_primary.c_str(), xmp_primary.size(), pos));
    }

    // Write ICC
    if (pIcc != nullptr && icc_size > 0) {
        const size_t length = icc_size + 2;
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP2, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, pIcc, icc_size, pos));
    }

    // Prepare and write ISO 21496-1 metadata
    if (kWriteIso21496_1Metadata) {
        const size_t length = 2 + isoNameSpaceLength + 4;
        uint8_t zero = 0;
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP2, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)kIsoNameSpace.c_str(), isoNameSpaceLength, pos));
        UHDR_ERR_CHECK(Write(dest, &zero, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &zero, 1, pos));  // 2 bytes minimum_version: (00 00)
        UHDR_ERR_CHECK(Write(dest, &zero, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &zero, 1, pos));  // 2 bytes writer_version: (00 00)
    }

    // Prepare and write MPF
    {
        const size_t length = 2 + calculateMpfSize();
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        size_t primary_image_size = pos + length + final_primary_jpg_image_ptr->data_sz;
        // between APP2 + package size + signature
        // ff e2 00 58 4d 50 46 00
        // 2 + 2 + 4 = 8 (bytes)
        // and ff d8 sign of the secondary image
        size_t secondary_image_offset = primary_image_size - pos - 8;
        std::shared_ptr<DataStruct> mpf = generateMpf(primary_image_size, 0, /* primary_image_offset */
            secondary_image_size, secondary_image_offset);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP2, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)mpf->getData(), mpf->getLength(), pos));
    }

    // Write primary image
    UHDR_ERR_CHECK(Write(dest, (uint8_t*)final_primary_jpg_image_ptr->data + 2,
        final_primary_jpg_image_ptr->data_sz - 2, pos));
    // Finish primary image

    // Begin secondary image (gain map)
    // Write SOI
    UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
    UHDR_ERR_CHECK(Write(dest, &kSOI, 1, pos));

    // Prepare and write XMP
    if (kWriteXmpMetadata) {
        const size_t length = xmp_secondary_length;
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP1, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)kXmpNameSpace.c_str(), xmpNameSpaceLength, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)xmp_secondary.c_str(), xmp_secondary.size(), pos));
    }

    // Prepare and write ISO 21496-1 metadata
    if (kWriteIso21496_1Metadata) {
        const size_t length = iso_secondary_length;
        const uint8_t lengthH = ((length >> 8) & 0xff);
        const uint8_t lengthL = (length & 0xff);
        UHDR_ERR_CHECK(Write(dest, &kStart, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &kAPP2, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthH, 1, pos));
        UHDR_ERR_CHECK(Write(dest, &lengthL, 1, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)kIsoNameSpace.c_str(), isoNameSpaceLength, pos));
        UHDR_ERR_CHECK(Write(dest, (void*)iso_secondary_data.data(), iso_secondary_data.size(), pos));
    }

    // Write secondary image
    UHDR_ERR_CHECK(
        Write(dest, (uint8_t*)gainmap_compressed->data + 2, gainmap_compressed->data_sz - 2, pos));

    // Set back length
    dest->data_sz = pos;
 



    // Done!
    return g_no_error;
}


std::shared_ptr<DataStruct> jpegR::generateMpf(size_t primary_image_size, size_t primary_image_offset,
    size_t secondary_image_size,
    size_t secondary_image_offset) {
    size_t mpf_size = calculateMpfSize();
    std::shared_ptr<DataStruct> dataStruct = std::make_shared<DataStruct>(mpf_size);

    dataStruct->write(static_cast<const void*>(kMpfSig), sizeof(kMpfSig));
#if USE_BIG_ENDIAN_IN_MPF
    dataStruct->write(static_cast<const void*>(kMpBigEndian), kMpEndianSize);
#else
    dataStruct->write(static_cast<const void*>(kMpLittleEndian), kMpEndianSize);
#endif


    // Set the Index IFD offset be the position after the endianness value and this offset.
    constexpr uint32_t indexIfdOffset = static_cast<uint16_t>(kMpEndianSize + sizeof(kMpfSig));
    dataStruct->write32(Endian_SwapBE32(indexIfdOffset));

    // We will write 3 tags (version, number of images, MP entries).
    dataStruct->write16(Endian_SwapBE16(kTagSerializedCount));

    // Write the version tag.
    dataStruct->write16(Endian_SwapBE16(kVersionTag));
    dataStruct->write16(Endian_SwapBE16(kVersionType));
    dataStruct->write32(Endian_SwapBE32(kVersionCount));
    dataStruct->write(kVersionExpected, kVersionSize);

    // Write the number of images.
    dataStruct->write16(Endian_SwapBE16(kNumberOfImagesTag));
    dataStruct->write16(Endian_SwapBE16(kNumberOfImagesType));
    dataStruct->write32(Endian_SwapBE32(kNumberOfImagesCount));
    dataStruct->write32(Endian_SwapBE32(kNumPictures));

    // Write the MP entries.
    dataStruct->write16(Endian_SwapBE16(kMPEntryTag));
    dataStruct->write16(Endian_SwapBE16(kMPEntryType));
    dataStruct->write32(Endian_SwapBE32(kMPEntrySize * kNumPictures));
    const uint32_t mpEntryOffset =
        static_cast<uint32_t>(dataStruct->getBytesWritten() -  // The bytes written so far
            sizeof(kMpfSig) +                // Excluding the MPF signature
            sizeof(uint32_t) +               // The 4 bytes for this offset
            sizeof(uint32_t));  // The 4 bytes for the attribute IFD offset.
    dataStruct->write32(Endian_SwapBE32(mpEntryOffset));

    // Write the attribute IFD offset (zero because we don't write it).
    dataStruct->write32(0);

    // Write the MP entries for primary image
    dataStruct->write32(Endian_SwapBE32(kMPEntryAttributeFormatJpeg | kMPEntryAttributeTypePrimary));
    dataStruct->write32(Endian_SwapBE32(primary_image_size));
    dataStruct->write32(Endian_SwapBE32(primary_image_offset));
    dataStruct->write16(0);
    dataStruct->write16(0);

    // Write the MP entries for secondary image
    dataStruct->write32(Endian_SwapBE32(kMPEntryAttributeFormatJpeg));
    dataStruct->write32(Endian_SwapBE32(secondary_image_size));
    dataStruct->write32(Endian_SwapBE32(secondary_image_offset));
    dataStruct->write16(0);
    dataStruct->write16(0);

    return dataStruct;
}

string jpegR::generateXmpForPrimaryImage(size_t secondary_image_length,
    uhdr_gainmap_metadata_ext_t& metadata) {
    const vector<string> kConDirSeq({ kConDirectory, string("rdf:Seq") });
    const vector<string> kLiItem({ string("rdf:li"), kConItem });

    std::stringstream ss;
    photos_editing_formats::image_io::XmlWriter writer(ss);
    writer.StartWritingElement("x:xmpmeta");
    writer.WriteXmlns("x", "adobe:ns:meta/");
    writer.WriteAttributeNameAndValue("x:xmptk", "Adobe XMP Core 5.1.2");
    writer.StartWritingElement("rdf:RDF");
    writer.WriteXmlns("rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
    writer.StartWritingElement("rdf:Description");
    writer.WriteXmlns(kContainerPrefix, kContainerUri);
    writer.WriteXmlns(kItemPrefix, kItemUri);
    writer.WriteXmlns(kGainMapPrefix, kGainMapUri);
    writer.WriteAttributeNameAndValue(kMapVersion, metadata.version);

    writer.StartWritingElements(kConDirSeq);

    size_t item_depth = writer.StartWritingElement("rdf:li");
    writer.WriteAttributeNameAndValue("rdf:parseType", "Resource");
    writer.StartWritingElement(kConItem);
    writer.WriteAttributeNameAndValue(kItemSemantic, kSemanticPrimary);
    writer.WriteAttributeNameAndValue(kItemMime, kMimeImageJpeg);
    writer.FinishWritingElementsToDepth(item_depth);

    writer.StartWritingElement("rdf:li");
    writer.WriteAttributeNameAndValue("rdf:parseType", "Resource");
    writer.StartWritingElement(kConItem);
    writer.WriteAttributeNameAndValue(kItemSemantic, kSemanticGainMap);
    writer.WriteAttributeNameAndValue(kItemMime, kMimeImageJpeg);
    writer.WriteAttributeNameAndValue(kItemLength, secondary_image_length);

    writer.FinishWriting();

    return ss.str();
}

bool jpegR::parse_image(uhdr_compressed_image_t* source_image, ParseResult* result)
{
    // Reset output fields if needed.
    //result->exif_ptr = nullptr;
    //result->exif_size = 0;

    // Validate input data.
    if (!source_image || !source_image->data || source_image->data_sz < 4) {
        std::cerr << "Invalid input data." << std::endl;
        return false;
    }

    // Correctly get data pointer.
    BYTE* bytes = static_cast<BYTE*>(source_image->data);
    const size_t data_sz = source_image->data_sz;

    size_t exif_pos = -1;
    size_t found_exif_size = 0;
    bool has_exif = false;

    if (parse_result.has_exif) {
        result->exif_ptr = parse_result.exif_ptr;
        result->exif_size = parse_result.exif_size;
        found_exif_size = parse_result.exif_size;
        exif_pos = parse_result.exif_pos;
    }

    // Initialize output image.
    memset(&result->new_jpg_image, 0, sizeof(uhdr_compressed_image_t));

    // Create a JPEG buffer without EXIF.
    result->new_jpg_image.data = malloc(data_sz);
    if (!result->new_jpg_image.data) {
        std::cerr << "Allocation failed: " << data_sz << " bytes" << std::endl;
        return false;
    }
    memcpy(result->new_jpg_image.data, source_image->data, data_sz);
    result->new_jpg_image.data_sz = data_sz;
    result->new_jpg_image.capacity = data_sz;
   
    //if (parse_result.has_exif) {
    //    result->new_jpg_image.data = malloc(data_sz);
    //    if (!result->new_jpg_image.data) {
    //        std::cerr << "Allocation failed: " << data_sz << " bytes" << std::endl;

    //        return false;
    //    }
    //    memcpy(result->new_jpg_image.data, source_image->data, data_sz);
    //    result->new_jpg_image.data_sz = data_sz;
    //    result->new_jpg_image.capacity = data_sz;
    //}
    //else {
    //    // No EXIF; copy the original data.
    //    result->new_jpg_image.data = malloc(data_sz);
    //    if (!result->new_jpg_image.data) {
    //        std::cerr << "Allocation failed: " << data_sz << " bytes" << std::endl;
    //        return false;
    //    }
    //    memcpy(result->new_jpg_image.data, source_image->data, data_sz);
    //    result->new_jpg_image.data_sz = data_sz;
    //    result->new_jpg_image.capacity = data_sz;
    //}

    // Copy metadata fields.
    result->new_jpg_image.cg = source_image->cg;
    result->new_jpg_image.ct = source_image->ct;
    result->new_jpg_image.range = source_image->range;

    return true;
}

std::string jpegR::generateXmpForSecondaryImage(uhdr_gainmap_metadata_ext_t& metadata) {
    const vector<string> kConDirSeq({ kConDirectory, string("rdf:Seq") });

    std::stringstream ss;
    photos_editing_formats::image_io::XmlWriter writer(ss);
    writer.StartWritingElement("x:xmpmeta");
    writer.WriteXmlns("x", "adobe:ns:meta/");
    writer.WriteAttributeNameAndValue("x:xmptk", "Adobe XMP Core 5.1.2");
    writer.StartWritingElement("rdf:RDF");
    writer.WriteXmlns("rdf", "http://www.w3.org/1999/02/22-rdf-syntax-ns#");
    writer.StartWritingElement("rdf:Description");
    writer.WriteXmlns(kGainMapPrefix, kGainMapUri);
    writer.WriteAttributeNameAndValue(kMapVersion, metadata.version);
    writer.WriteAttributeNameAndValue(kMapGainMapMin, log2(metadata.min_content_boost[0]));
    writer.WriteAttributeNameAndValue(kMapGainMapMax, log2(metadata.max_content_boost[0]));
    writer.WriteAttributeNameAndValue(kMapGamma, metadata.gamma[0]);
    writer.WriteAttributeNameAndValue(kMapOffsetSdr, metadata.offset_sdr[0]);
    writer.WriteAttributeNameAndValue(kMapOffsetHdr, metadata.offset_hdr[0]);
    writer.WriteAttributeNameAndValue(kMapHDRCapacityMin, log2(metadata.hdr_capacity_min));
    writer.WriteAttributeNameAndValue(kMapHDRCapacityMax, log2(metadata.hdr_capacity_max));
    writer.WriteAttributeNameAndValue(kMapBaseRenditionIsHDR, "False");
    writer.FinishWriting();

    return ss.str();
}


uhdr_error_info_t jpegR::encodeGainmapMetadata(
    const uhdr_gainmap_metadata_frac* in_metadata, std::vector<uint8_t>& out_data) {
    if (in_metadata == nullptr) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
            "received nullptr for gain map metadata descriptor");
        return status;
    }

    const uint16_t min_version = 0, writer_version = 0;
    streamWriteU16(out_data, min_version);
    streamWriteU16(out_data, writer_version);

    uint8_t flags = 0u;
    // Always write three channels for now for simplicity.
    // TODO(maryla): the draft says that this specifies the count of channels of the
    // gain map. But tone mapping is done in RGB space so there are always three
    // channels, even if the gain map is grayscale. Should this be revised?
    const uint8_t channelCount = in_metadata->allChannelsIdentical() ? 1u : 3u;

    if (channelCount == 3) {
        flags |= kIsMultiChannelMask;
    }
    if (in_metadata->useBaseColorSpace) {
        flags |= kUseBaseColorSpaceMask;
    }
    if (in_metadata->backwardDirection) {
        flags |= 4;
    }

    const uint32_t denom = in_metadata->baseHdrHeadroomD;
    bool useCommonDenominator = true;
    if (in_metadata->baseHdrHeadroomD != denom || in_metadata->alternateHdrHeadroomD != denom) {
        useCommonDenominator = false;
    }
    for (int c = 0; c < channelCount; ++c) {
        if (in_metadata->gainMapMinD[c] != denom || in_metadata->gainMapMaxD[c] != denom ||
            in_metadata->gainMapGammaD[c] != denom || in_metadata->baseOffsetD[c] != denom ||
            in_metadata->alternateOffsetD[c] != denom) {
            useCommonDenominator = false;
        }
    }
    if (useCommonDenominator) {
        flags |= 8;
    }
    streamWriteU8(out_data, flags);

    if (useCommonDenominator) {
        streamWriteU32(out_data, denom);
        streamWriteU32(out_data, in_metadata->baseHdrHeadroomN);
        streamWriteU32(out_data, in_metadata->alternateHdrHeadroomN);
        for (int c = 0; c < channelCount; ++c) {
            streamWriteS32(out_data, in_metadata->gainMapMinN[c]);
            streamWriteS32(out_data, in_metadata->gainMapMaxN[c]);
            streamWriteU32(out_data, in_metadata->gainMapGammaN[c]);
            streamWriteS32(out_data, in_metadata->baseOffsetN[c]);
            streamWriteS32(out_data, in_metadata->alternateOffsetN[c]);
        }
    }
    else {
        streamWriteU32(out_data, in_metadata->baseHdrHeadroomN);
        streamWriteU32(out_data, in_metadata->baseHdrHeadroomD);
        streamWriteU32(out_data, in_metadata->alternateHdrHeadroomN);
        streamWriteU32(out_data, in_metadata->alternateHdrHeadroomD);
        for (int c = 0; c < channelCount; ++c) {
            streamWriteS32(out_data, in_metadata->gainMapMinN[c]);
            streamWriteU32(out_data, in_metadata->gainMapMinD[c]);
            streamWriteS32(out_data, in_metadata->gainMapMaxN[c]);
            streamWriteU32(out_data, in_metadata->gainMapMaxD[c]);
            streamWriteU32(out_data, in_metadata->gainMapGammaN[c]);
            streamWriteU32(out_data, in_metadata->gainMapGammaD[c]);
            streamWriteS32(out_data, in_metadata->baseOffsetN[c]);
            streamWriteU32(out_data, in_metadata->baseOffsetD[c]);
            streamWriteS32(out_data, in_metadata->alternateOffsetN[c]);
            streamWriteU32(out_data, in_metadata->alternateOffsetD[c]);
        }
    }

    return g_no_error;
}

jpegR::uhdr_error_info_t jpegR::gainmapMetadataFloatToFraction(
    const jpegR::uhdr_gainmap_metadata_ext_t* from, jpegR::uhdr_gainmap_metadata_frac* to) {
    if (from == nullptr || to == nullptr) {
        jpegR::uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_INVALID_PARAM;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
            "received nullptr for gain map metadata descriptor");
        return status;
    }

    to->backwardDirection = false;
    to->useBaseColorSpace = from->use_base_cg;

#define CONVERT_FLT_TO_UNSIGNED_FRACTION(flt, numerator, denominator)                          \
  if (!floatToUnsignedFraction(flt, numerator, denominator)) {                                 \
    uhdr_error_info_t status;                                                                  \
    status.error_code = UHDR_CODEC_INVALID_PARAM;                                              \
    status.has_detail = 1;                                                                     \
    snprintf(status.detail, sizeof status.detail,                                              \
             "encountered error while representing float %f as a rational number (p/q form) ", \
             flt);                                                                             \
    return status;                                                                             \
  }

#define CONVERT_FLT_TO_SIGNED_FRACTION(flt, numerator, denominator)                            \
  if (!floatToSignedFraction(flt, numerator, denominator)) {                                   \
    uhdr_error_info_t status;                                                                  \
    status.error_code = UHDR_CODEC_INVALID_PARAM;                                              \
    status.has_detail = 1;                                                                     \
    snprintf(status.detail, sizeof status.detail,                                              \
             "encountered error while representing float %f as a rational number (p/q form) ", \
             flt);                                                                             \
    return status;                                                                             \
  }


    bool isSingleChannel = from->are_all_channels_identical();
    for (int i = 0; i < (isSingleChannel ? 1 : 3); i++) {
        CONVERT_FLT_TO_SIGNED_FRACTION(log2(from->max_content_boost[i]), &to->gainMapMaxN[i],
            &to->gainMapMaxD[i])

            CONVERT_FLT_TO_SIGNED_FRACTION(log2(from->min_content_boost[i]), &to->gainMapMinN[i],
                &to->gainMapMinD[i]);

        CONVERT_FLT_TO_UNSIGNED_FRACTION(from->gamma[i], &to->gainMapGammaN[i], &to->gainMapGammaD[i]);

        CONVERT_FLT_TO_SIGNED_FRACTION(from->offset_sdr[i], &to->baseOffsetN[i], &to->baseOffsetD[i]);

        CONVERT_FLT_TO_SIGNED_FRACTION(from->offset_hdr[i], &to->alternateOffsetN[i],
            &to->alternateOffsetD[i]);
    }

    if (isSingleChannel) {
        to->gainMapMaxN[2] = to->gainMapMaxN[1] = to->gainMapMaxN[0];
        to->gainMapMaxD[2] = to->gainMapMaxD[1] = to->gainMapMaxD[0];

        to->gainMapMinN[2] = to->gainMapMinN[1] = to->gainMapMinN[0];
        to->gainMapMinD[2] = to->gainMapMinD[1] = to->gainMapMinD[0];

        to->gainMapGammaN[2] = to->gainMapGammaN[1] = to->gainMapGammaN[0];
        to->gainMapGammaD[2] = to->gainMapGammaD[1] = to->gainMapGammaD[0];

        to->baseOffsetN[2] = to->baseOffsetN[1] = to->baseOffsetN[0];
        to->baseOffsetD[2] = to->baseOffsetD[1] = to->baseOffsetD[0];

        to->alternateOffsetN[2] = to->alternateOffsetN[1] = to->alternateOffsetN[0];
        to->alternateOffsetD[2] = to->alternateOffsetD[1] = to->alternateOffsetD[0];
    }

    CONVERT_FLT_TO_UNSIGNED_FRACTION(log2(from->hdr_capacity_min), &to->baseHdrHeadroomN,
        &to->baseHdrHeadroomD);

    CONVERT_FLT_TO_UNSIGNED_FRACTION(log2(from->hdr_capacity_max), &to->alternateHdrHeadroomN,
        &to->alternateHdrHeadroomD);

    return g_no_error;
}

bool jpegR::floatToSignedFraction(float v, int32_t* numerator, uint32_t* denominator) {
    uint32_t positive_numerator;
    if (!floatToUnsignedFractionImpl(fabs(v), INT32_MAX, &positive_numerator, denominator)) {
        return false;
    }
    *numerator = (int32_t)positive_numerator;
    if (v < 0) {
        *numerator *= -1;
    }
    return true;
}

static bool jpegR::floatToUnsignedFractionImpl(float v, uint32_t maxNumerator, uint32_t* numerator,
    uint32_t* denominator) {
    if (std::isnan(v) || v < 0 || v > maxNumerator) {
        return false;
    }

    // Maximum denominator: makes sure that the numerator is <= maxNumerator and the denominator
    // is <= UINT32_MAX.
    const uint64_t maxD = (v <= 1) ? UINT32_MAX : (uint64_t)floor(maxNumerator / v);

    // Find the best approximation of v as a fraction using continued fractions, see
    // https://en.wikipedia.org/wiki/Continued_fraction
    *denominator = 1;
    uint32_t previousD = 0;
    double currentV = (double)v - floor(v);
    int iter = 0;
    // Set a maximum number of iterations to be safe. Most numbers should
    // converge in less than ~20 iterations.
    // The golden ratio is the worst case and takes 39 iterations.
    const int maxIter = 39;
    while (iter < maxIter) {
        const double numeratorDouble = (double)(*denominator) * v;
        if (numeratorDouble > maxNumerator) {
            return false;
        }
        *numerator = (uint32_t)round(numeratorDouble);
        if (fabs(numeratorDouble - (*numerator)) == 0.0) {
            return true;
        }
        currentV = 1.0 / currentV;
        const double newD = previousD + floor(currentV) * (*denominator);
        if (newD > maxD) {
            // This is the best we can do with a denominator <= max_d.
            return true;
        }
        previousD = *denominator;
        if (newD > (double)UINT32_MAX) {
            return false;
        }
        *denominator = (uint32_t)newD;
        currentV -= floor(currentV);
        ++iter;
    }
    // Maximum number of iterations reached, return what we've found.
    // For max_iter >= 39 we shouldn't get here. max_iter can be set
    // to a lower value to speed up the algorithm if needed.
    *numerator = (uint32_t)round((double)(*denominator) * v);
    return true;
}

bool jpegR::floatToUnsignedFraction(float v, uint32_t* numerator, uint32_t* denominator) {
    return floatToUnsignedFractionImpl(v, UINT32_MAX, numerator, denominator);
}

uhdr_error_info_t jpegR::Write(uhdr_compressed_image_t* destination, const void* source, size_t length,
    size_t& position) {
    if (position + length > destination->capacity) {
        uhdr_error_info_t status;
        status.error_code = UHDR_CODEC_MEM_ERROR;
        status.has_detail = 1;
        snprintf(status.detail, sizeof status.detail,
            "output buffer to store compressed data is too small: write position: %zd, size: %zd, "
            "capacity: %zd",
            position, length, destination->capacity);
        return status;
    }

    memcpy((uint8_t*)destination->data + sizeof(uint8_t) * position, source, length);
    position += length;
    return g_no_error;
}

DataStruct::DataStruct(size_t s) {
    data = malloc(s);
    length = s;
    memset(data, 0, s);
    writePos = 0;
}

DataStruct::~DataStruct() {
    if (data != nullptr) {
        free(data);
    }
}

void* DataStruct::getData() { return data; }

size_t DataStruct::getLength() { return length; }

size_t DataStruct::getBytesWritten() { return writePos; }

bool DataStruct::write8(uint8_t value) {
    uint8_t v = value;
    return write(&v, 1);
}

bool DataStruct::write16(uint16_t value) {
    uint16_t v = value;
    return write(&v, 2);
}

bool DataStruct::write32(uint32_t value) {
    uint32_t v = value;
    return write(&v, 4);
}

bool jpegR::DataStruct::write(const void* src, size_t size) {
    if (writePos + size > length) {
        ALOGE("Writing out of boundary: write position: %zd, size: %zd, capacity: %zd", writePos, size,
            length);
        return false;
    }
    memcpy((uint8_t*)data + writePos, src, size);
    writePos += size;
    return true;
}

namespace photos_editing_formats {
    namespace image_io {

        using std::ostream;
        using std::string;
        using std::vector;

        namespace {

            const char kXmlnsColon[] = "xmlns:";

        }  // namespace

        XmlWriter::XmlWriter(std::ostream& os)
            : os_(os), element_count_(0), quote_mark_('"') {
        }

        void XmlWriter::WriteXmlns(const string& prefix, const string& uri) {
            string name = string(kXmlnsColon) + prefix;
            WriteAttributeNameAndValue(name, uri, true);
        }

        size_t XmlWriter::StartWritingElement(const string& element_name) {
            MaybeWriteCloseBracket(true);
            size_t current_depth = element_data_.size();
            if (current_depth > 0) {
                element_data_.back().has_children = true;
            }
            element_data_.emplace_back(element_name);
            os_ << indent_ << "<" << element_name;
            indent_ += "  ";
            element_count_ += 1;
            return current_depth;
        }

        void XmlWriter::FinishWritingElement() {
            if (!element_data_.empty()) {
                if (indent_.size() >= 2) {
                    indent_.resize(indent_.size() - 2);
                }
                auto& data = element_data_.back();
                if (!data.has_content && !data.has_children) {
                    if (!data.has_attributes || data.has_children) {
                        os_ << indent_;
                    }
                    os_ << "/>" << std::endl;
                }
                else {
                    if (!data.has_content) {
                        os_ << indent_;
                    }
                    os_ << "</" << data.name << ">" << std::endl;
                }
                element_data_.pop_back();
            }
        }

        void XmlWriter::FinishWritingElementsToDepth(size_t depth) {
            if (!element_data_.empty()) {
                for (size_t index = element_data_.size(); index > depth; --index) {
                    FinishWritingElement();
                }
            }
        }

        size_t XmlWriter::StartWritingElements(const vector<string>& element_names) {
            size_t current_depth = element_data_.size();
            for (const auto& element_name : element_names) {
                StartWritingElement(element_name);
            }
            return current_depth;
        }

        void XmlWriter::WriteElementAndContent(const string& element_name,
            const string& content) {
            StartWritingElement(element_name);
            WriteContent(content);
            FinishWritingElement();
        }

        void XmlWriter::WriteContent(const string& content) {
            MaybeWriteCloseBracket(false);
            if (!element_data_.empty()) {
                auto& data = element_data_.back();
                data.has_content = true;
                os_ << content;
            }
        }

        void XmlWriter::WriteAttributeNameAndValue(const string& name,
            const string& value,
            bool add_quote_marks) {
            WriteAttributeName(name);
            WriteAttributeValue(add_quote_marks, value, add_quote_marks);
        }

        void XmlWriter::WriteAttributeName(const string& name) {
            if (!element_data_.empty()) {
                os_ << std::endl << indent_ << name << "=";
                element_data_.back().has_attributes = true;
            }
        }

        void XmlWriter::WriteAttributeValue(bool add_leading_quote_mark,
            const string& value,
            bool add_trailing_quote_mark) {
            if (!element_data_.empty()) {
                if (add_leading_quote_mark) os_ << quote_mark_;
                os_ << value;
                if (add_trailing_quote_mark) os_ << quote_mark_;
            }
        }

        void XmlWriter::WriteComment(const std::string& comment) {
            MaybeWriteCloseBracket(true);
            os_ << indent_ << "<!-- " << comment << " -->" << std::endl;
            if (!element_data_.empty()) {
                auto& data = element_data_.back();
                data.has_children = true;
            }
        }

        bool XmlWriter::MaybeWriteCloseBracket(bool with_trailing_newline) {
            if (!element_data_.empty()) {
                auto& data = element_data_.back();
                if (!data.has_content && !data.has_children) {
                    os_ << ">";
                    if (with_trailing_newline) {
                        os_ << std::endl;
                    }
                    return true;
                }
            }
            return false;
        }

    }  // namespace image_io
}  // namespace photos_editing_formats