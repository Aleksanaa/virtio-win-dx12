// SPDX-License-Identifier: MIT
// Queries and predication. The DDI's query types are the API's except that D3D10's pipeline statistics (fewer
// counters) have a type of their own and the per-stream ones are numbered after it.
#include "device.h"

#include <cstring>

namespace vgd::umd {

namespace {

bool api_query(D3D10DDI_QUERY q, D3D11_QUERY* out)
{
    switch (q) {
    case D3D10DDI_QUERY_EVENT: *out = D3D11_QUERY_EVENT; return true;
    case D3D10DDI_QUERY_OCCLUSION: *out = D3D11_QUERY_OCCLUSION; return true;
    case D3D10DDI_QUERY_TIMESTAMP: *out = D3D11_QUERY_TIMESTAMP; return true;
    case D3D10DDI_QUERY_TIMESTAMPDISJOINT: *out = D3D11_QUERY_TIMESTAMP_DISJOINT; return true;
    case D3D10DDI_QUERY_PIPELINESTATS:
    case D3D11DDI_QUERY_PIPELINESTATS: *out = D3D11_QUERY_PIPELINE_STATISTICS; return true;
    case D3D10DDI_QUERY_OCCLUSIONPREDICATE: *out = D3D11_QUERY_OCCLUSION_PREDICATE; return true;
    case D3D10DDI_QUERY_STREAMOUTPUTSTATS: *out = D3D11_QUERY_SO_STATISTICS; return true;
    case D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE: *out = D3D11_QUERY_SO_OVERFLOW_PREDICATE; return true;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0: *out = D3D11_QUERY_SO_STATISTICS_STREAM0; return true;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1: *out = D3D11_QUERY_SO_STATISTICS_STREAM1; return true;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2: *out = D3D11_QUERY_SO_STATISTICS_STREAM2; return true;
    case D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3: *out = D3D11_QUERY_SO_STATISTICS_STREAM3; return true;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0: *out = D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0; return true;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1: *out = D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1; return true;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2: *out = D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2; return true;
    case D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3: *out = D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3; return true;
    default: return false;
    }
}

bool is_predicate(D3D11_QUERY q)
{
    return q == D3D11_QUERY_OCCLUSION_PREDICATE || q == D3D11_QUERY_SO_OVERFLOW_PREDICATE ||
           (q >= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 && q <= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3);
}

SIZE_T APIENTRY calc_private_query_size(D3D10DDI_HDEVICE, const D3D10DDIARG_CREATEQUERY*)
{
    return sizeof(QueryObject);
}

void APIENTRY create_query(D3D10DDI_HDEVICE h, const D3D10DDIARG_CREATEQUERY* a, D3D10DDI_HQUERY hq,
                           D3D10DDI_HRTQUERY)
{
    Device* d = Device::from(h);
    auto* q = new (hq.pDrvPrivate) QueryObject;
    q->type = a->Query;
    D3D11_QUERY type;
    if (!api_query(a->Query, &type)) {
        VGD_WARN("CreateQuery: query type %d (a counter?) not supported", a->Query);
        d->set_error(DXGI_DDI_ERR_UNSUPPORTED);
        return;
    }
    // The predicate hint has the API's value.
    const D3D11_QUERY_DESC desc{type, a->MiscFlags};
    HRESULT hr = is_predicate(type)
                     ? d->d3d->CreatePredicate(&desc, reinterpret_cast<ID3D11Predicate**>(q->query.put()))
                     : d->d3d->CreateQuery(&desc, q->query.put());
    if (FAILED(hr)) {
        VGD_WARN("CreateQuery: type %d failed: 0x%08lx", type, hr);
        d->set_error(E_OUTOFMEMORY);
    }
}

void APIENTRY destroy_query(D3D10DDI_HDEVICE, D3D10DDI_HQUERY hq)
{
    object<QueryObject>(hq)->~QueryObject();
}

void APIENTRY query_begin(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY hq)
{
    Device::from(h)->ctx->Begin(object<QueryObject>(hq)->query.get());
}

void APIENTRY query_end(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY hq)
{
    Device::from(h)->ctx->End(object<QueryObject>(hq)->query.get());
}

// The runtime's buffer is written only when the result is there (DXVK may write an event's FALSE while it waits).
void APIENTRY query_get_data(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY hq, void* data, UINT size, UINT flags)
{
    Device* d = Device::from(h);
    QueryObject* q = object<QueryObject>(hq);
    union {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        D3D11_QUERY_DATA_SO_STATISTICS so;
        UINT64 u64;
        BOOL b;
    } result{};
    const UINT api_size = q->query->GetDataSize();
    // The do-not-flush flag has the API's value.
    HRESULT hr = d->ctx->GetData(q->query.get(), &result, api_size, flags);
    if (hr == S_FALSE) {
        d->set_error(DXGI_DDI_ERR_WASSTILLDRAWING);
        return;
    }
    if (FAILED(hr)) {
        d->set_error(D3DDDIERR_DEVICEREMOVED);
        return;
    }
    if (!data)
        return;
    // D3D10's pipeline statistics are the API's first eight counters.
    std::memcpy(data, &result, std::min(size, api_size));
}

void APIENTRY set_predication(D3D10DDI_HDEVICE h, D3D10DDI_HQUERY hq, BOOL value)
{
    ID3D11Predicate* p = hq.pDrvPrivate ? static_cast<ID3D11Predicate*>(object<QueryObject>(hq)->query.get()) : nullptr;
    Device::from(h)->ctx->SetPredication(p, value);
}

} // namespace

void fill_query_ddi(D3D11_1DDI_DEVICEFUNCS& t)
{
    t.pfnCalcPrivateQuerySize = calc_private_query_size;
    t.pfnCreateQuery = create_query;
    t.pfnDestroyQuery = destroy_query;
    t.pfnQueryBegin = query_begin;
    t.pfnQueryEnd = query_end;
    t.pfnQueryGetData = query_get_data;
    t.pfnSetPredication = set_predication;
}

} // namespace vgd::umd
