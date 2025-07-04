﻿using DXRenderer;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Windows.Foundation.Collections;
using Windows.Foundation.Metadata;
using Windows.Graphics.Display;
using Windows.Storage;
using Windows.Storage.AccessCache;
using Windows.Storage.Pickers;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Input;
using Windows.UI.ViewManagement;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Controls.Primitives;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Navigation;

namespace HDRImageViewerCS
{
    /// <summary>
    /// Passed by the app to a new DXViewerPage. Note: the defaults need to be sensible.
    /// </summary>
    public struct DXViewerLaunchArgs
    {
        public bool useFullscreen;
        public bool hideUI;
        public bool forceBT2100;
        public bool hasCustomColorSpace;
        public DXRenderer.CustomSdrColorSpace customColorSpace;
        public bool hasForcedEffect;
        public DXRenderer.RenderEffectKind forcedEffect;
        public string initialFileToken; // StorageItemAccessList token
        public ErrorDialogType errorType; // If this is not DefaultValue, triggers the error dialog.
        public string errorFilename; // Only use this if ErrorDialogType is InvalidFile.
        public string rawCommandLine;
    }

    /// <summary>
    /// An empty page that can be used on its own or navigated to within a Frame.
    /// </summary>
    public sealed partial class DXViewerPage : Page
    {
        HDRImageViewerRenderer renderer;
        GestureRecognizer gestureRecognizer;

        ImageInfo imageInfo;
        ImageCLL imageCLL;
        AdvancedColorInfo dispInfo;

        bool isImageValid;
        bool isWindowVisible;
        bool enableExperimentalTools;
        bool enableGamutMap;
        bool profileColorimetryOverride;
        ImageLoaderOptions loaderOptions;
        string commandLine;
        DXRenderer.RenderEffectKind? forcedEffect;

        ToolTip tooltip;
        RenderOptionsViewModel viewModel;
        public RenderOptionsViewModel ViewModel { get { return viewModel; } }

        public DXViewerPage()
        {
            this.InitializeComponent();

            // Suppress the tooltip created by keyboard accelerators.
            tooltip = new ToolTip();
            tooltip.Visibility = Visibility.Collapsed;
            ToolTipService.SetToolTip(this, tooltip);

            isWindowVisible = true;
            isImageValid = false;
            imageCLL.maxNits = imageCLL.medianNits = -1.0f;

            // Register event handlers for page lifecycle.
            var window = Window.Current.CoreWindow;

            window.VisibilityChanged += OnVisibilityChanged;
            window.ResizeCompleted += OnResizeCompleted;

            var currDispInfo = DisplayInformation.GetForCurrentView();

            currDispInfo.DpiChanged += OnDpiChanged;
            currDispInfo.OrientationChanged += OnOrientationChanged;
            DisplayInformation.DisplayContentsInvalidated += OnDisplayContentsInvalidated;

            currDispInfo.AdvancedColorInfoChanged += OnAdvancedColorInfoChanged;
            var acInfo = currDispInfo.GetAdvancedColorInfo();

            currDispInfo.ColorProfileChanged += OnColorProfileChanged;

            swapChainPanel.CompositionScaleChanged += OnCompositionScaleChanged;
            swapChainPanel.SizeChanged += OnSwapChainPanelSizeChanged;

            // Pointer and manipulation events handle image pan and zoom.
            swapChainPanel.PointerPressed += OnPointerPressed;
            swapChainPanel.PointerMoved += OnPointerMoved;
            swapChainPanel.PointerReleased += OnPointerReleased;
            swapChainPanel.PointerCanceled += OnPointerCanceled;
            swapChainPanel.PointerWheelChanged += OnPointerWheelChanged;

            gestureRecognizer = new GestureRecognizer();
            gestureRecognizer.ManipulationStarted += OnManipulationStarted;
            gestureRecognizer.ManipulationUpdated += OnManipulationUpdated;
            gestureRecognizer.ManipulationCompleted += OnManipulationCompleted;
            gestureRecognizer.GestureSettings =
                GestureSettings.ManipulationTranslateX |
                GestureSettings.ManipulationTranslateY |
                GestureSettings.ManipulationScale;

            viewModel = new RenderOptionsViewModel();

            // At this point we have access to the device and can create the device-dependent resources.
            renderer = new HDRImageViewerRenderer(swapChainPanel);

            UpdateDisplayACState(acInfo);
        }

