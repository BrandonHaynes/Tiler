#include <pthread.h>

#include <iostream>
#include <string.h>

#include "VideoDecoder.h"
#include "TileVideoEncoder.h"

typedef struct Statistics
{
    unsigned long long start, end, frequency;
} Statistics;

int error(const char* message, const int exitCode)
{
    std::cerr << message;
    return exitCode;
}

void* DecodeWorker(void *arg)
{
    auto* decoder = (CudaDecoder*)arg;
    decoder->Start();

    return NULL;
}

int MatchFPS(const float fpsRatio, const int decodedFrames, const int encodedFrames)
{
    if (fpsRatio < 1.f)
    {
        // need to drop frame
        if (decodedFrames * fpsRatio < (encodedFrames + 1))
            return -1;
    }
    else if (fpsRatio > 1.f)
    {
        // need to duplicate frame	 
        auto duplicate = 0;
        while (decodedFrames*fpsRatio > encodedFrames + duplicate + 1)
            duplicate++;

        return duplicate;
    }

    return 0;
}

int PrintHelp()
{
    std::cout << "Usage : NvTranscoder \n"
                    "-i <string>                  Specify input .h264 file\n"
                    "-o <string>                  Specify output bitstream file\n"
                    "\n### Optional parameters ###\n"
                    "-size <int int>              Specify output resolution <width height>\n"
                    "-codec <integer>             Specify the codec \n"
                    "                                 0: H264\n"
                    "                                 1: HEVC\n"
                    "-preset <string>             Specify the preset for encoder settings\n"
                    "                                 hq : nvenc HQ \n"
                    "                                 hp : nvenc HP \n"
                    "                                 lowLatencyHP : nvenc low latency HP \n"
                    "                                 lowLatencyHQ : nvenc low latency HQ \n"
                    "                                 lossless : nvenc Lossless HP \n"
                    "-fps <integer>               Specify encoding frame rate\n"
                    "-goplength <integer>         Specify gop length\n"
                    "-numB <integer>              Specify number of B frames\n"
                    "-bitrate <integer>           Specify the encoding average bitrate\n"
                    "-vbvMaxBitrate <integer>     Specify the vbv max bitrate\n"
                    "-vbvSize <integer>           Specify the encoding vbv/hrd buffer size\n"
                    "-rcmode <integer>            Specify the rate control mode\n"
                    "                                 0:  Constant QP\n"
                    "                                 1:  Single pass VBR\n"
                    "                                 2:  Single pass CBR\n"
                    "                                 4:  Single pass VBR minQP\n"
                    "                                 8:  Two pass frame quality\n"
                    "                                 16: Two pass frame size cap\n"
                    "                                 32: Two pass VBR\n"
                    "-qp <integer>                Specify qp for Constant QP mode\n"
                    "-i_qfactor <float>           Specify qscale difference between I-frames and P-frames\n"
                    "-b_qfactor <float>           Specify qscale difference between P-frames and B-frames\n"
                    "-i_qoffset <float>           Specify qscale offset between I-frames and P-frames\n"
                    "-b_qoffset <float>           Specify qscale offset between P-frames and B-frames\n"
                    "-deviceID <integer>          Specify the GPU device on which encoding will take place\n"
                    "-help                        Prints Help Information\n\n";
    return 1;
}

int DisplayConfiguration(const EncodeConfig& configuration)
{
    printf("Encoding input           : \"%s\"\n", configuration.inputFileName);
    printf("         output          : \"%s\"\n", configuration.outputFileName);
    printf("         codec           : \"%s\"\n", configuration.codec == NV_ENC_HEVC ? "HEVC" : "H264");
    printf("         size            : %dx%d\n", configuration.width, configuration.height);
    printf("         bitrate         : %d bits/sec\n", configuration.bitrate);
    printf("         vbvMaxBitrate   : %d bits/sec\n", configuration.vbvMaxBitrate);
    printf("         vbvSize         : %d bits\n", configuration.vbvSize);
    printf("         fps             : %d frames/sec\n", configuration.fps);
    printf("         rcMode          : %s\n", configuration.rcMode == NV_ENC_PARAMS_RC_CONSTQP ? "CONSTQP" :
        configuration.rcMode == NV_ENC_PARAMS_RC_VBR ? "VBR" :
        configuration.rcMode == NV_ENC_PARAMS_RC_CBR ? "CBR" :
        configuration.rcMode == NV_ENC_PARAMS_RC_VBR_MINQP ? "VBR MINQP" :
        configuration.rcMode == NV_ENC_PARAMS_RC_2_PASS_QUALITY ? "TWO_PASS_QUALITY" :
        configuration.rcMode == NV_ENC_PARAMS_RC_2_PASS_FRAMESIZE_CAP ? "TWO_PASS_FRAMESIZE_CAP" :
        configuration.rcMode == NV_ENC_PARAMS_RC_2_PASS_VBR ? "TWO_PASS_VBR" : "UNKNOWN");
    if (configuration.gopLength == NVENC_INFINITE_GOPLENGTH)
        printf("         goplength       : INFINITE GOP \n");
    else
        printf("         goplength       : %d \n", configuration.gopLength);
    printf("         B frames        : %d \n", configuration.numB);
    printf("         QP              : %d \n", configuration.qp);
    printf("         preset          : %s\n", (configuration.presetGUID == NV_ENC_PRESET_LOW_LATENCY_HQ_GUID) ? "LOW_LATENCY_HQ" :
        (configuration.presetGUID == NV_ENC_PRESET_LOW_LATENCY_HP_GUID) ? "LOW_LATENCY_HP" :
        (configuration.presetGUID == NV_ENC_PRESET_HQ_GUID) ? "HQ_PRESET" :
        (configuration.presetGUID == NV_ENC_PRESET_HP_GUID) ? "HP_PRESET" :
        (configuration.presetGUID == NV_ENC_PRESET_LOSSLESS_HP_GUID) ? "LOSSLESS_HP" : "LOW_LATENCY_DEFAULT");
    printf("\n");

    return 0;
}

