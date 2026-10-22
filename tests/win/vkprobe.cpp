// SPDX-License-Identifier: MIT
// Guest probe: loads a Vulkan ICD directly (as the UMDs do, no loader) and reports, for each physical device,
// whether it meets vkd3d-proton's hard device requirements and what D3D12 features beyond them would need
// (tiled resources, feature levels 12_0-12_2), then the same for DXVK by the profiles in its
// VP_DXVK_requirements.json (D3D11 levels 10_1-12_1, D3D9). Plain-text output.
//   vkprobe [path\to\icd.dll]   default: ..\..\icd\vgd_vulkan_virtio.dll next to this exe (the share layout)
//   --extensions                also list every device extension
#include <windows.h>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_missing = 0;

void check(const char* what, bool ok)
{
    std::printf("  [%s] %s\n", ok ? " ok " : "MISS", what);
    if (!ok)
        g_missing++;
}

void info(const char* what, bool ok) { std::printf("  [%s] %s\n", ok ? "yes " : " no ", what); }

bool has(const std::vector<VkExtensionProperties>& exts, const char* name)
{
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, name))
            return true;
    return false;
}

template <typename T> void chain(VkPhysicalDeviceFeatures2& f2, T& s)
{
    s.pNext = f2.pNext;
    f2.pNext = &s;
}

template <typename T> void chain(VkPhysicalDeviceProperties2& p2, T& s)
{
    s.pNext = p2.pNext;
    p2.pNext = &s;
}

std::wstring default_icd()
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    std::wstring path(buf, n);
    return path.substr(0, path.find_last_of(L"\\/") + 1) + L"..\\..\\icd\\vgd_vulkan_virtio.dll";
}

} // namespace

int main(int argc, char** argv)
{
    std::wstring icd = default_icd();
    bool list_extensions = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--extensions"))
            list_extensions = true;
        else
            icd = std::wstring(argv[i], argv[i] + std::strlen(argv[i]));
    }

    HMODULE mod = LoadLibraryExW(icd.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!mod) {
        std::printf("LoadLibrary(%ls) failed: %lu\n", icd.c_str(), GetLastError());
        return 1;
    }
    using Negotiate = VkResult(VKAPI_PTR*)(uint32_t*);
    auto negotiate = reinterpret_cast<Negotiate>(
        reinterpret_cast<void*>(GetProcAddress(mod, "vk_icdNegotiateLoaderICDInterfaceVersion")));
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void*>(GetProcAddress(mod, "vk_icdGetInstanceProcAddr")));
    if (!gipa) {
        std::printf("%ls exports no vk_icdGetInstanceProcAddr\n", icd.c_str());
        return 1;
    }
    uint32_t version = 5;
    if (negotiate)
        negotiate(&version);
    std::printf("ICD %ls (loader interface %u)\n", icd.c_str(), version);

    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vkprobe";
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = create_instance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) {
        std::printf("vkCreateInstance: %d\n", r);
        return 1;
    }
#define GET(name) auto name = reinterpret_cast<PFN_##name>(gipa(instance, #name))
    GET(vkEnumeratePhysicalDevices);
    GET(vkGetPhysicalDeviceProperties2);
    GET(vkGetPhysicalDeviceFeatures2);
    GET(vkEnumerateDeviceExtensionProperties);
    GET(vkGetPhysicalDeviceQueueFamilyProperties);
    GET(vkDestroyInstance);
