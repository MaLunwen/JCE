/* Negative-only former DXGI Output.HMONITOR routing, injected into a copy of
 * the complete current owner. Production never compiles or calls this path. */
static bool display_source_legacy_output(JceWindowDxgiContext *ctx,JceWindowDisplaySnapshot *snapshot,
    JceWindowAdapterReason *reason)
{
    /* Keep the actual CCD owner compiled in this variant as well. */
    if(!ctx->monitor) return display_source_ccd(ctx,snapshot,reason);
    unsigned matches=0;
    bool complete=false;
    for(uint32_t i=0;i<JCE_WINDOW_ADAPTER_LIMIT;++i) {
        if(ctx->adapter){IDXGIAdapter_Release(ctx->adapter);ctx->adapter=NULL;}
        HRESULT result=IDXGIFactory_EnumAdapters(ctx->factory,i,&ctx->adapter);
        if(result==DXGI_ERROR_NOT_FOUND){complete=true;break;}
        if(FAILED(result) || !ctx->adapter){*reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;return false;}
        DXGI_ADAPTER_DESC desc;
        if(FAILED(IDXGIAdapter_GetDesc(ctx->adapter,&desc))){*reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;return false;}
        bool output_complete=false,adapter_matches=false;
        for(uint32_t j=0;j<32u;++j) {
            IDXGIOutput *output=NULL;
            result=IDXGIAdapter_EnumOutputs(ctx->adapter,j,&output);
            if(result==DXGI_ERROR_NOT_FOUND){if(output)IDXGIOutput_Release(output);output_complete=true;break;}
            if(FAILED(result) || !output){if(output)IDXGIOutput_Release(output);*reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;return false;}
            DXGI_OUTPUT_DESC output_desc;result=IDXGIOutput_GetDesc(output,&output_desc);
            IDXGIOutput_Release(output);
            if(FAILED(result)){*reason=JCE_WINDOW_ADAPTER_ENUMERATION_FAILED;return false;}
            if((uintptr_t)output_desc.Monitor==ctx->monitor)adapter_matches=true;
        }
        if(!output_complete){*reason=JCE_WINDOW_ADAPTER_ENUMERATION_LIMIT;return false;}
        if(adapter_matches){snapshot->source=(JceWindowDisplaySource){desc.AdapterLuid.LowPart,desc.AdapterLuid.HighPart};++matches;}
    }
    if(!complete){*reason=JCE_WINDOW_ADAPTER_ENUMERATION_LIMIT;return false;}
    if(matches!=1){*reason=matches ? JCE_WINDOW_ADAPTER_AMBIGUOUS : JCE_WINDOW_ADAPTER_NO_MATCH;return false;}
    return true;
}