float InitializeDecoder(CudaDecoder& decoder, CUVIDFrameQueue& queue, CUvideoctxlock& lock, EncodeConfig& configuration)
{
    int decodedW, decodedH, decodedFRN, decodedFRD, isProgressive;

    decoder.InitVideoDecoder(configuration.inputFileName, lock, &queue, configuration.width, configuration.height);

    decoder.GetCodecParam(&decodedW, &decodedH, &decodedFRN, &decodedFRD, &isProgressive);
    if (decodedFRN <= 0 || decodedFRD <= 0) {
        decodedFRN = 30;
        decodedFRD = 1;
    }

    if(configuration.width <= 0 || configuration.height <= 0) {
        configuration.width  = decodedW;
        configuration.height = decodedH;
    }

    float fpsRatio = 1.f;
    if (configuration.fps <= 0)
        configuration.fps = decodedFRN / decodedFRD;
    else
        fpsRatio = (float)configuration.fps * decodedFRD / decodedFRN;

    configuration.pictureStruct = isProgressive ? NV_ENC_PIC_STRUCT_FRAME : 0;
    queue.init(configuration.width, configuration.height);

    return fpsRatio;
}

void EncodeWorker(CudaDecoder& decoder, VideoEncoder& encoder, CUVIDFrameQueue& queue, EncodeConfig& configuration,
                  float fpsRatio)
{
    auto frmProcessed = 0;
    auto frmActual = 0;

    while(!(queue.isEndOfDecode() && queue.isEmpty()) )
    {
        CUVIDPARSERDISPINFO frame;

        if(queue.dequeue(&frame))
        {
            CUdeviceptr mappedFrame = 0;
            CUVIDPROCPARAMS oVPP = { 0 };
            unsigned int pitch;

            oVPP.progressive_frame = frame.progressive_frame;
            oVPP.second_field = 0;
            oVPP.top_field_first = frame.top_field_first;
            oVPP.unpaired_field = (frame.progressive_frame == 1 || frame.repeat_first_field <= 1);

            cuvidMapVideoFrame(decoder.GetDecoder(), frame.picture_index, &mappedFrame, &pitch, &oVPP);

            EncodeFrameConfig stEncodeConfig = { 0 };
            auto pictureType = (frame.progressive_frame || frame.repeat_first_field >= 2 ? NV_ENC_PIC_STRUCT_FRAME :
                (frame.top_field_first ? NV_ENC_PIC_STRUCT_FIELD_TOP_BOTTOM : NV_ENC_PIC_STRUCT_FIELD_BOTTOM_TOP));

            stEncodeConfig.device_pointer = mappedFrame;
            stEncodeConfig.pitch = pitch;
            stEncodeConfig.width = configuration.width;
            stEncodeConfig.height = configuration.height;

            auto dropOrDuplicate = MatchFPS(fpsRatio, frmProcessed, frmActual);
            for (auto i = 0; i <= dropOrDuplicate; i++) {
                encoder.EncodeFrame(&stEncodeConfig, pictureType);
                frmActual++;
            }
            frmProcessed++;

            cuvidUnmapVideoFrame(decoder.GetDecoder(), mappedFrame);
            queue.releaseFrame(&frame);
       }
    }

    encoder.EncodeFrame(NULL, NV_ENC_PIC_STRUCT_FRAME, true);
}

int ExecuteWorkers(CudaDecoder& decoder, VideoEncoder& encoder, CUVIDFrameQueue& frameQueue,
                   EncodeConfig& configuration, float fpsRatio, Statistics& statistics)
{
    pthread_t decode_pid;

    NvQueryPerformanceCounter(&statistics.start);

    // Start decoding thread
    pthread_create(&decode_pid, NULL, DecodeWorker, (void*)&decoder);

    // Execute encoder in main thread
    EncodeWorker(decoder, encoder, frameQueue, configuration, fpsRatio);

    pthread_join(decode_pid, NULL);

    return 0;
}

