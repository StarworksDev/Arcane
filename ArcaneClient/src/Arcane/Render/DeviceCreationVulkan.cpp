// The Vulkan CREATION HALF: loader, instance, debug messenger, physical
// device, logical device, the one graphics queue, and the F-3 device-removed
// observation point that belongs beside them. Everything here is native
// Vulkan.
//
// THE DEVICE-REMOVED OBSERVER LIVES HERE, beside the device it observes, and
// its once-only latch is file-local per backend. The two functions stay
// DISTINCT -- the file-local ObserveDeviceRemoved and the namespace-scope
// ObserveDeviceRemovedVulkan forwarder -- because NriDiagnostics::Disarm
// compares the hook slot against the address it installed, and collapsing
// them would change that address.
//
// See DeviceCreationVulkan.hpp for the consumer shape and what each field of
// the creation half is for.

#include <Arcane/Base/Diagnostics.hpp>
#include <Arcane/Platform/Platform.hpp>
#include <Arcane/Render/DeviceCreationVulkan.hpp>
#include <Arcane/Render/DeviceRemovedObservers.hpp>
#include <Arcane/Render/GpuInstrumentation.hpp>   // NoteGpuDeviceLost -- the host's device-lost latch
#include <Arcane/Render/RenderErrorLatch.hpp>
#include <Arcane/Render/RenderDeviceSettings.hpp>   // render.adapter, render.debug.minSeverity

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_beta.h>   // VkPhysicalDevicePortabilitySubsetFeaturesKHR (MoltenVK; a provisional extension)

