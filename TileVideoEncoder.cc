#include <string>
#include "VideoEncoder.h"
#include "dynlink_cuda.h" // <cuda.h>

#define BITSTREAM_BUFFER_SIZE 2*1024*1024

template<typename TCode, typename TReturn>
TReturn error(const char* component, const TCode code, const TReturn result)
{
    fprintf(stderr, "CUDA error %d in %s", code, component);
    return result;
}

template<typename TCode>
TCode error(const char* component, const TCode code)
{
    return error(component, code, code);
}

NVENCSTATUS VideoEncoder::Initialize(void* device, const NV_ENC_DEVICE_TYPE deviceType)
{
    NVENCSTATUS status;

    for(TileEncodeContext& context: tileEncodeContext)
        if((status = context.hardwareEncoder.Initialize(device, deviceType)) != NV_ENC_SUCCESS)
            return status;

    return NV_ENC_SUCCESS;
}

NVENCSTATUS VideoEncoder::CreateEncoders(EncodeConfig& rootConfiguration)
{
    NVENCSTATUS status;
    auto filenameTemplate = std::string(rootConfiguration.outputFileName);

    assert(tileDimensions.count);

    for(int i = 0; i < tileDimensions.count; i++)
        {
        auto tileConfiguration = rootConfiguration;
        auto tileFilename = std::string(filenameTemplate).replace(filenameTemplate.find('%'), 2, std::to_string(i));

        tileConfiguration.width = rootConfiguration.width / tileDimensions.columns;
        tileConfiguration.height = rootConfiguration.height / tileDimensions.rows;

        if((tileConfiguration.fOutput = fopen(tileFilename.c_str(), "wb")) == NULL)
            return error(tileFilename.c_str(), errno, NV_ENC_ERR_GENERIC);
        else if((status = tileEncodeContext[i].hardwareEncoder.CreateEncoder(&tileConfiguration)))
            return status;
        }

    presetGUID = tileEncodeContext[0].hardwareEncoder.GetPresetGUID(
            rootConfiguration.encoderPreset, rootConfiguration.codec);

    return NV_ENC_SUCCESS;
}

NVENCSTATUS VideoEncoder::AllocateIOBuffers(const EncodeConfig* configuration)
{
    encodeBufferSize = configuration->numB + 4;

    for(TileEncodeContext& context: tileEncodeContext)
    {
        context.encodeBufferQueue.Initialize(context.encodeBuffer, encodeBufferSize);
        AllocateIOBuffer(context, *configuration);
    }

    return NV_ENC_SUCCESS;
}

NVENCSTATUS VideoEncoder::AllocateIOBuffer(TileEncodeContext& context, const EncodeConfig& configuration)
{
    NVENCSTATUS status;
    CUresult result;
    auto sourceWidth  = configuration.width;// * tileDimensions.columns;
    auto sourceHeight = configuration.height;// * tileDimensions.rows;

    for (auto i = 0; i < encodeBufferSize; i++) {
        auto& buffer = context.encodeBuffer[i];

        if((result = cuvidCtxLock(lock, 0)) != CUDA_SUCCESS)
            return error("cuvidCtxLock", result, NV_ENC_ERR_GENERIC);
        else if((result = (cuMemAllocPitch(
                &buffer.stInputBfr.pNV12devPtr,
                (size_t*)&buffer.stInputBfr.uNV12Stride,
                sourceWidth,
                sourceHeight * 3/2, 16))) != CUDA_SUCCESS)
            return error("cuMemAllocPitch", result, NV_ENC_ERR_GENERIC);
        else if((result = cuvidCtxUnlock(lock, 0)) != CUDA_SUCCESS)
            return error("cuvidCtxUnlock", result, NV_ENC_ERR_GENERIC);
        else if((status = context.hardwareEncoder.NvEncRegisterResource(
                NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
                (void*)buffer.stInputBfr.pNV12devPtr,
                sourceWidth, sourceHeight,
                buffer.stInputBfr.uNV12Stride,
                &buffer.stInputBfr.nvRegisteredResource)) != NV_ENC_SUCCESS)
            return error("NvEncRegisterResource", status);
        else if((status = context.hardwareEncoder.NvEncCreateBitstreamBuffer(
            BITSTREAM_BUFFER_SIZE,
            &buffer.stOutputBfr.hBitstreamBuffer)) != NV_ENC_SUCCESS)
            return error("NvEncCreateBitstreamBuffer", status);
        else
        {
            buffer.stInputBfr.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12_PL;
            buffer.stInputBfr.dwWidth = sourceWidth;
            buffer.stInputBfr.dwHeight = sourceHeight;
            buffer.stOutputBfr.dwBitstreamBufferSize = BITSTREAM_BUFFER_SIZE;
            buffer.stOutputBfr.hOutputEvent = NULL;
        }
    }
}

NVENCSTATUS VideoEncoder::ReleaseIOBuffers()
{
    CUresult result;

    for(TileEncodeContext& context: tileEncodeContext)
        for (auto i = 0; i < encodeBufferSize; i++)
        {
            auto& buffer = context.encodeBuffer[i];

            if((result = cuvidCtxLock(lock, 0)) != CUDA_SUCCESS)
                return error("cuvidCtxLock", result, NV_ENC_ERR_GENERIC);
            else if((result = cuMemFree(buffer.stInputBfr.pNV12devPtr)) != CUDA_SUCCESS)
                return error("cuMemFree", result, NV_ENC_ERR_GENERIC);
            else if((result = cuvidCtxUnlock(lock, 0)) != CUDA_SUCCESS)
                return error("cuvidCtxUnlock", result, NV_ENC_ERR_GENERIC);
            else
            {
                context.hardwareEncoder.NvEncDestroyBitstreamBuffer(buffer.stOutputBfr.hBitstreamBuffer);
                buffer.stOutputBfr.hBitstreamBuffer = NULL;
            }
        }

    return NV_ENC_SUCCESS;
}

