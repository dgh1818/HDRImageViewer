#pragma once

namespace DXRenderer
{
    public value struct ImageInfo
    {
        unsigned int                                    bitsPerPixel;
        unsigned int                                    bitsPerChannel;
        bool                                            isFloat;
        Windows::Foundation::Size                       pixelSize;
        unsigned int                                    countColorProfiles;
        Windows::Graphics::Display::AdvancedColorKind   imageKind;
        bool                                            forceBT2100ColorSpace;
        bool                                            isValid;
        bool                                            isHeif;
        bool                                            hasAppleHdrGainMap;
        bool                                            hasIsoHeicHdrGainMap;
        bool                                            hasAppleHeicHdrGainMap;
        Windows::Foundation::Size                       gainMapPixelSize;
        bool                                            hasOverriddenColorProfile;
        bool                                            hasEXRChromaticitiesInfo;

        bool                                            hasCuvaHdrGainMap;
        bool                                            hasHuaweiIsoJpegHdrGainMap;
        bool                                            hasIsoJpegHdrGainMap;
        bool                                            hasHdrGainMapHeadroom;
        float                                           hdrGainMapHeadroom;
    };

    public value struct ImageCLL
    {
        float   maxNits;
        float   medianNits;
        float   maxNits255;
        bool    isSceneReferred; // If False, the CLL values are not calibrated to actual nits
                                 // should only be used to understand relative intensity of the image.
    };

}
