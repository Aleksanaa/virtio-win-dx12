// SPDX-License-Identifier: MIT
// Query heaps on Vulkan query pools. D3D12 queries need no reset and may be resolved whatever happened to them, so
// every query is made available when its heap is created, and reset only right before a begin (or a timestamp) that
// is really recorded: a resolve waits for results, and must never wait for a query nothing will end.
#include "internal.h"

#include <query_dxil.h>

#include <array>
#include <cstring>
#include <numeric>

namespace vgd {

namespace {

// D3D12_QUERY_DATA_PIPELINE_STATISTICS is Vulkan's eleven core statistics in Vulkan's bit order.
constexpr VkQueryPipelineStatisticFlags kPipelineStatistics =
    VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_VERTICES_BIT | VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_GEOMETRY_SHADER_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_CONTROL_SHADER_PATCHES_BIT |
    VK_QUERY_PIPELINE_STATISTIC_TESSELLATION_EVALUATION_SHADER_INVOCATIONS_BIT |
    VK_QUERY_PIPELINE_STATISTIC_COMPUTE_SHADER_INVOCATIONS_BIT;
static_assert(sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS) == 11 * 8);

// The heap type a query type needs.
bool matches(D3D12_QUERY_HEAP_TYPE heap, D3D12_QUERY_TYPE type)
{
    switch (type) {
    case D3D12_QUERY_TYPE_OCCLUSION:
    case D3D12_QUERY_TYPE_BINARY_OCCLUSION: return heap == D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    case D3D12_QUERY_TYPE_TIMESTAMP: return heap == D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: return heap == D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS;
    default: return false;
    }
}

// Fixup modes (query.hlsl).
constexpr uint32_t kFixupBinary = 0, kFixupScale = 1;

} // namespace

// --- Heaps -------------------------------------------------------------------------------------------------

QueryHeap::Impl::~Impl()
{
    if (pool)
        device->vk.vkDestroyQueryPool(device->device, pool, nullptr);
}

QueryHeap::QueryHeap(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
QueryHeap::~QueryHeap() = default;
D3D12_QUERY_HEAP_TYPE QueryHeap::type() const { return impl_->type; }
uint32_t QueryHeap::count() const { return impl_->count; }

HRESULT Device::create_query_heap(const D3D12_QUERY_HEAP_DESC& desc, std::unique_ptr<QueryHeap>* out)
{
    Device::Impl& d = *impl_;
    const VkPhysicalDeviceFeatures& f = d.physical->features;
    if (!desc.Count)
        return E_INVALIDARG;
    VkQueryPoolCreateInfo ci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    ci.queryCount = desc.Count;
    auto impl = std::make_unique<QueryHeap::Impl>();
    switch (desc.Type) {
    case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
        ci.queryType = VK_QUERY_TYPE_OCCLUSION;
        break;
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
        if (!d.physical->queue_families[d.direct.family].timestampValidBits) {
            log(LogLevel::warn, "query heap: the direct queue has no timestamps");
            return E_NOTIMPL;
        }
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        break;
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
        if (!f.pipelineStatisticsQuery || !f.geometryShader || !f.tessellationShader) {
            log(LogLevel::warn, "query heap: no pipeline statistics with geometry and tessellation counters");
            return E_NOTIMPL;
        }
        ci.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        ci.pipelineStatistics = kPipelineStatistics;
        impl->stride = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS);
        break;
    default:
        log(LogLevel::warn, "query heap type %d is not supported", static_cast<int>(desc.Type));
        return E_NOTIMPL;
    }
    impl->device = &d;
    impl->type = desc.Type;
    impl->count = desc.Count;
    VkResult r = d.vk.vkCreateQueryPool(d.device, &ci, nullptr, &impl->pool);
    if (r != VK_SUCCESS)
        return hresult_from_vk(r);

    // Make every query available: empty begin/end pairs (zero), or a timestamp. Queues wait for this submission
    // before their next one, like for image initialization.
    const VkQueryPool pool = impl->pool;
    const VkQueryType type = ci.queryType;
    HRESULT hr = d.init_submit([&](VkCommandBuffer cmd) {
        d.vk.vkCmdResetQueryPool(cmd, pool, 0, desc.Count);
        for (uint32_t i = 0; i < desc.Count; i++) {
            if (type == VK_QUERY_TYPE_TIMESTAMP) {
                d.vk.vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, i);
            } else {
                d.vk.vkCmdBeginQuery(cmd, pool, i, 0);
                d.vk.vkCmdEndQuery(cmd, pool, i);
            }
        }
    });
    if (FAILED(hr))
        return hr;
    out->reset(new QueryHeap(std::move(impl)));
    return S_OK;
}

