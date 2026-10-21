// SPDX-License-Identifier: MIT
// Command signatures and ExecuteIndirect, on core's (core/src/commands.cpp).
#include "device.h"
#include "log.h"
#include "objects.h"

#include <cstring>
#include <new>
#include <vector>

namespace vgd::umd {

namespace {

SIZE_T APIENTRY calc_private_command_signature_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*)
{
    return sizeof(CommandSignatureObject);
}

HRESULT APIENTRY create_command_signature(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001* args,
                                          D3D12DDI_HCOMMANDSIGNATURE hs)
{
    // The DDI's argument descriptions are the API's, field for field.
    static_assert(sizeof(D3D12DDI_INDIRECT_ARGUMENT_DESC) == sizeof(D3D12_INDIRECT_ARGUMENT_DESC));
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> descs(args->NumArgumentDescs);
    if (args->NumArgumentDescs)
        std::memcpy(descs.data(), args->pArgumentDescs, descs.size() * sizeof(descs[0]));
    const D3D12_COMMAND_SIGNATURE_DESC desc{args->ByteStride, args->NumArgumentDescs, descs.data(), args->NodeMask};
    auto* obj = new (hs.pDrvPrivate) CommandSignatureObject;
    HRESULT hr = vgd::CommandSignature::create(desc, &obj->signature);
    if (FAILED(hr)) {
        VGD_WARN("CreateCommandSignature: %u arguments (first type %d), stride %u failed: 0x%08lx",
                 args->NumArgumentDescs, args->NumArgumentDescs ? static_cast<int>(args->pArgumentDescs[0].Type) : -1,
                 args->ByteStride, hr);
        obj->~CommandSignatureObject();
        return E_INVALIDARG;
    }
    VGD_VERBOSE("CreateCommandSignature: type %d, stride %u", static_cast<int>(obj->signature.type), args->ByteStride);
    return S_OK;
}

void APIENTRY destroy_command_signature(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDSIGNATURE hs)
{
    object<CommandSignatureObject>(hs)->~CommandSignatureObject();
}

void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HCOMMANDSIGNATURE hs, UINT max_count,
                               D3D12DDIARG_BUFFER_PLACEMENT args, D3D12DDIARG_BUFFER_PLACEMENT count)
{
    const auto& c = count.BaseAddress.UMD;
    vgd::Resource* count_resource = c.hResource.pDrvPrivate ? object<ResourceObject>(c.hResource)->resource : nullptr;
    object<CommandListObject>(hl)->list->execute_indirect(
        object<CommandSignatureObject>(hs)->signature, max_count,
        *object<ResourceObject>(args.BaseAddress.UMD.hResource)->resource, args.BaseAddress.UMD.Offset, count_resource,
        c.Offset);
}

} // namespace

void fill_indirect_ddi(D3D12DDI_DEVICE_FUNCS_CORE_0088& t)
{
    t.pfnCalcPrivateCommandSignatureSize = calc_private_command_signature_size;
    t.pfnCreateCommandSignature = create_command_signature;
    t.pfnDestroyCommandSignature = destroy_command_signature;
}

void fill_indirect_list_ddi(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t)
{
    t.pfnExecuteIndirect = execute_indirect;
}

} // namespace vgd::umd