        private void SetWindowTitle(string message)
        {
            if (ApplicationView.GetForCurrentView() == null) return;

            if (commandLine != null)
            {
                ApplicationView.GetForCurrentView().Title = message + " (" + commandLine + ")";
            }
            else
            {
                ApplicationView.GetForCurrentView().Title = message;
            }
        }

        protected async override void OnNavigatedTo(NavigationEventArgs e)
        {
            base.OnNavigatedTo(e);

            if (e.Parameter.GetType() == typeof(DXViewerLaunchArgs))
            {
                var args = (DXViewerLaunchArgs)e.Parameter;

                commandLine = args.rawCommandLine;

                SetWindowTitle("");

                if (args.hasCustomColorSpace)
                {
                    loaderOptions.type = ImageLoaderOptionsType.CustomSdrColorSpace;
                    loaderOptions.customColorSpace = args.customColorSpace;
                }

                // Force BT.2100 overrides any custom profile.
                if (args.forceBT2100)
                {
                    loaderOptions.type = ImageLoaderOptionsType.ForceBT2100;
                }

                if (args.hideUI)
                {
                    SetUIHidden(true);
                }

                if (args.useFullscreen)
                {
                    SetUIFullscreen(true);
                }

                if (args.initialFileToken != null)
                {
                    var file = await StorageApplicationPermissions.FutureAccessList.GetFileAsync(args.initialFileToken);
                    await LoadImageAsync(file);
                }

                // Startup effect needs to be set after the image is loaded in UpdateDefaultRenderOptions.
                if (args.hasForcedEffect)
                {
                    forcedEffect = args.forcedEffect;
                }

                if (args.errorType != ErrorDialogType.DefaultValue)
                {
                    var error = new ErrorContentDialog(args.errorType);

                    if (args.errorType.HasFlag(ErrorDialogType.InvalidFile))
                    {
                        error.Title = args.errorFilename;
                    }

                    await error.ShowAsync();
                }
            }
        }

        private void UpdateDisplayACState(AdvancedColorInfo newAcInfo)
        {
            AdvancedColorKind oldDispKind = AdvancedColorKind.StandardDynamicRange;
            if (dispInfo != null)
            {
                // dispInfo won't be available until the first image has been loaded.
                oldDispKind = dispInfo.CurrentAdvancedColorKind;
            }

            // TODO: Confirm that newAcInfo is never null. I believe this was needed in past versions for RS4 compat.
            dispInfo = newAcInfo;
            AdvancedColorKind newDispKind = dispInfo.CurrentAdvancedColorKind;
            DisplayACState.Text = UIStrings.LABEL_ACKIND + UIStrings.ConvertACKindToString(newDispKind);

            int maxcll = (int)dispInfo.MaxLuminanceInNits;

            if (maxcll == 0)
            {
                // Luminance value of 0 means that no valid data was provided by the display.
                DisplayPeakLuminance.Text = UIStrings.LABEL_PEAKLUMINANCE + UIStrings.LABEL_UNKNOWN;
            }
            else
            {
                DisplayPeakLuminance.Text = UIStrings.LABEL_PEAKLUMINANCE + maxcll.ToString() + UIStrings.LABEL_LUMINANCE_NITS;
            }

            if (oldDispKind == newDispKind)
            {
                // Some changes, such as peak luminance or SDR white level, don't need to reset rendering options.
                UpdateRenderOptions();
            }
            else
            {
                // If display has changed kind between SDR/HDR/WCG, we must reset all rendering options.
                UpdateDefaultRenderOptions();
            }
        }