#undef GET

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    for (uint32_t d = 0; d < count; d++) {
        const VkPhysicalDevice pd = devices[d];
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());

        VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        VkPhysicalDeviceVulkan12Properties p12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceTransformFeedbackPropertiesEXT xfb{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        chain(p2, p11);
        chain(p2, p12);
        chain(p2, p13);
        const bool xfb_ext = has(exts, VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
        if (xfb_ext)
            chain(p2, xfb);
        VkPhysicalDeviceConservativeRasterizationPropertiesEXT cons{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT};
        const bool cons_ext = has(exts, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME);
        if (cons_ext)
            chain(p2, cons);
        vkGetPhysicalDeviceProperties2(pd, &p2);
        const VkPhysicalDeviceProperties& p = p2.properties;
        const bool v14 = p.apiVersion >= VK_API_VERSION_1_4;

        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
        VkPhysicalDeviceRobustness2FeaturesKHR rob{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR};
        VkPhysicalDeviceTransformFeedbackFeaturesEXT xfbf{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT};
        VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mut{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT};
        VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT rov{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
        VkPhysicalDeviceMeshShaderFeaturesEXT mesh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
        VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rt{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR as{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceFragmentShadingRateFeaturesKHR vrs{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR};
        VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceMaintenance6FeaturesKHR m6{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
        VkPhysicalDeviceDepthClipEnableFeaturesEXT clip{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT};
        VkPhysicalDeviceCustomBorderColorFeaturesEXT border{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
        VkPhysicalDeviceDepthBiasControlFeaturesEXT bias{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDeviceMultiDrawFeaturesEXT multi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT};
        VkPhysicalDeviceMemoryPriorityFeaturesEXT prio{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PRIORITY_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        chain(f2, f11);
        chain(f2, f12);
        chain(f2, f13);
        if (v14)
            chain(f2, f14);
        const bool rob_ext = has(exts, VK_KHR_ROBUSTNESS_2_EXTENSION_NAME) ||
                             has(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        if (rob_ext)
            chain(f2, rob);
        if (xfb_ext)
            chain(f2, xfbf);
        const bool mut_ext = has(exts, VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME) ||
                             has(exts, VK_VALVE_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME);
        if (mut_ext)
            chain(f2, mut);
        const bool rov_ext = has(exts, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
        if (rov_ext)
            chain(f2, rov);
        const bool mesh_ext = has(exts, VK_EXT_MESH_SHADER_EXTENSION_NAME);
        if (mesh_ext)
            chain(f2, mesh);
        const bool rq_ext = has(exts, VK_KHR_RAY_QUERY_EXTENSION_NAME);
        if (rq_ext)
            chain(f2, rq);
        const bool rt_ext = has(exts, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (rt_ext)
            chain(f2, rt);
        const bool as_ext = has(exts, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        if (as_ext)
            chain(f2, as);
        const bool vrs_ext = has(exts, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        if (vrs_ext)
            chain(f2, vrs);
        const bool m5_ext = has(exts, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        if (m5_ext)
            chain(f2, m5);
        const bool m6_ext = has(exts, VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
        if (m6_ext)
            chain(f2, m6);
        const bool clip_ext = has(exts, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
        if (clip_ext)
            chain(f2, clip);
        const bool border_ext = has(exts, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        if (border_ext)
            chain(f2, border);
        const bool bias_ext = has(exts, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
        if (bias_ext)
            chain(f2, bias);
        const bool eds3_ext = has(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        if (eds3_ext)
            chain(f2, eds3);
        const bool multi_ext = has(exts, VK_EXT_MULTI_DRAW_EXTENSION_NAME);
        if (multi_ext)
            chain(f2, multi);
        const bool prio_ext = has(exts, VK_EXT_MEMORY_PRIORITY_EXTENSION_NAME);
        if (prio_ext)
            chain(f2, prio);
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        const VkPhysicalDeviceFeatures& f = f2.features;

        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, queues.data());
        bool graphics = false, sparse_queue = false;
        for (const auto& q : queues) {
            graphics |= (q.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            sparse_queue |= (q.queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) && q.queueCount > 0;
        }

        std::printf("\nDevice %u: %s (Vulkan %u.%u.%u, %s %s), %u extensions\n", d, p.deviceName,
                    VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                    VK_API_VERSION_PATCH(p.apiVersion), p12.driverName, p12.driverInfo, ext_count);
        for (uint32_t i = 0; i < qcount; i++)
            std::printf("  queue family %u: flags 0x%x count %u\n", i, queues[i].queueFlags, queues[i].queueCount);

        g_missing = 0;
        std::printf(" vkd3d-proton hard requirements:\n");
        check("Vulkan 1.3", p.apiVersion >= VK_API_VERSION_1_3);
        check("graphics queue", graphics);
        check("samplerMirrorClampToEdge", f12.samplerMirrorClampToEdge);
        check("robustness2: robustBufferAccess2 + robustImageAccess2",
              rob_ext && rob.robustBufferAccess2 && rob.robustImageAccess2);
        check("robustness2: nullDescriptor", rob_ext && rob.nullDescriptor);
        check("shaderDrawParameters", f11.shaderDrawParameters);
        check("push descriptors", (v14 && f14.pushDescriptor) || has(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME));
        check("maintenance5", (v14 && f14.maintenance5) || has(exts, VK_KHR_MAINTENANCE_5_EXTENSION_NAME));
        check("maintenance6", (v14 && f14.maintenance6) || has(exts, VK_KHR_MAINTENANCE_6_EXTENSION_NAME));
        check("vertex attribute divisor (+ zero divisor)",
              v14 && f14.vertexAttributeInstanceRateDivisor && f14.vertexAttributeInstanceRateZeroDivisor);
        check("transform feedback + queries", xfb_ext && xfbf.transformFeedback && xfb.transformFeedbackQueries);
        check("single-texel buffer offset alignment (storage + uniform)",
              (p13.storageTexelBufferOffsetSingleTexelAlignment || p13.storageTexelBufferOffsetAlignmentBytes == 1) &&
                  (p13.uniformTexelBufferOffsetSingleTexelAlignment || p13.uniformTexelBufferOffsetAlignmentBytes == 1));
        std::printf(" vkd3d-proton expects in practice (descriptor model, submission):\n");
        check("mutable descriptor type", mut_ext && mut.mutableDescriptorType);
        check("descriptor indexing (runtime arrays, partially bound, update after bind)",
              f12.descriptorIndexing && f12.runtimeDescriptorArray && f12.descriptorBindingPartiallyBound &&
                  f12.descriptorBindingSampledImageUpdateAfterBind && f12.descriptorBindingStorageImageUpdateAfterBind &&
                  f12.descriptorBindingStorageBufferUpdateAfterBind &&
                  f12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
                  f12.descriptorBindingStorageTexelBufferUpdateAfterBind);
        check("bufferDeviceAddress", f12.bufferDeviceAddress);
        check("timelineSemaphore", f12.timelineSemaphore);
        check("synchronization2 + dynamicRendering", f13.synchronization2 && f13.dynamicRendering);
        info("VK_EXT_descriptor_buffer (faster descriptor path, optional)",
             has(exts, VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME));
        info("VK_EXT_descriptor_heap (our UMD's descriptor path)", has(exts, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME));
        const int hard_missing = g_missing;

        const VkPhysicalDeviceSparseProperties& sp = p.sparseProperties;
        std::printf(" tiled resources:\n");
        info("tier 1: sparseBinding, sparseResidencyBuffer/Image2D/Aliased, standard 2D block shape, sparse queue",
             f.sparseBinding && f.sparseResidencyBuffer && f.sparseResidencyImage2D && f.sparseResidencyAliased &&
                 sp.residencyStandard2DBlockShape && sparse_queue);
        info("tier 2: + shaderResourceResidency, shaderResourceMinLod, nonResidentStrict, no aligned mip size, "
             "minmax single-component filtering",
             f.shaderResourceResidency && f.shaderResourceMinLod && sp.residencyNonResidentStrict &&
                 !sp.residencyAlignedMipSize && p12.filterMinmaxSingleComponentFormats);
        info("tier 3/4: + sparseResidencyImage3D, standard 3D block shape",
             f.sparseResidencyImage3D && sp.residencyStandard3DBlockShape);
        std::printf("   sparseBinding %u buffer %u image2D %u image3D %u aliased %u; residency: 2D %u 3D %u strict %u "
                    "alignedMip %u; shaderResidency %u minLod %u; sparse queue %u\n",
                    f.sparseBinding, f.sparseResidencyBuffer, f.sparseResidencyImage2D, f.sparseResidencyImage3D,
                    f.sparseResidencyAliased, sp.residencyStandard2DBlockShape, sp.residencyStandard3DBlockShape,
                    sp.residencyNonResidentStrict, sp.residencyAlignedMipSize, f.shaderResourceResidency,
                    f.shaderResourceMinLod, sparse_queue);
        std::printf(" beyond 12_0:\n");
        info("12_1: ROVs (fragment shader interlock)", rov_ext && rov.fragmentShaderPixelInterlock);
        info("12_1: conservative rasterization", has(exts, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME));
        info("12_2: mesh shaders", mesh_ext && mesh.meshShader && mesh.taskShader);
        info("12_2: ray tracing (pipeline + ray query + acceleration structures)",
             rt_ext && rt.rayTracingPipeline && rq_ext && rq.rayQuery && as_ext && as.accelerationStructure);
        info("12_2: variable rate shading", vrs_ext && vrs.pipelineFragmentShadingRate &&
                                                vrs.attachmentFragmentShadingRate);
        info("64-bit shader atomics (SM 6.6)", f12.shaderBufferInt64Atomics);
        std::printf(" => %s\n", hard_missing ? "vkd3d-proton would not run on this device"
                                             : "vkd3d-proton's requirements are met");

        // DXVK: the capability blocks of VP_DXVK_requirements.json. MISS = required by the profile it is listed
        // under; "no" = optional (DXVK has a slower path or reports the feature off).
        const bool m5_ok = (v14 && f14.maintenance5) || (m5_ext && m5.maintenance5);
        const bool m6_ok = (v14 && f14.maintenance6) || (m6_ext && m6.maintenance6);
        const bool divisor_ok = v14 && f14.vertexAttributeInstanceRateDivisor &&
                                f14.vertexAttributeInstanceRateZeroDivisor;
        g_missing = 0;
        std::printf(" DXVK common (Vulkan 1.3 core + dxvk_common_required):\n");
        check("Vulkan 1.3", p.apiVersion >= VK_API_VERSION_1_3);
        check("robustBufferAccess", f.robustBufferAccess);
        check("1.1: multiview (6 views)", f11.multiview && p11.maxMultiviewViewCount >= 6);
        check("1.2: uniformBufferStandardLayout, subgroupBroadcastDynamicId, imagelessFramebuffer, "
              "separateDepthStencilLayouts, hostQueryReset, timelineSemaphore, shaderSubgroupExtendedTypes",
              f12.uniformBufferStandardLayout && f12.subgroupBroadcastDynamicId && f12.imagelessFramebuffer &&
                  f12.separateDepthStencilLayouts && f12.hostQueryReset && f12.timelineSemaphore &&
                  f12.shaderSubgroupExtendedTypes);
        check("1.2: vulkanMemoryModel (+ device scope), bufferDeviceAddress",
              f12.vulkanMemoryModel && f12.vulkanMemoryModelDeviceScope && f12.bufferDeviceAddress);
        check("1.3: robustImageAccess, shaderTerminateInvocation, zero-initialized workgroup memory, synchronization2, "
              "integer dot product, maintenance4, pipelineCreationCacheControl, subgroupSizeControl, "
              "computeFullSubgroups, demote to helper, inlineUniformBlock, dynamicRendering",
              f13.robustImageAccess && f13.shaderTerminateInvocation && f13.shaderZeroInitializeWorkgroupMemory &&
                  f13.synchronization2 && f13.shaderIntegerDotProduct && f13.maintenance4 &&
                  f13.pipelineCreationCacheControl && f13.subgroupSizeControl && f13.computeFullSubgroups &&
                  f13.shaderDemoteToHelperInvocation && f13.inlineUniformBlock && f13.dynamicRendering);
        check("fragmentStoresAndAtomics, samplerAnisotropy, shaderInt16, sampled image array dynamic indexing",
              f.fragmentStoresAndAtomics && f.samplerAnisotropy && f.shaderInt16 &&
                  f.shaderSampledImageArrayDynamicIndexing);
        check("16/8-bit storage, shaderInt8, shaderDrawParameters",
              f11.storageBuffer16BitAccess && f12.storageBuffer8BitAccess && f12.shaderInt8 &&
                  f11.shaderDrawParameters);
        check("descriptor indexing (sampled image update after bind, update unused while pending, partially bound, "
              "runtime arrays)",
              f12.descriptorIndexing && f12.descriptorBindingSampledImageUpdateAfterBind &&
                  f12.descriptorBindingUpdateUnusedWhilePending && f12.descriptorBindingPartiallyBound &&
                  f12.runtimeDescriptorArray);
        check("maintenance5 + maintenance6", m5_ok && m6_ok);
        check("robustness2: nullDescriptor + robustBufferAccess2", rob_ext && rob.nullDescriptor && rob.robustBufferAccess2);
        check("depth clip enable", clip_ext && clip.depthClipEnable);
        check("load/store op none", v14 || has(exts, VK_KHR_LOAD_STORE_OP_NONE_EXTENSION_NAME) ||
                                        has(exts, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME));
        check("maxPushConstantsSize >= 256", p.limits.maxPushConstantsSize >= 256);
        const int dxvk_common_missing = g_missing;

        g_missing = 0;
        std::printf(" DXVK D3D11 baseline (D3D10.1 / 11):\n");
        check("transform feedback + geometry streams", xfb_ext && xfbf.transformFeedback && xfbf.geometryStreams);
        check("depthBiasClamp, depthClamp, dualSrcBlend, fillModeNonSolid, fullDrawIndexUint32, geometryShader, "
              "imageCubeArray, independentBlend, multiViewport, occlusionQueryPrecise, sampleRateShading",
              f.depthBiasClamp && f.depthClamp && f.dualSrcBlend && f.fillModeNonSolid && f.fullDrawIndexUint32 &&
                  f.geometryShader && f.imageCubeArray && f.independentBlend && f.multiViewport &&
                  f.occlusionQueryPrecise && f.sampleRateShading);
        check("clip + cull distance, shaderImageGatherExtended, BC compression, samplerMirrorClampToEdge",
              f.shaderClipDistance && f.shaderCullDistance && f.shaderImageGatherExtended && f.textureCompressionBC &&
                  f12.samplerMirrorClampToEdge);
        const int d3d11_missing = dxvk_common_missing + g_missing;
        g_missing = 0;
        check("11_0: drawIndirectFirstInstance, multiDrawIndirect, tessellationShader",
              f.drawIndirectFirstInstance && f.multiDrawIndirect && f.tessellationShader);
        const int l110_missing = d3d11_missing + g_missing;
        g_missing = 0;
        check("11_1: logicOp, vertexPipelineStoresAndAtomics", f.logicOp && f.vertexPipelineStoresAndAtomics);
        const int l111_missing = l110_missing + g_missing;
        g_missing = 0;
        check("12_0: sparse binding + residency (buffer, image2D, aliased), shaderResourceResidency/MinLod, "
              "samplerFilterMinmax, standard 2D blocks, strict non-resident, no aligned mip size",
              f.sparseBinding && f.sparseResidencyBuffer && f.sparseResidencyImage2D && f.sparseResidencyAliased &&
                  f.shaderResourceResidency && f.shaderResourceMinLod && f12.samplerFilterMinmax &&
                  sp.residencyStandard2DBlockShape && sp.residencyNonResidentStrict && !sp.residencyAlignedMipSize);
        const int l120_missing = l111_missing + g_missing;
        g_missing = 0;
        check("12_1: conservative rasterization (degenerate triangles, fully covered input) + pixel interlock",
              cons_ext && cons.degenerateTrianglesRasterized && cons.fullyCoveredFragmentShaderInputVariable &&
                  rov_ext && rov.fragmentShaderPixelInterlock);
        const int l121_missing = l120_missing + g_missing;
        std::printf(" DXVK optional (faster paths, more features):\n");
        info("custom border color (+ without format)",
             border_ext && border.customBorderColors && border.customBorderColorWithoutFormat);
        info("depth bias control", bias_ext && bias.depthBiasControl && bias.depthBiasExact);
        info("extended dynamic state 3 (alpha to coverage, depth clip, samples, sample mask)",
             eds3_ext && eds3.extendedDynamicState3AlphaToCoverageEnable &&
                 eds3.extendedDynamicState3DepthClipEnable && eds3.extendedDynamicState3RasterizationSamples &&
                 eds3.extendedDynamicState3SampleMask);
        info("vertex attribute divisor (+ zero)", divisor_ok);
        info("multi draw", multi_ext && multi.multiDraw);
        info("memory priority", prio_ext && prio.memoryPriority);
        info("graphics pipeline library (async pipelines, less stutter)",
             has(exts, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME));
        info("VK_EXT_descriptor_heap", has(exts, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME));
        info("depthBounds, pipelineStatisticsQuery, shaderFloat64, shaderInt64",
             f.depthBounds && f.pipelineStatisticsQuery && f.shaderFloat64 && f.shaderInt64);
        const char* level = !l121_missing   ? "12_1"
                            : !l120_missing ? "12_0"
                            : !l111_missing ? "11_1"
                            : !l110_missing ? "11_0"
                            : !d3d11_missing ? "10_1"
                                             : nullptr;
        if (level)
            std::printf(" => DXVK D3D11 runs here, up to feature level %s\n", level);
        else
            std::printf(" => DXVK D3D11 would not run on this device\n");
        g_missing = 0;
        std::printf(" DXVK D3D9 baseline (beyond the common set):\n");
        check("geometryShader, imageCubeArray, depthClamp, depthBiasClamp, fillModeNonSolid, sampleRateShading, "
              "clip + cull distance, BC compression, occlusionQueryPrecise, independentBlend, fullDrawIndexUint32, "
              "shaderImageGatherExtended, samplerMirrorClampToEdge",
              f.geometryShader && f.imageCubeArray && f.depthClamp && f.depthBiasClamp && f.fillModeNonSolid &&
                  f.sampleRateShading && f.shaderClipDistance && f.shaderCullDistance && f.textureCompressionBC &&
                  f.occlusionQueryPrecise && f.independentBlend && f.fullDrawIndexUint32 &&
                  f.shaderImageGatherExtended && f12.samplerMirrorClampToEdge);
        std::printf(" => %s\n", dxvk_common_missing + g_missing ? "DXVK D3D9 would not run on this device"
                                                                  : "DXVK D3D9 runs here");

        if (list_extensions)
            for (const auto& e : exts)
                std::printf("    %s (rev %u)\n", e.extensionName, e.specVersion);
    }
    vkDestroyInstance(instance, nullptr);
    return 0;
}
