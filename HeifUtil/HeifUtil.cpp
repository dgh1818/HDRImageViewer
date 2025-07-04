// HeifUtil.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include "LibHeifHelpers.h"

int main(int argc, WCHAR* argv[])
{
    HRESULT hr = S_OK;
    heif_error herr = {};
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    PWSTR inputFilenameRaw = nullptr;
    PWSTR outputFilename = nullptr;
    std::wstring inputFilename;

    for (int i = 1; i < argc; ++i)
    {
        const WCHAR szInputArg[] = L"/input=";
        const UINT cchInputArg = ARRAYSIZE(szInputArg) - 1;
        if (!wcsncmp(szInputArg, argv[i], cchInputArg) && wcslen(argv[i]) > cchInputArg)
        {
            inputFilenameRaw = &argv[i][cchInputArg];

            inputFilename = std::wstring(inputFilenameRaw);
        }

        const WCHAR szOutputArg[] = L"/output=";
        const UINT cchOutputArg = ARRAYSIZE(szOutputArg) - 1;
        if (!wcsncmp(szOutputArg, argv[i], cchOutputArg) && wcslen(argv[i]) > cchOutputArg)
        {
            outputFilename = &argv[i][cchOutputArg];
        }
    }

    CComPtr<IWICImagingFactory2> fact;
    hr = CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&fact));

    DXRenderer::CHeifContext ctx;
    herr = heif_context_read_from_file(ctx.ptr, "input.heic", nullptr);

    DXRenderer::CHeifHandle mainHandle;
    herr = heif_context_get_primary_image_handle(ctx.ptr, &mainHandle.ptr);

    // Decode the main image into WIC. This code is currently not used.

    DXRenderer::CHeifImage mainImg;
    herr = heif_decode_image(mainHandle.ptr, &mainImg.ptr, heif_colorspace_RGB, heif_chroma_interleaved_RGB, 0);

    int mainWidth = heif_image_get_primary_width(mainImg.ptr);
    int mainHeight = heif_image_get_primary_height(mainImg.ptr);
    // Assuming 8bpc.

    int mainStride = 0;
    uint8_t* mainData = heif_image_get_plane(mainImg.ptr, heif_channel_interleaved, &mainStride);

    CComPtr<IWICBitmap> mainBmpRaw;
    hr = fact->CreateBitmapFromMemory(mainWidth, mainHeight, GUID_WICPixelFormat8bppGray, mainStride, mainStride * mainHeight, mainData, &mainBmpRaw);

    CComPtr<IWICBitmapScaler> scaler;
    hr = fact->CreateBitmapScaler(&scaler);
    hr = scaler->Initialize(mainBmpRaw, mainWidth / 2, mainHeight / 2, WICBitmapInterpolationModeLinear); // Filtering is good enough for exact 2x2 downscale.

    CComPtr<IWICFormatConverter> format;
    hr = fact->CreateFormatConverter(&format);
    hr = format->Initialize(scaler.p, GUID_WICPixelFormat8bppGray, WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);

    CComPtr<IWICBitmap> mainBitmap;
    hr = fact->CreateBitmapFromSource(format.p, WICBitmapCacheOnDemand, &mainBitmap);

    // Get all of the auxiliary images.

    int countAux = heif_image_handle_get_number_of_auxiliary_images(mainHandle.ptr, 0);
    std::vector<heif_item_id> auxIds(countAux);
    heif_image_handle_get_list_of_auxiliary_image_IDs(mainHandle.ptr, 0, auxIds.data(), auxIds.size());

    for (auto i : auxIds)
    {
        DXRenderer::CHeifHandle auxHandle;
        herr = heif_image_handle_get_auxiliary_image_handle(mainHandle.ptr, i, &auxHandle.ptr);

        DXRenderer::CHeifAuxType type;
        herr = heif_image_handle_get_auxiliary_type(auxHandle.ptr, &type.ptr);

        if (type.IsAppleHdrGainMap())
        {
            DXRenderer::CHeifImage img;
            herr = heif_decode_image(auxHandle.ptr, &img.ptr, heif_colorspace_monochrome, heif_chroma_monochrome, 0);

            int width = heif_image_get_primary_width(img.ptr);
            int height = heif_image_get_primary_height(img.ptr);
            int bitdepth = heif_image_get_bits_per_pixel_range(img.ptr, heif_channel_Y);

            int stride = 0;
            uint8_t* data = heif_image_get_plane(img.ptr, heif_channel_Y, &stride);

            CComPtr<IWICStream> stream;
            hr = fact->CreateStream(&stream);
            hr = stream->InitializeFromFilename(L"output.png", GENERIC_WRITE);

            CComPtr<IWICBitmap> bitmap;
            hr = fact->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat8bppGray, stride, stride * height, data, &bitmap);

            CComPtr<IWICBitmapEncoder> encoder;
            hr = fact->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            hr = encoder->Initialize(stream.p, WICBitmapEncoderNoCache);

            CComPtr<IWICBitmapFrameEncode> frame;
            hr = encoder->CreateNewFrame(&frame, nullptr);
            hr = frame->Initialize(nullptr);
            hr = frame->WriteSource(bitmap.p, nullptr);

            hr = frame->Commit();
            hr = encoder->Commit();
            hr = stream->Commit(STGC_DEFAULT);

            // Get main image pixels.. This code is currently unused.

            CComPtr<IWICBitmapLock> scaleLock;
            hr = mainBitmap->Lock(nullptr, GENERIC_READ, &scaleLock);
            UINT mainWidth = 0, mainHeight = 0, mainStride = 0;
            hr = scaleLock->GetSize(&mainWidth, &mainHeight);
            if ((mainWidth != width) || (mainHeight != height)) return -1; // Assert.

            hr = scaleLock->GetStride(&mainStride);
            
            std::vector<uint8_t> mainPixels(mainStride * mainHeight);

            // Immediately return once we have a gain map.
            return 0;
        }
    }

    return 0;
}