NVENCSTATUS VideoEncoder::FlushEncoder()
{
    NVENCSTATUS status;

    for(TileEncodeContext& context: tileEncodeContext)
    {
        if((status = context.hardwareEncoder.NvEncFlushEncoderQueue(NULL)) != NV_ENC_SUCCESS)
            return status;

        EncodeBuffer *encodeBuffer = context.encodeBufferQueue.GetPending();
        while (encodeBuffer)
        {
            context.hardwareEncoder.ProcessOutput(encodeBuffer);
            encodeBuffer = context.encodeBufferQueue.GetPending();

            if (encodeBuffer && encodeBuffer->stInputBfr.hInputSurface)
            {
                status = context.hardwareEncoder.NvEncUnmapInputResource(encodeBuffer->stInputBfr.hInputSurface);
                encodeBuffer->stInputBfr.hInputSurface = NULL;
            }
        }
    }

    return status;
}

NVENCSTATUS VideoEncoder::Deinitialize()
{
    NVENCSTATUS status;

    ReleaseIOBuffers();

    for(TileEncodeContext& context: tileEncodeContext)
        if((status = context.hardwareEncoder.NvEncDestroyEncoder()) != NV_ENC_SUCCESS)
            return status;

    return NV_ENC_SUCCESS;
}

EncodeBuffer* GetEncodeBuffer(TileEncodeContext& context)
{
    EncodeBuffer *encodeBuffer = context.encodeBufferQueue.GetAvailable();
    if (!encodeBuffer)
    {
        encodeBuffer = context.encodeBufferQueue.GetPending();
        context.hardwareEncoder.ProcessOutput(encodeBuffer);

        // UnMap the input buffer after frame done
        if (encodeBuffer->stInputBfr.hInputSurface)
        {
            context.hardwareEncoder.NvEncUnmapInputResource(encodeBuffer->stInputBfr.hInputSurface);
            encodeBuffer->stInputBfr.hInputSurface = NULL;
        }
        encodeBuffer = context.encodeBufferQueue.GetAvailable();
    }

    return encodeBuffer;
}

NVENCSTATUS VideoEncoder::EncodeFrame(EncodeFrameConfig *inputFrame,
                                      const NV_ENC_PIC_STRUCT inputFrameType, const bool flush)
{
    NVENCSTATUS status;
    CUresult result;

    if (flush)
        return FlushEncoder();

    assert(inputFrame);
    auto screenWidth = inputFrame->width;
    auto screenHeight = inputFrame->height;
    auto tileWidth = screenWidth / tileDimensions.columns;
    auto tileHeight = screenHeight / tileDimensions.rows;

    for(auto i = 0; i < tileDimensions.count; i++)
    {
        auto& context = tileEncodeContext[i];
        auto* encodeBuffer = GetEncodeBuffer(context);

        auto row = i / tileDimensions.columns;
        auto column = i % tileDimensions.columns;

        auto offsetX = column * tileWidth;
        auto offsetY = row * tileHeight;

        CUDA_MEMCPY2D copyParameters = {
            srcXInBytes:   offsetX,
            srcY:          offsetY,
            srcMemoryType: CU_MEMORYTYPE_DEVICE,
            srcHost:       NULL,
            srcDevice:     inputFrame->device_pointer,
            srcArray:      NULL,
            srcPitch:      inputFrame->pitch,

            dstXInBytes:   0,
            dstY:          0,
            dstMemoryType: CU_MEMORYTYPE_DEVICE,
            dstHost:       NULL,
            dstDevice:     (CUdeviceptr)encodeBuffer->stInputBfr.pNV12devPtr,
            dstArray:      NULL,
            dstPitch:      encodeBuffer->stInputBfr.uNV12Stride,

            WidthInBytes:  screenWidth - offsetX,
            Height:        (screenHeight - offsetY)*3/2,
            };

        if((result = cuvidCtxLock(lock, 0)) != CUDA_SUCCESS)
            return error("cuvidCtxLock", result, NV_ENC_ERR_GENERIC);
        else if((result = cuMemcpy2D(&copyParameters)) != CUDA_SUCCESS)
            return error("cuMemcpy2D", result, NV_ENC_ERR_GENERIC);
        else if((result = cuvidCtxUnlock(lock, 0)) != CUDA_SUCCESS)
            return error("cuvidCtxUnlock", result, NV_ENC_ERR_GENERIC);
        else if((status = context.hardwareEncoder.NvEncMapInputResource(
                encodeBuffer->stInputBfr.nvRegisteredResource,
                &encodeBuffer->stInputBfr.hInputSurface)) != NV_ENC_SUCCESS)
            return status;
        else
            context.hardwareEncoder.NvEncEncodeFrame(encodeBuffer, NULL, tileWidth, tileHeight, inputFrameType);
    }

    framesEncoded++;

    return NV_ENC_SUCCESS;
}