uint64_t Device::timestamp_frequency() const
{
    const float period = impl_->physical->limits.timestampPeriod;   // nanoseconds per tick
    return period > 0 ? static_cast<uint64_t>(1e9 / period + 0.5) : 0;
}

void Device::set_timestamp_frequency(uint64_t hz)
{
    impl_->timestamp_target.store(hz);
}

// --- The fixup pipeline ------------------------------------------------------------------------------------

const PipelineState* Device::Impl::get_query_fixup()
{
    std::lock_guard g(internal_lock);
    if (query_fixup)
        return query_fixup.get();
    // query.hlsl: the results as a root UAV, then the root constants.
    D3D12_ROOT_PARAMETER1 params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 6};
    D3D12_ROOT_SIGNATURE_DESC1 rs_desc{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    std::unique_ptr<RootSignature> rs;
    std::shared_ptr<const Shader> cs;
    std::unique_ptr<PipelineState> pso;
    HRESULT hr = RootSignature::create(rs_desc, &rs);
    if (SUCCEEDED(hr))
        hr = Shader::from_code(ShaderStage::compute, k_cs_query_fixup, &cs);
    if (SUCCEEDED(hr))
        hr = PipelineState::create_compute({std::move(rs), cs}, &pso);
    if (SUCCEEDED(hr))
        hr = pso->compile(*self);
    if (FAILED(hr)) {
        log(LogLevel::error, "query fixup pipeline: 0x%08x", static_cast<unsigned>(hr));
        return nullptr;
    }
    query_fixup = std::move(pso);
    return query_fixup.get();
}

// --- Recording ---------------------------------------------------------------------------------------------

void CommandList::Impl::end_active_queries()
{
    for (auto [pool, index] : active_queries)
        device->vk.vkCmdEndQuery(cmd, pool, index);
    active_queries.clear();
}