        // Based on image and display parameters, choose the best rendering options.
        private void UpdateDefaultRenderOptions()
        {
            if (!isImageValid)
            {
                // Render options are only meaningful if an image is already loaded.
                return;
            }

            switch (imageInfo.imageKind)
            {
                case AdvancedColorKind.StandardDynamicRange:
                case AdvancedColorKind.WideColorGamut:
                default:
                    // SDR and WCG images don't need to be tonemapped.
                    RenderEffectCombo.SelectedIndex = 0; // See RenderOptions.h for which value this indicates.

                    // Manual exposure adjustment is only useful for HDR content.
                    // SDR and WCG content is adjusted by the OS-provided AdvancedColorInfo.SdrWhiteLevel parameter.
                    ExposureAdjustSlider.Value = SdrExposureFormatter.ExposureToSlider(1.0);
                    ExposureAdjustPanel.Visibility = Visibility.Collapsed;
                    break;

                case AdvancedColorKind.HighDynamicRange:
                    // HDR images need to be tonemapped regardless of display kind.
                    RenderEffectCombo.SelectedIndex = 0; // See RenderOptions.h for which value this indicates.

                    ExposureAdjustPanel.Visibility = Visibility.Visible;
                    break;
            }

            // Defer applying the render effect command line arg until the image is loaded.
            if (forcedEffect.HasValue)
            {
                switch (forcedEffect.Value)
                {
                    case RenderEffectKind.None:
                        RenderEffectCombo.SelectedIndex = 0; // See RenderOptions.h for which value this indicates.
                        break;

                    case RenderEffectKind.HdrTonemap:
                        RenderEffectCombo.SelectedIndex = 1; // See RenderOptions.h for which value this indicates.
                        break;

                    case RenderEffectKind.SdrOverlay:
                        RenderEffectCombo.SelectedIndex = 2; // See RenderOptions.h for which value this indicates.
                        break;

                    case RenderEffectKind.MaxLuminance:
                        RenderEffectCombo.SelectedIndex = 3; // See RenderOptions.h for which value this indicates.
                        break;

                    case RenderEffectKind.LuminanceHeatmap:
                        RenderEffectCombo.SelectedIndex = 4; // See RenderOptions.h for which value this indicates.
                        break;
                }

                // Prevent manually changing the effect.
                RenderEffectCombo.IsEnabled = false;
            }

            UpdateRenderOptions();
        }

        // Common method for updating options on the renderer.
        private void UpdateRenderOptions()
        {
            if ((renderer != null) && (RenderEffectCombo.SelectedItem != null))
            {
                var tm = (EffectOption)RenderEffectCombo.SelectedItem;

                var dispcll = enableExperimentalTools ? (float)DispMaxCLLOverrideSlider.Value : 0.0f;

                if(dispInfo.CurrentAdvancedColorKind == AdvancedColorKind.HighDynamicRange)
                {
                    dispcll = 1000;
                } else
                {
                    dispcll = dispInfo.MaxLuminanceInNits;
                }


                    renderer.SetRenderOptions(
                        tm.Kind,
                        (float)SdrExposureFormatter.SliderToExposure(ExposureAdjustSlider.Value),
                        dispcll, // Display MaxCLL override
                        dispInfo,
                        enableGamutMap
                        );
            }
        }

        // Swap chain event handlers.

        private void OnSwapChainPanelSizeChanged(object sender, SizeChangedEventArgs e)
        {
            renderer.SetLogicalSize(e.NewSize);
            renderer.CreateWindowSizeDependentResources();
            renderer.Draw();
        }

        private void OnCompositionScaleChanged(SwapChainPanel sender, object args)
        {
            renderer.SetCompositionScale(sender.CompositionScaleX, sender.CompositionScaleY);
            renderer.CreateWindowSizeDependentResources();
            renderer.Draw();
        }

        // Display state event handlers.
        private void OnColorProfileChanged(DisplayInformation sender, object args)
        {
            UpdateDisplayACState(sender.GetAdvancedColorInfo());
        }

        private void OnAdvancedColorInfoChanged(DisplayInformation sender, object args)
        {
            UpdateDisplayACState(sender.GetAdvancedColorInfo());
        }

        private void OnDisplayContentsInvalidated(DisplayInformation sender, object args)
        {
            renderer.ValidateDevice();
            renderer.CreateWindowSizeDependentResources();
            renderer.Draw();
        }

        private void OnOrientationChanged(DisplayInformation sender, object args)
        {
            renderer.SetCurrentOrientation(sender.CurrentOrientation);
            renderer.CreateWindowSizeDependentResources();
            renderer.Draw();
        }

        private void OnDpiChanged(DisplayInformation sender, object args)
        {
            renderer.SetDpi(sender.LogicalDpi);
            renderer.CreateWindowSizeDependentResources();
            renderer.Draw();
        }

        // Window event handlers.

