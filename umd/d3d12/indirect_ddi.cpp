// SPDX-License-Identifier: MIT
// Command signatures and ExecuteIndirect, on vkd3d-proton's.
#include "device.h"
#include "log.h"
#include "objects.h"

#include <new>

namespace vgd::umd {

namespace {

SIZE_T APIENTRY calc_private_command_signature_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001*)
{
    return sizeof(CommandSignatureObject);
}

HRESULT APIENTRY create_command_signature(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATE_COMMAND_SIGNATURE_0001* args,
                                          D3D12DDI_HCOMMANDSIGNATURE hs)
{
    // The DDI's argument descriptions are the API's, field for field.
    static_assert(sizeof(D3D12DDI_INDIRECT_ARGUMENT_DESC) == sizeof(D3D12_INDIRECT_ARGUMENT_DESC));
    const D3D12_COMMAND_SIGNATURE_DESC desc{args->ByteStride, args->NumArgumentDescs,
                                            reinterpret_cast<const D3D12_INDIRECT_ARGUMENT_DESC*>(args->pArgumentDescs),
                                            args->NodeMask};
    ID3D12RootSignature* root =
        args->hRootSignature.pDrvPrivate ? object<RootSignatureObject>(args->hRootSignature)->rs.get() : nullptr;
    auto* obj = new (hs.pDrvPrivate) CommandSignatureObject;
    HRESULT hr = Device::from(h)->d3d->CreateCommandSignature(&desc, root, __uuidof(ID3D12CommandSignature),
                                                              obj->signature.put_void());
    if (FAILED(hr)) {
        VGD_WARN("CreateCommandSignature: %u arguments, stride %u failed: 0x%08lx", args->NumArgumentDescs,
                 args->ByteStride, hr);
        obj->~CommandSignatureObject();
        return E_OUTOFMEMORY;
    }
    return S_OK;
}

void APIENTRY destroy_command_signature(D3D12DDI_HDEVICE, D3D12DDI_HCOMMANDSIGNATURE hs)
{
    object<CommandSignatureObject>(hs)->~CommandSignatureObject();
}

void APIENTRY execute_indirect(D3D12DDI_HCOMMANDLIST hl, D3D12DDI_HCOMMANDSIGNATURE hs, UINT max_count,
                               D3D12DDIARG_BUFFER_PLACEMENT args, D3D12DDIARG_BUFFER_PLACEMENT count)
{
    object<CommandListObject>(hl)->list->ExecuteIndirect(
        object<CommandSignatureObject>(hs)->signature.get(), max_count, resource_of(args.BaseAddress.UMD.hResource),
        args.BaseAddress.UMD.Offset, resource_of(count.BaseAddress.UMD.hResource), count.BaseAddress.UMD.Offset);
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