#include <algorithm>
#include <atomic>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace Arcane
{
    namespace
    {
        // ONE source for the API version: VkApplicationInfo below and the
        // minorVersion the NRI wrapper is handed must never drift apart
        // (NRI capability contract item 5 -- NRI reads that field verbatim in
        // wrapper mode and never re-queries the device).
        // TRIPWIRE: if this ever bumps to 1.4 (or beyond), fold an actual
        // vkEnumerateInstanceVersion query into the apiMinorVersion min()
        // below per §6.8 -- today's min(physical minor, kApiMinorVersion)
        // stands in for min(instance minor, physical minor) only because the
        // hard-fail guard right below that min() pins physicalApiMinor to
        // always be >= kApiMinorVersion for a 1.3 request.
        constexpr uint32_t kApiVersion      = VK_API_VERSION_1_3;
        constexpr uint32_t kApiMinorVersion = VK_API_VERSION_MINOR(kApiVersion);

        // Surface extensions are requested even for headless devices: they
        // cost nothing without a surface, and keep one code path for both
        // headless tests and windowed swapchains.
        const char* kInstanceExtensions[] = {
            VK_KHR_SURFACE_EXTENSION_NAME,
#if ARC_PLATFORM_WINDOWS
            VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
            // Non-Windows: the X11/Wayland surface extensions are OPTIONAL
            // (kOptionalSurfaceExtensions below) -- which ones exist depends on
            // the loader/ICD, and a headless box must never fail over them.
            // NRI capability contract item 1 (hard): NRI's SwapChainVK::Create
            // calls GetPhysicalDeviceSurfaceFormats2KHR and
            // GetPhysicalDeviceSurfaceCapabilities2KHR through UNGUARDED
            // pointers, so its absence is a crash on the first swapchain.
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        };
#if !ARC_PLATFORM_WINDOWS
        // Linux port: the window-system surface extensions NRI's SwapChainVK
        // can consume (it is built with VK_USE_PLATFORM_XLIB_KHR + _WAYLAND_KHR
        // off-Windows), plus xcb, which SDL's X11 driver may use. Each is
        // requested only when the instance enumerates it (Init's sweep).
        const char* kOptionalSurfaceExtensions[] = {
            "VK_KHR_xlib_surface",
            "VK_KHR_xcb_surface",
            "VK_KHR_wayland_surface",
            // macOS port: MoltenVK's CAMetalLayer surface (NRI's SwapChainVK
            // is built with VK_USE_PLATFORM_METAL_EXT on a Mac target).
            "VK_EXT_metal_surface",
        };
#endif
        // macOS port (2026-10-07): MoltenVK is a Vulkan PORTABILITY
        // implementation -- the loader (1.3.216+) hides it from
        // vkEnumeratePhysicalDevices unless the instance enables
        // VK_KHR_portability_enumeration AND sets
        // VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR. Taken wherever the
        // instance offers it (a no-op for a conformant driver, which is still
        // enumerated), so no platform #if gates the device list.
        constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
        // ...and a portability DEVICE must enable VK_KHR_portability_subset
        // (VUID-VkDeviceCreateInfo-pProperties-04451); its feature struct is
        // queried and passed back verbatim like every other (contract item 4).
        constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
        // F-5a: the REQUIRED device extensions. The optional GPU-crash
        // diagnostics extensions are appended to a copy of this list only when
        // the physical device actually enumerates them -- see F-5c's sweep in
        // Init. Requesting an unsupported device extension is a hard
        // vkCreateDevice failure, which is precisely what must never happen
        // for a diagnostics nicety.
        const char* kDeviceExtensions[] = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME,
            // NRI capability contract item 2 (hard): NRI calls
            // vkCmdPushDescriptorSet unguarded for root descriptors and root
            // samplers, and reports rootDescriptorMaxNum = 0 without it.
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
        };
        // NRI capability contract item 3 (quality): NRI takes a documented
        // slower path when these are absent (maintenance5/6 -> the v1 bind and
        // push forms; memory_budget -> VMA without budget awareness), so they
        // ride F-5c's availability sweep rather than the hard-required list
        // above -- by the same rule stated there, a driver that lacks
        // maintenance6 must not fail device creation over a quality nicety.
        //
        // Named indices, not bare subscripts: the maintenance pair also needs
        // its FEATURE struct chained at creation (see item 3's second half in
        // Init), and that code has to name the one it means.
        enum OptionalDeviceExtension
        {
            kOptMaintenance5,
            kOptMaintenance6,
            kOptMemoryBudget,   // pure list check -- no feature struct exists
            kOptionalDeviceExtensionCount
        };
        const char* kOptionalDeviceExtensions[kOptionalDeviceExtensionCount] = {
            VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
            VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
            VK_EXT_MEMORY_BUDGET_EXTENSION_NAME,
        };
        // NRI capability contract item 9: VK_EXT_zero_initialize_device_memory
        // is deliberately NOT requested -- NRI's enableMemoryZeroInitialization
        // flips every image's initialLayout to ZERO_INITIALIZED_EXT and is only
        // legal with the extension enabled, so both stay off together.

        constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

        // F-3: the ONE device-removed observation point for this backend.
        // It is reached through RenderErrorLatch's hook slot, which
        // Render/Nri/NriDiagnostics::Arm fills with ObserveDeviceRemovedVulkan
        // below. Two producers drive that slot on this backend: the latch's
        // "Device Removed" substring scan (NoteNriError -- what NriCommon's
        // RouteNriError funnels every ARC_NRI_CHECK failure into), and its
        // TYPED seam NoteDeviceLost, which is how an nri::Result::DEVICE_LOST
        // off a QueueSubmit/AcquireNextTexture/QueuePresent gets here without
        // depending on message text at all.
        //
        // Once-only per armed device: a lost device keeps reporting loss on
        // every submit and every present, and the second report is worthless
        // -- the marker buffer and fault state belong to the FIRST one. Reset
        // when a new backend arms (project switch recreates the device).
        std::atomic<bool> g_deviceRemovedReported{ false };

        void ObserveDeviceRemoved()
        {
            if (g_deviceRemovedReported.exchange(true, std::memory_order_acq_rel))
                return;

            // The reason string is load-bearing: Diagnostics::DeriveKind
            // classifies the .arcdiag "kind" by case-sensitive substring, and
            // only a reason containing lowercase "gpu" resolves to a gpu kind
            // (here "gpu-crash", which is what makes the .gpudump sibling get
            // written). Same wording as the D3D12 backend. Do not reword.
            Diagnostics::WriteReport("gpu-crash: device removed");

            // AFTER the report, deliberately: hosts poll this latch and shut
            // down on it, and "observed" must always mean "the report exists".
            NoteGpuDeviceLost();
        }

        // Routes VK validation-layer output into the SAME latch every other
        // render-layer error producer reaches: errors increment
        // RenderErrorCount(), so a raw VUID fails the GPU tests exactly like
        // an NRI error would.
        // Typed vk:: signature (vk::PFN_DebugUtilsMessengerCallbackEXT), not the
        // raw C PFN_vkDebugUtilsMessengerCallbackEXT: setPfnUserCallback's
        // C-signature overload is deprecated in favor of this one.
        VKAPI_ATTR vk::Bool32 VKAPI_CALL VkDebugCallback(
            vk::DebugUtilsMessageSeverityFlagBitsEXT severity,
            vk::DebugUtilsMessageTypeFlagsEXT /*types*/,
            const vk::DebugUtilsMessengerCallbackDataEXT* data,
            void* /*userData*/)
        {
            const char* text = (data && data->pMessage) ? data->pMessage : "";
            if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eError)
                RenderErrorLatch::Instance().NoteNriError(text);
            else if (severity & vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning)
                ARC_WARN("[vk] {}", text);
            else
                ARC_INFO("[vk] {}", text);   // eInfo: subscribed only when render.debug.minSeverity is Info
            return VK_FALSE;
        }
    }

    // ----------------------------------------------------------------
    // The CREATION HALF.
    // ----------------------------------------------------------------
    // It sits at namespace scope (outside the anonymous namespace above)
    // because DeviceCreationVulkan.hpp declares it for its one consumer,
    // Nri/NriDevice.cpp's NativeDeviceOwner.
    //
    // Failure leaves `out` holding whatever was created; the caller's teardown
    // (DestroyVulkanNativeDevice, reached through ~NativeDeviceOwner) releases
    // it. Exceptions still propagate: VulkanDeviceCreation's loader member
    // throws when vulkan-1.dll is absent, and NativeDeviceOwner::Create's
    // try/catch is what reports it.
    bool CreateVulkanNativeDevice(const RenderDeviceDesc& desc, VulkanDeviceCreation& out)
    {
        // Recorded, not acted on: NRI's own validation layer is available in
        // wrapper mode (contract 1.1) and keys off the same switch the
        // instance's VK_LAYER_KHRONOS_validation below does. Pure member
        // write -- no call, no branch.
        out.enableValidation = desc.enableValidation;

        auto vkGetInstanceProcAddr =
            out.loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
        if (!vkGetInstanceProcAddr)
        {
            ARC_ERROR("Vulkan loader not available (vulkan-1.dll missing?)");
            return false;
        }
        VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

        std::vector<const char*> layers;
        if (desc.enableValidation)
        {
            try
            {
                for (const auto& layer : vk::enumerateInstanceLayerProperties())
                {
                    if (std::string_view(layer.layerName) == kValidationLayer)
                    {
                        layers.push_back(kValidationLayer);
                        break;
                    }
                }
            }
            catch (const vk::SystemError& e)
            {
                ARC_WARN("vk::enumerateInstanceLayerProperties threw: {}; "
                         "continuing without validation layer", e.what());
            }
            if (layers.empty())
                ARC_WARN("{} not installed; continuing without it", kValidationLayer);
        }

        std::vector<const char*> instanceExtensions(
            std::begin(kInstanceExtensions), std::end(kInstanceExtensions));

        // Enumerated once and shared by the two checks below; previously
        // this call sat inside the validation-only debug_utils loop.
        const auto availableInstanceExtensions =
            vk::enumerateInstanceExtensionProperties();
        const auto instanceExtensionAvailable = [&](std::string_view wanted) {
            for (const auto& ext : availableInstanceExtensions)
            {
                if (std::string_view(ext.extensionName) == wanted)
                    return true;
            }
            return false;
        };

        // NRI capability contract item 1: a hard-required instance
        // extension, named here rather than left to vk::createInstance to
        // report as a bare ErrorExtensionNotPresent (contract item 14's
        // rule -- the message must say WHICH capability is missing).
        if (!instanceExtensionAvailable(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME))
        {
            ARC_ERROR("Vulkan instance extension '{}' is unavailable; it is "
                      "hard-required (NRI wrapper capability contract item 1)",
                      VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            return false;
        }

#if !ARC_PLATFORM_WINDOWS
        for (const char* ext : kOptionalSurfaceExtensions)
        {
            if (instanceExtensionAvailable(ext))
                instanceExtensions.push_back(ext);
        }
#endif

        const bool portabilityEnumeration = instanceExtensionAvailable(kPortabilityEnumeration);
        if (portabilityEnumeration)
            instanceExtensions.push_back(kPortabilityEnumeration);

        bool debugUtils = false;
        if (desc.enableValidation &&
            instanceExtensionAvailable(VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
        {
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debugUtils = true;
        }

        // OPT-IN, DEFAULT OFF: synchronization validation.
        // `RenderDeviceDesc::enableSyncValidation` defaults false, so with the
        // flag off this block is one false branch (`syncFeatures` stays
        // unreferenced and `instanceInfo.pNext` stays null). It is asked for
        // by the frame-graph vehicle, which needs the hazard checks core
        // validation does not perform: a missing or misplaced barrier on the
        // graph's derived barrier chain is silent to core validation.
        //
        // Mechanism: VK_EXT_validation_features + a VkValidationFeaturesEXT
        // chained onto VkInstanceCreateInfo, the layer-configuration path the
        // Vulkan SDK documents for turning sync validation on from inside the
        // application (rather than via vk_layer_settings.txt or the
        // VK_LAYER_* environment variables, neither of which a scripted desk
        // command can rely on being set). Every miss degrades with a WARN --
        // losing a diagnostic must never fail device creation.
        bool syncValidation = false;
        // Both must outlive vk::createInstance below (the struct is reachable
        // from instanceInfo.pNext, and it points at this array) -- hence
        // function scope rather than inside the `if`.
        const vk::ValidationFeatureEnableEXT syncFeatureList[] = {
            vk::ValidationFeatureEnableEXT::eSynchronizationValidation
        };
        vk::ValidationFeaturesEXT syncFeatures;
        if (desc.enableSyncValidation)
        {
            if (layers.empty())
            {
                ARC_WARN("sync validation requested but {} is not loaded; "
                         "continuing without it", kValidationLayer);
            }
            else
            {
                // ENUMERATE WITH THE LAYER NAME. VK_EXT_validation_features is
                // provided by VK_LAYER_KHRONOS_validation, an EXPLICIT layer --
                // no ICD implements it. Per the Vulkan spec,
                // vkEnumerateInstanceExtensionProperties(pLayerName = NULL) --
                // which is what the `instanceExtensionAvailable` helper above
                // wraps -- returns ONLY extensions provided by the Vulkan
                // implementation and by IMPLICIT layers. Asking it about this
                // extension therefore answers "unavailable" on every stock SDK
                // machine, silently dropping sync validation while the vehicle
                // still exits clean and proves nothing. DO NOT re-route this
                // check through `instanceExtensionAvailable`.
                //
                // Requesting a layer-provided extension in the SAME
                // vkCreateInstance call that enables the layer is legal
                // precisely because `layers` is non-empty on this branch.
                bool featuresExtAvailable = false;
                try
                {
                    const std::string layerName(kValidationLayer);
                    for (const auto& ext : vk::enumerateInstanceExtensionProperties(layerName))
                    {
                        if (std::string_view(ext.extensionName) == VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME)
                        {
                            featuresExtAvailable = true;
                            break;
                        }
                    }
                }
                catch (const vk::SystemError& e)
                {
                    // Catch-and-degrade, matching the layer enumeration above.
                    // This call sits OUTSIDE the vk::createInstance try below,
                    // whose catch returns false -- letting a throw reach that
                    // one would fail device creation over a diagnostic, which
                    // is exactly what this block must never do.
                    ARC_WARN("vk::enumerateInstanceExtensionProperties('{}') threw: {}; "
                             "continuing without sync validation", kValidationLayer, e.what());
                }

                if (!featuresExtAvailable)
                {
                    ARC_WARN("sync validation requested but layer '{}' does not provide '{}'; "
                             "continuing without it", kValidationLayer,
                             VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                }
                else
                {
                    instanceExtensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
                    syncFeatures.setEnabledValidationFeatureCount(1)
                                .setPEnabledValidationFeatures(syncFeatureList);
                    syncValidation = true;
                }
            }
        }

        try
        {
            auto appInfo = vk::ApplicationInfo()
                .setPApplicationName("Arcane")
                .setApiVersion(kApiVersion);

            auto instanceInfo = vk::InstanceCreateInfo()
                .setPApplicationInfo(&appInfo)
                .setEnabledLayerCount((uint32_t)layers.size())
                .setPpEnabledLayerNames(layers.data())
                .setEnabledExtensionCount((uint32_t)instanceExtensions.size())
                .setPpEnabledExtensionNames(instanceExtensions.data());
            if (portabilityEnumeration)
                instanceInfo.setFlags(vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR);
            // Set only when the opt-in above actually took: an untouched
            // pNext is what every pre-existing caller produces.
            if (syncValidation)
            {
                instanceInfo.setPNext(&syncFeatures);
                ARC_INFO("Vulkan synchronization validation ENABLED");
            }

            out.instance = vk::createInstance(instanceInfo);
        }
        catch (const vk::SystemError& e)
        {
            ARC_ERROR("vk::createInstance failed: {}", e.what());
            return false;
        }

        VULKAN_HPP_DEFAULT_DISPATCHER.init(out.instance);

        if (debugUtils)
        {
            using Severity = vk::DebugUtilsMessageSeverityFlagBitsEXT;
            using Type = vk::DebugUtilsMessageTypeFlagBitsEXT;
            // render.debug.minSeverity (default Warning: Error | Warning) --
            // the same subscription as the D3D12 info queue's deny list.
            const MinSeverity minSeverity = Settings<RenderDebugSettings>().minSeverity;
            vk::DebugUtilsMessageSeverityFlagsEXT severities = Severity::eError;
            if (Reports(minSeverity, MinSeverity::Warning)) severities |= Severity::eWarning;
            if (Reports(minSeverity, MinSeverity::Info))    severities |= Severity::eInfo;
            auto messengerInfo = vk::DebugUtilsMessengerCreateInfoEXT()
                .setMessageSeverity(severities)
                .setMessageType(Type::eValidation | Type::eGeneral |
                                Type::ePerformance)
                .setPfnUserCallback(&VkDebugCallback);
            out.debugMessenger =
                out.instance.createDebugUtilsMessengerEXT(messengerInfo);
        }

        std::vector<vk::PhysicalDevice> physicalDevices;
        try
        {
            physicalDevices = out.instance.enumeratePhysicalDevices();
        }
        catch (const vk::SystemError& e)
        {
            ARC_ERROR("vk::enumeratePhysicalDevices failed: {}", e.what());
            return false;
        }

        if (physicalDevices.empty())
        {
            ARC_ERROR("No Vulkan physical devices");
            return false;
        }
        // render.adapter: -1 (the default) takes the first discrete GPU,
        // else [0]; an index picks that physical device, falling back to the
        // default when absent.
        const std::int32_t wantedAdapter = Settings<RenderSettings>().adapter;
        if (wantedAdapter >= 0 && static_cast<std::size_t>(wantedAdapter) < physicalDevices.size())
        {
            out.physicalDevice = physicalDevices[static_cast<std::size_t>(wantedAdapter)];
        }
        else
        {
            if (wantedAdapter >= 0)
                ARC_WARN("render.adapter {}: only {} Vulkan physical device(s); using the first discrete GPU",
                         wantedAdapter, physicalDevices.size());
            out.physicalDevice = physicalDevices[0];
            for (auto& candidate : physicalDevices)
            {
                if (candidate.getProperties().deviceType ==
                    vk::PhysicalDeviceType::eDiscreteGpu)
                {
                    out.physicalDevice = candidate;
                    break;
                }
            }
        }
        out.adapterName = std::string(
            out.physicalDevice.getProperties().deviceName.data());

        // NRI capability contract item 5. Two halves:
        //   (a) VK 1.3 was already an implicit requirement -- chaining
        //       VkPhysicalDeviceVulkan13Features below is illegal on an
        //       older device -- but nothing asserted it, so a 1.2 GPU
        //       failed as an opaque createDevice error. Say it plainly.
        //   (b) the minor recorded here is what the wrapper desc must
        //       carry: NRI reads minorVersion verbatim (it drives promoted-
        //       feature chaining AND VMA's vulkanApiVersion) and the
        //       correct value is min(instance minor, physical minor)
        //       -- contract §6 concern 8.
        const uint32_t physicalApiMinor =
            VK_API_VERSION_MINOR(out.physicalDevice.getProperties().apiVersion);
        if (physicalApiMinor < kApiMinorVersion)
        {
            ARC_ERROR("Vulkan physical device '{}' reports API 1.{}; 1.{} is "
                      "required (Vulkan 1.3 core feature structs are chained "
                      "at device creation)",
                      out.adapterName, physicalApiMinor, kApiMinorVersion);
            return false;
        }
        out.apiMinorVersion = std::min(physicalApiMinor, kApiMinorVersion);

        auto families = out.physicalDevice.getQueueFamilyProperties();
        for (uint32_t i = 0; i < (uint32_t)families.size(); ++i)
        {
            if (families[i].queueFlags & vk::QueueFlagBits::eGraphics)
            {
                out.graphicsQueueFamily = (int)i;
                break;
            }
        }
        if (out.graphicsQueueFamily < 0)
        {
            ARC_ERROR("No graphics queue family");
            return false;
        }

        // ------------------------------------------------------------
        // F-5c step 1+2: device-extension availability sweep.
        // ------------------------------------------------------------
        // Device extensions were previously passed through UNFILTERED --
        // fine for VK_KHR_swapchain (required anyway; its absence should
        // fail loudly) but fatal as a policy for optional diagnostics.
        // This mirrors the instance-extension shape already used above.
        std::vector<const char*> deviceExtensions(
            std::begin(kDeviceExtensions), std::end(kDeviceExtensions));

        bool haveBufferMarker  = false;
        bool haveDeviceFaultExt = false;
        bool haveDeviceFaultKhr = false;
        // Contract items 2 + 3 ride the SAME enumeration -- one pass, and
        // the diagnostics branches below are untouched.
        bool havePushDescriptor = false;
        bool havePortabilitySubset = false;
        bool haveOptional[kOptionalDeviceExtensionCount] = {};
        bool deviceExtensionsEnumerated = true;
        try
        {
            for (const auto& ext : out.physicalDevice.enumerateDeviceExtensionProperties())
            {
                const std::string_view name(ext.extensionName);
                if (name == VK_AMD_BUFFER_MARKER_EXTENSION_NAME)  haveBufferMarker   = true;
                // F-5d lists only the EXT spelling, but the KHR promotion
                // is in the vendored headers too (vulkan_core.h:14372) and
                // a newer driver may expose only that one. Accept EITHER;
                // the feature struct and the query differ per spelling, so
                // which one won is recorded and carried to the backend.
                else if (name == VK_EXT_DEVICE_FAULT_EXTENSION_NAME) haveDeviceFaultExt = true;
                else if (name == VK_KHR_DEVICE_FAULT_EXTENSION_NAME) haveDeviceFaultKhr = true;

                // NRI capability contract item 2: hard-required, and
                // already in kDeviceExtensions -- this only decides whether
                // the failure is named or an opaque createDevice error.
                if (name == VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)
                    havePushDescriptor = true;
                if (name == kPortabilitySubset)
                    havePortabilitySubset = true;
                // NRI capability contract item 3: quality extensions, taken
                // only where advertised (see kOptionalDeviceExtensions).
                for (int i = 0; i < kOptionalDeviceExtensionCount; ++i)
                {
                    if (name == kOptionalDeviceExtensions[i])
                        haveOptional[i] = true;
                }
            }
        }
        catch (const vk::SystemError& e)
        {
            ARC_WARN("vk::PhysicalDevice::enumerateDeviceExtensionProperties threw: {}; "
                     "continuing without GPU-crash diagnostics extensions", e.what());
            // The item-2 check below is a better error message, not the
            // enforcement: VK_KHR_push_descriptor stays in the required
            // list, so createDevice still fails loudly on a device without
            // it. Suppress the named check rather than reject a device we
            // simply could not enumerate -- that is a regression this TU
            // has never had.
            deviceExtensionsEnumerated = false;
        }

        // NRI capability contract item 2 (contract item 14's rule: name the
        // capability). NRI calls vkCmdPushDescriptorSet through a pointer
        // it never null-checks, and the frame-graph design puts per-frame
        // data behind CmdSetRootDescriptor -- this is load-bearing.
        if (deviceExtensionsEnumerated && !havePushDescriptor)
        {
            ARC_ERROR("Vulkan device extension '{}' is unavailable on '{}'; it is "
                      "hard-required (NRI wrapper capability contract item 2)",
                      VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, out.adapterName);
            return false;
        }

        // NRI capability contract item 3: absence is a documented slower
        // path inside NRI, never a failure -- so append, don't require.
        // haveOptional[] stays the single predicate: what lands in
        // deviceExtensions here is exactly what gets its feature struct
        // chained below, so the list can never claim a capability the
        // device was not actually created with.
        for (int i = 0; i < kOptionalDeviceExtensionCount; ++i)
        {
            if (haveOptional[i])
                deviceExtensions.push_back(kOptionalDeviceExtensions[i]);
            else
                ARC_INFO("Vulkan quality extension '{}' unavailable; NRI will take "
                         "its documented fallback path",
                         kOptionalDeviceExtensions[i]);
        }

        // An extension being present is not the same as its feature being
        // supported, and vkCreateDevice fails if a chained feature struct
        // asks for a VK_FALSE feature. Ask before enabling.
        vk::PhysicalDeviceFaultFeaturesEXT faultFeaturesExt;
        vk::PhysicalDeviceFaultFeaturesKHR faultFeaturesKhr;
        if (haveDeviceFaultExt)
        {
            vk::PhysicalDeviceFeatures2 supported;
            supported.pNext = &faultFeaturesExt;
            out.physicalDevice.getFeatures2(&supported);
            faultFeaturesExt.pNext = nullptr;  // queried as a leaf; re-chained below
            if (!faultFeaturesExt.deviceFault) haveDeviceFaultExt = false;
        }
        if (haveDeviceFaultKhr)
        {
            vk::PhysicalDeviceFeatures2 supported;
            supported.pNext = &faultFeaturesKhr;
            out.physicalDevice.getFeatures2(&supported);
            faultFeaturesKhr.pNext = nullptr;
            if (!faultFeaturesKhr.deviceFault) haveDeviceFaultKhr = false;
        }
        // Prefer EXT when both are advertised: it is the mature spelling
        // (spec version 2 vs 1) and the one F-5d binds against.
        if (haveDeviceFaultExt) haveDeviceFaultKhr = false;

        auto deviceFaultSpelling = VulkanCrashDesc::DeviceFault::None;
        if (haveDeviceFaultExt)
        {
            deviceExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            deviceFaultSpelling = VulkanCrashDesc::DeviceFault::Ext;
        }
        else if (haveDeviceFaultKhr)
        {
            deviceExtensions.push_back(VK_KHR_DEVICE_FAULT_EXTENSION_NAME);
            deviceFaultSpelling = VulkanCrashDesc::DeviceFault::Khr;
        }
        if (haveBufferMarker)
            deviceExtensions.push_back(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);

        ARC_INFO("Vulkan GPU-crash extensions: VK_AMD_buffer_marker={}, device_fault={}",
                 haveBufferMarker ? "yes" : "no",
                 haveDeviceFaultExt ? "EXT" : (haveDeviceFaultKhr ? "KHR" : "no"));

        // NRI capability contract item 7: exactly one GRAPHICS family with
        // queueCount = 1. NRI clamps the queueNum a wrapper desc declares
        // against PHYSICAL family capacity, not against what we created, so
        // this count and the desc's must stay equal -- declaring more would
        // hand back queues that do not exist.
        const float priority = 1.0f;
        auto queueInfo = vk::DeviceQueueCreateInfo()
            .setQueueFamilyIndex((uint32_t)out.graphicsQueueFamily)
            .setQueueCount(1)
            .setPQueuePriorities(&priority);

        // NRI capability contract item 4: query-then-pass-back-verbatim,
        // the pattern NVIDIA's own wrapper sample uses
        // (NRISamples/Source/Wrapper.cpp:340-360). In wrapper mode NRI
        // derives its ENTIRE capability model from
        // vkGetPhysicalDeviceFeatures2 on the PHYSICAL device -- it never
        // asks the logical device what was actually enabled, and has no way
        // to. So anything the GPU reports and we fail to enable becomes an
        // over-reported nri::DeviceDesc and, for the subset NRI uses
        // unconditionally, invalid Vulkan usage on the first call. Enabling
        // exactly the supported set is the only shape that cannot lie.
        //
        // This replaces a hand-picked pair of structs, which enabled three
        // bits and left the whole core-1.0 block VK_FALSE while NRI would
        // have reported the physically-supported ones as available. Those
        // three bits are still load-bearing and are in the hard set asserted
        // below: timelineSemaphore (queue submits chain
        // vk::TimelineSemaphoreSubmitInfo), synchronization2
        // (vkCmdPipelineBarrier2), and dynamicRendering (pipelines are built
        // with VkPipelineRenderingCreateInfo instead of a VkRenderPass, or
        // vkCreateGraphicsPipelines fails VUID-06576).
        vk::PhysicalDeviceFeatures2        enabledFeatures2;
        vk::PhysicalDeviceVulkan11Features vulkan11Features;
        vk::PhysicalDeviceVulkan12Features vulkan12Features;
        vk::PhysicalDeviceVulkan13Features vulkan13Features;
        enabledFeatures2.setPNext(&vulkan11Features);
        vulkan11Features.setPNext(&vulkan12Features);
        vulkan12Features.setPNext(&vulkan13Features);
        vulkan13Features.setPNext(nullptr);

        // The chain grows past vulkan13Features below (item 3's feature
        // structs, then the device-fault struct), so the tail is tracked
        // rather than assumed -- appending to a stale tail would silently
        // drop everything already attached there.
        void** chainTail = &vulkan13Features.pNext;
        const auto chainAppend = [&chainTail](auto& feature) {
            feature.pNext = nullptr;
            *chainTail = &feature;
            chainTail = &feature.pNext;
        };

        // NRI capability contract item 3, second half. At VK 1.3 the
        // maintenance5/6 FEATURE bits live in their own KHR structs, which
        // none of the promoted-to-core structs above carries -- so taking
        // the extension in the sweep without chaining these would create a
        // device with the extension enabled and its feature VK_FALSE. That
        // is precisely the meta-rule's failure mode, and NRI walks straight
        // into it: it gates feature-struct chaining on OUR extension-name
        // list (SharedVK.h:28-31) but sets m_IsSupported.maintenance5/6
        // from PHYSICAL support (DeviceVK.hpp:657-658, :673-674), then
        // calls vkCmdBindIndexBuffer2 / vkCmdBindDescriptorSets2 /
        // vkCmdPushConstants2 (CommandBufferVK.hpp:872-876, :928, :966) and
        // sets VMA's KHR_MAINTENANCE5 bit (DeviceVK.hpp:225) -- all of
        // which need the feature enabled at creation. Chained BEFORE the
        // query so they are filled and passed back verbatim, exactly as
        // item 4 does for the core structs. (VK_EXT_memory_budget needs
        // nothing here: it has no feature struct at all.)
        vk::PhysicalDeviceMaintenance5FeaturesKHR maintenance5Features;
        vk::PhysicalDeviceMaintenance6FeaturesKHR maintenance6Features;
        if (haveOptional[kOptMaintenance5])
            chainAppend(maintenance5Features);
        if (haveOptional[kOptMaintenance6])
            chainAppend(maintenance6Features);

        // A portability device (MoltenVK): the extension is mandatory once
        // enumerated, and its subset features (triangle fans, image-view
        // swizzles, separate stencil refs, ...) are queried and enabled
        // verbatim -- what the device reports VK_FALSE stays a gap NRI and the
        // engine must not use (docs/research/2026-10-07-macos-moltenvk-port.md
        // lists them). The raw struct, not vk::: VK_KHR_portability_subset is
        // a provisional (beta) extension the C++ bindings only expose with
        // VK_ENABLE_BETA_EXTENSIONS.
        VkPhysicalDevicePortabilitySubsetFeaturesKHR portabilityFeatures{};
        portabilityFeatures.sType = static_cast<VkStructureType>(1000163000);   // VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR
        if (havePortabilitySubset)
        {
            deviceExtensions.push_back(kPortabilitySubset);
            portabilityFeatures.pNext = nullptr;
            *chainTail = &portabilityFeatures;
            chainTail = &portabilityFeatures.pNext;
        }

        out.physicalDevice.getFeatures2(&enabledFeatures2);

        // Record every portability gap on the device log, so a MoltenVK run
        // names what it cannot do before anything trips over it.
        if (havePortabilitySubset)
        {
            const struct { VkBool32 supported; const char* name; } kSubset[] = {
                { portabilityFeatures.constantAlphaColorBlendFactors,         "constantAlphaColorBlendFactors"         },
                { portabilityFeatures.events,                                 "events"                                 },
                { portabilityFeatures.imageViewFormatReinterpretation,        "imageViewFormatReinterpretation"        },
                { portabilityFeatures.imageViewFormatSwizzle,                 "imageViewFormatSwizzle"                 },
                { portabilityFeatures.imageView2DOn3DImage,                   "imageView2DOn3DImage"                   },
                { portabilityFeatures.multisampleArrayImage,                  "multisampleArrayImage"                  },
                { portabilityFeatures.mutableComparisonSamplers,              "mutableComparisonSamplers"              },
                { portabilityFeatures.pointPolygons,                          "pointPolygons"                          },
                { portabilityFeatures.samplerMipLodBias,                      "samplerMipLodBias"                      },
                { portabilityFeatures.separateStencilMaskRef,                 "separateStencilMaskRef"                 },
                { portabilityFeatures.shaderSampleRateInterpolationFunctions, "shaderSampleRateInterpolationFunctions" },
                { portabilityFeatures.tessellationIsolines,                   "tessellationIsolines"                   },
                { portabilityFeatures.tessellationPointMode,                  "tessellationPointMode"                  },
                { portabilityFeatures.triangleFans,                           "triangleFans"                           },
                { portabilityFeatures.vertexAttributeAccessBeyondStride,      "vertexAttributeAccessBeyondStride"      },
            };
            std::string gaps;
            for (const auto& feature : kSubset)
            {
                if (feature.supported)
                    continue;
                if (!gaps.empty())
                    gaps += ", ";
                gaps += feature.name;
            }
            ARC_INFO("Vulkan portability device '{}': VK_KHR_portability_subset gaps: {}",
                     out.adapterName, gaps.empty() ? std::string("none") : gaps);
        }

        // NRI capability contract item 14's rule applied to item 4's hard
        // set: keyed off the PHYSICAL query, so the message names the
        // capability the GPU does not offer rather than surfacing as a
        // VUID on the first submit/buffer/query-reset. Every one of these
        // is either used unconditionally by NRI or, worse, invisible to it
        // (timelineSemaphore and hostQueryReset appear nowhere in NRI's
        // source -- there is no capability bit that could degrade).
        const struct { bool supported; const char* name; } kHardFeatures[] = {
            { (bool)vulkan12Features.timelineSemaphore,   "timelineSemaphore"   },
            { (bool)vulkan12Features.bufferDeviceAddress, "bufferDeviceAddress" },
            { (bool)vulkan12Features.hostQueryReset,      "hostQueryReset"      },
            { (bool)vulkan13Features.synchronization2,    "synchronization2"    },
            { (bool)vulkan13Features.dynamicRendering,    "dynamicRendering"    },
            { (bool)vulkan13Features.maintenance4,        "maintenance4"        },
        };
        bool hardFeaturesPresent = true;
        for (const auto& feature : kHardFeatures)
        {
            if (feature.supported)
                continue;
            ARC_ERROR("Vulkan feature '{}' is not supported by '{}'; it is "
                      "hard-required (NRI wrapper capability contract item 4)",
                      feature.name, out.adapterName);
            hardFeaturesPresent = false;
        }
        if (!hardFeaturesPresent)
            return false;

        // F-5c step 4: VK_EXT/KHR_device_fault requires its feature to be
        // enabled at device creation, so the matching struct is appended
        // to the TAIL of the existing chain (deviceInfo -> features2 ->
        // vulkan11 -> vulkan12 -> vulkan13 -> [maintenance5] ->
        // [maintenance6] -> fault; contract item 4 keeps it at the tail,
        // and it is queried as a leaf above so it never rides the
        // enable-what-is-supported chain -- which is why it appends AFTER
        // the query rather than before it like the maintenance structs).
        // The two structs are distinct types, not aliases -- chain the one
        // whose spelling was enabled above.
        // deviceFaultVendorBinary is opt-in on top and is what makes the
        // opaque vendor blob retrievable; take it only where advertised.
        if (deviceFaultSpelling == VulkanCrashDesc::DeviceFault::Ext)
        {
            faultFeaturesExt.deviceFault = VK_TRUE;
            chainAppend(faultFeaturesExt);
        }
        else if (deviceFaultSpelling == VulkanCrashDesc::DeviceFault::Khr)
        {
            faultFeaturesKhr.deviceFault = VK_TRUE;
            chainAppend(faultFeaturesKhr);
        }

        // pEnabledFeatures stays null (Vulkan-Hpp's default): the core-1.0
        // block travels in enabledFeatures2.features, and the two are
        // mutually exclusive.
        auto deviceInfo = vk::DeviceCreateInfo()
            .setQueueCreateInfoCount(1)
            .setPQueueCreateInfos(&queueInfo)
            .setEnabledExtensionCount((uint32_t)deviceExtensions.size())
            .setPpEnabledExtensionNames(deviceExtensions.data())
            .setPNext(&enabledFeatures2);

        try
        {
            out.device = out.physicalDevice.createDevice(deviceInfo);
        }
        catch (const vk::SystemError& e)
        {
            ARC_ERROR("vk::PhysicalDevice::createDevice failed: {}", e.what());
            return false;
        }

        // The per-module default dispatcher binds ONE VkDevice: a second
        // simultaneous Vulkan device would re-point the device-level
        // entry points and corrupt the first. Fine by design -- one
        // engine instance per process (see VulkanDispatchStorage.cpp).
        VULKAN_HPP_DEFAULT_DISPATCHER.init(out.device);
        out.graphicsQueue = out.device.getQueue((uint32_t)out.graphicsQueueFamily, 0);

        // NRI capability contract item 6: the extension lists become
        // members the moment the device they describe exists, so every
        // consumer reads ONE list rather than a copy that can drift. NRI
        // takes the device list verbatim in wrapper mode -- it is the sole
        // gate on which device-level entry points it resolves and which
        // extension feature structs it chains -- so "what we actually
        // enabled" has to survive past this function.
        //
        // NRI capability contract item 8 (the value, not the wiring -- the
        // field lives in the wrapper desc): nri::VKBindingOffsets is
        // {sRegister, tRegister, bRegister, uRegister} and ours must equal
        // the dxc -fvk-*-shift values in ShaderConventions.hpp's
        // kSpirvArgs, i.e. {s=128, t=0, b=256, u=384} -- NOT NRI's
        // reference {0, 128, 32, 64}. Change them in ShaderConventions.hpp
        // first and fan out, as that header says.
        out.enabledInstanceExtensions = instanceExtensions;
        out.enabledDeviceExtensions   = deviceExtensions;
        // The GPU-crash record travels with the rest of "what this device was
        // created with", for the same reason: it is decided here and consumed
        // after the split (VulkanCrashDesc, below). Neither value changes
        // after the sweep above.
        out.deviceFault  = deviceFaultSpelling;
        out.bufferMarker = haveBufferMarker;

        return true;
    }

    // What the tail of ~DeviceVulkan was, verbatim and in the same order:
    // device, then debug messenger, then instance. Handles are nulled as they
    // go so an owner can call this at the point its ordering requires
    // (contract item 15: the NRI device is destroyed BEFORE this runs) and
    // still hold the struct afterwards.
    void DestroyVulkanNativeDevice(VulkanDeviceCreation& creation)
    {
        if (creation.device)
        {
            creation.device.destroy();
            creation.device = nullptr;
        }
        if (creation.debugMessenger)
        {
            creation.instance.destroyDebugUtilsMessengerEXT(creation.debugMessenger);
            creation.debugMessenger = nullptr;
        }
        if (creation.instance)
        {
            creation.instance.destroy();
            creation.instance = nullptr;
        }
    }

    // The narrow export (DeviceRemovedObservers.hpp): the SAME observer
    // above, reachable BY ADDRESS from the Render module's installer. One
    // line, no state, no second observation point -- the once-only
    // `g_deviceRemovedReported` latch, the "gpu-crash: device removed" wording
    // and the NoteGpuDeviceLost ordering all stay in ObserveDeviceRemoved,
    // file-local.
    //
    // IT STAYS A SEPARATE FUNCTION FROM THE OBSERVER. Folding the
    // body up into this name would be a behaviour change, not a cleanup:
    // NriDiagnostics::Disarm clears the hook slot only when it still holds
    // the address Arm installed, and that address is THIS function's.
    void ObserveDeviceRemovedVulkan()
    {
        ObserveDeviceRemoved();
    }

    // Its twin (DeviceRemovedObservers.hpp): the store DeviceVulkan::Init used
    // to make one line above its own ResetGpuDeviceLost(). Since Task 8b
    // deleted that class, NriDiagnostics::Arm is the ONLY arming site left and
    // this is its only way to reach a latch that is deliberately file-local.
    void ResetDeviceRemovedLatchVulkan()
    {
        g_deviceRemovedReported.store(false, std::memory_order_release);
    }
}