        // ResizeCompleted is used to detect when the window has been moved between different displays.
        private void OnResizeCompleted(CoreWindow sender, object args)
        {
            UpdateRenderOptions();
        }

        private void OnVisibilityChanged(CoreWindow sender, VisibilityChangedEventArgs args)
        {
            isWindowVisible = args.Visible;
            if (isWindowVisible)
            {
                renderer.Draw();
            }
        }

        // Keyboard accelerators.

        private void ToggleUIInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (WorkaroundShouldIgnoreAccelerator()) return;

            if (Windows.UI.Xaml.Visibility.Collapsed == ControlsPanel.Visibility)
            {
                SetUIHidden(false);
            }
            else
            {
                SetUIHidden(true);
            }
        }

        private void ToggleFullscreenInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (WorkaroundShouldIgnoreAccelerator()) return;

            if (ApplicationView.GetForCurrentView().IsFullScreenMode)
            {
                SetUIFullscreen(false);
            }
            else
            {
                SetUIFullscreen(true);
            }
        }

        private void EscapeFullscreenInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            SetUIFullscreen(false);
        }

        private void ToggleExperimentalToolsInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (WorkaroundShouldIgnoreAccelerator()) return;

            SetExperimentalTools(!enableExperimentalTools);
        }

        private void ScrapeColorProfile()
        {

        }

        /// <summary>
        /// If the OS is not at least 19H1, then shows an error dialog to the user.
        /// </summary>
        /// <returns>True if the check succeeded, and execution should continue.</returns>

        private bool CheckHeifAvifOsVersion()
        {
            // TODO: This helper is part of DXRenderer, for simplicity just copy the OS check.
            if (!Windows.Foundation.Metadata.ApiInformation.IsApiContractPresent(
                "Windows.Foundation.UniversalApiContract", 8)) // 8 == Windows 1903/19H1
            {
                var dialog = new ErrorContentDialog(ErrorDialogType.Need19H1);
#pragma warning disable CS4014 // Because this call is not awaited, execution of the current method continues before the call is completed
                dialog.ShowAsync();
#pragma warning restore CS4014 // Because this call is not awaited, execution of the current method continues before the call is completed

                return false;
            }
            else
            {
                return true;
            }
        }

        private StorageFolder _currentFolder = null;
        private List<StorageFile> _fileList = new List<StorageFile>();
        private int _currentIndex = -1;
        private bool _isLoadingFileList = false;
        private bool _isLoadingImage = false;

        private void UpdateNavigationButtonsState()
        {
            // 确保在主线程更新UI
            if (!Dispatcher.HasThreadAccess)
            {
                Dispatcher.RunAsync(CoreDispatcherPriority.Normal, UpdateNavigationButtonsState);
                return;
            }

            try
            {
                // 根据加载状态决定按钮是否可用
                bool canNavigate = !_isLoadingFileList && !_isLoadingImage;

                // 检查是否有文件列表且不在加载中
                bool hasFiles = _fileList.Count > 1; // 至少需要2个文件才能导航

                // 检查索引合法性
                bool canGoPrev = canNavigate && hasFiles && _currentIndex > 0;
                bool canGoNext = canNavigate && hasFiles && _currentIndex < _fileList.Count - 1;

                // 更新按钮状态
                PrevImageButton.IsEnabled = canGoPrev;
                NextImageButton.IsEnabled = canGoNext;

                Debug.WriteLine($"更新导航按钮状态: 文件加载中={_isLoadingFileList}, 图片加载中={_isLoadingImage}, Prev={canGoPrev}, Next={canGoNext}");
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"更新导航按钮状态失败: {ex.Message}");
                // 安全处理：禁用所有导航按钮
                PrevImageButton.IsEnabled = false;
                NextImageButton.IsEnabled = false;
            }
        }

        private async void PrevImageButton_Click(object sender, RoutedEventArgs e)
        {
            if (_fileList.Count == 0 || _currentIndex <= 0)
            {
                Debug.WriteLine("无法向前导航：已在第一张图片");
                return;
            }

            int newIndex = _currentIndex - 1;
            Debug.WriteLine($"向前导航: {_currentIndex} -> {newIndex}");

            // 直接从文件列表中获取 StorageFile 对象
            StorageFile file = _fileList[newIndex];
            await LoadImageAsync(file);
        }

        private async void NextImageButton_Click(object sender, RoutedEventArgs e)
        {
            if (_fileList.Count == 0 || _currentIndex >= _fileList.Count - 1)
            {
                Debug.WriteLine("无法向后导航：已在最后一张图片");
                return;
            }

            int newIndex = _currentIndex + 1;
            Debug.WriteLine($"向后导航: {_currentIndex} -> {newIndex}");

            // 直接从文件列表中获取 StorageFile 对象
            StorageFile file = _fileList[newIndex];
            await LoadImageAsync(file);
        }

        private void PrevImageInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (WorkaroundShouldIgnoreAccelerator()) return;

            // 防止在加载中重复触发
            if (_isLoadingFileList || _isLoadingImage)
            {
                args.Handled = true;
                return;
            }

            // 模拟点击"上一张"按钮
            if (PrevImageButton.IsEnabled)
            {
                PrevImageButton_Click(null, null);
                args.Handled = true;
            }
        }

        private void NextImageInvoked(KeyboardAccelerator sender, KeyboardAcceleratorInvokedEventArgs args)
        {
            if (WorkaroundShouldIgnoreAccelerator()) return;

            // 防止在加载中重复触发
            if (_isLoadingFileList || _isLoadingImage)
            {
                args.Handled = true;
                return;
            }

            // 模拟点击"下一张"按钮
            if (NextImageButton.IsEnabled)
            {
                NextImageButton_Click(null, null);
                args.Handled = true;
            }
        }

        private async Task LoadImageListAsync(StorageFile imageFile)
        {
            _isLoadingFileList = true;
            UpdateNavigationButtonsState();

            try
            {
                // 获取当前图片所在的文件夹
                StorageFolder newFolder = await imageFile.GetParentAsync();

                // 检查文件夹是否变化
                bool folderChanged = _currentFolder == null ||
                                   !_currentFolder.IsEqual(newFolder);

                if (folderChanged)
                {
                    // 将文件夹添加到 FutureAccessList 以获取持久访问权限
                    string token = StorageApplicationPermissions.FutureAccessList.Add(newFolder);

                    // 获取文件夹中的所有文件
                    var files = await newFolder.GetFilesAsync();

                    // 过滤出支持的图片类型
                    var imageExtensions = new[]
                    {
                ".jpg", ".jpeg", ".png", ".bmp", ".gif", ".tiff",
                ".webp", ".heic", ".jxr", ".hdr", ".exr"
            };

                    _fileList = files
                        .OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase)
                        .ToList();

                    _currentFolder = newFolder;
                    Debug.WriteLine($"已加载新目录: {newFolder.Name}, 文件数量: {_fileList.Count}");
                }

                // 更新当前索引
                _currentIndex = _fileList.FindIndex(f => f.Path.Equals(imageFile.Path, StringComparison.OrdinalIgnoreCase));

                if (_currentIndex < 0 && _fileList.Count > 0)
                {
                    _currentIndex = 0;
                    Debug.WriteLine($"文件不在列表中，使用第一个文件: {_fileList[0].Name}");
                }
                else
                {
                    Debug.WriteLine($"当前索引: {_currentIndex}, 文件: {imageFile.Name}");
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"LoadImageListAsync 错误: 没有权限");
                _currentFolder = null;
                _fileList = new List<StorageFile>();
                _currentIndex = -1;
            }
            finally
            {
                // 无论成功或失败，都更新加载状态
                _isLoadingFileList = false;
                UpdateNavigationButtonsState();
            }
        }

        public async Task LoadImageAsync(StorageFile imageFile)
        {
            // File format handler registration is static vs. OS version (in the appxmanifset), so a user may attempt to activate
            // the app for a HEIF or AVIF image on RS5, which won't work.
            if (!CheckHeifAvifOsVersion())
            {
                return;
            }
            //LoadImageListAsync
            LoadImageListAsync(imageFile);

            isImageValid = false;
            ExposureAdjustSlider.IsEnabled = false;
            RenderEffectCombo.IsEnabled = false;
            PixelColorCheckbox.IsEnabled = false;
            DispMaxCLLOverrideSlider.IsEnabled = false;

            bool useDirectXTex = false;

            var type = imageFile.FileType.ToLowerInvariant();
            if (type == ".hdr" ||
                type == ".exr" ||
                type == ".dds")
            {
                useDirectXTex = true;
            }

            ImageInfo info;

            if (useDirectXTex)
            {
                // For formats that are loaded by DirectXTex, we must use a file path from the temporary folder.
                imageFile = await imageFile.CopyAsync(
                        ApplicationData.Current.TemporaryFolder,
                        imageFile.Name,
                        NameCollisionOption.ReplaceExisting);

                info = renderer.LoadImageFromDirectXTex(imageFile.Path, type, loaderOptions);
            }
            else
            {
                info = renderer.LoadImageFromWic(await imageFile.OpenAsync(FileAccessMode.Read), loaderOptions);
            }

            if (info.isValid == false)
            {
                if (_currentIndex >= 0 && _currentIndex < _fileList.Count)
                {
                    _fileList.RemoveAt(_currentIndex);
                    Debug.WriteLine($"已移除无效文件: {imageFile.Name}");
                }

                // 尝试加载下一个可用文件
                if (_fileList.Count > 0)
                {
                    // 确保索引在合法范围内
                    int newIndex = Math.Min(_currentIndex, _fileList.Count - 1);

                    // 直接使用 StorageFile 对象
                    StorageFile nextFile = _fileList[newIndex];
                    await LoadImageAsync(nextFile);
                }
                else
                {
                    // 没有有效文件，重置状态
                    _currentFolder = null;
                    _fileList = new List<StorageFile>();
                    _currentIndex = -1;
                    SetWindowTitle("没有可用的图片");

                    // 更新导航按钮状态
                    UpdateNavigationButtonsState();
                }

                // Exit before any of the current image state is modified.
                ErrorContentDialog dialog;

                if (type == ".heic" && info.isHeif == true)
                {
                    dialog = new ErrorContentDialog(ErrorDialogType.NeedHevc, imageFile.Name);
                }
                else if (type == ".avif" && info.isHeif == true)
                {
                    dialog = new ErrorContentDialog(ErrorDialogType.NeedAv1, imageFile.Name);
                }
                else
                {
                    dialog = new ErrorContentDialog(ErrorDialogType.InvalidFile, imageFile.Name);
                }

                await dialog.ShowAsync();

                return;
            }

            imageInfo = info;

            renderer.CreateImageDependentResources();
            imageCLL = renderer.FitImageToWindow(true); // On first load of image, need to generate HDR metadata.

            SetWindowTitle(imageFile.Name);
            ImageACKind.Text = UIStrings.LABEL_ACKIND + UIStrings.ConvertACKindToString(imageInfo.imageKind);
            ImageHasProfile.Text = UIStrings.LABEL_COLORPROFILE + (imageInfo.countColorProfiles > 0 ? UIStrings.LABEL_YES : UIStrings.LABEL_NO);
            ImageBitDepth.Text = UIStrings.LABEL_BITDEPTH + imageInfo.bitsPerChannel;
            ImageIsFloat.Text = UIStrings.LABEL_FLOAT + (imageInfo.isFloat ? UIStrings.LABEL_YES : UIStrings.LABEL_NO);

            if (imageCLL.maxNits < 0.0f || imageCLL.isSceneReferred == false)
            {
                ImageMaxCLL.Text = UIStrings.LABEL_MAXCLL + UIStrings.LABEL_NA;
            }
            else
            {
                ImageMaxCLL.Text = UIStrings.LABEL_MAXCLL + imageCLL.maxNits.ToString("N1") + UIStrings.LABEL_LUMINANCE_NITS;
            }

            if (imageCLL.medianNits < 0.0f || imageCLL.isSceneReferred == false)
            {
                ImageMedianCLL.Text = UIStrings.LABEL_MEDCLL + UIStrings.LABEL_NA;
            }
            else
            {
                ImageMedianCLL.Text = UIStrings.LABEL_MEDCLL + imageCLL.medianNits.ToString("N1") + UIStrings.LABEL_LUMINANCE_NITS;
            }

            // Image loading is done at this point.
            isImageValid = true;
            ExposureAdjustSlider.IsEnabled = true;
            RenderEffectCombo.IsEnabled = true;
            PixelColorCheckbox.IsEnabled = true;
            DispMaxCLLOverrideSlider.IsEnabled = true;

            if (imageInfo.imageKind == AdvancedColorKind.HighDynamicRange)
            {
                ExportImageButton.IsEnabled = true;
            }
            else
            {
                ExportImageButton.IsEnabled = false;
            }

            UpdateNavigationButtonsState();
            UpdateDefaultRenderOptions();
        }

        private async Task ExportImageAsync(StorageFile file)
        {
            Guid wicFormat = DirectXCppConstants.GUID_ContainerFormatJpeg;
            switch (file.FileType)
            {
                case ".jpg": // TODO: Remove this hardcoded constant.
                    wicFormat = DirectXCppConstants.GUID_ContainerFormatJpeg;
                    break;

                case ".png":
                    wicFormat = DirectXCppConstants.GUID_ContainerFormatPng;
                    break;

                case ".jxr":
                    wicFormat = DirectXCppConstants.GUID_ContainerFormatJxr;
                    break;
            }

            var ras = await file.OpenAsync(FileAccessMode.ReadWrite);

            if (file.FileType == ".jxr")
            {
                renderer.ExportImageToJxr(ras);
            }
            else
            {
                renderer.ExportImageToSdr(ras, wicFormat);
            }
        }

        private void SetUIHidden(bool value)
        {
            if (value == false)
            {
                ControlsPanel.Visibility = Visibility.Visible;
            }
            else
            {
                ControlsPanel.Visibility = Visibility.Collapsed;
            }
        }

        private void SetUIFullscreen(bool value)
        {
            if (value == false)
            {
                ApplicationView.GetForCurrentView().ExitFullScreenMode();
                ApplicationView.PreferredLaunchWindowingMode = ApplicationViewWindowingMode.Auto;
            }
            else
            {
                ApplicationView.GetForCurrentView().TryEnterFullScreenMode();
                ApplicationView.PreferredLaunchWindowingMode = ApplicationViewWindowingMode.FullScreen;
            }
        }

        private void SetExperimentalTools(bool value)
        {
            if (value == false)
            {
                enableExperimentalTools = false;
                ExperimentalTools.Visibility = Visibility.Collapsed;

            }
            else
            {
                enableExperimentalTools = true;
                ExperimentalTools.Visibility = Visibility.Visible;
            }

            // Right now this will both remove or apply the experimental tools.
            UpdateRenderOptions();
        }

        /// <summary>
        /// Works around an issue where keyboard accelerators sometimes trigger two back-to-back events.
        /// Checks if the previous accelerator event was within a timeout period, i.e. "debounce".
        /// </summary>
        /// <returns>Whether to ignore the last keyboard accelerator event.</returns>
        private bool WorkaroundShouldIgnoreAccelerator()
        {
            if (workaroundDebounceTimer.IsRunning == false)
            {
                workaroundDebounceTimer.Restart();
            }
            else
            {
                workaroundDebounceTimer.Stop();

                // Arbitrarily chosen threshold.
                if (workaroundDebounceTimer.ElapsedMilliseconds < 100)
                {
                    return true;
                }
            }

            return false;
        }
        private Stopwatch workaroundDebounceTimer = new Stopwatch();

        // Saves the current state of the app for suspend and terminate events.
        public void SaveInternalState(IPropertySet state)
        {
            renderer.Trim();
        }

        // Loads the current state of the app for resume events.
        public void LoadInternalState(IPropertySet state)
        {
        }

        // UI Element event handlers.

        private async void ExportImageButton_Click(object sender, RoutedEventArgs e)
        {
            var picker = new FileSavePicker
            {
                SuggestedStartLocation = PickerLocationId.PicturesLibrary,
                CommitButtonText = "Export image"
            };

            foreach (var format in UIStrings.FILEFORMATS_SAVE)
            {
                picker.FileTypeChoices.Add(format);
            }

            var pickedFile = await picker.PickSaveFileAsync();
            if (pickedFile != null)
            {
                await ExportImageAsync(pickedFile);
            }
        }

        private async void OpenImageButton_Click(object sender, RoutedEventArgs e)
        {
            var picker = new FileOpenPicker
            {
                SuggestedStartLocation = PickerLocationId.PicturesLibrary
            };

            foreach (var ext in UIStrings.FILEFORMATS_OPEN)
            {
                picker.FileTypeFilter.Add(ext);
            }

            // TODO: This helper is part of DXRenderer, for simplicity just copy the OS check.
            if (Windows.Foundation.Metadata.ApiInformation.IsApiContractPresent(
                "Windows.Foundation.UniversalApiContract", 8)) // 8 == Windows 1903/19H1
            {
                foreach (var ext in UIStrings.FILEFORMATS_OPEN_19H1)
                {
                    picker.FileTypeFilter.Add(ext);
                }
            }

            try
            {
                var file = await picker.PickSingleFileAsync();
                if (file != null)
                {
                    await LoadImageAsync(file);
                }
            }
            catch (Exception ex)
            {
                var dialog = new ErrorContentDialog(ErrorDialogType.InvalidFile, ex.Message);
                await dialog.ShowAsync();
            }
        }

        private void ExposureAdjustSlider_Changed(object sender, RangeBaseValueChangedEventArgs e)
        {
            UpdateRenderOptions();
        }

        private void RenderEffectCombo_Changed(object sender, SelectionChangedEventArgs e)
        {
            UpdateRenderOptions();
        }

        // Pointer input event handlers.

        private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
        {
            swapChainPanel.CapturePointer(e.Pointer);
            gestureRecognizer.ProcessDownEvent(e.GetCurrentPoint(swapChainPanel));
        }

        private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
        {
            gestureRecognizer.ProcessMoveEvents(e.GetIntermediatePoints(swapChainPanel));

            if (PixelColorCheckbox.IsChecked == true)
            {
                var color = renderer.GetPixelColorValue(e.GetCurrentPoint(swapChainPanel).Position);
                PixelColor.Text = "scRGB color: " + color.X.ToString("F2") + "," + color.Y.ToString("F2") + "," + color.Z.ToString("F2") + "," + color.W.ToString("F2");
            }
        }

        private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
        {
            gestureRecognizer.ProcessUpEvent(e.GetCurrentPoint(swapChainPanel));
            swapChainPanel.ReleasePointerCapture(e.Pointer);
        }

        private void OnPointerCanceled(object sender, PointerRoutedEventArgs e)
        {
            gestureRecognizer.CompleteGesture();
            swapChainPanel.ReleasePointerCapture(e.Pointer);
        }

        private void OnPointerWheelChanged(object sender, PointerRoutedEventArgs e)
        {
            // Passing isControlKeyDown = true causes the wheel delta to be treated as scrolling.
            gestureRecognizer.ProcessMouseWheelEvent(e.GetCurrentPoint(swapChainPanel), false, true);
        }

        private void OnManipulationStarted(GestureRecognizer sender, ManipulationStartedEventArgs args)
        {
        }
        private void OnManipulationUpdated(GestureRecognizer sender, ManipulationUpdatedEventArgs args)
        {
            renderer.UpdateManipulationState(args);
        }

        private void OnManipulationCompleted(GestureRecognizer sender, ManipulationCompletedEventArgs args)
        {
        }

        // Experimental options UI.

        private void DispMaxCLLOverrideSlider_ValueChanged(object sender, RangeBaseValueChangedEventArgs e)
        {
            UpdateRenderOptions();
        }

        private void PixelColorCheckbox_Click(object sender, RoutedEventArgs e)
        {
            if (PixelColorCheckbox.IsChecked == true)
            {
                renderer.SetTargetCpuReadbackSupport(true);
                tooltip.Visibility = Visibility.Visible;
            }
            else
            {
                renderer.SetTargetCpuReadbackSupport(false);
                tooltip.Visibility = Visibility.Collapsed;
            }
        }

        private void GamutMapCheckbox_Click(object sender, RoutedEventArgs e)
        {
            if (!enableExperimentalTools) return;

            if (WorkaroundShouldIgnoreAccelerator()) return;

            enableGamutMap = GamutMapCheckbox.IsChecked == true ? true : false;

            UpdateRenderOptions();
        }

        private void DispProfileOverride_Click(object sender, RoutedEventArgs e)
        {
            if (!enableExperimentalTools) return;

            if (WorkaroundShouldIgnoreAccelerator()) return;

            profileColorimetryOverride = DispProfileOverride.IsChecked == true ? true : false;

            ScrapeColorProfile();

            UpdateRenderOptions();
        }
    }
}