int DisplayStatistics(CudaDecoder& decoder, VideoEncoder& encoder, Statistics& statistics)
{
    if (encoder.GetEncodedFrames() > 0)
    {
        NvQueryPerformanceCounter(&statistics.end);
        NvQueryPerformanceFrequency(&statistics.frequency);

        auto elapsedTime = (double)(statistics.end - statistics.start)/(double)statistics.frequency;
        printf("Total time: %fms, Decoded Frames: %d, Encoded Frames: %ld, Average FPS: %f\n",
            elapsedTime * 1000,
            decoder.m_decodedFrames,
            encoder.GetEncodedFrames(),
            (float)encoder.GetEncodedFrames() / elapsedTime);
    }

    return 0;
}

int main(int argc, char* argv[])
{
    typedef void *CUDADRIVER;
    CUDADRIVER hHandleDriver = 0;
    CUcontext cudaCtx;
    CUdevice device;
    CUcontext curCtx;
    CUvideoctxlock lock;
    CUresult result;
    NVENCSTATUS status;
    CudaDecoder decoder;
    CUVIDFrameQueue frameQueue(lock);
    float fpsRatio = 1.f;
    Statistics statistics;


    if((result = cuInit(0, __CUDA_API_VERSION, hHandleDriver)) != CUDA_SUCCESS)
        return error("Error in cuInit", result);
	else if((result = cuvidInit(0)) != CUDA_SUCCESS)
        return error("Error in cuInit", result);

    EncodeConfig encodeConfig = { 0 };
    encodeConfig.endFrameIdx = INT_MAX;
    encodeConfig.bitrate = 5000000;
    encodeConfig.rcMode = NV_ENC_PARAMS_RC_CONSTQP;
    encodeConfig.gopLength = NVENC_INFINITE_GOPLENGTH;
    encodeConfig.codec = NV_ENC_H264;
    encodeConfig.fps = 0;
    encodeConfig.qp = 28;
    encodeConfig.i_quant_factor = DEFAULT_I_QFACTOR;
    encodeConfig.b_quant_factor = DEFAULT_B_QFACTOR;  
    encodeConfig.i_quant_offset = DEFAULT_I_QOFFSET;
    encodeConfig.b_quant_offset = DEFAULT_B_QOFFSET;   
    encodeConfig.presetGUID = NV_ENC_PRESET_DEFAULT_GUID;
    encodeConfig.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;

    unsigned int tileColumns = 8, tileRows = 4;

    // Verify arguments
    if((status = CNvHWEncoder::ParseArguments(&encodeConfig, argc, argv)) != NV_ENC_SUCCESS)
        return PrintHelp();
    else if (!encodeConfig.inputFileName || !encodeConfig.outputFileName)
        return PrintHelp();

    // Initialize CUDA
    else if((result = cuDeviceGet(&device, encodeConfig.deviceID)) != CUDA_SUCCESS)
        return error("cuDeviceGet", result);
    else if((result = cuCtxCreate(&cudaCtx, CU_CTX_SCHED_AUTO, device)) != CUDA_SUCCESS)
        return error("cuCtxCreate", result);
    else if((result = cuCtxPopCurrent(&curCtx)) != CUDA_SUCCESS)
        return error("cuCtxPopCurrent", result);
    else if((result = cuvidCtxLockCreate(&lock, curCtx)) != CUDA_SUCCESS)
        return error("cuvidCtxLockCreate", result);
    else if((fpsRatio = InitializeDecoder(decoder, frameQueue, lock, encodeConfig)) < 0)
        return error("InitializeDecoder", -1);

    // Initialize encoder
    VideoEncoder encoder(lock, tileColumns, tileRows);
    if((status = encoder.Initialize(cudaCtx, NV_ENC_DEVICE_TYPE_CUDA)) != NV_ENC_SUCCESS)
        return error("encoder.Initialize", -1);

//    encodeConfig.presetGUID = NV_ENC_PRESET_DEFAULT_GUID; //encoder->GetPresetGUID();
    else if(DisplayConfiguration(encodeConfig) != 0)
        return error("DisplayConfiguration", -1);
    else if((status = encoder.CreateEncoders(encodeConfig)) != NV_ENC_SUCCESS)
        return error("CreateEncoders", -1);
    else if((status = encoder.AllocateIOBuffers(&encodeConfig)) != NV_ENC_SUCCESS)
        return error("encoder.AllocateIOBuffers", -1);
    else if(ExecuteWorkers(decoder, encoder, frameQueue, encodeConfig, fpsRatio, statistics) != 0)
        return error("ExecuteWorkers", -1);
    else if(DisplayStatistics(decoder, encoder, statistics) != 0)
        return error("DisplayStatistics", -1);
    else if((status = encoder.Deinitialize()) != NV_ENC_SUCCESS)
        return error("encoder.Deinitialize", result);
    if((result = cuvidCtxLockDestroy(lock)) != CUDA_SUCCESS)
        return error("cuvidCtxLockDestroy", result);
    else if((result = cuCtxDestroy(cudaCtx)) != CUDA_SUCCESS)
        return error("cuCtxDestroy", result);
    else
        return 0;
}