void CommandList::begin_query(QueryHeap& heap, D3D12_QUERY_TYPE type, uint32_t index)
{
    Impl& l = *impl_;
    QueryHeap::Impl& h = heap.impl();
    if (type == D3D12_QUERY_TYPE_TIMESTAMP || !matches(h.type, type) || index >= h.count) {
        l.fail(E_INVALIDARG);
        return;
    }
    if (l.type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        // Vulkan needs a graphics queue for these; the query keeps its last result.
        log(LogLevel::warn, "query type %d on a non-direct command list is ignored", static_cast<int>(type));
        return;
    }
    for (auto [pool, i] : l.active_queries)
        if (pool == h.pool && i == index) {
            l.fail(E_INVALIDARG);
            return;
        }
    l.end_rendering();
    auto& vk = l.device->vk;
    vk.vkCmdResetQueryPool(l.cmd, h.pool, index, 1);
    // Binary occlusion only needs zero or not: the fixup makes it 0 or 1.
    vk.vkCmdBeginQuery(l.cmd, h.pool, index, type == D3D12_QUERY_TYPE_OCCLUSION ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    l.active_queries.push_back({h.pool, index});
}

void CommandList::end_query(QueryHeap& heap, D3D12_QUERY_TYPE type, uint32_t index)
{
    Impl& l = *impl_;
    QueryHeap::Impl& h = heap.impl();
    if (!matches(h.type, type) || index >= h.count) {
        l.fail(E_INVALIDARG);
        return;
    }
    auto& vk = l.device->vk;
    if (type == D3D12_QUERY_TYPE_TIMESTAMP) {
        const uint32_t family = l.device->slot_for(l.type).family;
        if (!l.device->physical->queue_families[family].timestampValidBits) {
            log(LogLevel::warn, "timestamp on a queue without timestamps is ignored");
            return;
        }
        l.end_rendering();
        vk.vkCmdResetQueryPool(l.cmd, h.pool, index, 1);
        vk.vkCmdWriteTimestamp2(l.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, h.pool, index);
        return;
    }
    for (auto it = l.active_queries.begin(); it != l.active_queries.end(); ++it)
        if (it->first == h.pool && it->second == index) {
            // Begun outside rendering, so it ends outside too.
            l.end_rendering();
            vk.vkCmdEndQuery(l.cmd, h.pool, index);
            l.active_queries.erase(it);
            return;
        }
    // Not begun here: ignored on non-direct lists (see begin_query), an application error otherwise.
    if (l.type == D3D12_COMMAND_LIST_TYPE_DIRECT)
        l.fail(E_INVALIDARG);
}

void CommandList::resolve_query_data(QueryHeap& heap, D3D12_QUERY_TYPE type, uint32_t start, uint32_t count,
                                     Resource& dst, uint64_t offset)
{
    Impl& l = *impl_;
    QueryHeap::Impl& h = heap.impl();
    Resource::Impl& r = dst.impl();
    if (!matches(h.type, type) || start > h.count || count > h.count - start || !r.buffer || offset % 8 ||
        offset > r.size || uint64_t(count) * h.stride > r.size - offset) {
        l.fail(E_INVALIDARG);
        return;
    }
    if (!count)
        return;
    l.end_rendering();
    auto& vk = l.device->vk;
    vk.vkCmdCopyQueryPoolResults(l.cmd, h.pool, start, count, r.buffer, offset, h.stride,
                                 VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

    // What D3D12 reports differently: binary occlusion as 0 or 1, timestamps at the rate the runtime was told.
    uint32_t mode;
    uint64_t num = 1, den = 1;
    if (type == D3D12_QUERY_TYPE_BINARY_OCCLUSION) {
        mode = kFixupBinary;
    } else if (type == D3D12_QUERY_TYPE_TIMESTAMP && l.device->timestamp_target.load()) {
        // ticks * period ns * target / 1e9 = ticks * num / den, with the period in femtoseconds.
        mode = kFixupScale;
        num = static_cast<uint64_t>(static_cast<double>(l.device->physical->limits.timestampPeriod) * 1e6 + 0.5) *
              l.device->timestamp_target.load();
        den = 1'000'000'000'000'000ull;
        const uint64_t g = std::gcd(num, den);
        num /= g;
        den /= g;
        // The shader computes t % den * num: keep num * den within 64 bits.
        while (num > 1 && den > 1 && num > UINT64_MAX / den) {
            num >>= 1;
            den >>= 1;
        }
        if (num == den)
            return;
    } else {
        return;
    }
    if (l.type == D3D12_COMMAND_LIST_TYPE_COPY) {
        log(LogLevel::warn, "query results of type %d resolved on a copy list are not fixed up",
            static_cast<int>(type));
        return;
    }
    const PipelineState* fixup = l.device->get_query_fixup();
    if (!fixup) {
        l.fail(E_OUTOFMEMORY);
        return;
    }
    const PipelineState::Impl& p = fixup->impl();
    const auto params = fixup->root_signature().parameters();
    std::array<uint8_t, 256> data{};
    const uint64_t va = r.address + offset;
    const uint32_t words[6] = {count, mode, uint32_t(num), uint32_t(num >> 32), uint32_t(den), uint32_t(den >> 32)};
    std::memcpy(data.data() + params[0].offset, &va, sizeof(va));
    std::memcpy(data.data() + params[1].offset, words, sizeof(words));

    global_barrier();
    vk.vkCmdBindPipeline(l.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
    vk.vkCmdPushConstants(l.cmd, p.layout, VK_SHADER_STAGE_ALL, 0, p.push_size, data.data());
    vk.vkCmdDispatch(l.cmd, (count + 63) / 64, 1, 1);
    global_barrier();
    // The application's compute state has to be bound again before its next dispatch.
    l.compute_root.bound = nullptr;
    l.compute_root.dirty = l.compute_root.heaps_dirty = true;
}

} // namespace vgd
